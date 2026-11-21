/*
 * joyride - more frames than the game's 23.3 a second, the game's speed kept.
 *
 * GTA advances its world one fixed step a frame and holds each frame to 3
 * ticks of a 70 Hz timer (the limiter at 0x47DC80 spins until a counter
 * the timer bumps, [0x775580], reaches 3). Its loop (0x41490C): flip the
 * last frame (0x430DA0 -> 0x414B10), read input, limiter, update the
 * players and the world and the camera (0x430B20), draw (0x430D40).
 *
 * Instead of spinning, joyride draws while the limiter waits: it calls the
 * game's draw and flip again with the world shown part of the way from the
 * previous step to the current one, as often as the display takes frames
 * (the presenter paces them to its refresh rate), and stops in time for
 * the next step, which it knows to the tick from the timer callback
 * (0x47DC70). Every draw, the game's own too, shows the world one step
 * back (interpolated, t = 0 for the game's own): 43 ms behind the
 * simulation, in exchange for smooth motion. The game keeps its 23.3 steps
 * a second; how many frames fit between them depends on the draw's cost
 * (about 3 ms at 1024x768 with sharp textures, 11 ms filtered).
 *
 * What moves between steps, and is interpolated for a draw and put back
 * after it:
 *   - the camera: the local player's view ([0x74F83C], records of 444
 *     bytes at 0x74F160): position +0x1C, +0x20, height +0x18, scale +0x24,
 *     zoom +0x28; then 0x43B7E0 and 0x43B780 derive the renderer's copies
 *     from it, as the game's camera update does;
 *   - each sprite object as it is drawn (0x47C050, reached through a
 *     function pointer; its attached sprites at +0x50 follow it): position
 *     +0, +4, +8 (16.16) and angle +0x18 (10 bits).
 * Each object's state is remembered per step by address; one that jumps
 * further than a step can move (spawned, reused, teleported) is not
 * interpolated that step. The game's own draw still runs each step (it
 * moves the HUD's clocks) but is not shown: it shows what joyride's last
 * frame showed.
 *
 * tools/postlift.py splices joyride_smooth_begin/end around the game's
 * own draw (0x41499E) and joyride_smooth_sprite_in/out at 0x47C050's entry
 * and exit; joyride_smooth_wait runs from the frame's safe point.
 * Off when the limiter is (F8), and in the game's alternate draw mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "win32hle.h"
#include "joyride_hle.h"

int joyride_smooth = 1;                 /* the overlay switches it */

#define STEP_NS (3ull * 1000000000ull / 70ull)   /* 3 ticks of the 70 Hz timer */

static uint32_t g_step;                 /* steps drawn so far */
static int32_t g_t = 0;                 /* the draw's position between steps, 0..65536 */
static int g_drawing;                   /* inside a draw with interpolation applied */
static int g_extra;                     /* flipping one of joyride's frames */
static int g_on_now;                    /* drawing between steps this step */
static int g_extras, g_extras_last;     /* frames joyride drew this step, the last */

/* The game's own frame at each step shows what joyride's last frame of the
 * step before already showed (t = 1 there is t = 0 here), at a moment of
 * the game's choosing: shown, it would hold the next step up waiting for a
 * display slot, or knock joyride's frames off the display's rhythm. While
 * joyride's frames are coming, the presenter leaves it out. */
int joyride_smooth_running(void) { return g_on_now; }
int joyride_smooth_skip(void) { return g_on_now && !g_extra && g_extras_last > 0; }

static inline int32_t lerp(int32_t a, int32_t b, int32_t t) {
    return a + (int32_t)(((int64_t)(b - a) * t) >> 16);
}

uint32_t joyride_smooth_steps(void) { return g_step; }

/* The timer callback (0x47DC70, on the timer's thread): when the counter
 * the limiter waits on last moved, so the next step's start is known to
 * the tick rather than guessed from when the loop came round. */
static volatile uint64_t g_tick_ns;
void joyride_smooth_tick(void) { g_tick_ns = SDL_GetTicksNS(); }

/* when the limiter will let the next step start */
static uint64_t step_due(void) {
    int32_t c = (int32_t)MEM32(0x775580);
    uint64_t tick = 1000000000ull / 70ull, last = g_tick_ns;
    if (!last) return SDL_GetTicksNS() + (uint64_t)(3 - c) * tick;
    /* the counter is c after the tick at `last` (the callback bumps it right after) */
    return last + (uint64_t)(3 - c) * tick;
}

/* ---- objects, by address ------------------------------------------------ */

#define NOBJ 8192
typedef struct { uint32_t addr, stamp; int32_t prev[3], cur[3]; uint16_t pa, ca; uint8_t has_prev; } obj_t;
static obj_t g_obj[NOBJ];

static obj_t *slot(uint32_t addr) {
    uint32_t h = (addr * 2654435761u) >> 19;              /* 13 bits */
    for (int i = 0; i < 16; i++) {
        obj_t *o = &g_obj[(h + (uint32_t)i) & (NOBJ - 1)];
        if (o->addr == addr || !o->addr || o->stamp + 2 < g_step) return o;
    }
    return &g_obj[h];
}

/* the most an object can move in a step and still be interpolated */
#define JUMP (8 << 16)

/* object now (step g_step) -> where to draw it */
static int track(uint32_t a, int32_t pos[3], uint16_t *ang) {
    obj_t *o = slot(a);
    int32_t now[3] = { (int32_t)MEM32(a), (int32_t)MEM32(a + 4), (int32_t)MEM32(a + 8) };
    uint16_t nang = MEM16(a + 0x18);
    if (o->addr != a || o->stamp != g_step) {
        o->has_prev = o->addr == a && o->stamp + 1 == g_step;
        if (o->has_prev) memcpy(o->prev, o->cur, sizeof o->prev), o->pa = o->ca;
        o->addr = a, o->stamp = g_step;
        memcpy(o->cur, now, sizeof now), o->ca = nang;
        if (o->has_prev)
            for (int i = 0; i < 3; i++)
                if (abs(o->cur[i] - o->prev[i]) > JUMP) o->has_prev = 0;
    }
    if (!o->has_prev) return 0;
    for (int i = 0; i < 3; i++) pos[i] = lerp(o->prev[i], o->cur[i], g_t);
    int d = ((int)o->ca - (int)o->pa) & 0x3FF;            /* the short way round */
    if (d >= 0x200) d -= 0x400;
    *ang = (uint16_t)((o->pa + (int)(((int64_t)d * g_t) >> 16)) & 0x3FF);
    return 1;
}

/* 0x47C050 entry and exit: one sprite object (and what is attached to it) */
#define SDEPTH 64
static uint32_t g_saddr[SDEPTH];
static int32_t g_spos[SDEPTH][3];
static uint16_t g_sang[SDEPTH];
static int g_sn;

void joyride_smooth_sprite_in(uint32_t a) {
    int k = g_sn++;
    if (k >= SDEPTH) return;
    g_saddr[k] = 0;
    if (!g_drawing || !a) return;
    int32_t pos[3];
    uint16_t ang;
    if (!track(a, pos, &ang)) return;
    g_saddr[k] = a;
    for (int i = 0; i < 3; i++) g_spos[k][i] = (int32_t)MEM32(a + 4 * i), MEM32(a + 4 * i) = (uint32_t)pos[i];
    g_sang[k] = MEM16(a + 0x18), MEM16(a + 0x18) = ang;
}

void joyride_smooth_sprite_out(void) {
    int k = --g_sn;
    if (k < 0) { g_sn = 0; return; }
    if (k >= SDEPTH || !g_saddr[k]) return;
    uint32_t a = g_saddr[k];
    for (int i = 0; i < 3; i++) MEM32(a + 4 * i) = (uint32_t)g_spos[k][i];
    MEM16(a + 0x18) = g_sang[k];
}

/* ---- the camera ------------------------------------------------------- */

static const int k_cam[] = { 0x18, 0x1C, 0x20, 0x24, 0x28 };
#define NCAM (int)(sizeof k_cam / sizeof *k_cam)
static int32_t g_cprev[NCAM], g_ccur[NCAM], g_csave[NCAM];
static uint32_t g_cstamp = ~0u;
static int g_chas;

static uint32_t view(void) { return 0x74F160u + MEM32(0x74F83C) * 444u; }

static void camera_apply(void) {
    uint32_t v = view();
    int32_t now[NCAM];
    for (int i = 0; i < NCAM; i++) now[i] = (int32_t)MEM32(v + (uint32_t)k_cam[i]);
    if (g_cstamp != g_step) {
        g_chas = g_cstamp + 1 == g_step;
        if (g_chas) memcpy(g_cprev, g_ccur, sizeof g_cprev);
        memcpy(g_ccur, now, sizeof now);
        g_cstamp = g_step;
        if (g_chas && (abs(g_ccur[1] - g_cprev[1]) > 64 * 8 || abs(g_ccur[2] - g_cprev[2]) > 64 * 8)) g_chas = 0;
    }
    memcpy(g_csave, now, sizeof now);
    if (!g_chas) return;
    for (int i = 0; i < NCAM; i++) MEM32(v + (uint32_t)k_cam[i]) = (uint32_t)lerp(g_cprev[i], g_ccur[i], g_t);
    hle_call_guest(0x43B7E0, 0, NULL);                     /* the renderer's copies of the view */
    hle_call_guest(0x43B780, 0, NULL);
}

static void camera_restore(void) {
    if (!g_chas) return;
    uint32_t v = view();
    for (int i = 0; i < NCAM; i++) MEM32(v + (uint32_t)k_cam[i]) = (uint32_t)g_csave[i];
    hle_call_guest(0x43B7E0, 0, NULL);
    hle_call_guest(0x43B780, 0, NULL);
}

/* ---- the HUD ------------------------------------------------------------ */

/* The draw is the game's too: its HUD (0x483390) counts draws per player
 * ([0x7847D8 + 36 * player]) to time its messages, so an extra draw would
 * hurry them. joyride's draws put the HUD module's state back after them;
 * everything else a draw writes is the renderer's own (texture cache and
 * its counters, the sprite sort tree, the sprite drawer's stack). */
#define HUD_LO 0x784000u
#define HUD_HI 0x785200u
static uint8_t g_hud[HUD_HI - HUD_LO];
static void hud_save(void) { memcpy(g_hud, hle_ptr(HUD_LO), sizeof g_hud); }
static void hud_restore(void) { memcpy(hle_ptr(HUD_LO), g_hud, sizeof g_hud); }

/* ---- draws -------------------------------------------------------------- */

static int active(void) {
    return joyride_smooth && MEM8(0x502F34) && MEM32(0x775584) && MEM32(0x51323C) == 0;
}

static void begin(int32_t t) {
    g_t = t;
    g_drawing = 1;
    g_sn = 0;
    camera_apply();
}

static void end(void) {
    camera_restore();
    g_drawing = 0;
}

/* around the game's own draw (0x41499E): a new step, shown at t = 0 */
void joyride_smooth_begin(void) {
    g_step++;
    if (active()) begin(0);
}

void joyride_smooth_end(void) {
    if (g_drawing) end();
}

/* From the frame's safe point: the game's frame for this step has just been
 * flipped; draw more until the next step is due. A frame reaches the screen
 * at the later of "drawn" and the display's next slot (the presenter paces
 * joyride's frames to it), so that is when its picture should be of, and
 * it must be there before the step is due or the game would run late. */
void joyride_smooth_wait(void) {
    uint64_t now = SDL_GetTicksNS();
    g_on_now = active();
    g_extras_last = g_extras, g_extras = 0;
    if (!g_on_now) return;
    static uint64_t draw_ns = 8000000;      /* how long a draw takes, smoothed */
    uint64_t start = step_due() - STEP_NS;  /* when this step's time began */
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("JOYRIDE_SMOOTH_DEBUG") != NULL;
    if (dbg && g_step % 50 == 0)
        fprintf(stderr, "[smooth] step %u: safe point at +%.1f ms, draw %.1f ms, last step %d frames\n", g_step,
                (double)(int64_t)(now - start) / 1e6, draw_ns / 1e6, g_extras_last);
    for (;;) {
        if ((int32_t)MEM32(0x775580) >= 3) break;          /* the step is due */
        now = SDL_GetTicksNS();
        uint64_t slot = joyride_screen_next_slot(), at = now + draw_ns;
        if (slot > at) at = slot;
        if (at + 2000000 >= step_due()) break;              /* would not be up in time */
        int32_t t = at <= start ? 0 : (int32_t)(((at - start) << 16) / STEP_NS);
        if (t > 65536) t = 65536;
        begin(t);
        hud_save();
        hle_call_guest(0x430D40, 0, NULL);                 /* draw */
        hud_restore();
        end();
        draw_ns = (draw_ns * 3 + (SDL_GetTicksNS() - now)) / 4;
        g_extra = 1;
        hle_call_guest(0x414B10, 0, NULL);                 /* flip, paced to the display */
        g_extra = 0;
        g_extras++;
    }
}
