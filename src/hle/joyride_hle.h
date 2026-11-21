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
void joyride_texfilter_init(void);

/* the screen size in play, JOYRIDE_RES (display.c): after boot, before run */
void joyride_display_setup(void);

/* 1 while joyride's Flip marks the primary (src/platform/screen_sdl3.c) */
extern int joyride_flipping;

#endif
