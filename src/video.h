/* Nordstjernen — inline video playback and poster cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_VIDEO_H
#define NS_VIDEO_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_box ns_box;
typedef struct ns_node ns_node;

typedef struct ns_video {
    char        *url;
    int          natural_width;
    int          natural_height;
    ns_texture  *poster_texture;
    ns_texture  *frame_texture;
    gboolean     loaded;
    gboolean     failed;

    void        *player;
    const void  *dom_node;
    gboolean     is_camera;
    gboolean     autoplay;
    gboolean     loop;
    gboolean     controls;
    gboolean     muted;
    gboolean     has_audio;
    gboolean     audio_opened;
    char        *audio_url;
    char        *audio_file;
    gsize        audio_file_len;
    guint        audio_file_gen;
    double       audio_timeline_start;
    char        *pending_audio_file;
    gsize        pending_audio_file_len;
    guint        pending_audio_file_gen;
    double       pending_audio_start;
    char        *video_file;
    gsize        video_file_len;
    guint        video_file_gen;
    char        *pending_video_file;
    gsize        pending_video_file_len;
    guint        pending_video_file_gen;
    double       pending_video_start;
    gboolean     video_opened;
    double       rect_x, rect_y, rect_w, rect_h;
    double       clip_x, clip_y, clip_w, clip_h;
    int          rect_fit;
    gint64       last_paint_us;
    gboolean     rect_dirty;
    gint64       rect_sent_us;
    double       sent_rect_x, sent_rect_y, sent_rect_w, sent_rect_h;
    double       sent_clip_x, sent_clip_y, sent_clip_w, sent_clip_h;
    int          sent_rect_fit;
    char        *token;
    gboolean     playing;
    gboolean     ended;
    double       volume;
    gboolean     meta_sent;
    gboolean     buf_sent;
    guint        seq;
    gint64       base_us;
    gint64       last_refresh_us;
    gboolean     stalled;
    gint64       stall_since_us;
    guint        mse_id;
    double       cur_time;
    double       prev_tick_time;
    double       last_emit_time;
    double       duration;
    GPtrArray   *cues;
    gboolean     track_requested;
} ns_video;

typedef struct ns_video_cache ns_video_cache;
typedef void (*ns_video_js_cb)(const void *dom_node, const char *kind,
                               double value, gpointer user_data);
typedef void (*ns_video_audio_cb)(const char *command, gpointer user_data);

ns_video_cache *ns_video_cache_new(void);
void            ns_video_cache_free(ns_video_cache *cache);
void            ns_video_cache_set_base(ns_video_cache *cache,
                                        const char *base_url);
void            ns_video_cache_set_js_cb(ns_video_cache *cache,
                                         ns_video_js_cb cb, gpointer user_data);
void            ns_video_cache_set_audio_cb(ns_video_cache *cache,
                                            ns_video_audio_cb cb,
                                            gpointer user_data);

void     ns_video_cache_discover(ns_video_cache *cache, const ns_box *root,
                                 const ns_node *doc, gint64 now_us);
void     ns_video_cache_note_layout(ns_video_cache *cache, const ns_box *root,
                                    double scroll_x, double scroll_y,
                                    double scale);
gboolean ns_video_cache_tick(ns_video_cache *cache, gint64 now_us);
void     ns_video_cache_flush_composites(ns_video_cache *cache,
                                         gint64 now_us);
gboolean ns_video_cache_animating(const ns_video_cache *cache);
gboolean ns_video_cache_waiting_growth(const ns_video_cache *cache);
gboolean ns_video_cache_mse_append(ns_video_cache *cache, guint stream_id,
                                   char kind, const guint8 *data, gsize len);
void     ns_video_cache_mse_eos(ns_video_cache *cache, guint stream_id);
double   ns_video_cache_mse_buffered(ns_video_cache *cache, guint stream_id,
                                     char kind, double *start);
gboolean ns_video_cache_mse_remove(ns_video_cache *cache, guint stream_id,
                                   char kind, double start, double end);
gsize    ns_video_cache_mse_bytes(ns_video_cache *cache, guint stream_id,
                                  char kind);
gboolean ns_video_cache_has_pending(const ns_video_cache *cache);
gboolean ns_video_cache_helper_event(ns_video_cache *cache, const char *token,
                                     const char *kind);

gboolean ns_video_url_is_inline(const char *url);
gboolean ns_video_cache_seek_node(ns_video_cache *cache, const void *dom_node,
                                  double seconds, gint64 now_us);
gboolean ns_video_cache_set_node_playing(ns_video_cache *cache,
                                         const void *dom_node, gboolean play,
                                         gint64 now_us);
gboolean ns_video_cache_set_node_muted(ns_video_cache *cache,
                                       const void *dom_node, gboolean muted);
gboolean ns_video_cache_set_node_volume(ns_video_cache *cache,
                                        const void *dom_node, double volume);
gboolean ns_video_cache_toggle(ns_video_cache *cache, ns_video *v, gint64 now_us);
gboolean ns_video_toggle(ns_video *v, gint64 now_us);
void     ns_video_play(ns_video *v, gint64 now_us);
void     ns_video_pause(ns_video *v, gint64 now_us);
void     ns_video_note_paint_rect(ns_video *v, double x, double y,
                                   double w, double h, int fit);
void     ns_video_note_paint_clip(ns_video *v, double x, double y,
                                  double w, double h);
gboolean ns_video_helper_composited(const ns_video *v);
const char *ns_video_active_cue_text(const ns_video *v);

G_END_DECLS

#endif
