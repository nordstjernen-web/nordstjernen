/* Nordstjernen — Opus audio decode + PulseAudio output cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "audio.h"

#include <string.h>

#include "net.h"
#include "webm.h"

#ifdef ND_HAVE_AUDIO
#include <opus/opus.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#endif

struct nd_audio {
    char           *url;
    nd_audio_cache *cache;
    GByteArray     *body;
    gboolean        loaded;
    gboolean        failed;
    gboolean        loop;
    GThread        *thread;
    GMutex          lock;
    gint            stop;
    gint            started;
};

typedef struct nd_audio_pending {
    nd_audio       *audio;
    nd_audio_cache *cache;
    gboolean        dead;
} nd_audio_pending;

struct nd_audio_cache {
    GHashTable *by_url;
    GPtrArray  *pending;
};

gboolean
nd_audio_available(void)
{
#ifdef ND_HAVE_AUDIO
    return TRUE;
#else
    return FALSE;
#endif
}

#ifdef ND_HAVE_AUDIO

#define ND_AUDIO_MAX_FRAME 5760

static void
audio_play_loop(nd_audio *a)
{
    nd_webm *demux = nd_webm_open(a->body->data, a->body->len);
    if (!demux) return;
    const nd_webm_track *t = nd_webm_audio_track(demux);
    if (!t || !t->codec_id || strcmp(t->codec_id, "A_OPUS") != 0) {
        nd_webm_close(demux);
        return;
    }
    int channels = t->channels > 0 ? t->channels : 2;
    if (channels > 2) channels = 2;
    int err = 0;
    OpusDecoder *dec = opus_decoder_create(48000, channels, &err);
    if (!dec || err != OPUS_OK) {
        if (dec) opus_decoder_destroy(dec);
        nd_webm_close(demux);
        return;
    }
    pa_sample_spec spec = {
        .format   = PA_SAMPLE_S16NE,
        .rate     = 48000,
        .channels = (guint8)channels,
    };
    pa_buffer_attr attr = {
        .maxlength = (guint32)-1,
        .tlength   = (guint32)(48000 * channels * 2 / 10),
        .prebuf    = (guint32)-1,
        .minreq    = (guint32)-1,
        .fragsize  = (guint32)-1,
    };
    int pa_err = 0;
    pa_simple *pa = pa_simple_new(NULL, "Nordstjernen", PA_STREAM_PLAYBACK,
                                  NULL, "video", &spec, NULL, &attr, &pa_err);
    if (!pa) {
        opus_decoder_destroy(dec);
        nd_webm_close(demux);
        return;
    }
    short *pcm = g_malloc(sizeof(short) * ND_AUDIO_MAX_FRAME * channels);
    for (;;) {
        if (g_atomic_int_get(&a->stop)) break;
        nd_webm_frame f;
        if (!nd_webm_next_audio_frame(demux, &f)) {
            gboolean loop;
            g_mutex_lock(&a->lock);
            loop = a->loop;
            g_mutex_unlock(&a->lock);
            if (!loop) break;
            nd_webm_seek_start(demux);
            continue;
        }
        int decoded = opus_decode(dec, f.data, (opus_int32)f.len,
                                  pcm, ND_AUDIO_MAX_FRAME, 0);
        if (decoded <= 0) continue;
        if (pa_simple_write(pa, pcm,
                            (size_t)decoded * channels * sizeof(short),
                            &pa_err) < 0)
            break;
    }
    pa_simple_drain(pa, &pa_err);
    pa_simple_free(pa);
    g_free(pcm);
    opus_decoder_destroy(dec);
    nd_webm_close(demux);
}

static gpointer
audio_thread_main(gpointer user_data)
{
    nd_audio *a = user_data;
    audio_play_loop(a);
    return NULL;
}

static void
audio_maybe_start(nd_audio *a)
{
    if (!a->loaded || a->failed) return;
    if (!g_atomic_int_compare_and_exchange(&a->started, 0, 1)) return;
    a->thread = g_thread_new("nd-audio", audio_thread_main, a);
}

#endif

static void
nd_audio_free(gpointer p)
{
    nd_audio *a = p;
    if (!a) return;
#ifdef ND_HAVE_AUDIO
    g_atomic_int_set(&a->stop, 1);
    if (a->thread) {
        g_thread_join(a->thread);
        a->thread = NULL;
    }
    g_mutex_clear(&a->lock);
#endif
    if (a->body) g_byte_array_unref(a->body);
    g_free(a->url);
    g_free(a);
}

nd_audio_cache *
nd_audio_cache_new(void)
{
    nd_audio_cache *c = g_new0(nd_audio_cache, 1);
    c->by_url  = g_hash_table_new_full(g_str_hash, g_str_equal,
                                       g_free, nd_audio_free);
    c->pending = g_ptr_array_new();
    return c;
}

void
nd_audio_cache_free(nd_audio_cache *cache)
{
    if (!cache) return;
    for (guint i = 0; i < cache->pending->len; i++) {
        nd_audio_pending *pp = g_ptr_array_index(cache->pending, i);
        pp->dead = TRUE;
    }
    g_hash_table_destroy(cache->by_url);
    g_ptr_array_free(cache->pending, TRUE);
    g_free(cache);
}

void
nd_audio_set_loop(nd_audio *a, gboolean loop)
{
    if (!a) return;
#ifdef ND_HAVE_AUDIO
    g_mutex_lock(&a->lock);
    a->loop = loop;
    g_mutex_unlock(&a->lock);
#else
    a->loop = loop;
#endif
}

static void
on_audio_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_audio_pending *pp = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    if (pp->dead) {
        nd_response_free(resp);
        g_clear_error(&err);
        g_free(pp);
        return;
    }
    nd_audio *a = pp->audio;
    if (!resp || resp->error || !resp->body || resp->body->len == 0) {
        a->failed = TRUE;
        g_clear_error(&err);
        nd_response_free(resp);
    } else {
        a->body = g_byte_array_ref(resp->body);
        a->loaded = TRUE;
        nd_response_free(resp);
#ifdef ND_HAVE_AUDIO
        audio_maybe_start(a);
#endif
    }
    g_ptr_array_remove_fast(pp->cache->pending, pp);
    g_free(pp);
}

nd_audio *
nd_audio_cache_get(nd_audio_cache *cache, const char *url,
                   const char *top_url, gboolean loop)
{
    if (!cache || !url) return NULL;
#ifndef ND_HAVE_AUDIO
    (void)top_url;
    (void)loop;
    return NULL;
#else
    nd_audio *cached = g_hash_table_lookup(cache->by_url, url);
    if (cached) {
        nd_audio_set_loop(cached, loop);
        return cached;
    }
    nd_audio *a = g_new0(nd_audio, 1);
    a->url   = g_strdup(url);
    a->cache = cache;
    a->loop  = loop;
    g_mutex_init(&a->lock);
    g_hash_table_insert(cache->by_url, g_strdup(url), a);
    nd_audio_pending *pp = g_new0(nd_audio_pending, 1);
    pp->audio = a;
    pp->cache = cache;
    g_ptr_array_add(cache->pending, pp);
    nd_net_fetch_async(url, top_url, NULL, on_audio_fetched, pp);
    return a;
#endif
}

void
nd_audio_pause_all(nd_audio_cache *cache)
{
    if (!cache) return;
#ifdef ND_HAVE_AUDIO
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        nd_audio *a = value;
        g_atomic_int_set(&a->stop, 1);
    }
#endif
}
