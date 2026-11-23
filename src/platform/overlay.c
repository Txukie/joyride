/*
 * joyride - the in-game settings overlay, and the settings file.
 *
 * F1 (or L3+R3 on a controller) opens it over the game; in play the game
 * pauses while it is open (joyride_safe_point holds the game at its frame
 * limiter, 0x47DC80, and keeps the window showing). Up and Down pick a
 * setting, Left and Right change it, Enter applies a new screen size, F1
 * or Esc closes. On a controller (src/platform/pad.c turns it into those
 * keys): d-pad or left stick, A accepts, B goes back.
 *
 * Settings: the screen size in play (the game's own 1024x768, or any size
 * joyride's DirectDraw offers, widescreen included; src/hle/display.c),
 * texture filtering (src/hle/texfilter.c), smooth motion (frames between
 * the game's steps, src/hle/smooth.c), the presenter's scaling and
 * fullscreen. They are kept in joyride.cfg next to the overlay directory,
 * one `key=value` a line; JOYRIDE_RES, JOYRIDE_FILTER, JOYRIDE_SMOOTH and
 * JOYRIDE_SCALE override the file for one run without changing it.
 *
 * Drawn on the CPU into the converted frame, with SDL's built-in 8x8 debug
 * font through a software renderer, scaled to the frame's height.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "win32hle.h"
#include "joyride_hle.h"

joyride_settings joyride_cfg = { 0, 0, 1, 0, 0, 1, 0, 1, 0, 0 };
static char g_path[4096];

/* ---- settings file ------------------------------------------------------- */

void joyride_config_load(const char *path) {
    snprintf(g_path, sizeof g_path, "%s", path);
    FILE *f = fopen(path, "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) {
        char key[64], val[128];
        if (sscanf(line, " %63[^= ] = %127s", key, val) != 2) continue;
        if (!strcmp(key, "resolution")) {
            int w, h;
            if (sscanf(val, "%dx%d", &w, &h) == 2 && joyride_display_valid(w, h)) joyride_cfg.res_w = w, joyride_cfg.res_h = h;
        } else if (!strcmp(key, "filter")) joyride_cfg.filter = atoi(val) != 0;
        else if (!strcmp(key, "scaling")) {
            for (int i = 0; joyride_screen_scaling_name(i); i++)
                if (!strcmp(val, joyride_screen_scaling_name(i))) joyride_cfg.scaling = i;
        } else if (!strcmp(key, "fullscreen")) joyride_cfg.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "smooth")) joyride_cfg.smooth = atoi(val) != 0;
        else if (!strcmp(key, "renderer")) joyride_cfg.hwrender = !strcmp(val, "gpu");
        else if (!strcmp(key, "controller")) joyride_cfg.controls_modern = strcmp(val, "classic") != 0;
        else if (!strcmp(key, "hud")) joyride_cfg.hud = !strcmp(val, "auto") ? 0 : atoi(val) < 1 ? 1 : atoi(val) > 4 ? 4 : atoi(val);
        else if (!strcmp(key, "driveby")) joyride_cfg.driveby = !strcmp(val, "always");
    }
    if (f) fclose(f);

    /* one-run overrides */
    const char *e;
    if ((e = getenv("JOYRIDE_RES")) && *e) {
        int w = 0, h = 0;
        if (!strcmp(e, "desktop")) hle_screen_desktop(&w, &h);
        else if (!strcmp(e, "game")) w = h = 0;
        else if (sscanf(e, "%dx%d", &w, &h) != 2 || !joyride_display_valid(w, h)) {
            fprintf(stderr, "[joyride] JOYRIDE_RES=%s: expected <width>x<height>, desktop or game\n", e);
            w = joyride_cfg.res_w, h = joyride_cfg.res_h;
        }
        joyride_cfg.res_w = w, joyride_cfg.res_h = h;
    }
    if ((e = getenv("JOYRIDE_FILTER")) && *e) joyride_cfg.filter = *e != '0';    /* "cpu": on, on the CPU */
    if ((e = getenv("JOYRIDE_SMOOTH")) && *e) joyride_cfg.smooth = *e != '0';
    if ((e = getenv("JOYRIDE_RENDERER")) && *e) joyride_cfg.hwrender = !strcmp(e, "gpu");
    if ((e = getenv("JOYRIDE_DRIVEBY")) && *e) joyride_cfg.driveby = !strcmp(e, "always");
    if ((e = getenv("JOYRIDE_HUD")) && *e) joyride_cfg.hud = !strcmp(e, "auto") ? 0 : atoi(e) < 1 ? 1 : atoi(e) > 4 ? 4 : atoi(e);
    if ((e = getenv("JOYRIDE_CONTROLLER")) && *e) joyride_cfg.controls_modern = strcmp(e, "classic") != 0;
    if ((e = getenv("JOYRIDE_SCALE")) && *e)
        for (int i = 0; joyride_screen_scaling_name(i); i++)
            if (!strcmp(e, joyride_screen_scaling_name(i))) joyride_cfg.scaling = i;
}

void joyride_config_save(void) {
    if (!g_path[0]) return;
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# joyride settings (the F1 overlay writes this)\n");
    if (joyride_cfg.res_w) fprintf(f, "resolution=%dx%d\n", joyride_cfg.res_w, joyride_cfg.res_h);
    else fprintf(f, "resolution=game\n");
    fprintf(f, "filter=%d\n", joyride_cfg.filter);
    fprintf(f, "scaling=%s\n", joyride_screen_scaling_name(joyride_cfg.scaling));
    fprintf(f, "fullscreen=%d\n", joyride_cfg.fullscreen);
    fprintf(f, "smooth=%d\n", joyride_cfg.smooth);
    fprintf(f, "renderer=%s\n", joyride_cfg.hwrender ? "gpu" : "software");
    fprintf(f, "controller=%s\n", joyride_cfg.controls_modern ? "modern" : "classic");
    if (joyride_cfg.hud) fprintf(f, "hud=%d\n", joyride_cfg.hud);
    else fprintf(f, "hud=auto\n");
    fprintf(f, "driveby=%s\n", joyride_cfg.driveby ? "always" : "game");
    fclose(f);
    rename(tmp, g_path);
}

/* ---- the menu ------------------------------------------------------------ */

enum { ITEM_RES, ITEM_RENDERER, ITEM_HUD, ITEM_FILTER, ITEM_SMOOTH, ITEM_CONTROLS, ITEM_DRIVEBY, ITEM_SCALING, ITEM_FULLSCREEN, ITEM_RESUME, ITEM_COUNT };

static int g_open, g_sel;
static int g_sizes[HLE_DD_MAX_MODES + 1][2], g_nsizes;   /* [0] = the game's own */
static int g_size_i;                                    /* shown in the menu */

static void list_sizes(void) {
    int modes[HLE_DD_MAX_MODES][2], dw = 0, dh = 0;
    hle_screen_desktop(&dw, &dh);
    int n = hle_dd_modes(dw, dh, modes);
    g_nsizes = 0;
    g_sizes[g_nsizes][0] = 0, g_sizes[g_nsizes++][1] = 0;
    for (int i = 0; i < n; i++)
        if (joyride_display_valid(modes[i][0], modes[i][1]) && !(modes[i][0] == 1024 && modes[i][1] == 768))
            g_sizes[g_nsizes][0] = modes[i][0], g_sizes[g_nsizes++][1] = modes[i][1];
}

static int size_index(int w, int h) {
    for (int i = 0; i < g_nsizes; i++) if (g_sizes[i][0] == w && g_sizes[i][1] == h) return i;
    return 0;
}

int joyride_overlay_open(void) { return g_open; }

/* ---- the pause menu: Start or F6 in play ------------------------------------
 * The game held as for the settings (joyride_safe_point); Continue, Save
 * (savegame.c; not in a network game) and Quit, which gives the game its
 * own Esc once play resumes and then Enter to its "Ya want out?": the level
 * ends and its results show. */
enum { P_CONTINUE, P_SAVE, P_QUIT, P_COUNT };
static int g_pause, g_psel, g_quit, g_pconfirm;
static char g_pmsg[160];

void joyride_pause_toggle(void) {
    if (g_open && g_pause) { g_open = g_pause = 0; joyride_screen_refresh(); return; }
    if (g_open || !joyride_pad_in_play()) return;
    g_open = g_pause = 1, g_psel = P_CONTINUE, g_pmsg[0] = 0, g_pconfirm = 0;
    joyride_screen_refresh();
}

static int pause_key(int sc) {
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F6: g_open = g_pause = 0; break;
    case SDL_SCANCODE_UP:
        g_pconfirm = 0, g_pmsg[0] = 0;
        do g_psel = (g_psel + P_COUNT - 1) % P_COUNT; while (g_psel == P_SAVE && !joyride_savegame_can_save());
        break;
    case SDL_SCANCODE_DOWN:
        g_pconfirm = 0, g_pmsg[0] = 0;
        do g_psel = (g_psel + 1) % P_COUNT; while (g_psel == P_SAVE && !joyride_savegame_can_save());
        break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
        if (g_psel == P_CONTINUE) g_open = g_pause = 0;
        else if (g_psel == P_SAVE) {
            int secs = joyride_savegame_mission_secs();
            if (secs >= 0 && !g_pconfirm) {                 /* a mission under way: say what is lost first */
                g_pconfirm = 1;
                snprintf(g_pmsg, sizeof g_pmsg, "The mission under way is not\nkept: it started %d min %02d s\nago. Enter again to save.",
                         secs / 60, secs % 60);
            } else {
                g_pconfirm = 0;
                joyride_savegame_save(g_pmsg, sizeof g_pmsg);
            }
        }
        else g_open = g_pause = 0, g_quit = 1;
        break;
    default: break;
    }
    joyride_screen_refresh();
    return 1;
}

static void set_open(int on) {
    g_open = on;
    if (on) {
        list_sizes();
        g_size_i = size_index(joyride_cfg.res_w, joyride_cfg.res_h);
        g_sel = 0;
    }
    joyride_screen_refresh();
}

static void change(int dir) {
    switch (g_sel) {
    case ITEM_RES:
        g_size_i = (g_size_i + dir + g_nsizes) % g_nsizes;
        break;
    case ITEM_FILTER:
        joyride_cfg.filter = !joyride_cfg.filter;
        joyride_texfilter_set(joyride_cfg.filter);
        joyride_config_save();
        break;
    case ITEM_RENDERER:
        if (!joyride_cfg.hwrender && !joyride_screen_hwrender()) break;   /* no GPU path here */
        joyride_cfg.hwrender = !joyride_cfg.hwrender;
        joyride_hwrender = joyride_cfg.hwrender;
        joyride_config_save();
        break;
    case ITEM_HUD:
        joyride_cfg.hud = (joyride_cfg.hud + dir + 5) % 5;
        joyride_config_save();
        break;
    case ITEM_DRIVEBY:
        joyride_cfg.driveby = !joyride_cfg.driveby;
        joyride_config_save();
        break;
    case ITEM_CONTROLS:
        joyride_cfg.controls_modern = !joyride_cfg.controls_modern;
        joyride_controls_modern = joyride_cfg.controls_modern;
        joyride_config_save();
        break;
    case ITEM_SMOOTH:
        joyride_cfg.smooth = !joyride_cfg.smooth;
        joyride_smooth = joyride_cfg.smooth;
        joyride_config_save();
        break;
    case ITEM_SCALING: {
        int n = 0;
        while (joyride_screen_scaling_name(n)) n++;
        joyride_cfg.scaling = (joyride_cfg.scaling + dir + n) % n;
        joyride_screen_scaling(joyride_cfg.scaling);
        joyride_config_save();
        break;
    }
    case ITEM_FULLSCREEN:
        joyride_cfg.fullscreen = !joyride_cfg.fullscreen;
        joyride_screen_fullscreen(joyride_cfg.fullscreen);
        joyride_config_save();
        break;
    }
}

static void apply_size(void) {
    int w = g_sizes[g_size_i][0], h = g_sizes[g_size_i][1];
    if (w == joyride_cfg.res_w && h == joyride_cfg.res_h) return;
    joyride_cfg.res_w = w, joyride_cfg.res_h = h;
    joyride_config_save();
    joyride_display_request(w, h);
}

int joyride_overlay_key(int sc, int down, int repeat) {
    if (sc == SDL_SCANCODE_F1) {
        if (down && !repeat && !g_pause) set_open(!g_open);
        return 1;
    }
    if (sc == SDL_SCANCODE_F6 && !g_open && joyride_pad_in_play()) {   /* the game's pause key: ours */
        if (down && !repeat) joyride_pause_toggle();
        return 1;
    }
    if (!g_open) return 0;
    if (!down) return 0;                         /* key-ups reach the game: nothing stays held */
    if (g_pause) return pause_key(sc);
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: set_open(0); break;
    case SDL_SCANCODE_UP: g_sel = (g_sel + ITEM_COUNT - 1) % ITEM_COUNT; break;
    case SDL_SCANCODE_DOWN: g_sel = (g_sel + 1) % ITEM_COUNT; break;
    case SDL_SCANCODE_LEFT: change(-1); break;
    case SDL_SCANCODE_RIGHT: change(1); break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
        if (g_sel == ITEM_RES) apply_size();
        else if (g_sel == ITEM_RESUME) set_open(0);
        else change(1);
        break;
    default: break;
    }
    joyride_screen_refresh();
    return 1;
}

/* ---- drawing ------------------------------------------------------------- */

static void size_name(int i, char *out, size_t n) {
    if (!g_sizes[i][0]) snprintf(out, n, "1024 x 768 (game)");
    else snprintf(out, n, "%d x %d", g_sizes[i][0], g_sizes[i][1]);
}

static void draw_pause(SDL_Renderer *r, float vw, float vh) {
    const float pw = 32 * 8, ph = (P_COUNT + 8) * 12;
    SDL_FRect panel = { (vw - pw) / 2, (vh - ph) / 2, pw, ph };
    SDL_SetRenderDrawColor(r, 16, 16, 24, 235);
    SDL_RenderFillRect(r, &panel);
    SDL_SetRenderDrawColor(r, 230, 200, 40, 255);
    SDL_RenderRect(r, &panel);
    float x = panel.x + 12, y = panel.y + 10;
    SDL_RenderDebugText(r, x, y, "Paused");
    y += 20;
    static const char *const label[P_COUNT] = { "Continue", "Save game", "Quit" };
    int can = joyride_savegame_can_save();
    for (int i = 0; i < P_COUNT; i++, y += 12) {
        if (i == g_psel) {
            SDL_FRect bar = { panel.x + 4, y - 2, pw - 8, 12 };
            SDL_SetRenderDrawColor(r, 70, 60, 10, 255);
            SDL_RenderFillRect(r, &bar);
        }
        int off = i == P_SAVE && !can;
        SDL_SetRenderDrawColor(r, off ? 110 : 255, off ? 110 : 255, off ? 110 : 255, 255);
        SDL_RenderDebugText(r, x, y, i == P_SAVE && off && joyride_net_driveby() >= 0 ? "Save game (not online)" :
                                     i == P_SAVE && g_pconfirm ? "Save anyway" : label[i]);
    }
    y += 8;
    SDL_SetRenderDrawColor(r, 230, 200, 40, 255);
    char lines[sizeof g_pmsg];                         /* the message, a line at a time */
    snprintf(lines, sizeof lines, "%s", g_pmsg);
    for (char *l = strtok(lines, "\n"); l; l = strtok(NULL, "\n"), y += 10) SDL_RenderDebugText(r, x, y, l);
}

void joyride_overlay_draw(uint32_t *bgra, int w, int h) {
    if (!g_open || w < 320 || h < 200) return;
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ARGB8888, bgra, w * 4);
    if (!s) return;
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(s);
    if (!r) { SDL_DestroySurface(s); return; }

    int k = h / 240 < 1 ? 1 : h / 240;           /* 8x8 font: about 30 lines a screen */
    float vw = (float)w / k, vh = (float)h / k;   /* the canvas, in font pixels */
    SDL_SetRenderScale(r, (float)k, (float)k);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 150);
    SDL_RenderFillRect(r, NULL);
    if (g_pause) {
        draw_pause(r, vw, vh);
        SDL_RenderPresent(r);
        SDL_DestroyRenderer(r);
        SDL_DestroySurface(s);
        return;
    }

    const float pw = 37 * 8, ph = (ITEM_COUNT + 5) * 12;
    SDL_FRect panel = { (vw - pw) / 2, (vh - ph) / 2, pw, ph };
    SDL_SetRenderDrawColor(r, 16, 16, 24, 235);
    SDL_RenderFillRect(r, &panel);
    SDL_SetRenderDrawColor(r, 230, 200, 40, 255);
    SDL_RenderRect(r, &panel);

    float x = panel.x + 12, y = panel.y + 10;
    SDL_RenderDebugText(r, x, y, "joyride settings");
    y += 20;

    int cw, ch;
    joyride_display_current(&cw, &ch);
    for (int i = 0; i < ITEM_COUNT; i++, y += 12) {
        char label[32] = "", val[64] = "";
        switch (i) {
        case ITEM_RES:
            snprintf(label, sizeof label, "Screen size");
            size_name(g_size_i, val, sizeof val);
            break;
        case ITEM_FILTER:
            snprintf(label, sizeof label, "Textures");
            snprintf(val, sizeof val, "%s", !joyride_cfg.filter ? "sharp" :
                     joyride_texfilter == 2 ? "bilinear (3dfx, GPU)" : "bilinear (3dfx, CPU)");
            break;
        case ITEM_RENDERER:
            snprintf(label, sizeof label, "Renderer");
            snprintf(val, sizeof val, "%s", joyride_cfg.hwrender ? "GPU (3dfx-style)" :
                     joyride_screen_hwrender() ? "software (original)" : "software (no GPU path)");
            break;
        case ITEM_HUD:
            snprintf(label, sizeof label, "HUD size");
            if (!joyride_cfg.hwrender) snprintf(val, sizeof val, "original (GPU renderer only)");
            else if (!joyride_cfg.hud) snprintf(val, sizeof val, "auto (x%d)", joyride_screen_hud_scale(ch));
            else snprintf(val, sizeof val, "x%d", joyride_cfg.hud);
            break;
        case ITEM_SMOOTH:
            snprintf(label, sizeof label, "Motion");
            snprintf(val, sizeof val, "%s", joyride_cfg.smooth ? "smooth" : "original (23 fps)");
            break;
        case ITEM_CONTROLS:
            snprintf(label, sizeof label, "Controller");
            snprintf(val, sizeof val, "%s", joyride_cfg.controls_modern ? "modern (twin stick)" : "classic (tank)");
            break;
        case ITEM_DRIVEBY:
            snprintf(label, sizeof label, "Drive-bys");
            if (joyride_net_driveby() >= 0)                  /* a network game: the host's */
                snprintf(val, sizeof val, "%s (host's rule)", joyride_net_driveby() ? "always" : "as the game");
            else snprintf(val, sizeof val, "%s", joyride_cfg.driveby ? "always" : "as the game (London 1961)");
            break;
        case ITEM_SCALING:
            snprintf(label, sizeof label, "Scaling");
            snprintf(val, sizeof val, "%s", joyride_screen_scaling_name(joyride_cfg.scaling));
            break;
        case ITEM_FULLSCREEN:
            snprintf(label, sizeof label, "Fullscreen");
            snprintf(val, sizeof val, "%s", joyride_cfg.fullscreen ? "on" : "off");
            break;
        case ITEM_RESUME:
            snprintf(label, sizeof label, "Resume");
            break;
        }
        if (i == g_sel) {
            SDL_FRect bar = { panel.x + 4, y - 2, pw - 8, 12 };
            SDL_SetRenderDrawColor(r, 70, 60, 10, 255);
            SDL_RenderFillRect(r, &bar);
        }
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        SDL_RenderDebugText(r, x, y, label);
        if (val[0]) {
            char shown[80];
            snprintf(shown, sizeof shown, i == g_sel ? "< %s >" : "  %s", val);
            SDL_SetRenderDrawColor(r, 230, 200, 40, 255);
            SDL_RenderDebugText(r, x + 12 * 8, y, shown);
        }
    }
    y += 8;
    SDL_SetRenderDrawColor(r, 150, 150, 150, 255);
    int pending = g_sizes[g_size_i][0] != joyride_cfg.res_w || g_sizes[g_size_i][1] != joyride_cfg.res_h;
    SDL_RenderDebugText(r, x, y, g_sel == ITEM_RES && pending ? "Enter/A: use this size" : "Arrows: change  Esc/B: close");

    SDL_RenderPresent(r);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(s);
}

/* ---- the frame's safe point ------------------------------------------------ */

/* Spliced into the frame limiter (0x47DC80): between two frames of play,
 * the machine held. Applies a new screen size, and holds the game while
 * the overlay is open (the window keeps being pumped and redrawn; timer
 * callbacks wait, so the game's clock stops too). */
void joyride_safe_point(void) {
    joyride_display_apply();
    if (g_open) {
        while (g_open) {
            hle_screen_pump();
            joyride_screen_refresh();
            SDL_Delay(16);
        }
        joyride_display_apply();
    }
    static uint64_t quit_at;                       /* the pause menu's Quit: the game's Esc, then Enter */
    if (g_quit == 1) {
        g_quit = 2, quit_at = SDL_GetTicks() + 150;
        joyride_screen_key(SDL_SCANCODE_ESCAPE, 1);
        joyride_screen_key(SDL_SCANCODE_ESCAPE, 0);
    } else if (g_quit == 2 && SDL_GetTicks() >= quit_at) {
        g_quit = 0;
        joyride_screen_key(SDL_SCANCODE_RETURN, 1);
        joyride_screen_key(SDL_SCANCODE_RETURN, 0);
    }
    joyride_smooth_wait();                         /* frames between steps (smooth.c) */
}
