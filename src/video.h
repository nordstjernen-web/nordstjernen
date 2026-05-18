/* Nordstjernen — VP9 video decode + frame cache + YouTube watch page interception.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_VIDEO_H
#define ND_VIDEO_H

#include <gdk/gdk.h>
#include <glib.h>

G_BEGIN_DECLS

gboolean nd_youtube_is_watch_url(const char *url);

gboolean nd_youtube_host_needs_browser_ua(const char *host);

const char *nd_youtube_browser_user_agent(void);

char *nd_youtube_render_watch_page(const char *url,
                                   const char *body,
                                   gsize body_len);

typedef struct nd_video {
    char        *url;
    int          natural_width;
    int          natural_height;
    GdkTexture  *poster_texture;
    GdkTexture  *frame_texture;
    gboolean     loaded;
    gboolean     failed;
    gboolean     ended;
    guint        current_frame;
    gint64       last_frame_us;
    gint64       start_wallclock_us;
    GByteArray  *body;
    gpointer     decoder;
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

gboolean nd_video_advance_frame(nd_video *v);

gboolean nd_video_tick(nd_video *v, gint64 now_us);

void     nd_video_restart(nd_video *v);

G_END_DECLS

#endif
