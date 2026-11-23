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
 * Mission progress is the next step: the script's state (its processes,
 * 0x676620.., its commands' records, 0x676660, its objects, 0x5F30E8)
 * holds the world's people and cars too, so which of it can be put back is
 * still being worked out.
 *
 * Not in a network game: everyone must play the same game.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define MAGIC "JRSV"
#define VERSION 1u

static char g_dir[4096];
static int g_menus = 1;          /* menus seen since the last level started */

typedef struct {
    char magic[4];
    uint32_t version;
    int32_t section;
    char map[32], name[32];
    int32_t score, multiplier;
    uint8_t digits[0x1C];        /* +0x10C..+0x128 */
    uint8_t ammo[4];             /* +0x140..+0x143 */
    uint32_t weapon;             /* +0x148 */
} save_t;


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

/* a level is under way (it started, no menu shown since), of one's own */
int joyride_savegame_can_save(void) {
    return !g_menus && joyride_net_driveby() < 0 && (int32_t)MEM32(0x6B3E28) > 0;
}

int joyride_savegame_save(char *msg, size_t n) {
    if (!joyride_savegame_can_save()) { snprintf(msg, n, "Cannot save here"); return 0; }
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

    char path[4200], tmp[4300];
    mkdir(g_dir, 0755);
    path_of(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f || fwrite(&s, sizeof s, 1, f) != 1) {
        if (f) fclose(f);
        snprintf(msg, n, "Save failed: %s", strerror(errno));
        return 0;
    }
    fclose(f);
    if (rename(tmp, path)) { snprintf(msg, n, "Save failed: %s", strerror(errno)); return 0; }
    snprintf(msg, n, "Saved: %d points", s.score);
    fprintf(stderr, "[save] %s: %s, section %d, %d points x%d\n", path, s.name, s.section, s.score, s.multiplier);
    return 1;
}

/* the save for this character and level, put back over the level just
 * started */
static void load(void) {
    if (joyride_net_driveby() >= 0 || (int32_t)MEM32(0x6B3E28) <= 0) return;
    char path[4200];
    path_of(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    save_t s;
    int ok = fread(&s, sizeof s, 1, f) == 1;
    fclose(f);
    char map[32];
    guest_str(0x6B3E88, map, sizeof map);
    if (!ok || memcmp(s.magic, MAGIC, 4) || s.version != VERSION || s.section != (int32_t)MEM32(0x6B3E28) ||
        strncmp(s.map, map, sizeof map)) {
        fprintf(stderr, "[save] %s: not this level's, left alone\n", path);
        return;
    }
    uint32_t r = rec();
    MEM32(r + 0xDC) = (uint32_t)s.score, MEM32(r + 0xE0) = (uint32_t)s.multiplier;
    for (uint32_t i = 0; i < sizeof s.digits; i++) MEM8(r + 0x10C + i) = s.digits[i];
    for (uint32_t i = 0; i < 4; i++) MEM8(r + 0x140 + i) = s.ammo[i];
    MEM32(r + 0x148) = s.weapon;
    fprintf(stderr, "[save] loaded %s: %d points x%d\n", path, s.score, s.multiplier);
}

void joyride_savegame_menus(void) { g_menus = 1; }

void joyride_savegame_step(void) {
    if (!g_menus) return;
    g_menus = 0;                                          /* the first step of a level */
    load();
}
