/*
 * joyride - drive-bys: London 1961's, on GTA's 2002 executable, and in any
 * of the games when the player opts in (the F1 menu's "Drive-bys: always";
 * in a network game the host's choice, dplay.c, for every machine).
 *
 * London 1961's own executable (1999) has two mission commands the 2002
 * one lacks, PLAYER_DRIVE_BY and PLAYER_DRIVE_BY_OFF (its commands 154 and
 * 155: they set or clear a flag, [0x571ADC]; some cheats set it too). While
 * it is set, fire in a car (0x484E50) shoots the driver's weapon out of the
 * car's side: a copy of the driver's ped is put beside the car, half its
 * width out along its heading and its length plus two across, facing 45
 * degrees off the heading, and the ped's own "fire" (0x46DE40) is run on the
 * copy.
 *
 * Here: the launcher writes the two commands as DONOWT (the script's no-op)
 * with a marker the 2002 parser keeps, 31961 on and 31960 off; the DONOWT
 * handler (0x44A88E, spliced by tools/postlift.py) passes each command it
 * runs to joyride_script_nop. In the car's step (the splice at 0x40A418,
 * pad.c), while it is on and the driver holds fire (0x453210 sets the
 * driver's ped +0x46), the same shot: the 2002 ped's fire is 0x4532F0, the
 * same routine with the ped's fields at aligned places (weapon +0x60, fire
 * +0x46, reload +0x64, x/y/z +0x90/+0x94/+0x98, heading +0xA8), the sine
 * tables 0x511E28 and 0x512228 for London's 0x572DE0 and 0x5731E0.
 */
#include <stdio.h>
#include <string.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define CMD_DONOWT 17
#define MARK_ON 31961
#define MARK_OFF 31960

static int g_on;                         /* the mission's switch */
static uint32_t g_ped;                   /* guest memory for the ped's copy */
static int g_reload[8];                  /* steps until each player's next shot */

/* each DONOWT (and the three other commands that share its handler) the
 * script runs. Its 24-byte record, the type in the first word, one of the
 * script's commands (0x676660, 24 bytes each): in ebp from the interpreter's
 * switch (0x447214), which also keeps it at [esp + 0x10]; other ways into
 * the handler leave something else in ebp. */
#define CMDS 0x676660u
#define NCMDS 8192u
static int is_cmd(uint32_t rec) { return rec >= CMDS && rec < CMDS + NCMDS * 24u && (rec - CMDS) % 24u == 0; }

void joyride_script_nop(uint32_t ebp, uint32_t esp) {
    uint32_t rec = is_cmd(ebp) ? ebp : is_cmd(MEM32(esp + 0x10)) ? MEM32(esp + 0x10) : 0;
    if (!rec || MEM16(rec) != CMD_DONOWT) return;
    for (uint32_t o = 2; o + 2 <= 24; o += 2) {
        uint16_t v = MEM16(rec + o);
        if (v == MARK_ON || v == MARK_OFF) {
            if (g_on != (v == MARK_ON)) fprintf(stderr, "[driveby] %s\n", v == MARK_ON ? "on" : "off");
            g_on = v == MARK_ON;
            return;
        }
    }
}

/* on now: the mission allows it (London 1961), or the rule is "always" (the
 * host's in a network game, else this player's setting) */
int joyride_driveby_on(void) {
    int net = joyride_net_driveby();
    return g_on || (net >= 0 ? net : joyride_cfg.driveby);
}

/* the car's step (pad.c's splice at 0x40A418) */
void joyride_driveby(uint32_t car) {
    if (!joyride_driveby_on() || MEM16(car + 0x22) == 0x25) return;   /* the tank has its own gun */
    uint32_t ped = (uint32_t)(int32_t)(int16_t)MEM16(car + 2);
    if ((int32_t)ped < 0) return;
    uint32_t p = 0x7284E0u + ped * 256u;
    int32_t player = (int16_t)hle_call_guest(0x464440, 1, &ped);
    if (player < 0 || player >= 8) return;
    if (g_reload[player] > 0) g_reload[player]--;
    if (!MEM8(p + 0x46) || g_reload[player] > 0) return;  /* fire not held, or reloading */
    if (!g_ped) g_ped = hle_va(hle_alloc(256));
    if (!g_ped) return;
    for (uint32_t i = 0; i < 256; i += 4) MEM32(g_ped + i) = MEM32(p + i);
    int h = MEM16(car + 0x268) & 0x3FF, w = (int16_t)MEM16(car + 0x28), len = (int16_t)MEM16(car + 0x2A) + 2;
    int a = h * 4, b = ((h + 0x100) & 0x3FF) * 4;
    int32_t x = (int32_t)MEM32(0x511E28u + a) * w + (int32_t)MEM32(0x511E28u + b) * len + (int32_t)MEM32(car + 0x250);
    int32_t y = (int32_t)MEM32(0x512228u + a) * w + (int32_t)MEM32(0x512228u + b) * len + (int32_t)MEM32(car + 0x254);
    MEM32(g_ped + 0x90) = (uint32_t)x, MEM32(g_ped + 0x94) = (uint32_t)y, MEM32(g_ped + 0x98) = MEM32(car + 0x258);
    MEM16(g_ped + 0xA8) = (uint16_t)((h - 0x80) & 0x3FF);
    MEM8(g_ped + 0x49) = 100;
    /* in a car the ped carries none: the player's current one (L1/R1 change
     * it while drive-bys are on, pad.c), else the first with ammunition */
    uint32_t rec = 0x74F160u + (uint32_t)player * 444u;
    uint32_t wpn = MEM32(g_ped + 0x60);
    if (!wpn) wpn = MEM32(rec + 0x148);
    for (uint32_t w = 1; w <= 4 && (!wpn || !MEM8(rec + 0x13F + wpn)); w++) wpn = MEM8(rec + 0x13F + w) ? w : 0;
    if (!wpn) return;                                     /* no weapon, no drive-by */
    MEM32(g_ped + 0x60) = wpn;
    MEM16(g_ped + 0x64) = 0;                              /* ready: the reload is kept here, per player */
    uint32_t arg = g_ped;
    hle_call_guest(0x4532F0, 1, &arg);
    int16_t r = (int16_t)MEM16(g_ped + 0x64);
    g_reload[player] = r > 0 ? r : 0;
}
