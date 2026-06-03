/* Nordstjernen — video poster cache for the external-player handoff.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_VIDEO_H
#define ND_VIDEO_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct nd_video {
    char        *url;
    int          natural_width;
    int          natural_height;
    nd_texture  *poster_texture;
    gboolean     loaded;
    gboolean     failed;
} nd_video;

typedef struct nd_video_cache nd_video_cache;
typedef void (*nd_video_ready_cb)(nd_video *v, gpointer user_data);

nd_video_cache *nd_video_cache_new(void);
void            nd_video_cache_free(nd_video_cache *cache);

nd_video *nd_video_cache_get(nd_video_cache *cache,
                             const char *url,
                             const char *poster_url,
                             const char *top_url,
                             nd_video_ready_cb cb,
                             gpointer user_data);

G_END_DECLS

#endif
