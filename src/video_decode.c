/* Nordstjernen — inline video decoding: MPEG-1 and libav-backed containers.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "video_decode.h"

#include <stdio.h>
#include <string.h>

#include "pl_mpeg.h"

#ifdef NS_HAVE_LIBAV
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#endif

#ifdef NS_HAVE_LIBAV
typedef struct {
    AVFormatContext   *fmt;
    AVIOContext       *avio;
    AVCodecContext    *vdec;
    struct SwsContext *sws;
    AVPacket          *pkt;
    AVFrame           *frame;
    guint8            *data;
    gsize              len;
    gsize              cap;
    gsize              pos;
    int                vstream;
    AVRational         time_base;
    gboolean           draining;
} ns_lav;
#endif

struct ns_video_player {
    plm_t      *plm;
#ifdef NS_HAVE_LIBAV
    ns_lav     *lav;
#endif
    int         width;
    int         height;
    int         has_audio;
    double      duration;
    double      demuxed_end;
    ns_texture *cur_tex;
    double      cur_time;
    ns_texture *pending_tex;
    double      pending_time;
};

static gboolean
bytes_are_mpeg1(const guint8 *bytes, gsize len)
{
    return bytes && len >= 4 && bytes[0] == 0x00 && bytes[1] == 0x00 &&
           bytes[2] == 0x01 && (bytes[3] == 0xBA || bytes[3] == 0xB3);
}

#ifdef NS_HAVE_LIBAV
static gboolean
bytes_are_matroska(const guint8 *bytes, gsize len)
{
    return bytes && len >= 4 && bytes[0] == 0x1A && bytes[1] == 0x45 &&
           bytes[2] == 0xDF && bytes[3] == 0xA3;
}

static gboolean
bytes_are_isobmff(const guint8 *bytes, gsize len)
{
    return bytes && len >= 12 && memcmp(bytes + 4, "ftyp", 4) == 0;
}
#endif

#ifdef NS_HAVE_LIBAV
static int
ns_lav_read(void *opaque, uint8_t *buf, int sz)
{
    ns_lav *L = opaque;
    gsize avail = L->len - L->pos;
    if (avail == 0) return AVERROR_EOF;
    if ((gsize)sz > avail) sz = (int)avail;
    memcpy(buf, L->data + L->pos, (gsize)sz);
    L->pos += (gsize)sz;
    return sz;
}

static int64_t
ns_lav_seek(void *opaque, int64_t off, int whence)
{
    ns_lav *L = opaque;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return (int64_t)L->len;
    int64_t base = (whence == SEEK_CUR) ? (int64_t)L->pos
                 : (whence == SEEK_END) ? (int64_t)L->len : 0;
    int64_t np = base + off;
    if (np < 0 || (gsize)np > L->len) return -1;
    L->pos = (gsize)np;
    return np;
}

static void
ns_lav_free(ns_lav *L)
{
    if (!L) return;
    if (L->sws) sws_freeContext(L->sws);
    if (L->frame) av_frame_free(&L->frame);
    if (L->pkt) av_packet_free(&L->pkt);
    if (L->vdec) avcodec_free_context(&L->vdec);
    if (L->fmt) avformat_close_input(&L->fmt);
    if (L->avio) { av_freep(&L->avio->buffer); avio_context_free(&L->avio); }
    g_free(L->data);
    g_free(L);
}

static ns_lav *
ns_lav_open(const guint8 *bytes, gsize len, int *out_w, int *out_h,
            double *out_dur, double *out_end, int *out_has_audio)
{
    av_log_set_level(AV_LOG_QUIET);

    ns_lav *L = g_new0(ns_lav, 1);
    L->data = g_try_malloc(len);
    if (!L->data) { g_free(L); return NULL; }
    memcpy(L->data, bytes, len);
    L->len = len;
    L->cap = len;

    size_t bufsz = 32768;
    unsigned char *iobuf = av_malloc(bufsz);
    if (!iobuf) { ns_lav_free(L); return NULL; }
    L->avio = avio_alloc_context(iobuf, (int)bufsz, 0, L,
                                 ns_lav_read, NULL, ns_lav_seek);
    if (!L->avio) { av_free(iobuf); ns_lav_free(L); return NULL; }

    L->fmt = avformat_alloc_context();
    if (!L->fmt) { ns_lav_free(L); return NULL; }
    L->fmt->pb = L->avio;
    if (avformat_open_input(&L->fmt, NULL, NULL, NULL) < 0) {
        L->fmt = NULL;
        ns_lav_free(L);
        return NULL;
    }
    if (avformat_find_stream_info(L->fmt, NULL) < 0) { ns_lav_free(L); return NULL; }

    const AVCodec *dec = NULL;
    L->vstream = av_find_best_stream(L->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (L->vstream < 0 || !dec) { ns_lav_free(L); return NULL; }

    AVStream *vs = L->fmt->streams[L->vstream];
    enum AVCodecID id = vs->codecpar->codec_id;
    if (id != AV_CODEC_ID_VP9 && id != AV_CODEC_ID_VP8 &&
        id != AV_CODEC_ID_H264 && id != AV_CODEC_ID_HEVC &&
        id != AV_CODEC_ID_AV1 && id != AV_CODEC_ID_MPEG4 &&
        id != AV_CODEC_ID_THEORA) {
        ns_lav_free(L);
        return NULL;
    }

    L->vdec = avcodec_alloc_context3(dec);
    if (!L->vdec) { ns_lav_free(L); return NULL; }
    if (avcodec_parameters_to_context(L->vdec, vs->codecpar) < 0 ||
        avcodec_open2(L->vdec, dec, NULL) < 0) {
        ns_lav_free(L);
        return NULL;
    }

    int w = vs->codecpar->width, h = vs->codecpar->height;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { ns_lav_free(L); return NULL; }

    L->time_base = vs->time_base;
    L->pkt = av_packet_alloc();
    L->frame = av_frame_alloc();
    if (!L->pkt || !L->frame) { ns_lav_free(L); return NULL; }

    double dur = 0.0;
    if (L->fmt->duration > 0)
        dur = (double)L->fmt->duration / (double)AV_TIME_BASE;
    else if (vs->duration > 0)
        dur = (double)vs->duration * av_q2d(vs->time_base);

    double last_pts = 0.0;
    while (av_read_frame(L->fmt, L->pkt) >= 0) {
        if (L->pkt->stream_index == L->vstream &&
            L->pkt->pts != AV_NOPTS_VALUE) {
            double t = (double)L->pkt->pts * av_q2d(vs->time_base);
            if (t > last_pts) last_pts = t;
        }
        av_packet_unref(L->pkt);
    }
    av_seek_frame(L->fmt, L->vstream, 0, AVSEEK_FLAG_BACKWARD);

    *out_w = w;
    *out_h = h;
    *out_dur = dur;
    *out_end = last_pts;
    *out_has_audio =
        av_find_best_stream(L->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) >= 0;
    return L;
}

static double
ns_lav_probe_chunk_end(const guint8 *init, gsize init_len,
                       const guint8 *chunk, gsize chunk_len)
{
    if (!chunk || !chunk_len) return 0.0;
    gsize total = init_len + chunk_len;
    guint8 *joined = g_try_malloc(total);
    if (!joined) return 0.0;
    if (init_len) memcpy(joined, init, init_len);
    memcpy(joined + init_len, chunk, chunk_len);

    ns_lav probe = {0};
    probe.data = joined;
    probe.len = total;

    double end = 0.0;
    unsigned char *iobuf = av_malloc(32768);
    if (!iobuf) { g_free(joined); return 0.0; }
    AVIOContext *avio = avio_alloc_context(iobuf, 32768, 0, &probe,
                                           ns_lav_read, NULL, ns_lav_seek);
    if (!avio) { av_free(iobuf); g_free(joined); return 0.0; }
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        g_free(joined);
        return 0.0;
    }
    fmt->pb = avio;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        g_free(joined);
        return 0.0;
    }
    AVPacket *pkt = av_packet_alloc();
    if (pkt) {
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->pts != AV_NOPTS_VALUE &&
                pkt->stream_index >= 0 &&
                pkt->stream_index < (int)fmt->nb_streams) {
                AVRational tb = fmt->streams[pkt->stream_index]->time_base;
                double t = ((double)pkt->pts +
                            (pkt->duration > 0 ? (double)pkt->duration : 0.0)) *
                           av_q2d(tb);
                if (t > end) end = t;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    avformat_close_input(&fmt);
    av_freep(&avio->buffer);
    avio_context_free(&avio);
    g_free(joined);
    return end;
}

static void
ns_lav_rewind(ns_lav *L)
{
    av_seek_frame(L->fmt, L->vstream, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(L->vdec);
    L->draining = FALSE;
}

static gboolean
ns_lav_seek_to(ns_lav *L, double seconds)
{
    int64_t ts = (int64_t)(seconds / av_q2d(L->time_base));
    if (av_seek_frame(L->fmt, L->vstream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return FALSE;
    avcodec_flush_buffers(L->vdec);
    L->draining = FALSE;
    return TRUE;
}

static ns_texture *
ns_lav_frame_to_texture(ns_lav *L, AVFrame *frame, int w, int h)
{
    gsize stride = (gsize)w * 4;
    gsize buf_len = stride * (gsize)h;
    guint8 *bgra = g_try_malloc(buf_len);
    if (!bgra) return NULL;

    L->sws = sws_getCachedContext(L->sws, frame->width, frame->height,
                                  frame->format, w, h, AV_PIX_FMT_BGRA,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!L->sws) { g_free(bgra); return NULL; }

    uint8_t *dst[4] = { bgra, NULL, NULL, NULL };
    int dst_stride[4] = { (int)stride, 0, 0, 0 };
    sws_scale(L->sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, frame->height, dst, dst_stride);

    GBytes *gb = g_bytes_new_take(bgra, buf_len);
    ns_texture *tex = ns_texture_new(w, h, NS_TEXTURE_BGRA_PREMULTIPLIED,
                                     gb, stride);
    g_bytes_unref(gb);
    return tex;
}

static gboolean
ns_lav_next(ns_video_player *player, ns_texture **out_tex, double *out_pts)
{
    ns_lav *L = player->lav;
    for (;;) {
        int r = avcodec_receive_frame(L->vdec, L->frame);
        if (r == 0) {
            double t = (L->frame->pts == AV_NOPTS_VALUE)
                ? player->pending_time
                : (double)L->frame->pts * av_q2d(L->time_base);
            *out_tex = ns_lav_frame_to_texture(L, L->frame,
                                               player->width, player->height);
            *out_pts = t;
            av_frame_unref(L->frame);
            return TRUE;
        }
        if (r == AVERROR_EOF || (r < 0 && r != AVERROR(EAGAIN)))
            return FALSE;

        int rr;
        while ((rr = av_read_frame(L->fmt, L->pkt)) >= 0 &&
               L->pkt->stream_index != L->vstream)
            av_packet_unref(L->pkt);

        if (rr >= 0) {
            avcodec_send_packet(L->vdec, L->pkt);
            av_packet_unref(L->pkt);
        } else if (!L->draining) {
            avcodec_send_packet(L->vdec, NULL);
            L->draining = TRUE;
        } else {
            return FALSE;
        }
    }
}
#endif /* NS_HAVE_LIBAV */

ns_video_player *
ns_video_player_new(const guint8 *bytes, gsize len)
{
    if (!bytes || len < 8) return NULL;

    if (bytes_are_mpeg1(bytes, len)) {
        guint8 *copy = g_try_malloc(len);
        if (!copy) return NULL;
        memcpy(copy, bytes, len);
        plm_t *plm = plm_create_with_memory(copy, len, TRUE);
        if (!plm) { g_free(copy); return NULL; }

        plm_set_audio_enabled(plm, FALSE);
        plm_set_loop(plm, FALSE);

        int w = plm_get_width(plm);
        int h = plm_get_height(plm);
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
            plm_destroy(plm);
            return NULL;
        }

        ns_video_player *player = g_new0(ns_video_player, 1);
        player->plm = plm;
        player->width = w;
        player->height = h;
        player->has_audio = plm_get_num_audio_streams(plm) > 0;
        player->duration = plm_get_duration(plm);
        player->demuxed_end = player->duration;
        player->cur_time = -1.0;
        player->pending_time = -1.0;
        return player;
    }

#ifdef NS_HAVE_LIBAV
    if (bytes_are_matroska(bytes, len) || bytes_are_isobmff(bytes, len)) {
        int w = 0, h = 0, has_audio = 0;
        double dur = 0.0;
        double demuxed_end = 0.0;
        ns_lav *L = ns_lav_open(bytes, len, &w, &h, &dur, &demuxed_end,
                                &has_audio);
        if (!L) return NULL;

        ns_video_player *player = g_new0(ns_video_player, 1);
        player->lav = L;
        player->width = w;
        player->height = h;
        player->has_audio = has_audio;
        player->duration = dur;
        player->demuxed_end = demuxed_end;
        player->cur_time = -1.0;
        player->pending_time = -1.0;
        return player;
    }
#endif

    return NULL;
}

void
ns_video_player_free(ns_video_player *player)
{
    if (!player) return;
    ns_texture_unref(player->cur_tex);
    ns_texture_unref(player->pending_tex);
    if (player->plm) plm_destroy(player->plm);
#ifdef NS_HAVE_LIBAV
    if (player->lav) ns_lav_free(player->lav);
#endif
    g_free(player);
}

gboolean
ns_video_player_has_audio(const ns_video_player *player)
{
    return player ? player->has_audio != 0 : FALSE;
}

int
ns_video_player_width(const ns_video_player *player)
{
    return player ? player->width : 0;
}

int
ns_video_player_height(const ns_video_player *player)
{
    return player ? player->height : 0;
}

double
ns_video_player_duration(const ns_video_player *player)
{
    return player ? player->duration : 0.0;
}

double
ns_video_player_buffered_end(const ns_video_player *player)
{
    return player ? player->demuxed_end : 0.0;
}

void
ns_video_player_note_end(ns_video_player *player, double end)
{
    if (!player || end <= 0.0) return;
    if (end > player->demuxed_end) player->demuxed_end = end;
    if (end > player->duration) player->duration = end;
}

double
ns_video_probe_chunk_end(const guint8 *init, gsize init_len,
                         const guint8 *chunk, gsize chunk_len)
{
#ifdef NS_HAVE_LIBAV
    return ns_lav_probe_chunk_end(init, init_len, chunk, chunk_len);
#else
    (void)init; (void)init_len; (void)chunk; (void)chunk_len;
    return 0.0;
#endif
}

gboolean
ns_video_codec_available(const char *codec)
{
#ifdef NS_HAVE_LIBAV
    if (!codec || !*codec) return FALSE;
    enum AVCodecID id = AV_CODEC_ID_NONE;
    if (g_str_has_prefix(codec, "avc1") || g_str_has_prefix(codec, "avc3"))
        id = AV_CODEC_ID_H264;
    else if (g_str_has_prefix(codec, "hvc1") || g_str_has_prefix(codec, "hev1"))
        id = AV_CODEC_ID_HEVC;
    else if (g_str_has_prefix(codec, "av01"))
        id = AV_CODEC_ID_AV1;
    else if (g_str_has_prefix(codec, "vp09") || g_str_has_prefix(codec, "vp9"))
        id = AV_CODEC_ID_VP9;
    else if (g_str_has_prefix(codec, "vp08") || g_str_has_prefix(codec, "vp8"))
        id = AV_CODEC_ID_VP8;
    else if (g_str_has_prefix(codec, "mp4a.40"))
        id = AV_CODEC_ID_AAC;
    else if (strstr(codec, "opus"))
        id = AV_CODEC_ID_OPUS;
    else if (strstr(codec, "vorbis"))
        id = AV_CODEC_ID_VORBIS;
    if (id == AV_CODEC_ID_NONE) return FALSE;
    return avcodec_find_decoder(id) != NULL;
#else
    (void)codec;
    return FALSE;
#endif
}

gboolean
ns_video_player_extend(ns_video_player *player, const guint8 *bytes, gsize len)
{
#ifdef NS_HAVE_LIBAV
    if (!player || !player->lav || !bytes) return FALSE;
    ns_lav *L = player->lav;
    if (len < L->len) return FALSE;
    if (len == L->len) return TRUE;
    gsize head = MIN(L->len, (gsize)4096);
    if (memcmp(bytes, L->data, head) != 0) return FALSE;
    if (L->len > 4096 &&
        memcmp(bytes + L->len - 4096, L->data + L->len - 4096, 4096) != 0)
        return FALSE;

    if (len > L->cap) {
        gsize cap = L->cap ? L->cap : len;
        while (cap < len) {
            if (cap > G_MAXSIZE / 2) { cap = len; break; }
            cap *= 2;
        }
        guint8 *grown = g_try_realloc(L->data, cap);
        if (!grown) return FALSE;
        L->data = grown;
        L->cap = cap;
    }
    memcpy(L->data + L->len, bytes + L->len, len - L->len);
    L->len = len;
    L->avio->eof_reached = 0;
    L->avio->error = 0;
    if (L->draining) {
        avcodec_flush_buffers(L->vdec);
        L->draining = FALSE;
    }
    return TRUE;
#else
    (void)player; (void)bytes; (void)len;
    return FALSE;
#endif
}

static ns_texture *
ns_plm_frame_to_texture(ns_video_player *player, plm_frame_t *frame)
{
    int w = player->width, h = player->height;
    gsize stride = (gsize)w * 4;
    gsize buf_len = stride * (gsize)h;
    guint8 *bgra = g_try_malloc(buf_len);
    if (!bgra)
        return NULL;
    memset(bgra, 0xFF, buf_len);
    plm_frame_to_bgra(frame, bgra, (int)stride);

    GBytes *bytes = g_bytes_new_take(bgra, buf_len);
    ns_texture *tex = ns_texture_new(w, h, NS_TEXTURE_BGRA_PREMULTIPLIED,
                                     bytes, stride);
    g_bytes_unref(bytes);
    return tex;
}

static void
ns_video_backend_rewind(ns_video_player *player)
{
    if (player->plm) plm_rewind(player->plm);
#ifdef NS_HAVE_LIBAV
    else if (player->lav) ns_lav_rewind(player->lav);
#endif
}

static gboolean
ns_video_backend_next(ns_video_player *player, ns_texture **out_tex,
                      double *out_pts, gboolean *out_eof)
{
    *out_eof = FALSE;
    if (player->plm) {
        plm_frame_t *frame = plm_decode_video(player->plm);
        if (!frame) {
            *out_eof = plm_has_ended(player->plm);
            return FALSE;
        }
        *out_tex = ns_plm_frame_to_texture(player, frame);
        *out_pts = frame->time;
        return TRUE;
    }
#ifdef NS_HAVE_LIBAV
    if (player->lav) {
        if (ns_lav_next(player, out_tex, out_pts)) return TRUE;
        *out_eof = TRUE;
        return FALSE;
    }
#endif
    *out_eof = TRUE;
    return FALSE;
}

ns_texture *
ns_video_player_frame_at(ns_video_player *player, double seconds,
                         gboolean loop, gboolean *out_ended)
{
    if (out_ended) *out_ended = FALSE;
    if (!player) return NULL;
    if (!player->plm
#ifdef NS_HAVE_LIBAV
        && !player->lav
#endif
       ) return NULL;
    if (seconds < 0) seconds = 0;

    gboolean backward = seconds + 1e-4 < player->cur_time;
    gboolean far_forward = player->cur_time >= 0.0 &&
                           seconds > player->cur_time + 2.0;
    if (backward || far_forward) {
        gboolean sought = FALSE;
#ifdef NS_HAVE_LIBAV
        if (player->lav)
            sought = ns_lav_seek_to(player->lav, seconds);
#endif
        if (!sought) {
            if (!backward) sought = TRUE;
            else ns_video_backend_rewind(player);
        }
        if (backward || !sought || far_forward) {
            player->cur_time = -1.0;
            ns_texture_clear(&player->pending_tex);
            player->pending_time = -1.0;
        }
    }

    gboolean changed = FALSE;
    for (;;) {
        if (player->pending_time >= 0.0) {
            if (player->pending_time <= seconds || player->cur_time < 0.0) {
                ns_texture_unref(player->cur_tex);
                player->cur_tex = player->pending_tex;
                player->cur_time = player->pending_time;
                player->pending_tex = NULL;
                player->pending_time = -1.0;
                changed = TRUE;
                continue;
            }
            break;
        }

        ns_texture *tex = NULL;
        double pts = 0.0;
        gboolean eof = FALSE;
        if (!ns_video_backend_next(player, &tex, &pts, &eof)) {
            if (!loop && eof && out_ended)
                *out_ended = TRUE;
            break;
        }
        ns_texture_unref(player->pending_tex);
        player->pending_tex = tex;
        player->pending_time = pts < 0.0 ? 0.0 : pts;
    }

    return changed ? player->cur_tex : NULL;
}
