/* Nordstjernen — inline video playback and poster cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include <stdio.h>
#include "video.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "camera.h"
#include "dom.h"
#include "image.h"
#include "layout.h"
#include "net.h"
#include "video_decode.h"

#define NS_VIDEO_MAX_BYTES (256u * 1024u * 1024u)

typedef struct ns_vtt_cue {
    double start;
    double end;
    char  *text;
} ns_vtt_cue;

static void
ns_vtt_cue_free(gpointer p)
{
    ns_vtt_cue *c = p;
    if (!c) return;
    g_free(c->text);
    g_free(c);
}

static gboolean
ns_vtt_read_uint(const char **pp, long *out)
{
    const char *p = *pp;
    if (!g_ascii_isdigit(*p)) return FALSE;
    long v = 0;
    while (g_ascii_isdigit(*p)) {
        if (v < 100000000L) v = v * 10 + (*p - '0');
        p++;
    }
    *out = v;
    *pp = p;
    return TRUE;
}

static double
ns_vtt_parse_ts(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    long a, b, c;
    if (!ns_vtt_read_uint(&s, &a)) return -1.0;
    if (*s != ':') return -1.0;
    s++;
    if (!ns_vtt_read_uint(&s, &b)) return -1.0;
    long hours = 0, mins, secs;
    if (*s == ':') {
        s++;
        if (!ns_vtt_read_uint(&s, &c)) return -1.0;
        hours = a; mins = b; secs = c;
    } else {
        mins = a; secs = b;
    }
    long ms = 0;
    if (*s == '.' || *s == ',') {
        s++;
        long f = 0;
        int nd = 0;
        while (g_ascii_isdigit(*s)) { f = f * 10 + (*s - '0'); s++; nd++; }
        while (nd < 3) { f *= 10; nd++; }
        while (nd > 3) { f /= 10; nd--; }
        ms = f;
    }
    return (double)hours * 3600.0 + (double)mins * 60.0 + (double)secs
         + (double)ms / 1000.0;
}

static char *
ns_vtt_clean_text(const char *raw)
{
    GString *out = g_string_new(NULL);
    for (const char *p = raw; *p; ) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            p = e + 1;
            continue;
        }
        if (*p == '&') {
            if (g_str_has_prefix(p, "&amp;"))  { g_string_append_c(out, '&'); p += 5; continue; }
            if (g_str_has_prefix(p, "&lt;"))   { g_string_append_c(out, '<'); p += 4; continue; }
            if (g_str_has_prefix(p, "&gt;"))   { g_string_append_c(out, '>'); p += 4; continue; }
            if (g_str_has_prefix(p, "&nbsp;")) { g_string_append_c(out, ' '); p += 6; continue; }
            if (g_str_has_prefix(p, "&lrm;"))  { p += 5; continue; }
            if (g_str_has_prefix(p, "&rlm;"))  { p += 5; continue; }
        }
        g_string_append_c(out, *p);
        p++;
    }
    return g_string_free(out, FALSE);
}

static GPtrArray *
ns_vtt_parse(const char *text)
{
    GPtrArray *cues = g_ptr_array_new_with_free_func(ns_vtt_cue_free);
    char **lines = g_strsplit(text, "\n", -1);
    guint n = g_strv_length(lines);
    for (guint i = 0; i < n; ) {
        char *line = lines[i];
        gsize ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = '\0';
        const char *arrow = strstr(line, "-->");
        if (!arrow) { i++; continue; }

        double start = ns_vtt_parse_ts(line);
        double end = ns_vtt_parse_ts(arrow + 3);
        i++;
        GString *txt = g_string_new(NULL);
        while (i < n) {
            char *tl = lines[i];
            gsize tll = strlen(tl);
            if (tll && tl[tll - 1] == '\r') tl[--tll] = '\0';
            if (tll == 0) { i++; break; }
            if (txt->len) g_string_append_c(txt, '\n');
            char *clean = ns_vtt_clean_text(tl);
            g_string_append(txt, clean);
            g_free(clean);
            i++;
        }
        if (start >= 0.0 && end > start && txt->len > 0) {
            ns_vtt_cue *cue = g_new0(ns_vtt_cue, 1);
            cue->start = start;
            cue->end = end;
            cue->text = g_string_free(txt, FALSE);
            g_ptr_array_add(cues, cue);
        } else {
            g_string_free(txt, TRUE);
        }
    }
    g_strfreev(lines);
    return cues;
}

const char *
ns_video_active_cue_text(const ns_video *v)
{
    if (!v || !v->cues) return NULL;
    double t = v->cur_time;
    for (guint i = 0; i < v->cues->len; i++) {
        const ns_vtt_cue *c = g_ptr_array_index(v->cues, i);
        if (t >= c->start && t < c->end) return c->text;
    }
    return NULL;
}

struct ns_video_cache {
    GHashTable       *by_url;
    GHashTable       *requested;
    GHashTable       *mse_streams;
    GHashTable       *node_state;
    GPtrArray        *pending;
    char             *base_url;
    ns_video_js_cb    js_cb;
    gpointer          js_user;
    ns_video_audio_cb audio_cb;
    gpointer          audio_user;
    guint             next_token;
    guint             next_seq;
};

typedef struct ns_pending {
    ns_video       *video;
    ns_video_cache *cache;
    gboolean        dead;
} ns_pending;

static void ns_video_materialize_audio(ns_video_cache *cache, ns_video *v,
                                       const guint8 *data, gsize len,
                                       guint gen);

typedef struct ns_node_media_state {
    gboolean has_playing, playing;
    gboolean has_muted, muted;
    gboolean has_volume;
    double   volume;
    gboolean has_seek;
    double   seek_time;
} ns_node_media_state;

typedef struct ns_mse_stream {
    guint       id;
    GByteArray *video_bytes;
    GByteArray *audio_bytes;
    GByteArray *video_init;
    GByteArray *audio_init;
    double      video_end;
    double      audio_end;
    gboolean    eos;
    gboolean    audio_dirty;
    gboolean    audio_rebased;
    gint64      audio_flush_us;
    gboolean    video_dirty;
    gint64      video_flush_us;
    guint       video_gen;
    guint       audio_gen;
} ns_mse_stream;

static gboolean
ns_video_helper_enabled(void)
{
    return g_getenv("NS_VIDEO_HELPER") != NULL;
}

static gboolean
ns_mse_bytes_are_init_segment(const guint8 *d, gsize n)
{
    if (n >= 8 && d[4] == 'f' && d[5] == 't' && d[6] == 'y' && d[7] == 'p')
        return TRUE;
    if (n >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3)
        return TRUE;
    return FALSE;
}

static void
ns_mse_stream_free(gpointer p)
{
    ns_mse_stream *s = p;
    if (!s) return;
    if (s->video_init) g_byte_array_free(s->video_init, TRUE);
    if (s->audio_init) g_byte_array_free(s->audio_init, TRUE);
    if (s->video_bytes) g_byte_array_free(s->video_bytes, TRUE);
    if (s->audio_bytes) g_byte_array_free(s->audio_bytes, TRUE);
    g_free(s);
}
static void on_msaudio_fetched(GObject *src, GAsyncResult *result,
                               gpointer user_data);
static void ns_video_build_player(ns_pending *pending, ns_response *resp);

static void
ns_video_free(gpointer p)
{
    ns_video *v = p;
    if (!v) return;
    g_free(v->url);
    g_free(v->audio_url);
    if (v->audio_file) {
        if (g_str_has_prefix(v->audio_file, "file://"))
            g_unlink(v->audio_file + 7);
        g_free(v->audio_file);
    }
    if (v->video_file) {
        if (g_str_has_prefix(v->video_file, "file://"))
            g_unlink(v->video_file + 7);
        g_free(v->video_file);
    }
    g_free(v->token);
    if (v->cues) g_ptr_array_free(v->cues, TRUE);
    ns_texture_unref(v->poster_texture);
    ns_texture_unref(v->frame_texture);
    ns_video_player_free(v->player);
    g_free(v);
}

ns_video_cache *
ns_video_cache_new(void)
{
    ns_video_cache *c = g_new0(ns_video_cache, 1);
    c->by_url = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ns_video_free);
    c->requested = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    c->mse_streams = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                           NULL, ns_mse_stream_free);
    c->node_state = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, g_free);
    c->pending = g_ptr_array_new();
    return c;
}

void
ns_video_cache_set_base(ns_video_cache *cache, const char *base_url)
{
    if (!cache) return;
    g_free(cache->base_url);
    cache->base_url = g_strdup(base_url);
}

void
ns_video_cache_set_js_cb(ns_video_cache *cache, ns_video_js_cb cb, gpointer user_data)
{
    if (!cache) return;
    cache->js_cb = cb;
    cache->js_user = user_data;
}

void
ns_video_cache_set_audio_cb(ns_video_cache *cache, ns_video_audio_cb cb,
                            gpointer user_data)
{
    if (!cache) return;
    cache->audio_cb = cb;
    cache->audio_user = user_data;
}

static void
ns_video_emit_audio(ns_video_cache *cache, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void
ns_video_emit_audio(ns_video_cache *cache, const char *fmt, ...)
{
    if (!cache || !cache->audio_cb) return;
    va_list ap;
    va_start(ap, fmt);
    char *cmd = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    cache->audio_cb(cmd, cache->audio_user);
    g_free(cmd);
}

static gboolean
ns_video_url_audio_safe(const char *url)
{
    if (!url || !*url) return FALSE;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return FALSE;
    return TRUE;
}

static void
ns_video_audio_start(ns_video_cache *cache, ns_video *v)
{
    if (v && g_getenv("NS_DBG_AUDIO"))
        g_printerr("[audio-start] muted=%d has_audio=%d file=%s playing=%d\n",
                   v->muted, v->has_audio,
                   v->audio_file ? v->audio_file : "-", v->playing);
    if (!cache || !cache->audio_cb || !v || v->muted) return;
    if (!v->has_audio && !v->audio_file) return;
    const char *src = v->audio_file ? v->audio_file : v->url;
    if (!ns_video_url_audio_safe(src)) return;
    if (!v->token)
        v->token = g_strdup_printf("nv%u", ++cache->next_token);
    if (!v->audio_opened) {
        ns_video_emit_audio(cache, "open %s %s", v->token, src);
        if (v->loop)
            ns_video_emit_audio(cache, "loop %s 1", v->token);
        if (v->volume >= 0.0 && v->volume < 1.0)
            ns_video_emit_audio(cache, "volume %s %.3f", v->token, v->volume);
        v->audio_opened = TRUE;
    }
    ns_video_emit_audio(cache, "play %s", v->token);
}

static void
ns_video_audio_pause(ns_video_cache *cache, ns_video *v)
{
    if (!cache || !cache->audio_cb || !v || !v->audio_opened) return;
    ns_video_emit_audio(cache, "pause %s", v->token);
}

static void
ns_video_audio_stop(ns_video_cache *cache, ns_video *v)
{
    if (!cache || !cache->audio_cb || !v || !v->audio_opened) return;
    ns_video_emit_audio(cache, "stop %s", v->token);
    v->audio_opened = FALSE;
}

static void
ns_video_audio_resync(ns_video_cache *cache, ns_video *v)
{
    if (!cache || !cache->audio_cb || !v || !v->audio_opened) return;
    ns_video_emit_audio(cache, "seek %s 0", v->token);
}

static void
ns_video_helper_playpause(ns_video_cache *cache, ns_video *v, gboolean play)
{
    if (!cache || !cache->audio_cb || !v || !v->video_opened) return;
    ns_video_emit_audio(cache, "video %s %s", play ? "play" : "pause",
                        v->token);
}

static void
ns_video_helper_seek(ns_video_cache *cache, ns_video *v, double seconds)
{
    if (!cache || !cache->audio_cb || !v || !v->video_opened) return;
    ns_video_emit_audio(cache, "video seek %s %.3f", v->token, seconds);
}

static void
ns_video_helper_stop(ns_video_cache *cache, ns_video *v)
{
    if (!cache || !cache->audio_cb || !v || !v->video_opened) return;
    ns_video_emit_audio(cache, "video stop %s", v->token);
    v->video_opened = FALSE;
}

void
ns_video_note_paint_rect(ns_video *v, double x, double y, double w, double h)
{
    if (v && g_getenv("NS_DBG_AUDIO"))
        g_printerr("[note-rect] %.0f,%.0f %.0fx%.0f opened=%d\n",
                   x, y, w, h, v->video_opened);
    if (!v) return;
    v->last_paint_us = g_get_monotonic_time();
    v->rect_x = x;
    v->rect_y = y;
    v->rect_w = w;
    v->rect_h = h;
    if (fabs(x - v->sent_rect_x) > 0.5 || fabs(y - v->sent_rect_y) > 0.5 ||
        fabs(w - v->sent_rect_w) > 0.5 || fabs(h - v->sent_rect_h) > 0.5)
        v->rect_dirty = TRUE;
}

static void
ns_video_helper_flush_rect(ns_video_cache *cache, ns_video *v, gint64 now_us)
{
    if (!v->video_opened || !cache->audio_cb) return;
    if (!v->rect_dirty &&
        now_us - v->rect_sent_us < G_GINT64_CONSTANT(1000000))
        return;
    if (v->rect_w <= 0.5 || v->rect_h <= 0.5) return;
    v->rect_dirty = FALSE;
    v->rect_sent_us = now_us;
    v->sent_rect_x = v->rect_x;
    v->sent_rect_y = v->rect_y;
    v->sent_rect_w = v->rect_w;
    v->sent_rect_h = v->rect_h;
    ns_video_emit_audio(cache, "video rect %s %d %d %d %d",
                        v->token, (int)lround(v->rect_x),
                        (int)lround(v->rect_y), (int)lround(v->rect_w),
                        (int)lround(v->rect_h));
}

gboolean
ns_video_helper_composited(const ns_video *v)
{
    return v && ns_video_helper_enabled() &&
           (v->video_opened || v->mse_id != 0);
}

void
ns_video_cache_free(ns_video_cache *cache)
{
    if (!cache) return;
    if (cache->audio_cb) {
        GHashTableIter it;
        gpointer key, val;
        g_hash_table_iter_init(&it, cache->by_url);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            ns_video_audio_stop(cache, val);
            ns_video_helper_stop(cache, val);
        }
    }
    for (guint i = 0; i < cache->pending->len; i++) {
        ns_pending *p = g_ptr_array_index(cache->pending, i);
        p->dead = TRUE;
    }
    g_hash_table_destroy(cache->by_url);
    g_hash_table_destroy(cache->requested);
    g_hash_table_destroy(cache->mse_streams);
    g_hash_table_destroy(cache->node_state);
    g_ptr_array_free(cache->pending, TRUE);
    g_free(cache->base_url);
    g_free(cache);
}

static void
ns_video_emit_js(ns_video_cache *cache, ns_video *v, const char *kind, double value)
{
    if (cache && cache->js_cb && v && v->dom_node)
        cache->js_cb(v->dom_node, kind, value, cache->js_user);
}

void
ns_video_play(ns_video *v, gint64 now_us)
{
    if (!v || !v->player) return;
    if (v->playing) return;
    if (v->ended) { v->cur_time = 0.0; v->ended = FALSE; }
    v->base_us = now_us - (gint64)(v->cur_time * 1e6);
    v->playing = TRUE;
}

void
ns_video_pause(ns_video *v, gint64 now_us)
{
    if (!v || !v->player || !v->playing) return;
    v->cur_time = (double)(now_us - v->base_us) / 1e6;
    v->playing = FALSE;
}

gboolean
ns_video_toggle(ns_video *v, gint64 now_us)
{
    if (!v || !v->player) return FALSE;
    if (v->playing) ns_video_pause(v, now_us);
    else ns_video_play(v, now_us);
    return TRUE;
}

static ns_node_media_state *
ns_video_node_state(ns_video_cache *cache, const void *dom_node)
{
    ns_node_media_state *st = g_hash_table_lookup(cache->node_state,
                                                  (gpointer)dom_node);
    if (!st) {
        st = g_new0(ns_node_media_state, 1);
        g_hash_table_insert(cache->node_state, (gpointer)dom_node, st);
    }
    return st;
}

static void
ns_video_apply_node_state(ns_video_cache *cache, ns_video *v,
                          const void *dom_node)
{
    ns_node_media_state *st = g_hash_table_lookup(cache->node_state,
                                                  (gpointer)dom_node);
    if (!st) return;
    if (st->has_muted)
        v->muted = st->muted;
    if (st->has_volume)
        v->volume = st->volume;
    if (st->has_seek) {
        v->cur_time = st->seek_time;
        v->prev_tick_time = st->seek_time;
        v->last_emit_time = st->seek_time;
        if (v->playing)
            v->base_us = g_get_monotonic_time()
                       - (gint64)(st->seek_time * 1e6);
    }
    if (st->has_playing) {
        if (v->player && st->playing != v->playing) {
            gint64 now = g_get_monotonic_time();
            if (st->playing) {
                ns_video_play(v, now);
                ns_video_audio_start(cache, v);
            } else {
                ns_video_pause(v, now);
                ns_video_audio_pause(cache, v);
            }
            ns_video_helper_playpause(cache, v, st->playing);
        } else {
            v->playing = st->playing;
        }
    }
    g_hash_table_remove(cache->node_state, (gpointer)dom_node);
}

static ns_video *
ns_video_cache_find_by_node(ns_video_cache *cache, const void *dom_node)
{
    ns_video *best = NULL;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *cand = val;
        if (cand->dom_node != dom_node || cand->is_camera) continue;
        if (!best || cand->seq > best->seq) best = cand;
    }
    return best;
}

gboolean
ns_video_cache_seek_node(ns_video_cache *cache, const void *dom_node,
                         double seconds, gint64 now_us)
{
    if (!cache || !dom_node) return FALSE;
    ns_video *v = ns_video_cache_find_by_node(cache, dom_node);
    if (v && v->is_camera) return FALSE;
    if (!v || !v->player) {
        if (v) {
            v->cur_time = seconds < 0.0 ? 0.0 : seconds;
            v->prev_tick_time = v->cur_time;
            v->last_emit_time = v->cur_time;
        } else {
            ns_node_media_state *st = ns_video_node_state(cache, dom_node);
            st->has_seek = TRUE;
            st->seek_time = seconds < 0.0 ? 0.0 : seconds;
        }
        return TRUE;
    }

    double t = seconds;
    if (t < 0.0) t = 0.0;
    if (v->duration > 0.0 && t > v->duration) t = v->duration;
    v->cur_time = t;
    v->prev_tick_time = t;
    v->last_emit_time = t;
    v->ended = v->duration > 0.0 && t >= v->duration && !v->loop;
    if (v->playing)
        v->base_us = now_us - (gint64)(t * 1e6);

    gboolean ended = FALSE;
    ns_texture *frame = ns_video_player_frame_at(v->player, t, v->loop, &ended);
    if (frame) {
        ns_texture_unref(v->frame_texture);
        v->frame_texture = ns_texture_ref(frame);
    }
    if (v->audio_opened)
        ns_video_emit_audio(cache, "seek %s %.3f", v->token, t);
    ns_video_helper_seek(cache, v, t);
    ns_video_emit_js(cache, v, "pos", t);
    return TRUE;
}

gboolean
ns_video_cache_set_node_playing(ns_video_cache *cache, const void *dom_node,
                                gboolean play, gint64 now_us)
{
    if (!cache || !dom_node) return FALSE;
    ns_video *v = ns_video_cache_find_by_node(cache, dom_node);
    if (v && v->is_camera) return FALSE;
    if (!v) {
        ns_node_media_state *st = ns_video_node_state(cache, dom_node);
        st->has_playing = TRUE;
        st->playing = play;
        return TRUE;
    }
    if (!v->player) { v->playing = play; return TRUE; }
    if (play == v->playing) return FALSE;
    if (play) {
        ns_video_play(v, now_us);
        ns_video_audio_start(cache, v);
    } else {
        ns_video_pause(v, now_us);
        ns_video_audio_pause(cache, v);
    }
    ns_video_helper_playpause(cache, v, play);
    return TRUE;
}

gboolean
ns_video_cache_set_node_volume(ns_video_cache *cache, const void *dom_node,
                               double volume)
{
    if (!cache || !dom_node) return FALSE;
    ns_video *v = ns_video_cache_find_by_node(cache, dom_node);
    if (!v) {
        ns_node_media_state *st = ns_video_node_state(cache, dom_node);
        st->has_volume = TRUE;
        st->volume = volume;
        return TRUE;
    }
    v->volume = volume;
    if (v->token && v->audio_opened)
        ns_video_emit_audio(cache, "volume %s %.3f", v->token, volume);
    if (volume > 0 && !v->muted && v->playing && !v->audio_opened) {
        ns_video_audio_start(cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(cache, "seek %s %.3f", v->token, v->cur_time);
    }
    return TRUE;
}

gboolean
ns_video_cache_set_node_muted(ns_video_cache *cache, const void *dom_node,
                              gboolean muted)
{
    if (!cache || !dom_node) return FALSE;
    ns_video *v = ns_video_cache_find_by_node(cache, dom_node);
    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[set-muted] muted=%d video=%s was=%d\n",
                   muted, v ? "yes" : "NULL", v ? v->muted : -1);
    if (!v) {
        ns_node_media_state *st = ns_video_node_state(cache, dom_node);
        st->has_muted = TRUE;
        st->muted = muted;
        return TRUE;
    }
    if (v->muted == muted) return TRUE;
    v->muted = muted;
    if (muted) {
        ns_video_audio_pause(cache, v);
    } else if (v->playing) {
        ns_video_audio_start(cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(cache, "seek %s %.3f", v->token, v->cur_time);
    }
    return TRUE;
}

gboolean
ns_video_cache_toggle(ns_video_cache *cache, ns_video *v, gint64 now_us)
{
    if (!v || !v->player) return FALSE;
    if (v->playing && v->muted) {
        v->muted = FALSE;
        ns_video_audio_start(cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(cache, "seek %s %.3f", v->token, v->cur_time);
        ns_video_emit_js(cache, v, "unmuted", v->cur_time);
        return TRUE;
    }
    gboolean was_playing = v->playing;
    ns_video_toggle(v, now_us);
    ns_video_emit_js(cache, v, was_playing ? "pause" : "play", v->cur_time);
    if (was_playing) ns_video_audio_pause(cache, v);
    else ns_video_audio_start(cache, v);
    ns_video_helper_playpause(cache, v, !was_playing);
    return TRUE;
}

static gboolean
ns_video_url_is_growing_stream(const char *url)
{
    if (!url) return FALSE;
    const char *marker = strstr(url, "#ndms=");
    return marker != NULL && strstr(marker, "&eos") == NULL;
}

static gboolean
ns_video_stream_growing(ns_video_cache *cache, const ns_video *v)
{
    if (v->mse_id) {
        ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                               GUINT_TO_POINTER(v->mse_id));
        return !s || !s->eos;
    }
    return ns_video_url_is_growing_stream(v->url);
}

static guint
ns_mse_url_id(const char *url)
{
    if (!url || !g_str_has_prefix(url, "blob:nd-mse/")) return 0;
    return (guint)g_ascii_strtoull(url + strlen("blob:nd-mse/"), NULL, 10);
}

static char *
ns_video_stream_key(const char *abs_url)
{
    guint mse_id = ns_mse_url_id(abs_url);
    if (mse_id) return g_strdup_printf("ndmse:%u", mse_id);
    const char *marker = strstr(abs_url, "#ndms=");
    return marker ? g_strndup(abs_url, (gsize)(marker - abs_url))
                  : g_strdup(abs_url);
}

static gboolean
extend_timed(ns_video_player *player, const guint8 *data, gsize len)
{
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = ns_video_player_extend(player, data, len);
    if (g_getenv("NS_PROFILE"))
        fprintf(stderr, "[profile] mse extend %6.1fms  %zub ok=%d\n",
                (double)(g_get_monotonic_time() - t0) / 1000.0,
                (size_t)len, ok);
    return ok;
}

static void
on_extend_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    ns_video *v = pending->video;
    if (resp && !resp->error && resp->body && resp->body->len > 0 &&
        resp->body->len <= NS_VIDEO_MAX_BYTES) {
        if (!v->player) {
            ns_video_build_player(pending, resp);
            v->loaded = TRUE;
        } else if (extend_timed(v->player, resp->body->data,
                                resp->body->len)) {
            v->buf_sent = FALSE;
            if (v->has_audio && !v->audio_url)
                ns_video_materialize_audio(pending->cache, v,
                                           resp->body->data,
                                           resp->body->len, 0);
        } else {
            ns_video_player *fresh =
                ns_video_player_new(resp->body->data, resp->body->len);
            if (fresh) {
                ns_video_player_free(v->player);
                v->player = fresh;
                v->duration = ns_video_player_duration(fresh);
                v->has_audio = ns_video_player_has_audio(fresh);
                v->buf_sent = FALSE;
                if (v->playing)
                    v->base_us = g_get_monotonic_time()
                               - (gint64)(v->cur_time * 1e6);
            }
        }
    }
    g_clear_error(&err);
    ns_response_free(resp);
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

static void
ns_video_check_audio_attr(ns_video_cache *cache, ns_video *v,
                          const ns_node *dom)
{
    const char *asrc = dom ? ns_element_get_attr(dom, "data-audio-src") : NULL;
    if (!asrc || !*asrc || !g_str_has_prefix(asrc, "blob:")) return;
    if (v->audio_url && strcmp(v->audio_url, asrc) == 0) return;
    g_free(v->audio_url);
    v->audio_url = g_strdup(asrc);
    ns_pending *pa = g_new0(ns_pending, 1);
    pa->video = v;
    pa->cache = cache;
    g_ptr_array_add(cache->pending, pa);
    ns_net_fetch_async(asrc, cache->base_url, NULL, on_msaudio_fetched, pa);
}

static void
ns_video_refresh_growing_stream(ns_video_cache *cache, ns_video *v,
                                gint64 now_us)
{
    if (v->mse_id) return;
    if (!v->url || !v->player) return;
    if (now_us - v->last_refresh_us < G_GINT64_CONSTANT(500000)) return;
    v->last_refresh_us = now_us;
    char *base = ns_video_stream_key(v->url);
    ns_pending *pe = g_new0(ns_pending, 1);
    pe->video = v;
    pe->cache = cache;
    g_ptr_array_add(cache->pending, pe);
    ns_net_fetch_async(base, cache->base_url, NULL, on_extend_fetched, pe);
    g_free(base);
    ns_video_check_audio_attr(cache, v, v->dom_node);
}

static void
ns_video_note_stream_version(ns_video_cache *cache, ns_video *v,
                             const ns_node *dom, const char *abs_url)
{
    if (!v || v->is_camera || !abs_url) return;
    if (v->url && strcmp(v->url, abs_url) == 0) return;
    if (g_hash_table_contains(cache->requested, abs_url)) return;
    g_hash_table_add(cache->requested, g_strdup(abs_url));
    g_free(v->url);
    v->url = g_strdup(abs_url);
    ns_pending *pe = g_new0(ns_pending, 1);
    pe->video = v;
    pe->cache = cache;
    g_ptr_array_add(cache->pending, pe);
    ns_net_fetch_async(abs_url, cache->base_url, NULL,
                       on_extend_fetched, pe);
    ns_video_check_audio_attr(cache, v, dom);
}

static void
ns_video_build_player(ns_pending *pending, ns_response *resp)
{
    ns_video *v = pending->video;
    if (!resp || resp->error || !resp->body || resp->body->len == 0 ||
        resp->body->len > NS_VIDEO_MAX_BYTES)
        return;

    ns_video_player *player = ns_video_player_new(resp->body->data,
                                                  resp->body->len);
    if (!player) return;

    v->player = player;
    v->duration = ns_video_player_duration(player);
    v->has_audio = ns_video_player_has_audio(player);
    if (v->natural_width <= 0)  v->natural_width  = ns_video_player_width(player);
    if (v->natural_height <= 0) v->natural_height = ns_video_player_height(player);

    {
        gboolean ended = FALSE;
        ns_texture *frame = ns_video_player_frame_at(player, v->cur_time,
                                                     v->loop, &ended);
        if (frame) {
            ns_texture_unref(v->frame_texture);
            v->frame_texture = ns_texture_ref(frame);
        }
    }

    if (v->has_audio && !v->audio_file && v->url &&
        g_str_has_prefix(v->url, "blob:"))
        ns_video_materialize_audio(pending->cache, v,
                                   resp->body->data, resp->body->len, 0);

    if (v->playing) {
        if (v->base_us == 0)
            v->base_us = g_get_monotonic_time()
                       - (gint64)(v->cur_time * 1e6);
        ns_video_audio_start(pending->cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(pending->cache, "seek %s %.3f",
                                v->token, v->cur_time);
    } else if (v->autoplay) {
        ns_video_play(v, g_get_monotonic_time());
        ns_video_audio_start(pending->cache, v);
        ns_video_emit_js(pending->cache, v, "play", v->cur_time);
    }
}

static gboolean
ns_video_stream_file_append(const char *file_url, const guint8 *data,
                            gsize len, gsize *written)
{
    if (!file_url || !g_str_has_prefix(file_url, "file://") ||
        *written > len)
        return FALSE;
    if (*written == len) return TRUE;
    FILE *f = g_fopen(file_url + 7, "ab");
    if (!f) return FALSE;
    gsize wrote = fwrite(data + *written, 1, len - *written, f);
    fclose(f);
    if (wrote != len - *written) return FALSE;
    *written = len;
    return TRUE;
}

static char *
ns_video_stream_file_create(ns_video_cache *cache, const char *subdir,
                            char prefix, const guint8 *data, gsize len)
{
    char *dir = g_build_filename(g_get_user_cache_dir(),
                                 "nordstjernen", subdir, NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_strdup_printf("%s/%c%d-%u.dat", dir, prefix,
                                 (int)getpid(), ++cache->next_token);
    char *file_url = NULL;
    if (g_file_set_contents(path, (const char *)data, (gssize)len, NULL))
        file_url = g_strdup_printf("file://%s", path);
    g_free(path);
    g_free(dir);
    return file_url;
}

static void
ns_video_materialize_audio(ns_video_cache *cache, ns_video *v,
                           const guint8 *data, gsize len, guint gen)
{
    if (!(v->audio_file && gen == v->audio_file_gen &&
          ns_video_stream_file_append(v->audio_file, data, len,
                                      &v->audio_file_len))) {
        char *file_url = ns_video_stream_file_create(cache, "msaudio", 'a',
                                                     data, len);
        if (!file_url) return;
        char *old_file = v->audio_file;
        v->audio_file = file_url;
        v->audio_file_len = len;
        v->audio_file_gen = gen;
        if (old_file) {
            if (g_str_has_prefix(old_file, "file://"))
                g_unlink(old_file + 7);
            g_free(old_file);
        }
    }
    if (v->audio_opened) {
        ns_video_emit_audio(cache, "reload %s %s", v->token, v->audio_file);
        if (v->playing && !v->muted)
            ns_video_emit_audio(cache, "play %s", v->token);
    } else if (v->playing && !v->muted) {
        ns_video_audio_start(cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(cache, "seek %s %.3f",
                                v->token, v->cur_time);
    }
}

static void
ns_video_materialize_video(ns_video_cache *cache, ns_video *v,
                           const guint8 *data, gsize len, guint gen)
{
    if (!ns_video_helper_enabled() || !cache->audio_cb) return;
    gboolean grew = v->video_file && gen == v->video_file_gen &&
        ns_video_stream_file_append(v->video_file, data, len,
                                    &v->video_file_len);
    if (!grew) {
        char *file_url = ns_video_stream_file_create(cache, "msvideo", 'v',
                                                     data, len);
        if (!file_url) return;
        char *old_file = v->video_file;
        v->video_file = file_url;
        v->video_file_len = len;
        v->video_file_gen = gen;
        if (old_file) {
            if (g_str_has_prefix(old_file, "file://"))
                g_unlink(old_file + 7);
            g_free(old_file);
        }
    }
    if (!v->token)
        v->token = g_strdup_printf("nv%u", ++cache->next_token);
    if (!v->video_opened) {
        ns_video_emit_audio(cache, "video open %s %s", v->token,
                            v->video_file);
        v->video_opened = TRUE;
        if (v->cur_time > 0)
            ns_video_helper_seek(cache, v, v->cur_time);
        if (v->playing)
            ns_video_helper_playpause(cache, v, TRUE);
    } else if (!grew) {
        ns_video_emit_audio(cache, "video reload %s %s", v->token,
                            v->video_file);
    }
}

static ns_video *
ns_video_for_mse_id(ns_video_cache *cache, guint id)
{
    char key[32];
    g_snprintf(key, sizeof key, "ndmse:%u", id);
    return g_hash_table_lookup(cache->by_url, key);
}

static void
ns_video_mse_attach_player(ns_video *v, ns_mse_stream *s)
{
    if (v->player || !s->video_bytes || s->video_bytes->len < 4096)
        return;
    ns_video_player *player = ns_video_player_new(s->video_bytes->data,
                                                  s->video_bytes->len);
    if (!player) return;
    v->player = player;
    v->duration = ns_video_player_duration(player);
    v->has_audio = ns_video_player_has_audio(player);
    if (v->natural_width <= 0)
        v->natural_width = ns_video_player_width(player);
    if (v->natural_height <= 0)
        v->natural_height = ns_video_player_height(player);
    v->loaded = TRUE;
    if (!ns_video_helper_enabled()) {
        gboolean ended = FALSE;
        ns_texture *frame = ns_video_player_frame_at(player, v->cur_time,
                                                     v->loop, &ended);
        if (frame) {
            ns_texture_unref(v->frame_texture);
            v->frame_texture = ns_texture_ref(frame);
        }
    }
    if (v->playing && v->base_us == 0)
        v->base_us = g_get_monotonic_time() - (gint64)(v->cur_time * 1e6);
}

static void
ns_video_mse_sync(ns_video_cache *cache, ns_mse_stream *s, ns_video *v)
{
    if (!v || !s) return;
    gboolean helper_owns = ns_video_helper_enabled() &&
                           (v->video_opened || v->mse_id != 0);
    if (!v->player) {
        ns_video_mse_attach_player(v, s);
        if (v->player) v->buf_sent = FALSE;
    } else if (!helper_owns && s->video_bytes &&
               s->video_bytes->len > 0 &&
               ns_video_player_extend(v->player, s->video_bytes->data,
                                      s->video_bytes->len)) {
        v->buf_sent = FALSE;
    } else if (helper_owns && s->video_bytes && s->video_bytes->len > 0) {
        v->buf_sent = FALSE;
    }
    if (v->player && s->video_end > 0.0) {
        ns_video_player_note_end(v->player, s->video_end);
        double d = ns_video_player_duration(v->player);
        if (d > v->duration) v->duration = d;
    }
    gint64 now = g_get_monotonic_time();
    if (s->video_bytes && s->video_bytes->len > 0 && s->video_dirty &&
        (s->eos || now - s->video_flush_us > G_GINT64_CONSTANT(250000))) {
        s->video_dirty = FALSE;
        s->video_flush_us = now;
        ns_video_materialize_video(cache, v, s->video_bytes->data,
                                   s->video_bytes->len, s->video_gen);
    }
    gboolean has_side_audio = s->audio_bytes && s->audio_bytes->len > 0;
    if (has_side_audio && s->audio_dirty &&
        (s->eos || now - s->audio_flush_us > G_GINT64_CONSTANT(250000))) {
        s->audio_dirty = FALSE;
        s->audio_flush_us = now;
        ns_video_materialize_audio(cache, v, s->audio_bytes->data,
                                   s->audio_bytes->len, s->audio_gen);
        if (s->audio_rebased) {
            s->audio_rebased = FALSE;
            if (v->audio_opened)
                ns_video_emit_audio(cache, "seek %s 0", v->token);
        }
    }
    if (!has_side_audio && v->player && v->has_audio && !v->audio_file &&
        s->video_bytes && s->video_bytes->len > 0)
        ns_video_materialize_audio(cache, v, s->video_bytes->data,
                                   s->video_bytes->len, s->video_gen);
}

gboolean
ns_video_cache_mse_append(ns_video_cache *cache, guint stream_id, char kind,
                          const guint8 *data, gsize len)
{
    if (!cache || !stream_id || !data || !len) return FALSE;
    ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                           GUINT_TO_POINTER(stream_id));
    if (!s) {
        s = g_new0(ns_mse_stream, 1);
        s->id = stream_id;
        g_hash_table_insert(cache->mse_streams,
                            GUINT_TO_POINTER(stream_id), s);
    }
    GByteArray **dst = kind == 'a' ? &s->audio_bytes : &s->video_bytes;
    GByteArray **init = kind == 'a' ? &s->audio_init : &s->video_init;
    double *appended_end = kind == 'a' ? &s->audio_end : &s->video_end;
    if (!*dst) *dst = g_byte_array_new();
    ns_video *sv = ns_video_for_mse_id(cache, stream_id);

    gboolean is_init = ns_mse_bytes_are_init_segment(data, len);
    if (is_init && *init && (*dst)->len > 0 && (*init)->len == len &&
        memcmp((*init)->data, data, len) == 0)
        return TRUE;
    gboolean new_generation = (*dst)->len > 0 && is_init;
    if (new_generation) {
        g_byte_array_set_size(*dst, 0);
        if (kind == 'a') {
            s->audio_rebased = TRUE;
            s->audio_gen++;
        } else {
            s->video_gen++;
            if (sv && sv->player) {
                ns_video_player_free(sv->player);
                sv->player = NULL;
            }
        }
    }
    if (is_init) {
        if (!*init) *init = g_byte_array_new();
        g_byte_array_set_size(*init, 0);
        g_byte_array_append(*init, data, len);
    }
    if ((*dst)->len + len > NS_VIDEO_MAX_BYTES) return FALSE;
    g_byte_array_append(*dst, data, len);
    if (kind == 'a') s->audio_dirty = TRUE;
    else s->video_dirty = TRUE;

    if (!is_init) {
        double chunk_end = ns_video_probe_chunk_end(
            *init ? (*init)->data : NULL, *init ? (*init)->len : 0,
            data, len);
        if (chunk_end > *appended_end) *appended_end = chunk_end;
    }

    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[mse-append] id=%u kind=%c len=%zu total=%u vend=%.1f "
                   "aend=%.1f%s\n",
                   stream_id, kind, len, (*dst)->len, s->video_end,
                   s->audio_end, new_generation ? " NEW-GEN" : "");
    ns_video_mse_sync(cache, s, sv);
    return TRUE;
}

double
ns_video_cache_mse_buffered(ns_video_cache *cache, guint stream_id, char kind)
{
    if (!cache || !stream_id) return 0.0;
    ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                           GUINT_TO_POINTER(stream_id));
    if (!s) return 0.0;
    return kind == 'a' ? s->audio_end : s->video_end;
}

void
ns_video_cache_mse_eos(ns_video_cache *cache, guint stream_id)
{
    if (!cache || !stream_id) return;
    ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                           GUINT_TO_POINTER(stream_id));
    if (!s) return;
    s->eos = TRUE;
    ns_video_mse_sync(cache, s, ns_video_for_mse_id(cache, stream_id));
}

static void
on_msaudio_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    ns_video_cache *cache = pending->cache;
    ns_video *v = pending->video;
    if (!pending->dead && resp && !resp->error && resp->body &&
        resp->body->len > 0 && resp->body->len <= NS_VIDEO_MAX_BYTES)
        ns_video_materialize_audio(cache, v, resp->body->data,
                                   resp->body->len, 0);
    g_clear_error(&err);
    if (resp) ns_response_free(resp);
    if (!pending->dead)
        g_ptr_array_remove_fast(cache->pending, pending);
    g_free(pending);
}

static void
on_video_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    ns_video_build_player(pending, resp);
    g_clear_error(&err);
    ns_response_free(resp);
    pending->video->loaded = TRUE;
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

static void
on_poster_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    if (resp && !resp->error && resp->body && resp->body->len > 0) {
        int w = 0, h = 0;
        ns_texture *tex = ns_image_decode_bytes(resp->body->data,
                                                resp->body->len, &w, &h);
        if (tex) {
            pending->video->poster_texture = tex;
            if (pending->video->natural_width  <= 0) pending->video->natural_width  = w;
            if (pending->video->natural_height <= 0) pending->video->natural_height = h;
        }
    }
    g_clear_error(&err);
    ns_response_free(resp);
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

static void
on_track_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    ns_video *v = pending->video;
    if (resp && !resp->error && resp->body && resp->body->len > 0 &&
        resp->body->len <= NS_VIDEO_MAX_BYTES) {
        char *text = g_strndup((const char *)resp->body->data, resp->body->len);
        GPtrArray *cues = ns_vtt_parse(text);
        g_free(text);
        if (v->cues) g_ptr_array_free(v->cues, TRUE);
        v->cues = cues;
    }
    g_clear_error(&err);
    ns_response_free(resp);
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

static const ns_node *
ns_video_pick_track(const ns_node *dom)
{
    for (const ns_node *c = dom->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "track") != 0) continue;
        if (!ns_element_get_attr(c, "default")) continue;
        const char *kind = ns_element_get_attr(c, "kind");
        if (kind && *kind &&
            g_ascii_strcasecmp(kind, "subtitles") != 0 &&
            g_ascii_strcasecmp(kind, "captions") != 0)
            continue;
        const char *tsrc = ns_element_get_attr(c, "src");
        if (tsrc && *tsrc) return c;
    }
    return NULL;
}

static void
ns_video_discover_track(ns_video_cache *cache, ns_video *v, const ns_node *dom)
{
    if (!v || !dom || v->track_requested) return;
    const ns_node *track = ns_video_pick_track(dom);
    if (!track) return;
    const char *tsrc = ns_element_get_attr(track, "src");
    char *abs = ns_url_resolve(cache->base_url, tsrc);
    if (!abs) return;
    v->track_requested = TRUE;
    ns_pending *pt = g_new0(ns_pending, 1);
    pt->video = v;
    pt->cache = cache;
    g_ptr_array_add(cache->pending, pt);
    ns_net_fetch_async(abs, cache->base_url, NULL, on_track_fetched, pt);
    g_free(abs);
}

static gboolean
attr_present(const ns_node *n, const char *name)
{
    const char *v = n ? ns_element_get_attr(n, name) : NULL;
    return v != NULL;
}

static gboolean url_is_inline_video(const char *url);

gboolean
ns_video_url_is_inline(const char *url)
{
    return url_is_inline_video(url);
}

static gboolean
url_is_inline_video(const char *url)
{
    if (!url) return FALSE;
    if (g_str_has_prefix(url, "blob:")) return TRUE;
    gsize n = strcspn(url, "?#");
    static const char *const exts[] = {
        ".mpg", ".mpeg", ".m1v", ".mpeg1", ".mpg1",
#ifdef NS_HAVE_LIBAV
        ".mp4", ".m4v", ".webm",
#endif
        NULL,
    };
    for (int i = 0; exts[i]; i++) {
        gsize el = strlen(exts[i]);
        if (n >= el && g_ascii_strncasecmp(url + n - el, exts[i], el) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
ns_video_cache_start(ns_video_cache *cache, const ns_node *dom, ns_box *box,
                     const char *abs_url, const char *poster_abs)
{
    guint mse_id = ns_mse_url_id(abs_url);
    char *key = mse_id ? g_strdup_printf("ndmse:%u", mse_id)
                       : ns_video_stream_key(abs_url);
    ns_video *v = g_hash_table_lookup(cache->by_url, key);
    if (v) {
        if (box) box->media->video = v;
        if (!mse_id)
            ns_video_note_stream_version(cache, v, dom, abs_url);
        g_free(key);
        return;
    }

    v = g_new0(ns_video, 1);
    v->url = g_strdup(abs_url);
    v->dom_node = dom;
    v->autoplay = attr_present(dom, "autoplay");
    v->loop = attr_present(dom, "loop");
    v->controls = attr_present(dom, "controls");
    v->muted = attr_present(dom, "muted");
    v->volume = 1.0;
    v->cur_time = 0.0;
    v->seq = ++cache->next_seq;
    v->mse_id = mse_id;
    g_hash_table_insert(cache->by_url, key, v);
    if (box) box->media->video = v;
    if (dom) ns_video_apply_node_state(cache, v, dom);
    if (dom) ns_video_discover_track(cache, v, dom);

    if (mse_id) {
        ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                               GUINT_TO_POINTER(mse_id));
        if (s) ns_video_mse_sync(cache, s, v);
        return;
    }

    ns_pending *pv = g_new0(ns_pending, 1);
    pv->video = v;
    pv->cache = cache;
    g_ptr_array_add(cache->pending, pv);
    ns_net_fetch_async(abs_url, cache->base_url, NULL, on_video_fetched, pv);

    const char *asrc = ns_element_get_attr(dom, "data-audio-src");
    if (asrc && *asrc && g_str_has_prefix(asrc, "blob:")) {
        v->audio_url = g_strdup(asrc);
        ns_pending *pa = g_new0(ns_pending, 1);
        pa->video = v;
        pa->cache = cache;
        g_ptr_array_add(cache->pending, pa);
        ns_net_fetch_async(asrc, cache->base_url, NULL,
                           on_msaudio_fetched, pa);
    }

    if (poster_abs && *poster_abs) {
        ns_pending *pp = g_new0(ns_pending, 1);
        pp->video = v;
        pp->cache = cache;
        g_ptr_array_add(cache->pending, pp);
        ns_net_fetch_async(poster_abs, cache->base_url, NULL, on_poster_fetched, pp);
    }
}

static void
ns_video_discover_dom(ns_video_cache *cache, const ns_node *node)
{
    if (!node) return;
    if (node->kind == NS_NODE_ELEMENT && node->name &&
        strcmp(node->name, "video") == 0) {
        const char *src = ns_element_get_attr(node, "src");
        if (!src || !*src) src = ns_element_get_attr(node, NS_MEDIA_SRC_ATTR);
        if (src && *src) {
            char *abs = g_str_has_prefix(src, "blob:")
                ? g_strdup(src)
                : ns_url_resolve(cache->base_url, src);
            if (abs && url_is_inline_video(abs) &&
                (g_str_has_prefix(abs, "http://") ||
                 g_str_has_prefix(abs, "https://") ||
                 g_str_has_prefix(abs, "file://") ||
                 g_str_has_prefix(abs, "blob:"))) {
                char *skey = ns_video_stream_key(abs);
                ns_video *existing = g_hash_table_lookup(cache->by_url, skey);
                g_free(skey);
                if (existing) {
                    existing->dom_node = node;
                    ns_video_apply_node_state(cache, existing, node);
                    ns_video_discover_track(cache, existing, node);
                    ns_video_note_stream_version(cache, existing, node, abs);
                } else if (!g_hash_table_contains(cache->requested, abs)) {
                    g_hash_table_add(cache->requested, g_strdup(abs));
                    const char *poster_raw =
                        ns_element_get_attr(node, "poster");
                    char *poster = (poster_raw && *poster_raw)
                        ? ns_url_resolve(cache->base_url, poster_raw) : NULL;
                    ns_video_cache_start(cache, node, NULL, abs, poster);
                    g_free(poster);
                }
            }
            g_free(abs);
        }
    }
    for (const ns_node *c = node->first_child; c; c = c->next_sibling)
        ns_video_discover_dom(cache, c);
}

void
ns_video_cache_discover(ns_video_cache *cache, const ns_box *root,
                        const ns_node *doc, gint64 now_us)
{
    (void)now_us;
    if (!cache || !root) return;

    GPtrArray *vids = g_ptr_array_new();
    ns_layout_collect_videos(root, vids);
    for (guint i = 0; i < vids->len; i++) {
        ns_box *box = g_ptr_array_index(vids, i);
        if (!box->media || !box->dom) continue;
        if (box->media->video) continue;
        const char *stream = ns_element_get_attr(box->dom, NS_MEDIA_STREAM_ATTR);
        if (stream && g_strcmp0(stream, "camera") == 0) {
            char *ckey = g_strdup_printf("camera:%p", (const void *)box->dom);
            ns_video *cv = g_hash_table_lookup(cache->by_url, ckey);
            if (!cv) {
                cv = g_new0(ns_video, 1);
                cv->is_camera = TRUE;
                cv->dom_node = box->dom;
                cv->playing = TRUE;
                g_hash_table_insert(cache->by_url, g_strdup(ckey), cv);
            }
            box->media->video = cv;
            g_free(ckey);
            continue;
        }
        const char *src = box->media->video_src;
        if (!src || !*src) continue;
        char *abs = g_str_has_prefix(src, "blob:") ? g_strdup(src)
                                                   : ns_url_resolve(cache->base_url, src);
        if (!abs) continue;
        if ((!g_str_has_prefix(abs, "http://") && !g_str_has_prefix(abs, "https://") &&
             !g_str_has_prefix(abs, "file://") && !g_str_has_prefix(abs, "blob:")) ||
            !url_is_inline_video(abs)) {
            g_free(abs);
            continue;
        }
        char *poster = NULL;
        if (box->media->video_poster)
            poster = ns_url_resolve(cache->base_url, box->media->video_poster);

        char *skey = ns_video_stream_key(abs);
        ns_video *existing = g_hash_table_lookup(cache->by_url, skey);
        g_free(skey);
        if (existing) {
            box->media->video = existing;
            if (box->dom) {
                existing->dom_node = box->dom;
                ns_video_apply_node_state(cache, existing, box->dom);
                ns_video_discover_track(cache, existing, box->dom);
            }
            ns_video_note_stream_version(cache, existing, box->dom, abs);
        } else if (!g_hash_table_contains(cache->requested, abs)) {
            g_hash_table_add(cache->requested, g_strdup(abs));
            ns_video_cache_start(cache, box->dom, box, abs, poster);
        }
        g_free(poster);
        g_free(abs);
    }
    if (doc)
        ns_video_discover_dom(cache, doc);

    g_ptr_array_free(vids, TRUE);
}

typedef struct {
    ns_video   *v;
    const char *kind;
    double      value;
} ns_video_emit_rec;

static void
ns_video_queue_emit(GArray *q, ns_video *v, const char *kind, double value)
{
    ns_video_emit_rec rec = { v, kind, value };
    g_array_append_val(q, rec);
}

gboolean
ns_video_cache_tick(ns_video_cache *cache, gint64 now_us)
{
    if (!cache) return FALSE;
    gboolean changed = FALSE;
    GArray *emits = g_array_new(FALSE, FALSE, sizeof(ns_video_emit_rec));
    GHashTableIter it;
    gpointer key, val;
    guint opened_count = 0;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *c = val;
        if (c->video_opened || c->audio_opened) opened_count++;
    }
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *v = val;
        if (opened_count >= 2 && (v->video_opened || v->audio_opened) &&
            v->last_paint_us > 0 &&
            now_us - v->last_paint_us > (gint64)3000000) {
            ns_video_audio_stop(cache, v);
            ns_video_helper_stop(cache, v);
            v->playing = FALSE;
            opened_count--;
            continue;
        }
        if (v->is_camera) {
            ns_camera *cam = ns_camera_active();
            if (cam) {
                ns_texture *frame = ns_camera_next_frame(cam);
                if (frame) {
                    ns_texture_unref(v->frame_texture);
                    v->frame_texture = frame;
                    v->natural_width  = ns_texture_get_width(frame);
                    v->natural_height = ns_texture_get_height(frame);
                    changed = TRUE;
                }
            }
            continue;
        }
        if (!v->player) continue;

        if (!v->meta_sent && v->duration > 0.0) {
            v->meta_sent = TRUE;
            ns_video_queue_emit(emits, v, "meta", v->duration);
            if (v->natural_width > 0)
                ns_video_queue_emit(emits, v, "vwidth", (double)v->natural_width);
            if (v->natural_height > 0)
                ns_video_queue_emit(emits, v, "vheight", (double)v->natural_height);
        }
        if (!v->buf_sent) {
            v->buf_sent = TRUE;
            double buffered_end = ns_video_player_buffered_end(v->player);
            if (buffered_end <= 0.0) buffered_end = v->duration;
            if (buffered_end > 0.0)
                ns_video_queue_emit(emits, v, "buf", buffered_end);
        }
        gboolean helper_owns = ns_video_helper_enabled() &&
                               (v->video_opened || v->mse_id != 0);
        gboolean helper = ns_video_helper_enabled() && v->video_opened;
        if (helper)
            ns_video_helper_flush_rect(cache, v, now_us);
        if (!v->playing) continue;

        if (!v->audio_opened && !v->muted &&
            (v->has_audio || v->audio_file)) {
            ns_video_audio_start(cache, v);
            if (v->audio_opened && v->cur_time > 0)
                ns_video_emit_audio(cache, "seek %s %.3f", v->token,
                                    v->cur_time);
        }

        if (v->base_us == 0)
            v->base_us = now_us - (gint64)(v->cur_time * 1e6);
        double elapsed = (double)(now_us - v->base_us) / 1e6;
        if (g_getenv("NS_DBG_CLOCK") && v->mse_id)
            g_printerr("[clock] elapsed=%.2f prev=%.2f cur=%.2f edge=%.2f\n",
                       elapsed, v->prev_tick_time, v->cur_time,
                       ns_video_player_buffered_end(v->player));
        if (elapsed - v->prev_tick_time > 1.0) {
            elapsed = v->prev_tick_time + 1.0;
            v->base_us = now_us - (gint64)(elapsed * 1e6);
            if (v->audio_opened && !helper)
                ns_video_emit_audio(cache, "seek %s %.3f", v->token, elapsed);
        }
        double t = elapsed;
        if (v->loop && v->duration > 0.0)
            t = fmod(elapsed, v->duration);

        if (ns_video_stream_growing(cache, v)) {
            double edge = ns_video_player_buffered_end(v->player) - 0.35;
            if (edge < 0) edge = 0;
            if (t > edge) {
                ns_video_refresh_growing_stream(cache, v, now_us);
                v->base_us = now_us - (gint64)(edge * 1e6);
                t = edge;
                if (!v->stall_since_us)
                    v->stall_since_us = now_us;
                if (!v->stalled &&
                    now_us - v->stall_since_us > G_GINT64_CONSTANT(1500000)) {
                    v->stalled = TRUE;
                    ns_video_queue_emit(emits, v, "waiting", edge);
                }
            } else {
                v->stall_since_us = 0;
            }
        }

        if (v->loop && t + 1e-3 < v->prev_tick_time)
            ns_video_audio_resync(cache, v);
        v->prev_tick_time = t;

        gboolean ended = FALSE;
        ns_texture *frame = helper_owns ? NULL
            : ns_video_player_frame_at(v->player, t, v->loop, &ended);
        if (helper_owns && !v->loop && v->duration > 0.0 && t >= v->duration)
            ended = TRUE;
        if (helper && v->stalled &&
            t + 0.5 < ns_video_player_buffered_end(v->player)) {
            v->stalled = FALSE;
            ns_video_queue_emit(emits, v, "resumed", t);
        }
        if (frame) {
            ns_texture_unref(v->frame_texture);
            v->frame_texture = ns_texture_ref(frame);
            changed = TRUE;
            if (v->stalled) {
                v->stalled = FALSE;
                ns_video_queue_emit(emits, v, "resumed", t);
            }
        }
        v->cur_time = t;
        if (t - v->last_emit_time >= 0.20 || ended) {
            v->last_emit_time = t;
            ns_video_queue_emit(emits, v, "pos", v->cur_time);
        }
        if (ended) {
            if (ns_video_stream_growing(cache, v)) {
                v->prev_tick_time = v->cur_time;
                v->base_us = now_us - (gint64)(v->cur_time * 1e6);
                if (!v->stalled) {
                    v->stalled = TRUE;
                    ns_video_queue_emit(emits, v, "waiting", v->cur_time);
                }
                ns_video_refresh_growing_stream(cache, v, now_us);
                continue;
            }
            v->playing = FALSE;
            v->ended = TRUE;
            v->cur_time = v->duration;
            ns_video_audio_stop(cache, v);
            ns_video_helper_playpause(cache, v, FALSE);
            ns_video_queue_emit(emits, v, "ended", v->duration);
        }
    }
    for (guint ei = 0; ei < emits->len; ei++) {
        ns_video_emit_rec *rec =
            &g_array_index(emits, ns_video_emit_rec, ei);
        ns_video_emit_js(cache, rec->v, rec->kind, rec->value);
    }
    g_array_free(emits, TRUE);
    return changed;
}

gboolean
ns_video_cache_has_pending(const ns_video_cache *cache)
{
    return cache && cache->pending->len > 0;
}

gboolean
ns_video_cache_waiting_growth(const ns_video_cache *cache)
{
    if (!cache) return FALSE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, ((ns_video_cache *)cache)->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *v = val;
        if (v->playing && v->stalled) return TRUE;
    }
    return FALSE;
}

gboolean
ns_video_cache_animating(const ns_video_cache *cache)
{
    if (!cache) return FALSE;
    if (cache->pending->len > 0) return TRUE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *v = val;
        if (v->is_camera && ns_camera_active()) return TRUE;
        if (v->player && v->playing) return TRUE;
    }
    return FALSE;
}
