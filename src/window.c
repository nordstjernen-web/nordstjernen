/* Nordstjernen — per-window construction (toolbar / search bar / content / status). */

#include "window.h"

#include <pango/pango.h>

void
nd_window_build_toolbar(nd_window *w, GtkWidget *header, const char *home_url)
{
    w->back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_set_tooltip_text(w->back_button, "Back");
    gtk_widget_set_sensitive(w->back_button, FALSE);
    g_signal_connect(w->back_button, "clicked", G_CALLBACK(on_back_clicked), w);

    w->forward_button = gtk_button_new_from_icon_name("go-next-symbolic");
    gtk_widget_set_tooltip_text(w->forward_button, "Forward");
    gtk_widget_set_sensitive(w->forward_button, FALSE);
    g_signal_connect(w->forward_button, "clicked", G_CALLBACK(on_forward_clicked), w);

    w->home_button = gtk_button_new_from_icon_name("go-home-symbolic");
    char *home_tip = g_strdup_printf("Home (%s)", home_url ? home_url : "");
    gtk_widget_set_tooltip_text(w->home_button, home_tip);
    g_free(home_tip);
    g_signal_connect(w->home_button, "clicked", G_CALLBACK(on_home_clicked), w);

    w->new_window_button = gtk_button_new_from_icon_name("window-new-symbolic");
    gtk_widget_set_tooltip_text(w->new_window_button, "New window (Ctrl+N)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(w->new_window_button),
                                   "app.new-window");

    w->reload_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(w->reload_button, "Reload");
    g_signal_connect(w->reload_button, "clicked", G_CALLBACK(on_reload_clicked), w);

    w->about_button = gtk_button_new_from_icon_name("help-about-symbolic");
    gtk_widget_set_tooltip_text(w->about_button, "About Nordstjernen (about:nordstjernen)");
    g_signal_connect(w->about_button, "clicked", G_CALLBACK(on_about_clicked), w);

    w->console_button = gtk_button_new_from_icon_name("utilities-terminal-symbolic");
    gtk_widget_set_tooltip_text(w->console_button, "JavaScript console (Ctrl+Shift+J)");
    g_signal_connect(w->console_button, "clicked", G_CALLBACK(on_win_open_console), w);

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
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->new_window_button);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), w->url_entry);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->about_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->console_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->bookmarks_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->view_dropdown);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->stop_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->go_button);
    gtk_header_bar_pack_end  (GTK_HEADER_BAR(header), w->bookmark_button);
}

void
nd_window_build_search_bar(nd_window *w, GtkWidget *vbox)
{
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
}

void
nd_window_build_content(nd_window *w, GtkWidget *vbox)
{
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

    GtkGesture *secondary = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
    g_signal_connect(secondary, "pressed", G_CALLBACK(nd_on_drawing_right_pressed), w);
    gtk_widget_add_controller(w->drawing_area, GTK_EVENT_CONTROLLER(secondary));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_drawing_motion), w);
    gtk_widget_add_controller(w->drawing_area, motion);

    gtk_widget_set_focusable(w->drawing_area, TRUE);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(nd_on_drawing_key_pressed), w);
    g_signal_connect(key, "key-released", G_CALLBACK(nd_on_drawing_key_released), w);
    gtk_widget_add_controller(w->drawing_area, key);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_render),
                                  w->drawing_area);
    gtk_stack_add_named(GTK_STACK(w->content_stack), scrolled_render, "render");

    gtk_stack_set_visible_child_name(GTK_STACK(w->content_stack), "text");
    gtk_box_append(GTK_BOX(vbox), w->content_stack);
}

void
nd_window_build_status_bar(nd_window *w, GtkWidget *vbox)
{
    w->status_label = gtk_label_new("Ready");
    gtk_widget_set_halign(w->status_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(w->status_label, 8);
    gtk_widget_set_margin_end(w->status_label, 8);
    gtk_widget_set_margin_top(w->status_label, 4);
    gtk_widget_set_margin_bottom(w->status_label, 4);
    gtk_label_set_ellipsize(GTK_LABEL(w->status_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(w->status_label), 0.0f);
    gtk_box_append(GTK_BOX(vbox), w->status_label);
}
