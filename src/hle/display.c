/*
 * joyride - the game's screen size in play, any shape, changeable in game.
 *
 * How the 2002 exe picks its mode:
 *
 *  1. SciTech MGL (linked in) knows a fixed set of sizes: one table of
 *     76-byte records per pixel depth (32, 15, 16, 8-bit at 0x4B5A60,
 *     0x4B5F68, 0x4B63E0, 0x4B68E8 for 640x480), each holding width - 1
 *     and height - 1, and 0x48B770 reports a mode number's size from
 *     constants in code. Only DirectDraw modes matching a record exist for
 *     it. (Its 1152x864 and 1280x960 records are typos and never match.)
 *  2. The game (0x4153E0) keeps MGL's modes that are at most 1600 wide and
 *     4:3 or 16:10 (0x415418), 16 or 32-bit, in a list at [0x504CD0]:
 *     28-byte entries, + 0x18 its copy of the mode (MGL mode number, width,
 *     height, bpp).
 *  3. It plays in the entry whose index is in [0x503224], which 0x414D30
 *     returns to the mode switch (0x414CC0). The game always leaves it at
 *     6: 1024x768 32-bit with the modes joyride's DirectDraw offers. A
 *     screen-mode menu that would change it in play (0x482D00) is never
 *     shown in this release.
 *
 * joyride makes MGL's 1600x1200 mode its "custom" slot: before MGL reads
 * the records (joyride_display_setup, from main) they are set to the size
 * wanted, joyride_mode_size corrects the size 0x48B770 reports, and
 * joyride_pick_mode_allowed lets it past the game's 4:3 / 16:10 / 1600
 * check. joyride_pick_mode makes the chosen size the mode in play. In
 * play, the overlay asks for a size (joyride_display_request) and the
 * frame's safe point (joyride_display_apply) retargets the custom slot if
 * needed and runs the game's own mode switch, 0x414CC0 then 0x47CD10, as
 * its unused menu does.
 *
 * The game scales its camera by the screen's width (a wide mode would just
 * zoom in) and squashes non-4:3 modes vertically (for 640x400 on a 4:3
 * monitor). joyride_view_scale makes the scale follow the height, as if
 * the screen were 4:3 at that height (the same for 4:3 modes), and
 * joyride_square_pixels keeps pixels square: a wider screen shows more of
 * the city to the sides at the usual zoom. The renderer's tables have room
 * for 4096 columns and about 1700 rows; sizes past 3840x1600 are refused.
 * tools/postlift.py splices all of these into the lifted code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define MAX_W 3840
#define MAX_H 1600

static int g_sw, g_sh;               /* the size chosen, 0 = the game's own */
static int g_cw = 1600, g_ch = 1200; /* the custom slot's size */
static int g_rw, g_rh;               /* a size asked for in play, to apply */

/* MGL's 1600x1200 records, one per pixel depth (32, 15, 16, 8-bit) */
static const uint32_t k_mgl_1600[] = { 0x4B5C28, 0x4B6130, 0x4B65A8, 0x4B6AB0 };
static int g_mgl_ok;

/* sizes MGL has records for anyway: these need no custom slot */
static int native(int w, int h) {
    return (w == 640 && h == 480) || (w == 800 && h == 600) || (w == 1024 && h == 768);
}

int joyride_display_valid(int w, int h) {
    return w >= 640 && h >= 480 && w <= MAX_W && h <= MAX_H;
}

static void set_mgl(int w, int h) {
    for (size_t i = 0; i < sizeof k_mgl_1600 / sizeof *k_mgl_1600; i++) {
        MEM32(k_mgl_1600[i]) = (uint32_t)w - 1;
        MEM32(k_mgl_1600[i] + 4) = (uint32_t)h - 1;
    }
}

void joyride_display_setup(int w, int h) {
    g_mgl_ok = 1;
    for (size_t i = 0; i < sizeof k_mgl_1600 / sizeof *k_mgl_1600; i++)
        if (MEM32(k_mgl_1600[i]) != 1599 || MEM32(k_mgl_1600[i] + 4) != 1199) g_mgl_ok = 0;
    if (!g_mgl_ok) { fprintf(stderr, "[joyride] MGL's mode table is not where expected; screen size fixed\n"); return; }
    if (!w || !joyride_display_valid(w, h)) return;
    g_sw = w, g_sh = h;
    if (!native(w, h)) {
        g_cw = w, g_ch = h;
        set_mgl(w, h);
    }
}

void joyride_display_current(int *w, int *h) { *w = g_sw, *h = g_sh; }

/* 0x48BFE2: MGL's size of a mode number, as it returns it */
void joyride_mode_size(uint32_t wp, uint32_t hp) {
    if (g_mgl_ok && MEM32(wp) == 1600 && MEM32(hp) == 1200) MEM32(wp) = (uint32_t)g_cw, MEM32(hp) = (uint32_t)g_ch;
}

/* The camera: each view's scale (0x43BB30) is its width * zoom / 320. */
uint32_t joyride_view_scale(uint32_t view_h, uint32_t zoom, uint32_t scale) {
    (void)scale;
    return (uint32_t)(int32_t)((int64_t)((int64_t)view_h * 4 / 3) * (int32_t)zoom / 320);
}

/* 0x46456E: a view that is not 4:3 gets its y squashed by 5/6, for modes
 * like 640x400 shown on a 4:3 monitor. joyride's pixels are square. */
int joyride_square_pixels(void) { return 1; }

/* 0x415418: may this MGL mode (width, height) go in the game's list? */
int joyride_pick_mode_allowed(uint32_t w, uint32_t h) {
    return g_mgl_ok && (int)w == g_cw && (int)h == g_ch;
}

static uint32_t entries(void);
static uint32_t entry_rec(uint32_t i);

/* Make the custom slot w x h: MGL's records and the game's copies of it. */
static void retarget(int w, int h) {
    if (native(w, h) || (w == g_cw && h == g_ch)) return;
    uint32_t n = entries();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = entry_rec(i);
        if ((int)MEM32(r + 4) == g_cw && (int)MEM32(r + 8) == g_ch) MEM32(r + 4) = (uint32_t)w, MEM32(r + 8) = (uint32_t)h;
    }
    g_cw = w, g_ch = h;
    set_mgl(w, h);
}

/* the game's copy of entry i: width, height, bits per pixel (its record) */
static uint32_t entry_rec(uint32_t i) { return MEM32(MEM32(0x504CD0) + i * 28 + 0x18); }
static uint32_t entries(void) {
    uint32_t n = MEM32(0x503204) + MEM32(0x503208) + MEM32(0x50320C);
    return MEM32(0x504CD0) && n <= 256 ? n : 0;
}

static int find(int w, int h) {
    int best = -1;
    uint32_t best_bpp = 0, n = entries();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = entry_rec(i), bpp = MEM32(r + 12);
        if ((int)MEM32(r + 4) == w && (int)MEM32(r + 8) == h && bpp >= 16 && bpp > best_bpp) best = (int)i, best_bpp = bpp;
    }
    return best;
}

/* 0x414D30: the index of the mode to play in; `game` is the game's own */
uint32_t joyride_pick_mode(uint32_t game) {
    static int listed;
    uint32_t n = entries();
    if (!n) return game;
    if (!listed++) {
        fprintf(stderr, "[joyride] the game's modes:");
        for (uint32_t i = 0; i < n; i++) {
            uint32_t r = entry_rec(i);
            if (MEM32(r + 12) >= 16) fprintf(stderr, " %ux%ux%u", MEM32(r + 4), MEM32(r + 8), MEM32(r + 12));
        }
        fprintf(stderr, "\n");
    }
    if (g_rw) {                                    /* chosen in the menus: applies now */
        retarget(g_rw, g_rh);
        g_sw = g_rw, g_sh = g_rh, g_rw = g_rh = 0;
    }
    if (!g_sw) return game;
    int i = find(g_sw, g_sh);
    if (i < 0) {
        fprintf(stderr, "[joyride] screen size: no %dx%d mode on offer; keeping the game's own\n", g_sw, g_sh);
        g_sw = g_sh = 0;
        return game;
    }
    MEM8(0x503224) = (uint8_t)i;
    return (uint32_t)i;
}

/* ---- in play ------------------------------------------------------------- */

/* From the overlay: in play it applies at the next frame's safe point; in
 * the menus, when play starts (joyride_pick_mode). 0 x 0: the game's own. */
void joyride_display_request(int w, int h) {
    if (!g_mgl_ok || (w && !joyride_display_valid(w, h))) return;
    g_rw = w ? w : 1024, g_rh = w ? h : 768;
}

/* At the frame's safe point (0x47DC80, between frames, the machine held). */
void joyride_display_apply(void) {
    if (!g_rw) return;
    int w = g_rw, h = g_rh;
    g_rw = g_rh = 0;
    if (!entries()) return;
    retarget(w, h);
    int i = find(w, h);
    if (i < 0) { fprintf(stderr, "[joyride] screen size: no %dx%d mode\n", w, h); return; }
    g_sw = w, g_sh = h;
    uint32_t mark = hle_scratch_mark(), idx = hle_scratch(4);
    MEM8(idx) = (uint8_t)i;
    hle_call_guest(0x414CC0, 1, &idx);            /* set the mode by list index */
    hle_call_guest(0x47CD10, 0, NULL);            /* what the game's menu does next */
    hle_scratch_release(mark);
    fprintf(stderr, "[joyride] screen size: %dx%d\n", w, h);
}
