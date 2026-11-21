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
#include <sys/stat.h>
#include <unistd.h>

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
 * JOYRIDE_QUIT_AFTER=<sec>  exit after that many seconds (scripted runs) */
static const char *g_dump_dir;
static uint32_t g_quit_after_ms, g_start_ms;

static void frame_hook(const uint8_t *px, int w, int h, int pitch, int bpp) {
    uint32_t now = hle_ticks_ms();
    if (g_quit_after_ms && now - g_start_ms > g_quit_after_ms) {
        fprintf(stderr, "[joyride] JOYRIDE_QUIT_AFTER reached\n");
        _exit(0);
    }
    static uint32_t last;
    static int n;
    if (!g_dump_dir || (last && now - last < 1000)) return;
    last = now;
    const uint32_t *pal = NULL;
    const uint8_t *p2; int w2, h2, pitch2, bpp2; unsigned seq;
    if (bpp == 8) hle_dd_frame(&p2, &w2, &h2, &pitch2, &bpp2, &pal, &seq);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/frame_%04d.ppm", g_dump_dir, n++);
    FILE *f = fopen(path, "wb");
    if (!f) return;
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
}

static void overlay_dir(char *out, size_t n) {
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/joyride/overlay", xdg);
    else snprintf(out, n, "%s/.local/share/joyride/overlay", home ? home : ".");
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
    recomp_host_init();

    g_dump_dir = getenv("JOYRIDE_DUMP");
    if (g_dump_dir) mkdirs(g_dump_dir);
    if (getenv("JOYRIDE_QUIT_AFTER")) g_quit_after_ms = (uint32_t)atoi(getenv("JOYRIDE_QUIT_AFTER")) * 1000u;
    g_start_ms = hle_ticks_ms();
    hle_screen_frame_hook = frame_hook;
    /* JOYRIDE_HEADLESS=1: no window (scripted runs, CI). */
    int headless = getenv("JOYRIDE_HEADLESS") && atoi(getenv("JOYRIDE_HEADLESS"));
    if (hle_screen_open("joyride", 0, headless) != 0) {
        fprintf(stderr, "joyride: cannot open the screen\n");
        return 69;
    }

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
    fprintf(stderr, "[joyride] install %s, overlay %s\n", root, overlay);
    return (int)recomp_host_run();
}
