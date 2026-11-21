/*
 * joyride - the game's screen size in play (JOYRIDE_RES), any shape.
 *
 * How the 2002 exe picks its mode:
 *
 *  1. SciTech MGL (linked in) knows a fixed set of sizes: one table of
 *     76-byte records per pixel depth (32, 15, 16, 8-bit at 0x4B5A60,
 *     0x4B5F68, 0x4B63E0, 0x4B68E8 for 640x480), each holding width - 1
 *     and height - 1. Only DirectDraw modes matching a record exist for it.
 *     (Its 1152x864 and 1280x960 records are typos and never match.)
 *  2. The game (0x4153E0) keeps MGL's modes that are at most 1600 wide and
 *     4:3 or 16:10 (0x415418), 16 or 32-bit, in a list at [0x504CD0]:
 *     28-byte entries, + 0x18 the MGL record (mode, width, height, bpp).
 *  3. It plays in the entry whose index is in [0x503224], which 0x414D30
 *     returns to the mode switch (0x414CC0). The game always leaves it at
 *     6: 1024x768 32-bit with the modes joyride's DirectDraw offers.
 *
 * With JOYRIDE_RES=<w>x<h> (or "desktop"), joyride turns MGL's 1600x1200
 * mode into that size: its records, before MGL reads them
 * (joyride_display_setup, from main), and the size MGL reports for the
 * mode number (0x48B770 builds those from constants in code;
 * joyride_mode_size). tools/postlift.py splices those calls and two more:
 * joyride_mode_allowed() lets that size past the 4:3 / 16:10 / 1600 check,
 * and joyride_pick_mode() makes it the mode in play.
 *
 * The game scales its camera by the screen's width (a wide mode would just
 * zoom in) and squashes non-4:3 modes vertically (for 640x400 on a 4:3
 * monitor); in joyride's mode the scale follows the height and pixels are
 * square (joyride_view_scale, joyride_square_pixels), so a wider screen
 * shows more of the city to the sides at the usual zoom. The renderer's tables have room for 4096 columns and
 * about 1700 rows; joyride refuses sizes past 3840x1600.
 *
 * The size must be one joyride's DirectDraw offers (win32hle's list: the
 * common 4:3, 16:9, 16:10 and ultrawide sizes up to 4K, and the desktop's
 * own); the log lists the game's modes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define MAX_W 3840
#define MAX_H 1600

static int g_w, g_h;          /* wanted size, 0 = the game's choice */

/* MGL's 1600x1200 records, one per pixel depth (32, 15, 16, 8-bit) */
static const uint32_t k_mgl_1600[] = { 0x4B5C28, 0x4B6130, 0x4B65A8, 0x4B6AB0 };

void joyride_display_setup(void) {
    const char *e = getenv("JOYRIDE_RES");
    if (!e || !*e) return;
    if (!strcmp(e, "desktop")) {
        if (!hle_screen_desktop(&g_w, &g_h)) {
            fprintf(stderr, "[joyride] JOYRIDE_RES=desktop: no desktop size (headless?)\n");
            g_w = g_h = 0;
        }
    } else if (sscanf(e, "%dx%d", &g_w, &g_h) != 2) {
        fprintf(stderr, "[joyride] JOYRIDE_RES=%s: expected <width>x<height> or desktop\n", e);
        g_w = g_h = 0;
    }
    if (g_w && (g_w < 640 || g_h < 480 || g_w > MAX_W || g_h > MAX_H)) {
        fprintf(stderr, "[joyride] JOYRIDE_RES=%dx%d: from 640x480 to %dx%d\n", g_w, g_h, MAX_W, MAX_H);
        g_w = g_h = 0;
    }
    if (!g_w) return;
    for (size_t i = 0; i < sizeof k_mgl_1600 / sizeof *k_mgl_1600; i++) {
        if (MEM32(k_mgl_1600[i]) != 1599 || MEM32(k_mgl_1600[i] + 4) != 1199) {
            fprintf(stderr, "[joyride] JOYRIDE_RES: MGL's mode table is not where expected; ignored\n");
            g_w = g_h = 0;
            return;
        }
    }
    for (size_t i = 0; i < sizeof k_mgl_1600 / sizeof *k_mgl_1600; i++) {
        MEM32(k_mgl_1600[i]) = (uint32_t)g_w - 1;
        MEM32(k_mgl_1600[i] + 4) = (uint32_t)g_h - 1;
    }
    fprintf(stderr, "[joyride] JOYRIDE_RES: MGL's 1600x1200 mode becomes %dx%d\n", g_w, g_h);
}

/* 0x48BFE2: MGL's size of a mode number, as it returns it. Its sizes are
 * constants in code, so the rewritten 1600x1200 mode is resized here. */
void joyride_mode_size(uint32_t wp, uint32_t hp) {
    if (g_w && MEM32(wp) == 1600 && MEM32(hp) == 1200) MEM32(wp) = (uint32_t)g_w, MEM32(hp) = (uint32_t)g_h;
}

/* The camera. Each view's scale (0x43BB30) is its width * zoom / 320, so
 * at any width the view spans the same stretch of city across, and a wide
 * screen just cuts the top and bottom off. In a mode of joyride's the
 * scale follows the height instead, as if the screen were 4:3 at that
 * height: the same view up and down as the game's own modes, and more to
 * the sides the wider it is. */
uint32_t joyride_view_scale(uint32_t view_h, uint32_t zoom, uint32_t scale) {
    if (!g_w) return scale;
    return (uint32_t)(int32_t)((int64_t)((int64_t)view_h * 4 / 3) * (int32_t)zoom / 320);
}

/* 0x46456E: a view that is not 4:3 gets its y squashed by 5/6, for modes
 * like 640x400 shown on a 4:3 monitor. joyride's pixels are square. */
int joyride_square_pixels(void) {
    return g_w != 0;
}

/* 0x415418: may this MGL mode (width, height) go in the game's list? */
int joyride_pick_mode_allowed(uint32_t w, uint32_t h) {
    return g_w && (int)w == g_w && (int)h == g_h;
}

/* the mode record of entry i: width, height, bits per pixel */
static void entry(uint32_t i, uint32_t *w, uint32_t *h, uint32_t *bpp) {
    uint32_t rec = MEM32(MEM32(0x504CD0) + i * 28 + 0x18);
    *w = MEM32(rec + 4), *h = MEM32(rec + 8), *bpp = MEM32(rec + 12);
}

/* 0x414D30: the index of the mode to play in; `game` is the game's own */
uint32_t joyride_pick_mode(uint32_t game) {
    static int listed;
    uint32_t n = MEM32(0x503204) + MEM32(0x503208) + MEM32(0x50320C), w, h, bpp;
    if (!MEM32(0x504CD0) || n > 256) return game;
    if (!listed++) {
        fprintf(stderr, "[joyride] the game's modes (* in use; JOYRIDE_RES=<w>x<h> sets one):");
        for (uint32_t i = 0; i < n; i++) {
            entry(i, &w, &h, &bpp);
            if (bpp >= 16) fprintf(stderr, " %s%ux%ux%u", i == game && !g_w ? "*" : "", w, h, bpp);
        }
        fprintf(stderr, "\n");
    }
    if (!g_w) return game;
    int best = -1;
    uint32_t best_bpp = 0;
    for (uint32_t i = 0; i < n; i++) {
        entry(i, &w, &h, &bpp);
        if ((int)w == g_w && (int)h == g_h && bpp >= 16 && bpp > best_bpp) best = (int)i, best_bpp = bpp;
    }
    if (best < 0) {
        fprintf(stderr, "[joyride] JOYRIDE_RES: no %dx%d mode on offer (not a size joyride's DirectDraw lists); "
                        "keeping the game's own\n", g_w, g_h);
        g_w = 0;
        return game;
    }
    static int said;
    if (!said++) fprintf(stderr, "[joyride] playing at %dx%d, %u-bit\n", g_w, g_h, best_bpp);
    MEM8(0x503224) = (uint8_t)best;
    return (uint32_t)best;
}
