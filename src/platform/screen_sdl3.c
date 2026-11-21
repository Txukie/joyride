/*
 * joyride - the window, input and presenter, on SDL3 and SDL_GPU.
 *
 * Replaces pcrecomp win32hle's screen.c (SDL2) behind the same host-facing
 * interface (hle_screen_open, hle_screen_pump, hle_screen_desktop and the
 * frame hook). The game draws into DirectDraw surfaces in memory; each shown
 * frame is converted to BGRA8, uploaded to a GPU texture and blitted, scaled,
 * onto the window's swapchain with SDL_BlitGPUTexture. SDL_GPU picks Vulkan,
 * Metal or Direct3D 12 for the platform. No shaders are involved yet.
 *
 * Scaling: sharp (nearest to an integer multiple, then linear
 * to the window: crisp pixels without uneven ones), smooth (linear),
 * nearest, integer (largest whole multiple that fits, black around).
 * F12: the settings overlay (src/platform/overlay.c), which holds these and
 * the screen size and texture filtering. F11 or Alt+Enter: fullscreen. Closing the window asks the game to quit
 * (WM_CLOSE); asking again quits outright.
 *
 * The key tables (vk_of, scan_of) follow screen.c's.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define WM_CLOSE       0x0010u
#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP   0x0208u

/* the hooks screen.c defined for hosts; kept so the interface is the same */
void (*hle_screen_frame_hook)(const uint8_t *px, int w, int h, int pitch, int bpp);
int  (*hle_screen_key_hook)(int sdl_scancode, int down);
void (*hle_screen_poll_hook)(void);
int  (*hle_screen_cursor_hook)(void);
int  (*hle_screen_compose_hook)(const uint8_t *px16, int pitch, int w, int h, uint32_t *out);
int  (*hle_screen_overlay_hook)(const uint8_t *px16, int pitch, int w, int h,
                                uint32_t *ov, int *dx, int *dy, int *dw, int *dh);

int joyride_flipping;                    /* set by joyride's Flip around its hle_dd_mark */

enum { MODE_SHARP, MODE_SMOOTH, MODE_NEAREST, MODE_INTEGER, MODE_COUNT };
static const char *const k_modes[MODE_COUNT] = { "sharp", "smooth", "nearest", "integer" };

static int g_headless, g_fullscreen, g_mode = MODE_SHARP, g_close_asked;
static SDL_Window *g_win;
static SDL_GPUDevice *g_gpu;
static SDL_GPUTexture *g_tex, *g_pre;    /* the frame; sharp mode's integer-scaled copy */
static SDL_GPUTransferBuffer *g_upload;
static int g_tw, g_th, g_pw, g_ph;       /* sizes they were made at */
static uint32_t *g_bgra;                 /* the frame converted */
static size_t g_bgra_n;
static int g_src_w, g_src_h;             /* the game's picture */
static SDL_FRect g_dst;                  /* where it went, in window pixels */
static unsigned g_shown_seq = ~0u;
static uint32_t g_last_present;
static pthread_t g_video_thread;
static int g_max_fps = -1;
static uint64_t g_last_flip_ns;
static int g_force;                      /* the next present shows the frame even if unchanged */

/* ---- keys -------------------------------------------------------------- */

static int vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return 0x61 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    default: return 0;
    }
}

/* the PC/AT set-1 scan code, which lParam carries */
static uint32_t scan_of(SDL_Scancode s) {
    static const uint8_t t[SDL_SCANCODE_COUNT] = {
        [SDL_SCANCODE_ESCAPE] = 0x01, [SDL_SCANCODE_1] = 2, [SDL_SCANCODE_2] = 3, [SDL_SCANCODE_3] = 4, [SDL_SCANCODE_4] = 5,
        [SDL_SCANCODE_5] = 6, [SDL_SCANCODE_6] = 7, [SDL_SCANCODE_7] = 8, [SDL_SCANCODE_8] = 9, [SDL_SCANCODE_9] = 10,
        [SDL_SCANCODE_0] = 11, [SDL_SCANCODE_MINUS] = 12, [SDL_SCANCODE_EQUALS] = 13, [SDL_SCANCODE_BACKSPACE] = 14,
        [SDL_SCANCODE_TAB] = 15, [SDL_SCANCODE_Q] = 16, [SDL_SCANCODE_W] = 17, [SDL_SCANCODE_E] = 18, [SDL_SCANCODE_R] = 19,
        [SDL_SCANCODE_T] = 20, [SDL_SCANCODE_Y] = 21, [SDL_SCANCODE_U] = 22, [SDL_SCANCODE_I] = 23, [SDL_SCANCODE_O] = 24,
        [SDL_SCANCODE_P] = 25, [SDL_SCANCODE_LEFTBRACKET] = 26, [SDL_SCANCODE_RIGHTBRACKET] = 27, [SDL_SCANCODE_RETURN] = 28,
        [SDL_SCANCODE_LCTRL] = 29, [SDL_SCANCODE_A] = 30, [SDL_SCANCODE_S] = 31, [SDL_SCANCODE_D] = 32, [SDL_SCANCODE_F] = 33,
        [SDL_SCANCODE_G] = 34, [SDL_SCANCODE_H] = 35, [SDL_SCANCODE_J] = 36, [SDL_SCANCODE_K] = 37, [SDL_SCANCODE_L] = 38,
        [SDL_SCANCODE_SEMICOLON] = 39, [SDL_SCANCODE_APOSTROPHE] = 40, [SDL_SCANCODE_GRAVE] = 41, [SDL_SCANCODE_LSHIFT] = 42,
        [SDL_SCANCODE_BACKSLASH] = 43, [SDL_SCANCODE_Z] = 44, [SDL_SCANCODE_X] = 45, [SDL_SCANCODE_C] = 46, [SDL_SCANCODE_V] = 47,
        [SDL_SCANCODE_B] = 48, [SDL_SCANCODE_N] = 49, [SDL_SCANCODE_M] = 50, [SDL_SCANCODE_COMMA] = 51, [SDL_SCANCODE_PERIOD] = 52,
        [SDL_SCANCODE_SLASH] = 53, [SDL_SCANCODE_RSHIFT] = 54, [SDL_SCANCODE_LALT] = 56, [SDL_SCANCODE_SPACE] = 57,
        [SDL_SCANCODE_CAPSLOCK] = 58, [SDL_SCANCODE_F1] = 59, [SDL_SCANCODE_F2] = 60, [SDL_SCANCODE_F3] = 61, [SDL_SCANCODE_F4] = 62,
        [SDL_SCANCODE_F5] = 63, [SDL_SCANCODE_F6] = 64, [SDL_SCANCODE_F7] = 65, [SDL_SCANCODE_F8] = 66, [SDL_SCANCODE_F9] = 67,
        [SDL_SCANCODE_F10] = 68, [SDL_SCANCODE_F11] = 87, [SDL_SCANCODE_F12] = 88, [SDL_SCANCODE_HOME] = 71, [SDL_SCANCODE_UP] = 72,
        [SDL_SCANCODE_PAGEUP] = 73, [SDL_SCANCODE_LEFT] = 75, [SDL_SCANCODE_RIGHT] = 77, [SDL_SCANCODE_END] = 79,
        [SDL_SCANCODE_DOWN] = 80, [SDL_SCANCODE_PAGEDOWN] = 81, [SDL_SCANCODE_INSERT] = 82, [SDL_SCANCODE_DELETE] = 83 };
    return (unsigned)s < SDL_SCANCODE_COUNT ? t[s] : 0;
}

/* ---- frames ------------------------------------------------------------ */

/* The game's pixels as BGRA8 (what a little-endian XRGB frame already is). */
static int convert(const uint8_t *px, int w, int h, int pitch, int bpp, const uint32_t *pal) {
    size_t n = (size_t)w * (size_t)h;
    if (n > g_bgra_n) {
        free(g_bgra);
        g_bgra = (uint32_t *)malloc(n * 4);
        g_bgra_n = g_bgra ? n : 0;
        if (!g_bgra) return 0;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * (size_t)pitch;
        uint32_t *o = g_bgra + (size_t)y * (size_t)w;
        switch (bpp) {
        case 32:
            for (int x = 0; x < w; x++) o[x] = ((const uint32_t *)row)[x] | 0xFF000000u;
            break;
        case 24:
            for (int x = 0; x < w; x++)
                o[x] = 0xFF000000u | (uint32_t)row[3 * x + 2] << 16 | (uint32_t)row[3 * x + 1] << 8 | row[3 * x];
            break;
        case 16:
            for (int x = 0; x < w; x++) {
                uint32_t c = ((const uint16_t *)row)[x];
                uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                o[x] = 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            }
            break;
        case 8:
            for (int x = 0; x < w; x++) o[x] = 0xFF000000u | (pal ? pal[row[x]] & 0xFFFFFFu : row[x] * 0x010101u);
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static SDL_GPUTexture *make_tex(int w, int h, SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo ti = {
        .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
        .usage = usage, .width = (Uint32)w, .height = (Uint32)h,
        .layer_count_or_depth = 1, .num_levels = 1, .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    return SDL_CreateGPUTexture(g_gpu, &ti);
}

static int ensure_resources(int w, int h) {
    if (g_tex && g_tw == w && g_th == h) return 1;
    if (g_tex) SDL_ReleaseGPUTexture(g_gpu, g_tex);
    if (g_upload) SDL_ReleaseGPUTransferBuffer(g_gpu, g_upload);
    g_tex = make_tex(w, h, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    SDL_GPUTransferBufferCreateInfo bi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)(w * h * 4) };
    g_upload = SDL_CreateGPUTransferBuffer(g_gpu, &bi);
    g_tw = w, g_th = h;
    if (!g_tex || !g_upload) { fprintf(stderr, "[screen] GPU resources: %s\n", SDL_GetError()); return 0; }
    return 1;
}

/* Where the picture goes in a sw x sh swapchain, for the scaling mode. */
static void place(int w, int h, int sw, int sh) {
    double s = fmin((double)sw / w, (double)sh / h);
    if (g_mode == MODE_INTEGER && s >= 1.0) s = floor(s);
    g_dst.w = (float)floor(w * s), g_dst.h = (float)floor(h * s);
    g_dst.x = (float)floor((sw - g_dst.w) / 2), g_dst.y = (float)floor((sh - g_dst.h) / 2);
}

static void blit(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, int sw, int sh,
                 SDL_GPUTexture *dst, SDL_FRect d, SDL_GPUFilter filter, int clear) {
    SDL_GPUBlitInfo bi;
    SDL_zero(bi);
    bi.source.texture = src, bi.source.w = (Uint32)sw, bi.source.h = (Uint32)sh;
    bi.destination.texture = dst;
    bi.destination.x = (Uint32)d.x, bi.destination.y = (Uint32)d.y;
    bi.destination.w = (Uint32)d.w, bi.destination.h = (Uint32)d.h;
    bi.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    bi.clear_color = (SDL_FColor){ 0, 0, 0, 1 };
    bi.filter = filter;
    SDL_BlitGPUTexture(cmd, &bi);
}

/* Hold to the frame rate the game was made for (JOYRIDE_MAX_FPS, default
 * 60; 0 = as fast as it goes): GTA moves a step per frame. The machine is
 * given up while waiting so timer callbacks run. */
static void pace(void) {
    if (g_max_fps < 0) {
        const char *e = getenv("JOYRIDE_MAX_FPS");
        g_max_fps = e ? atoi(e) : 60;
    }
    uint64_t now = SDL_GetTicksNS();
    if (g_max_fps > 0 && g_last_flip_ns) {
        uint64_t due = g_last_flip_ns + 1000000000ull / (uint64_t)g_max_fps;
        if (now < due && due - now < 1000000000ull) {
            hle_block_begin();
            SDL_DelayPrecise(due - now);
            hle_block_end();
            now = due;
        }
    }
    g_last_flip_ns = now;
}

static void present(int flip) {
    const uint8_t *px;
    const uint32_t *pal = NULL;
    int w, h, pitch, bpp;
    unsigned seq = 0;
    if (!hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, &seq) &&
        !hle_gdi_window_frame(&px, &w, &h, &pitch, &bpp, &seq)) return;
    uint32_t now = hle_ticks_ms();
    if (!flip && !g_force && seq == g_shown_seq && now - g_last_present < 250) return;
    if (!flip && !g_force && now - g_last_present < 15) return;
    g_force = 0;
    g_shown_seq = seq, g_last_present = now;
    g_src_w = w, g_src_h = h;
    if (hle_screen_frame_hook) hle_screen_frame_hook(px, w, h, pitch, bpp);
    if (flip) pace();
    if (!g_gpu || !convert(px, w, h, pitch, bpp, pal) || !ensure_resources(w, h)) return;
    joyride_overlay_draw(g_bgra, w, h);

    void *map = SDL_MapGPUTransferBuffer(g_gpu, g_upload, true);
    if (!map) return;
    memcpy(map, g_bgra, (size_t)w * (size_t)h * 4);
    SDL_UnmapGPUTransferBuffer(g_gpu, g_upload);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_gpu);
    if (!cmd) return;
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = { .transfer_buffer = g_upload, .offset = 0,
                                       .pixels_per_row = (Uint32)w, .rows_per_layer = (Uint32)h };
    SDL_GPUTextureRegion dst = { .texture = g_tex, .w = (Uint32)w, .h = (Uint32)h, .d = 1 };
    SDL_UploadToGPUTexture(cp, &src, &dst, true);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;
    hle_block_begin();                                   /* a wait for the display: let timers run */
    int ok = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_win, &swap, &sw, &sh);
    hle_block_end();
    if (!ok || !swap || !sw || !sh) { SDL_SubmitGPUCommandBuffer(cmd); return; }

    place(w, h, (int)sw, (int)sh);
    if (g_mode == MODE_SHARP && (g_dst.w > w || g_dst.h > h)) {
        int k = (int)ceil(fmax(g_dst.w / w, g_dst.h / h));
        if (!g_pre || g_pw != w * k || g_ph != h * k) {
            if (g_pre) SDL_ReleaseGPUTexture(g_gpu, g_pre);
            g_pre = make_tex(w * k, h * k, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
            g_pw = w * k, g_ph = h * k;
        }
        if (g_pre) {
            blit(cmd, g_tex, w, h, g_pre, (SDL_FRect){ 0, 0, (float)g_pw, (float)g_ph }, SDL_GPU_FILTER_NEAREST, 0);
            blit(cmd, g_pre, g_pw, g_ph, swap, g_dst, SDL_GPU_FILTER_LINEAR, 1);
        } else {
            blit(cmd, g_tex, w, h, swap, g_dst, SDL_GPU_FILTER_LINEAR, 1);
        }
    } else {
        SDL_GPUFilter f = g_mode == MODE_SMOOTH || g_mode == MODE_SHARP ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        blit(cmd, g_tex, w, h, swap, g_dst, f, 1);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

/* The game drew into the primary (a blit, an Unlock, a Flip). */
static void primary_changed(void) {
    if (!g_headless && !pthread_equal(pthread_self(), g_video_thread)) return;
    present(joyride_flipping || hle_dd_flipping());
}
static void present_gdi(void) { present(0); }

/* the overlay asks: show the frame again (with the overlay as it is now) */
void joyride_screen_refresh(void) {
    static int busy;                      /* a script key from inside present */
    if (busy || (!g_headless && !pthread_equal(pthread_self(), g_video_thread))) return;
    busy = 1;
    g_force = 1;
    present(0);
    busy = 0;
}

/* ---- input ------------------------------------------------------------- */

/* window coordinates (points) -> game pixels, clamped to the picture */
static void to_game(float x, float y, int *gx, int *gy) {
    int w = g_src_w, h = g_src_h, bpp;
    if (w <= 0 || h <= 0) hle_dd_mode(&w, &h, &bpp);
    float d = g_win ? SDL_GetWindowPixelDensity(g_win) : 1.0f;
    x *= d, y *= d;
    if (g_dst.w <= 0 || g_dst.h <= 0 || w <= 0 || h <= 0) { *gx = (int)x, *gy = (int)y; return; }
    int ix = (int)((x - g_dst.x) * w / g_dst.w), iy = (int)((y - g_dst.y) * h / g_dst.h);
    *gx = ix < 0 ? 0 : ix >= w ? w - 1 : ix;
    *gy = iy < 0 ? 0 : iy >= h ? h - 1 : iy;
}

static void set_fullscreen(int on) {
    g_fullscreen = on;
    if (g_win) SDL_SetWindowFullscreen(g_win, on);
    if (g_win) { if (on) SDL_HideCursor(); else SDL_ShowCursor(); }
}

int joyride_screen_scaling(int set) {
    if (set >= 0 && set < MODE_COUNT) { g_mode = set; g_shown_seq = ~0u; }
    return g_mode;
}
const char *joyride_screen_scaling_name(int i) { return i >= 0 && i < MODE_COUNT ? k_modes[i] : NULL; }
int joyride_screen_fullscreen(int set) {
    if (set >= 0 && set != g_fullscreen) set_fullscreen(set);
    return g_fullscreen;
}

/* A key, from the keyboard or a script (joyride_screen_key): the overlay,
 * joyride's own keys, then the game. */
static void key_event(SDL_Scancode sc, int down, int repeat, SDL_Keymod mod) {
    if (hle_screen_key_hook && hle_screen_key_hook((int)sc, down)) return;
    if (joyride_overlay_open() && down && sc != SDL_SCANCODE_F11 &&
        joyride_overlay_key((int)sc, down, repeat)) return;
    if ((sc == SDL_SCANCODE_RETURN && (mod & SDL_KMOD_ALT)) || sc == SDL_SCANCODE_F11) {
        if (down && !repeat) {
            set_fullscreen(!g_fullscreen);
            joyride_cfg.fullscreen = g_fullscreen;
            joyride_config_save();
            if (joyride_overlay_open()) joyride_screen_refresh();
        }
        return;
    }
    if (sc == SDL_SCANCODE_F12 || joyride_overlay_open()) {
        if (joyride_overlay_key((int)sc, down, repeat)) return;
    }
    int vk = vk_of(sc);
    if (vk) hle_input_key(down, (uint32_t)vk, scan_of(sc));
}

void joyride_screen_key(int sdl_scancode, int down) { key_event((SDL_Scancode)sdl_scancode, down, 0, 0); }

void hle_screen_pump(void) {
    SDL_Event e;
    while (g_win && SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
            uint32_t hw = hle_first_hwnd();
            if (hw && !g_close_asked) { g_close_asked = 1; hle_post_message(hw, WM_CLOSE, 0, 0); }
            else hle_exit(0);                            /* asked twice, or no window to ask */
            break;
        }
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
            key_event(e.key.scancode, e.type == SDL_EVENT_KEY_DOWN, e.key.repeat, e.key.mod);
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            int gx, gy;
            to_game(e.motion.x, e.motion.y, &gx, &gy);
            hle_input_mouse(WM_MOUSEMOVE, gx, gy);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            int gx, gy, down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            to_game(e.button.x, e.button.y, &gx, &gy);
            uint32_t m = e.button.button == SDL_BUTTON_RIGHT ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                       : e.button.button == SDL_BUTTON_MIDDLE ? (down ? WM_MBUTTONDOWN : WM_MBUTTONUP)
                       : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
            hle_input_mouse(m, gx, gy);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            if (y != 0) hle_input_wheel((int)(y * 120));
            break;
        }
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_EXPOSED:
            g_shown_seq = ~0u;
            break;
        default: break;
        }
    }
    if (hle_screen_poll_hook) hle_screen_poll_hook();
    present(0);
}

/* ---- set-up -------------------------------------------------------------- */

int hle_screen_desktop(int *w, int *h) {
    if (g_headless || !SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(g_win ? SDL_GetDisplayForWindow(g_win) : SDL_GetPrimaryDisplay());
    if (!dm) return 0;
    *w = dm->w, *h = dm->h;
    return 1;
}

int hle_screen_open(const char *title, int fullscreen, int headless) {
    g_headless = headless, g_fullscreen = fullscreen;
    g_video_thread = pthread_self();
    hle_set_pump_hook(hle_screen_pump);
    hle_dd_primary_hook = primary_changed;
    hle_gdi_window_hook = present_gdi;
    const char *m = getenv("JOYRIDE_SCALE");
    for (int i = 0; m && i < MODE_COUNT; i++) if (!strcmp(m, k_modes[i])) g_mode = i;
    if (headless) return 0;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");        /* SIGINT and SIGTERM end the process */
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "[screen] SDL: %s\n", SDL_GetError()); return -1; }
    g_win = SDL_CreateWindow(title, 1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
                                               | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!g_win) { fprintf(stderr, "[screen] window: %s\n", SDL_GetError()); return -1; }
    g_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
                                getenv("JOYRIDE_GPU_DEBUG") != NULL, NULL);
    if (!g_gpu || !SDL_ClaimWindowForGPUDevice(g_gpu, g_win)) {
        fprintf(stderr, "[screen] SDL_GPU: %s\n", SDL_GetError());
        return -1;
    }
    /* Mailbox where there is one: a new frame replaces a waiting one, no
     * tearing and no stall on the display; pace() keeps the rate. */
    SDL_GPUPresentMode pm = SDL_WindowSupportsGPUPresentMode(g_gpu, g_win, SDL_GPU_PRESENTMODE_MAILBOX)
                          ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
    SDL_SetGPUSwapchainParameters(g_gpu, g_win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, pm);
    if (fullscreen) SDL_HideCursor();
    fprintf(stderr, "[screen] SDL %d.%d.%d, %s video, SDL_GPU on %s, %s present, F12 settings, F11 fullscreen\n",
            SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
            SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver(), SDL_GetGPUDeviceDriver(g_gpu),
            pm == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : "vsync");
    return 0;
}
