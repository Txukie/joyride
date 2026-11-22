/*
 * joyride - debugging aids in play (the controller's d-pad left and right;
 * F7 and F8 on the keyboard).
 *
 * Left (F7): a report. joyride keeps the last 10 seconds of play in memory,
 * each game step (the player, the car, the camera, the controls) and each
 * frame drawn (its time, its place between steps, the camera it used), and
 * writes them to <data>/debug/<time>/report.txt, with the next 10 frames
 * on screen (screen_NN.png: the GPU's picture; cpu.png: the game's
 * surface; frames.txt: when each was shown) and the game's memory
 * (memory.bin, 0x4AB000..0x791000). The log line says where.
 *
 * Right (F8): all weapons (99 rounds each: player +0x13F + slot, slots
 * 1-4) and, in a vehicle, the next model the style has (swap_model).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "win32hle.h"
#include "joyride_hle.h"

static char g_dir[4200];

void joyride_debug_init(const char *data_dir) {
    snprintf(g_dir, sizeof g_dir, "%s/debug", data_dir);
}

/* ---- the record ------------------------------------------------------------ */

#define NSTEP 256                      /* 11 s of steps */
#define NFRAME 2048                    /* 10 s at 200 fps */

typedef struct {
    uint64_t ns;
    uint32_t step, state, id, word;
    int32_t ped_x, ped_y, ped_h;
    int32_t car_model, car_x, car_y, car_z, car_h, car_f0;
    float car_force;
    int32_t cam_x, cam_y, cam_z, cam_scale, cam_zoom;
    float pad[6], share_f, share_b;
    uint32_t buttons;
    int32_t chain_kind, chain_n, chain_t, score;   /* the kills-in-a-row the game counts (0x462000) */
} step_t;

typedef struct {
    uint64_t ns, slot_ns;
    uint32_t step;
    int32_t t, cam_x, cam_y, cam_scale;
    int kind;                          /* 0 the game's own frame, 1 joyride's, 2 presented */
} frame_t;

static step_t g_steps[NSTEP];
static frame_t g_frames[NFRAME];
static uint32_t g_ns, g_nf, g_step;

static uint32_t rec(void) { return 0x74F160u + MEM32(0x74F83C) * 444u; }

/* Cars that turn up above the street: every step each car's height is
 * compared with the last; one set up anew (its place jumped) or risen by
 * more than half a level is noted, with where it is and what drives it.
 * Street level is z 255 (16.16 >> 16; smaller is higher). */
#define NCARS 400
#define NCAREV 128
/* each car's last setup (0x4067C0, spliced at its callers): where it was
 * called from, the height asked for, and the guest stack above it, which
 * holds the arguments of the spawners further up */
#define NSTACK 16
typedef struct { uint32_t site, step; int32_t z; uint32_t stack[NSTACK]; } made_t;
static made_t g_made[NCARS];
void joyride_debug_car_made(uint32_t site, uint32_t esp) {
    int slot = (int16_t)MEM16(esp + 20);
    if (slot < 0 || slot >= NCARS) return;
    made_t *m = &g_made[slot];
    m->site = site, m->step = g_step, m->z = (int32_t)MEM32(esp + 8);
    for (int i = 0; i < NSTACK; i++) m->stack[i] = MEM32(esp + 4u * (uint32_t)i);
}

typedef struct { uint32_t step; int id, model, driver, f6, x, y, z, px, py, pz; made_t made; } carev_t;
static carev_t g_carev[NCAREV];
static uint32_t g_ncarev;
static int32_t g_cx[NCARS], g_cy[NCARS], g_cz[NCARS];
static void watch_cars(void) {
    for (int i = 0; i < NCARS; i++) {
        uint32_t c = 0x4BE248u + (uint32_t)i * 688u;
        int32_t x = (int32_t)MEM32(c + 0x250) >> 16, y = (int32_t)MEM32(c + 0x254) >> 16, z = (int32_t)MEM32(c + 0x258) >> 16;
        int moved = abs(x - g_cx[i]) > 256 || abs(y - g_cy[i]) > 256;
        if ((x || y) && z < 200 && (moved || g_cz[i] - z > 32)) {
            carev_t *e = &g_carev[g_ncarev++ % NCAREV];
            *e = (carev_t){ g_step, i, (int16_t)MEM16(c + 0x22), (int16_t)MEM16(c + 2), (int16_t)MEM16(c + 6), x, y, z,
                            g_cx[i], g_cy[i], g_cz[i], g_made[i] };
        }
        g_cx[i] = x, g_cy[i] = y, g_cz[i] = z;
    }
}

/* each game step (from the input reader, 0x433141) */
void joyride_debug_step(uint32_t word) {
    step_t *s = &g_steps[g_ns++ % NSTEP];
    memset(s, 0, sizeof *s);
    uint32_t r = rec();
    watch_cars();
    s->ns = SDL_GetTicksNS(), s->step = ++g_step, s->word = word ? MEM32(word) : 0;
    s->state = MEM32(r + 0xA4), s->id = MEM32(r + 0xA8) & 0xFFFF;
    if (s->state == 2) {
        uint32_t p = 0x7284E0u + s->id * 256u;
        s->ped_x = (int32_t)MEM32(p + 0x90), s->ped_y = (int32_t)MEM32(p + 0x94), s->ped_h = MEM16(p + 0xA8);
    } else {
        uint32_t c = 0x4BE248u + s->id * 688u;
        s->car_model = MEM16(c + 0x22), s->car_h = MEM16(c + 0x268), s->car_f0 = MEM16(c + 0xF0);
        s->car_x = (int32_t)MEM32(c + 0x250), s->car_y = (int32_t)MEM32(c + 0x254), s->car_z = (int32_t)MEM32(c + 0x258);
        memcpy(&s->car_force, hle_ptr(c + 0x188), 4);
    }
    s->cam_x = (int32_t)MEM32(r + 0x1C), s->cam_y = (int32_t)MEM32(r + 0x20), s->cam_z = (int32_t)MEM32(r + 0x18);
    s->cam_scale = (int32_t)MEM32(r + 0x24), s->cam_zoom = (int32_t)MEM32(r + 0x28);
    joyride_pad_state(s->pad, &s->buttons, &s->share_f, &s->share_b);
    /* the player's kills of one kind in a row (Gouranga: the 7th of kind 7,
     * the Krishnas, each within 13 steps of the last) and the score */
    uint32_t pl = 0x74F160u + 0x1BCu * (MEM32(0x74F83C) & 3);
    s->chain_kind = (int32_t)MEM32(pl + 0x158), s->chain_n = (int16_t)MEM16(pl + 0x15C);
    s->chain_t = (int16_t)MEM16(pl + 0x154), s->score = (int32_t)MEM32(pl + 0xDC);
}

/* each frame drawn (smooth.c, after the camera is placed for it), and each
 * frame presented (the presenter) */
void joyride_debug_frame(int kind, int32_t t, uint64_t slot_ns) {
    frame_t *f = &g_frames[g_nf++ % NFRAME];
    uint32_t r = rec();
    f->ns = SDL_GetTicksNS(), f->slot_ns = slot_ns, f->step = g_step, f->t = t, f->kind = kind;
    f->cam_x = (int32_t)MEM32(r + 0x1C), f->cam_y = (int32_t)MEM32(r + 0x20), f->cam_scale = (int32_t)MEM32(r + 0x24);
}

/* ---- the report ------------------------------------------------------------ */

static void save_memory(const char *dir) {
    char path[4400];
    snprintf(path, sizeof path, "%s/memory.bin", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(hle_ptr(0x4AB000), 1, 0x791000 - 0x4AB000, f);
    fclose(f);
}

void joyride_debug_report(void) {
    if (!g_dir[0]) return;
    char stamp[64], dir[4300], path[4400];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    mkdir(g_dir, 0755);
    snprintf(dir, sizeof dir, "%s/%s", g_dir, stamp);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[debug] can't write %s\n", path); return; }

    const joyride_settings *c = &joyride_cfg;
    int sw, sh;
    joyride_display_current(&sw, &sh);
    fprintf(f, "joyride debug report %s\n", stamp);
    fprintf(f, "settings: resolution %dx%d (in play %dx%d, %u bits), filter %d (mode %u), scaling %d, fullscreen %d, smooth %d, renderer %s, controller %s\n",
            c->res_w, c->res_h, sw, sh, MEM32(0x504CB8), c->filter, joyride_texfilter, c->scaling, c->fullscreen, c->smooth,
            c->hwrender ? "gpu" : "software", c->controls_modern ? "modern" : "classic");
    fprintf(f, "frame period %.3f ms (cap), hw renderer active %d\n", joyride_screen_frame_ns() / 1e6, joyride_hw_active());
    int gp, gq, gr, gm, gmp;
    joyride_gbuf_stats(&gp, &gq, &gr, &gm, &gmp);
    fprintf(f, "texture slots: pages %d of %d, palettes %d; started over %d times (%d mid-frame, %d of them for palettes)\n",
            gp, joyride_tex_gpu_pages, gq, gr, gm, gmp);
    uint32_t r = rec();
    fprintf(f, "player %u: state %u id %u; weapons", MEM32(0x74F83C), MEM32(r + 0xA4), MEM32(r + 0xA8) & 0xFFFF);
    for (int w = 0; w < 5; w++) fprintf(f, " %u", MEM8(r + 0x13F + w));
    fprintf(f, " (current %u)\n\n", MEM32(r + 0x148));

    uint32_t n = g_ns < NSTEP ? g_ns : NSTEP;
    uint64_t t0 = n ? g_steps[(g_ns - n) % NSTEP].ns : 0;
    fprintf(f, "STEPS (ms from the first; positions 16.16 pixels, shown /65536)\n");
    fprintf(f, "ms step dt | st id | ped x y h | car model x y z h turret force | cam x y z scale zoom | pad lx ly rx ry lt rt btn share f b | word | kills kind n timer score\n");
    uint64_t prev = 0;
    for (uint32_t i = g_ns - n; i != g_ns; i++) {
        const step_t *s = &g_steps[i % NSTEP];
        fprintf(f, "%.1f %u %.1f | %u %u | %.2f %.2f %d | %d %.2f %.2f %.2f %d %d %.3f | %d %d %d %d %d | %.2f %.2f %.2f %.2f %.2f %.2f %x %.2f %.2f | %08x | %d %d %d %d\n",
                (s->ns - t0) / 1e6, s->step, prev ? (s->ns - prev) / 1e6 : 0.0, s->state, s->id,
                s->ped_x / 65536.0, s->ped_y / 65536.0, s->ped_h,
                s->car_model, s->car_x / 65536.0, s->car_y / 65536.0, s->car_z / 65536.0, s->car_h, s->car_f0, s->car_force,
                s->cam_x, s->cam_y, s->cam_z, s->cam_scale, s->cam_zoom,
                s->pad[0], s->pad[1], s->pad[2], s->pad[3], s->pad[4], s->pad[5], s->buttons, s->share_f, s->share_b, s->word,
                s->chain_kind, s->chain_n, s->chain_t, s->score);
        prev = s->ns;
    }
    n = g_nf < NFRAME ? g_nf : NFRAME;
    fprintf(f, "\nFRAMES (kind 0 the game's draw, 1 joyride's between steps, 2 presented; t = place in the step, 1.0 = a whole step; slot = when it was meant to show)\n");
    fprintf(f, "ms dt kind step t slot cam x y scale\n");
    prev = 0;
    for (uint32_t i = g_nf - n; i != g_nf; i++) {
        const frame_t *fr = &g_frames[i % NFRAME];
        if (fr->ns < t0) continue;
        fprintf(f, "%.2f %.2f %d %u %.3f %.2f %d %d %d\n", (fr->ns - t0) / 1e6, prev ? (fr->ns - prev) / 1e6 : 0.0, fr->kind,
                fr->step, fr->t / 65536.0, fr->slot_ns ? ((double)fr->slot_ns - (double)t0) / 1e6 : 0.0,
                fr->cam_x, fr->cam_y, fr->cam_scale);
        prev = fr->ns;
    }
    fprintf(f, "\nCARS ABOVE THE STREET (appeared there, or rose; z 255 is street level, smaller is higher)\n");
    fprintf(f, "step car model driver f6 | x y z | was x y z | set up at step, from, z asked | stack\n");
    n = g_ncarev < NCAREV ? g_ncarev : NCAREV;
    for (uint32_t i = g_ncarev - n; i != g_ncarev; i++) {
        const carev_t *e = &g_carev[i % NCAREV];
        fprintf(f, "%u %d %d %d %d | %d %d %d | %d %d %d | %u %08X %.2f |", e->step, e->id, e->model, e->driver, e->f6,
                e->x, e->y, e->z, e->px, e->py, e->pz, e->made.step, e->made.site, e->made.z / 65536.0);
        for (int k = 0; k < NSTACK; k++) fprintf(f, " %X", e->made.stack[k]);
        fprintf(f, "\n");
    }
    fclose(f);
    save_memory(dir);
    joyride_screen_snapshot(dir);                       /* the next frames: screen_NN.png, cpu.png */
    fprintf(stderr, "[debug] report written to %s\n", dir);
}

/* ---- all weapons, the next vehicle ------------------------------------------ */

/* The models the style has, by number: 0x4BE178[model] is its entry in
 * the list at [0x501574] (count [0x501570]), -1 none. */
static int model_info(int m) {
    int i = (int16_t)MEM16(0x4BE178u + 2u * (uint32_t)m);
    if (i < 0 || i >= (int)MEM32(0x501570)) return -1;
    return MEM8(MEM32(MEM32(0x501574) + 4u * (uint32_t)i) + 0x6B) == m ? i : -1;   /* the info names its model */
}
static int next_model(int cur) {
    for (int k = 1; k <= 64; k++) {
        int m = (cur + k) % 64;
        if (m != cur && model_info(m) >= 0) return m;
    }
    return -1;
}

/* The car set up again as `model` where it is. The game's own set-up
 * (0x4067C0) can't be used on a car in play: it also puts the car in the
 * world's object lists (0x436C30), and a second time links it into itself
 * (the game hung). So only what depends on the model is done again, as
 * 0x4067C0 does it: the dimensions and the info, the sprite, the body's
 * corners (0x434300), the driving's numbers for the model (0x460AD0). */
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
/* The tank's turret: two objects of the world (0x44CAD0), the base on the
 * car (kind 1, type 0x21) and the gun on the base (type 0x22), their numbers
 * in 0x11A and 0x11C, which other models use as flags. Made as 0x4067C0
 * makes them for a tank; taken away (0x44EB90, the gun first) when the
 * tank becomes something else. */
static void tank_turret(uint32_t c, int was, int model) {
    if (was == 0x25 && model != 0x25) {
        uint32_t gun = (uint32_t)(int32_t)(int16_t)MEM16(c + 0x11C), base = (uint32_t)(int32_t)(int16_t)MEM16(c + 0x11A);
        if ((int32_t)gun > 0) hle_call_guest(0x44EB90, 1, &gun);
        if ((int32_t)base > 0) hle_call_guest(0x44EB90, 1, &base);
        MEM16(c + 0x11A) = 0, MEM16(c + 0x11C) = 0, MEM16(c + 0x12C) = 0;
    } else if (was != 0x25 && model == 0x25) {
        uint32_t a[5] = { (uint32_t)(int32_t)(int16_t)MEM16(c), 1, 0, 0, 0x21 };
        uint32_t base = hle_call_guest(0x44CAD0, 5, a);
        MEM16(c + 0x11A) = (uint16_t)base;
        if ((int32_t)base <= 0) return;                  /* no room in the world's objects */
        uint32_t b[5] = { (uint32_t)(int32_t)(int16_t)base, 0, 0, 0x1E, 0x22 };
        MEM16(c + 0x11C) = (uint16_t)hle_call_guest(0x44CAD0, 5, b);
        hle_call_guest(0x44CAB0, 0, NULL);
        MEM16(c + 0x12C) = 1;
    }
}
/* The tank's cannon (0x489300, from the car's step at 0x40A4C8 while its
 * driver's fire flag, ped +0x46, is set) only fires while the driving
 * player has a mission timer running (+0x196 of the player's record, the
 * rampages' countdown): the game's own tanks are a rampage's. The tank the
 * debug swap made fires at any time: in the car's step, before the game's
 * tank code (the splice at 0x40A418, joyride_pad_throttle), the cannon is
 * fired with the timer seen as running, and the fire flag taken. */
static uint32_t g_tank;                  /* the car the swap made a tank (0: none) */
void joyride_debug_tank_fire(uint32_t car) {
    if (car != g_tank || MEM16(car + 0x22) != 0x25) return;
    uint32_t ped = (uint32_t)(int32_t)(int16_t)MEM16(car + 2);
    if ((int32_t)ped < 0 || !MEM8(0x7284E0u + ped * 256u + 0x46)) return;
    int32_t player = (int16_t)hle_call_guest(0x464440, 1, &ped);
    if (player < 0) return;
    uint32_t timer = 0x74F2F6u + (uint32_t)player * 444u;
    if (MEM16(timer) != 0xFFFF) return;                  /* running: the game's own way */
    MEM16(timer) = 1;
    uint32_t c = car;
    hle_call_guest(0x489300, 1, &c);
    MEM16(timer) = 0xFFFF;
    MEM8(0x7284E0u + ped * 256u + 0x46) = 0;
}
static void swap_model(uint32_t c, int model) {
    tank_turret(c, (int16_t)MEM16(c + 0x22), model);
    g_tank = model == 0x25 ? c : g_tank == c ? 0 : g_tank;
    int idx = model_info(model);
    uint32_t info = MEM32(MEM32(0x501574) + 4u * (uint32_t)idx);
    int old_len = (int16_t)MEM16(c + 0x36);
    MEM32(c + 0x2AC) = info;
    MEM16(c + 0x20) = MEM16(info + 0xE), MEM16(c + 0x32) = MEM16(info + 0x10);
    MEM16(c + 0x28) = model == 4 ? 0x3C : MEM16(info + 0xA);
    MEM16(c + 0x2A) = MEM16(info + 0xC), MEM16(c + 0xF8) = MEM16(info + 6);
    MEM16(c + 0x24) = MEM16(info + 0xAE), MEM16(c + 0x26) = MEM16(info + 0xB0);
    MEM16(c + 0x34) = MEM8(info + 0x6A), MEM16(c + 0x36) = (uint16_t)-(int16_t)MEM16(info + 2);
    MEM16(c + 0x2C) = MEM16(info), MEM16(c + 0x30) = MEM16(info + 4);
    MEM16(c + 0x8C) = (uint16_t)((int16_t)MEM16(info + 2) >> 1), MEM16(c + 0x8A) = (uint16_t)((int16_t)MEM16(info) >> 1);
    /* the sprite (0x47C9F0's part that depends on the model, 0x47C9D0) */
    uint32_t o = c + 0x250, look = MEM16(info + 6);
    MEM16(o + 0x16) = (uint16_t)look, MEM32(o + 0x48) = MEM32(0x773E38u + 4u * look), MEM32(o + 0x1C) = 0;
    MEM16(o + 0x12) = (uint16_t)((uint32_t)idx * 12u + MEM32(0x77530C));
    MEM16(c + 0x22) = (uint16_t)model;
    MEM32(c + 0x108) = model == 0x2F ? 0x20000 : model == 0x25 ? 0x40000 : 0x30000;
    MEM16(c + 0xEE) = model == 0x13 ? 1 : model == 1 ? 2 : model == 0x2B ? 3 : 0;
    MEM16(c + 0xCE) = (uint16_t)((MEM8(c + 0x28) & 7) - 4);
    /* the two axle points the driving works from (0x14/0x18 and 0xAC/0xB0),
     * half a length ahead of and behind the middle as 0x4067C0 puts them:
     * moved apart or together for the new length, about their middle */
    int new_len = (int16_t)MEM16(c + 0x36);
    if (old_len != 0 && new_len != old_len) {
        int64_t ax = (int32_t)MEM32(c + 0x14), ay = (int32_t)MEM32(c + 0x18);
        int64_t bx = (int32_t)MEM32(c + 0xAC), by = (int32_t)MEM32(c + 0xB0);
        int64_t mx = (ax + bx) / 2, my = (ay + by) / 2;
        int64_t hx = (bx - ax) / 2 * new_len / old_len, hy = (by - ay) / 2 * new_len / old_len;
        MEM32(c + 0x14) = (uint32_t)(mx - hx), MEM32(c + 0x18) = (uint32_t)(my - hy);
        MEM32(c + 0xAC) = (uint32_t)(mx + hx), MEM32(c + 0xB0) = (uint32_t)(my + hy);
    }
    /* the body's corners, from the new size */
    uint32_t a[8] = { MEM32(c + 0x250), MEM32(c + 0x254), MEM32(c + 0x258), MEM16(c + 0x8A), MEM16(c + 0x8C),
                      MEM16(c + 0x268), MEM16(c + 0x30), c + 0x1DC };
    for (int i = 3; i < 7; i++) a[i] = (uint32_t)(int32_t)(int16_t)a[i];
    hle_call_guest(0x434300, 8, a);
    /* and the copy of them the driving keeps from the step before (0x405A80) */
    uint32_t self = c;
    hle_call_guest(0x405A80, 1, &self);
    /* the driving's numbers for the model (0x460AD0, without its zeroing of
     * the motion): left as the old model's, a long car is driven with a short
     * one's and is thrown about as soon as it turns */
    MEM32(c + 0x1B8) = f2u((float)(int8_t)MEM8(info + 0x76));
    MEM32(c + 0x1BC) = f2u((float)(int8_t)MEM8(info + 0x77));
    MEM32(c + 0x1B4) = f2u((float)(int32_t)MEM32(info + 0x78));
    MEM32(c + 0x1B0) = MEM32(info + 0x7C);
    MEM32(c + 0x14C) = MEM32(info + 0x80);
}

void joyride_debug_give(void) {
    uint32_t r = rec();
    for (int w = 1; w <= 4; w++) MEM8(r + 0x13F + w) = 99;
    fprintf(stderr, "[debug] all weapons\n");
    if (MEM32(r + 0xA4) == 2) return;                    /* on foot: weapons only */
    uint32_t id = MEM32(r + 0xA8) & 0xFFFF, c = 0x4BE248u + id * 688u;
    int cur = (int16_t)MEM16(c + 0x22), next = next_model(cur);
    if (next < 0 || next == cur) return;
    swap_model(c, next);
    fprintf(stderr, "[debug] vehicle %u: model %d -> %d\n", id, cur, next);
}
