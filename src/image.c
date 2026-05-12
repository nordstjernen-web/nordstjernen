/* Nordstjernen — image cache (PNG/JPEG/GIF). */

#include "image.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

#include "config.h"
#include "net.h"

struct nd_image_cache {
    GHashTable *by_url;
    GPtrArray  *pending;
};

typedef struct nd_pending {
    nd_image          *img;
    nd_image_cache    *cache;
    nd_image_ready_cb  cb;
    gpointer           user_data;
} nd_pending;

static void
nd_image_free(gpointer p)
{
    nd_image *img = p;
    if (!img) return;
    g_free(img->url);
    if (img->texture) g_object_unref(img->texture);
    g_free(img);
}

nd_image_cache *
nd_image_cache_new(void)
{
    nd_image_cache *c = g_new0(nd_image_cache, 1);
    c->by_url = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nd_image_free);
    c->pending = g_ptr_array_new();
    return c;
}

void
nd_image_cache_free(nd_image_cache *cache)
{
    if (!cache) return;
    g_hash_table_destroy(cache->by_url);
    g_ptr_array_free(cache->pending, TRUE);
    g_free(cache);
}

static GdkTexture *
texture_from_bytes(const guchar *data, gsize len, int *out_w, int *out_h)
{
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    GError *err = NULL;
    if (!gdk_pixbuf_loader_write(loader, data, len, &err) ||
        !gdk_pixbuf_loader_close(loader, &err)) {
        if (err) g_error_free(err);
        g_object_unref(loader);
        return NULL;
    }
    GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pixbuf) {
        g_object_unref(loader);
        return NULL;
    }
    g_object_ref(pixbuf);
    *out_w = gdk_pixbuf_get_width(pixbuf);
    *out_h = gdk_pixbuf_get_height(pixbuf);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkTexture *tex = gdk_texture_new_for_pixbuf(pixbuf);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref(pixbuf);
    g_object_unref(loader);
    return tex;
}

static void
on_image_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_pending *pending = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    if (!resp) {
        pending->img->failed = TRUE;
        if (err) g_error_free(err);
        if (pending->cb) pending->cb(pending->img, pending->user_data);
        g_ptr_array_remove_fast(pending->cache->pending, pending);
        g_free(pending);
        return;
    }
    if (resp->error || !resp->body || resp->body->len == 0) {
        pending->img->failed = TRUE;
    } else {
        int w = 0, h = 0;
        GdkTexture *tex = texture_from_bytes(resp->body->data, resp->body->len, &w, &h);
        if (tex) {
            pending->img->texture = tex;
            pending->img->natural_width  = w;
            pending->img->natural_height = h;
            pending->img->loaded = TRUE;
        } else {
            pending->img->failed = TRUE;
        }
    }
    nd_response_free(resp);
    if (pending->cb) pending->cb(pending->img, pending->user_data);
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

nd_image *
nd_image_cache_get(nd_image_cache *cache,
                   const char *url,
                   nd_image_ready_cb cb,
                   gpointer user_data)
{
    if (!cache || !url) return NULL;
    nd_image *cached = g_hash_table_lookup(cache->by_url, url);
    if (cached) return cached;

    nd_image *img = g_new0(nd_image, 1);
    img->url = g_strdup(url);
    g_hash_table_insert(cache->by_url, g_strdup(url), img);

    const nd_config *cfg = nd_config_get();
    if (cfg && !cfg->images_enabled) {
        img->failed = TRUE;
        if (cb) cb(img, user_data);
        return img;
    }

    nd_pending *pending = g_new0(nd_pending, 1);
    pending->img = img;
    pending->cache = cache;
    pending->cb = cb;
    pending->user_data = user_data;
    g_ptr_array_add(cache->pending, pending);
    nd_net_fetch_async(url, NULL, on_image_fetched, pending);
    return img;
}
