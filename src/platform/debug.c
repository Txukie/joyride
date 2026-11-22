/*
 * joyride - debugging aids in play (the controller's d-pad left and right;
 * F7 and F8 on the keyboard).
 *
 * Left (F7): a report. joyride keeps the last 10 seconds of play in memory,
 * each game step (the player, the car, the camera, the controls) and each
 * frame drawn (its time, its place between steps, the camera it used), and
 * writes them to <data>/debug/<time>/report.txt, with the frame on screen
 * (screen.ppm: the GPU's picture; cpu.ppm: the game's surface) and the
 * game's memory (memory.bin, 0x4AB000..0x791000). The log line says where.
 *
 * Right (F8): all weapons (99 rounds each: player +0x13F + slot, slots
 * 1-4) and, in a vehicle, the next model in the style's list
 * ([0x501574][i] + 0x6B): the car is set up again in place by the game's own
 * 0x4067C0 (x, y, z, heading, model, slot), its driver and state put back.
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

/* each game step (from the input reader, 0x433141) */
void joyride_debug_step(uint32_t word) {
    step_t *s = &g_steps[g_ns++ % NSTEP];
    memset(s, 0, sizeof *s);
    uint32_t r = rec();
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
    uint32_t r = rec();
    fprintf(f, "player %u: state %u id %u; weapons", MEM32(0x74F83C), MEM32(r + 0xA4), MEM32(r + 0xA8) & 0xFFFF);
    for (int w = 0; w < 5; w++) fprintf(f, " %u", MEM8(r + 0x13F + w));
    fprintf(f, " (current %u)\n\n", MEM32(r + 0x148));

    uint32_t n = g_ns < NSTEP ? g_ns : NSTEP;
    uint64_t t0 = n ? g_steps[(g_ns - n) % NSTEP].ns : 0;
    fprintf(f, "STEPS (ms from the first; positions 16.16 pixels, shown /65536)\n");
    fprintf(f, "ms step dt | st id | ped x y h | car model x y z h turret force | cam x y z scale zoom | pad lx ly rx ry lt rt btn share f b | word\n");
    uint64_t prev = 0;
    for (uint32_t i = g_ns - n; i != g_ns; i++) {
        const step_t *s = &g_steps[i % NSTEP];
        fprintf(f, "%.1f %u %.1f | %u %u | %.2f %.2f %d | %d %.2f %.2f %.2f %d %d %.3f | %d %d %d %d %d | %.2f %.2f %.2f %.2f %.2f %.2f %x %.2f %.2f | %08x\n",
                (s->ns - t0) / 1e6, s->step, prev ? (s->ns - prev) / 1e6 : 0.0, s->state, s->id,
                s->ped_x / 65536.0, s->ped_y / 65536.0, s->ped_h,
                s->car_model, s->car_x / 65536.0, s->car_y / 65536.0, s->car_z / 65536.0, s->car_h, s->car_f0, s->car_force,
                s->cam_x, s->cam_y, s->cam_z, s->cam_scale, s->cam_zoom,
                s->pad[0], s->pad[1], s->pad[2], s->pad[3], s->pad[4], s->pad[5], s->buttons, s->share_f, s->share_b, s->word);
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
    fclose(f);
    save_memory(dir);
    joyride_screen_snapshot(dir);                       /* the next frame: screen.ppm, cpu.ppm */
    fprintf(stderr, "[debug] report written to %s\n", dir);
}

/* ---- all weapons, the next vehicle ------------------------------------------ */

void joyride_debug_give(void) {
    uint32_t r = rec();
    for (int w = 1; w <= 4; w++) MEM8(r + 0x13F + w) = 99;
    fprintf(stderr, "[debug] all weapons\n");
    if (MEM32(r + 0xA4) == 2) return;                    /* on foot: weapons only */
    /* NOT WORKING YET: the game hangs a few steps after the car is set up
     * again in place; kept behind JOYRIDE_DEBUG_SWAP=1 until it's found */
    if (!getenv("JOYRIDE_DEBUG_SWAP")) { fprintf(stderr, "[debug] vehicle swap is off (JOYRIDE_DEBUG_SWAP=1)\n"); return; }

    uint32_t id = MEM32(r + 0xA8) & 0xFFFF, c = 0x4BE248u + id * 688u;
    int cur = (int16_t)MEM16(c + 0x22), n = (int)MEM32(0x501570), next = -1, first = -1;
    uint32_t infos = MEM32(0x501574);
    for (int i = 0; i < n && i < 100; i++) {            /* the models the style has, in order */
        int m = MEM8(MEM32(infos + 4u * (uint32_t)i) + 0x6B);
        if (first < 0 || m < first) first = m;
        if (m > cur && (next < 0 || m < next)) next = m;
    }
    if (next < 0) next = first;
    if (next < 0) return;
    static uint8_t keep[688];
    memcpy(keep, hle_ptr(c), sizeof keep);
    uint32_t a[6] = { MEM32(c + 0x250), MEM32(c + 0x254), MEM32(c + 0x258), MEM16(c + 0x268), (uint32_t)next, id };
    hle_call_guest(0x4067C0, 6, a);
    /* the driver and what the game keeps of it */
    static const int k_fields[][2] = { { 0x02, 2 }, { 0x04, 2 }, { 0x06, 2 }, { 0x88, 2 }, { 0x9C, 4 }, { 0xB8, 2 },
                                       { 0x110, 2 }, { 0x13C, 2 }, { 0x244, 4 } };
    for (size_t i = 0; i < sizeof k_fields / sizeof *k_fields; i++)
        memcpy((uint8_t *)hle_ptr(c) + k_fields[i][0], keep + k_fields[i][0], (size_t)k_fields[i][1]);
    fprintf(stderr, "[debug] vehicle %u: model %d -> %d\n", id, cur, next);
}
