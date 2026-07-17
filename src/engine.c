/* Nordstjernen — synchronous fetch/cascade/layout/capture pipeline.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "engine.h"

#include <cairo-pdf.h>
#include <cairo.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "css.h"
#include "debuglog.h"
#include "image.h"
#include "paint.h"
#include "render.h"

typedef struct fetch_state {
    GMainLoop  *loop;
    ns_response *resp;
    GError      *error;
} fetch_state;

static int g_engine_blocking_depth;

gboolean
ns_engine_in_blocking_fetch(void)
{
    return g_engine_blocking_depth > 0;
}

static guint64 g_engine_relayout_count;
static gint64  g_engine_relayout_us;

static void
ns_engine_perf_add_relayout(gint64 elapsed_us)
{
    g_engine_relayout_count++;
    g_engine_relayout_us += elapsed_us;
}

void
ns_engine_layout_perf(guint64 *relayouts, double *total_ms)
{
    if (relayouts) *relayouts = g_engine_relayout_count;
    if (total_ms)  *total_ms  = g_engine_relayout_us / 1000.0;
}

static void
on_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    fetch_state *st = user_data;
    st->resp = ns_net_fetch_finish(result, &st->error);
    g_main_loop_quit(st->loop);
}

ns_response *
ns_engine_fetch_blocking(const char *url, const char *top_url, GError **error)
{
    fetch_state st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);
    ns_net_fetch_async(url, top_url, NULL, on_fetch_done, &st);
    g_engine_blocking_depth++;
    g_main_loop_run(st.loop);
    g_engine_blocking_depth--;
    g_main_loop_unref(st.loop);
    if (error) *error = st.error;
    else g_clear_error(&st.error);
    return st.resp;
}

ns_response *
ns_engine_post_blocking(const char *url, const char *top_url,
                        const void *body, gsize body_len,
                        const char *content_type, GError **error)
{
    fetch_state st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);
    ns_net_post_async(url, top_url, body, body_len, content_type,
                      NULL, on_fetch_done, &st);
    g_engine_blocking_depth++;
    g_main_loop_run(st.loop);
    g_engine_blocking_depth--;
    g_main_loop_unref(st.loop);
    if (error) *error = st.error;
    else g_clear_error(&st.error);
    return st.resp;
}

static gboolean
content_type_is_css(const char *ct)
{
    if (!ct || !*ct) return TRUE; /* missing type: be lenient */
    while (*ct == ' ' || *ct == '\t') ct++;
    return g_ascii_strncasecmp(ct, "text/css", 8) == 0 &&
           (ct[8] == '\0' || ct[8] == ';' || ct[8] == ' ' || ct[8] == '\t');
}

static GBytes *
fetch_css_bytes(const char *url, const char *top_url, GHashTable *cache,
                gboolean strict_mime)
{
    if (!url || !*url) return NULL;
    guint8 attempts = 0;
    if (cache) {
        GBytes *hit = g_hash_table_lookup(cache, url);
        if (hit) {
            gsize hsize = 0;
            const guint8 *hdata = g_bytes_get_data(hit, &hsize);
            gboolean fail_marker = hsize == 0 ||
                (hsize == 1 && hdata && hdata[0] <= 8);
            if (!fail_marker) return g_bytes_ref(hit);
            attempts = hsize == 1 ? hdata[0] : 1;
            if (attempts >= 3) return NULL;
        }
    }
    ns_response *resp = ns_engine_fetch_blocking(url, top_url, NULL);
    GBytes *bytes = NULL;
    gboolean enforce_mime = strict_mime ||
        (resp && ns_net_header_is_nosniff(resp->x_content_type_options));
    gboolean mime_ok = !enforce_mime || !resp ||
                       content_type_is_css(resp->content_type);
    if (resp && !resp->error && resp->status < 400 && mime_ok &&
        resp->body && resp->body->len > 0) {
        bytes = g_bytes_new(resp->body->data, resp->body->len);
        if (cache)
            g_hash_table_insert(cache, g_strdup(url), g_bytes_ref(bytes));
    } else if (cache) {
        guint8 marker = attempts + 1;
        g_hash_table_insert(cache, g_strdup(url),
                            g_bytes_new(&marker, 1));
    }
    if (resp) ns_response_free(resp);
    return bytes;
}

static gboolean
rel_has_token(const char *rel, const char *token)
{
    if (!rel || !token || !*token) return FALSE;
    gchar **parts = g_strsplit_set(rel, " \t\r\n\f", -1);
    gboolean found = FALSE;
    for (gchar **p = parts; *p; p++) {
        if (**p && g_ascii_strcasecmp(*p, token) == 0) {
            found = TRUE;
            break;
        }
    }
    g_strfreev(parts);
    return found;
}

static gboolean
rel_is_stylesheet(const char *rel)
{
    return rel_has_token(rel, "stylesheet") &&
           !rel_has_token(rel, "alternate");
}

static void
preload_collect(const ns_node *n, const char *base, gboolean include_images,
                GPtrArray *out, GHashTable *seen,
                GPtrArray *connect_out, GHashTable *connect_seen, int depth)
{
    if (!n || depth >= 512) return;
    const char *attr = NULL;
    if (include_images && ns_node_is_element_named(n, "img")) {
        const char *loading = ns_element_get_attr(n, "loading");
        if (!loading || g_ascii_strcasecmp(loading, "lazy") != 0)
            attr = ns_element_get_attr(n, "src");
    } else if (ns_node_is_element_named(n, "script"))
        attr = ns_element_get_attr(n, "src");
    else if (ns_node_is_element_named(n, "link")) {
        const char *rel = ns_element_get_attr(n, "rel");
        if (rel && (rel_has_token(rel, "preconnect") ||
                    rel_has_token(rel, "dns-prefetch"))) {
            const char *href = ns_element_get_attr(n, "href");
            char *abs = (href && *href) ? ns_url_resolve(base, href) : NULL;
            char *origin = abs ? ns_url_origin_from(abs) : NULL;
            if (origin && ns_url_is_http_or_https(origin) &&
                !g_hash_table_contains(connect_seen, origin)) {
                g_hash_table_add(connect_seen, g_strdup(origin));
                g_ptr_array_add(connect_out, origin);
                origin = NULL;
            }
            g_free(origin);
            g_free(abs);
        } else if (rel && (rel_has_token(rel, "stylesheet") ||
                           rel_has_token(rel, "preload") ||
                           rel_has_token(rel, "modulepreload") ||
                           rel_has_token(rel, "prefetch"))) {
            attr = ns_element_get_attr(n, "href");
        }
    }
    if (attr && *attr && !g_str_has_prefix(attr, "data:")) {
        char *abs = ns_url_resolve(base, attr);
        if (abs && ns_url_is_http_or_https(abs) &&
            !g_hash_table_contains(seen, abs)) {
            g_hash_table_add(seen, g_strdup(abs));
            g_ptr_array_add(out, abs);
            abs = NULL;
        }
        g_free(abs);
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling)
        preload_collect(c, base, include_images, out, seen,
                        connect_out, connect_seen, depth + 1);
}

static void
on_preload_fetched(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    (void)user_data;
    ns_response *resp = ns_net_fetch_finish(res, NULL);
    if (resp) ns_response_free(resp);
}

void
ns_engine_speculative_preload(ns_node *doc, const char *base_url,
                              gboolean include_images)
{
    if (!doc || !base_url || !ns_url_is_http_or_https(base_url))
        return;
    GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *connects = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    GHashTable *connect_seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, NULL);
    preload_collect(doc, base_url, include_images, urls, seen,
                    connects, connect_seen, 0);
    g_hash_table_destroy(seen);
    g_hash_table_destroy(connect_seen);
    for (guint i = 0; i < connects->len; i++)
        ns_net_preconnect_async(g_ptr_array_index(connects, i));
    for (guint i = 0; i < urls->len; i++)
        ns_net_fetch_async(g_ptr_array_index(urls, i), base_url, NULL,
                           on_preload_fetched, NULL);
    g_ptr_array_free(urls, TRUE);
    g_ptr_array_free(connects, TRUE);
}

static void
append_stylesheet_expanded(GPtrArray *out, ns_css_stylesheet *sh,
                           const char *base_url, const char *top_url,
                           GHashTable *seen, GHashTable *cache, int depth)
{
    if (!out || !sh) return;
    if (depth < NS_CSS_IMPORT_MAX_DEPTH && sh->imports) {
        for (guint i = 0; i < sh->imports->len; i++) {
            ns_css_import *im = &g_array_index(sh->imports, ns_css_import, i);
            if (!im->url || !*im->url) continue;
            if (im->media && *im->media &&
                !ns_css_media_query_matches(im->media))
                continue;
            char *abs = ns_url_resolve(base_url, im->url);
            if (!abs) continue;
            if (seen && g_hash_table_contains(seen, abs)) {
                g_free(abs);
                continue;
            }
            if (seen) g_hash_table_add(seen, g_strdup(abs));
            GBytes *bytes = fetch_css_bytes(abs, top_url, cache, TRUE);
            if (bytes) {
                gsize len = 0;
                const char *data = g_bytes_get_data(bytes, &len);
                ns_css_stylesheet *child =
                    ns_css_stylesheet_parse(data, (gssize)len);
                if (child) {
                    if (im->layer_name)
                        ns_css_stylesheet_force_layer(child, im->layer_name);
                    append_stylesheet_expanded(out, child, abs, top_url, seen,
                                               cache, depth + 1);
                }
                g_bytes_unref(bytes);
            }
            g_free(abs);
        }
    }
    ns_css_stylesheet_resolve_urls(sh, base_url);
    g_ptr_array_add(out, sh);
}

typedef struct {
    GPtrArray  *out;
    GHashTable *cache;
    GString    *run;
    const char *run_base;
    const char *top_url;
    gboolean    strict_css_mime;
} sheet_collect_ctx;

static void
sheet_run_flush(sheet_collect_ctx *cc)
{
    if (!cc->run || cc->run->len == 0) return;
    ns_css_stylesheet *sh =
        ns_css_merged_styles_cached(cc->run->str, (gssize)cc->run->len);
    if (sh) {
        GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, NULL);
        append_stylesheet_expanded(cc->out, sh, cc->run_base, cc->top_url, seen,
                                   cc->cache, 0);
        g_hash_table_destroy(seen);
    }
    g_string_set_size(cc->run, 0);
    cc->run_base = NULL;
}

static void
collect_stylesheets_walk(ns_node *n, const char *base_url,
                         sheet_collect_ctx *cc, int depth)
{
    if (!n || depth >= 512 || ns_node_is_element_named(n, "noscript")) return;
    if (ns_node_is_element_named(n, "iframe")) {
        sheet_run_flush(cc);
        const char *furl = ns_element_get_attr(n, "data-nd-frame-url");
        if (furl && *furl) base_url = furl;
    }
    GPtrArray *out = cc->out;
    GHashTable *cache = cc->cache;
    if (ns_node_is_element_named(n, "style")) {
        char *css = ns_css_style_element_text(n);
        if (css) {
            if (cc->run_base && cc->run_base != base_url)
                sheet_run_flush(cc);
            if (strstr(css, "@import")) {
                sheet_run_flush(cc);
                ns_css_stylesheet *sh =
                    ns_css_stylesheet_from_style_element_cached(n);
                if (sh) {
                    GHashTable *seen =
                        g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
                    append_stylesheet_expanded(out, sh, base_url, cc->top_url,
                                               seen, cache, 0);
                    g_hash_table_destroy(seen);
                }
            } else {
                g_string_append(cc->run, css);
                g_string_append_c(cc->run, '\n');
                cc->run_base = base_url;
            }
            g_free(css);
        }
    } else if (ns_node_is_element_named(n, "link") && base_url) {
        sheet_run_flush(cc);
        const char *rel = ns_element_get_attr(n, "rel");
        const char *href = ns_element_get_attr(n, "href");
        const char *media = ns_element_get_attr(n, "media");
        if (href && *href && rel_is_stylesheet(rel) &&
            (!media || !*media || ns_css_media_query_matches(media))) {
            char *abs = ns_url_resolve(base_url, href);
            GBytes *bytes = fetch_css_bytes(abs, cc->top_url, cache,
                                            cc->strict_css_mime);
            if (bytes) {
                gsize len = 0;
                const char *data = g_bytes_get_data(bytes, &len);
                ns_css_stylesheet *sh =
                    ns_css_stylesheet_parse_url_cached(abs, data, (gssize)len);
                if (sh) {
                    GHashTable *seen =
                        g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, NULL);
                    if (abs) g_hash_table_add(seen, g_strdup(abs));
                    append_stylesheet_expanded(out, sh, abs, cc->top_url, seen,
                                               cache, 0);
                    g_hash_table_destroy(seen);
                }
                g_bytes_unref(bytes);
            }
            g_free(abs);
        }
    }
    for (ns_node *c = n->first_child; c; c = c->next_sibling)
        collect_stylesheets_walk(c, base_url, cc, depth + 1);
}

void
ns_engine_collect_stylesheets(ns_node *doc, const char *base_url,
                              GPtrArray *out, GHashTable *css_cache)
{
    sheet_collect_ctx cc = {
        .out = out, .cache = css_cache,
        .run = g_string_new(NULL), .run_base = NULL,
        .top_url = base_url,
        .strict_css_mime = doc && !(doc->flags & NS_NODE_QUIRKS),
    };
    collect_stylesheets_walk(doc, base_url, &cc, 0);
    sheet_run_flush(&cc);
    g_string_free(cc.run, TRUE);
}

GHashTable *
ns_engine_compute_cascade(ns_node *doc, const char *base_url,
                          GHashTable *css_cache)
{
    ns_css_relayout_enter();
    ns_css_set_doc_base(base_url);
    ns_css_style_element_cache_begin();
    GPtrArray *page_sheets = g_ptr_array_new();
    ns_engine_collect_stylesheets(doc, base_url, page_sheets, css_cache);
    GHashTable *styles = ns_css_compute(doc,
        (const ns_css_stylesheet *const *)page_sheets->pdata,
        page_sheets->len);
    for (guint i = 0; i < page_sheets->len; i++)
        ns_css_stylesheet_free(g_ptr_array_index(page_sheets, i));
    g_ptr_array_free(page_sheets, TRUE);
    ns_css_relayout_leave();
    return styles;
}

GHashTable *
ns_engine_relayout(ns_node *doc, const char *base_url,
                   int viewport_width, double viewport_height,
                   ns_image_cache *images, ns_anim *anim,
                   ns_js *js, GHashTable *css_cache,
                   const ns_node *focused, const ns_node *hover,
                   gsize caret_byte,
                   gsize sel_anchor_byte, ns_box **out_layout)
{
    ns_css_relayout_enter();
    ns_css_set_doc_base(base_url);
    ns_css_style_element_cache_begin();
    GPtrArray *sheets = g_ptr_array_new();
    ns_engine_collect_stylesheets(doc, base_url, sheets, css_cache);

    ns_render_ctx rc = {
        .doc             = doc,
        .sheets          = (const ns_css_stylesheet *const *)sheets->pdata,
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
        .hover_node      = hover,
        .caret_byte      = caret_byte,
        .sel_anchor_byte = sel_anchor_byte,
    };
    static int profile_env = -1;
    if (profile_env < 0)
        profile_env = g_getenv("NS_PROFILE") != NULL ? 1 : 0;
    GHashTable *styles;
    gint64 relayout_t0 = g_get_monotonic_time();
    if (profile_env) {
        ns_render_profile prof;
        gint64 t0 = g_get_monotonic_time();
        styles = ns_render_relayout_profile(&rc, out_layout, &prof);
        gint64 total = g_get_monotonic_time() - t0;
        guint nstyles = styles ? g_hash_table_size(styles) : 0u;
        g_printerr("[profile] relayout vw=%d nodes=%u total=%.2fms "
                   "css=%.2f style=%.2f layout=%.2f",
                   viewport_width, nstyles, total / 1000.0,
                   prof.css1_us / 1000.0, prof.style1_us / 1000.0,
                   prof.layout1_us / 1000.0);
        if (prof.container_pass)
            g_printerr(" | containers=%u cq_collect=%.2f css2=%.2f style2=%.2f "
                       "layout2=%.2f",
                       prof.containers, prof.container_us / 1000.0,
                       prof.css2_us / 1000.0, prof.style2_us / 1000.0,
                       prof.layout2_us / 1000.0);
        g_printerr("\n");
    } else {
        styles = ns_render_relayout(&rc, out_layout);
    }
    ns_engine_perf_add_relayout(g_get_monotonic_time() - relayout_t0);
    ns_debug_log_emit(NS_DLOG_RENDER, "relayout", "styles=%u vw=%d",
                      styles ? g_hash_table_size(styles) : 0u, viewport_width);

    for (guint i = 0; i < sheets->len; i++)
        ns_css_stylesheet_free(g_ptr_array_index(sheets, i));
    g_ptr_array_free(sheets, TRUE);
    ns_css_relayout_leave();
    return styles;
}

void
ns_engine_load_keyframes(ns_anim *anim, ns_node *doc, const char *base_url,
                         GHashTable *css_cache)
{
    if (!anim) return;
    GPtrArray *sheets = g_ptr_array_new();
    ns_engine_collect_stylesheets(doc, base_url, sheets, css_cache);
    for (guint i = 0; i < sheets->len; i++) {
        const ns_css_stylesheet *sh = g_ptr_array_index(sheets, i);
        if (sh) ns_anim_load_from_stylesheet(anim, sh);
    }
    for (guint i = 0; i < sheets->len; i++)
        ns_css_stylesheet_free(g_ptr_array_index(sheets, i));
    g_ptr_array_free(sheets, TRUE);
}

void
ns_engine_anim_observe(ns_anim *anim, GHashTable *styles, gint64 now_us)
{
    if (!anim || !styles) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val))
        ns_anim_observe(anim, (const ns_node *)key, (const ns_style *)val, now_us);
    ns_anim_prune(anim, styles);
}

typedef struct {
    GMainLoop      *loop;
    int             pending;
    ns_image_cache *cache;
} imgs_fetch_state;

typedef struct {
    imgs_fetch_state *st;
    char             *abs;
} img_fetch_item;

static void
on_image_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    img_fetch_item *it = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (resp && !resp->error && resp->body && resp->body->len > 0) {
        int w = 0, h = 0;
        ns_texture *tex = ns_image_decode_bytes(resp->body->data,
                                                resp->body->len, &w, &h);
        if (tex)
            ns_image_cache_insert_loaded(it->st->cache, it->abs, tex, w, h);
    }
    if (resp) ns_response_free(resp);
    g_clear_error(&err);
    if (--it->st->pending == 0)
        g_main_loop_quit(it->st->loop);
    g_free(it->abs);
    g_free(it);
}

#define NS_LAZY_IMAGE_MARGIN_PX 4000.0

static gboolean
engine_image_is_lazy(const ns_box *box)
{
    if (!box || box->kind != NS_BOX_IMAGE || !box->dom) return FALSE;
    const char *l = ns_element_get_attr(box->dom, "loading");
    return l && g_ascii_strcasecmp(l, "lazy") == 0;
}

static GHashTable *
engine_collect_wanted_images(ns_box *root, const char *base_url,
                             ns_image_cache *cache, double scroll_y,
                             double viewport_h, gboolean *deferred_any)
{
    GPtrArray *imgs = g_ptr_array_new();
    ns_layout_collect_images(root, imgs);
    GHashTable *wanted = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    double lazy_limit = (viewport_h > 0.0)
        ? scroll_y + viewport_h + NS_LAZY_IMAGE_MARGIN_PX : G_MAXDOUBLE;
    for (guint i = 0; i < imgs->len; i++) {
        ns_box *box = g_ptr_array_index(imgs, i);
        if (!box->media) continue;
        if (box->y > lazy_limit && engine_image_is_lazy(box)) {
            if (deferred_any) *deferred_any = TRUE;
            continue;
        }
        GPtrArray *srcs = g_ptr_array_new();
        if (box->media->image_src)
            g_ptr_array_add(srcs, box->media->image_src);
        else if (box->media->bg_layer_srcs) {
            for (guint li = 0; li < box->media->bg_layer_srcs->len; li++) {
                char *lsrc = g_ptr_array_index(box->media->bg_layer_srcs, li);
                if (lsrc) g_ptr_array_add(srcs, lsrc);
            }
        } else if (box->media->bg_image_src)
            g_ptr_array_add(srcs, box->media->bg_image_src);
        if (box->media->marker_image_src)
            g_ptr_array_add(srcs, box->media->marker_image_src);
        for (guint si = 0; si < srcs->len; si++) {
            const char *src = g_ptr_array_index(srcs, si);
            if (g_str_has_prefix(src, "nd-inline-svg:")) continue;
            char *abs = ns_url_resolve(base_url, src);
            if (!abs) continue;
            if (ns_image_cache_peek(cache, abs) ||
                g_hash_table_contains(wanted, abs)) {
                g_free(abs);
                continue;
            }
            g_hash_table_add(wanted, abs);
        }
        g_ptr_array_free(srcs, TRUE);
    }
    g_ptr_array_free(imgs, TRUE);
    return wanted;
}

struct ns_engine_img_session {
    int             refs;
    gboolean        dead;
    int             outstanding;
    ns_image_cache *cache;
    void          (*arrived_cb)(gpointer user_data);
    gpointer        user_data;
};

static void
img_session_unref(ns_engine_img_session *s)
{
    if (--s->refs == 0)
        g_free(s);
}

typedef struct img_async_item {
    ns_engine_img_session *session;
    char                  *abs;
} img_async_item;

static void
on_image_fetch_async_done(GObject *src, GAsyncResult *result,
                          gpointer user_data)
{
    (void)src;
    img_async_item *it = user_data;
    ns_engine_img_session *s = it->session;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (!s->dead && resp && !resp->error && resp->body &&
        resp->body->len > 0) {
        int w = 0, h = 0;
        ns_texture *tex = ns_image_decode_bytes(resp->body->data,
                                                resp->body->len, &w, &h);
        if (tex)
            ns_image_cache_insert_loaded(s->cache, it->abs, tex, w, h);
    }
    if (resp) ns_response_free(resp);
    g_clear_error(&err);
    if (s->outstanding > 0) s->outstanding--;
    if (!s->dead && s->arrived_cb)
        s->arrived_cb(s->user_data);
    img_session_unref(s);
    g_free(it->abs);
    g_free(it);
}

ns_engine_img_session *
ns_engine_fetch_images_start(ns_box *root, const char *base_url,
                             ns_image_cache *cache,
                             GHashTable *requested,
                             double scroll_y, double viewport_h,
                             gboolean *deferred_any,
                             void (*arrived_cb)(gpointer user_data),
                             gpointer user_data)
{
    if (!root || !base_url || !cache) return NULL;
    GHashTable *wanted = engine_collect_wanted_images(root, base_url, cache,
                                                      scroll_y, viewport_h,
                                                      deferred_any);
    if (requested) {
        GHashTableIter rit;
        gpointer rkey;
        g_hash_table_iter_init(&rit, wanted);
        while (g_hash_table_iter_next(&rit, &rkey, NULL))
            if (g_hash_table_contains(requested, rkey))
                g_hash_table_iter_remove(&rit);
        g_hash_table_iter_init(&rit, wanted);
        while (g_hash_table_iter_next(&rit, &rkey, NULL))
            g_hash_table_add(requested, g_strdup(rkey));
    }
    guint n = g_hash_table_size(wanted);
    if (n == 0) {
        g_hash_table_destroy(wanted);
        return NULL;
    }

    ns_engine_img_session *s = g_new0(ns_engine_img_session, 1);
    s->refs = 1;
    s->outstanding = (int)n;
    s->cache = cache;
    s->arrived_cb = arrived_cb;
    s->user_data = user_data;

    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, wanted);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        img_async_item *item = g_new0(img_async_item, 1);
        item->session = s;
        item->abs = g_strdup(key);
        s->refs++;
        ns_net_fetch_async(item->abs, base_url, NULL,
                           on_image_fetch_async_done, item);
    }
    g_hash_table_destroy(wanted);
    return s;
}

int
ns_engine_img_session_outstanding(const ns_engine_img_session *s)
{
    return s ? s->outstanding : 0;
}

void
ns_engine_img_session_close(ns_engine_img_session *s)
{
    if (!s) return;
    s->dead = TRUE;
    s->arrived_cb = NULL;
    img_session_unref(s);
}

void
ns_engine_fetch_images(ns_box *root, const char *base_url,
                       ns_image_cache *cache)
{
    if (!root || !base_url || !cache) return;
    GHashTable *wanted = engine_collect_wanted_images(root, base_url, cache,
                                                      0.0, 0.0, NULL);

    guint n = g_hash_table_size(wanted);
    if (n == 0) {
        g_hash_table_destroy(wanted);
        return;
    }

    imgs_fetch_state st = {0};
    st.loop = g_main_loop_new(NULL, FALSE);
    st.pending = (int)n;
    st.cache = cache;

    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, wanted);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        img_fetch_item *item = g_new0(img_fetch_item, 1);
        item->st = &st;
        item->abs = g_strdup(key);
        ns_net_fetch_async(item->abs, base_url, NULL,
                           on_image_fetch_done, item);
    }
    g_engine_blocking_depth++;
    g_main_loop_run(st.loop);
    g_engine_blocking_depth--;
    g_main_loop_unref(st.loop);
    g_hash_table_destroy(wanted);
}

int
ns_engine_write_png(const ns_box *root, const char *path)
{
    if (!root || !path) return 2;
    const int kCairoMax = 30000;
    double cw = root->content_width;
    if (!(cw > 0)) cw = 1024;
    if (cw > kCairoMax) cw = kCairoMax;
    int w = (int)cw;
    double max_bottom = ns_box_max_bottom(root, root->content_height);
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
    ns_paint(cr, root, NULL);
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
ns_engine_write_pdf(const ns_box *root, const char *path)
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
    cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_CREATOR,
                                   "Nordstjernen");
    time_t now = time(NULL);
    struct tm tm_utc;
#if defined(_WIN32)
    int have_tm = gmtime_s(&tm_utc, &now) == 0;
#else
    int have_tm = gmtime_r(&now, &tm_utc) != NULL;
#endif
    if (have_tm) {
        char iso[32];
        if (strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) > 0)
            cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_CREATE_DATE,
                                           iso);
    }
    cairo_t *cr = cairo_create(surf);
    ns_paint(cr, root, NULL);
    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return 0;
}

void
ns_engine_dump_text(const ns_box *b, GString *out)
{
    if (!b) return;
    if (b->kind == NS_BOX_INLINE && b->text && *b->text) {
        g_string_append(out, b->text);
        g_string_append_c(out, '\n');
    } else if (b->kind == NS_BOX_IMAGE && b->dom) {
        const char *alt = ns_element_get_attr(b->dom, "alt");
        const char *src = b->media ? b->media->image_src : NULL;
        if (alt && *alt) g_string_append_printf(out, "[image: %s]\n", alt);
        else if (src)    g_string_append_printf(out, "[image: %s]\n", src);
        else             g_string_append(out, "[image]\n");
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        ns_engine_dump_text(c, out);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            ns_engine_dump_text(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box, out);
}

void
ns_engine_dump_layout(const ns_box *b, int indent, GString *out)
{
    if (!b) return;
    for (int i = 0; i < indent; i++) g_string_append_c(out, ' ');
    g_string_append_printf(out, "%s @(%.0f,%.0f) %.0fx%.0f",
        ns_box_kind_name(b->kind), b->x, b->y,
        b->content_width, b->content_height);
    if (b->dom && b->dom->name) {
        const char *id = b->dom->kind == NS_NODE_ELEMENT
                       ? ns_element_get_attr(b->dom, "id") : NULL;
        if (id && *id)
            g_string_append_printf(out, " <%s#%s>", b->dom->name, id);
        else
            g_string_append_printf(out, " <%s>", b->dom->name);
    }
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
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        ns_engine_dump_layout(c, indent + 2, out);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            ns_engine_dump_layout(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box,
                indent + 2, out);
}

char *
ns_engine_suffix_before_ext(const char *path, const char *suffix)
{
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
    const char *back  = strrchr(path, '\\');
    if (back && (!slash || back > slash)) slash = back;
    const char *dot = strrchr(path, '.');
    if (dot && (!slash || dot > slash))
        return g_strdup_printf("%.*s%s%s",
                               (int)(dot - path), path, suffix, dot);
    return g_strconcat(path, suffix, NULL);
}
