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
    gboolean     loaded;
    gboolean     failed;
};

typedef void (*nd_image_ready_cb)(nd_image *img, gpointer user_data);

nd_image_cache *nd_image_cache_new(void);
void            nd_image_cache_free(nd_image_cache *cache);

nd_image       *nd_image_cache_get(nd_image_cache *cache,
                                   const char     *url,
                                   nd_image_ready_cb cb,
                                   gpointer        user_data);

G_END_DECLS

#endif
