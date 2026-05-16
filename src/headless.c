/* Nordstjernen — headless engine driver.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "headless.h"

#include <cairo-pdf.h>
#include <cairo.h>
#include <stdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include "cache.h"
#include "compatibility.h"
#include "config.h"
#include "css.h"
#include "dom.h"
#include "html.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"
#include "paint.h"
#include "video.h"

typedef struct fetch_state {
    GMainLoop  *loop;
    nd_response *resp;
    GError      *error;
} fetch_state;

static void
on_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    fetch_state *st = user_data;
    st->resp = nd_net_fetch_finish(result, &st->error);
    g_main_loop_quit(st->loop);
}

static nd_response *
fetch_url_blocking(const char *url, GError **error)
{
    fetch_state st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);
    nd_net_fetch_async(url, NULL, NULL, on_fetch_done, &st);
    g_main_loop_run(st.loop);
    g_main_loop_unref(st.loop);
    if (error) *error = st.error;
    else g_clear_error(&st.error);
    return st.resp;
}

static gboolean
settle_quit_cb(gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void
settle_main_loop(int ms)
{
    if (ms <= 0) return;
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(ms, settle_quit_cb, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

static void
dump_text_walk(const nd_box *b, GString *out)
{
    if (!b) return;
    if (b->kind == ND_BOX_INLINE && b->text && *b->text) {
        g_string_append(out, b->text);
        g_string_append_c(out, '\n');
    } else if (b->kind == ND_BOX_IMAGE && b->dom) {
        const char *alt = nd_element_get_attr(b->dom, "alt");
        const char *src = b->image_src;
        if (alt && *alt) g_string_append_printf(out, "[image: %s]\n", alt);
        else if (src)    g_string_append_printf(out, "[image: %s]\n", src);
        else             g_string_append(out, "[image]\n");
    }
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        dump_text_walk(c, out);
}

static void
dump_layout_walk(const nd_box *b, int indent, GString *out)
{
    if (!b) return;
    for (int i = 0; i < indent; i++) g_string_append_c(out, ' ');
    g_string_append_printf(out, "%s @(%.0f,%.0f) %.0fx%.0f",
        nd_box_kind_name(b->kind), b->x, b->y,
        b->content_width, b->content_height);
    if (b->dom && b->dom->name) g_string_append_printf(out, " <%s>", b->dom->name);
    if (b->image_src) g_string_append_printf(out, " img=%s", b->image_src);
    if (b->text && *b->text) {
        gsize n = strlen(b->text);
        if (n > 40) {
            g_string_append_printf(out, " text=\"%.40s…\"", b->text);
        } else {
            g_string_append_printf(out, " text=\"%s\"", b->text);
        }
    }
    g_string_append_c(out, '\n');
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        dump_layout_walk(c, indent + 2, out);
}

static void
fetch_images_into_cache(nd_box *root, const char *base_url,
                        nd_image_cache *cache)
{
    if (!root || !base_url || !cache) return;
    GPtrArray *imgs = g_ptr_array_new();
    nd_layout_collect_images(root, imgs);
    for (guint i = 0; i < imgs->len; i++) {
        nd_box *box = g_ptr_array_index(imgs, i);
        const char *src = box->image_src ? box->image_src : box->bg_image_src;
        if (!src) continue;
        char *abs = nd_url_resolve(base_url, src);
        if (!abs) continue;
        if (nd_image_cache_peek(cache, abs)) { g_free(abs); continue; }
        nd_response *resp = fetch_url_blocking(abs, NULL);
        if (resp && !resp->error && resp->body && resp->body->len > 0) {
            int w = 0, h = 0;
            GdkTexture *tex = nd_image_decode_bytes(
                resp->body->data, resp->body->len, &w, &h);
            if (tex)
                nd_image_cache_insert_loaded(cache, abs, tex, w, h);
        }
        if (resp) nd_response_free(resp);
        g_free(abs);
    }
    g_ptr_array_free(imgs, TRUE);
}

static int
write_png(const nd_box *root, const char *path)
{
    if (!root || !path) return 2;
    int w = (int)root->content_width;
    if (w <= 0) w = 1024;
    int h = (int)root->content_height + 32;
    if (h <= 0) h = 768;
    const int kCairoMax = 30000;
    if (w > kCairoMax) w = kCairoMax;
    if (h > kCairoMax) {
        fprintf(stderr,
            "headless: page is %d px tall; PNG capped at %d (cairo limit)\n",
            h, kCairoMax);
        h = kCairoMax;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        fprintf(stderr, "headless: failed to create PNG surface\n");
        return 2;
    }
    cairo_t *cr = cairo_create(surf);
    nd_paint(cr, root, NULL);
    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "headless: PNG write failed: %s\n",
                cairo_status_to_string(st));
        return 2;
    }
    return 0;
}

static int
write_pdf(const nd_box *root, const char *path)
{
    if (!root || !path) return 2;
    double w = root->content_width > 0 ? root->content_width : 595.0;
    double h = root->content_height > 0 ? (root->content_height + 32) : 842.0;
    cairo_surface_t *surf = cairo_pdf_surface_create(path, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        fprintf(stderr, "headless: failed to create PDF surface\n");
        return 2;
    }
    cairo_t *cr = cairo_create(surf);
    nd_paint(cr, root, NULL);
    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return 0;
}

static void
fetch_external_stylesheets(nd_node *doc, const char *base_url, GPtrArray *out)
{
    if (!doc || !base_url) return;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GQueue queue = G_QUEUE_INIT;
    g_queue_push_tail(&queue, doc);
    while (!g_queue_is_empty(&queue)) {
        nd_node *n = g_queue_pop_head(&queue);
        if (nd_node_is_element_named(n, "link")) {
            const char *rel  = nd_element_get_attr(n, "rel");
            const char *href = nd_element_get_attr(n, "href");
            if (rel && href && *href &&
                g_ascii_strcasecmp(rel, "stylesheet") == 0) {
                char *abs = nd_url_resolve(base_url, href);
                if (abs && !g_hash_table_contains(seen, abs)) {
                    g_hash_table_add(seen, g_strdup(abs));
                    nd_response *resp = fetch_url_blocking(abs, NULL);
                    if (resp && !resp->error &&
                        resp->body && resp->body->len > 0) {
                        nd_css_stylesheet *sh = nd_css_stylesheet_parse(
                            (const char *)resp->body->data,
                            (gssize)resp->body->len);
                        if (sh) g_ptr_array_add(out, sh);
                    }
                    if (resp) nd_response_free(resp);
                }
                g_free(abs);
            }
        }
        for (nd_node *c = n->first_child; c; c = c->next_sibling)
            g_queue_push_tail(&queue, c);
    }
    g_hash_table_destroy(seen);
}

static GHashTable *
compute_cascade(nd_node *doc, const char *base_url)
{
    GPtrArray *page_sheets = g_ptr_array_new();
    nd_collect_inline_stylesheets(doc, page_sheets);
    fetch_external_stylesheets(doc, base_url, page_sheets);
    nd_css_stylesheet *compat = nd_compat_stylesheet_for_url(base_url);
    if (compat) g_ptr_array_add(page_sheets, compat);
    GHashTable *styles = nd_css_compute(doc,
        (const nd_css_stylesheet *const *)page_sheets->pdata,
        page_sheets->len);
    for (guint i = 0; i < page_sheets->len; i++)
        nd_css_stylesheet_free(g_ptr_array_index(page_sheets, i));
    g_ptr_array_free(page_sheets, TRUE);
    return styles;
}

static void
headless_js_log(const char *line, gpointer user_data)
{
    (void)user_data;
    fprintf(stderr, "[js] %s\n", line);
}

static void
headless_js_mutated(gpointer user_data) { (void)user_data; }

static void
headless_js_navigate(const char *url, gboolean reload, gpointer user_data)
{
    (void)reload; (void)user_data;
    fprintf(stderr, "[js navigate %s]\n", url ? url : "");
}

int
nd_headless_run(const nd_headless_opts *opts)
{
    if (!opts || !opts->url || !*opts->url) {
        fprintf(stderr, "headless: --url is required\n");
        return 2;
    }
#ifdef G_OS_WIN32
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    GError *err = NULL;
    const char *fetch_target = opts->url;
    char *consent_target = nd_google_unwrap_consent_url(opts->url);
    if (consent_target) fetch_target = consent_target;
    char *google_rewrite = nd_google_rewrite_url(fetch_target);
    if (google_rewrite) fetch_target = google_rewrite;
    nd_response *resp = fetch_url_blocking(fetch_target, &err);
    g_free(consent_target);
    g_free(google_rewrite);
    if (!resp) {
        fprintf(stderr, "headless: fetch failed: %s\n",
                err ? err->message : "unknown error");
        g_clear_error(&err);
        return 1;
    }
    if (resp->error) {
        fprintf(stderr, "headless: fetch error: %s\n", resp->error);
        nd_response_free(resp);
        return 1;
    }

    const char *raw = resp->body ? (const char *)resp->body->data : "";
    gsize raw_len = resp->body ? resp->body->len : 0;
    char *decoded = nd_html_decode_body(raw, raw_len);
    const char *page_url = resp->final_url ? resp->final_url : opts->url;
    if (decoded && nd_youtube_is_watch_url(page_url)) {
        char *rewritten = nd_youtube_render_watch_page(page_url, decoded,
                                                       strlen(decoded));
        if (rewritten) { g_free(decoded); decoded = rewritten; }
    }
    nd_node *doc = nd_html_parse_for_url(page_url,
                                         decoded ? decoded : "",
                                         decoded ? (gssize)strlen(decoded) : 0);
    nd_compat_rewrite_doc(doc, page_url);

    const nd_config *cfg = nd_config_get();
    nd_js *js = NULL;
    if (cfg && cfg->javascript_enabled) {
        js = nd_js_new(headless_js_log, NULL,
                       headless_js_mutated, NULL,
                       headless_js_navigate, NULL);
        if (js) nd_js_run_scripts_in_doc(js, doc, resp->final_url);
    }

    if (opts->settle_ms > 0) settle_main_loop(opts->settle_ms);

    int vw = opts->viewport_width > 0 ? opts->viewport_width : 1000;
    nd_css_set_viewport((double)vw, (double)vw * 0.75);
    GHashTable *styles = compute_cascade(doc, page_url);
    nd_box *layout = nd_layout_build(doc, styles, (double)vw, NULL, 0, NULL, NULL);
    if (js) {
        nd_js_set_layout_root(js, layout);
        if (opts->settle_ms > 0) settle_main_loop(opts->settle_ms);
    }

    int rc = 0;
    GString *out = g_string_new(NULL);
    nd_image_cache *image_cache = NULL;

    switch (opts->dump) {
    case ND_DUMP_TEXT:
        dump_text_walk(layout, out);
        fwrite(out->str, 1, out->len, stdout);
        break;
    case ND_DUMP_DOM: {
        GString *dom = nd_node_dump(doc);
        fwrite(dom->str, 1, dom->len, stdout);
        g_string_free(dom, TRUE);
        break;
    }
    case ND_DUMP_LAYOUT:
        dump_layout_walk(layout, 0, out);
        fwrite(out->str, 1, out->len, stdout);
        break;
    case ND_DUMP_PNG:
    case ND_DUMP_PDF: {
        const char *base = resp->final_url ? resp->final_url : opts->url;
        image_cache = nd_image_cache_new();
        fetch_images_into_cache(layout, base, image_cache);
        nd_box_free(layout);
        layout = nd_layout_build(doc, styles, (double)vw, NULL, 0,
                                 image_cache, base);
        if (js) nd_js_set_layout_root(js, layout);
        nd_paint_set_js(js);
        if (opts->dump == ND_DUMP_PNG)
            rc = write_png(layout, opts->out_path);
        else
            rc = write_pdf(layout, opts->out_path);
        break;
    }
    }
    g_string_free(out, TRUE);

    g_free(decoded);
    if (js)            nd_js_set_layout_root(js, NULL);
    if (layout)        nd_box_free(layout);
    if (styles)        g_hash_table_destroy(styles);
    if (js)            nd_js_free(js);
    if (doc)           nd_node_free(doc);
    if (image_cache)   nd_image_cache_free(image_cache);
    nd_response_free(resp);
    return rc;
}
