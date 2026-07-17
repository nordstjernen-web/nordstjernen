/* Nordstjernen — GTK tabbed process-per-tab browser shell (IPC renderer). */

#include "procwindow.h"
#include <glib/gstdio.h>
#include "procview.h"
#include "i18n.h"
#include "rproc_http.h"
#include "rproc_inproc.h"
#include "threaddump.h"
#include "watchdog.h"
#include "bookmarks.h"
#include "cache.h"
#include "config.h"
#include "history.h"
#include "net.h"
#include "security.h"
#include "version.h"
#include "image.h"
#ifdef __APPLE__
#include "macos_dock.h"
#endif

#include <stdlib.h>
#include <string.h>

#define NS_PROC_APP_ID "org.nordstjernen.WebBrowser"

static int g_initial_win_w;
static int g_initial_win_h;

void
ns_procapp_set_window_size(int width, int height)
{
    g_initial_win_w = width;
    g_initial_win_h = height;
}

typedef struct {
    GtkApplication *app;
    GtkWidget      *window;
    GtkWidget      *notebook;
    GtkWidget      *tabstrip;
    GtkWidget      *newtab_btn;
    GtkWidget      *security_icon;
    GtkWidget      *address;
    GtkWidget      *back;
    GtkWidget      *forward;
    GtkWidget      *reload;
    GtkWidget      *spinner;
    GtkWidget      *status;
    char           *status_base;
    gboolean        webgl_active;
    GtkWidget      *bookmarks_button;
    char           *home_url;
    ns_bookmarks   *bookmarks;
    char           *session_path;
    guint           session_timer;
    GtkWidget      *task_mgr_win;
    GtkWidget      *downloads_win;
    GtkWidget      *downloads_list;
} ProcWindow;

static const char *
ns_brand_versioned(void)
{
    static char brand[128];
    if (!brand[0])
        g_snprintf(brand, sizeof brand, "%s %s",
                   ns_i18n("Nordstjernen"), NS_VERSION);
    return brand;
}

static void
procwindow_free(gpointer data)
{
    ProcWindow *pw = data;
    if (pw->session_timer)
        g_source_remove(pw->session_timer);
    g_free(pw->session_path);
    g_free(pw->home_url);
    g_free(pw->status_base);
    if (pw->bookmarks)
        ns_bookmarks_free(pw->bookmarks);
    g_free(pw);
}

static void
install_icon_search_paths(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return;
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
    if (!theme)
        return;
    gtk_icon_theme_add_resource_path(theme, "/org/nordstjernen/WebBrowser/icons");
    const char *exe = ns_app_self_exe();
    if (!exe)
        return;
    char *dir = g_path_get_dirname(exe);
    const char *rel[] = { "share/icons",      "../share/icons",
                          "data/icons",       "../data/icons",
                          "../../data/icons", "../../../data/icons",
                          "../../../../data/icons", NULL };
    for (int i = 0; rel[i]; i++) {
        char *p = g_build_filename(dir, rel[i], NULL);
        gtk_icon_theme_add_search_path(theme, p);
        g_free(p);
    }
    g_free(dir);
}

static void
install_status_css(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return;
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        p,
        ".ns-procstatus {"
        "  padding: 2px 8px;"
        "  border-top: 1px solid alpha(currentColor, 0.15);"
        "  font-size: smaller;"
        "}"
        ".ns-toolbar button, .ns-toolbar entry {"
        "  min-height: 26px;"
        "}"
        ".ns-toolbar entry { padding-top: 2px; padding-bottom: 2px; }"
        ".ns-sec-icon { margin-left: 2px; margin-right: 2px; }"
        ".ns-sec-secure { color: #2e9e44; }"
        ".ns-sec-invalid { color: #e01b24; }"
        ".ns-sec-warn { color: #e5a50a; }"
        ".ns-tabstrip { padding: 0; }"
        ".ns-tab { padding: 0; }"
        ".ns-tab button { min-height: 0; padding: 2px 4px; }"
        ".ns-tab > button.ns-tab-label {"
        "  padding: 2px 8px;"
        "  border-bottom: 2px solid transparent;"
        "}"
        ".ns-tab > button.ns-tab-label.ns-tab-active {"
        "  background-color: alpha(white, 0.65);"
        "  border-bottom-color: @accent_color;"
        "  border-top-left-radius: 5px;"
        "  border-top-right-radius: 5px;"
        "  font-weight: bold;"
        "}"
        ".ns-newtab { min-height: 0; padding: 2px 6px; }");
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static void
set_accessible_label(GtkWidget *w, const char *label)
{
    gtk_accessible_update_property(GTK_ACCESSIBLE(w),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static GtkWidget *
toolbar_button(const char *icon, const char *tooltip, GCallback cb,
               gpointer data)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    gtk_widget_set_tooltip_text(b, tooltip);
    set_accessible_label(b, tooltip);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static NsProcView *
view_for_page(GtkWidget *page)
{
    return page ? g_object_get_data(G_OBJECT(page), "ns-proc-view") : NULL;
}

static NsProcView *
current_view(ProcWindow *pw)
{
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(pw->notebook));
    if (idx < 0)
        return NULL;
    return view_for_page(
        gtk_notebook_get_nth_page(GTK_NOTEBOOK(pw->notebook), idx));
}

static char *
normalize_url(const char *input)
{
    char *trimmed = g_strstrip(g_strdup(input ? input : ""));
    if (!*trimmed)
        return trimmed;
    if (g_str_has_prefix(trimmed, "about:") ||
        g_str_has_prefix(trimmed, "file:") ||
        g_str_has_prefix(trimmed, "data:") || strstr(trimmed, "://"))
        return trimmed;
    char *local = ns_url_from_local_path(trimmed);
    if (local) {
        g_free(trimmed);
        return local;
    }
    if (ns_address_is_search(trimmed)) {
        char *out = ns_search_url_for(trimmed);
        g_free(trimmed);
        return out;
    }
    char *out = g_strconcat("https://", trimmed, NULL);
    g_free(trimmed);
    return out;
}

static void
set_loading_ui(ProcWindow *pw, gboolean loading)
{
    gtk_widget_set_visible(pw->spinner, loading);
    gtk_spinner_set_spinning(GTK_SPINNER(pw->spinner), loading);
}

static char *
address_display_url(const char *url)
{
    if (!url || !*url) return g_strdup("");
    if (!strchr(url, '%')) return g_strdup(url);
    char *dec = g_uri_unescape_string(url, NULL);
    if (!dec) return g_strdup(url);
    if (!g_utf8_validate(dec, -1, NULL)) {
        g_free(dec);
        return g_strdup(url);
    }
    for (const char *p = dec; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c < 0x20 || c == 0x7f ||
            (c >= 0x200e && c <= 0x200f) ||
            (c >= 0x202a && c <= 0x202e) ||
            (c >= 0x2066 && c <= 0x2069)) {
            g_free(dec);
            return g_strdup(url);
        }
    }
    return dec;
}

static void
set_address_text(ProcWindow *pw, const char *url)
{
    char *shown = address_display_url(url);
    gtk_editable_set_text(GTK_EDITABLE(pw->address), shown);
    g_free(shown);
}

static void
update_security_indicator(ProcWindow *pw, NsProcView *v)
{
    GtkWidget *icon = pw->security_icon;
    if (!icon)
        return;
    gtk_widget_remove_css_class(icon, "ns-sec-secure");
    gtk_widget_remove_css_class(icon, "ns-sec-invalid");
    gtk_widget_remove_css_class(icon, "ns-sec-warn");

    const char *url = v ? ns_proc_view_url(v) : NULL;
    int sec = v ? ns_proc_view_security(v) : NS_SEC_NONE;
    const char *icon_name = NULL, *label = NULL, *css = NULL;
    switch (sec) {
    case NS_SEC_SECURE:
        icon_name = "security-high-symbolic";
        label = ns_i18n("Secure — the certificate is valid");
        css = "ns-sec-secure";
        break;
    case NS_SEC_INVALID:
        icon_name = "security-low-symbolic";
        label = ns_i18n("Not secure — the certificate is not trusted");
        css = "ns-sec-invalid";
        break;
    case NS_SEC_PLAIN:
        icon_name = "channel-insecure-symbolic";
        label = ns_i18n("Not secure — the connection is not encrypted");
        css = "ns-sec-warn";
        break;
    default:
        break;
    }
    if (!icon_name || !url || !*url) {
        gtk_widget_set_visible(icon, FALSE);
        return;
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), icon_name);
    gtk_widget_add_css_class(icon, css);

    GString *tip = g_string_new(label);
    char *host = ns_url_host_from(url);
    if (host && *host)
        g_string_append_printf(tip, "\n%s", host);
    const char *ip = ns_proc_view_remote_ip(v);
    if (ip && *ip)
        g_string_append_printf(tip, "\n%s %s", ns_i18n("Server:"), ip);
    gtk_widget_set_tooltip_text(icon, tip->str);
    g_string_free(tip, TRUE);
    g_free(host);
    gtk_widget_set_visible(icon, TRUE);
}

static void
update_chrome(ProcWindow *pw)
{
    NsProcView *v = current_view(pw);
    if (!v) {
        gtk_editable_set_text(GTK_EDITABLE(pw->address), "");
        gtk_window_set_title(GTK_WINDOW(pw->window), ns_brand_versioned());
        gtk_widget_set_sensitive(pw->back, FALSE);
        gtk_widget_set_sensitive(pw->forward, FALSE);
        update_security_indicator(pw, NULL);
        set_loading_ui(pw, FALSE);
        return;
    }
    set_loading_ui(pw, ns_proc_view_is_loading(v));
    const char *url = ns_proc_view_url(v);
    const char *title = ns_proc_view_title(v);
    set_address_text(pw, url);
    const char *brand = ns_brand_versioned();
    char *wt = g_strdup_printf("%s — %s",
                               title && *title ? title : brand, brand);
    gtk_window_set_title(GTK_WINDOW(pw->window), wt);
    g_free(wt);
    gtk_widget_set_sensitive(pw->back, ns_proc_view_can_back(v));
    gtk_widget_set_sensitive(pw->forward, ns_proc_view_can_forward(v));
    update_security_indicator(pw, v);
}

static void proc_window_add_tab(ProcWindow *pw, const char *url,
                                gboolean foreground);

typedef struct {
    char      *url;
    char      *path;
    char      *name;
    GtkWidget *progress;
    GtkWidget *status;
    GtkWidget *open;
    guint      pulse;
    gboolean   ok;
    gint64     size;
} NsDownload;

static void show_downloads_window(ProcWindow *pw);
static void pw_start_download(ProcWindow *pw, const char *url,
                              const char *suggested);

static const char *
downloads_dir(void)
{
    const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (d && *d)
        return d;
    static char *fallback;
    if (!fallback)
        fallback = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    return fallback;
}

static void
download_open_path(const char *path)
{
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri) {
        g_app_info_launch_default_for_uri(uri, NULL, NULL);
        g_free(uri);
    }
}

static void
on_download_open(GtkButton *b, gpointer ud)
{
    (void)b;
    download_open_path((const char *)ud);
}

static void
download_free_str(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

static void
on_open_downloads_folder(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    download_open_path(downloads_dir());
}

static gboolean
download_pulse(gpointer ud)
{
    NsDownload *d = ud;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(d->progress));
    return G_SOURCE_CONTINUE;
}

static gboolean
download_finish_idle(gpointer ud)
{
    NsDownload *d = ud;
    if (d->pulse) { g_source_remove(d->pulse); d->pulse = 0; }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->progress),
                                  d->ok ? 1.0 : 0.0);
    if (d->ok) {
        char *sz = g_format_size((guint64)d->size);
        char *msg = g_strdup_printf("%s — %s", d->name, sz);
        gtk_label_set_text(GTK_LABEL(d->status), msg);
        gtk_widget_set_sensitive(d->open, TRUE);
        g_free(sz);
        g_free(msg);
    } else {
        char *msg = g_strdup_printf("%s — %s", d->name, ns_i18n("Failed"));
        gtk_label_set_text(GTK_LABEL(d->status), msg);
        g_free(msg);
    }
    g_free(d->url);
    g_free(d->path);
    g_free(d->name);
    g_free(d);
    return G_SOURCE_REMOVE;
}

static gpointer
download_worker(gpointer ud)
{
    NsDownload *d = ud;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_blocking(d->url, NULL, &err);
    if (resp && !resp->error && resp->body &&
        g_file_set_contents(d->path, (const char *)resp->body->data,
                            resp->body->len, NULL)) {
        d->ok = TRUE;
        d->size = resp->body->len;
        ns_security_mark_download_origin(
            d->path, resp->final_url ? resp->final_url : d->url);
    }
    if (resp) ns_response_free(resp);
    g_clear_error(&err);
    g_idle_add(download_finish_idle, d);
    return NULL;
}

static GtkWidget *
download_row_new(const char *name, gboolean done, const char *open_path,
                 NsDownload *d)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);
    GtkWidget *status = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(status), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_MIDDLE);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *open = gtk_button_new_with_label(ns_i18n("Open"));
    gtk_widget_set_sensitive(open, done);
    g_signal_connect_data(open, "clicked", G_CALLBACK(on_download_open),
                          g_strdup(open_path), download_free_str, 0);
    if (d) {
        GtkWidget *progress = gtk_progress_bar_new();
        gtk_widget_set_hexpand(progress, TRUE);
        gtk_widget_set_valign(progress, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(hb), progress);
        d->progress = progress;
        d->status = status;
        d->open = open;
    } else {
        GtkWidget *spacer = gtk_label_new("");
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(hb), spacer);
    }
    gtk_box_append(GTK_BOX(hb), open);
    gtk_box_append(GTK_BOX(row), status);
    gtk_box_append(GTK_BOX(row), hb);
    return row;
}

static void
downloads_populate_recent(ProcWindow *pw)
{
    const char *dir = downloads_dir();
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd) return;
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    const char *nm;
    while ((nm = g_dir_read_name(gd)) && files->len < 200) {
        if (nm[0] == '.') continue;
        g_ptr_array_add(files, g_build_filename(dir, nm, NULL));
    }
    g_dir_close(gd);
    g_ptr_array_sort(files, (GCompareFunc)g_strcmp0);
    for (guint i = 0; i < files->len && i < 25; i++) {
        const char *path = g_ptr_array_index(files, i);
        if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) continue;
        char *base = g_path_get_basename(path);
        GtkWidget *row = download_row_new(base, TRUE, path, NULL);
        gtk_list_box_append(GTK_LIST_BOX(pw->downloads_list), row);
        g_free(base);
    }
    g_ptr_array_free(files, TRUE);
}

static gboolean
downloads_win_close(GtkWindow *win, gpointer ud)
{
    (void)ud;
    gtk_widget_set_visible(GTK_WIDGET(win), FALSE);
    return TRUE;
}

static void
show_downloads_window(ProcWindow *pw)
{
    if (pw->downloads_win) {
        gtk_window_present(GTK_WINDOW(pw->downloads_win));
        return;
    }
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), ns_i18n("Downloads"));
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 420);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(pw->window));
    g_signal_connect(win, "close-request",
                     G_CALLBACK(downloads_win_close), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(header, 8);
    gtk_widget_set_margin_end(header, 8);
    gtk_widget_set_margin_top(header, 8);
    gtk_widget_set_margin_bottom(header, 4);
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *folder = gtk_button_new_with_label(ns_i18n("Open folder"));
    g_signal_connect(folder, "clicked",
                     G_CALLBACK(on_open_downloads_folder), NULL);
    gtk_box_append(GTK_BOX(header), spacer);
    gtk_box_append(GTK_BOX(header), folder);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_window_set_child(GTK_WINDOW(win), box);

    pw->downloads_win = win;
    pw->downloads_list = list;
    downloads_populate_recent(pw);
    gtk_window_present(GTK_WINDOW(win));
}

static void
pw_start_download(ProcWindow *pw, const char *url, const char *suggested)
{
    if (!url || !*url) return;
    char *name = NULL;
    if (suggested && *suggested)
        name = g_path_get_basename(suggested);
    if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "/") == 0) {
        g_free(name);
        char *base = g_path_get_basename(url);
        char *q = base ? strchr(base, '?') : NULL;
        if (q) *q = '\0';
        if (base && *base && strcmp(base, ".") != 0 && strcmp(base, "/") != 0)
            name = base;
        else { g_free(base); name = g_strdup("download"); }
    }
    const char *dir = downloads_dir();
    char *path = g_build_filename(dir, name, NULL);
    for (int n = 1; g_file_test(path, G_FILE_TEST_EXISTS) && n < 1000; n++) {
        g_free(path);
        char *alt = g_strdup_printf("%s.%d", name, n);
        path = g_build_filename(dir, alt, NULL);
        g_free(alt);
    }

    show_downloads_window(pw);

    NsDownload *d = g_new0(NsDownload, 1);
    d->url = g_strdup(url);
    d->path = path;
    d->name = name;
    GtkWidget *row = download_row_new(name, FALSE, path, d);
    gtk_list_box_prepend(GTK_LIST_BOX(pw->downloads_list), row);
    d->pulse = g_timeout_add(120, download_pulse, d);
    GThread *t = g_thread_new("ns-download", download_worker, d);
    if (t) g_thread_unref(t);
}


static void
pw_render_status(ProcWindow *pw)
{
    const char *base = pw->status_base ? pw->status_base : "";
    if (pw->webgl_active) {
        char *composed = *base
            ? g_strconcat(base, " · ", ns_i18n("WebGL enabled"), NULL)
            : g_strdup(ns_i18n("WebGL enabled"));
        gtk_label_set_text(GTK_LABEL(pw->status), composed);
        g_free(composed);
    } else {
        gtk_label_set_text(GTK_LABEL(pw->status), base);
    }
}

static void
on_view_notify(NsProcView *v, NsProcEvent evt, const char *text,
               gpointer user_data)
{
    ProcWindow *pw = user_data;
    GtkWidget *page = ns_proc_view_widget(v);
    int idx = page ? gtk_notebook_page_num(GTK_NOTEBOOK(pw->notebook), page)
                   : -1;
    gboolean is_current = (v == current_view(pw));

    switch (evt) {
    case NS_PROC_EVT_TITLE: {
        if (!ns_proc_view_is_private(v))
            ns_history_record(ns_proc_view_url(v), text);
        if (idx >= 0) {
            GtkWidget *p =
                gtk_notebook_get_nth_page(GTK_NOTEBOOK(pw->notebook), idx);
            GtkWidget *label = g_object_get_data(G_OBJECT(p), "ns-tab-label");
            const char *t = text && *text ? text : ns_i18n("Untitled");
            char *clip = g_strndup(t, 40);
            if (label)
                gtk_label_set_text(GTK_LABEL(label), clip);
            g_free(clip);
        }
        if (is_current)
            update_chrome(pw);
        break;
    }
    case NS_PROC_EVT_URL:
        if (is_current) {
            set_address_text(pw, text);
            pw->webgl_active = FALSE;
            g_clear_pointer(&pw->status_base, g_free);
            pw_render_status(pw);
        }
        break;
    case NS_PROC_EVT_STATUS:
        if (is_current) {
            g_free(pw->status_base);
            pw->status_base = g_strdup(text ? text : "");
            pw_render_status(pw);
        }
        break;
    case NS_PROC_EVT_WEBGL:
        if (is_current && !pw->webgl_active) {
            pw->webgl_active = TRUE;
            pw_render_status(pw);
        }
        break;
    case NS_PROC_EVT_HISTORY:
        if (is_current) {
            gtk_widget_set_sensitive(pw->back, ns_proc_view_can_back(v));
            gtk_widget_set_sensitive(pw->forward, ns_proc_view_can_forward(v));
        }
        break;
    case NS_PROC_EVT_NEWTAB:
        if (text && *text)
            proc_window_add_tab(pw, text, FALSE);
        break;
    case NS_PROC_EVT_LOADING:
        if (is_current)
            set_loading_ui(pw, text && *text == '1');
        break;
    case NS_PROC_EVT_DOWNLOAD:
        if (text && *text) {
            char **parts = g_strsplit(text, "\t", 2);
            pw_start_download(pw, parts[0],
                              parts[1] && *parts[1] ? parts[1] : NULL);
            g_strfreev(parts);
        }
        break;
    case NS_PROC_EVT_FAVICON:
        if (idx >= 0 && !ns_proc_view_is_private(v)) {
            GtkWidget *p =
                gtk_notebook_get_nth_page(GTK_NOTEBOOK(pw->notebook), idx);
            GtkWidget *icon = g_object_get_data(G_OBJECT(p), "ns-tab-icon");
            GdkPaintable *fav = ns_proc_view_favicon(v);
            if (icon && fav)
                gtk_image_set_from_paintable(GTK_IMAGE(icon), fav);
            else if (icon)
                gtk_image_set_from_icon_name(GTK_IMAGE(icon),
                                             "text-x-generic-symbolic");
            if (icon)
                gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
        }
        break;
    }
}

static void
update_active_tab(ProcWindow *pw)
{
    GtkWidget *current = NULL;
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(pw->notebook));
    if (idx >= 0)
        current = gtk_notebook_get_nth_page(GTK_NOTEBOOK(pw->notebook), idx);
    for (GtkWidget *w = gtk_widget_get_first_child(pw->tabstrip);
         w; w = gtk_widget_get_next_sibling(w)) {
        GtkWidget *page = g_object_get_data(G_OBJECT(w), "ns-page");
        GtkWidget *btn = g_object_get_data(G_OBJECT(w), "ns-tab-button");
        if (!page || !btn)
            continue;
        if (page == current)
            gtk_widget_add_css_class(btn, "ns-tab-active");
        else
            gtk_widget_remove_css_class(btn, "ns-tab-active");
    }
}

static void
on_tab_clicked(GtkButton *button, gpointer user_data)
{
    ProcWindow *pw = g_object_get_data(G_OBJECT(button), "ns-pw");
    GtkWidget *page = user_data;
    int idx = gtk_notebook_page_num(GTK_NOTEBOOK(pw->notebook), page);
    if (idx >= 0)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(pw->notebook), idx);
}

static void
on_tab_close(GtkButton *button, gpointer user_data)
{
    ProcWindow *pw = g_object_get_data(G_OBJECT(button), "ns-pw");
    GtkWidget *page = user_data;
    GtkWidget *wrapper = g_object_get_data(G_OBJECT(page), "ns-strip-tab");
    int idx = gtk_notebook_page_num(GTK_NOTEBOOK(pw->notebook), page);
    if (idx >= 0)
        gtk_notebook_remove_page(GTK_NOTEBOOK(pw->notebook), idx);
    if (wrapper)
        gtk_box_remove(GTK_BOX(pw->tabstrip), wrapper);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(pw->notebook)) == 0)
        gtk_window_close(GTK_WINDOW(pw->window));
}

static void
proc_window_add_tab_full(ProcWindow *pw, const char *url, gboolean foreground,
                         gboolean private_mode)
{
    if (!private_mode && ns_rproc_single_process_enabled() &&
        gtk_notebook_get_n_pages(GTK_NOTEBOOK(pw->notebook)) > 0) {
        NsProcView *cur = current_view(pw);
        if (cur) {
            char *r = normalize_url(url);
            gtk_editable_set_text(GTK_EDITABLE(pw->address), r);
            ns_proc_view_load(cur, r);
            g_free(r);
            return;
        }
    }

    NsProcView *v = ns_proc_view_new();
    if (private_mode)
        ns_proc_view_set_private(v, TRUE);
    ns_proc_view_set_notify(v, on_view_notify, pw);
    GtkWidget *page = ns_proc_view_widget(v);
    g_object_set_data(G_OBJECT(page), "ns-proc-view", v);

    GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(wrapper, "ns-tab");

    GtkWidget *tabbtn = gtk_button_new();
    gtk_button_set_has_frame(GTK_BUTTON(tabbtn), FALSE);
    gtk_widget_add_css_class(tabbtn, "ns-tab-label");
    GtkWidget *tabcontent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *icon = gtk_image_new_from_icon_name(
        private_mode ? "user-not-tracked-symbolic" : "text-x-generic-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    if (private_mode)
        gtk_widget_set_tooltip_text(tabbtn, ns_i18n("Private tab"));
    GtkWidget *label = gtk_label_new(
        private_mode ? ns_i18n("Private tab") : ns_i18n("New Tab"));
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(label), 16);
    gtk_box_append(GTK_BOX(tabcontent), icon);
    gtk_box_append(GTK_BOX(tabcontent), label);
    gtk_button_set_child(GTK_BUTTON(tabbtn), tabcontent);
    g_object_set_data(G_OBJECT(tabbtn), "ns-pw", pw);
    g_signal_connect(tabbtn, "clicked", G_CALLBACK(on_tab_clicked), page);
    gtk_box_append(GTK_BOX(wrapper), tabbtn);

    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    gtk_widget_set_tooltip_text(close, ns_i18n("Close tab"));
    set_accessible_label(close, ns_i18n("Close tab"));
    g_object_set_data(G_OBJECT(close), "ns-pw", pw);
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close), page);
    gtk_box_append(GTK_BOX(wrapper), close);

    g_object_set_data(G_OBJECT(wrapper), "ns-page", page);
    g_object_set_data(G_OBJECT(wrapper), "ns-tab-button", tabbtn);
    g_object_set_data(G_OBJECT(page), "ns-tab-label", label);
    g_object_set_data(G_OBJECT(page), "ns-tab-icon", icon);
    g_object_set_data(G_OBJECT(page), "ns-strip-tab", wrapper);

    GtkWidget *blank = gtk_label_new(NULL);
    int idx = gtk_notebook_append_page(GTK_NOTEBOOK(pw->notebook), page, blank);

    g_object_ref(pw->newtab_btn);
    gtk_box_remove(GTK_BOX(pw->tabstrip), pw->newtab_btn);
    gtk_box_append(GTK_BOX(pw->tabstrip), wrapper);
    gtk_box_append(GTK_BOX(pw->tabstrip), pw->newtab_btn);
    g_object_unref(pw->newtab_btn);

    if (foreground)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(pw->notebook), idx);
    update_active_tab(pw);

    char *resolved = normalize_url(url);
    ns_proc_view_load(v, resolved);
    g_free(resolved);
}

static void
proc_window_add_tab(ProcWindow *pw, const char *url, gboolean foreground)
{
    proc_window_add_tab_full(pw, url, foreground, FALSE);
}

static gboolean
address_select_all_idle(gpointer user_data)
{
    ProcWindow *pw = user_data;
    if (pw->address && GTK_IS_EDITABLE(pw->address))
        gtk_editable_select_region(GTK_EDITABLE(pw->address), 0, -1);
    return G_SOURCE_REMOVE;
}

static void
on_address_focus_enter(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    (void)ctrl;
    g_idle_add(address_select_all_idle, user_data);
}

static void
on_address_activate(GtkEntry *entry, gpointer user_data)
{
    ProcWindow *pw = user_data;
    NsProcView *v = current_view(pw);
    char *resolved = normalize_url(gtk_editable_get_text(GTK_EDITABLE(entry)));
    if (!*resolved) {
        g_free(resolved);
        return;
    }
    if (!v) {
        proc_window_add_tab(pw, resolved, TRUE);
        v = current_view(pw);
    } else {
        gtk_editable_set_text(GTK_EDITABLE(pw->address), resolved);
        ns_proc_view_load(v, resolved);
    }
    if (v)
        ns_proc_view_focus(v);
    g_free(resolved);
}

static void
on_back_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_back(v);
}

static void
on_forward_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_forward(v);
}

static void
on_reload_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_reload(v);
}

static void
on_home_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    ProcWindow *pw = ud;
    NsProcView *v = current_view(pw);
    if (v)
        ns_proc_view_load(v, pw->home_url ? pw->home_url : "about:start");
}

static void
on_logo_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_load(v, "https://nordstjernen.org");
}

static void
on_go_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    ProcWindow *pw = ud;
    on_address_activate(GTK_ENTRY(pw->address), pw);
}

static void
on_newtab_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    proc_window_add_tab(ud, "about:start", TRUE);
}

#if defined(NS_HAVE_AI) && !defined(__APPLE__)
static void
on_ai_window_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    proc_window_add_tab(ud, "about:ai-window", TRUE);
}
#endif

static void
on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    (void)nb;
    (void)page;
    (void)num;
    update_active_tab(ud);
    update_chrome(ud);
}

static void
act_back(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_back(v);
}

static void
act_forward(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_forward(v);
}

static void
act_reload(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_reload(v);
}

static void
act_find(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_find_open(v);
}

static void
act_console(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_toggle_console(v);
}

static void
act_home(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    ProcWindow *pw = ud;
    NsProcView *v = current_view(pw);
    if (v)
        ns_proc_view_load(v, pw->home_url ? pw->home_url : "about:start");
}

static void
act_new_tab(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    proc_window_add_tab(ud, "about:start", TRUE);
}

static void
act_new_private_tab(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    proc_window_add_tab_full(ud, "about:start", TRUE, TRUE);
}

#if defined(NS_HAVE_AI) && !defined(__APPLE__)
static void
act_new_ai_window(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    proc_window_add_tab(ud, "about:ai-window", TRUE);
}
#endif

static void
act_close_tab(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    ProcWindow *pw = ud;
    int idx = gtk_notebook_get_current_page(GTK_NOTEBOOK(pw->notebook));
    if (idx < 0)
        return;
    gtk_notebook_remove_page(GTK_NOTEBOOK(pw->notebook), idx);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(pw->notebook)) == 0)
        gtk_window_close(GTK_WINDOW(pw->window));
}

static void
act_focus_address(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    ProcWindow *pw = ud;
    gtk_widget_grab_focus(pw->address);
    gtk_editable_select_region(GTK_EDITABLE(pw->address), 0, -1);
}

static void
act_focus_page(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_focus(v);
}

static void
act_zoom_in(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_zoom_in(v);
}

static void
act_zoom_out(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_zoom_out(v);
}

static void
act_zoom_reset(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    NsProcView *v = current_view(ud);
    if (v)
        ns_proc_view_zoom_reset(v);
}

static void
act_step_tab(ProcWindow *pw, int delta)
{
    GtkNotebook *nb = GTK_NOTEBOOK(pw->notebook);
    int n = gtk_notebook_get_n_pages(nb);
    if (n <= 1)
        return;
    int idx = gtk_notebook_get_current_page(nb);
    idx = ((idx + delta) % n + n) % n;
    gtk_notebook_set_current_page(nb, idx);
}

static void
act_next_tab(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    act_step_tab(ud, 1);
}

static void
act_prev_tab(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    act_step_tab(ud, -1);
}

static void
act_quit(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a;
    (void)p;
    ProcWindow *pw = ud;
    gtk_window_close(GTK_WINDOW(pw->window));
}

static void act_about(GSimpleAction *action, GVariant *parameter,
                      gpointer user_data);
static void act_settings(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data);
static void on_bookmarks_clicked(GtkButton *button, gpointer user_data);

/* ---- Task manager: lists each tab's sandboxed renderer process ---- */

typedef struct {
    ProcWindow *pw;
    GtkWidget  *list;
    GtkWidget  *dump_win;
    GtkWidget  *dump_view;
    guint       timer;
    GHashTable *cpu_hist;
    gint64      now_us;
    guint       tick;
    int         ncpu;
} NsTaskMgr;

typedef struct {
    double base_cpu;
    gint64 base_us;
    double pct;
    guint  tick;
} NsCpuHist;

static double
task_mgr_cpu_pct(NsTaskMgr *tm, int pid, double cpu_now)
{
    if (pid <= 0 || cpu_now < 0) return -1.0;
    NsCpuHist *h = g_hash_table_lookup(tm->cpu_hist, GINT_TO_POINTER(pid));
    if (!h) {
        h = g_new0(NsCpuHist, 1);
        h->base_cpu = cpu_now;
        h->base_us = tm->now_us;
        h->pct = -1.0;
        h->tick = tm->tick;
        g_hash_table_insert(tm->cpu_hist, GINT_TO_POINTER(pid), h);
        return -1.0;
    }
    if (h->tick == tm->tick) return h->pct;
    double dt = (double)(tm->now_us - h->base_us) / 1e6;
    double pct = -1.0;
    if (dt >= 0.1 && cpu_now >= h->base_cpu) {
        pct = (cpu_now - h->base_cpu) / dt * 100.0;
        double cap = tm->ncpu > 0 ? tm->ncpu * 100.0 : 100.0;
        if (pct > cap) pct = cap;
        if (pct < 0.0) pct = 0.0;
    }
    h->base_cpu = cpu_now;
    h->base_us = tm->now_us;
    h->pct = pct;
    h->tick = tm->tick;
    return pct;
}

static gboolean
task_mgr_hist_stale(gpointer key, gpointer val, gpointer data)
{
    (void)key;
    return ((NsCpuHist *)val)->tick != ((NsTaskMgr *)data)->tick;
}

static GtkWidget *
task_mgr_header_label(const char *text, int width, gfloat xalign, gboolean expand)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    if (width > 0) gtk_label_set_width_chars(GTK_LABEL(l), width);
    if (expand) gtk_widget_set_hexpand(l, TRUE);
    gtk_widget_add_css_class(l, "heading");
    return l;
}

static void
task_mgr_add_row(NsTaskMgr *tm, const char *icon_name, const char *name,
                 int pid, const char *state, long rss, NsProcView *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);

    GtkWidget *l_title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand(l_title, TRUE);
    if (icon_name) {
        GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
        gtk_box_append(GTK_BOX(l_title), icon);
    }
    GtkWidget *l_name = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(l_name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l_name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(l_name, TRUE);
    gtk_box_append(GTK_BOX(l_title), l_name);

    char pidbuf[24];
    if (pid > 0) g_snprintf(pidbuf, sizeof pidbuf, "%d", pid);
    else         g_strlcpy(pidbuf, "—", sizeof pidbuf);
    GtkWidget *l_pid = gtk_label_new(pidbuf);
    gtk_label_set_width_chars(GTK_LABEL(l_pid), 8);
    gtk_label_set_xalign(GTK_LABEL(l_pid), 1);

    char thrbuf[24];
    int threads = pid > 0 ? ns_rproc_http_proc_threads(pid) : -1;
    if (threads >= 0) g_snprintf(thrbuf, sizeof thrbuf, "%d", threads);
    else              g_strlcpy(thrbuf, "—", sizeof thrbuf);
    GtkWidget *l_thr = gtk_label_new(thrbuf);
    gtk_label_set_width_chars(GTK_LABEL(l_thr), 8);
    gtk_label_set_xalign(GTK_LABEL(l_thr), 1);

    GtkWidget *l_state = gtk_label_new(state);
    gtk_label_set_width_chars(GTK_LABEL(l_state), 11);
    gtk_label_set_xalign(GTK_LABEL(l_state), 0);

    char membuf[24];
    if (rss >= 0) g_snprintf(membuf, sizeof membuf, "%.1f MB", rss / 1024.0);
    else          g_strlcpy(membuf, "—", sizeof membuf);
    GtkWidget *l_mem = gtk_label_new(membuf);
    gtk_label_set_width_chars(GTK_LABEL(l_mem), 10);
    gtk_label_set_xalign(GTK_LABEL(l_mem), 1);

    char timebuf[24];
    double cpu = pid > 0 ? ns_rproc_http_proc_cpu(pid) : -1.0;
    if (cpu < 0)        g_strlcpy(timebuf, "—", sizeof timebuf);
    else if (cpu >= 60) g_snprintf(timebuf, sizeof timebuf, "%d:%04.1f",
                                   (int)cpu / 60, cpu - (int)(cpu / 60) * 60);
    else                g_snprintf(timebuf, sizeof timebuf, "%.1f s", cpu);
    GtkWidget *l_time = gtk_label_new(timebuf);
    gtk_label_set_width_chars(GTK_LABEL(l_time), 9);
    gtk_label_set_xalign(GTK_LABEL(l_time), 1);

    char pctbuf[24];
    double pct = task_mgr_cpu_pct(tm, pid, cpu);
    if (pct < 0) g_strlcpy(pctbuf, "—", sizeof pctbuf);
    else         g_snprintf(pctbuf, sizeof pctbuf, "%.1f %%", pct);
    GtkWidget *l_pct = gtk_label_new(pctbuf);
    gtk_label_set_width_chars(GTK_LABEL(l_pct), 7);
    gtk_label_set_xalign(GTK_LABEL(l_pct), 1);

    gtk_box_append(GTK_BOX(box), l_title);
    gtk_box_append(GTK_BOX(box), l_pid);
    gtk_box_append(GTK_BOX(box), l_thr);
    gtk_box_append(GTK_BOX(box), l_state);
    gtk_box_append(GTK_BOX(box), l_mem);
    gtk_box_append(GTK_BOX(box), l_pct);
    gtk_box_append(GTK_BOX(box), l_time);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    if (v) g_object_set_data(G_OBJECT(row), "ns-view", v);
    g_object_set_data(G_OBJECT(row), "ns-pid", GINT_TO_POINTER(pid));
    g_object_set_data_full(G_OBJECT(row), "ns-name", g_strdup(name), g_free);
    gtk_list_box_append(GTK_LIST_BOX(tm->list), row);
}

static void
task_mgr_refresh(NsTaskMgr *tm)
{
    tm->tick++;
    tm->now_us = g_get_monotonic_time();

    GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(tm->list));
    int selected_pid = sel
        ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(sel), "ns-pid")) : 0;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(tm->list)))
        gtk_list_box_remove(GTK_LIST_BOX(tm->list), child);

    int wpid = ns_watchdog_supervisor_pid();
    if (wpid > 0) {
        char wstate[32] = "";
        long wrss = -1;
        ns_rproc_http_proc_info(wpid, wstate, sizeof wstate, &wrss);
        char *wname = g_strdup_printf("%s (%s)", ns_i18n("Nordstjernen"),
                                      ns_i18n("watchdog"));
        task_mgr_add_row(tm, "applications-system-symbolic", wname, wpid,
                         wstate, wrss, NULL);
        g_free(wname);
    }

    {
        int gpid = ns_rproc_self_pid();
        char gstate[32] = "";
        long grss = -1;
        ns_rproc_http_proc_info(gpid, gstate, sizeof gstate, &grss);
        char *gname = g_strdup_printf("%s (GTK frontend)",
                                      ns_i18n("Nordstjernen"));
        task_mgr_add_row(tm, "web-browser-symbolic", gname, gpid, gstate,
                         grss, NULL);
        g_free(gname);
    }

    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(tm->pw->notebook));
    for (int i = 0; i < n; i++) {
        NsProcView *v = view_for_page(
            gtk_notebook_get_nth_page(GTK_NOTEBOOK(tm->pw->notebook), i));
        if (!v) continue;

        int pid = ns_proc_view_renderer_pid(v);
        char state[32] = "starting";
        long rss = -1;
        if (pid > 0) {
            ns_rproc_http_proc_info(pid, state, sizeof state, &rss);
        } else if (ns_rproc_single_process_enabled()) {
            pid = ns_rproc_self_pid();
            ns_rproc_http_proc_info(pid, state, sizeof state, &rss);
            g_strlcpy(state, "in-process", sizeof state);
        }

        const char *title = ns_proc_view_title(v);
        const char *url = ns_proc_view_url(v);
        const char *tab = (title && *title) ? title
                        : (url && *url)     ? url : ns_i18n("New Tab");
        char *name = g_strdup_printf("%s  —  %s", ns_i18n("HTML renderer"), tab);

        task_mgr_add_row(tm, "text-x-generic-symbolic", name, pid, state,
                         rss, v);
        g_free(name);

        int apid = ns_proc_view_audio_pid(v);
        if (apid > 0) {
            char astate[32] = "";
            long arss = -1;
            ns_rproc_http_proc_info(apid, astate, sizeof astate, &arss);
            char *aname = g_strdup_printf("   ⤷ %s", ns_i18n("Audio playback"));
            task_mgr_add_row(tm, "audio-volume-high-symbolic", aname, apid,
                             astate, arss, v);
            g_free(aname);
        }
        int vpid = ns_proc_view_video_pid(v);
        if (vpid > 0) {
            char vstate[32] = "";
            long vrss = -1;
            ns_rproc_http_proc_info(vpid, vstate, sizeof vstate, &vrss);
            char *vname = g_strdup_printf("   ⤷ %s", ns_i18n("Video decoder"));
            task_mgr_add_row(tm, "video-x-generic-symbolic", vname, vpid,
                             vstate, vrss, v);
            g_free(vname);
        }
    }

    if (selected_pid > 0) {
        for (GtkWidget *r = gtk_widget_get_first_child(tm->list);
             r; r = gtk_widget_get_next_sibling(r)) {
            if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "ns-pid"))
                    == selected_pid) {
                gtk_list_box_select_row(GTK_LIST_BOX(tm->list),
                                        GTK_LIST_BOX_ROW(r));
                break;
            }
        }
    }

    g_hash_table_foreach_remove(tm->cpu_hist, task_mgr_hist_stale, tm);
}

static gboolean
task_mgr_tick(gpointer data)
{
    task_mgr_refresh(data);
    return G_SOURCE_CONTINUE;
}

static void
task_mgr_end_task(GtkButton *button, gpointer data)
{
    (void)button;
    NsTaskMgr *tm = data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(tm->list));
    NsProcView *v = row ? g_object_get_data(G_OBJECT(row), "ns-view") : NULL;
    int pid = row ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "ns-pid"))
                  : 0;
    if (v) {
        if (pid > 0 && pid == ns_proc_view_video_pid(v))
            ns_proc_view_stop_video(v);
        else if (pid > 0 && pid == ns_proc_view_audio_pid(v))
            ns_proc_view_stop_audio(v);
        else
            ns_proc_view_end_task(v);
    }
    task_mgr_refresh(tm);
}

static void
task_mgr_refresh_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    task_mgr_refresh(data);
}

static void
task_mgr_dump_win_destroyed(GtkWidget *win, gpointer data)
{
    (void)win;
    NsTaskMgr *tm = data;
    tm->dump_win = NULL;
    tm->dump_view = NULL;
}

static void
task_mgr_show_dump(NsTaskMgr *tm, const char *text)
{
    if (!tm->dump_win) {
        GtkWidget *win = gtk_window_new();
        gtk_window_set_title(GTK_WINDOW(win), ns_i18n("Thread dump"));
        gtk_window_set_transient_for(GTK_WINDOW(win),
                                     GTK_WINDOW(tm->pw->window));
        gtk_window_set_default_size(GTK_WINDOW(win), 680, 480);

        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(scroll, TRUE);
        GtkWidget *view = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
        gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_NONE);
        gtk_widget_set_margin_start(view, 8);
        gtk_widget_set_margin_end(view, 8);
        gtk_widget_set_margin_top(view, 8);
        gtk_widget_set_margin_bottom(view, 8);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
        gtk_window_set_child(GTK_WINDOW(win), scroll);

        tm->dump_win = win;
        tm->dump_view = view;
        g_signal_connect(win, "destroy",
                         G_CALLBACK(task_mgr_dump_win_destroyed), tm);
    }

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tm->dump_view));
    char *valid = g_utf8_make_valid(text, -1);
    gtk_text_buffer_set_text(buf, valid, -1);
    g_free(valid);
    gtk_window_present(GTK_WINDOW(tm->dump_win));
}

static void
task_mgr_thread_dump(GtkButton *button, gpointer data)
{
    (void)button;
    NsTaskMgr *tm = data;
    GString *out = g_string_new(NULL);
    int dumped = 0;
    for (GtkWidget *r = gtk_widget_get_first_child(tm->list);
         r; r = gtk_widget_get_next_sibling(r)) {
        int pid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "ns-pid"));
        if (pid <= 0) continue;
        const char *nm = g_object_get_data(G_OBJECT(r), "ns-name");
        char *text = ns_thread_dump_text(pid, nm ? nm : "process");
        if (!text) continue;
        fputs(text, stderr);
        if (dumped) g_string_append_c(out, '\n');
        g_string_append(out, text);
        free(text);
        dumped++;
    }
    fflush(stderr);
    if (!dumped)
        g_string_append(out, ns_i18n("No processes to dump."));
    task_mgr_show_dump(tm, out->str);
    g_string_free(out, TRUE);
}

static void
task_mgr_destroyed(GtkWidget *win, gpointer data)
{
    (void)win;
    NsTaskMgr *tm = data;
    if (tm->timer) g_source_remove(tm->timer);
    if (tm->cpu_hist) g_hash_table_destroy(tm->cpu_hist);
    if (tm->dump_win) gtk_window_destroy(GTK_WINDOW(tm->dump_win));
    if (tm->pw->task_mgr_win) tm->pw->task_mgr_win = NULL;
    g_free(tm);
}

static void
act_downloads(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    show_downloads_window((ProcWindow *)user_data);
}

static void
act_task_manager(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ProcWindow *pw = user_data;
    if (pw->task_mgr_win) {
        gtk_window_present(GTK_WINDOW(pw->task_mgr_win));
        return;
    }

    GtkWidget *win = gtk_window_new();
    char *tm_title = g_strdup_printf("%s — %s", ns_i18n("Task Manager"),
                                     ns_i18n("Nordstjernen"));
    gtk_window_set_title(GTK_WINDOW(win), tm_title);
    g_free(tm_title);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(pw->window));
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 380);

    NsTaskMgr *tm = g_new0(NsTaskMgr, 1);
    tm->pw = pw;
    tm->cpu_hist = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, g_free);
    tm->ncpu = (int)g_get_num_processors();

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(hdr, 10);
    gtk_widget_set_margin_end(hdr, 10);
    gtk_widget_set_margin_top(hdr, 8);
    gtk_widget_set_margin_bottom(hdr, 4);
    gtk_box_append(GTK_BOX(hdr), task_mgr_header_label(ns_i18n("Task"), 0, 0, TRUE));
    gtk_box_append(GTK_BOX(hdr), task_mgr_header_label(ns_i18n("Process ID"), 8, 1, FALSE));
    gtk_box_append(GTK_BOX(hdr), task_mgr_header_label(ns_i18n("Threads"), 8, 1, FALSE));
    gtk_box_append(GTK_BOX(hdr), task_mgr_header_label(ns_i18n("State"), 11, 0, FALSE));
    gtk_box_append(GTK_BOX(hdr), task_mgr_header_label(ns_i18n("Memory"), 10, 1, FALSE));
    gtk_box_append(GTK_BOX(hdr), task_mgr_header_label(ns_i18n("CPU %"), 7, 1, FALSE));
    gtk_box_append(GTK_BOX(hdr), task_mgr_header_label(ns_i18n("CPU time"), 9, 1, FALSE));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    tm->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(tm->list), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), tm->list);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 10);
    gtk_widget_set_margin_end(bar, 10);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 8);
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *dump_btn = gtk_button_new_with_label(ns_i18n("Thread dump"));
    g_signal_connect(dump_btn, "clicked",
                     G_CALLBACK(task_mgr_thread_dump), tm);
    GtkWidget *refresh_btn = gtk_button_new_with_label(ns_i18n("Refresh"));
    g_signal_connect(refresh_btn, "clicked",
                     G_CALLBACK(task_mgr_refresh_clicked), tm);
    GtkWidget *end_btn = gtk_button_new_with_label(ns_i18n("End task"));
    gtk_widget_add_css_class(end_btn, "destructive-action");
    g_signal_connect(end_btn, "clicked", G_CALLBACK(task_mgr_end_task), tm);
    gtk_box_append(GTK_BOX(bar), spacer);
    gtk_box_append(GTK_BOX(bar), dump_btn);
    gtk_box_append(GTK_BOX(bar), refresh_btn);
    gtk_box_append(GTK_BOX(bar), end_btn);

    gtk_box_append(GTK_BOX(vbox), hdr);
    gtk_box_append(GTK_BOX(vbox), scroll);
    gtk_box_append(GTK_BOX(vbox), bar);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    pw->task_mgr_win = win;
    g_signal_connect(win, "destroy", G_CALLBACK(task_mgr_destroyed), tm);

    task_mgr_refresh(tm);
    tm->timer = g_timeout_add(1500, task_mgr_tick, tm);
    gtk_window_present(GTK_WINDOW(win));
}

static void
install_action(ProcWindow *pw, const char *name, GCallback cb,
               const char *const *accels)
{
    GSimpleAction *act = g_simple_action_new(name, NULL);
    g_signal_connect(act, "activate", cb, pw);
    g_action_map_add_action(G_ACTION_MAP(pw->window), G_ACTION(act));
    g_object_unref(act);
    if (accels) {
        char *full = g_strconcat("win.", name, NULL);
        gtk_application_set_accels_for_action(pw->app, full, accels);
        g_free(full);
    }
}

static void
install_shortcuts(ProcWindow *pw)
{
    install_action(pw, "back", G_CALLBACK(act_back),
                   (const char *[]){ "<Alt>Left", NULL });
    install_action(pw, "forward", G_CALLBACK(act_forward),
                   (const char *[]){ "<Alt>Right", NULL });
    install_action(pw, "reload", G_CALLBACK(act_reload),
                   (const char *[]){ "<Ctrl>r", "F5", NULL });
    install_action(pw, "find", G_CALLBACK(act_find),
                   (const char *[]){ "<Ctrl>f", NULL });
    install_action(pw, "console", G_CALLBACK(act_console),
                   (const char *[]){ "<Ctrl><Shift>j", "F12", NULL });
    install_action(pw, "home", G_CALLBACK(act_home),
                   (const char *[]){ "<Alt>Home", NULL });
    install_action(pw, "new-tab", G_CALLBACK(act_new_tab),
                   (const char *[]){ "<Ctrl>t", NULL });
    install_action(pw, "new-private-tab", G_CALLBACK(act_new_private_tab),
                   (const char *[]){ "<Ctrl><Alt>p", NULL });
#if defined(NS_HAVE_AI) && !defined(__APPLE__)
    install_action(pw, "new-ai-window", G_CALLBACK(act_new_ai_window),
                   (const char *[]){ "<Ctrl><Shift>a", NULL });
#endif
    install_action(pw, "close-tab", G_CALLBACK(act_close_tab),
                   (const char *[]){ "<Ctrl>w", NULL });
    install_action(pw, "focus-address", G_CALLBACK(act_focus_address),
                   (const char *[]){ "<Ctrl>l", NULL });
    install_action(pw, "focus-page", G_CALLBACK(act_focus_page),
                   (const char *[]){ "Escape", NULL });
    install_action(pw, "zoom-in", G_CALLBACK(act_zoom_in),
                   (const char *[]){ "<Ctrl>plus", "<Ctrl>equal",
                                     "<Ctrl>KP_Add", NULL });
    install_action(pw, "zoom-out", G_CALLBACK(act_zoom_out),
                   (const char *[]){ "<Ctrl>minus", "<Ctrl>KP_Subtract",
                                     NULL });
    install_action(pw, "zoom-reset", G_CALLBACK(act_zoom_reset),
                   (const char *[]){ "<Ctrl>0", "<Ctrl>KP_0", NULL });
    install_action(pw, "next-tab", G_CALLBACK(act_next_tab),
                   (const char *[]){ "<Ctrl>Page_Down", "<Ctrl>Tab", NULL });
    install_action(pw, "prev-tab", G_CALLBACK(act_prev_tab),
                   (const char *[]){ "<Ctrl>Page_Up", "<Ctrl><Shift>Tab",
                                     NULL });
    install_action(pw, "task-manager", G_CALLBACK(act_task_manager),
                   (const char *[]){ "<Shift>Escape", NULL });
    install_action(pw, "downloads", G_CALLBACK(act_downloads),
                   (const char *[]){ "<Ctrl>j", NULL });
    install_action(pw, "about", G_CALLBACK(act_about), NULL);
    install_action(pw, "settings", G_CALLBACK(act_settings),
                   (const char *[]){ "<Ctrl>comma", NULL });
    install_action(pw, "quit", G_CALLBACK(act_quit),
                   (const char *[]){ "<Ctrl>q", NULL });

    if (ns_rproc_single_process_enabled()) {
        GAction *nt = g_action_map_lookup_action(G_ACTION_MAP(pw->window),
                                                 "new-tab");
        if (nt) g_simple_action_set_enabled(G_SIMPLE_ACTION(nt), FALSE);
        GAction *npt = g_action_map_lookup_action(G_ACTION_MAP(pw->window),
                                                  "new-private-tab");
        if (npt) g_simple_action_set_enabled(G_SIMPLE_ACTION(npt), FALSE);
    }
}

static gboolean
on_window_key_pressed(GtkEventControllerKey *controller, guint keyval,
                      guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)state;
    ProcWindow *pw = user_data;
    GtkWindow *win = GTK_WINDOW(pw->window);
    if (keyval == GDK_KEY_F11) {
        if (gtk_window_is_fullscreen(win)) {
            gtk_window_unfullscreen(win);
            gtk_window_maximize(win);
        } else {
            gtk_window_fullscreen(win);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape && gtk_window_is_fullscreen(win)) {
        gtk_window_unfullscreen(win);
        gtk_window_maximize(win);
        return TRUE;
    }
    return FALSE;
}

static ProcWindow *
proc_window_new(GtkApplication *app, const char *home_url)
{
    ProcWindow *pw = g_new0(ProcWindow, 1);
    pw->app = app;
    pw->home_url = g_strdup(home_url && *home_url ? home_url : "about:start");
    pw->bookmarks = ns_bookmarks_load();
    pw->window = gtk_application_window_new(app);
    g_object_set_data_full(G_OBJECT(pw->window), "ns-procwindow", pw,
                           (GDestroyNotify)procwindow_free);
    gtk_window_set_title(GTK_WINDOW(pw->window), ns_brand_versioned());
    if (g_initial_win_w > 0 && g_initial_win_h > 0) {
        gtk_window_set_default_size(GTK_WINDOW(pw->window),
                                    g_initial_win_w, g_initial_win_h);
    } else {
        gtk_window_set_default_size(GTK_WINDOW(pw->window), 1024, 768);
        gtk_window_maximize(GTK_WINDOW(pw->window));
    }

    GtkEventController *winkeys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(winkeys, GTK_PHASE_CAPTURE);
    g_signal_connect(winkeys, "key-pressed",
                     G_CALLBACK(on_window_key_pressed), pw);
    gtk_widget_add_controller(pw->window, winkeys);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    pw->tabstrip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(pw->tabstrip, "ns-tabstrip");
    gtk_widget_set_hexpand(pw->tabstrip, TRUE);
    pw->newtab_btn = gtk_button_new_from_icon_name("tab-new-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(pw->newtab_btn), FALSE);
    gtk_widget_add_css_class(pw->newtab_btn, "ns-newtab");
    gtk_widget_set_tooltip_text(pw->newtab_btn, ns_i18n("New tab"));
    set_accessible_label(pw->newtab_btn, ns_i18n("New tab"));
    g_signal_connect(pw->newtab_btn, "clicked",
                     G_CALLBACK(on_newtab_clicked), pw);
    gtk_box_append(GTK_BOX(pw->tabstrip), pw->newtab_btn);
    if (ns_rproc_single_process_enabled())
        gtk_widget_set_visible(pw->newtab_btn, FALSE);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), pw->tabstrip);
    gtk_window_set_titlebar(GTK_WINDOW(pw->window), header);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(toolbar, "ns-toolbar");
    gtk_widget_set_margin_top(toolbar, 2);
    gtk_widget_set_margin_bottom(toolbar, 2);
    gtk_widget_set_margin_start(toolbar, 4);
    gtk_widget_set_margin_end(toolbar, 4);

    pw->back = toolbar_button("nordstjernen-back", ns_i18n("Back"),
                              G_CALLBACK(on_back_clicked), pw);
    pw->forward = toolbar_button("nordstjernen-forward", ns_i18n("Forward"),
                                 G_CALLBACK(on_forward_clicked), pw);
    pw->reload = toolbar_button("nordstjernen-reload", ns_i18n("Reload"),
                                G_CALLBACK(on_reload_clicked), pw);
    GtkWidget *home = toolbar_button("nordstjernen-home", ns_i18n("Home"),
                                     G_CALLBACK(on_home_clicked), pw);

    pw->spinner = gtk_spinner_new();
    gtk_widget_set_tooltip_text(pw->spinner, ns_i18n("Loading"));
    gtk_widget_set_valign(pw->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(pw->spinner, FALSE);

    pw->security_icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(pw->security_icon), 16);
    gtk_widget_set_valign(pw->security_icon, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(pw->security_icon, "ns-sec-icon");
    gtk_widget_set_visible(pw->security_icon, FALSE);

    pw->address = gtk_entry_new();
    gtk_widget_set_hexpand(pw->address, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(pw->address),
                                   ns_i18n("Enter a URL and press Enter"));
    set_accessible_label(pw->address, ns_i18n("Address and search bar"));
    g_signal_connect(pw->address, "activate",
                     G_CALLBACK(on_address_activate), pw);
    GtkEventController *addr_focus = gtk_event_controller_focus_new();
    g_signal_connect(addr_focus, "enter",
                     G_CALLBACK(on_address_focus_enter), pw);
    gtk_widget_add_controller(pw->address, addr_focus);

    GtkWidget *go = toolbar_button("nordstjernen-go", ns_i18n("Go"),
                                   G_CALLBACK(on_go_clicked), pw);
    pw->bookmarks_button = toolbar_button("user-bookmarks-symbolic",
                                          ns_i18n("Bookmarks"),
                                          G_CALLBACK(on_bookmarks_clicked), pw);
#if defined(NS_HAVE_AI) && !defined(__APPLE__)
    GtkWidget *ai_window_button =
        toolbar_button("nordstjernen-ai", ns_i18n("New AI Window"),
                       G_CALLBACK(on_ai_window_clicked), pw);
#endif

    GMenu *appmenu = g_menu_new();
    g_menu_append(appmenu, ns_i18n("New Tab"), "win.new-tab");
    g_menu_append(appmenu, ns_i18n("New Private Tab"), "win.new-private-tab");
#if defined(NS_HAVE_AI) && !defined(__APPLE__)
    g_menu_append(appmenu, ns_i18n("New AI Window"), "win.new-ai-window");
#endif
    g_menu_append(appmenu, ns_i18n("Reload"), "win.reload");
    g_menu_append(appmenu, ns_i18n("Find in Page"), "win.find");
    g_menu_append(appmenu, ns_i18n("JavaScript Console"), "win.console");
    g_menu_append(appmenu, ns_i18n("Downloads"), "win.downloads");
    g_menu_append(appmenu, ns_i18n("Task Manager"), "win.task-manager");
    g_menu_append(appmenu, ns_i18n("Settings"), "win.settings");
    GMenu *appmenu_about = g_menu_new();
    g_menu_append(appmenu_about, ns_i18n("About Nordstjernen"), "win.about");
    g_menu_append_section(appmenu, NULL, G_MENU_MODEL(appmenu_about));
    g_object_unref(appmenu_about);
    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button),
                                  "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button),
                                   G_MENU_MODEL(appmenu));
    gtk_widget_set_tooltip_text(menu_button, ns_i18n("Menu"));
    set_accessible_label(menu_button, ns_i18n("Menu"));
    g_object_unref(appmenu);

    GtkWidget *logo = gtk_image_new_from_icon_name("nordstjernen");
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 24);
    GtkWidget *logo_button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(logo_button), logo);
    gtk_button_set_has_frame(GTK_BUTTON(logo_button), FALSE);
    gtk_widget_set_tooltip_text(logo_button, ns_i18n("Visit nordstjernen.org"));
    set_accessible_label(logo_button, ns_i18n("Visit nordstjernen.org"));
    g_signal_connect(logo_button, "clicked", G_CALLBACK(on_logo_clicked), pw);

    gtk_box_append(GTK_BOX(toolbar), pw->back);
    gtk_box_append(GTK_BOX(toolbar), pw->forward);
    gtk_box_append(GTK_BOX(toolbar), pw->reload);
    gtk_box_append(GTK_BOX(toolbar), home);
    gtk_box_append(GTK_BOX(toolbar), pw->spinner);
    gtk_box_append(GTK_BOX(toolbar), pw->security_icon);
    gtk_box_append(GTK_BOX(toolbar), pw->address);
    gtk_box_append(GTK_BOX(toolbar), go);
    gtk_box_append(GTK_BOX(toolbar), pw->bookmarks_button);
#if defined(NS_HAVE_AI) && !defined(__APPLE__)
    gtk_box_append(GTK_BOX(toolbar), ai_window_button);
#endif
    gtk_box_append(GTK_BOX(toolbar), menu_button);
    gtk_box_append(GTK_BOX(toolbar), logo_button);
    gtk_box_append(GTK_BOX(vbox), toolbar);

    pw->notebook = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(pw->notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(pw->notebook), FALSE);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(pw->notebook), TRUE);
    gtk_widget_set_hexpand(pw->notebook, TRUE);
    gtk_widget_set_vexpand(pw->notebook, TRUE);
    g_signal_connect(pw->notebook, "switch-page",
                     G_CALLBACK(on_switch_page), pw);
    gtk_box_append(GTK_BOX(vbox), pw->notebook);

    pw->status = gtk_label_new("");
    gtk_widget_set_halign(pw->status, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(pw->status), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(pw->status, "ns-procstatus");
    gtk_box_append(GTK_BOX(vbox), pw->status);

    gtk_window_set_child(GTK_WINDOW(pw->window), vbox);
    install_shortcuts(pw);

    return pw;
}

static void
act_about(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ProcWindow *pw = user_data;
    NsProcView *v = current_view(pw);
    if (v)
        ns_proc_view_load(v, "about:nordstjernen");
}

static void
act_settings(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    ProcWindow *pw = user_data;
    NsProcView *v = current_view(pw);
    if (v)
        ns_proc_view_load(v, "about:settings");
}

static void
on_bookmark_activate(GtkButton *button, gpointer user_data)
{
    ProcWindow *pw = user_data;
    const char *url = g_object_get_data(G_OBJECT(button), "ns-bm-url");
    NsProcView *v = current_view(pw);
    if (url && v)
        ns_proc_view_load(v, url);
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(button),
                                             GTK_TYPE_POPOVER);
    if (pop)
        gtk_popover_popdown(GTK_POPOVER(pop));
}

static void
on_bookmark_remove(GtkButton *button, gpointer user_data)
{
    ProcWindow *pw = user_data;
    const char *url = g_object_get_data(G_OBJECT(button), "ns-bm-url");
    if (url && pw->bookmarks) {
        ns_bookmarks_remove(pw->bookmarks, url);
        GtkWidget *row = gtk_widget_get_parent(GTK_WIDGET(button));
        GtkWidget *list = row ? gtk_widget_get_parent(row) : NULL;
        if (list && row)
            gtk_box_remove(GTK_BOX(list), row);
    }
}

static void
on_add_bookmark(GtkButton *button, gpointer user_data)
{
    (void)button;
    ProcWindow *pw = user_data;
    NsProcView *v = current_view(pw);
    if (!v || !pw->bookmarks)
        return;
    const char *url = ns_proc_view_url(v);
    const char *title = ns_proc_view_title(v);
    if (url && *url && !ns_bookmarks_contains(pw->bookmarks, url)) {
        ns_bookmarks_add(pw->bookmarks, url, title);
        gtk_label_set_text(GTK_LABEL(pw->status), ns_i18n("Bookmark added"));
    }
}

static GtkWidget *
build_bookmarks_popover(ProcWindow *pw)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_size_request(box, 320, -1);

    GtkWidget *add = gtk_button_new_with_label(ns_i18n("Bookmark this page"));
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_bookmark), pw);
    gtk_box_append(GTK_BOX(box), add);
    gtk_box_append(GTK_BOX(box),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 280);
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    guint n = pw->bookmarks ? ns_bookmarks_count(pw->bookmarks) : 0;
    if (n == 0) {
        GtkWidget *empty = gtk_label_new(ns_i18n("No bookmarks yet"));
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_box_append(GTK_BOX(list), empty);
    }
    for (guint i = 0; i < n; i++) {
        const ns_bookmark *bm = ns_bookmarks_get(pw->bookmarks, i);
        if (!bm || !bm->url) continue;
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *open = gtk_button_new_with_label(
            (bm->title && *bm->title) ? bm->title : bm->url);
        gtk_button_set_has_frame(GTK_BUTTON(open), FALSE);
        gtk_widget_set_hexpand(open, TRUE);
        gtk_widget_set_halign(open, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(open, bm->url);
        g_object_set_data_full(G_OBJECT(open), "ns-bm-url",
                               g_strdup(bm->url), g_free);
        g_signal_connect(open, "clicked", G_CALLBACK(on_bookmark_activate), pw);
        GtkWidget *del = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_button_set_has_frame(GTK_BUTTON(del), FALSE);
        gtk_widget_set_tooltip_text(del, ns_i18n("Remove bookmark"));
        set_accessible_label(del, ns_i18n("Remove bookmark"));
        g_object_set_data_full(G_OBJECT(del), "ns-bm-url",
                               g_strdup(bm->url), g_free);
        g_signal_connect(del, "clicked", G_CALLBACK(on_bookmark_remove), pw);
        gtk_box_append(GTK_BOX(row), open);
        gtk_box_append(GTK_BOX(row), del);
        gtk_box_append(GTK_BOX(list), row);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_box_append(GTK_BOX(box), scroll);

    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    return pop;
}

static void
on_bookmarks_clicked(GtkButton *button, gpointer user_data)
{
    ProcWindow *pw = user_data;
    GtkWidget *pop = build_bookmarks_popover(pw);
    gtk_widget_set_parent(pop, GTK_WIDGET(button));
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
}

typedef struct {
    char    *url;
    char    *session_path;
    gboolean recover;
    gboolean private_mode;
} ProcAppCtx;

static gboolean
session_url_recoverable(const char *u)
{
    return u && (g_str_has_prefix(u, "http://") ||
                 g_str_has_prefix(u, "https://") ||
                 g_str_has_prefix(u, "ftp://") ||
                 g_str_has_prefix(u, "file://"));
}

static gboolean
write_session_cb(gpointer data)
{
    ProcWindow *pw = data;
    if (!pw->session_path)
        return G_SOURCE_REMOVE;
    GString *s = g_string_new(NULL);
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(pw->notebook));
    for (int i = 0; i < n; i++) {
        NsProcView *v = view_for_page(
            gtk_notebook_get_nth_page(GTK_NOTEBOOK(pw->notebook), i));
        if (v && ns_proc_view_is_private(v))
            continue;
        const char *u = v ? ns_proc_view_url(v) : NULL;
        if (session_url_recoverable(u)) {
            g_string_append(s, u);
            g_string_append_c(s, '\n');
        }
    }
    g_file_set_contents(pw->session_path, s->str, (gssize)s->len, NULL);
    g_string_free(s, TRUE);
    return G_SOURCE_CONTINUE;
}

static void
on_proc_activate(GtkApplication *app, gpointer user_data)
{
    ProcAppCtx *ctx = user_data;
    install_icon_search_paths();
    gtk_window_set_default_icon_name("nordstjernen");
#ifdef __APPLE__
    ns_macos_set_dock_icon();
#endif
    install_status_css();
    ProcWindow *pw = proc_window_new(app, "about:start");
    pw->session_path = g_strdup(ctx->session_path);

    gboolean opened = FALSE;
    if (ctx->recover && ctx->session_path) {
        char *contents = NULL;
        if (g_file_get_contents(ctx->session_path, &contents, NULL, NULL)) {
            char **lines = g_strsplit(contents, "\n", -1);
            for (int i = 0; lines && lines[i]; i++) {
                if (session_url_recoverable(lines[i])) {
                    proc_window_add_tab(pw, lines[i], !opened);
                    opened = TRUE;
                }
            }
            g_strfreev(lines);
        }
        g_free(contents);
        if (opened)
            gtk_label_set_text(GTK_LABEL(pw->status),
                               ns_i18n("Recovered the previous session after "
                                       "an unexpected exit"));
    }
    if (!opened)
        proc_window_add_tab_full(pw, ctx->url ? ctx->url : "about:start", TRUE,
                                 ctx->private_mode);

    if (pw->session_path)
        pw->session_timer = g_timeout_add_seconds(4, write_session_cb, pw);

    gtk_window_present(GTK_WINDOW(pw->window));
}

static void
procapp_clear_cache_dir(const char *name, gint64 min_age_s)
{
    char *dir = g_build_filename(g_get_user_cache_dir(), "nordstjernen",
                                 name, NULL);
    gint64 cutoff = g_get_real_time() / G_USEC_PER_SEC - min_age_s;
    GQueue *stack = g_queue_new();
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    g_queue_push_head(stack, g_strdup(dir));
    guint guard = 0;
    while (!g_queue_is_empty(stack) && guard++ < 100000) {
        char *d = g_queue_pop_head(stack);
        GDir *gd = g_dir_open(d, 0, NULL);
        if (gd) {
            const char *e;
            while ((e = g_dir_read_name(gd))) {
                char *child = g_build_filename(d, e, NULL);
                if (g_file_test(child, G_FILE_TEST_IS_SYMLINK) ||
                    !g_file_test(child, G_FILE_TEST_IS_DIR)) {
                    GStatBuf st;
                    if (min_age_s <= 0 ||
                        (g_lstat(child, &st) == 0 && st.st_mtime < cutoff))
                        g_unlink(child);
                    g_free(child);
                } else {
                    g_queue_push_head(stack, child);
                }
            }
            g_dir_close(gd);
        }
        g_ptr_array_add(dirs, d);
    }
    for (guint i = dirs->len; i > 1; i--)
        g_rmdir(g_ptr_array_index(dirs, i - 1));
    g_queue_free_full(stack, g_free);
    g_ptr_array_free(dirs, TRUE);
    g_free(dir);
}

static void
procapp_clear_http_caches(gboolean at_exit)
{
    static const char *const object_dirs[] = {
        "cache", "jsbc", "webfonts", "frames",
    };
    static const char *const stream_dirs[] = { "msaudio", "msvideo" };
    for (gsize i = 0; i < G_N_ELEMENTS(object_dirs); i++)
        procapp_clear_cache_dir(object_dirs[i], at_exit ? 0 : 3600);
    for (gsize i = 0; i < G_N_ELEMENTS(stream_dirs); i++)
        procapp_clear_cache_dir(stream_dirs[i], 3600);
}

int
ns_procapp_run(const char *startup_url, const char *session_path,
               gboolean recover, gboolean private_mode)
{
    if (ns_proc_video_helper_available())
        g_setenv("NS_VIDEO_HELPER", "1", TRUE);
    procapp_clear_http_caches(FALSE);
    ProcAppCtx ctx = {
        .url = g_strdup(startup_url),
        .session_path = g_strdup(session_path),
        .recover = recover,
        .private_mode = private_mode,
    };
    GtkApplication *app =
        gtk_application_new(NS_PROC_APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_proc_activate), &ctx);
    int status = g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    procapp_clear_http_caches(TRUE);
    g_free(ctx.url);
    g_free(ctx.session_path);
    return status;
}
