/*
 * joyride - saving a game, which GTA itself never had.
 *
 * The pause menu (Start, or F6; overlay.c) offers Save. One save per
 * character and level: the character's name (player record +0xCC, the
 * name chosen in the front end) and the mission script's section the level
 * was loaded from ([0x6B3E28]: 1 and 2 are Liberty City's two chapters,
 * 102 and 103 San Andreas', 202 and 203 Vice City's; London's the same
 * slots), with the map's name as a check. Kept in
 * <data>/saves/<name>_<section>.sav (saves-<profile>/ for a game of its
 * own, JOYRIDE_PROFILE).
 *
 * What is kept, from the player's record (0x74F160 + 0x1BC each): the
 * score (+0xDC) and its digits as the HUD shows them (+0x10C..+0x128), the
 * multiplier (+0xE0), each weapon's ammunition (+0x13F + weapon, 1 to 4)
 * and the weapon in hand (+0x148). Not the place, the wanted level or
 * anything broken: the level always starts afresh at its spawn point, and
 * the save is put back over it on the first step of play (the first after
 * the game's menus, which run in 16-bit colour; play in 32).
 *
 * Missions: passed, or not started. The script keeps its progress in its
 * counters, objects of its own (32 bytes each at 0x5F30E8, the type first:
 * COUNTER 12, MISSION_COUNTER 66, SECRET_MISSION_COUNTER 67; the values at
 * +4 and +8; found by the script line that made them, through the table of
 * lines at 0x6561C0): which jobs were taken, whether one is under way, how
 * many missions are left (+4 of a MISSION_COUNTER, counting down). Taking a
 * job changes its counters at once, pass or fail (a failed phone job is not
 * offered again), so the counters are kept as they were at a checkpoint:
 *   - the level's start (after a save was put back) and each pass (a
 *     MISSION_COUNTER's or SECRET_MISSION_COUNTER's +4 going down) make one,
 *     following the counters for 5 s more (the level's first lines, the
 *     pass's tidying up: the job's under-way counter going back);
 *   - the first change after a checkpoint is a mission starting: the game
 *     is saved there by itself (autosave), the checkpoint's counters with
 *     the score and weapons of that moment;
 *   - a save during a mission keeps the score and weapons of the moment and
 *     the counters of the checkpoint: the pause menu warns first, with the
 *     time since the mission started.
 * Taking a phone's job also lets go of the phone (TELEPHONE objects, type
 * 7: +4, the booth's world object, becomes -1; the booth stays): kept, and
 * let go of again on loading. And the script's processes (32, each a
 * place in the script and some state; 0x6761C0..) and its commands' state
 * (timers, what each waits on; 0x676660, 24 bytes each): after the first
 * mission its main loop is another one, further on. These are kept as they
 * are once the checkpoint has settled (5 s after the level's start or a
 * pass, and again while the script's opening goes on: refresh()), with
 * each KICKSTART's list of the processes it started (version 5: what
 * KILL_SIDE_PROC ends when a job is taken), and put back on loading.
 * Put back, the counters and phones let the script's own loops offer the
 * next jobs (a failed one is offered again: it was not passed).
 *
 * Retrying a failed mission (the option "retry", off by default): the
 * checkpoint also keeps every script object (0x5F30E8, 32 bytes each: what
 * the mission's cars and people are, phones, counters), the trigger table
 * (0x771620, 210 of 32 bytes: +8 each one's state, DISABLE's 4) and, for
 * each car there before the mission (CAR, type 0, +4 its car), where it
 * stood and its model. A failure is the game's "MISSION FAILED!" or
 * "FRENZY FAILED!" (texts 2501 and 2504; the big message, 0x481320, sets
 * 0x785102 and copies its text to 0x77EC80). Once the message is gone, at
 * least RETRY_MIN_MS after it came up and with the script's processes
 * unchanged for STARTUP_STILL_MS (its way back to the phones over), the city
 * goes back by itself to the checkpoint before the mission, so the player
 * can go back to where it started and start it again: what the mission made is taken away
 * (its cars as KILL_CAR does, 0x43CE60 and 0x4773A0, unless someone is in
 * one; its people as KILL_PED, 0x477320 and 0x41A250, and off the list of
 * the mission's people), the objects, triggers, processes and commands are
 * put back (the phone rings again, a car's trigger waits again), and a car
 * from before the mission that is wrecked or was moved (a job taken by
 * getting in) is made again where the script first put it, as PARKED_ON
 * does (0x475840, 0x4770A0). The score and weapons are not touched.
 * The saves keep to "passed or not started": after a failure the
 * checkpoint stays the one before the mission (loading offers the job
 * again); only the baseline the next mission's start is seen from moves on.
 *
 * Not in a network game: everyone must play the same game.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define MAGIC "JRSV"
#define VERSION 5u               /* 1: no counters, 2: no phones, 3: no processes, 4: no KICKSTART lists (all still read) */
#define MAXPHONE 512
#define MAXCOUNT 256

#define OBJS 0x5F30E8u           /* the script's objects, 32 bytes each */
#define MAXOBJ 4096u
#define LINES 0x6561C0u          /* script line -> object, int16, -1 none */
#define NLINES 65536u
#define CMDS 0x676660u           /* the script's commands, 24 bytes each */
#define CMDLINES 0x693B30u       /* script line -> command, int16 */
#define MAXCMD 8192u
#define CMD_KICKSTART 89u        /* its command's +0x10: an object of its own, beyond the lines' */
#define MAXKICK 256u

/* the script's processes, 32 of them: each array by process (0x448EDD,
 * KICKSTART, fills them; 0x44919D, KILL_PROCESS, empties them) */
static const struct { uint32_t va, size; } k_proc[] = {
    { 0x6761C0, 2 }, { 0x676620, 2 }, { 0x6560B8, 2 }, { 0x676348, 2 }, { 0x656180, 2 },
    { 0x6B3DE8, 2 }, { 0x676280, 2 }, { 0x676200, 4 }, { 0x6762C8, 4 },
};
#define NPROCARR (sizeof k_proc / sizeof *k_proc)
#define PROCBYTES (32u * (2u * 7u + 4u * 2u))
enum { T_TELEPHONE = 7, T_COUNTER = 12, T_MISSIONS = 66, T_SECRETS = 67 };

typedef struct {
    char magic[4];
    uint32_t version;
    int32_t section;
    char map[32], name[32];
    int32_t score, multiplier;
    uint8_t digits[0x1C];        /* +0x10C..+0x128 */
    uint8_t ammo[4];             /* +0x140..+0x143 */
    uint32_t weapon;             /* +0x148 */
    uint32_t ncount;             /* version 2: the counters that follow */
} save_t;
typedef struct { int32_t line, a, b; } counter_t;   /* the object's +4 and +8 */

static char g_dir[4096];
static int g_menus = 1;          /* menus seen since the last level started */

/* this level's counters: their objects and lines, and the checkpoint */
static int g_n;
static uint32_t g_obj[MAXCOUNT];
static counter_t g_check[MAXCOUNT];
/* this level's phones: taking a job takes its phone out of the city (the
 * object's +4, the world object, becomes -1); kept with the checkpoint */
static int g_np;
static uint32_t g_pobj[MAXPHONE];
static int32_t g_pline[MAXPHONE];
static uint8_t g_pgone[MAXPHONE];
/* this level's processes and commands at the checkpoint */
static uint8_t g_cproc[PROCBYTES];
static uint8_t *g_ccmd;
static uint32_t g_ncmd;
/* each KICKSTART's object (no line names it): +0x10.. the processes it
 * started, which KILL_SIDE_PROC (0x4493C1) ends. Kept with the commands
 * (after them, 32 bytes each), or a process put back is one nothing ends:
 * the pointer loop between jobs went on pointing at the phones */
static uint32_t g_kcmd[MAXKICK], g_nkick;   /* the KICKSTART commands; their objects are given as they first run */
#define CMDBYTES ((size_t)g_ncmd * 24u + (size_t)g_nkick * 32u)
static int g_pend_nokick;        /* the save has none (version 4): the start-up's stay */
/* a save's processes and commands, waiting for the level's start-up to end
 * (its first processes are kicked off over its first seconds) */
static uint8_t *g_pend_proc, *g_pend_cmd;
static uint64_t g_level_t0, g_stable_t0;
static uint32_t g_stable_sig;
#define STARTUP_MIN_MS 3000      /* not before */
#define STARTUP_STILL_MS 2500    /* the processes running unchanged that long */
static uint64_t g_started;       /* when a mission started (ms); 0: none since the checkpoint */
static uint64_t g_settle;        /* until then (ms) the checkpoint follows the counters */
static int g_settle_base;        /* ...only the baseline (after a failure) */
#define SETTLE_MS 5000           /* a level's first lines, a pass's tidying up */
#define FAIL_SETTLE_MS 15000     /* a failure's tidying up: the script's way back to its phones */
/* the counters and phones a mission's start is seen from: the checkpoint's,
 * or after a failure the city as the failure left it */
static counter_t g_base[MAXCOUNT];
static uint8_t g_bgone[MAXPHONE];

/* ---- retrying: the city before the mission ---- */
#define TRIGS 0x771620u          /* the trigger table */
#define TRIGBYTES (210u * 32u)
#define CARS 0x4BE248u           /* the cars, 0x2B0 bytes each (0x408200) */
#define CARSIZE 0x2B0u
#define PEDS 0x7284E0u           /* the people, 0x100 bytes each (0x44F500) */
enum { T_CAR = 0, T_PED = 99 };
typedef struct { int16_t ok, model; int32_t bx, by, rot; } carinfo_t;   /* where it stood: in blocks */
typedef struct {
    uint8_t proc[PROCBYTES], trig[TRIGBYTES];
    uint8_t *cmds, *objs;
    carinfo_t *cars;
} city_t;
static uint32_t g_nobj;          /* this level's objects */
static int32_t *g_oline;         /* each one's script line */
static city_t g_ccity, g_rcity;  /* at the checkpoint; the one a retry goes back to */
static int g_retry;              /* a failed mission to put back */
static uint64_t g_fail_t;        /* when it failed (ms) */
#define RETRY_MIN_MS 5000        /* not before, after the failure */
static int g_fail_on;            /* the failure message up, the last step */
static char g_failtxt[2][48];

static uint32_t rec(void) { return 0x74F160u + MEM32(0x74F83C) * 0x1BCu; }

void joyride_savegame_init(const char *data_dir) {
    const char *prof = getenv("JOYRIDE_PROFILE");
    if (prof && *prof && !strchr(prof, '/') && strcmp(prof, ".") && strcmp(prof, ".."))
        snprintf(g_dir, sizeof g_dir, "%s/saves-%.64s", data_dir, prof);
    else snprintf(g_dir, sizeof g_dir, "%s/saves", data_dir);
}

static void guest_str(uint32_t va, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) {
        char c = (char)MEM8(va + i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

/* this character and level's file */
static void path_of(char *out, size_t n) {
    char name[32], safe[32];
    guest_str(rec() + 0xCC, name, sizeof name);
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < sizeof safe; i++) {
        char c = name[i];
        safe[j++] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_';
    }
    safe[j] = 0;
    snprintf(out, n, "%s/%s_%d.sav", g_dir, j ? safe : "player", (int32_t)MEM32(0x6B3E28));
}

/* ---- the counters -------------------------------------------------------- */

static int is_counter(int32_t t) { return t == T_COUNTER || t == T_MISSIONS || t == T_SECRETS; }

static void find_counters(void) {
    g_n = g_np = 0;
    for (uint32_t line = 0; line < NLINES; line++) {
        int32_t o = (int16_t)MEM16(LINES + 2 * line);
        if (o < 0 || (uint32_t)o >= MAXOBJ) continue;
        uint32_t a = OBJS + (uint32_t)o * 32u;
        int32_t t = (int32_t)MEM32(a);
        if (is_counter(t) && g_n < MAXCOUNT) g_obj[g_n] = a, g_check[g_n].line = (int32_t)line, g_n++;
        else if (t == T_TELEPHONE && g_np < MAXPHONE) g_pobj[g_np] = a, g_pline[g_np] = (int32_t)line, g_np++;
    }
}

static int phone_gone(int i) { return (int32_t)MEM32(g_pobj[i] + 4) < 0; }

/* the number of the script's commands: one past the highest a line names */
static uint32_t count_cmds(void) {
    int32_t hi = -1;
    for (uint32_t line = 0; line < NLINES; line++) {
        int32_t c = (int16_t)MEM16(CMDLINES + 2 * line);
        if (c > hi) hi = c;
    }
    return hi < 0 ? 0 : (uint32_t)hi + 1 > MAXCMD ? MAXCMD : (uint32_t)hi + 1;
}

/* the processes and commands: taken, put back */
/* a KICKSTART's object: given the first time it runs (0x448FEF: the first
 * free one down from 1399, type 0x64 then), 0 before; the processes it
 * started at +0x10, +0x14, +0x18 and +4 (-1 none) */
static uint32_t kick_obj(uint32_t cmd) {
    int32_t o = (int32_t)MEM32(CMDS + cmd * 24u + 0x10);
    return o > 0 && (uint32_t)o < MAXOBJ ? OBJS + (uint32_t)o * 32u : 0;
}
static void take_script(uint8_t *proc, uint8_t *cmds) {
    size_t o = 0;
    for (size_t k = 0; k < NPROCARR; k++)
        for (uint32_t i = 0; i < 32u * k_proc[k].size; i++) proc[o++] = MEM8(k_proc[k].va + i);
    for (uint32_t i = 0; i < g_ncmd * 24u; i++) cmds[i] = MEM8(CMDS + i);
    for (uint32_t k = 0; k < g_nkick; k++) {
        uint32_t a = kick_obj(g_kcmd[k]);
        for (uint32_t i = 0; i < 32; i++) cmds[g_ncmd * 24u + k * 32u + i] = a ? MEM8(a + i) : 0xFF;
    }
}
static void put_script(const uint8_t *proc, const uint8_t *cmds) {
    size_t o = 0;
    for (size_t k = 0; k < NPROCARR; k++)
        for (uint32_t i = 0; i < 32u * k_proc[k].size; i++) MEM8(k_proc[k].va + i) = proc[o++];
    for (uint32_t i = 0; i < g_ncmd * 24u; i++) MEM8(CMDS + i) = cmds[i];
    for (uint32_t k = 0; k < g_nkick; k++) {             /* the commands put back: their objects then */
        uint32_t a = kick_obj(g_kcmd[k]);
        if (a) for (uint32_t i = 0; i < 32; i++) MEM8(a + i) = cmds[g_ncmd * 24u + k * 32u + i];
    }
}
static void find_kicks(void) {
    g_nkick = 0;
    for (uint32_t i = 0; i < g_ncmd; i++) {
        if (MEM16(CMDS + i * 24u) == CMD_KICKSTART && g_nkick < MAXKICK) g_kcmd[g_nkick++] = i;
    }
}

static void city_free(city_t *c) {
    free(c->cmds), free(c->objs), free(c->cars);
    c->cmds = c->objs = NULL, c->cars = NULL;
}
static int city_alloc(city_t *c) {
    city_free(c);
    c->cmds = malloc(CMDBYTES ? CMDBYTES : 1);
    c->objs = malloc((size_t)(g_nobj ? g_nobj : 1) * 32u);
    c->cars = calloc(g_nobj ? g_nobj : 1, sizeof *c->cars);
    return c->cmds && c->objs && c->cars;
}
static void city_copy(city_t *to, const city_t *from) {
    memcpy(to->proc, from->proc, PROCBYTES), memcpy(to->trig, from->trig, TRIGBYTES);
    memcpy(to->cmds, from->cmds, CMDBYTES), memcpy(to->objs, from->objs, (size_t)g_nobj * 32u);
    memcpy(to->cars, from->cars, (size_t)g_nobj * sizeof *to->cars);
}

static uint32_t car_at(int32_t id) { return CARS + (uint32_t)id * CARSIZE; }
/* there and not wrecked (0x43E870, IS_GOAL_DEAD's: gone, or 100 damage) */
static int car_alive(int32_t id) {
    uint32_t c = car_at(id);
    return (int16_t)MEM16(c + 8) != -1 && 100 - (int16_t)MEM16(c + 0xFC) > 0;
}
/* where a script object is (0x43E680), in its units: 64 a block */
static void obj_where(int32_t line, int32_t *x, int32_t *y, int32_t *z, int32_t *rot) {
    uint32_t a[5] = { (uint32_t)line, 0x676610, 0x676614, 0x676618, 0x67660C };
    hle_call_guest(0x43E680, 5, a);
    *x = (int32_t)MEM32(0x676610), *y = (int32_t)MEM32(0x676614), *z = (int32_t)MEM32(0x676618);
    *rot = (int32_t)MEM32(0x67660C);
}

static void city_take(city_t *c) {
    if (!c->objs) return;
    take_script(c->proc, c->cmds);
    for (uint32_t i = 0; i < TRIGBYTES; i++) c->trig[i] = MEM8(TRIGS + i);
    for (uint32_t i = 0; i < g_nobj * 32u; i++) c->objs[i] = MEM8(OBJS + i);
    for (uint32_t o = 0; o < g_nobj; o++) {
        uint32_t a = OBJS + o * 32u;
        int32_t id = (int32_t)MEM32(a + 4);
        c->cars[o].ok = 0;
        if ((int32_t)MEM32(a) != T_CAR || id < 0 || id >= 1024 || g_oline[o] < 0 || !car_alive(id)) continue;
        int32_t x, y, z, rot;
        obj_where(g_oline[o], &x, &y, &z, &rot);
        c->cars[o] = (carinfo_t){ 1, (int16_t)MEM16(car_at(id) + 0x22), x >> 6, y >> 6, rot };
    }
}

/* the script's running processes (which of the 32), for "still" */
static uint32_t proc_sig(void) {
    uint32_t sig = 2166136261u;
    for (uint32_t i = 0; i < 32; i++) sig = (sig ^ MEM16(0x6761C0 + 2 * i)) * 16777619u;
    return sig;
}
static uint32_t g_snap_sig;          /* the processes running then */
static uint64_t g_cp_t;              /* when (ms) */
#define REFRESH_MS 30000             /* the script's opening, or a pass's tidying up, over by then */
static int g_failed;                 /* a mission failed since: the checkpoint is the one before it */

static void checkpoint(int settle) {
    g_snap_sig = proc_sig(), g_failed = 0, g_cp_t = SDL_GetTicks();
    for (int i = 0; i < g_n; i++) g_check[i].a = (int32_t)MEM32(g_obj[i] + 4), g_check[i].b = (int32_t)MEM32(g_obj[i] + 8);
    for (int i = 0; i < g_np; i++) g_pgone[i] = (uint8_t)phone_gone(i);
    if (g_ccmd) take_script(g_cproc, g_ccmd);
    memcpy(g_base, g_check, sizeof g_base), memcpy(g_bgone, g_pgone, sizeof g_bgone);
    if (joyride_cfg.retry) city_take(&g_ccity);
    g_started = 0;
    if (settle) g_settle = SDL_GetTicks() + SETTLE_MS, g_settle_base = 0;
}

/* the script at the checkpoint taken again, the counters and phones as
 * they were: the checkpoint's time (5 s after a level's start or a pass)
 * can be in the middle of the script's opening (its briefing, then the loop
 * that points at the phones started), and a city put back there runs it
 * again, a second loop pointing at the phones through the next job. Taken
 * once the processes running have kept still, and only while nothing has
 * happened since (no mission under way or failed, no counter or phone
 * changed) and soon after the checkpoint: the opening also disables
 * triggers, as a frenzy starting does, and a frenzy changes no counter */
static void refresh(void) {
    if (g_ccmd) take_script(g_cproc, g_ccmd);
    if (joyride_cfg.retry) city_take(&g_ccity);
    g_snap_sig = proc_sig();
}

/* only the baseline: the city as a failure left it */
static void baseline(void) {
    for (int i = 0; i < g_n; i++) g_base[i].a = (int32_t)MEM32(g_obj[i] + 4), g_base[i].b = (int32_t)MEM32(g_obj[i] + 8);
    for (int i = 0; i < g_np; i++) g_bgone[i] = (uint8_t)phone_gone(i);
}

/* ---- saving ---------------------------------------------------------------- */

/* a level is under way (it started, no menu shown since), of one's own */
int joyride_savegame_can_save(void) {
    return !g_menus && joyride_net_driveby() < 0 && (int32_t)MEM32(0x6B3E28) > 0;
}

/* seconds since the mission under way started, or -1 if none */
int joyride_savegame_mission_secs(void) {
    return g_started ? (int)((SDL_GetTicks() - g_started) / 1000) : -1;
}

static int write_save(char *msg, size_t n, const char *what) {
    save_t s;
    memset(&s, 0, sizeof s);
    memcpy(s.magic, MAGIC, 4);
    s.version = VERSION;
    s.section = (int32_t)MEM32(0x6B3E28);
    guest_str(0x6B3E88, s.map, sizeof s.map);
    uint32_t r = rec();
    guest_str(r + 0xCC, s.name, sizeof s.name);
    s.score = (int32_t)MEM32(r + 0xDC), s.multiplier = (int32_t)MEM32(r + 0xE0);
    for (uint32_t i = 0; i < sizeof s.digits; i++) s.digits[i] = MEM8(r + 0x10C + i);
    for (uint32_t i = 0; i < 4; i++) s.ammo[i] = MEM8(r + 0x140 + i);
    s.weapon = MEM32(r + 0x148);
    s.ncount = (uint32_t)g_n;

    char path[4200], tmp[4300];
    mkdir(g_dir, 0755);
    path_of(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int32_t gone[MAXPHONE];
    uint32_t ngone = 0;
    for (int i = 0; i < g_np; i++)
        if (g_pgone[i]) gone[ngone++] = g_pline[i];
    uint32_t ncmd = g_ccmd ? g_ncmd : 0;
    int ok = f && fwrite(&s, sizeof s, 1, f) == 1 && fwrite(g_check, sizeof *g_check, (size_t)g_n, f) == (size_t)g_n &&
             fwrite(&ngone, 4, 1, f) == 1 && fwrite(gone, 4, ngone, f) == ngone &&
             fwrite(&ncmd, 4, 1, f) == 1 && (!ncmd || (fwrite(g_cproc, 1, PROCBYTES, f) == PROCBYTES &&
                                                       fwrite(g_ccmd, 24, ncmd, f) == ncmd &&
                                                       fwrite(&g_nkick, 4, 1, f) == 1 &&
                                                       fwrite(g_ccmd + ncmd * 24u, 32, g_nkick, f) == g_nkick));
    if (f && fclose(f)) ok = 0;
    if (!ok || rename(tmp, path)) {
        snprintf(msg, n, "Save failed: %s", strerror(errno));
        remove(tmp);
        return 0;
    }
    int left = -1;
    for (int i = 0; i < g_n; i++)
        if ((int32_t)MEM32(g_obj[i]) == T_MISSIONS) left = left < 0 ? g_check[i].a : left + g_check[i].a;
    snprintf(msg, n, "Saved: %d points", s.score);
    fprintf(stderr, "[save] %s %s: %s, section %d, %d points x%d, %d counters (missions left %d), %u phones taken\n", what,
            path, s.name, s.section, s.score, s.multiplier, g_n, left, ngone);
    return 1;
}

int joyride_savegame_save(char *msg, size_t n) {
    if (!joyride_savegame_can_save()) { snprintf(msg, n, "Cannot save here"); return 0; }
    return write_save(msg, n, g_started ? "saved (the mission under way not kept)" : "saved");
}

/* ---- retrying ------------------------------------------------------------------- */

/* the two texts of a failure, as the level's language has them (0x47D9A0,
 * by key: the keys' strings are in the game, 0x4B3734 "2501", 0x4B3724
 * "2504"), letters and digits only: the message is copied as it is shown */
static void squeeze(const char *in, char *out, size_t n) {
    size_t j = 0;
    for (; *in && j + 1 < n; in++)
        if ((*in >= 'A' && *in <= 'Z') || (*in >= 'a' && *in <= 'z') || (*in >= '0' && *in <= '9')) out[j++] = *in;
    out[j] = 0;
}
static void fail_texts(void) {
    static const uint32_t key[2] = { 0x4B3734, 0x4B3724 };
    for (int i = 0; i < 2; i++) {
        uint32_t a = key[i];
        uint32_t p = hle_call_guest(0x47D9A0, 1, &a);
        char t[64] = "";
        if (p) guest_str(p, t, sizeof t);
        squeeze(t, g_failtxt[i], sizeof g_failtxt[i]);
    }
}
/* the message is cut into its lines where it is kept (a 0 between them:
 * "MISSION", "FAILED!"), and what follows is an older one's: read past the
 * cuts and compare the start */
static int fail_shown(void) {
    if (!MEM8(0x785102)) return 0;
    char t[44], q[48];
    for (uint32_t i = 0; i + 1 < sizeof t; i++) t[i] = MEM8(0x77EC80 + i) ? (char)MEM8(0x77EC80 + i) : ' ';
    t[sizeof t - 1] = 0;
    squeeze(t, q, sizeof q);
    for (int i = 0; i < 2; i++)
        if (g_failtxt[i][0] && !strncmp(q, g_failtxt[i], strlen(g_failtxt[i]))) return 1;
    return 0;
}

static int del_car(int32_t id) {
    uint32_t a = (uint32_t)id;
    hle_call_guest(0x43CE60, 1, &a);                      /* off the mission's cars */
    a = (uint32_t)id;
    return (uint8_t)hle_call_guest(0x4773A0, 1, &a);      /* 0: someone in it, left */
}
static void del_ped(int32_t id) {
    int32_t st = (int32_t)MEM32(PEDS + (uint32_t)id * 0x100u + 0x70);
    if (st == 0xC || st == 0x17) return;                  /* dead: the body stays */
    uint32_t a = (uint32_t)id;
    hle_call_guest(0x477320, 1, &a);
    a = (uint32_t)id;
    hle_call_guest(0x41A250, 1, &a);
    int32_t n = (int32_t)MEM32(0x6B3B80);                 /* off the mission's people, as KILL_PED */
    for (int32_t i = 0; i < n && i < 160; i++)
        if ((int32_t)MEM32(0x676390u + 4u * (uint32_t)i) == id) {
            for (int32_t k = i; k + 1 < n; k++) MEM32(0x676390u + 4u * (uint32_t)k) = MEM32(0x676394u + 4u * (uint32_t)k);
            MEM32(0x676390u + 4u * (uint32_t)(n - 1)) = 0xFFFFFFFFu;
            MEM32(0x6B3B80) = (uint32_t)(n - 1);
            break;
        }
}
/* a car where the script first put it, as PARKED_ON does */
static int32_t make_car(int32_t line, const carinfo_t *ci, int32_t bz) {
    int32_t x = ci->bx * 64, y = ci->by * 64, z = bz * 64;
    uint32_t a[6] = { (uint32_t)ci->bx, (uint32_t)ci->by, (uint32_t)bz, (uint32_t)(int32_t)ci->model, (uint32_t)ci->rot, 0 };
    int32_t id = (int16_t)hle_call_guest(0x475840, 6, a);
    if (id < 0) return -1;
    uint32_t b[6] = { (uint32_t)(x + 32), (uint32_t)(y + 32), (uint32_t)z, (uint32_t)id, (uint32_t)(int32_t)ci->model, (uint32_t)ci->rot };
    hle_call_guest(0x4770A0, 6, b);
    MEM16(car_at(id) + 0x134) = (uint16_t)line;
    MEM8(car_at(id) + 0x139) = 0;
    return id;
}

static void retry_now(void) {
    city_t *r = &g_rcity;
    int cars = 0, peds = 0, made = 0;
    static int32_t gone[1024];
    int ngone = 0;
    /* what the mission made: away */
    for (uint32_t o = 0; o < g_nobj; o++) {
        uint32_t a = OBJS + o * 32u;
        int32_t t = (int32_t)MEM32(a), id = (int32_t)MEM32(a + 4);
        int32_t ot, oid;                                  /* the object before the mission */
        memcpy(&ot, r->objs + o * 32u, 4), memcpy(&oid, r->objs + o * 32u + 4, 4);
        if (id < 0 || (t == ot && id == oid)) continue;
        int seen = 0;
        for (int k = 0; k < ngone; k++) seen |= gone[k] == (t << 16 | id);
        if (seen) continue;
        if (t == T_CAR && id < 1024) { if (del_car(id)) cars++; }
        else if (t == T_PED && id < 512) del_ped(id), peds++;
        else continue;
        if (ngone < 1024) gone[ngone++] = t << 16 | id;
    }
    /* the cars from before: wrecked or moved, made again */
    static uint8_t again[MAXOBJ];
    memset(again, 0, sizeof again);
    for (uint32_t o = 0; o < g_nobj; o++) {
        const carinfo_t *ci = &r->cars[o];
        if (!ci->ok) continue;
        int32_t oid;
        memcpy(&oid, r->objs + o * 32u + 4, 4);
        int32_t x, y, z, rot;
        int alive = car_alive(oid);
        if (alive) obj_where(g_oline[o], &x, &y, &z, &rot);
        if (alive && x >> 6 == ci->bx && y >> 6 == ci->by) continue;   /* where it was */
        if (!alive || del_car(oid)) again[o] = 1;   /* a wreck is left to the game; not one with someone in it */
    }
    /* the script as it was */
    for (uint32_t i = 0; i < g_nobj * 32u; i++) MEM8(OBJS + i) = r->objs[i];
    for (uint32_t i = 0; i < TRIGBYTES; i++) MEM8(TRIGS + i) = r->trig[i];
    put_script(r->proc, r->cmds);
    /* not in the old one's place in the cars: what burns there (a wreck's
     * fire) is the place's, and would burn on the new car. Its +0x139 set
     * keeps the game's search for a free one (0x407AB0) off it */
    static int32_t held[MAXOBJ];
    int nheld = 0;
    for (uint32_t o = 0; o < g_nobj; o++) {
        int32_t oid;
        memcpy(&oid, r->objs + o * 32u + 4, 4);
        if (again[o] && oid >= 0 && oid < 1024 && !MEM8(car_at(oid) + 0x139))
            MEM8(car_at(oid) + 0x139) = 1, held[nheld++] = oid;
    }
    for (uint32_t o = 0; o < g_nobj; o++) {
        if (!again[o]) continue;
        uint32_t a = OBJS + o * 32u;
        int32_t id = make_car(g_oline[o], &r->cars[o], (int32_t)MEM32(a + 0x18));
        MEM32(a + 4) = (uint32_t)id;
        if (id >= 0) made++;
    }
    for (int k = 0; k < nheld; k++) MEM8(car_at(held[k]) + 0x139) = 0;
    g_retry = 0;
    checkpoint(1);
    fprintf(stderr, "[save] retry: the city before the mission (%d cars and %d people of the mission taken away, %d cars made again)\n",
            cars, peds, made);
}

/* ---- loading ----------------------------------------------------------------- */

/* the save for this character and level, put back over the level just
 * started */
static void load(void) {
    if (joyride_net_driveby() >= 0 || (int32_t)MEM32(0x6B3E28) <= 0) return;
    char path[4200];
    path_of(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    save_t s;
    memset(&s, 0, sizeof s);
    size_t v1 = sizeof s - sizeof s.ncount;               /* a version 1 file ends before ncount */
    int ok = fread(&s, 1, sizeof s, f) >= v1;
    char map[32];
    guest_str(0x6B3E88, map, sizeof map);
    if (!ok || memcmp(s.magic, MAGIC, 4) || s.version < 1 || s.version > VERSION ||
        s.section != (int32_t)MEM32(0x6B3E28) || strncmp(s.map, map, sizeof map)) {
        fprintf(stderr, "[save] %s: not this level's, left alone\n", path);
        fclose(f);
        return;
    }
    uint32_t r = rec();
    MEM32(r + 0xDC) = (uint32_t)s.score, MEM32(r + 0xE0) = (uint32_t)s.multiplier;
    for (uint32_t i = 0; i < sizeof s.digits; i++) MEM8(r + 0x10C + i) = s.digits[i];
    for (uint32_t i = 0; i < 4; i++) MEM8(r + 0x140 + i) = s.ammo[i];
    MEM32(r + 0x148) = s.weapon;
    int put = 0;
    /* mission progress only from a save that has all of it (version 4): the
     * counters and phones without the processes left the script waiting on
     * a job already taken, and no job came */
    if (s.version >= 4) {
        for (uint32_t k = 0; k < s.ncount && k < MAXCOUNT; k++) {
            counter_t c;
            if (fread(&c, sizeof c, 1, f) != 1) break;
            if (c.line < 0 || (uint32_t)c.line >= NLINES) continue;
            int32_t o = (int16_t)MEM16(LINES + 2 * (uint32_t)c.line);
            if (o < 0 || (uint32_t)o >= MAXOBJ) continue;
            uint32_t a = OBJS + (uint32_t)o * 32u;
            if (!is_counter((int32_t)MEM32(a))) continue;    /* the script changed: not this counter */
            MEM32(a + 4) = (uint32_t)c.a, MEM32(a + 8) = (uint32_t)c.b;
            put++;
        }
    }
    int taken = 0;
    uint32_t ngone = 0;
    if (s.version >= 4 && fread(&ngone, 4, 1, f) == 1) {  /* the phones whose jobs were taken: out of the city */
        for (uint32_t k = 0; k < ngone && k < MAXPHONE; k++) {
            int32_t line;
            if (fread(&line, 4, 1, f) != 1 || line < 0 || (uint32_t)line >= NLINES) break;
            int32_t o = (int16_t)MEM16(LINES + 2 * (uint32_t)line);
            if (o < 0 || (uint32_t)o >= MAXOBJ) continue;
            uint32_t a = OBJS + (uint32_t)o * 32u;
            if ((int32_t)MEM32(a) != T_TELEPHONE || (int32_t)MEM32(a + 4) < 0) continue;
            MEM32(a + 4) = 0xFFFFFFFFu;                       /* as the game does: the booth stays in the city */
            taken++;
        }
    }
    int procs = 0;
    uint32_t ncmd = 0;
    if (s.version >= 4 && fread(&ncmd, 4, 1, f) == 1 && ncmd && ncmd == count_cmds()) {
        uint8_t *proc = malloc(PROCBYTES), *cmds = malloc(CMDBYTES);
        if (proc && cmds && fread(proc, 1, PROCBYTES, f) == PROCBYTES && fread(cmds, 24, ncmd, f) == ncmd) {
            uint32_t nkick = 0;
            g_pend_nokick = !(s.version >= 5 && fread(&nkick, 4, 1, f) == 1 && nkick == g_nkick &&
                              fread(cmds + ncmd * 24u, 32, nkick, f) == nkick);
            g_pend_proc = proc, g_pend_cmd = cmds, proc = cmds = NULL;   /* put back once the start-up is over */
            procs = 1;
        }
        free(proc), free(cmds);
    }
    fclose(f);
    fprintf(stderr, "[save] loaded %s: %d points x%d, %d counters, %d phones taken, processes %s\n", path, s.score,
            s.multiplier, put, taken, procs ? "to put back after the start-up" : "not in the save");
}

/* ---- each step ---------------------------------------------------------------- */

void joyride_savegame_menus(void) { g_menus = 1; }

void joyride_savegame_step(void) {
    if (g_menus) {                                        /* the first step of a level */
        g_menus = 0;
        g_ncmd = count_cmds();
        find_kicks();
        free(g_ccmd);
        g_ccmd = g_ncmd ? malloc(CMDBYTES) : NULL;
        free(g_pend_proc), free(g_pend_cmd);
        g_pend_proc = g_pend_cmd = NULL;
        g_level_t0 = g_stable_t0 = SDL_GetTicks(), g_stable_sig = 0;
        g_nobj = 0;
        for (uint32_t line = 0; line < NLINES; line++) {
            int32_t o = (int16_t)MEM16(LINES + 2 * line);
            if (o >= 0 && (uint32_t)o < MAXOBJ && (uint32_t)o + 1 > g_nobj) g_nobj = (uint32_t)o + 1;
        }
        free(g_oline);
        g_oline = malloc((size_t)(g_nobj ? g_nobj : 1) * sizeof *g_oline);
        for (uint32_t o = 0; g_oline && o < g_nobj; o++) g_oline[o] = -1;
        for (uint32_t line = 0; g_oline && line < NLINES; line++) {
            int32_t o = (int16_t)MEM16(LINES + 2 * line);
            if (o >= 0 && (uint32_t)o < g_nobj && g_oline[o] < 0) g_oline[o] = (int32_t)line;
        }
        if (!g_oline || !city_alloc(&g_ccity) || !city_alloc(&g_rcity)) city_free(&g_ccity), city_free(&g_rcity);
        g_retry = g_fail_on = 0;
        fail_texts();
        load();
        find_counters();
        checkpoint(1);
        return;
    }
    if (joyride_net_driveby() >= 0 || (int32_t)MEM32(0x6B3E28) <= 0) return;
    uint32_t sig = proc_sig();
    uint64_t now = SDL_GetTicks();
    if (sig != g_stable_sig) g_stable_sig = sig, g_stable_t0 = now;
    int still = now - g_stable_t0 >= STARTUP_STILL_MS;
    if (g_pend_proc) {                                    /* the save's processes, once the start-up is over */
        if (now - g_level_t0 < STARTUP_MIN_MS || now - g_stable_t0 < STARTUP_STILL_MS) return;
        if (g_pend_nokick)                                /* the start-up's lists, as they are */
            for (uint32_t k = 0; k < g_nkick; k++) {
                uint32_t a = kick_obj(g_kcmd[k]);
                for (uint32_t i = 0; i < 32; i++) g_pend_cmd[g_ncmd * 24u + k * 32u + i] = a ? MEM8(a + i) : 0xFF;
            }
        put_script(g_pend_proc, g_pend_cmd);
        free(g_pend_proc), free(g_pend_cmd);
        g_pend_proc = g_pend_cmd = NULL;
        checkpoint(1);
        fprintf(stderr, "[save] processes put back\n");
        return;
    }
    int fail = fail_shown();
    if (g_retry && !fail && !g_fail_on && joyride_cfg.retry && g_rcity.objs &&
        now - g_fail_t >= RETRY_MIN_MS && still) {        /* the failed mission, back */
        retry_now();
        return;
    }
    if (fail && !g_fail_on) {                             /* "MISSION FAILED!": what the mission was, kept */
        if (joyride_cfg.retry && g_ccity.objs) {
            city_copy(&g_rcity, &g_ccity);
            g_retry = 1, g_fail_t = now;
        }
        g_failed = 1;
        g_started = 0;                                    /* nothing under way; the checkpoint stays */
        baseline();
        g_settle = SDL_GetTicks() + FAIL_SETTLE_MS, g_settle_base = 1;
        fprintf(stderr, "[save] a mission failed%s\n", g_retry ? ": it is put back once the script has tidied up" : "");
    }
    g_fail_on = fail;
    int passed = 0, changed = 0;
    for (int i = 0; i < g_n; i++) {
        int32_t a = (int32_t)MEM32(g_obj[i] + 4), b = (int32_t)MEM32(g_obj[i] + 8), t = (int32_t)MEM32(g_obj[i]);
        if (a != g_base[i].a || b != g_base[i].b) changed = 1;
        if ((t == T_MISSIONS || t == T_SECRETS) && a < g_base[i].a) passed = 1;
    }
    for (int i = 0; i < g_np; i++)
        if (phone_gone(i) != g_bgone[i]) changed = 1;     /* a job's phone taken out: a mission starting */
    if (g_settle && !g_started && SDL_GetTicks() >= g_settle) {   /* settled: the checkpoint as it is now */
        if (g_settle_base) baseline();
        else checkpoint(0);
        g_settle = 0;
        return;
    }
    if (!passed && !changed && !g_started && !g_failed && !g_settle && still && sig != g_snap_sig &&
        now - g_cp_t < REFRESH_MS)
        refresh();                                        /* the script still since the checkpoint */
    if (passed) {                                         /* a mission passed: the new checkpoint */
        checkpoint(1);
        g_retry = 0;
        fprintf(stderr, "[save] a mission passed: checkpoint\n");
    } else if (changed && !g_started && SDL_GetTicks() < g_settle) {
        if (g_settle_base) baseline();                    /* the failure's tidying up */
        else checkpoint(0);                               /* the script settling: still the checkpoint */
    } else if (changed && !g_started) {                   /* a mission starts: saved as it was */
        g_started = SDL_GetTicks() ? SDL_GetTicks() : 1;
        g_retry = 0;
        char msg[64];
        write_save(msg, sizeof msg, "autosaved (a mission starts)");
    }
}
