/* Nordstjernen — GTK 4 application shell. */

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

#include "bookmarks.h"
#include "cache.h"
#include "config.h"
#include "css.h"
#include "headless.h"
#include "html.h"
#include "image.h"
#include "video.h"
#include "js.h"
#include "layout.h"
#include "net.h"
#include "paint.h"
#include "security.h"
#include "window.h"

#define ND_APP_ID     "com.nordstjernen.Browser"
#define ND_TITLE      "Nordstjernen"

static char         *g_startup_url_override;
static char         *g_self_exe;
static char         *g_home_url;
static nd_bookmarks *g_bookmarks;
static GFileMonitor *g_bookmarks_monitor;
static char         *g_context_menu_link;

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
static void nd_window_set_busy(nd_window *w, gboolean busy);
static void nd_window_render(nd_window *w);
static void nd_window_clear_cache(nd_window *w);
static void nd_window_update_nav_state(nd_window *w);
static void nd_window_open(GtkApplication *app, const char *startup_url);
static void nd_spawn_window(GtkApplication *app, const char *url);
static void nd_setup_bookmarks_watch(GtkApplication *app);
static void nd_window_kick_image_loads(nd_window *w);
static void nd_window_kick_video_loads(nd_window *w);
static void nd_window_refresh_bookmark_button(nd_window *w);
static const char *nd_window_current_url(nd_window *w);
static char       *nd_window_current_title(nd_window *w);
static void        nd_window_js_log(const char *line, gpointer user_data);
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
static void nd_window_maybe_submit_form(nd_window *w, const nd_node *clicked);
static char *nd_resolve_url(const nd_window *w, const char *href);
static void nd_on_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data);

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
nd_window_clear_cache(nd_window *w)
{
    if (w->refresh_source) {
        g_source_remove(w->refresh_source);
        w->refresh_source = 0;
    }
    g_free(w->last_body); w->last_body = NULL; w->last_body_len = 0;
    g_free(w->last_content_type); w->last_content_type = NULL;
    if (w->csp) { nd_csp_free(w->csp); w->csp = NULL; }
    if (w->layout_tree) { nd_box_free(w->layout_tree); w->layout_tree = NULL; }
    if (w->style_table) { g_hash_table_destroy(w->style_table); w->style_table = NULL; }
    if (w->parsed_doc)  { nd_node_free(w->parsed_doc);  w->parsed_doc  = NULL; }
    if (w->js)          { nd_js_free(w->js);            w->js          = NULL; }
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
}

static void
nd_window_scroll_to_fragment(nd_window *w)
{
    if (!w->pending_fragment || !w->layout_tree || !w->render_vadj) return;
    const nd_box *target = nd_box_find_by_id(w->layout_tree, w->pending_fragment);
    if (!target) return;
    double upper = gtk_adjustment_get_upper(w->render_vadj);
    double page  = gtk_adjustment_get_page_size(w->render_vadj);
    double y = target->y;
    if (y > upper - page) y = upper - page;
    if (y < 0) y = 0;
    gtk_adjustment_set_value(w->render_vadj, y);
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

static void
nd_window_js_mutated(gpointer user_data)
{
    nd_window *w = user_data;
    if (!w) return;
    if (w->layout_tree) { nd_box_free(w->layout_tree); w->layout_tree = NULL; }
    if (w->style_table) { g_hash_table_destroy(w->style_table); w->style_table = NULL; }
    if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
    nd_window_apply_page_title(w);
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
    double y = box->y;
    GtkAdjustment *adj = w->render_vadj;
    double upper = gtk_adjustment_get_upper(adj);
    double page  = gtk_adjustment_get_page_size(adj);
    if (y > upper - page) y = upper - page;
    if (y < 0) y = 0;
    gtk_adjustment_set_value(adj, y);
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
nd_clear_radio_group(nd_node *root, const char *name, const nd_node *keep)
{
    if (!root) return;
    if (root->kind == ND_NODE_ELEMENT && root->name &&
        strcmp(root->name, "input") == 0 && root != keep) {
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
    char *ename = g_uri_escape_string(name, NULL, FALSE);
    char *evalue = g_uri_escape_string(value ? value : "", NULL, FALSE);
    if (!*first) g_string_append_c(query, '&');
    g_string_append(query, ename);
    g_string_append_c(query, '=');
    g_string_append(query, evalue);
    *first = FALSE;
    g_free(ename); g_free(evalue);
}

static const nd_node *
select_chosen_option(const nd_node *select)
{
    const nd_node *first_opt = NULL;
    for (const nd_node *c = select->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "optgroup") == 0) {
            for (const nd_node *cc = c->first_child; cc; cc = cc->next_sibling) {
                if (cc->kind == ND_NODE_ELEMENT && cc->name && strcmp(cc->name, "option") == 0) {
                    if (!first_opt) first_opt = cc;
                    if (nd_element_get_attr(cc, "selected")) return cc;
                }
            }
        } else if (strcmp(c->name, "option") == 0) {
            if (!first_opt) first_opt = c;
            if (nd_element_get_attr(c, "selected")) return c;
        }
    }
    return first_opt;
}

static char *
option_value(const nd_node *option)
{
    if (!option) return NULL;
    const char *v = nd_element_get_attr(option, "value");
    if (v) return g_strdup(v);
    char *text = nd_node_collect_text(option);
    if (!text) return g_strdup("");
    return text;
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
                const nd_node *opt = select_chosen_option(n);
                char *v = option_value(opt);
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

static void
nd_window_maybe_submit_form(nd_window *w, const nd_node *clicked)
{
    if (!clicked) return;
    if (nd_element_get_attr(clicked, "disabled")) return;
    gboolean from_text_input = nd_input_is_text_like(clicked);
    gboolean from_js = (clicked->kind == ND_NODE_ELEMENT && clicked->name &&
                        strcmp(clicked->name, "form") == 0);
    if (!from_text_input && !from_js && !is_submit_trigger(clicked)) return;
    const nd_node *form = clicked;
    while (form && !(form->kind == ND_NODE_ELEMENT && form->name &&
                     strcmp(form->name, "form") == 0))
        form = form->parent;
    if (!form) return;

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

    GString *query = g_string_new(NULL);
    gboolean first = TRUE;
    form_collect_inputs(form, query, &first, clicked);

    const char *action = nd_element_get_attr(form, "action");
    const char *formaction = (!from_text_input && clicked) ?
        nd_element_get_attr(clicked, "formaction") : NULL;
    if (formaction && *formaction) action = formaction;
    char *abs_action;
    if (!action || !*action) abs_action = g_strdup(nd_window_current_url(w));
    else                      abs_action = nd_resolve_url(w, action);
    if (!abs_action) { g_string_free(query, TRUE); return; }

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
        gtk_editable_set_text(GTK_EDITABLE(w->url_entry), abs_action);
        w->current_fetch = g_cancellable_new();
        nd_window_set_busy(w, TRUE);
        nd_window_update_nav_state(w);
        nd_window_set_status(w, "POST %s …", abs_action);
        nd_net_post_async(abs_action, query->str, query->len,
                          "application/x-www-form-urlencoded",
                          w->current_fetch, nd_on_fetch_done, w);
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
nd_console_entry_activate(GtkEntry *entry, gpointer user_data)
{
    nd_window *w = user_data;
    const char *src = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!src || !*src) return;
    char *echo = g_strdup_printf("> %s", src);
    nd_window_console_append(w, echo);
    g_free(echo);
    const nd_config *cfg = nd_config_get();
    if (!w->js && cfg && cfg->javascript_enabled) {
        w->js = nd_js_new(nd_window_js_log, w,
                          nd_window_js_mutated, w,
                          nd_window_js_navigate, w);
        if (w->js) {
            nd_js_set_scroll_to_cb(w->js, nd_window_js_scroll_to, w);
            nd_js_set_form_submit_cb(w->js, nd_window_js_form_submit, w);
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
nd_window_open_console(nd_window *w)
{
    if (w->console.window) {
        gtk_window_present(GTK_WINDOW(w->console.window));
        if (w->console.entry) gtk_widget_grab_focus(w->console.entry);
        return;
    }
    w->console.window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(w->console.window), "JavaScript Console — Nordstjernen");
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
nd_window_apply_page_title(nd_window *w)
{
    if (!w->parsed_doc) {
        gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE);
        return;
    }
    nd_node *title = nd_node_find_first_element(w->parsed_doc, "title");
    if (!title) {
        gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE);
        return;
    }
    char *raw = nd_node_collect_text(title);
    if (!raw || !*raw) { g_free(raw); gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE); return; }
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
        gtk_window_set_title(GTK_WINDOW(w->window), full);
        g_free(full);
    } else {
        gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE);
    }
    g_string_free(trimmed, TRUE);
}

static void
nd_window_ensure_layout(nd_window *w, double viewport_width)
{
    if (!w->last_body) return;
    if (w->layout_tree && w->parsed_doc &&
        w->layout_tree->content_width >= viewport_width - 0.5 &&
        w->layout_tree->content_width <= viewport_width + 0.5)
        return;

    if (w->layout_tree) { nd_box_free(w->layout_tree); w->layout_tree = NULL; }
    if (w->style_table) { g_hash_table_destroy(w->style_table); w->style_table = NULL; }

    if (!w->parsed_doc)
        w->parsed_doc = nd_html_parse_for_page(w->last_body, (gssize)w->last_body_len);

    GPtrArray *page_sheets = g_ptr_array_new();
    nd_collect_inline_stylesheets(w->parsed_doc, page_sheets);
    guint page_sheets_count = page_sheets->len;

    if (w->external_stylesheets)
        for (guint i = 0; i < w->external_stylesheets->len; i++)
            g_ptr_array_add(page_sheets,
                            g_ptr_array_index(w->external_stylesheets, i));

    w->style_table = nd_css_compute(w->parsed_doc,
        (const nd_css_stylesheet *const *)page_sheets->pdata,
        page_sheets->len);

    for (guint i = 0; i < page_sheets_count; i++)
        nd_css_stylesheet_free(g_ptr_array_index(page_sheets, i));
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
                                     w->focused_input, w->caret_byte);
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
    return nd_html_decode_body(body, len, NULL);
}

static void
nd_window_render(nd_window *w)
{
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
        nd_node *doc = nd_html_parse_for_page(w->last_body, (gssize)w->last_body_len);
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

void
nd_on_drawing_pressed(GtkGestureClick *gesture, int n_press,
                      double x, double y, gpointer user_data)
{
    (void)n_press;
    nd_window *w = user_data;
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
                        nd_window_js_mutated(w);
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
                    if (cur->kind == ND_NODE_ELEMENT && cur->name &&
                        strcmp(cur->name, "label") == 0) {
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
    const char *href = link->href;
    if (g_str_has_prefix(href, "javascript:")) {
        const char *code = href + strlen("javascript:");
        if (w->js && *code) {
            char *result = nd_js_eval_source(w->js, code, "href");
            g_free(result);
            if (nd_js_consume_mutated(w->js)) nd_window_js_mutated(w);
        }
        return;
    }
    GdkEvent *event = gtk_event_controller_get_current_event(
        GTK_EVENT_CONTROLLER(gesture));
    GdkModifierType mods = event ? gdk_event_get_modifier_state(event) : 0;
    gboolean open_in_new_window =
        (mods & GDK_CONTROL_MASK) != 0 ||
        (link->target && strcmp(link->target, "_blank") == 0);
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
    if (abs_url) {
        if (open_in_new_window) {
            GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
            nd_spawn_window(app, abs_url);
        } else {
            nd_window_load_url(w, abs_url, ND_LOAD_USER);
        }
        g_free(abs_url);
    }
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
        { "ctx-open-link-new-window", G_CALLBACK(on_ctx_open_link_new_window) },
        { "ctx-copy-link",            G_CALLBACK(on_ctx_copy_link) },
        { "ctx-copy-url",             G_CALLBACK(on_ctx_copy_url) },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(items); i++) {
        GSimpleAction *a = g_simple_action_new(items[i].name, NULL);
        g_signal_connect(a, "activate", items[i].cb, w);
        g_action_map_add_action(G_ACTION_MAP(w->window), G_ACTION(a));
        g_object_unref(a);
    }
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
    const char *href = nd_box_hit_link(w->layout_tree, x, y);
    if (href) g_context_menu_link = g_strdup(href);

    GMenu *menu = g_menu_new();

    if (g_context_menu_link) {
        GMenu *link_section = g_menu_new();
        g_menu_append(link_section, "Open Link in New Window", "win.ctx-open-link-new-window");
        g_menu_append(link_section, "Copy Link Address",       "win.ctx-copy-link");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(link_section));
        g_object_unref(link_section);
    }

    GMenu *nav_section = g_menu_new();
    g_menu_append(nav_section, "Back",    "win.back");
    g_menu_append(nav_section, "Forward", "win.forward");
    g_menu_append(nav_section, "Reload",  "win.reload");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(nav_section));
    g_object_unref(nav_section);

    GMenu *page_section = g_menu_new();
    g_menu_append(page_section, "Copy Page URL",       "win.ctx-copy-url");
    g_menu_append(page_section, "Save Page As PDF…",   "win.print");
    g_menu_append(page_section, "JavaScript Console",  "win.open-console");
    g_menu_append(page_section, "Find on Page",        "win.find");
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
        if (w->refresh_source) {
            g_source_remove(w->refresh_source);
            w->refresh_source = 0;
        }
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
    if (w->caret_blink_source) {
        g_source_remove(w->caret_blink_source);
        w->caret_blink_source = 0;
    }
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
    nd_input_set_value(target, s->str);
    w->caret_byte = del_start + insert_len;
    g_string_free(s, TRUE);
    if (w->js) {
        nd_js_dispatch_event(w->js, target, "input", NULL);
        (void)nd_js_consume_mutated(w->js);
    }
    nd_window_reset_caret_blink(w);
    nd_window_js_mutated(w);
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
    if (w->layout_tree) { nd_box_free(w->layout_tree); w->layout_tree = NULL; }
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
            nd_js_dispatch_event(w->js, old, "blur", NULL);
        }
        g_free(w->focused_input_initial);
        w->focused_input_initial = NULL;
    }
    w->focused_input = target;
    w->caret_byte = 0;
    if (w->caret_blink_source) {
        g_source_remove(w->caret_blink_source);
        w->caret_blink_source = 0;
    }
    nd_paint_set_caret_visible(TRUE);
    if (target) {
        nd_window_ensure_im_context(w);
        w->focused_input_initial = g_strdup(nd_input_current_value(target));
        w->caret_byte = w->focused_input_initial ? strlen(w->focused_input_initial) : 0;
        w->caret_blink_on = TRUE;
        w->caret_blink_source = g_timeout_add(530, nd_window_caret_blink_tick, w);
        if (w->im_context) gtk_im_context_focus_in(w->im_context);
        if (w->js)
            nd_js_dispatch_event(w->js, target, "focus", NULL);
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
nd_draw_render(GtkDrawingArea *area, cairo_t *cr,
               int width, int height, gpointer user_data)
{
    (void)area;
    (void)height;
    nd_window *w = user_data;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (!w->last_body || !is_html_content_type(w->last_content_type))
        return;
    nd_window_ensure_layout(w, (double)width);
    if (!w->layout_tree) return;
    nd_paint(cr, w->layout_tree, w->search_query);
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
    (void)img;
    nd_window *w = user_data;
    if (w->mode == ND_VIEW_RENDER && w->drawing_area)
        gtk_widget_queue_draw(w->drawing_area);
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
} nd_css_fetch;

static void
on_external_css_loaded(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_css_fetch *fetch = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);
    if (err) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) && fetch->w)
            nd_window_set_status(fetch->w, "CSS fetch failed: %s", err->message);
        g_error_free(err);
        nd_response_free(resp);
        g_free(fetch->url);
        g_free(fetch);
        return;
    }
    if (!resp) { g_free(fetch->url); g_free(fetch); return; }
    nd_window *w = fetch->w;
    if (resp->body && resp->body->len > 0 && w && w->external_stylesheets) {
        nd_css_stylesheet *sh = nd_css_stylesheet_parse(
            (const char *)resp->body->data, (gssize)resp->body->len);
        if (sh) {
            g_ptr_array_add(w->external_stylesheets, sh);
            if (w->layout_tree) { nd_box_free(w->layout_tree); w->layout_tree = NULL; }
            if (w->style_table) { g_hash_table_destroy(w->style_table); w->style_table = NULL; }
            if (w->drawing_area) gtk_widget_queue_draw(w->drawing_area);
        }
    }
    nd_response_free(resp);
    g_free(fetch->url);
    g_free(fetch);
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
        if (n->kind == ND_NODE_ELEMENT && n->name &&
            strcmp(n->name, "link") == 0) {
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
                    nd_css_fetch *fetch = g_new0(nd_css_fetch, 1);
                    fetch->w = w;
                    fetch->url = abs;
                    nd_net_fetch_async(abs, w->css_cancellable,
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

static void
nd_window_kick_image_loads(nd_window *w)
{
    if (!w->layout_tree || !w->images) return;
    GPtrArray *imgs = g_ptr_array_new();
    nd_layout_collect_images(w->layout_tree, imgs);
    for (guint i = 0; i < imgs->len; i++) {
        nd_box *box = g_ptr_array_index(imgs, i);
        if (!box->image_src) continue;
        char *abs = nd_resolve_url(w, box->image_src);
        if (!abs) continue;
        if (nd_window_subresource_blocked(w, abs, ND_CSP_IMG, "image")) {
            g_free(abs);
            continue;
        }
        box->image = nd_image_cache_get(w->images, abs, on_image_ready, w);
        g_free(abs);
    }
    g_ptr_array_free(imgs, TRUE);
}

static void
nd_window_kick_video_loads(nd_window *w)
{
    if (!w->layout_tree || !w->videos) return;
    GPtrArray *vids = g_ptr_array_new();
    nd_layout_collect_videos(w->layout_tree, vids);
    for (guint i = 0; i < vids->len; i++) {
        nd_box *box = g_ptr_array_index(vids, i);
        if (!box->video_src) continue;
        char *abs = nd_resolve_url(w, box->video_src);
        if (!abs) continue;
        if (nd_window_subresource_blocked(w, abs, ND_CSP_MEDIA, "video")) {
            g_free(abs);
            continue;
        }
        char *poster_abs = NULL;
        if (box->video_poster) poster_abs = nd_resolve_url(w, box->video_poster);
        if (poster_abs &&
            nd_window_subresource_blocked(w, poster_abs, ND_CSP_IMG, "video-poster")) {
            g_free(poster_abs);
            poster_abs = NULL;
        }
        box->video = nd_video_cache_get(w->videos, abs,
                                        poster_abs, on_video_ready, w);
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
                       : "https://duckduckgo.com/lite/?q=%s";
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

static void
nd_on_fetch_done(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    nd_window *w = user_data;
    GError *err = NULL;
    nd_response *resp = nd_net_fetch_finish(result, &err);

    g_clear_object(&w->current_fetch);
    nd_window_set_busy(w, FALSE);

    if (!resp) {
        if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            nd_window_set_status(w, "Cancelled");
        else
            nd_window_set_status(w, "Error: %s", err ? err->message : "unknown");
        if (err)
            g_error_free(err);
        return;
    }

    if (resp->error) {
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
        w->mode = ND_VIEW_RENDER;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(w->view_dropdown),
                                   (guint)w->mode);
        nd_window_render(w);
        nd_window_ensure_layout(w, nd_layout_viewport());
        gtk_window_set_title(GTK_WINDOW(w->window), "Error — " ND_TITLE);
        nd_response_free(resp);
        return;
    }

    if (resp->tls_warning) {
        nd_window_set_status(w, "%s", resp->tls_warning);
    } else if (resp->status >= 400) {
        nd_window_set_status(w, "%ld %s", resp->status,
                             resp->final_url ? resp->final_url : "");
    }

    nd_window_clear_cache(w);
    if (resp->body && resp->body->len > 0) {
        char *decoded = nd_html_decode_body((const char *)resp->body->data,
                                    resp->body->len, resp->content_type);
        w->last_body = decoded;
        w->last_body_len = strlen(decoded);
    }
    w->last_content_type = g_strdup(resp->content_type ? resp->content_type : "");
    if (resp->csp_header && *resp->csp_header)
        w->csp = nd_csp_parse(resp->csp_header);

    if (is_html_content_type(w->last_content_type))
        w->mode = ND_VIEW_RENDER;
    else
        w->mode = ND_VIEW_RAW;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->view_dropdown),
                               (guint)w->mode);

    nd_window_render(w);
    if (is_html_content_type(w->last_content_type)) {
        nd_window_ensure_layout(w, nd_layout_viewport());
        nd_window_apply_page_title(w);
    } else {
        gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE);
    }
    nd_window_refresh_bookmark_button(w);
    if (w->pending_fragment && w->render_vadj) {
        nd_window_scroll_to_fragment(w);
    } else if (w->render_vadj) {
        gtk_adjustment_set_value(w->render_vadj, 0);
    }

    if (w->parsed_doc) {
        nd_window_apply_meta_refresh(w);
        const nd_config *cfg = nd_config_get();
        if (!w->js && cfg && cfg->javascript_enabled) {
            w->js = nd_js_new(nd_window_js_log, w,
                              nd_window_js_mutated, w,
                              nd_window_js_navigate, w);
            if (w->js) {
                nd_js_set_scroll_to_cb(w->js, nd_window_js_scroll_to, w);
                nd_js_set_form_submit_cb(w->js, nd_window_js_form_submit, w);
            }
        }
        if (w->js) {
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

    gtk_editable_set_text(GTK_EDITABLE(w->url_entry), url);

    w->current_fetch = g_cancellable_new();
    nd_window_set_busy(w, TRUE);
    nd_window_update_nav_state(w);
    nd_window_set_status(w, "Loading %s …", url);
    nd_net_fetch_async(url, w->current_fetch, nd_on_fetch_done, w);
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
nd_window_refresh_bookmark_button(nd_window *w)
{
    const char *url = nd_window_current_url(w);
    gboolean star_on = url && g_bookmarks && nd_bookmarks_contains(g_bookmarks, url);
    gtk_button_set_icon_name(GTK_BUTTON(w->bookmark_button),
        star_on ? "starred-symbolic" : "non-starred-symbolic");
    gtk_widget_set_tooltip_text(w->bookmark_button,
        star_on ? "Remove bookmark for this page" : "Bookmark this page");
}

void
on_bookmark_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    const char *url = nd_window_current_url(w);
    if (!url || !g_bookmarks) return;
    if (nd_bookmarks_contains(g_bookmarks, url)) {
        nd_bookmarks_remove(g_bookmarks, url);
    } else {
        char *title = nd_window_current_title(w);
        nd_bookmarks_add(g_bookmarks, url, title ? title : url);
        g_free(title);
    }
    nd_window_refresh_bookmark_button(w);
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

void
on_bookmarks_clicked(GtkButton *button, gpointer user_data)
{
    nd_window *w = user_data;
    if (!g_bookmarks) return;
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, 360, -1);
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
        GtkWidget *row = gtk_button_new();
        gtk_button_set_has_frame(GTK_BUTTON(row), FALSE);
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
        gtk_button_set_child(GTK_BUTTON(row), rowbox);
        g_object_set_data_full(G_OBJECT(row), "nd-url", g_strdup(b->url), g_free);
        g_signal_connect(row, "clicked", G_CALLBACK(on_bookmark_open), w);
        gtk_box_append(GTK_BOX(box), row);
    }
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_widget_set_parent(popover, GTK_WIDGET(button));
    gtk_popover_popup(GTK_POPOVER(popover));
}

static gboolean
is_text_input(const nd_node *n)
{
    if (!n || n->kind != ND_NODE_ELEMENT || !n->name) return FALSE;
    if (strcmp(n->name, "textarea") == 0) return TRUE;
    if (strcmp(n->name, "input") != 0) return FALSE;
    const char *type = nd_element_get_attr(n, "type");
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "text") == 0 ||
           g_ascii_strcasecmp(type, "search") == 0 ||
           g_ascii_strcasecmp(type, "email") == 0 ||
           g_ascii_strcasecmp(type, "url") == 0 ||
           g_ascii_strcasecmp(type, "tel") == 0 ||
           g_ascii_strcasecmp(type, "number") == 0 ||
           g_ascii_strcasecmp(type, "password") == 0;
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
        if (n->kind == ND_NODE_ELEMENT && n->name &&
            strcmp(n->name, "option") == 0)
            nd_element_remove_attr(n, "selected");
        for (nd_node *c = n->first_child; c; c = c->next_sibling)
            g_queue_push_tail(&queue, c);
    }
    nd_element_set_attr(ctx->option, "selected", "");
    nd_window_js_mutated(ctx->w);
    if (ctx->w->js)
        nd_js_dispatch_event(ctx->w->js, ctx->select_node, "change", NULL);
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
        if (n->kind == ND_NODE_ELEMENT && n->name &&
            strcmp(n->name, "option") == 0) {
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

static const nd_node *
find_form_role_ancestor(const nd_node *n, gboolean *is_text, gboolean *is_button)
{
    *is_text = FALSE;
    *is_button = FALSE;
    for (const nd_node *p = n; p; p = p->parent) {
        if (is_text_input(p))   { *is_text = TRUE;   return p; }
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
                if (p->kind == ND_NODE_ELEMENT && p->name &&
                    strcmp(p->name, "a") == 0) {
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

void
on_view_changed(GObject *dropdown, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    nd_window *w = user_data;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    if (sel == GTK_INVALID_LIST_POSITION) return;
    w->mode = (nd_view_mode)sel;
    nd_window_render(w);
}

static void
on_window_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    nd_window *w = user_data;
    if (w->caret_blink_source) {
        g_source_remove(w->caret_blink_source);
        w->caret_blink_source = 0;
    }
    if (w->refresh_source) {
        g_source_remove(w->refresh_source);
        w->refresh_source = 0;
    }
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
nd_spawn_window(GtkApplication *app, const char *url)
{
    if (!g_self_exe) {
        nd_window_open(app, url);
        return;
    }
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, g_self_exe);
    if (url && *url) g_ptr_array_add(args, (gpointer)url);
    g_ptr_array_add(args, NULL);
    GError *err = NULL;
    gboolean ok = g_spawn_async(NULL, (char **)args->pdata, NULL,
                                G_SPAWN_SEARCH_PATH_FROM_ENVP | G_SPAWN_DEFAULT,
                                NULL, NULL, NULL, &err);
    g_ptr_array_free(args, TRUE);
    if (!ok) {
        g_clear_error(&err);
        nd_window_open(app, url);
    }
}

static void
nd_window_open(GtkApplication *app, const char *startup_url)
{
    nd_window *w = g_new0(nd_window, 1);

    w->history = g_ptr_array_new();
    w->cursor  = -1;
    w->images  = nd_image_cache_new();
    w->videos  = nd_video_cache_new();
    w->zoom    = 1.0;

    w->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE);
    const nd_config *cfg = nd_config_get();
    int win_w = cfg && cfg->window_width_px  > 0 ? cfg->window_width_px  : 1280;
    int win_h = cfg && cfg->window_height_px > 0 ? cfg->window_height_px :  800;
    gtk_window_set_default_size(GTK_WINDOW(w->window), win_w, win_h);
    g_object_set_data(G_OBJECT(w->window), "nd-window", w);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_window_destroy), w);
    nd_window_install_actions(w);
    nd_install_ctx_actions(w);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(w->window), header);
    nd_window_build_toolbar(w, header, g_home_url);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(w->window), vbox);
    nd_window_build_search_bar(w, vbox);
    nd_window_build_content(w, vbox);
    nd_window_build_status_bar(w, vbox);

    gtk_widget_grab_focus(w->url_entry);
    gtk_window_maximize(GTK_WINDOW(w->window));
    gtk_window_present(GTK_WINDOW(w->window));

    const char *url = startup_url;
    if (!url || !*url) url = g_home_url;
    nd_window_load_url(w, url, ND_LOAD_USER);
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
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
on_win_print(GSimpleAction *action, GVariant *parameter, gpointer user_data)
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
nd_window_after_zoom(nd_window *w)
{
    if (w->layout_tree) { nd_box_free(w->layout_tree); w->layout_tree = NULL; }
    if (w->style_table) { g_hash_table_destroy(w->style_table); w->style_table = NULL; }
    if (w->parsed_doc)  { nd_node_free(w->parsed_doc);  w->parsed_doc  = NULL; }
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
    guint n = nd_box_count_matches(w->layout_tree, w->search_query);
    char *msg = g_strdup_printf("%u match%s", n, n == 1 ? "" : "es");
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
    gtk_widget_queue_draw(w->drawing_area);
    nd_window_update_match_count(w);
}

void
on_search_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    nd_window *w = user_data;
    if (!w->search_query || !w->layout_tree || !w->render_vadj) return;
    double current_y = gtk_adjustment_get_value(w->render_vadj);
    const nd_box *next = nd_box_first_match_below(w->layout_tree,
                                                  w->search_query,
                                                  current_y + 2);
    if (!next) {
        next = nd_box_first_match_below(w->layout_tree, w->search_query, -1);
        if (!next) return;
    }
    double upper = gtk_adjustment_get_upper(w->render_vadj);
    double page  = gtk_adjustment_get_page_size(w->render_vadj);
    double y = next->y - 24;
    if (y > upper - page) y = upper - page;
    if (y < 0) y = 0;
    gtk_adjustment_set_value(w->render_vadj, y);
}

static void
on_win_close(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    nd_window *w = user_data;
    gtk_window_destroy(GTK_WINDOW(w->window));
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
        { "open-console", G_CALLBACK(on_win_open_console) },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(actions); i++) {
        GSimpleAction *a = g_simple_action_new(actions[i].name, NULL);
        g_signal_connect(a, "activate", actions[i].cb, w);
        g_action_map_add_action(G_ACTION_MAP(w->window), G_ACTION(a));
        g_object_unref(a);
    }
}

static void
nd_install_actions(GtkApplication *app)
{
    GSimpleAction *new_window = g_simple_action_new("new-window", NULL);
    g_signal_connect(new_window, "activate", G_CALLBACK(on_app_new_window), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(new_window));
    g_object_unref(new_window);

    GSimpleAction *quit = g_simple_action_new("quit", NULL);
    g_signal_connect(quit, "activate", G_CALLBACK(on_app_quit), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(quit));
    g_object_unref(quit);

    nd_setup_bookmarks_watch(app);

    const struct {
        const char *action;
        const char *accels[3];
    } binds[] = {
        { "app.new-window", { "<Primary>n", "<Primary>t", NULL } },
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
    nd_bookmarks_free(g_bookmarks);
    g_bookmarks = nd_bookmarks_load();
    GList *list = gtk_application_get_windows(app);
    for (GList *l = list; l; l = l->next) {
        nd_window *w = g_object_get_data(G_OBJECT(l->data), "nd-window");
        if (w) nd_window_refresh_bookmark_button(w);
    }
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
        g_error_free(err);
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

#ifdef G_OS_WIN32
static GLogWriterOutput
nd_log_writer_win32(GLogLevelFlags log_level,
                    const GLogField *fields, gsize n_fields,
                    gpointer user_data)
{
    (void)user_data;
    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].value) {
            const char *m = fields[i].value;
            if (strstr(m, "win32 session dbus binary not found"))
                return G_LOG_WRITER_HANDLED;
        }
    }
    return g_log_writer_default(log_level, fields, n_fields, user_data);
}

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
    nd_config_init();
#ifdef G_OS_WIN32
    g_log_set_writer_func(nd_log_writer_win32, NULL, NULL);
    nd_win32_set_app_id();
    nd_win32_anchor_gtk_data();
#endif

    gboolean headless = FALSE;
    nd_headless_opts hopts = {
        .url = NULL,
        .dump = ND_DUMP_TEXT,
        .out_path = NULL,
        .viewport_width = 1000,
        .settle_ms = 200,
    };
    for (int i = 1; i < argc; i++) {
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
            if      (g_str_has_prefix(v, "text"))   hopts.dump = ND_DUMP_TEXT;
            else if (g_str_has_prefix(v, "dom"))    hopts.dump = ND_DUMP_DOM;
            else if (g_str_has_prefix(v, "layout")) hopts.dump = ND_DUMP_LAYOUT;
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
    g_bookmarks = nd_bookmarks_load();

    GApplicationFlags app_flags = G_APPLICATION_HANDLES_COMMAND_LINE |
                                  G_APPLICATION_NON_UNIQUE;
    GtkApplication *app = gtk_application_new(ND_APP_ID, app_flags);
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
    nd_cache_shutdown();
    nd_net_shutdown();
    nd_config_shutdown();
    return status;
}
