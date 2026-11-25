/*
 * joyride - Smacker (smackw32.dll), imported by ordinal.
 *
 * The game plays one movie, GTADATA\MOVIE.SMK (the intro: 320x200, 25 fps,
 * mono 44.1 kHz), from 0x44B160:
 *
 *   SmackSoundUseMSS(digdriver); s = SmackOpen(name, 0xFE000, -1);
 *   SmackToBuffer(s, 0, 20, pitch, height, buf, 0)      an 8-bit MGL buffer
 *   per frame: if s->NewPalette, set s->Palette; SmackDoFrame(s);
 *              while (SmackToBufferRect(s, 0)) blit s->LastRect, doubled;
 *              SmackNextFrame(s); while (SmackWait(s)) ;   any key skips
 *   SmackClose(s)
 *
 * Decoding is ffmpeg's (smackvideo, smackaudio); the sound goes to its own
 * SDL3 audio stream, as Smacker on Miles would have mixed it. Built without
 * ffmpeg (JOYRIDE_WITH_FFMPEG unset), SmackOpen fails and the game skips the
 * movie.
 *
 * The Smack structure the game reads (SMACK.H): Frames +0x0C, NewPalette
 * +0x68, Palette[768] +0x6C, FrameNum +0x374, LastRectx/y/w/h +0x380..+0x38C.
 */
#define _GNU_SOURCE
#ifndef JOYRIDE_OSPATH
#define JOYRIDE_OSPATH(p) (p)            /* Windows: compat/win/posix_win.h */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "joyride_hle.h"

#define SMK_VERSION    0x00
#define SMK_WIDTH      0x04
#define SMK_HEIGHT     0x08
#define SMK_FRAMES     0x0C
#define SMK_MSPERFRAME 0x10
#define SMK_NEWPALETTE 0x68
#define SMK_PALETTE    0x6C
#define SMK_FRAMENUM   0x374
#define SMK_LASTRECTX  0x380
#define SMK_LASTRECTY  0x384
#define SMK_LASTRECTW  0x388
#define SMK_LASTRECTH  0x38C
#define SMK_SIZE       0x400

static void s_SmackSoundUseMSS(void) { RET(1, 1); }          /* #33 (digdriver) -> ok */

#ifdef JOYRIDE_WITH_FFMPEG

#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

typedef struct {
    uint32_t va;                     /* the Smack structure in guest memory */
    AVFormatContext *fmt;
    AVCodecContext *vdec, *adec;
    int vidx, aidx;
    AVPacket *pkt;
    AVFrame *frame;
    int w, h;
    uint8_t *pix;                    /* the current frame's 8-bit pixels */
    uint32_t pal[256];
    int have_pal;
    uint32_t buf, pitch, height, left, top;   /* SmackToBuffer's destination */
    int rect_pending;
    uint64_t t0_ns, ns_per_frame;
    uint32_t frame_no;
    SDL_AudioStream *audio;
} movie_t;

static movie_t *g_movie;             /* the game plays one at a time */
static int g_trace = -1;             /* JOYRIDE_SMKTRACE=1: log the calls */
#define TRACE(...) do { if (g_trace < 0) g_trace = getenv("JOYRIDE_SMKTRACE") != NULL; \
                        if (g_trace) fprintf(stderr, "[smack] " __VA_ARGS__); } while (0)

int joyride_movie_playing(void) { return g_movie != NULL; }
static movie_t *movie_of(uint32_t va) { return g_movie && g_movie->va == va ? g_movie : NULL; }

static void movie_free(movie_t *m) {
    if (!m) return;
    if (m->audio) SDL_DestroyAudioStream(m->audio);
    av_frame_free(&m->frame);
    av_packet_free(&m->pkt);
    avcodec_free_context(&m->vdec);
    avcodec_free_context(&m->adec);
    avformat_close_input(&m->fmt);
    free(m->pix);
    if (m->va) hle_free(hle_ptr(m->va));
    free(m);
}

static AVCodecContext *open_dec(AVFormatContext *fmt, int idx) {
    const AVCodec *c = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    if (!c) return NULL;
    AVCodecContext *ctx = avcodec_alloc_context3(c);
    if (!ctx || avcodec_parameters_to_context(ctx, fmt->streams[idx]->codecpar) < 0 || avcodec_open2(ctx, c, NULL) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

/* The movie's sound: its own SDL3 stream, converted by SDL to the device's. */
static void audio_out(movie_t *m) {
    AVFrame *f = m->frame;
    if (!m->audio) {
        SDL_AudioFormat fmt;
        switch (f->format) {
        case AV_SAMPLE_FMT_U8: case AV_SAMPLE_FMT_U8P: fmt = SDL_AUDIO_U8; break;
        case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: fmt = SDL_AUDIO_S16; break;
        case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: fmt = SDL_AUDIO_F32; break;
        default: return;
        }
        SDL_AudioSpec spec = { fmt, f->ch_layout.nb_channels, f->sample_rate };
        if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
        m->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!m->audio) { fprintf(stderr, "[smack] no audio: %s\n", SDL_GetError()); return; }
        SDL_ResumeAudioStreamDevice(m->audio);
    }
    int ch = f->ch_layout.nb_channels, bps = av_get_bytes_per_sample((enum AVSampleFormat)f->format);
    if (!av_sample_fmt_is_planar((enum AVSampleFormat)f->format) || ch == 1) {
        SDL_PutAudioStreamData(m->audio, f->data[0], f->nb_samples * ch * bps);
        return;
    }
    size_t n = (size_t)f->nb_samples * (size_t)ch * (size_t)bps;
    uint8_t *il = (uint8_t *)malloc(n);
    if (!il) return;
    for (int s = 0; s < f->nb_samples; s++)
        for (int c = 0; c < ch; c++)
            memcpy(il + ((size_t)s * ch + c) * bps, f->data[c] + (size_t)s * bps, (size_t)bps);
    SDL_PutAudioStreamData(m->audio, il, (int)n);
    free(il);
}

/* Decode up to the next video frame, playing the sound met on the way.
 * 1: a frame is in m->pix; 0: the end. */
static int next_frame(movie_t *m) {
    for (;;) {
        int r = av_read_frame(m->fmt, m->pkt);
        if (r < 0) return 0;
        AVCodecContext *dec = m->pkt->stream_index == m->vidx ? m->vdec
                            : m->pkt->stream_index == m->aidx ? m->adec : NULL;
        int got_video = 0;
        if (dec && avcodec_send_packet(dec, m->pkt) >= 0) {
            while (avcodec_receive_frame(dec, m->frame) >= 0) {
                if (dec == m->adec) { audio_out(m); continue; }
                AVFrame *f = m->frame;
                if (f->format != AV_PIX_FMT_PAL8) continue;
                for (int y = 0; y < m->h && y < f->height; y++)
                    memcpy(m->pix + (size_t)y * m->w, f->data[0] + (size_t)y * f->linesize[0],
                           (size_t)(m->w < f->width ? m->w : f->width));
                const uint32_t *p = (const uint32_t *)f->data[1];
                uint8_t *gpal = (uint8_t *)hle_ptr(m->va + SMK_PALETTE);
                int changed = !m->have_pal || memcmp(p, m->pal, sizeof m->pal);
                if (changed) {
                    memcpy(m->pal, p, sizeof m->pal);
                    for (int i = 0; i < 256; i++)
                        gpal[3 * i] = (uint8_t)(p[i] >> 16), gpal[3 * i + 1] = (uint8_t)(p[i] >> 8), gpal[3 * i + 2] = (uint8_t)p[i];
                    m->have_pal = 1;
                }
                MEM32(m->va + SMK_NEWPALETTE) = (uint32_t)changed;
                got_video = 1;
            }
        }
        av_packet_unref(m->pkt);
        if (got_video) return 1;
    }
}

/* #14 SmackOpen(name, flags, extrabuf) -> Smack* */
static void s_SmackOpen(void) {
    const char *name = A32(0) ? ASTR(0) : NULL;
    char host[1024];
    if (!name || !hle_host_path(name, host, sizeof host)) {
        fprintf(stderr, "[smack] SmackOpen(%s): not found\n", name ? name : "(handle)");
        RET(0, 3);
    }
    movie_t *m = (movie_t *)calloc(1, sizeof *m);
    if (!m || avformat_open_input(&m->fmt, JOYRIDE_OSPATH(host), NULL, NULL) < 0 || avformat_find_stream_info(m->fmt, NULL) < 0) {
        fprintf(stderr, "[smack] SmackOpen(%s): ffmpeg cannot read it\n", name);
        movie_free(m);
        RET(0, 3);
    }
    m->vidx = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    m->aidx = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    m->vdec = m->vidx >= 0 ? open_dec(m->fmt, m->vidx) : NULL;
    m->adec = m->aidx >= 0 ? open_dec(m->fmt, m->aidx) : NULL;
    m->pkt = av_packet_alloc();
    m->frame = av_frame_alloc();
    if (!m->vdec || !m->pkt || !m->frame) { movie_free(m); RET(0, 3); }
    m->w = m->vdec->width, m->h = m->vdec->height;
    m->pix = (uint8_t *)calloc((size_t)m->w * (size_t)m->h, 1);

    /* Frames and rate from the file's own header (SMK2/SMK4): ffmpeg's
     * stream has no frame count. Rate > 0: ms; < 0: 1/100 000 s; 0: 10 fps. */
    uint32_t frames = 0;
    int32_t rate = 0;
    FILE *f = fopen(host, "rb");
    if (f) {
        uint8_t hd[20];
        if (fread(hd, 1, 20, f) == 20) {
            frames = (uint32_t)(hd[12] | hd[13] << 8 | hd[14] << 16 | (uint32_t)hd[15] << 24);
            rate = (int32_t)(hd[16] | hd[17] << 8 | hd[18] << 16 | (uint32_t)hd[19] << 24);
        }
        fclose(f);
    }
    m->ns_per_frame = rate > 0 ? (uint64_t)rate * 1000000ull : rate < 0 ? (uint64_t)(-rate) * 10000ull : 100000000ull;

    m->va = hle_va(hle_alloc(SMK_SIZE));
    MEM32(m->va + SMK_VERSION) = 0x324B4D53u;                /* "SMK2" */
    MEM32(m->va + SMK_WIDTH) = (uint32_t)m->w;
    MEM32(m->va + SMK_HEIGHT) = (uint32_t)m->h;
    MEM32(m->va + SMK_FRAMES) = frames;
    MEM32(m->va + SMK_MSPERFRAME) = (uint32_t)(m->ns_per_frame / 1000000ull);
    movie_free(g_movie);
    g_movie = m;
    fprintf(stderr, "[smack] %s: %dx%d, %u frames, %.2f fps%s\n", name, m->w, m->h, frames,
            1e9 / (double)m->ns_per_frame, m->adec ? ", with sound" : "");
    RET(m->va, 3);
}

/* #23 SmackToBuffer(s, left, top, pitch, destheight, buf, flags) */
static void s_SmackToBuffer(void) {
    movie_t *m = movie_of(A32(0));
    if (m) m->left = A32(1), m->top = A32(2), m->pitch = A32(3), m->height = A32(4), m->buf = A32(5);
    TRACE("ToBuffer left %u top %u pitch %u height %u buf 0x%08X\n", A32(1), A32(2), A32(3), A32(4), A32(5));
    RET(0, 7);
}

/* #19 SmackDoFrame(s): decode the frame into the game's buffer */
static void s_SmackDoFrame(void) {
    movie_t *m = movie_of(A32(0));
    TRACE("DoFrame %u\n", m ? m->frame_no : 0);
    if (!m) RET(0, 1);
    if (!m->t0_ns) m->t0_ns = SDL_GetTicksNS();
    if (next_frame(m) && m->buf) {
        for (int y = 0; y < m->h && m->top + (uint32_t)y < m->height; y++)
            memcpy(hle_ptr(m->buf + (m->top + (uint32_t)y) * m->pitch + m->left), m->pix + (size_t)y * m->w, (size_t)m->w);
        m->rect_pending = 1;
    }
    MEM32(m->va + SMK_FRAMENUM) = m->frame_no;
    RET(0, 1);
}

/* #28 SmackToBufferRect(s, surface): the changed rectangles, one per call
 * (here: the whole frame once), in s->LastRect; 0 when there are no more */
static void s_SmackToBufferRect(void) {
    movie_t *m = movie_of(A32(0));
    if (!m || !m->rect_pending) RET(0, 2);
    m->rect_pending = 0;
    MEM32(m->va + SMK_LASTRECTX) = m->left;
    MEM32(m->va + SMK_LASTRECTY) = m->top;
    MEM32(m->va + SMK_LASTRECTW) = (uint32_t)m->w;
    MEM32(m->va + SMK_LASTRECTH) = (uint32_t)m->h;
    RET(1, 2);
}

/* #21 SmackNextFrame(s) */
static void s_SmackNextFrame(void) {
    movie_t *m = movie_of(A32(0));
    if (m) m->frame_no++;
    RET(0, 1);
}

/* #32 SmackWait(s): nonzero while the next frame is not yet due. The game
 * spins on it, so wait a little here, with the machine given up. */
static void s_SmackWait(void) {
    movie_t *m = movie_of(A32(0));
    if (!m || !m->t0_ns) RET(0, 1);
    uint64_t due = m->t0_ns + (uint64_t)m->frame_no * m->ns_per_frame, now = SDL_GetTicksNS();
    if (now >= due) RET(0, 1);
    uint64_t wait = due - now;
    hle_block_begin();
    SDL_DelayPrecise(wait < 2000000ull ? wait : 2000000ull);
    hle_block_end();
    RET(1, 1);
}

/* #18 SmackClose(s) */
static void s_SmackClose(void) {
    movie_t *m = movie_of(A32(0));
    TRACE("Close at frame %u\n", m ? m->frame_no : 0);
    if (m) { movie_free(m); g_movie = NULL; }
    RET(0, 1);
}

#else  /* no ffmpeg: the game skips the movie */

int joyride_movie_playing(void) { return 0; }
static void s_SmackOpen(void) {
    fprintf(stderr, "[smack] SmackOpen(%s): built without ffmpeg, movie skipped\n", A32(0) ? ASTR(0) : "(handle)");
    RET(0, 3);
}
static void s_SmackClose(void)        { RET(0, 1); }
static void s_SmackDoFrame(void)      { RET(0, 1); }
static void s_SmackNextFrame(void)    { RET(0, 1); }
static void s_SmackToBuffer(void)     { RET(0, 7); }
static void s_SmackToBufferRect(void) { RET(0, 2); }
static void s_SmackWait(void)         { RET(0, 1); }

#endif

/* Ordinals from the shipped SMACKW32.DLL's export table. */
const win32hle_shim joyride_smack[] = {
    { "smackw32.dll#14", s_SmackOpen },       { "smackw32.dll#18", s_SmackClose },
    { "smackw32.dll#19", s_SmackDoFrame },    { "smackw32.dll#21", s_SmackNextFrame },
    { "smackw32.dll#23", s_SmackToBuffer },   { "smackw32.dll#28", s_SmackToBufferRect },
    { "smackw32.dll#32", s_SmackWait },       { "smackw32.dll#33", s_SmackSoundUseMSS },
    { NULL, NULL },
};
