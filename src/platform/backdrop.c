/* joyride - a picture around the menus on screens that are not 4:3.
 *
 * The game's menus are 640 x 480 whatever the screen; on a wider one they
 * stand between black bars. A wide picture made to go around the menu
 * background (its middle left dark, where the menu covers it) is drawn
 * filling the window behind them instead, cropped about its middle to the
 * window's shape (screen_sdl3.c). Looked for in <data>/extracted/menu/:
 * the game's, <game>.png (gta1, london1969, london1961), made from its own
 * menus by tools/backdrops.py (the launcher runs it; a picture of the
 * player's own can take its place), else delta.png (SDL reads it) or
 * delta.jpg (read through ffmpeg, as the intro movie is, when joyride has
 * it). None: bars, as before.
 */
#ifndef JOYRIDE_OSPATH
#define JOYRIDE_OSPATH(p) (p)            /* Windows: compat/win/posix_win.h */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "joyride_hle.h"

#ifdef JOYRIDE_WITH_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#endif

static uint8_t *g_px;                    /* bytes B, G, R, A (the screen's textures), w * 4 a row */
static int g_w, g_h;

static int from_png(const char *path) {
    SDL_Surface *s = SDL_LoadPNG(JOYRIDE_OSPATH(path));
    if (!s) return 0;
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_BGRA32);
    SDL_DestroySurface(s);
    if (!c) return 0;
    g_px = malloc((size_t)c->w * c->h * 4);
    if (g_px) {
        for (int y = 0; y < c->h; y++) memcpy(g_px + (size_t)y * c->w * 4, (uint8_t *)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
        g_w = c->w, g_h = c->h;
    }
    SDL_DestroySurface(c);
    return g_px != NULL;
}

#ifdef JOYRIDE_WITH_FFMPEG
static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* one picture, decoded to planar YUV (JPEG's full range) or grey, made BGRA */
static int from_jpeg(const char *path) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, JOYRIDE_OSPATH(path), NULL, NULL) < 0) return 0;
    int ok = 0, si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    const AVCodec *dec = si >= 0 ? avcodec_find_decoder(fmt->streams[si]->codecpar->codec_id) : NULL;
    AVCodecContext *cx = dec ? avcodec_alloc_context3(dec) : NULL;
    AVPacket *pk = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    if (!cx || !pk || !fr || avcodec_parameters_to_context(cx, fmt->streams[si]->codecpar) < 0 || avcodec_open2(cx, dec, NULL) < 0)
        goto done;
    int got = 0;
    while (!got && av_read_frame(fmt, pk) >= 0) {
        if (pk->stream_index == si && avcodec_send_packet(cx, pk) >= 0) got = avcodec_receive_frame(cx, fr) >= 0;
        av_packet_unref(pk);
    }
    if (!got) {
        avcodec_send_packet(cx, NULL);
        got = avcodec_receive_frame(cx, fr) >= 0;
    }
    const AVPixFmtDescriptor *d = got ? av_pix_fmt_desc_get((enum AVPixelFormat)fr->format) : NULL;
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BE)) || d->comp[0].depth != 8 ||
        !(d->flags & AV_PIX_FMT_FLAG_PLANAR || d->nb_components == 1)) {
        fprintf(stderr, "[backdrop] %s: picture format not handled\n", path);
        goto done;
    }
    g_w = fr->width, g_h = fr->height;
    g_px = malloc((size_t)g_w * g_h * 4);
    if (!g_px) goto done;
    int grey = d->nb_components < 3, sx = d->log2_chroma_w, sy = d->log2_chroma_h;
    /* full range unless it says limited (JPEG is full) */
    int limited = fr->color_range == AVCOL_RANGE_MPEG && (enum AVPixelFormat)fr->format != AV_PIX_FMT_YUVJ420P &&
                  (enum AVPixelFormat)fr->format != AV_PIX_FMT_YUVJ422P && (enum AVPixelFormat)fr->format != AV_PIX_FMT_YUVJ444P;
    for (int y = 0; y < g_h; y++) {
        const uint8_t *py = fr->data[0] + (size_t)y * fr->linesize[0];
        const uint8_t *pu = grey ? NULL : fr->data[1] + (size_t)(y >> sy) * fr->linesize[1];
        const uint8_t *pv = grey ? NULL : fr->data[2] + (size_t)(y >> sy) * fr->linesize[2];
        uint8_t *o = g_px + (size_t)y * g_w * 4;
        for (int x = 0; x < g_w; x++, o += 4) {
            int Y = py[x], U = grey ? 128 : pu[x >> sx], V = grey ? 128 : pv[x >> sx];
            if (limited) Y = (Y - 16) * 255 / 219, U = 128 + (U - 128) * 255 / 224, V = 128 + (V - 128) * 255 / 224;
            o[2] = clamp8(Y + (int)(1.402f * (V - 128)));
            o[1] = clamp8(Y - (int)(0.344136f * (U - 128) + 0.714136f * (V - 128)));
            o[0] = clamp8(Y + (int)(1.772f * (U - 128)));
            o[3] = 255;
        }
    }
    ok = 1;
done:
    av_frame_free(&fr);
    av_packet_free(&pk);
    avcodec_free_context(&cx);
    avformat_close_input(&fmt);
    return ok;
}
#endif

void joyride_backdrop_init(const char *data_dir) {
    char path[4096];
    /* this game's (JOYRIDE_PROFILE: London's, main.c; none: GTA's), made by
     * tools/backdrops.py or put there by the player; else delta.png or
     * delta.jpg */
    const char *prof = getenv("JOYRIDE_PROFILE");
    int safe = prof && *prof && !strchr(prof, '/') && strcmp(prof, ".") && strcmp(prof, "..");
    snprintf(path, sizeof path, "%s/extracted/menu/%.64s.png", data_dir, safe ? prof : "gta1");
    int ok = from_png(path);
    if (!ok) {
        snprintf(path, sizeof path, "%s/extracted/menu/delta.png", data_dir);
        ok = from_png(path);
    }
#ifdef JOYRIDE_WITH_FFMPEG
    if (!ok) {
        snprintf(path, sizeof path, "%s/extracted/menu/delta.jpg", data_dir);
        FILE *f = fopen(path, "rb");
        if (f) fclose(f), ok = from_jpeg(path);
    }
#endif
    if (ok) fprintf(stderr, "[backdrop] %s, %dx%d\n", path, g_w, g_h);
}

int joyride_backdrop(const uint8_t **bgra, int *w, int *h) {
    if (!g_px) return 0;
    *bgra = g_px, *w = g_w, *h = g_h;
    return 1;
}
