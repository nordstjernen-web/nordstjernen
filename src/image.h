/* Nordstjernen — image cache API (PNG/JPEG/GIF). */

#ifndef ND_IMAGE_H
#define ND_IMAGE_H

#include <gdk/gdk.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_image_cache nd_image_cache;
typedef struct nd_image       nd_image;

struct nd_image {
    char        *url;
    GdkTexture  *texture;
    int          natural_width;
    int          natural_height;
    long         http_status;
    char        *error;
    gboolean     loaded;
    gboolean     failed;
};

typedef void (*nd_image_ready_cb)(nd_image *img, gpointer user_data);

nd_image_cache *nd_image_cache_new(void);
void            nd_image_cache_free(nd_image_cache *cache);

nd_image       *nd_image_cache_get(nd_image_cache *cache,
                                   const char     *url,
                                   const char     *top_url,
                                   nd_image_ready_cb cb,
                                   gpointer        user_data);

nd_image       *nd_image_cache_peek(nd_image_cache *cache, const char *url);

GdkTexture *nd_image_decode_bytes(const guchar *data, gsize len,
                                  int *out_w, int *out_h);

gboolean nd_image_pixbuf_supports_mime(const char *mime);

const char *nd_image_accept_header_fragment(void);

G_END_DECLS

#endif
