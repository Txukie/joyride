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
extern uint32_t joyride_texfilter;            /* 0 off, 1 CPU, 2 GPU */
void joyride_texfilter_set(int on);
/* the GPU path's recording (texfilter.c), for the presenter */
void joyride_gbuf_size(int pitch_px, int rows);
int  joyride_gbuf_frame(const uint32_t **gb, int *pitch_px, int *rows);
void joyride_gbuf_changes(void (*page)(void *ctx, int slot, const uint8_t *texels),
                          void (*pal)(void *ctx, int slot, const uint32_t *colours), void *ctx);
void joyride_gbuf_done(void);
void joyride_gbuf_stats(int *pages, int *pals, int *resets, int *mid, int *midpal);
extern int joyride_tex_gpu_pages;           /* layers of the GPU's page texture */
void joyride_gbuf_forget(void);

/* the screen size in play (display.c): setup after boot, before run, with
 * the size wanted (0 x 0: the game's own); request from the overlay; apply
 * at the frame's safe point */
void joyride_display_setup(int w, int h);
void joyride_display_request(int w, int h);
void joyride_display_apply(void);
void joyride_display_current(int *w, int *h);
int  joyride_display_valid(int w, int h);
extern uint32_t joyride_rows;              /* the renderer's row table (guest address) */

/* the hardware renderer (hwrender.c): the world's quads, drawn by the GPU */
#define JOYRIDE_KEY 0xA5FF00FFu            /* the surface before the CPU draws (no game colour has A5 on top) */
#define JOYRIDE_QUAD_VERTICAL 1u           /* spans run down, not across */
#define JOYRIDE_QUAD_KEYED    2u           /* colour 0 is see-through */
#define JOYRIDE_QUAD_BLEND    4u           /* 50/50 with what is below */
#define JOYRIDE_QUAD_TILE     8u           /* a 64x64 world tile: filter within it */
typedef struct {                           /* std430, as shaders/hwquad.vert reads it */
    float x[4], y[4];                      /* corners, game pixels */
    float u[4], v[4];                      /* page coordinates, texels */
    uint32_t page, pal, mode, pad;         /* texture page slot, palette slot, JOYRIDE_QUAD_* */
} joyride_quad;
extern int joyride_hwrender;
void joyride_hw_begin(void);
int  joyride_hw_quads(const joyride_quad **q);   /* -1: not in use this frame */
void joyride_hw_done(void);
int  joyride_hw_active(void);                 /* this draw is the hardware renderer's */
void joyride_hw_mark(uint32_t surface, int band, int x0, int x1);   /* the CPU drew there (bands of 64 rows) */
void joyride_gbuf_dims(int *pitch_px, int *rows);

/* controllers (src/platform/pad.c) and the radio (mss.c) */
extern int joyride_controls_modern;
void joyride_pad_init(void);
void joyride_pad_event(const void *sdl_event);
void joyride_radio_step(int dir);

/* the city map (src/platform/map.c) */
void joyride_pad_poll(void);
void joyride_pad_state(float axes[6], uint32_t *buttons, float *share_f, float *share_b);
/* debugging (src/platform/debug.c) */
void joyride_debug_init(const char *data_dir);
void joyride_debug_step(uint32_t word);
void joyride_debug_frame(int kind, int32_t t, uint64_t slot_ns);
void joyride_debug_report(void);
void joyride_debug_give(void);
void joyride_debug_tank_fire(uint32_t car);
/* London 1961's drive-bys (src/platform/driveby.c) */
void joyride_script_nop(uint32_t ebp, uint32_t esp);
void joyride_driveby(uint32_t car);
int joyride_driveby_on(void);
/* network play's rules from the host (dplay.c): -1 not in a network game */
int joyride_net_driveby(void);
/* saving (src/platform/savegame.c) */
void joyride_savegame_init(const char *data_dir);
void joyride_savegame_menus(void);              /* a frame of the menus was shown */
void joyride_savegame_step(void);               /* each game step in play (pad.c) */
int  joyride_savegame_can_save(void);           /* in play, a level of one's own (not a network game) */
int  joyride_savegame_save(char *msg, size_t n);/* 1 saved; msg says what happened */
int  joyride_savegame_mission_secs(void);      /* since the mission under way started, -1 none */
/* the pause menu (overlay.c) */
void joyride_pause_toggle(void);
int  joyride_quit_asking(void);                 /* the game's "Ya want out?" is up, from the menu's Quit */
int  joyride_pad_in_play(void);
void joyride_screen_snapshot(const char *dir);     /* the next frame presented, saved there */                /* each pump: the pad in menus (pad.c) */
void joyride_map_init(const char *install_root, const char *data_dir);
/* the picture around the menus on wide screens (src/platform/backdrop.c) */
void joyride_backdrop_init(const char *data_dir);
int joyride_backdrop(const uint8_t **bgra, int *w, int *h);
int joyride_movie_playing(void);                /* the intro movie is on the screen (smack.c) */
void joyride_map_toggle(void);
int  joyride_map_open(void);
void joyride_camera_zoom(int dir);
uint32_t joyride_zoom_height(uint32_t z);
void joyride_smooth_offset(float *x, float *y);
int  joyride_smooth_frame_kind(void);     /* the camera's rounding (smooth.c) */
void joyride_hw_offset(float *x, float *y);         /* for the draw's quads (hwrender.c) */         /* the play view (display.c) */
const uint8_t *joyride_map_picture(int *size, int *id);
int  joyride_map_dots(float player[2], float obj[][2], int max);

/* frames between the game's steps (smooth.c) */
extern int joyride_smooth;
void joyride_smooth_wait(void);
int  joyride_smooth_skip(void);               /* the presenter: leave this flip out */
int  joyride_smooth_running(void);            /* drawing between steps now */
uint64_t joyride_smooth_target(uint64_t *deadline);   /* the presenter: this flip's slot, or 0 */

/* the in-game settings overlay (src/platform/overlay.c) */
typedef struct {
    int res_w, res_h;       /* screen size in play, 0 = the game's own (1024x768) */
    int filter;             /* bilinear texture filtering */
    int scaling;            /* presenter scaling mode, index into its list */
    int fullscreen;
    int smooth;             /* frames between the game's steps (smooth.c) */
    int hwrender;           /* the world drawn by the GPU (hwrender.c) */
    int controls_modern;    /* the controller scheme (pad.c) */
    int hud;                /* HUD size with the GPU renderer: 0 auto, else that many times */
    int driveby;            /* drive-bys: 0 as the game (London 1961's missions), 1 always */
    int retry;              /* a failed mission is put back, to start again (savegame.c) */
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
uint64_t joyride_screen_frame_ns(void);        /* frame time at the cap, 0 = uncapped */
int  joyride_screen_gpu_filter(void);          /* the presenter can filter on the GPU */
int  joyride_screen_hwrender(void);
int  joyride_screen_hud_scale(int h);          /* the HUD's size for a picture h rows high */            /* ... and draw the world (hardware renderer) */       /* when a paced flip can next show (SDL ns) */
void joyride_screen_key(int sdl_scancode, int down);   /* as if from the keyboard */

/* 1 while joyride's Flip marks the primary (src/platform/screen_sdl3.c) */
extern int joyride_flipping;

#endif
