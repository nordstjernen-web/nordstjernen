/* Nordstjernen — minimal WebM/Matroska demuxer (read-only, video + audio frames).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "webm.h"

#include <string.h>

struct nd_webm {
    const guint8 *data;
    gsize         len;
    gsize         segment_start;
    gsize         segment_end;
    gsize         cluster_pos;
    gsize         cluster_end;
    gint64        cluster_timecode;
    gint64        timecode_scale_ns;
    int           video_track_num;
    int           audio_track_num;
    nd_webm_track video;
    nd_webm_track audio;
    gboolean      have_video;
    gboolean      have_audio;
};

static gboolean
read_vint(const guint8 *p, gsize avail, guint64 *out_value,
          gsize *out_consumed, gboolean keep_marker)
{
    if (avail == 0) return FALSE;
    guint8 b = p[0];
    if (b == 0) return FALSE;
    int len = 1;
    guint8 mask = 0x80;
    while (!(b & mask) && len < 8) { len++; mask >>= 1; }
    if (len > 8 || (gsize)len > avail) return FALSE;
    guint64 v = keep_marker ? b : (guint64)(b & (mask - 1));
    for (int i = 1; i < len; i++) v = (v << 8) | p[i];
    *out_value = v;
    *out_consumed = (gsize)len;
    return TRUE;
}

static guint64
read_uint(const guint8 *p, gsize len)
{
    if (len > 8) len = 8;
    guint64 v = 0;
    for (gsize i = 0; i < len; i++) v = (v << 8) | p[i];
    return v;
}

static double
read_float(const guint8 *p, gsize len)
{
    if (len == 4) {
        guint32 u = (guint32)read_uint(p, 4);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    if (len == 8) {
        guint64 u = read_uint(p, 8);
        double f;
        memcpy(&f, &u, 8);
        return f;
    }
    return 0.0;
}

static gboolean
next_element(const guint8 *buf, gsize buf_len, gsize *pos,
             guint32 *out_id, const guint8 **out_data, gsize *out_size)
{
    if (*pos >= buf_len) return FALSE;
    guint64 id_v;
    gsize id_consumed = 0, size_consumed = 0;
    if (!read_vint(buf + *pos, buf_len - *pos, &id_v, &id_consumed, TRUE))
        return FALSE;
    *pos += id_consumed;
    if (*pos >= buf_len) return FALSE;
    guint64 size;
    if (!read_vint(buf + *pos, buf_len - *pos, &size, &size_consumed, FALSE))
        return FALSE;
    *pos += size_consumed;
    if (*pos > buf_len || size > (guint64)(buf_len - *pos)) return FALSE;
    *out_id   = (guint32)id_v;
    *out_data = buf + *pos;
    *out_size = (gsize)size;
    return TRUE;
}

#define ID_EBML            0x1A45DFA3u
#define ID_SEGMENT         0x18538067u
#define ID_INFO            0x1549A966u
#define ID_TIMECODE_SCALE  0x2AD7B1u
#define ID_TRACKS          0x1654AE6Bu
#define ID_TRACK_ENTRY     0xAEu
#define ID_TRACK_NUMBER    0xD7u
#define ID_TRACK_TYPE      0x83u
#define ID_CODEC_ID        0x86u
#define ID_CODEC_PRIVATE   0x63A2u
#define ID_VIDEO           0xE0u
#define ID_AUDIO           0xE1u
#define ID_PIXEL_WIDTH     0xB0u
#define ID_PIXEL_HEIGHT    0xBAu
#define ID_SAMPLING_FREQ   0xB5u
#define ID_CHANNELS        0x9Fu
#define ID_CLUSTER         0x1F43B675u
#define ID_TIMECODE        0xE7u
#define ID_SIMPLE_BLOCK    0xA3u
#define ID_BLOCK_GROUP     0xA0u
#define ID_BLOCK           0xA1u

static void
parse_video(const guint8 *p, gsize len, nd_webm_track *t)
{
    gsize pos = 0;
    guint32 id;
    const guint8 *d;
    gsize sz;
    while (next_element(p, len, &pos, &id, &d, &sz)) {
        if (id == ID_PIXEL_WIDTH)  t->width  = (int)read_uint(d, sz);
        if (id == ID_PIXEL_HEIGHT) t->height = (int)read_uint(d, sz);
        pos += sz;
    }
}

static void
parse_audio(const guint8 *p, gsize len, nd_webm_track *t)
{
    gsize pos = 0;
    guint32 id;
    const guint8 *d;
    gsize sz;
    while (next_element(p, len, &pos, &id, &d, &sz)) {
        if (id == ID_SAMPLING_FREQ) t->sample_rate = (int)read_float(d, sz);
        if (id == ID_CHANNELS)      t->channels    = (int)read_uint(d, sz);
        pos += sz;
    }
}

static void
parse_track_entry(nd_webm *w, const guint8 *p, gsize len)
{
    nd_webm_track t = {0};
    int track_num = 0;
    int track_type = 0;
    gsize pos = 0;
    guint32 id;
    const guint8 *d;
    gsize sz;
    while (next_element(p, len, &pos, &id, &d, &sz)) {
        if (id == ID_TRACK_NUMBER) track_num  = (int)read_uint(d, sz);
        if (id == ID_TRACK_TYPE)   track_type = (int)read_uint(d, sz);
        if (id == ID_CODEC_ID) {
            g_free(t.codec_id);
            t.codec_id = g_strndup((const char *)d, sz);
        }
        if (id == ID_CODEC_PRIVATE) {
            g_free(t.codec_private);
            t.codec_private = g_memdup2(d, sz);
            t.codec_private_len = sz;
        }
        if (id == ID_VIDEO) parse_video(d, sz, &t);
        if (id == ID_AUDIO) parse_audio(d, sz, &t);
        pos += sz;
    }
    if (track_type == 1 && !w->have_video) {
        w->video_track_num = track_num;
        w->video = t;
        w->have_video = TRUE;
    } else if (track_type == 2 && !w->have_audio) {
        w->audio_track_num = track_num;
        w->audio = t;
        w->have_audio = TRUE;
    } else {
        g_free(t.codec_id);
        g_free(t.codec_private);
    }
}

static void
parse_tracks(nd_webm *w, const guint8 *p, gsize len)
{
    gsize pos = 0;
    guint32 id;
    const guint8 *d;
    gsize sz;
    while (next_element(p, len, &pos, &id, &d, &sz)) {
        if (id == ID_TRACK_ENTRY) parse_track_entry(w, d, sz);
        pos += sz;
    }
}

static void
parse_info(nd_webm *w, const guint8 *p, gsize len)
{
    gsize pos = 0;
    guint32 id;
    const guint8 *d;
    gsize sz;
    while (next_element(p, len, &pos, &id, &d, &sz)) {
        if (id == ID_TIMECODE_SCALE) {
            guint64 v = read_uint(d, sz);
            if (v > 0) w->timecode_scale_ns = (gint64)v;
        }
        pos += sz;
    }
}

nd_webm *
nd_webm_open(const guint8 *body, gsize len)
{
    if (!body || len < 4) return NULL;
    nd_webm *w = g_new0(nd_webm, 1);
    w->data = body;
    w->len = len;
    w->timecode_scale_ns = 1000000;

    gsize pos = 0;
    guint32 id;
    const guint8 *d;
    gsize sz;
    while (next_element(body, len, &pos, &id, &d, &sz)) {
        if (id == ID_SEGMENT) {
            w->segment_start = pos;
            w->segment_end = pos + sz;
            break;
        }
        pos += sz;
    }
    if (w->segment_end == 0) goto fail;

    gsize seg_pos = w->segment_start;
    while (seg_pos < w->segment_end) {
        if (!next_element(body, w->segment_end, &seg_pos, &id, &d, &sz)) break;
        if (id == ID_INFO)    parse_info(w, d, sz);
        if (id == ID_TRACKS)  parse_tracks(w, d, sz);
        if (id == ID_CLUSTER) {
            w->cluster_pos = seg_pos;
            w->cluster_end = seg_pos + sz;
            break;
        }
        seg_pos += sz;
    }
    if (!(w->have_video || w->have_audio) || w->cluster_pos == 0) goto fail;
    return w;
fail:
    nd_webm_close(w);
    return NULL;
}

void
nd_webm_close(nd_webm *w)
{
    if (!w) return;
    g_free(w->video.codec_id);
    g_free(w->video.codec_private);
    g_free(w->audio.codec_id);
    g_free(w->audio.codec_private);
    g_free(w);
}

const nd_webm_track *
nd_webm_video_track(const nd_webm *w)
{
    return w && w->have_video ? &w->video : NULL;
}

const nd_webm_track *
nd_webm_audio_track(const nd_webm *w)
{
    return w && w->have_audio ? &w->audio : NULL;
}

static gboolean
parse_block_header(const guint8 *p, gsize len, int target_track,
                   const guint8 **out_payload, gsize *out_payload_len,
                   gint64 *out_rel_tc, gboolean *out_keyframe)
{
    if (len < 4) return FALSE;
    guint64 tn;
    gsize tn_consumed;
    if (!read_vint(p, len, &tn, &tn_consumed, FALSE)) return FALSE;
    if ((int)tn != target_track) return FALSE;
    if (tn_consumed + 3 > len) return FALSE;
    gint16 rel = (gint16)((p[tn_consumed] << 8) | p[tn_consumed + 1]);
    guint8 flags = p[tn_consumed + 2];
    *out_rel_tc = rel;
    *out_keyframe = (flags & 0x80) != 0;
    int lacing = (flags >> 1) & 0x03;
    if (lacing != 0) return FALSE;
    *out_payload = p + tn_consumed + 3;
    *out_payload_len = len - (tn_consumed + 3);
    return TRUE;
}

static const guint8 *
unwrap_block_group(const guint8 *p, gsize sz, gsize *out_len)
{
    gsize pos = 0;
    guint32 id;
    const guint8 *d;
    gsize esz;
    while (next_element(p, sz, &pos, &id, &d, &esz)) {
        if (id == ID_BLOCK) { *out_len = esz; return d; }
        pos += esz;
    }
    return NULL;
}

static gboolean
nd_webm_next_track_frame(nd_webm *w, int track_num, nd_webm_frame *out)
{
    if (!w || !out || track_num <= 0) return FALSE;
    while (w->cluster_pos < w->segment_end) {
        guint32 id;
        const guint8 *d;
        gsize sz;
        while (next_element(w->data, w->cluster_end, &w->cluster_pos,
                            &id, &d, &sz)) {
            if (id == ID_TIMECODE) {
                w->cluster_timecode = (gint64)read_uint(d, sz);
            } else if (id == ID_SIMPLE_BLOCK || id == ID_BLOCK_GROUP) {
                const guint8 *bp = d;
                gsize bsz = sz;
                if (id == ID_BLOCK_GROUP) {
                    bp = unwrap_block_group(d, sz, &bsz);
                    if (!bp) { w->cluster_pos += sz; continue; }
                }
                const guint8 *payload;
                gsize payload_len;
                gint64 rel_tc;
                gboolean kf;
                if (parse_block_header(bp, bsz, track_num,
                                       &payload, &payload_len, &rel_tc, &kf)) {
                    out->data = payload;
                    out->len = payload_len;
                    out->timecode_us = (w->cluster_timecode + rel_tc) *
                                       w->timecode_scale_ns / 1000;
                    out->keyframe = kf;
                    w->cluster_pos += sz;
                    return TRUE;
                }
            }
            w->cluster_pos += sz;
        }
        if (w->cluster_end >= w->segment_end) break;
        gsize seg_pos = w->cluster_end;
        gboolean advanced = FALSE;
        while (next_element(w->data, w->segment_end, &seg_pos, &id, &d, &sz)) {
            if (id == ID_CLUSTER) {
                w->cluster_pos = seg_pos;
                w->cluster_end = seg_pos + sz;
                w->cluster_timecode = 0;
                advanced = TRUE;
                break;
            }
            seg_pos += sz;
        }
        if (!advanced) break;
    }
    return FALSE;
}

gboolean
nd_webm_next_video_frame(nd_webm *w, nd_webm_frame *out)
{
    if (!w || !w->have_video) return FALSE;
    return nd_webm_next_track_frame(w, w->video_track_num, out);
}

gboolean
nd_webm_next_audio_frame(nd_webm *w, nd_webm_frame *out)
{
    if (!w || !w->have_audio) return FALSE;
    return nd_webm_next_track_frame(w, w->audio_track_num, out);
}

void
nd_webm_seek_start(nd_webm *w)
{
    if (!w) return;
    w->cluster_pos = 0;
    w->cluster_end = 0;
    w->cluster_timecode = 0;
    gsize seg_pos = w->segment_start;
    guint32 id;
    const guint8 *d;
    gsize sz;
    while (next_element(w->data, w->segment_end, &seg_pos, &id, &d, &sz)) {
        if (id == ID_CLUSTER) {
            w->cluster_pos = seg_pos;
            w->cluster_end = seg_pos + sz;
            return;
        }
        seg_pos += sz;
    }
}
