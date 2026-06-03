/* Nordstjernen — public C embedding API implementation.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "libnordstjernen.h"

#include <cairo.h>
#include <gio/gio.h>
#include <glib.h>
#include <stdint.h>
#include <string.h>

#include "anim.h"
#include "bcache.h"
#include "cache.h"
#include "config.h"
#include "css.h"
#include "dom.h"
#include "engine.h"
#include "font.h"
#include "html.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"
#include "paint.h"

struct nd_browser {
    nd_node        *doc;
    nd_box         *layout;
    GHashTable     *styles;
    nd_js          *js;
    nd_anim        *anim;
    nd_image_cache *images;
    GHashTable     *css_cache;
    char           *base_url;
    int             vw;
    double          vh;
    gboolean        images_fetched;
};

static void
browser_relayout(nd_browser *b)
{
    if (b->js && b->layout) nd_js_set_layout_root(b->js, NULL);
    if (b->layout) { nd_box_free(b->layout); b->layout = NULL; }
    if (b->js && b->styles) nd_js_set_style_table(b->js, NULL);
    if (b->styles) { g_hash_table_destroy(b->styles); b->styles = NULL; }
    b->styles = nd_engine_relayout(b->doc, b->base_url, b->vw, b->vh,
                                   b->images, b->anim, b->js,
                                   b->css_cache, NULL, 0, 0, &b->layout);
}

static void
walk_max_bottom(const nd_box *b, double *out)
{
    if (!b) return;
    double bottom = b->y + b->content_height;
    if (bottom > *out) *out = bottom;
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        walk_max_bottom(c, out);
}

static void
browser_ensure_images(nd_browser *browser)
{
    if (browser->images_fetched) return;
    nd_engine_fetch_images(browser->layout, browser->base_url,
                           browser->images);
    browser_relayout(browser);
    browser->images_fetched = TRUE;
}

static void
browser_flush(gpointer user_data)
{
    nd_browser *b = user_data;
    if (!b || !b->js) return;
    if (!b->layout || nd_js_consume_mutated(b->js))
        browser_relayout(b);
}

static gboolean
settle_quit_cb(gpointer user_data)
{
    g_main_loop_quit(user_data);
    return G_SOURCE_REMOVE;
}

static gboolean
settle_tick_cb(gpointer user_data)
{
    nd_browser *b = user_data;
    gint64 now = g_get_monotonic_time();
    if (b->images) nd_image_cache_tick(b->images, now);
    if (b->anim) nd_anim_tick(b->anim, now);
    if (b->js && nd_js_run_animation_frame(b->js) &&
        nd_js_consume_mutated(b->js))
        browser_relayout(b);
    return G_SOURCE_CONTINUE;
}

static void
browser_settle(nd_browser *b, int settle_ms)
{
    if (settle_ms <= 0) return;
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(settle_ms, settle_quit_cb, loop);
    guint tick = g_timeout_add(16, settle_tick_cb, b);
    g_main_loop_run(loop);
    g_source_remove(tick);
    g_main_loop_unref(loop);
}

static void browser_js_log(const char *line, gpointer ud) { (void)line; (void)ud; }
static void browser_js_mutated(gpointer ud) { (void)ud; }
static void browser_js_navigate(const char *url, gboolean reload, gpointer ud)
{ (void)url; (void)reload; (void)ud; }

int
nd_browser_init(void)
{
    nd_net_init();
    nd_net_set_allow_file_urls(TRUE);
    nd_cache_init();
    nd_bcache_init();
    nd_font_init();
    return 0;
}

void
nd_browser_shutdown(void)
{
    nd_font_shutdown();
    nd_bcache_shutdown();
    nd_cache_shutdown();
    nd_net_shutdown();
    nd_config_shutdown();
}

static char *
resolve_local_path(const char *url)
{
    if (!url || strstr(url, "://") ||
        g_str_has_prefix(url, "about:") || g_str_has_prefix(url, "data:") ||
        !g_file_test(url, G_FILE_TEST_EXISTS))
        return NULL;
    char *abs = g_canonicalize_filename(url, NULL);
    char *file_url = g_filename_to_uri(abs, NULL, NULL);
    g_free(abs);
    return file_url;
}

nd_browser *
nd_browser_open(const char *url, int viewport_width, int settle_ms)
{
    if (!url || !*url) return NULL;

    char *file_url = resolve_local_path(url);
    const char *fetch_url = file_url ? file_url : url;

    GError *err = NULL;
    nd_response *resp = nd_engine_fetch_blocking(fetch_url, NULL, &err);
    if (!resp || resp->error || !resp->body) {
        if (resp) nd_response_free(resp);
        g_clear_error(&err);
        g_free(file_url);
        return NULL;
    }
    g_clear_error(&err);

    char *base = g_strdup(resp->final_url ? resp->final_url : fetch_url);
    g_free(file_url);

    char *decoded = nd_html_decode_body((const char *)resp->body->data,
                                        resp->body->len);
    nd_node *doc = nd_html_parse(decoded ? decoded : "",
                                 decoded ? (gssize)strlen(decoded) : 0);
    g_free(decoded);
    nd_response_free(resp);

    int vw = viewport_width > 0 ? viewport_width : 1000;
    double vh = (double)vw * 0.75;
    nd_css_set_viewport((double)vw, vh);
    const char *frag = strchr(url, '#');
    nd_css_set_target_fragment(frag && *(frag + 1) ? frag + 1 : NULL);

    nd_browser *b = g_new0(nd_browser, 1);
    b->doc = doc;
    b->base_url = base;
    b->vw = vw;
    b->vh = vh;
    b->css_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)g_bytes_unref);
    b->images = nd_image_cache_new();
    b->styles = nd_engine_compute_cascade(doc, base, b->css_cache);

    b->anim = nd_anim_new();
    nd_engine_load_keyframes(b->anim, doc, base, b->css_cache);
    nd_engine_anim_observe(b->anim, b->styles, g_get_monotonic_time());

    b->js = nd_js_new(browser_js_log, NULL,
                      browser_js_mutated, NULL,
                      browser_js_navigate, NULL);
    if (b->js) {
        nd_js_set_style_table(b->js, b->styles);
        nd_js_set_image_cache(b->js, b->images);
        nd_js_set_layout_flush_cb(b->js, browser_flush, b);
        nd_js_run_scripts_in_doc(b->js, doc, base);
    }

    browser_settle(b, settle_ms);
    browser_relayout(b);
    return b;
}

char *
nd_browser_render_text(nd_browser *browser)
{
    if (!browser || !browser->layout) return NULL;
    GString *out = g_string_new(NULL);
    nd_engine_dump_text(browser->layout, out);
    char *text = malloc(out->len + 1);
    if (text) {
        memcpy(text, out->str, out->len);
        text[out->len] = '\0';
    }
    g_string_free(out, TRUE);
    return text;
}

int
nd_browser_render_image(nd_browser *browser, const char *path)
{
    if (!browser || !browser->layout || !path) return -1;

    browser_ensure_images(browser);

    nd_paint_set_js(browser->js);
    nd_paint_set_anim(browser->anim);

    int rc;
    gsize len = strlen(path);
    if (len >= 4 && g_ascii_strcasecmp(path + len - 4, ".pdf") == 0)
        rc = nd_engine_write_pdf(browser->layout, path);
    else
        rc = nd_engine_write_png(browser->layout, path);

    nd_paint_set_anim(NULL);
    nd_paint_set_js(NULL);
    return rc;
}

int
nd_browser_page_size(nd_browser *browser, int *out_width, int *out_height)
{
    if (!browser || !browser->layout) return -1;
    double w = browser->layout->content_width;
    if (!(w > 0)) w = browser->vw;
    double bottom = browser->layout->content_height;
    walk_max_bottom(browser->layout, &bottom);
    if (!(bottom > 0)) bottom = 0;
    if (out_width)  *out_width  = (int)w;
    if (out_height) *out_height = (int)bottom + 32;
    return 0;
}

int
nd_browser_render_rgba(nd_browser *browser, int scroll_x, int scroll_y,
                       int width, int height, double scale,
                       unsigned char *out, int stride)
{
    if (!browser || !browser->layout || !out) return -1;
    if (width <= 0 || height <= 0 || stride < width * 4) return -1;
    if (!(scale > 0)) scale = 1.0;

    browser_ensure_images(browser);

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return -1;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -(double)scroll_x, -(double)scroll_y);

    nd_paint_set_js(browser->js);
    nd_paint_set_anim(browser->anim);
    nd_paint(cr, browser->layout, NULL);
    nd_paint_set_anim(NULL);
    nd_paint_set_js(NULL);

    cairo_destroy(cr);
    cairo_surface_flush(surf);

    const unsigned char *src = cairo_image_surface_get_data(surf);
    int src_stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < height; y++) {
        const unsigned char *srow = src + (size_t)y * src_stride;
        unsigned char *drow = out + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            uint32_t px;
            memcpy(&px, srow + x * 4, sizeof px);
            drow[x * 4 + 0] = (unsigned char)((px >> 16) & 0xFF);
            drow[x * 4 + 1] = (unsigned char)((px >> 8) & 0xFF);
            drow[x * 4 + 2] = (unsigned char)(px & 0xFF);
            drow[x * 4 + 3] = (unsigned char)((px >> 24) & 0xFF);
        }
    }
    cairo_surface_destroy(surf);
    return 0;
}

char *
nd_browser_link_at(nd_browser *browser, int x, int y)
{
    if (!browser || !browser->layout) return NULL;

    /* Touch taps are imprecise, so probe the exact point first and then a
     * small ring around it (CSS px) before giving up. */
    static const int kR = 6;
    static const int probe[][2] = {
        { 0, 0 },
        { 0, -kR }, { 0, kR }, { -kR, 0 }, { kR, 0 },
        { -kR, -kR }, { kR, -kR }, { -kR, kR }, { kR, kR },
    };
    for (int i = 0; i < (int)(sizeof probe / sizeof probe[0]); i++) {
        const char *href = nd_box_hit_link(browser->layout,
                                           (double)(x + probe[i][0]),
                                           (double)(y + probe[i][1]));
        if (href && *href) return nd_url_resolve(browser->base_url, href);
    }
    return NULL;
}

char *
nd_browser_title(nd_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    nd_node *title = nd_node_find_first_element(browser->doc, "title");
    if (!title) return NULL;
    char *raw = nd_node_collect_text(title);
    if (!raw) return NULL;

    GString *out = g_string_new(NULL);
    gboolean prev_ws = TRUE;
    for (const char *p = raw; *p; p++) {
        gboolean ws = (*p == ' ' || *p == '\t' || *p == '\n' ||
                       *p == '\r' || *p == '\f');
        if (ws) {
            if (!prev_ws) g_string_append_c(out, ' ');
            prev_ws = TRUE;
        } else {
            g_string_append_c(out, *p);
            prev_ws = FALSE;
        }
    }
    if (out->len > 0 && out->str[out->len - 1] == ' ')
        g_string_set_size(out, out->len - 1);
    g_free(raw);

    if (out->len == 0) { g_string_free(out, TRUE); return NULL; }
    char *result = strdup(out->str);
    g_string_free(out, TRUE);
    return result;
}

char *
nd_browser_url(nd_browser *browser)
{
    if (!browser || !browser->base_url) return NULL;
    return strdup(browser->base_url);
}

static void
collect_links(const nd_node *node, const char *base, GString *out,
              GHashTable *seen)
{
    for (const nd_node *c = node->first_child; c; c = c->next_sibling) {
        if (nd_node_is_element_named(c, "a")) {
            const char *href = nd_element_get_attr(c, "href");
            if (href && *href && href[0] != '#' &&
                !g_str_has_prefix(href, "javascript:")) {
                char *abs = nd_url_resolve(base, href);
                if (abs && *abs && !g_hash_table_contains(seen, abs)) {
                    g_hash_table_add(seen, g_strdup(abs));
                    if (out->len) g_string_append_c(out, '\n');
                    g_string_append(out, abs);
                }
                g_free(abs);
            }
        }
        collect_links(c, base, out, seen);
    }
}

char *
nd_browser_links(nd_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    GString *out = g_string_new(NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    collect_links(browser->doc, browser->base_url, out, seen);
    g_hash_table_destroy(seen);
    if (out->len == 0) { g_string_free(out, TRUE); return NULL; }
    char *result = strdup(out->str);
    g_string_free(out, TRUE);
    return result;
}

void
nd_browser_close(nd_browser *browser)
{
    if (!browser) return;
    nd_paint_set_anim(NULL);
    if (browser->js) {
        nd_js_set_layout_root(browser->js, NULL);
        nd_js_set_style_table(browser->js, NULL);
    }
    if (browser->anim) nd_anim_free(browser->anim);
    if (browser->layout) nd_box_free(browser->layout);
    if (browser->styles) g_hash_table_destroy(browser->styles);
    if (browser->css_cache) g_hash_table_destroy(browser->css_cache);
    if (browser->js) nd_js_free(browser->js);
    if (browser->doc) nd_node_free(browser->doc);
    if (browser->images) nd_image_cache_free(browser->images);
    g_free(browser->base_url);
    g_free(browser);
}
