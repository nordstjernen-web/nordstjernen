/* Nordstjernen — GTK 4 application shell. */

#include <gtk/gtk.h>
#include <string.h>

#include <cairo-pdf.h>
#include <math.h>

#include "bookmarks.h"
#include "css.h"
#include "history.h"
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

static char         *g_startup_url_override;
static nd_bookmarks *g_bookmarks;
static nd_history   *g_history;

#define ND_LAYOUT_VIEWPORT 1000.0

typedef struct nd_window {
    GtkWidget    *window;
    GtkWidget    *url_entry;
    GtkWidget    *back_button;
    GtkWidget    *forward_button;
    GtkWidget    *home_button;
    GtkWidget    *reload_button;
    GtkWidget    *about_button;
    GtkWidget    *bookmark_button;
    GtkWidget    *bookmarks_button;
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
    char         *pending_fragment;

    double        zoom;

    GtkWidget    *search_revealer;
    GtkWidget    *search_entry;
    GtkWidget    *search_count_label;
    char         *search_query;

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
static void nd_window_refresh_bookmark_button(nd_window *w);
static const char *nd_window_current_url(nd_window *w);
static char       *nd_window_current_title(nd_window *w);
static void nd_window_install_actions(nd_window *w);
static void on_search_changed(GtkEditable *entry, gpointer user_data);
static void on_search_activate(GtkEntry *entry, gpointer user_data);

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
extract_charset(const char *content_type)
{
    if (!content_type) return NULL;
    const char *s = strstr(content_type, "charset=");
    if (!s) {
        s = strstr(content_type, "charset =");
        if (!s) return NULL;
    }
    s = strchr(s, '=') + 1;
    while (*s == ' ' || *s == '"' || *s == '\'') s++;
    const char *e = s;
    while (*e && *e != ';' && *e != ' ' && *e != '"' && *e != '\'' &&
           *e != '\r' && *e != '\n')
        e++;
    if (e == s) return NULL;
    return g_strndup(s, (gsize)(e - s));
}

static char *
sniff_meta_charset(const char *body, gsize len)
{
    if (!body) return NULL;
    gsize scan = len < 2048 ? len : 2048;
    GString *lower = g_string_new(NULL);
    for (gsize i = 0; i < scan; i++) {
        char c = body[i];
        g_string_append_c(lower, (c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    char *result = NULL;
    const char *p = strstr(lower->str, "charset=");
    if (p) {
        p += 8;
        while (*p == ' ' || *p == '"' || *p == '\'') p++;
        const char *q = p;
        while (*q && *q != '"' && *q != '\'' && *q != ' ' && *q != '/' &&
               *q != '>' && *q != ';' && *q != '\r' && *q != '\n')
            q++;
        if (q > p) result = g_strndup(p, (gsize)(q - p));
    }
    g_string_free(lower, TRUE);
    return result;
}

static char *
decode_body(const char *body, gsize len, const char *content_type)
{
    if (!body || len == 0) return g_strdup("");
    char *charset = extract_charset(content_type);
    if (!charset) charset = sniff_meta_charset(body, len);

    if (charset) {
        char *upper = g_ascii_strup(charset, -1);
        if (strcmp(upper, "UTF-8") == 0 || strcmp(upper, "UTF8") == 0 ||
            strcmp(upper, "US-ASCII") == 0) {
            g_free(charset); g_free(upper);
            if (g_utf8_validate(body, (gssize)len, NULL))
                return g_strndup(body, len);
        } else {
            GError *err = NULL;
            gsize written = 0;
            char *out = g_convert(body, (gssize)len, "UTF-8", charset,
                                  NULL, &written, &err);
            g_free(charset); g_free(upper);
            if (out) return out;
            if (err) g_error_free(err);
        }
    } else if (g_utf8_validate(body, (gssize)len, NULL)) {
        return g_strndup(body, len);
    }
    GError *err = NULL;
    gsize written = 0;
    char *out = g_convert(body, (gssize)len, "UTF-8", "ISO-8859-1",
                          NULL, &written, &err);
    if (out) return out;
    if (err) g_error_free(err);
    return g_strdup("(unable to decode response body)\n");
}

static char *
to_utf8_or_pass(const char *body, gsize len)
{
    return decode_body(body, len, NULL);
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
    nd_paint(cr, w->layout_tree, w->search_query);
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
    char *full = g_strconcat("https://duckduckgo.com/?q=", escaped, NULL);
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
        nd_window_ensure_layout(w, ND_LAYOUT_VIEWPORT);
        gtk_window_set_title(GTK_WINDOW(w->window), "Error — " ND_TITLE);
        nd_response_free(resp);
        return;
    }

    if (resp->status >= 400) {
        nd_window_set_status(w, "%ld %s", resp->status,
                             resp->final_url ? resp->final_url : "");
    }

    nd_window_clear_cache(w);
    if (resp->body && resp->body->len > 0) {
        char *decoded = decode_body((const char *)resp->body->data,
                                    resp->body->len, resp->content_type);
        w->last_body = decoded;
        w->last_body_len = strlen(decoded);
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
    nd_window_refresh_bookmark_button(w);
    if (w->pending_fragment && w->render_vadj) {
        nd_window_scroll_to_fragment(w);
    } else if (w->render_vadj) {
        gtk_adjustment_set_value(w->render_vadj, 0);
    }

    if (g_history && resp->status >= 200 && resp->status < 400) {
        const char *url = nd_window_current_url(w);
        char *title = nd_window_current_title(w);
        if (url) nd_history_visit(g_history, url, title);
        g_free(title);
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

static void
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

static void
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
    g_free(w->pending_fragment);
    g_free(w->search_query);
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
    w->zoom    = 1.0;

    w->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(w->window), ND_DEFAULT_W, ND_DEFAULT_H);
    g_object_set_data(G_OBJECT(w->window), "nd-window", w);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_window_destroy), w);
    nd_window_install_actions(w);

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

    w->bookmark_button = gtk_button_new_from_icon_name("non-starred-symbolic");
    gtk_widget_set_tooltip_text(w->bookmark_button, "Bookmark this page");
    g_signal_connect(w->bookmark_button, "clicked", G_CALLBACK(on_bookmark_clicked), w);

    w->bookmarks_button = gtk_button_new_from_icon_name("user-bookmarks-symbolic");
    gtk_widget_set_tooltip_text(w->bookmarks_button, "Show bookmarks");
    g_signal_connect(w->bookmarks_button, "clicked", G_CALLBACK(on_bookmarks_clicked), w);

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
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->bookmarks_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->view_dropdown);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->stop_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->go_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->bookmark_button);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(w->window), vbox);

    w->search_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(w->search_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(search_box, 4);
    gtk_widget_set_margin_bottom(search_box, 4);
    gtk_widget_set_margin_start(search_box, 4);
    gtk_widget_set_margin_end(search_box, 4);
    w->search_entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(w->search_entry, TRUE);
    g_signal_connect(w->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), w);
    g_signal_connect(w->search_entry, "activate",
                     G_CALLBACK(on_search_activate), w);
    GtkWidget *search_label = gtk_label_new("Find:");
    w->search_count_label = gtk_label_new("");
    gtk_widget_add_css_class(w->search_count_label, "dim-label");
    gtk_widget_set_margin_start(w->search_count_label, 8);
    gtk_box_append(GTK_BOX(search_box), search_label);
    gtk_box_append(GTK_BOX(search_box), w->search_entry);
    gtk_box_append(GTK_BOX(search_box), w->search_count_label);
    gtk_revealer_set_child(GTK_REVEALER(w->search_revealer), search_box);
    gtk_box_append(GTK_BOX(vbox), w->search_revealer);

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

    nd_window_ensure_layout(w, ND_LAYOUT_VIEWPORT);
    if (!w->layout_tree) { g_free(path); return; }
    double pw = ND_LAYOUT_VIEWPORT;
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

static void
on_search_changed(GtkEditable *entry, gpointer user_data)
{
    nd_window *w = user_data;
    g_free(w->search_query);
    const char *text = gtk_editable_get_text(entry);
    w->search_query = text && *text ? g_strdup(text) : NULL;
    gtk_widget_queue_draw(w->drawing_area);
    nd_window_update_match_count(w);
}

static void
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
    };
    for (gsize i = 0; i < G_N_ELEMENTS(binds); i++)
        gtk_application_set_accels_for_action(app, binds[i].action, binds[i].accels);
}

int
main(int argc, char **argv)
{
    nd_net_init();
    g_bookmarks = nd_bookmarks_load();
    g_history   = nd_history_load();

    GtkApplication *app = gtk_application_new(ND_APP_ID,
        G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup",      G_CALLBACK(nd_install_actions), NULL);
    g_signal_connect(app, "activate",     G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(nd_on_command_line), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    nd_bookmarks_free(g_bookmarks);
    g_bookmarks = NULL;
    nd_history_free(g_history);
    g_history = NULL;
    g_free(g_startup_url_override);
    nd_net_shutdown();
    return status;
}
