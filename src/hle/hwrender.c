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
 * Each draw (the game's own and smooth.c's) starts by clearing the surface
 * to a key colour; what the CPU still draws over it (HUD, text) is laid
 * over the GPU's world by the presenter (screen_sdl3.c, shaders/hwquad.*,
 * shaders/composite.frag).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "joyride_hle.h"

int joyride_hwrender;                  /* the overlay's "Renderer: GPU" */

#define MAXQ 8192
static joyride_quad g_q[MAXQ];
static int g_nq, g_drawing;

uint32_t joyride_tex_page_slot(uint32_t page);
uint32_t joyride_tex_pal_slot(uint32_t pal);

/* the surface the game draws into, from its row table */
static uint32_t surface(uint32_t *pitch) {
    *pitch = MEM32(0x503218);
    return MEM32(0x503228);
}

/* A draw starts: forget the last one's quads and clear the surface to the
 * key, so whatever the CPU draws over it can be told from the world. */
void joyride_hw_begin(void) {
    g_nq = 0;
    g_drawing = joyride_hwrender && joyride_screen_hwrender() && MEM32(0x504CB8) == 32;
    if (!g_drawing) return;
    int w, h;
    joyride_gbuf_dims(&w, &h);                     /* the surface's pitch in pixels, rows */
    uint32_t pitch, base = surface(&pitch);
    if (!w || !h || !base || pitch != (uint32_t)w * 4) { g_drawing = 0; return; }
    uint32_t *p = (uint32_t *)hle_ptr(base);
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) p[i] = JOYRIDE_KEY;
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
    return 1;
}

/* for the presenter: this frame's quads, in drawing order (-1: the frame
 * is the software's), then done with them */
int joyride_hw_quads(const joyride_quad **q) {
    *q = g_q;
    return g_drawing ? g_nq : -1;
}

void joyride_hw_done(void) { g_drawing = 0, g_nq = 0; }
