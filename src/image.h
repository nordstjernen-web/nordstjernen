/* Nordstjernen — image cache API (PNG/JPEG/GIF).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_IMAGE_H
#define ND_IMAGE_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct nd_image_cache nd_image_cache;
typedef struct nd_image       nd_image;

typedef struct nd_image_anim_frame {
    nd_texture *texture;
    int         delay_ms;
} nd_image_anim_frame;

struct nd_image {
    char        *url;
    nd_texture  *texture;
    void        *render_surface;
    int          natural_width;
    int          natural_height;
    long         http_status;
    char        *error;
    gint64       failed_at_us;
    int          attempts;
    gboolean     loaded;
    gboolean     failed;
    GArray      *anim_frames;
    gint64       anim_start_us;
    int          anim_current;
    int          anim_total_ms;
};

typedef void (*nd_image_ready_cb)(nd_image *img, gpointer user_data);

nd_image_cache *nd_image_cache_new(void);
void            nd_image_cache_free(nd_image_cache *cache);

nd_image       *nd_image_cache_get(nd_image_cache *cache,
                                   const char     *url,
                                   const char     *top_url,
                                   nd_image_ready_cb cb,
                                   gpointer        user_data);

void            nd_image_cache_cancel_cb(nd_image_cache *cache,
                                         gpointer user_data);

nd_image       *nd_image_cache_peek(nd_image_cache *cache, const char *url);

nd_image       *nd_image_cache_insert_loaded(nd_image_cache *cache,
                                             const char     *url,
                                             nd_texture     *texture,
                                             int             width,
                                             int             height);

nd_texture *nd_image_decode_bytes(const guchar *data, gsize len,
                                  int *out_w, int *out_h);

nd_texture *nd_image_decode_wuffs(const guchar *data, gsize len,
                                  int *out_w, int *out_h);

GArray *nd_image_decode_wuffs_anim(const guchar *data, gsize len,
                                   int *out_w, int *out_h);

guint8 *nd_image_wuffs_decode_to_bgra(const guchar *data, gsize len,
                                      int *out_w, int *out_h,
                                      gsize *out_stride, gsize *out_buf_len);

gboolean nd_image_wuffs_supports_bytes(const guchar *data, gsize len);

nd_texture *nd_image_decode_ico(const guchar *data, gsize len,
                                int *out_w, int *out_h);

gboolean nd_image_cache_tick(nd_image_cache *cache, gint64 now_us);

gboolean nd_image_should_retry(const nd_image *img, gint64 now_us);

#ifdef ND_HAVE_AVIF
nd_texture *nd_image_decode_avif(const guchar *data, gsize len,
                                 int *out_w, int *out_h);

gboolean nd_image_avif_supports_bytes(const guchar *data, gsize len);
#endif

gboolean nd_image_pixbuf_supports_mime(const char *mime);

const char *nd_image_accept_header_fragment(void);

G_END_DECLS

#endif
