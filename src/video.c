/* Nordstjernen — VP9 video decode + YouTube watch page rewriting.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "video.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <string.h>

#include "audio.h"
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

void
nd_video_restart(nd_video *v)
{
    if (!v || !v->loaded || v->failed) return;
    v->ended = FALSE;
    v->current_frame = 0;
    v->last_frame_us = 0;
    v->start_wallclock_us = 0;
    nd_vpx_state *st = v->decoder;
    if (st && st->demux) {
        nd_webm_seek_start(st->demux);
        st->seen_keyframe = FALSE;
    }
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

void
nd_video_restart(nd_video *v)
{
    (void)v;
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
static const char k_browser_ua[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

const char *
nd_youtube_browser_user_agent(void)
{
    return k_browser_ua;
}

static gboolean
host_is_youtube_site(const char *host)
{
    if (!host) return FALSE;
    return g_ascii_strcasecmp(host, "www.youtube.com") == 0 ||
           g_ascii_strcasecmp(host, "youtube.com")     == 0 ||
           g_ascii_strcasecmp(host, "m.youtube.com")   == 0 ||
           g_ascii_strcasecmp(host, "youtu.be")        == 0;
}

static gboolean
host_is_youtube_media(const char *host)
{
    if (!host) return FALSE;
    if (g_str_has_suffix(host, ".googlevideo.com")) return TRUE;
    if (g_str_has_suffix(host, ".ytimg.com")) return TRUE;
    if (g_str_has_suffix(host, ".ggpht.com")) return TRUE;
    return FALSE;
}

gboolean
nd_youtube_host_needs_browser_ua(const char *host)
{
    return host_is_youtube_site(host) || host_is_youtube_media(host);
}

gboolean
nd_youtube_is_watch_url(const char *url)
{
    if (!url) return FALSE;
    char *host = nd_url_host_from(url);
    gboolean is_yt = host_is_youtube_site(host);
    gboolean is_short_host = host &&
        g_ascii_strcasecmp(host, "youtu.be") == 0;
    g_free(host);
    if (!is_yt) return FALSE;
    const char *scheme = strstr(url, "://");
    if (!scheme) return FALSE;
    const char *path = strchr(scheme + 3, '/');
    if (!path) return FALSE;
    if (is_short_host)
        return path[0] == '/' && path[1] && path[1] != '?';
    return g_str_has_prefix(path, "/watch?") ||
           g_str_has_prefix(path, "/watch/") ||
           strcmp(path, "/watch") == 0 ||
           g_str_has_prefix(path, "/embed/") ||
           g_str_has_prefix(path, "/shorts/") ||
           g_str_has_prefix(path, "/live/");
}

static const char *
skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static const char *
skip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"') return p;
    p++;
    while (p < end) {
        if (*p == '\\') {
            p++;
            if (p < end) p++;
        } else if (*p == '"') {
            return p + 1;
        } else {
            p++;
        }
    }
    return end;
}

static const char *
skip_value(const char *p, const char *end)
{
    p = skip_ws(p, end);
    if (p >= end) return p;
    if (*p == '"') return skip_string(p, end);
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (p < end && depth > 0) {
            if (*p == '"') p = skip_string(p, end);
            else if (*p == open)  { depth++; p++; }
            else if (*p == close) { depth--; p++; }
            else p++;
        }
        return p;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    return p;
}

static char *
parse_json_string(const char *p, const char *end, const char **out_after)
{
    if (p >= end || *p != '"') return NULL;
    p++;
    GString *s = g_string_new(NULL);
    while (p < end && *p != '"') {
        if (*p == '\\') {
            p++;
            if (p >= end) break;
            switch (*p) {
            case '"':  g_string_append_c(s, '"');  p++; break;
            case '\\': g_string_append_c(s, '\\'); p++; break;
            case '/':  g_string_append_c(s, '/');  p++; break;
            case 'b':  g_string_append_c(s, '\b'); p++; break;
            case 'f':  g_string_append_c(s, '\f'); p++; break;
            case 'n':  g_string_append_c(s, '\n'); p++; break;
            case 'r':  g_string_append_c(s, '\r'); p++; break;
            case 't':  g_string_append_c(s, '\t'); p++; break;
            case 'u':
                p++;
                if (p + 4 > end) goto done;
                {
                    char hex[5] = { p[0], p[1], p[2], p[3], 0 };
                    p += 4;
                    guint cp = (guint)g_ascii_strtoull(hex, NULL, 16);
                    gchar buf[8];
                    gint n = g_unichar_to_utf8(cp, buf);
                    if (n > 0) g_string_append_len(s, buf, n);
                }
                break;
            default:
                g_string_append_c(s, *p);
                p++;
                break;
            }
        } else {
            g_string_append_c(s, *p);
            p++;
        }
    }
done:
    if (p < end && *p == '"') p++;
    if (out_after) *out_after = p;
    return g_string_free(s, FALSE);
}

static const char *
find_key_in_object(const char *obj, const char *end, const char *key)
{
    if (!obj || obj >= end || *obj != '{') return NULL;
    gsize keylen = strlen(key);
    const char *p = obj + 1;
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end) return NULL;
        if (*p == '}') return NULL;
        if (*p != '"') return NULL;
        const char *key_after = skip_string(p, end);
        if (key_after <= p + 1) return NULL;
        gsize klen = (gsize)((key_after - 1) - (p + 1));
        gboolean match = (klen == keylen &&
                          strncmp(p + 1, key, keylen) == 0);
        p = skip_ws(key_after, end);
        if (p >= end || *p != ':') return NULL;
        p++;
        p = skip_ws(p, end);
        if (match) return p;
        p = skip_value(p, end);
        p = skip_ws(p, end);
        if (p < end && *p == ',') p++;
    }
    return NULL;
}

static char *
get_string_field(const char *obj, const char *end, const char *key)
{
    const char *v = find_key_in_object(obj, end, key);
    if (!v || v >= end || *v != '"') return NULL;
    return parse_json_string(v, end, NULL);
}

static gint64
get_int_field(const char *obj, const char *end, const char *key, gint64 dflt)
{
    const char *v = find_key_in_object(obj, end, key);
    if (!v || v >= end) return dflt;
    if (*v == '"') {
        char *s = parse_json_string(v, end, NULL);
        gint64 n = s ? g_ascii_strtoll(s, NULL, 10) : dflt;
        g_free(s);
        return n;
    }
    return g_ascii_strtoll(v, NULL, 10);
}

static const char *
find_player_response(const char *body, gsize len, const char **out_end)
{
    const char *needles[] = {
        "var ytInitialPlayerResponse =",
        "ytInitialPlayerResponse =",
        "\"ytInitialPlayerResponse\":",
        NULL
    };
    const char *end = body + len;
    for (int i = 0; needles[i]; i++) {
        const char *hit = g_strstr_len(body, (gssize)len, needles[i]);
        if (!hit) continue;
        const char *p = hit + strlen(needles[i]);
        p = skip_ws(p, end);
        if (p >= end || *p != '{') continue;
        const char *finish = skip_value(p, end);
        if (finish <= p) continue;
        if (out_end) *out_end = finish;
        return p;
    }
    return NULL;
}

typedef struct yt_format {
    char *mime_type;
    char *url;
    char *quality_label;
    int   width;
    int   height;
    gint64 bitrate;
} yt_format;

static void
yt_format_clear(yt_format *f)
{
    if (!f) return;
    g_free(f->mime_type);
    g_free(f->url);
    g_free(f->quality_label);
    memset(f, 0, sizeof(*f));
}

static gboolean
mime_is_webm_with(const char *m, const char *kind, const char *const *codecs)
{
    if (!m) return FALSE;
    if (g_ascii_strncasecmp(m, kind, 10) != 0) return FALSE;
    for (int i = 0; codecs[i]; i++)
        if (strstr(m, codecs[i])) return TRUE;
    return FALSE;
}

static int
score_video_format(const yt_format *f)
{
    if (!f || !f->url || !f->mime_type) return -1;
    static const char *const vp9[] = { "vp9", "vp09", NULL };
    static const char *const vp8[] = { "vp8", "vp08", NULL };
    int score;
    if (mime_is_webm_with(f->mime_type, "video/webm", vp9)) score = 10000;
    else if (mime_is_webm_with(f->mime_type, "video/webm", vp8)) score = 5000;
    else return -1;
    int h = f->height > 0 ? f->height : 360;
    int dist = h - 720;
    if (dist < 0) dist = -dist * 2;
    score -= dist;
    return score;
}

static int
score_audio_format(const yt_format *f)
{
    if (!f || !f->url || !f->mime_type) return -1;
    static const char *const opus[] = { "opus", NULL };
    if (!mime_is_webm_with(f->mime_type, "audio/webm", opus)) return -1;
    return f->bitrate > 0 ? (int)(f->bitrate / 100) : 1;
}

typedef struct pick_ctx {
    yt_format best_video;
    int       best_video_score;
    yt_format best_audio;
    int       best_audio_score;
} pick_ctx;

static void
yt_format_assign_copy(yt_format *dst, const yt_format *src)
{
    yt_format_clear(dst);
    dst->mime_type     = g_strdup(src->mime_type);
    dst->url           = g_strdup(src->url);
    dst->quality_label = g_strdup(src->quality_label);
    dst->width   = src->width;
    dst->height  = src->height;
    dst->bitrate = src->bitrate;
}

static void
consider_format(pick_ctx *ctx, const char *item, const char *item_end)
{
    yt_format f = {0};
    if (item >= item_end || *item != '{') return;
    f.mime_type     = get_string_field(item, item_end, "mimeType");
    f.url           = get_string_field(item, item_end, "url");
    f.quality_label = get_string_field(item, item_end, "qualityLabel");
    f.width         = (int)get_int_field(item, item_end, "width",  0);
    f.height        = (int)get_int_field(item, item_end, "height", 0);
    f.bitrate       = get_int_field(item, item_end, "bitrate", 0);

    int vs = score_video_format(&f);
    if (vs > ctx->best_video_score) {
        yt_format_assign_copy(&ctx->best_video, &f);
        ctx->best_video_score = vs;
    }
    int as = score_audio_format(&f);
    if (as > ctx->best_audio_score) {
        yt_format_assign_copy(&ctx->best_audio, &f);
        ctx->best_audio_score = as;
    }
    yt_format_clear(&f);
}

static void
walk_formats(const char *array_start, const char *end, pick_ctx *ctx)
{
    if (!array_start || array_start >= end || *array_start != '[') return;
    const char *p = array_start + 1;
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') return;
        const char *item = p;
        const char *after = skip_value(p, end);
        if (after <= item) return;
        consider_format(ctx, item, after);
        p = skip_ws(after, end);
        if (p < end && *p == ',') p++;
    }
}

static char *
escape_html(const char *s)
{
    if (!s) return g_strdup("");
    GString *out = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '&': g_string_append(out, "&amp;");  break;
        case '"': g_string_append(out, "&quot;"); break;
        case '<': g_string_append(out, "&lt;");   break;
        case '>': g_string_append(out, "&gt;");   break;
        default:  g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

static char *
pick_best_thumbnail(const char *details_obj, const char *end)
{
    const char *thumb = find_key_in_object(details_obj, end, "thumbnail");
    if (!thumb) return NULL;
    const char *arr = find_key_in_object(thumb, end, "thumbnails");
    if (!arr || *arr != '[') return NULL;
    const char *p = arr + 1;
    char *best = NULL;
    int best_w = 0;
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') break;
        const char *item = p;
        const char *after = skip_value(p, end);
        if (after <= item) break;
        if (*item == '{') {
            char *u = get_string_field(item, after, "url");
            int w = (int)get_int_field(item, after, "width", 0);
            if (u && w >= best_w) {
                g_free(best);
                best = u;
                best_w = w;
            } else {
                g_free(u);
            }
        }
        p = skip_ws(after, end);
        if (p < end && *p == ',') p++;
    }
    return best;
}

typedef struct yt_details {
    char  *title;
    char  *author;
    char  *video_id;
    char  *view_count;
    char  *length_seconds;
    char  *description;
    char  *poster_url;
} yt_details;

static void
yt_details_clear(yt_details *d)
{
    if (!d) return;
    g_free(d->title);
    g_free(d->author);
    g_free(d->video_id);
    g_free(d->view_count);
    g_free(d->length_seconds);
    g_free(d->description);
    g_free(d->poster_url);
    memset(d, 0, sizeof(*d));
}

static char *
format_view_count(const char *s)
{
    if (!s || !*s) return NULL;
    gint64 n = g_ascii_strtoll(s, NULL, 10);
    if (n <= 0) return NULL;
    GString *g = g_string_new(NULL);
    char buf[32];
    g_snprintf(buf, sizeof buf, "%" G_GINT64_FORMAT, n);
    int len = (int)strlen(buf);
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) g_string_append_c(g, ',');
        g_string_append_c(g, buf[i]);
    }
    g_string_append(g, " views");
    return g_string_free(g, FALSE);
}

static char *
format_duration(const char *s)
{
    if (!s || !*s) return NULL;
    gint64 sec = g_ascii_strtoll(s, NULL, 10);
    if (sec <= 0) return NULL;
    gint64 h = sec / 3600;
    gint64 m = (sec % 3600) / 60;
    gint64 r = sec % 60;
    if (h > 0)
        return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT
                               ":%02" G_GINT64_FORMAT, h, m, r);
    return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, m, r);
}

static char *
canonical_watch_url(const yt_details *d, const char *original_url)
{
    if (d && d->video_id && *d->video_id)
        return g_strdup_printf("https://www.youtube.com/watch?v=%s",
                               d->video_id);
    return g_strdup(original_url ? original_url : "");
}

static void
append_meta(GString *out, const yt_details *d)
{
    if (!d) return;
    gboolean any = FALSE;
    if (d->author && *d->author) {
        char *a = escape_html(d->author);
        g_string_append_printf(out, "<span class=\"channel\">%s</span>", a);
        g_free(a);
        any = TRUE;
    }
    char *views = format_view_count(d->view_count);
    if (views) {
        if (any) g_string_append(out, " &middot; ");
        char *e = escape_html(views);
        g_string_append(out, e);
        g_free(e);
        g_free(views);
        any = TRUE;
    }
    char *dur = format_duration(d->length_seconds);
    if (dur) {
        if (any) g_string_append(out, " &middot; ");
        char *e = escape_html(dur);
        g_string_append(out, e);
        g_free(e);
        g_free(dur);
    }
}

static void
append_description(GString *out, const char *desc)
{
    if (!desc || !*desc) return;
    g_string_append(out, "<pre class=\"desc\">");
    char *e = escape_html(desc);
    g_string_append(out, e);
    g_free(e);
    g_string_append(out, "</pre>");
}

static const char k_yt_style[] =
    "body{margin:0;background:#0e0e0e;color:#eee;"
    "font:14px system-ui,-apple-system,sans-serif;}"
    "header{padding:10px 16px;background:#181818;}"
    "header h1{margin:0;font-size:16px;}"
    ".meta{padding:6px 16px;background:#141414;color:#aaa;font-size:12px;}"
    ".meta .channel{color:#ddd;font-weight:600;}"
    "main{display:block;}"
    "video,main img{display:block;margin:0 auto;max-width:100%;"
    "background:#000;height:auto;}"
    ".desc{margin:12px 16px;padding:10px;background:#161616;"
    "color:#ccc;white-space:pre-wrap;word-wrap:break-word;"
    "font:13px/1.45 system-ui,-apple-system,sans-serif;"
    "max-height:18em;overflow:hidden;}"
    "footer{padding:8px 16px;color:#888;font-size:12px;background:#101010;}"
    "a{color:#79b8ff;}";

static char *
build_error_page(const char *original_url, const yt_details *d,
                 const char *reason)
{
    char *t  = escape_html(d && d->title ? d->title : "YouTube video");
    char *r  = escape_html(reason ? reason : "Playback unavailable.");
    char *canon = canonical_watch_url(d, original_url);
    char *canon_e = escape_html(canon);
    GString *meta = g_string_new(NULL);
    if (d) append_meta(meta, d);
    GString *poster_html = g_string_new(NULL);
    if (d && d->poster_url && *d->poster_url) {
        char *p = escape_html(d->poster_url);
        g_string_append_printf(poster_html, "<img src=\"%s\" alt=\"\">", p);
        g_free(p);
    }
    GString *page = g_string_new(NULL);
    g_string_append_printf(page,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>%s</title><style>%s</style></head><body>"
        "<header><h1>%s</h1></header>",
        t, k_yt_style, t);
    if (meta->len > 0)
        g_string_append_printf(page, "<div class=\"meta\">%s</div>", meta->str);
    g_string_append_printf(page,
        "<main>%s</main>"
        "<p style=\"padding:0 16px;color:#ccc;line-height:1.45;\">%s</p>",
        poster_html->str, r);
    if (d) append_description(page, d->description);
    g_string_append_printf(page,
        "<footer>Watch on YouTube: "
        "<a href=\"%s\">%s</a></footer></body></html>",
        canon_e, canon_e);
    g_string_free(meta, TRUE);
    g_string_free(poster_html, TRUE);
    g_free(t);
    g_free(r);
    g_free(canon);
    g_free(canon_e);
    return g_string_free(page, FALSE);
}

static void
extract_details(const char *resp, const char *resp_end, yt_details *d)
{
    const char *details = find_key_in_object(resp, resp_end, "videoDetails");
    if (!details || *details != '{') return;
    if (!d->title)
        d->title          = get_string_field(details, resp_end, "title");
    if (!d->author)
        d->author         = get_string_field(details, resp_end, "author");
    if (!d->video_id)
        d->video_id       = get_string_field(details, resp_end, "videoId");
    if (!d->view_count)
        d->view_count     = get_string_field(details, resp_end, "viewCount");
    if (!d->length_seconds)
        d->length_seconds = get_string_field(details, resp_end, "lengthSeconds");
    if (!d->description)
        d->description    = get_string_field(details, resp_end, "shortDescription");
    if (!d->poster_url)
        d->poster_url     = pick_best_thumbnail(details, resp_end);
}

static void
pick_streams_from_response(const char *resp, const char *resp_end, pick_ctx *ctx)
{
    const char *sd = find_key_in_object(resp, resp_end, "streamingData");
    if (!sd || *sd != '{') return;
    const char *adaptive = find_key_in_object(sd, resp_end, "adaptiveFormats");
    if (adaptive) walk_formats(adaptive, resp_end, ctx);
    const char *progressive = find_key_in_object(sd, resp_end, "formats");
    if (progressive) walk_formats(progressive, resp_end, ctx);
}

static char *
strndup_until(const char *s, const char *delims)
{
    const char *e = s;
    while (*e && !strchr(delims, *e)) e++;
    return e > s ? g_strndup(s, e - s) : NULL;
}

static char *
extract_video_id_from_url(const char *url)
{
    if (!url) return NULL;
    char *host = nd_url_host_from(url);
    const char *scheme = strstr(url, "://");
    const char *path = scheme ? strchr(scheme + 3, '/') : NULL;
    char *id = NULL;
    if (host && g_ascii_strcasecmp(host, "youtu.be") == 0 &&
        path && path[0] == '/' && path[1] && path[1] != '?')
        id = strndup_until(path + 1, "?&#/");
    if (!id && path) {
        static const char *const prefixes[] = {
            "/embed/", "/shorts/", "/live/", NULL
        };
        for (int i = 0; prefixes[i] && !id; i++) {
            if (g_str_has_prefix(path, prefixes[i]))
                id = strndup_until(path + strlen(prefixes[i]), "?&#/");
        }
    }
    g_free(host);
    if (id) return id;
    const char *q = strchr(url, '?');
    if (!q) return NULL;
    q++;
    while (*q) {
        const char *amp = strchr(q, '&');
        const char *eq  = strchr(q, '=');
        if (eq && (!amp || eq < amp) && eq - q == 1 && q[0] == 'v') {
            const char *vstart = eq + 1;
            const char *vend = amp ? amp : vstart + strlen(vstart);
            return g_strndup(vstart, vend - vstart);
        }
        if (!amp) break;
        q = amp + 1;
    }
    return NULL;
}

typedef struct yt_client {
    const char *body_template;
    const char *user_agent;
    const char *client_name_header;
    const char *client_version_header;
} yt_client;

static const yt_client k_yt_clients[] = {
    {
        "{\"videoId\":\"%s\","
        "\"context\":{\"client\":{"
        "\"clientName\":\"IOS\",\"clientVersion\":\"20.10.4\","
        "\"deviceMake\":\"Apple\",\"deviceModel\":\"iPhone16,2\","
        "\"platform\":\"MOBILE\",\"osName\":\"iOS\","
        "\"osVersion\":\"18.3.2.22D82\","
        "\"hl\":\"en\",\"gl\":\"US\""
        "}}}",
        "com.google.ios.youtube/20.10.4 "
        "(iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)",
        "X-YouTube-Client-Name: 5",
        "X-YouTube-Client-Version: 20.10.4",
    },
    {
        "{\"videoId\":\"%s\","
        "\"context\":{\"client\":{"
        "\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"1.62.27\","
        "\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest 3\","
        "\"androidSdkVersion\":32,"
        "\"osName\":\"Android\",\"osVersion\":\"12L\","
        "\"hl\":\"en\",\"gl\":\"US\""
        "}}}",
        "com.google.android.apps.youtube.vr.oculus/1.62.27 "
        "(Linux; U; Android 12L; en_US) gzip",
        "X-YouTube-Client-Name: 28",
        "X-YouTube-Client-Version: 1.62.27",
    },
};

static GByteArray *
fetch_innertube_with_client(const char *video_id, const yt_client *c)
{
    if (!video_id || !*video_id) return NULL;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    char *body = g_strdup_printf(c->body_template, video_id);
#pragma GCC diagnostic pop
    char *ua   = g_strdup_printf("User-Agent: %s", c->user_agent);
    const char *headers[] = {
        ua,
        c->client_name_header,
        c->client_version_header,
        "Origin: https://www.youtube.com",
        "X-ND-Timeout-Seconds: 8",
        NULL,
    };
    GError *err = NULL;
    nd_response *resp = nd_net_request_blocking(
        "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
        "https://www.youtube.com/",
        "POST", body, strlen(body),
        "application/json", headers, NULL, &err);
    g_free(body);
    g_free(ua);
    g_clear_error(&err);
    if (!resp) return NULL;
    GByteArray *out = NULL;
    if (!resp->error && resp->body && resp->body->len > 0)
        out = g_byte_array_ref(resp->body);
    nd_response_free(resp);
    return out;
}

char *
nd_youtube_render_watch_page(const char *url, const char *body, gsize body_len)
{
    if (!nd_youtube_is_watch_url(url)) return NULL;

    yt_details d = {0};
    pick_ctx ctx = { .best_video_score = -1, .best_audio_score = -1 };

    const char *resp_end = NULL;
    const char *resp = body && body_len > 0
        ? find_player_response(body, body_len, &resp_end) : NULL;
    if (resp && resp_end) {
        extract_details(resp, resp_end, &d);
        pick_streams_from_response(resp, resp_end, &ctx);
    }

    GByteArray *innertube = NULL;
    if (!ctx.best_video.url) {
        char *vid = d.video_id ? g_strdup(d.video_id)
                               : extract_video_id_from_url(url);
        if (vid) {
            const gsize n_clients =
                sizeof k_yt_clients / sizeof k_yt_clients[0];
            for (gsize i = 0; i < n_clients && !ctx.best_video.url; i++) {
                if (innertube) g_byte_array_unref(innertube);
                innertube = fetch_innertube_with_client(vid, &k_yt_clients[i]);
                if (!innertube || innertube->len == 0) continue;
                const char *body2 = (const char *)innertube->data;
                const char *end2  = body2 + innertube->len;
                pick_streams_from_response(body2, end2, &ctx);
                extract_details(body2, end2, &d);
            }
            if (!d.video_id) d.video_id = vid;
            else g_free(vid);
        }
    }

    char *page = NULL;
    if (ctx.best_video.url && ctx.best_video.mime_type) {
        char *t   = escape_html(d.title ? d.title : "YouTube video");
        char *src = escape_html(ctx.best_video.url);
        char *ps  = escape_html(d.poster_url ? d.poster_url : "");
        int  w    = ctx.best_video.width  > 0 ? ctx.best_video.width  : 640;
        int  h    = ctx.best_video.height > 0 ? ctx.best_video.height : 360;
        char *ql  = escape_html(ctx.best_video.quality_label ?
                                ctx.best_video.quality_label : "");
        char *mt  = escape_html(ctx.best_video.mime_type);
        char *audio_src = ctx.best_audio.url
            ? escape_html(ctx.best_audio.url) : NULL;
        char *canon   = canonical_watch_url(&d, url);
        char *canon_e = escape_html(canon);
        GString *meta = g_string_new(NULL);
        append_meta(meta, &d);
        GString *out = g_string_new(NULL);
        g_string_append_printf(out,
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>%s</title><style>%s</style></head><body>"
            "<header><h1>%s</h1></header>",
            t, k_yt_style, t);
        if (meta->len > 0)
            g_string_append_printf(out,
                "<div class=\"meta\">%s</div>", meta->str);
        g_string_append(out, "<main><video src=\"");
        g_string_append(out, src);
        g_string_append(out, "\" poster=\"");
        g_string_append(out, ps);
        if (audio_src) {
            g_string_append(out, "\" data-audio-src=\"");
            g_string_append(out, audio_src);
        }
        g_string_append_printf(out,
            "\" width=\"%d\" height=\"%d\" loop></video></main>", w, h);
        append_description(out, d.description);
        g_string_append_printf(out, "<footer>%s", mt);
        if (*ql) g_string_append_printf(out, " &middot; %s", ql);
        if (audio_src && nd_audio_available())
            g_string_append(out, " &middot; Opus audio");
        else
            g_string_append(out, " &middot; video only (no audio)");
        g_string_append_printf(out,
            " &middot; <a href=\"%s\">Watch on YouTube</a></footer>"
            "</body></html>", canon_e);
        page = g_string_free(out, FALSE);
        g_string_free(meta, TRUE);
        g_free(t);
        g_free(src);
        g_free(ps);
        g_free(ql);
        g_free(mt);
        g_free(audio_src);
        g_free(canon);
        g_free(canon_e);
    } else {
        page = build_error_page(url, &d,
            "No playable stream for this video. Both the in-page "
            "ytInitialPlayerResponse and the InnerTube fallback "
            "(IOS + ANDROID_VR clients) failed to return a WebM/VP9 "
            "stream with a direct URL. Usually this means the video "
            "is signature-ciphered (the YouTube player JS is required "
            "to descramble the URL — Nordstjernen does not run that), "
            "or the network blocked the YouTube / googlevideo hosts.");
    }

    yt_format_clear(&ctx.best_video);
    yt_format_clear(&ctx.best_audio);
    yt_details_clear(&d);
    if (innertube) g_byte_array_unref(innertube);
    return page;
}
