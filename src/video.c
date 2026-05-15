/* Nordstjernen — VP9 video decode (libvpx) + frame cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "video.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <string.h>

#include "config.h"
#include "image.h"
#include "net.h"
#include "webm.h"

#ifdef ND_HAVE_VPX
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#endif

struct nd_video_cache {
    GHashTable *by_url;
    GPtrArray  *pending;
};

#ifdef ND_HAVE_VPX
typedef struct nd_vpx_state {
    vpx_codec_ctx_t  codec;
    gboolean         initialized;
    gboolean         seen_keyframe;
    nd_webm         *demux;
} nd_vpx_state;
#endif

typedef struct nd_pending {
    nd_video          *video;
    nd_video_cache    *cache;
    nd_video_ready_cb  cb;
    gpointer           user_data;
    gboolean           is_poster;
    gboolean           dead;
} nd_pending;

static void
nd_video_free(gpointer p)
{
    nd_video *v = p;
    if (!v) return;
    g_free(v->url);
    if (v->poster_texture) g_object_unref(v->poster_texture);
    if (v->frame_texture)  g_object_unref(v->frame_texture);
    if (v->body) g_byte_array_unref(v->body);
#ifdef ND_HAVE_VPX
    nd_vpx_state *st = v->decoder;
    if (st) {
        if (st->initialized) vpx_codec_destroy(&st->codec);
        nd_webm_close(st->demux);
        g_free(st);
    }
#else
    (void)v->decoder;
#endif
    g_free(v);
}

nd_video_cache *
nd_video_cache_new(void)
{
    nd_video_cache *c = g_new0(nd_video_cache, 1);
    c->by_url = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nd_video_free);
    c->pending = g_ptr_array_new();
    return c;
}

void
nd_video_cache_free(nd_video_cache *cache)
{
    if (!cache) return;
    for (guint i = 0; i < cache->pending->len; i++) {
        nd_pending *p = g_ptr_array_index(cache->pending, i);
        p->dead = TRUE;
    }
    g_hash_table_destroy(cache->by_url);
    g_ptr_array_free(cache->pending, TRUE);
    g_free(cache);
}

#ifdef ND_HAVE_VPX
static void
yuv_to_rgba(const vpx_image_t *img, guchar *out)
{
    int w = (int)img->d_w;
    int h = (int)img->d_h;
    const guint8 *y_plane = img->planes[VPX_PLANE_Y];
    const guint8 *u_plane = img->planes[VPX_PLANE_U];
    const guint8 *v_plane = img->planes[VPX_PLANE_V];
    int y_stride = img->stride[VPX_PLANE_Y];
    int u_stride = img->stride[VPX_PLANE_U];
    int v_stride = img->stride[VPX_PLANE_V];
    int x_sub = (img->x_chroma_shift == 1) ? 2 : 1;
    int y_sub = (img->y_chroma_shift == 1) ? 2 : 1;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int Y = y_plane[j * y_stride + i];
            int U = u_plane[(j / y_sub) * u_stride + (i / x_sub)] - 128;
            int V = v_plane[(j / y_sub) * v_stride + (i / x_sub)] - 128;
            int C = Y - 16;
            int R = (298 * C + 409 * V + 128) >> 8;
            int G = (298 * C - 100 * U - 208 * V + 128) >> 8;
            int B = (298 * C + 516 * U + 128) >> 8;
            if (R < 0) R = 0;
            if (R > 255) R = 255;
            if (G < 0) G = 0;
            if (G > 255) G = 255;
            if (B < 0) B = 0;
            if (B > 255) B = 255;
            guchar *p = out + (j * w + i) * 4;
            p[0] = (guchar)R;
            p[1] = (guchar)G;
            p[2] = (guchar)B;
            p[3] = 0xff;
        }
    }
}

static GdkTexture *
texture_from_vpx(const vpx_image_t *img, int *out_w, int *out_h)
{
    int w = (int)img->d_w;
    int h = (int)img->d_h;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return NULL;
    guchar *rgba = g_malloc((gsize)w * (gsize)h * 4);
    yuv_to_rgba(img, rgba);
    GBytes *bytes = g_bytes_new_take(rgba, (gsize)w * (gsize)h * 4);
    GdkTexture *tex = gdk_memory_texture_new(w, h, GDK_MEMORY_R8G8B8A8,
                                             bytes, (gsize)w * 4);
    g_bytes_unref(bytes);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return tex;
}

static gboolean
nd_video_init_decoder(nd_video *v)
{
    if (!v || !v->body || v->body->len == 0) return FALSE;
    nd_vpx_state *st = g_new0(nd_vpx_state, 1);
    st->demux = nd_webm_open(v->body->data, v->body->len);
    if (!st->demux) { g_free(st); return FALSE; }
    const nd_webm_track *t = nd_webm_video_track(st->demux);
    if (!t || !t->codec_id || strcmp(t->codec_id, "V_VP9") != 0) {
        nd_webm_close(st->demux);
        g_free(st);
        return FALSE;
    }
    vpx_codec_dec_cfg_t cfg = {0};
    cfg.threads = 1;
    if (vpx_codec_dec_init(&st->codec, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) {
        nd_webm_close(st->demux);
        g_free(st);
        return FALSE;
    }
    st->initialized = TRUE;
    v->decoder = st;
    if (v->natural_width  <= 0) v->natural_width  = t->width;
    if (v->natural_height <= 0) v->natural_height = t->height;
    return TRUE;
}

gboolean
nd_video_advance_frame(nd_video *v)
{
    if (!v || !v->loaded || v->failed) return FALSE;
    if (!v->decoder && !nd_video_init_decoder(v)) {
        v->failed = TRUE;
        return FALSE;
    }
    nd_vpx_state *st = v->decoder;
    nd_webm_frame frame;
    while (nd_webm_next_video_frame(st->demux, &frame)) {
        if (!st->seen_keyframe) {
            if (!frame.keyframe) continue;
            st->seen_keyframe = TRUE;
        }
        if (vpx_codec_decode(&st->codec, frame.data, (unsigned int)frame.len,
                             NULL, 0) != VPX_CODEC_OK)
            continue;
        vpx_codec_iter_t iter = NULL;
        vpx_image_t *img = vpx_codec_get_frame(&st->codec, &iter);
        if (!img) continue;
        int w = 0, h = 0;
        GdkTexture *tex = texture_from_vpx(img, &w, &h);
        while (vpx_codec_get_frame(&st->codec, &iter)) ;
        if (!tex) continue;
        if (v->frame_texture) g_object_unref(v->frame_texture);
        v->frame_texture = tex;
        if (w > 0) v->natural_width  = w;
        if (h > 0) v->natural_height = h;
        v->current_frame++;
        v->last_frame_us = frame.timecode_us;
        return TRUE;
    }
    v->ended = TRUE;
    return FALSE;
}

gboolean
nd_video_tick(nd_video *v, gint64 now_us)
{
    if (!v || !v->loaded || v->failed || v->ended) return FALSE;
    if (v->start_wallclock_us == 0) v->start_wallclock_us = now_us;
    gint64 elapsed = now_us - v->start_wallclock_us;
    gboolean updated = FALSE;
    int budget = 4;
    while (budget-- > 0 && v->last_frame_us <= elapsed) {
        gint64 before = v->last_frame_us;
        if (!nd_video_advance_frame(v)) break;
        updated = TRUE;
        if (v->last_frame_us <= before) break;
    }
    return updated;
}
#else
gboolean
nd_video_advance_frame(nd_video *v)
{
    (void)v;
    return FALSE;
}

gboolean
nd_video_tick(nd_video *v, gint64 now_us)
{
    (void)v;
    (void)now_us;
    return FALSE;
}
#endif

static void
on_video_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_pending *pending = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    if (pending->dead) {
        nd_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    if (!resp) {
        pending->video->failed = TRUE;
        g_clear_error(&err);
        if (pending->cb) pending->cb(pending->video, pending->user_data);
        g_ptr_array_remove_fast(pending->cache->pending, pending);
        g_free(pending);
        return;
    }
    if (resp->error || !resp->body || resp->body->len == 0) {
        pending->video->failed = TRUE;
    } else if (pending->is_poster) {
        int w = 0, h = 0;
        GdkTexture *tex = nd_image_decode_bytes(resp->body->data,
                                                resp->body->len, &w, &h);
        if (tex) {
            pending->video->poster_texture = tex;
            if (pending->video->natural_width  <= 0) pending->video->natural_width  = w;
            if (pending->video->natural_height <= 0) pending->video->natural_height = h;
        }
    } else {
        if (pending->video->body) g_byte_array_unref(pending->video->body);
        pending->video->body = g_byte_array_ref(resp->body);
        pending->video->loaded = TRUE;
#ifdef ND_HAVE_VPX
        nd_video_advance_frame(pending->video);
#endif
    }
    nd_response_free(resp);
    if (pending->cb) pending->cb(pending->video, pending->user_data);
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

nd_video *
nd_video_cache_get(nd_video_cache *cache,
                   const char *url,
                   const char *poster_url,
                   const char *top_url,
                   nd_video_ready_cb cb,
                   gpointer user_data)
{
    if (!cache || !url) return NULL;
    nd_video *cached = g_hash_table_lookup(cache->by_url, url);
    if (cached && !cached->failed) return cached;
    if (cached) g_hash_table_remove(cache->by_url, url);
    nd_video *v = g_new0(nd_video, 1);
    v->url = g_strdup(url);
    g_hash_table_insert(cache->by_url, g_strdup(url), v);

    if (poster_url && *poster_url) {
        nd_pending *pp = g_new0(nd_pending, 1);
        pp->video = v;
        pp->cache = cache;
        pp->cb = cb;
        pp->user_data = user_data;
        pp->is_poster = TRUE;
        g_ptr_array_add(cache->pending, pp);
        nd_net_fetch_async(poster_url, top_url, NULL, on_video_fetched, pp);
    }

    nd_pending *pending = g_new0(nd_pending, 1);
    pending->video = v;
    pending->cache = cache;
    pending->cb = cb;
    pending->user_data = user_data;
    g_ptr_array_add(cache->pending, pending);
    nd_net_fetch_async(url, top_url, NULL, on_video_fetched, pending);
    return v;
}
