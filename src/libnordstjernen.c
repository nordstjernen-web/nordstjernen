/* Nordstjernen — public C embedding API implementation.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "libnordstjernen.h"

#include <cairo.h>
#include <gio/gio.h>
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ai.h"
#include "anim.h"
#include "bytecode_cache.h"
#include "cache.h"
#include "history.h"
#include "config.h"
#include "css.h"
#include "dom.h"
#include "engine.h"
#include "font.h"
#include "forms.h"
#include "html.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"
#include "spellcheck.h"
#include "paint.h"
#include "pdf.h"
#include "render.h"
#include "safebrowsing.h"
#include "security.h"
#include "selection.h"
#include "video.h"
#include "camera.h"
#include "webgl.h"

#define NS_IMAGE_RELAYOUT_BATCH 8

struct ns_browser {
    ns_node        *doc;
    ns_box         *layout;
    GHashTable     *styles;
    ns_js          *js;
    ns_anim        *anim;
    ns_image_cache *images;
    ns_video_cache *videos;
    GHashTable     *css_cache;
    char           *base_url;
    char           *doc_charset;
    char           *doc_language;
    int             vw;
    double          vh;
    gboolean        images_fetched;
    gboolean        has_deferred_lazy;
    gboolean        bfcache_ok;
    double          cur_scroll_x;
    double          cur_scroll_y;
    double          js_scroll_x;
    double          js_scroll_y;
    double          cur_viewport_h;
    GPtrArray      *img_sessions;
    guint           image_arrivals_since_layout;
    GHashTable     *img_requested;
    gboolean        dirty;
    gboolean        relaying;
    char           *pending_nav;
    char           *pending_download;
    GString        *pending_audio;
    char           *refresh_url;
    gint64          refresh_due_us;
    char           *pending_post_body;
    gsize           pending_post_len;
    char           *pending_post_ct;
    gsize           caret_byte;
    gsize           sel_anchor_byte;
    ns_selection    selection;
    const ns_node  *hover_node;
    const ns_node  *open_select;
    gboolean        datalist_suppressed;
    const ns_node  *press_node;
    int             press_x;
    int             press_y;
    int             press_mods;
    gboolean        press_active;
    char           *search_query;
    gboolean        search_case;
    const ns_box   *search_active;
    GString        *console_buf;
    guint64         layout_sig[2];
    int             layout_osc;
    gint64          last_layout_us;
    gint64          damp_until_us;
    gboolean        damp_logged;
    gint64          hover_relayout_us;
    gint64          relayout_cost_us;
    gboolean        hover_restyle_pending;
    ns_box         *sb_box;
    const ns_node  *sb_node;
    double          sb_grab;
    gboolean        sb_dragging;
    int             security;
    char           *remote_ip;
};

#define NS_LAYOUT_OSC_THRESHOLD 6
#define NS_LAYOUT_RAPID_US (100 * 1000)
#define NS_LAYOUT_DAMP_US (700 * 1000)

static guint64
layout_signature_walk(const ns_box *b, guint64 h)
{
    if (!b) return h;
    gint32 q[4] = {
        (gint32)(b->x * 4), (gint32)(b->y * 4),
        (gint32)(b->content_width * 4), (gint32)(b->content_height * 4),
    };
    const guchar *bytes = (const guchar *)q;
    h ^= (guint64)b->kind;
    h *= 0x100000001b3ULL;
    for (gsize i = 0; i < sizeof q; i++) {
        h ^= bytes[i];
        h *= 0x100000001b3ULL;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        h = layout_signature_walk(c, h);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            h = layout_signature_walk(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box, h);
    return h;
}

static guint64
layout_signature(const ns_box *root)
{
    return layout_signature_walk(root, 0xcbf29ce484222325ULL);
}

static void
browser_damp_reset(ns_browser *b)
{
    b->layout_osc = 0;
    b->damp_until_us = 0;
    b->damp_logged = FALSE;
}

typedef struct { double x, y; } browser_scroll_pos;

static void
browser_collect_scroll(const ns_box *b, GHashTable *map)
{
    if (!b) return;
    if (b->dom && (b->scroll_y != 0.0 || b->scroll_x != 0.0)) {
        browser_scroll_pos *p = g_new(browser_scroll_pos, 1);
        p->x = b->scroll_x;
        p->y = b->scroll_y;
        g_hash_table_insert(map, (gpointer)b->dom, p);
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        browser_collect_scroll(c, map);
}

static void
browser_restore_scroll(ns_box *b, GHashTable *map)
{
    if (!b) return;
    if (b->dom) {
        browser_scroll_pos *p = g_hash_table_lookup(map, b->dom);
        if (p) {
            double maxy = b->scroll_max_y > 0 ? b->scroll_max_y : 0;
            double maxx = b->scroll_max_x > 0 ? b->scroll_max_x : 0;
            double y = p->y < 0 ? 0 : (p->y > maxy ? maxy : p->y);
            double x = p->x < 0 ? 0 : (p->x > maxx ? maxx : p->x);
            b->scroll_y = y;
            b->scroll_x = x;
        }
    }
    for (ns_box *c = b->first_child; c; c = c->next_sibling)
        browser_restore_scroll(c, map);
}

static void
browser_relayout(ns_browser *b)
{
    if (b->relaying) { b->dirty = TRUE; return; }
    b->relaying = TRUE;
    if (b->js)
        (void)ns_js_consume_mutated(b->js);
    b->image_arrivals_since_layout = 0;
    GHashTable *scroll_save =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    browser_collect_scroll(b->layout, scroll_save);
    ns_css_set_viewport((double)b->vw, b->vh);
    ns_css_set_doc_language(b->doc_language);
    if (b->js) ns_js_sync_window_metrics(b->js);
    b->search_active = NULL;
    ns_selection_clear(&b->selection);
    if (b->js && b->layout) ns_js_set_layout_root(b->js, NULL);
    if (b->layout) {
        ns_paint_3d_invalidate();
        ns_box_free(b->layout);
        b->layout = NULL;
        b->sb_box = NULL;
        b->sb_node = NULL;
        b->sb_dragging = FALSE;
    }
    if (b->js && b->styles) ns_js_set_style_table(b->js, NULL);
    if (b->styles) { g_hash_table_destroy(b->styles); b->styles = NULL; }
    ns_layout_set_open_select(b->open_select);
    {
        const ns_node *fn = b->js ? ns_js_focused_node(b->js) : NULL;
        gboolean dl_open = !b->datalist_suppressed && fn &&
                           ns_node_is_element_named(fn, "input") &&
                           ns_element_get_attr(fn, "list") != NULL;
        ns_layout_set_datalist_open(dl_open);
    }
    gint64 relayout_t0 = g_get_monotonic_time();
    b->styles = ns_engine_relayout(b->doc, b->base_url, b->vw, b->vh,
                                   b->images, b->anim, b->js,
                                   b->css_cache,
                                   b->js ? ns_js_focused_node(b->js) : NULL,
                                   b->hover_node,
                                   b->caret_byte, b->sel_anchor_byte,
                                   &b->layout);
    b->relayout_cost_us = g_get_monotonic_time() - relayout_t0;
    b->relaying = FALSE;
    if (g_hash_table_size(scroll_save) > 0)
        browser_restore_scroll(b->layout, scroll_save);
    g_hash_table_destroy(scroll_save);
    b->images_fetched = FALSE;
    b->has_deferred_lazy = FALSE;
    if (b->js) {
        ns_js_set_style_table(b->js, b->styles);
        ns_js_set_layout_root(b->js, b->layout);
    }

    gint64 now = g_get_monotonic_time();
    gboolean rapid = now - b->last_layout_us < NS_LAYOUT_RAPID_US;
    b->last_layout_us = now;
    guint64 sig = layout_signature(b->layout);
    if (sig == b->layout_sig[0] || sig == b->layout_sig[1]) {
        if (rapid && b->layout_osc < G_MAXINT) b->layout_osc++;
    } else {
        browser_damp_reset(b);
    }
    b->layout_sig[1] = b->layout_sig[0];
    b->layout_sig[0] = sig;
}

static gboolean
browser_relayout_from_mutation(ns_browser *b)
{
    if (b->layout && b->layout_osc >= NS_LAYOUT_OSC_THRESHOLD) {
        gint64 now = g_get_monotonic_time();
        if (now < b->damp_until_us) {
            b->dirty = TRUE;
            return FALSE;
        }
        b->damp_until_us = now + NS_LAYOUT_DAMP_US;
        if (!b->damp_logged) {
            b->damp_logged = TRUE;
            g_message("nordstjernen: layout dampener engaged "
                      "(script reflow loop with no user input)");
        }
    }
    browser_relayout(b);
    return TRUE;
}

static gboolean
overflow_keyword_hidden(const char *kw)
{
    return kw && (g_ascii_strcasecmp(kw, "hidden") == 0 ||
                  g_ascii_strcasecmp(kw, "clip") == 0);
}

static gboolean
box_axis_overflow_hidden(const ns_box *b, ns_css_prop axis)
{
    if (!b || !b->style) return FALSE;
    const char *kw = ns_style_keyword(b->style, axis);
    if (!kw) kw = ns_style_keyword(b->style, NS_CSS_OVERFLOW);
    return overflow_keyword_hidden(kw);
}

static gboolean
root_axis_overflow_hidden(const ns_box *b, ns_css_prop axis)
{
    if (!b) return FALSE;
    if (b->dom && b->dom->name &&
        (strcmp(b->dom->name, "html") == 0 ||
         strcmp(b->dom->name, "body") == 0) &&
        box_axis_overflow_hidden(b, axis))
        return TRUE;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        if (root_axis_overflow_hidden(c, axis)) return TRUE;
    return FALSE;
}

static int browser_images_outstanding(ns_browser *browser);

static void
browser_image_arrived(gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b) return;
    b->image_arrivals_since_layout++;
    if (b->image_arrivals_since_layout >= NS_IMAGE_RELAYOUT_BATCH ||
        browser_images_outstanding(b) == 0) {
        b->image_arrivals_since_layout = 0;
        b->dirty = TRUE;
    }
}

static int
browser_images_outstanding(ns_browser *browser)
{
    if (!browser->img_sessions) return 0;
    int total = 0;
    for (guint i = 0; i < browser->img_sessions->len; ) {
        ns_engine_img_session *s = g_ptr_array_index(browser->img_sessions, i);
        int o = ns_engine_img_session_outstanding(s);
        if (o == 0) {
            ns_engine_img_session_close(s);
            g_ptr_array_remove_index_fast(browser->img_sessions, i);
            continue;
        }
        total += o;
        i++;
    }
    return total;
}

static void
browser_ensure_images(ns_browser *browser)
{
    if (browser->images_fetched && !browser->has_deferred_lazy) return;
    if (!browser->img_requested)
        browser->img_requested = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
    if (!browser->img_sessions)
        browser->img_sessions = g_ptr_array_new();
    double vp_h = browser->cur_viewport_h > 0.0 ? browser->cur_viewport_h
                                                : browser->vh;
    gboolean deferred = FALSE;
    ns_engine_img_session *s =
        ns_engine_fetch_images_start(browser->layout, browser->base_url,
                                     browser->images, browser->img_requested,
                                     browser->cur_scroll_y, vp_h, &deferred,
                                     browser_image_arrived, browser);
    if (s) g_ptr_array_add(browser->img_sessions, s);
    browser->images_fetched = TRUE;
    browser->has_deferred_lazy = deferred;
}

static void
browser_wait_images(ns_browser *browser)
{
    browser_ensure_images(browser);
    gint64 deadline = g_get_monotonic_time() + (gint64)15 * G_USEC_PER_SEC;
    while (browser_images_outstanding(browser) > 0 &&
           g_get_monotonic_time() < deadline)
        g_main_context_iteration(NULL, TRUE);
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
}

static void
browser_flush(gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b || !b->js) return;
    if (!b->layout || b->dirty || ns_js_consume_mutated(b->js)) {
        browser_relayout(b);
        b->dirty = FALSE;
    }
}

static gboolean
settle_quit_cb(gpointer user_data)
{
    g_main_loop_quit(user_data);
    return G_SOURCE_CONTINUE;
}

typedef struct settle_ctx {
    ns_browser *b;
    GMainLoop  *loop;
    int         quiet_ticks;
} settle_ctx;

#define NS_SETTLE_QUIET_TICKS 3

static gboolean
browser_settle_quiet(ns_browser *b)
{
    if (b->dirty) return FALSE;
    if (b->js && ns_js_has_pending_work(b->js)) return FALSE;
    if (b->js && ns_js_has_pending_animation_frame(b->js)) return FALSE;
    if (b->images && ns_image_cache_has_pending(b->images)) return FALSE;
    if (b->videos && ns_video_cache_has_pending(b->videos)) return FALSE;
    if (g_main_context_pending(NULL)) return FALSE;
    return TRUE;
}

static gboolean
settle_tick_cb(gpointer user_data)
{
    settle_ctx *ctx = user_data;
    ns_browser *b = ctx->b;
    gint64 now = g_get_monotonic_time();
    if (b->images) ns_image_cache_tick(b->images, now);
    if (b->videos && b->layout) {
        ns_video_cache_discover(b->videos, b->layout, b->doc, now);
        ns_video_cache_tick(b->videos, now);
    }
    if (b->anim) ns_anim_tick(b->anim, now);
    if (b->anim && b->js) ns_js_dispatch_anim_events(b->js, b->anim);
    if (b->js) ns_js_run_animation_frame(b->js);
    if (b->dirty || (b->js && ns_js_consume_mutated(b->js))) {
        if (browser_relayout_from_mutation(b))
            b->dirty = FALSE;
    }
    if (browser_settle_quiet(b)) {
        if (++ctx->quiet_ticks >= NS_SETTLE_QUIET_TICKS) {
            g_main_loop_quit(ctx->loop);
            return G_SOURCE_CONTINUE;
        }
    } else {
        ctx->quiet_ticks = 0;
    }
    return G_SOURCE_CONTINUE;
}

static void
browser_settle(ns_browser *b, int settle_ms)
{
    if (settle_ms <= 0) return;
    if (b->videos && b->layout)
        ns_video_cache_discover(b->videos, b->layout, b->doc, g_get_monotonic_time());
    if (browser_settle_quiet(b)) return;
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    settle_ctx ctx = { .b = b, .loop = loop };
    guint quit = g_timeout_add(settle_ms, settle_quit_cb, loop);
    guint tick = g_timeout_add(16, settle_tick_cb, &ctx);
    g_main_loop_run(loop);
    g_source_remove(tick);
    g_source_remove(quit);
    g_main_loop_unref(loop);
}

static void
browser_js_log(const char *line, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !line) return;
    if (!b->console_buf) b->console_buf = g_string_new(NULL);
    if (b->console_buf->len > 256u * 1024u)
        g_string_erase(b->console_buf, 0,
                       (gssize)(b->console_buf->len - 192u * 1024u));
    g_string_append(b->console_buf, line);
    g_string_append_c(b->console_buf, '\n');
}
static void browser_js_mutated(gpointer ud) { ns_browser *b = ud; if (b) b->dirty = TRUE; }

static gboolean
browser_allows_navigation_url(ns_browser *b, const char *url)
{
    if (!url || !g_str_has_prefix(url, "file:")) return TRUE;
    return b && b->base_url && g_str_has_prefix(b->base_url, "file:");
}

static char *
browser_resolve_navigation(ns_browser *b, const char *href)
{
    if (!b || !href) return NULL;
    char *abs = ns_url_resolve(b->base_url, href);
    if (!browser_allows_navigation_url(b, abs)) {
        g_free(abs);
        return NULL;
    }
    return abs;
}

static void browser_js_navigate(const char *url, gboolean reload, gpointer ud)
{
    (void)reload;
    ns_browser *b = ud;
    if (!b || !url || !*url) return;
    g_free(b->pending_nav);
    b->pending_nav = browser_resolve_navigation(b, url);
}

static void browser_js_download(const char *url, const char *filename, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !url || !*url) return;
    char *abs = browser_resolve_navigation(b, url);
    g_free(b->pending_download);
    b->pending_download = g_strdup_printf("%s\t%s", abs ? abs : url,
                                          filename ? filename : "");
    g_free(abs);
}

#define NS_PENDING_AUDIO_MAX 15000

static void
browser_js_audio(const char *command, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !command || !*command) return;
    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[audio-cmd] %s\n", command);
    if (!b->pending_audio) b->pending_audio = g_string_new(NULL);
    if (b->pending_audio->len >= NS_PENDING_AUDIO_MAX) return;
    g_string_append(b->pending_audio, command);
    g_string_append_c(b->pending_audio, '\n');
}

static void
browser_js_video(const void *node, const char *kind, double value, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->js) return;
    ns_js_video_event(b->js, node, kind, value);
}

static gboolean
browser_media_seek(const void *node, double seconds, gpointer ud)
{
    ns_browser *b = ud;
    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[media-seek] to=%.3f\n", seconds);
    if (!b || !b->videos) return FALSE;
    return ns_video_cache_seek_node(b->videos, node, seconds,
                                    g_get_monotonic_time());
}

static void
browser_media_play(const void *node, gboolean play, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return;
    ns_video_cache_set_node_playing(b->videos, node, play,
                                    g_get_monotonic_time());
    if (b->js) ns_js_request_repaint(b->js);
}

static void
browser_media_muted(const void *node, gboolean muted, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return;
    ns_video_cache_set_node_muted(b->videos, node, muted);
    if (b->js) ns_js_request_repaint(b->js);
}

static void
browser_media_volume(const void *node, double volume, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return;
    ns_video_cache_set_node_volume(b->videos, node, volume);
    if (b->js) ns_js_request_repaint(b->js);
}

static gboolean
browser_mse_data(guint stream_id, char kind, const guint8 *data, gsize len,
                 gboolean eos, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return FALSE;
    if (eos) {
        ns_video_cache_mse_eos(b->videos, stream_id);
        return TRUE;
    }
    return ns_video_cache_mse_append(b->videos, stream_id, kind, data, len);
}

static double
browser_mse_buffered(guint stream_id, char kind, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return 0.0;
    return ns_video_cache_mse_buffered(b->videos, stream_id, kind);
}

char *
ns_browser_take_pending_audio(ns_browser *browser)
{
    if (!browser || !browser->pending_audio ||
        browser->pending_audio->len == 0)
        return NULL;
    char *out = g_strdup(browser->pending_audio->str);
    g_string_truncate(browser->pending_audio, 0);
    return out;
}

int
ns_browser_init(void)
{
    ns_config_init();
    if (ns_config_get()->harden_allocator)
        ns_security_harden_allocator();
    ns_net_init();
    ns_net_set_allow_file_urls(TRUE);
    ns_cache_init();
    ns_bytecode_cache_init();
    ns_history_init();
    ns_font_init();
    ns_spell_init();
    return 0;
}

void
ns_browser_sandbox(const char *self_exe)
{
    ns_security_win32_mitigations_init(FALSE);
    ns_security_sandbox_init(self_exe);
    ns_security_seccomp_init();
}

void
ns_browser_shutdown(void)
{
    ns_ai_shutdown();
    ns_font_shutdown();
    ns_bytecode_cache_shutdown();
    ns_history_shutdown();
    ns_cache_shutdown();
    ns_net_shutdown();
    ns_config_shutdown();
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

static const char *
browser_find_meta_refresh(const ns_node *n, int depth)
{
    if (!n || depth > 1024) return NULL;
    if (ns_node_is_element_named(n, "meta")) {
        const char *equiv = ns_element_get_attr(n, "http-equiv");
        if (equiv && g_ascii_strcasecmp(equiv, "refresh") == 0) {
            const char *content = ns_element_get_attr(n, "content");
            if (content && *content) return content;
        }
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const char *found = browser_find_meta_refresh(c, depth + 1);
        if (found) return found;
    }
    return NULL;
}

static void
browser_arm_declarative_refresh(ns_browser *b, const char *header_value)
{
    double seconds = 0.0;
    char *target = NULL;
    gboolean armed = header_value &&
        ns_net_parse_refresh(header_value, &seconds, &target);
    if (!armed) {
        const char *meta = browser_find_meta_refresh(b->doc, 0);
        armed = meta && ns_net_parse_refresh(meta, &seconds, &target);
    }
    if (!armed) return;
    if (target) {
        b->refresh_url = browser_resolve_navigation(b, target);
        g_free(target);
        if (!b->refresh_url) return;
    } else {
        b->refresh_url = g_strdup(b->base_url);
    }
    b->refresh_due_us = g_get_monotonic_time() + (gint64)(seconds * 1e6);
}

static gboolean
browser_content_type_starts(const char *content_type, const char *prefix)
{
    return content_type && prefix &&
        g_ascii_strncasecmp(content_type, prefix, strlen(prefix)) == 0;
}

static gboolean
browser_content_type_is_html(const char *content_type)
{
    return browser_content_type_starts(content_type, "text/html") ||
           browser_content_type_starts(content_type, "application/xhtml");
}

static gboolean
browser_content_type_is_json(const char *content_type)
{
    return browser_content_type_starts(content_type, "application/json") ||
           browser_content_type_starts(content_type, "text/json") ||
           (content_type && strstr(content_type, "+json") != NULL);
}

static gboolean
browser_content_type_is_xml(const char *content_type)
{
    if (!content_type) return FALSE;
    if (strstr(content_type, "xhtml") || strstr(content_type, "svg"))
        return FALSE;
    return browser_content_type_starts(content_type, "text/xml") ||
           browser_content_type_starts(content_type, "application/xml") ||
           strstr(content_type, "+xml") != NULL;
}

static gboolean
browser_content_type_is_pdf(const char *content_type)
{
    return browser_content_type_starts(content_type, "application/pdf") ||
           browser_content_type_starts(content_type, "application/x-pdf");
}

static char *
browser_text_document(const char *url, const char *text)
{
    char *esc_url = ns_html_escape_text(url && *url ? url : "text file");
    char *esc_text = ns_html_escape_text(text ? text : "");
    char *html = g_strconcat(
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>",
        esc_url,
        "</title><style>"
        "body{margin:0;background:#fff;color:#111}"
        "pre{margin:0;padding:12px;font:13px/1.45 ui-monospace,"
        "\"SF Mono\",Menlo,Consolas,monospace;white-space:pre-wrap;"
        "overflow-wrap:anywhere}"
        "</style></head><body><pre>",
        esc_text,
        "</pre></body></html>",
        NULL);
    g_free(esc_url);
    g_free(esc_text);
    return html;
}

static void
browser_prepare_document_response(ns_response *resp)
{
    if (!resp || !resp->body || !resp->content_type)
        return;
    const char *final_url = resp->final_url ? resp->final_url : "";
    char *html = NULL;
    if (browser_content_type_starts(resp->content_type, "image/")) {
        html = ns_html_image_document(final_url);
    } else if (browser_content_type_is_pdf(resp->content_type)) {
        html = ns_pdf_document_html(resp->body->data, resp->body->len,
                                    final_url);
    } else if (browser_content_type_is_json(resp->content_type)) {
        char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                                 resp->body->len,
                                                 resp->content_type, NULL);
        html = ns_html_json_document(final_url, decoded,
                                     decoded ? strlen(decoded) : 0);
        if (!html) html = browser_text_document(final_url, decoded);
        g_free(decoded);
    } else if (browser_content_type_is_xml(resp->content_type)) {
        char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                                 resp->body->len,
                                                 resp->content_type, NULL);
        html = ns_html_xml_document(final_url, decoded,
                                    decoded ? strlen(decoded) : 0);
        if (!html) html = browser_text_document(final_url, decoded);
        g_free(decoded);
    } else if (browser_content_type_starts(resp->content_type, "text/") &&
               !browser_content_type_is_html(resp->content_type)) {
        char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                                 resp->body->len,
                                                 resp->content_type, NULL);
        html = browser_text_document(final_url, decoded);
        g_free(decoded);
    }
    if (!html)
        return;
    g_byte_array_set_size(resp->body, 0);
    g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
    g_free(html);
    g_free(resp->content_type);
    resp->content_type = g_strdup("text/html; charset=utf-8");
}

static void
browser_apply_meta_csp(ns_js *js, const ns_node *node, int depth)
{
    if (depth > 1024) return;
    for (const ns_node *c = node ? node->first_child : NULL; c;
         c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "meta") == 0) {
            const char *he = ns_element_get_attr(c, "http-equiv");
            if (he && g_ascii_strcasecmp(he, "content-security-policy") == 0) {
                const char *content = ns_element_get_attr(c, "content");
                if (content && *content)
                    ns_js_add_csp_header(js, content);
            }
        }
        browser_apply_meta_csp(js, c, depth + 1);
    }
}

static gboolean
headers_have_no_store(const char *raw)
{
    if (!raw) return FALSE;
    char *low = g_ascii_strdown(raw, -1);
    gboolean found = strstr(low, "no-store") != NULL;
    g_free(low);
    return found;
}

static void browser_js_form_submit(const ns_node *form, const ns_node *submitter,
                                   gpointer user_data);

static ns_browser *
browser_build_from_doc(ns_node *doc, char *base, int viewport_width,
                       double viewport_height, int settle_ms,
                       gboolean bfcache_ok, char *refresh_hdr,
                       char *doc_language, char *csp_header, char *doc_charset,
                       const char *url,
                       const ns_js_navigation_timing *navigation_timing)
{
    int vw = viewport_width > 0 ? viewport_width : 1000;
    double vh = viewport_height > 0.0
        ? viewport_height
        : (double)vw * 0.75;
    ns_css_set_viewport((double)vw, vh);
    const char *frag = strchr(url, '#');
    ns_css_set_target_fragment(frag && *(frag + 1) ? frag + 1 : NULL);

    if (ns_config_get()->speculative_preload)
        ns_engine_speculative_preload(doc, base, FALSE);

    ns_browser *b = g_new0(ns_browser, 1);
    b->doc = doc;
    b->doc_charset = doc_charset;
    b->doc_language = doc_language;
    b->base_url = base;
    ns_css_set_doc_language(b->doc_language);
    b->vw = vw;
    b->vh = vh;
    b->bfcache_ok = bfcache_ok;
    b->css_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)g_bytes_unref);
    b->images = ns_image_cache_new();
    b->videos = ns_video_cache_new();
    ns_video_cache_set_base(b->videos, base);
    b->styles = ns_engine_compute_cascade(doc, base, b->css_cache);

    b->anim = ns_anim_new();
    ns_engine_load_keyframes(b->anim, doc, base, b->css_cache);
    ns_engine_anim_observe(b->anim, b->styles, g_get_monotonic_time());

    b->js = ns_js_new(browser_js_log, b,
                      browser_js_mutated, b,
                      browser_js_navigate, b,
                      navigation_timing);
    if (b->js) {
        ns_js_set_style_table(b->js, b->styles);
        ns_js_set_image_cache(b->js, b->images);
        ns_js_set_form_submit_cb(b->js, browser_js_form_submit, b);
        ns_js_set_layout_flush_cb(b->js, browser_flush, b);
        ns_js_set_download_cb(b->js, browser_js_download, b);
        ns_js_set_audio_cb(b->js, browser_js_audio, b);
        ns_js_set_media_seek_cb(b->js, browser_media_seek, b);
        ns_js_set_media_play_cb(b->js, browser_media_play, b);
        ns_js_set_media_muted_cb(b->js, browser_media_muted, b);
        ns_js_set_mse_cb(b->js, browser_mse_data, b);
        ns_js_set_mse_buffered_cb(b->js, browser_mse_buffered, b);
        ns_js_set_media_volume_cb(b->js, browser_media_volume, b);
        ns_js_add_csp_header(b->js, csp_header);
        browser_apply_meta_csp(b->js, doc, 0);
        ns_js_run_scripts_in_doc(b->js, doc, base);
    }
    g_free(csp_header);
    if (b->videos) {
        ns_video_cache_set_js_cb(b->videos, browser_js_video, b);
        ns_video_cache_set_audio_cb(b->videos, browser_js_audio, b);
    }

    browser_arm_declarative_refresh(b, refresh_hdr);
    g_free(refresh_hdr);

    if (!b->layout || b->dirty)
        browser_relayout(b);
    browser_settle(b, settle_ms);
    if (!b->layout || b->dirty)
        browser_relayout(b);
    return b;
}

static char *g_pending_referrer;

void
ns_browser_set_next_referrer(const char *url)
{
    g_free(g_pending_referrer);
    g_pending_referrer = (url && *url) ? g_strdup(url) : NULL;
}

static ns_browser *
browser_open_common(const char *url, int viewport_width, double viewport_height,
                    int settle_ms,
                    const void *body, size_t body_len, const char *content_type)
{
    if (!url || !*url) return NULL;

    g_autofree char *referrer = g_pending_referrer;
    g_pending_referrer = NULL;

    if (g_str_has_prefix(url, NS_UNSAFE_CONTINUE_SCHEME)) {
        char *real = g_strdup(url + strlen(NS_UNSAFE_CONTINUE_SCHEME));
        char *host = ns_url_host_from(real);
        if (host) {
            ns_safebrowsing_allow_host(host);
            g_free(host);
        }
        ns_browser *b = browser_open_common(real, viewport_width,
                                            viewport_height, settle_ms,
                                            body, body_len, content_type);
        g_free(real);
        return b;
    }

    if (!body) {
        char *host = ns_url_host_from(url);
        if (host && ns_safebrowsing_blocked(host)) {
            char *html = ns_safebrowsing_interstitial(url, host);
            g_free(host);
            ns_node *doc = ns_html_parse(html, html ? (gssize)strlen(html) : 0);
            g_free(html);
            return browser_build_from_doc(doc, g_strdup(url), viewport_width,
                                          viewport_height, settle_ms, FALSE,
                                          NULL, NULL, NULL, g_strdup("UTF-8"),
                                          url, NULL);
        }
        g_free(host);
    }

    char *file_url = resolve_local_path(url);
    const char *fetch_url = file_url ? file_url : url;

    char *stripped_url = body ? NULL : ns_url_strip_tracking_params(fetch_url);
    if (stripped_url)
        fetch_url = stripped_url;

    char *https_url = body ? NULL : ns_net_https_first_upgrade(fetch_url);

    GError *err = NULL;
    ns_response *resp = NULL;
    ns_net_set_navigation_fetch(TRUE);
    if (https_url) {
        resp = ns_engine_fetch_blocking(https_url, referrer, &err);
        if (resp && !resp->error && resp->body) {
            fetch_url = https_url;
        } else {
            if (resp) ns_response_free(resp);
            resp = NULL;
            g_clear_error(&err);
        }
    }
    if (!resp)
        resp = body
            ? ns_engine_post_blocking(fetch_url, referrer, body, body_len,
                                      content_type, &err)
            : ns_engine_fetch_blocking(fetch_url, referrer, &err);
    ns_net_set_navigation_fetch(FALSE);
    if (resp && resp->error && !body &&
        g_str_has_prefix(fetch_url, "https://") &&
        (!resp->body || resp->body->len == 0)) {
        char *html = ns_build_error_page(fetch_url, 0, resp->error);
        if (html) {
            if (!resp->body)
                resp->body = g_byte_array_new();
            g_byte_array_set_size(resp->body, 0);
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
            g_free(resp->error);
            resp->error = NULL;
            g_free(resp->content_type);
            resp->content_type = g_strdup("text/html; charset=utf-8");
            g_free(resp->final_url);
            resp->final_url = g_strdup(fetch_url);
            resp->security = NS_SEC_INVALID;
        }
    }
    if (!resp || resp->error || !resp->body) {
        if (resp) ns_response_free(resp);
        g_clear_error(&err);
        g_free(file_url);
        g_free(stripped_url);
        g_free(https_url);
        return NULL;
    }
    g_clear_error(&err);

    char *base = g_strdup(resp->final_url ? resp->final_url : fetch_url);
    if (!body) {
        char *base_stripped = ns_url_strip_tracking_params(base);
        if (base_stripped) {
            g_free(base);
            base = base_stripped;
        }
    }
    gboolean bfcache_ok = !body &&
        (g_str_has_prefix(base, "http://") ||
         g_str_has_prefix(base, "https://")) &&
        resp->status >= 200 && resp->status < 400 &&
        !headers_have_no_store(resp->raw_headers);
    char *refresh_hdr = g_strdup(resp->refresh);
    char *doc_language = g_strdup(resp->content_language);
    char *csp_header = g_strdup(resp->csp_header);
    g_free(file_url);
    g_free(stripped_url);
    g_free(https_url);
    browser_prepare_document_response(resp);

    char *doc_charset = NULL;
    char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                             resp->body->len,
                                             resp->content_type,
                                             &doc_charset);
    ns_node *doc = ns_html_parse(decoded ? decoded : "",
                                 decoded ? (gssize)strlen(decoded) : 0);
    g_free(decoded);
    int sec = resp->security;
    if (sec == NS_SEC_NONE) {
        const char *u = resp->final_url ? resp->final_url : fetch_url;
        if (g_str_has_prefix(u, "https://"))
            sec = NS_SEC_SECURE;
        else if (g_str_has_prefix(u, "http://"))
            sec = NS_SEC_PLAIN;
    }
    char *ip = g_strdup(resp->remote_ip);
    ns_js_navigation_timing navigation_timing = {
        .origin_us = resp->request_start_us,
        .origin_real_ms = resp->request_start_real_ms,
        .domain_lookup_start_ms = 0,
        .domain_lookup_end_ms = resp->domain_lookup_ms,
        .connect_start_ms = resp->domain_lookup_ms,
        .connect_end_ms = resp->connect_ms,
        .secure_connection_start_ms = resp->connect_ms < resp->tls_ms
            ? resp->connect_ms : 0,
        .request_start_ms = resp->pretransfer_ms,
        .response_start_ms = resp->response_start_ms,
        .response_end_ms = resp->response_end_ms,
    };
    ns_response_free(resp);

    ns_browser *b = browser_build_from_doc(doc, base, viewport_width,
                                           viewport_height, settle_ms,
                                           bfcache_ok, refresh_hdr, doc_language,
                                           csp_header, doc_charset, url,
                                           &navigation_timing);
    if (b) {
        b->security = sec;
        b->remote_ip = ip;
    } else {
        g_free(ip);
    }
    return b;
}

ns_browser *
ns_browser_open(const char *url, int viewport_width, int settle_ms)
{
    return browser_open_common(url, viewport_width, 0.0, settle_ms,
                               NULL, 0, NULL);
}

ns_browser *
ns_browser_open_viewport(const char *url, int viewport_width,
                         double viewport_height, int settle_ms)
{
    return browser_open_common(url, viewport_width, viewport_height, settle_ms,
                               NULL, 0, NULL);
}

ns_browser *
ns_browser_open_post(const char *url, int viewport_width, int settle_ms,
                     const void *body, size_t body_len,
                     const char *content_type)
{
    return browser_open_common(url, viewport_width, 0.0, settle_ms,
                               body, body_len, content_type);
}

ns_browser *
ns_browser_open_post_viewport(const char *url, int viewport_width,
                              double viewport_height, int settle_ms,
                              const void *body, size_t body_len,
                              const char *content_type)
{
    return browser_open_common(url, viewport_width, viewport_height, settle_ms,
                               body, body_len, content_type);
}

char *
ns_browser_render_text(ns_browser *browser)
{
    if (!browser || !browser->layout) return NULL;
    GString *out = g_string_new(NULL);
    ns_engine_dump_text(browser->layout, out);
    char *text = malloc(out->len + 1);
    if (text) {
        memcpy(text, out->str, out->len);
        text[out->len] = '\0';
    }
    g_string_free(out, TRUE);
    return text;
}

char *
ns_browser_dump_dom(ns_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    GString *out = ns_node_dump(browser->doc);
    return out ? g_string_free(out, FALSE) : NULL;
}

char *
ns_browser_dump_layout(ns_browser *browser)
{
    if (!browser || !browser->layout) return NULL;
    GString *out = g_string_new(NULL);
    ns_engine_dump_layout(browser->layout, 0, out);
    return g_string_free(out, FALSE);
}

static int
node_count_walk(const ns_node *n, int depth)
{
    if (depth > 1024) return 0;
    int c = 0;
    for (; n; n = n->next_sibling)
        c += 1 + node_count_walk(n->first_child, depth + 1);
    return c;
}

static int
box_count_walk(const ns_box *b)
{
    int c = 0;
    for (; b; b = b->next_sibling)
        c += 1 + box_count_walk(b->first_child);
    return c;
}

static void
perf_dump_threads(GString *out)
{
    g_string_append(out, "Threads\n");
#if defined(__linux__)
    GError *err = NULL;
    GDir *dir = g_dir_open("/proc/self/task", 0, &err);
    if (!dir) {
        g_string_append_printf(out, "  (unavailable: %s)\n",
                               err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    const char *tid;
    int n = 0;
    while ((tid = g_dir_read_name(dir))) {
        char *path = g_strdup_printf("/proc/self/task/%s/comm", tid);
        char *comm = NULL;
        if (g_file_get_contents(path, &comm, NULL, NULL) && comm) {
            g_string_append_printf(out, "  [%s] %s", tid, g_strchomp(comm));
            g_string_append_c(out, '\n');
            n++;
        }
        g_free(comm);
        g_free(path);
    }
    g_dir_close(dir);
    g_string_append_printf(out, "  %d thread%s total\n", n, n == 1 ? "" : "s");
#else
    g_string_append(out, "  (thread enumeration is Linux-only)\n");
#endif
}

char *
ns_browser_dump_performance(ns_browser *browser)
{
    if (!browser) return NULL;
    GString *out = g_string_new(NULL);

    int pw = 0, ph = 0;
    ns_browser_page_size(browser, &pw, &ph);

    g_string_append(out, "Document\n");
    g_string_append_printf(out, "  url         %s\n",
                           browser->base_url ? browser->base_url : "");
    g_string_append_printf(out, "  charset     %s\n",
                           browser->doc_charset ? browser->doc_charset : "");
    g_string_append_printf(out, "  DOM nodes   %d\n",
                           node_count_walk(browser->doc, 0));
    g_string_append_printf(out, "  layout boxes %d\n",
                           box_count_walk(browser->layout));
    g_string_append_printf(out, "  page size   %d x %d px\n", pw, ph);

    g_string_append(out, "\nLayout\n");
    g_string_append_printf(out, "  viewport    %d x %.0f px\n",
                           browser->vw, browser->vh);
    g_string_append_printf(out, "  last reflow %.2f ms\n",
                           browser->relayout_cost_us / 1000.0);
    g_string_append_printf(out, "  oscillation %d\n", browser->layout_osc);

    g_string_append_c(out, '\n');
    if (browser->js)
        ns_js_dump_stats(browser->js, out);

    g_string_append_c(out, '\n');
    perf_dump_threads(out);

    return g_string_free(out, FALSE);
}

int
ns_browser_render_image(ns_browser *browser, const char *path)
{
    if (!browser || !browser->layout || !path) return -1;

    browser_wait_images(browser);

    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);

    int rc;
    gsize len = strlen(path);
    if (len >= 4 && g_ascii_strcasecmp(path + len - 4, ".pdf") == 0)
        rc = ns_engine_write_pdf(browser->layout, path);
    else
        rc = ns_engine_write_png(browser->layout, path);

    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);
    return rc;
}

int
ns_browser_tick(ns_browser *browser, int budget_ms)
{
    if (!browser) return -1;
    if (budget_ms < 0) budget_ms = 0;

    if (browser->refresh_due_us && !browser->pending_nav &&
        g_get_monotonic_time() >= browser->refresh_due_us) {
        browser->refresh_due_us = 0;
        browser->pending_nav = browser->refresh_url;
        browser->refresh_url = NULL;
    }

    gint64 deadline = g_get_monotonic_time() + (gint64)budget_ms * 1000;
    gboolean changed = FALSE;
    gboolean video_changed = FALSE;
    gboolean other_changed = FALSE;
    int guard = 0;
    if (browser->js &&
        (fabs(browser->cur_scroll_x - browser->js_scroll_x) > 0.5 ||
         fabs(browser->cur_scroll_y - browser->js_scroll_y) > 0.5)) {
        browser->js_scroll_x = browser->cur_scroll_x;
        browser->js_scroll_y = browser->cur_scroll_y;
        ns_js_note_viewport_scroll(browser->js, browser->cur_scroll_x,
                                   browser->cur_scroll_y);
    }
    if (browser->hover_restyle_pending) {
        gint64 now = g_get_monotonic_time();
        gint64 min_gap = browser->relayout_cost_us * 2;
        if (min_gap < 60000) min_gap = 60000;
        if (now - browser->hover_relayout_us >= min_gap) {
            browser->hover_restyle_pending = FALSE;
            browser->hover_relayout_us = now;
            browser_relayout(browser);
            browser->dirty = FALSE;
            changed = TRUE;
        }
    }
    for (;;) {
        gint64 now = g_get_monotonic_time();
        if (browser->images && ns_image_cache_tick(browser->images, now)) {
            changed = TRUE;
            other_changed = TRUE;
        }
        if (browser->videos && browser->layout) {
            ns_video_cache_discover(browser->videos, browser->layout, browser->doc, now);
            if (ns_video_cache_tick(browser->videos, now)) {
                changed = TRUE;
                video_changed = TRUE;
            }
        }
        if (browser->anim && ns_anim_tick(browser->anim, now)) {
            changed = TRUE;
            other_changed = TRUE;
        }
        if (browser->anim && browser->js)
            ns_js_dispatch_anim_events(browser->js, browser->anim);
        if (browser->js && ns_js_run_animation_frame(browser->js)) {
            changed = TRUE;
            other_changed = TRUE;
        }

        gboolean did_iter = FALSE;
        int it = 0;
        while (g_main_context_pending(NULL) && it++ < 64) {
            g_main_context_iteration(NULL, FALSE);
            did_iter = TRUE;
            changed = TRUE;
        }

        if (!did_iter) break;
        if (++guard >= 4096) break;
        if (g_get_monotonic_time() >= deadline) break;
    }
    if (browser->dirty ||
        (browser->js && ns_js_consume_mutated(browser->js))) {
        if (browser_relayout_from_mutation(browser)) {
            changed = TRUE;
            other_changed = TRUE;
            browser->dirty = FALSE;
        }
    }
    (void)video_changed;
    (void)other_changed;
    if (!changed && browser->videos &&
        ns_video_cache_waiting_growth(browser->videos))
        changed = TRUE;
    return changed ? 1 : 0;
}

int
ns_browser_animating(ns_browser *browser)
{
    if (!browser) return 0;
    if (browser->dirty) return 1;
    if (browser->hover_restyle_pending) return 1;
    if (browser->refresh_due_us || browser->refresh_url) return 1;
    if (browser_images_outstanding(browser) > 0) return 1;
    if (browser->js && ns_js_has_pending_animation_frame(browser->js))
        return 1;
    if (browser->js && ns_js_has_pending_work(browser->js))
        return 1;
    if (browser->anim && ns_anim_has_active(browser->anim))
        return 1;
    if (browser->images && ns_image_cache_animating(browser->images))
        return 1;
    if (browser->videos && ns_video_cache_animating(browser->videos))
        return 1;
    return 0;
}

int
ns_browser_set_viewport(ns_browser *browser, int css_width, double css_height)
{
    if (!browser || !browser->doc || css_width <= 0) return -1;
    if (css_height <= 0.0) css_height = (double)css_width * 0.75;
    if (css_width == browser->vw && css_height == browser->vh) return 0;
    browser->vw = css_width;
    browser->vh = css_height;
    ns_css_set_viewport((double)browser->vw, browser->vh);
    if (browser->js) {
        ns_js_sync_window_metrics(browser->js);
        ns_js_dispatch_resize(browser->js);
    }
    browser_damp_reset(browser);
    browser_relayout(browser);
    return 0;
}

int
ns_browser_set_viewport_width(ns_browser *browser, int css_width)
{
    return ns_browser_set_viewport(browser, css_width,
                                   (double)css_width * 0.75);
}

int
ns_browser_page_size(ns_browser *browser, int *out_width, int *out_height)
{
    if (!browser || !browser->layout) return -1;
    gboolean hide_x = root_axis_overflow_hidden(browser->layout,
                                                NS_CSS_OVERFLOW_X);
    gboolean hide_y = root_axis_overflow_hidden(browser->layout,
                                                NS_CSS_OVERFLOW_Y);
    double w = hide_x ? browser->vw : browser->layout->content_width;
    if (!(w > 0)) w = browser->vw;
    double bottom = hide_y ? browser->vh : browser->layout->content_height;
    if (!hide_y)
        bottom = ns_box_max_bottom(browser->layout, bottom);
    if (!(bottom > 0)) bottom = 0;
    if (out_width)  *out_width  = (int)w;
    int ypad = (!hide_y && bottom > browser->vh + 0.5) ? 32 : 0;
    if (out_height) *out_height = (int)bottom + ypad;
    return 0;
}

int
ns_browser_render_rgba(ns_browser *browser, int scroll_x, int scroll_y,
                       int width, int height, double scale,
                       unsigned char *out, int stride)
{
    if (!browser || !browser->layout || !out) return -1;
    if (width <= 0 || height <= 0 || stride < width * 4) return -1;
    if (!(scale > 0)) scale = 1.0;

    browser->cur_scroll_x = (double)scroll_x;
    browser->cur_scroll_y = (double)scroll_y;
    browser->cur_viewport_h = (double)height / scale;
    browser_ensure_images(browser);

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return -1;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_tolerance(cr, scale > 0 ? 0.5 / scale : 0.5);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -(double)scroll_x, -(double)scroll_y);


    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);
    ns_paint_set_search(browser->search_case, browser->search_active);
    const char *highlight = browser->search_query;
    if (ns_selection_has_range(&browser->selection))
        ns_paint_with_selection(cr, browser->layout, highlight,
                                &browser->selection);
    else
        ns_paint(cr, browser->layout, highlight);
    ns_paint_set_search(FALSE, NULL);
    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);

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

int
ns_browser_render_argb32(ns_browser *browser, int scroll_x, int scroll_y,
                         int width, int height, double scale,
                         unsigned char *out, int stride)
{
    if (!browser || !browser->layout || !out) return -1;
    if (width <= 0 || height <= 0 || stride < width * 4) return -1;
    if (!(scale > 0)) scale = 1.0;

    browser->cur_scroll_x = (double)scroll_x;
    browser->cur_scroll_y = (double)scroll_y;
    browser->cur_viewport_h = (double)height / scale;
    browser_ensure_images(browser);

    cairo_surface_t *surf =
        cairo_image_surface_create_for_data(out, CAIRO_FORMAT_ARGB32,
                                            width, height, stride);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return -1;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_tolerance(cr, scale > 0 ? 0.5 / scale : 0.5);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -(double)scroll_x, -(double)scroll_y);



    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);
    ns_paint_set_search(browser->search_case, browser->search_active);
    const char *highlight = browser->search_query;
    gint64 paint_t0 = g_get_monotonic_time();
    if (ns_selection_has_range(&browser->selection))
        ns_paint_with_selection(cr, browser->layout, highlight,
                                &browser->selection);
    else
        ns_paint(cr, browser->layout, highlight);
    if (g_getenv("NS_PROFILE"))
        g_printerr("[profile] paint %6.1fms %dx%d\n",
                   (double)(g_get_monotonic_time() - paint_t0) / 1000.0,
                   width, height);
    ns_paint_set_search(FALSE, NULL);
    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    const char *dump_dir = g_getenv("NS_FRAME_DUMP");
    if (dump_dir) {
        static int frame_no;
        if (frame_no % 30 == 0) {
            char *path = g_strdup_printf("%s/frame-%05d.png",
                                         dump_dir, frame_no);
            cairo_surface_write_to_png(surf, path);
            g_free(path);
        }
        frame_no++;
    }
    cairo_surface_destroy(surf);
    return 0;
}

static const ns_node *browser_hit_node(ns_browser *browser, int x, int y);

char *
ns_browser_link_at(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout) return NULL;

    static const int kR = 6;
    static const int probe[][2] = {
        { 0, 0 },
        { 0, -kR }, { 0, kR }, { -kR, 0 }, { kR, 0 },
        { -kR, -kR }, { kR, -kR }, { -kR, kR }, { kR, kR },
    };
    for (int i = 0; i < (int)(sizeof probe / sizeof probe[0]); i++) {
        int px = x + probe[i][0], py = y + probe[i][1];
        const char *href = ns_box_hit_link(browser->layout,
                                           (double)px, (double)py);
        if (!href || !*href) {
            const ns_node *node = browser_hit_node(browser, px, py);
            for (const ns_node *a = node; a && (!href || !*href); a = a->parent)
                if (ns_node_is_element_named(a, "a"))
                    href = ns_element_get_attr(a, "href");
        }
        if (href && *href) return browser_resolve_navigation(browser, href);
    }
    return NULL;
}

char *
ns_browser_cursor_at(ns_browser *browser, int x, int y)
{
    static const char *const known[] = {
        "default", "none", "context-menu", "help", "pointer", "progress",
        "wait", "cell", "crosshair", "text", "vertical-text", "alias",
        "copy", "move", "no-drop", "not-allowed", "grab", "grabbing",
        "all-scroll", "col-resize", "row-resize", "n-resize", "e-resize",
        "s-resize", "w-resize", "ne-resize", "nw-resize", "se-resize",
        "sw-resize", "ew-resize", "ns-resize", "nesw-resize", "nwse-resize",
        "zoom-in", "zoom-out",
    };
    if (!browser || !browser->layout || !browser->styles) return NULL;

    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    const ns_node *node = hit ? hit->dom : NULL;
    const ns_node *inline_node =
        ns_box_hit_inline_dom(browser->layout, (double)x, (double)y);
    if (inline_node) node = inline_node;
    const ns_node *form_node =
        ns_box_hit_form_dom(browser->layout, (double)x, (double)y);
    if (form_node) node = form_node;

    const ns_style *style = NULL;
    for (const ns_node *n = node; n && !style; n = n->parent)
        style = g_hash_table_lookup(browser->styles, n);
    if (!style) return NULL;

    const ns_css_value *v = style->values[NS_CSS_CURSOR];
    char *match = NULL;
    if (v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword) {
        char **tokens = g_strsplit_set(v->u.keyword, ", \t", -1);
        for (int i = 0; tokens && tokens[i]; i++) {
            if (!tokens[i][0]) continue;
            for (gsize k = 0; k < G_N_ELEMENTS(known); k++)
                if (g_ascii_strcasecmp(tokens[i], known[k]) == 0) {
                    g_free(match);
                    match = g_strdup(known[k]);
                }
        }
        g_strfreev(tokens);
    }
    if (match) return match;

    if (ns_box_hit_link(browser->layout, (double)x, (double)y)) return NULL;
    if (form_node)
        return ns_node_is_text_input(form_node) ? g_strdup("text") : NULL;
    for (const ns_node *n = node; n; n = n->parent)
        if (ns_node_is_contenteditable_host(n)) return g_strdup("text");
    if (ns_selection_text_at(browser->layout, (double)x, (double)y))
        return g_strdup("text");
    return NULL;
}

char *
ns_browser_select(ns_browser *browser, int kind, int x, int y)
{
    if (!browser || !browser->layout) return NULL;
    switch (kind) {
    case 0: ns_selection_anchor_at(&browser->selection, browser->layout,
                                   (double)x, (double)y); break;
    case 1: ns_selection_extend_to(&browser->selection, browser->layout,
                                   (double)x, (double)y); break;
    case 2: ns_selection_clear(&browser->selection); break;
    case 3: ns_selection_select_all(&browser->selection, browser->layout);
            break;
    case 4: return ns_selection_collect_text(browser->layout,
                                             &browser->selection);
    default: break;
    }
    return NULL;
}

static void
browser_hover_dispatch(ns_browser *b, const ns_node *target, int x, int y,
                       const char *ptr_type, const char *mouse_type,
                       const ns_node *related)
{
    if (!b->js || !target) return;
    ns_js_dispatch_mouse_event(b->js, target, ptr_type, (double)x, (double)y,
                               (double)x, (double)y, 0, 0,
                               FALSE, FALSE, FALSE, FALSE, related, NULL);
    ns_js_dispatch_mouse_event(b->js, target, mouse_type, (double)x, (double)y,
                               (double)x, (double)y, 0, 0,
                               FALSE, FALSE, FALSE, FALSE, related, NULL);
}

int
ns_browser_hover(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout) return -1;

    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    const ns_node *node = hit ? hit->dom : NULL;
    const ns_node *inline_node =
        ns_box_hit_inline_dom(browser->layout, (double)x, (double)y);
    if (inline_node) node = inline_node;
    const ns_node *form_node =
        ns_box_hit_form_dom(browser->layout, (double)x, (double)y);
    if (form_node) node = form_node;

    const ns_node *prev = browser->hover_node;
    gboolean changed = node != prev;
    browser->hover_node = node;

    gboolean dirty = FALSE;
    if (browser->js) {
        if (changed) {
            browser_hover_dispatch(browser, prev, x, y, "pointerout",
                                   "mouseout", node);
            browser_hover_dispatch(browser, prev, x, y, "pointerleave",
                                   "mouseleave", node);
            browser_hover_dispatch(browser, node, x, y, "pointerover",
                                   "mouseover", prev);
            browser_hover_dispatch(browser, node, x, y, "pointerenter",
                                   "mouseenter", prev);
        }
        browser_hover_dispatch(browser, node, x, y, "pointermove",
                               "mousemove", NULL);
        if (ns_js_consume_mutated(browser->js)) dirty = TRUE;
    }

    gboolean hover_restyle = changed && ns_render_page_uses_hover() &&
                             !ns_selection_has_range(&browser->selection);
    if (dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        return 1;
    }
    if (hover_restyle) {
        gint64 now = g_get_monotonic_time();
        gint64 min_gap = browser->relayout_cost_us * 2;
        if (min_gap < 60000) min_gap = 60000;
        if (now - browser->hover_relayout_us >= min_gap) {
            browser->hover_relayout_us = now;
            browser->hover_restyle_pending = FALSE;
            browser_relayout(browser);
            browser->dirty = FALSE;
            return 1;
        }
        browser->hover_restyle_pending = TRUE;
    }
    return 0;
}

int
ns_browser_scroll_at(ns_browser *browser, int x, int y, int dx, int dy)
{
    if (!browser || !browser->layout) return 0;

    ns_box *box = ns_box_hit_scrollable(browser->layout, (double)x, (double)y);
    if (!box) return 0;

    int consumed = 0;
    if (dy != 0 && box->scroll_max_y > 0) {
        double ny = box->scroll_y + dy;
        if (ny < 0) ny = 0;
        if (ny > box->scroll_max_y) ny = box->scroll_max_y;
        if (ny != box->scroll_y) { box->scroll_y = ny; consumed = 1; }
    }
    if (dx != 0 && box->scroll_max_x > 0) {
        double nx = box->scroll_x + dx;
        if (nx < 0) nx = 0;
        if (nx > box->scroll_max_x) nx = box->scroll_max_x;
        if (nx != box->scroll_x) { box->scroll_x = nx; consumed = 1; }
    }

    if (consumed && browser->js && box->dom)
        ns_js_dispatch_event(browser->js, box->dom, "scroll", NULL);
    return consumed;
}

static gboolean
sb_vgeom(const ns_box *b, double *track_x, double *track_w,
         double *track_y, double *track_h, double *thumb_y, double *thumb_h)
{
    if (!b || b->scroll_max_y <= 0) return FALSE;
    double py = b->y + b->margin.top + b->border.top;
    double ph = b->content_height + b->padding.top + b->padding.bottom;
    if (ph <= 16.0) return FALSE;
    double px = b->x + b->margin.left + b->border.left;
    double pw = b->content_width + b->padding.left + b->padding.right;
    double tw = 8.0;
    double th = ph - 2.0;
    double total = ph + b->scroll_max_y;
    double thh = th * (ph / total);
    if (thh < 16.0) thh = 16.0;
    if (thh > th) thh = th;
    *track_x = px + pw - tw - 1.0;
    *track_w = tw;
    *track_y = py + 1.0;
    *track_h = th;
    *thumb_h = thh;
    *thumb_y = *track_y + (th - thh) * (b->scroll_y / b->scroll_max_y);
    return TRUE;
}

static ns_box *
box_find_scrollable_by_dom(ns_box *root, const ns_node *node)
{
    if (!root || !node) return NULL;
    if (root->dom == node && root->scrolls) return root;
    for (ns_box *c = root->first_child; c; c = c->next_sibling) {
        ns_box *m = box_find_scrollable_by_dom(c, node);
        if (m) return m;
    }
    return NULL;
}

int
ns_browser_scrollbar_press(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout) return 0;
    double lx = 0, ly = 0;
    ns_box *box = ns_box_hit_scrollbar(browser->layout, (double)x, (double)y,
                                       &lx, &ly);
    if (!box) return 0;

    double tx, tw, ty, th, thy, thh;
    if (!sb_vgeom(box, &tx, &tw, &ty, &th, &thy, &thh)) return 0;
    if (lx < tx - 3.0 || lx > tx + tw + 3.0 || ly < ty || ly > ty + th)
        return 0;

    double grab;
    if (ly >= thy && ly <= thy + thh) {
        grab = ly - thy;
    } else {
        grab = thh / 2.0;
        double ns = (th > thh)
            ? (ly - ty - grab) / (th - thh) * box->scroll_max_y : 0.0;
        if (ns < 0) ns = 0;
        if (ns > box->scroll_max_y) ns = box->scroll_max_y;
        box->scroll_y = ns;
        if (box->dom && browser->js)
            ns_js_dispatch_event(browser->js, box->dom, "scroll", NULL);
    }

    browser->sb_dragging = TRUE;
    browser->sb_box = box;
    browser->sb_node = box->dom;
    browser->sb_grab = grab;
    return 1;
}

int
ns_browser_scrollbar_drag(ns_browser *browser, int x, int y)
{
    (void)x;
    if (!browser || !browser->sb_dragging) return 0;
    ns_box *box = browser->sb_box;
    if (browser->sb_node)
        box = box_find_scrollable_by_dom(browser->layout, browser->sb_node);
    if (!box) {
        browser->sb_dragging = FALSE;
        browser->sb_box = NULL;
        return 0;
    }
    browser->sb_box = box;

    double tx, tw, ty, th, thy, thh;
    if (!sb_vgeom(box, &tx, &tw, &ty, &th, &thy, &thh) || th <= thh)
        return 0;
    double ns = ((double)y - ty - browser->sb_grab) / (th - thh)
              * box->scroll_max_y;
    if (ns < 0) ns = 0;
    if (ns > box->scroll_max_y) ns = box->scroll_max_y;
    if (ns == box->scroll_y) return 0;
    box->scroll_y = ns;
    if (box->dom && browser->js)
        ns_js_dispatch_event(browser->js, box->dom, "scroll", NULL);
    return 1;
}

void
ns_browser_scrollbar_release(ns_browser *browser)
{
    if (!browser) return;
    browser->sb_dragging = FALSE;
    browser->sb_box = NULL;
    browser->sb_node = NULL;
}

int
ns_browser_drop_files(ns_browser *browser, int x, int y,
                      const char *const *paths, int n_paths)
{
    if (!browser || !browser->js || !browser->layout || !paths || n_paths <= 0)
        return 0;

    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    const ns_node *target = hit ? hit->dom : NULL;
    if (!target && browser->doc)
        target = ns_node_find_first_element(browser->doc, "body");
    if (!target) target = browser->doc;
    if (!target) return 0;

    ns_js_drag_session *session = ns_js_drag_session_new(browser->js);
    if (!session) return 0;
    for (int i = 0; i < n_paths; i++)
        if (paths[i]) ns_js_drag_session_add_file(session, paths[i]);

    gboolean accept = FALSE;
    gboolean prevented = FALSE;
    ns_js_dispatch_drag_event(browser->js, session, target, "dragenter",
                              x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                              NULL, &prevented);
    if (prevented) accept = TRUE;
    prevented = FALSE;
    ns_js_dispatch_drag_event(browser->js, session, target, "dragover",
                              x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                              NULL, &prevented);
    if (prevented) accept = TRUE;
    if (accept)
        ns_js_dispatch_drag_event(browser->js, session, target, "drop",
                                  x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                                  NULL, &prevented);
    else
        ns_js_dispatch_drag_event(browser->js, session, target, "dragleave",
                                  x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                                  NULL, &prevented);
    ns_js_drag_session_free(session);

    if (ns_js_consume_mutated(browser->js)) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        return 1;
    }
    return 0;
}

char *
ns_browser_console_drain(ns_browser *browser)
{
    if (!browser || !browser->console_buf || browser->console_buf->len == 0)
        return NULL;
    char *out = g_strdup(browser->console_buf->str);
    g_string_truncate(browser->console_buf, 0);
    return out;
}

char *
ns_browser_eval(ns_browser *browser, const char *src)
{
    if (!browser || !browser->js || !src) return NULL;
    browser_damp_reset(browser);
    char *res = ns_js_eval_source(browser->js, src, "devtools-console");
    if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
    return res;
}

int
ns_browser_contextmenu(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout || !browser->js) return 0;
    const ns_node *node = browser_hit_node(browser, x, y);
    if (!node) return 0;
    gboolean prevented = FALSE;
    ns_js_dispatch_mouse_event(browser->js, node, "contextmenu",
                               (double)x, (double)y, (double)x, (double)y,
                               2, 0, FALSE, FALSE, FALSE, FALSE, NULL,
                               &prevented);
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
    return prevented ? 1 : 0;
}

char *
ns_browser_media_at(ns_browser *browser, int x, int y, int *out_is_video,
                    int *out_stream)
{
    if (out_is_video) *out_is_video = 0;
    if (out_stream) *out_stream = 0;
    if (!browser || !browser->layout) return NULL;

    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    const ns_box *media = NULL;
    for (const ns_box *b = hit; b; b = b->parent) {
        gboolean has_media_url =
            b->media && (b->media->video_src || b->media->video_audio_src);
        gboolean has_internal_video =
            b->dom && b->dom->kind == NS_NODE_ELEMENT &&
            (ns_element_get_attr(b->dom, NS_MEDIA_SRC_ATTR) != NULL ||
             ns_element_get_attr(b->dom, NS_MEDIA_STREAM_ATTR) != NULL);
        if (b->dom && (ns_node_is_element_named(b->dom, "video") ||
                       ns_node_is_element_named(b->dom, "audio") ||
                       has_media_url || has_internal_video)) {
            media = b;
            break;
        }
    }
    if (!media || !media->dom) return NULL;

    if (media->media && media->media->video) {
        ns_video *iv = media->media->video;
        if (iv->is_camera)
            return NULL;
        if (iv->player) {
            ns_video_cache_toggle(browser->videos, iv, g_get_monotonic_time());
            return NULL;
        }
    }
    if (media->dom->kind == NS_NODE_ELEMENT) {
        const char *stream_kind = ns_element_get_attr(media->dom, NS_MEDIA_STREAM_ATTR);
        if (stream_kind && g_strcmp0(stream_kind, "camera") == 0)
            return NULL;
    }

    gboolean is_video =
        ns_node_is_element_named(media->dom, "video") ||
        (media->media && media->media->video_src) ||
        (media->dom->kind == NS_NODE_ELEMENT &&
         (ns_element_get_attr(media->dom, NS_MEDIA_SRC_ATTR) != NULL ||
          ns_element_get_attr(media->dom, NS_MEDIA_STREAM_ATTR) != NULL));
    gboolean force_stream =
        media->dom->kind == NS_NODE_ELEMENT &&
        ns_element_get_attr(media->dom, NS_MEDIA_STREAM_ATTR) != NULL;
    const char *msrc = NULL;
    if (media->media) {
        if (is_video) {
            msrc = media->media->video_src;
            if (!msrc) msrc = media->media->video_audio_src;
        } else {
            msrc = media->media->video_audio_src;
        }
    }
    if ((!msrc || !*msrc) && media->dom->kind == NS_NODE_ELEMENT)
        msrc = ns_element_get_attr(media->dom, NS_MEDIA_SRC_ATTR);
    char *abs = (!force_stream && msrc) ? ns_url_resolve(browser->base_url, msrc)
                                        : NULL;
    gboolean stream = force_stream || !abs || g_str_has_prefix(abs, "blob:") ||
                      g_str_has_prefix(abs, "data:");
    if (!stream && is_video && abs && ns_video_url_is_inline(abs)) {
        g_free(abs);
        return NULL;
    }
    if (stream) {
        g_free(abs);
        abs = browser->base_url ? g_strdup(browser->base_url) : NULL;
    }
    if (!abs) return NULL;
    if (g_str_has_prefix(abs, "file://") &&
        (!browser->base_url || !g_str_has_prefix(browser->base_url, "file://"))) {
        g_free(abs);
        return NULL;
    }
    if (out_is_video) *out_is_video = is_video ? 1 : 0;
    if (out_stream) *out_stream = stream ? 1 : 0;
    return abs;
}

int
ns_browser_find(ns_browser *browser, const char *query, int case_sensitive,
                int direction, int from_y, int *out_total, int *out_current,
                int *out_y)
{
    if (out_total) *out_total = 0;
    if (out_current) *out_current = 0;
    if (out_y) *out_y = 0;
    if (!browser || !browser->layout) return -1;

    gboolean cs = case_sensitive != 0;
    if (!query || !*query) {
        g_clear_pointer(&browser->search_query, g_free);
        browser->search_active = NULL;
        browser->search_case = cs;
        return 0;
    }

    if (!browser->search_query || strcmp(browser->search_query, query) != 0 ||
        browser->search_case != cs) {
        g_free(browser->search_query);
        browser->search_query = g_strdup(query);
        browser->search_active = NULL;
    }
    browser->search_case = cs;

    guint total = ns_box_count_matches(browser->layout, query, cs);
    if (out_total) *out_total = (int)total;
    if (total == 0) {
        browser->search_active = NULL;
        return 0;
    }

    double cur_y = browser->search_active ? browser->search_active->y
                                          : (double)from_y;
    const ns_box *target = NULL;
    if (direction == 2) {
        target = ns_box_first_match_above(browser->layout, query, cur_y, cs);
        if (!target)
            target = ns_box_first_match_above(browser->layout, query,
                                              G_MAXDOUBLE, cs);
    } else if (direction == 1) {
        target = ns_box_first_match_below(browser->layout, query, cur_y + 2,
                                          cs);
        if (!target)
            target = ns_box_first_match_below(browser->layout, query, -1, cs);
    } else {
        target = ns_box_first_match_below(browser->layout, query,
                                          (double)from_y - 1, cs);
        if (!target)
            target = ns_box_first_match_below(browser->layout, query, -1, cs);
    }

    browser->search_active = target;
    if (target) {
        if (out_y) *out_y = (int)target->y;
        if (out_current)
            *out_current = (int)ns_box_match_ordinal(browser->layout, query,
                                                     target, cs);
    }
    return 0;
}

char *
ns_browser_take_post(ns_browser *browser, size_t *out_len, char **out_ct)
{
    if (out_len) *out_len = 0;
    if (out_ct) *out_ct = NULL;
    if (!browser || !browser->pending_post_body) return NULL;
    char *body = browser->pending_post_body;
    browser->pending_post_body = NULL;
    if (out_len) *out_len = browser->pending_post_len;
    browser->pending_post_len = 0;
    if (out_ct) *out_ct = browser->pending_post_ct;
    else        g_free(browser->pending_post_ct);
    browser->pending_post_ct = NULL;
    return body;
}

static void
browser_perform_form_navigation(ns_browser *b, const ns_node *form,
                                const ns_node *clicked)
{
    if (!b || !form || !b->doc) return;
    gboolean from_text = clicked && ns_node_is_text_input(clicked);

    const char *method = ns_element_get_attr(form, "method");
    const char *formmethod = (clicked && !from_text)
                             ? ns_element_get_attr(clicked, "formmethod") : NULL;
    if (formmethod && *formmethod) method = formmethod;
    gboolean is_post = method && g_ascii_strcasecmp(method, "post") == 0;

    const char *action = ns_element_get_attr(form, "action");
    const char *formaction = (clicked && !from_text)
                             ? ns_element_get_attr(clicked, "formaction") : NULL;
    if (formaction && *formaction) action = formaction;
    char *abs_action = (action && *action) ? ns_url_resolve(b->base_url, action)
                                           : g_strdup(b->base_url);
    if (!abs_action) return;
    if (!browser_allows_navigation_url(b, abs_action)) {
        g_free(abs_action);
        return;
    }
    if (b->js && !ns_js_csp_form_action_allowed(b->js, abs_action)) {
        g_free(abs_action);
        return;
    }

    const char *accept_charset = ns_element_get_attr(form, "accept-charset");
    ns_form_set_submission_charset(
        (accept_charset && *accept_charset) ? accept_charset
                                            : b->doc_charset);

    if (is_post) {
        GString *body = g_string_new(NULL);
        gboolean first = TRUE;
        ns_form_collect_inputs(form, b->doc, b->doc, body, &first, clicked);
        ns_form_set_submission_charset(NULL);
        g_free(b->pending_post_body);
        g_free(b->pending_post_ct);
        b->pending_post_len = body->len;
        b->pending_post_body = g_string_free(body, FALSE);
        b->pending_post_ct = g_strdup("application/x-www-form-urlencoded");
        g_free(b->pending_nav);
        b->pending_nav = abs_action;
        return;
    }

    GString *query = g_string_new(NULL);
    gboolean first = TRUE;
    ns_form_collect_inputs(form, b->doc, b->doc, query, &first, clicked);
    ns_form_set_submission_charset(NULL);

    char *frag = strchr(abs_action, '#');
    if (frag) *frag = '\0';
    char *full;
    if (query->len == 0) {
        full = g_strdup(abs_action);
    } else {
        full = strchr(abs_action, '?')
            ? g_strdup_printf("%s&%s", abs_action, query->str)
            : g_strdup_printf("%s?%s", abs_action, query->str);
    }
    g_string_free(query, TRUE);
    g_free(abs_action);
    g_free(b->pending_nav);
    b->pending_nav = full;
}

static void
browser_js_form_submit(const ns_node *form, const ns_node *submitter,
                       gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b || !form) return;
    browser_perform_form_navigation(b, form, submitter ? submitter : form);
}

static void
browser_submit_form(ns_browser *b, const ns_node *clicked)
{
    if (!clicked || !b->doc) return;
    gboolean from_text = ns_node_is_text_input(clicked);
    gboolean from_form = ns_node_is_element_named(clicked, "form");
    if (!from_text && !from_form && !ns_form_is_submit_trigger(clicked))
        return;
    const ns_node *form = from_form ? clicked : ns_form_owner(clicked, b->doc);
    if (!form) return;

    if (!ns_element_get_attr(form, "novalidate") &&
        !ns_element_get_attr(clicked, "formnovalidate")) {
        const ns_node *bad = ns_form_first_invalid(form, b->doc, b->doc);
        if (bad) {
            if (b->js) ns_js_dispatch_event(b->js, bad, "invalid", NULL);
            return;
        }
    }

    if (b->js) {
        gboolean prevented = FALSE;
        ns_js_dispatch_submit_event(b->js, form, clicked, &prevented);
        if (ns_js_consume_mutated(b->js)) b->dirty = TRUE;
        if (prevented) return;
    }

    browser_perform_form_navigation(b, form, clicked);
}

static const ns_node *
browser_hit_node(ns_browser *browser, int x, int y)
{
    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    const ns_node *node = hit ? hit->dom : NULL;
    const ns_node *inline_node =
        ns_box_hit_inline_dom(browser->layout, (double)x, (double)y);
    if (inline_node)
        node = inline_node;
    const ns_node *form_node =
        ns_box_hit_form_dom(browser->layout, (double)x, (double)y);
    if (form_node)
        node = form_node;
    return node;
}

char *
ns_browser_press(ns_browser *browser, int x, int y, int mods)
{
    if (!browser || !browser->layout) return NULL;
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);
    ns_selection_clear(&browser->selection);

    const ns_node *node = browser_hit_node(browser, x, y);
    browser->press_node = node;
    browser->press_x = x;
    browser->press_y = y;
    browser->press_mods = mods;
    browser->press_active = node != NULL;

    ns_css_set_active_node(node);
    if (node && ns_render_page_uses_active()) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }

    if (browser->js && node) {
        gboolean sh = (mods & 1) != 0, ct = (mods & 2) != 0;
        gboolean al = (mods & 4) != 0, me = (mods & 8) != 0;
        ns_js_dispatch_mouse_event(browser->js, node, "pointerdown",
                                   (double)x, (double)y, (double)x, (double)y,
                                   0, 1, sh, ct, al, me, NULL, NULL);
        ns_js_dispatch_mouse_event(browser->js, node, "mousedown",
                                   (double)x, (double)y, (double)x, (double)y,
                                   0, 1, sh, ct, al, me, NULL, NULL);
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }

    gboolean in_datalist = FALSE;
    for (const ns_node *a = node; a; a = a->parent)
        if (ns_node_is_element_named(a, "datalist")) { in_datalist = TRUE; break; }
    if (browser->js && !in_datalist) {
        const ns_node *focus = NULL;
        for (const ns_node *a = node; a; a = a->parent)
            if (ns_node_is_focusable(a)) { focus = a; break; }
        ns_js_set_focus(browser->js, focus);
        const char *val = focus ? ns_node_editable_value(focus) : NULL;
        browser->caret_byte = val ? strlen(val) : 0;
        browser->sel_anchor_byte = browser->caret_byte;
        browser->datalist_suppressed =
            !(focus && ns_node_is_element_named(focus, "input") &&
              ns_element_get_attr(focus, "list") != NULL);
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }

    if (browser->js) {
        if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }

    char *nav = browser->pending_nav;
    browser->pending_nav = NULL;
    return nav;
}

static gboolean
ns_is_dropdown_select(const ns_node *n)
{
    if (!ns_node_is_element_named(n, "select")) return FALSE;
    if (ns_element_get_attr(n, "multiple")) return FALSE;
    const char *sz = ns_element_get_attr(n, "size");
    if (sz && atoi(sz) > 1) return FALSE;
    return TRUE;
}

static void browser_input_replace(ns_browser *b, ns_node *node, gsize del_start,
                                  gsize del_end, const char *insert);

static gboolean
browser_datalist_click(ns_browser *browser, const ns_node *node)
{
    const ns_node *option = NULL, *dl = NULL;
    for (const ns_node *a = node; a; a = a->parent) {
        if (!option && ns_node_is_element_named(a, "option")) option = a;
        if (ns_node_is_element_named(a, "datalist")) { dl = a; break; }
        if (ns_node_is_element_named(a, "select")) return FALSE;
    }
    if (!option || !dl || !browser->js) return FALSE;
    ns_node *inp = (ns_node *)ns_js_focused_node(browser->js);
    if (!inp || !ns_node_is_element_named(inp, "input")) return FALSE;
    const char *ov = ns_element_get_attr(option, "value");
    char *val = (ov && *ov) ? g_strdup(ov) : ns_option_label_dup(option);
    const char *cur = ns_node_editable_value(inp);
    browser_input_replace(browser, inp, 0, cur ? strlen(cur) : 0,
                          val ? val : "");
    gboolean p = FALSE;
    ns_js_dispatch_event(browser->js, inp, "change", &p);
    ns_js_consume_mutated(browser->js);
    g_free(val);
    browser->datalist_suppressed = TRUE;
    browser->dirty = TRUE;
    return TRUE;
}

static gboolean
browser_dropdown_click(ns_browser *browser, const ns_node *node)
{
    if (browser_datalist_click(browser, node)) return TRUE;
    const ns_node *option = NULL, *select = NULL;
    for (const ns_node *a = node; a; a = a->parent) {
        if (!option && ns_node_is_element_named(a, "option")) option = a;
        if (ns_node_is_element_named(a, "select")) { select = a; break; }
    }
    if (select && option && !ns_is_dropdown_select(select) &&
        !ns_element_get_attr(select, "disabled")) {
        gboolean multiple = ns_element_get_attr(select, "multiple") != NULL;
        gboolean toggle = multiple && (browser->press_mods & 2) != 0;
        if (browser->js) {
            if (toggle)
                ns_js_select_toggle_option(browser->js, (ns_node *)option);
            else
                ns_js_select_choose_option(browser->js, (ns_node *)option);
            ns_js_consume_mutated(browser->js);
        }
        browser->dirty = TRUE;
        return TRUE;
    }
    if (browser->open_select && option && select == browser->open_select) {
        if (browser->js &&
            ns_js_select_choose_option(browser->js, (ns_node *)option)) {
            ns_js_consume_mutated(browser->js);
            browser->open_select = NULL;
        }
        browser->dirty = TRUE;
        return TRUE;
    }
    if (select && ns_is_dropdown_select(select) &&
        !ns_element_get_attr(select, "disabled")) {
        browser->open_select =
            (browser->open_select == select) ? NULL : select;
        browser->dirty = TRUE;
        return TRUE;
    }
    if (browser->open_select) {
        browser->open_select = NULL;
        browser->dirty = TRUE;
    }
    return FALSE;
}

char *
ns_browser_release_click(ns_browser *browser, int *out_changed)
{
    if (out_changed) *out_changed = 0;
    if (!browser) {
        if (out_changed) *out_changed = -1;
        return NULL;
    }
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);

    const ns_node *node = browser->press_active ? browser->press_node : NULL;
    int x = browser->press_x;
    int y = browser->press_y;
    int mods = browser->press_mods;
    browser->press_node = NULL;
    browser->press_active = FALSE;

    gboolean prevented = FALSE;
    if (browser->js && node) {
        gboolean sh = (mods & 1) != 0, ct = (mods & 2) != 0;
        gboolean al = (mods & 4) != 0, me = (mods & 8) != 0;
        ns_js_dispatch_mouse_event(browser->js, node, "pointerup",
                                   (double)x, (double)y, (double)x, (double)y,
                                   0, 0, sh, ct, al, me, NULL, NULL);
        ns_js_dispatch_mouse_event(browser->js, node, "mouseup",
                                   (double)x, (double)y, (double)x, (double)y,
                                   0, 0, sh, ct, al, me, NULL, NULL);
        ns_js_dispatch_mouse_event(browser->js, node, "click",
                                   (double)x, (double)y, (double)x, (double)y,
                                   0, 0, sh, ct, al, me, NULL, &prevented);
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }

    gboolean select_consumed =
        !prevented && node && browser_dropdown_click(browser, node);

    if (!select_consumed) {
    if (!prevented && browser->js && node &&
        ns_js_click_activate(browser->js, node))
        browser->dirty = TRUE;

    if (!prevented && node && browser->js &&
        ns_js_activate_summary(browser->js, node)) {
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    } else if (!prevented && !browser->pending_nav && node &&
        ns_form_is_submit_trigger(node)) {
        browser_submit_form(browser, node);
    } else if (!prevented && node && browser->js && browser->doc &&
               ns_form_is_reset_trigger(node)) {
        ns_node *form = (ns_node *)ns_form_owner(node, browser->doc);
        if (form) {
            ns_js_form_reset(browser->js, form);
            if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
        }
    } else if (!prevented && !browser->pending_nav) {
        const char *href = NULL;
        for (const ns_node *a = node; a && !href; a = a->parent) {
            if (ns_node_is_element_named(a, "a")) {
                const char *h = ns_element_get_attr(a, "href");
                if (h && *h) href = h;
            }
        }
        if (!href)
            href = ns_box_hit_link(browser->layout, (double)x, (double)y);
        if (href && *href)
            browser->pending_nav = browser_resolve_navigation(browser, href);
    }
    }

    const ns_node *prev = ns_css_set_active_node(NULL);
    if (prev && ns_render_page_uses_active())
        browser->dirty = TRUE;

    if (browser->js) {
        if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        if (out_changed) *out_changed = 1;
    }

    char *nav = browser->pending_nav;
    browser->pending_nav = NULL;
    return nav;
}

int
ns_browser_release(ns_browser *browser)
{
    int changed = 0;
    char *nav = ns_browser_release_click(browser, &changed);
    free(nav);
    return changed;
}

static void
browser_video_click_toggle(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout || !browser->videos) return;
    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    for (const ns_box *b = hit; b; b = b->parent) {
        if (!b->media || !b->media->video) continue;
        ns_video *iv = b->media->video;
        if (iv->is_camera || !iv->player) return;
        if (browser->js && b->dom &&
            ns_js_node_has_click_handler(browser->js, b->dom))
            return;
        ns_video_cache_toggle(browser->videos, iv, g_get_monotonic_time());
        browser->dirty = TRUE;
        return;
    }
}

char *
ns_browser_click(ns_browser *browser, int x, int y, int mods)
{
    char *nav = ns_browser_press(browser, x, y, mods);
    if (nav && *nav) {
        if (browser) {
            browser->press_node = NULL;
            browser->press_active = FALSE;
            ns_css_set_active_node(NULL);
        }
        return nav;
    }
    free(nav);
    char *out = ns_browser_release_click(browser, NULL);
    if (!out || !*out)
        browser_video_click_toggle(browser, x, y);
    return out;
}

static void
browser_input_replace(ns_browser *b, ns_node *node, gsize del_start,
                      gsize del_end, const char *insert)
{
    const char *cur = ns_node_editable_value(node);
    if (!cur) return;
    gsize cur_len = strlen(cur);
    if (del_start > cur_len) del_start = cur_len;
    if (del_end > cur_len) del_end = cur_len;
    if (del_end < del_start) del_end = del_start;
    gsize ins_len = insert ? strlen(insert) : 0;

    GString *s = g_string_sized_new(cur_len - (del_end - del_start) + ins_len);
    g_string_append_len(s, cur, (gssize)del_start);
    if (ins_len) g_string_append_len(s, insert, (gssize)ins_len);
    g_string_append_len(s, cur + del_end, (gssize)(cur_len - del_end));

    if (b->js) {
        gboolean prevented = FALSE;
        ns_js_dispatch_event(b->js, node, "beforeinput", &prevented);
        if (prevented || ns_js_focused_node(b->js) != node) {
            g_string_free(s, TRUE);
            return;
        }
    }
    ns_node_set_editable_value(node, s->str);
    b->caret_byte = del_start + ins_len;
    b->sel_anchor_byte = b->caret_byte;
    g_string_free(s, TRUE);
    if (b->js) {
        ns_js_dispatch_event(b->js, node, "input", NULL);
        (void)ns_js_consume_mutated(b->js);
    }
}

static gboolean
browser_edit_key(ns_browser *b, ns_node *node, const char *key, int mods)
{
    gboolean shift = (mods & 1) != 0, ctrl = (mods & 2) != 0;
    if (mods & (4 | 8)) return FALSE;
    const char *cur = ns_node_editable_value(node);
    if (!cur || !key || !*key) return FALSE;
    gsize cur_len = strlen(cur);
    if (b->caret_byte > cur_len) b->caret_byte = cur_len;
    if (b->sel_anchor_byte > cur_len) b->sel_anchor_byte = cur_len;
    gsize sel_lo = b->sel_anchor_byte < b->caret_byte ? b->sel_anchor_byte
                                                      : b->caret_byte;
    gsize sel_hi = b->sel_anchor_byte < b->caret_byte ? b->caret_byte
                                                      : b->sel_anchor_byte;
    gboolean has_sel = sel_lo != sel_hi;
    gboolean multiline = (node->name && strcmp(node->name, "textarea") == 0) ||
                         ns_node_is_contenteditable_host(node);

    if (ctrl) {
        if ((key[0] == 'a' || key[0] == 'A') && !key[1]) {
            b->sel_anchor_byte = 0;
            b->caret_byte = cur_len;
            return TRUE;
        }
        return FALSE;
    }
    if (strcmp(key, "Backspace") == 0) {
        if (has_sel) browser_input_replace(b, node, sel_lo, sel_hi, NULL);
        else if (b->caret_byte > 0) {
            const char *prev = g_utf8_prev_char(cur + b->caret_byte);
            browser_input_replace(b, node, (gsize)(prev - cur), b->caret_byte,
                                  NULL);
        }
        return TRUE;
    }
    if (strcmp(key, "Delete") == 0) {
        if (has_sel) browser_input_replace(b, node, sel_lo, sel_hi, NULL);
        else if (b->caret_byte < cur_len) {
            const char *nxt = g_utf8_next_char(cur + b->caret_byte);
            browser_input_replace(b, node, b->caret_byte, (gsize)(nxt - cur),
                                  NULL);
        }
        return TRUE;
    }
    if (strcmp(key, "ArrowLeft") == 0) {
        if (has_sel && !shift) b->caret_byte = sel_lo;
        else if (b->caret_byte > 0)
            b->caret_byte = (gsize)(g_utf8_prev_char(cur + b->caret_byte) - cur);
        if (!shift) b->sel_anchor_byte = b->caret_byte;
        return TRUE;
    }
    if (strcmp(key, "ArrowRight") == 0) {
        if (has_sel && !shift) b->caret_byte = sel_hi;
        else if (b->caret_byte < cur_len)
            b->caret_byte = (gsize)(g_utf8_next_char(cur + b->caret_byte) - cur);
        if (!shift) b->sel_anchor_byte = b->caret_byte;
        return TRUE;
    }
    if (strcmp(key, "Home") == 0) {
        b->caret_byte = 0;
        if (!shift) b->sel_anchor_byte = 0;
        return TRUE;
    }
    if (strcmp(key, "End") == 0) {
        b->caret_byte = cur_len;
        if (!shift) b->sel_anchor_byte = cur_len;
        return TRUE;
    }
    if (strcmp(key, "Enter") == 0) {
        if (multiline) {
            browser_input_replace(b, node, sel_lo, sel_hi, "\n");
            return TRUE;
        }
        browser_submit_form(b, node);
        return TRUE;
    }
    if (g_utf8_strlen(key, -1) == 1 &&
        !g_unichar_iscntrl(g_utf8_get_char(key))) {
        browser_input_replace(b, node, sel_lo, sel_hi, key);
        return TRUE;
    }
    return FALSE;
}

static int
browser_key_char_code(const char *key)
{
    if (!key || !*key || !g_utf8_validate(key, -1, NULL))
        return 0;
    const char *next = g_utf8_next_char(key);
    if (!next || *next)
        return 0;
    gunichar ch = g_utf8_get_char(key);
    return g_unichar_iscntrl(ch) ? 0 : (int)ch;
}

static gboolean
browser_accesskey_matches(const ns_node *el, const char *key)
{
    const char *ak = ns_element_get_attr(el, "accesskey");
    if (!ak || !*ak || !key || !*key) return FALSE;
    gsize klen = strlen(key);
    for (const char *p = ak; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        gsize len = (gsize)(p - s);
        if (len == klen && g_ascii_strncasecmp(s, key, len) == 0)
            return TRUE;
    }
    return FALSE;
}

static const ns_node *
browser_find_accesskey(const ns_node *n, const char *key, int depth)
{
    if (!n || depth > 1024) return NULL;
    if (n->kind == NS_NODE_ELEMENT && n->name &&
        g_ascii_strcasecmp(n->name, "template") == 0)
        return NULL;
    if (n->kind == NS_NODE_ELEMENT && browser_accesskey_matches(n, key) &&
        !ns_element_effectively_inert(n))
        return n;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *m = browser_find_accesskey(c, key, depth + 1);
        if (m) return m;
    }
    return NULL;
}

static gboolean
browser_select_key(ns_browser *browser, ns_node *select, const char *key,
                   int mods)
{
    if (!key || !*key || !browser->js) return FALSE;
    gboolean dropdown = ns_is_dropdown_select(select);
    if (strcmp(key, "ArrowDown") == 0)
        return ns_js_select_step(browser->js, select, +1);
    if (strcmp(key, "ArrowUp") == 0)
        return ns_js_select_step(browser->js, select, -1);
    if (strcmp(key, "Home") == 0)
        return ns_js_select_edge(browser->js, select, FALSE);
    if (strcmp(key, "End") == 0)
        return ns_js_select_edge(browser->js, select, TRUE);
    if (dropdown && (strcmp(key, "Enter") == 0 || strcmp(key, " ") == 0)) {
        browser->open_select = (browser->open_select == select) ? NULL : select;
        return TRUE;
    }
    if (dropdown && strcmp(key, "Escape") == 0 &&
        browser->open_select == select) {
        browser->open_select = NULL;
        return TRUE;
    }
    if ((mods & (2 | 4 | 8)) == 0 && g_utf8_validate(key, -1, NULL) &&
        g_utf8_strlen(key, -1) == 1 && key[0] != ' ' &&
        g_unichar_isprint(g_utf8_get_char(key)))
        return ns_js_select_typeahead(browser->js, select, key);
    return FALSE;
}

char *
ns_browser_key_full(ns_browser *browser, int kind, const char *key,
                    const char *code, int keycode, int mods,
                    int *out_prevented)
{
    if (out_prevented) *out_prevented = 0;
    if (!browser || !browser->js) return NULL;
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);

    const ns_node *target = ns_js_focused_node(browser->js);
    if (!target && browser->doc)
        target = ns_node_find_first_element(browser->doc, "body");
    if (!target) return NULL;

    if (kind == 2) {
        const ns_node *f = ns_js_focused_node(browser->js);
        if (f && ns_node_editable_value(f) && key && *key &&
            g_utf8_validate(key, -1, NULL)) {
            const char *cur = ns_node_editable_value(f);
            gsize cur_len = cur ? strlen(cur) : 0;
            if (browser->caret_byte > cur_len)
                browser->caret_byte = cur_len;
            if (browser->sel_anchor_byte > cur_len)
                browser->sel_anchor_byte = cur_len;
            gsize lo = browser->sel_anchor_byte < browser->caret_byte
                       ? browser->sel_anchor_byte : browser->caret_byte;
            gsize hi = browser->sel_anchor_byte < browser->caret_byte
                       ? browser->caret_byte : browser->sel_anchor_byte;
            browser_input_replace(browser, (ns_node *)f, lo, hi, key);
            browser->datalist_suppressed = FALSE;
            browser->dirty = TRUE;
        }
    } else {
        gboolean prevented = FALSE;
        ns_js_dispatch_key_event_full(browser->js, target,
                                      kind == 1 ? "keyup" : "keydown",
                                      key ? key : "", code ? code : "",
                                      keycode, 0,
                                      (mods & 1) != 0, (mods & 2) != 0,
                                      (mods & 4) != 0, (mods & 8) != 0,
                                      &prevented);
        if (out_prevented && prevented)
            *out_prevented = 1;
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;

        if (kind == 0 && !prevented && (mods & 4) && !(mods & (2 | 8)) &&
            key && g_utf8_validate(key, -1, NULL) &&
            g_utf8_strlen(key, -1) == 1 && browser->doc) {
            const ns_node *ak = browser_find_accesskey(browser->doc, key, 0);
            if (ak) {
                ns_js_set_focus(browser->js, ak);
                ns_js_activate_element(browser->js, ak);
                if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            }
        }

        int char_code = browser_key_char_code(key);
        if (kind == 3 && !prevented && char_code > 0 &&
            (mods & (2 | 4 | 8)) == 0) {
            gboolean press_prevented = FALSE;
            ns_js_dispatch_key_event_full(browser->js, target, "keypress",
                                          key ? key : "", code ? code : "",
                                          keycode, char_code,
                                          (mods & 1) != 0, FALSE, FALSE, FALSE,
                                          &press_prevented);
            if (out_prevented && press_prevented)
                *out_prevented = 1;
            if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
        }

        if (!prevented && kind == 0 && key && strcmp(key, "Tab") == 0) {
            gboolean backward = (mods & 1) != 0;
            const ns_node *next =
                ns_js_sequential_focus_target(browser->js, backward);
            if (next) {
                ns_js_set_focus(browser->js, (ns_node *)next);
                const char *val = ns_node_editable_value(next);
                browser->caret_byte = val ? strlen(val) : 0;
                browser->sel_anchor_byte = browser->caret_byte;
                browser->dirty = TRUE;
            }
        } else if (!prevented && kind == 0) {
            const ns_node *f = ns_js_focused_node(browser->js);
            if (f && ns_node_is_element_named(f, "input") &&
                !browser->datalist_suppressed && key &&
                strcmp(key, "Escape") == 0 &&
                ns_element_get_attr(f, "list") != NULL) {
                browser->datalist_suppressed = TRUE;
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            } else if (f && ns_node_is_element_named(f, "select") &&
                !ns_element_get_attr(f, "disabled") &&
                browser_select_key(browser, (ns_node *)f, key, mods)) {
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            } else if (f && ns_node_editable_value(f) &&
                browser_edit_key(browser, (ns_node *)f, key, mods))
                browser->dirty = TRUE;
        }
    }

    if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }

    char *nav = browser->pending_nav;
    browser->pending_nav = NULL;
    return nav;
}

char *
ns_browser_key(ns_browser *browser, int kind, const char *key,
               const char *code, int keycode, int mods)
{
    return ns_browser_key_full(browser, kind, key, code, keycode, mods, NULL);
}

int
ns_browser_focused_editable(ns_browser *browser)
{
    if (!browser || !browser->js)
        return 0;
    const ns_node *f = ns_js_focused_node(browser->js);
    return f && ns_node_editable_value(f) ? 1 : 0;
}

static gsize
browser_utf8_boundary(const char *s, gsize off)
{
    gsize len = s ? strlen(s) : 0;
    if (off >= len)
        return len;
    while (off > 0 && (((unsigned char)s[off] & 0xc0) == 0x80))
        off--;
    return off;
}

char *
ns_browser_focused_editable_value(ns_browser *browser, size_t *out_caret,
                                  size_t *out_anchor)
{
    if (out_caret)
        *out_caret = 0;
    if (out_anchor)
        *out_anchor = 0;
    if (!browser || !browser->js)
        return NULL;
    const ns_node *f = ns_js_focused_node(browser->js);
    const char *cur = f ? ns_node_editable_value(f) : NULL;
    if (!cur)
        return NULL;
    gsize len = strlen(cur);
    gsize caret = browser->caret_byte > len ? len : browser->caret_byte;
    gsize anchor = browser->sel_anchor_byte > len ? len
                                                  : browser->sel_anchor_byte;
    caret = browser_utf8_boundary(cur, caret);
    anchor = browser_utf8_boundary(cur, anchor);
    if (out_caret)
        *out_caret = caret;
    if (out_anchor)
        *out_anchor = anchor;
    return strdup(cur);
}

int
ns_browser_set_focused_editable_selection(ns_browser *browser, size_t caret,
                                          size_t anchor)
{
    if (!browser || !browser->js)
        return 0;
    const ns_node *f = ns_js_focused_node(browser->js);
    const char *cur = f ? ns_node_editable_value(f) : NULL;
    if (!cur)
        return 0;
    browser->caret_byte = browser_utf8_boundary(cur, (gsize)caret);
    browser->sel_anchor_byte = browser_utf8_boundary(cur, (gsize)anchor);
    browser->dirty = TRUE;
    browser_relayout(browser);
    browser->dirty = FALSE;
    return 1;
}

char *
ns_browser_title(ns_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    ns_node *title = ns_node_find_first_element(browser->doc, "title");
    if (!title) return NULL;
    char *raw = ns_node_collect_text(title);
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
ns_browser_url(ns_browser *browser)
{
    if (!browser || !browser->base_url) return NULL;
    return strdup(browser->base_url);
}

int
ns_browser_security(ns_browser *browser, const char **out_ip)
{
    if (out_ip)
        *out_ip = browser ? browser->remote_ip : NULL;
    return browser ? browser->security : NS_SEC_NONE;
}

char *
ns_browser_take_pending_nav(ns_browser *browser)
{
    if (!browser || !browser->pending_nav) return NULL;
    char *out = strdup(browser->pending_nav);
    g_clear_pointer(&browser->pending_nav, g_free);
    return out;
}

char *
ns_browser_take_pending_webgl(ns_browser *browser)
{
    (void)browser;
    return ns_webgl_take_pending_origin();
}

char *
ns_browser_take_pending_download(ns_browser *browser)
{
    if (!browser || !browser->pending_download) return NULL;
    char *out = browser->pending_download;
    browser->pending_download = NULL;
    return out;
}

void
ns_browser_resolve_webgl(ns_browser *browser, const char *origin, int allow)
{
    (void)browser;
    ns_webgl_set_decision(origin, allow);
}

char *
ns_browser_take_pending_camera(ns_browser *browser)
{
    (void)browser;
    return ns_camera_take_pending_origin();
}

void
ns_browser_resolve_camera(ns_browser *browser, const char *origin, int allow)
{
    ns_camera_set_decision(origin, allow);
    if (browser && browser->js) {
        char *r = ns_js_eval_source(browser->js,
            allow ? "__nd_camera_resolve_pending(true)"
                  : "__nd_camera_resolve_pending(false)",
            "camera-resolve");
        free(r);
    }
}

static void
collect_links(const ns_node *node, const char *base, GString *out,
              GHashTable *seen, int depth)
{
    if (!node || depth > 1024) return;
    for (const ns_node *c = node->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "a")) {
            const char *href = ns_element_get_attr(c, "href");
            if (href && *href && href[0] != '#' &&
                !g_str_has_prefix(href, "javascript:")) {
                char *abs = ns_url_resolve(base, href);
                if (abs && *abs && !g_hash_table_contains(seen, abs)) {
                    g_hash_table_add(seen, g_strdup(abs));
                    if (out->len) g_string_append_c(out, '\n');
                    g_string_append(out, abs);
                }
                g_free(abs);
            }
        }
        collect_links(c, base, out, seen, depth + 1);
    }
}

char *
ns_browser_links(ns_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    GString *out = g_string_new(NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    collect_links(browser->doc, browser->base_url, out, seen, 0);
    g_hash_table_destroy(seen);
    if (out->len == 0) { g_string_free(out, TRUE); return NULL; }
    char *result = strdup(out->str);
    g_string_free(out, TRUE);
    return result;
}

static gboolean
rel_token_is_icon(const char *rel)
{
    if (!rel)
        return FALSE;
    for (const char *p = rel; *p;) {
        while (*p && g_ascii_isspace(*p))
            p++;
        const char *start = p;
        while (*p && !g_ascii_isspace(*p))
            p++;
        if ((size_t)(p - start) == 4 &&
            g_ascii_strncasecmp(start, "icon", 4) == 0)
            return TRUE;
    }
    return FALSE;
}

static const char *
find_icon_href(const ns_node *node, int depth)
{
    if (!node || depth > 1024) return NULL;
    for (const ns_node *c = node->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "link")) {
            const char *href = ns_element_get_attr(c, "href");
            if (href && *href && rel_token_is_icon(ns_element_get_attr(c, "rel")))
                return href;
        }
        const char *found = find_icon_href(c, depth + 1);
        if (found)
            return found;
    }
    return NULL;
}

char *
ns_browser_favicon_url(ns_browser *browser)
{
    if (!browser || !browser->doc)
        return NULL;
    const char *href = find_icon_href(browser->doc, 0);
    char *abs = (href && *href) ? ns_url_resolve(browser->base_url, href) : NULL;
    if (abs && *abs) {
        char *out = strdup(abs);
        g_free(abs);
        return out;
    }
    g_free(abs);
    char *origin = ns_url_origin_from(browser->base_url);
    char *out = (origin && *origin) ? g_strconcat(origin, "/favicon.ico", NULL)
                                    : NULL;
    g_free(origin);
    if (!out)
        return NULL;
    char *dup = strdup(out);
    g_free(out);
    return dup;
}

int
ns_browser_bfcache_eligible(ns_browser *browser)
{
    return browser && browser->bfcache_ok;
}

void
ns_browser_bfcache_park(ns_browser *browser)
{
    if (!browser) return;
    if (browser->js)
        ns_js_fire_page_transition(browser->js, "pagehide", TRUE);
}

void
ns_browser_bfcache_restore(ns_browser *browser, int viewport_width,
                           double viewport_height)
{
    if (!browser) return;
    if (viewport_width > 0 && viewport_height > 0.0)
        ns_browser_set_viewport(browser, viewport_width, viewport_height);
    if (browser->js)
        ns_js_fire_page_transition(browser->js, "pageshow", TRUE);
}

int
ns_browser_busy(const ns_browser *browser)
{
    if (!browser) return 0;
    if (ns_engine_in_blocking_fetch()) return 1;
    return browser->js && ns_js_in_pump(browser->js);
}

void
ns_browser_close(ns_browser *browser)
{
    if (!browser) return;
    if (browser->img_sessions) {
        for (guint i = 0; i < browser->img_sessions->len; i++)
            ns_engine_img_session_close(
                g_ptr_array_index(browser->img_sessions, i));
        g_ptr_array_free(browser->img_sessions, TRUE);
    }
    if (browser->img_requested) g_hash_table_destroy(browser->img_requested);
    ns_css_set_active_node(NULL);
    ns_paint_set_anim(NULL);
    if (browser->js) {
        ns_js_set_layout_root(browser->js, NULL);
        ns_js_set_style_table(browser->js, NULL);
    }
    if (browser->anim) ns_anim_free(browser->anim);
    if (browser->layout) { ns_paint_3d_invalidate(); ns_box_free(browser->layout); }
    if (browser->styles) g_hash_table_destroy(browser->styles);
    if (browser->css_cache) g_hash_table_destroy(browser->css_cache);
    if (browser->js) ns_js_free(browser->js);
    if (browser->doc) ns_node_free(browser->doc);
    if (browser->videos) ns_video_cache_free(browser->videos);
    if (browser->images) ns_image_cache_free(browser->images);
    g_free(browser->base_url);
    g_free(browser->doc_charset);
    g_free(browser->doc_language);
    g_free(browser->pending_nav);
    g_free(browser->pending_download);
    if (browser->pending_audio) g_string_free(browser->pending_audio, TRUE);
    g_free(browser->refresh_url);
    g_free(browser->pending_post_body);
    g_free(browser->pending_post_ct);
    g_free(browser->search_query);
    g_free(browser->remote_ip);
    if (browser->console_buf) g_string_free(browser->console_buf, TRUE);
    g_free(browser);
}
