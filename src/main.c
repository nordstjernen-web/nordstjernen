/* Nordstjernen — GTK 4 application shell.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include <cairo-pdf.h>
#include <librsvg/rsvg.h>
#include <math.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#endif

#include "quickjs.h"
#include "anim.h"
#include "bcache.h"
#include "bookmarks.h"
#include "cache.h"
#include "config.h"
#include "console.h"
#include "css.h"
#include "ctxmenu.h"
#include "dialogs.h"
#include "debuglog.h"
#include "env.h"
#include "export.h"
#include "find.h"
#include "forms.h"
#include "headless.h"
#include "html.h"
#include "image.h"
#include "video.h"
#include "media.h"
#include "js.h"
#include "layout.h"
#include "mobile.h"
#include "net.h"
#include "paint.h"
#include "profiler.h"
#include "render.h"
#include "security.h"
#include "font.h"
#include "pdf.h"
#include "selection.h"
#include "version.h"
#include "window.h"

#define ND_APP_ID     "com.nordstjernen.Browser"
#define ND_TITLE      "Nordstjernen"

static char         *g_startup_url_override;
static char         *g_screenshot_path;
static int           g_screenshot_delay_ms;
static int           g_screenshot_every_ms;
static char         *g_self_exe;
static char         *g_home_url;
static nd_bookmarks *g_bookmarks;
static GFileMonitor *g_bookmarks_monitor;

nd_bookmarks *nd_app_bookmarks(void) { return g_bookmarks; }
const char   *nd_app_home_url(void)  { return g_home_url; }
void nd_app_set_home_url(const char *url)
{
    g_free(g_home_url);
    g_home_url = g_strdup(url ? url : "");
}

static gboolean
nd_profile_enabled(void)
{
    static gint cached = -1;
    if (G_UNLIKELY(cached < 0))
        cached = g_getenv("ND_PROFILE") ? 1 : 0;
    return cached == 1;
}

double
nd_layout_viewport(void)
{
    const nd_config *c = nd_config_get();
    return c && c->layout_viewport_px > 0 ? (double)c->layout_viewport_px : 1000.0;
}

static void nd_window_sync_selection_to_js(nd_window *w);
static void nd_window_record_final_url(nd_window *w, const nd_response *resp);
static void nd_window_set_busy(nd_window *w, gboolean busy);
static void nd_window_clear_cache(nd_window *w);
static void nd_install_icon_search_paths(void);
static void nd_window_open(GtkApplication *app, const char *startup_url);
static void nd_browser_close_tab(nd_window *w);
static void nd_window_update_tab_label(nd_window *w);
static void nd_setup_bookmarks_watch(GtkApplication *app);
static void nd_window_kick_image_loads(nd_window *w);
static void nd_window_kick_video_loads(nd_window *w);
static void nd_window_kick_favicon(nd_window *w);
static void        nd_window_js_log(const char *line, gpointer user_data);
static void        nd_window_js_soft_nav(const char *url, gboolean replace,
                                         gpointer user_data);
static void nd_window_install_actions(nd_window *w);
static void nd_window_kick_stylesheet_loads(nd_window *w);
static void nd_window_js_flush_layout(gpointer user_data);
static gboolean mixed_content_blocked(nd_window *w, const char *abs_url,
                                      const char *kind);
static gboolean csp_blocked(nd_window *w, nd_csp_kind kind, const char *abs_url,
                            const char *kind_word);
static gboolean nd_window_subresource_blocked(nd_window *w, const char *abs_url,
                                              nd_csp_kind csp_kind,
                                              const char *kind_word);
static void nd_window_apply_page_title(nd_window *w);
static void nd_window_apply_meta_refresh(nd_window *w, const nd_response *resp);
static gboolean nd_input_is_text_like(const nd_node *n);
static void nd_window_set_focused_input(nd_window *w, nd_node *target);
static void nd_window_open_select_popover(nd_window *w, nd_node *select_node,
                                          double x, double y);
static void nd_window_open_file_chooser(nd_window *w, nd_node *input);
static void nd_window_maybe_reset_form(nd_window *w, const nd_node *clicked);
static void nd_window_maybe_submit_form(nd_window *w, const nd_node *clicked);
static void nd_on_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data);

typedef struct nd_fetch_ctx {
    guint wid;
    guint gen;
} nd_fetch_ctx;

static gpointer
nd_fetch_ctx_new(nd_window *w)
{
    nd_fetch_ctx *c = g_new0(nd_fetch_ctx, 1);
    c->wid = w->id;
    c->gen = ++w->fetch_gen;
    return c;
}

static void     nd_window_mark_alive(nd_window *w);
static void     nd_window_mark_dead(nd_window *w);


void
nd_window_set_status(nd_window *w, const char *fmt, ...)
{
    (void)w; (void)fmt;
}

static void
nd_window_set_body_text(nd_window *w, const char *text, gssize len)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->text_view));
    gtk_text_buffer_set_text(buf, text ? text : "", (int)len);
}

static gboolean
nd_image_retryable_failure(const nd_image *img)
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
    return TRUE;
}

static guint
nd_image_retry_delay_ms(const nd_image *img)
{
    return img && img->attempts <= 1 ? 2000 : 10000;
}

static gboolean
nd_window_image_retry_cb(gpointer user_data)
{
    nd_window *w = user_data;
    if (!w) return G_SOURCE_REMOVE;
    w->image_retry_source = 0;
    if (w->mode == ND_VIEW_RENDER) {
        nd_window_kick_image_loads(w);
        if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    }
    return G_SOURCE_REMOVE;
}

static void
nd_window_schedule_image_retry(nd_window *w, const nd_image *img)
{
    if (!w || w->image_retry_source || !nd_image_retryable_failure(img))
        return;
    w->image_retry_source =
        g_timeout_add(nd_image_retry_delay_ms(img), nd_window_image_retry_cb, w);
}

static void
nd_window_sync_selection_to_js(nd_window *w)
{
    if (!w || !w->js) return;
    gboolean has = w->layout_tree && nd_selection_has_range(&w->selection);
    char *text = NULL;
    double x = 0, y = 0, sw = 0, sh = 0;
    if (has) {
        text = nd_selection_collect_text(w->layout_tree, &w->selection);
        nd_selection_bounds(w->layout_tree, &w->selection, &x, &y, &sw, &sh);
    }
    nd_js_set_selection(w->js, text ? text : "", has, x, y, sw, sh);
    g_free(text);
}

static void
nd_window_drop_layout(nd_window *w)
{
    if (w->layout_tree) {
        if (w->js) nd_js_set_layout_root(w->js, NULL);
        nd_box_free(w->layout_tree);
        w->layout_tree = NULL;
        nd_selection_clear(&w->selection);
        w->search_active_box = NULL;
    }
    if (w->style_table) {
        if (w->js) nd_js_set_style_table(w->js, NULL);
        g_hash_table_destroy(w->style_table);
        w->style_table = NULL;
    }
}

static void
nd_window_clear_cache(nd_window *w)
{
    g_clear_handle_id(&w->refresh_source, g_source_remove);
    g_clear_handle_id(&w->image_retry_source, g_source_remove);
    g_clear_pointer(&w->last_body, g_free);
    w->last_body_len = 0;
    g_clear_pointer(&w->last_content_type, g_free);
    if (w->csp) { if (w->js) nd_js_set_csp(w->js, NULL); nd_csp_free(w->csp); w->csp = NULL; }
    if (w->pdf) { nd_pdf_free(w->pdf); w->pdf = NULL; }
    nd_window_drop_layout(w);
    w->focused_input = NULL;
    g_clear_pointer(&w->focused_input_initial, g_free);
    g_clear_handle_id(&w->caret_blink_source, g_source_remove);
    if (w->parsed_doc) { nd_node_free(w->parsed_doc); w->parsed_doc = NULL; }
    if (w->js)         { nd_js_free(w->js);           w->js         = NULL; }
    if (w->css_cancellable) {
        g_cancellable_cancel(w->css_cancellable);
        g_clear_object(&w->css_cancellable);
    }
    if (w->external_stylesheets) {
        for (guint i = 0; i < w->external_stylesheets->len; i++)
            nd_css_stylesheet_free(g_ptr_array_index(w->external_stylesheets, i));
        g_ptr_array_set_size(w->external_stylesheets, 0);
    }
    if (w->external_css_seen)
        g_hash_table_remove_all(w->external_css_seen);
    w->css_inflight = 0;
    w->first_paint_done = FALSE;
    w->layout_dirty = TRUE;
    w->favicon_loaded = FALSE;
    g_clear_handle_id(&w->js_relayout_idle_id, g_source_remove);
}

static void
nd_adjustment_scroll_to(GtkAdjustment *adj, double y)
{
    double upper = gtk_adjustment_get_upper(adj);
    double page  = gtk_adjustment_get_page_size(adj);
    if (y > upper - page) y = upper - page;
    if (y < 0) y = 0;
    gtk_adjustment_set_value(adj, y);
}

static void
nd_window_scroll_to_fragment(nd_window *w)
{
    if (!w->pending_fragment || !w->render_vadj) return;
    if (!*w->pending_fragment ||
        g_ascii_strcasecmp(w->pending_fragment, "top") == 0) {
        nd_adjustment_scroll_to(w->render_vadj, 0);
        g_free(w->pending_fragment);
        w->pending_fragment = NULL;
        return;
    }
    if (!w->layout_tree) return;
    const nd_box *target =
        nd_box_find_by_id_or_name(w->layout_tree, w->pending_fragment);
    if (!target) return;
    nd_adjustment_scroll_to(w->render_vadj, target->y);
    g_free(w->pending_fragment);
    w->pending_fragment = NULL;
}

static void
nd_window_js_log(const char *line, gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !line) return;
    const char *nl = strchr(line, '\n');
    if (nl) {
        char *first = g_strndup(line, (gsize)(nl - line));
        nd_window_set_status(w, "JS: %s", first);
        g_free(first);
    } else {
        nd_window_set_status(w, "JS: %s", line);
    }
    nd_window_console_append(w, line);
}

static gboolean
nd_window_js_relayout_now(gpointer user_data)
{
    nd_window *w = user_data;
    if (!w) return G_SOURCE_REMOVE;
    w->js_relayout_idle_id = 0;
    if (w->js && w->layout_tree)
        nd_js_fire_media_load_events(w->js, w->layout_tree);
    nd_window_drop_layout(w);
    w->layout_dirty = TRUE;
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    nd_window_apply_page_title(w);
    return G_SOURCE_REMOVE;
}

void
nd_window_js_mutated(gpointer user_data)
{
    nd_window *w = user_data;
    if (!w) return;
    w->dom_mutated = TRUE;
    if (w->js_relayout_idle_id) return;
    w->js_relayout_idle_id =
        g_timeout_add(2000, nd_window_js_relayout_now, w);
}

static void
nd_window_js_scroll_to(const nd_node *target, gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !target || !w->layout_tree || !w->render_vadj) return;
    const char *id = nd_element_get_attr(target, "id");
    if (!id || !*id) return;
    const nd_box *box = nd_box_find_by_id(w->layout_tree, id);
    if (!box) return;
    nd_adjustment_scroll_to(w->render_vadj, box->y);
}

static void
nd_window_js_repaint(gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !w->drawing_area) return;
    gtk_widget_queue_draw(w->drawing_area);
}

static gboolean
nd_window_js_clipboard_write(const char *text, gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !w->window) return FALSE;
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(w->window));
    if (!cb) return FALSE;
    gdk_clipboard_set_text(cb, text ? text : "");
    return TRUE;
}

static void
nd_window_js_form_submit(const nd_node *form, const nd_node *submitter,
                         gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !form) return;
    nd_window_maybe_submit_form(w, submitter ? submitter : form);
}

static void
nd_window_js_navigate(const char *url, gboolean reload, gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !url) return;
    char *resolved = NULL;
    if (!reload && url[0] && !strstr(url, "://") &&
        !g_str_has_prefix(url, "about:") &&
        !g_str_has_prefix(url, "data:") &&
        !g_str_has_prefix(url, "mailto:")) {
        resolved = nd_resolve_url(w, url);
    }
    const char *target = resolved ? resolved : url;
    if (reload) {
        nd_window_load_url(w, target, ND_LOAD_HISTORY);
    } else {
        nd_window_load_url(w, target, ND_LOAD_USER);
    }
    g_free(resolved);
}

static void
nd_window_js_soft_nav(const char *url, gboolean replace, gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !url) return;
    if (w->url_entry) {
        char *disp = nd_url_to_display(url);
        gtk_editable_set_text(GTK_EDITABLE(w->url_entry), disp ? disp : url);
        g_free(disp);
    }
    if (!w->history) return;
    if (replace) {
        if (w->cursor >= 0 && w->cursor < (int)w->history->len) {
            g_free(g_ptr_array_index(w->history, w->cursor));
            w->history->pdata[w->cursor] = g_strdup(url);
        } else {
            g_ptr_array_add(w->history, g_strdup(url));
            w->cursor = (int)w->history->len - 1;
        }
    } else {
        while ((int)w->history->len > w->cursor + 1) {
            g_free(g_ptr_array_index(w->history, w->history->len - 1));
            g_ptr_array_set_size(w->history, w->history->len - 1);
        }
        g_ptr_array_add(w->history, g_strdup(url));
        w->cursor = (int)w->history->len - 1;
    }
    nd_window_update_nav_state(w);
}

static void
nd_window_maybe_reset_form(nd_window *w, const nd_node *clicked)
{
    if (!clicked) return;
    if (nd_element_effectively_disabled(clicked)) return;
    if (nd_element_effectively_inert(clicked)) return;
    if (!nd_form_is_reset_trigger(clicked)) return;
    const nd_node *doc = w && w->parsed_doc ? w->parsed_doc : nd_node_root(clicked);
    nd_node *form = (nd_node *)nd_form_owner(clicked, doc);
    if (!form) return;
    if (w->js) {
        gboolean prevented = FALSE;
        nd_js_dispatch_event(w->js, form, "reset", &prevented);
        if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
        if (prevented) return;
    }
    nd_form_reset_owned_controls(form, (nd_node *)(doc ? doc : form),
                                 doc ? doc : form);
    nd_window_js_mutated(w);
}

static void
nd_window_maybe_submit_form(nd_window *w, const nd_node *clicked)
{
    if (!clicked) return;
    if (nd_element_effectively_disabled(clicked)) return;
    if (nd_element_effectively_inert(clicked)) return;
    if (nd_form_is_reset_trigger(clicked)) {
        nd_window_maybe_reset_form(w, clicked);
        return;
    }
    gboolean from_text_input = nd_input_is_text_like(clicked);
    gboolean from_js = nd_node_is_element_named(clicked, "form");
    if (!from_text_input && !from_js && !nd_form_is_submit_trigger(clicked)) return;
    const nd_node *doc = w && w->parsed_doc ? w->parsed_doc : nd_node_root(clicked);
    const nd_node *form = from_js ? clicked : nd_form_owner(clicked, doc);
    if (!form) return;

    if (!nd_element_get_attr(form, "novalidate") &&
        (!clicked || !nd_element_get_attr(clicked, "formnovalidate"))) {
        const nd_node *bad = nd_form_first_invalid(form, doc ? doc : form,
                                                   doc ? doc : form);
        if (bad) {
            if (nd_input_is_text_like(bad)) {
                nd_window_set_focused_input(w, (nd_node *)bad);
                if (w->drawing_area) gtk_widget_grab_focus(w->drawing_area);
            }
            if (w->js)
                nd_js_dispatch_event(w->js, bad, "invalid", NULL);
            const char *name = nd_element_get_attr(bad, "name");
            nd_window_set_status(w, "Please fill out the %s field correctly",
                                 name && *name ? name : "highlighted");
            return;
        }
    }

    if (w->js) {
        gboolean prevented = FALSE;
        nd_js_dispatch_submit_event(w->js, form, clicked, &prevented);
        if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
        if (prevented) return;
    }

    const char *method = nd_element_get_attr(form, "method");
    const char *formmethod = (!from_text_input && clicked) ?
        nd_element_get_attr(clicked, "formmethod") : NULL;
    if (formmethod && *formmethod) method = formmethod;

    if (method && g_ascii_strcasecmp(method, "dialog") == 0) {
        const nd_node *dialog = form;
        while (dialog && !nd_node_is_element_named(dialog, "dialog"))
            dialog = dialog->parent;
        if (dialog && w->js) {
            const char *rv = NULL;
            if (!from_text_input && clicked && nd_form_is_submit_trigger(clicked))
                rv = nd_element_get_attr(clicked, "value");
            nd_js_dialog_close(w->js, (nd_node *)dialog, rv);
            if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
        }
        return;
    }
    gboolean is_post = method && g_ascii_strcasecmp(method, "post") == 0;

    const char *enctype = nd_element_get_attr(form, "enctype");
    const char *formenctype = (!from_text_input && clicked) ?
        nd_element_get_attr(clicked, "formenctype") : NULL;
    if (formenctype && *formenctype) enctype = formenctype;
    const nd_node *root = doc ? doc : form;
    gboolean has_files = nd_form_has_file_upload(form, root, root);
    gboolean use_multipart = is_post &&
        (has_files ||
         (enctype && g_ascii_strcasecmp(enctype, "multipart/form-data") == 0));

    GString *body = NULL;
    char *content_type = NULL;
    if (use_multipart) {
        char *boundary = nd_multipart_boundary();
        body = g_string_new(NULL);
        nd_form_collect_multipart(form, root, root, body, boundary, clicked);
        g_string_append_printf(body, "--%s--\r\n", boundary);
        content_type = g_strdup_printf("multipart/form-data; boundary=%s",
                                       boundary);
        g_free(boundary);
    } else {
        body = g_string_new(NULL);
        gboolean first = TRUE;
        nd_form_collect_inputs(form, root, root, body, &first, clicked);
        content_type = g_strdup("application/x-www-form-urlencoded");
    }

    const char *action = nd_element_get_attr(form, "action");
    const char *formaction = (!from_text_input && clicked) ?
        nd_element_get_attr(clicked, "formaction") : NULL;
    if (formaction && *formaction) action = formaction;
    char *abs_action;
    if (!action || !*action) abs_action = g_strdup(nd_window_current_url(w));
    else                      abs_action = nd_resolve_url(w, action);
    if (!abs_action) {
        g_string_free(body, TRUE);
        g_free(content_type);
        return;
    }

    if (is_post) {
        if (w->current_fetch) {
            g_cancellable_cancel(w->current_fetch);
            g_clear_object(&w->current_fetch);
        }
        while ((int)w->history->len > w->cursor + 1) {
            g_free(g_ptr_array_index(w->history, w->history->len - 1));
            g_ptr_array_set_size(w->history, w->history->len - 1);
        }
        g_ptr_array_add(w->history, g_strdup(abs_action));
        w->cursor = (int)w->history->len - 1;
        char *disp = nd_url_to_display(abs_action);
        gtk_editable_set_text(GTK_EDITABLE(w->url_entry),
                              disp ? disp : abs_action);
        g_free(disp);
        w->current_fetch = g_cancellable_new();
        nd_window_set_busy(w, TRUE);
        nd_window_update_nav_state(w);
        nd_window_set_status(w, "POST %s …", abs_action);
        nd_net_post_async(abs_action, nd_window_current_url(w),
                          body->str, body->len,
                          content_type,
                          w->current_fetch, nd_on_fetch_done,
                          nd_fetch_ctx_new(w));
        g_free(abs_action);
        g_string_free(body, TRUE);
        g_free(content_type);
        return;
    }
    GString *query = body;
    g_free(content_type);

    if (query->len == 0) {
        nd_window_load_url(w, abs_action, ND_LOAD_USER);
        g_free(abs_action);
        g_string_free(query, TRUE);
        return;
    }

    char *frag = strchr(abs_action, '#');
    if (frag) *frag = '\0';
    char *sep = strchr(abs_action, '?');
    char *full = sep ? g_strdup_printf("%s&%s", abs_action, query->str)
                     : g_strdup_printf("%s?%s", abs_action, query->str);
    g_free(abs_action);
    g_string_free(query, TRUE);
    nd_window_load_url(w, full, ND_LOAD_USER);
    g_free(full);
}

static void
nd_window_set_title_if_active(nd_window *w, const char *full)
{
    nd_window *active = w->window
        ? g_object_get_data(G_OBJECT(w->window), "nd-window") : NULL;
    if (active == w)
        gtk_window_set_title(GTK_WINDOW(w->window), full);
}

static void
nd_window_apply_page_title(nd_window *w)
{
    nd_window_update_tab_label(w);
    if (!w->parsed_doc) {
        nd_window_set_title_if_active(w, ND_TITLE);
        return;
    }
    nd_node *title = nd_node_find_first_element(w->parsed_doc, "title");
    if (!title) {
        nd_window_set_title_if_active(w, ND_TITLE);
        return;
    }
    char *raw = nd_node_collect_text(title);
    if (!raw || !*raw) { g_free(raw); nd_window_set_title_if_active(w, ND_TITLE); return; }
    GString *trimmed = g_string_new(NULL);
    gboolean prev_ws = TRUE;
    for (const char *p = raw; *p; p++) {
        char c = *p;
        gboolean ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f');
        if (ws) { if (!prev_ws) g_string_append_c(trimmed, ' '); prev_ws = TRUE; }
        else    { g_string_append_c(trimmed, c); prev_ws = FALSE; }
    }
    if (trimmed->len > 0 && trimmed->str[trimmed->len - 1] == ' ')
        g_string_set_size(trimmed, trimmed->len - 1);
    g_free(raw);

    if (trimmed->len > 0) {
        char *full = g_strdup_printf("%s — %s", trimmed->str, ND_TITLE);
        nd_window_set_title_if_active(w, full);
        g_free(full);
        if (w->drawing_area) {
            char *aria = g_strdup_printf("Web page: %s", trimmed->str);
            gtk_accessible_update_property(GTK_ACCESSIBLE(w->drawing_area),
                GTK_ACCESSIBLE_PROPERTY_LABEL, aria, -1);
            g_free(aria);
        }
    } else {
        nd_window_set_title_if_active(w, ND_TITLE);
    }
    g_string_free(trimmed, TRUE);
}

gboolean
nd_window_raf_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer ud)
{
    (void)widget; (void)clock;
    nd_window *w = ud;
    if (!w) return G_SOURCE_CONTINUE;
    gboolean redraw = FALSE;
    gint64 now_us = g_get_monotonic_time();
    if (w->images && nd_image_cache_tick(w->images, now_us))
        redraw = TRUE;
    if (w->anim && nd_anim_tick(w->anim, now_us))
        redraw = TRUE;
    if (w->js && nd_js_run_animation_frame(w->js)) {
        if (nd_js_consume_mutated(w->js)) {
            nd_window_js_mutated(w);
            return G_SOURCE_CONTINUE;
        }
        redraw = TRUE;
    }
    if (redraw && w->drawing_area)
        gtk_widget_queue_draw(w->drawing_area);
    return G_SOURCE_CONTINUE;
}

static char *
nd_window_render_resolve(const char *href, gpointer ud)
{
    return nd_resolve_url((const nd_window *)ud, href);
}

static gboolean
nd_window_render_font_allowed(const char *abs_url, gpointer ud)
{
    return !nd_window_subresource_blocked((nd_window *)ud, abs_url,
                                          ND_CSP_FONT, "font");
}

static void
nd_window_mark_layout_dirty(nd_window *w)
{
    if (!w) return;
    if (w->layout_tree) {
        if (w->js) nd_js_set_layout_root(w->js, NULL);
        nd_box_free(w->layout_tree);
        w->layout_tree = NULL;
        nd_selection_clear(&w->selection);
        w->search_active_box = NULL;
    }
    if (w->style_table) {
        if (w->js) nd_js_set_style_table(w->js, NULL);
        g_hash_table_destroy(w->style_table);
        w->style_table = NULL;
    }
    w->layout_dirty = TRUE;
}

static void
nd_window_append_stylesheet_expanded(nd_window *w, GPtrArray *out,
                                     nd_css_stylesheet *sh,
                                     const char *base_url,
                                     GHashTable *seen,
                                     int depth)
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
            if (w && nd_window_subresource_blocked(w, abs, ND_CSP_STYLE,
                                                   "stylesheet")) {
                g_free(abs);
                continue;
            }
            if (seen) g_hash_table_add(seen, g_strdup(abs));
            GError *err = NULL;
            nd_response *resp = nd_net_request_blocking(
                abs, w ? nd_window_current_url(w) : NULL, "GET",
                NULL, 0, NULL, NULL, w ? w->css_cancellable : NULL, &err);
            if (err) {
                if (w && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                    char *line = g_strdup_printf("[error] stylesheet: %s — %s",
                                                 abs, err->message);
                    nd_window_console_append(w, line);
                    g_free(line);
                }
                g_clear_error(&err);
            } else if (resp && !resp->error && resp->status < 400 &&
                       resp->body && resp->body->len > 0) {
                nd_css_stylesheet *child = nd_css_stylesheet_parse(
                    (const char *)resp->body->data, (gssize)resp->body->len);
                if (child) {
                    if (im->layer_name)
                        nd_css_stylesheet_force_layer(child, im->layer_name);
                    nd_window_append_stylesheet_expanded(w, out, child, abs,
                                                         seen, depth + 1);
                }
            } else if (w && resp) {
                char *line = resp->error
                    ? g_strdup_printf("[error] stylesheet: %s — %s",
                                      abs, resp->error)
                    : g_strdup_printf("[error] stylesheet: %s — HTTP %ld",
                                      abs, resp->status);
                nd_window_console_append(w, line);
                g_free(line);
            }
            if (resp) nd_response_free(resp);
            g_free(abs);
        }
    }
    nd_css_stylesheet_resolve_urls(sh, base_url);
    g_ptr_array_add(out, sh);
}

void
nd_window_ensure_layout(nd_window *w, double viewport_width)
{
    if (!w->last_body) return;
    if (w->layout_tree && w->parsed_doc && !w->layout_dirty &&
        fabs(viewport_width - w->last_viewport_w) < 16.0)
        return;

    gboolean profile = nd_profile_enabled();
    gint64 t_start = g_get_monotonic_time();
    if (w->layout_tree) { if (w->js) nd_js_set_layout_root(w->js, NULL); nd_box_free(w->layout_tree); w->layout_tree = NULL; nd_selection_clear(&w->selection); w->search_active_box = NULL; }
    if (w->style_table) { if (w->js) nd_js_set_style_table(w->js, NULL); g_hash_table_destroy(w->style_table); w->style_table = NULL; }
    gint64 t_after_free = g_get_monotonic_time();

    const char *page_url = nd_window_current_url(w);
    gboolean parsed_fresh = FALSE;
    if (!w->parsed_doc) {
        w->parsed_doc = nd_html_parse(w->last_body,
                                      (gssize)w->last_body_len);
        parsed_fresh = TRUE;
    }
    gint64 t_after_parse = g_get_monotonic_time();

    GPtrArray *inline_sheets = g_ptr_array_new();
    GPtrArray *page_sheets = g_ptr_array_new();
    nd_collect_inline_stylesheets(w->parsed_doc, inline_sheets);
    GHashTable *inline_import_seen =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < inline_sheets->len; i++) {
        nd_css_stylesheet *sh = g_ptr_array_index(inline_sheets, i);
        nd_window_append_stylesheet_expanded(w, page_sheets, sh, page_url,
                                             inline_import_seen, 0);
    }
    g_hash_table_destroy(inline_import_seen);
    g_ptr_array_free(inline_sheets, TRUE);
    guint inline_sheet_count = page_sheets->len;

    if (w->external_stylesheets)
        for (guint i = 0; i < w->external_stylesheets->len; i++)
            g_ptr_array_add(page_sheets,
                            g_ptr_array_index(w->external_stylesheets, i));

    double viewport_height = viewport_width * 0.75;
    if (w->drawing_area) {
        GtkWidget *sw = gtk_widget_get_ancestor(w->drawing_area,
                                                GTK_TYPE_SCROLLED_WINDOW);
        double vis_h = 0;
        if (sw) {
            GtkAdjustment *va =
                gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
            double page = va ? gtk_adjustment_get_page_size(va) : 0;
            if (page > 100) vis_h = page;
            else { int swh = gtk_widget_get_height(sw); if (swh > 100) vis_h = swh; }
        }
        if (vis_h <= 100) {
            int alloc_h = gtk_widget_get_height(w->drawing_area);
            if (alloc_h > 100) vis_h = (double)alloc_h;
        }
        if (vis_h > 100) viewport_height = vis_h;
    }

    gint64 t_before_layout = g_get_monotonic_time();
    nd_render_ctx rc = {
        .doc             = w->parsed_doc,
        .sheets          = (const nd_css_stylesheet *const *)page_sheets->pdata,
        .n_sheets        = page_sheets->len,
        .viewport_width  = viewport_width,
        .viewport_height = viewport_height,
        .zoom            = w->zoom,
        .images          = w->images,
        .base_url        = nd_window_current_url(w),
        .anim            = w->anim,
        .js              = w->js,
        .focused_input   = w->focused_input,
        .caret_byte      = w->caret_byte,
        .sel_anchor_byte = w->sel_anchor_byte,
        .resolve_url     = nd_window_render_resolve,
        .font_allowed    = nd_window_render_font_allowed,
        .cb_ud           = w,
    };
    w->style_table = nd_render_relayout(&rc, &w->layout_tree);
    gint64 t_after_layout = g_get_monotonic_time();

    for (guint i = 0; i < inline_sheet_count; i++)
        nd_css_stylesheet_free(g_ptr_array_index(page_sheets, i));
    g_ptr_array_free(page_sheets, TRUE);
    nd_window_apply_page_title(w);
    nd_window_kick_image_loads(w);
    nd_window_kick_video_loads(w);
    nd_window_kick_stylesheet_loads(w);
    if (w->drawing_area && w->layout_tree) {
        double ext_w = 0, ext_h = 0;
        nd_box_content_extent(w->layout_tree, &ext_w, &ext_h);
        int h = (int)(ext_h + 32);
        int min_w = (int)(ext_w + 0.5);
        if (min_w <= (int)viewport_width) min_w = -1;
        gtk_widget_set_size_request(w->drawing_area, min_w, h);
        if (g_getenv("ND_LAYOUT_DEBUG"))
            g_warning("layout: vp=%.0f ext=%.0fx%.0f root=%.0fx%.0f",
                      viewport_width, ext_w, ext_h,
                      w->layout_tree->content_width,
                      w->layout_tree->content_height);
    }
    gint64 t_end = g_get_monotonic_time();
    w->last_render_us = t_end - t_start;
    w->layout_dirty = FALSE;
    w->last_viewport_w = viewport_width;
    {
        guint style_count = w->style_table ? g_hash_table_size(w->style_table) : 0;
        nd_debug_log_emit(ND_DLOG_RENDER, "layout",
            "vp=%.0f styles=%u sheets=%u parse=%.1fms%s render=%.1fms "
            "total=%.1fms",
            viewport_width, style_count, (guint)inline_sheet_count,
            (t_after_parse  - t_after_free)   / 1000.0,
            parsed_fresh ? "" : "(cached)",
            (t_after_layout- t_before_layout) / 1000.0,
            (t_end         - t_start)         / 1000.0);
        if (profile) {
            g_printerr("[profile] render vp=%.0f styles=%u sheets=%u "
                       "free=%.1fms parse=%.1fms%s render=%.1fms total=%.1fms\n",
                       viewport_width, style_count, (guint)inline_sheet_count,
                       (t_after_free   - t_start)        / 1000.0,
                       (t_after_parse  - t_after_free)   / 1000.0,
                       parsed_fresh ? "" : "(cached)",
                       (t_after_layout- t_before_layout) / 1000.0,
                       (t_end         - t_start)         / 1000.0);
        }
    }
}

static void
nd_window_js_flush_layout(gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !w->js) return;
    gboolean mutated = nd_js_consume_mutated(w->js);
    gboolean dirty = !w->layout_tree || mutated;
    if (!dirty) return;
    if (mutated && w->layout_tree) w->layout_dirty = TRUE;
    double vw = w->last_viewport_w > 0 ? w->last_viewport_w : nd_layout_viewport();
    nd_window_ensure_layout(w, vw);
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
}

void
nd_window_ensure_js(nd_window *w)
{
    if (!w || w->js) {
        if (w && w->js) nd_js_set_image_cache(w->js, w->images);
        return;
    }
    w->js = nd_js_new(nd_window_js_log, w,
                      nd_window_js_mutated, w,
                      nd_window_js_navigate, w);
    if (!w->js) return;
    nd_js_set_scroll_to_cb(w->js, nd_window_js_scroll_to, w);
    nd_js_set_form_submit_cb(w->js, nd_window_js_form_submit, w);
    nd_js_set_soft_nav_cb(w->js, nd_window_js_soft_nav, w);
    nd_js_set_repaint_cb(w->js, nd_window_js_repaint, w);
    nd_js_set_layout_flush_cb(w->js, nd_window_js_flush_layout, w);
    nd_js_set_clipboard_write_cb(w->js, nd_window_js_clipboard_write, w);
    nd_js_set_image_cache(w->js, w->images);
}

static gboolean
is_html_content_type(const char *ct)
{
    if (!ct) return FALSE;
    return g_ascii_strncasecmp(ct, "text/html", 9) == 0 ||
           g_ascii_strncasecmp(ct, "application/xhtml+xml", 21) == 0 ||
           g_ascii_strncasecmp(ct, "application/xml", 15) == 0 ||
           g_ascii_strncasecmp(ct, "text/xml", 8) == 0;
}

static char *
to_utf8_or_pass(const char *body, gsize len)
{
    return nd_html_decode_body(body, len);
}

void
nd_window_render(nd_window *w)
{
    if (w->pdf) {
        gtk_stack_set_visible_child_name(GTK_STACK(w->content_stack), "render");
        if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
        return;
    }
    if (!w->last_body) {
        nd_window_set_body_text(w, "", 0);
        return;
    }

    gboolean is_html = is_html_content_type(w->last_content_type);

    if (w->mode == ND_VIEW_RENDER && is_html) {
        gtk_stack_set_visible_child_name(GTK_STACK(w->content_stack), "render");
        gtk_widget_queue_draw(w->drawing_area);
        return;
    }

    gtk_stack_set_visible_child_name(GTK_STACK(w->content_stack), "text");

    if (w->mode == ND_VIEW_DOM && is_html) {
        nd_node *doc = nd_html_parse(w->last_body,
                                     (gssize)w->last_body_len);
        GString *dump = nd_node_dump(doc);
        nd_window_set_body_text(w, dump->str, (gssize)dump->len);
        g_string_free(dump, TRUE);
        nd_node_free(doc);
        return;
    }

    if (w->mode == ND_VIEW_LAYOUT && is_html) {
        nd_window_ensure_layout(w, nd_layout_viewport());
        GString *dump = nd_box_dump(w->layout_tree);
        nd_window_set_body_text(w, dump->str, (gssize)dump->len);
        g_string_free(dump, TRUE);
        return;
    }

    char *utf8 = to_utf8_or_pass(w->last_body, w->last_body_len);
    nd_window_set_body_text(w, utf8, -1);
    g_free(utf8);
}

static void
nd_window_follow_href(nd_window *w, const char *href, const char *target,
                      GdkModifierType mods)
{
    if (!href || !*href) return;
    if (g_str_has_prefix(href, "javascript:")) {
        const char *code = href + strlen("javascript:");
        if (!nd_csp_javascript_url_allowed(w->csp)) {
            g_warning("CSP blocked: javascript: URL navigation");
            return;
        }
        if (w->js && *code) {
            char *result = nd_js_eval_source(w->js, code, "href");
            g_free(result);
            if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
        }
        return;
    }
    if (g_str_has_prefix(href, "#")) {
        const char *frag = href + 1;
        const char *cur = nd_window_current_url(w);
        char *base = cur ? g_strdup(cur) : g_strdup("");
        char *hash = strchr(base, '#');
        if (hash) *hash = '\0';
        char *old_url = g_strdup(cur ? cur : "");
        char *new_url = g_strconcat(base, "#", frag, NULL);
        g_free(base);
        g_free(w->pending_fragment);
        w->pending_fragment = g_strdup(frag);
        nd_css_set_target_fragment(*frag ? frag : NULL);
        nd_window_js_soft_nav(new_url, FALSE, w);
        w->layout_dirty = TRUE;
        nd_window_ensure_layout(w, nd_layout_viewport());
        nd_window_scroll_to_fragment(w);
        if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
        if (w->js && strcmp(old_url, new_url) != 0)
            nd_js_dispatch_hashchange(w->js, old_url, new_url);
        g_free(old_url);
        g_free(new_url);
        return;
    }
    if (g_str_has_prefix(href, "mailto:")) return;
    char *abs_url = nd_resolve_url(w, href);
    if (!abs_url) return;
    gboolean new_win = (mods & GDK_CONTROL_MASK) != 0 ||
                       (target && strcmp(target, "_blank") == 0);
    if (new_win) {
        GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
        nd_spawn_window(app, abs_url);
    } else {
        nd_window_load_url(w, abs_url, ND_LOAD_USER);
    }
    g_free(abs_url);
}

static gboolean
nd_is_labelable(const nd_node *n)
{
    if (!n || n->kind != ND_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "select")   == 0 ||
        strcmp(n->name, "textarea") == 0 ||
        strcmp(n->name, "button")   == 0 ||
        strcmp(n->name, "meter")    == 0 ||
        strcmp(n->name, "progress") == 0 ||
        strcmp(n->name, "output")   == 0)
        return TRUE;
    if (strcmp(n->name, "input") == 0) {
        const char *type = nd_element_get_attr(n, "type");
        return !(type && g_ascii_strcasecmp(type, "hidden") == 0);
    }
    return FALSE;
}

static nd_node *
nd_first_labelable_descendant(const nd_node *n)
{
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (nd_is_labelable(c)) return (nd_node *)c;
        nd_node *deep = nd_first_labelable_descendant(c);
        if (deep) return deep;
    }
    return NULL;
}

gboolean
nd_window_media_target(nd_window *w, const nd_box *hit,
                       char **out_url, gboolean *out_is_video,
                       gboolean *out_stream)
{
    if (!hit || !hit->dom) return FALSE;
    gboolean is_video = nd_node_is_element_named(hit->dom, "video");
    gboolean is_audio = nd_node_is_element_named(hit->dom, "audio");
    if (!is_video && !is_audio) return FALSE;
    const char *msrc = NULL;
    if (hit->media)
        msrc = is_video ? hit->media->video_src : hit->media->video_audio_src;
    char *abs = msrc ? nd_resolve_url(w, msrc) : NULL;
    gboolean stream = !abs || g_str_has_prefix(abs, "blob:") ||
                      g_str_has_prefix(abs, "data:");
    if (stream) {
        g_free(abs);
        const char *page = nd_window_current_url(w);
        abs = page ? g_strdup(page) : NULL;
    }
    if (!abs) return FALSE;
    *out_url = abs;
    *out_is_video = is_video;
    *out_stream = stream;
    return TRUE;
}

void
nd_on_drawing_pressed(GtkGestureClick *gesture, int n_press,
                      double x, double y, gpointer user_data)
{
    (void)n_press;
    nd_window *w = user_data;
    if (nd_selection_has_range(&w->selection)) {
        nd_selection_clear(&w->selection);
        nd_window_sync_selection_to_js(w);
        if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    }
    if (!w->layout_tree) return;
    const nd_link_range *link = nd_box_hit_link_range(w->layout_tree, x, y);
    if (!link) {
        const nd_node *form_target = nd_box_hit_form_dom(w->layout_tree, x, y);
        const nd_box *hit = nd_box_hit_test(w->layout_tree, x, y);
        if (form_target) {
            if (nd_element_effectively_disabled(form_target) ||
                nd_element_effectively_inert(form_target)) {
                nd_window_set_focused_input(w, NULL);
                return;
            }
            gboolean prevented = FALSE;
            if (w->js) {
                nd_js_dispatch_event(w->js, form_target, "click", &prevented);
                if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
            }
            if (!prevented) {
                if (nd_input_is_text_like(form_target)) {
                    nd_window_set_focused_input(w, (nd_node *)form_target);
                    gtk_widget_grab_focus(w->drawing_area);
                } else if (form_target->kind == ND_NODE_ELEMENT &&
                           form_target->name &&
                           strcmp(form_target->name, "input") == 0) {
                    const char *type = nd_element_get_attr(form_target, "type");
                    if (type && g_ascii_strcasecmp(type, "checkbox") == 0) {
                        if (nd_element_get_attr(form_target, "checked"))
                            nd_element_remove_attr((nd_node *)form_target, "checked");
                        else
                            nd_element_set_attr((nd_node *)form_target, "checked", "");
                        if (w->js) {
                            nd_js_dispatch_event(w->js, form_target, "input",  NULL);
                            nd_js_dispatch_event(w->js, form_target, "change", NULL);
                        }
                        nd_window_js_mutated(w);
                    } else if (type && g_ascii_strcasecmp(type, "radio") == 0) {
                        nd_clear_radio_group_for(w->parsed_doc, form_target);
                        nd_element_set_attr((nd_node *)form_target, "checked", "");
                        if (w->js) {
                            nd_js_dispatch_event(w->js, form_target, "input",  NULL);
                            nd_js_dispatch_event(w->js, form_target, "change", NULL);
                        }
                        nd_window_js_mutated(w);
                    } else if (type && g_ascii_strcasecmp(type, "file") == 0) {
                        nd_window_set_focused_input(w, NULL);
                        nd_window_open_file_chooser(w, (nd_node *)form_target);
                    } else {
                        nd_window_set_focused_input(w, NULL);
                        nd_window_maybe_submit_form(w, form_target);
                    }
                } else {
                    nd_window_set_focused_input(w, NULL);
                    nd_window_maybe_submit_form(w, form_target);
                }
            }
            return;
        }
        if (hit && hit->dom) {
            if (nd_element_effectively_inert(hit->dom)) {
                nd_window_set_focused_input(w, NULL);
                return;
            }
            gboolean prevented = FALSE;
            if (w->js) {
                nd_js_dispatch_event(w->js, hit->dom, "click", &prevented);
                if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
            }
            if (!prevented) {
                char *media_abs = NULL;
                gboolean is_video_el = FALSE, stream = FALSE;
                if (nd_window_media_target(w, hit, &media_abs,
                                           &is_video_el, &stream)) {
                    if (nd_media_launch_external(GTK_WINDOW(w->window),
                                                 media_abs, is_video_el, stream))
                        nd_window_set_status(w, "Opening %s in external player…",
                                             is_video_el ? "video" : "audio");
                    g_free(media_abs);
                    return;
                }
                if (nd_node_is_element_named(hit->dom, "img")) {
                    const char *usemap = nd_element_get_attr(hit->dom, "usemap");
                    if (usemap && *usemap && w->parsed_doc) {
                        double cx0 = hit->x + hit->margin.left +
                                     hit->border.left + hit->padding.left;
                        double cy0 = hit->y + hit->margin.top +
                                     hit->border.top + hit->padding.top;
                        const char *atarget = NULL;
                        char *ahref = nd_image_map_resolve(w->parsed_doc, usemap,
                                                           x - cx0, y - cy0,
                                                           hit->content_width,
                                                           hit->content_height,
                                                           &atarget);
                        if (ahref) {
                            GdkEvent *ev = gtk_event_controller_get_current_event(
                                GTK_EVENT_CONTROLLER(gesture));
                            GdkModifierType mods =
                                ev ? gdk_event_get_modifier_state(ev) : 0;
                            nd_window_follow_href(w, ahref, atarget, mods);
                            g_free(ahref);
                            return;
                        }
                    }
                }
                const nd_node *cur = hit->dom;
                gboolean handled = FALSE;
                while (cur && !handled) {
                    if (nd_node_is_element_named(cur, "a")) {
                        const char *href = nd_element_get_attr(cur, "href");
                        if (href && *href) {
                            GdkEvent *event = gtk_event_controller_get_current_event(
                                GTK_EVENT_CONTROLLER(gesture));
                            GdkModifierType mods = event ?
                                gdk_event_get_modifier_state(event) : 0;
                            const char *target = nd_element_get_attr(cur, "target");
                            nd_window_follow_href(w, href, target, mods);
                            handled = TRUE;
                            break;
                        }
                    }
                    if (nd_node_is_element_named(cur, "label")) {
                        nd_node *target = NULL;
                        const char *for_id = nd_element_get_attr(cur, "for");
                        if (for_id && *for_id && w->parsed_doc)
                            target = nd_node_find_by_id(w->parsed_doc, for_id);
                        if (!target)
                            target = nd_first_labelable_descendant(cur);
                        if (target && target != cur && nd_is_labelable(target)) {
                            cur = target;
                            continue;
                        }
                    }
                    if (nd_input_is_text_like(cur)) {
                        nd_window_set_focused_input(w, (nd_node *)cur);
                        gtk_widget_grab_focus(w->drawing_area);
                        handled = TRUE;
                        break;
                    }
                    if (nd_node_is_contenteditable_host(cur)) {
                        nd_window_set_focused_input(w, (nd_node *)cur);
                        gtk_widget_grab_focus(w->drawing_area);
                        handled = TRUE;
                        break;
                    }
                    if (cur->kind == ND_NODE_ELEMENT && cur->name) {
                        if (strcmp(cur->name, "select") == 0) {
                            nd_window_open_select_popover(w, (nd_node *)cur, x, y);
                            handled = TRUE;
                            break;
                        }
                        if (strcmp(cur->name, "summary") == 0 &&
                            cur->parent && cur->parent->kind == ND_NODE_ELEMENT &&
                            cur->parent->name &&
                            strcmp(cur->parent->name, "details") == 0) {
                            nd_node *details = cur->parent;
                            gboolean now_open;
                            if (nd_element_get_attr(details, "open")) {
                                nd_element_remove_attr(details, "open");
                                now_open = FALSE;
                            } else {
                                nd_element_set_attr(details, "open", "");
                                now_open = TRUE;
                            }
                            if (w->js)
                                nd_js_details_toggle_open(w->js, details, now_open);
                            nd_window_js_mutated(w);
                            return;
                        }
                        if (strcmp(cur->name, "input") == 0) {
                            const char *type = nd_element_get_attr(cur, "type");
                            if (type && g_ascii_strcasecmp(type, "checkbox") == 0) {
                                if (nd_element_get_attr(cur, "checked"))
                                    nd_element_remove_attr((nd_node *)cur, "checked");
                                else
                                    nd_element_set_attr((nd_node *)cur, "checked", "");
                                if (w->js) {
                                    nd_js_dispatch_event(w->js, cur, "input",  NULL);
                                    nd_js_dispatch_event(w->js, cur, "change", NULL);
                                }
                                nd_window_js_mutated(w);
                                handled = TRUE;
                                break;
                            }
                            if (type && g_ascii_strcasecmp(type, "radio") == 0) {
                                nd_clear_radio_group_for(w->parsed_doc, cur);
                                nd_element_set_attr((nd_node *)cur, "checked", "");
                                if (w->js) {
                                    nd_js_dispatch_event(w->js, cur, "input",  NULL);
                                    nd_js_dispatch_event(w->js, cur, "change", NULL);
                                }
                                nd_window_js_mutated(w);
                                handled = TRUE;
                                break;
                            }
                            if (type && g_ascii_strcasecmp(type, "file") == 0) {
                                nd_window_set_focused_input(w, NULL);
                                nd_window_open_file_chooser(w, (nd_node *)cur);
                                handled = TRUE;
                                break;
                            }
                        }
                    }
                    cur = cur->parent;
                }
                if (!handled && hit->dom) {
                    const nd_node *probe[1] = { hit->dom };
                    GQueue q = G_QUEUE_INIT;
                    g_queue_push_tail(&q, (gpointer)probe[0]);
                    nd_node *picked = NULL;
                    while (!g_queue_is_empty(&q) && !picked) {
                        const nd_node *n = g_queue_pop_head(&q);
                        if (nd_input_is_text_like(n)) { picked = (nd_node *)n; break; }
                        for (const nd_node *d = n->first_child; d; d = d->next_sibling)
                            g_queue_push_tail(&q, (gpointer)d);
                    }
                    g_queue_clear(&q);
                    if (picked) {
                        nd_window_set_focused_input(w, picked);
                        gtk_widget_grab_focus(w->drawing_area);
                        handled = TRUE;
                    }
                }
                if (!handled) {
                    nd_window_set_focused_input(w, NULL);
                    nd_window_maybe_submit_form(w, hit->dom);
                }
            }
        }
        return;
    }
    if (link->dom && nd_element_effectively_inert(link->dom)) {
        nd_window_set_focused_input(w, NULL);
        return;
    }
    if (w->js && link->dom) {
        gboolean prevented = FALSE;
        nd_js_dispatch_event(w->js, link->dom, "click", &prevented);
        if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
        if (prevented) return;
    }
    GdkEvent *event = gtk_event_controller_get_current_event(
        GTK_EVENT_CONTROLLER(gesture));
    GdkModifierType mods = event ? gdk_event_get_modifier_state(event) : 0;
    nd_window_follow_href(w, link->href, link->target, mods);
}

static const nd_node *
nd_window_key_target(nd_window *w)
{
    if (!w->parsed_doc) return NULL;
    nd_node *body = nd_node_find_first_element(w->parsed_doc, "body");
    return body ? body : w->parsed_doc;
}

static gboolean
nd_input_is_text_like(const nd_node *n)
{
    return nd_node_is_text_input(n);
}

static gboolean
nd_node_in_tree(const nd_node *root, const nd_node *needle)
{
    if (!root) return FALSE;
    if (root == needle) return TRUE;
    for (const nd_node *c = root->first_child; c; c = c->next_sibling)
        if (nd_node_in_tree(c, needle)) return TRUE;
    return FALSE;
}

static gboolean
nd_window_focused_input_live(nd_window *w)
{
    if (!w->focused_input) return FALSE;
    if (nd_element_effectively_inert(w->focused_input) ||
        nd_element_effectively_disabled(w->focused_input)) {
        nd_window_set_focused_input(w, NULL);
        return FALSE;
    }
    if (w->parsed_doc && !nd_node_in_tree(w->parsed_doc, w->focused_input)) {
        w->focused_input = NULL;
        return FALSE;
    }
    return TRUE;
}

static const char *
nd_input_current_value(const nd_node *n)
{
    return nd_node_editable_value(n);
}

static void
nd_input_set_value(nd_node *n, const char *value)
{
    nd_node_set_editable_value(n, value);
}

typedef struct nd_refresh_ctx {
    nd_window *w;
    char *url;
} nd_refresh_ctx;

static void
nd_refresh_ctx_free(gpointer data)
{
    nd_refresh_ctx *ctx = data;
    if (!ctx) return;
    g_free(ctx->url);
    g_free(ctx);
}

static gboolean
nd_window_refresh_fire(gpointer data)
{
    nd_refresh_ctx *ctx = data;
    if (ctx->w) {
        ctx->w->refresh_source = 0;
        if (ctx->url)
            nd_window_load_url(ctx->w, ctx->url, ND_LOAD_USER);
    }
    return G_SOURCE_REMOVE;
}

static inline gboolean
nd_refresh_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static gboolean
nd_parse_refresh(const char *s, guint *out_delay, char **out_raw_url)
{
    *out_delay = 0;
    *out_raw_url = NULL;
    if (!s) return FALSE;
    const char *p = s;
    while (nd_refresh_is_ws(*p)) p++;
    const char *digits = p;
    while (g_ascii_isdigit(*p)) p++;
    if (p == digits && *p != '.') return FALSE;
    guint delay = 0;
    for (const char *d = digits; d < p; d++) {
        delay = delay * 10 + (guint)(*d - '0');
        if (delay > 600) { delay = 600; break; }
    }
    while (g_ascii_isdigit(*p) || *p == '.') p++;
    while (nd_refresh_is_ws(*p)) p++;
    if (*p == ';' || *p == ',') {
        p++;
        while (nd_refresh_is_ws(*p)) p++;
    }
    if (*p) {
        if ((p[0] == 'u' || p[0] == 'U') &&
            (p[1] == 'r' || p[1] == 'R') &&
            (p[2] == 'l' || p[2] == 'L')) {
            const char *q = p + 3;
            while (nd_refresh_is_ws(*q)) q++;
            if (*q == '=') {
                q++;
                while (nd_refresh_is_ws(*q)) q++;
                p = q;
            }
        }
        char quote = 0;
        if (*p == '"' || *p == '\'') { quote = *p; p++; }
        const char *start = p;
        const char *end;
        if (quote) {
            end = p;
            while (*end && *end != quote) end++;
        } else {
            end = start + strlen(start);
            while (end > start && nd_refresh_is_ws(end[-1])) end--;
        }
        if (end > start)
            *out_raw_url = g_strndup(start, (gsize)(end - start));
    }
    *out_delay = delay;
    return TRUE;
}

static const char *
nd_window_meta_refresh_content(nd_window *w)
{
    if (!w->parsed_doc) return NULL;
    nd_node *head = nd_node_find_first_element(w->parsed_doc, "head");
    if (!head) return NULL;
    for (const nd_node *c = head->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "meta") != 0) continue;
        const char *equiv = nd_element_get_attr(c, "http-equiv");
        if (!equiv || g_ascii_strcasecmp(equiv, "refresh") != 0) continue;
        const char *content = nd_element_get_attr(c, "content");
        if (content && *content) return content;
    }
    return NULL;
}

static void
nd_window_apply_meta_refresh(nd_window *w, const nd_response *resp)
{
    const char *content = (resp && resp->refresh && *resp->refresh)
                              ? resp->refresh
                              : nd_window_meta_refresh_content(w);
    if (!content) return;

    guint delay = 0;
    char *raw = NULL;
    if (!nd_parse_refresh(content, &delay, &raw)) return;

    char *url = raw ? nd_resolve_url(w, raw)
                    : g_strdup(nd_window_current_url(w));
    g_free(raw);
    if (!url) return;

    g_clear_handle_id(&w->refresh_source, g_source_remove);
    nd_refresh_ctx *ctx = g_new0(nd_refresh_ctx, 1);
    ctx->w = w;
    ctx->url = url;
    w->refresh_source = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, delay,
                                                   nd_window_refresh_fire, ctx,
                                                   nd_refresh_ctx_free);
}

static gboolean
nd_window_caret_blink_tick(gpointer user_data)
{
    nd_window *w = user_data;
    if (!w->focused_input) {
        w->caret_blink_source = 0;
        nd_paint_set_caret_visible(TRUE);
        return G_SOURCE_REMOVE;
    }
    w->caret_blink_on = !w->caret_blink_on;
    nd_paint_set_caret_visible(w->caret_blink_on);
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    return G_SOURCE_CONTINUE;
}

static void
nd_window_reset_caret_blink(nd_window *w)
{
    w->caret_blink_on = TRUE;
    nd_paint_set_caret_visible(TRUE);
    g_clear_handle_id(&w->caret_blink_source, g_source_remove);
    if (w->focused_input)
        w->caret_blink_source = g_timeout_add(530, nd_window_caret_blink_tick, w);
}

static void nd_on_im_commit(GtkIMContext *im, const char *str, gpointer user_data);

static void
nd_window_ensure_im_context(nd_window *w)
{
    if (w->im_context || !w->drawing_area) return;
    w->im_context = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(w->im_context, w->drawing_area);
    g_signal_connect(w->im_context, "commit", G_CALLBACK(nd_on_im_commit), w);
}

static void
nd_window_input_replace(nd_window *w, gsize del_start, gsize del_end,
                        const char *insert, gsize insert_len)
{
    if (!nd_window_focused_input_live(w)) return;
    nd_node *target = w->focused_input;
    const char *cur = nd_input_current_value(target);
    gsize cur_len = strlen(cur);
    if (del_start > cur_len) del_start = cur_len;
    if (del_end   > cur_len) del_end   = cur_len;
    if (del_end < del_start) del_end = del_start;
    if (insert && insert_len && nd_form_control_length_limits_apply(target)) {
        const char *ml = nd_element_get_attr(target, "maxlength");
        if (ml && *ml) {
            long maxl = atol(ml);
            if (maxl >= 0) {
                glong kept = g_utf8_strlen(cur, (gssize)del_start) +
                             g_utf8_strlen(cur + del_end,
                                           (gssize)(cur_len - del_end));
                glong room = maxl - kept;
                if (room < 0) room = 0;
                if (g_utf8_strlen(insert, (gssize)insert_len) > room) {
                    const char *p = insert;
                    for (glong i = 0; i < room; i++) p = g_utf8_next_char(p);
                    insert_len = (gsize)(p - insert);
                    if (insert_len == 0) return;
                }
            }
        }
    }
    GString *s = g_string_sized_new(cur_len - (del_end - del_start) + insert_len);
    g_string_append_len(s, cur, (gssize)del_start);
    if (insert && insert_len) g_string_append_len(s, insert, (gssize)insert_len);
    g_string_append_len(s, cur + del_end, (gssize)(cur_len - del_end));
    if (w->js) {
        gboolean prevented = FALSE;
        nd_js_dispatch_event(w->js, target, "beforeinput", &prevented);
        if (prevented) { g_string_free(s, TRUE); return; }
        if (w->focused_input != target || !nd_window_focused_input_live(w)) {
            g_string_free(s, TRUE);
            return;
        }
    }
    nd_input_set_value(target, s->str);
    w->caret_byte = del_start + insert_len;
    w->sel_anchor_byte = w->caret_byte;
    g_string_free(s, TRUE);
    if (w->js) {
        nd_js_dispatch_event(w->js, target, "input", NULL);
        (void)nd_js_consume_mutated(w->js);
    }
    nd_window_reset_caret_blink(w);
    g_clear_handle_id(&w->js_relayout_idle_id, g_source_remove);
    nd_window_js_relayout_now(w);
}

static void
nd_on_im_commit(GtkIMContext *im, const char *str, gpointer user_data)
{
    (void)im;
    nd_window *w = user_data;
    if (!w->focused_input || !str || !*str) return;
    nd_window_input_replace(w, w->caret_byte, w->caret_byte, str, strlen(str));
}

static void
nd_on_paste_text(GObject *source, GAsyncResult *result, gpointer user_data)
{
    nd_window *w = user_data;
    GError *err = NULL;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &err);
    if (text && nd_window_focused_input_live(w)) {
        gboolean is_multiline =
            (w->focused_input->name &&
             strcmp(w->focused_input->name, "textarea") == 0) ||
            nd_node_is_contenteditable_host(w->focused_input);
        if (!is_multiline) {
            for (char *p = text; *p; p++)
                if (*p == '\n' || *p == '\r') *p = ' ';
        }
        nd_window_input_replace(w, w->caret_byte, w->caret_byte, text, strlen(text));
    }
    g_clear_error(&err);
    g_free(text);
}

static void
nd_window_input_paste(nd_window *w)
{
    if (!w->focused_input || !w->drawing_area) return;
    GdkClipboard *cb = gtk_widget_get_clipboard(w->drawing_area);
    gdk_clipboard_read_text_async(cb, NULL, nd_on_paste_text, w);
}

static void
nd_window_set_focused_input(nd_window *w, nd_node *target)
{
    if (w->focused_input == target) return;
    if (target && (nd_element_effectively_disabled(target) ||
                   nd_element_effectively_inert(target)))
        target = NULL;
    if (w->layout_tree) { if (w->js) nd_js_set_layout_root(w->js, NULL); nd_box_free(w->layout_tree); w->layout_tree = NULL; nd_selection_clear(&w->selection); w->search_active_box = NULL; }
    w->layout_dirty = TRUE;
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    if (w->focused_input) {
        nd_node *old = w->focused_input;
        gboolean old_live = !w->parsed_doc || nd_node_in_tree(w->parsed_doc, old);
        if (w->im_context) {
            gtk_im_context_reset(w->im_context);
            gtk_im_context_focus_out(w->im_context);
        }
        if (w->js && old_live) {
            const char *cur = nd_input_current_value(old);
            if (!nd_node_is_contenteditable_host(old) &&
                w->focused_input_initial &&
                (!cur || strcmp(cur, w->focused_input_initial) != 0))
                nd_js_dispatch_event(w->js, old, "change", NULL);
            nd_js_dispatch_event(w->js, old, "blur",     NULL);
            nd_js_dispatch_event(w->js, old, "focusout", NULL);
        }
        g_free(w->focused_input_initial);
        w->focused_input_initial = NULL;
    }
    w->focused_input = target;
    w->caret_byte = 0;
    g_clear_handle_id(&w->caret_blink_source, g_source_remove);
    nd_paint_set_caret_visible(TRUE);
    if (target) {
        nd_window_ensure_im_context(w);
        nd_node_flatten_editable(target);
        w->focused_input_initial = g_strdup(nd_input_current_value(target));
        w->caret_byte = w->focused_input_initial ? strlen(w->focused_input_initial) : 0;
        w->sel_anchor_byte = w->caret_byte;
        w->caret_blink_on = TRUE;
        w->caret_blink_source = g_timeout_add(530, nd_window_caret_blink_tick, w);
        if (w->im_context) gtk_im_context_focus_in(w->im_context);
        if (w->js) {
            nd_js_dispatch_event(w->js, target, "focus",   NULL);
            nd_js_dispatch_event(w->js, target, "focusin", NULL);
        }
    }
    if (w->js) nd_js_set_focused_node(w->js, target);
}

static gboolean
nd_window_handle_input_key(nd_window *w, guint keyval, GdkModifierType state)
{
    if (!nd_window_focused_input_live(w)) return FALSE;
    nd_node *target = w->focused_input;
    const char *cur = nd_input_current_value(target);
    gsize cur_len = strlen(cur);
    if (w->caret_byte > cur_len) w->caret_byte = cur_len;
    if (w->sel_anchor_byte > cur_len) w->sel_anchor_byte = cur_len;

    gboolean ctrl  = (state & GDK_CONTROL_MASK) != 0;
    gboolean alt   = (state & GDK_ALT_MASK)     != 0;
    gboolean meta  = (state & GDK_META_MASK)    != 0;
    gboolean shift = (state & GDK_SHIFT_MASK)   != 0;
    gboolean is_textarea = target->name && strcmp(target->name, "textarea") == 0;
    gboolean is_multiline = is_textarea || nd_node_is_contenteditable_host(target);

    gsize sel_lo = w->sel_anchor_byte < w->caret_byte
                   ? w->sel_anchor_byte : w->caret_byte;
    gsize sel_hi = w->sel_anchor_byte < w->caret_byte
                   ? w->caret_byte : w->sel_anchor_byte;
    gboolean has_sel = sel_lo != sel_hi;

    if (alt || meta) return FALSE;

    if (ctrl) {
        if (keyval == GDK_KEY_v || keyval == GDK_KEY_V) {
            nd_window_input_paste(w);
            return TRUE;
        }
        if (keyval == GDK_KEY_c || keyval == GDK_KEY_C ||
            keyval == GDK_KEY_x || keyval == GDK_KEY_X) {
            if (has_sel && w->drawing_area) {
                char *sub = g_strndup(cur + sel_lo, sel_hi - sel_lo);
                GdkClipboard *cb = gtk_widget_get_clipboard(w->drawing_area);
                gdk_clipboard_set_text(cb, sub);
                g_free(sub);
                if (keyval == GDK_KEY_x || keyval == GDK_KEY_X)
                    nd_window_input_replace(w, sel_lo, sel_hi, NULL, 0);
            }
            return TRUE;
        }
        if (keyval == GDK_KEY_a || keyval == GDK_KEY_A) {
            w->sel_anchor_byte = 0;
            w->caret_byte = cur_len;
            nd_window_reset_caret_blink(w);
            nd_window_js_mutated(w);
            return TRUE;
        }
        if (keyval == GDK_KEY_Left) {
            w->caret_byte = 0;
            if (!shift) w->sel_anchor_byte = w->caret_byte;
            nd_window_reset_caret_blink(w);
            nd_window_js_mutated(w);
            return TRUE;
        }
        if (keyval == GDK_KEY_Right) {
            w->caret_byte = cur_len;
            if (!shift) w->sel_anchor_byte = w->caret_byte;
            nd_window_reset_caret_blink(w);
            nd_window_js_mutated(w);
            return TRUE;
        }
        return FALSE;
    }

    if (keyval == GDK_KEY_Escape) {
        nd_window_set_focused_input(w, NULL);
        if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (is_multiline) {
            nd_window_input_replace(w, sel_lo, sel_hi, "\n", 1);
            return TRUE;
        }
        nd_node *submit_target = target;
        nd_window_set_focused_input(w, NULL);
        nd_window_maybe_submit_form(w, submit_target);
        return TRUE;
    }
    if (keyval == GDK_KEY_BackSpace) {
        if (has_sel) {
            nd_window_input_replace(w, sel_lo, sel_hi, NULL, 0);
            return TRUE;
        }
        if (w->caret_byte == 0) return TRUE;
        const char *prev = g_utf8_prev_char(cur + w->caret_byte);
        nd_window_input_replace(w, (gsize)(prev - cur), w->caret_byte, NULL, 0);
        return TRUE;
    }
    if (keyval == GDK_KEY_Delete) {
        if (has_sel) {
            nd_window_input_replace(w, sel_lo, sel_hi, NULL, 0);
            return TRUE;
        }
        if (w->caret_byte >= cur_len) return TRUE;
        const char *nxt = g_utf8_next_char(cur + w->caret_byte);
        nd_window_input_replace(w, w->caret_byte, (gsize)(nxt - cur), NULL, 0);
        return TRUE;
    }
    if (keyval == GDK_KEY_Left) {
        if (has_sel && !shift) {
            w->caret_byte = sel_lo;
        } else if (w->caret_byte > 0) {
            const char *prev = g_utf8_prev_char(cur + w->caret_byte);
            w->caret_byte = (gsize)(prev - cur);
        }
        if (!shift) w->sel_anchor_byte = w->caret_byte;
        nd_window_reset_caret_blink(w);
        nd_window_js_mutated(w);
        return TRUE;
    }
    if (keyval == GDK_KEY_Right) {
        if (has_sel && !shift) {
            w->caret_byte = sel_hi;
        } else if (w->caret_byte < cur_len) {
            const char *nxt = g_utf8_next_char(cur + w->caret_byte);
            w->caret_byte = (gsize)(nxt - cur);
        }
        if (!shift) w->sel_anchor_byte = w->caret_byte;
        nd_window_reset_caret_blink(w);
        nd_window_js_mutated(w);
        return TRUE;
    }
    if (keyval == GDK_KEY_Home) {
        w->caret_byte = 0;
        if (!shift) w->sel_anchor_byte = w->caret_byte;
        nd_window_reset_caret_blink(w);
        nd_window_js_mutated(w);
        return TRUE;
    }
    if (keyval == GDK_KEY_End) {
        w->caret_byte = cur_len;
        if (!shift) w->sel_anchor_byte = w->caret_byte;
        nd_window_reset_caret_blink(w);
        nd_window_js_mutated(w);
        return TRUE;
    }
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
        const char *itype = target->name &&
            strcmp(target->name, "input") == 0
            ? nd_element_get_attr(target, "type") : NULL;
        if (itype && g_ascii_strcasecmp(itype, "number") == 0) {
            const char *sv = nd_element_get_attr(target, "step");
            double step = sv && *sv ? g_ascii_strtod(sv, NULL) : 1.0;
            if (!(step > 0)) step = 1.0;
            double val = *cur ? g_ascii_strtod(cur, NULL) : 0.0;
            val += (keyval == GDK_KEY_Up) ? step : -step;
            const char *mn = nd_element_get_attr(target, "min");
            const char *mx = nd_element_get_attr(target, "max");
            if (mn && *mn) { double m = g_ascii_strtod(mn, NULL); if (val < m) val = m; }
            if (mx && *mx) { double m = g_ascii_strtod(mx, NULL); if (val > m) val = m; }
            char buf[32];
            g_snprintf(buf, sizeof buf, "%g", val);
            nd_input_set_value(target, buf);
            w->caret_byte = strlen(buf);
            w->sel_anchor_byte = w->caret_byte;
            if (w->js) {
                nd_js_dispatch_event(w->js, target, "input",  NULL);
                nd_js_dispatch_event(w->js, target, "change", NULL);
                (void)nd_js_consume_mutated(w->js);
            }
            nd_window_reset_caret_blink(w);
            g_clear_handle_id(&w->js_relayout_idle_id, g_source_remove);
            nd_window_js_relayout_now(w);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
nd_keyval_is_action(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter:
    case GDK_KEY_Escape:
    case GDK_KEY_Tab:    case GDK_KEY_ISO_Left_Tab:
    case GDK_KEY_Up:     case GDK_KEY_Down:
    case GDK_KEY_Left:   case GDK_KEY_Right:
    case GDK_KEY_Home:   case GDK_KEY_End:
    case GDK_KEY_Page_Up: case GDK_KEY_Page_Down:
    case GDK_KEY_BackSpace: case GDK_KEY_Delete:
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean
nd_im_has_preedit(GtkIMContext *im)
{
    if (!im) return FALSE;
    char *str = NULL;
    gtk_im_context_get_preedit_string(im, &str, NULL, NULL);
    gboolean has = str && *str;
    g_free(str);
    return has;
}

static gboolean
nd_dispatch_key_event_common(nd_window *w, const char *type, guint keyval,
                             GdkModifierType state, GdkEvent *event)
{
    if (strcmp(type, "keydown") == 0 && w->focused_input) {
        nd_window_ensure_im_context(w);
        gboolean action_key = nd_keyval_is_action(keyval);
        gboolean composing  = nd_im_has_preedit(w->im_context);
        if (!(action_key && !composing) && w->im_context && event &&
            gtk_im_context_filter_keypress(w->im_context, event))
            return TRUE;
        if (nd_window_handle_input_key(w, keyval, state))
            return TRUE;
    }
    if (!w->js) return FALSE;
    const nd_node *target = w->focused_input ? w->focused_input
                                             : nd_window_key_target(w);
    if (!target) return FALSE;
    const char *name = gdk_keyval_name(keyval);
    char key_buf[8] = {0};
    gunichar uc = gdk_keyval_to_unicode(keyval);
    const char *key;
    if (uc >= 0x20 && uc != 0x7f) {
        int len = g_unichar_to_utf8(uc, key_buf);
        key_buf[len] = 0;
        key = key_buf;
    } else {
        key = name ? name : "";
    }
    if (name) {
        if (strcmp(name, "Up") == 0)         key = "ArrowUp";
        else if (strcmp(name, "Down") == 0)  key = "ArrowDown";
        else if (strcmp(name, "Left") == 0)  key = "ArrowLeft";
        else if (strcmp(name, "Right") == 0) key = "ArrowRight";
        else if (strcmp(name, "Return") == 0 ||
                 strcmp(name, "KP_Enter") == 0) key = "Enter";
        else if (strcmp(name, "Escape") == 0) key = "Escape";
        else if (strcmp(name, "BackSpace") == 0) key = "Backspace";
        else if (strcmp(name, "Tab") == 0)    key = "Tab";
        else if (strcmp(name, "Page_Up") == 0)   key = "PageUp";
        else if (strcmp(name, "Page_Down") == 0) key = "PageDown";
        else if (strcmp(name, "Home") == 0)   key = "Home";
        else if (strcmp(name, "End") == 0)    key = "End";
        else if (strcmp(name, "Delete") == 0) key = "Delete";
        else if (strcmp(name, "Insert") == 0) key = "Insert";
    }
    gboolean prevented = FALSE;
    nd_js_dispatch_key_event(w->js, target, type, key, name ? name : "",
                             (int)keyval,
                             (state & GDK_SHIFT_MASK)   != 0,
                             (state & GDK_CONTROL_MASK) != 0,
                             (state & GDK_ALT_MASK)     != 0,
                             (state & GDK_META_MASK)    != 0,
                             &prevented);
    if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
    return prevented;
}

gboolean
nd_on_drawing_key_pressed(GtkEventControllerKey *c, guint keyval, guint keycode,
                          GdkModifierType state, gpointer user_data)
{
    (void)keycode;
    nd_window *w = user_data;
    if (keyval == GDK_KEY_Escape && w->js &&
        nd_js_close_topmost_modal_dialog(w->js)) {
        if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
        if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
        return TRUE;
    }
    if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) &&
        !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) && w->js) {
        gboolean backward = (state & GDK_SHIFT_MASK) != 0 ||
                            keyval == GDK_KEY_ISO_Left_Tab;
        const nd_node *target = nd_js_sequential_focus_target(w->js, backward);
        if (target) {
            if (nd_node_is_editable(target))
                nd_window_set_focused_input(w, (nd_node *)target);
            else {
                nd_window_set_focused_input(w, NULL);
                nd_js_set_focus(w->js, target);
            }
            if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
            if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
            return TRUE;
        }
    }
    if ((state & GDK_CONTROL_MASK) && !w->focused_input) {
        if (keyval == GDK_KEY_c || keyval == GDK_KEY_C) {
            char *text = nd_selection_collect_text(w->layout_tree, &w->selection);
            if (text && *text) {
                GdkClipboard *cb = gtk_widget_get_clipboard(w->drawing_area);
                gdk_clipboard_set_text(cb, text);
                nd_window_set_status(w, "Copied %d characters",
                                     (int)g_utf8_strlen(text, -1));
                g_free(text);
                return TRUE;
            }
            g_free(text);
        } else if (keyval == GDK_KEY_a || keyval == GDK_KEY_A) {
            if (w->layout_tree &&
                nd_selection_select_all(&w->selection, w->layout_tree)) {
                nd_window_sync_selection_to_js(w);
                if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
                return TRUE;
            }
        }
    }
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(c));
    gboolean prevented = nd_dispatch_key_event_common(user_data, "keydown", keyval, state, event);
    if (prevented) return TRUE;
    if (!w->focused_input && w->render_vadj) {
        double page = gtk_adjustment_get_page_size(w->render_vadj);
        double step = gtk_adjustment_get_step_increment(w->render_vadj);
        if (step <= 0) step = 40;
        double cur = gtk_adjustment_get_value(w->render_vadj);
        double lo  = gtk_adjustment_get_lower(w->render_vadj);
        double hi  = gtk_adjustment_get_upper(w->render_vadj);
        double max_value = hi - page;
        if (max_value < lo) max_value = lo;
        double target = cur;
        switch (keyval) {
        case GDK_KEY_Page_Down: target = cur + page * 0.9; break;
        case GDK_KEY_Page_Up:   target = cur - page * 0.9; break;
        case GDK_KEY_space:
            if (!(state & GDK_SHIFT_MASK)) target = cur + page * 0.9;
            else                            target = cur - page * 0.9;
            break;
        case GDK_KEY_End:
            if (!(state & (GDK_CONTROL_MASK))) target = max_value;
            else                                target = max_value;
            break;
        case GDK_KEY_Home:
            target = lo;
            break;
        case GDK_KEY_Up:
            if (!(state & GDK_CONTROL_MASK)) target = cur - step * 3;
            break;
        case GDK_KEY_Down:
            if (!(state & GDK_CONTROL_MASK)) target = cur + step * 3;
            break;
        default: return FALSE;
        }
        if (target < lo) target = lo;
        if (target > max_value) target = max_value;
        if (target != cur) {
            gtk_adjustment_set_value(w->render_vadj, target);
            return TRUE;
        }
    }
    return FALSE;
}

void
nd_on_drawing_key_released(GtkEventControllerKey *c, guint keyval, guint keycode,
                           GdkModifierType state, gpointer user_data)
{
    (void)keycode;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(c));
    nd_dispatch_key_event_common(user_data, "keyup", keyval, state, event);
}

gboolean
nd_on_drawing_scroll(GtkEventControllerScroll *c, double dx, double dy,
                     gpointer user_data)
{
    (void)c;
    nd_window *w = user_data;
    if (!w->layout_tree || !w->drawing_area) return FALSE;
    nd_box *target = nd_box_hit_scrollable(w->layout_tree,
                                           w->cursor_x, w->cursor_y);
    if (!target) return FALSE;
    double step = 40.0;
    double new_x = target->scroll_x + dx * step;
    double new_y = target->scroll_y + dy * step;
    if (new_x < 0) new_x = 0;
    if (new_x > target->scroll_max_x) new_x = target->scroll_max_x;
    if (new_y < 0) new_y = 0;
    if (new_y > target->scroll_max_y) new_y = target->scroll_max_y;
    gboolean changed_x = (new_x != target->scroll_x);
    gboolean changed_y = (new_y != target->scroll_y);
    if (!changed_x && !changed_y) return FALSE;
    target->scroll_x = new_x;
    target->scroll_y = new_y;
    gtk_widget_queue_draw(w->drawing_area);
    return TRUE;
}

void
nd_on_drawing_pressed_middle(GtkGestureClick *gesture, int n_press,
                             double x, double y, gpointer user_data)
{
    (void)gesture; (void)n_press;
    nd_window *w = user_data;
    if (!w->layout_tree) return;
    const char *href = nd_box_hit_link(w->layout_tree, x, y);
    if (!href) return;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
    char *abs_url = nd_resolve_url(w, href);
    if (abs_url) {
        nd_spawn_window(app, abs_url);
        g_free(abs_url);
    }
}

void
nd_on_drawing_drag_begin(GtkGestureDrag *gesture, double x, double y,
                         gpointer user_data)
{
    (void)gesture;
    nd_window *w = user_data;
    w->drag_start_x = x;
    w->drag_start_y = y;
    if (!w->layout_tree) return;
    if (nd_box_hit_link_range(w->layout_tree, x, y)) {
        nd_selection_clear(&w->selection);
        nd_window_sync_selection_to_js(w);
        if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
        return;
    }
    nd_selection_anchor_at(&w->selection, w->layout_tree, x, y);
    nd_window_sync_selection_to_js(w);
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
}

void
nd_on_drawing_drag_update(GtkGestureDrag *gesture, double dx, double dy,
                          gpointer user_data)
{
    (void)gesture;
    nd_window *w = user_data;
    if (!w->layout_tree || !w->selection.active) return;
    nd_selection_extend_to(&w->selection, w->layout_tree,
                           w->drag_start_x + dx, w->drag_start_y + dy);
    nd_window_sync_selection_to_js(w);
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
}

void
nd_on_drawing_drag_end(GtkGestureDrag *gesture, double dx, double dy,
                       gpointer user_data)
{
    (void)gesture;
    nd_window *w = user_data;
    if (!w->layout_tree || !w->selection.active) return;
    if (fabs(dx) < 2 && fabs(dy) < 2) {
        nd_selection_clear(&w->selection);
    } else {
        nd_selection_extend_to(&w->selection, w->layout_tree,
                               w->drag_start_x + dx, w->drag_start_y + dy);
    }
    nd_window_sync_selection_to_js(w);
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
}

void
nd_draw_render(GtkDrawingArea *area, cairo_t *cr,
               int width, int height, gpointer user_data)
{
    (void)area;
    (void)height;
    nd_window *w = user_data;
    cairo_set_source_rgb(cr, 0.94, 0.94, 0.95);
    cairo_paint(cr);
    if (w->pdf) {
        double total_h = 0;
        nd_pdf_paint(w->pdf, cr, (double)width, &total_h);
        int h_req = (int)(total_h + 0.5);
        if (h_req > height) gtk_widget_set_size_request(w->drawing_area, -1, h_req);
        return;
    }
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (!w->last_body || !is_html_content_type(w->last_content_type))
        return;
    if (!w->first_paint_done && w->css_inflight > 0)
        return;
    double vw = (double)width;
    GtkWidget *sw = gtk_widget_get_ancestor(w->drawing_area,
                                            GTK_TYPE_SCROLLED_WINDOW);
    if (sw) {
        GtkAdjustment *ha =
            gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(sw));
        double page = ha ? gtk_adjustment_get_page_size(ha) : 0;
        if (page > 0) vw = page;
        else { int sww = gtk_widget_get_width(sw); if (sww > 0) vw = (double)sww; }
    }
    nd_window_ensure_layout(w, vw);
    if (!w->layout_tree) return;
    nd_paint_set_js(w->js);
    nd_paint_set_anim(w->anim);
    gboolean profile = nd_profile_enabled();
    gint64 t_paint = profile ? g_get_monotonic_time() : 0;
    nd_paint_with_selection(cr, w->layout_tree, w->search_query, &w->selection);
    gint64 paint_us = g_get_monotonic_time() - t_paint;
    nd_debug_log_emit(ND_DLOG_RENDER, "paint",
                      "vp=%d total=%.1fms%s",
                      width, paint_us / 1000.0,
                      w->first_paint_done ? "" : " (first)");
    if (profile)
        g_printerr("[profile] paint vp=%d total=%.1fms\n",
                   width, paint_us / 1000.0);
    w->first_paint_done = TRUE;
}

char *
nd_resolve_url(const nd_window *w, const char *href)
{
    if (!href || !*href) return NULL;
    if (w->cursor < 0 || w->cursor >= (int)w->history->len)
        return nd_url_resolve(NULL, href);
    const char *base = g_ptr_array_index(w->history, w->cursor);
    g_autofree char *resolved_base = NULL;
    if (w->parsed_doc) {
        nd_node *base_el = nd_node_find_first_element(w->parsed_doc, "base");
        if (base_el) {
            const char *b = nd_element_get_attr(base_el, "href");
            if (b && *b) {
                resolved_base = nd_url_resolve(base, b);
                if (resolved_base && nd_url_is_http_or_https(resolved_base))
                    base = resolved_base;
            }
        }
    }
    return nd_url_resolve(base, href);
}

static gboolean
nd_window_image_ready_needs_relayout(nd_window *w, nd_image *img)
{
    if (!w->layout_tree || !img) return TRUE;
    GPtrArray *imgs = g_ptr_array_new();
    nd_layout_collect_images(w->layout_tree, imgs);
    gboolean any_ref = FALSE;
    gboolean needs = FALSE;
    for (guint i = 0; i < imgs->len && !needs; i++) {
        nd_box *box = g_ptr_array_index(imgs, i);
        if (!box->media) continue;
        if (box->media->image == (void *)img) {
            any_ref = TRUE;
            if (!box->media->size_independent_of_image) needs = TRUE;
        }
        if (box->media->bg_image == (void *)img)
            any_ref = TRUE;
    }
    g_ptr_array_free(imgs, TRUE);
    return needs || !any_ref;
}

static void
on_image_ready(nd_image *img, gpointer user_data)
{
    nd_window *w = user_data;
    if (img && img->failed && img->url) {
        char *line = g_strdup_printf("[error] image: %s — %s",
                                     img->url,
                                     img->error ? img->error : "failed");
        nd_window_console_append(w, line);
        nd_debug_log_emit(ND_DLOG_ERROR, "image", "%s: %s",
                          img->url, img->error ? img->error : "failed");
        g_free(line);
    } else if (img && img->url) {
        nd_debug_log_emit(ND_DLOG_RENDER, "image",
                          "ready %dx%d %s",
                          img->natural_width, img->natural_height, img->url);
    }
    if (img && img->failed)
        nd_window_schedule_image_retry(w, img);
    if (w->mode != ND_VIEW_RENDER || !w->drawing_area) return;
    if (nd_window_image_ready_needs_relayout(w, img)) {
        if (!w->js_relayout_idle_id)
            w->js_relayout_idle_id =
                g_timeout_add(50, nd_window_js_relayout_now, w);
    } else {
        gtk_widget_queue_draw(w->drawing_area);
    }
}

static void
on_video_ready(nd_video *v, gpointer user_data)
{
    (void)v;
    nd_window *w = user_data;
    if (w->mode == ND_VIEW_RENDER && w->drawing_area)
        gtk_widget_queue_draw(w->drawing_area);
}

typedef struct nd_css_fetch {
    guint      w_id;
    char      *url;
    char      *integrity;
    char      *scope_id;
} nd_css_fetch;

static void
on_external_css_loaded(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_css_fetch *fetch = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    nd_window *w = nd_window_for_id(fetch->w_id);
    if (!w) {
        g_clear_error(&err);
        nd_response_free(resp);
        g_free(fetch->url);
        g_free(fetch->integrity);
        g_free(fetch->scope_id);
        g_free(fetch);
        return;
    }
    if (w->css_inflight > 0) w->css_inflight--;
    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            nd_window_set_status(w, "CSS fetch failed: %s", err->message);
            char *line = g_strdup_printf("[error] stylesheet: %s — %s",
                                         fetch->url, err->message);
            nd_window_console_append(w, line);
            g_free(line);
        }
        g_clear_error(&err);
        nd_response_free(resp);
        g_free(fetch->url);
        g_free(fetch->integrity);
        g_free(fetch->scope_id);
        g_free(fetch);
        goto maybe_paint;
    }
    if (!resp) {
        g_free(fetch->url);
        g_free(fetch->integrity);
        g_free(fetch->scope_id);
        g_free(fetch);
        goto maybe_paint;
    }
    if (resp->error) {
        char *line = g_strdup_printf("[error] stylesheet: %s — %s",
                                     fetch->url, resp->error);
        nd_window_console_append(w, line);
        g_free(line);
    } else if (resp->status >= 400) {
        char *line = g_strdup_printf("[error] stylesheet: %s — HTTP %ld",
                                     fetch->url, resp->status);
        nd_window_console_append(w, line);
        g_free(line);
    }
    if (resp->body && resp->body->len > 0 && w && w->external_stylesheets &&
        !nd_security_sri_check(fetch->integrity, resp->body->data, resp->body->len)) {
        char *line = g_strdup_printf(
            "SRI mismatch: stylesheet %s (integrity=\"%s\")",
            fetch->url, fetch->integrity);
        nd_window_console_append(w, line);
        g_free(line);
    } else if (resp->body && resp->body->len > 0 && w && w->external_stylesheets) {
        char *scoped = fetch->scope_id
            ? nd_css_scope_css((const char *)resp->body->data,
                                (gssize)resp->body->len, fetch->scope_id)
            : NULL;
        nd_css_stylesheet *sh = scoped
            ? nd_css_stylesheet_parse(scoped, (gssize)strlen(scoped))
            : nd_css_stylesheet_parse((const char *)resp->body->data,
                                      (gssize)resp->body->len);
        g_free(scoped);
        if (sh) {
            GHashTable *seen =
                g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            g_hash_table_add(seen, g_strdup(fetch->url));
            nd_window_append_stylesheet_expanded(w, w->external_stylesheets,
                                                 sh, fetch->url, seen, 0);
            g_hash_table_destroy(seen);
            nd_window_mark_layout_dirty(w);
        }
    }
    nd_response_free(resp);
    g_free(fetch->url);
    g_free(fetch->integrity);
    g_free(fetch->scope_id);
    g_free(fetch);
maybe_paint:
    if (w->css_inflight == 0 && !w->first_paint_done)
        w->first_paint_done = TRUE;
    if (w->css_inflight == 0 && w->drawing_area)
        gtk_widget_queue_draw(w->drawing_area);
}

static char *
extract_attr_value(const char *tag, const char *end, const char *name)
{
    gsize nlen = strlen(name);
    const char *p = tag;
    while (p + nlen < end) {
        if (g_ascii_strncasecmp(p, name, nlen) == 0) {
            const char *after = p + nlen;
            while (after < end && (*after == ' ' || *after == '\t' ||
                                    *after == '\r' || *after == '\n'))
                after++;
            if (after < end && *after == '=') {
                after++;
                while (after < end && (*after == ' ' || *after == '\t' ||
                                        *after == '\r' || *after == '\n'))
                    after++;
                if (after >= end) return NULL;
                char quote = 0;
                if (*after == '"' || *after == '\'') { quote = *after; after++; }
                const char *start = after;
                while (after < end) {
                    if (quote && *after == quote) break;
                    if (!quote && (*after == ' ' || *after == '\t' ||
                                   *after == '>' || *after == '/' ||
                                   *after == '\r' || *after == '\n')) break;
                    after++;
                }
                return g_strndup(start, (gsize)(after - start));
            }
        }
        p++;
    }
    return NULL;
}

static void
nd_window_preload_stylesheets(nd_window *w, const char *html, gsize len)
{
    if (!html || len == 0) return;
    if (!w->external_stylesheets) {
        w->external_stylesheets = g_ptr_array_new();
        w->external_css_seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                      g_free, NULL);
    }
    if (!w->css_cancellable) w->css_cancellable = g_cancellable_new();

    const char *p = html;
    const char *end = html + len;
    while (p < end) {
        const char *lt = memchr(p, '<', (gsize)(end - p));
        if (!lt) break;
        if (lt + 5 < end && g_ascii_strncasecmp(lt, "<link", 5) == 0 &&
            (lt[5] == ' ' || lt[5] == '\t' || lt[5] == '\r' ||
             lt[5] == '\n' || lt[5] == '/' || lt[5] == '>')) {
            const char *gt = memchr(lt, '>', (gsize)(end - lt));
            if (!gt) break;
            char *rel  = extract_attr_value(lt + 5, gt, "rel");
            char *href = extract_attr_value(lt + 5, gt, "href");
            char *integrity = extract_attr_value(lt + 5, gt, "integrity");
            if (rel && href && *href) {
                gboolean is_sheet = FALSE;
                gchar **tokens = g_strsplit_set(rel, " \t\r\n", -1);
                for (int i = 0; tokens[i]; i++) {
                    if (g_ascii_strcasecmp(tokens[i], "stylesheet") == 0) {
                        is_sheet = TRUE; break;
                    }
                }
                g_strfreev(tokens);
                if (is_sheet) {
                    char *abs = nd_resolve_url(w, href);
                    if (abs && !nd_window_subresource_blocked(
                                    w, abs, ND_CSP_STYLE, "stylesheet") &&
                        !g_hash_table_contains(w->external_css_seen, abs)) {
                        g_hash_table_add(w->external_css_seen, g_strdup(abs));
                        nd_css_fetch *fetch = g_new0(nd_css_fetch, 1);
                        fetch->w_id = w->id;
                        fetch->url = abs;
                        fetch->integrity = integrity ? g_strdup(integrity) : NULL;
                        w->css_inflight++;
                        nd_net_fetch_async(abs, nd_window_current_url(w), w->css_cancellable,
                                           on_external_css_loaded, fetch);
                    } else {
                        g_free(abs);
                    }
                }
            }
            g_free(rel);
            g_free(href);
            g_free(integrity);
            p = gt + 1;
            continue;
        }
        p = lt + 1;
    }
}

static void
nd_window_kick_stylesheet_loads(nd_window *w)
{
    if (!w->parsed_doc) return;
    if (!w->external_stylesheets) {
        w->external_stylesheets = g_ptr_array_new();
        w->external_css_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    if (!w->css_cancellable) w->css_cancellable = g_cancellable_new();

    GQueue queue = G_QUEUE_INIT;
    g_queue_push_tail(&queue, w->parsed_doc);
    while (!g_queue_is_empty(&queue)) {
        nd_node *n = g_queue_pop_head(&queue);
        if (nd_node_is_element_named(n, "link")) {
            const char *rel = nd_element_get_attr(n, "rel");
            const char *href = nd_element_get_attr(n, "href");
            const char *media = nd_element_get_attr(n, "media");
            if (rel && href && *href &&
                g_ascii_strcasecmp(rel, "stylesheet") == 0 &&
                (!media || !*media || nd_css_media_query_matches(media))) {
                char *abs = nd_resolve_url(w, href);
                if (abs && nd_window_subresource_blocked(w, abs, ND_CSP_STYLE, "stylesheet")) {
                    g_free(abs);
                    continue;
                }
                if (abs && !g_hash_table_contains(w->external_css_seen, abs)) {
                    g_hash_table_add(w->external_css_seen, g_strdup(abs));
                    const char *integrity = nd_element_get_attr(n, "integrity");
                    nd_css_fetch *fetch = g_new0(nd_css_fetch, 1);
                    fetch->w_id = w->id;
                    fetch->url = abs;
                    fetch->integrity = integrity ? g_strdup(integrity) : NULL;
                    fetch->scope_id = nd_css_assign_iframe_scope(n);
                    w->css_inflight++;
                    nd_net_fetch_async(abs, nd_window_current_url(w), w->css_cancellable,
                                       on_external_css_loaded, fetch);
                    continue;
                }
                g_free(abs);
            }
        }
        for (nd_node *c = n->first_child; c; c = c->next_sibling)
            g_queue_push_tail(&queue, c);
    }
}

static gboolean
mixed_content_blocked(nd_window *w, const char *abs_url, const char *kind)
{
    const char *page = nd_window_current_url(w);
    if (!page || !g_str_has_prefix(page, "https://")) return FALSE;
    if (!g_str_has_prefix(abs_url, "http://")) return FALSE;
    g_warning("mixed-content blocked: %s %s on https page", kind, abs_url);
    return TRUE;
}

static gboolean
nd_window_subresource_blocked(nd_window *w, const char *abs_url,
                              nd_csp_kind csp_kind, const char *kind_word)
{
    return mixed_content_blocked(w, abs_url, kind_word) ||
           csp_blocked(w, csp_kind, abs_url, kind_word);
}

static gboolean
csp_blocked(nd_window *w, nd_csp_kind kind, const char *abs_url,
            const char *kind_word)
{
    if (!w->csp) return FALSE;
    if (abs_url && g_str_has_prefix(abs_url, "nd-inline-svg:")) return FALSE;
    if (nd_csp_allows(w->csp, kind, abs_url, nd_window_current_url(w)))
        return FALSE;
    g_warning("CSP blocked: %s %s", kind_word, abs_url);
    return TRUE;
}

static const char *
nd_link_rel_icon_kind(const char *rel)
{
    if (!rel) return NULL;
    gchar **toks = g_strsplit_set(rel, " \t\n\r\f", -1);
    const char *match = NULL;
    for (gchar **p = toks; *p; p++) {
        if (!**p) continue;
        if (g_ascii_strcasecmp(*p, "icon") == 0 ||
            g_ascii_strcasecmp(*p, "shortcut") == 0 ||
            g_ascii_strcasecmp(*p, "apple-touch-icon") == 0 ||
            g_ascii_strcasecmp(*p, "apple-touch-icon-precomposed") == 0) {
            match = "icon";
            break;
        }
    }
    g_strfreev(toks);
    return match;
}

static char *
nd_window_pick_favicon_href(nd_window *w)
{
    if (!w->parsed_doc) return NULL;
    nd_node *head = nd_node_find_first_element(w->parsed_doc, "head");
    if (!head) return NULL;
    char *fallback = NULL;
    for (const nd_node *c = head->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (g_ascii_strcasecmp(c->name, "link") != 0) continue;
        const char *rel = nd_element_get_attr(c, "rel");
        const char *href = nd_element_get_attr(c, "href");
        if (!rel || !href || !*href) continue;
        if (!nd_link_rel_icon_kind(rel)) continue;
        return g_strdup(href);
    }
    return fallback;
}

static char *
nd_window_default_favicon_url(nd_window *w)
{
    const char *page = nd_window_current_url(w);
    if (!page) return NULL;
    if (!nd_url_is_http_or_https(page))
        return NULL;
    char *origin = nd_url_origin_from(page);
    if (!origin || !*origin) { g_free(origin); return NULL; }
    char *url = g_strdup_printf("%s/favicon.ico", origin);
    g_free(origin);
    return url;
}

static void
on_favicon_ready(nd_image *img, gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !w->tab_icon) return;
    if (!img || !img->loaded || !img->texture) return;
    if (img->natural_width <= 0 || img->natural_height <= 0) return;
    gtk_image_set_from_paintable(GTK_IMAGE(w->tab_icon),
                                 GDK_PAINTABLE(img->texture));
    gtk_image_set_pixel_size(GTK_IMAGE(w->tab_icon), 14);
    w->favicon_loaded = TRUE;
}

static void
nd_window_kick_favicon(nd_window *w)
{
    if (!w || !w->images || !w->tab_icon) return;
    if (w->favicon_loaded) return;
    const nd_config *cfg = nd_config_get();
    if (cfg && !cfg->images_enabled) return;
    const char *page = nd_window_current_url(w);
    if (!page) return;
    if (g_str_has_prefix(page, "about:") || g_str_has_prefix(page, "file:") ||
        g_str_has_prefix(page, "data:"))
        return;

    char *href = nd_window_pick_favicon_href(w);
    char *abs = href ? nd_resolve_url(w, href) : NULL;
    g_free(href);
    if (!abs) abs = nd_window_default_favicon_url(w);
    if (!abs) return;
    if (!nd_url_is_http_or_https(abs)) {
        g_free(abs);
        return;
    }
    if (nd_window_subresource_blocked(w, abs, ND_CSP_IMG, "favicon")) {
        g_free(abs);
        return;
    }
    nd_image_cache_get(w->images, abs, page, on_favicon_ready, w);
    g_free(abs);
}

static double
nd_window_visible_page_height(nd_window *w)
{
    double page = 0;
    if (w && w->render_vadj)
        page = gtk_adjustment_get_page_size(w->render_vadj);
    if (page <= 100 && w && w->drawing_area) {
        int h = gtk_widget_get_height(w->drawing_area);
        if (h > 100) page = h;
    }
    if (page <= 100 && w && w->window) {
        int h = gtk_widget_get_height(w->window);
        if (h > 100) page = h;
    }
    return page > 100 ? page : 900.0;
}

static gboolean
nd_box_in_fixed_layer(const nd_box *box)
{
    for (const nd_box *p = box; p; p = p->parent) {
        const nd_css_value *pos = p->style
            ? p->style->values[ND_CSS_POSITION]
            : NULL;
        if (nd_css_keyword_is(pos, "fixed") ||
            nd_css_keyword_is(pos, "sticky"))
            return TRUE;
    }
    return FALSE;
}

static gboolean
nd_window_image_box_near_view(const nd_window *w, const nd_box *box)
{
    if (!w || !box || !w->render_vadj) return TRUE;
    if (nd_box_in_fixed_layer(box)) return TRUE;
    double top = gtk_adjustment_get_value(w->render_vadj);
    double page = nd_window_visible_page_height((nd_window *)w);
    double margin = page * 2.5;
    if (margin < 1500) margin = 1500;
    double box_h = box->content_height + box->padding.top + box->padding.bottom +
                   box->border.top + box->border.bottom;
    if (box_h < 1) box_h = 1;
    double bottom = top + page;
    return box->y + box_h >= top - margin &&
           box->y <= bottom + margin;
}

static gboolean
nd_image_inflight(const nd_image *img)
{
    return img && !img->loaded && !img->failed;
}

static guint
nd_window_near_image_inflight_count(const nd_window *w, GPtrArray *imgs)
{
    guint n = 0;
    for (guint i = 0; i < imgs->len; i++) {
        nd_box *box = g_ptr_array_index(imgs, i);
        if (!box || !box->media) continue;
        if (!nd_window_image_box_near_view(w, box)) continue;
        if (nd_image_inflight(box->media->image)) n++;
        if (nd_image_inflight(box->media->bg_image)) n++;
    }
    return n;
}

static void
nd_window_kick_image_loads(nd_window *w)
{
    if (!w->layout_tree || !w->images) return;
    gint64 now_us = g_get_monotonic_time();
    GPtrArray *imgs = g_ptr_array_new();
    nd_layout_collect_images(w->layout_tree, imgs);
    guint inflight = nd_window_near_image_inflight_count(w, imgs);
    for (guint i = 0; i < imgs->len; i++) {
        nd_box *box = g_ptr_array_index(imgs, i);
        if (!box->media) continue;
        if (!nd_window_image_box_near_view(w, box)) continue;
        if (nd_image_should_retry(box->media->image, now_us))
            box->media->image = NULL;
        if (nd_image_should_retry(box->media->bg_image, now_us))
            box->media->bg_image = NULL;
        if (box->media->image_src &&
            !box->media->image &&
            !g_str_has_prefix(box->media->image_src, "nd-inline-svg:")) {
            if (inflight >= 12) continue;
            char *abs = nd_resolve_url(w, box->media->image_src);
            if (abs) {
                if (nd_window_subresource_blocked(w, abs, ND_CSP_IMG, "image")) {
                    g_free(abs);
                } else {
                    box->media->image = nd_image_cache_get(w->images, abs,
                        nd_window_current_url(w), on_image_ready, w);
                    if (nd_image_inflight(box->media->image)) inflight++;
                    g_free(abs);
                }
            }
        }
        if (box->media->bg_image_src && !box->media->bg_image) {
            if (inflight >= 12) continue;
            char *abs = nd_resolve_url(w, box->media->bg_image_src);
            if (abs) {
                if (nd_window_subresource_blocked(w, abs, ND_CSP_IMG, "image")) {
                    g_free(abs);
                } else {
                    box->media->bg_image = nd_image_cache_get(w->images, abs,
                        nd_window_current_url(w), on_image_ready, w);
                    if (nd_image_inflight(box->media->bg_image)) inflight++;
                    g_free(abs);
                }
            }
        }
    }
    g_ptr_array_free(imgs, TRUE);
}

void
nd_window_render_vadjustment_changed(GtkAdjustment *adj, gpointer ud)
{
    (void)adj;
    nd_window *w = ud;
    if (!w || w->mode != ND_VIEW_RENDER) return;
    nd_window_kick_image_loads(w);
}

static void
nd_window_kick_video_loads(nd_window *w)
{
    if (!w->layout_tree || !w->videos) return;
    GPtrArray *vids = g_ptr_array_new();
    nd_layout_collect_videos(w->layout_tree, vids);
    for (guint i = 0; i < vids->len; i++) {
        nd_box *box = g_ptr_array_index(vids, i);
        if (!box->media || !box->media->video_src) continue;
        nd_box_media *m = box->media;
        char *abs = nd_resolve_url(w, m->video_src);
        if (!abs) continue;
        if (nd_window_subresource_blocked(w, abs, ND_CSP_MEDIA, "video")) {
            g_free(abs);
            continue;
        }
        char *poster_abs = NULL;
        if (m->video_poster) poster_abs = nd_resolve_url(w, m->video_poster);
        if (poster_abs &&
            nd_window_subresource_blocked(w, poster_abs, ND_CSP_IMG, "video-poster")) {
            g_free(poster_abs);
            poster_abs = NULL;
        }
        m->video = nd_video_cache_get(w->videos, abs, poster_abs,
                                      nd_window_current_url(w),
                                      on_video_ready, w);
        g_free(abs);
        g_free(poster_abs);
    }
    g_ptr_array_free(vids, TRUE);
}

static gboolean
looks_like_host(const char *s, size_t len)
{
    if (len == 0) return FALSE;
    gboolean has_dot = FALSE;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t') return FALSE;
        if (c == '.') has_dot = TRUE;
        if (c == '/' || c == '?' || c == '#') break;
    }
    return has_dot;
}

static char *
nd_normalize_url(const char *raw)
{
    if (!raw)
        return NULL;

    while (*raw == ' ' || *raw == '\t' || *raw == '\r' || *raw == '\n')
        raw++;
    size_t len = strlen(raw);
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t' ||
                       raw[len - 1] == '\r' || raw[len - 1] == '\n'))
        len--;
    if (len == 0)
        return NULL;

    {
        size_t out_len = 0;
        for (size_t i = 0; i < len; i++) {
            char c = raw[i];
            if (c == '\t' || c == '\r' || c == '\n') continue;
            out_len++;
        }
        if (out_len != len) {
            char *out = g_malloc(out_len + 1);
            size_t j = 0;
            for (size_t i = 0; i < len; i++) {
                char c = raw[i];
                if (c == '\t' || c == '\r' || c == '\n') continue;
                out[j++] = c;
            }
            out[j] = '\0';
            char *resolved = nd_normalize_url(out);
            g_free(out);
            return resolved;
        }
    }

    if ((len >= 6 && g_ascii_strncasecmp(raw, "about:", 6) == 0))
        return g_strndup(raw, len);

    gboolean has_scheme = FALSE;
    for (size_t i = 0; i < len; i++) {
        if (raw[i] == ':' && i + 2 < len && raw[i + 1] == '/' && raw[i + 2] == '/') {
            has_scheme = TRUE;
            break;
        }
        if (!g_ascii_isalnum(raw[i]) && raw[i] != '+' && raw[i] != '-' && raw[i] != '.')
            break;
    }

    if (has_scheme)
        return g_strndup(raw, len);

#ifdef G_OS_WIN32
    if (len >= 3 && g_ascii_isalpha(raw[0]) && raw[1] == ':' &&
        (raw[2] == '\\' || raw[2] == '/')) {
        GString *p = g_string_new("file:///");
        g_string_append_c(p, g_ascii_toupper(raw[0]));
        g_string_append_c(p, ':');
        for (size_t i = 2; i < len; i++) {
            char c = raw[i] == '\\' ? '/' : raw[i];
            if (c == ' ') g_string_append(p, "%20");
            else          g_string_append_c(p, c);
        }
        return g_string_free(p, FALSE);
    }
    if (len >= 3 && raw[0] == '\\' && raw[1] == '\\') {
        GString *p = g_string_new("file://");
        for (size_t i = 2; i < len; i++) {
            char c = raw[i] == '\\' ? '/' : raw[i];
            if (c == ' ') g_string_append(p, "%20");
            else          g_string_append_c(p, c);
        }
        return g_string_free(p, FALSE);
    }
#endif

    if (looks_like_host(raw, len) || raw[0] == '/') {
        char *bare = g_strndup(raw, len);
        char *full = g_strconcat("https://", bare, NULL);
        g_free(bare);
        return full;
    }

    char *query = g_strndup(raw, len);
    char *escaped = g_uri_escape_string(query, NULL, FALSE);
    g_free(query);
    const nd_config *cfg = nd_config_get();
    const char *tmpl = cfg && cfg->search_engine && *cfg->search_engine
                       ? cfg->search_engine
                       : "https://www.google.com/search?q=%s";
    const char *pct = strstr(tmpl, "%s");
    char *full;
    if (pct) {
        char *prefix = g_strndup(tmpl, (gsize)(pct - tmpl));
        full = g_strconcat(prefix, escaped, pct + 2, NULL);
        g_free(prefix);
    } else {
        full = g_strconcat(tmpl, escaped, NULL);
    }
    g_free(escaped);
    return full;
}

static char *
nd_download_extract_disposition_name(const char *disp)
{
    if (!disp || !*disp) return NULL;
    const char *p = strstr(disp, "filename*=");
    if (p) {
        p += 10;
        const char *q = strchr(p, '\'');
        if (q) {
            q = strchr(q + 1, '\'');
            if (q) p = q + 1;
        }
        gsize len = strcspn(p, ";");
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        if (len > 0) {
            char *raw = g_strndup(p, len);
            char *decoded = g_uri_unescape_string(raw, NULL);
            g_free(raw);
            if (decoded && *decoded) return decoded;
            g_free(decoded);
        }
    }
    p = strstr(disp, "filename=");
    if (p) {
        p += 9;
        while (*p == ' ' || *p == '\t') p++;
        const char *end;
        if (*p == '"') {
            p++;
            end = strchr(p, '"');
            if (!end) end = p + strlen(p);
        } else {
            end = p + strcspn(p, ";");
            while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
        }
        if (end > p) return g_strndup(p, (gsize)(end - p));
    }
    return NULL;
}

static char *
nd_download_suggest_filename(const char *url, const char *disp)
{
    char *name = nd_download_extract_disposition_name(disp);
    if (name && *name) {
        for (char *s = name; *s; s++)
            if (*s == '/' || *s == '\\') *s = '_';
        return name;
    }
    g_free(name);
    if (!url) return g_strdup("download");
    const char *q = strchr(url, '?');
    gsize end_off = q ? (gsize)(q - url) : strlen(url);
    const char *frag = memchr(url, '#', end_off);
    if (frag) end_off = (gsize)(frag - url);
    gsize slash = end_off;
    while (slash > 0 && url[slash - 1] != '/') slash--;
    if (slash < end_off) {
        char *raw = g_strndup(url + slash, end_off - slash);
        char *decoded = g_uri_unescape_string(raw, NULL);
        g_free(raw);
        if (decoded && *decoded) return decoded;
        g_free(decoded);
    }
    return g_strdup("download");
}

static gboolean
nd_should_download(const char *content_type, const char *content_disposition)
{
    if (content_disposition) {
        char *lc = g_ascii_strdown(content_disposition, -1);
        gboolean attach = strstr(lc, "attachment") != NULL;
        g_free(lc);
        if (attach) return TRUE;
    }
    if (!content_type) return FALSE;
    if (g_ascii_strncasecmp(content_type, "application/octet-stream", 24) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/pdf", 15) == 0 &&
        !nd_pdf_available()) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/zip", 15) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/x-tar", 17) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/gzip", 16) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/x-gzip", 18) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/x-bzip2", 19) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/x-xz", 16) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/x-7z-compressed", 27) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/x-rar-compressed", 28) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/x-debian-package", 28) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/vnd.android.package-archive", 39) == 0) return TRUE;
    if (g_ascii_strncasecmp(content_type, "application/x-msdownload", 24) == 0) return TRUE;
    return FALSE;
}

typedef struct nd_download_pending {
    guint      w_id;
    GBytes    *bytes;
    char      *url;
} nd_download_pending;

static void
nd_download_pending_free(nd_download_pending *p)
{
    if (!p) return;
    if (p->bytes) g_bytes_unref(p->bytes);
    g_free(p->url);
    g_free(p);
}

static void
nd_download_save_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    nd_download_pending *p = user_data;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
    nd_window *w = nd_window_for_id(p->w_id);
    if (!file) {
        if (w) {
            if (err && !g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
                nd_window_set_status(w, "Download cancelled: %s", err->message);
            else
                nd_window_set_status(w, "Download cancelled");
        }
        g_clear_error(&err);
        nd_download_pending_free(p);
        return;
    }
    g_clear_error(&err);
    char *path = g_file_get_path(file);
    gsize sz = 0;
    gconstpointer data = g_bytes_get_data(p->bytes, &sz);
    GError *werr = NULL;
    gboolean ok = g_file_replace_contents(file, data, sz, NULL, FALSE,
                                          G_FILE_CREATE_REPLACE_DESTINATION,
                                          NULL, NULL, &werr);
    if (w) {
        if (ok) nd_window_set_status(w, "Saved %s (%" G_GSIZE_FORMAT " bytes)",
                                     path ? path : "(file)", sz);
        else    nd_window_set_status(w, "Save failed: %s",
                                     werr ? werr->message : "unknown");
    }
    g_clear_error(&werr);
    g_free(path);
    g_object_unref(file);
    nd_download_pending_free(p);
}

static void
nd_window_record_final_url(nd_window *w, const nd_response *resp)
{
    if (!w || !resp || !resp->final_url) return;
    if (!nd_url_is_http_or_https(resp->final_url))
        return;
    if (w->url_entry) {
        char *disp = nd_url_to_display(resp->final_url);
        const char *show = disp ? disp : resp->final_url;
        const char *cur = gtk_editable_get_text(GTK_EDITABLE(w->url_entry));
        if (!cur || strcmp(cur, show) != 0)
            gtk_editable_set_text(GTK_EDITABLE(w->url_entry), show);
        g_free(disp);
    }
    if (w->history && w->cursor >= 0 && w->cursor < (int)w->history->len) {
        char *cur = g_ptr_array_index(w->history, w->cursor);
        if (!cur || strcmp(cur, resp->final_url) != 0) {
            g_free(cur);
            w->history->pdata[w->cursor] = g_strdup(resp->final_url);
        }
    }
}

static void
nd_window_offer_download(nd_window *w, const nd_response *resp)
{
    if (!resp || !resp->body || resp->body->len == 0) return;
    nd_download_pending *p = g_new0(nd_download_pending, 1);
    p->w_id = w->id;
    p->bytes = g_bytes_new(resp->body->data, resp->body->len);
    p->url = g_strdup(resp->final_url ? resp->final_url : "");

    char *suggested = nd_download_suggest_filename(resp->final_url,
                                                   resp->content_disposition);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save file");
    if (suggested) gtk_file_dialog_set_initial_name(dialog, suggested);
    g_free(suggested);
    gtk_file_dialog_save(dialog, GTK_WINDOW(w->window), NULL,
                         nd_download_save_done, p);
    g_object_unref(dialog);
    nd_window_set_status(w, "Downloading %s ...",
                         resp->final_url ? resp->final_url : "");
}

static void
nd_on_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_fetch_ctx *ctx = user_data;
    guint wid = ctx ? ctx->wid : 0;
    guint gen = ctx ? ctx->gen : 0;
    g_free(ctx);
    nd_window *w = nd_window_for_id(wid);
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);

    if (!w) {
        nd_response_free(resp);
        g_clear_error(&err);
        return;
    }

    if (gen != w->fetch_gen) {
        nd_response_free(resp);
        g_clear_error(&err);
        return;
    }

    g_clear_object(&w->current_fetch);

    if (!resp) {
        if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            nd_window_set_status(w, "Cancelled");
            if (err) g_clear_error(&err);
            nd_window_set_busy(w, FALSE);
            return;
        }
        const char *emsg = err ? err->message : "unknown error";
        nd_window_set_status(w, "Error: %s", emsg);
        char *line = g_strdup_printf("[error] page fetch failed: %s", emsg);
        nd_window_console_append(w, line);
        g_free(line);
        const char *fetch_url = w->history && w->cursor >= 0 &&
            w->cursor < (int)w->history->len
            ? g_ptr_array_index(w->history, w->cursor) : NULL;
        char *html = nd_build_error_page(fetch_url, 0, emsg);
        nd_window_clear_cache(w);
        w->last_body = html;
        w->last_body_len = strlen(html);
        w->last_content_type = g_strdup("text/html; charset=utf-8");
        w->dom_mutated = FALSE;
        w->mode = ND_VIEW_RENDER;
        nd_window_render(w);
        nd_window_ensure_layout(w, nd_layout_viewport());
        nd_window_set_title_if_active(w, "Error — " ND_TITLE);
        if (err) g_clear_error(&err);
        nd_window_set_busy(w, FALSE);
        return;
    }

    nd_window_record_final_url(w, resp);
    nd_debug_log_emit(ND_DLOG_NET, "fetch",
                      "status=%ld %s len=%zu type=%s",
                      resp->status, resp->final_url ? resp->final_url : "(no-url)",
                      (size_t)(resp->body ? resp->body->len : 0),
                      resp->content_type ? resp->content_type : "?");

    if (resp->error) {
        char *line = g_strdup_printf("[error] page transport error: %s",
                                     resp->error);
        nd_window_console_append(w, line);
        g_free(line);
        nd_window_set_status(w, "Transport error: %s", resp->error);
        nd_window_clear_cache(w);
        char *html = nd_build_error_page(
            resp->final_url ? resp->final_url : "",
            resp->status, resp->error);
        w->last_body = html;
        w->last_body_len = strlen(html);
        w->last_content_type = g_strdup("text/html; charset=utf-8");
        w->dom_mutated = FALSE;
        w->mode = ND_VIEW_RENDER;
        nd_window_render(w);
        nd_window_ensure_layout(w, nd_layout_viewport());
        nd_window_set_title_if_active(w, "Error — " ND_TITLE);
        nd_response_free(resp);
        nd_window_set_busy(w, FALSE);
        return;
    }

    if (resp->status < 400 &&
        nd_should_download(resp->content_type, resp->content_disposition)) {
        nd_window_offer_download(w, resp);
        if (w->history && w->cursor >= 0 && (int)w->history->len > w->cursor + 1) {
            g_free(g_ptr_array_index(w->history, w->history->len - 1));
            g_ptr_array_set_size(w->history, w->history->len - 1);
        } else if (w->history && (int)w->history->len > 0 &&
                   w->cursor == (int)w->history->len - 1) {
            const char *cur = g_ptr_array_index(w->history, w->cursor);
            if (cur && resp->final_url && strcmp(cur, resp->final_url) == 0) {
                g_free(g_ptr_array_index(w->history, w->cursor));
                g_ptr_array_set_size(w->history, w->history->len - 1);
                w->cursor--;
                if (w->cursor >= 0 && w->cursor < (int)w->history->len) {
                    const char *prev = g_ptr_array_index(w->history, w->cursor);
                    if (prev && w->url_entry) {
                        char *disp = nd_url_to_display(prev);
                        gtk_editable_set_text(GTK_EDITABLE(w->url_entry),
                                              disp ? disp : prev);
                        g_free(disp);
                    }
                }
                nd_window_update_nav_state(w);
            }
        }
        nd_response_free(resp);
        nd_window_set_busy(w, FALSE);
        return;
    }

    if (resp->tls_warning) {
        nd_window_set_status(w, "%s", resp->tls_warning);
        char *line = g_strdup_printf("[warn] TLS: %s", resp->tls_warning);
        nd_window_console_append(w, line);
        g_free(line);
    } else if (resp->status >= 400) {
        nd_window_set_status(w, "%ld %s", resp->status,
                             resp->final_url ? resp->final_url : "");
        char *line = g_strdup_printf("[error] HTTP %ld: %s",
                                     resp->status,
                                     resp->final_url ? resp->final_url : "");
        nd_window_console_append(w, line);
        g_free(line);

        gboolean body_is_html =
            resp->content_type &&
            (g_ascii_strncasecmp(resp->content_type, "text/html", 9) == 0 ||
             g_ascii_strncasecmp(resp->content_type, "application/xhtml", 17) == 0);
        gboolean body_useful = resp->body && resp->body->len > 64 && body_is_html;
        if (!body_useful) {
            char *html = nd_build_error_page(
                resp->final_url ? resp->final_url : "",
                resp->status, NULL);
            nd_window_clear_cache(w);
            w->last_body = html;
            w->last_body_len = strlen(html);
            w->last_content_type = g_strdup("text/html; charset=utf-8");
            w->dom_mutated = FALSE;
            w->mode = ND_VIEW_RENDER;
            nd_window_render(w);
            nd_window_ensure_layout(w, nd_layout_viewport());
            nd_window_set_title_if_active(w, "Error — " ND_TITLE);
            nd_response_free(resp);
            nd_window_set_busy(w, FALSE);
            return;
        }
    }

    nd_window_clear_cache(w);

    if (nd_pdf_available() && resp->content_type &&
        g_ascii_strncasecmp(resp->content_type, "application/pdf", 15) == 0 &&
        resp->body && resp->body->len > 0) {
        w->pdf = nd_pdf_new_from_bytes(resp->body->data, resp->body->len);
        if (w->pdf) {
            w->last_content_type = g_strdup("application/pdf");
            w->mode = ND_VIEW_RENDER;
            nd_window_render(w);
            char *title = g_path_get_basename(resp->final_url
                                              ? resp->final_url : "document.pdf");
            char *q = strchr(title, '?');
            if (q) *q = '\0';
            char *full_title = g_strdup_printf("%s — %s",
                title && *title ? title : "PDF", ND_TITLE);
            nd_window_set_title_if_active(w, full_title);
            g_free(full_title);
            g_free(title);
            nd_window_update_tab_label(w);
            if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
            nd_window_set_status(w, "%ld  %s  (PDF, %" G_GSIZE_FORMAT " bytes, %d pages)",
                                 resp->status,
                                 resp->final_url ? resp->final_url : "",
                                 (gsize)resp->body->len,
                                 nd_pdf_n_pages(w->pdf));
            nd_response_free(resp);
            nd_window_set_busy(w, FALSE);
            return;
        }
    }

    nd_window_set_stage(w, ND_STAGE_PARSING);
    if (resp->body && resp->body->len > 0) {
        char *decoded = nd_html_decode_body((const char *)resp->body->data,
                                    resp->body->len);
        if (!decoded) decoded = g_strdup("");
        w->last_body = decoded;
        w->last_body_len = strlen(decoded);
        w->dom_mutated = FALSE;
        if (is_html_content_type(resp->content_type))
            nd_window_preload_stylesheets(w, decoded, w->last_body_len);
    }
    w->last_content_type = g_strdup(resp->content_type ? resp->content_type : "");
    if (w->csp) { nd_csp_free(w->csp); w->csp = NULL; }
    if (resp->csp_header && *resp->csp_header)
        w->csp = nd_csp_parse(resp->csp_header);

    if (is_html_content_type(w->last_content_type))
        w->mode = ND_VIEW_RENDER;
    else
        w->mode = ND_VIEW_RAW;

    nd_css_set_target_fragment(
        w->pending_fragment && *w->pending_fragment
            ? w->pending_fragment : NULL);
    nd_window_set_stage(w, ND_STAGE_RENDERING);
    nd_window_render(w);
    if (is_html_content_type(w->last_content_type)) {
        nd_window_ensure_layout(w, nd_layout_viewport());
        nd_window_apply_page_title(w);
        nd_window_kick_favicon(w);
    } else {
        nd_window_set_title_if_active(w, ND_TITLE);
        nd_window_update_tab_label(w);
    }
    if (w->pending_fragment && w->render_vadj) {
        nd_window_scroll_to_fragment(w);
    } else if (w->render_vadj) {
        gtk_adjustment_set_value(w->render_vadj, 0);
    }

    if (w->parsed_doc) {
        nd_window_apply_meta_refresh(w, resp);
        if (w->js) {
            const char *prev_url = nd_js_current_url(w->js);
            const char *new_url  = nd_window_current_url(w);
            if (prev_url && *prev_url && new_url && *new_url) {
                g_autofree char *prev_origin = nd_url_origin_from(prev_url);
                g_autofree char *new_origin  = nd_url_origin_from(new_url);
                if (prev_origin && new_origin &&
                    strcmp(prev_origin, new_origin) != 0) {
                    nd_js_free(w->js);
                    w->js = NULL;
                }
            }
        }
        nd_window_ensure_js(w);
        if (w->js) {
            nd_js_set_csp(w->js, w->csp);
            nd_js_set_image_cache(w->js, w->images);
            if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
            for (int i = 0; i < 64 && g_main_context_iteration(NULL, FALSE); i++) { }
            if (!nd_window_for_id(wid)) { nd_response_free(resp); return; }
            nd_window_set_stage(w, ND_STAGE_SCRIPTING);
            nd_js_run_scripts_in_doc(w->js, w->parsed_doc,
                                     nd_window_current_url(w));
            if (!nd_window_for_id(wid)) { nd_response_free(resp); return; }
            if (nd_js_consume_mutated(w->js))
                nd_window_js_mutated(w);
        }
    }

    nd_window_set_status(w, "%ld  %s  (%s, %" G_GSIZE_FORMAT " bytes)",
                         resp->status,
                         resp->final_url ? resp->final_url : "",
                         resp->content_type ? resp->content_type : "?",
                         (gsize)w->last_body_len);
    nd_response_free(resp);
    nd_window_set_busy(w, FALSE);
    nd_window_set_stage(w, ND_STAGE_DONE);
}

void
nd_window_load_url(nd_window *w, const char *raw_url, nd_load_source src)
{
    char *url = nd_normalize_url(raw_url);
    if (!url) {
        nd_window_set_status(w, "Empty URL");
        return;
    }
    char *upgraded = nd_net_hsts_upgrade(url);
    if (upgraded) { g_free(url); url = upgraded; }

    char *mobile = nd_mobile_rewrite_url(url);
    if (mobile) { g_free(url); url = mobile; }

    g_free(w->pending_fragment);
    w->pending_fragment = NULL;
    char *hash = strchr(url, '#');
    if (hash) {
        w->pending_fragment = g_strdup(hash + 1);
        *hash = '\0';
        const char *cur = nd_window_current_url(w);
        if (cur && strcmp(cur, url) == 0) {
            char *old_url = g_strdup(cur);
            char *new_url = g_strconcat(url, "#",
                                        w->pending_fragment ? w->pending_fragment : "",
                                        NULL);
            g_free(url);
            nd_css_set_target_fragment(
                w->pending_fragment && *w->pending_fragment
                    ? w->pending_fragment : NULL);
            if (src != ND_LOAD_HISTORY)
                nd_window_js_soft_nav(new_url, FALSE, w);
            w->layout_dirty = TRUE;
            nd_window_ensure_layout(w, nd_layout_viewport());
            nd_window_scroll_to_fragment(w);
            if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
            if (w->js && strcmp(old_url, new_url) != 0)
                nd_js_dispatch_hashchange(w->js, old_url, new_url);
            g_free(old_url);
            g_free(new_url);
            return;
        }
    }

    if (w->current_fetch) {
        g_cancellable_cancel(w->current_fetch);
        g_clear_object(&w->current_fetch);
    }

    if (src == ND_LOAD_USER) {

        while ((int)w->history->len > w->cursor + 1) {
            g_free(g_ptr_array_index(w->history, w->history->len - 1));
            g_ptr_array_set_size(w->history, w->history->len - 1);
        }

        gboolean is_dup = FALSE;
        if (w->cursor >= 0 && w->cursor < (int)w->history->len) {
            const char *cur = g_ptr_array_index(w->history, w->cursor);
            if (cur && strcmp(cur, url) == 0) is_dup = TRUE;
        }
        if (!is_dup) {
            g_ptr_array_add(w->history, g_strdup(url));
            w->cursor = (int)w->history->len - 1;
        }
    }

    char *disp = nd_url_to_display(url);
    gtk_editable_set_text(GTK_EDITABLE(w->url_entry), disp ? disp : url);
    g_free(disp);

    w->current_fetch = g_cancellable_new();
    nd_window_set_busy(w, TRUE);
    nd_window_update_nav_state(w);
    nd_window_set_status(w, "Loading %s …", url);
    nd_debug_log_emit(ND_DLOG_NET, "navigate", "%s", url);
    nd_net_fetch_async(url, NULL, w->current_fetch, nd_on_fetch_done,
                       nd_fetch_ctx_new(w));
    g_free(url);
}

static void
nd_window_set_busy(nd_window *w, gboolean busy)
{
    if (!w) return;
    w->busy = busy;
    if (w->go_button && GTK_IS_WIDGET(w->go_button))
        gtk_widget_set_sensitive(w->go_button, !busy);
    if (w->home_button && GTK_IS_WIDGET(w->home_button))
        gtk_widget_set_sensitive(w->home_button, !busy);
    gboolean can_stop = busy || (w->js && !nd_js_is_halted(w->js));
    if (w->stop_button && GTK_IS_WIDGET(w->stop_button)) {
        gtk_widget_set_sensitive(w->stop_button, can_stop);
        gtk_widget_set_tooltip_text(w->stop_button,
            busy ? "Stop loading" : "Stop scripts");
    }
    nd_window_set_stage(w, busy ? ND_STAGE_FETCHING : ND_STAGE_IDLE);
    nd_window_update_logo_loading(w, busy);
    if (w->window && GTK_IS_WIDGET(w->window))
        gtk_widget_set_cursor_from_name(w->window, busy ? "wait" : NULL);
    if (w->drawing_area && GTK_IS_WIDGET(w->drawing_area))
        gtk_widget_set_cursor_from_name(w->drawing_area, busy ? "wait" : NULL);
    if (busy) {
        gtk_widget_set_sensitive(w->back_button, FALSE);
        gtk_widget_set_sensitive(w->forward_button, FALSE);
    } else {
        nd_window_update_nav_state(w);
    }
}

void
nd_window_update_nav_state(nd_window *w)
{
    if (!w || !w->window || !GTK_IS_WINDOW(w->window)) return;
    gboolean can_back    = w->cursor > 0;
    gboolean can_forward = w->cursor >= 0 && w->cursor + 1 < (int)w->history->len;
    if (w->back_button && GTK_IS_WIDGET(w->back_button))
        gtk_widget_set_sensitive(w->back_button, can_back);
    if (w->forward_button && GTK_IS_WIDGET(w->forward_button))
        gtk_widget_set_sensitive(w->forward_button, can_forward);
    GAction *ab = g_action_map_lookup_action(G_ACTION_MAP(w->window), "back");
    GAction *af = g_action_map_lookup_action(G_ACTION_MAP(w->window), "forward");
    if (G_IS_SIMPLE_ACTION(ab))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(ab), can_back);
    if (G_IS_SIMPLE_ACTION(af))
        g_simple_action_set_enabled(G_SIMPLE_ACTION(af), can_forward);
}

void
on_go_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->url_entry));
    nd_window_load_url(w, text, ND_LOAD_USER);
}

void
on_stop_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    g_action_group_activate_action(G_ACTION_GROUP(w->window), "stop", NULL);
}

void
on_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    nd_window *w = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->url_entry));
    nd_window_load_url(w, text, ND_LOAD_USER);
}

void
on_back_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    if (w->cursor <= 0) return;
    w->cursor--;
    const char *url = g_ptr_array_index(w->history, w->cursor);
    nd_window_load_url(w, url, ND_LOAD_HISTORY);
}

void
on_forward_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    if (w->cursor < 0 || w->cursor + 1 >= (int)w->history->len) return;
    w->cursor++;
    const char *url = g_ptr_array_index(w->history, w->cursor);
    nd_window_load_url(w, url, ND_LOAD_HISTORY);
}

void
on_home_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    nd_window_load_url(w, g_home_url, ND_LOAD_USER);
}

void
on_reload_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    if (w->cursor < 0 || w->cursor >= (int)w->history->len) return;
    const char *cur = g_ptr_array_index(w->history, w->cursor);
    nd_window_load_url(w, cur, ND_LOAD_HISTORY);
}

const char *
nd_window_current_url(nd_window *w)
{
    if (w->cursor < 0 || w->cursor >= (int)w->history->len) return NULL;
    return g_ptr_array_index(w->history, w->cursor);
}

char *
nd_window_current_title(nd_window *w)
{
    if (!w->parsed_doc) return NULL;
    nd_node *title = nd_node_find_first_element(w->parsed_doc, "title");
    if (!title) return NULL;
    return nd_node_collect_text(title);
}

static gboolean
is_button_like(const nd_node *n)
{
    if (!n || n->kind != ND_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "button") == 0) return TRUE;
    if (strcmp(n->name, "input") != 0) return FALSE;
    const char *type = nd_element_get_attr(n, "type");
    if (!type) return FALSE;
    return g_ascii_strcasecmp(type, "submit") == 0 ||
           g_ascii_strcasecmp(type, "button") == 0 ||
           g_ascii_strcasecmp(type, "reset")  == 0 ||
           g_ascii_strcasecmp(type, "checkbox") == 0 ||
           g_ascii_strcasecmp(type, "radio") == 0;
}

static gboolean nd_node_reachable_from(const nd_node *root, const nd_node *target);

typedef struct nd_select_pick_ctx {
    nd_window *w;
    nd_node *select_node;
    nd_node *option;
    GtkWidget *popover;
} nd_select_pick_ctx;

static void
nd_select_pick(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    nd_select_pick_ctx *ctx = user_data;
    if (!ctx || !ctx->select_node || !ctx->option) return;
    if (!nd_node_reachable_from(ctx->w->parsed_doc, ctx->select_node) ||
        !nd_node_reachable_from(ctx->w->parsed_doc, ctx->option)) {
        if (ctx->popover) gtk_popover_popdown(GTK_POPOVER(ctx->popover));
        return;
    }
    GQueue queue = G_QUEUE_INIT;
    g_queue_push_tail(&queue, ctx->select_node);
    while (!g_queue_is_empty(&queue)) {
        nd_node *n = g_queue_pop_head(&queue);
        if (nd_node_is_element_named(n, "option"))
            nd_element_remove_attr(n, "selected");
        for (nd_node *c = n->first_child; c; c = c->next_sibling)
            g_queue_push_tail(&queue, c);
    }
    nd_element_set_attr(ctx->option, "selected", "");
    nd_window_js_mutated(ctx->w);
    if (ctx->w->js) {
        nd_js_dispatch_event(ctx->w->js, ctx->select_node, "input",  NULL);
        nd_js_dispatch_event(ctx->w->js, ctx->select_node, "change", NULL);
    }
    if (ctx->popover) gtk_popover_popdown(GTK_POPOVER(ctx->popover));
}

static void
nd_select_pick_free(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

static void
nd_window_open_select_popover(nd_window *w, nd_node *select_node, double x, double y)
{
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, w->drawing_area);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scrolled, 240, 320);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
    gtk_popover_set_child(GTK_POPOVER(popover), scrolled);

    GQueue queue = G_QUEUE_INIT;
    g_queue_push_tail(&queue, select_node);
    while (!g_queue_is_empty(&queue)) {
        nd_node *n = g_queue_pop_head(&queue);
        if (nd_node_is_element_named(n, "option")) {
            char *label = nd_node_collect_text(n);
            GtkWidget *btn = gtk_button_new_with_label(label ? label : "");
            gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
            gtk_widget_set_halign(btn, GTK_ALIGN_START);
            nd_select_pick_ctx *ctx = g_new0(nd_select_pick_ctx, 1);
            ctx->w = w;
            ctx->select_node = select_node;
            ctx->option = n;
            ctx->popover = popover;
            g_signal_connect_data(btn, "clicked", G_CALLBACK(nd_select_pick),
                                  ctx, nd_select_pick_free, 0);
            gtk_box_append(GTK_BOX(list), btn);
            g_free(label);
            continue;
        }
        for (nd_node *c = n->first_child; c; c = c->next_sibling)
            g_queue_push_tail(&queue, c);
    }
    gtk_popover_popup(GTK_POPOVER(popover));
    g_signal_connect_swapped(popover, "closed",
                             G_CALLBACK(gtk_widget_unparent), popover);
}

typedef struct nd_file_chooser_ctx {
    guint    w_id;
    nd_node *input;
} nd_file_chooser_ctx;

static gboolean
nd_node_reachable_depth(const nd_node *root, const nd_node *target, int depth)
{
    if (!root || depth >= 512) return FALSE;
    if (root == target) return TRUE;
    for (const nd_node *c = root->first_child; c; c = c->next_sibling)
        if (nd_node_reachable_depth(c, target, depth + 1)) return TRUE;
    return FALSE;
}

static gboolean
nd_node_reachable_from(const nd_node *root, const nd_node *target)
{
    if (!root || !target) return FALSE;
    return nd_node_reachable_depth(root, target, 0);
}

static void
nd_on_file_chooser_response(GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
    nd_file_chooser_ctx *ctx = user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, result, &err);
    nd_window *w = ctx ? nd_window_for_id(ctx->w_id) : NULL;
    if (w && ctx->input && !nd_node_reachable_from(w->parsed_doc, ctx->input))
        ctx->input = NULL;
    if (file && w && ctx->input) {
        char *path = g_file_get_path(file);
        if (path) {
            nd_element_set_attr(ctx->input, "data-nd-file-path", path);
            const char *base = strrchr(path, '/');
#ifdef G_OS_WIN32
            const char *base_w = strrchr(path, '\\');
            if (!base || (base_w && base_w > base)) base = base_w;
#endif
            nd_element_set_attr(ctx->input, "value", base ? base + 1 : path);
            g_free(path);
            if (w->js) {
                nd_js_dispatch_event(w->js, ctx->input, "input",  NULL);
                nd_js_dispatch_event(w->js, ctx->input, "change", NULL);
            }
            nd_window_js_mutated(w);
        }
    }
    if (err) g_error_free(err);
    if (file) g_object_unref(file);
    g_free(ctx);
}

static void
nd_window_open_file_chooser(nd_window *w, nd_node *input)
{
    if (!w || !input) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose file to upload");

    const char *accept = nd_element_get_attr(input, "accept");
    if (accept && *accept) {
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        GtkFileFilter *f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, accept);
        char **parts = g_strsplit(accept, ",", -1);
        for (int i = 0; parts[i]; i++) {
            char *p = g_strstrip(parts[i]);
            if (!*p) continue;
            if (*p == '.') {
                char *pat = g_strdup_printf("*%s", p);
                gtk_file_filter_add_pattern(f, pat);
                g_free(pat);
            } else if (strchr(p, '/')) {
                gtk_file_filter_add_mime_type(f, p);
            }
        }
        g_strfreev(parts);
        g_list_store_append(filters, f);
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        g_object_unref(f);
        g_object_unref(filters);
    }

    nd_file_chooser_ctx *ctx = g_new0(nd_file_chooser_ctx, 1);
    ctx->w_id = w->id;
    ctx->input = input;
    GtkWindow *parent = w->window ? GTK_WINDOW(w->window) : NULL;
    gtk_file_dialog_open(dialog, parent, NULL,
                         nd_on_file_chooser_response, ctx);
    g_object_unref(dialog);
}

static const nd_node *
find_form_role_ancestor(const nd_node *n, gboolean *is_text, gboolean *is_button)
{
    *is_text = FALSE;
    *is_button = FALSE;
    for (const nd_node *p = n; p; p = p->parent) {
        if (nd_input_is_text_like(p) ||
            nd_node_is_contenteditable_host(p)) { *is_text = TRUE; return p; }
        if (is_button_like(p))  { *is_button = TRUE; return p; }
    }
    return NULL;
}

static const char *
nd_supported_cursor_name(const char *name)
{
    static const char *known[] = {
        "default", "context-menu", "help", "pointer", "progress", "wait",
        "cell", "crosshair", "text", "vertical-text", "alias", "copy",
        "move", "no-drop", "not-allowed", "grab", "grabbing",
        "e-resize", "n-resize", "ne-resize", "nw-resize", "s-resize",
        "se-resize", "sw-resize", "w-resize", "ew-resize", "ns-resize",
        "nesw-resize", "nwse-resize", "col-resize", "row-resize",
        "all-scroll", "zoom-in", "zoom-out",
    };
    if (name && *name)
        for (guint i = 0; i < G_N_ELEMENTS(known); i++)
            if (strcmp(name, known[i]) == 0) return name;
    return "default";
}

void
on_drawing_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data)
{
    (void)ctrl;
    nd_window *w = user_data;
    w->cursor_x = x;
    w->cursor_y = y;
    if (w->busy) return;
    if (!w->layout_tree) return;
    const char *href = nd_box_hit_link(w->layout_tree, x, y);
    const nd_box *hit = NULL;
    if (!href) {
        hit = nd_box_hit_test(w->layout_tree, x, y);
        if (hit && hit->dom) {
            for (const nd_node *p = hit->dom; p; p = p->parent) {
                if (nd_node_is_element_named(p, "a")) {
                    const char *h = nd_element_get_attr(p, "href");
                    if (h && *h) { href = h; break; }
                }
            }
        }
    }
    const char *cursor_name = "default";
    gboolean media_hover = FALSE;
    if (href) {
        cursor_name = "pointer";
    } else {
        const nd_node *form_target = nd_box_hit_form_dom(w->layout_tree, x, y);
        if (!hit) hit = nd_box_hit_test(w->layout_tree, x, y);
        if (form_target) {
            if (nd_input_is_text_like(form_target))
                cursor_name = "text";
            else
                cursor_name = "pointer";
        } else if (hit && hit->dom &&
                   (nd_node_is_element_named(hit->dom, "video") ||
                    nd_node_is_element_named(hit->dom, "audio"))) {
            cursor_name = "pointer";
            media_hover = TRUE;
        } else if (hit && hit->dom) {
            gboolean t, btn;
            find_form_role_ancestor(hit->dom, &t, &btn);
            if (t)        cursor_name = "text";
            else if (btn) cursor_name = "pointer";
            else {
                for (const nd_box *bp = hit; bp; bp = bp->parent) {
                    if (bp->style && bp->style->values[ND_CSS_CURSOR] &&
                        bp->style->values[ND_CSS_CURSOR]->kind == ND_CSS_V_KEYWORD) {
                        cursor_name = bp->style->values[ND_CSS_CURSOR]->u.keyword;
                        break;
                    }
                }
                if (g_strcmp0(cursor_name, "default") == 0) {
                    for (const nd_node *p = hit->dom; p; p = p->parent) {
                        if (p->kind == ND_NODE_ELEMENT && p->name &&
                            (strcmp(p->name, "summary") == 0 ||
                             strcmp(p->name, "label") == 0 ||
                             strcmp(p->name, "select") == 0 ||
                             strcmp(p->name, "details") == 0)) {
                            cursor_name = "pointer";
                            break;
                        }
                    }
                }
            }
        }
    }
    cursor_name = nd_supported_cursor_name(cursor_name);
    GdkCursor *cur = gdk_cursor_new_from_name(cursor_name, NULL);
    if (!cur && strcmp(cursor_name, "default") != 0)
        cur = gdk_cursor_new_from_name("default", NULL);
    gtk_widget_set_cursor(w->drawing_area, cur);
    if (cur) g_object_unref(cur);
    if (href)
        nd_window_set_status(w, "%s", href);
    else if (media_hover)
        nd_window_set_status(w, "Click to play in external player");
}

static void
on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    nd_window *w = user_data;
    nd_window_mark_dead(w);
    g_clear_handle_id(&w->caret_blink_source, g_source_remove);
    g_clear_handle_id(&w->refresh_source, g_source_remove);
    g_clear_handle_id(&w->logo_anim_source, g_source_remove);
    g_clear_handle_id(&w->stage_done_source, g_source_remove);
    w->logo_image = NULL;
    if (w->im_context) {
        gtk_im_context_set_client_widget(w->im_context, NULL);
        g_clear_object(&w->im_context);
    }
    if (w->current_fetch) {
        g_cancellable_cancel(w->current_fetch);
        g_clear_object(&w->current_fetch);
    }
    nd_window_clear_cache(w);
    g_free(w->pending_fragment);
    g_free(w->search_query);
    g_free(w->context_menu_link);
    g_free(w->context_menu_image);
    g_free(w->context_menu_selection);
    g_free(w->context_menu_media);
    if (w->history) {
        for (guint i = 0; i < w->history->len; i++)
            g_free(g_ptr_array_index(w->history, i));
        g_ptr_array_free(w->history, TRUE);
    }
    if (w->images) nd_image_cache_free(w->images);
    if (w->videos) nd_video_cache_free(w->videos);
    if (w->anim)   nd_anim_free(w->anim);
    if (w->external_stylesheets) g_ptr_array_free(w->external_stylesheets, TRUE);
    if (w->external_css_seen)    g_hash_table_destroy(w->external_css_seen);
    nd_window_console_close(w);
    g_free(w);
}

static void
on_app_new_window(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    GtkApplication *app = user_data;
    nd_spawn_window(app, NULL);
}

static void
on_app_new_tab(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    GtkApplication *app = user_data;
    GtkWindow *active = gtk_application_get_active_window(app);
    if (!active) {
        nd_window_open(app, NULL);
        return;
    }
    nd_window *nw = nd_browser_add_tab(GTK_WIDGET(active), app, g_home_url);
    if (nw) {
        nd_browser_set_active(GTK_WIDGET(active), nw);
        gtk_widget_grab_focus(nw->url_entry);
    }
}

void
nd_spawn_window(GtkApplication *app, const char *url)
{
    nd_window_open(app, url);
}

static void
nd_media_open_uri_cb(GtkWindow *parent, const char *uri)
{
    if (!parent || !uri) return;
    GtkApplication *app = gtk_window_get_application(parent);
    nd_browser_add_tab(GTK_WIDGET(parent), app, uri);
}

static void
on_tab_button_clicked(GtkButton *b, gpointer user_data)
{
    (void)b;
    nd_window *w = user_data;
    nd_browser_set_active(w->window, w);
}

static void
on_tab_close_clicked(GtkButton *b, gpointer user_data)
{
    (void)b;
    nd_window *w = user_data;
    nd_browser_close_tab(w);
}

static void
on_new_tab_clicked(GtkButton *b, gpointer user_data)
{
    (void)b;
    GtkWidget *toplevel = user_data;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(toplevel));
    nd_window *nw = nd_browser_add_tab(toplevel, app, g_home_url);
    if (nw) nd_browser_set_active(toplevel, nw);
}

static void
on_toplevel_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    GPtrArray *tabs = user_data;
    if (!tabs) return;
    for (guint i = 0; i < tabs->len; i++) {
        nd_window *w = g_ptr_array_index(tabs, i);
        on_window_destroy(NULL, w);
    }
    g_ptr_array_free(tabs, TRUE);
}

void
nd_browser_set_active(GtkWidget *toplevel, nd_window *w)
{
    if (!toplevel || !w) return;
    GtkStack *stack = g_object_get_data(G_OBJECT(toplevel), "nd-stack");
    if (stack && w->page_root)
        gtk_stack_set_visible_child(stack, w->page_root);
    g_object_set_data(G_OBJECT(toplevel), "nd-window", w);
    nd_window_install_actions(w);
    nd_install_ctx_actions(w);
    GtkBox *strip = g_object_get_data(G_OBJECT(toplevel), "nd-tab-strip");
    if (strip) {
        GPtrArray *tabs = g_object_get_data(G_OBJECT(toplevel), "nd-tabs");
        if (tabs) {
            for (guint i = 0; i < tabs->len; i++) {
                nd_window *t = g_ptr_array_index(tabs, i);
                if (!t->tab_button) continue;
                if (t == w) gtk_widget_add_css_class(t->tab_button, "suggested-action");
                else        gtk_widget_remove_css_class(t->tab_button, "suggested-action");
            }
        }
    }
    nd_window_apply_page_title(w);
}

static void
nd_window_update_tab_label(nd_window *w)
{
    if (!w || !w->tab_label) return;
    char *title = nd_window_current_title(w);
    const char *show = (title && *title) ? title : nd_window_current_url(w);
    if (!show || !*show) show = "New Tab";
    char *valid = g_utf8_make_valid(show, -1);
    char short_label[256];
    g_utf8_strncpy(short_label, valid, 40);
    gtk_label_set_text(GTK_LABEL(w->tab_label), short_label);
    g_free(valid);
    g_free(title);

    if (w->tab_icon && !w->favicon_loaded) {
        const char *url = nd_window_current_url(w);
        const char *icon = "web-browser-symbolic";
        if (url && g_str_has_prefix(url, "about:"))     icon = "nordstjernen";
        else if (url && g_str_has_prefix(url, "file:")) icon = "folder-symbolic";
        gtk_image_set_from_icon_name(GTK_IMAGE(w->tab_icon), icon);
    }
}

static GHashTable *g_live_windows;
static guint       g_next_window_id;

static void
nd_window_mark_alive(nd_window *w)
{
    if (!g_live_windows) g_live_windows = g_hash_table_new(NULL, NULL);
    if (!w->id) w->id = ++g_next_window_id;
    g_hash_table_insert(g_live_windows, GUINT_TO_POINTER(w->id), w);
}

static void
nd_window_mark_dead(nd_window *w)
{
    if (g_live_windows) g_hash_table_remove(g_live_windows, GUINT_TO_POINTER(w->id));
}

nd_window *
nd_window_for_id(guint id)
{
    if (!g_live_windows || !id) return NULL;
    return g_hash_table_lookup(g_live_windows, GUINT_TO_POINTER(id));
}

nd_window *
nd_browser_add_tab(GtkWidget *toplevel, GtkApplication *app, const char *url)
{
    (void)app;
    if (!toplevel) return NULL;
    GtkStack *stack = g_object_get_data(G_OBJECT(toplevel), "nd-stack");
    GtkBox   *strip = g_object_get_data(G_OBJECT(toplevel), "nd-tab-strip");
    GPtrArray *tabs = g_object_get_data(G_OBJECT(toplevel), "nd-tabs");
    if (!stack || !strip || !tabs) return NULL;

    nd_window *w = g_new0(nd_window, 1);
    nd_window_mark_alive(w);
    w->window  = toplevel;
    w->history = g_ptr_array_new();
    w->cursor  = -1;
    w->images  = nd_image_cache_new();
    w->videos  = nd_video_cache_new();
    w->anim    = nd_anim_new();
    w->zoom    = 1.0;

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    w->page_root = page;

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(toolbar, "toolbar");
    gtk_widget_add_css_class(toolbar, "nd-toolbar");
    nd_window_build_toolbar(w, toolbar, g_home_url);
    gtk_box_append(GTK_BOX(page), toolbar);

    nd_window_build_search_bar(w, page);
    nd_window_build_content(w, page);

    char page_name[32];
    g_snprintf(page_name, sizeof page_name, "tab-%p", (void *)w);
    gtk_stack_add_named(stack, page, page_name);

    GtkWidget *tab_button = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(tab_button, "nd-tab");

    GtkWidget *tab_activate = gtk_button_new();
    gtk_widget_add_css_class(tab_activate, "flat");
    GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *tab_icon = gtk_image_new_from_icon_name("web-browser-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(tab_icon), 14);
    gtk_box_append(GTK_BOX(tab_box), tab_icon);
    GtkWidget *tab_label = gtk_label_new("New Tab");
    gtk_label_set_ellipsize(GTK_LABEL(tab_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(tab_label), 24);
    gtk_box_append(GTK_BOX(tab_box), tab_label);
    gtk_button_set_child(GTK_BUTTON(tab_activate), tab_box);
    g_signal_connect(tab_activate, "clicked", G_CALLBACK(on_tab_button_clicked), w);
    gtk_box_append(GTK_BOX(tab_button), tab_activate);

    GtkWidget *close_button = gtk_button_new_from_icon_name("window-close");
    gtk_widget_add_css_class(close_button, "flat");
    gtk_widget_add_css_class(close_button, "nd-tab-close");
    gtk_widget_set_tooltip_text(close_button, "Close tab");
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked), w);
    gtk_box_append(GTK_BOX(tab_button), close_button);
    w->tab_button = tab_button;
    w->tab_icon   = tab_icon;
    w->tab_label  = tab_label;

    GtkWidget *new_tab_btn = g_object_get_data(G_OBJECT(toplevel), "nd-new-tab-button");
    if (new_tab_btn) {
        g_object_ref(new_tab_btn);
        gtk_box_remove(GTK_BOX(strip), new_tab_btn);
    }
    gtk_box_append(GTK_BOX(strip), tab_button);
    if (new_tab_btn) {
        gtk_box_append(GTK_BOX(strip), new_tab_btn);
        g_object_unref(new_tab_btn);
    }

    g_ptr_array_add(tabs, w);

    const char *target = (url && *url) ? url : g_home_url;
    if (target && *target) nd_window_load_url(w, target, ND_LOAD_USER);
    return w;
}

static void
nd_browser_close_tab(nd_window *w)
{
    if (!w || !w->window) return;
    GtkWidget *toplevel = w->window;
    GPtrArray *tabs = g_object_get_data(G_OBJECT(toplevel), "nd-tabs");
    GtkStack *stack = g_object_get_data(G_OBJECT(toplevel), "nd-stack");
    GtkBox   *strip = g_object_get_data(G_OBJECT(toplevel), "nd-tab-strip");
    if (!tabs) return;

    if (tabs->len <= 1) {
        gtk_window_destroy(GTK_WINDOW(toplevel));
        return;
    }

    guint idx = 0;
    gboolean found = g_ptr_array_find(tabs, w, &idx);
    g_ptr_array_remove(tabs, w);
    if (strip && w->tab_button) gtk_box_remove(GTK_BOX(strip), w->tab_button);
    if (stack && w->page_root) gtk_stack_remove(stack, w->page_root);

    nd_window *next = NULL;
    if (found && tabs->len > 0)
        next = g_ptr_array_index(tabs, idx < tabs->len ? idx : tabs->len - 1);
    else if (tabs->len > 0)
        next = g_ptr_array_index(tabs, 0);

    on_window_destroy(NULL, w);

    if (next) nd_browser_set_active(toplevel, next);
}

static void
nd_window_open(GtkApplication *app, const char *startup_url)
{
    GtkWidget *toplevel = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(toplevel), ND_TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(toplevel), "nordstjernen");
    const nd_config *cfg = nd_config_get();
    int win_w = cfg && cfg->window_width_px  > 0 ? cfg->window_width_px  : 1280;
    int win_h = cfg && cfg->window_height_px > 0 ? cfg->window_height_px :  800;
    gtk_window_set_default_size(GTK_WINDOW(toplevel), win_w, win_h);

    GPtrArray *tabs = g_ptr_array_new();
    g_object_set_data(G_OBJECT(toplevel), "nd-tabs", tabs);
    g_signal_connect(toplevel, "destroy", G_CALLBACK(on_toplevel_destroy), tabs);

    GtkWidget *titlebar = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(titlebar), TRUE);
    gtk_widget_add_css_class(titlebar, "nd-titlebar");
    gtk_window_set_titlebar(GTK_WINDOW(toplevel), titlebar);

    GtkWidget *tab_strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_start(tab_strip, 20);
    GtkWidget *strip_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(strip_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(strip_scroll), tab_strip);
    gtk_widget_set_hexpand(strip_scroll, TRUE);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(titlebar), strip_scroll);
    g_object_set_data(G_OBJECT(toplevel), "nd-tab-strip", tab_strip);

    GtkWidget *new_tab_button = gtk_button_new_from_icon_name("tab-new");
    gtk_widget_add_css_class(new_tab_button, "flat");
    gtk_widget_set_tooltip_text(new_tab_button, "New tab (Ctrl+T)");
    g_signal_connect(new_tab_button, "clicked", G_CALLBACK(on_new_tab_clicked), toplevel);
    gtk_box_append(GTK_BOX(tab_strip), new_tab_button);
    g_object_set_data(G_OBJECT(toplevel), "nd-new-tab-button", new_tab_button);

    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_window_set_child(GTK_WINDOW(toplevel), stack);
    g_object_set_data(G_OBJECT(toplevel), "nd-stack", stack);

    const char *url = startup_url;
    if (!url || !*url) url = g_home_url;
    nd_window *first = nd_browser_add_tab(toplevel, app, url);

    gtk_window_maximize(GTK_WINDOW(toplevel));
    gtk_window_present(GTK_WINDOW(toplevel));

    if (first) {
        nd_browser_set_active(toplevel, first);
        gtk_widget_grab_focus(first->url_entry);
    }
}

static gboolean
nd_gtk_prefers_dark(void)
{
    GtkSettings *settings = gtk_settings_get_default();
    if (!settings) return FALSE;
    gboolean prefer_dark = FALSE;
    g_object_get(settings, "gtk-application-prefer-dark-theme", &prefer_dark, NULL);
    if (prefer_dark) return TRUE;
    char *theme = NULL;
    g_object_get(settings, "gtk-theme-name", &theme, NULL);
    gboolean dark = FALSE;
    if (theme) {
        char *lower = g_ascii_strdown(theme, -1);
        if (strstr(lower, "dark")) dark = TRUE;
        g_free(lower);
        g_free(theme);
    }
    return dark;
}

static void
nd_apply_user_prefs_to_css(void)
{
    const nd_config *c = nd_config_get();
    nd_color_scheme_pref cs = c ? c->color_scheme : ND_COLOR_SCHEME_PREF_AUTO;
    nd_css_color_scheme scheme = ND_CSS_COLOR_SCHEME_LIGHT;
    switch (cs) {
    case ND_COLOR_SCHEME_PREF_LIGHT: scheme = ND_CSS_COLOR_SCHEME_LIGHT; break;
    case ND_COLOR_SCHEME_PREF_DARK:  scheme = ND_CSS_COLOR_SCHEME_DARK;  break;
    case ND_COLOR_SCHEME_PREF_AUTO:
    default:
        scheme = nd_gtk_prefers_dark() ? ND_CSS_COLOR_SCHEME_DARK
                                       : ND_CSS_COLOR_SCHEME_LIGHT;
        break;
    }
    nd_css_set_color_scheme(scheme);

    nd_reduced_motion_pref rm = c ? c->reduced_motion : ND_REDUCED_MOTION_PREF_AUTO;
    nd_css_reduced_motion m = ND_CSS_REDUCED_MOTION_NO_PREFERENCE;
    switch (rm) {
    case ND_REDUCED_MOTION_PREF_REDUCE:        m = ND_CSS_REDUCED_MOTION_REDUCE; break;
    case ND_REDUCED_MOTION_PREF_NO_PREFERENCE: m = ND_CSS_REDUCED_MOTION_NO_PREFERENCE; break;
    case ND_REDUCED_MOTION_PREF_AUTO:
    default: {
        GtkSettings *settings = gtk_settings_get_default();
        gboolean enable_anim = TRUE;
        if (settings)
            g_object_get(settings, "gtk-enable-animations", &enable_anim, NULL);
        m = enable_anim ? ND_CSS_REDUCED_MOTION_NO_PREFERENCE
                        : ND_CSS_REDUCED_MOTION_REDUCE;
        break;
    }
    }
    nd_css_set_reduced_motion(m);
}

static void
on_gtk_theme_changed(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec; (void)user_data;
    nd_apply_user_prefs_to_css();
}

static gboolean
nd_screenshot_timer_cb(gpointer user_data)
{
    GtkApplication *app = user_data;
    static int seq = 0;
    GtkWindow *win = gtk_application_get_active_window(app);
    nd_window *w = win ? g_object_get_data(G_OBJECT(win), "nd-window") : NULL;
    if (!w || !g_screenshot_path) return G_SOURCE_REMOVE;
    char *path;
    if (g_screenshot_every_ms > 0) {
        char *dot = strrchr(g_screenshot_path, '.');
        if (dot)
            path = g_strdup_printf("%.*s-%03d%s",
                                   (int)(dot - g_screenshot_path),
                                   g_screenshot_path, seq, dot);
        else
            path = g_strdup_printf("%s-%03d.png", g_screenshot_path, seq);
    } else {
        path = g_strdup(g_screenshot_path);
    }
    seq++;
    if (nd_window_screenshot_to(w, path))
        g_printerr("[screenshot] wrote %s\n", path);
    else
        g_printerr("[screenshot] failed to write %s\n", path);
    g_free(path);
    if (g_screenshot_every_ms > 0 && seq < 60)
        return G_SOURCE_CONTINUE;
    return G_SOURCE_REMOVE;
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    nd_install_icon_search_paths();
    nd_apply_user_prefs_to_css();
    GtkSettings *settings = gtk_settings_get_default();
    if (settings) {
        g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme",
                         G_CALLBACK(on_gtk_theme_changed), NULL);
        g_signal_connect(settings, "notify::gtk-theme-name",
                         G_CALLBACK(on_gtk_theme_changed), NULL);
        g_signal_connect(settings, "notify::gtk-enable-animations",
                         G_CALLBACK(on_gtk_theme_changed), NULL);
    }
    const char *startup_url = g_startup_url_override;
    if (!startup_url || !*startup_url) startup_url = g_getenv("ND_STARTUP_URL");
    nd_window_open(app, startup_url);
    if (g_screenshot_path) {
        int first = g_screenshot_every_ms > 0 ? g_screenshot_every_ms
                                              : g_screenshot_delay_ms;
        if (first > 0)
            g_timeout_add(first, nd_screenshot_timer_cb, app);
    }
}

static int
nd_on_command_line(GApplication *app, GApplicationCommandLine *cmdline, gpointer user_data)
{
    (void)user_data;
    gchar **argv;
    gint argc = 0;
    argv = g_application_command_line_get_arguments(cmdline, &argc);
    g_free(g_startup_url_override);
    g_startup_url_override = NULL;
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (g_str_has_prefix(argv[i], "--screenshot=")) {
            g_free(g_screenshot_path);
            g_screenshot_path = g_strdup(argv[i] + 13);
            if (g_screenshot_delay_ms <= 0) g_screenshot_delay_ms = 3500;
        } else if (g_str_has_prefix(argv[i], "--screenshot-delay=")) {
            g_screenshot_delay_ms = (int)g_ascii_strtoll(argv[i] + 19, NULL, 10);
        } else if (g_str_has_prefix(argv[i], "--screenshot-every=")) {
            g_screenshot_every_ms = (int)g_ascii_strtoll(argv[i] + 19, NULL, 10);
        } else if (argv[i][0] != '-' && !g_startup_url_override) {
            g_startup_url_override = g_strdup(argv[i]);
        }
    }
    g_application_activate(app);
    g_strfreev(argv);
    return 0;
}

static void
on_win_focus_url(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    gtk_widget_grab_focus(w->url_entry);
    gtk_editable_select_region(GTK_EDITABLE(w->url_entry), 0, -1);
}

static void
on_win_reload(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (w->cursor < 0 || w->cursor >= (int)w->history->len) return;
    const char *cur = g_ptr_array_index(w->history, w->cursor);
    nd_window_load_url(w, cur, ND_LOAD_HISTORY);
}

static void
on_win_stop(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (w->search_revealer &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(w->search_revealer))) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(w->search_revealer), FALSE);
        g_free(w->search_query);
        w->search_query = NULL;
        gtk_widget_queue_draw(w->drawing_area);
        return;
    }
    if (w->window && GTK_IS_WINDOW(w->window) &&
        gtk_window_is_fullscreen(GTK_WINDOW(w->window))) {
        gtk_window_unfullscreen(GTK_WINDOW(w->window));
        nd_window_set_status(w, "Exited full screen");
        return;
    }
    gboolean did_something = FALSE;
    if (w->current_fetch) {
        g_cancellable_cancel(w->current_fetch);
        g_clear_object(&w->current_fetch);
        w->fetch_gen++;
        did_something = TRUE;
    }
    if (w->css_cancellable) {
        g_cancellable_cancel(w->css_cancellable);
        did_something = TRUE;
    }
    if (w->js && !nd_js_is_halted(w->js)) {
        nd_js_halt(w->js);
        did_something = TRUE;
    }
    if (did_something) {
        nd_window_set_status(w, "Stopped");
        nd_window_set_busy(w, FALSE);
        if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    }
}

static void
nd_window_after_zoom(nd_window *w)
{
    if (w->layout_tree) { if (w->js) nd_js_set_layout_root(w->js, NULL); nd_box_free(w->layout_tree); w->layout_tree = NULL; nd_selection_clear(&w->selection); w->search_active_box = NULL; }
    if (w->style_table) { if (w->js) nd_js_set_style_table(w->js, NULL); g_hash_table_destroy(w->style_table); w->style_table = NULL; }
    if (w->parsed_doc)  { nd_node_free(w->parsed_doc);  w->parsed_doc  = NULL; }
    w->layout_dirty = TRUE;
    w->focused_input = NULL;
    g_free(w->focused_input_initial);
    w->focused_input_initial = NULL;
    if (w->js)          { nd_js_free(w->js);            w->js          = NULL; }
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
}

static void
on_win_zoom_in(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    w->zoom *= 1.1;
    if (w->zoom > 5.0) w->zoom = 5.0;
    nd_window_after_zoom(w);
}

static void
on_win_zoom_out(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    w->zoom /= 1.1;
    if (w->zoom < 0.4) w->zoom = 0.4;
    nd_window_after_zoom(w);
}

static void
on_win_zoom_reset(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    w->zoom = 1.0;
    nd_window_after_zoom(w);
}

static void
on_win_fullscreen(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (!w->window || !GTK_IS_WINDOW(w->window)) return;
    if (gtk_window_is_fullscreen(GTK_WINDOW(w->window))) {
        gtk_window_unfullscreen(GTK_WINDOW(w->window));
        nd_window_set_status(w, "Exited full screen");
    } else {
        gtk_window_fullscreen(GTK_WINDOW(w->window));
        nd_window_set_status(w, "Full screen — press F11 or Esc to exit");
    }
}

gboolean
on_url_entry_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                         guint keycode, GdkModifierType state,
                         gpointer user_data)
{
    (void)ctrl; (void)keycode; (void)state;
    nd_window *w = user_data;
    if (keyval != GDK_KEY_Escape) return FALSE;
    const char *cur = nd_window_current_url(w);
    if (cur) {
        char *disp = nd_url_to_display(cur);
        gtk_editable_set_text(GTK_EDITABLE(w->url_entry), disp ? disp : cur);
        g_free(disp);
    } else {
        gtk_editable_set_text(GTK_EDITABLE(w->url_entry), "");
    }
    if (w->drawing_area) gtk_widget_grab_focus(w->drawing_area);
    return TRUE;
}

static void
on_win_close(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    nd_browser_close_tab(w);
}

static void
on_win_back(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (w->cursor <= 0) return;
    w->cursor--;
    nd_window_load_url(w, g_ptr_array_index(w->history, w->cursor), ND_LOAD_HISTORY);
}

static void
on_win_forward(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (w->cursor < 0 || w->cursor + 1 >= (int)w->history->len) return;
    w->cursor++;
    nd_window_load_url(w, g_ptr_array_index(w->history, w->cursor), ND_LOAD_HISTORY);
}

static void
on_app_quit(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    GtkApplication *app = user_data;
    g_application_quit(G_APPLICATION(app));
}

static void
nd_window_install_actions(nd_window *w)
{
    static const struct {
        const char *name;
        GCallback   cb;
    } actions[] = {
        { "focus-url", G_CALLBACK(on_win_focus_url) },
        { "reload",    G_CALLBACK(on_win_reload)    },
        { "stop",      G_CALLBACK(on_win_stop)      },
        { "close",     G_CALLBACK(on_win_close)     },
        { "back",      G_CALLBACK(on_win_back)      },
        { "forward",   G_CALLBACK(on_win_forward)   },
        { "find",      G_CALLBACK(on_win_find)      },
        { "zoom-in",   G_CALLBACK(on_win_zoom_in)   },
        { "zoom-out",  G_CALLBACK(on_win_zoom_out)  },
        { "zoom-reset",G_CALLBACK(on_win_zoom_reset)},
        { "print",     G_CALLBACK(on_win_print)     },
        { "save-pdf",  G_CALLBACK(on_win_save_pdf)  },
        { "save-html", G_CALLBACK(on_win_save_html) },
        { "screenshot", G_CALLBACK(on_win_screenshot) },
        { "open-console", G_CALLBACK(on_win_open_console) },
        { "fullscreen", G_CALLBACK(on_win_fullscreen) },
    };
    GActionMap *map = G_ACTION_MAP(w->window);
    for (gsize i = 0; i < G_N_ELEMENTS(actions); i++) {
        g_action_map_remove_action(map, actions[i].name);
        GSimpleAction *a = g_simple_action_new(actions[i].name, NULL);
        g_signal_connect(a, "activate", actions[i].cb, w);
        g_action_map_add_action(map, G_ACTION(a));
        g_object_unref(a);
    }
}

static void
nd_install_css(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    static const char css[] =
        "headerbar.nd-titlebar { min-height: 24px; padding: 0 2px; }\n"
        "headerbar.nd-titlebar button { min-height: 20px; min-width: 20px; padding: 0 4px; }\n"
        "headerbar.nd-titlebar windowcontrols button { min-height: 20px; min-width: 20px; }\n"
        "button.nd-tab { min-height: 18px; padding: 0 6px; }\n"
        "button.nd-tab-close { min-height: 16px; min-width: 16px; padding: 0; }\n"
        "box.nd-toolbar { padding: 4px 6px; border-bottom: 1px solid alpha(currentColor, 0.12); }\n"
        "box.nd-toolbar > button { min-height: 28px; min-width: 28px; padding: 0 6px; }\n"
        "box.nd-toolbar > entry { min-height: 28px; margin: 0 4px; }\n"
        "window.nd-about box.nd-about-content { padding: 24px 36px; }\n"
        "window.nd-about label.nd-about-name {\n"
        "  font-size: 20pt; font-weight: 600; margin-top: 8px;\n"
        "}\n"
        "window.nd-about label.nd-about-version {\n"
        "  font-size: 11pt; color: alpha(currentColor, 0.7);\n"
        "}\n"
        "window.nd-about label.nd-about-tagline {\n"
        "  font-style: italic; color: alpha(currentColor, 0.65);\n"
        "  margin-top: 12px;\n"
        "}\n"
        "window.nd-about label.nd-about-copy {\n"
        "  font-size: 9pt; color: alpha(currentColor, 0.55);\n"
        "  margin-top: 16px;\n"
        "}\n";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, css);
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static void
nd_install_icon_search_paths(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
    if (!theme) return;
    if (!g_self_exe) return;
    char *exe_dir = g_path_get_dirname(g_self_exe);
    if (!exe_dir) return;
    const char *try_rel[] = {
        "share/icons",
        "../share/icons",
        "../../data/icons",
        NULL,
    };
    for (int i = 0; try_rel[i]; i++) {
        char *p = g_build_filename(exe_dir, try_rel[i], NULL);
        gtk_icon_theme_add_search_path(theme, p);
        g_free(p);
    }
    g_free(exe_dir);
}

static void
nd_install_actions(GtkApplication *app)
{
    nd_install_icon_search_paths();
    nd_install_css();

    GSimpleAction *new_window = g_simple_action_new("new-window", NULL);
    g_signal_connect(new_window, "activate", G_CALLBACK(on_app_new_window), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(new_window));
    g_object_unref(new_window);

    GSimpleAction *new_tab = g_simple_action_new("new-tab", NULL);
    g_signal_connect(new_tab, "activate", G_CALLBACK(on_app_new_tab), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(new_tab));
    g_object_unref(new_tab);

    GSimpleAction *quit = g_simple_action_new("quit", NULL);
    g_signal_connect(quit, "activate", G_CALLBACK(on_app_quit), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit));
    g_object_unref(quit);

    nd_setup_bookmarks_watch(app);

    const struct {
        const char *action;
        const char *accels[3];
    } binds[] = {
        { "app.new-window", { "<Primary>n", NULL, NULL } },
        { "app.new-tab",    { "<Primary>t", NULL, NULL } },
        { "app.quit",       { "<Primary>q", NULL, NULL } },
        { "win.focus-url",  { "<Primary>l", "F6", NULL } },
        { "win.reload",     { "<Primary>r", "F5", NULL } },
        { "win.stop",       { "Escape", NULL, NULL } },
        { "win.close",      { "<Primary>w", NULL, NULL } },
        { "win.back",       { "<Alt>Left", NULL, NULL } },
        { "win.forward",    { "<Alt>Right", NULL, NULL } },
        { "win.find",       { "<Primary>f", NULL, NULL } },
        { "win.zoom-in",    { "<Primary>plus", "<Primary>equal", NULL } },
        { "win.zoom-out",   { "<Primary>minus", NULL, NULL } },
        { "win.zoom-reset", { "<Primary>0", NULL, NULL } },
        { "win.print",      { "<Primary>p", NULL, NULL } },
        { "win.save-pdf",   { "<Primary><Shift>s", NULL, NULL } },
        { "win.screenshot", { "<Primary><Shift>p", NULL, NULL } },
        { "win.open-console", { "<Primary><Shift>j", NULL, NULL } },
        { "win.fullscreen", { "F11", NULL, NULL } },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(binds); i++)
        gtk_application_set_accels_for_action(app, binds[i].action, binds[i].accels);
}

static void
on_bookmarks_file_changed(GFileMonitor *mon, GFile *file, GFile *other,
                          GFileMonitorEvent event, gpointer user_data)
{
    (void)mon; (void)file; (void)other;
    GtkApplication *app = user_data;
    if (event != G_FILE_MONITOR_EVENT_CHANGED &&
        event != G_FILE_MONITOR_EVENT_CREATED &&
        event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
        return;
    (void)app;
    nd_bookmarks_free(g_bookmarks);
    g_bookmarks = nd_bookmarks_load();
}

static void
nd_setup_bookmarks_watch(GtkApplication *app)
{
    char *path = g_build_filename(g_get_user_config_dir(),
                                  ND_APP_DIR_NAME, "bookmarks.txt", NULL);
    GFile *file = g_file_new_for_path(path);
    g_free(path);
    GError *err = NULL;
    g_bookmarks_monitor = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, &err);
    g_object_unref(file);
    if (g_bookmarks_monitor) {
        g_signal_connect(g_bookmarks_monitor, "changed",
                         G_CALLBACK(on_bookmarks_file_changed), app);
    } else if (err) {
        g_clear_error(&err);
    }
}

static void
init_self_exe(const char *argv0)
{
#ifdef __linux__
    char *resolved = g_file_read_link("/proc/self/exe", NULL);
    if (resolved) { g_self_exe = resolved; return; }
#endif
#ifdef __APPLE__
    {
        uint32_t size = 0;
        _NSGetExecutablePath(NULL, &size);
        if (size > 0 && size <= 32768) {
            char *raw = g_malloc(size);
            if (_NSGetExecutablePath(raw, &size) == 0) {
                char *real = realpath(raw, NULL);
                if (real) {
                    g_self_exe = g_strdup(real);
                    free(real);
                } else {
                    g_self_exe = g_strdup(raw);
                }
            }
            g_free(raw);
            if (g_self_exe) return;
        }
    }
#endif
#ifdef G_OS_WIN32
    {
        DWORD cap = MAX_PATH;
        wchar_t *buf = g_new(wchar_t, cap);
        DWORD n = GetModuleFileNameW(NULL, buf, cap);
        while (n >= cap && cap < 32768) {
            cap *= 2;
            wchar_t *bigger = g_renew(wchar_t, buf, cap);
            buf = bigger;
            n = GetModuleFileNameW(NULL, buf, cap);
        }
        if (n > 0 && n < cap)
            g_self_exe = g_utf16_to_utf8((gunichar2 *)buf, -1, NULL, NULL, NULL);
        g_free(buf);
        if (g_self_exe) return;
    }
#endif
    if (argv0) {
        if (g_path_is_absolute(argv0))
            g_self_exe = g_strdup(argv0);
        else
            g_self_exe = g_find_program_in_path(argv0);
        if (!g_self_exe) g_self_exe = g_strdup(argv0);
    }
    if (!g_self_exe)
        g_debug("init_self_exe: could not resolve own binary path; "
                "new-window will run in-process");
}

const char *nd_app_self_exe(void);

const char *
nd_app_self_exe(void)
{
    return g_self_exe;
}

static GLogWriterOutput
nd_log_writer(GLogLevelFlags log_level,
              const GLogField *fields, gsize n_fields,
              gpointer user_data)
{
    (void)user_data;
    const char *captured = NULL;
    const char *captured_domain = NULL;
    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].value) {
            const char *m = fields[i].value;
            captured = m;
            if (strstr(m, "win32 session dbus binary not found"))
                return G_LOG_WRITER_HANDLED;
            if (strstr(m, "but sizes must be >= 0"))
                return G_LOG_WRITER_HANDLED;
            if (strstr(m, "Baselines must be inside the widget size"))
                return G_LOG_WRITER_HANDLED;
            if (strstr(m, "without a current allocation"))
                return G_LOG_WRITER_HANDLED;
            if (strstr(m, "No IM module matching GTK_IM_MODULE="))
                return G_LOG_WRITER_HANDLED;
        }
        if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0 && fields[i].value)
            captured_domain = fields[i].value;
    }
    if (captured) {
        nd_dlog_level lvl = ND_DLOG_INFO;
        if (log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
            lvl = ND_DLOG_ERROR;
        else if (log_level & G_LOG_LEVEL_WARNING)
            lvl = ND_DLOG_WARN;
        nd_debug_log_emit(lvl, captured_domain ? captured_domain : "glib",
                          "%s", captured);
    }
    return g_log_writer_default(log_level, fields, n_fields, user_data);
}

#ifdef G_OS_WIN32

__declspec(dllimport) HRESULT __stdcall
SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID);

static void
nd_win32_set_app_id(void)
{
    (void)SetCurrentProcessExplicitAppUserModelID(L"Nordstjernen.Browser");
}

static void
nd_win32_anchor_gtk_data(void)
{
    if (!g_self_exe) return;
    char *dir = g_path_get_dirname(g_self_exe);
    if (!dir) return;
    char *share_dir = g_build_filename(dir, "share", NULL);
    if (g_file_test(share_dir, G_FILE_TEST_IS_DIR)) {
        if (!g_getenv("GTK_DATA_PREFIX")) g_setenv("GTK_DATA_PREFIX", dir, TRUE);
        if (!g_getenv("GTK_EXE_PREFIX"))  g_setenv("GTK_EXE_PREFIX",  dir, TRUE);
        if (!g_getenv("XDG_DATA_DIRS"))   g_setenv("XDG_DATA_DIRS", share_dir, TRUE);
    }
    g_free(share_dir);
    if (!g_getenv("GDK_PIXBUF_MODULE_FILE")) {
        char *loaders = g_build_filename(dir,
            "lib", "gdk-pixbuf-2.0", "2.10.0", "loaders.cache", NULL);
        if (g_file_test(loaders, G_FILE_TEST_EXISTS))
            g_setenv("GDK_PIXBUF_MODULE_FILE", loaders, TRUE);
        g_free(loaders);
    }
    {
        char *ca = g_build_filename(dir,
            "etc", "ssl", "certs", "ca-bundle.crt", NULL);
        if (g_file_test(ca, G_FILE_TEST_EXISTS)) {
            if (!g_getenv("CURL_CA_BUNDLE")) g_setenv("CURL_CA_BUNDLE", ca, TRUE);
            if (!g_getenv("SSL_CERT_FILE"))  g_setenv("SSL_CERT_FILE",  ca, TRUE);
        }
        g_free(ca);
    }
    g_free(dir);
}

static gboolean
nd_win32_args_need_console(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (g_strcmp0(argv[i], "--headless")     == 0 ||
            g_strcmp0(argv[i], "--print-config") == 0 ||
            g_str_has_prefix(argv[i], "--dump=") ||
            g_str_has_prefix(argv[i], "--url=")  ||
            g_str_has_prefix(argv[i], "--viewport=") ||
            g_str_has_prefix(argv[i], "--inspect=") ||
            g_str_has_prefix(argv[i], "--inspect-at=") ||
            g_str_has_prefix(argv[i], "--settle-ms="))
            return TRUE;
    }
    return FALSE;
}

static gboolean
nd_win32_fd_is_bound(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

static void
nd_win32_attach_parent_console(void)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE *fp;
    if (!nd_win32_fd_is_bound(_fileno(stdout)))
        (void)freopen_s(&fp, "CONOUT$", "w", stdout);
    if (!nd_win32_fd_is_bound(_fileno(stderr)))
        (void)freopen_s(&fp, "CONOUT$", "w", stderr);
    if (!nd_win32_fd_is_bound(_fileno(stdin)))
        (void)freopen_s(&fp, "CONIN$",  "r", stdin);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}
#endif

void nd_main_on_font_loaded(const char *family, gpointer user_data);

void
nd_main_on_font_loaded(const char *family, gpointer user_data)
{
    (void)family;
    GtkApplication *app = user_data;
    if (!app) return;
    GList *windows = gtk_application_get_windows(app);
    for (GList *l = windows; l; l = l->next) {
        GtkWindow *win = l->data;
        nd_window *w = g_object_get_data(G_OBJECT(win), "nd-window");
        if (!w) continue;
        w->layout_dirty = TRUE;
        if (w->layout_tree) {
            if (w->js) nd_js_set_layout_root(w->js, NULL);
            nd_box_free(w->layout_tree);
            w->layout_tree = NULL;
            nd_selection_clear(&w->selection);
            w->search_active_box = NULL;
        }
        if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    }
}

#ifdef __linux__
static gboolean
nd_linux_has_gpu_render_node(void)
{
    GDir *dir = g_dir_open("/dev/dri", 0, NULL);
    if (!dir) return FALSE;
    const char *name;
    gboolean found = FALSE;
    while ((name = g_dir_read_name(dir))) {
        if (g_str_has_prefix(name, "renderD") || g_str_has_prefix(name, "card")) {
            found = TRUE;
            break;
        }
    }
    g_dir_close(dir);
    return found;
}
#endif

static void
nd_apply_gsk_renderer_auto(void)
{
#ifdef __linux__
    if (!nd_linux_has_gpu_render_node()) {
        g_setenv("GSK_RENDERER", "cairo", TRUE);
        g_message("no GPU detected; using the software (cairo) renderer");
    }
#endif
}

static void
nd_apply_gsk_renderer(const char *pref)
{
    if (g_getenv("GSK_RENDERER")) return;
    if (!pref || !*pref ||
        g_ascii_strcasecmp(pref, "auto")    == 0 ||
        g_ascii_strcasecmp(pref, "default") == 0 ||
        g_ascii_strcasecmp(pref, "system")  == 0) {
        nd_apply_gsk_renderer_auto();
        return;
    }
    static const char *const known[] = {
        "gl", "ngl", "opengl", "vulkan", "cairo", "help",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(known); i++) {
        if (g_ascii_strcasecmp(pref, known[i]) == 0) {
            g_setenv("GSK_RENDERER", known[i], TRUE);
            return;
        }
    }
    g_warning("ignoring unknown gsk_renderer '%s' "
              "(expected one of: auto, gl, ngl, vulkan, cairo)", pref);
}

int
main(int argc, char **argv)
{
#ifdef G_OS_WIN32
    if (nd_win32_args_need_console(argc, argv))
        nd_win32_attach_parent_console();
#endif
    if (!nd_security_refuse_root()) return 77;
    nd_security_win32_mitigations_init();
    init_self_exe(argc > 0 ? argv[0] : NULL);
    for (int i = 1; i < argc; i++) {
        const char *p = NULL;
        if (g_str_has_prefix(argv[i], "--dump=")) {
            const char *v = argv[i] + 7;
            if      (g_str_has_prefix(v, "png:")) p = v + 4;
            else if (g_str_has_prefix(v, "pdf:")) p = v + 4;
        } else if (g_str_has_prefix(argv[i], "--screenshot=")) {
            p = argv[i] + 13;
        }
        if (!p || !*p || *p == '-') continue;
        char *dir = g_path_get_dirname(p);
        if (dir && *dir) {
            g_mkdir_with_parents(dir, 0700);
            nd_security_add_writable_dir(dir);
        }
        g_free(dir);
    }
    nd_media_broker_start();
    nd_media_set_open_uri_handler(nd_media_open_uri_cb);
    nd_security_sandbox_init(g_self_exe);
    nd_security_seccomp_init();
    nd_config_init();
    nd_debug_log_init();
    g_log_set_writer_func(nd_log_writer, NULL, NULL);
#ifdef G_OS_WIN32
    nd_win32_set_app_id();
    nd_win32_anchor_gtk_data();
#endif

    gboolean headless = FALSE;
    gboolean dump_set = FALSE;
    const char *proxy_override = NULL;
    const char *gsk_renderer_override = NULL;
    nd_headless_opts hopts = {
        .url = NULL,
        .dump = ND_DUMP_TEXT,
        .out_path = NULL,
        .viewport_width = 1000,
        .settle_ms = 200,
        .time_ms = 1000,
    };
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--proxy=")) {
            proxy_override = argv[i] + 8;
            nd_net_set_proxy_override(proxy_override);
            continue;
        }
        if (g_str_has_prefix(argv[i], "--gsk-renderer=")) {
            gsk_renderer_override = argv[i] + 15;
            continue;
        }
        if (g_strcmp0(argv[i], "--print-config") == 0) {
            char *dump = nd_config_dump();
            fputs(dump, stdout);
            g_free(dump);
            nd_config_shutdown();
            return 0;
        }
        if (g_strcmp0(argv[i], "--headless") == 0) {
            headless = TRUE;
        } else if (g_str_has_prefix(argv[i], "--dump=")) {
            const char *v = argv[i] + 7;
            dump_set = TRUE;
            if      (g_strcmp0(v, "text")   == 0) hopts.dump = ND_DUMP_TEXT;
            else if (g_strcmp0(v, "dom")    == 0) hopts.dump = ND_DUMP_DOM;
            else if (g_strcmp0(v, "layout") == 0) hopts.dump = ND_DUMP_LAYOUT;
            else if (g_str_has_prefix(v, "png:"))   { hopts.dump = ND_DUMP_PNG; hopts.out_path = v + 4; }
            else if (g_str_has_prefix(v, "pdf:"))   { hopts.dump = ND_DUMP_PDF; hopts.out_path = v + 4; }
        } else if (g_str_has_prefix(argv[i], "--viewport=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 11, &end, 10);
            if (end != argv[i] + 11 && *end == '\0' && n > 0 && n < 100000)
                hopts.viewport_width = (int)n;
        } else if (g_str_has_prefix(argv[i], "--viewport-height=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 18, &end, 10);
            if (end != argv[i] + 18 && *end == '\0' && n > 0 && n < 100000)
                hopts.viewport_height = (int)n;
        } else if (g_str_has_prefix(argv[i], "--settle-ms=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 12, &end, 10);
            if (end != argv[i] + 12 && *end == '\0' && n >= 0 && n < 600000)
                hopts.settle_ms = (int)n;
        } else if (g_str_has_prefix(argv[i], "--time-ms=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 10, &end, 10);
            if (end != argv[i] + 10 && *end == '\0' && n >= 0 && n < 600000)
                hopts.time_ms = (int)n;
        } else if (g_str_has_prefix(argv[i], "--act=")) {
            hopts.actions = argv[i] + 6;
        } else if (g_str_has_prefix(argv[i], "--inspect=")) {
            hopts.inspect = argv[i] + 10;
        } else if (g_str_has_prefix(argv[i], "--inspect-at=")) {
            hopts.inspect_at = argv[i] + 13;
        } else if (g_strcmp0(argv[i], "--debug") == 0) {
            hopts.debug_levels = nd_headless_debug_mask("all");
        } else if (g_str_has_prefix(argv[i], "--debug=")) {
            hopts.debug_levels = nd_headless_debug_mask(argv[i] + 8);
        } else if (g_str_has_prefix(argv[i], "--url=")) {
            hopts.url = argv[i] + 6;
        } else if (argv[i][0] != '-' && !hopts.url) {
            hopts.url = argv[i];
        }
    }
    if (!dump_set && (hopts.inspect || hopts.inspect_at))
        hopts.dump = ND_DUMP_NONE;
    if (headless) {
        nd_net_init();
        nd_net_set_allow_file_urls(TRUE);
        if (hopts.debug_levels & (1u << ND_DLOG_NET))
            nd_net_set_log_fetches(TRUE);
        nd_cache_init();
        nd_bcache_init();
        nd_font_init();
        char *file_url = NULL;
        if (hopts.url && !strstr(hopts.url, "://") &&
            !g_str_has_prefix(hopts.url, "about:") &&
            !g_str_has_prefix(hopts.url, "data:") &&
            g_file_test(hopts.url, G_FILE_TEST_EXISTS)) {
            char *abs = g_canonicalize_filename(hopts.url, NULL);
            file_url = g_filename_to_uri(abs, NULL, NULL);
            g_free(abs);
            if (file_url) hopts.url = file_url;
        }
        int rc = nd_headless_run(&hopts);
        g_free(file_url);
        nd_bcache_shutdown();
        nd_cache_shutdown();
        nd_net_shutdown();
        nd_config_shutdown();
        return rc;
    }

    {
        const nd_config *cfg = nd_config_get();
        g_home_url = g_strdup(cfg && cfg->home_url ? cfg->home_url : "");
        nd_apply_gsk_renderer(gsk_renderer_override ? gsk_renderer_override
                              : (cfg ? cfg->gsk_renderer : NULL));
    }
    nd_net_init();
    nd_cache_init();
    nd_bcache_init();
    nd_font_init();
    g_bookmarks = nd_bookmarks_load();

    GApplicationFlags app_flags = G_APPLICATION_HANDLES_COMMAND_LINE |
                                  G_APPLICATION_NON_UNIQUE;
    GtkApplication *app = gtk_application_new(ND_APP_ID, app_flags);
    nd_font_set_loaded_cb(nd_main_on_font_loaded, app);
    g_signal_connect(app, "startup",      G_CALLBACK(nd_install_actions), NULL);
    g_signal_connect(app, "activate",     G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(nd_on_command_line), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    g_clear_object(&g_bookmarks_monitor);
    nd_bookmarks_free(g_bookmarks);
    g_bookmarks = NULL;
    g_free(g_startup_url_override);
    g_free(g_self_exe);
    g_self_exe = NULL;
    g_free(g_home_url);
    g_home_url = NULL;
    nd_font_shutdown();
    nd_bcache_shutdown();
    nd_cache_shutdown();
    nd_net_shutdown();
    nd_config_shutdown();
    return status;
}
