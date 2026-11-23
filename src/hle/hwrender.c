/*
 * joyride - the optional hardware renderer: the world drawn by the GPU.
 *
 * GTA's 3dfx build (docs/exe-3dfx.md) kept the geometry on the CPU and had
 * the card draw each textured quad. joyride does the same with the 2002
 * exe: its software rasterizer is entered at three places in 32-bit modes,
 * and each is captured as a quad and not drawn:
 *
 *   0x496CF0  the polygon fill (lids 0x497710, sprites 0x49787C and
 *             0x4979C9): the record at [0x78E8C0], before clipping: type
 *             at +2 (2: colour 0 see-through; else blended 50/50 with
 *             what is below, the game's table at [0x78C108] is an exact
 *             average), vertex count at +6, texture base at +8, corners as
 *             (x, y) shorts at +0x10, page coordinates as (u, v) bytes at
 *             +0x20. Spans are horizontal; each scanline interpolates along
 *             the two edges it crosses, then across.
 *   0x49A78C  walls and floors drawn in horizontal spans, from the state
 *             block 0x4B8C58..0x4B8C88 the entries (0x497035) normalise
 *             all eight orientations into: rows c70..c74, left edge c60 ->
 *             c68, right edge c64 -> c6c; texture along the span (u, page
 *             column, flipped if c80 & 0x400000) and down the rows (v);
 *             c80 & 0x80: colour 0 see-through.
 *   0x49ABDC  the same in vertical spans (0x497332): columns c60..c64, top
 *             edge c70 -> c74, bottom edge c78 -> c7c; u along the span
 *             (down), v across the columns.
 * Texture page and offsets as for the texture filter (texfilter.c, which
 * also keeps the page slots, palette slots and frozen pages shared here);
 * the shading table is [0x78C10C].
 *
 * Each draw (the game's own and smooth.c's) starts with the surface all key
 * colour; what the CPU still draws over it (HUD, text) is laid over the
 * GPU's world by the presenter (screen_sdl3.c, shaders/hwquad.*,
 * shaders/composite.frag). The presenter scans each frame for what the CPU
 * drew, in bands of rows, uploads only that, and marks it here, so only
 * that is put back to the key: the HUD's few corners, not the whole
 * screen, three times over, every frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "joyride_hle.h"

int joyride_hwrender;                  /* the overlay's "Renderer: GPU" */
static int g_arrow;                    /* the HUD is drawing a direction arrow (postlift.py's splice) */
void joyride_hw_arrow(int on) { g_arrow = on; }
extern uint32_t joyride_texclamp;      /* texfilter.c: the polygon drawn is a tile (lid) */
#define LID_GROW 0.6f                  /* pixels a lid is drawn past each side: no seams */

#define MAXQ 8192
static joyride_quad g_q[MAXQ];
static int g_nq, g_drawing;

uint32_t joyride_tex_page_slot(uint32_t page);
uint32_t joyride_tex_pal_slot(uint32_t pal);

/* the surface the game draws into, from its row table */
static uint32_t surface(uint32_t *pitch) {
    *pitch = MEM32(0x503218);
    return MEM32(joyride_rows);
}

/* What the CPU drew over the key since the last clear, by bands of rows
 * (the presenter scans each frame it gets, shown or not, and marks them):
 * the next clear only has to put those back. A full clear when the surface
 * is new, or the previous draw was not this renderer's. */
#define BAND 64
#define MAXBANDS 64
static int g_dirty_x0[MAXBANDS], g_dirty_x1[MAXBANDS];   /* x1 < x0: clean */
static uint32_t g_last_base, g_last_pitch;
static int g_last_h, g_was_drawing;

void joyride_hw_mark(uint32_t surface, int band, int x0, int x1) {
    (void)surface;                                 /* the frame shown is a copy: the marks are the last draw's */
    if (band < 0 || band >= MAXBANDS) return;
    if (x0 < g_dirty_x0[band]) g_dirty_x0[band] = x0;
    if (x1 > g_dirty_x1[band]) g_dirty_x1[band] = x1;
}

/* A draw starts: forget the last one's quads and put the key back where
 * the CPU drew, so whatever it draws now can be told from the world. */
static float g_ox, g_oy;                           /* this draw's shift (smooth.c) */
void joyride_hw_offset(float *x, float *y) { *x = g_ox, *y = g_oy; }

void joyride_hw_begin(void) {
    g_nq = 0;
    joyride_smooth_offset(&g_ox, &g_oy);
    int was = g_was_drawing;
    g_drawing = joyride_hwrender && joyride_screen_hwrender() && MEM32(0x504CB8) == 32;
    g_was_drawing = g_drawing;
    if (!g_drawing) return;
    int w, h;
    joyride_gbuf_dims(&w, &h);                     /* the surface's pitch in pixels, rows */
    uint32_t pitch, base = surface(&pitch);
    if (!w || !h || !base || pitch != (uint32_t)w * 4 || (h + BAND - 1) / BAND > MAXBANDS) { g_drawing = g_was_drawing = 0; return; }
    uint32_t *p = (uint32_t *)hle_ptr(base);
    static uint32_t *keyrow;
    static int keyw;
    if (keyw < w) {
        free(keyrow);
        if (!(keyrow = malloc((size_t)w * 4))) { keyw = 0; g_drawing = g_was_drawing = 0; return; }
        for (int i = 0; i < w; i++) keyrow[i] = JOYRIDE_KEY;
        keyw = w;
    }
    int full = !was || base != g_last_base || pitch != g_last_pitch || h != g_last_h;
    g_last_base = base, g_last_pitch = pitch, g_last_h = h;
    for (int b = 0; b * BAND < h; b++) {
        int x0 = full ? 0 : g_dirty_x0[b], x1 = full ? w - 1 : g_dirty_x1[b];
        if (x1 >= x0)                              /* copied from a row of key: memcpy is vectorised */
            for (int y = b * BAND; y < h && y < (b + 1) * BAND; y++)
                memcpy(p + (size_t)y * (size_t)w + x0, keyrow, (size_t)(x1 - x0 + 1) * 4);
        g_dirty_x0[b] = w, g_dirty_x1[b] = -1;
    }
}

static joyride_quad *add(uint32_t page, uint32_t mode) {
    if (g_nq >= MAXQ) return NULL;
    joyride_quad *q = &g_q[g_nq++];
    uint32_t pal = MEM32(0x78C10C) & 0xFFFF00FFu;
    q->page = joyride_tex_page_slot(page & ~0xFFFFu);
    q->pal = joyride_tex_pal_slot(pal);
    q->mode = mode;
    return q;
}

static void corner(joyride_quad *q, int i, float x, float y, float u, float v) {
    q->x[i] = x, q->y[i] = y, q->u[i] = u, q->v[i] = v;
}

/* A tile's quad (a lid, a wall, a floor's spans) grows a little past its
 * corners: the game works out each tile's corners on its own, rounded, and
 * draws its last row and column, so two neighbours can miss each other by a
 * pixel; on the GPU the gap showed as a dark line across water and roads.
 * Overlapping by a fraction of a pixel does not show. Not sprites. */
static void grow(joyride_quad *q) {
    float cx = 0, cy = 0;
    for (int i = 0; i < 4; i++) cx += q->x[i], cy += q->y[i];
    cx /= 4, cy /= 4;
    for (int i = 0; i < 4; i++) {
        q->x[i] += q->x[i] > cx ? LID_GROW : q->x[i] < cx ? -LID_GROW : 0;
        q->y[i] += q->y[i] > cy ? LID_GROW : q->y[i] < cy ? -LID_GROW : 0;
    }
}

/* 0x496CF0's entry: 1 = captured (the fill is skipped) */
int joyride_hw_polygon(void) {
    if (!g_drawing) return 0;
    uint32_t p = MEM32(0x78E8C0);
    uint32_t type = MEM16(p + 2), n = MEM16(p + 6), tex = MEM32(p + 8);
    if (n < 3 || n > 4) return 0;
    uint32_t mode = JOYRIDE_QUAD_KEYED | (type != 2 ? JOYRIDE_QUAD_BLEND : 0);
    joyride_quad *q = add(tex, mode);
    if (!q) return 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t k = i < n ? i : n - 1;                       /* a triangle repeats its last corner */
        corner(q, (int)i, (float)(int16_t)MEM16(p + 0x10 + 4 * k), (float)(int16_t)MEM16(p + 0x12 + 4 * k),
               (float)MEM8(p + 0x20 + 2 * k) + 0.5f, (float)MEM8(p + 0x21 + 2 * k) + 0.5f);   /* texel centres */
    }
    if (joyride_texclamp) grow(q);                          /* a lid (the tiles' entry, 0x497710, sets the clamp) */
    /* A direction arrow, at the HUD's size: the game draws it at the size it
     * had on a 640 x 480 screen; it grows about its own middle, staying where
     * the game put it around the player. */
    if (g_arrow) {
        int sw, sh;
        joyride_display_current(&sw, &sh);
        float k = (float)joyride_screen_hud_scale(sh ? sh : 768);
        if (k > 1.0f) {
            float cx = 0, cy = 0;
            for (int i = 0; i < 4; i++) cx += q->x[i] * 0.25f, cy += q->y[i] * 0.25f;
            for (int i = 0; i < 4; i++) q->x[i] = cx + (q->x[i] - cx) * k, q->y[i] = cy + (q->y[i] - cy) * k;
        }
    }
    return 1;
}

/* the wall leaves' texture: page, offsets, flip, see-through */
static joyride_quad *wall(uint32_t axis) {
    uint32_t c80 = MEM32(0x4B8C80);
    uint32_t mode = axis | ((c80 & 0x80) ? JOYRIDE_QUAD_KEYED : 0) | JOYRIDE_QUAD_TILE;
    return add(MEM32(0x4B8C58), mode);
}

/* 0x49A78C's entry: horizontal spans */
int joyride_hw_hwall(void) {
    if (!g_drawing) return 0;
    joyride_quad *q = wall(0);
    if (!q) return 0;
    int32_t c60 = (int32_t)MEM32(0x4B8C60), c64 = (int32_t)MEM32(0x4B8C64), c68 = (int32_t)MEM32(0x4B8C68),
            c6c = (int32_t)MEM32(0x4B8C6C), c70 = (int32_t)MEM32(0x4B8C70), c74 = (int32_t)MEM32(0x4B8C74);
    float ox = (float)(MEM32(0x4B8C84) >> 16), oy = (float)(MEM32(0x4B8C88) >> 16);
    int flip = (MEM32(0x4B8C80) & 0x400000) != 0;
    float ul = flip ? ox + 64 : ox, ur = flip ? ox : ox + 64;
    corner(q, 0, (float)c60, (float)c70, ul, oy);
    corner(q, 1, (float)c64, (float)c70, ur, oy);
    corner(q, 2, (float)c6c, (float)c74, ur, oy + 64);
    corner(q, 3, (float)c68, (float)c74, ul, oy + 64);
    grow(q);
    return 1;
}

/* 0x49ABDC's entry: vertical spans */
int joyride_hw_vwall(void) {
    if (!g_drawing) return 0;
    joyride_quad *q = wall(JOYRIDE_QUAD_VERTICAL);
    if (!q) return 0;
    int32_t c60 = (int32_t)MEM32(0x4B8C60), c64 = (int32_t)MEM32(0x4B8C64), c70 = (int32_t)MEM32(0x4B8C70),
            c74 = (int32_t)MEM32(0x4B8C74), c78 = (int32_t)MEM32(0x4B8C78), c7c = (int32_t)MEM32(0x4B8C7C);
    float ox = (float)(MEM32(0x4B8C84) >> 16), oy = (float)(MEM32(0x4B8C88) >> 16);
    int flip = (MEM32(0x4B8C80) & 0x400000) != 0;
    float ut = flip ? ox + 64 : ox, ub = flip ? ox : ox + 64;
    corner(q, 0, (float)c60, (float)c70, ut, oy);
    corner(q, 1, (float)c64, (float)c74, ut, oy + 64);
    corner(q, 2, (float)c64, (float)c7c, ub, oy + 64);
    corner(q, 3, (float)c60, (float)c78, ub, oy);
    grow(q);
    return 1;
}

/* for the presenter: this frame's quads, in drawing order (-1: the frame
 * is the software's), then done with them */
int joyride_hw_quads(const joyride_quad **q) {
    *q = g_q;
    return g_drawing ? g_nq : -1;
}

void joyride_hw_done(void) { g_drawing = 0, g_nq = 0; }
int joyride_hw_active(void) { return g_drawing; }
