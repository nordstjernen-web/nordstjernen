/* Nordstjernen — chrome dialogs: About, Settings, and the bookmarks list. */

#include <gtk/gtk.h>

#include "dialogs.h"
#include "bookmarks.h"
#include "cache.h"
#include "config.h"
#include "version.h"
#include "window.h"


typedef struct nd_about_logo {
    GtkWidget *image;
    GArray    *frames;
    int        index;
    guint      source_id;
} nd_about_logo;

static gboolean nd_about_logo_advance(gpointer user_data);

static void
nd_about_logo_show_frame(nd_about_logo *al)
{
    if (!al || !al->frames || al->frames->len == 0) return;
    nd_image_anim_frame *f =
        &g_array_index(al->frames, nd_image_anim_frame, al->index);
    if (f->texture && al->image) {
        gtk_image_set_from_paintable(GTK_IMAGE(al->image),
                                     GDK_PAINTABLE(f->texture));
    }
    int delay = f->delay_ms > 0 ? f->delay_ms : 100;
    if (delay < 20) delay = 20;
    al->source_id = g_timeout_add(delay, nd_about_logo_advance, al);
}

static gboolean
nd_about_logo_advance(gpointer user_data)
{
    nd_about_logo *al = user_data;
    al->source_id = 0;
    if (!al->frames || al->frames->len == 0 || !al->image)
        return G_SOURCE_REMOVE;
    al->index = (al->index + 1) % (int)al->frames->len;
    nd_about_logo_show_frame(al);
    return G_SOURCE_REMOVE;
}

static void
nd_about_logo_free(gpointer data)
{
    nd_about_logo *al = data;
    if (!al) return;
    if (al->source_id) {
        g_source_remove(al->source_id);
        al->source_id = 0;
    }
    al->image = NULL;
    g_free(al);
}

void
on_about_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(w->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_title(GTK_WINDOW(dlg), "About Nordstjernen");
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_widget_add_css_class(dlg, "nd-about");

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(vbox, "nd-about-content");
    gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);

    nd_about_logo *al = g_new0(nd_about_logo, 1);
    al->frames = nd_logo_anim_frames();
    al->index = 0;

    GtkWidget *logo = gtk_image_new();
    gtk_widget_set_halign(logo, GTK_ALIGN_CENTER);
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 128);
    al->image = logo;
    if (al->frames && al->frames->len > 0) {
        nd_about_logo_show_frame(al);
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(logo), "nordstjernen");
    }
    g_object_set_data_full(G_OBJECT(logo), "nd-about-logo",
                           al, nd_about_logo_free);
    gtk_box_append(GTK_BOX(vbox), logo);

    GtkWidget *name = gtk_label_new("Nordstjernen");
    gtk_widget_add_css_class(name, "nd-about-name");
    gtk_box_append(GTK_BOX(vbox), name);

    GtkWidget *version = gtk_label_new("Version " ND_VERSION);
    gtk_widget_add_css_class(version, "nd-about-version");
    gtk_box_append(GTK_BOX(vbox), version);

    if (ND_BUILD_DATE[0]) {
        GtkWidget *built = gtk_label_new("Built " ND_BUILD_DATE);
        gtk_widget_add_css_class(built, "nd-about-version");
        gtk_box_append(GTK_BOX(vbox), built);
    }

    GtkWidget *tagline = gtk_label_new("The Nordstjernen Web Navigator");
    gtk_widget_add_css_class(tagline, "nd-about-tagline");
    gtk_box_append(GTK_BOX(vbox), tagline);

    GtkWidget *copy = gtk_label_new(
        "Copyright \xc2\xa9 2026 Andreas R\xc3\xb8sdal\n"
        "Released under the Nordstjernen Source License v1.0");
    gtk_label_set_justify(GTK_LABEL(copy), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(copy, "nd-about-copy");
    gtk_box_append(GTK_BOX(vbox), copy);

    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(close_btn, 16);
    g_signal_connect_swapped(close_btn, "clicked",
                             G_CALLBACK(gtk_window_destroy), dlg);
    gtk_box_append(GTK_BOX(vbox), close_btn);

    gtk_window_set_child(GTK_WINDOW(dlg), vbox);
    gtk_window_present(GTK_WINDOW(dlg));
}

void
on_logo_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    nd_window *w = user_data;
    nd_window_load_url(w, "https://www.nordstjernen.org/", ND_LOAD_USER);
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

    nd_app_set_home_url(c->home_url);

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
    if (!url || !nd_app_bookmarks()) return;
    char *url_copy = g_strdup(url);
    nd_bookmarks_remove(nd_app_bookmarks(), url_copy);
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
    if (!nd_app_bookmarks()) return;
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
    guint count = nd_bookmarks_count(nd_app_bookmarks());
    if (count == 0) {
        GtkWidget *empty = gtk_label_new("No bookmarks yet — star a page to add one.");
        gtk_widget_set_margin_top(empty, 12);
        gtk_widget_set_margin_bottom(empty, 12);
        gtk_widget_set_margin_start(empty, 12);
        gtk_widget_set_margin_end(empty, 12);
        gtk_box_append(GTK_BOX(box), empty);
    }
    for (guint i = 0; i < count; i++) {
        const nd_bookmark *b = nd_bookmarks_get(nd_app_bookmarks(), i);
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

