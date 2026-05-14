/* Nordstjernen — VP9 video decode + frame cache. */

#ifndef ND_VIDEO_H
#define ND_VIDEO_H

#include <gdk/gdk.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_video {
    char        *url;
    int          natural_width;
    int          natural_height;
    GdkTexture  *poster_texture;
    GdkTexture  *frame_texture;
    gboolean     loaded;
    gboolean     failed;
    gboolean     playing;
    gboolean     ended;
    guint        frame_count;
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

G_END_DECLS

#endif
