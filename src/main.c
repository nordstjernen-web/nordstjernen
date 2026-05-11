/* Nordstjernen — GTK 4 application shell. */

#include <gtk/gtk.h>
#include <string.h>

#include "css.h"
#include "html.h"
#include "image.h"
#include "layout.h"
#include "net.h"
#include "paint.h"

#define ND_APP_ID     "com.nordstjernen.Browser"
#define ND_TITLE      "Nordstjernen"
#define ND_DEFAULT_W  1280
#define ND_DEFAULT_H  800
#define ND_HOME_URL   "https://duckduckgo.com"

typedef enum nd_view_mode {
    ND_VIEW_RENDER = 0,
    ND_VIEW_RAW = 1,
    ND_VIEW_DOM = 2,
    ND_VIEW_LAYOUT = 3,
} nd_view_mode;

static char *g_startup_url_override;

#define ND_LAYOUT_VIEWPORT 1000.0

typedef struct nd_window {
    GtkWidget    *window;
    GtkWidget    *url_entry;
    GtkWidget    *back_button;
    GtkWidget    *forward_button;
    GtkWidget    *home_button;
    GtkWidget    *reload_button;
    GtkWidget    *about_button;
    GtkWidget    *go_button;
    GtkWidget    *stop_button;
    GtkWidget    *view_dropdown;
    GtkWidget    *content_stack;
    GtkWidget    *text_view;
    GtkWidget    *drawing_area;
    GtkAdjustment *render_vadj;
    nd_box       *layout_tree;
    GHashTable   *style_table;
    nd_node      *parsed_doc;
    GtkWidget    *status_label;
    GCancellable *current_fetch;
    nd_view_mode  mode;

    GPtrArray    *history;
    int           cursor;

    char         *last_body;
    gsize         last_body_len;
    char         *last_content_type;

    nd_image_cache *images;
} nd_window;

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
static void nd_window_kick_image_loads(nd_window *w);

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
    g_free(w->last_body); w->last_body = NULL; w->last_body_len = 0;
    g_free(w->last_content_type); w->last_content_type = NULL;
    if (w->layout_tree) { nd_box_free(w->layout_tree); w->layout_tree = NULL; }
    if (w->style_table) { g_hash_table_destroy(w->style_table); w->style_table = NULL; }
    if (w->parsed_doc)  { nd_node_free(w->parsed_doc);  w->parsed_doc  = NULL; }
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
    if (w->parsed_doc)  { nd_node_free(w->parsed_doc);  w->parsed_doc  = NULL; }

    w->parsed_doc = nd_html_parse(w->last_body, (gssize)w->last_body_len);
    w->style_table = nd_css_compute(w->parsed_doc, NULL, 0);
    w->layout_tree = nd_layout_build(w->parsed_doc, w->style_table, viewport_width);
    nd_window_apply_page_title(w);
    nd_window_kick_image_loads(w);
}

static gboolean
is_html_content_type(const char *ct)
{
    if (!ct) return FALSE;
    return g_ascii_strncasecmp(ct, "text/html", 9) == 0 ||
           g_ascii_strncasecmp(ct, "application/xhtml+xml", 21) == 0;
}

static char *
to_utf8_or_pass(const char *body, gsize len)
{
    if (!body || len == 0) return g_strdup("");
    if (g_utf8_validate(body, (gssize)len, NULL))
        return g_strndup(body, len);
    GError *err = NULL;
    gsize  written = 0;
    char  *out = g_convert(body, (gssize)len,
                           "UTF-8", "ISO-8859-1",
                           NULL, &written, &err);
    if (out) return out;
    if (err) g_error_free(err);
    return g_strdup("(non-UTF-8 body; charset detection not implemented yet)\n");
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
        nd_node *doc = nd_html_parse(w->last_body, (gssize)w->last_body_len);
        GString *dump = nd_node_dump(doc);
        nd_window_set_body_text(w, dump->str, (gssize)dump->len);
        g_string_free(dump, TRUE);
        nd_node_free(doc);
        return;
    }

    if (w->mode == ND_VIEW_LAYOUT && is_html) {
        nd_window_ensure_layout(w, ND_LAYOUT_VIEWPORT);
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
nd_on_drawing_pressed(GtkGestureClick *gesture, int n_press,
                      double x, double y, gpointer user_data)
{
    (void)n_press;
    nd_window *w = user_data;
    if (!w->layout_tree) return;
    const char *href = nd_box_hit_link(w->layout_tree, x, y);
    if (!href) return;
    GdkEvent *event = gtk_event_controller_get_current_event(
        GTK_EVENT_CONTROLLER(gesture));
    GdkModifierType mods = event ? gdk_event_get_modifier_state(event) : 0;
    gboolean open_in_new_window =
        (mods & GDK_CONTROL_MASK) != 0;
    char *abs_url = NULL;
    if (g_str_has_prefix(href, "http://") || g_str_has_prefix(href, "https://")) {
        abs_url = g_strdup(href);
    } else if (w->cursor >= 0 && w->cursor < (int)w->history->len) {
        const char *base = g_ptr_array_index(w->history, w->cursor);
        if (g_str_has_prefix(href, "//")) {
            abs_url = g_strconcat("https:", href, NULL);
        } else if (href[0] == '/') {
            const char *scheme_end = strstr(base, "://");
            if (scheme_end) {
                const char *host_start = scheme_end + 3;
                const char *host_end = strchr(host_start, '/');
                gsize host_len = host_end ? (gsize)(host_end - base) : strlen(base);
                abs_url = g_strconcat(g_strndup(base, host_len), href, NULL);
            }
        } else if (g_str_has_prefix(href, "#") || g_str_has_prefix(href, "javascript:") ||
                   g_str_has_prefix(href, "mailto:")) {
            return;
        } else {
            const char *q = strrchr(base, '/');
            if (q && q > strstr(base, "://") + 2) {
                gsize prefix_len = (gsize)(q - base) + 1;
                abs_url = g_strconcat(g_strndup(base, prefix_len), href, NULL);
            } else {
                abs_url = g_strconcat(base, "/", href, NULL);
            }
        }
    }
    if (abs_url) {
        if (open_in_new_window) {
            GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
            if (app) nd_window_open(app, abs_url);
            else     nd_window_load_url(w, abs_url, ND_LOAD_USER);
        } else {
            nd_window_load_url(w, abs_url, ND_LOAD_USER);
        }
        g_free(abs_url);
    }
}

static void
nd_on_drawing_pressed_middle(GtkGestureClick *gesture, int n_press,
                             double x, double y, gpointer user_data)
{
    (void)gesture; (void)n_press;
    nd_window *w = user_data;
    if (!w->layout_tree) return;
    const char *href = nd_box_hit_link(w->layout_tree, x, y);
    if (!href) return;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(w->window));
    if (!app) return;
    if (g_str_has_prefix(href, "http://") || g_str_has_prefix(href, "https://")) {
        nd_window_open(app, href);
        return;
    }
    if (w->cursor >= 0 && w->cursor < (int)w->history->len) {
        const char *base = g_ptr_array_index(w->history, w->cursor);
        char *abs_url = NULL;
        if (g_str_has_prefix(href, "//")) {
            abs_url = g_strconcat("https:", href, NULL);
        } else if (href[0] == '/') {
            const char *scheme_end = strstr(base, "://");
            if (scheme_end) {
                const char *host_start = scheme_end + 3;
                const char *host_end = strchr(host_start, '/');
                gsize host_len = host_end ? (gsize)(host_end - base) : strlen(base);
                char *root = g_strndup(base, host_len);
                abs_url = g_strconcat(root, href, NULL);
                g_free(root);
            }
        }
        if (abs_url) {
            nd_window_open(app, abs_url);
            g_free(abs_url);
        }
    }
}

static void
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
    double content_h = w->layout_tree->content_height;
    int min_h = (int)(content_h + 32);
    if (min_h < height) min_h = height;
    gtk_widget_set_size_request(GTK_WIDGET(area), -1, min_h);
    nd_paint(cr, w->layout_tree);
}

static char *
nd_resolve_url(const nd_window *w, const char *href)
{
    if (!href || !*href) return NULL;
    if (g_str_has_prefix(href, "http://") ||
        g_str_has_prefix(href, "https://") ||
        g_str_has_prefix(href, "data:") ||
        g_str_has_prefix(href, "about:"))
        return g_strdup(href);
    if (w->cursor < 0 || w->cursor >= (int)w->history->len) return NULL;
    const char *base = g_ptr_array_index(w->history, w->cursor);
    if (g_str_has_prefix(href, "//")) return g_strconcat("https:", href, NULL);
    if (href[0] == '/') {
        const char *scheme_end = strstr(base, "://");
        if (!scheme_end) return NULL;
        const char *host_start = scheme_end + 3;
        const char *host_end = strchr(host_start, '/');
        gsize host_len = host_end ? (gsize)(host_end - base) : strlen(base);
        char *root = g_strndup(base, host_len);
        char *full = g_strconcat(root, href, NULL);
        g_free(root);
        return full;
    }
    const char *q = strrchr(base, '/');
    if (q && q > strstr(base, "://") + 2) {
        gsize prefix_len = (gsize)(q - base) + 1;
        char *prefix = g_strndup(base, prefix_len);
        char *full = g_strconcat(prefix, href, NULL);
        g_free(prefix);
        return full;
    }
    return g_strconcat(base, "/", href, NULL);
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
        box->image = nd_image_cache_get(w->images, abs, on_image_ready, w);
        g_free(abs);
    }
    g_ptr_array_free(imgs, TRUE);
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

    char *bare = g_strndup(raw, len);
    char *full = g_strconcat("https://", bare, NULL);
    g_free(bare);
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
        nd_window_set_body_text(w, "", 0);
        nd_response_free(resp);
        return;
    }

    nd_window_clear_cache(w);
    if (resp->body && resp->body->len > 0) {
        w->last_body = g_memdup2(resp->body->data, resp->body->len);
        w->last_body_len = resp->body->len;
    }
    w->last_content_type = g_strdup(resp->content_type ? resp->content_type : "");

    if (is_html_content_type(w->last_content_type))
        w->mode = ND_VIEW_RENDER;
    else
        w->mode = ND_VIEW_RAW;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->view_dropdown),
                               (guint)w->mode);

    nd_window_render(w);
    if (is_html_content_type(w->last_content_type)) {
        nd_window_ensure_layout(w, ND_LAYOUT_VIEWPORT);
        nd_window_apply_page_title(w);
    } else {
        gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE);
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

static void
on_go_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->url_entry));
    nd_window_load_url(w, text, ND_LOAD_USER);
}

static void
on_stop_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    if (w->current_fetch)
        g_cancellable_cancel(w->current_fetch);
}

static void
on_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    nd_window *w = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->url_entry));
    nd_window_load_url(w, text, ND_LOAD_USER);
}

static void
on_back_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    if (w->cursor <= 0) return;
    w->cursor--;
    const char *url = g_ptr_array_index(w->history, w->cursor);
    nd_window_load_url(w, url, ND_LOAD_HISTORY);
}

static void
on_forward_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    if (w->cursor < 0 || w->cursor + 1 >= (int)w->history->len) return;
    w->cursor++;
    const char *url = g_ptr_array_index(w->history, w->cursor);
    nd_window_load_url(w, url, ND_LOAD_HISTORY);
}

static void
on_home_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    nd_window_load_url(w, ND_HOME_URL, ND_LOAD_USER);
}

static void
on_reload_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    if (w->cursor < 0 || w->cursor >= (int)w->history->len) return;
    const char *cur = g_ptr_array_index(w->history, w->cursor);
    nd_window_load_url(w, cur, ND_LOAD_HISTORY);
}

static void
on_about_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    nd_window_load_url(w, "about:mozilla", ND_LOAD_USER);
}

static void
on_drawing_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer user_data)
{
    (void)ctrl;
    nd_window *w = user_data;
    if (!w->layout_tree) return;
    const char *href = nd_box_hit_link(w->layout_tree, x, y);
    GdkCursor *cur = gdk_cursor_new_from_name(href ? "pointer" : "default", NULL);
    gtk_widget_set_cursor(w->drawing_area, cur);
    if (cur) g_object_unref(cur);
    if (href)
        nd_window_set_status(w, "%s", href);
}

static void
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
    if (w->current_fetch) {
        g_cancellable_cancel(w->current_fetch);
        g_clear_object(&w->current_fetch);
    }
    nd_window_clear_cache(w);
    if (w->history) {
        for (guint i = 0; i < w->history->len; i++)
            g_free(g_ptr_array_index(w->history, i));
        g_ptr_array_free(w->history, TRUE);
    }
    if (w->images) nd_image_cache_free(w->images);
    g_free(w);
}

static void
on_app_new_window(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    GtkApplication *app = user_data;
    nd_window_open(app, NULL);
}

static void
nd_window_open(GtkApplication *app, const char *startup_url)
{
    nd_window *w = g_new0(nd_window, 1);

    w->history = g_ptr_array_new();
    w->cursor  = -1;
    w->images  = nd_image_cache_new();

    w->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(w->window), ND_DEFAULT_W, ND_DEFAULT_H);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_window_destroy), w);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(w->window), header);

    w->back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_set_tooltip_text(w->back_button, "Back");
    gtk_widget_set_sensitive(w->back_button, FALSE);
    g_signal_connect(w->back_button, "clicked", G_CALLBACK(on_back_clicked), w);

    w->forward_button = gtk_button_new_from_icon_name("go-next-symbolic");
    gtk_widget_set_tooltip_text(w->forward_button, "Forward");
    gtk_widget_set_sensitive(w->forward_button, FALSE);
    g_signal_connect(w->forward_button, "clicked", G_CALLBACK(on_forward_clicked), w);

    w->home_button = gtk_button_new_from_icon_name("go-home-symbolic");
    gtk_widget_set_tooltip_text(w->home_button, "Home (" ND_HOME_URL ")");
    g_signal_connect(w->home_button, "clicked", G_CALLBACK(on_home_clicked), w);

    w->reload_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(w->reload_button, "Reload");
    g_signal_connect(w->reload_button, "clicked", G_CALLBACK(on_reload_clicked), w);

    w->about_button = gtk_button_new_from_icon_name("help-about-symbolic");
    gtk_widget_set_tooltip_text(w->about_button, "About Nordstjernen (about:mozilla)");
    g_signal_connect(w->about_button, "clicked", G_CALLBACK(on_about_clicked), w);

    w->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->url_entry),
                                   "Enter URL (e.g. https://lite.cnn.com)");
    gtk_widget_set_hexpand(w->url_entry, TRUE);
    gtk_widget_set_size_request(w->url_entry, 400, -1);
    g_signal_connect(w->url_entry, "activate", G_CALLBACK(on_entry_activate), w);

    w->go_button = gtk_button_new_with_label("Go");
    gtk_widget_set_tooltip_text(w->go_button, "Load the URL in the address bar");
    g_signal_connect(w->go_button, "clicked", G_CALLBACK(on_go_clicked), w);

    w->stop_button = gtk_button_new_from_icon_name("process-stop-symbolic");
    gtk_widget_set_tooltip_text(w->stop_button, "Stop loading");
    gtk_widget_set_sensitive(w->stop_button, FALSE);
    g_signal_connect(w->stop_button, "clicked", G_CALLBACK(on_stop_clicked), w);

    const char *view_labels[] = { "Render", "Raw", "DOM", "Layout", NULL };
    w->view_dropdown = gtk_drop_down_new_from_strings(view_labels);
    gtk_widget_set_tooltip_text(w->view_dropdown,
        "Select view: raw response bytes, DOM tree dump, or layout tree dump.");
    g_signal_connect(w->view_dropdown, "notify::selected",
                     G_CALLBACK(on_view_changed), w);

    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->back_button);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->forward_button);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->reload_button);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->home_button);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->url_entry);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->about_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->view_dropdown);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->stop_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->go_button);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(w->window), vbox);

    w->content_stack = gtk_stack_new();
    gtk_widget_set_hexpand(w->content_stack, TRUE);
    gtk_widget_set_vexpand(w->content_stack, TRUE);

    GtkWidget *scrolled_text = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_text),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    w->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(w->text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(w->text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(w->text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_text), w->text_view);
    gtk_stack_add_named(GTK_STACK(w->content_stack), scrolled_text, "text");

    GtkWidget *scrolled_render = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_render),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    w->render_vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(scrolled_render));
    w->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(w->drawing_area, TRUE);
    gtk_widget_set_vexpand(w->drawing_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->drawing_area),
                                   nd_draw_render, w, NULL);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(nd_on_drawing_pressed), w);
    gtk_widget_add_controller(w->drawing_area, GTK_EVENT_CONTROLLER(click));

    GtkGesture *middle = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
    g_signal_connect(middle, "pressed", G_CALLBACK(nd_on_drawing_pressed_middle), w);
    gtk_widget_add_controller(w->drawing_area, GTK_EVENT_CONTROLLER(middle));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_drawing_motion), w);
    gtk_widget_add_controller(w->drawing_area, motion);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_render),
                                  w->drawing_area);
    gtk_stack_add_named(GTK_STACK(w->content_stack), scrolled_render, "render");

    gtk_stack_set_visible_child_name(GTK_STACK(w->content_stack), "text");
    gtk_box_append(GTK_BOX(vbox), w->content_stack);

    w->status_label = gtk_label_new("Ready");
    gtk_widget_set_halign(w->status_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(w->status_label, 8);
    gtk_widget_set_margin_end(w->status_label, 8);
    gtk_widget_set_margin_top(w->status_label, 4);
    gtk_widget_set_margin_bottom(w->status_label, 4);
    gtk_label_set_ellipsize(GTK_LABEL(w->status_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(w->status_label), 0.0f);
    gtk_box_append(GTK_BOX(vbox), w->status_label);

    gtk_widget_grab_focus(w->url_entry);
    gtk_window_maximize(GTK_WINDOW(w->window));
    gtk_window_present(GTK_WINDOW(w->window));

    const char *url = startup_url;
    if (!url || !*url) url = ND_HOME_URL;
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
nd_install_actions(GtkApplication *app)
{
    GSimpleAction *new_window = g_simple_action_new("new-window", NULL);
    g_signal_connect(new_window, "activate", G_CALLBACK(on_app_new_window), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(new_window));
    g_object_unref(new_window);

    const char *new_window_accels[] = { "<Primary>n", NULL };
    gtk_application_set_accels_for_action(app, "app.new-window", new_window_accels);
}

int
main(int argc, char **argv)
{
    nd_net_init();

    GtkApplication *app = gtk_application_new(ND_APP_ID,
        G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup",      G_CALLBACK(nd_install_actions), NULL);
    g_signal_connect(app, "activate",     G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(nd_on_command_line), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    g_free(g_startup_url_override);
    nd_net_shutdown();
    return status;
}
