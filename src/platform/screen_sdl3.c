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
 * F1: the settings overlay (src/platform/overlay.c), which holds these and
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
static int convert_to(uint32_t *out, const uint8_t *px, int w, int h, int pitch, int bpp, const uint32_t *pal) {
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * (size_t)pitch;
        uint32_t *o = out + (size_t)y * (size_t)w;
        switch (bpp) {
        case 32:                                 /* the hardware renderer's key: see-through */
            for (int x = 0; x < w; x++) {
                uint32_t c = ((const uint32_t *)row)[x];
                o[x] = c == JOYRIDE_KEY ? 0 : c | 0xFF000000u;
            }
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
    g_tex = make_tex(w, h, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);   /* cleared by the hw path */
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

/* No more frames than the display shows (JOYRIDE_MAX_FPS, default the
 * display's refresh rate, else 60; 0 = as fast as it goes). The game's own
 * frames come at its 23.3 a second; smooth.c's frames between them fill
 * up to this. With the game's limiter off (F8) it moves a step a frame, so
 * this is then its speed. The machine is given up while waiting so timer
 * callbacks run. */
static int display_hz(void) {
    if (!g_win) return 60;
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_win));
    return dm && dm->refresh_rate >= 30.0f ? (int)(dm->refresh_rate + 0.5f) : 60;
}

static int g_fps_env = -1;               /* JOYRIDE_MAX_FPS given */

/* the cap now: the display's rate while smooth.c draws between steps; 60
 * otherwise (menus and the limiter off move a step a frame) */
static int cap_fps(void) {
    if (g_max_fps < 0) {
        const char *e = getenv("JOYRIDE_MAX_FPS");
        g_fps_env = e != NULL;
        g_max_fps = e ? atoi(e) : display_hz();
    }
    if (g_fps_env || joyride_smooth_running()) return g_max_fps;
    return g_max_fps > 60 ? 60 : g_max_fps;
}

static void pace(void) {
    uint64_t deadline = 0, target = joyride_smooth_target(&deadline);
    if (target) {                                    /* smooth.c's frame: its grid slot, if there is time */
        uint64_t until = target < deadline ? target : deadline, now = SDL_GetTicksNS();
        if (now < until && until - now < 1000000000ull) {
            hle_block_begin();
            SDL_DelayPrecise(until - now);
            hle_block_end();
        }
        g_last_flip_ns = target;
        return;
    }
    int fps = cap_fps();
    uint64_t now = SDL_GetTicksNS();
    if (fps > 0 && g_last_flip_ns) {
        uint64_t due = g_last_flip_ns + 1000000000ull / (uint64_t)fps;
        if (now < due && due - now < 1000000000ull) {
            hle_block_begin();
            SDL_DelayPrecise(due - now);
            hle_block_end();
            now = due;
        }
    }
    g_last_flip_ns = now;
}


/* ---- texture filtering on the GPU --------------------------------------- */

/* shaders/: GLSL compiled to SPIR-V by the build (glslc), as C arrays */
#ifdef JOYRIDE_GPU_FILTER
static const uint32_t k_vs[] =
#include "fullscreen.vert.h"
;
static const uint32_t k_fs[] =
#include "texfilter.frag.h"
;
static const uint32_t k_hw_vs[] =
#include "hwquad.vert.h"
;
static const uint32_t k_hw_fs[] =
#include "hwquad.frag.h"
;
static const uint32_t k_comp_fs[] =
#include "composite.frag.h"
;
static const uint32_t k_map_fs[] =
#include "map.frag.h"
;
#endif

static SDL_GPUGraphicsPipeline *g_fpipe, *g_hwpipe, *g_comppipe, *g_mappipe;
static SDL_GPUSampler *g_linear;
static SDL_GPUSampler *g_near;
static SDL_GPUTexture *g_gtex, *g_pages, *g_pals, *g_filt;
static int g_gtw, g_gth, g_fw, g_fh;
static SDL_GPUTransferBuffer *g_fup;
static Uint32 g_fup_cap;
static int g_filter_ok = -1;                 /* -1 not tried yet */

static int filter_init(void) {
    if (g_filter_ok >= 0) return g_filter_ok;
    g_filter_ok = 0;
#ifdef JOYRIDE_GPU_FILTER
    if (!g_gpu || !(SDL_GetGPUShaderFormats(g_gpu) & SDL_GPU_SHADERFORMAT_SPIRV)) return 0;
    if (!SDL_GPUTextureSupportsFormat(g_gpu, SDL_GPU_TEXTUREFORMAT_R8_UINT, SDL_GPU_TEXTURETYPE_2D_ARRAY, SDL_GPU_TEXTUREUSAGE_SAMPLER) ||
        !SDL_GPUTextureSupportsFormat(g_gpu, SDL_GPU_TEXTUREFORMAT_R32G32_UINT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER))
        return 0;
    SDL_GPUShaderCreateInfo vi = { .code_size = sizeof k_vs, .code = (const Uint8 *)k_vs, .entrypoint = "main",
                                   .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = SDL_GPU_SHADERSTAGE_VERTEX };
    SDL_GPUShaderCreateInfo fi = { .code_size = sizeof k_fs, .code = (const Uint8 *)k_fs, .entrypoint = "main",
                                   .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                   .num_samplers = 4 };
    SDL_GPUShader *vs = SDL_CreateGPUShader(g_gpu, &vi), *fs = SDL_CreateGPUShader(g_gpu, &fi);
    if (vs && fs) {
        SDL_GPUColorTargetDescription ct = { .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM };
        SDL_GPUGraphicsPipelineCreateInfo pi = {
            .vertex_shader = vs, .fragment_shader = fs, .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .target_info = { .color_target_descriptions = &ct, .num_color_targets = 1 },
        };
        g_fpipe = SDL_CreateGPUGraphicsPipeline(g_gpu, &pi);
    }
    if (fs) SDL_ReleaseGPUShader(g_gpu, fs);
    /* the hardware renderer: quads (blended over what is below), then the
     * CPU's pixels over them */
    SDL_GPUShaderCreateInfo hv = { .code_size = sizeof k_hw_vs, .code = (const Uint8 *)k_hw_vs, .entrypoint = "main",
                                   .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = SDL_GPU_SHADERSTAGE_VERTEX,
                                   .num_storage_buffers = 1, .num_uniform_buffers = 1 };
    SDL_GPUShaderCreateInfo hf = { .code_size = sizeof k_hw_fs, .code = (const Uint8 *)k_hw_fs, .entrypoint = "main",
                                   .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                   .num_samplers = 2, .num_storage_buffers = 1, .num_uniform_buffers = 1 };
    SDL_GPUShaderCreateInfo cf = { .code_size = sizeof k_comp_fs, .code = (const Uint8 *)k_comp_fs, .entrypoint = "main",
                                   .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                   .num_samplers = 1, .num_uniform_buffers = 1 };
    SDL_GPUShaderCreateInfo mf = { .code_size = sizeof k_map_fs, .code = (const Uint8 *)k_map_fs, .entrypoint = "main",
                                   .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
                                   .num_samplers = 1, .num_uniform_buffers = 1 };
    SDL_GPUShader *hvs = SDL_CreateGPUShader(g_gpu, &hv), *hfs = SDL_CreateGPUShader(g_gpu, &hf), *cfs = SDL_CreateGPUShader(g_gpu, &cf);
    SDL_GPUShader *mfs = SDL_CreateGPUShader(g_gpu, &mf);
    if (hvs && hfs && cfs && vs) {
        SDL_GPUColorTargetDescription blend = {
            .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
            .blend_state = { .enable_blend = true,
                             .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                             .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                             .color_blend_op = SDL_GPU_BLENDOP_ADD,
                             .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                             .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
                             .alpha_blend_op = SDL_GPU_BLENDOP_ADD },
        };
        SDL_GPUGraphicsPipelineCreateInfo pq = {
            .vertex_shader = hvs, .fragment_shader = hfs, .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .target_info = { .color_target_descriptions = &blend, .num_color_targets = 1 },
        };
        g_hwpipe = SDL_CreateGPUGraphicsPipeline(g_gpu, &pq);
        SDL_GPUColorTargetDescription plain = { .format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM };
        SDL_GPUGraphicsPipelineCreateInfo pc = {
            .vertex_shader = vs, .fragment_shader = cfs, .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
            .target_info = { .color_target_descriptions = &plain, .num_color_targets = 1 },
        };
        g_comppipe = SDL_CreateGPUGraphicsPipeline(g_gpu, &pc);
        if (mfs) {                                       /* the city map: blended over the picture */
            SDL_GPUGraphicsPipelineCreateInfo pm = {
                .vertex_shader = vs, .fragment_shader = mfs, .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
                .target_info = { .color_target_descriptions = &blend, .num_color_targets = 1 },
            };
            g_mappipe = SDL_CreateGPUGraphicsPipeline(g_gpu, &pm);
        }
    }
    if (mfs) SDL_ReleaseGPUShader(g_gpu, mfs);
    if (vs) SDL_ReleaseGPUShader(g_gpu, vs);
    if (hvs) SDL_ReleaseGPUShader(g_gpu, hvs);
    if (hfs) SDL_ReleaseGPUShader(g_gpu, hfs);
    if (cfs) SDL_ReleaseGPUShader(g_gpu, cfs);
    if (!g_hwpipe || !g_comppipe) fprintf(stderr, "[screen] hardware renderer unavailable: %s\n", SDL_GetError());
    SDL_GPUSamplerCreateInfo si = { .min_filter = SDL_GPU_FILTER_NEAREST, .mag_filter = SDL_GPU_FILTER_NEAREST,
                                    .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                                    .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                    .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                    .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE };
    g_near = SDL_CreateGPUSampler(g_gpu, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    g_linear = SDL_CreateGPUSampler(g_gpu, &si);
    SDL_GPUTextureCreateInfo pg = { .type = SDL_GPU_TEXTURETYPE_2D_ARRAY, .format = SDL_GPU_TEXTUREFORMAT_R8_UINT,
                                    .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = 256, .height = 256,
                                    .layer_count_or_depth = 1024, .num_levels = 1 };
    g_pages = SDL_CreateGPUTexture(g_gpu, &pg);    /* 256 layers is all a GPU has to allow */
    if (!g_pages) pg.layer_count_or_depth = 256, g_pages = SDL_CreateGPUTexture(g_gpu, &pg);
    joyride_tex_gpu_pages = (int)pg.layer_count_or_depth;
    g_pals = make_tex(256, 4096, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    g_filter_ok = g_fpipe && g_near && g_pages && g_pals;
    if (!g_filter_ok) fprintf(stderr, "[screen] texture filtering on the GPU unavailable: %s\n", SDL_GetError());
    joyride_gbuf_forget();
#endif
    return g_filter_ok;
}

int joyride_screen_gpu_filter(void) { return filter_init(); }

/* what changed this frame, gathered before the upload */
typedef struct { int npages, npals; int page_slot[1024]; const uint8_t *page[1024]; int pal_slot[4096]; uint32_t *pals; } changes_t;
static void on_page(void *ctx, int slot, const uint8_t *texels) {
    changes_t *c = ctx;
    if (c->npages < 1024) c->page_slot[c->npages] = slot, c->page[c->npages++] = texels;
}
static void on_pal(void *ctx, int slot, const uint32_t *colours) {
    changes_t *c = ctx;
    if (c->npals < 4096) { c->pal_slot[c->npals] = slot; memcpy(c->pals + (size_t)c->npals * 256, colours, 1024); c->npals++; }
}

/* Upload the frame's recording and what changed, and re-sample the recorded
 * pixels of g_tex into g_filt (shaders/texfilter.frag). */
static int filter_pass(SDL_GPUCommandBuffer *cmd, const uint32_t *gb, int gw, int gh, int w, int h) {
    if (!g_gtex || g_gtw != gw || g_gth != gh) {
        if (g_gtex) SDL_ReleaseGPUTexture(g_gpu, g_gtex);
        SDL_GPUTextureCreateInfo ti = { .type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R32G32_UINT,
                                        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = (Uint32)gw, .height = (Uint32)gh,
                                        .layer_count_or_depth = 1, .num_levels = 1 };
        g_gtex = SDL_CreateGPUTexture(g_gpu, &ti);
        g_gtw = gw, g_gth = gh;
    }
    if (!g_filt || g_fw != w || g_fh != h) {
        if (g_filt) SDL_ReleaseGPUTexture(g_gpu, g_filt);
        g_filt = make_tex(w, h, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        g_fw = w, g_fh = h;
    }
    if (!g_gtex || !g_filt) return 0;

    static changes_t ch;
    static uint32_t pals[4096 * 256];
    ch.npages = ch.npals = 0, ch.pals = pals;
    joyride_gbuf_changes(on_page, on_pal, &ch);

    Uint32 gsize = (Uint32)gw * (Uint32)gh * 8;
    Uint32 need = gsize + (Uint32)ch.npages * 65536u + (Uint32)ch.npals * 1024u;
    if (need > g_fup_cap) {
        if (g_fup) SDL_ReleaseGPUTransferBuffer(g_gpu, g_fup);
        SDL_GPUTransferBufferCreateInfo bi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = need + need / 4 };
        g_fup = SDL_CreateGPUTransferBuffer(g_gpu, &bi);
        g_fup_cap = g_fup ? bi.size : 0;
        if (!g_fup) return 0;
    }
    uint8_t *map = SDL_MapGPUTransferBuffer(g_gpu, g_fup, true);
    if (!map) return 0;
    memcpy(map, gb, gsize);
    Uint32 off = gsize;
    for (int i = 0; i < ch.npages; i++, off += 65536u) memcpy(map + off, ch.page[i], 65536);
    memcpy(map + off, pals, (size_t)ch.npals * 1024);
    SDL_UnmapGPUTransferBuffer(g_gpu, g_fup);

    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = { .transfer_buffer = g_fup, .offset = 0, .pixels_per_row = (Uint32)gw, .rows_per_layer = (Uint32)gh };
    SDL_GPUTextureRegion dst = { .texture = g_gtex, .w = (Uint32)gw, .h = (Uint32)gh, .d = 1 };
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    off = gsize;
    for (int i = 0; i < ch.npages; i++, off += 65536u) {
        SDL_GPUTextureTransferInfo s2 = { .transfer_buffer = g_fup, .offset = off, .pixels_per_row = 256, .rows_per_layer = 256 };
        SDL_GPUTextureRegion d2 = { .texture = g_pages, .layer = (Uint32)ch.page_slot[i], .w = 256, .h = 256, .d = 1 };
        SDL_UploadToGPUTexture(cp, &s2, &d2, false);
    }
    for (int i = 0; i < ch.npals; i++, off += 1024u) {
        SDL_GPUTextureTransferInfo s2 = { .transfer_buffer = g_fup, .offset = off, .pixels_per_row = 256, .rows_per_layer = 1 };
        SDL_GPUTextureRegion d2 = { .texture = g_pals, .y = (Uint32)ch.pal_slot[i], .w = 256, .h = 1, .d = 1 };
        SDL_UploadToGPUTexture(cp, &s2, &d2, false);
    }
    SDL_EndGPUCopyPass(cp);

    SDL_GPUColorTargetInfo ct = { .texture = g_filt, .load_op = SDL_GPU_LOADOP_DONT_CARE, .store_op = SDL_GPU_STOREOP_STORE };
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!rp) return 0;
    SDL_BindGPUGraphicsPipeline(rp, g_fpipe);
    SDL_GPUTextureSamplerBinding b[4] = { { g_tex, g_near }, { g_gtex, g_near }, { g_pages, g_near }, { g_pals, g_near } };
    SDL_BindGPUFragmentSamplers(rp, 0, b, 4);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
    return 1;
}

/* ---- the hardware renderer ------------------------------------------------ */

static SDL_GPUTexture *g_world;                  /* the frame, drawn by the GPU */
static int g_ww, g_wh;
static SDL_GPUBuffer *g_qbuf;
static Uint32 g_qcap;
static SDL_GPUTransferBuffer *g_qup;
static Uint32 g_qup_cap;

int joyride_screen_hwrender(void) { return filter_init() && g_hwpipe && g_comppipe; }

/* Draw the frame's quads (hwrender.c) and the CPU's pixels of g_tex over
 * them into g_world; *ow, *oh: its size. */
static int hw_pass(SDL_GPUCommandBuffer *cmd, const joyride_quad *quads, int n, int w, int h, int *ow, int *oh) {
    if (!joyride_screen_hwrender()) return 0;
    int scale = 1;
    int tw = w * scale, th = h * scale;
    if (!g_world || g_ww != tw || g_wh != th) {
        if (g_world) SDL_ReleaseGPUTexture(g_gpu, g_world);
        g_world = make_tex(tw, th, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        g_ww = tw, g_wh = th;
        if (!g_world) return 0;
    }
    /* pages and palettes that changed, as for the filter */
    static changes_t ch;
    static uint32_t pals[4096 * 256];
    ch.npages = ch.npals = 0, ch.pals = pals;
    joyride_gbuf_changes(on_page, on_pal, &ch);
    Uint32 qsize = (Uint32)(n > 0 ? n : 1) * (Uint32)sizeof(joyride_quad);
    if (qsize > g_qcap) {
        if (g_qbuf) SDL_ReleaseGPUBuffer(g_gpu, g_qbuf);
        SDL_GPUBufferCreateInfo bi = { .usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, .size = qsize * 2 };
        g_qbuf = SDL_CreateGPUBuffer(g_gpu, &bi);
        g_qcap = g_qbuf ? bi.size : 0;
        if (!g_qbuf) return 0;
    }
    Uint32 need = qsize + (Uint32)ch.npages * 65536u + (Uint32)ch.npals * 1024u;
    if (need > g_qup_cap) {
        if (g_qup) SDL_ReleaseGPUTransferBuffer(g_gpu, g_qup);
        SDL_GPUTransferBufferCreateInfo bi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = need + need / 4 };
        g_qup = SDL_CreateGPUTransferBuffer(g_gpu, &bi);
        g_qup_cap = g_qup ? bi.size : 0;
        if (!g_qup) return 0;
    }
    uint8_t *map = SDL_MapGPUTransferBuffer(g_gpu, g_qup, true);
    if (!map) return 0;
    if (n > 0) memcpy(map, quads, (size_t)n * sizeof(joyride_quad));
    Uint32 off = qsize;
    for (int i = 0; i < ch.npages; i++, off += 65536u) memcpy(map + off, ch.page[i], 65536);
    memcpy(map + off, pals, (size_t)ch.npals * 1024);
    SDL_UnmapGPUTransferBuffer(g_gpu, g_qup);

    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    if (n > 0) {
        SDL_GPUTransferBufferLocation src = { .transfer_buffer = g_qup, .offset = 0 };
        SDL_GPUBufferRegion dst = { .buffer = g_qbuf, .offset = 0, .size = (Uint32)n * (Uint32)sizeof(joyride_quad) };
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    }
    off = qsize;
    for (int i = 0; i < ch.npages; i++, off += 65536u) {
        SDL_GPUTextureTransferInfo s2 = { .transfer_buffer = g_qup, .offset = off, .pixels_per_row = 256, .rows_per_layer = 256 };
        SDL_GPUTextureRegion d2 = { .texture = g_pages, .layer = (Uint32)ch.page_slot[i], .w = 256, .h = 256, .d = 1 };
        SDL_UploadToGPUTexture(cp, &s2, &d2, false);
    }
    for (int i = 0; i < ch.npals; i++, off += 1024u) {
        SDL_GPUTextureTransferInfo s2 = { .transfer_buffer = g_qup, .offset = off, .pixels_per_row = 256, .rows_per_layer = 1 };
        SDL_GPUTextureRegion d2 = { .texture = g_pals, .y = (Uint32)ch.pal_slot[i], .w = 256, .h = 1, .d = 1 };
        SDL_UploadToGPUTexture(cp, &s2, &d2, false);
    }
    SDL_EndGPUCopyPass(cp);

    SDL_GPUColorTargetInfo ct = { .texture = g_world, .load_op = SDL_GPU_LOADOP_CLEAR,
                                  .clear_color = { 0, 0, 0, 1 }, .store_op = SDL_GPU_STOREOP_STORE };
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!rp) return 0;
    float ox, oy;                                        /* the camera's rounding (smooth.c) */
    joyride_hw_offset(&ox, &oy);
    float view[4] = { (float)w, (float)h, ox, oy };
    float prm[4] = { (float)scale, joyride_texfilter ? 1.0f : 0.0f, ox, oy };
    if (n > 0) {
        SDL_BindGPUGraphicsPipeline(rp, g_hwpipe);
        SDL_BindGPUVertexStorageBuffers(rp, 0, &g_qbuf, 1);
        SDL_GPUTextureSamplerBinding b[2] = { { g_pages, g_near }, { g_pals, g_near } };
        SDL_BindGPUFragmentSamplers(rp, 0, b, 2);
        SDL_BindGPUFragmentStorageBuffers(rp, 0, &g_qbuf, 1);
        SDL_PushGPUVertexUniformData(cmd, 0, view, sizeof view);
        SDL_PushGPUFragmentUniformData(cmd, 0, prm, sizeof prm);
        SDL_DrawGPUPrimitives(rp, 6, (Uint32)n, 0, 0);
    }
    SDL_BindGPUGraphicsPipeline(rp, g_comppipe);
    SDL_GPUTextureSamplerBinding fb = { g_tex, g_near };
    SDL_BindGPUFragmentSamplers(rp, 0, &fb, 1);
    SDL_PushGPUFragmentUniformData(cmd, 0, prm, sizeof prm);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
    *ow = tw, *oh = th;
    return 1;
}

/* ---- the city map (src/platform/map.c) ------------------------------------ */

static SDL_GPUTexture *g_maptex;
static int g_map_id = -1;

/* Over the picture `pic` (pw x ph), when the map is open. */
static void map_pass(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *pic, int pw, int ph) {
    if (!joyride_map_open() || !filter_init() || !g_mappipe || !g_linear) return;
    int size, id;
    const uint8_t *m = joyride_map_picture(&size, &id);
    if (!m) return;
    if (!g_maptex || id != g_map_id) {                   /* a city's picture goes up once */
        if (g_maptex) SDL_ReleaseGPUTexture(g_gpu, g_maptex);
        g_maptex = make_tex(size, size, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        SDL_GPUTransferBufferCreateInfo bi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)(size * size * 4) };
        SDL_GPUTransferBuffer *up = g_maptex ? SDL_CreateGPUTransferBuffer(g_gpu, &bi) : NULL;
        void *map = up ? SDL_MapGPUTransferBuffer(g_gpu, up, false) : NULL;
        if (!map) { if (up) SDL_ReleaseGPUTransferBuffer(g_gpu, up); return; }
        memcpy(map, m, (size_t)size * size * 4);
        SDL_UnmapGPUTransferBuffer(g_gpu, up);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src = { .transfer_buffer = up, .pixels_per_row = (Uint32)size, .rows_per_layer = (Uint32)size };
        SDL_GPUTextureRegion dst = { .texture = g_maptex, .w = (Uint32)size, .h = (Uint32)size, .d = 1 };
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
        SDL_ReleaseGPUTransferBuffer(g_gpu, up);
        g_map_id = id;
    }
    struct { float rect[4], view[4], player[4], obj[8][4], count[4]; } u = { 0 };
    float side = (float)(pw < ph ? pw : ph) * 0.96f;
    u.rect[0] = ((float)pw - side) / 2, u.rect[1] = ((float)ph - side) / 2, u.rect[2] = side;
    u.rect[3] = side / 160.0f < 3.0f ? 3.0f : side / 160.0f;
    float pl[2], ob[8][2];
    int n = joyride_map_dots(pl, ob, 8);
    u.view[0] = 0, u.view[1] = 0, u.view[2] = 1;             /* the whole city */
    u.player[0] = pl[0], u.player[1] = pl[1], u.player[2] = 1;
    for (int i = 0; i < n; i++) u.obj[i][0] = ob[i][0], u.obj[i][1] = ob[i][1];
    u.count[0] = (float)n;
    SDL_GPUColorTargetInfo ct = { .texture = pic, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE };
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (!rp) return;
    SDL_BindGPUGraphicsPipeline(rp, g_mappipe);
    SDL_GPUTextureSamplerBinding b = { g_maptex, g_linear };
    SDL_BindGPUFragmentSamplers(rp, 0, &b, 1);
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
    SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    SDL_EndGPURenderPass(rp);
}

/* JOYRIDE_HEADLESS_GPU=1 with JOYRIDE_GPU_DUMP=<dir>: no window, but the GPU
 * filters, and the result is read back and saved, once a second (tests). */
static void headless_dump(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, int w, int h) {
    static const char *dir;
    static uint32_t last;
    static int n;
    if (!dir) dir = getenv("JOYRIDE_GPU_DUMP");
    uint32_t now = hle_ticks_ms();
    if (!dir || (last && now - last < 1000)) { SDL_SubmitGPUCommandBuffer(cmd); return; }
    last = now;
    SDL_GPUTransferBufferCreateInfo bi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = (Uint32)(w * h * 4) };
    SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(g_gpu, &bi);
    if (!down) { SDL_SubmitGPUCommandBuffer(cmd); return; }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion r = { .texture = src, .w = (Uint32)w, .h = (Uint32)h, .d = 1 };
    SDL_GPUTextureTransferInfo t = { .transfer_buffer = down, .pixels_per_row = (Uint32)w, .rows_per_layer = (Uint32)h };
    SDL_DownloadFromGPUTexture(cp, &r, &t);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (f) { SDL_WaitForGPUFences(g_gpu, true, &f, 1); SDL_ReleaseGPUFence(g_gpu, f); }
    const uint8_t *px = SDL_MapGPUTransferBuffer(g_gpu, down, false);
    char path[1024];
    snprintf(path, sizeof path, "%s/gpu_%04d.ppm", dir, n++);
    FILE *fo = px ? fopen(path, "wb") : NULL;
    if (fo) {
        fprintf(fo, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; i++) { uint8_t rgb[3] = { px[4 * i + 2], px[4 * i + 1], px[4 * i] }; fwrite(rgb, 1, 3, fo); }
        fclose(fo);
    }
    if (px) SDL_UnmapGPUTransferBuffer(g_gpu, down);
    SDL_ReleaseGPUTransferBuffer(g_gpu, down);
}

/* debug.c's report: the next SNAP_FRAMES frames presented are kept, as the
 * GPU made them (screen_NN.png), with the game's surface for the first
 * (cpu.png); each is copied in memory as it goes by and a thread writes the
 * PNGs after, so the frames keep coming at their pace. frames.txt says when
 * each was presented (the report's FRAMES record has the same clock). */
#define SNAP_FRAMES 10
static char g_snap[4300];
static int g_snap_left, g_snap_n;
typedef struct { uint8_t *px; int w, h, pitch; char path[4400]; } shot_t;
static shot_t g_shots[SNAP_FRAMES + 1];
static FILE *g_snap_log;
static uint64_t g_snap_t0;

void joyride_screen_snapshot(const char *dir) {
    if (g_snap_left) return;                               /* one at a time */
    snprintf(g_snap, sizeof g_snap, "%s", dir);
    char path[4400];
    snprintf(path, sizeof path, "%s/frames.txt", dir);
    g_snap_log = fopen(path, "w");
    if (g_snap_log) fprintf(g_snap_log, "frame ms_after_press ns(SDL) kind\n");
    g_snap_t0 = SDL_GetTicksNS();
    g_snap_left = SNAP_FRAMES, g_snap_n = 0;
}

/* the writer has its own copy of the list: a report asked for while it
 * still writes the last one's (seconds at 4K) starts a list of its own */
typedef struct { int n; shot_t s[SNAP_FRAMES + 1]; } shots_t;
static int write_shots(void *arg) {
    shots_t *l = arg;
    for (int i = 0; i < l->n; i++) {
        SDL_Surface *sf = SDL_CreateSurfaceFrom(l->s[i].w, l->s[i].h, SDL_PIXELFORMAT_XRGB8888, l->s[i].px, l->s[i].pitch);
        if (sf) SDL_SavePNG(sf, l->s[i].path), SDL_DestroySurface(sf);
        free(l->s[i].px);
    }
    fprintf(stderr, "[debug] %d frames saved\n", l->n);
    free(l);
    return 0;
}

static void keep_shot(const uint8_t *px, int w, int h, int pitch, const char *name) {
    if (g_snap_n > SNAP_FRAMES) return;
    uint8_t *copy = malloc((size_t)pitch * (size_t)h);
    if (!copy) return;
    memcpy(copy, px, (size_t)pitch * (size_t)h);
    g_shots[g_snap_n].px = copy, g_shots[g_snap_n].w = w, g_shots[g_snap_n].h = h, g_shots[g_snap_n].pitch = pitch;
    snprintf(g_shots[g_snap_n].path, sizeof g_shots[g_snap_n].path, "%s/%s", g_snap, name);
    g_snap_n++;
}

/* `cmd` is submitted with `src` read back and kept; returns a new one */
static SDL_GPUCommandBuffer *snapshot(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *src, int w, int h, const char *name) {
    SDL_GPUTransferBufferCreateInfo bi = { .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = (Uint32)(w * h * 4) };
    SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(g_gpu, &bi);
    if (!down) return cmd;
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion r = { .texture = src, .w = (Uint32)w, .h = (Uint32)h, .d = 1 };
    SDL_GPUTextureTransferInfo t = { .transfer_buffer = down, .pixels_per_row = (Uint32)w, .rows_per_layer = (Uint32)h };
    SDL_DownloadFromGPUTexture(cp, &r, &t);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (f) { SDL_WaitForGPUFences(g_gpu, true, &f, 1); SDL_ReleaseGPUFence(g_gpu, f); }
    const uint8_t *px = SDL_MapGPUTransferBuffer(g_gpu, down, false);
    if (px) keep_shot(px, w, h, w * 4, name), SDL_UnmapGPUTransferBuffer(g_gpu, down);
    SDL_ReleaseGPUTransferBuffer(g_gpu, down);
    return SDL_AcquireGPUCommandBuffer(g_gpu);
}

/* in the presenter, once the frame is made: keep it while a capture runs */
static SDL_GPUCommandBuffer *snap_frame(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *pic, int pw, int ph,
                                        const uint8_t *px, int w, int h, int pitch, int bpp, int kind) {
    if (!g_snap_left) return cmd;
    int i = SNAP_FRAMES - g_snap_left;
    char name[64];
    if (i == 0 && bpp == 32) keep_shot(px, w, h, pitch, "cpu.png");
    snprintf(name, sizeof name, "screen_%02d.png", i);
    if (g_snap_log) fprintf(g_snap_log, "%d %.2f %llu %d\n", i, (SDL_GetTicksNS() - g_snap_t0) / 1e6,
                            (unsigned long long)SDL_GetTicksNS(), kind);
    if (bpp == 32) cmd = snapshot(cmd, pic, pw, ph, name);
    if (--g_snap_left == 0) {
        if (g_snap_log) fclose(g_snap_log), g_snap_log = NULL;
        shots_t *l = malloc(sizeof *l);
        if (l) {
            l->n = g_snap_n;
            memcpy(l->s, g_shots, sizeof g_shots);
            SDL_Thread *t = SDL_CreateThread(write_shots, "joyride-shots", l);
            if (t) SDL_DetachThread(t);
            else write_shots(l);
        } else for (int k = 0; k < g_snap_n; k++) free(g_shots[k].px);
        g_snap_n = 0;
    }
    return cmd;
}

/* the time between two frames at the cap (0 = uncapped); smooth.c's grid */
uint64_t joyride_screen_frame_ns(void) {
    int fps = cap_fps();
    return fps > 0 ? 1000000000ull / (uint64_t)fps : 0;
}

/* The hardware renderer's CPU layer: the surface is the key colour except
 * where the CPU drew (HUD, text). Find that by bands of 64 rows (pairs of
 * pixels compared at once), tell hwrender.c (its next clear only puts those
 * back), and return each band's extent: only they are converted and sent. */
#define HW_BAND 64
typedef struct { int x0, x1; } band_t;
static int hw_scan(const uint8_t *px, int w, int h, int pitch, band_t *bands) {
    static uint32_t *keyrow;                           /* a row of nothing but the key */
    static int keyw;
    if (keyw < w) {
        free(keyrow);
        keyrow = malloc((size_t)w * 4);
        if (!keyrow) { keyw = 0; return 0; }
        for (int i = 0; i < w; i++) keyrow[i] = JOYRIDE_KEY;
        keyw = w;
    }
    uint32_t va = hle_va(px);
    int nb = (h + HW_BAND - 1) / HW_BAND;
    for (int b = 0; b < nb; b++) {
        int x0 = w, x1 = -1;
        for (int y = b * HW_BAND; y < h && y < (b + 1) * HW_BAND; y++) {
            const uint32_t *row = (const uint32_t *)(px + (size_t)y * (size_t)pitch);
            if (!memcmp(row, keyrow, (size_t)w * 4)) continue;   /* most rows: nothing (vectorised) */
            int i = 0;
            while (i < x0 && row[i] == JOYRIDE_KEY) i++;
            if (i < x0) x0 = i;
            int j = w - 1;
            while (j > x1 && row[j] == JOYRIDE_KEY) j--;
            if (j > x1) x1 = j;
        }
        if (x1 >= x0) joyride_hw_mark(va, b, x0, x1);
        if (bands) bands[b].x0 = x0, bands[b].x1 = x1;
    }
    return nb;
}

/* ---- the HUD, larger --------------------------------------------------- */

/* The game draws its HUD at the size it had at 640 x 480, at any screen
 * size: at 4K a score is 25 pixels high. With the GPU renderer the CPU's
 * layer is nothing but the HUD and the game's text, so it is enlarged here,
 * by whole times (pixels stay sharp), piece by piece: the layer's pieces
 * (what is drawn, with gaps of up to 16 pixels closed) found on a grid of
 * 8 x 8 cells; a piece near an edge of the screen stays on that edge (the
 * score in its corner), one away from the edges grows about its middle
 * (WASTED!, the mission text). */
int joyride_screen_hud_scale(int h) {
    if (joyride_cfg.hud > 0) return joyride_cfg.hud;
    int k = (h + 360) / 720;                          /* 4K: 3, 1440p and 1080p: 2 */
    return k < 1 ? 1 : k;
}

#define HUD_CELL 8
#define HUD_MAXBOX 64
typedef struct { int x, y, w, h; int dx, dy, k; } hudbox_t;
static int hud_boxes(const uint8_t *px, int w, int h, int pitch, const band_t *bands, int nb, int k, hudbox_t *box) {
    static uint8_t *grid;
    static int *stack, gcap;
    int gw = (w + HUD_CELL - 1) / HUD_CELL, gh = (h + HUD_CELL - 1) / HUD_CELL;
    if (gw * gh > gcap) {
        free(grid), free(stack);
        grid = malloc((size_t)gw * gh), stack = malloc(sizeof(int) * (size_t)gw * gh);
        gcap = grid && stack ? gw * gh : 0;
        if (!gcap) return -1;
    }
    memset(grid, 0, (size_t)gw * gh);
    for (int b = 0; b < nb; b++) {                    /* 1: something drawn in the cell */
        if (bands[b].x1 < bands[b].x0) continue;
        for (int y = b * HW_BAND; y < h && y < (b + 1) * HW_BAND; y++) {
            const uint32_t *row = (const uint32_t *)(px + (size_t)y * (size_t)pitch);
            uint8_t *g = grid + (size_t)(y / HUD_CELL) * gw;
            for (int x = bands[b].x0; x <= bands[b].x1; x++)
                if (row[x] != JOYRIDE_KEY) g[x / HUD_CELL] = 1, x |= HUD_CELL - 1;   /* the cell is known: on to the next */
        }
    }
    int n = 0;
    for (int c = 0; c < gw * gh; c++) {               /* the pieces: 2 marks one seen */
        if (grid[c] != 1) continue;
        if (n == HUD_MAXBOX) return -1;
        int sp = 0, x0 = gw, y0 = gh, x1 = -1, y1 = -1;
        stack[sp++] = c, grid[c] = 2;
        while (sp) {
            int e = stack[--sp], ex = e % gw, ey = e / gw;
            if (ex < x0) x0 = ex;
            if (ex > x1) x1 = ex;
            if (ey < y0) y0 = ey;
            if (ey > y1) y1 = ey;
            for (int yy = ey - 2; yy <= ey + 2; yy++)
                for (int xx = ex - 2; xx <= ex + 2; xx++)
                    if (xx >= 0 && yy >= 0 && xx < gw && yy < gh && grid[yy * gw + xx] == 1)
                        grid[yy * gw + xx] = 2, stack[sp++] = yy * gw + xx;
        }
        hudbox_t *o = &box[n++];
        o->x = x0 * HUD_CELL, o->y = y0 * HUD_CELL;
        o->w = (x1 + 1) * HUD_CELL > w ? w - o->x : (x1 + 1 - x0) * HUD_CELL;
        o->h = (y1 + 1) * HUD_CELL > h ? h - o->y : (y1 + 1 - y0) * HUD_CELL;
        int kk = k;
        while (kk > 1 && (o->w * kk > w || o->h * kk > h)) kk--;
        o->k = kk;
        int dw = o->w * kk, dh = o->h * kk;
        /* on an edge it is near (within 15% of the screen), else about its middle */
        if (o->x < w * 15 / 100) o->dx = o->x * kk;
        else if (o->x + o->w > w * 85 / 100) o->dx = w - (w - o->x) * kk;
        else o->dx = o->x + o->w / 2 - dw / 2;
        if (o->y < h * 15 / 100) o->dy = o->y * kk;
        else if (o->y + o->h > h * 85 / 100) o->dy = h - (h - o->y) * kk;
        else o->dy = o->y + o->h / 2 - dh / 2;
        if (o->dx < 0) o->dx = 0;
        if (o->dx + dw > w) o->dx = w - dw;
        if (o->dy < 0) o->dy = 0;
        if (o->dy + dh > h) o->dy = h - dh;
    }
    return n;
}

/* Upload only the scanned bands of the CPU layer, over a frame texture
 * cleared to transparent (the composite pass keeps what is opaque). */
static int hw_upload(SDL_GPUCommandBuffer *cmd, const uint8_t *px, int w, int h, int pitch) {
    static band_t bands[4096 / HW_BAND + 1];
    static hudbox_t box[HUD_MAXBOX];
    if ((h + HW_BAND - 1) / HW_BAND > (int)(sizeof bands / sizeof *bands)) return 0;
    int nb = hw_scan(px, w, h, pitch, bands);
    int k = joyride_screen_hud_scale(h), nbox = k > 1 ? hud_boxes(px, w, h, pitch, bands, nb, k, box) : -1;
    if (nbox >= 0) {                                  /* the pieces, larger, if they fit the buffer */
        size_t need = 0;
        for (int i = 0; i < nbox; i++) need += (size_t)box[i].w * box[i].k * (size_t)box[i].h * box[i].k * 4;
        if (need > (size_t)w * h * 4) nbox = -1;
    }
    if (nbox >= 0) {
        uint8_t *map = SDL_MapGPUTransferBuffer(g_gpu, g_upload, true);
        if (!map) return 0;
        size_t off = 0;
        for (int i = 0; i < nbox; i++) {
            const hudbox_t *b = &box[i];
            int dw = b->w * b->k;
            for (int y = 0; y < b->h; y++) {
                const uint32_t *row = (const uint32_t *)(px + (size_t)(b->y + y) * (size_t)pitch) + b->x;
                uint32_t *o = (uint32_t *)(map + off);
                for (int x = 0; x < b->w; x++) {
                    uint32_t v = row[x] == JOYRIDE_KEY ? 0 : row[x] | 0xFF000000u;
                    for (int r = 0; r < b->k; r++) o[x * b->k + r] = v;
                }
                for (int r = 1; r < b->k; r++) memcpy(o + (size_t)r * dw, o, (size_t)dw * 4);
                off += (size_t)dw * b->k * 4;
            }
        }
        SDL_UnmapGPUTransferBuffer(g_gpu, g_upload);
        SDL_GPUColorTargetInfo ct = { .texture = g_tex, .load_op = SDL_GPU_LOADOP_CLEAR,
                                      .clear_color = { 0, 0, 0, 0 }, .store_op = SDL_GPU_STOREOP_STORE };
        SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
        if (rp) SDL_EndGPURenderPass(rp);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        off = 0;
        for (int i = 0; i < nbox; i++) {
            const hudbox_t *b = &box[i];
            int dw = b->w * b->k, dh = b->h * b->k;
            SDL_GPUTextureTransferInfo src = { .transfer_buffer = g_upload, .offset = (Uint32)off,
                                               .pixels_per_row = (Uint32)dw, .rows_per_layer = (Uint32)dh };
            SDL_GPUTextureRegion dst = { .texture = g_tex, .x = (Uint32)b->dx, .y = (Uint32)b->dy,
                                         .w = (Uint32)dw, .h = (Uint32)dh, .d = 1 };
            SDL_UploadToGPUTexture(cp, &src, &dst, false);
            off += (size_t)dw * dh * 4;
        }
        SDL_EndGPUCopyPass(cp);
        return 1;
    }
    uint8_t *map = SDL_MapGPUTransferBuffer(g_gpu, g_upload, true);
    if (!map) return 0;
    size_t off = 0;
    for (int b = 0; b < nb; b++) {
        if (bands[b].x1 < bands[b].x0) continue;
        int bw = bands[b].x1 - bands[b].x0 + 1;
        for (int y = b * HW_BAND; y < h && y < (b + 1) * HW_BAND; y++) {
            const uint32_t *row = (const uint32_t *)(px + (size_t)y * (size_t)pitch) + bands[b].x0;
            uint32_t *o = (uint32_t *)(map + off);
            for (int x = 0; x < bw; x++) o[x] = row[x] == JOYRIDE_KEY ? 0 : row[x] | 0xFF000000u;
            off += (size_t)bw * 4;
        }
    }
    SDL_UnmapGPUTransferBuffer(g_gpu, g_upload);
    SDL_GPUColorTargetInfo ct = { .texture = g_tex, .load_op = SDL_GPU_LOADOP_CLEAR,
                                  .clear_color = { 0, 0, 0, 0 }, .store_op = SDL_GPU_STOREOP_STORE };
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
    if (rp) SDL_EndGPURenderPass(rp);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    off = 0;
    for (int b = 0; b < nb; b++) {
        if (bands[b].x1 < bands[b].x0) continue;
        int bw = bands[b].x1 - bands[b].x0 + 1, y0 = b * HW_BAND, rows = (y0 + HW_BAND <= h ? HW_BAND : h - y0);
        SDL_GPUTextureTransferInfo src = { .transfer_buffer = g_upload, .offset = (Uint32)off,
                                           .pixels_per_row = (Uint32)bw, .rows_per_layer = (Uint32)rows };
        SDL_GPUTextureRegion dst = { .texture = g_tex, .x = (Uint32)bands[b].x0, .y = (Uint32)y0,
                                     .w = (Uint32)bw, .h = (Uint32)rows, .d = 1 };
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        off += (size_t)bw * (size_t)rows * 4;
    }
    SDL_EndGPUCopyPass(cp);
    return 1;
}

static void present(int flip) {
    const uint8_t *px;
    const uint32_t *pal = NULL;
    int w, h, pitch, bpp;
    unsigned seq = 0;
    if (!hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, &seq) &&
        !hle_gdi_window_frame(&px, &w, &h, &pitch, &bpp, &seq)) return;
    if (flip && joyride_smooth_skip()) {                 /* smooth.c draws this moment itself */
        if (joyride_hw_active() && bpp == 32) hw_scan(px, w, h, pitch, NULL);   /* what to clear next */
        return;
    }
    /* With the GPU renderer, a frame is the world's quads over the CPU's
     * layer, and a draw's quads are used once. A refresh that is not a flip
     * (the window pump's, when 15 ms pass between flips: a step's update
     * at 4K takes about that) has no quads, and showed the CPU layer alone:
     * the HUD over black, a flash. The last frame stays on screen instead. */
    static int last_was_gpu;
    const joyride_quad *pending;
    if (!flip && last_was_gpu && bpp == 32 && joyride_hw_quads(&pending) < 0 && !joyride_overlay_open()) return;
    uint32_t now = hle_ticks_ms();
    if (!flip && !g_force && seq == g_shown_seq && now - g_last_present < 250) return;
    if (!flip && !g_force && now - g_last_present < 15) return;
    g_force = 0;
    g_shown_seq = seq, g_last_present = now;
    g_src_w = w, g_src_h = h;
    if (hle_screen_frame_hook) hle_screen_frame_hook(px, w, h, pitch, bpp);
    if (flip) pace();
    if (!g_gpu || !ensure_resources(w, h)) return;
    SDL_GPUCommandBuffer *cmd;
    if (joyride_hw_active() && bpp == 32 && !joyride_overlay_open()) {
        /* the hardware renderer: only the CPU's few pixels go up */
        if (!(cmd = SDL_AcquireGPUCommandBuffer(g_gpu))) return;
        if (!hw_upload(cmd, px, w, h, pitch)) { SDL_SubmitGPUCommandBuffer(cmd); return; }
    } else {
        /* converted straight into the upload buffer, the overlay drawn on it */
        uint32_t *map = SDL_MapGPUTransferBuffer(g_gpu, g_upload, true);
        if (!map) return;
        int ok = convert_to(map, px, w, h, pitch, bpp, pal);
        if (ok) joyride_overlay_draw(map, w, h);
        SDL_UnmapGPUTransferBuffer(g_gpu, g_upload);
        if (!ok) return;
        if (joyride_hw_active() && bpp == 32) hw_scan(px, w, h, pitch, NULL);
        if (!(cmd = SDL_AcquireGPUCommandBuffer(g_gpu))) return;
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src = { .transfer_buffer = g_upload, .offset = 0,
                                           .pixels_per_row = (Uint32)w, .rows_per_layer = (Uint32)h };
        SDL_GPUTextureRegion dst = { .texture = g_tex, .w = (Uint32)w, .h = (Uint32)h, .d = 1 };
        SDL_UploadToGPUTexture(cp, &src, &dst, true);
        SDL_EndGPUCopyPass(cp);
    }

    SDL_GPUTexture *pic = g_tex;                         /* what goes to the window */
    int pw = w, ph = h;                                  /* its size */
    if (bpp == 32) joyride_gbuf_size(pitch / 4, h);      /* texfilter.c and hwrender.c use this */
    const joyride_quad *quads;
    int nq = joyride_hw_quads(&quads);
    last_was_gpu = nq >= 0 || (last_was_gpu && !flip);
    if (nq >= 0) {                                       /* the world was drawn by the GPU */
        if (hw_pass(cmd, quads, nq, w, h, &pw, &ph)) pic = g_world;
        joyride_hw_done();
    } else if (joyride_texfilter == 2 && filter_init()) {
        const uint32_t *gb;
        int gw, gh;
        if (joyride_gbuf_frame(&gb, &gw, &gh) && gw >= w && gh >= h && filter_pass(cmd, gb, gw, gh, w, h)) pic = g_filt;
    }
    joyride_gbuf_done();
    map_pass(cmd, pic, pw, ph);
    if (g_snap_left && !(cmd = snap_frame(cmd, pic, pw, ph, px, w, h, pitch, bpp, joyride_smooth_frame_kind()))) return;
    joyride_debug_frame(2, 0, 0);                        /* debug.c's record: presented */
    if (!g_win) { headless_dump(cmd, pic, pw, ph); return; }

    SDL_GPUTexture *swap = NULL;
    Uint32 sw = 0, sh = 0;
    hle_block_begin();                                   /* a wait for the display: let timers run */
    int ok = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g_win, &swap, &sw, &sh);
    hle_block_end();
    if (!ok || !swap || !sw || !sh) { SDL_SubmitGPUCommandBuffer(cmd); return; }

    place(w, h, (int)sw, (int)sh);                       /* the game's size sets the shape */
    if (g_mode == MODE_SHARP && (g_dst.w > pw || g_dst.h > ph)) {
        int k = (int)ceil(fmax(g_dst.w / pw, g_dst.h / ph));
        if (!g_pre || g_pw != pw * k || g_ph != ph * k) {
            if (g_pre) SDL_ReleaseGPUTexture(g_gpu, g_pre);
            g_pre = make_tex(pw * k, ph * k, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
            g_pw = pw * k, g_ph = ph * k;
        }
        if (g_pre) {
            blit(cmd, pic, pw, ph, g_pre, (SDL_FRect){ 0, 0, (float)g_pw, (float)g_ph }, SDL_GPU_FILTER_NEAREST, 0);
            blit(cmd, g_pre, g_pw, g_ph, swap, g_dst, SDL_GPU_FILTER_LINEAR, 1);
        } else {
            blit(cmd, pic, pw, ph, swap, g_dst, SDL_GPU_FILTER_LINEAR, 1);
        }
    } else {
        SDL_GPUFilter f = g_mode == MODE_SMOOTH || g_mode == MODE_SHARP ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        blit(cmd, pic, pw, ph, swap, g_dst, f, 1);
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
    if (sc == SDL_SCANCODE_F1 || joyride_overlay_open()) {
        if (joyride_overlay_key((int)sc, down, repeat)) return;
    }
    if (sc == SDL_SCANCODE_F7 || sc == SDL_SCANCODE_F8) {   /* debugging (debug.c) */
        if (down && !repeat) sc == SDL_SCANCODE_F7 ? joyride_debug_report() : joyride_debug_give();
        return;
    }
    if (sc == SDL_SCANCODE_M) {                          /* the city map (map.c) */
        if (down && !repeat) joyride_map_toggle();
        return;
    }
    if (sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_KP_PLUS || sc == SDL_SCANCODE_MINUS || sc == SDL_SCANCODE_KP_MINUS) {
        int dir = sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_KP_PLUS ? +1 : -1;
        if (down) joyride_camera_zoom(dir);
        return;                                          /* zoom the game (display.c) */
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
        case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_REMOVED:
            joyride_pad_event(&e);                       /* src/platform/pad.c */
            break;
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
    joyride_pad_poll();                                  /* the pad in menus, L3+R3 (pad.c) */
    if (hle_screen_poll_hook) hle_screen_poll_hook();
    present(0);
}

/* ---- set-up -------------------------------------------------------------- */

int hle_screen_desktop(int *w, int *h) {
    if (g_headless || !SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(g_win ? SDL_GetDisplayForWindow(g_win) : SDL_GetPrimaryDisplay());
    if (!dm) return 0;
    /* in pixels: with desktop scaling (Wayland's 150%, say) the mode is in
     * scaled units, a size no screen has. Offered as one more mode, it also
     * pushed the last of the list, 3840x2160 at 32 bits, past what the
     * game's mode table holds, and the game played in 16 bits, where the GPU
     * renderer and filter can't help. */
    float d = dm->pixel_density > 0 ? dm->pixel_density : 1.0f;
    *w = (int)lrintf((float)dm->w * d), *h = (int)lrintf((float)dm->h * d);
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
    if (headless) {
        /* JOYRIDE_HEADLESS_GPU=1: a GPU device without a window (tests) */
        if (getenv("JOYRIDE_HEADLESS_GPU") && SDL_InitSubSystem(SDL_INIT_VIDEO))
            g_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL,
                                        getenv("JOYRIDE_GPU_DEBUG") != NULL, NULL);
        return 0;
    }

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");        /* SIGINT and SIGTERM end the process */
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "[screen] SDL: %s\n", SDL_GetError()); return -1; }
    joyride_pad_init();
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
    fprintf(stderr, "[screen] SDL %d.%d.%d, %s video, SDL_GPU on %s, %s present, F1 settings, F11 fullscreen\n",
            SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
            SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver(), SDL_GetGPUDeviceDriver(g_gpu),
            pm == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : "vsync");
    return 0;
}
