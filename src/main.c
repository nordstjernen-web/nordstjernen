/* Nordstjernen — GTK 4 application shell.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include <gtk/gtk.h>
#include <string.h>

#include <cairo-pdf.h>
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
#include "bookmarks.h"
#include "cache.h"
#include "compatibility.h"
#include "config.h"
#include "css.h"
#include "env.h"
#include "headless.h"
#include "html.h"
#include "image.h"
#include "video.h"
#include "js.h"
#include "layout.h"
#include "net.h"
#include "paint.h"
#include "security.h"
#include "font.h"
#include "pdf.h"
#include "selection.h"
#include "version.h"
#include "window.h"

#define ND_APP_ID     "com.nordstjernen.Browser"
#define ND_TITLE      "Nordstjernen"

static char         *g_startup_url_override;
static char         *g_self_exe;
static char         *g_home_url;
static nd_bookmarks *g_bookmarks;
static GFileMonitor *g_bookmarks_monitor;
static char         *g_context_menu_link;
static char         *g_context_menu_image;
static char         *g_context_menu_selection;

static double
nd_layout_viewport(void)
{
    const nd_config *c = nd_config_get();
    return c && c->layout_viewport_px > 0 ? (double)c->layout_viewport_px : 1000.0;
}

typedef enum nd_load_source {
    ND_LOAD_USER,
    ND_LOAD_HISTORY,
} nd_load_source;

static void nd_window_load_url(nd_window *w, const char *raw_url, nd_load_source src);
static void nd_window_sync_selection_to_js(nd_window *w);
static void nd_window_record_final_url(nd_window *w, const nd_response *resp);
static void nd_window_set_busy(nd_window *w, gboolean busy);
static void nd_window_render(nd_window *w);
static void nd_window_clear_cache(nd_window *w);
static void nd_window_update_nav_state(nd_window *w);
static void nd_install_icon_search_paths(void);
static void nd_window_open(GtkApplication *app, const char *startup_url);
static void nd_spawn_window(GtkApplication *app, const char *url);
static nd_window *nd_browser_add_tab(GtkWidget *toplevel, GtkApplication *app,
                                     const char *url);
static void nd_browser_close_tab(nd_window *w);
static void nd_browser_set_active(GtkWidget *toplevel, nd_window *w);
static void nd_window_update_tab_label(nd_window *w);
static void nd_setup_bookmarks_watch(GtkApplication *app);
static void nd_window_kick_image_loads(nd_window *w);
static void nd_window_kick_video_loads(nd_window *w);
static void nd_window_kick_favicon(nd_window *w);
static const char *nd_window_current_url(nd_window *w);
static char       *nd_window_current_title(nd_window *w);
static void        nd_window_js_log(const char *line, gpointer user_data);
static void        nd_window_js_soft_nav(const char *url, gboolean replace,
                                         gpointer user_data);
static void nd_window_install_actions(nd_window *w);
static void nd_window_kick_stylesheet_loads(nd_window *w);
static gboolean mixed_content_blocked(nd_window *w, const char *abs_url,
                                      const char *kind);
static gboolean csp_blocked(nd_window *w, nd_csp_kind kind, const char *abs_url,
                            const char *kind_word);
static gboolean nd_window_subresource_blocked(nd_window *w, const char *abs_url,
                                              nd_csp_kind csp_kind,
                                              const char *kind_word);
static void nd_window_apply_page_title(nd_window *w);
static void nd_window_apply_meta_refresh(nd_window *w);
static void nd_clear_radio_group(nd_node *root, const char *name,
                                 const nd_node *keep);
static gboolean nd_input_is_text_like(const nd_node *n);
static void nd_window_set_focused_input(nd_window *w, nd_node *target);
static void nd_window_open_select_popover(nd_window *w, nd_node *select_node,
                                          double x, double y);
static void nd_window_open_file_chooser(nd_window *w, nd_node *input);
static void nd_window_maybe_submit_form(nd_window *w, const nd_node *clicked);
static char *nd_resolve_url(const nd_window *w, const char *href);
static void nd_on_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data);
static void     nd_window_mark_alive(nd_window *w);
static void     nd_window_mark_dead(nd_window *w);
static gboolean nd_window_alive(nd_window *w);

static void
nd_window_set_status(nd_window *w, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void
nd_window_set_status(nd_window *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(w->status_label), msg);
    g_free(msg);
}

static void
nd_window_set_body_text(nd_window *w, const char *text, gssize len)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->text_view));
    gtk_text_buffer_set_text(buf, text ? text : "", (int)len);
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
    g_clear_handle_id(&w->video_tick_source, g_source_remove);
    if (w->audios) {
        nd_audio_cache_free(w->audios);
        w->audios = nd_audio_cache_new();
    }
    g_clear_pointer(&w->last_body, g_free);
    w->last_body_len = 0;
    g_clear_pointer(&w->last_content_type, g_free);
    if (w->csp) { if (w->js) nd_js_set_csp(w->js, NULL); nd_csp_free(w->csp); w->csp = NULL; }
    if (w->pdf) { nd_pdf_free(w->pdf); w->pdf = NULL; }
    nd_window_drop_layout(w);
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
    if (!w->pending_fragment || !w->layout_tree || !w->render_vadj) return;
    const nd_box *target = nd_box_find_by_id(w->layout_tree, w->pending_fragment);
    if (!target) return;
    nd_adjustment_scroll_to(w->render_vadj, target->y);
    g_free(w->pending_fragment);
    w->pending_fragment = NULL;
}

static void
nd_window_console_append(nd_window *w, const char *line)
{
    if (!w || !w->console.buffer || !line) return;
    GDateTime *now = g_date_time_new_now_local();
    char *ts = g_strdup_printf("%02d:%02d:%02d  ",
                               g_date_time_get_hour(now),
                               g_date_time_get_minute(now),
                               g_date_time_get_second(now));
    g_date_time_unref(now);
    const char *tag = NULL;
    if (g_str_has_prefix(line, "[error]")) tag = "error";
    else if (g_str_has_prefix(line, "[warn]")) tag = "warn";
    else if (g_str_has_prefix(line, "[alert]")) tag = "alert";
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(w->console.buffer, &end);
    GtkTextMark *start_mark = gtk_text_buffer_create_mark(w->console.buffer,
                                                          NULL, &end, TRUE);
    gtk_text_buffer_insert(w->console.buffer, &end, ts, -1);
    GtkTextIter ts_start;
    gtk_text_buffer_get_iter_at_mark(w->console.buffer, &ts_start, start_mark);
    GtkTextIter ts_end_iter;
    gtk_text_buffer_get_end_iter(w->console.buffer, &ts_end_iter);
    gtk_text_buffer_apply_tag_by_name(w->console.buffer, "timestamp",
                                      &ts_start, &ts_end_iter);
    gtk_text_buffer_delete_mark(w->console.buffer, start_mark);

    GtkTextIter body_start;
    gtk_text_buffer_get_end_iter(w->console.buffer, &body_start);
    GtkTextMark *body_mark = gtk_text_buffer_create_mark(w->console.buffer,
                                                         NULL, &body_start, TRUE);
    char *with_nl = g_strconcat(line, "\n", NULL);
    GtkTextIter ins_end;
    gtk_text_buffer_get_end_iter(w->console.buffer, &ins_end);
    gtk_text_buffer_insert(w->console.buffer, &ins_end, with_nl, -1);
    g_free(with_nl);
    if (tag) {
        GtkTextIter line_start, line_end;
        gtk_text_buffer_get_iter_at_mark(w->console.buffer, &line_start, body_mark);
        gtk_text_buffer_get_end_iter(w->console.buffer, &line_end);
        gtk_text_buffer_apply_tag_by_name(w->console.buffer, tag,
                                          &line_start, &line_end);
    }
    gtk_text_buffer_delete_mark(w->console.buffer, body_mark);
    g_free(ts);
}

static void
nd_window_js_log(const char *line, gpointer user_data)
{
    nd_window *w = user_data;
    if (!w || !line) return;
    nd_window_set_status(w, "JS: %s", line);
    nd_window_console_append(w, line);
}

static gboolean
nd_window_js_relayout_now(gpointer user_data)
{
    nd_window *w = user_data;
    if (!w) return G_SOURCE_REMOVE;
    w->js_relayout_idle_id = 0;
    nd_window_drop_layout(w);
    w->layout_dirty = TRUE;
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    nd_window_apply_page_title(w);
    return G_SOURCE_REMOVE;
}

static void
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
nd_clear_radio_group(nd_node *root, const char *name, const nd_node *keep)
{
    if (!root) return;
    if (nd_node_is_element_named(root, "input") && root != keep) {
        const char *type = nd_element_get_attr(root, "type");
        const char *grp = nd_element_get_attr(root, "name");
        if (type && grp && g_ascii_strcasecmp(type, "radio") == 0 &&
            strcmp(grp, name) == 0)
            nd_element_remove_attr(root, "checked");
    }
    for (nd_node *c = root->first_child; c; c = c->next_sibling)
        nd_clear_radio_group(c, name, keep);
}

static gboolean
is_submit_trigger(const nd_node *n)
{
    if (!n || n->kind != ND_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "button") == 0) {
        const char *type = nd_element_get_attr(n, "type");
        return !type || g_ascii_strcasecmp(type, "submit") == 0;
    }
    if (strcmp(n->name, "input") == 0) {
        const char *type = nd_element_get_attr(n, "type");
        return type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                        g_ascii_strcasecmp(type, "image") == 0);
    }
    return FALSE;
}

static void
append_form_field(GString *query, gboolean *first, const char *name, const char *value)
{
    g_autofree char *ename = g_uri_escape_string(name, NULL, FALSE);
    g_autofree char *evalue = g_uri_escape_string(value ? value : "", NULL, FALSE);
    if (!*first) g_string_append_c(query, '&');
    g_string_append(query, ename);
    g_string_append_c(query, '=');
    g_string_append(query, evalue);
    *first = FALSE;
}

static gboolean
form_has_file_upload(const nd_node *n)
{
    if (!n) return FALSE;
    if (nd_node_is_element_named(n, "input")) {
        const char *type = nd_element_get_attr(n, "type");
        if (type && g_ascii_strcasecmp(type, "file") == 0 &&
            nd_element_get_attr(n, "data-nd-file-path"))
            return TRUE;
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        if (form_has_file_upload(c)) return TRUE;
    return FALSE;
}

static char *
nd_make_multipart_boundary(void)
{
    guint32 r[4];
    if (!nd_security_csprng_fill(r, sizeof r)) {
        r[0] = g_random_int(); r[1] = g_random_int();
        r[2] = g_random_int(); r[3] = g_random_int();
    }
    return g_strdup_printf("----NordstjernenFormBoundary%08x%08x%08x%08x",
                           r[0], r[1], r[2], r[3]);
}

static void
multipart_append_field(GString *body, const char *boundary,
                       const char *name, const char *value)
{
    g_string_append_printf(body, "--%s\r\n", boundary);
    g_string_append_printf(body,
        "Content-Disposition: form-data; name=\"%s\"\r\n\r\n",
        name ? name : "");
    if (value) g_string_append(body, value);
    g_string_append(body, "\r\n");
}

static gboolean
multipart_append_file(GString *body, const char *boundary,
                      const char *name, const char *path)
{
    if (!path || !*path) {
        g_string_append_printf(body, "--%s\r\n", boundary);
        g_string_append_printf(body,
            "Content-Disposition: form-data; name=\"%s\"; filename=\"\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n\r\n",
            name ? name : "");
        return TRUE;
    }
    char *contents = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path, &contents, &len, &err)) {
        if (err) g_error_free(err);
        return FALSE;
    }
    const char *base = strrchr(path, '/');
#ifdef G_OS_WIN32
    const char *base_w = strrchr(path, '\\');
    if (!base || (base_w && base_w > base)) base = base_w;
#endif
    const char *fname = base ? base + 1 : path;
    g_autofree char *mime = g_content_type_guess(path, (const guchar *)contents,
                                                  len < 4096 ? len : 4096, NULL);
    g_autofree char *type = mime ? g_content_type_get_mime_type(mime) : NULL;
    g_string_append_printf(body, "--%s\r\n", boundary);
    g_string_append_printf(body,
        "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n\r\n",
        name ? name : "", fname,
        type && *type ? type : "application/octet-stream");
    g_string_append_len(body, contents, (gssize)len);
    g_string_append(body, "\r\n");
    g_free(contents);
    return TRUE;
}

static void
form_collect_multipart(const nd_node *n, GString *body, const char *boundary,
                       const nd_node *submitter)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && n->name) {
        gboolean is_input    = strcmp(n->name, "input") == 0;
        gboolean is_textarea = strcmp(n->name, "textarea") == 0;
        gboolean is_select   = strcmp(n->name, "select") == 0;
        gboolean is_button   = strcmp(n->name, "button") == 0;
        if (is_input || is_textarea || is_select || is_button) {
            const char *name = nd_element_get_attr(n, "name");
            if (!name || !*name) goto recurse_mp;
            if (nd_element_get_attr(n, "disabled")) goto recurse_mp;
            if (is_input) {
                const char *type = nd_element_get_attr(n, "type");
                if (type && g_ascii_strcasecmp(type, "file") == 0) {
                    const char *path = nd_element_get_attr(n, "data-nd-file-path");
                    multipart_append_file(body, boundary, name, path);
                    goto recurse_mp;
                }
                if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                             g_ascii_strcasecmp(type, "radio") == 0)) {
                    if (!nd_element_get_attr(n, "checked")) goto recurse_mp;
                }
                if (type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                             g_ascii_strcasecmp(type, "image") == 0)) {
                    if (n == submitter) {
                        const char *v = nd_element_get_attr(n, "value");
                        multipart_append_field(body, boundary, name,
                                               v ? v : "Submit");
                    }
                    goto recurse_mp;
                }
                if (type && (g_ascii_strcasecmp(type, "button") == 0 ||
                             g_ascii_strcasecmp(type, "reset")  == 0))
                    goto recurse_mp;
                multipart_append_field(body, boundary, name,
                                       nd_element_get_attr(n, "value"));
            } else if (is_textarea) {
                char *text = nd_node_collect_text(n);
                multipart_append_field(body, boundary, name, text ? text : "");
                g_free(text);
            } else if (is_select) {
                const nd_node *opt = nd_select_chosen_option(n);
                char *v = nd_option_value_dup(opt);
                multipart_append_field(body, boundary, name, v ? v : "");
                g_free(v);
                goto recurse_mp;
            } else if (is_button) {
                const char *type = nd_element_get_attr(n, "type");
                gboolean acts_as_submit = !type ||
                                          g_ascii_strcasecmp(type, "submit") == 0;
                if (acts_as_submit && n == submitter) {
                    const char *v = nd_element_get_attr(n, "value");
                    if (!v) {
                        char *text = nd_node_collect_text(n);
                        multipart_append_field(body, boundary, name,
                                               text ? text : "");
                        g_free(text);
                    } else {
                        multipart_append_field(body, boundary, name, v);
                    }
                }
                goto recurse_mp;
            }
        }
    }
recurse_mp:
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        form_collect_multipart(c, body, boundary, submitter);
}

static void
form_collect_inputs(const nd_node *n, GString *query, gboolean *first,
                    const nd_node *submitter)
{
    if (!n) return;
    if (n->kind == ND_NODE_ELEMENT && n->name) {
        gboolean is_input    = strcmp(n->name, "input") == 0;
        gboolean is_textarea = strcmp(n->name, "textarea") == 0;
        gboolean is_select   = strcmp(n->name, "select") == 0;
        gboolean is_button   = strcmp(n->name, "button") == 0;
        if (is_input || is_textarea || is_select || is_button) {
            const char *name = nd_element_get_attr(n, "name");
            if (!name || !*name) goto recurse;
            if (nd_element_get_attr(n, "disabled")) goto recurse;
            if (is_input) {
                const char *type = nd_element_get_attr(n, "type");
                if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                             g_ascii_strcasecmp(type, "radio") == 0)) {
                    if (!nd_element_get_attr(n, "checked")) goto recurse;
                }
                if (type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                             g_ascii_strcasecmp(type, "image") == 0)) {
                    if (n == submitter) {
                        const char *v = nd_element_get_attr(n, "value");
                        append_form_field(query, first, name, v ? v : "Submit");
                    }
                    goto recurse;
                }
                if (type && (g_ascii_strcasecmp(type, "button") == 0 ||
                             g_ascii_strcasecmp(type, "reset")  == 0 ||
                             g_ascii_strcasecmp(type, "file")   == 0))
                    goto recurse;
                const char *value = nd_element_get_attr(n, "value");
                append_form_field(query, first, name, value);
            } else if (is_textarea) {
                char *text = nd_node_collect_text(n);
                append_form_field(query, first, name, text ? text : "");
                g_free(text);
            } else if (is_select) {
                const nd_node *opt = nd_select_chosen_option(n);
                char *v = nd_option_value_dup(opt);
                append_form_field(query, first, name, v ? v : "");
                g_free(v);
                goto recurse;
            } else if (is_button) {
                const char *type = nd_element_get_attr(n, "type");
                gboolean acts_as_submit = !type || g_ascii_strcasecmp(type, "submit") == 0;
                if (acts_as_submit && n == submitter) {
                    const char *v = nd_element_get_attr(n, "value");
                    if (!v) {
                        char *text = nd_node_collect_text(n);
                        append_form_field(query, first, name, text ? text : "");
                        g_free(text);
                    } else {
                        append_form_field(query, first, name, v);
                    }
                }
                goto recurse;
            }
        }
    }
recurse:
    for (const nd_node *c = n->first_child; c; c = c->next_sibling)
        form_collect_inputs(c, query, first, submitter);
}

static gboolean
nd_value_matches_pattern(const char *value, const char *pattern)
{
    if (!pattern || !*pattern) return TRUE;
    char *anchored = g_strdup_printf("^(?:%s)$", pattern);
    GError *err = NULL;
    GRegex *re = g_regex_new(anchored, 0, 0, &err);
    g_free(anchored);
    if (!re) { g_clear_error(&err); return TRUE; }
    gboolean ok = g_regex_match(re, value ? value : "", 0, NULL);
    g_regex_unref(re);
    return ok;
}

static gboolean
nd_value_matches_type(const char *value, const char *type)
{
    if (!value || !*value || !type) return TRUE;
    if (g_ascii_strcasecmp(type, "email") == 0) {
        const char *at = strchr(value, '@');
        if (!at || at == value) return FALSE;
        const char *dot = strchr(at + 1, '.');
        if (!dot || dot == at + 1 || *(dot + 1) == '\0') return FALSE;
        return TRUE;
    }
    if (g_ascii_strcasecmp(type, "url") == 0) {
        return g_ascii_strncasecmp(value, "http://",  7) == 0 ||
               g_ascii_strncasecmp(value, "https://", 8) == 0 ||
               g_ascii_strncasecmp(value, "ftp://",   6) == 0;
    }
    if (g_ascii_strcasecmp(type, "number") == 0 ||
        g_ascii_strcasecmp(type, "range")  == 0) {
        char *end = NULL;
        g_ascii_strtod(value, &end);
        if (!end || end == value) return FALSE;
        while (*end == ' ' || *end == '\t') end++;
        return *end == '\0';
    }
    return TRUE;
}

static const nd_node *
nd_form_first_invalid(const nd_node *n)
{
    if (!n) return NULL;
    if (n->kind == ND_NODE_ELEMENT && n->name) {
        gboolean is_input    = strcmp(n->name, "input") == 0;
        gboolean is_textarea = strcmp(n->name, "textarea") == 0;
        gboolean is_select   = strcmp(n->name, "select") == 0;
        if (is_input || is_textarea || is_select) {
            if (!nd_element_get_attr(n, "disabled") &&
                !nd_element_get_attr(n, "readonly")) {
                const char *type = is_input ? nd_element_get_attr(n, "type") : NULL;
                gboolean skip = type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                                         g_ascii_strcasecmp(type, "button") == 0 ||
                                         g_ascii_strcasecmp(type, "reset")  == 0 ||
                                         g_ascii_strcasecmp(type, "image")  == 0 ||
                                         g_ascii_strcasecmp(type, "hidden") == 0);
                if (!skip) {
                    const char *value;
                    char *collected = NULL;
                    if (is_textarea) {
                        collected = nd_node_collect_text(n);
                        value = collected ? collected : "";
                    } else if (is_select) {
                        const nd_node *opt = nd_select_chosen_option(n);
                        collected = nd_option_value_dup(opt);
                        value = collected ? collected : "";
                    } else {
                        value = nd_element_get_attr(n, "value");
                        if (!value) value = "";
                    }
                    gboolean required = nd_element_get_attr(n, "required") != NULL;
                    if (required && !*value) {
                        g_free(collected);
                        return n;
                    }
                    if (*value) {
                        const char *pattern = nd_element_get_attr(n, "pattern");
                        if (!nd_value_matches_pattern(value, pattern)) {
                            g_free(collected);
                            return n;
                        }
                        if (is_input && !nd_value_matches_type(value, type)) {
                            g_free(collected);
                            return n;
                        }
                        const char *minlen = nd_element_get_attr(n, "minlength");
                        const char *maxlen = nd_element_get_attr(n, "maxlength");
                        glong vlen = (glong)g_utf8_strlen(value, -1);
                        if (minlen) {
                            glong mn = (glong)nd_parse_int(minlen, 0, 0, 1000000);
                            if (vlen < mn) { g_free(collected); return n; }
                        }
                        if (maxlen) {
                            glong mx = (glong)nd_parse_int(maxlen, 0, 0, 1000000);
                            if (vlen > mx) { g_free(collected); return n; }
                        }
                    }
                    g_free(collected);
                }
            }
        }
    }
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        const nd_node *m = nd_form_first_invalid(c);
        if (m) return m;
    }
    return NULL;
}

static void
nd_window_maybe_submit_form(nd_window *w, const nd_node *clicked)
{
    if (!clicked) return;
    if (nd_element_get_attr(clicked, "disabled")) return;
    gboolean from_text_input = nd_input_is_text_like(clicked);
    gboolean from_js = nd_node_is_element_named(clicked, "form");
    if (!from_text_input && !from_js && !is_submit_trigger(clicked)) return;
    const nd_node *form = clicked;
    while (form && !nd_node_is_element_named(form, "form"))
        form = form->parent;
    if (!form) return;

    if (!nd_element_get_attr(form, "novalidate") &&
        (!clicked || !nd_element_get_attr(clicked, "formnovalidate"))) {
        const nd_node *bad = nd_form_first_invalid(form);
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
        nd_js_dispatch_event(w->js, form, "submit", &prevented);
        if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
        if (prevented) return;
    }

    const char *method = nd_element_get_attr(form, "method");
    const char *formmethod = (!from_text_input && clicked) ?
        nd_element_get_attr(clicked, "formmethod") : NULL;
    if (formmethod && *formmethod) method = formmethod;
    gboolean is_post = method && g_ascii_strcasecmp(method, "post") == 0;

    const char *enctype = nd_element_get_attr(form, "enctype");
    const char *formenctype = (!from_text_input && clicked) ?
        nd_element_get_attr(clicked, "formenctype") : NULL;
    if (formenctype && *formenctype) enctype = formenctype;
    gboolean has_files = form_has_file_upload(form);
    gboolean use_multipart = is_post &&
        (has_files ||
         (enctype && g_ascii_strcasecmp(enctype, "multipart/form-data") == 0));

    GString *body = NULL;
    char *content_type = NULL;
    if (use_multipart) {
        char *boundary = nd_make_multipart_boundary();
        body = g_string_new(NULL);
        form_collect_multipart(form, body, boundary, clicked);
        g_string_append_printf(body, "--%s--\r\n", boundary);
        content_type = g_strdup_printf("multipart/form-data; boundary=%s",
                                       boundary);
        g_free(boundary);
    } else {
        body = g_string_new(NULL);
        gboolean first = TRUE;
        form_collect_inputs(form, body, &first, clicked);
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
                          w->current_fetch, nd_on_fetch_done, w);
        g_free(abs_action);
        g_string_free(body, TRUE);
        g_free(content_type);
        return;
    }
    GString *query = body;
    g_free(content_type);

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
nd_console_entry_activate(GtkEntry *entry, gpointer user_data)
{
    nd_window *w = user_data;
    const char *src = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!src || !*src) return;
    char *echo = g_strdup_printf("> %s", src);
    nd_window_console_append(w, echo);
    g_free(echo);
    if (!w->js) {
        w->js = nd_js_new(nd_window_js_log, w,
                          nd_window_js_mutated, w,
                          nd_window_js_navigate, w);
        if (w->js) {
            nd_js_set_scroll_to_cb(w->js, nd_window_js_scroll_to, w);
            nd_js_set_form_submit_cb(w->js, nd_window_js_form_submit, w);
            nd_js_set_soft_nav_cb(w->js, nd_window_js_soft_nav, w);
        }
    }
    if (w->js) {
        char *result = nd_js_eval_source(w->js, src, "console");
        if (nd_js_consume_mutated(w->js))
            nd_window_js_mutated(w);
        if (result) {
            nd_window_console_append(w, result);
            g_free(result);
        }
    } else {
        nd_window_console_append(w, "(no JS context)");
    }
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void
nd_console_emit_env_line(const char *label, const char *value, gpointer user_data)
{
    nd_window *w = user_data;
    char *line = g_strdup_printf("%-11s : %s", label, value);
    nd_window_console_append(w, line);
    g_free(line);
}

static void
nd_window_console_emit_banner(nd_window *w)
{
    char *line;

    line = g_strdup_printf("Nordstjernen %s — JavaScript console", ND_VERSION);
    nd_window_console_append(w, line); g_free(line);

    nd_env_each(nd_console_emit_env_line, w);

    if (w->last_render_us > 0) {
        double ms = (double)w->last_render_us / 1000.0;
        line = g_strdup_printf("%-11s : %.1f ms", "Last render", ms);
    } else {
        line = g_strdup_printf("%-11s : (not measured yet)", "Last render");
    }
    nd_window_console_append(w, line); g_free(line);

    const char *enc = nd_net_supported_encodings();
    line = g_strdup_printf("%-11s : %s", "Encodings",
                           (enc && *enc) ? enc : "(identity only)");
    nd_window_console_append(w, line); g_free(line);

    nd_window_console_append(w, "");
}

static void
nd_window_open_console(nd_window *w)
{
    if (w->console.window) {
        gtk_window_present(GTK_WINDOW(w->console.window));
        if (w->console.entry) gtk_widget_grab_focus(w->console.entry);
        return;
    }
    w->console.window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(w->console.window), "JavaScript Console — Nordstjernen");
    gtk_window_set_icon_name(GTK_WINDOW(w->console.window), "nordstjernen");
    gtk_window_set_default_size(GTK_WINDOW(w->console.window), 720, 480);
    gtk_window_set_transient_for(GTK_WINDOW(w->console.window), GTK_WINDOW(w->window));
    g_object_add_weak_pointer(G_OBJECT(w->console.window), (gpointer *)&w->console.window);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(w->console.window), vbox);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_view), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(text_view), 6);
    w->console.buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    gtk_text_buffer_create_tag(w->console.buffer, "warn",
                               "foreground", "#b25400", NULL);
    gtk_text_buffer_create_tag(w->console.buffer, "error",
                               "foreground", "#c00",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(w->console.buffer, "alert",
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(w->console.buffer, "timestamp",
                               "foreground", "#888", NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), text_view);
    gtk_box_append(GTK_BOX(vbox), scrolled);

    GtkWidget *input_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(input_row, 4);
    gtk_widget_set_margin_end(input_row, 4);
    gtk_widget_set_margin_top(input_row, 4);
    gtk_widget_set_margin_bottom(input_row, 4);
    GtkWidget *prompt = gtk_label_new(">");
    w->console.entry = gtk_entry_new();
    gtk_widget_set_hexpand(w->console.entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->console.entry),
                                   "Evaluate JavaScript in this page");
    gtk_box_append(GTK_BOX(input_row), prompt);
    gtk_box_append(GTK_BOX(input_row), w->console.entry);
    gtk_box_append(GTK_BOX(vbox), input_row);

    g_signal_connect(w->console.entry, "activate",
                     G_CALLBACK(nd_console_entry_activate), w);

    nd_window_console_emit_banner(w);

    gtk_window_present(GTK_WINDOW(w->console.window));
    gtk_widget_grab_focus(w->console.entry);
}

void
on_win_open_console(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    nd_window_open_console(w);
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

static void
nd_window_ensure_layout(nd_window *w, double viewport_width)
{
    if (!w->last_body) return;
    if (w->layout_tree && w->parsed_doc && !w->layout_dirty &&
        fabs(viewport_width - w->last_viewport_w) < 16.0)
        return;

    gint64 t_start = g_get_monotonic_time();
    if (w->layout_tree) { if (w->js) nd_js_set_layout_root(w->js, NULL); nd_box_free(w->layout_tree); w->layout_tree = NULL; nd_selection_clear(&w->selection); w->search_active_box = NULL; }
    if (w->style_table) { if (w->js) nd_js_set_style_table(w->js, NULL); g_hash_table_destroy(w->style_table); w->style_table = NULL; }

    const char *page_url = nd_window_current_url(w);
    if (!w->parsed_doc) {
        w->parsed_doc = nd_html_parse_for_url(page_url, w->last_body,
                                              (gssize)w->last_body_len);
        nd_compat_rewrite_doc(w->parsed_doc, page_url);
    }

    GPtrArray *page_sheets = g_ptr_array_new();
    nd_collect_inline_stylesheets(w->parsed_doc, page_sheets);
    guint inline_sheet_count = page_sheets->len;

    if (w->external_stylesheets)
        for (guint i = 0; i < w->external_stylesheets->len; i++)
            g_ptr_array_add(page_sheets,
                            g_ptr_array_index(w->external_stylesheets, i));

    nd_css_stylesheet *compat_sheet = nd_compat_stylesheet_for_url(page_url);
    if (compat_sheet) g_ptr_array_add(page_sheets, compat_sheet);

    double viewport_height = viewport_width * 0.75;
    if (w->drawing_area) {
        int alloc_h = gtk_widget_get_height(w->drawing_area);
        if (alloc_h > 100) viewport_height = (double)alloc_h;
    }
    nd_css_set_viewport(viewport_width, viewport_height);

    w->style_table = nd_css_compute(w->parsed_doc,
        (const nd_css_stylesheet *const *)page_sheets->pdata,
        page_sheets->len);

    if (w->anim) {
        for (guint i = 0; i < page_sheets->len; i++) {
            const nd_css_stylesheet *sh = g_ptr_array_index(page_sheets, i);
            if (sh) nd_anim_load_from_stylesheet(w->anim, sh);
        }
        gint64 now_us = g_get_monotonic_time();
        GHashTableIter it;
        gpointer key, val;
        g_hash_table_iter_init(&it, w->style_table);
        while (g_hash_table_iter_next(&it, &key, &val))
            nd_anim_observe(w->anim, (const nd_node *)key,
                            (const nd_style *)val, now_us);
    }

    if (nd_font_available()) {
        for (guint i = 0; i < page_sheets->len; i++) {
            const nd_css_stylesheet *sh = g_ptr_array_index(page_sheets, i);
            if (!sh || !sh->font_faces) continue;
            for (guint j = 0; j < sh->font_faces->len; j++) {
                const nd_css_font_face *ff = &g_array_index(sh->font_faces,
                                                            nd_css_font_face, j);
                if (!ff->family || !ff->src_url) continue;
                char *abs = nd_resolve_url(w, ff->src_url);
                if (!abs) continue;
                if (nd_window_subresource_blocked(w, abs, ND_CSP_FONT, "font")) {
                    g_free(abs);
                    continue;
                }
                nd_font_request(ff->family, abs, nd_window_current_url(w));
                g_free(abs);
            }
        }
    }

    for (guint i = 0; i < inline_sheet_count; i++)
        nd_css_stylesheet_free(g_ptr_array_index(page_sheets, i));
    if (compat_sheet) nd_css_stylesheet_free(compat_sheet);
    g_ptr_array_free(page_sheets, TRUE);
    if (w->zoom > 0 && fabs(w->zoom - 1.0) > 0.001) {
        GHashTableIter it;
        gpointer key, val;
        g_hash_table_iter_init(&it, w->style_table);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            nd_style *st = val;
            if (!st || !st->values[ND_CSS_FONT_SIZE]) continue;
            if (st->values[ND_CSS_FONT_SIZE]->kind != ND_CSS_V_LENGTH) continue;
            st->values[ND_CSS_FONT_SIZE]->u.length.v *= w->zoom;
        }
    }
    w->layout_tree = nd_layout_build(w->parsed_doc, w->style_table, viewport_width,
                                     w->focused_input, w->caret_byte,
                                     w->images, nd_window_current_url(w));
    if (w->js) {
        nd_js_set_style_table(w->js, w->style_table);
        nd_js_set_layout_root(w->js, w->layout_tree);
    }
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
    }
    w->last_render_us = g_get_monotonic_time() - t_start;
    w->layout_dirty = FALSE;
    w->last_viewport_w = viewport_width;
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

static void
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
        nd_node *doc = nd_html_parse_for_url(nd_window_current_url(w),
                                             w->last_body,
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
        if (*frag) {
            g_free(w->pending_fragment);
            w->pending_fragment = g_strdup(frag);
            nd_window_scroll_to_fragment(w);
        }
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
                        const char *group = nd_element_get_attr(form_target, "name");
                        const nd_node *form = form_target;
                        while (form && !(form->kind == ND_NODE_ELEMENT &&
                                         form->name &&
                                         strcmp(form->name, "form") == 0))
                            form = form->parent;
                        if (form && group)
                            nd_clear_radio_group((nd_node *)form, group, form_target);
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
            gboolean prevented = FALSE;
            if (w->js) {
                nd_js_dispatch_event(w->js, hit->dom, "click", &prevented);
                if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
            }
            if (!prevented) {
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
                        if (!target) {
                            for (const nd_node *d = cur->first_child; d; d = d->next_sibling) {
                                if (nd_input_is_text_like(d)) {
                                    target = (nd_node *)d;
                                    break;
                                }
                            }
                        }
                        if (target && nd_input_is_text_like(target)) {
                            nd_window_set_focused_input(w, target);
                            gtk_widget_grab_focus(w->drawing_area);
                            handled = TRUE;
                            break;
                        }
                    }
                    if (nd_input_is_text_like(cur)) {
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
                            if (nd_element_get_attr(details, "open"))
                                nd_element_remove_attr(details, "open");
                            else
                                nd_element_set_attr(details, "open", "");
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
                                const char *group = nd_element_get_attr(cur, "name");
                                const nd_node *form = cur;
                                while (form && !(form->kind == ND_NODE_ELEMENT &&
                                                 form->name &&
                                                 strcmp(form->name, "form") == 0))
                                    form = form->parent;
                                if (form && group)
                                    nd_clear_radio_group((nd_node *)form, group, cur);
                                nd_element_set_attr((nd_node *)cur, "checked", "");
                                if (w->js) {
                                    nd_js_dispatch_event(w->js, cur, "input",  NULL);
                                    nd_js_dispatch_event(w->js, cur, "change", NULL);
                                }
                                nd_window_js_mutated(w);
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


static char *
nd_build_search_url(const char *query)
{
    if (!query || !*query) return NULL;
    char *escaped = g_uri_escape_string(query, NULL, FALSE);
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
nd_search_snippet_label(const char *text)
{
    char *flat = g_strdup(text ? text : "");
    for (char *p = flat; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    }
    g_strstrip(flat);
    const char *end = flat;
    int chars = 0;
    while (*end && chars < 30) {
        end = g_utf8_next_char(end);
        chars++;
    }
    char *label = *end
        ? g_strdup_printf("Search the Web for \"%.*s…\"",
                          (int)(end - flat), flat)
        : g_strdup_printf("Search the Web for \"%s\"", flat);
    g_free(flat);
    return label;
}

static void
on_ctx_open_link(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_link) return;
    char *abs = nd_resolve_url(w, g_context_menu_link);
    if (!abs) return;
    nd_window_load_url(w, abs, ND_LOAD_USER);
    g_free(abs);
}

static void
on_ctx_open_link_new_tab(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_link) return;
    char *abs = nd_resolve_url(w, g_context_menu_link);
    if (!abs) return;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
    nd_window *nw = nd_browser_add_tab(w->window, app, abs);
    if (nw) nd_browser_set_active(w->window, nw);
    g_free(abs);
}

static void
on_ctx_open_link_new_window(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_link) return;
    char *abs = nd_resolve_url(w, g_context_menu_link);
    if (abs) {
        GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
        nd_spawn_window(app, abs);
        g_free(abs);
    }
}

static void
on_ctx_copy_link(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_link) return;
    char *abs = nd_resolve_url(w, g_context_menu_link);
    GdkClipboard *cb = gtk_widget_get_clipboard(w->window);
    gdk_clipboard_set_text(cb, abs ? abs : g_context_menu_link);
    nd_window_set_status(w, "Copied %s", abs ? abs : g_context_menu_link);
    g_free(abs);
}

static void
on_ctx_bookmark_link(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_link || !g_bookmarks) return;
    char *abs = nd_resolve_url(w, g_context_menu_link);
    const char *url = abs ? abs : g_context_menu_link;
    if (nd_bookmarks_contains(g_bookmarks, url)) {
        nd_bookmarks_remove(g_bookmarks, url);
        nd_window_set_status(w, "Removed bookmark %s", url);
    } else {
        nd_bookmarks_add(g_bookmarks, url, url);
        nd_window_set_status(w, "Bookmarked %s", url);
    }
    g_free(abs);
}

static void
on_ctx_open_image(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_image) return;
    char *abs = nd_resolve_url(w, g_context_menu_image);
    if (!abs) return;
    nd_window_load_url(w, abs, ND_LOAD_USER);
    g_free(abs);
}

static void
on_ctx_open_image_new_tab(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_image) return;
    char *abs = nd_resolve_url(w, g_context_menu_image);
    if (!abs) return;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
    nd_window *nw = nd_browser_add_tab(w->window, app, abs);
    if (nw) nd_browser_set_active(w->window, nw);
    g_free(abs);
}

static void
on_ctx_copy_image_address(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_image) return;
    char *abs = nd_resolve_url(w, g_context_menu_image);
    GdkClipboard *cb = gtk_widget_get_clipboard(w->window);
    gdk_clipboard_set_text(cb, abs ? abs : g_context_menu_image);
    nd_window_set_status(w, "Copied %s", abs ? abs : g_context_menu_image);
    g_free(abs);
}

static void
on_ctx_copy_selection(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_selection || !*g_context_menu_selection) return;
    GdkClipboard *cb = gtk_widget_get_clipboard(w->drawing_area
                                                ? w->drawing_area : w->window);
    gdk_clipboard_set_text(cb, g_context_menu_selection);
    nd_window_set_status(w, "Copied %d characters",
                         (int)g_utf8_strlen(g_context_menu_selection, -1));
}

static void
on_ctx_search_selection(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!g_context_menu_selection || !*g_context_menu_selection) return;
    char *url = nd_build_search_url(g_context_menu_selection);
    if (!url) return;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
    nd_window *nw = nd_browser_add_tab(w->window, app, url);
    if (nw) nd_browser_set_active(w->window, nw);
    g_free(url);
}

static void
on_ctx_view_source(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (!w->last_body) return;
    w->mode = (w->mode == ND_VIEW_RAW) ? ND_VIEW_RENDER : ND_VIEW_RAW;
    nd_window_render(w);
}

static void
on_ctx_bookmark_page(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    const char *url = nd_window_current_url(w);
    if (!url || !g_bookmarks) return;
    if (nd_bookmarks_contains(g_bookmarks, url)) {
        nd_bookmarks_remove(g_bookmarks, url);
        nd_window_set_status(w, "Removed bookmark %s", url);
    } else {
        char *title = nd_window_current_title(w);
        nd_bookmarks_add(g_bookmarks, url, title ? title : url);
        nd_window_set_status(w, "Bookmarked %s", url);
        g_free(title);
    }
}

static void
on_ctx_home(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    if (g_home_url && *g_home_url)
        nd_window_load_url(w, g_home_url, ND_LOAD_USER);
}

static void
on_ctx_copy_url(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    nd_window *w = ud;
    const char *url = nd_window_current_url(w);
    if (!url) return;
    GdkClipboard *cb = gtk_widget_get_clipboard(w->window);
    gdk_clipboard_set_text(cb, url);
    nd_window_set_status(w, "Copied %s", url);
}

static void
nd_install_ctx_actions(nd_window *w)
{
    static const struct { const char *name; GCallback cb; } items[] = {
        { "ctx-open-link",            G_CALLBACK(on_ctx_open_link) },
        { "ctx-open-link-new-tab",    G_CALLBACK(on_ctx_open_link_new_tab) },
        { "ctx-open-link-new-window", G_CALLBACK(on_ctx_open_link_new_window) },
        { "ctx-copy-link",            G_CALLBACK(on_ctx_copy_link) },
        { "ctx-bookmark-link",        G_CALLBACK(on_ctx_bookmark_link) },
        { "ctx-open-image",           G_CALLBACK(on_ctx_open_image) },
        { "ctx-open-image-new-tab",   G_CALLBACK(on_ctx_open_image_new_tab) },
        { "ctx-copy-image-address",   G_CALLBACK(on_ctx_copy_image_address) },
        { "ctx-copy-selection",       G_CALLBACK(on_ctx_copy_selection) },
        { "ctx-search-selection",     G_CALLBACK(on_ctx_search_selection) },
        { "ctx-copy-url",             G_CALLBACK(on_ctx_copy_url) },
        { "ctx-view-source",          G_CALLBACK(on_ctx_view_source) },
        { "ctx-bookmark-page",        G_CALLBACK(on_ctx_bookmark_page) },
        { "ctx-home",                 G_CALLBACK(on_ctx_home) },
    };
    GActionMap *map = G_ACTION_MAP(w->window);
    for (gsize i = 0; i < G_N_ELEMENTS(items); i++) {
        g_action_map_remove_action(map, items[i].name);
        GSimpleAction *a = g_simple_action_new(items[i].name, NULL);
        g_signal_connect(a, "activate", items[i].cb, w);
        g_action_map_add_action(map, G_ACTION(a));
        g_object_unref(a);
    }
}

static const nd_box *
nd_box_find_image_ancestor(const nd_box *hit)
{
    for (const nd_box *b = hit; b; b = b->parent) {
        if (b->kind == ND_BOX_IMAGE && b->media && b->media->image_src
            && *b->media->image_src)
            return b;
    }
    return NULL;
}

void
nd_on_drawing_right_pressed(GtkGestureClick *gesture, int n_press,
                            double x, double y, gpointer user_data)
{
    (void)gesture; (void)n_press;
    nd_window *w = user_data;
    if (!w->layout_tree) return;

    g_free(g_context_menu_link);
    g_context_menu_link = NULL;
    g_free(g_context_menu_image);
    g_context_menu_image = NULL;
    g_free(g_context_menu_selection);
    g_context_menu_selection = NULL;

    const char *href = nd_box_hit_link(w->layout_tree, x, y);
    const nd_box *hit = nd_box_hit_test(w->layout_tree, x, y);
    if (!href && hit && hit->dom) {
        for (const nd_node *p = hit->dom; p; p = p->parent) {
            if (nd_node_is_element_named(p, "a")) {
                const char *h = nd_element_get_attr(p, "href");
                if (h && *h) { href = h; break; }
            }
        }
    }
    if (href) g_context_menu_link = g_strdup(href);

    const nd_box *img = nd_box_find_image_ancestor(hit);
    if (img) g_context_menu_image = g_strdup(img->media->image_src);

    if (nd_selection_has_range(&w->selection)) {
        char *text = nd_selection_collect_text(w->layout_tree, &w->selection);
        if (text && *text) g_context_menu_selection = text;
        else g_free(text);
    }

    nd_window_update_nav_state(w);

    GMenu *menu = g_menu_new();

    if (g_context_menu_link) {
        GMenu *link_section = g_menu_new();
        g_menu_append(link_section, "Open Link",               "win.ctx-open-link");
        g_menu_append(link_section, "Open Link in New Tab",    "win.ctx-open-link-new-tab");
        g_menu_append(link_section, "Open Link in New Window", "win.ctx-open-link-new-window");
        g_menu_append(link_section, "Copy Link Address",       "win.ctx-copy-link");
        char *link_abs = nd_resolve_url(w, g_context_menu_link);
        gboolean link_bm = g_bookmarks && nd_bookmarks_contains(
            g_bookmarks, link_abs ? link_abs : g_context_menu_link);
        g_free(link_abs);
        g_menu_append(link_section,
                      link_bm ? "Remove Bookmark for Link" : "Bookmark This Link",
                      "win.ctx-bookmark-link");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(link_section));
        g_object_unref(link_section);
    }

    if (g_context_menu_image) {
        GMenu *img_section = g_menu_new();
        g_menu_append(img_section, "Open Image",            "win.ctx-open-image");
        g_menu_append(img_section, "Open Image in New Tab", "win.ctx-open-image-new-tab");
        g_menu_append(img_section, "Copy Image Address",    "win.ctx-copy-image-address");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(img_section));
        g_object_unref(img_section);
    }

    if (g_context_menu_selection) {
        GMenu *sel_section = g_menu_new();
        g_menu_append(sel_section, "Copy", "win.ctx-copy-selection");
        char *search_label = nd_search_snippet_label(g_context_menu_selection);
        g_menu_append(sel_section, search_label, "win.ctx-search-selection");
        g_free(search_label);
        g_menu_append_section(menu, NULL, G_MENU_MODEL(sel_section));
        g_object_unref(sel_section);
    }

    GMenu *nav_section = g_menu_new();
    g_menu_append(nav_section, "Back",    "win.back");
    g_menu_append(nav_section, "Forward", "win.forward");
    g_menu_append(nav_section, "Reload",  "win.reload");
    if (g_home_url && *g_home_url)
        g_menu_append(nav_section, "Home", "win.ctx-home");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(nav_section));
    g_object_unref(nav_section);

    GMenu *view_section = g_menu_new();
    g_menu_append(view_section, "Zoom In",    "win.zoom-in");
    g_menu_append(view_section, "Zoom Out",   "win.zoom-out");
    g_menu_append(view_section, "Reset Zoom", "win.zoom-reset");
    if (w->last_body) {
        g_menu_append(view_section,
                      w->mode == ND_VIEW_RAW ? "Exit Source View" : "View Page Source",
                      "win.ctx-view-source");
    }
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view_section));
    g_object_unref(view_section);

    GMenu *page_section = g_menu_new();
    const char *cur_url = nd_window_current_url(w);
    gboolean page_bm = cur_url && g_bookmarks &&
                       nd_bookmarks_contains(g_bookmarks, cur_url);
    g_menu_append(page_section,
                  page_bm ? "Remove Bookmark for Page" : "Bookmark This Page",
                  "win.ctx-bookmark-page");
    g_menu_append(page_section, "Copy Page URL",       "win.ctx-copy-url");
    g_menu_append(page_section, "Find on Page",        "win.find");
    g_menu_append(page_section, "Print…",              "win.print");
    g_menu_append(page_section, "Save Page As PDF…",   "win.save-pdf");
    g_menu_append(page_section, "Save Page As HTML…",  "win.save-html");
    g_menu_append(page_section, "JavaScript Console",  "win.open-console");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(page_section));
    g_object_unref(page_section);

    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_widget_set_parent(popover, w->drawing_area);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    gtk_popover_popup(GTK_POPOVER(popover));
    g_signal_connect_swapped(popover, "closed", G_CALLBACK(gtk_widget_unparent), popover);
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
    if (!n || n->kind != ND_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "textarea") == 0) return TRUE;
    if (strcmp(n->name, "input") != 0) return FALSE;
    const char *type = nd_element_get_attr(n, "type");
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "text")     == 0 ||
           g_ascii_strcasecmp(type, "search")   == 0 ||
           g_ascii_strcasecmp(type, "email")    == 0 ||
           g_ascii_strcasecmp(type, "url")      == 0 ||
           g_ascii_strcasecmp(type, "tel")      == 0 ||
           g_ascii_strcasecmp(type, "number")   == 0 ||
           g_ascii_strcasecmp(type, "password") == 0;
}

static const char *
nd_input_current_value(const nd_node *n)
{
    if (!n) return "";
    if (n->name && strcmp(n->name, "textarea") == 0) {
        for (const nd_node *c = n->first_child; c; c = c->next_sibling)
            if (c->kind == ND_NODE_TEXT && c->text)
                return c->text;
        return "";
    }
    const char *v = nd_element_get_attr(n, "value");
    return v ? v : "";
}

static void
nd_input_set_value(nd_node *n, const char *value)
{
    if (!n) return;
    if (n->name && strcmp(n->name, "textarea") == 0) {
        for (nd_node *c = n->first_child; c; ) {
            nd_node *next = c->next_sibling;
            nd_node_remove(c);
            nd_node_free(c);
            c = next;
        }
        nd_node *text = nd_node_new_text(g_strdup(value ? value : ""));
        nd_node_append_child(n, text);
    } else {
        nd_element_set_attr(n, "value", value ? value : "");
    }
}

typedef struct nd_refresh_ctx {
    nd_window *w;
    char *url;
} nd_refresh_ctx;

static gboolean
nd_window_refresh_fire(gpointer data)
{
    nd_refresh_ctx *ctx = data;
    if (ctx->w) {
        ctx->w->refresh_source = 0;
        if (ctx->url)
            nd_window_load_url(ctx->w, ctx->url, ND_LOAD_USER);
    }
    g_free(ctx->url);
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

static void
nd_window_apply_meta_refresh(nd_window *w)
{
    if (!w->parsed_doc) return;
    nd_node *head = nd_node_find_first_element(w->parsed_doc, "head");
    if (!head) return;
    for (const nd_node *c = head->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "meta") != 0) continue;
        const char *equiv = nd_element_get_attr(c, "http-equiv");
        if (!equiv || g_ascii_strcasecmp(equiv, "refresh") != 0) continue;
        const char *content = nd_element_get_attr(c, "content");
        if (!content || !*content) continue;
        char *end = NULL;
        double secs = g_ascii_strtod(content, &end);
        if (!end || end == content) continue;
        char *url = NULL;
        const char *p = strchr(end, ';');
        if (p) {
            p++;
            while (*p == ' ') p++;
            if (g_ascii_strncasecmp(p, "url=", 4) == 0) {
                p += 4;
                while (*p == ' ' || *p == '\'' || *p == '"') p++;
                const char *e = p;
                while (*e && *e != '\'' && *e != '"') e++;
                char *raw = g_strndup(p, (gsize)(e - p));
                url = nd_resolve_url(w, raw);
                g_free(raw);
            }
        }
        if (!url) url = g_strdup(nd_window_current_url(w));
        if (!url) continue;
        guint delay = (guint)(secs < 0 ? 0 : secs);
        if (delay > 600) delay = 600;
        g_clear_handle_id(&w->refresh_source, g_source_remove);
        nd_refresh_ctx *ctx = g_new0(nd_refresh_ctx, 1);
        ctx->w = w;
        ctx->url = url;
        w->refresh_source = g_timeout_add_seconds(delay,
                                                  nd_window_refresh_fire, ctx);
        return;
    }
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
    nd_node *target = w->focused_input;
    if (!target) return;
    const char *cur = nd_input_current_value(target);
    gsize cur_len = strlen(cur);
    if (del_start > cur_len) del_start = cur_len;
    if (del_end   > cur_len) del_end   = cur_len;
    if (del_end < del_start) del_end = del_start;
    GString *s = g_string_sized_new(cur_len - (del_end - del_start) + insert_len);
    g_string_append_len(s, cur, (gssize)del_start);
    if (insert && insert_len) g_string_append_len(s, insert, (gssize)insert_len);
    g_string_append_len(s, cur + del_end, (gssize)(cur_len - del_end));
    if (w->js) {
        gboolean prevented = FALSE;
        nd_js_dispatch_event(w->js, target, "beforeinput", &prevented);
        if (prevented) { g_string_free(s, TRUE); return; }
    }
    nd_input_set_value(target, s->str);
    w->caret_byte = del_start + insert_len;
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
    if (text && w->focused_input) {
        gboolean is_textarea = w->focused_input->name &&
                               strcmp(w->focused_input->name, "textarea") == 0;
        if (!is_textarea) {
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
    if (w->layout_tree) { if (w->js) nd_js_set_layout_root(w->js, NULL); nd_box_free(w->layout_tree); w->layout_tree = NULL; nd_selection_clear(&w->selection); w->search_active_box = NULL; }
    w->layout_dirty = TRUE;
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    if (w->focused_input) {
        nd_node *old = w->focused_input;
        if (w->im_context) {
            gtk_im_context_reset(w->im_context);
            gtk_im_context_focus_out(w->im_context);
        }
        if (w->js) {
            const char *cur = nd_input_current_value(old);
            if (w->focused_input_initial &&
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
        w->focused_input_initial = g_strdup(nd_input_current_value(target));
        w->caret_byte = w->focused_input_initial ? strlen(w->focused_input_initial) : 0;
        w->caret_blink_on = TRUE;
        w->caret_blink_source = g_timeout_add(530, nd_window_caret_blink_tick, w);
        if (w->im_context) gtk_im_context_focus_in(w->im_context);
        if (w->js) {
            nd_js_dispatch_event(w->js, target, "focus",   NULL);
            nd_js_dispatch_event(w->js, target, "focusin", NULL);
        }
    }
}

static gboolean
nd_window_handle_input_key(nd_window *w, guint keyval, GdkModifierType state)
{
    if (!w->focused_input) return FALSE;
    nd_node *target = w->focused_input;
    const char *cur = nd_input_current_value(target);
    gsize cur_len = strlen(cur);
    if (w->caret_byte > cur_len) w->caret_byte = cur_len;

    gboolean ctrl  = (state & GDK_CONTROL_MASK) != 0;
    gboolean alt   = (state & GDK_ALT_MASK)     != 0;
    gboolean meta  = (state & GDK_META_MASK)    != 0;
    gboolean is_textarea = target->name && strcmp(target->name, "textarea") == 0;

    if (alt || meta) return FALSE;

    if (ctrl) {
        if (keyval == GDK_KEY_v || keyval == GDK_KEY_V) {
            nd_window_input_paste(w);
            return TRUE;
        }
        if (keyval == GDK_KEY_a || keyval == GDK_KEY_A) {
            w->caret_byte = 0;
            nd_window_reset_caret_blink(w);
            nd_window_js_mutated(w);
            return TRUE;
        }
        if (keyval == GDK_KEY_Left) {
            w->caret_byte = 0;
            nd_window_reset_caret_blink(w);
            nd_window_js_mutated(w);
            return TRUE;
        }
        if (keyval == GDK_KEY_Right) {
            w->caret_byte = cur_len;
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
        if (is_textarea) {
            nd_window_input_replace(w, w->caret_byte, w->caret_byte, "\n", 1);
            return TRUE;
        }
        nd_node *submit_target = target;
        nd_window_set_focused_input(w, NULL);
        nd_window_maybe_submit_form(w, submit_target);
        return TRUE;
    }
    if (keyval == GDK_KEY_BackSpace) {
        if (w->caret_byte == 0) return TRUE;
        const char *prev = g_utf8_prev_char(cur + w->caret_byte);
        nd_window_input_replace(w, (gsize)(prev - cur), w->caret_byte, NULL, 0);
        return TRUE;
    }
    if (keyval == GDK_KEY_Delete) {
        if (w->caret_byte >= cur_len) return TRUE;
        const char *nxt = g_utf8_next_char(cur + w->caret_byte);
        nd_window_input_replace(w, w->caret_byte, (gsize)(nxt - cur), NULL, 0);
        return TRUE;
    }
    if (keyval == GDK_KEY_Left) {
        if (w->caret_byte > 0) {
            const char *prev = g_utf8_prev_char(cur + w->caret_byte);
            w->caret_byte = (gsize)(prev - cur);
            nd_window_reset_caret_blink(w);
            nd_window_js_mutated(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Right) {
        if (w->caret_byte < cur_len) {
            const char *nxt = g_utf8_next_char(cur + w->caret_byte);
            w->caret_byte = (gsize)(nxt - cur);
            nd_window_reset_caret_blink(w);
            nd_window_js_mutated(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Home) {
        w->caret_byte = 0;
        nd_window_reset_caret_blink(w);
        nd_window_js_mutated(w);
        return TRUE;
    }
    if (keyval == GDK_KEY_End) {
        w->caret_byte = cur_len;
        nd_window_reset_caret_blink(w);
        nd_window_js_mutated(w);
        return TRUE;
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
    return nd_dispatch_key_event_common(user_data, "keydown", keyval, state, event);
}

void
nd_on_drawing_key_released(GtkEventControllerKey *c, guint keyval, guint keycode,
                           GdkModifierType state, gpointer user_data)
{
    (void)keycode;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(c));
    nd_dispatch_key_event_common(user_data, "keyup", keyval, state, event);
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
    nd_window_ensure_layout(w, (double)width);
    if (!w->layout_tree) return;
    nd_paint_set_js(w->js);
    nd_paint_set_anim(w->anim);
    nd_paint_with_selection(cr, w->layout_tree, w->search_query, &w->selection);
    w->first_paint_done = TRUE;
}

static char *
nd_resolve_url(const nd_window *w, const char *href)
{
    if (!href || !*href) return NULL;
    if (w->cursor < 0 || w->cursor >= (int)w->history->len)
        return nd_url_resolve(NULL, href);
    const char *base = g_ptr_array_index(w->history, w->cursor);
    if (w->parsed_doc) {
        nd_node *base_el = nd_node_find_first_element(w->parsed_doc, "base");
        if (base_el) {
            const char *b = nd_element_get_attr(base_el, "href");
            if (b && nd_url_is_http_or_https(b)) base = b;
        }
    }
    return nd_url_resolve(base, href);
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
        g_free(line);
    }
    if (w->mode != ND_VIEW_RENDER || !w->drawing_area) return;
    if (!w->js_relayout_idle_id)
        w->js_relayout_idle_id =
            g_timeout_add(50, nd_window_js_relayout_now, w);
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
    nd_window *w;
    char      *url;
    char      *integrity;
} nd_css_fetch;

static void
on_external_css_loaded(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_css_fetch *fetch = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    if (!nd_window_alive(fetch->w)) {
        g_clear_error(&err);
        nd_response_free(resp);
        g_free(fetch->url);
        g_free(fetch->integrity);
        g_free(fetch);
        return;
    }
    nd_window *w = fetch->w;
    if (w->css_inflight > 0) w->css_inflight--;
    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) && fetch->w) {
            nd_window_set_status(fetch->w, "CSS fetch failed: %s", err->message);
            char *line = g_strdup_printf("[error] stylesheet: %s — %s",
                                         fetch->url, err->message);
            nd_window_console_append(fetch->w, line);
            g_free(line);
        }
        g_clear_error(&err);
        nd_response_free(resp);
        g_free(fetch->url);
        g_free(fetch->integrity);
        g_free(fetch);
        goto maybe_paint;
    }
    if (!resp) {
        g_free(fetch->url);
        g_free(fetch->integrity);
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
        nd_css_stylesheet *sh = nd_css_stylesheet_parse(
            (const char *)resp->body->data, (gssize)resp->body->len);
        if (sh) {
            g_ptr_array_add(w->external_stylesheets, sh);
            if (w->layout_tree) { if (w->js) nd_js_set_layout_root(w->js, NULL); nd_box_free(w->layout_tree); w->layout_tree = NULL; nd_selection_clear(&w->selection); w->search_active_box = NULL; }
            if (w->style_table) { if (w->js) nd_js_set_style_table(w->js, NULL); g_hash_table_destroy(w->style_table); w->style_table = NULL; }
            w->layout_dirty = TRUE;
        }
    }
    nd_response_free(resp);
    g_free(fetch->url);
    g_free(fetch->integrity);
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
                        fetch->w = w;
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
            if (rel && href && *href &&
                g_ascii_strcasecmp(rel, "stylesheet") == 0) {
                char *abs = nd_resolve_url(w, href);
                if (abs && nd_window_subresource_blocked(w, abs, ND_CSP_STYLE, "stylesheet")) {
                    g_free(abs);
                    continue;
                }
                if (abs && !g_hash_table_contains(w->external_css_seen, abs)) {
                    g_hash_table_add(w->external_css_seen, g_strdup(abs));
                    const char *integrity = nd_element_get_attr(n, "integrity");
                    nd_css_fetch *fetch = g_new0(nd_css_fetch, 1);
                    fetch->w = w;
                    fetch->url = abs;
                    fetch->integrity = integrity ? g_strdup(integrity) : NULL;
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
    if (!g_str_has_prefix(page, "http://") &&
        !g_str_has_prefix(page, "https://"))
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
    if (!g_str_has_prefix(abs, "http://") &&
        !g_str_has_prefix(abs, "https://")) {
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

static void
nd_window_kick_image_loads(nd_window *w)
{
    if (!w->layout_tree || !w->images) return;
    GPtrArray *imgs = g_ptr_array_new();
    nd_layout_collect_images(w->layout_tree, imgs);
    for (guint i = 0; i < imgs->len; i++) {
        nd_box *box = g_ptr_array_index(imgs, i);
        if (!box->media) continue;
        if (box->media->image_src) {
            char *abs = nd_resolve_url(w, box->media->image_src);
            if (abs) {
                if (nd_window_subresource_blocked(w, abs, ND_CSP_IMG, "image")) {
                    g_free(abs);
                } else {
                    box->media->image = nd_image_cache_get(w->images, abs,
                        nd_window_current_url(w), on_image_ready, w);
                    g_free(abs);
                }
            }
        }
        if (box->media->bg_image_src) {
            char *abs = nd_resolve_url(w, box->media->bg_image_src);
            if (abs) {
                if (nd_window_subresource_blocked(w, abs, ND_CSP_IMG, "image")) {
                    g_free(abs);
                } else {
                    box->media->bg_image = nd_image_cache_get(w->images, abs,
                        nd_window_current_url(w), on_image_ready, w);
                    g_free(abs);
                }
            }
        }
    }
    g_ptr_array_free(imgs, TRUE);
}

static gboolean
nd_window_video_tick(gpointer user_data)
{
    nd_window *w = user_data;
    if (!w->layout_tree || w->mode != ND_VIEW_RENDER) {
        w->video_tick_source = 0;
        return G_SOURCE_REMOVE;
    }
    GPtrArray *vids = g_ptr_array_new();
    nd_layout_collect_videos(w->layout_tree, vids);
    gboolean any_updated = FALSE;
    gboolean any_active = FALSE;
    gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < vids->len; i++) {
        nd_box *box = g_ptr_array_index(vids, i);
        if (!box->media) continue;
        nd_video *v = box->media->video;
        if (!v || !v->loaded || v->failed) continue;
        if (v->ended && box->media->video_loop) nd_video_restart(v);
        if (!v->ended) any_active = TRUE;
        if (nd_video_tick(v, now)) any_updated = TRUE;
    }
    g_ptr_array_free(vids, TRUE);
    if (any_updated && w->drawing_area)
        gtk_widget_queue_draw(w->drawing_area);
    if (!any_active) {
        w->video_tick_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
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
        if (m->video_audio_src && w->audios) {
            char *audio_abs = nd_resolve_url(w, m->video_audio_src);
            if (audio_abs &&
                !nd_window_subresource_blocked(w, audio_abs,
                                               ND_CSP_MEDIA, "audio")) {
                m->audio = nd_audio_cache_get(w->audios, audio_abs,
                                              nd_window_current_url(w),
                                              m->video_loop);
            }
            g_free(audio_abs);
        }
        g_free(abs);
        g_free(poster_abs);
    }
    if (vids->len > 0 && w->video_tick_source == 0)
        w->video_tick_source = g_timeout_add(33, nd_window_video_tick, w);
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

    while (*raw == ' ' || *raw == '\t')
        raw++;
    size_t len = strlen(raw);
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t' ||
                       raw[len - 1] == '\r' || raw[len - 1] == '\n'))
        len--;
    if (len == 0)
        return NULL;

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
    nd_window *w;
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
    if (!file) {
        if (nd_window_alive(p->w)) {
            if (err && !g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
                nd_window_set_status(p->w, "Download cancelled: %s", err->message);
            else
                nd_window_set_status(p->w, "Download cancelled");
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
    if (nd_window_alive(p->w)) {
        if (ok) nd_window_set_status(p->w, "Saved %s (%" G_GSIZE_FORMAT " bytes)",
                                     path ? path : "(file)", sz);
        else    nd_window_set_status(p->w, "Save failed: %s",
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
    if (!g_str_has_prefix(resp->final_url, "http://") &&
        !g_str_has_prefix(resp->final_url, "https://"))
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
    p->w = w;
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
    nd_window *w = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);

    if (!nd_window_alive(w)) {
        nd_response_free(resp);
        g_clear_error(&err);
        return;
    }

    g_clear_object(&w->current_fetch);
    nd_window_set_busy(w, FALSE);

    if (!resp) {
        if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            nd_window_set_status(w, "Cancelled");
        } else {
            nd_window_set_status(w, "Error: %s", err ? err->message : "unknown");
            char *line = g_strdup_printf("[error] page fetch failed: %s",
                                         err ? err->message : "unknown");
            nd_window_console_append(w, line);
            g_free(line);
        }
        if (err)
            g_clear_error(&err);
        return;
    }

    nd_window_record_final_url(w, resp);

    if (resp->error) {
        char *line = g_strdup_printf("[error] page transport error: %s",
                                     resp->error);
        nd_window_console_append(w, line);
        g_free(line);
        nd_window_set_status(w, "Transport error: %s", resp->error);
        nd_window_clear_cache(w);
        char *html = g_markup_printf_escaped(
            "<!doctype html><html><head><title>Cannot load</title></head>"
            "<body><h1>Cannot load %s</h1>"
            "<p style=\"color:#a00\">%s</p>"
            "<p>Check the URL and your network connection, then "
            "<a href=\"%s\">try again</a>.</p>"
            "</body></html>",
            resp->final_url ? resp->final_url : "this page",
            resp->error,
            resp->final_url ? resp->final_url : "");
        w->last_body = html;
        w->last_body_len = strlen(html);
        w->last_content_type = g_strdup("text/html; charset=utf-8");
        w->dom_mutated = FALSE;
        w->mode = ND_VIEW_RENDER;
        nd_window_render(w);
        nd_window_ensure_layout(w, nd_layout_viewport());
        nd_window_set_title_if_active(w, "Error — " ND_TITLE);
        nd_response_free(resp);
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
            return;
        }
    }

    gboolean youtube_rewritten = FALSE;
    if (resp->body && resp->body->len > 0) {
        char *decoded = nd_html_decode_body((const char *)resp->body->data,
                                    resp->body->len);
        const char *page_url = resp->final_url ? resp->final_url
                                               : nd_window_current_url(w);
        if (nd_youtube_is_watch_url(page_url)) {
            char *rewritten = nd_youtube_render_watch_page(
                page_url, decoded, strlen(decoded));
            if (rewritten) {
                g_free(decoded);
                decoded = rewritten;
                youtube_rewritten = TRUE;
            }
        }
        w->last_body = decoded;
        w->last_body_len = strlen(decoded);
        w->dom_mutated = FALSE;
        if (is_html_content_type(resp->content_type))
            nd_window_preload_stylesheets(w, decoded, w->last_body_len);
    }
    w->last_content_type = g_strdup(
        youtube_rewritten ? "text/html; charset=utf-8" :
        (resp->content_type ? resp->content_type : ""));
    if (!youtube_rewritten && resp->csp_header && *resp->csp_header)
        w->csp = nd_csp_parse(resp->csp_header);

    if (is_html_content_type(w->last_content_type))
        w->mode = ND_VIEW_RENDER;
    else
        w->mode = ND_VIEW_RAW;

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
        nd_window_apply_meta_refresh(w);
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
        if (!w->js) {
            w->js = nd_js_new(nd_window_js_log, w,
                              nd_window_js_mutated, w,
                              nd_window_js_navigate, w);
            if (w->js) {
                nd_js_set_scroll_to_cb(w->js, nd_window_js_scroll_to, w);
                nd_js_set_form_submit_cb(w->js, nd_window_js_form_submit, w);
                nd_js_set_soft_nav_cb(w->js, nd_window_js_soft_nav, w);
            }
        }
        if (w->js) {
            nd_js_set_csp(w->js, w->csp);
            if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
            while (g_main_context_iteration(NULL, FALSE)) { }
            nd_js_run_scripts_in_doc(w->js, w->parsed_doc,
                                     nd_window_current_url(w));
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
}

static void
nd_window_load_url(nd_window *w, const char *raw_url, nd_load_source src)
{
    char *url = nd_normalize_url(raw_url);
    if (!url) {
        nd_window_set_status(w, "Empty URL");
        return;
    }
    char *upgraded = nd_net_hsts_upgrade(url);
    if (upgraded) { g_free(url); url = upgraded; }

    char *consent_target = nd_google_unwrap_consent_url(url);
    if (consent_target) { g_free(url); url = consent_target; }
    char *google_rewrite = nd_google_rewrite_url(url);
    if (google_rewrite) { g_free(url); url = google_rewrite; }

    g_free(w->pending_fragment);
    w->pending_fragment = NULL;
    char *hash = strchr(url, '#');
    if (hash) {
        w->pending_fragment = g_strdup(hash + 1);
        *hash = '\0';
        const char *cur = nd_window_current_url(w);
        if (cur && strcmp(cur, url) == 0) {
            g_free(url);
            nd_window_scroll_to_fragment(w);
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
    nd_net_fetch_async(url, NULL, w->current_fetch, nd_on_fetch_done, w);
    g_free(url);
}

static void
nd_window_set_busy(nd_window *w, gboolean busy)
{
    gtk_widget_set_sensitive(w->go_button, !busy);
    gtk_widget_set_sensitive(w->home_button, !busy);
    gtk_widget_set_sensitive(w->stop_button, busy);
    gtk_spinner_set_spinning(GTK_SPINNER(w->spinner_anim), busy);
    gtk_stack_set_visible_child_name(GTK_STACK(w->spinner),
                                     busy ? "busy" : "idle");
    gtk_widget_set_tooltip_text(w->spinner, busy ? "Loading…" : "Idle");
    if (w->window)
        gtk_widget_set_cursor_from_name(w->window, busy ? "progress" : NULL);
    if (busy) {
        gtk_widget_set_sensitive(w->back_button, FALSE);
        gtk_widget_set_sensitive(w->forward_button, FALSE);
    } else {
        nd_window_update_nav_state(w);
    }
}

static void
nd_window_update_nav_state(nd_window *w)
{
    gboolean can_back    = w->cursor > 0;
    gboolean can_forward = w->cursor >= 0 && w->cursor + 1 < (int)w->history->len;
    gtk_widget_set_sensitive(w->back_button, can_back);
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
    if (w->current_fetch)
        g_cancellable_cancel(w->current_fetch);
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

void
on_about_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    nd_window_load_url(w, "about:nordstjernen", ND_LOAD_USER);
}

typedef struct nd_settings_dialog {
    nd_window *w;
    GtkWidget *dialog;
    GtkWidget *http_proxy_entry;
    GtkWidget *home_url_entry;
    GtkWidget *search_engine_entry;
} nd_settings_dialog;

static void
on_settings_dialog_save(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_settings_dialog *sd = user_data;
    nd_config *c = nd_config_mut();

    const char *http_proxy   = gtk_editable_get_text(GTK_EDITABLE(sd->http_proxy_entry));
    const char *home_url     = gtk_editable_get_text(GTK_EDITABLE(sd->home_url_entry));
    const char *search       = gtk_editable_get_text(GTK_EDITABLE(sd->search_engine_entry));

    g_free(c->http_proxy);
    c->http_proxy = g_strdup(http_proxy ? http_proxy : "");
    g_free(c->home_url);
    c->home_url = g_strdup(home_url ? home_url : "");
    g_free(c->search_engine);
    c->search_engine = g_strdup(search ? search : "");

    g_free(g_home_url);
    g_home_url = g_strdup(c->home_url);

    GError *err = NULL;
    if (!nd_config_save(&err)) {
        nd_window_set_status(sd->w, "Failed to save settings: %s",
                             err ? err->message : "unknown error");
        g_clear_error(&err);
    } else {
        nd_window_set_status(sd->w, "Settings saved to %s",
                             nd_config_path());
    }
    gtk_window_destroy(GTK_WINDOW(sd->dialog));
}

static void
on_settings_dialog_cancel(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_settings_dialog *sd = user_data;
    gtk_window_destroy(GTK_WINDOW(sd->dialog));
}

static void
on_settings_clear_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_settings_dialog *sd = user_data;
    nd_window *w = sd->w;

    nd_cache_clear();

    char *keep = NULL;
    if (w->history && w->cursor >= 0 && w->cursor < (int)w->history->len)
        keep = g_strdup(g_ptr_array_index(w->history, w->cursor));
    if (w->history) {
        for (guint i = 0; i < w->history->len; i++)
            g_free(g_ptr_array_index(w->history, i));
        g_ptr_array_set_size(w->history, 0);
    }
    if (keep) {
        g_ptr_array_add(w->history, keep);
        w->cursor = 0;
    } else {
        w->cursor = -1;
    }
    nd_window_update_nav_state(w);
    nd_window_set_status(w, "Cleared cache and history");
}

static void
on_settings_dialog_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    g_free(user_data);
}

static GtkWidget *
nd_settings_add_row(GtkWidget *grid, int row, const char *label_text,
                    GtkWidget *control)
{
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_END);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), label,   0, row, 1, 1);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_halign(control, GTK_ALIGN_FILL);
    gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
    return control;
}

void
on_settings_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    const nd_config *c = nd_config_get();

    nd_settings_dialog *sd = g_new0(nd_settings_dialog, 1);
    sd->w = w;
    sd->dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(sd->dialog), "Settings — Nordstjernen");
    gtk_window_set_icon_name(GTK_WINDOW(sd->dialog), "nordstjernen");
    gtk_window_set_default_size(GTK_WINDOW(sd->dialog), 520, -1);
    gtk_window_set_resizable(GTK_WINDOW(sd->dialog), FALSE);
    gtk_window_set_transient_for(GTK_WINDOW(sd->dialog), GTK_WINDOW(w->window));
    gtk_window_set_modal(GTK_WINDOW(sd->dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(sd->dialog), TRUE);
    g_signal_connect(sd->dialog, "destroy",
                     G_CALLBACK(on_settings_dialog_destroy), sd);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(sd->dialog), vbox);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(vbox), grid);

    sd->home_url_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(sd->home_url_entry),
                          c->home_url ? c->home_url : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(sd->home_url_entry),
                                   "about:start");
    nd_settings_add_row(grid, 0, "Home URL:", sd->home_url_entry);

    sd->search_engine_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(sd->search_engine_entry),
                          c->search_engine ? c->search_engine : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(sd->search_engine_entry),
                                   "https://example.com/search?q=%s");
    nd_settings_add_row(grid, 1, "Search engine:", sd->search_engine_entry);

    sd->http_proxy_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(sd->http_proxy_entry),
                          c->http_proxy ? c->http_proxy : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(sd->http_proxy_entry),
                                   "http://host:port  (leave blank for direct)");
    nd_settings_add_row(grid, 2, "HTTP proxy:", sd->http_proxy_entry);

    GtkWidget *hint = gtk_label_new(NULL);
    char *hint_text = g_strdup_printf(
        "Saved to %s. New requests pick up changes immediately.",
        nd_config_path() ? nd_config_path() : "(no config path)");
    gtk_label_set_text(GTK_LABEL(hint), hint_text);
    g_free(hint_text);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(vbox), hint);

    GtkWidget *button_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(button_row, 8);
    GtkWidget *clear  = gtk_button_new_with_label("Clear cache and history");
    g_signal_connect(clear, "clicked",
                     G_CALLBACK(on_settings_clear_clicked), sd);
    gtk_box_append(GTK_BOX(button_row), clear);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(button_row), spacer);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *save   = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect(cancel, "clicked",
                     G_CALLBACK(on_settings_dialog_cancel), sd);
    g_signal_connect(save, "clicked",
                     G_CALLBACK(on_settings_dialog_save), sd);
    gtk_box_append(GTK_BOX(button_row), cancel);
    gtk_box_append(GTK_BOX(button_row), save);
    gtk_box_append(GTK_BOX(vbox), button_row);

    gtk_window_present(GTK_WINDOW(sd->dialog));
    gtk_widget_grab_focus(sd->home_url_entry);
}

static const char *
nd_window_current_url(nd_window *w)
{
    if (w->cursor < 0 || w->cursor >= (int)w->history->len) return NULL;
    return g_ptr_array_index(w->history, w->cursor);
}

static char *
nd_window_current_title(nd_window *w)
{
    if (!w->parsed_doc) return NULL;
    nd_node *title = nd_node_find_first_element(w->parsed_doc, "title");
    if (!title) return NULL;
    return nd_node_collect_text(title);
}

static void
on_bookmark_open(GtkButton *button, gpointer user_data)
{
    nd_window *w = user_data;
    const char *url = g_object_get_data(G_OBJECT(button), "nd-url");
    if (!url) return;
    nd_window_load_url(w, url, ND_LOAD_USER);
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
}

static void
on_bookmark_open_new_window(GtkButton *button, gpointer user_data)
{
    nd_window *w = user_data;
    const char *url = g_object_get_data(G_OBJECT(button), "nd-url");
    if (!url) return;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
    nd_spawn_window(app, url);
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
}

static void
on_bookmark_delete(GtkButton *button, gpointer user_data)
{
    nd_window *w = user_data;
    const char *url = g_object_get_data(G_OBJECT(button), "nd-url");
    if (!url || !g_bookmarks) return;
    char *url_copy = g_strdup(url);
    nd_bookmarks_remove(g_bookmarks, url_copy);
    nd_window_set_status(w, "Removed bookmark %s", url_copy);
    g_free(url_copy);
    GtkWidget *row = gtk_widget_get_parent(GTK_WIDGET(button));
    GtkWidget *list = row ? gtk_widget_get_parent(row) : NULL;
    if (list && row) gtk_box_remove(GTK_BOX(list), row);
}

void
on_bookmarks_clicked(GtkButton *button, gpointer user_data)
{
    nd_window *w = user_data;
    if (!g_bookmarks) return;
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(outer, 420, -1);
    GtkWidget *heading = gtk_label_new("Bookmarks");
    gtk_widget_add_css_class(heading, "heading");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_widget_set_margin_top(heading, 8);
    gtk_widget_set_margin_bottom(heading, 4);
    gtk_widget_set_margin_start(heading, 12);
    gtk_widget_set_margin_end(heading, 12);
    gtk_box_append(GTK_BOX(outer), heading);
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 420);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    guint count = nd_bookmarks_count(g_bookmarks);
    if (count == 0) {
        GtkWidget *empty = gtk_label_new("No bookmarks yet — star a page to add one.");
        gtk_widget_set_margin_top(empty, 12);
        gtk_widget_set_margin_bottom(empty, 12);
        gtk_widget_set_margin_start(empty, 12);
        gtk_widget_set_margin_end(empty, 12);
        gtk_box_append(GTK_BOX(box), empty);
    }
    for (guint i = 0; i < count; i++) {
        const nd_bookmark *b = nd_bookmarks_get(g_bookmarks, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *open = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(open), FALSE);
        gtk_widget_set_hexpand(open, TRUE);
        gtk_widget_set_halign(open, GTK_ALIGN_FILL);
        GtkWidget *rowbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *t = gtk_label_new(b->title);
        gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
        GtkWidget *u = gtk_label_new(b->url);
        gtk_label_set_xalign(GTK_LABEL(u), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(u), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(u, "dim-label");
        gtk_box_append(GTK_BOX(rowbox), t);
        gtk_box_append(GTK_BOX(rowbox), u);
        gtk_button_set_child(GTK_BUTTON(open), rowbox);
        gtk_widget_set_tooltip_text(open, "Open in this window");
        g_object_set_data_full(G_OBJECT(open), "nd-url", g_strdup(b->url), g_free);
        g_signal_connect(open, "clicked", G_CALLBACK(on_bookmark_open), w);

        GtkWidget *new_win = gtk_button_new_from_icon_name("window-new-symbolic");
        gtk_widget_set_tooltip_text(new_win, "Open in a new window");
        gtk_button_set_has_frame(GTK_BUTTON(new_win), FALSE);
        g_object_set_data_full(G_OBJECT(new_win), "nd-url", g_strdup(b->url), g_free);
        g_signal_connect(new_win, "clicked", G_CALLBACK(on_bookmark_open_new_window), w);

        GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_tooltip_text(del, "Delete bookmark");
        gtk_button_set_has_frame(GTK_BUTTON(del), FALSE);
        g_object_set_data_full(G_OBJECT(del), "nd-url", g_strdup(b->url), g_free);
        g_signal_connect(del, "clicked", G_CALLBACK(on_bookmark_delete), w);

        gtk_box_append(GTK_BOX(row), open);
        gtk_box_append(GTK_BOX(row), new_win);
        gtk_box_append(GTK_BOX(row), del);
        gtk_box_append(GTK_BOX(box), row);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), box);
    gtk_box_append(GTK_BOX(outer), scrolled);
    gtk_popover_set_child(GTK_POPOVER(popover), outer);
    gtk_widget_set_parent(popover, GTK_WIDGET(button));
    gtk_popover_popup(GTK_POPOVER(popover));
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
    nd_window *w;
    nd_node   *input;
} nd_file_chooser_ctx;

static void
nd_on_file_chooser_response(GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
    nd_file_chooser_ctx *ctx = user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, result, &err);
    if (file && ctx && ctx->w && nd_window_alive(ctx->w) && ctx->input) {
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
            if (ctx->w->js) {
                nd_js_dispatch_event(ctx->w->js, ctx->input, "input",  NULL);
                nd_js_dispatch_event(ctx->w->js, ctx->input, "change", NULL);
            }
            nd_window_js_mutated(ctx->w);
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
    ctx->w = w;
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
        if (nd_input_is_text_like(p)) { *is_text = TRUE; return p; }
        if (is_button_like(p))  { *is_button = TRUE; return p; }
    }
    return NULL;
}

void
on_drawing_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data)
{
    (void)ctrl;
    nd_window *w = user_data;
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
    GdkCursor *cur = gdk_cursor_new_from_name(cursor_name, NULL);
    gtk_widget_set_cursor(w->drawing_area, cur);
    if (cur) g_object_unref(cur);
    if (href)
        nd_window_set_status(w, "%s", href);
}

static void
on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    nd_window *w = user_data;
    nd_window_mark_dead(w);
    g_clear_handle_id(&w->caret_blink_source, g_source_remove);
    g_clear_handle_id(&w->refresh_source, g_source_remove);
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
    if (w->js) nd_js_free(w->js);
    if (w->history) {
        for (guint i = 0; i < w->history->len; i++)
            g_free(g_ptr_array_index(w->history, i));
        g_ptr_array_free(w->history, TRUE);
    }
    if (w->images) nd_image_cache_free(w->images);
    if (w->videos) nd_video_cache_free(w->videos);
    if (w->audios) nd_audio_cache_free(w->audios);
    if (w->anim)   nd_anim_free(w->anim);
    if (w->external_stylesheets) g_ptr_array_free(w->external_stylesheets, TRUE);
    if (w->external_css_seen)    g_hash_table_destroy(w->external_css_seen);
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

static void
nd_spawn_window(GtkApplication *app, const char *url)
{
    nd_window_open(app, url);
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

static void
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
    char short_label[64];
    g_snprintf(short_label, sizeof short_label, "%s", show);
    gtk_label_set_text(GTK_LABEL(w->tab_label), short_label);
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

static void
nd_window_mark_alive(nd_window *w)
{
    if (!g_live_windows) g_live_windows = g_hash_table_new(NULL, NULL);
    g_hash_table_add(g_live_windows, w);
}

static void
nd_window_mark_dead(nd_window *w)
{
    if (g_live_windows) g_hash_table_remove(g_live_windows, w);
}

static gboolean
nd_window_alive(nd_window *w)
{
    return g_live_windows && g_hash_table_contains(g_live_windows, w);
}

static nd_window *
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
    w->audios  = nd_audio_cache_new();
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
    nd_window_build_status_bar(w, page);

    char page_name[32];
    g_snprintf(page_name, sizeof page_name, "tab-%p", (void *)w);
    gtk_stack_add_named(stack, page, page_name);

    GtkWidget *tab_button = gtk_button_new();
    gtk_widget_add_css_class(tab_button, "flat");
    gtk_widget_add_css_class(tab_button, "nd-tab");
    GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *tab_icon = gtk_image_new_from_icon_name("web-browser-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(tab_icon), 14);
    gtk_box_append(GTK_BOX(tab_box), tab_icon);
    GtkWidget *tab_label = gtk_label_new("New Tab");
    gtk_label_set_ellipsize(GTK_LABEL(tab_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(tab_label), 24);
    gtk_box_append(GTK_BOX(tab_box), tab_label);
    GtkWidget *close_button = gtk_button_new_from_icon_name("window-close");
    gtk_widget_add_css_class(close_button, "flat");
    gtk_widget_add_css_class(close_button, "nd-tab-close");
    gtk_widget_set_tooltip_text(close_button, "Close tab");
    g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked), w);
    gtk_box_append(GTK_BOX(tab_box), close_button);
    gtk_button_set_child(GTK_BUTTON(tab_button), tab_box);
    g_signal_connect(tab_button, "clicked", G_CALLBACK(on_tab_button_clicked), w);
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
        if (argv[i] && argv[i][0] != '-') {
            g_startup_url_override = g_strdup(argv[i]);
            break;
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
    if (w->current_fetch) g_cancellable_cancel(w->current_fetch);
}

static void
nd_save_pdf_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    nd_window *w = user_data;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file) return;
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;

    nd_window_ensure_layout(w, nd_layout_viewport());
    if (!w->layout_tree) { g_free(path); return; }
    double pw = nd_layout_viewport();
    double ph = w->layout_tree->content_height + 32;
    cairo_surface_t *surf = cairo_pdf_surface_create(path, pw, ph);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        nd_window_set_status(w, "Cannot write %s", path);
        g_free(path);
        return;
    }
    cairo_t *cr = cairo_create(surf);
    nd_paint(cr, w->layout_tree, NULL);
    cairo_destroy(cr);
    cairo_surface_finish(surf);
    cairo_surface_destroy(surf);
    nd_window_set_status(w, "Saved PDF: %s", path);
    g_free(path);
}

static void
on_win_save_pdf(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (!w->layout_tree) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save page as PDF");
    char *title_text = nd_window_current_title(w);
    char *suggested  = g_strdup_printf("%s.pdf",
        title_text && *title_text ? title_text : "page");
    g_free(title_text);
    gtk_file_dialog_set_initial_name(dialog, suggested);
    g_free(suggested);
    gtk_file_dialog_save(dialog, GTK_WINDOW(w->window), NULL,
                         nd_save_pdf_done, w);
    g_object_unref(dialog);
}

static void
nd_save_html_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    nd_window *w = user_data;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!file) return;
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;

    char *body = NULL;
    gsize body_len = 0;
    if (!w->dom_mutated && w->last_body && w->last_body_len > 0) {
        body = g_memdup2(w->last_body, w->last_body_len);
        body_len = w->last_body_len;
    } else if (w->parsed_doc) {
        body = nd_node_outer_html(w->parsed_doc);
        body_len = body ? strlen(body) : 0;
    }
    if (!body || body_len == 0) {
        g_free(body);
        nd_window_set_status(w, "Nothing to save");
        g_free(path);
        return;
    }

    GError *err = NULL;
    if (!g_file_set_contents(path, body, (gssize)body_len, &err)) {
        nd_window_set_status(w, "Cannot write %s: %s",
                             path, err ? err->message : "(unknown)");
        g_clear_error(&err);
    } else {
        nd_window_set_status(w, "Saved HTML: %s", path);
    }
    g_free(body);
    g_free(path);
}

static void
on_win_save_html(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (!w->parsed_doc && (!w->last_body || w->last_body_len == 0)) return;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save page as HTML");
    char *title_text = nd_window_current_title(w);
    char *suggested  = g_strdup_printf("%s.html",
        title_text && *title_text ? title_text : "page");
    g_free(title_text);
    gtk_file_dialog_set_initial_name(dialog, suggested);
    g_free(suggested);
    gtk_file_dialog_save(dialog, GTK_WINDOW(w->window), NULL,
                         nd_save_html_done, w);
    g_object_unref(dialog);
}

typedef struct nd_print_ctx {
    nd_window *w;
    double     scale;
    double     page_content_h;
    int        n_pages;
    double    *page_offsets;
    char      *header_title;
    char      *header_url;
} nd_print_ctx;

static void
nd_print_ctx_free(nd_print_ctx *pc)
{
    if (!pc) return;
    g_free(pc->page_offsets);
    g_free(pc->header_title);
    g_free(pc->header_url);
    g_free(pc);
}

static void
nd_print_collect_breakpoints(const nd_box *root, GArray *ys)
{
    if (!root) return;
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_BOX_BLOCK && c->kind != ND_BOX_TABLE &&
            c->kind != ND_BOX_TABLE_ROW && c->kind != ND_BOX_IMAGE &&
            c->kind != ND_BOX_VIDEO)
            continue;
        double top    = c->y + c->margin.top;
        double height = c->content_height + c->padding.top + c->padding.bottom +
                        c->border.top + c->border.bottom;
        double bottom = top + height;
        g_array_append_val(ys, top);
        g_array_append_val(ys, bottom);
        if (height > 0)
            nd_print_collect_breakpoints(c, ys);
    }
}

static int
nd_print_compare_double(gconstpointer a, gconstpointer b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

static void
nd_on_print_begin(GtkPrintOperation *op, GtkPrintContext *ctx, gpointer user_data)
{
    nd_print_ctx *pc = user_data;
    nd_window *w = pc->w;
    nd_window_ensure_layout(w, nd_layout_viewport());
    if (!w->layout_tree) {
        gtk_print_operation_set_n_pages(op, 1);
        pc->n_pages = 1;
        return;
    }
    double page_w = gtk_print_context_get_width(ctx);
    double page_h = gtk_print_context_get_height(ctx);
    double doc_w  = nd_layout_viewport();
    double doc_h  = w->layout_tree->content_height + 16;
    if (doc_w <= 0 || doc_h <= 0) {
        gtk_print_operation_set_n_pages(op, 1);
        pc->n_pages = 1;
        return;
    }
    pc->scale = page_w / doc_w;
    double header_h = 18.0;
    double footer_h = 18.0;
    pc->page_content_h = (page_h - header_h - footer_h) / pc->scale;
    if (pc->page_content_h < 100) pc->page_content_h = 100;

    GArray *breaks = g_array_new(FALSE, FALSE, sizeof(double));
    nd_print_collect_breakpoints(w->layout_tree, breaks);
    g_array_sort(breaks, nd_print_compare_double);

    GArray *offsets = g_array_new(FALSE, FALSE, sizeof(double));
    double zero = 0.0;
    g_array_append_val(offsets, zero);
    double tolerance = pc->page_content_h * 0.20;
    while (TRUE) {
        double cur = g_array_index(offsets, double, offsets->len - 1);
        if (cur + pc->page_content_h >= doc_h) break;
        double hard = cur + pc->page_content_h;
        double soft = cur + pc->page_content_h - tolerance;
        double next = hard;
        for (guint i = 0; i < breaks->len; i++) {
            double y = g_array_index(breaks, double, i);
            if (y > soft && y <= hard && y > cur + 16) {
                if (y > next - tolerance) next = y;
            }
        }
        if (next <= cur + 16) next = cur + pc->page_content_h;
        g_array_append_val(offsets, next);
    }

    pc->n_pages = (int)offsets->len;
    pc->page_offsets = g_new(double, pc->n_pages);
    for (int i = 0; i < pc->n_pages; i++)
        pc->page_offsets[i] = g_array_index(offsets, double, i);

    g_array_free(breaks, TRUE);
    g_array_free(offsets, TRUE);

    gtk_print_operation_set_n_pages(op, pc->n_pages);
}

static void
nd_on_print_draw_page(GtkPrintOperation *op, GtkPrintContext *ctx,
                      int page_nr, gpointer user_data)
{
    (void)op;
    nd_print_ctx *pc = user_data;
    nd_window *w = pc->w;
    cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
    double page_w = gtk_print_context_get_width(ctx);
    double page_h = gtk_print_context_get_height(ctx);

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    PangoLayout *header = gtk_print_context_create_pango_layout(ctx);
    pango_layout_set_text(header,
                          pc->header_title && *pc->header_title
                          ? pc->header_title : "Nordstjernen", -1);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 9");
    pango_layout_set_font_description(header, fd);
    pango_layout_set_width(header, (int)(page_w * PANGO_SCALE));
    pango_layout_set_ellipsize(header, PANGO_ELLIPSIZE_END);
    cairo_move_to(cr, 0, 2);
    pango_cairo_show_layout(cr, header);
    g_object_unref(header);

    PangoLayout *footer = gtk_print_context_create_pango_layout(ctx);
    char *footer_text = g_strdup_printf("%s   —   Page %d of %d",
        pc->header_url ? pc->header_url : "",
        page_nr + 1, pc->n_pages);
    pango_layout_set_text(footer, footer_text, -1);
    g_free(footer_text);
    pango_layout_set_font_description(footer, fd);
    pango_layout_set_width(footer, (int)(page_w * PANGO_SCALE));
    pango_layout_set_ellipsize(footer, PANGO_ELLIPSIZE_END);
    int fw, fh;
    pango_layout_get_pixel_size(footer, &fw, &fh);
    cairo_move_to(cr, 0, page_h - fh - 2);
    pango_cairo_show_layout(cr, footer);
    g_object_unref(footer);
    pango_font_description_free(fd);
    cairo_set_line_width(cr, 0.3);
    cairo_move_to(cr, 0, 16);
    cairo_line_to(cr, page_w, 16);
    cairo_move_to(cr, 0, page_h - 16);
    cairo_line_to(cr, page_w, page_h - 16);
    cairo_stroke(cr);
    cairo_restore(cr);

    if (!w->layout_tree) return;

    double offset = (pc->page_offsets && page_nr >= 0 && page_nr < pc->n_pages)
        ? pc->page_offsets[page_nr]
        : (double)page_nr * pc->page_content_h;

    cairo_save(cr);
    cairo_translate(cr, 0, 18.0);
    cairo_rectangle(cr, 0, 0, page_w, page_h - 36.0);
    cairo_clip(cr);
    cairo_scale(cr, pc->scale, pc->scale);
    cairo_translate(cr, 0, -offset);
    nd_paint(cr, w->layout_tree, NULL);
    cairo_restore(cr);
}

static void
nd_on_print_done(GtkPrintOperation *op, GtkPrintOperationResult result,
                 gpointer user_data)
{
    (void)op;
    nd_print_ctx *pc = user_data;
    if (nd_window_alive(pc->w)) {
        if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
            GError *err = NULL;
            gtk_print_operation_get_error(op, &err);
            nd_window_set_status(pc->w, "Print error: %s",
                                 err ? err->message : "unknown");
            g_clear_error(&err);
        } else if (result == GTK_PRINT_OPERATION_RESULT_APPLY) {
            nd_window_set_status(pc->w, "Sent %d page%s to printer",
                                 pc->n_pages, pc->n_pages == 1 ? "" : "s");
        }
    }
    nd_print_ctx_free(pc);
}

static void
on_win_print(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    nd_window_ensure_layout(w, nd_layout_viewport());
    if (!w->layout_tree) {
        nd_window_set_status(w, "Nothing to print");
        return;
    }
    nd_print_ctx *pc = g_new0(nd_print_ctx, 1);
    pc->w = w;
    pc->header_title = nd_window_current_title(w);
    pc->header_url   = g_strdup(nd_window_current_url(w));

    GtkPrintOperation *op = gtk_print_operation_new();
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_use_full_page(op, FALSE);
    gtk_print_operation_set_embed_page_setup(op, TRUE);
    gtk_print_operation_set_show_progress(op, TRUE);
    if (pc->header_title && *pc->header_title)
        gtk_print_operation_set_job_name(op, pc->header_title);
    else
        gtk_print_operation_set_job_name(op, "Nordstjernen page");

    g_signal_connect(op, "begin-print", G_CALLBACK(nd_on_print_begin),     pc);
    g_signal_connect(op, "draw-page",   G_CALLBACK(nd_on_print_draw_page), pc);
    g_signal_connect(op, "done",        G_CALLBACK(nd_on_print_done),      pc);

    GError *err = NULL;
    gtk_print_operation_run(op,
                            GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                            GTK_WINDOW(w->window), &err);
    if (err) {
        nd_window_set_status(w, "Print failed: %s", err->message);
        g_clear_error(&err);
    }
    g_object_unref(op);
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
    if (w->status_label) {
        char *msg = g_strdup_printf("Zoom: %d%%", (int)(w->zoom * 100 + 0.5));
        gtk_label_set_text(GTK_LABEL(w->status_label), msg);
        g_free(msg);
    }
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
on_win_find(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    if (!w->search_revealer) return;
    gtk_revealer_set_reveal_child(GTK_REVEALER(w->search_revealer), TRUE);
    gtk_widget_grab_focus(w->search_entry);
    gtk_editable_select_region(GTK_EDITABLE(w->search_entry), 0, -1);
}

static void
nd_window_update_match_count(nd_window *w)
{
    if (!w->search_count_label) return;
    if (!w->search_query || !w->layout_tree) {
        gtk_label_set_text(GTK_LABEL(w->search_count_label), "");
        return;
    }
    guint total = nd_box_count_matches(w->layout_tree, w->search_query,
                                       w->search_case_sensitive);
    guint cur = w->search_active_box
        ? nd_box_match_ordinal(w->layout_tree, w->search_query,
                               w->search_active_box,
                               w->search_case_sensitive)
        : 0;
    char *msg;
    if (total == 0)
        msg = g_strdup("No matches");
    else if (cur > 0)
        msg = g_strdup_printf("%u of %u", cur, total);
    else
        msg = g_strdup_printf("%u match%s", total, total == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(w->search_count_label), msg);
    g_free(msg);
}

void
on_search_changed(GtkEditable *entry, gpointer user_data)
{
    nd_window *w = user_data;
    g_free(w->search_query);
    const char *text = gtk_editable_get_text(entry);
    w->search_query = text && *text ? g_strdup(text) : NULL;
    w->search_active_box = NULL;
    nd_paint_set_search(w->search_case_sensitive, NULL);
    gtk_widget_queue_draw(w->drawing_area);
    nd_window_update_match_count(w);
}

static void
nd_window_scroll_to_match(nd_window *w, const nd_box *target)
{
    if (!target || !w->render_vadj) return;
    w->search_active_box = target;
    nd_paint_set_search(w->search_case_sensitive, target);
    double upper = gtk_adjustment_get_upper(w->render_vadj);
    double page  = gtk_adjustment_get_page_size(w->render_vadj);
    double y = target->y - 24;
    if (y > upper - page) y = upper - page;
    if (y < 0) y = 0;
    gtk_adjustment_set_value(w->render_vadj, y);
    gtk_widget_queue_draw(w->drawing_area);
    nd_window_update_match_count(w);
}

static void
nd_window_search_advance(nd_window *w, gboolean backward)
{
    if (!w->search_query || !w->layout_tree || !w->render_vadj) return;
    double current_y = gtk_adjustment_get_value(w->render_vadj);
    const nd_box *target;
    if (backward) {
        target = nd_box_first_match_above(w->layout_tree, w->search_query,
                                          current_y, w->search_case_sensitive);
        if (!target)
            target = nd_box_first_match_above(w->layout_tree, w->search_query,
                                              G_MAXDOUBLE,
                                              w->search_case_sensitive);
    } else {
        target = nd_box_first_match_below(w->layout_tree, w->search_query,
                                          current_y + 2,
                                          w->search_case_sensitive);
        if (!target)
            target = nd_box_first_match_below(w->layout_tree, w->search_query,
                                              -1, w->search_case_sensitive);
    }
    if (target) nd_window_scroll_to_match(w, target);
}

void
on_search_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    nd_window_search_advance(user_data, FALSE);
}

void
on_search_case_toggled(GtkToggleButton *btn, gpointer user_data)
{
    nd_window *w = user_data;
    w->search_case_sensitive = gtk_toggle_button_get_active(btn);
    w->search_active_box = NULL;
    nd_paint_set_search(w->search_case_sensitive, NULL);
    gtk_widget_queue_draw(w->drawing_area);
    nd_window_update_match_count(w);
}

void
on_search_stop(GtkSearchEntry *entry, gpointer user_data)
{
    (void)entry;
    nd_window *w = user_data;
    if (w->search_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(w->search_revealer), FALSE);
    w->search_active_box = NULL;
    nd_paint_set_search(w->search_case_sensitive, NULL);
    g_free(w->search_query);
    w->search_query = NULL;
    nd_window_update_match_count(w);
    gtk_widget_queue_draw(w->drawing_area);
    if (w->drawing_area) gtk_widget_grab_focus(w->drawing_area);
}

gboolean
on_search_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                      guint keycode, GdkModifierType state,
                      gpointer user_data)
{
    (void)ctrl; (void)keycode;
    nd_window *w = user_data;
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
        (state & GDK_SHIFT_MASK)) {
        nd_window_search_advance(w, TRUE);
        return TRUE;
    }
    return FALSE;
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
        { "open-console", G_CALLBACK(on_win_open_console) },
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
        "box.nd-toolbar > entry { min-height: 28px; margin: 0 4px; }\n";
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
        { "win.open-console", { "<Primary><Shift>j", NULL, NULL } },
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
        if (size > 0) {
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
    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].value) {
            const char *m = fields[i].value;
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

int
main(int argc, char **argv)
{
#ifdef G_OS_WIN32
    if (nd_win32_args_need_console(argc, argv))
        nd_win32_attach_parent_console();
#endif
    if (!nd_security_refuse_root()) return 77;
    init_self_exe(argc > 0 ? argv[0] : NULL);
    nd_security_sandbox_init(g_self_exe);
    nd_security_seccomp_init();
    nd_config_init();
    g_log_set_writer_func(nd_log_writer, NULL, NULL);
#ifdef G_OS_WIN32
    nd_win32_set_app_id();
    nd_win32_anchor_gtk_data();
#endif

    gboolean headless = FALSE;
    const char *proxy_override = NULL;
    nd_headless_opts hopts = {
        .url = NULL,
        .dump = ND_DUMP_TEXT,
        .out_path = NULL,
        .viewport_width = 1000,
        .settle_ms = 200,
    };
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--proxy=")) {
            proxy_override = argv[i] + 8;
            nd_net_set_proxy_override(proxy_override);
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
        } else if (g_str_has_prefix(argv[i], "--settle-ms=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 12, &end, 10);
            if (end != argv[i] + 12 && *end == '\0' && n >= 0 && n < 600000)
                hopts.settle_ms = (int)n;
        } else if (g_str_has_prefix(argv[i], "--url=")) {
            hopts.url = argv[i] + 6;
        } else if (argv[i][0] != '-' && !hopts.url) {
            hopts.url = argv[i];
        }
    }
    if (headless) {
        nd_net_init();
        nd_cache_init();
        int rc = nd_headless_run(&hopts);
        nd_cache_shutdown();
        nd_net_shutdown();
        nd_config_shutdown();
        return rc;
    }

    {
        const nd_config *cfg = nd_config_get();
        g_home_url = g_strdup(cfg && cfg->home_url ? cfg->home_url : "");
    }
    nd_net_init();
    nd_cache_init();
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
    g_free(g_context_menu_link);
    g_context_menu_link = NULL;
    nd_font_shutdown();
    nd_cache_shutdown();
    nd_net_shutdown();
    nd_config_shutdown();
    return status;
}
