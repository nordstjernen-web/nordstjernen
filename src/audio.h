/* Nordstjernen — Opus audio decode + PulseAudio output cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_AUDIO_H
#define ND_AUDIO_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_audio_cache nd_audio_cache;
typedef struct nd_audio       nd_audio;

nd_audio_cache *nd_audio_cache_new(void);
void            nd_audio_cache_free(nd_audio_cache *cache);

nd_audio *nd_audio_cache_get(nd_audio_cache *cache,
                             const char     *url,
                             const char     *top_url,
                             gboolean        loop);

void nd_audio_set_loop(nd_audio *a, gboolean loop);
void nd_audio_pause_all(nd_audio_cache *cache);

gboolean nd_audio_available(void);

G_END_DECLS

#endif
