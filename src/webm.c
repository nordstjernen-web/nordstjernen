/* Nordstjernen — minimal WebM/Matroska demuxer (read-only, video frames).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
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
    nd_webm_track video;
    gboolean      have_video;
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

static gboolean
read_size(const guint8 *p, gsize avail, guint64 *out, gsize *consumed)
{
    return read_vint(p, avail, out, consumed, FALSE);
}

static gboolean
read_id(const guint8 *p, gsize avail, guint32 *out_id, gsize *consumed)
{
    guint64 v;
    if (!read_vint(p, avail, &v, consumed, TRUE)) return FALSE;
    *out_id = (guint32)v;
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

#define ID_EBML            0x1A45DFA3u
#define ID_SEGMENT         0x18538067u
#define ID_INFO            0x1549A966u
#define ID_TIMECODE_SCALE  0x2AD7B1u
#define ID_TRACKS          0x1654AE6Bu
#define ID_TRACK_ENTRY     0xAEu
#define ID_TRACK_NUMBER    0xD7u
#define ID_TRACK_TYPE      0x83u
#define ID_CODEC_ID        0x86u
#define ID_VIDEO           0xE0u
#define ID_PIXEL_WIDTH     0xB0u
#define ID_PIXEL_HEIGHT    0xBAu
#define ID_CLUSTER         0x1F43B675u
#define ID_TIMECODE        0xE7u
#define ID_SIMPLE_BLOCK    0xA3u
#define ID_BLOCK_GROUP     0xA0u
#define ID_BLOCK           0xA1u

static void
parse_video(nd_webm *w, const guint8 *p, gsize len, nd_webm_track *t)
{
    gsize pos = 0;
    while (pos < len) {
        guint32 id;
        gsize id_consumed = 0, size_consumed = 0;
        guint64 size;
        if (!read_id(p + pos, len - pos, &id, &id_consumed)) return;
        pos += id_consumed;
        if (pos >= len) return;
        if (!read_size(p + pos, len - pos, &size, &size_consumed)) return;
        pos += size_consumed;
        if (pos > len || size > (guint64)(len - pos)) return;
        if (id == ID_PIXEL_WIDTH)  t->width  = (int)read_uint(p + pos, (gsize)size);
        if (id == ID_PIXEL_HEIGHT) t->height = (int)read_uint(p + pos, (gsize)size);
        pos += (gsize)size;
    }
    (void)w;
}

static void
parse_track_entry(nd_webm *w, const guint8 *p, gsize len)
{
    nd_webm_track t = {0};
    int track_num = 0;
    int track_type = 0;
    gsize pos = 0;
    while (pos < len) {
        guint32 id;
        gsize id_consumed = 0, size_consumed = 0;
        guint64 size;
        if (!read_id(p + pos, len - pos, &id, &id_consumed)) return;
        pos += id_consumed;
        if (pos >= len) return;
        if (!read_size(p + pos, len - pos, &size, &size_consumed)) return;
        pos += size_consumed;
        if (pos > len || size > (guint64)(len - pos)) return;
        if (id == ID_TRACK_NUMBER) track_num  = (int)read_uint(p + pos, (gsize)size);
        if (id == ID_TRACK_TYPE)   track_type = (int)read_uint(p + pos, (gsize)size);
        if (id == ID_CODEC_ID) {
            g_free(t.codec_id);
            t.codec_id = g_strndup((const char *)(p + pos), (gsize)size);
        }
        if (id == ID_VIDEO) parse_video(w, p + pos, (gsize)size, &t);
        pos += (gsize)size;
    }
    if (track_type == 1 && !w->have_video) {
        w->video_track_num = track_num;
        w->video = t;
        w->have_video = TRUE;
    } else {
        g_free(t.codec_id);
    }
}

static void
parse_tracks(nd_webm *w, const guint8 *p, gsize len)
{
    gsize pos = 0;
    while (pos < len) {
        guint32 id;
        gsize id_consumed = 0, size_consumed = 0;
        guint64 size;
        if (!read_id(p + pos, len - pos, &id, &id_consumed)) return;
        pos += id_consumed;
        if (pos >= len) return;
        if (!read_size(p + pos, len - pos, &size, &size_consumed)) return;
        pos += size_consumed;
        if (pos > len || size > (guint64)(len - pos)) return;
        if (id == ID_TRACK_ENTRY) parse_track_entry(w, p + pos, (gsize)size);
        pos += (gsize)size;
    }
}

static void
parse_info(nd_webm *w, const guint8 *p, gsize len)
{
    gsize pos = 0;
    while (pos < len) {
        guint32 id;
        gsize id_consumed = 0, size_consumed = 0;
        guint64 size;
        if (!read_id(p + pos, len - pos, &id, &id_consumed)) return;
        pos += id_consumed;
        if (pos >= len) return;
        if (!read_size(p + pos, len - pos, &size, &size_consumed)) return;
        pos += size_consumed;
        if (pos > len || size > (guint64)(len - pos)) return;
        if (id == ID_TIMECODE_SCALE) {
            guint64 v = read_uint(p + pos, (gsize)size);
            if (v > 0) w->timecode_scale_ns = (gint64)v;
        }
        pos += (gsize)size;
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
    while (pos < len) {
        guint32 id;
        gsize id_consumed = 0, size_consumed = 0;
        guint64 size;
        if (!read_id(body + pos, len - pos, &id, &id_consumed)) goto fail;
        pos += id_consumed;
        if (pos >= len) goto fail;
        if (!read_size(body + pos, len - pos, &size, &size_consumed)) goto fail;
        pos += size_consumed;
        if (pos > len) goto fail;
        if (id == ID_SEGMENT) {
            w->segment_start = pos;
            if (size == 0xFFFFFFFFFFFFFFull || size > (guint64)(len - pos))
                w->segment_end = len;
            else
                w->segment_end = pos + (gsize)size;
            break;
        }
        if (size > (guint64)(len - pos)) goto fail;
        pos += (gsize)size;
    }
    if (w->segment_end == 0) goto fail;
    gsize seg_pos = w->segment_start;
    while (seg_pos < w->segment_end) {
        guint32 id;
        gsize id_consumed = 0, size_consumed = 0;
        guint64 size;
        if (!read_id(body + seg_pos, w->segment_end - seg_pos, &id, &id_consumed)) break;
        seg_pos += id_consumed;
        if (seg_pos >= w->segment_end) break;
        if (!read_size(body + seg_pos, w->segment_end - seg_pos, &size, &size_consumed)) break;
        seg_pos += size_consumed;
        if (seg_pos > w->segment_end ||
            size > (guint64)(w->segment_end - seg_pos)) break;
        if (id == ID_INFO)    parse_info(w, body + seg_pos, (gsize)size);
        if (id == ID_TRACKS)  parse_tracks(w, body + seg_pos, (gsize)size);
        if (id == ID_CLUSTER) {
            w->cluster_pos = seg_pos;
            w->cluster_end = seg_pos + (gsize)size;
            break;
        }
        seg_pos += (gsize)size;
    }
    if (!w->have_video || w->cluster_pos == 0) goto fail;
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
    g_free(w);
}

const nd_webm_track *
nd_webm_video_track(const nd_webm *w)
{
    return w && w->have_video ? &w->video : NULL;
}

static gboolean
parse_block_header(const guint8 *p, gsize len, int video_track,
                   const guint8 **out_payload, gsize *out_payload_len,
                   gint64 *out_rel_tc, gboolean *out_keyframe)
{
    if (len < 4) return FALSE;
    guint64 tn;
    gsize tn_consumed;
    if (!read_vint(p, len, &tn, &tn_consumed, FALSE)) return FALSE;
    if ((int)tn != video_track) return FALSE;
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

gboolean
nd_webm_next_video_frame(nd_webm *w, nd_webm_frame *out)
{
    if (!w || !out) return FALSE;
    while (w->cluster_pos < w->segment_end) {
        while (w->cluster_pos < w->cluster_end) {
            guint32 id;
            gsize id_consumed = 0, size_consumed = 0;
            guint64 size;
            if (!read_id(w->data + w->cluster_pos,
                         w->cluster_end - w->cluster_pos, &id, &id_consumed))
                break;
            w->cluster_pos += id_consumed;
            if (!read_size(w->data + w->cluster_pos,
                           w->cluster_end - w->cluster_pos, &size, &size_consumed))
                break;
            w->cluster_pos += size_consumed;
            if (w->cluster_pos + size > w->cluster_end) break;
            const guint8 *p = w->data + w->cluster_pos;
            gsize sz = (gsize)size;
            if (id == ID_TIMECODE) {
                w->cluster_timecode = (gint64)read_uint(p, sz);
            } else if (id == ID_SIMPLE_BLOCK ||
                       (id == ID_BLOCK_GROUP)) {
                const guint8 *bp = p;
                gsize bsz = sz;
                if (id == ID_BLOCK_GROUP) {
                    gsize gp = 0;
                    while (gp < sz) {
                        guint32 gid;
                        gsize gc = 0, gsc = 0;
                        guint64 gsize_;
                        if (!read_id(p + gp, sz - gp, &gid, &gc)) break;
                        gp += gc;
                        if (!read_size(p + gp, sz - gp, &gsize_, &gsc)) break;
                        gp += gsc;
                        if (gp + gsize_ > sz) break;
                        if (gid == ID_BLOCK) {
                            bp = p + gp;
                            bsz = (gsize)gsize_;
                            break;
                        }
                        gp += (gsize)gsize_;
                    }
                }
                const guint8 *payload;
                gsize payload_len;
                gint64 rel_tc;
                gboolean kf;
                if (parse_block_header(bp, bsz, w->video_track_num,
                                       &payload, &payload_len, &rel_tc, &kf)) {
                    out->data = payload;
                    out->len = payload_len;
                    out->timecode_us = (w->cluster_timecode + rel_tc) *
                                       w->timecode_scale_ns / 1000;
                    out->keyframe = kf || (id == ID_SIMPLE_BLOCK && kf);
                    w->cluster_pos += sz;
                    return TRUE;
                }
            }
            w->cluster_pos += sz;
        }
        if (w->cluster_end >= w->segment_end) break;
        gsize seg_pos = w->cluster_end;
        gboolean advanced = FALSE;
        while (seg_pos < w->segment_end) {
            guint32 id;
            gsize id_consumed = 0, size_consumed = 0;
            guint64 size;
            if (!read_id(w->data + seg_pos, w->segment_end - seg_pos,
                         &id, &id_consumed)) break;
            seg_pos += id_consumed;
            if (!read_size(w->data + seg_pos, w->segment_end - seg_pos,
                           &size, &size_consumed)) break;
            seg_pos += size_consumed;
            if (seg_pos > w->segment_end || size > (guint64)(w->segment_end - seg_pos)) break;
            if (id == ID_CLUSTER) {
                w->cluster_pos = seg_pos;
                w->cluster_end = seg_pos + (gsize)size;
                w->cluster_timecode = 0;
                advanced = TRUE;
                break;
            }
            seg_pos += (gsize)size;
        }
        if (!advanced) break;
    }
    return FALSE;
}

void
nd_webm_seek_start(nd_webm *w)
{
    if (!w) return;
    w->cluster_pos = 0;
    w->cluster_end = 0;
    w->cluster_timecode = 0;
    gsize seg_pos = w->segment_start;
    while (seg_pos < w->segment_end) {
        guint32 id;
        gsize id_consumed = 0, size_consumed = 0;
        guint64 size;
        if (!read_id(w->data + seg_pos, w->segment_end - seg_pos, &id, &id_consumed)) break;
        seg_pos += id_consumed;
        if (!read_size(w->data + seg_pos, w->segment_end - seg_pos, &size, &size_consumed)) break;
        seg_pos += size_consumed;
        if (seg_pos > w->segment_end || size > (guint64)(w->segment_end - seg_pos)) break;
        if (id == ID_CLUSTER) {
            w->cluster_pos = seg_pos;
            w->cluster_end = seg_pos + (gsize)size;
            return;
        }
        seg_pos += (gsize)size;
    }
}
