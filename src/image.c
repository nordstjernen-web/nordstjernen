/* Nordstjernen — image cache (PNG/JPEG/GIF/SVG). */

#include "image.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <librsvg/rsvg.h>
#include <string.h>

#include "config.h"
#include "net.h"

#ifdef G_OS_WIN32
static gboolean
nd_image_mime_blocked_on_platform(const char *bare)
{
    return g_str_equal(bare, "image/avif")  ||
           g_str_equal(bare, "image/heif")  ||
           g_str_equal(bare, "image/heic")  ||
           g_str_equal(bare, "image/heif-sequence") ||
           g_str_equal(bare, "image/heic-sequence") ||
           g_str_equal(bare, "image/jxl");
}

static gboolean
nd_image_bytes_blocked_on_platform(const guchar *data, gsize len)
{
    if (!data || len < 12) return FALSE;
    if (memcmp(data, "\xFF\x0A", 2) == 0) return TRUE;
    if (memcmp(data, "\x00\x00\x00", 3) == 0 &&
        memcmp(data + 4, "JXL ", 4) == 0) return TRUE;
    if (memcmp(data + 4, "ftyp", 4) != 0) return FALSE;
    static const char *const brands[] = {
        "avif", "avis", "heic", "heix", "hevc", "hevx",
        "mif1", "msf1", "heim", "heis", "hevm", "hevs",
        NULL
    };
    for (int i = 0; brands[i]; i++)
        if (memcmp(data + 8, brands[i], 4) == 0) return TRUE;
    return FALSE;
}
#endif

struct nd_image_cache {
    GHashTable *by_url;
    GPtrArray  *pending;
};

typedef struct nd_pending {
    nd_image          *img;
    nd_image_cache    *cache;
    nd_image_ready_cb  cb;
    gpointer           user_data;
    gboolean           dead;
} nd_pending;

static void
nd_image_free(gpointer p)
{
    nd_image *img = p;
    if (!img) return;
    g_free(img->url);
    g_free(img->error);
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
    for (guint i = 0; i < cache->pending->len; i++) {
        nd_pending *p = g_ptr_array_index(cache->pending, i);
        p->dead = TRUE;
    }
    g_hash_table_destroy(cache->by_url);
    g_ptr_array_free(cache->pending, TRUE);
    g_free(cache);
}

static GHashTable *
pixbuf_supported_mimes_set(void)
{
    static gsize once = 0;
    static GHashTable *mimes = NULL;
    if (g_once_init_enter(&once)) {
        mimes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        GSList *formats = gdk_pixbuf_get_formats();
        for (GSList *p = formats; p; p = p->next) {
            GdkPixbufFormat *f = p->data;
            if (gdk_pixbuf_format_is_disabled(f)) continue;
            gchar **mts = gdk_pixbuf_format_get_mime_types(f);
            for (int i = 0; mts && mts[i]; i++) {
                gchar *low = g_ascii_strdown(mts[i], -1);
                g_hash_table_replace(mimes, low, GINT_TO_POINTER(1));
            }
            g_strfreev(mts);
        }
        g_slist_free(formats);
        g_once_init_leave(&once, 1);
    }
    return mimes;
}

const char *
nd_image_accept_header_fragment(void)
{
    static gsize once = 0;
    static char *fragment = NULL;
    if (g_once_init_enter(&once)) {
        GString *out = g_string_new("image/png,image/jpeg");
        const char *extras[] = {
            "image/gif", "image/svg+xml", "image/tiff", "image/bmp",
            "image/x-icon", "image/vnd.microsoft.icon",
            "image/webp", "image/avif", "image/jxl",
            NULL
        };
        for (int i = 0; extras[i]; i++) {
            if (!nd_image_pixbuf_supports_mime(extras[i])) continue;
            g_string_append_c(out, ',');
            g_string_append(out, extras[i]);
        }
        fragment = g_string_free(out, FALSE);
        g_once_init_leave(&once, 1);
    }
    return fragment;
}

gboolean
nd_image_pixbuf_supports_mime(const char *mime)
{
    if (!mime || !*mime) return FALSE;
    while (g_ascii_isspace(*mime)) mime++;
    const char *end = mime;
    while (*end && *end != ';' && !g_ascii_isspace(*end)) end++;
    if (end == mime) return FALSE;
    gchar *bare = g_ascii_strdown(mime, end - mime);
    static const char *const whitelist[] = {
        "image/png", "image/jpeg", "image/jpg",
        "image/gif", "image/webp", "image/svg+xml",
        "image/avif", "image/jxl",
        NULL
    };
    gboolean ok = FALSE;
    for (int i = 0; whitelist[i]; i++) {
        if (g_str_equal(bare, whitelist[i])) { ok = TRUE; break; }
    }
#ifdef G_OS_WIN32
    if (nd_image_mime_blocked_on_platform(bare)) ok = FALSE;
#endif
    if (ok && g_str_equal(bare, "image/svg+xml")) {
        g_free(bare);
        return TRUE;
    }
    if (ok) {
        GHashTable *mimes = pixbuf_supported_mimes_set();
        ok = g_hash_table_contains(mimes, bare);
    }
    g_free(bare);
    return ok;
}

static GdkTexture *
nd_image_decode_svg(const guchar *data, gsize len, int *out_w, int *out_h)
{
    GError *err = NULL;
    RsvgHandle *handle = rsvg_handle_new_from_data(data, len, &err);
    g_clear_error(&err);
    if (!handle) return NULL;
    GdkPixbuf *pixbuf = rsvg_handle_get_pixbuf_and_error(handle, &err);
    g_clear_error(&err);
    if (!pixbuf) {
        g_object_unref(handle);
        return NULL;
    }
    if (out_w) *out_w = gdk_pixbuf_get_width(pixbuf);
    if (out_h) *out_h = gdk_pixbuf_get_height(pixbuf);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkTexture *tex = gdk_texture_new_for_pixbuf(pixbuf);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref(pixbuf);
    g_object_unref(handle);
    return tex;
}

GdkTexture *
nd_image_decode_bytes(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if (!data || len == 0) return NULL;
#ifdef G_OS_WIN32
    if (nd_image_bytes_blocked_on_platform(data, len)) return NULL;
#endif

    GBytes *bytes = g_bytes_new(data, len);
    GError *err = NULL;
    GdkTexture *tex = gdk_texture_new_from_bytes(bytes, &err);
    g_bytes_unref(bytes);
    if (tex) {
        g_clear_error(&err);
        if (out_w) *out_w = gdk_texture_get_width(tex);
        if (out_h) *out_h = gdk_texture_get_height(tex);
        return tex;
    }
    g_clear_error(&err);

    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    gboolean ok = gdk_pixbuf_loader_write(loader, data, len, &err);
    g_clear_error(&err);
    if (!gdk_pixbuf_loader_close(loader, &err)) ok = FALSE;
    g_clear_error(&err);
    GdkPixbuf *pixbuf = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : NULL;
    if (!pixbuf) {
        g_object_unref(loader);
        return nd_image_decode_svg(data, len, out_w, out_h);
    }
    if (out_w) *out_w = gdk_pixbuf_get_width(pixbuf);
    if (out_h) *out_h = gdk_pixbuf_get_height(pixbuf);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkTexture *out = gdk_texture_new_for_pixbuf(pixbuf);
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref(loader);
    return out;
}

static void
on_image_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_pending *pending = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    if (pending->dead) {
        nd_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    if (!resp) {
        pending->img->failed = TRUE;
        pending->img->error = err && err->message
            ? g_strdup(err->message) : g_strdup("fetch failed");
        g_clear_error(&err);
        if (pending->cb) pending->cb(pending->img, pending->user_data);
        g_ptr_array_remove_fast(pending->cache->pending, pending);
        g_free(pending);
        return;
    }
    pending->img->http_status = resp->status;
    if (resp->error) {
        pending->img->failed = TRUE;
        pending->img->error = g_strdup(resp->error);
    } else if (!resp->body || resp->body->len == 0) {
        pending->img->failed = TRUE;
        if (resp->status >= 400)
            pending->img->error = g_strdup_printf("HTTP %ld", resp->status);
        else
            pending->img->error = g_strdup("empty response");
    } else {
        int w = 0, h = 0;
        GdkTexture *tex = nd_image_decode_bytes(resp->body->data, resp->body->len, &w, &h);
        if (tex) {
            pending->img->texture = tex;
            pending->img->natural_width  = w;
            pending->img->natural_height = h;
            pending->img->loaded = TRUE;
        } else {
            pending->img->failed = TRUE;
            pending->img->error = g_strdup("decode failed");
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

nd_image *
nd_image_cache_peek(nd_image_cache *cache, const char *url)
{
    if (!cache || !url) return NULL;
    return g_hash_table_lookup(cache->by_url, url);
}
