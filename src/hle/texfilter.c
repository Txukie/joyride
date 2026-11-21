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
 * Two ways to do it (joyride_texfilter):
 *   2, on the GPU, as the 3dfx card did: the loops draw the plain texel as
 *      always and joyride_tex_record (texfilter_inline.h, compiled into the
 *      loops) only records, per pixel, where it came
 *      from (texture page slot, palette slot, position to 1/64 texel, the
 *      clamp, and a 17-bit check of the colour written); the presenter
 *      uploads that, the pages and palettes that changed, and
 *      shaders/texfilter.frag re-samples the recorded pixels. A pixel drawn
 *      over afterwards (HUD, text) fails its check and is left alone.
 *   1, on the CPU: joyride_bilerp32 blends at the fetch and the colour load
 *      writes the blend. Where the GPU path is not available (no SPIR-V).
 *
 * 32-bit colour only: in 8-bit modes a blend has no palette entry, and the
 * 16-bit loops are not hooked yet. The F12 overlay switches it (overlay.c).
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "joyride_hle.h"

uint32_t joyride_texfilter = 1;     /* 0 off, 1 CPU, 2 GPU; read by the spliced code */
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
    *c0 = k; *c1 = k < 255 ? k + 1 : 255;
    return a;
}

/* a + (b - a) * f / 256 on all four channels at once: red and blue in one
 * word, alpha and green in the other, each lane wide enough for 255 * 256 */
static inline uint32_t lerp(uint32_t c0, uint32_t c1, uint32_t f) {
    uint32_t g = 256 - f;
    uint32_t rb = (((c0 & 0x00FF00FFu) * g + (c1 & 0x00FF00FFu) * f) >> 8) & 0x00FF00FFu;
    uint32_t ag = ((((c0 >> 8) & 0x00FF00FFu) * g + ((c1 >> 8) & 0x00FF00FFu) * f)) & 0xFF00FF00u;
    return rb | ag;
}

/* transparent loops, some neighbours colour 0: those carry no weight */
static uint32_t blend_some(const uint32_t t[4], const uint32_t c[4], uint32_t a, uint32_t b) {
    uint32_t w[4] = { (256 - a) * (256 - b), a * (256 - b), (256 - a) * b, a * b };
    uint32_t r = 0, g = 0, bl = 0, x = 0, sum = 0;
    for (int i = 0; i < 4; i++) {
        if (!w[i] || !t[i]) continue;
        r += (c[i] >> 16 & 255) * w[i], g += (c[i] >> 8 & 255) * w[i];
        bl += (c[i] & 255) * w[i], x += (c[i] >> 24) * w[i];
        sum += w[i];
    }
    if (!sum) return 0;
    uint32_t h = sum / 2;
    return ((x + h) / sum) << 24 | ((r + h) / sum) << 16 | ((g + h) / sum) << 8 | ((bl + h) / sum);
}

/* The four texels and their colours of the last blend: magnified, a texel
 * covers several pixels in a row, and they all blend the same four. */
static uint32_t g_key = ~0u, g_kpal = ~0u, g_t[4], g_c[4];

uint32_t joyride_bilerp32(uint32_t addr, uint32_t fu, uint32_t fv, uint32_t pal, uint32_t transparent) {
    if (transparent && !GUEST(addr)[0]) return 0;          /* the loop skips this texel anyway */
    uint32_t page = addr & ~0xFFFFu;
    pal &= 0xFFFF00FFu;                                     /* the loop put the texel in bits 8..15 */
    int u0, u1, v0, v1;
    int tile = (int)joyride_texclamp;
    uint32_t a = (uint32_t)split(addr & 0xFF, fu & 0xFFFF, tile, &u0, &u1);
    uint32_t b = (uint32_t)split((addr >> 8) & 0xFF, fv & 0xFFFF, tile, &v0, &v1);
    /* u1, v1 are u0 + 1, v0 + 1 (inside the page): the clamp only moves u0, v0
     * and zeroes the weight, so the four texels follow from the key */
    uint32_t key = page | (uint32_t)v0 << 8 | (uint32_t)u0;
    if (key != g_key || pal != g_kpal) {
        const uint8_t *pg = GUEST(page);
        g_t[0] = pg[v0 << 8 | u0], g_t[1] = pg[v0 << 8 | u1];
        g_t[2] = pg[v1 << 8 | u0], g_t[3] = pg[v1 << 8 | u1];
        for (int i = 0; i < 4; i++) g_c[i] = colour(pal, g_t[i]);
        g_key = key, g_kpal = pal;
    }
    if (transparent && !(g_t[0] && g_t[1] && g_t[2] && g_t[3]) && (a || b))
        return blend_some(g_t, g_c, a, b);
    return lerp(lerp(g_c[0], g_c[1], a), lerp(g_c[2], g_c[3], a), b);
}

/* each polygon: what to clamp to (1 world tile, 0 sprite), and forget the
 * cached texels (a converted-texture slot may hold another tile now) */
void joyride_tex_begin(uint32_t tile) {
    joyride_texclamp = tile;
    g_key = ~0u;
}

/* ---- the GPU path: what each pixel was drawn from ------------------------ */

/* Shared with the per-pixel part, texfilter_inline.h, compiled into the
 * spliced loops themselves. */
uint32_t *joyride_gb;               /* 2 words a pixel, rows of the surface's pitch */
uint32_t joyride_gb_n;              /* pixels in it */
int joyride_gb_used;                /* something recorded since the last frame */
uint32_t joyride_gb_last_page = ~0u, joyride_gb_last_pal = ~0u;   /* a span keeps to one of each */
uint32_t joyride_gb_last_page_slot, joyride_gb_last_pal_slot;
#define g_gb joyride_gb
#define g_gb_used joyride_gb_used
#define g_last_page joyride_gb_last_page
#define g_last_pal joyride_gb_last_pal
#define g_last_page_slot joyride_gb_last_page_slot
#define g_last_pal_slot joyride_gb_last_pal_slot
static int g_gw, g_gh;              /* pixels a row (pitch / 4), rows */

/* The record, 2 words a pixel (shaders/texfilter.frag reads it):
 *   0: u | v << 14 | check >> 13 << 28, u and v 8.6 in the page
 *   1: page slot (7 bits, 0 = none) | palette slot << 7 (10) |
 *      transparent << 17 | tile << 18 | check << 19 (low 13 of 17)
 * The 17-bit check of the colour written lets a pixel drawn over later (or
 * left from an earlier frame) be told apart, so nothing is cleared. */
#define MAXPAGES 128
#define MAXPALS 1024

/* texture pages (64 KB, 256x256 texels) by guest address >> 16 -> slot 1..127 */
static uint8_t g_page_slot[65536];
static uint32_t g_page_addr[256];
static uint64_t g_page_hash[256];
static uint8_t g_page_used[256];
static int g_npages = 1;

/* shading tables (256 colours, 256 bytes apart) -> slot 0..1023 */
#define NPAL 4096
static uint32_t g_pal_key[NPAL * 2];   /* open addressing: key, slot + 1 */
static uint32_t g_pal_addr[NPAL];
static uint64_t g_pal_hash[NPAL];
static uint8_t g_pal_used[NPAL];
static int g_npals;

static uint8_t *g_frozen[256];
static uint8_t g_free[256];
static int g_nfree;
static int g_resets;
static void reset_slots(void) {
    g_resets++;
    g_last_pal = g_last_page = ~0u;
    for (int i = 0; i < 256; i++) free(g_frozen[i]), g_frozen[i] = NULL;
    g_nfree = 0;
    memset(g_page_slot, 0, sizeof g_page_slot), memset(g_page_hash, 0, sizeof g_page_hash);
    memset(g_page_used, 0, sizeof g_page_used), g_npages = 1;
    memset(g_pal_key, 0, sizeof g_pal_key), memset(g_pal_hash, 0, sizeof g_pal_hash);
    memset(g_pal_used, 0, sizeof g_pal_used), g_npals = 0;
}

/* Pages the game rewrites mid-frame (its cache of converted tiles, 0x49B044,
 * reuses slots as it goes): the pixels recorded so far need the texels as
 * they were, so the page's slot keeps a copy of them and the page moves to
 * a new slot for what comes after. Frozen slots are uploaded from the copy
 * at the next frame and then freed. */

static int new_slot(void) {
    if (g_nfree) return g_free[--g_nfree];
    if (g_npages >= MAXPAGES) reset_slots();
    return g_npages++;
}

static int page_slot(uint32_t page) {
    if (page == g_last_page) return (int)g_last_page_slot;
    uint32_t k = page >> 16;
    int s = g_page_slot[k];
    if (!s) {
        s = new_slot();
        g_page_slot[k] = (uint8_t)s, g_page_addr[s] = page, g_page_hash[s] = 0;
    }
    g_page_used[s] = 1;
    g_last_page = page, g_last_page_slot = (uint32_t)s;
    return s;
}

/* Spliced at the tile converter's entry (0x49B044): about to write the
 * 64x64 tile at `dst` (its rotated copy in the cache). */
void joyride_tex_overwrite(uint32_t dst) {
    if (joyride_texfilter != 2) return;
    uint32_t k = dst >> 16;
    int s = g_page_slot[k];
    if (!s || !g_page_used[s] || g_frozen[s]) return;
    if (!(g_frozen[s] = malloc(65536))) return;
    memcpy(g_frozen[s], GUEST(k << 16), 65536);
    g_page_slot[k] = 0;                                     /* the next use gets a new slot */
    g_last_page = ~0u;
}


static int pal_slot(uint32_t pal) {
    if (pal == g_last_pal) return (int)g_last_pal_slot;
    for (;;) {
        uint32_t h = (pal * 2654435761u) >> 20;            /* 12 bits */
        for (int i = 0; i < 64; i++) {
            uint32_t *e = &g_pal_key[2 * ((h + (uint32_t)i) & (NPAL - 1))];
            if (e[1] && e[0] == pal) {
                g_pal_used[e[1] - 1] = 1;
                g_last_pal = pal, g_last_pal_slot = e[1] - 1;
                return (int)g_last_pal_slot;
            }
            if (!e[1]) {
                if (g_npals >= MAXPALS) break;
                int s = g_npals++;
                e[0] = pal, e[1] = (uint32_t)s + 1;
                g_pal_addr[s] = pal, g_pal_hash[s] = 0, g_pal_used[s] = 1;
                g_last_pal = pal, g_last_pal_slot = (uint32_t)s;
                return s;
            }
        }
        reset_slots();                                       /* full: start over */
    }
}


/* The per-pixel recording is texfilter_inline.h, in the loops; these are
 * its slow paths, for a page or palette other than the last one. */
uint32_t joyride_tex_page_slot(uint32_t page) { return (uint32_t)page_slot(page); }
uint32_t joyride_tex_pal_slot(uint32_t pal) { return (uint32_t)pal_slot(pal); }

/* ---- for the presenter ---------------------------------------------------- */

/* the surface the game draws into: pitch in pixels, rows */
void joyride_gbuf_size(int pitch_px, int rows) {
    if (pitch_px == g_gw && rows == g_gh && g_gb) return;
    free(g_gb);
    g_gw = pitch_px, g_gh = rows;
    g_gb = calloc((size_t)pitch_px * (size_t)rows * 2, 4);
    joyride_gb_n = g_gb ? (uint32_t)pitch_px * (uint32_t)rows : 0;
    g_gb_used = 0;
}

/* the recording of the frame just drawn, if any */
int joyride_gbuf_frame(const uint32_t **gb, int *pitch_px, int *rows) {
    if (!g_gb_used || !g_gb) return 0;
    *gb = g_gb, *pitch_px = g_gw, *rows = g_gh;
    return 1;
}

static uint64_t hash(const uint8_t *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 29;
    }
    return h | 1;
}

/* pages used since the last frame whose texels changed since uploaded:
 * calls `up` with slot and 64 KB; palettes likewise with 256 colours */
void joyride_gbuf_changes(void (*page)(void *ctx, int slot, const uint8_t *texels),
                          void (*pal)(void *ctx, int slot, const uint32_t *colours), void *ctx) {
    for (int s = 1; s < g_npages; s++) {
        if (g_frozen[s]) {                                   /* the copy, once; then the slot is free */
            page(ctx, s, g_frozen[s]);
            g_page_used[s] = 0, g_page_hash[s] = 0;
            continue;
        }
        if (!g_page_used[s]) continue;
        g_page_used[s] = 0;
        const uint8_t *d = GUEST(g_page_addr[s]);
        uint64_t h = hash(d, 65536);
        if (h != g_page_hash[s]) { g_page_hash[s] = h; page(ctx, s, d); }
    }
    uint32_t c[256];
    for (int s = 0; s < g_npals; s++) {
        if (!g_pal_used[s]) continue;
        g_pal_used[s] = 0;
        for (int t = 0; t < 256; t++) c[t] = colour(g_pal_addr[s], (uint32_t)t) | 0xFF000000u;
        uint64_t h = hash((const uint8_t *)c, sizeof c);
        if (h != g_pal_hash[s]) { g_pal_hash[s] = h; pal(ctx, s, c); }
    }
}

/* the frame is on its way: start the next recording clean */
void joyride_gbuf_done(void) {
    g_last_pal = g_last_page = ~0u;                           /* the used marks start over */
    for (int s = 1; s < g_npages; s++)                        /* frozen copies were uploaded */
        if (g_frozen[s]) { free(g_frozen[s]); g_frozen[s] = NULL; g_free[g_nfree++] = (uint8_t)s; }
    static int n;
    if (getenv("JOYRIDE_GBUF_DEBUG") && n++ % 50 == 0)
        fprintf(stderr, "[gbuf] pages %d, palettes %d, resets %d\n", g_npages - 1, g_npals, g_resets);
    g_gb_used = 0;                                            /* (stale entries fail their check) */
}

/* everything uploaded is gone (the GPU textures were remade) */
void joyride_gbuf_forget(void) {
    memset(g_page_hash, 0, sizeof g_page_hash), memset(g_pal_hash, 0, sizeof g_pal_hash);
}

/* filtering on or off: on the GPU when the presenter can (SPIR-V), else CPU */
void joyride_texfilter_set(int on) {
    const char *e = getenv("JOYRIDE_FILTER");               /* JOYRIDE_FILTER=cpu: never the GPU */
    int cpu = e && !strcmp(e, "cpu");
    joyride_texfilter = !on ? 0 : !cpu && joyride_screen_gpu_filter() ? 2 : 1;
}
