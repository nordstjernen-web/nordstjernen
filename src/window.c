/* Nordstjernen — per-window construction (toolbar / search bar / content / status).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "window.h"

#include <pango/pango.h>

static GtkWidget *
make_toolbar_button(const char *icon, const char *tooltip,
                    GCallback handler, nd_window *w)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tooltip);
    if (handler) g_signal_connect(b, "clicked", handler, w);
    return b;
}

void
nd_window_build_toolbar(nd_window *w, GtkWidget *toolbar, const char *home_url)
{
    w->back_button = make_toolbar_button("go-previous", "Back",
                                         G_CALLBACK(on_back_clicked), w);
    gtk_widget_set_sensitive(w->back_button, FALSE);

    w->forward_button = make_toolbar_button("go-next", "Forward",
                                            G_CALLBACK(on_forward_clicked), w);
    gtk_widget_set_sensitive(w->forward_button, FALSE);

    g_autofree char *home_tip = g_strdup_printf("Home (%s)",
                                                 home_url ? home_url : "");
    w->home_button = make_toolbar_button("go-home", home_tip,
                                         G_CALLBACK(on_home_clicked), w);

    w->reload_button = make_toolbar_button("view-refresh", "Reload",
                                           G_CALLBACK(on_reload_clicked), w);

    w->about_button = make_toolbar_button("help-about",
        "About Nordstjernen (about:nordstjernen)",
        G_CALLBACK(on_about_clicked), w);

#ifdef __APPLE__
    const char *console_tip = "JavaScript console (\xe2\x8c\x98\xe2\x87\xa7J)";
#else
    const char *console_tip = "JavaScript console (Ctrl+Shift+J)";
#endif
    w->console_button = make_toolbar_button("utilities-terminal",
                                            console_tip, NULL, w);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(w->console_button),
                                   "win.open-console");

    w->bookmarks_button = make_toolbar_button("user-bookmarks",
        "Show bookmarks", G_CALLBACK(on_bookmarks_clicked), w);

    w->settings_button = make_toolbar_button("preferences-system",
        "Settings", G_CALLBACK(on_settings_clicked), w);

    w->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->url_entry),
                                   "Enter URL or search term");
    gtk_widget_set_hexpand(w->url_entry, TRUE);
    gtk_widget_set_halign(w->url_entry, GTK_ALIGN_FILL);
    gtk_widget_set_size_request(w->url_entry, 200, -1);
    g_signal_connect(w->url_entry, "activate", G_CALLBACK(on_entry_activate), w);

    w->go_button = make_toolbar_button("pan-end-symbolic",
        "Load the URL in the address bar",
        G_CALLBACK(on_go_clicked), w);

    w->stop_button = make_toolbar_button("process-stop", "Stop loading",
                                         G_CALLBACK(on_stop_clicked), w);
    gtk_widget_set_sensitive(w->stop_button, FALSE);

    GtkWidget *busy_indicator = gtk_stack_new();
    GtkWidget *idle_placeholder = gtk_label_new("");
    GtkWidget *busy_spinner = gtk_spinner_new();
    gtk_stack_add_named(GTK_STACK(busy_indicator), idle_placeholder, "idle");
    gtk_stack_add_named(GTK_STACK(busy_indicator), busy_spinner,     "busy");
    gtk_stack_set_visible_child_name(GTK_STACK(busy_indicator), "idle");
    gtk_widget_set_tooltip_text(busy_indicator, "Idle");
    w->spinner = busy_indicator;
    w->spinner_anim = busy_spinner;

    gtk_box_append(GTK_BOX(toolbar), w->back_button);
    gtk_box_append(GTK_BOX(toolbar), w->forward_button);
    gtk_box_append(GTK_BOX(toolbar), w->reload_button);
    gtk_box_append(GTK_BOX(toolbar), w->stop_button);
    gtk_box_append(GTK_BOX(toolbar), w->home_button);
    gtk_box_append(GTK_BOX(toolbar), w->url_entry);
    gtk_box_append(GTK_BOX(toolbar), w->go_button);
    gtk_box_append(GTK_BOX(toolbar), w->spinner);
    gtk_box_append(GTK_BOX(toolbar), w->bookmarks_button);
    gtk_box_append(GTK_BOX(toolbar), w->console_button);
    gtk_box_append(GTK_BOX(toolbar), w->settings_button);
    gtk_box_append(GTK_BOX(toolbar), w->about_button);

    GtkWidget *logo = gtk_image_new_from_icon_name("nordstjernen");
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 26);
    gtk_widget_set_tooltip_text(logo, "Nordstjernen");
    gtk_widget_set_margin_start(logo, 4);
    gtk_widget_set_margin_end(logo, 2);
    gtk_box_append(GTK_BOX(toolbar), logo);
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
    g_signal_connect(w->search_entry, "stop-search",
                     G_CALLBACK(on_search_stop), w);
    GtkEventController *search_keys = gtk_event_controller_key_new();
    g_signal_connect(search_keys, "key-pressed",
                     G_CALLBACK(on_search_key_pressed), w);
    gtk_widget_add_controller(w->search_entry, search_keys);
    GtkWidget *search_label = gtk_label_new("Find:");
    w->search_count_label = gtk_label_new("");
    gtk_widget_add_css_class(w->search_count_label, "dim-label");
    gtk_widget_set_margin_start(w->search_count_label, 8);
    w->search_case_toggle = gtk_toggle_button_new_with_label("Aa");
    gtk_widget_set_tooltip_text(w->search_case_toggle, "Match case");
    gtk_widget_add_css_class(w->search_case_toggle, "flat");
    g_signal_connect(w->search_case_toggle, "toggled",
                     G_CALLBACK(on_search_case_toggled), w);
    gtk_box_append(GTK_BOX(search_box), search_label);
    gtk_box_append(GTK_BOX(search_box), w->search_entry);
    gtk_box_append(GTK_BOX(search_box), w->search_case_toggle);
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
    gtk_accessible_update_property(GTK_ACCESSIBLE(w->drawing_area),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Web page contents",
        GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
            "Rendered web page; use Tab to focus inputs, "
            "Ctrl+L to focus the address bar.",
        -1);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->drawing_area),
                                   nd_draw_render, w, NULL);
    gtk_widget_add_tick_callback(w->drawing_area, nd_window_raf_tick, w, NULL);
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

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(nd_on_drawing_drag_begin),  w);
    g_signal_connect(drag, "drag-update", G_CALLBACK(nd_on_drawing_drag_update), w);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(nd_on_drawing_drag_end),    w);
    gtk_widget_add_controller(w->drawing_area, GTK_EVENT_CONTROLLER(drag));

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
