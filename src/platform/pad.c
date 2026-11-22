/*
 * joyride - controllers: SDL3 gamepads mapped onto GTA's own controls.
 *
 * How the game reads its controls (0x432E00, once a step): key events for
 * its 10 controls (steer left/right, forward, back, jump/handbrake, fire,
 * enter/exit, special, next and previous weapon; Space, Ctrl, Return, Tab,
 * X, Z by default) become changes in a 32-bit action word
 * through 0x4331F0 (press) and 0x4333C0 (release). Its joystick support
 * already carries analog values through 0x4335A0: steering -7..+7 (word
 * bits 9-12), forward and back levels 0..3. The word is what each step
 * acts on (0x463EC0, into the player record at 0x74F160 + 444 * player)
 * and what network play exchanges, so joyride adds its changes to it at
 * the end of 0x432E00 (spliced at 0x433141) with the game's own handlers:
 * no game state is written, and network games stay in step.
 *
 * What the game does with them: on foot (player state +0xA4 == 2) any
 * forward level runs (no walk), back walks backwards, slower; turning is
 * digital and ramps 16 then 48 a step (of 1024 a circle). The ped is
 * 0x7284E0 + 256 * id (id at player +0xA8), heading at +0xA8: 0 faces down
 * the screen, rising turns left, forward is (sin, cos) of it.
 *
 * Two schemes (the F12 overlay's "Controller"):
 *   modern  On foot the left stick is the direction to run in and the
 *           right stick the direction to face (else the left stick's, else
 *           the last): joyride turns the character towards it (held turns
 *           while far, single 16-unit taps near: within ~3 degrees) and
 *           runs forward, or walks back when facing away from where the
 *           stick points. X fire, A jump, Y enter/exit, B special, L1/R1
 *           previous/next weapon. Driving: left stick X steers (the game's
 *           analog steering), R2 accelerates, L2 brakes, X fire, A
 *           handbrake, Y exit, B special, L1/R1 radio station.
 *   classic The game's tank controls on the pad: left stick steers and
 *           its up/down (or R2/L2) go forward/back, same buttons.
 * Start: the game's Esc menu (it pauses the game). Select: reserved (map).
 * The right stick does not aim turrets yet.
 *
 * Values are sent only when they change, so the keyboard keeps working
 * alongside the pad. JOYRIDE_PAD_SCRIPT="t:lx,ly,rx,ry,lt,rt,buttons;..."
 * plays a pad for tests (axes -1..1, buttons as SDL gamepad button bits).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "win32hle.h"
#include "joyride_hle.h"

int joyride_controls_modern = 1;           /* the overlay's "Controller" */

typedef struct { float lx, ly, rx, ry, lt, rt; uint32_t buttons; int present; } pad_t;
#define BTN(b) (1u << (b))

static SDL_Gamepad *g_pad;

void joyride_pad_event(const void *ev) {
    const SDL_Event *e = ev;
    if (e->type == SDL_EVENT_GAMEPAD_ADDED && !g_pad) {
        g_pad = SDL_OpenGamepad(e->gdevice.which);
        if (g_pad) fprintf(stderr, "[pad] %s\n", SDL_GetGamepadName(g_pad));
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED && g_pad && SDL_GetGamepadID(g_pad) == e->gdevice.which) {
        SDL_CloseGamepad(g_pad);
        g_pad = NULL;
    }
}

void joyride_pad_init(void) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) fprintf(stderr, "[pad] SDL: %s\n", SDL_GetError());
}

/* ---- the pad's state: SDL, or the test script ---------------------------- */

static float axis(SDL_GamepadAxis a) {
    float v = (float)SDL_GetGamepadAxis(g_pad, a) / 32767.0f;
    return v < -1 ? -1 : v > 1 ? 1 : v;
}

static int script(pad_t *p) {
    static const char *spec = (const char *)1;
    if (spec == (const char *)1) spec = getenv("JOYRIDE_PAD_SCRIPT");
    if (!spec) return 0;
    double t = (double)SDL_GetTicks() / 1000.0;
    memset(p, 0, sizeof *p);
    p->present = 1;
    const char *s = spec;                     /* the last entry whose time has come */
    while (s && *s) {
        double at;
        pad_t q = { 0 };
        unsigned b = 0;
        if (sscanf(s, "%lf:%f,%f,%f,%f,%f,%f,%x", &at, &q.lx, &q.ly, &q.rx, &q.ry, &q.lt, &q.rt, &b) >= 7 && at <= t) {
            q.buttons = b, q.present = 1;
            *p = q;
        }
        s = strchr(s, ';');
        if (s) s++;
    }
    return 1;
}

static void read_pad(pad_t *p) {
    if (script(p)) return;
    memset(p, 0, sizeof *p);
    if (!g_pad) return;
    p->present = 1;
    p->lx = axis(SDL_GAMEPAD_AXIS_LEFTX), p->ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    p->rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), p->ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    p->lt = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), p->rt = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++)
        if (SDL_GetGamepadButton(g_pad, (SDL_GamepadButton)b)) p->buttons |= BTN(b);
}

/* ---- the game's handlers --------------------------------------------------- */

/* the game's control indices, as its handlers number them (checked against
 * the keys: Space 4, Ctrl 5, Return 6, Tab 7, X 8, Z 9) */
enum { C_LEFT, C_RIGHT, C_FORWARD, C_BACK, C_JUMP, C_FIRE, C_ENTER, C_SPECIAL, C_NEXT, C_PREV };

static uint32_t g_word;                    /* the action word being built (guest address) */

static void analog(int control, int v) {
    uint32_t a[3] = { (uint32_t)control, (uint32_t)v, g_word };
    hle_call_guest(0x4335A0, 3, a);
}
static void button(int control, int down) {
    uint32_t a[2] = { (uint32_t)control, g_word };
    hle_call_guest(down ? 0x4331F0 : 0x4333C0, 2, a);
}

/* what joyride last sent, to send only changes */
static int g_steer = 0, g_fwd = 0, g_back = 0, g_held[10];

static void steer(int s) {                 /* -7 (left) .. +7 (right) */
    if (s == g_steer) return;
    g_steer = s;
    int m = s < 0 ? -s : s;
    int v = m ? (m * 127 + 6) / 7 - 9 : -9;   /* the game's (v + 9) * 7 / 127 */
    analog(s < 0 ? C_LEFT : C_RIGHT, v);
}
static void throttle(int fwd, int back) {  /* levels 0..3 */
    static const int v[4] = { 0, 22, 64, 106 };   /* the game's (3v + 63) / 127 */
    if (fwd != g_fwd) { g_fwd = fwd; analog(C_FORWARD, v[fwd]); }
    if (back != g_back) { g_back = back; analog(C_BACK, v[back]); }
}
static void hold(int control, int down) {
    if (down == g_held[control]) return;
    g_held[control] = down;
    button(control, down);
}

/* ---- the schemes ------------------------------------------------------------ */

static float dead(float v, float d) { return fabsf(v) < d ? 0 : (v - copysignf(d, v)) / (1 - d); }
static int level(float v) { return v < 0.2f ? 0 : v < 0.45f ? 1 : v < 0.75f ? 2 : 3; }

static float g_aim_x, g_aim_y;             /* where the character last faced, by the sticks */
static int g_aim_set, g_last_turn;

static void on_foot_modern(const pad_t *p, uint32_t rec) {
    uint32_t id = MEM32(rec + 0xA8) & 0xFFFF;
    int h = MEM16(0x7284E0 + id * 256 + 0xA8) & 0x3FF;
    float lm = hypotf(p->lx, p->ly), rm = hypotf(p->rx, p->ry);
    int moving = lm > 0.3f, aiming = rm > 0.4f;
    if (aiming) g_aim_x = p->rx, g_aim_y = p->ry, g_aim_set = 1;
    else if (moving) g_aim_x = p->lx, g_aim_y = p->ly, g_aim_set = 1;

    int s = 0, diff = 0;
    if (g_aim_set) {
        int target = (int)lrintf(atan2f(g_aim_x, g_aim_y) * 1024.0f / (2.0f * (float)M_PI)) & 0x3FF;
        diff = ((target - h + 512) & 0x3FF) - 512;          /* > 0: turn left (heading rises) */
        int m = diff < 0 ? -diff : diff;
        if (m >= 64) s = diff > 0 ? -7 : 7;                  /* far: hold the turn */
        else if (m >= 10 && !g_last_turn) s = diff > 0 ? -7 : 7;   /* near: one 16-unit step */
    }
    g_last_turn = s != 0;
    steer(s);

    int fwd = 0, back = 0;
    if (moving) {
        float a = (float)h * 2.0f * (float)M_PI / 1024.0f;
        float dot = (sinf(a) * p->lx + cosf(a) * p->ly) / lm;   /* facing . stick */
        if (aiming) { if (dot > 0.35f) fwd = 3; else if (dot < -0.35f) back = 3; }
        else if ((diff < 0 ? -diff : diff) < 128) fwd = 3;      /* turn first, then run */
    }
    throttle(fwd, back);
    static int log = -1;
    if (log < 0) log = getenv("JOYRIDE_PAD_LOG") != NULL;
    if (log) {
        uint32_t ped = 0x7284E0 + id * 256;
        fprintf(stderr, "[pad] heading %d target-diff %d steer %d fwd %d back %d pos %d,%d\n", h, diff, s, fwd, back,
                (int32_t)MEM32(ped + 0x90) >> 16, (int32_t)MEM32(ped + 0x94) >> 16);
    }

    hold(C_FIRE, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_WEST)));
    hold(C_JUMP, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_SOUTH)));
    hold(C_ENTER, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_NORTH)));
    hold(C_SPECIAL, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_EAST)));
    hold(C_PREV, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)));
    hold(C_NEXT, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)));
}

static void driving_modern(const pad_t *p, uint32_t prev_buttons) {
    steer((int)lrintf(dead(p->lx, 0.15f) * 7.0f));
    int fwd = level(p->rt), back = level(p->lt);
    if (fwd && back) back = 0;                               /* both: accelerate wins */
    throttle(fwd, back);
    static int log = -1;
    if (log < 0) log = getenv("JOYRIDE_PAD_LOG") != NULL;
    if (log) fprintf(stderr, "[pad] driving: steer %d fwd %d back %d\n", g_steer, g_fwd, g_back);
    hold(C_FIRE, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_WEST)));
    hold(C_JUMP, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_SOUTH)));     /* handbrake */
    hold(C_ENTER, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_NORTH)));
    hold(C_SPECIAL, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_EAST)));
    hold(C_PREV, 0), hold(C_NEXT, 0);
    uint32_t pressed = p->buttons & ~prev_buttons;
    if (pressed & BTN(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) joyride_radio_step(-1);
    if (pressed & BTN(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) joyride_radio_step(1);
}

static void classic(const pad_t *p) {
    steer((int)lrintf(dead(p->lx, 0.15f) * 7.0f));
    float up = -p->ly > p->rt ? -p->ly : p->rt, down = p->ly > p->lt ? p->ly : p->lt;
    throttle(level(up), level(down));
    hold(C_FIRE, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_WEST)));
    hold(C_JUMP, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_SOUTH)));
    hold(C_ENTER, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_NORTH)));
    hold(C_SPECIAL, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_EAST)));
    hold(C_PREV, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)));
    hold(C_NEXT, !!(p->buttons & BTN(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)));
}

/* Spliced at 0x433141: the action word at `word` (guest) is complete for
 * this step; add the pad's changes to it. */
void joyride_pad_input(uint32_t word) {
    static uint32_t prev;
    pad_t p;
    read_pad(&p);
    if (!p.present) { prev = 0; return; }
    g_word = word;
    uint32_t rec = 0x74F160u + MEM32(0x74F83C) * 444u;
    int on_foot = MEM32(rec + 0xA4) == 2;
    if (!joyride_controls_modern) classic(&p);
    else if (on_foot) on_foot_modern(&p, rec);
    else driving_modern(&p, prev);
    /* Start: the game's Esc menu (pauses), through the keyboard's path */
    uint32_t pressed = p.buttons & ~prev, released = prev & ~p.buttons;
    if (pressed & BTN(SDL_GAMEPAD_BUTTON_START)) joyride_screen_key(SDL_SCANCODE_ESCAPE, 1);
    if (released & BTN(SDL_GAMEPAD_BUTTON_START)) joyride_screen_key(SDL_SCANCODE_ESCAPE, 0);
    prev = p.buttons;
}
