/*
 * joyride - bilinear texture filtering for the software renderer, the one
 * visual feature of GTA's 3dfx build (GTADOS/GTAFX.EXE, docs/exe-3dfx.md).
 *
 * The 2002 exe draws the world with hand-written span loops (0x498E32 to
 * 0x49B001). Each texel fetch reads one byte from a 256x256 8-bit texture
 * page, `ebx` holding the address (low byte u, next byte v, the rest the
 * page), then looks the colour up in the face's shading table: the 32-bit
 * pixel at palette + texel * 256. tools/postlift.py splices two calls into
 * those loops: at the fetch, joyride_bilerp32() blends the four texels
 * around the exact (sub-texel) sample position, each through the same
 * shading table; at the colour load, the loop's colour register takes the
 * blend instead. Geometry, draw order, clipping and the HUD stay the game's.
 *
 * World textures are 64x64 tiles inside the page; their neighbours are
 * clamped to the tile so faces never bleed into the next texture. Sprites
 * (cars, people) are packed at any size, so they clamp to the page only and
 * rely on their transparent border; the polygon entry points set which. In loops that skip
 * colour 0 (transparent texels), zero neighbours carry no weight, so
 * cut-out edges keep their original shape and never pick up a dark fringe.
 *
 * 32-bit colour only: in 8-bit modes a blend has no palette entry, and the
 * 16-bit loops are not hooked yet. F10 toggles (JOYRIDE_FILTER=0 starts off).
 */
#include <stdint.h>
#include <stdlib.h>

#include "joyride_hle.h"

uint32_t joyride_texfilter = 1;     /* read by the spliced code; F10 toggles */
uint32_t joyride_texc;              /* the blend, from fetch to colour load */
uint32_t joyride_texclamp = 1;      /* 1: world tile (64x64), 0: sprite (whole page) */

#define GUEST(a) ((const uint8_t *)(uintptr_t)(RECOMP_MEM_BASE + (uint32_t)(a)))

static inline uint32_t texel(uint32_t page, int u, int v) {
    return GUEST(page)[(v << 8) | u];
}

static inline uint32_t colour(uint32_t pal, uint32_t t) {
    return *(const uint32_t *)GUEST(pal + (t << 8));
}

/* one coordinate: integer part `i` (absolute in the page), fraction `f`
 * (16 bits); returns the two neighbouring texel columns/rows clamped to the
 * 64-texel tile (or the page) and the weight (0..256) of the second */
static inline int split(uint32_t i, uint32_t f, int tile, int *c0, int *c1) {
    int lo = tile ? (int)(i & ~63u) : 0, hi = tile ? lo + 63 : 255;
    int p = (int)((i << 8) | (f >> 8)) - 128;            /* texel centres */
    int a = p & 255, k = p >> 8;
    if (k < lo) { k = lo; a = 0; }
    if (k >= hi) { k = hi; a = 0; }
    *c0 = k; *c1 = k + 1;
    return a;
}

uint32_t joyride_bilerp32(uint32_t addr, uint32_t fu, uint32_t fv, uint32_t pal, uint32_t transparent) {
    if (transparent && !GUEST(addr)[0]) return 0;          /* the loop skips this texel anyway */
    uint32_t page = addr & ~0xFFFFu;
    pal &= 0xFFFF00FFu;                                     /* the loop put the texel in bits 8..15 */
    int u0, u1, v0, v1;
    int tile = (int)joyride_texclamp;
    int a = split(addr & 0xFF, fu & 0xFFFF, tile, &u0, &u1);
    int b = split((addr >> 8) & 0xFF, fv & 0xFFFF, tile, &v0, &v1);
    uint32_t t[4] = { texel(page, u0, v0), texel(page, u1, v0), texel(page, u0, v1), texel(page, u1, v1) };
    int w[4] = { (256 - a) * (256 - b), a * (256 - b), (256 - a) * b, a * b };
    uint32_t r = 0, g = 0, bl = 0, x = 0, sum = 0;
    for (int i = 0; i < 4; i++) {
        if (!w[i] || (transparent && !t[i])) continue;
        uint32_t c = colour(pal, t[i]);
        r += (c >> 16 & 255) * (uint32_t)w[i];
        g += (c >> 8 & 255) * (uint32_t)w[i];
        bl += (c & 255) * (uint32_t)w[i];
        x += (c >> 24) * (uint32_t)w[i];
        sum += (uint32_t)w[i];
    }
    if (!sum) return colour(pal, (uint32_t)GUEST(addr)[0]);
    uint32_t h = sum / 2;
    return ((x + h) / sum) << 24 | ((r + h) / sum) << 16 | ((g + h) / sum) << 8 | ((bl + h) / sum);
}

void joyride_texfilter_init(void) {
    const char *e = getenv("JOYRIDE_FILTER");
    if (e && *e == '0') joyride_texfilter = 0;
}
