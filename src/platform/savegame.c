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
 * pass: the script back to waiting) and put back on loading.
 * Put back, the counters and phones let the script's own loops offer the
 * next jobs (a failed one is offered again: it was not passed).
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
#define VERSION 4u               /* 1: no counters, 2: no phones, 3: no processes (all still read) */
#define MAXPHONE 512
#define MAXCOUNT 256

#define OBJS 0x5F30E8u           /* the script's objects, 32 bytes each */
#define MAXOBJ 4096u
#define LINES 0x6561C0u          /* script line -> object, int16, -1 none */
#define NLINES 65536u
#define CMDS 0x676660u           /* the script's commands, 24 bytes each */
#define CMDLINES 0x693B30u       /* script line -> command, int16 */
#define MAXCMD 8192u

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
/* a save's processes and commands, waiting for the level's start-up to end
 * (its first processes are kicked off over its first seconds) */
static uint8_t *g_pend_proc, *g_pend_cmd;
static uint64_t g_level_t0, g_stable_t0;
static uint32_t g_stable_sig;
#define STARTUP_MIN_MS 3000      /* not before */
#define STARTUP_STILL_MS 2500    /* the processes running unchanged that long */
static uint64_t g_started;       /* when a mission started (ms); 0: none since the checkpoint */
static uint64_t g_settle;        /* until then (ms) the checkpoint follows the counters */
#define SETTLE_MS 5000           /* a level's first lines, a pass's tidying up */

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
static void take_script(uint8_t *proc, uint8_t *cmds) {
    size_t o = 0;
    for (size_t k = 0; k < NPROCARR; k++)
        for (uint32_t i = 0; i < 32u * k_proc[k].size; i++) proc[o++] = MEM8(k_proc[k].va + i);
    for (uint32_t i = 0; i < g_ncmd * 24u; i++) cmds[i] = MEM8(CMDS + i);
}
static void put_script(const uint8_t *proc, const uint8_t *cmds) {
    size_t o = 0;
    for (size_t k = 0; k < NPROCARR; k++)
        for (uint32_t i = 0; i < 32u * k_proc[k].size; i++) MEM8(k_proc[k].va + i) = proc[o++];
    for (uint32_t i = 0; i < g_ncmd * 24u; i++) MEM8(CMDS + i) = cmds[i];
}

static void checkpoint(int settle) {
    for (int i = 0; i < g_n; i++) g_check[i].a = (int32_t)MEM32(g_obj[i] + 4), g_check[i].b = (int32_t)MEM32(g_obj[i] + 8);
    for (int i = 0; i < g_np; i++) g_pgone[i] = (uint8_t)phone_gone(i);
    if (g_ccmd) take_script(g_cproc, g_ccmd);
    g_started = 0;
    if (settle) g_settle = SDL_GetTicks() + SETTLE_MS;
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
                                                       fwrite(g_ccmd, 24, ncmd, f) == ncmd));
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
    if (s.version >= 2) {
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
    if (s.version >= 3 && fread(&ngone, 4, 1, f) == 1) {  /* the phones whose jobs were taken: out of the city */
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
        uint8_t *proc = malloc(PROCBYTES), *cmds = malloc((size_t)ncmd * 24u);
        if (proc && cmds && fread(proc, 1, PROCBYTES, f) == PROCBYTES && fread(cmds, 24, ncmd, f) == ncmd) {
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
        free(g_ccmd);
        g_ccmd = g_ncmd ? malloc((size_t)g_ncmd * 24u) : NULL;
        free(g_pend_proc), free(g_pend_cmd);
        g_pend_proc = g_pend_cmd = NULL;
        g_level_t0 = g_stable_t0 = SDL_GetTicks(), g_stable_sig = 0;
        load();
        find_counters();
        checkpoint(1);
        return;
    }
    if (joyride_net_driveby() >= 0 || (int32_t)MEM32(0x6B3E28) <= 0) return;
    if (g_pend_proc) {                                    /* the save's processes, once the start-up is over */
        uint32_t sig = 2166136261u;
        for (uint32_t i = 0; i < 32; i++) sig = (sig ^ MEM16(0x6761C0 + 2 * i)) * 16777619u;
        uint64_t now = SDL_GetTicks();
        if (sig != g_stable_sig) g_stable_sig = sig, g_stable_t0 = now;
        if (now - g_level_t0 < STARTUP_MIN_MS || now - g_stable_t0 < STARTUP_STILL_MS) return;
        put_script(g_pend_proc, g_pend_cmd);
        free(g_pend_proc), free(g_pend_cmd);
        g_pend_proc = g_pend_cmd = NULL;
        checkpoint(1);
        fprintf(stderr, "[save] processes put back\n");
        return;
    }
    int passed = 0, changed = 0;
    for (int i = 0; i < g_n; i++) {
        int32_t a = (int32_t)MEM32(g_obj[i] + 4), b = (int32_t)MEM32(g_obj[i] + 8), t = (int32_t)MEM32(g_obj[i]);
        if (a != g_check[i].a || b != g_check[i].b) changed = 1;
        if ((t == T_MISSIONS || t == T_SECRETS) && a < g_check[i].a) passed = 1;
    }
    for (int i = 0; i < g_np; i++)
        if (phone_gone(i) != g_pgone[i]) changed = 1;     /* a job's phone taken out: a mission starting */
    if (g_settle && !g_started && SDL_GetTicks() >= g_settle) {   /* settled: the checkpoint as it is now */
        checkpoint(0);
        g_settle = 0;
        return;
    }
    if (passed) {                                         /* a mission passed: the new checkpoint */
        checkpoint(1);
        fprintf(stderr, "[save] a mission passed: checkpoint\n");
    } else if (changed && !g_started && SDL_GetTicks() < g_settle) {
        checkpoint(0);                                    /* the script settling: still the checkpoint */
    } else if (changed && !g_started) {                   /* a mission starts: saved as it was */
        g_started = SDL_GetTicks() ? SDL_GetTicks() : 1;
        char msg[64];
        write_save(msg, sizeof msg, "autosaved (a mission starts)");
    }
}
