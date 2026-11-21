/* joyride's own HLE modules, registered ahead of win32hle's so they win. */
#ifndef JOYRIDE_HLE_H
#define JOYRIDE_HLE_H

#include "win32hle.h"

extern const win32hle_shim joyride_mss[];     /* mss32.dll (Miles) */
extern const win32hle_shim joyride_smack[];   /* smackw32.dll (Smacker), by ordinal */
extern const win32hle_shim joyride_ddraw[];   /* ddraw.dll exports win32hle lacks, Flip */
extern const win32hle_shim joyride_dplay[];   /* dplayx.dll (DirectPlay) */

void joyride_dplay_shutdown(void);           /* close any network session (dplay.c) */

/* bilinear texture filtering (texfilter.c): on/off, and the env setup */
extern uint32_t joyride_texfilter;

/* the screen size in play (display.c): setup after boot, before run, with
 * the size wanted (0 x 0: the game's own); request from the overlay; apply
 * at the frame's safe point */
void joyride_display_setup(int w, int h);
void joyride_display_request(int w, int h);
void joyride_display_apply(void);
void joyride_display_current(int *w, int *h);
int  joyride_display_valid(int w, int h);

/* the in-game settings overlay (src/platform/overlay.c) */
typedef struct {
    int res_w, res_h;       /* screen size in play, 0 = the game's own (1024x768) */
    int filter;             /* bilinear texture filtering */
    int scaling;            /* presenter scaling mode, index into its list */
    int fullscreen;
} joyride_settings;
extern joyride_settings joyride_cfg;
void joyride_config_load(const char *path);   /* then the JOYRIDE_* overrides */
void joyride_config_save(void);
int  joyride_overlay_open(void);
int  joyride_overlay_key(int sdl_scancode, int down, int repeat);   /* 1 = consumed */
void joyride_overlay_draw(uint32_t *bgra, int w, int h);
void joyride_safe_point(void);                 /* each frame in play (0x47DC80) */

/* the presenter's side (screen_sdl3.c) */
int  joyride_screen_scaling(int set);          /* set < 0: just read */
const char *joyride_screen_scaling_name(int i); /* NULL past the last */
int  joyride_screen_fullscreen(int set);
void joyride_screen_refresh(void);             /* show the current frame again */
void joyride_screen_key(int sdl_scancode, int down);   /* as if from the keyboard */

/* 1 while joyride's Flip marks the primary (src/platform/screen_sdl3.c) */
extern int joyride_flipping;

#endif
