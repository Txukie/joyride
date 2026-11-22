/*
 * joyride - the city map (Select on the controller, M on the keyboard).
 *
 * GTA has no map picture, so joyride draws one per city from the game's
 * own data, the first time it runs, and keeps it in
 * ~/.local/share/joyride/maps/<city>.bmp:
 *
 *   <city>.CMP  28-byte header (version, style number, ...; then the sizes
 *               of the route, object, column, block and nav sections), a
 *               256x256 table of column offsets, the columns (empty levels
 *               at the top, then that many fewer than 6 block numbers, from
 *               the bottom up) and the blocks (8 bytes: type bits, the
 *               lid's rotation in the top two; an extra byte, remap in
 *               bits 3-4 and flips in 5-6; then the left, right, top,
 *               bottom and lid tile numbers).
 *   STYLE00n.G24  64-byte header of section sizes; side, lid and aux tiles
 *               (64x64, 8-bit, 16 to a 256x256 page); the colour tables (256
 *               colours each, 64 to a 64 KB page, colour c of table k at
 *               c * 256 + k * 4, the same layout the game's renderer reads);
 *               the palette index (4 entries a tile) and the rest.
 *
 * Each of the 65536 cells is the lid of its topmost block with one (lid L
 * is tile sides + L), rotated and flipped as the block says, averaged down
 * to 8x8: a 2048x2048 picture.
 *
 * In play the map shows over the game at 70% (the presenter's map pass,
 * shaders/map.frag): the player as a bright blue dot (on foot the ped's
 * position, ped +0x90/+0x94; driving, the car's centre), the game's objective
 * arrows (0x784234, kind at 0x784744: a car or a person by id, or a
 * point; the red arrow at 0x785110 when 0x78472C is set) as bright red
 * dots. The current city is the map file the game loaded ([0x6B3E88]).
 */
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define CELL 8
#define MAPPX (256 * CELL)

static const char *const k_cities[] = { "nyc", "sanb", "miami" };
#define NCITY 3

static char g_root[4096], g_dir[4096];
static int g_open;

/* ---- building -------------------------------------------------------------- */

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = len > 0 ? malloc((size_t)len) : NULL;
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) { free(d); d = NULL; }
    fclose(f);
    *n = d ? (size_t)len : 0;
    return d;
}

/* GTADATA/<name>, whatever the case on disk */
static uint8_t *game_file(const char *name, size_t *n) {
    char path[4200], up[64];
    size_t i = 0;
    for (; name[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)name[i]);
    up[i] = 0;
    snprintf(path, sizeof path, "%s/GTADATA/%s", g_root, up);
    uint8_t *d = slurp(path, n);
    if (!d) { snprintf(path, sizeof path, "%s/gtadata/%s", g_root, name); d = slurp(path, n); }
    return d;
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void write_bmp(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t row = (uint32_t)w * 3, size = 54 + row * (uint32_t)h;
    uint8_t hd[54] = { 'B', 'M' };
    memcpy(hd + 2, &size, 4);
    hd[10] = 54, hd[14] = 40;
    memcpy(hd + 18, &w, 4);
    int nh = -h;                                           /* top-down */
    memcpy(hd + 22, &nh, 4);
    hd[26] = 1, hd[28] = 24;
    fwrite(hd, 1, 54, f);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            uint8_t bgr[3] = { p[2], p[1], p[0] };
            fwrite(bgr, 1, 3, f);
        }
    fclose(f);
}

static uint8_t *read_bmp(const char *path) {               /* our own files only */
    size_t n;
    uint8_t *d = slurp(path, &n);
    if (!d || n != 54 + (size_t)MAPPX * MAPPX * 3 || d[0] != 'B') { free(d); return NULL; }
    uint8_t *rgba = malloc((size_t)MAPPX * MAPPX * 4);
    for (size_t i = 0; rgba && i < (size_t)MAPPX * MAPPX; i++) {
        const uint8_t *p = d + 54 + i * 3;
        rgba[4 * i] = p[0], rgba[4 * i + 1] = p[1], rgba[4 * i + 2] = p[2], rgba[4 * i + 3] = 255;   /* BGRA */
    }
    free(d);
    return rgba;
}

static int build(const char *city, const char *out) {
    char name[32];
    size_t cn, sn;
    snprintf(name, sizeof name, "%s.cmp", city);
    uint8_t *cmp = game_file(name, &cn);
    if (!cmp || cn < 28 + 262144) { free(cmp); return 0; }
    snprintf(name, sizeof name, "style%03u.g24", cmp[4]);
    uint8_t *sty = game_file(name, &sn);
    if (!sty || sn < 64) { free(cmp); free(sty); return 0; }

    uint32_t col_size = u32(cmp + 16), blk_size = u32(cmp + 20);
    const uint8_t *base = cmp + 28, *cols = base + 262144, *blks = cols + col_size;
    uint32_t h[16];
    for (int i = 0; i < 16; i++) h[i] = u32(sty + 4 * i);
    uint32_t nside = h[1] / 4096, ntiles = (h[1] + h[2] + h[3]) / 4096;
    size_t clut0 = 64 + (size_t)h[1] + h[2] + h[3] + h[4];
    size_t rest = (size_t)h[10] + h[11] + h[12] + h[13] + h[14] + h[15];
    size_t pal0 = sn - rest;                               /* the colour tables are padded */
    if ((size_t)(blks - cmp) + blk_size > cn || pal0 <= clut0 || pal0 + h[10] > sn) { free(cmp); free(sty); return 0; }

    uint8_t *rgb = calloc((size_t)MAPPX * MAPPX, 3);
    for (int cy = 0; cy < 256; cy++)
        for (int cx = 0; cx < 256; cx++) {
            uint32_t off = u32(base + 4 * (cy * 256 + cx));
            if (off + 2 > col_size) continue;
            const uint8_t *c = cols + off;
            int empty = u16(c), n = 6 - empty;
            const uint8_t *b = NULL;
            for (int k = n - 1; k >= 0; k--) {                /* from the top down */
                if ((size_t)(c + 2 + 2 * k + 2 - cols) > col_size) break;
                uint32_t bi = u16(c + 2 + 2 * k);
                if ((size_t)bi * 8 + 8 > blk_size) continue;
                if (blks[bi * 8 + 7]) { b = blks + bi * 8; break; }
            }
            if (!b) continue;
            uint32_t tile = nside + b[7], tm = u16(b), ext = b[2];
            if (tile >= ntiles || tile * 4 + 3 >= h[10] / 2) continue;
            uint32_t clut = u16(sty + pal0 + 2 * (tile * 4 + ((ext >> 3) & 3)));
            size_t cl = clut0 + (size_t)(clut / 64) * 65536 + (clut % 64) * 4;
            const uint8_t *px = sty + 64 + (size_t)(tile / 16) * 65536 + ((tile % 16) / 4) * 64 * 256 + (tile % 4) * 64;
            int rot = tm >> 14, flipx = ext & 0x40, flipy = ext & 0x20;
            for (int oy = 0; oy < CELL; oy++)
                for (int ox = 0; ox < CELL; ox++) {
                    unsigned r = 0, g = 0, bl = 0;
                    for (int sy = 0; sy < 64 / CELL; sy++)
                        for (int sx = 0; sx < 64 / CELL; sx++) {
                            int u = ox * (64 / CELL) + sx, v = oy * (64 / CELL) + sy, t;
                            /* the cell's (u, v) in the tile: undo the flips, then the rotation */
                            if (flipx) u = 63 - u;
                            if (flipy) v = 63 - v;
                            for (int k = 0; k < rot; k++) t = u, u = 63 - v, v = t;
                            uint8_t i = px[v * 256 + u];
                            if (cl + (size_t)i * 256 + 2 >= sn) continue;
                            const uint8_t *col = sty + cl + (size_t)i * 256;
                            bl += col[0], g += col[1], r += col[2];
                        }
                    int cnt = (64 / CELL) * (64 / CELL);
                    uint8_t *o = rgb + (((size_t)cy * CELL + oy) * MAPPX + (size_t)cx * CELL + ox) * 3;
                    o[0] = (uint8_t)(r / cnt), o[1] = (uint8_t)(g / cnt), o[2] = (uint8_t)(bl / cnt);
                }
        }
    write_bmp(out, rgb, MAPPX, MAPPX);
    free(rgb), free(cmp), free(sty);
    return 1;
}

static void *build_all(void *arg) {
    (void)arg;
    for (int i = 0; i < NCITY; i++) {
        char path[4200];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s.bmp", g_dir, k_cities[i]);
        if (stat(path, &st) == 0) continue;
        char tmp[4300];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if (build(k_cities[i], tmp)) { rename(tmp, path); fprintf(stderr, "[map] built %s\n", path); }
        else fprintf(stderr, "[map] could not build the %s map\n", k_cities[i]);
    }
    return NULL;
}

/* At start-up: build the maps not built yet, in the background. */
void joyride_map_init(const char *install_root, const char *data_dir) {
    snprintf(g_root, sizeof g_root, "%s", install_root);
    snprintf(g_dir, sizeof g_dir, "%s/maps", data_dir);
    mkdir(g_dir, 0755);
    pthread_t t;
    if (pthread_create(&t, NULL, build_all, NULL) == 0) pthread_detach(t);
}

/* ---- in play ------------------------------------------------------------- */

void joyride_map_toggle(void) { g_open = !g_open; }
int joyride_map_open(void) { return g_open; }


/* the city now: the map file the game loaded */
static int city(void) {
    char name[16] = { 0 };
    for (int i = 0; i < 15; i++) name[i] = (char)tolower(MEM8(0x6B3E88 + i));
    for (int i = 0; i < NCITY; i++)
        if (!strncmp(name, k_cities[i], strlen(k_cities[i])) && name[strlen(k_cities[i])] == '.') return i;
    return -1;
}

/* The picture for the city in play (BGRA, MAPPX square; NULL if none yet):
 * `*id` changes when it does. */
const uint8_t *joyride_map_picture(int *size, int *id) {
    static int loaded = -1;
    static uint8_t *pic;
    int c = city();
    if (c < 0) return NULL;
    if (c != loaded) {
        char path[4200];
        snprintf(path, sizeof path, "%s/%s.bmp", g_dir, k_cities[c]);
        uint8_t *p = read_bmp(path);
        if (!p) return NULL;                               /* still building */
        free(pic);
        pic = p, loaded = c;
    }
    *size = MAPPX, *id = loaded;
    return pic;
}

static int pos_ped(uint32_t id, float *x, float *y) {
    uint32_t p = 0x7284E0u + (id & 0xFFFF) * 256u;
    *x = (float)(int32_t)MEM32(p + 0x90) / 65536.0f, *y = (float)(int32_t)MEM32(p + 0x94) / 65536.0f;
    return 1;
}
/* cars: 688 bytes at 0x4BE248; the four corners' x at +0x38..+0x44 and
 * the centre's at +0x48, the corners' y at +0x4C..+0x58 and the centre's
 * at +0x5C (16.16, pixels) */
static int pos_car(uint32_t id, float *x, float *y) {
    uint32_t c = 0x4BE248u + (id & 0xFFFF) * 688u;
    *x = (float)(int32_t)MEM32(c + 0x48) / 65536.0f, *y = (float)(int32_t)MEM32(c + 0x5C) / 65536.0f;
    return 1;
}

/* Dots, in map units (0..1 across the city): the player's, then up to
 * `max` objectives. Returns how many objectives. */
int joyride_map_dots(float player[2], float obj[][2], int max) {
    uint32_t rec = 0x74F160u + MEM32(0x74F83C) * 444u;
    float x = 0, y = 0;
    uint32_t st = MEM32(rec + 0xA4), id = MEM32(rec + 0xA8);
    if (st == 2) pos_ped(id, &x, &y);
    else pos_car(id, &x, &y);
    player[0] = x / 16384.0f, player[1] = y / 16384.0f;
    int n = 0;
    if (MEM32(0x784234) && n < max) {
        uint32_t kind = MEM32(0x784744), oid = MEM16(0x784748);
        int ok = 1;
        if (kind == 0) ok = pos_car(oid, &x, &y);
        else if (kind == 3) ok = pos_ped(oid, &x, &y);
        else if (kind == 1) x = (float)(int32_t)MEM32(0x78474C) / 65536.0f, y = (float)(int32_t)MEM32(0x784750) / 65536.0f;
        else ok = 0;
        if (ok) obj[n][0] = x / 16384.0f, obj[n][1] = y / 16384.0f, n++;
    }
    if (MEM32(0x78472C) && n < max) {
        obj[n][0] = (float)(int32_t)MEM32(0x785110) / 65536.0f / 16384.0f;
        obj[n][1] = (float)(int32_t)MEM32(0x785114) / 65536.0f / 16384.0f;
        n++;
    }
    return n;
}
