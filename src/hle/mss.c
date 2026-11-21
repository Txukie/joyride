/*
 * joyride - Miles Sound System (mss32.dll) for the 2002 GTA executable.
 *
 * The 38 AIL_ calls the game imports: a digital driver, sample voices that
 * play PCM straight out of guest memory, file streams (the WAV music and
 * speech), and timers that call back into the game. Everything is mixed in
 * software to 44.1 kHz stereo 16-bit.
 *
 * Handles given to the game are small blocks in guest memory (the game only
 * passes them back); the state lives host-side, found by handle.
 *
 * Registered before win32hle's own mss module, so these names win.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define OUT_RATE 44100
#define MAX_SAMPLES 64
#define MAX_STREAMS 8
#define MAX_TIMERS 16

/* AIL_sample_status / AIL_stream_status */
#define SMP_FREE    1
#define SMP_DONE    2
#define SMP_PLAYING 4
#define SMP_STOPPED 8

/* sample formats (AIL_set_sample_type) */
#define DIG_F_16BITS_MASK  1
#define DIG_F_STEREO_MASK  2
#define DIG_PCM_SIGN       1

typedef struct {
    uint32_t handle;            /* guest VA handed to the game */
    int      status;
    uint32_t addr, len;         /* PCM in guest memory */
    int      format, flags;
    int      rate, volume, pan, loops;
    uint64_t pos;               /* 32.32 fixed-point byte-frame position */
} voice_t;

typedef struct {
    uint32_t handle;
    int      status;
    FILE    *f;
    uint32_t data_off, data_len;    /* bytes of PCM in the file */
    int      channels, bits, rate;
    int      volume, loops;
    uint64_t pos;                   /* 32.32 fixed-point frames */
    uint8_t  buf[65536];            /* read cache */
    uint32_t buf_frame, buf_frames;
} stream_t;

typedef struct {
    uint32_t handle, callback, user;
    volatile int used, running, released;
    uint32_t period_us;
    pthread_t thread;
} atimer_t;

static voice_t   g_voice[MAX_SAMPLES];
static stream_t  g_stream[MAX_STREAMS];
static atimer_t  g_timer[MAX_TIMERS];
static pthread_mutex_t g_mix = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_dev;
static uint32_t g_driver;           /* the one HDIGDRIVER */
static int g_master = 127;
static uint64_t g_t0_ms;

static uint32_t new_handle(uint32_t size) { return hle_va(hle_alloc(size)); }

static voice_t *voice_of(uint32_t h) {
    for (int i = 0; i < MAX_SAMPLES; i++) if (g_voice[i].handle == h) return &g_voice[i];
    return NULL;
}
static stream_t *stream_of(uint32_t h) {
    for (int i = 0; i < MAX_STREAMS; i++) if (g_stream[i].handle == h) return &g_stream[i];
    return NULL;
}
static atimer_t *timer_of(uint32_t h) {
    for (int i = 0; i < MAX_TIMERS; i++) if (g_timer[i].used && g_timer[i].handle == h) return &g_timer[i];
    return NULL;
}

/* ---- mixing -------------------------------------------------------------- */

static inline int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static void pan_gains(int vol, int pan, int *l, int *r) {
    /* vol 0..127, pan 0 (left) .. 64 (centre) .. 127 (right); gains in 1/16384 */
    int v = vol * g_master;              /* 0 .. 16129 */
    *l = pan <= 64 ? v : v * (127 - pan) / 63;
    *r = pan >= 64 ? v : v * pan / 64;
}

static void mix_voice(voice_t *v, int32_t *out, int frames) {
    int bps = (v->format & DIG_F_16BITS_MASK) ? 2 : 1;
    int ch = (v->format & DIG_F_STEREO_MASK) ? 2 : 1;
    uint32_t frame_bytes = (uint32_t)(bps * ch);
    uint32_t nframes = v->len / frame_bytes;
    if (!nframes || v->rate <= 0) { v->status = SMP_DONE; return; }
    uint64_t step = ((uint64_t)v->rate << 32) / OUT_RATE;
    const uint8_t *pcm = (const uint8_t *)hle_ptr(v->addr);
    int gl, gr;
    pan_gains(v->volume, v->pan, &gl, &gr);
    for (int i = 0; i < frames; i++) {
        uint32_t fi = (uint32_t)(v->pos >> 32);
        if (fi >= nframes) {
            if (v->loops == 1) { v->status = SMP_DONE; return; }
            if (v->loops > 1) v->loops--;
            v->pos -= (uint64_t)nframes << 32;
            fi = (uint32_t)(v->pos >> 32);
            if (fi >= nframes) { v->pos = 0; fi = 0; }
        }
        const uint8_t *p = pcm + fi * frame_bytes;
        int sl, sr;
        if (bps == 2) {
            sl = (int16_t)(p[0] | p[1] << 8);
            sr = ch == 2 ? (int16_t)(p[2] | p[3] << 8) : sl;
        } else {
            int signed8 = v->flags & DIG_PCM_SIGN;
            sl = signed8 ? (int8_t)p[0] * 256 : (p[0] - 128) * 256;
            sr = ch == 2 ? (signed8 ? (int8_t)p[1] * 256 : (p[1] - 128) * 256) : sl;
        }
        out[2 * i]     += sl * gl >> 14;
        out[2 * i + 1] += sr * gr >> 14;
        v->pos += step;
    }
}

/* Fill the stream's cache so that frame `fi` is in it; 0 at end of data. */
static int stream_fetch(stream_t *s, uint32_t fi) {
    uint32_t fb = (uint32_t)(s->channels * s->bits / 8);
    uint32_t total = s->data_len / fb;
    if (fi >= total) return 0;
    if (fi >= s->buf_frame && fi < s->buf_frame + s->buf_frames) return 1;
    uint32_t want = sizeof s->buf / fb;
    if (fi + want > total) want = total - fi;
    fseek(s->f, (long)(s->data_off + fi * fb), SEEK_SET);
    s->buf_frames = (uint32_t)fread(s->buf, fb, want, s->f);
    s->buf_frame = fi;
    return s->buf_frames > 0;
}

static void mix_stream(stream_t *s, int32_t *out, int frames) {
    uint64_t step = ((uint64_t)s->rate << 32) / OUT_RATE;
    uint32_t fb = (uint32_t)(s->channels * s->bits / 8);
    int gl, gr;
    pan_gains(s->volume, 64, &gl, &gr);
    for (int i = 0; i < frames; i++) {
        uint32_t fi = (uint32_t)(s->pos >> 32);
        if (!stream_fetch(s, fi)) {
            if (s->loops == 1) { s->status = SMP_DONE; return; }
            if (s->loops > 1) s->loops--;
            s->pos = 0;
            if (!stream_fetch(s, 0)) { s->status = SMP_DONE; return; }
            fi = 0;
        }
        const uint8_t *p = s->buf + (fi - s->buf_frame) * fb;
        int sl, sr;
        if (s->bits == 16) {
            sl = (int16_t)(p[0] | p[1] << 8);
            sr = s->channels == 2 ? (int16_t)(p[2] | p[3] << 8) : sl;
        } else {
            sl = (p[0] - 128) * 256;
            sr = s->channels == 2 ? (p[1] - 128) * 256 : sl;
        }
        out[2 * i]     += sl * gl >> 14;
        out[2 * i + 1] += sr * gr >> 14;
        s->pos += step;
    }
}

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int frames = len / 4;
    int32_t acc[4096 * 2];
    int16_t *o = (int16_t *)stream;
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        memset(acc, 0, sizeof(int32_t) * 2 * (size_t)n);
        pthread_mutex_lock(&g_mix);
        for (int i = 0; i < MAX_SAMPLES; i++)
            if (g_voice[i].handle && g_voice[i].status == SMP_PLAYING) mix_voice(&g_voice[i], acc, n);
        for (int i = 0; i < MAX_STREAMS; i++)
            if (g_stream[i].handle && g_stream[i].status == SMP_PLAYING) mix_stream(&g_stream[i], acc, n);
        pthread_mutex_unlock(&g_mix);
        for (int i = 0; i < 2 * n; i++) o[i] = (int16_t)clamp16(acc[i]);
        o += 2 * n;
        frames -= n;
    }
}

static void audio_open(void) {
    if (g_dev) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[mss] no audio: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_dev) SDL_PauseAudioDevice(g_dev, 0);
    else fprintf(stderr, "[mss] no audio device: %s\n", SDL_GetError());
}

/* ---- timers ---------------------------------------------------------------- */

static void *timer_thread(void *arg) {
    atimer_t *t = (atimer_t *)arg;
    uint64_t due = SDL_GetTicks64() * 1000;
    while (!t->released) {
        if (!t->running || !t->period_us) { usleep(1000); due = SDL_GetTicks64() * 1000; continue; }
        due += t->period_us;
        int64_t wait = (int64_t)due - (int64_t)(SDL_GetTicks64() * 1000);
        if (wait > 0) usleep((useconds_t)wait);
        else if (wait < -200000) due = SDL_GetTicks64() * 1000;      /* far behind: no catch-up storm */
        if (t->running && !t->released) {
            uint32_t a[1] = { t->user };
            hle_call_guest(t->callback, 1, a);
        }
    }
    t->used = 0;
    return NULL;
}

/* ---- the shims --------------------------------------------------------------- */

static void a_startup(void)  { g_t0_ms = SDL_GetTicks64(); RET(1, 0); }
static void a_shutdown(void) {
    for (int i = 0; i < MAX_TIMERS; i++) if (g_timer[i].used) g_timer[i].released = 1;
    if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }
    RET(0, 0);
}
static void a_set_preference(void) { RET(0, 2); }            /* (number, value) -> previous */
static void a_ms_count(void)       { RET((uint32_t)(SDL_GetTicks64() - g_t0_ms), 0); }
static void a_mem_alloc_lock(void) { RET(hle_va(hle_alloc(A32(0) ? A32(0) : 1)), 1); }
static void a_mem_free_lock(void)  { if (A32(0)) hle_free(hle_ptr(A32(0))); RET(0, 1); }

/* (HDIGDRIVER *drvr, LPHWAVEOUT *lphWaveOut, s32 device, LPWAVEFORMAT fmt) -> 0 ok */
static void a_waveOutOpen(void) {
    audio_open();
    if (!g_driver) g_driver = new_handle(64);
    if (A32(0)) MEM32(A32(0)) = g_driver;
    if (A32(1)) MEM32(A32(1)) = 0;
    RET(0, 4);
}
static void a_waveOutClose(void) { RET(0, 1); }
/* (drv, s32 *rate, s32 *format, char *string) */
static void a_digital_configuration(void) {
    if (A32(1)) MEM32(A32(1)) = OUT_RATE;
    if (A32(2)) MEM32(A32(2)) = DIG_F_16BITS_MASK | DIG_F_STEREO_MASK;
    if (A32(3)) strcpy(ASTR(3), "joyride software mixer");
    RET(0, 4);
}
static void a_set_digital_master_volume(void) { g_master = (int)A32(1) & 127; RET(0, 2); }

static void a_allocate_sample_handle(void) {
    pthread_mutex_lock(&g_mix);
    for (int i = 0; i < MAX_SAMPLES; i++) if (!g_voice[i].handle) {
        memset(&g_voice[i], 0, sizeof g_voice[i]);
        g_voice[i].handle = new_handle(64);
        g_voice[i].status = SMP_DONE;
        g_voice[i].volume = 127; g_voice[i].pan = 64; g_voice[i].loops = 1; g_voice[i].rate = 11025;
        uint32_t h = g_voice[i].handle;
        pthread_mutex_unlock(&g_mix);
        RET(h, 1);
    }
    pthread_mutex_unlock(&g_mix);
    RET(0, 1);
}
static void a_release_sample_handle(void) {
    pthread_mutex_lock(&g_mix);
    voice_t *v = voice_of(A32(0));
    if (v) { hle_free(hle_ptr(v->handle)); memset(v, 0, sizeof *v); }
    pthread_mutex_unlock(&g_mix);
    RET(0, 1);
}
static void a_init_sample(void) {
    pthread_mutex_lock(&g_mix);
    voice_t *v = voice_of(A32(0));
    if (v) {
        uint32_t h = v->handle;
        memset(v, 0, sizeof *v);
        v->handle = h; v->status = SMP_DONE;
        v->volume = 127; v->pan = 64; v->loops = 1; v->rate = 11025;
    }
    pthread_mutex_unlock(&g_mix);
    RET(0, 1);
}

#define VOICE_SET(fn, argc, body) static void fn(void) { \
    pthread_mutex_lock(&g_mix); voice_t *v = voice_of(A32(0)); if (v) { body; } \
    pthread_mutex_unlock(&g_mix); RET(0, argc); }

VOICE_SET(a_set_sample_type,          3, v->format = (int)A32(1); v->flags = (int)A32(2))
VOICE_SET(a_set_sample_address,       3, v->addr = A32(1); v->len = A32(2); v->pos = 0)
VOICE_SET(a_set_sample_playback_rate, 2, v->rate = (int)A32(1))
VOICE_SET(a_set_sample_volume,        2, v->volume = (int)A32(1) & 127)
VOICE_SET(a_set_sample_pan,           2, v->pan = (int)A32(1) & 127)
VOICE_SET(a_set_sample_loop_count,    2, v->loops = (int)A32(1))
VOICE_SET(a_start_sample,             1, v->pos = 0; v->status = SMP_PLAYING)
VOICE_SET(a_end_sample,               1, v->status = SMP_DONE)

static void a_sample_status(void) {
    pthread_mutex_lock(&g_mix);
    voice_t *v = voice_of(A32(0));
    int st = v ? v->status : SMP_FREE;
    pthread_mutex_unlock(&g_mix);
    RET(st, 1);
}

/* WAV header: find fmt and data chunks. */
static int wav_parse(stream_t *s) {
    uint8_t h[12];
    if (fread(h, 1, 12, s->f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, s->f) != 8) return 0;
        uint32_t n = (uint32_t)(c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t f[16];
            if (n < 16 || fread(f, 1, 16, s->f) != 16) return 0;
            if ((f[0] | f[1] << 8) != 1) return 0;            /* PCM only */
            s->channels = f[2] | f[3] << 8;
            s->rate = (int)(f[4] | f[5] << 8 | f[6] << 16 | (uint32_t)f[7] << 24);
            s->bits = f[14] | f[15] << 8;
            fseek(s->f, (long)(n - 16 + (n & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            s->data_off = (uint32_t)ftell(s->f);
            s->data_len = n;
            return s->channels && s->rate && (s->bits == 8 || s->bits == 16);
        } else {
            fseek(s->f, (long)(n + (n & 1)), SEEK_CUR);
        }
    }
}

/* (drv, char *filename, s32 stream_mem) -> HSTREAM */
static void a_open_stream(void) {
    const char *name = ASTR(1);
    char host[1024];
    if (!hle_host_path(name, host, sizeof host)) {
        fprintf(stderr, "[mss] open_stream %s: not found\n", name);
        RET(0, 3);
    }
    pthread_mutex_lock(&g_mix);
    for (int i = 0; i < MAX_STREAMS; i++) if (!g_stream[i].handle) {
        stream_t *s = &g_stream[i];
        memset(s, 0, sizeof *s);
        s->f = fopen(host, "rb");
        if (!s->f || !wav_parse(s)) {
            if (s->f) fclose(s->f);
            memset(s, 0, sizeof *s);
            pthread_mutex_unlock(&g_mix);
            fprintf(stderr, "[mss] open_stream %s: not a PCM WAV\n", name);
            RET(0, 3);
        }
        s->handle = new_handle(64);
        s->status = SMP_STOPPED; s->volume = 127; s->loops = 1;
        uint32_t h = s->handle;
        pthread_mutex_unlock(&g_mix);
        RET(h, 3);
    }
    pthread_mutex_unlock(&g_mix);
    RET(0, 3);
}
static void a_close_stream(void) {
    pthread_mutex_lock(&g_mix);
    stream_t *s = stream_of(A32(0));
    if (s) { if (s->f) fclose(s->f); hle_free(hle_ptr(s->handle)); memset(s, 0, sizeof *s); }
    pthread_mutex_unlock(&g_mix);
    RET(0, 1);
}

#define STREAM_SET(fn, argc, body) static void fn(void) { \
    pthread_mutex_lock(&g_mix); stream_t *s = stream_of(A32(0)); if (s) { body; } \
    pthread_mutex_unlock(&g_mix); RET(0, argc); }

STREAM_SET(a_start_stream,           1, s->pos = 0; s->status = SMP_PLAYING)
STREAM_SET(a_pause_stream,           2, if (A32(1)) { if (s->status == SMP_PLAYING) s->status = SMP_STOPPED; }
                                        else if (s->status == SMP_STOPPED) s->status = SMP_PLAYING)
STREAM_SET(a_set_stream_volume,      2, s->volume = (int)A32(1) & 127)
STREAM_SET(a_set_stream_loop_count,  2, s->loops = (int)A32(1))
STREAM_SET(a_set_stream_position,    2, uint32_t fb = (uint32_t)(s->channels * s->bits / 8);
                                        s->pos = (uint64_t)(A32(1) / fb) << 32)
static void a_service_stream(void) { RET(0, 2); }          /* we stream from the mixer */

static void a_stream_status(void) {
    pthread_mutex_lock(&g_mix);
    stream_t *s = stream_of(A32(0));
    int st = s ? s->status : SMP_FREE;
    pthread_mutex_unlock(&g_mix);
    RET(st, 1);
}
static void a_stream_position(void) {
    pthread_mutex_lock(&g_mix);
    stream_t *s = stream_of(A32(0));
    uint32_t p = s ? (uint32_t)(s->pos >> 32) * (uint32_t)(s->channels * s->bits / 8) : 0;
    pthread_mutex_unlock(&g_mix);
    RET(p, 1);
}
/* (st, s32 *datarate, s32 *sndtype, s32 *length, s32 *memory) */
static void a_stream_info(void) {
    pthread_mutex_lock(&g_mix);
    stream_t *s = stream_of(A32(0));
    if (s) {
        if (A32(1)) MEM32(A32(1)) = (uint32_t)s->rate;
        if (A32(2)) MEM32(A32(2)) = (s->bits == 16 ? DIG_F_16BITS_MASK : 0) | (s->channels == 2 ? DIG_F_STEREO_MASK : 0);
        if (A32(3)) MEM32(A32(3)) = s->data_len;
        if (A32(4)) MEM32(A32(4)) = sizeof s->buf;
    }
    pthread_mutex_unlock(&g_mix);
    RET(0, 5);
}

static void a_register_timer(void) {                     /* (callback) -> HTIMER */
    for (int i = 0; i < MAX_TIMERS; i++) if (!g_timer[i].used) {
        atimer_t *t = &g_timer[i];
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->handle = (uint32_t)(i + 1);
        t->callback = A32(0);
        t->period_us = 1000000 / 100;
        if (pthread_create(&t->thread, NULL, timer_thread, t) != 0) { t->used = 0; RET(0xFFFFFFFFu, 1); }
        pthread_detach(t->thread);
        RET(t->handle, 1);
    }
    RET(0xFFFFFFFFu, 1);
}
static void a_set_timer_frequency(void) {                /* (timer, hz) */
    atimer_t *t = timer_of(A32(0));
    if (t && A32(1)) t->period_us = 1000000u / A32(1);
    RET(0, 2);
}
static void a_start_timer(void)          { atimer_t *t = timer_of(A32(0)); if (t) t->running = 1; RET(0, 1); }
static void a_stop_timer(void)           { atimer_t *t = timer_of(A32(0)); if (t) t->running = 0; RET(0, 1); }
static void a_release_timer_handle(void) { atimer_t *t = timer_of(A32(0)); if (t) { t->running = 0; t->released = 1; } RET(0, 1); }

const win32hle_shim joyride_mss[] = {
    { "_AIL_startup@0", a_startup },                 { "_AIL_shutdown@0", a_shutdown },
    { "_AIL_set_preference@8", a_set_preference },   { "_AIL_ms_count@0", a_ms_count },
    { "_AIL_mem_alloc_lock@4", a_mem_alloc_lock },   { "_AIL_mem_free_lock@4", a_mem_free_lock },
    { "_AIL_waveOutOpen@16", a_waveOutOpen },        { "_AIL_waveOutClose@4", a_waveOutClose },
    { "_AIL_digital_configuration@16", a_digital_configuration },
    { "_AIL_set_digital_master_volume@8", a_set_digital_master_volume },
    { "_AIL_allocate_sample_handle@4", a_allocate_sample_handle },
    { "_AIL_release_sample_handle@4", a_release_sample_handle },
    { "_AIL_init_sample@4", a_init_sample },
    { "_AIL_set_sample_type@12", a_set_sample_type },
    { "_AIL_set_sample_address@12", a_set_sample_address },
    { "_AIL_set_sample_playback_rate@8", a_set_sample_playback_rate },
    { "_AIL_set_sample_volume@8", a_set_sample_volume },
    { "_AIL_set_sample_pan@8", a_set_sample_pan },
    { "_AIL_set_sample_loop_count@8", a_set_sample_loop_count },
    { "_AIL_start_sample@4", a_start_sample },       { "_AIL_end_sample@4", a_end_sample },
    { "_AIL_sample_status@4", a_sample_status },
    { "_AIL_open_stream@12", a_open_stream },        { "_AIL_close_stream@4", a_close_stream },
    { "_AIL_start_stream@4", a_start_stream },       { "_AIL_pause_stream@8", a_pause_stream },
    { "_AIL_service_stream@8", a_service_stream },
    { "_AIL_set_stream_volume@8", a_set_stream_volume },
    { "_AIL_set_stream_loop_count@8", a_set_stream_loop_count },
    { "_AIL_set_stream_position@8", a_set_stream_position },
    { "_AIL_stream_position@4", a_stream_position }, { "_AIL_stream_status@4", a_stream_status },
    { "_AIL_stream_info@20", a_stream_info },
    { "_AIL_register_timer@4", a_register_timer },
    { "_AIL_set_timer_frequency@8", a_set_timer_frequency },
    { "_AIL_start_timer@4", a_start_timer },         { "_AIL_stop_timer@4", a_stop_timer },
    { "_AIL_release_timer_handle@4", a_release_timer_handle },
    { NULL, NULL },
};
