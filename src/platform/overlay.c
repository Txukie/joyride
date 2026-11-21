/*
 * joyride - the in-game settings overlay, and the settings file.
 *
 * F12 opens it over the game; in play the game pauses while it is open
 * (joyride_safe_point holds the game at its frame limiter, 0x47DC80, and
 * keeps the window showing). Up and Down pick a setting, Left and Right
 * change it, Enter applies a new screen size, F12 or Esc closes.
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

joyride_settings joyride_cfg = { 0, 0, 1, 0, 0, 1 };
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
    if ((e = getenv("JOYRIDE_FILTER")) && *e) joyride_cfg.filter = *e != '0';
    if ((e = getenv("JOYRIDE_SMOOTH")) && *e) joyride_cfg.smooth = *e != '0';
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
    fprintf(f, "# joyride settings (the F12 overlay writes this)\n");
    if (joyride_cfg.res_w) fprintf(f, "resolution=%dx%d\n", joyride_cfg.res_w, joyride_cfg.res_h);
    else fprintf(f, "resolution=game\n");
    fprintf(f, "filter=%d\n", joyride_cfg.filter);
    fprintf(f, "scaling=%s\n", joyride_screen_scaling_name(joyride_cfg.scaling));
    fprintf(f, "fullscreen=%d\n", joyride_cfg.fullscreen);
    fprintf(f, "smooth=%d\n", joyride_cfg.smooth);
    fclose(f);
    rename(tmp, g_path);
}

/* ---- the menu ------------------------------------------------------------ */

enum { ITEM_RES, ITEM_FILTER, ITEM_SMOOTH, ITEM_SCALING, ITEM_FULLSCREEN, ITEM_RESUME, ITEM_COUNT };

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
        joyride_texfilter = (uint32_t)joyride_cfg.filter;
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
    if (sc == SDL_SCANCODE_F12) {
        if (down && !repeat) set_open(!g_open);
        return 1;
    }
    if (!g_open) return 0;
    if (!down) return 0;                         /* key-ups reach the game: nothing stays held */
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
            snprintf(val, sizeof val, "%s", joyride_cfg.filter ? "bilinear (3dfx)" : "sharp");
            break;
        case ITEM_SMOOTH:
            snprintf(label, sizeof label, "Motion");
            snprintf(val, sizeof val, "%s", joyride_cfg.smooth ? "smooth" : "original (23 fps)");
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
    SDL_RenderDebugText(r, x, y, g_sel == ITEM_RES && pending ? "Enter: use this size" : "Arrows: change  F12: close");

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
    joyride_smooth_wait();                         /* frames between steps (smooth.c) */
}
