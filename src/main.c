/*
 * Nordstjernen Web Navigator
 * Copyright 2026 Andreas Røsdal
 *
 * Phase 1 shell: address bar + load button. Fetched body bytes are
 * printed verbatim into a GtkTextView (UTF-8 only; non-UTF-8 input is
 * shown as a notice). Layout, rendering, etc. come in later phases.
 */

#include <gtk/gtk.h>
#include <string.h>

#include "html.h"
#include "net.h"

#define ND_APP_ID    "com.nordstjernen.Browser"
#define ND_TITLE     "Nordstjernen"
#define ND_DEFAULT_W 1024
#define ND_DEFAULT_H 720

typedef enum nd_view_mode {
    ND_VIEW_RAW,
    ND_VIEW_DOM,
} nd_view_mode;

typedef struct nd_window {
    GtkWidget    *window;
    GtkWidget    *url_entry;
    GtkWidget    *load_button;
    GtkWidget    *stop_button;
    GtkWidget    *view_toggle;
    GtkWidget    *text_view;
    GtkWidget    *status_label;
    GCancellable *current_fetch;
    nd_view_mode  mode;
    /* Cached body so we can switch views without refetching. */
    char         *last_body;
    gsize         last_body_len;
    char         *last_content_type;
} nd_window;

static void nd_window_load_url(nd_window *w, const char *raw_url);
static void nd_window_set_busy(nd_window *w, gboolean busy);
static void nd_window_render(nd_window *w);
static void nd_window_clear_cache(nd_window *w);

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

    if (w->mode == ND_VIEW_DOM && is_html_content_type(w->last_content_type)) {
        nd_node *doc = nd_html_parse(w->last_body, (gssize)w->last_body_len);
        GString *dump = nd_node_dump(doc);
        nd_window_set_body_text(w, dump->str, (gssize)dump->len);
        g_string_free(dump, TRUE);
        nd_node_free(doc);
        return;
    }

    char *utf8 = to_utf8_or_pass(w->last_body, w->last_body_len);
    nd_window_set_body_text(w, utf8, -1);
    g_free(utf8);
}

static char *
nd_normalize_url(const char *raw)
{
    if (!raw)
        return NULL;
    /* trim whitespace */
    while (*raw == ' ' || *raw == '\t')
        raw++;
    size_t len = strlen(raw);
    while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t' ||
                       raw[len - 1] == '\r' || raw[len - 1] == '\n'))
        len--;
    if (len == 0)
        return NULL;

    /* If there's no scheme://, prepend https://. We deliberately keep
     * this dumb — a real URL bar gets smarter heuristics later. */
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

    /* Clear the in-flight cancellable. */
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

    /* Cache the body so the user can flip between RAW and DOM views
     * without refetching. */
    nd_window_clear_cache(w);
    if (resp->body && resp->body->len > 0) {
        w->last_body = g_memdup2(resp->body->data, resp->body->len);
        w->last_body_len = resp->body->len;
    }
    w->last_content_type = g_strdup(resp->content_type ? resp->content_type : "");

    /* If the content is HTML, default to DOM view to make the parser
     * visible; otherwise show raw bytes. */
    if (is_html_content_type(w->last_content_type))
        w->mode = ND_VIEW_DOM;
    else
        w->mode = ND_VIEW_RAW;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->view_toggle),
                                 w->mode == ND_VIEW_DOM);

    nd_window_render(w);
    nd_window_set_status(w, "%ld  %s  (%s, %" G_GSIZE_FORMAT " bytes)",
                         resp->status,
                         resp->final_url ? resp->final_url : "",
                         resp->content_type ? resp->content_type : "?",
                         (gsize)w->last_body_len);
    nd_response_free(resp);
}

static void
nd_window_load_url(nd_window *w, const char *raw_url)
{
    char *url = nd_normalize_url(raw_url);
    if (!url) {
        nd_window_set_status(w, "Empty URL");
        return;
    }

    /* Cancel any in-flight fetch. */
    if (w->current_fetch) {
        g_cancellable_cancel(w->current_fetch);
        g_clear_object(&w->current_fetch);
    }

    w->current_fetch = g_cancellable_new();
    nd_window_set_busy(w, TRUE);
    nd_window_set_status(w, "Loading %s …", url);
    nd_net_fetch_async(url, w->current_fetch, nd_on_fetch_done, w);
    g_free(url);
}

static void
nd_window_set_busy(nd_window *w, gboolean busy)
{
    gtk_widget_set_sensitive(w->load_button, !busy);
    gtk_widget_set_sensitive(w->stop_button, busy);
}

static void
on_load_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(w->url_entry));
    nd_window_load_url(w, text);
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
    nd_window_load_url(w, text);
}

static void
on_view_toggled(GtkToggleButton *button, gpointer user_data)
{
    nd_window *w = user_data;
    w->mode = gtk_toggle_button_get_active(button) ? ND_VIEW_DOM : ND_VIEW_RAW;
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
    g_free(w);
}

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    (void)user_data;
    nd_window *w = g_new0(nd_window, 1);

    w->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(w->window), ND_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(w->window), ND_DEFAULT_W, ND_DEFAULT_H);
    g_signal_connect(w->window, "destroy", G_CALLBACK(on_window_destroy), w);

    /* Header bar */
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(w->window), header);

    w->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->url_entry),
                                   "Enter URL (e.g. https://lite.cnn.com)");
    gtk_widget_set_hexpand(w->url_entry, TRUE);
    gtk_widget_set_size_request(w->url_entry, 400, -1);
    g_signal_connect(w->url_entry, "activate", G_CALLBACK(on_entry_activate), w);

    w->load_button = gtk_button_new_with_label("Load");
    g_signal_connect(w->load_button, "clicked", G_CALLBACK(on_load_clicked), w);

    w->stop_button = gtk_button_new_with_label("Stop");
    gtk_widget_set_sensitive(w->stop_button, FALSE);
    g_signal_connect(w->stop_button, "clicked", G_CALLBACK(on_stop_clicked), w);

    w->view_toggle = gtk_toggle_button_new_with_label("DOM");
    gtk_widget_set_tooltip_text(w->view_toggle,
        "Toggle between raw response bytes and a debug DOM dump.");
    g_signal_connect(w->view_toggle, "toggled", G_CALLBACK(on_view_toggled), w);

    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->url_entry);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->stop_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->load_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->view_toggle);

    /* Main body: vertical box [ scrolled text view (expanding) | status ] */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(w->window), vbox);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    w->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(w->text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(w->text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(w->text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), w->text_view);

    gtk_box_append(GTK_BOX(vbox), scrolled);

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
    gtk_window_present(GTK_WINDOW(w->window));
}

int
main(int argc, char **argv)
{
    nd_net_init();

    GtkApplication *app = gtk_application_new(ND_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    nd_net_shutdown();
    return status;
}
