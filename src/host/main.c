/*
 * joyride - host entry point.
 *
 *   joyride [--strict] <game install dir>
 *
 * The install dir is the folder holding WINO/ and GTADATA/ (Steam or the
 * Rockstar Classics release). The original executable is read from it at
 * start-up for its data sections; its code is the lifted C linked into us.
 * Nothing in the install is modified: files the game writes (saves, config)
 * go to an overlay directory under the user's data dir.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL3/SDL_keyboard.h>

#include "win32hle.h"
#include "host.h"
#include "joyride_hle.h"

#define EXE_NAME "Grand Theft Auto.exe"

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdirs(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

#define HKLM 0x80000002u
#define GTA_KEY "SOFTWARE\\DMA Design\\Grand Theft Auto"

/* Defaults for what GTA Settings.exe would write, used only before the
 * game has saved a registry of its own in the overlay. */
static void joyride_seed_registry(void) {
    hle_reg_set_dword(HKLM, GTA_KEY, "Language", 0);           /* English */
    /* "Control 0".."Control 9": DirectInput scan codes (>0xFF: joystick),
     * the defaults GTA Settings.exe ships with: left, right, up, down,
     * space, enter, left ctrl, X, Z, tab. */
    static const uint32_t keys[10] = {0xCB, 0xCD, 0xC8, 0xD0, 0x39,
                                      0x1C, 0x1D, 0x2D, 0x2C, 0x0F};
    for (int i = 0; i < 10; i++) {
        char name[16];
        snprintf(name, sizeof name, "Control %d", i);
        hle_reg_set_dword(HKLM, GTA_KEY "\\Controls", name, keys[i]);
    }
}

/* ---- developer switches ------------------------------------------------
 * JOYRIDE_DUMP=<dir>        save the shown frame as <dir>/frame_NNNN.ppm, once a second
 *                           (JOYRIDE_DUMP_EVERY_MS=<ms> changes the interval; 0 = every frame,
 *                           JOYRIDE_DUMP_FROM=<s> starts at that time)
 * JOYRIDE_QUIT_AFTER=<sec>  exit after that many seconds (scripted runs) */
static const char *g_dump_dir;
static uint32_t g_quit_after_ms, g_start_ms;

/* JOYRIDE_KEYS="4:RETURN,6.5:DOWN,10:UP:3": press a key at each time in
 * seconds and release it 120 ms later, or after the given seconds. */
/* by SDL scancode: the keys go where the keyboard's go (overlay, then game) */
static const struct { const char *name; int sdl; } KEYNAMES[] = {
    {"RETURN", 40}, {"ESCAPE", 41}, {"SPACE", 44}, {"UP", 82}, {"DOWN", 81}, {"LEFT", 80}, {"RIGHT", 79},
    {"CTRL", 224}, {"TAB", 43}, {"Z", 29}, {"X", 27}, {"F8", 65}, {"F9", 66}, {"F10", 67}, {"F12", 69},
};
#define MAX_SCRIPT 64
static struct { uint32_t at_ms, hold_ms; int sdl, state; } g_script[MAX_SCRIPT];
static int g_script_n;

static void parse_keys(const char *spec) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok && g_script_n < MAX_SCRIPT; tok = strtok(NULL, ",")) {
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = 0;
        uint32_t hold = 120;                               /* "t:KEY:seconds" holds the key */
        char *dur = strchr(colon + 1, ':');
        if (dur) { *dur = 0; hold = (uint32_t)(atof(dur + 1) * 1000.0); }
        int sdl = 0;
        for (size_t k = 0; k < sizeof KEYNAMES / sizeof KEYNAMES[0]; k++)
            if (!strcasecmp(colon + 1, KEYNAMES[k].name)) sdl = KEYNAMES[k].sdl;
        if (!sdl) sdl = (int)SDL_GetScancodeFromName(colon + 1);     /* any other: SDL's name ("P", "F1") */
        if (sdl) {
            g_script[g_script_n].at_ms = (uint32_t)(atof(tok) * 1000.0);
            g_script[g_script_n].sdl = sdl;
            g_script[g_script_n].hold_ms = hold;
            g_script_n++;
        }
    }
}

static void run_script(uint32_t t) {
    for (int i = 0; i < g_script_n; i++) {
        if (g_script[i].state == 0 && t >= g_script[i].at_ms) {
            fprintf(stderr, "[joyride] t=%.1fs key %d\n", t / 1000.0, g_script[i].sdl);
            g_script[i].state = 1;
            joyride_screen_key(g_script[i].sdl, 1);
        } else if (g_script[i].state == 1 && t >= g_script[i].at_ms + g_script[i].hold_ms) {
            g_script[i].state = 2;
            joyride_screen_key(g_script[i].sdl, 0);
        }
    }
}

/* JOYRIDE_MEMSNAP="<dir>:<from_s>:<count>": from that time on, save the
 * game's .data/.bss (0x4AB000..0x791000) at each of <count> frames, for
 * finding variables by how they change. */
static char g_snap_dir[PATH_MAX];
static uint32_t g_snap_from_ms;
static int g_snap_left;

static void mem_snapshot(uint32_t t) {
    if (g_snap_left <= 0 || t < g_snap_from_ms) return;
    static int n;
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/mem_%04d.bin", g_snap_dir, n++);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(hle_ptr(0x4AB000), 1, 0x791000 - 0x4AB000, f); fclose(f); }
    g_snap_left--;
}

static void frame_hook(const uint8_t *px, int w, int h, int pitch, int bpp) {
    uint32_t now = hle_ticks_ms();
    run_script(now - g_start_ms);
    mem_snapshot(now - g_start_ms);
    if (g_quit_after_ms && now - g_start_ms > g_quit_after_ms) {
        fprintf(stderr, "[joyride] JOYRIDE_QUIT_AFTER reached\n");
        joyride_dplay_shutdown();                    /* prints the session's message counts */
        _exit(0);
    }
    static uint32_t last;
    static int n;
    static int every = -1;
    if (every < 0) every = getenv("JOYRIDE_DUMP_EVERY_MS") ? atoi(getenv("JOYRIDE_DUMP_EVERY_MS")) : 1000;
    static int from_ms = -1;                         /* JOYRIDE_DUMP_FROM=<s>: start saving then */
    if (from_ms < 0) from_ms = getenv("JOYRIDE_DUMP_FROM") ? (int)(atof(getenv("JOYRIDE_DUMP_FROM")) * 1000) : 0;
    if (!g_dump_dir || now - g_start_ms < (uint32_t)from_ms || (last && now - last < (uint32_t)every)) return;
    last = now;
    const uint32_t *pal = NULL;
    const uint8_t *p2; int w2, h2, pitch2, bpp2; unsigned seq;
    if (bpp == 8) hle_dd_frame(&p2, &w2, &h2, &pitch2, &bpp2, &pal, &seq);
    uint32_t *shown = NULL;                          /* with the overlay drawn, when it is open */
    if (joyride_overlay_open() && bpp == 32 && (shown = malloc((size_t)w * (size_t)h * 4))) {
        for (int y = 0; y < h; y++) memcpy(shown + (size_t)y * (size_t)w, px + (size_t)y * (size_t)pitch, (size_t)w * 4);
        joyride_overlay_draw(shown, w, h);
        px = (const uint8_t *)shown, pitch = w * 4;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/frame_%04d.ppm", g_dump_dir, n++);
    FILE *f = fopen(path, "wb");
    if (!f) { free(shown); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * (size_t)pitch;
        for (int x = 0; x < w; x++) {
            uint8_t rgb[3] = {0, 0, 0};
            if (bpp == 16) {
                uint16_t c = (uint16_t)(row[2 * x] | row[2 * x + 1] << 8);
                rgb[0] = (uint8_t)((c >> 11 & 31) * 255 / 31);
                rgb[1] = (uint8_t)((c >> 5 & 63) * 255 / 63);
                rgb[2] = (uint8_t)((c & 31) * 255 / 31);
            } else if (bpp == 32 || bpp == 24) {
                const uint8_t *q = row + x * (bpp / 8);
                rgb[0] = q[2], rgb[1] = q[1], rgb[2] = q[0];
            } else if (bpp == 8 && pal) {
                uint32_t c = pal[row[x]];
                rgb[0] = (uint8_t)(c >> 16), rgb[1] = (uint8_t)(c >> 8), rgb[2] = (uint8_t)c;
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    free(shown);
}

/* JOYRIDE_PROFILE=<name> (the launcher sets it for each game: London's
 * saves are not GTA's) gives the game its own overlay, overlay-<name>,
 * and its own city maps (map.c); the settings stay shared. */
static void overlay_dir(char *out, size_t n) {
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME"), *p = getenv("JOYRIDE_PROFILE");
    char suffix[80] = "";
    if (p && *p && !strchr(p, '/') && strcmp(p, ".") && strcmp(p, "..")) snprintf(suffix, sizeof suffix, "-%.64s", p);
    if (xdg && *xdg) snprintf(out, n, "%s/joyride/overlay%s", xdg, suffix);
    else snprintf(out, n, "%s/.local/share/joyride/overlay%s", home ? home : ".", suffix);
}

int main(int argc, char **argv) {
    int strict = 0, a = 1;
    if (a < argc && !strcmp(argv[a], "--strict")) { strict = 1; a++; }
    if (a >= argc) {
        fprintf(stderr, "usage: %s [--strict] <game install dir>\n", argv[0]);
        return 64;
    }

    char root[PATH_MAX], wino[PATH_MAX], exe[PATH_MAX + 32], overlay[PATH_MAX];
    if (!realpath(argv[a], root)) { perror(argv[a]); return 66; }
    snprintf(wino, sizeof wino, "%s/WINO", root);
    if (!is_dir(wino)) { fprintf(stderr, "joyride: %s has no WINO folder\n", root); return 66; }
    snprintf(exe, sizeof exe, "%s/%s", wino, EXE_NAME);

    overlay_dir(overlay, sizeof overlay);
    mkdirs(overlay);

    /* SciTech MGL only tries its DirectDraw driver if ddraw.dll exists in
     * the system directory (GetSystemDirectoryA + FindFirstFileA). A
     * placeholder in the overlay satisfies the check; LoadLibraryA and
     * DirectDrawCreate are answered by the HLE layer, not by this file. */
    char sysdir[PATH_MAX + 32], marker[PATH_MAX + 64];
    snprintf(sysdir, sizeof sysdir, "%s/WINDOWS/SYSTEM32", overlay);
    mkdirs(sysdir);
    snprintf(marker, sizeof marker, "%s/ddraw.dll", sysdir);
    FILE *m = fopen(marker, "ab");
    if (m) fclose(m);

    /* C: is the install; the game runs from C:\WINO and reaches ..\GTADATA. */
    hle_set_drive('C', root);
    hle_set_overlay(root, overlay);
    if (chdir(wino) != 0) { perror(wino); return 66; }
    hle_set_command_line("\"C:\\WINO\\" EXE_NAME "\"");

    win32hle_register(joyride_mss);     /* first registration of a name wins */
    win32hle_register(joyride_smack);
    win32hle_register(joyride_ddraw);
    win32hle_register(joyride_dplay);
    recomp_host_init();

    g_dump_dir = getenv("JOYRIDE_DUMP");
    if (g_dump_dir) mkdirs(g_dump_dir);
    if (getenv("JOYRIDE_QUIT_AFTER")) g_quit_after_ms = (uint32_t)atoi(getenv("JOYRIDE_QUIT_AFTER")) * 1000u;
    g_start_ms = hle_ticks_ms();
    if (getenv("JOYRIDE_KEYS")) parse_keys(getenv("JOYRIDE_KEYS"));
    if (getenv("JOYRIDE_MEMSNAP")) {
        char spec[PATH_MAX];
        snprintf(spec, sizeof spec, "%s", getenv("JOYRIDE_MEMSNAP"));
        char *a = strrchr(spec, ':');
        if (a) { *a = 0; g_snap_left = atoi(a + 1); }
        char *b = strrchr(spec, ':');
        if (b) { *b = 0; g_snap_from_ms = (uint32_t)(atof(b + 1) * 1000.0); }
        snprintf(g_snap_dir, sizeof g_snap_dir, "%s", spec);
        mkdirs(g_snap_dir);
    }
    hle_screen_frame_hook = frame_hook;
    /* JOYRIDE_HEADLESS=1: no window (scripted runs, CI). */
    int headless = getenv("JOYRIDE_HEADLESS") && atoi(getenv("JOYRIDE_HEADLESS"));
    if (hle_screen_open("joyride", 0, headless) != 0) {
        fprintf(stderr, "joyride: cannot open the screen\n");
        return 69;
    }

    /* Settings (the F1 overlay keeps them): joyride.cfg beside the overlay
     * directory, then the JOYRIDE_* overrides. After the screen is up, for
     * JOYRIDE_RES=desktop. */
    char cfgfile[PATH_MAX + 32];
    snprintf(cfgfile, sizeof cfgfile, "%s/../joyride.cfg", overlay);
    joyride_config_load(cfgfile);
    char datadir[PATH_MAX + 8];
    snprintf(datadir, sizeof datadir, "%s/..", overlay);
    joyride_map_init(root, datadir);                     /* builds the city maps the first time */
    joyride_debug_init(datadir);
    joyride_backdrop_init(datadir);
    joyride_texfilter_set(joyride_cfg.filter);
    joyride_smooth = joyride_cfg.smooth;
    joyride_hwrender = joyride_cfg.hwrender;
    joyride_controls_modern = joyride_cfg.controls_modern;
    joyride_screen_scaling(joyride_cfg.scaling);
    if (joyride_cfg.fullscreen && !headless) joyride_screen_fullscreen(1);

    /* The registry persists in the overlay (win32hle writes it back on
     * change). GTA Settings.exe normally creates the game's key; seed it
     * with defaults the first time so the game starts without it. */
    char regfile[PATH_MAX + 32];
    snprintf(regfile, sizeof regfile, "%s/registry.txt", overlay);
    int fresh = access(regfile, F_OK) != 0;
    hle_reg_load(regfile);
    if (fresh) joyride_seed_registry();
    int rc = strict ? recomp_host_boot(exe) : recomp_host_boot_permissive(exe);
    if (rc) { fprintf(stderr, "joyride: boot failed (%d)\n", rc); return 70; }
    joyride_display_setup(joyride_cfg.res_w, joyride_cfg.res_h);

    /* The 2002 release ships with its intro movie switched off: the flag the
     * movie player checks (0x4AC06C, "skip the movie") starts at 1 in the
     * executable's data and nothing clears it, though MOVIE.SMK is still
     * installed. Clear it so the intro plays, unless JOYRIDE_NO_INTRO=1. */
    if (!(getenv("JOYRIDE_NO_INTRO") && atoi(getenv("JOYRIDE_NO_INTRO"))))
        MEM32(0x4AC06Cu) = 0;
    fprintf(stderr, "[joyride] install %s, overlay %s\n", root, overlay);
    return (int)recomp_host_run();
}
