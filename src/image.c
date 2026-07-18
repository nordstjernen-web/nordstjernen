/* Nordstjernen — image cache (PNG/JPEG/GIF/SVG).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "image.h"

#ifdef NS_HAVE_GDK_PIXBUF
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

static gboolean
ns_image_builtin_supports_mime(const char *bare)
{
    static const char *const types[] = {
        "image/png", "image/jpeg", "image/jpg", "image/gif",
        "image/bmp", "image/webp", "image/x-icon",
        "image/vnd.microsoft.icon",
        NULL
    };
    for (int i = 0; types[i]; i++)
        if (g_str_equal(bare, types[i])) return TRUE;
#ifdef NS_HAVE_LIBRSVG
    if (g_str_equal(bare, "image/svg+xml")) return TRUE;
#endif
#ifdef NS_HAVE_AVIF
    if (g_str_equal(bare, "image/avif")) return TRUE;
#endif
    return FALSE;
}
#ifdef NS_HAVE_LIBRSVG
#include <librsvg/rsvg.h>
#endif
#include <math.h>
#include <string.h>
#include <cairo.h>

#include "config.h"
#include "net.h"

#ifdef G_OS_WIN32
static gboolean
ns_image_mime_blocked_on_platform(const char *bare)
{
    return g_str_equal(bare, "image/avif")  ||
           g_str_equal(bare, "image/heif")  ||
           g_str_equal(bare, "image/heic")  ||
           g_str_equal(bare, "image/heif-sequence") ||
           g_str_equal(bare, "image/heic-sequence") ||
           g_str_equal(bare, "image/jxl");
}

static gboolean
ns_image_bytes_blocked_on_platform(const guchar *data, gsize len)
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

enum { NS_IMAGE_CACHE_BUDGET_BYTES = 256 * 1024 * 1024 };

struct ns_image_cache {
    GHashTable *by_url;
    GPtrArray  *pending;
    guint64     gen;
    gint64      total_bytes;
};

typedef struct ns_pending {
    ns_image          *img;
    ns_image_cache    *cache;
    ns_image_ready_cb  cb;
    gpointer           user_data;
    gboolean           dead;
} ns_pending;

static void
ns_image_anim_frame_clear(gpointer data)
{
    ns_image_anim_frame *f = data;
    if (f) ns_texture_unref(f->texture);
}

void
ns_image_pixel_frame_clear(gpointer data)
{
    ns_image_pixel_frame *f = data;
    if (f) g_free(f->pixels);
}

static void
ns_image_free(gpointer p)
{
    ns_image *img = p;
    if (!img) return;
    g_free(img->url);
    g_free(img->error);
    if (img->render_surface) cairo_surface_destroy(img->render_surface);
    if (img->anim_frames) g_array_free(img->anim_frames, TRUE);
    else ns_texture_unref(img->texture);
    g_free(img);
}

static gint64
ns_image_decoded_bytes(const ns_image *img)
{
    if (!img) return 0;
    if (img->anim_frames && img->anim_frames->len > 0) {
        gint64 total = 0;
        for (guint i = 0; i < img->anim_frames->len; i++) {
            const ns_image_anim_frame *f =
                &g_array_index(img->anim_frames, ns_image_anim_frame, i);
            if (!f->texture) continue;
            int w = ns_texture_get_width(f->texture);
            int h = ns_texture_get_height(f->texture);
            if (w > 0 && h > 0) total += (gint64)w * (gint64)h * 4;
        }
        return total;
    }
    if (img->texture) {
        int w = ns_texture_get_width(img->texture);
        int h = ns_texture_get_height(img->texture);
        if (w > 0 && h > 0) return (gint64)w * (gint64)h * 4;
    }
    return 0;
}

static void
ns_image_cache_account(ns_image_cache *cache, ns_image *img)
{
    if (!cache || !img || !img->loaded || img->bytes != 0) return;
    img->bytes = ns_image_decoded_bytes(img);
    img->gen = cache->gen;
    cache->total_bytes += img->bytes;
}

static gboolean
ns_image_has_pending(const ns_image_cache *cache, const ns_image *img)
{
    for (guint i = 0; i < cache->pending->len; i++) {
        const ns_pending *p = g_ptr_array_index(cache->pending, i);
        if (!p->dead && p->img == img) return TRUE;
    }
    return FALSE;
}

static void
ns_image_cache_purge(ns_image_cache *cache, ns_image *img)
{
    cache->total_bytes -= img->bytes;
    img->bytes = 0;
    if (img->anim_frames) {
        g_array_free(img->anim_frames, TRUE);
        img->anim_frames = NULL;
        img->texture = NULL;
    } else {
        ns_texture_clear(&img->texture);
    }
    if (img->render_surface) {
        cairo_surface_destroy(img->render_surface);
        img->render_surface = NULL;
    }
    img->anim_current = 0;
    img->anim_total_ms = 0;
    img->anim_start_us = 0;
    img->loaded = FALSE;
    img->purged = TRUE;
}

ns_image_cache *
ns_image_cache_new(void)
{
    ns_image_cache *c = g_new0(ns_image_cache, 1);
    c->by_url = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ns_image_free);
    c->pending = g_ptr_array_new();
    return c;
}

void
ns_image_cache_free(ns_image_cache *cache)
{
    if (!cache) return;
    for (guint i = 0; i < cache->pending->len; i++) {
        ns_pending *p = g_ptr_array_index(cache->pending, i);
        p->dead = TRUE;
    }
    g_hash_table_destroy(cache->by_url);
    g_ptr_array_free(cache->pending, TRUE);
    g_free(cache);
}

static void
ns_image_fire_pending(ns_pending *pending)
{
    ns_image *img = pending->img;
    ns_image_cache *cache = pending->cache;
    GPtrArray *fire = g_ptr_array_new();
    for (guint i = 0; i < cache->pending->len; ) {
        ns_pending *p = g_ptr_array_index(cache->pending, i);
        if (p->img == img) {
            g_ptr_array_remove_index_fast(cache->pending, i);
            g_ptr_array_add(fire, p);
        } else {
            i++;
        }
    }
    for (guint i = 0; i < fire->len; i++) {
        ns_pending *p = g_ptr_array_index(fire, i);
        if (p->cb) p->cb(img, p->user_data);
        g_free(p);
    }
    g_ptr_array_free(fire, TRUE);
}

#ifdef NS_HAVE_GDK_PIXBUF
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
#endif /* NS_HAVE_GDK_PIXBUF */

#ifdef NS_HAVE_GDK_PIXBUF

#define NS_PIXBUF_MAX_DIM    16384
#define NS_PIXBUF_MAX_PIXELS (64 * 1024 * 1024)

static void
ns_image_pixbuf_size_prepared(GdkPixbufLoader *loader, gint width, gint height,
                              gpointer user)
{
    gboolean *too_big = user;
    if (width <= 0 || height <= 0 ||
        width > NS_PIXBUF_MAX_DIM || height > NS_PIXBUF_MAX_DIM ||
        (gint64)width * (gint64)height > NS_PIXBUF_MAX_PIXELS) {
        *too_big = TRUE;
        gdk_pixbuf_loader_set_size(loader, 1, 1);
    }
}

static GdkPixbuf *
ns_image_pixbuf_decode_capped(const guint8 *data, gsize len,
                              GdkPixbufLoader **out_loader)
{
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    gboolean too_big = FALSE;
    gulong sid = g_signal_connect(loader, "size-prepared",
                                  G_CALLBACK(ns_image_pixbuf_size_prepared),
                                  &too_big);
    GError *err = NULL;
    gboolean ok = gdk_pixbuf_loader_write(loader, data, len, &err);
    g_clear_error(&err);
    if (!gdk_pixbuf_loader_close(loader, &err)) ok = FALSE;
    g_clear_error(&err);
    g_signal_handler_disconnect(loader, sid);
    GdkPixbuf *pixbuf = (ok && !too_big)
        ? gdk_pixbuf_loader_get_pixbuf(loader) : NULL;
    *out_loader = loader;
    return pixbuf;
}

static guint8 *
ns_image_pixbuf_to_bgra(GdkPixbuf *pixbuf,
                        int *out_w, int *out_h,
                        gsize *out_stride, gsize *out_buf_len)
{
    if (!pixbuf) return NULL;
    int w = gdk_pixbuf_get_width(pixbuf);
    int h = gdk_pixbuf_get_height(pixbuf);
    int n = gdk_pixbuf_get_n_channels(pixbuf);
    int bits = gdk_pixbuf_get_bits_per_sample(pixbuf);
    int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
    gboolean alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    const guchar *src = gdk_pixbuf_get_pixels(pixbuf);
    if (w <= 0 || h <= 0 || n < 3 || bits != 8 || !src ||
        src_stride <= 0)
        return NULL;
    if ((gsize)w > G_MAXSIZE / (gsize)n)
        return NULL;
    if ((gsize)w > G_MAXSIZE / 4 ||
        (gsize)h > G_MAXSIZE / ((gsize)w * 4))
        return NULL;
    if ((gsize)src_stride < (gsize)w * (gsize)n)
        return NULL;
    gsize stride = (gsize)w * 4;
    gsize len = stride * (gsize)h;
    guint8 *dst = g_try_malloc(len);
    if (!dst) return NULL;
    for (int y = 0; y < h; y++) {
        const guchar *s = src + (gsize)y * (gsize)src_stride;
        guint8 *d = dst + (gsize)y * stride;
        for (int x = 0; x < w; x++) {
            guint8 r = s[(gsize)x * n + 0];
            guint8 g = s[(gsize)x * n + 1];
            guint8 b = s[(gsize)x * n + 2];
            guint8 a = alpha ? s[(gsize)x * n + 3] : 255;
            d[(gsize)x * 4 + 0] = (guint8)(((guint) b * a + 127) / 255);
            d[(gsize)x * 4 + 1] = (guint8)(((guint) g * a + 127) / 255);
            d[(gsize)x * 4 + 2] = (guint8)(((guint) r * a + 127) / 255);
            d[(gsize)x * 4 + 3] = a;
        }
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_stride) *out_stride = stride;
    if (out_buf_len) *out_buf_len = len;
    return dst;
}
#endif /* NS_HAVE_GDK_PIXBUF */

const char *
ns_image_accept_header_fragment(void)
{
    static gsize once = 0;
    static char *fragment = NULL;
    if (g_once_init_enter(&once)) {
        GString *out = g_string_new(
            "image/png,image/jpeg,image/x-icon,image/vnd.microsoft.icon");
        const char *extras[] = {
            "image/gif", "image/svg+xml", "image/tiff", "image/bmp",
            "image/avif",
            NULL
        };
        for (int i = 0; extras[i]; i++) {
            if (!ns_image_pixbuf_supports_mime(extras[i])) continue;
            g_string_append_c(out, ',');
            g_string_append(out, extras[i]);
        }
        fragment = g_string_free(out, FALSE);
        g_once_init_leave(&once, 1);
    }
    return fragment;
}

gboolean
ns_image_pixbuf_supports_mime(const char *mime)
{
    if (!mime || !*mime) return FALSE;
    while (g_ascii_isspace(*mime)) mime++;
    const char *end = mime;
    while (*end && *end != ';' && !g_ascii_isspace(*end)) end++;
    if (end == mime) return FALSE;
    gchar *bare = g_ascii_strdown(mime, end - mime);
    if (ns_image_builtin_supports_mime(bare)) {
        g_free(bare);
        return TRUE;
    }
#ifdef G_OS_WIN32
    if (ns_image_mime_blocked_on_platform(bare)) {
        g_free(bare);
        return FALSE;
    }
#ifdef NS_HAVE_GDK_PIXBUF
    if (g_str_equal(bare, "image/tiff")) {
        g_free(bare);
        return TRUE;
    }
#endif
#endif
    gboolean ok = FALSE;
#ifdef NS_HAVE_GDK_PIXBUF
    if (g_str_equal(bare, "image/tiff") || g_str_equal(bare, "image/jxl")) {
        GHashTable *mimes = pixbuf_supported_mimes_set();
        ok = g_hash_table_contains(mimes, bare);
    }
#endif
    g_free(bare);
    return ok;
}

#ifdef NS_HAVE_LIBRSVG
static ns_texture *
ns_image_decode_svg(const guchar *data, gsize len, int *out_w, int *out_h)
{
    enum {
        NS_SVG_MAX_INPUT_BYTES = 4 * 1024 * 1024,
        NS_SVG_MAX_DIM_PX      = 4096,
        NS_SVG_MAX_PIXELS      = 4096 * 4096,
        NS_SVG_DEFAULT_DIM_PX  = 512,
    };

    if (!data || len == 0 || len > NS_SVG_MAX_INPUT_BYTES) return NULL;

    GError *err = NULL;
    RsvgHandle *handle = rsvg_handle_new_with_flags(RSVG_HANDLE_FLAGS_NONE);
    if (!handle) return NULL;
    rsvg_handle_set_base_uri(handle, "about:blank");
    GInputStream *stream = g_memory_input_stream_new_from_data(data, (gssize)len, NULL);
    gboolean read_ok = rsvg_handle_read_stream_sync(handle, stream, NULL, &err);
    g_object_unref(stream);
    g_clear_error(&err);
    if (!read_ok) {
        g_object_unref(handle);
        return NULL;
    }

    double w = NS_SVG_DEFAULT_DIM_PX;
    double h = NS_SVG_DEFAULT_DIM_PX;
    gboolean intrinsic_resolved = FALSE;
#if LIBRSVG_CHECK_VERSION(2, 52, 0)
    gdouble iw = 0, ih = 0;
    gboolean got_size = rsvg_handle_get_intrinsic_size_in_pixels(handle, &iw, &ih);
    if (got_size && iw > 0 && ih > 0) { w = iw; h = ih; intrinsic_resolved = TRUE; }
#endif
#if LIBRSVG_CHECK_VERSION(2, 46, 0)
    if (!intrinsic_resolved) {
        gboolean has_w = FALSE, has_h = FALSE, has_vb = FALSE;
        RsvgLength rw = {0}, rh = {0};
        RsvgRectangle vb = {0};
        rsvg_handle_get_intrinsic_dimensions(handle,
            &has_w, &rw, &has_h, &rh, &has_vb, &vb);
        if (has_vb && vb.width > 0 && vb.height > 0) {
            w = vb.width;
            h = vb.height;
            intrinsic_resolved = TRUE;
        }
    }
#endif

    if (w > NS_SVG_MAX_DIM_PX || h > NS_SVG_MAX_DIM_PX) {
        double s = (double)NS_SVG_MAX_DIM_PX / MAX(w, h);
        w *= s; h *= s;
    }
    if (w * h > (double)NS_SVG_MAX_PIXELS) {
        double s = sqrt((double)NS_SVG_MAX_PIXELS / (w * h));
        w *= s; h *= s;
    }
    int iw_px = (int)CLAMP(w, 1.0, (double)NS_SVG_MAX_DIM_PX);
    int ih_px = (int)CLAMP(h, 1.0, (double)NS_SVG_MAX_DIM_PX);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       iw_px, ih_px);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        g_object_unref(handle);
        return NULL;
    }
    cairo_t *cr = cairo_create(surf);
    RsvgRectangle viewport = { .x = 0, .y = 0, .width = iw_px, .height = ih_px };
    gboolean rendered = rsvg_handle_render_document(handle, cr, &viewport, &err);
    cairo_destroy(cr);
    g_clear_error(&err);
    g_object_unref(handle);
    if (!rendered) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_surface_flush(surf);

    int stride = cairo_image_surface_get_stride(surf);
    const guchar *pixels = cairo_image_surface_get_data(surf);
    GBytes *bytes = g_bytes_new_with_free_func(
        pixels, (gsize)stride * (gsize)ih_px,
        (GDestroyNotify)cairo_surface_destroy, surf);

    ns_texture *tex = ns_texture_new(
        iw_px, ih_px,
        NS_TEXTURE_DEFAULT,
        bytes, (gsize)stride);
    g_bytes_unref(bytes);
    if (!tex) return NULL;

    if (out_w) *out_w = iw_px;
    if (out_h) *out_h = ih_px;
    return tex;
}
#endif /* NS_HAVE_LIBRSVG */

ns_texture *
ns_image_decode_bytes(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if (!data || len == 0) return NULL;

    if (len >= 4 && data[0] == 0 && data[1] == 0 &&
        (data[2] == 1 || data[2] == 2) && data[3] == 0) {
        ns_texture *tex = ns_image_decode_ico(data, len, out_w, out_h);
        if (tex) return tex;
    }

    if (ns_image_wuffs_supports_bytes(data, len)) {
        ns_texture *tex = ns_image_decode_wuffs(data, len, out_w, out_h);
        if (tex) return tex;
    }

    if (ns_image_webp_supports_bytes(data, len)) {
        ns_texture *tex = ns_image_decode_webp(data, len, out_w, out_h);
        if (tex) return tex;
    }

#ifdef NS_HAVE_AVIF
    if (ns_image_avif_supports_bytes(data, len)) {
        ns_texture *tex = ns_image_decode_avif(data, len, out_w, out_h);
        if (tex) return tex;
    }
#endif

#ifdef G_OS_WIN32
    if (ns_image_bytes_blocked_on_platform(data, len)) return NULL;
#endif

#ifdef NS_HAVE_GDK_PIXBUF
    GdkPixbufLoader *loader = NULL;
    GdkPixbuf *pixbuf = ns_image_pixbuf_decode_capped(data, len, &loader);
    int w = 0, h = 0;
    gsize stride = 0, buf_len = 0;
    guint8 *bgra = ns_image_pixbuf_to_bgra(pixbuf, &w, &h, &stride, &buf_len);
    g_object_unref(loader);
    if (bgra) {
        GBytes *bytes = g_bytes_new_take(bgra, buf_len);
        ns_texture *out = ns_texture_new(w, h, NS_TEXTURE_BGRA_PREMULTIPLIED,
                                         bytes, stride);
        g_bytes_unref(bytes);
        if (out) {
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            return out;
        }
    }
#ifdef NS_HAVE_LIBRSVG
    return ns_image_decode_svg(data, len, out_w, out_h);
#else
    return NULL;
#endif
#elif defined(NS_HAVE_LIBRSVG)
    return ns_image_decode_svg(data, len, out_w, out_h);
#else
    (void)out_w;
    (void)out_h;
    return NULL;
#endif
}

static guint8 *
ns_image_texture_to_pixels(ns_texture *tex, int w, int h,
                           gsize *out_stride, gsize *out_buf_len)
{
    if (!tex || w <= 0 || h <= 0) {
        ns_texture_unref(tex);
        return NULL;
    }
    if ((gsize)w > G_MAXSIZE / 4 ||
        (gsize)h > G_MAXSIZE / ((gsize)w * 4)) {
        ns_texture_unref(tex);
        return NULL;
    }
    gsize stride = (gsize)w * 4;
    gsize buf_len = stride * (gsize)h;
    guint8 *pixels = g_try_malloc(buf_len);
    if (!pixels) {
        ns_texture_unref(tex);
        return NULL;
    }
    ns_texture_download(tex, pixels, stride);
    ns_texture_unref(tex);
    if (out_stride) *out_stride = stride;
    if (out_buf_len) *out_buf_len = buf_len;
    return pixels;
}

guint8 *
ns_image_decode_bytes_to_pixels(const guchar *data, gsize len,
                                int *out_w, int *out_h,
                                gsize *out_stride, gsize *out_buf_len,
                                ns_texture_format *out_format)
{
    if (!data || len == 0) return NULL;

    if (ns_image_wuffs_supports_bytes(data, len)) {
        guint8 *pix = ns_image_wuffs_decode_to_bgra(data, len, out_w, out_h,
                                                    out_stride, out_buf_len);
        if (pix) {
            if (out_format) *out_format = NS_TEXTURE_BGRA_PREMULTIPLIED;
            return pix;
        }
    }

    if (ns_image_webp_supports_bytes(data, len)) {
        guint8 *pix = ns_image_webp_decode_to_bgra(data, len, out_w, out_h,
                                                   out_stride, out_buf_len);
        if (pix) {
            if (out_format) *out_format = NS_TEXTURE_BGRA_PREMULTIPLIED;
            return pix;
        }
    }

#ifdef NS_HAVE_AVIF
    if (ns_image_avif_supports_bytes(data, len)) {
        int w = 0, h = 0;
        ns_texture *tex = ns_image_decode_avif(data, len, &w, &h);
        guint8 *pix = ns_image_texture_to_pixels(tex, w, h,
                                                 out_stride, out_buf_len);
        if (pix) {
            if (out_w) *out_w = w;
            if (out_h) *out_h = h;
            if (out_format) *out_format = NS_TEXTURE_BGRA_PREMULTIPLIED;
            return pix;
        }
    }
#endif

#ifdef G_OS_WIN32
    if (ns_image_bytes_blocked_on_platform(data, len)) return NULL;
#endif

#ifdef NS_HAVE_GDK_PIXBUF
    GdkPixbufLoader *loader = NULL;
    GdkPixbuf *pixbuf = ns_image_pixbuf_decode_capped(data, len, &loader);
    guint8 *pix = ns_image_pixbuf_to_bgra(pixbuf, out_w, out_h,
                                          out_stride, out_buf_len);
    g_object_unref(loader);
    if (pix) {
        if (out_format) *out_format = NS_TEXTURE_BGRA_PREMULTIPLIED;
        return pix;
    }
#endif

    int w = 0, h = 0;
    ns_texture *tex = ns_image_decode_bytes(data, len, &w, &h);
    guint8 *pixels = ns_image_texture_to_pixels(tex, w, h,
                                                out_stride, out_buf_len);
    if (!pixels) return NULL;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_format) *out_format = NS_TEXTURE_DEFAULT;
    return pixels;
}

static GArray *
ns_image_anim_frames_from_pixels(GArray *pixel_frames,
                                 int *out_w, int *out_h,
                                 int *out_total_ms)
{
    if (!pixel_frames || pixel_frames->len == 0) return NULL;
    GArray *frames = g_array_new(FALSE, FALSE, sizeof(ns_image_anim_frame));
    g_array_set_clear_func(frames, ns_image_anim_frame_clear);
    gboolean ok = TRUE;
    int total = 0;
    int w = 0, h = 0;
    for (guint i = 0; i < pixel_frames->len; i++) {
        ns_image_pixel_frame *pf =
            &g_array_index(pixel_frames, ns_image_pixel_frame, i);
        if (!pf->pixels || pf->pixels_len == 0 ||
            pf->width <= 0 || pf->height <= 0) {
            ok = FALSE;
            break;
        }
        GBytes *bytes = g_bytes_new_take(pf->pixels, pf->pixels_len);
        pf->pixels = NULL;
        ns_texture *tex = ns_texture_new(pf->width, pf->height, pf->format,
                                         bytes, pf->stride);
        g_bytes_unref(bytes);
        if (!tex) {
            ok = FALSE;
            break;
        }
        int delay = pf->delay_ms > 0 ? pf->delay_ms : 100;
        ns_image_anim_frame f = { tex, delay };
        g_array_append_val(frames, f);
        total += delay;
        if (i == 0) {
            w = pf->width;
            h = pf->height;
        }
    }
    if (!ok || frames->len == 0) {
        g_array_free(frames, TRUE);
        return NULL;
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_total_ms) *out_total_ms = total > 0 ? total : 1;
    return frames;
}

typedef struct {
    ns_texture *tex;
    GArray     *frames;
    int         w;
    int         h;
} ns_img_decoded;

static ns_img_decoded
ns_image_decode_body(const guchar *data, gsize len)
{
    ns_img_decoded d = { NULL, NULL, 0, 0 };
    int w = 0, h = 0;
    GArray *frames = NULL;
    if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
        frames = ns_image_decode_wuffs_anim(data, len, &w, &h);
    } else if (ns_image_webp_supports_bytes(data, len)) {
        GArray *pixel_frames =
            ns_image_decode_webp_anim_to_pixels(data, len, &w, &h);
        if (pixel_frames) {
            int total = 0;
            frames = ns_image_anim_frames_from_pixels(pixel_frames, &w, &h,
                                                      &total);
            g_array_free(pixel_frames, TRUE);
        }
    }
    if (frames && frames->len > 1) {
        d.frames = frames;
        d.w = w;
        d.h = h;
        return d;
    }
    if (frames) {
        g_array_set_clear_func(frames, ns_image_anim_frame_clear);
        g_array_free(frames, TRUE);
    }
    d.tex = ns_image_decode_bytes(data, len, &w, &h);
    d.w = w;
    d.h = h;
    return d;
}

static void
ns_image_apply_decoded_state(ns_image *img, ns_img_decoded *d,
                             const char *content_type, gsize body_len)
{
    if (d->frames) {
        g_array_set_clear_func(d->frames, ns_image_anim_frame_clear);
        img->anim_frames = d->frames;
        ns_image_anim_frame *f0 =
            &g_array_index(d->frames, ns_image_anim_frame, 0);
        img->texture = f0->texture;
        img->natural_width  = d->w;
        img->natural_height = d->h;
        int total = 0;
        for (guint i = 0; i < d->frames->len; i++)
            total += g_array_index(d->frames, ns_image_anim_frame, i).delay_ms;
        img->anim_total_ms = total > 0 ? total : 1;
        img->anim_start_us = g_get_monotonic_time();
        img->loaded = TRUE;
    } else if (d->tex) {
        img->texture = d->tex;
        img->natural_width  = d->w;
        img->natural_height = d->h;
        img->loaded = TRUE;
    } else {
        img->failed = TRUE;
        img->failed_at_us = g_get_monotonic_time();
        img->error = g_strdup_printf("could not decode image (%s, %u bytes)",
            content_type && *content_type ? content_type : "unknown type",
            (unsigned)body_len);
    }
}

typedef struct {
    ns_pending *pending;
    GBytes     *body;
    char       *content_type;
    gsize       body_len;
} ns_decode_job;

static void
ns_decode_job_free(ns_decode_job *job)
{
    if (!job) return;
    if (job->body) g_bytes_unref(job->body);
    g_free(job->content_type);
    g_free(job);
}

static void
image_decode_worker(GTask *task, gpointer source, gpointer task_data,
                    GCancellable *cancellable)
{
    (void)source;
    (void)cancellable;
    ns_decode_job *job = task_data;
    gsize len = 0;
    const guchar *data = g_bytes_get_data(job->body, &len);
    ns_img_decoded *d = g_new(ns_img_decoded, 1);
    *d = ns_image_decode_body(data, len);
    g_task_return_pointer(task, d, g_free);
}

static void
on_image_decoded(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    ns_decode_job *job = user_data;
    ns_img_decoded *d = g_task_propagate_pointer(G_TASK(res), NULL);
    ns_pending *pending = job->pending;
    if (pending->dead) {
        if (d) {
            if (d->frames) {
                g_array_set_clear_func(d->frames, ns_image_anim_frame_clear);
                g_array_free(d->frames, TRUE);
            }
            if (d->tex) ns_texture_unref(d->tex);
            g_free(d);
        }
        g_free(pending);
        ns_decode_job_free(job);
        return;
    }
    ns_img_decoded result = { NULL, NULL, 0, 0 };
    if (d) {
        result = *d;
        g_free(d);
    }
    ns_image_apply_decoded_state(pending->img, &result,
                                 job->content_type, job->body_len);
    ns_image_cache_account(pending->cache, pending->img);
    ns_image_fire_pending(pending);
    ns_decode_job_free(job);
}

static void
on_image_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    if (!resp) {
        pending->img->failed = TRUE;
        pending->img->failed_at_us = g_get_monotonic_time();
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
        pending->img->failed_at_us = g_get_monotonic_time();
        pending->img->error = g_strdup(resp->error);
    } else if (resp->status >= 400) {
        pending->img->failed = TRUE;
        pending->img->failed_at_us = g_get_monotonic_time();
        pending->img->error = g_strdup_printf("HTTP %ld", resp->status);
    } else if (!resp->body || resp->body->len == 0) {
        pending->img->failed = TRUE;
        pending->img->failed_at_us = g_get_monotonic_time();
        pending->img->error = g_strdup("empty response");
    } else if (ns_config_get()->async_image_decode) {
        ns_decode_job *job = g_new0(ns_decode_job, 1);
        job->pending = pending;
        job->body = g_bytes_new(resp->body->data, resp->body->len);
        job->content_type =
            g_strdup(resp->content_type ? resp->content_type : "");
        job->body_len = resp->body->len;
        ns_response_free(resp);
        GTask *task = g_task_new(NULL, NULL, on_image_decoded, job);
        g_task_set_task_data(task, job, NULL);
        g_task_run_in_thread(task, image_decode_worker);
        g_object_unref(task);
        return;
    } else {
        ns_img_decoded d = ns_image_decode_body(resp->body->data,
                                                resp->body->len);
        ns_image_apply_decoded_state(pending->img, &d,
                                     resp->content_type, resp->body->len);
    }
    ns_response_free(resp);
    ns_image_cache_account(pending->cache, pending->img);
    ns_image_fire_pending(pending);
}

gboolean
ns_image_should_retry(const ns_image *img, gint64 now_us)
{
    if (!img || !img->failed || img->attempts >= 3) return FALSE;
    if (img->http_status > 0 &&
        img->http_status != 408 &&
        img->http_status != 425 &&
        img->http_status != 429 &&
        img->http_status != 500 &&
        img->http_status != 502 &&
        img->http_status != 503 &&
        img->http_status != 504)
        return FALSE;
    gint64 wait_us = img->attempts <= 1 ? 2 * G_USEC_PER_SEC
                                        : 10 * G_USEC_PER_SEC;
    if (img->http_status == 429)
        wait_us = img->attempts <= 1 ? 15 * G_USEC_PER_SEC
                                     : 45 * G_USEC_PER_SEC;
    return img->failed_at_us == 0 || now_us - img->failed_at_us >= wait_us;
}

static void
ns_image_cache_start_request(ns_image_cache *cache,
                             ns_image *img,
                             const char *url,
                             const char *top_url,
                             ns_image_ready_cb cb,
                             gpointer user_data)
{
    ns_pending *pending = g_new0(ns_pending, 1);
    pending->img = img;
    pending->cache = cache;
    pending->cb = cb;
    pending->user_data = user_data;
    g_ptr_array_add(cache->pending, pending);
    img->attempts++;
    ns_net_fetch_async(url, top_url, NULL, on_image_fetched, pending);
}

ns_image *
ns_image_cache_get(ns_image_cache *cache,
                   const char *url,
                   const char *top_url,
                   ns_image_ready_cb cb,
                   gpointer user_data)
{
    if (!cache || !url) return NULL;
    const ns_config *cfg = ns_config_get();
    ns_image *cached = g_hash_table_lookup(cache->by_url, url);
    if (cached) {
        cached->gen = cache->gen;
        if (cfg && !cfg->images_enabled) {
            if (cb && (cached->loaded || cached->failed))
                cb(cached, user_data);
            return cached;
        }
        if (cached->purged && !ns_image_has_pending(cache, cached)) {
            cached->purged = FALSE;
            ns_image_cache_start_request(cache, cached, url, top_url,
                                         cb, user_data);
            return cached;
        }
        if (ns_image_should_retry(cached, g_get_monotonic_time())) {
            cached->failed = FALSE;
            cached->http_status = 0;
            cached->failed_at_us = 0;
            g_clear_pointer(&cached->error, g_free);
            ns_image_cache_start_request(cache, cached, url, top_url,
                                         cb, user_data);
            return cached;
        }
        if (cached->loaded || cached->failed) {
            if (cb) cb(cached, user_data);
        } else if (cb) {
            gboolean dup = FALSE;
            for (guint i = 0; i < cache->pending->len; i++) {
                ns_pending *p = g_ptr_array_index(cache->pending, i);
                if (!p->dead && p->img == cached &&
                    p->cb == cb && p->user_data == user_data) {
                    dup = TRUE;
                    break;
                }
            }
            if (!dup) {
                ns_pending *pending = g_new0(ns_pending, 1);
                pending->img = cached;
                pending->cache = cache;
                pending->cb = cb;
                pending->user_data = user_data;
                g_ptr_array_add(cache->pending, pending);
            }
        }
        return cached;
    }

    ns_image *img = g_new0(ns_image, 1);
    img->url = g_strdup(url);
    g_hash_table_insert(cache->by_url, g_strdup(url), img);

    if (cfg && !cfg->images_enabled) {
        img->failed = TRUE;
        if (cb) cb(img, user_data);
        return img;
    }

    ns_image_cache_start_request(cache, img, url, top_url, cb, user_data);
    return img;
}

void
ns_image_cache_cancel_cb(ns_image_cache *cache, gpointer user_data)
{
    if (!cache || !cache->pending) return;
    for (guint i = 0; i < cache->pending->len; i++) {
        ns_pending *p = g_ptr_array_index(cache->pending, i);
        if (p->user_data == user_data) {
            p->cb = NULL;
            p->user_data = NULL;
        }
    }
}

ns_image *
ns_image_cache_peek(ns_image_cache *cache, const char *url)
{
    if (!cache || !url) return NULL;
    ns_image *img = g_hash_table_lookup(cache->by_url, url);
    if (img) img->gen = cache->gen;
    return img;
}

gboolean
ns_image_cache_tick(ns_image_cache *cache, gint64 now_us)
{
    if (!cache) return FALSE;
    gboolean any = FALSE;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        ns_image *img = value;
        if (!img->anim_frames || img->anim_frames->len < 2) continue;
        if (img->anim_total_ms <= 0) continue;
        gint64 elapsed_ms = (now_us - img->anim_start_us) / 1000;
        if (elapsed_ms < 0) elapsed_ms = 0;
        int phase = (int)(elapsed_ms % img->anim_total_ms);
        int idx = 0, acc = 0;
        for (guint i = 0; i < img->anim_frames->len; i++) {
            ns_image_anim_frame *f =
                &g_array_index(img->anim_frames, ns_image_anim_frame, i);
            acc += f->delay_ms;
            if (phase < acc) { idx = (int)i; break; }
            idx = (int)i;
        }
        if (idx != img->anim_current) {
            img->anim_current = idx;
            ns_image_anim_frame *f =
                &g_array_index(img->anim_frames, ns_image_anim_frame, idx);
            img->texture = f->texture;
            any = TRUE;
        }
    }
    return any;
}

gboolean
ns_image_cache_animating(const ns_image_cache *cache)
{
    if (!cache) return FALSE;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        const ns_image *img = value;
        if (img->loaded && img->anim_frames && img->anim_frames->len > 1)
            return TRUE;
    }
    return FALSE;
}

gboolean
ns_image_cache_has_pending(const ns_image_cache *cache)
{
    return cache && cache->pending->len > 0;
}

ns_image *
ns_image_cache_insert_loaded(ns_image_cache *cache, const char *url,
                             ns_texture *texture, int width, int height)
{
    if (!cache || !url || !texture) return NULL;
    ns_image *existing = g_hash_table_lookup(cache->by_url, url);
    if (existing) return existing;
    ns_image *img = g_new0(ns_image, 1);
    img->url = g_strdup(url);
    img->texture = texture;
    img->natural_width = width;
    img->natural_height = height;
    img->loaded = TRUE;
    g_hash_table_insert(cache->by_url, g_strdup(url), img);
    ns_image_cache_account(cache, img);
    return img;
}

ns_image *
ns_image_cache_insert_encoded(ns_image_cache *cache, const char *url,
                              const guchar *data, gsize len)
{
    if (!cache || !url || !data || len == 0) return NULL;
    ns_image *existing = g_hash_table_lookup(cache->by_url, url);
    if (existing) return existing;

    int w = 0, h = 0;
    {
        GArray *pixel_frames = NULL;
        if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
            pixel_frames = ns_image_decode_wuffs_anim_to_pixels(data, len,
                                                                &w, &h);
        else if (ns_image_webp_supports_bytes(data, len))
            pixel_frames = ns_image_decode_webp_anim_to_pixels(data, len,
                                                               &w, &h);
        if (pixel_frames && pixel_frames->len > 1) {
            int fw = 0, fh = 0, total = 0;
            GArray *frames = ns_image_anim_frames_from_pixels(pixel_frames,
                                                              &fw, &fh, &total);
            g_array_free(pixel_frames, TRUE);
            if (frames && frames->len > 1) {
                ns_image *img = g_new0(ns_image, 1);
                img->url = g_strdup(url);
                img->anim_frames = frames;
                ns_image_anim_frame *f0 =
                    &g_array_index(frames, ns_image_anim_frame, 0);
                img->texture = f0->texture;
                img->natural_width = fw;
                img->natural_height = fh;
                img->anim_total_ms = total;
                img->anim_start_us = g_get_monotonic_time();
                img->loaded = TRUE;
                g_hash_table_insert(cache->by_url, g_strdup(url), img);
                ns_image_cache_account(cache, img);
                return img;
            }
            if (frames) g_array_free(frames, TRUE);
        } else if (pixel_frames) {
            g_array_free(pixel_frames, TRUE);
        }
    }

    ns_texture *tex = ns_image_decode_bytes(data, len, &w, &h);
    if (!tex) return NULL;
    return ns_image_cache_insert_loaded(cache, url, tex, w, h);
}

void
ns_image_cache_begin_generation(ns_image_cache *cache)
{
    if (cache) cache->gen++;
}

void
ns_image_cache_collect(ns_image_cache *cache)
{
    if (!cache) return;
    while (cache->total_bytes > NS_IMAGE_CACHE_BUDGET_BYTES) {
        ns_image *victim = NULL;
        guint64 oldest = G_MAXUINT64;
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, cache->by_url);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            ns_image *img = value;
            if (!img->loaded || img->bytes == 0) continue;
            if (img->gen == cache->gen) continue;
            if (ns_image_has_pending(cache, img)) continue;
            if (img->gen < oldest) {
                oldest = img->gen;
                victim = img;
            }
        }
        if (!victim) break;
        ns_image_cache_purge(cache, victim);
    }
}
