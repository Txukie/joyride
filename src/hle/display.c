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
 * for 4096 columns; its row table (the address of each row, 0x503228) for
 * 1706 rows before the game's mode list, so joyride moves it to a table of
 * 4096 rows in guest memory (joyride_rows; tools/postlift.py points the
 * lifted code at it). Sizes up to 3840x2160 are accepted.
 * tools/postlift.py splices all of these into the lifted code.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define MAX_W 3840
#define MAX_H 2160
#define ROWS 4096
#define GUARD (512u * 1024u - ROWS * 2)

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
    int max_h = joyride_rows == 0x503228 && g_mgl_ok ? 1600 : MAX_H;
    return w >= 640 && h >= 480 && w <= MAX_W && h <= max_h;
}

static void set_mgl(int w, int h) {
    for (size_t i = 0; i < sizeof k_mgl_1600 / sizeof *k_mgl_1600; i++) {
        MEM32(k_mgl_1600[i]) = (uint32_t)w - 1;
        MEM32(k_mgl_1600[i] + 4) = (uint32_t)h - 1;
    }
}

uint32_t joyride_rows = 0x503228;   /* the renderer's row table; the game's own until setup */

void joyride_display_setup(int w, int h) {
    /* the wall drawers (0x49ABDC) look a span's first row up before they
     * clip it, so rows far off the screen are read (and then not used):
     * the table sits in the middle of 1 MB of zeros */
    uint32_t t = hle_vm_reserve(0, GUARD * 2 + ROWS * 4);
    if (t) joyride_rows = t + GUARD;
    else fprintf(stderr, "[joyride] no guest memory for the row table; screens up to 1600 rows only\n");
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
/* joyride's camera zoom (d-pad up/down, + and -): how much bigger the
 * street is drawn, eased towards the level chosen each time the game sets
 * the scale (each step); the renderer gets a camera moved to match
 * (joyride_zoom_height). */
static const float k_zoom[] = { 0.5f, 0.625f, 0.75f, 0.875f, 1.0f, 1.25f, 1.5f, 2.0f };
#define ZOOM_DEFAULT 4
static int g_zoom_i = ZOOM_DEFAULT;
static float g_zoom = 1.0f;

void joyride_camera_zoom(int dir) {
    int n = (int)(sizeof k_zoom / sizeof *k_zoom);
    g_zoom_i += dir > 0 ? 1 : -1;
    if (g_zoom_i < 0) g_zoom_i = 0;
    if (g_zoom_i >= n) g_zoom_i = n - 1;
}

uint32_t joyride_view_scale(uint32_t view_h, uint32_t zoom, uint32_t scale) {
    (void)scale;
    float want = k_zoom[g_zoom_i];
    g_zoom += (want - g_zoom) * 0.3f;                  /* about a third of a second */
    if (g_zoom > want * 0.995f && g_zoom < want * 1.005f) g_zoom = want;
    int64_t s = (int64_t)((int64_t)view_h * 4 / 3) * (int32_t)zoom / 320;
    if (g_zoom > 1.0f) s = (int64_t)((float)s * g_zoom);  /* closer: a narrower view */
    return (uint32_t)(int32_t)s;
}

/* The camera's height as the renderer reads it (0x43B620, 0x43B780,
 * 0x43B7E0). A block level L is drawn at scale / (height + 64 L); the street
 * is drawn at level 5 (+0x140: measured, its 64-pixel tiles on screen). The zoom moves the camera so the street comes out
 * that much smaller, and walls keep their perspective (scaling the scale
 * down instead stretched them across the street). Closer, the camera would
 * soon be under the roofs (it is only ~80 above them), so zooming in narrows
 * the view instead (joyride_view_scale). */
uint32_t joyride_zoom_height(uint32_t z) {
    if (g_zoom >= 1.0f) return z;                      /* closer: the camera can't go below the roofs */
    return (uint32_t)(int32_t)lrintf(((float)(int32_t)z + 320.0f) / g_zoom - 320.0f);
}

/* 0x43B7E0, at 0x43B877: a level's count of blocks across and down
 * (`rec`, +0, +4), the screen's width and height over its tiles' size, + 3.
 * The renderer projects the corners of that many blocks into tables of
 * 65 x 65 (0x54F2A0, two of them, filled by 0x43B620) and nothing checks.
 * Right after the switch to a big mode the size comes from the new mode's
 * width and the old one's camera scale: 3840 pixels over the tiles of a
 * 640 view, about 100 blocks. The rows past 65 went over the second table
 * and on into the top level of the map's block types (0x55FAB0), the
 * northern quarter of the city, where traffic then found "road" on the
 * roofs and drove out onto them. At most 64, which no view needs. */
void joyride_view_blocks(uint32_t rec) {
    for (uint32_t i = 0; i < 2; i++)
        if ((int32_t)MEM32(rec + 4 * i) > 64) MEM32(rec + 4 * i) = 64;
}

/* 0x43CA66 / 0x43BB62: the living area's half height (times 3, before
 * the game's shift), as the zoom shows more or less of the city */
uint32_t joyride_live_zoom(uint32_t h3) {
    return (uint32_t)(int32_t)((float)(int32_t)h3 / g_zoom);
}

/* 0x43CA66: the half width (world pixels) of the area peds and cars live
 * in around view `v`, made for a 4:3 view: as wide as the view really is
 * (its width and height at +8, +0xC). */
uint32_t joyride_live_half_width(uint32_t half, uint32_t v) {
    static int off = -1;
    if (off < 0) off = getenv("JOYRIDE_NARROW_LIVE") != NULL;   /* tests: the game's own 4:3 area */
    if (off) return half;
    half = (uint32_t)(int32_t)((float)(int32_t)half / g_zoom);   /* the zoom shows more or less */
    int64_t w = (int32_t)MEM32(v + 8), h = (int32_t)MEM32(v + 0xC);
    if (h <= 0 || w * 3 <= h * 4) return half;
    return (uint32_t)((int64_t)(int32_t)half * w * 3 / (h * 4));
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
    if (i >= 0 && MEM32(entry_rec((uint32_t)i) + 12) < 32)
        fprintf(stderr, "[joyride] %dx%d only in 16 bits: the GPU renderer and texture filter need 32\n", g_sw, g_sh);
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
