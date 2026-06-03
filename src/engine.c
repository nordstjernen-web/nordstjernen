/* Nordstjernen — synchronous fetch/cascade/layout/capture pipeline.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "engine.h"

#include <cairo-pdf.h>
#include <cairo.h>
#include <stdio.h>
#include <string.h>

#include "css.h"
#include "debuglog.h"
#include "image.h"
#include "paint.h"
#include "render.h"

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

nd_response *
nd_engine_fetch_blocking(const char *url, const char *top_url, GError **error)
{
    fetch_state st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);
    nd_net_fetch_async(url, top_url, NULL, on_fetch_done, &st);
    g_main_loop_run(st.loop);
    g_main_loop_unref(st.loop);
    if (error) *error = st.error;
    else g_clear_error(&st.error);
    return st.resp;
}

static GBytes *
fetch_css_bytes(const char *url, GHashTable *cache)
{
    if (!url || !*url) return NULL;
    if (cache) {
        GBytes *hit = g_hash_table_lookup(cache, url);
        if (hit) return g_bytes_ref(hit);
    }
    nd_response *resp = nd_engine_fetch_blocking(url, NULL, NULL);
    GBytes *bytes = NULL;
    if (resp && !resp->error && resp->status < 400 &&
        resp->body && resp->body->len > 0) {
        bytes = g_bytes_new(resp->body->data, resp->body->len);
        if (cache)
            g_hash_table_insert(cache, g_strdup(url), g_bytes_ref(bytes));
    }
    if (resp) nd_response_free(resp);
    return bytes;
}

static void
append_stylesheet_expanded(GPtrArray *out, nd_css_stylesheet *sh,
                           const char *base_url, GHashTable *seen,
                           GHashTable *cache, int depth)
{
    if (!out || !sh) return;
    if (depth < ND_CSS_IMPORT_MAX_DEPTH && sh->imports) {
        for (guint i = 0; i < sh->imports->len; i++) {
            nd_css_import *im = &g_array_index(sh->imports, nd_css_import, i);
            if (!im->url || !*im->url) continue;
            if (im->media && *im->media &&
                !nd_css_media_query_matches(im->media))
                continue;
            char *abs = nd_url_resolve(base_url, im->url);
            if (!abs) continue;
            if (seen && g_hash_table_contains(seen, abs)) {
                g_free(abs);
                continue;
            }
            if (seen) g_hash_table_add(seen, g_strdup(abs));
            GBytes *bytes = fetch_css_bytes(abs, cache);
            if (bytes) {
                gsize len = 0;
                const char *data = g_bytes_get_data(bytes, &len);
                nd_css_stylesheet *child =
                    nd_css_stylesheet_parse(data, (gssize)len);
                if (child) {
                    if (im->layer_name)
                        nd_css_stylesheet_force_layer(child, im->layer_name);
                    append_stylesheet_expanded(out, child, abs, seen, cache,
                                               depth + 1);
                }
                g_bytes_unref(bytes);
            }
            g_free(abs);
        }
    }
    nd_css_stylesheet_resolve_urls(sh, base_url);
    g_ptr_array_add(out, sh);
}

static void
collect_inline_stylesheets_expanded(nd_node *doc, const char *base_url,
                                    GPtrArray *out, GHashTable *cache)
{
    GPtrArray *inline_sheets = g_ptr_array_new();
    nd_collect_inline_stylesheets(doc, inline_sheets);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    for (guint i = 0; i < inline_sheets->len; i++) {
        nd_css_stylesheet *sh = g_ptr_array_index(inline_sheets, i);
        append_stylesheet_expanded(out, sh, base_url, seen, cache, 0);
    }
    g_hash_table_destroy(seen);
    g_ptr_array_free(inline_sheets, TRUE);
}

static void
fetch_external_stylesheets(nd_node *doc, const char *base_url, GPtrArray *out,
                           GHashTable *cache)
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
            const char *media = nd_element_get_attr(n, "media");
            if (rel && href && *href &&
                g_ascii_strcasecmp(rel, "stylesheet") == 0 &&
                (!media || !*media || nd_css_media_query_matches(media))) {
                char *abs = nd_url_resolve(base_url, href);
                if (abs && !g_hash_table_contains(seen, abs)) {
                    g_hash_table_add(seen, g_strdup(abs));
                    GBytes *bytes = fetch_css_bytes(abs, cache);
                    if (bytes) {
                        gsize len = 0;
                        const char *data = g_bytes_get_data(bytes, &len);
                        nd_css_stylesheet *sh =
                            nd_css_stylesheet_parse(data, (gssize)len);
                        if (sh)
                            append_stylesheet_expanded(out, sh, abs, seen,
                                                       cache, 0);
                        g_bytes_unref(bytes);
                    }
                }
                g_free(abs);
            }
        }
        for (nd_node *c = n->first_child; c; c = c->next_sibling)
            g_queue_push_tail(&queue, c);
    }
    g_hash_table_destroy(seen);
}

void
nd_engine_collect_stylesheets(nd_node *doc, const char *base_url,
                              GPtrArray *out, GHashTable *css_cache)
{
    collect_inline_stylesheets_expanded(doc, base_url, out, css_cache);
    fetch_external_stylesheets(doc, base_url, out, css_cache);
}

GHashTable *
nd_engine_compute_cascade(nd_node *doc, const char *base_url,
                          GHashTable *css_cache)
{
    GPtrArray *page_sheets = g_ptr_array_new();
    nd_engine_collect_stylesheets(doc, base_url, page_sheets, css_cache);
    GHashTable *styles = nd_css_compute(doc,
        (const nd_css_stylesheet *const *)page_sheets->pdata,
        page_sheets->len);
    for (guint i = 0; i < page_sheets->len; i++)
        nd_css_stylesheet_free(g_ptr_array_index(page_sheets, i));
    g_ptr_array_free(page_sheets, TRUE);
    return styles;
}

GHashTable *
nd_engine_relayout(nd_node *doc, const char *base_url,
                   int viewport_width, double viewport_height,
                   nd_image_cache *images, nd_anim *anim,
                   nd_js *js, GHashTable *css_cache,
                   const nd_node *focused, gsize caret_byte,
                   gsize sel_anchor_byte, nd_box **out_layout)
{
    GPtrArray *sheets = g_ptr_array_new();
    nd_engine_collect_stylesheets(doc, base_url, sheets, css_cache);

    nd_render_ctx rc = {
        .doc             = doc,
        .sheets          = (const nd_css_stylesheet *const *)sheets->pdata,
        .n_sheets        = sheets->len,
        .viewport_width  = (double)viewport_width,
        .viewport_height = viewport_height > 0 ? viewport_height
                                               : (double)viewport_width * 0.75,
        .zoom            = 1.0,
        .images          = images,
        .base_url        = base_url,
        .anim            = anim,
        .js              = js,
        .focused_input   = focused,
        .caret_byte      = caret_byte,
        .sel_anchor_byte = sel_anchor_byte,
    };
    GHashTable *styles = nd_render_relayout(&rc, out_layout);
    nd_debug_log_emit(ND_DLOG_RENDER, "relayout", "styles=%u vw=%d",
                      styles ? g_hash_table_size(styles) : 0u, viewport_width);

    for (guint i = 0; i < sheets->len; i++)
        nd_css_stylesheet_free(g_ptr_array_index(sheets, i));
    g_ptr_array_free(sheets, TRUE);
    return styles;
}

void
nd_engine_load_keyframes(nd_anim *anim, nd_node *doc, const char *base_url,
                         GHashTable *css_cache)
{
    if (!anim) return;
    GPtrArray *sheets = g_ptr_array_new();
    nd_engine_collect_stylesheets(doc, base_url, sheets, css_cache);
    for (guint i = 0; i < sheets->len; i++) {
        const nd_css_stylesheet *sh = g_ptr_array_index(sheets, i);
        if (sh) nd_anim_load_from_stylesheet(anim, sh);
    }
    for (guint i = 0; i < sheets->len; i++)
        nd_css_stylesheet_free(g_ptr_array_index(sheets, i));
    g_ptr_array_free(sheets, TRUE);
}

void
nd_engine_anim_observe(nd_anim *anim, GHashTable *styles, gint64 now_us)
{
    if (!anim || !styles) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val))
        nd_anim_observe(anim, (const nd_node *)key, (const nd_style *)val, now_us);
    nd_anim_prune(anim, styles);
}

void
nd_engine_fetch_images(nd_box *root, const char *base_url,
                       nd_image_cache *cache)
{
    if (!root || !base_url || !cache) return;
    GPtrArray *imgs = g_ptr_array_new();
    nd_layout_collect_images(root, imgs);
    for (guint i = 0; i < imgs->len; i++) {
        nd_box *box = g_ptr_array_index(imgs, i);
        if (!box->media) continue;
        const char *src = box->media->image_src
                          ? box->media->image_src
                          : box->media->bg_image_src;
        if (!src) continue;
        if (g_str_has_prefix(src, "nd-inline-svg:")) continue;
        char *abs = nd_url_resolve(base_url, src);
        if (!abs) continue;
        if (nd_image_cache_peek(cache, abs)) { g_free(abs); continue; }
        nd_response *resp = nd_engine_fetch_blocking(abs, base_url, NULL);
        if (resp && !resp->error && resp->body && resp->body->len > 0) {
            int w = 0, h = 0;
            nd_texture *tex = nd_image_decode_bytes(
                resp->body->data, resp->body->len, &w, &h);
            if (tex)
                nd_image_cache_insert_loaded(cache, abs, tex, w, h);
        }
        if (resp) nd_response_free(resp);
        g_free(abs);
    }
    g_ptr_array_free(imgs, TRUE);
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

int
nd_engine_write_png(const nd_box *root, const char *path)
{
    if (!root || !path) return 2;
    const int kCairoMax = 30000;
    double cw = root->content_width;
    if (!(cw > 0)) cw = 1024;
    if (cw > kCairoMax) cw = kCairoMax;
    int w = (int)cw;
    double max_bottom = root->content_height;
    walk_max_bottom(root, &max_bottom);
    if (!(max_bottom > 0)) max_bottom = 0;
    if (max_bottom > (double)kCairoMax) max_bottom = kCairoMax;
    int h = (int)max_bottom + 32;
    if (h <= 0) h = 768;
    if (w > kCairoMax) w = kCairoMax;
    if (h > kCairoMax) {
        fprintf(stderr,
            "engine: page is %d px tall; PNG capped at %d (cairo limit)\n",
            h, kCairoMax);
        h = kCairoMax;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        fprintf(stderr, "engine: failed to create PNG surface\n");
        return 2;
    }
    cairo_t *cr = cairo_create(surf);
    nd_paint(cr, root, NULL);
    cairo_destroy(cr);
    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "engine: PNG write failed: %s\n",
                cairo_status_to_string(st));
        return 2;
    }
    return 0;
}

int
nd_engine_write_pdf(const nd_box *root, const char *path)
{
    if (!root || !path) return 2;
    double w = root->content_width > 0 ? root->content_width : 595.0;
    double h = root->content_height > 0 ? (root->content_height + 32) : 842.0;
    cairo_surface_t *surf = cairo_pdf_surface_create(path, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        fprintf(stderr, "engine: failed to create PDF surface\n");
        return 2;
    }
    cairo_t *cr = cairo_create(surf);
    nd_paint(cr, root, NULL);
    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return 0;
}

void
nd_engine_dump_text(const nd_box *b, GString *out)
{
    if (!b) return;
    if (b->kind == ND_BOX_INLINE && b->text && *b->text) {
        g_string_append(out, b->text);
        g_string_append_c(out, '\n');
    } else if (b->kind == ND_BOX_IMAGE && b->dom) {
        const char *alt = nd_element_get_attr(b->dom, "alt");
        const char *src = b->media ? b->media->image_src : NULL;
        if (alt && *alt) g_string_append_printf(out, "[image: %s]\n", alt);
        else if (src)    g_string_append_printf(out, "[image: %s]\n", src);
        else             g_string_append(out, "[image]\n");
    }
    for (const nd_box *c = b->first_child; c; c = c->next_sibling)
        nd_engine_dump_text(c, out);
}

void
nd_engine_dump_layout(const nd_box *b, int indent, GString *out)
{
    if (!b) return;
    for (int i = 0; i < indent; i++) g_string_append_c(out, ' ');
    g_string_append_printf(out, "%s @(%.0f,%.0f) %.0fx%.0f",
        nd_box_kind_name(b->kind), b->x, b->y,
        b->content_width, b->content_height);
    if (b->dom && b->dom->name) g_string_append_printf(out, " <%s>", b->dom->name);
    if (b->media && b->media->image_src)
        g_string_append_printf(out, " img=%s", b->media->image_src);
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
        nd_engine_dump_layout(c, indent + 2, out);
}

char *
nd_engine_suffix_before_ext(const char *path, const char *suffix)
{
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
    const char *back  = strrchr(path, '\\');
    if (back > slash) slash = back;
    const char *dot = strrchr(path, '.');
    if (dot && (!slash || dot > slash))
        return g_strdup_printf("%.*s%s%s",
                               (int)(dot - path), path, suffix, dot);
    return g_strconcat(path, suffix, NULL);
}
