/* Nordstjernen — per-window construction (toolbar / search bar / content / status).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "window.h"

#include <pango/pango.h>

#include "ctxmenu.h"
#include "debuglog.h"
#include "dialogs.h"
#include "find.h"

extern const char *nd_app_self_exe(void);

static void
nd_window_log_renderer(GtkWidget *widget, gpointer user_data)
{
    (void)user_data;
    GtkNative *native = gtk_widget_get_native(widget);
    GskRenderer *renderer = native ? gtk_native_get_renderer(native) : NULL;
    const char *requested = g_getenv("GSK_RENDERER");
    nd_debug_log_emit(ND_DLOG_RENDER, "gsk",
        "active renderer %s (GSK_RENDERER=%s)",
        renderer ? G_OBJECT_TYPE_NAME(renderer) : "(none)",
        requested && *requested ? requested : "auto");
}

static GtkWidget *
make_toolbar_button(const char *icon, const char *tooltip,
                    GCallback handler, nd_window *w)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tooltip);
    if (handler) g_signal_connect(b, "clicked", handler, w);
    return b;
}

static char *
nd_logo_read_first(const char *const *rel_paths, gsize *out_len)
{
    const char *exe = nd_app_self_exe();
    char *exe_dir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    char *contents = NULL;
    gsize len = 0;
    for (int i = 0; rel_paths[i]; i++) {
        char *path = g_build_filename(exe_dir, rel_paths[i], NULL);
        gboolean ok = g_file_get_contents(path, &contents, &len, NULL);
        g_free(path);
        if (ok) break;
    }
    g_free(exe_dir);
    if (out_len) *out_len = contents ? len : 0;
    return contents;
}

GArray *
nd_logo_anim_frames(void)
{
    static GArray *cached = NULL;
    static gboolean tried = FALSE;
    if (tried) return cached;
    tried = TRUE;

    static const char *const gif_paths[] = {
        "share/icons/hicolor/scalable/apps/nordstjernen.gif",
        "../share/icons/hicolor/scalable/apps/nordstjernen.gif",
        "../../data/icons/hicolor/scalable/apps/nordstjernen.gif",
        "data/icons/hicolor/scalable/apps/nordstjernen.gif",
        NULL,
    };
    gsize gif_len = 0;
    char *gif = nd_logo_read_first(gif_paths, &gif_len);
    if (!gif) return NULL;

    GArray *frames = nd_image_decode_wuffs_anim(
        (const guchar *)gif, gif_len, NULL, NULL);
    g_free(gif);
    if (!frames) return NULL;
    if (frames->len < 2) {
        for (guint i = 0; i < frames->len; i++) {
            nd_image_anim_frame *f =
                &g_array_index(frames, nd_image_anim_frame, i);
            if (f->texture) g_object_unref(f->texture);
        }
        g_array_free(frames, TRUE);
        return NULL;
    }
    cached = frames;
    return cached;
}

static gboolean nd_logo_anim_tick(gpointer user_data);

static void
nd_logo_anim_schedule_next(nd_window *w, int frame_index)
{
    GArray *frames = nd_logo_anim_frames();
    if (!frames || frames->len == 0) return;
    nd_image_anim_frame *f =
        &g_array_index(frames, nd_image_anim_frame, frame_index);
    int delay = f->delay_ms > 0 ? f->delay_ms : 100;
    if (delay < 20) delay = 20;
    w->logo_anim_source = g_timeout_add(delay, nd_logo_anim_tick, w);
}

static gboolean
nd_logo_anim_tick(gpointer user_data)
{
    nd_window *w = user_data;
    w->logo_anim_source = 0;
    GArray *frames = nd_logo_anim_frames();
    if (!frames || frames->len == 0 || !w->logo_image)
        return G_SOURCE_REMOVE;
    w->logo_anim_index = (w->logo_anim_index + 1) % (int)frames->len;
    nd_image_anim_frame *f =
        &g_array_index(frames, nd_image_anim_frame, w->logo_anim_index);
    if (f->texture) {
        gtk_image_set_from_paintable(GTK_IMAGE(w->logo_image),
                                     GDK_PAINTABLE(f->texture));
        gtk_image_set_pixel_size(GTK_IMAGE(w->logo_image), 26);
    }
    nd_logo_anim_schedule_next(w, w->logo_anim_index);
    return G_SOURCE_REMOVE;
}

void
nd_window_update_logo_loading(nd_window *w, gboolean loading)
{
    if (!w || !w->logo_image) return;
    GArray *frames = loading ? nd_logo_anim_frames() : NULL;
    if (loading && frames && frames->len > 0) {
        if (w->logo_anim_source) return;
        w->logo_anim_index = 0;
        nd_image_anim_frame *f0 =
            &g_array_index(frames, nd_image_anim_frame, 0);
        if (f0->texture) {
            gtk_image_set_from_paintable(GTK_IMAGE(w->logo_image),
                                         GDK_PAINTABLE(f0->texture));
            gtk_image_set_pixel_size(GTK_IMAGE(w->logo_image), 26);
        }
        nd_logo_anim_schedule_next(w, 0);
    } else {
        if (w->logo_anim_source) {
            g_source_remove(w->logo_anim_source);
            w->logo_anim_source = 0;
        }
        w->logo_anim_index = 0;
        gtk_image_set_from_icon_name(GTK_IMAGE(w->logo_image), "nordstjernen");
        gtk_image_set_pixel_size(GTK_IMAGE(w->logo_image), 26);
    }
}

static const char *
nd_stage_page_name(nd_load_stage s)
{
    switch (s) {
    case ND_STAGE_CONNECTING: return "connecting";
    case ND_STAGE_FETCHING:   return "fetching";
    case ND_STAGE_PARSING:    return "parsing";
    case ND_STAGE_STYLING:    return "rendering";
    case ND_STAGE_SCRIPTING:  return "scripting";
    case ND_STAGE_RENDERING:  return "rendering";
    case ND_STAGE_DONE:       return "done";
    default:                  return "fetching";
    }
}

static const char *
nd_stage_tooltip(nd_load_stage s)
{
    switch (s) {
    case ND_STAGE_CONNECTING:
        return "Connecting — resolving the host and opening the connection…";
    case ND_STAGE_FETCHING:
        return "Fetching — downloading the page from the server…";
    case ND_STAGE_PARSING:
        return "Parsing — building the document tree (HTML → DOM)…";
    case ND_STAGE_STYLING:
        return "Styling — matching CSS rules and resolving the cascade…";
    case ND_STAGE_SCRIPTING:
        return "Running scripts — executing the page's JavaScript…";
    case ND_STAGE_RENDERING:
        return "Rendering — laying out and painting the page…";
    case ND_STAGE_DONE:
        return "Done — the page finished loading and rendering.";
    default:
        return "Idle";
    }
}

static gboolean
nd_stage_done_revert(gpointer user_data)
{
    nd_window *w = user_data;
    w->stage_done_source = 0;
    nd_window_set_stage(w, ND_STAGE_IDLE);
    return G_SOURCE_REMOVE;
}

void
nd_window_set_stage(nd_window *w, nd_load_stage stage)
{
    if (!w || !w->spinner || !GTK_IS_STACK(w->spinner)) return;
    if (w->stage_done_source) {
        g_source_remove(w->stage_done_source);
        w->stage_done_source = 0;
    }
    w->stage = stage;

    if (stage == ND_STAGE_IDLE) {
        if (w->spinner_anim && GTK_IS_SPINNER(w->spinner_anim))
            gtk_spinner_set_spinning(GTK_SPINNER(w->spinner_anim), FALSE);
        gtk_stack_set_visible_child_name(GTK_STACK(w->spinner), "idle");
        gtk_widget_set_tooltip_text(w->spinner, "Idle");
        return;
    }

    if (w->stage_stack && GTK_IS_STACK(w->stage_stack))
        gtk_stack_set_visible_child_name(GTK_STACK(w->stage_stack),
                                         nd_stage_page_name(stage));
    if (w->spinner_anim && GTK_IS_SPINNER(w->spinner_anim))
        gtk_spinner_set_spinning(GTK_SPINNER(w->spinner_anim),
                                 stage != ND_STAGE_DONE);
    gtk_stack_set_visible_child_name(GTK_STACK(w->spinner), "busy");
    gtk_widget_set_tooltip_text(w->spinner, nd_stage_tooltip(stage));

    if (stage == ND_STAGE_DONE)
        w->stage_done_source = g_timeout_add(1100, nd_stage_done_revert, w);
}

void
nd_window_build_toolbar(nd_window *w, GtkWidget *toolbar, const char *home_url)
{
    w->back_button = make_toolbar_button("nordstjernen-back", "Back",
                                         G_CALLBACK(on_back_clicked), w);
    gtk_widget_set_sensitive(w->back_button, FALSE);

    w->forward_button = make_toolbar_button("nordstjernen-forward", "Forward",
                                            G_CALLBACK(on_forward_clicked), w);
    gtk_widget_set_sensitive(w->forward_button, FALSE);

    g_autofree char *home_tip = g_strdup_printf("Home (%s)",
                                                 home_url ? home_url : "");
    w->home_button = make_toolbar_button("nordstjernen-home", home_tip,
                                         G_CALLBACK(on_home_clicked), w);

    w->reload_button = make_toolbar_button("nordstjernen-reload", "Reload",
                                           G_CALLBACK(on_reload_clicked), w);

    w->about_button = make_toolbar_button("nordstjernen-about",
        "About Nordstjernen", G_CALLBACK(on_about_clicked), w);

#ifdef __APPLE__
    const char *console_tip = "JavaScript console (\xe2\x8c\x98\xe2\x87\xa7J)";
#else
    const char *console_tip = "JavaScript console (Ctrl+Shift+J)";
#endif
    w->console_button = make_toolbar_button("nordstjernen-console",
                                            console_tip, NULL, w);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(w->console_button),
                                   "win.open-console");

    w->bookmarks_button = make_toolbar_button("nordstjernen-bookmarks",
        "Show bookmarks", G_CALLBACK(on_bookmarks_clicked), w);

    w->settings_button = make_toolbar_button("nordstjernen-settings",
        "Settings", G_CALLBACK(on_settings_clicked), w);

    w->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->url_entry),
                                   "Enter URL or search term");
    gtk_entry_set_input_purpose(GTK_ENTRY(w->url_entry),
                                GTK_INPUT_PURPOSE_URL);
    gtk_entry_set_input_hints(GTK_ENTRY(w->url_entry),
                              GTK_INPUT_HINT_NO_SPELLCHECK |
                              GTK_INPUT_HINT_NO_EMOJI);
    gtk_accessible_update_property(GTK_ACCESSIBLE(w->url_entry),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Address bar",
        GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
            "URL or search term; press Enter to load.",
        -1);
    gtk_widget_set_hexpand(w->url_entry, TRUE);
    gtk_widget_set_halign(w->url_entry, GTK_ALIGN_FILL);
    gtk_widget_set_size_request(w->url_entry, 200, -1);
    g_signal_connect(w->url_entry, "activate", G_CALLBACK(on_entry_activate), w);
    GtkEventController *url_keys = gtk_event_controller_key_new();
    g_signal_connect(url_keys, "key-pressed",
                     G_CALLBACK(on_url_entry_key_pressed), w);
    gtk_widget_add_controller(w->url_entry, url_keys);

    w->go_button = make_toolbar_button("nordstjernen-go",
        "Load the URL in the address bar",
        G_CALLBACK(on_go_clicked), w);

    w->stop_button = make_toolbar_button("nordstjernen-stop", "Stop loading",
                                         G_CALLBACK(on_stop_clicked), w);
    gtk_widget_set_sensitive(w->stop_button, FALSE);

    GtkWidget *busy_indicator = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(busy_indicator),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(busy_indicator), 150);
    GtkWidget *idle_placeholder = gtk_label_new("");

    GtkWidget *busy_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *busy_spinner = gtk_spinner_new();
    GtkWidget *stage_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stage_stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(stage_stack), 180);
    static const struct { const char *page, *icon; } stage_pages[] = {
        { "connecting", "nordstjernen-stage-connect" },
        { "fetching",   "nordstjernen-stage-fetch" },
        { "parsing",    "nordstjernen-stage-parse" },
        { "scripting",  "nordstjernen-stage-script" },
        { "rendering",  "nordstjernen-stage-render" },
        { "done",       "nordstjernen-stage-done" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(stage_pages); i++) {
        GtkWidget *img = gtk_image_new_from_icon_name(stage_pages[i].icon);
        gtk_image_set_pixel_size(GTK_IMAGE(img), 18);
        gtk_stack_add_named(GTK_STACK(stage_stack), img, stage_pages[i].page);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(stage_stack), "fetching");
    gtk_box_append(GTK_BOX(busy_box), busy_spinner);
    gtk_box_append(GTK_BOX(busy_box), stage_stack);

    gtk_stack_add_named(GTK_STACK(busy_indicator), idle_placeholder, "idle");
    gtk_stack_add_named(GTK_STACK(busy_indicator), busy_box, "busy");
    gtk_stack_set_visible_child_name(GTK_STACK(busy_indicator), "idle");
    gtk_widget_set_tooltip_text(busy_indicator, "Idle");
    w->spinner = busy_indicator;
    w->spinner_anim = busy_spinner;
    w->stage_stack = stage_stack;
    w->stage = ND_STAGE_IDLE;

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

    GtkWidget *logo_image = gtk_image_new_from_icon_name("nordstjernen");
    gtk_image_set_pixel_size(GTK_IMAGE(logo_image), 26);
    w->logo_image = logo_image;
    GtkWidget *logo_button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(logo_button), logo_image);
    gtk_button_set_has_frame(GTK_BUTTON(logo_button), FALSE);
    gtk_widget_set_tooltip_text(logo_button,
        "Nordstjernen (https://www.nordstjernen.org/)");
    gtk_widget_set_margin_start(logo_button, 4);
    gtk_widget_set_margin_end(logo_button, 2);
    g_signal_connect(logo_button, "clicked", G_CALLBACK(on_logo_clicked), w);
    gtk_box_append(GTK_BOX(toolbar), logo_button);
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
    gtk_accessible_update_property(GTK_ACCESSIBLE(w->search_count_label),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Search matches",
        -1);
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
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(w->text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(w->text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(w->text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(w->text_view), 8);
    gtk_accessible_update_property(GTK_ACCESSIBLE(w->text_view),
        GTK_ACCESSIBLE_PROPERTY_LABEL, "Page source",
        GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
            "Raw text or HTML source of the loaded document.",
        -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_text), w->text_view);
    gtk_stack_add_named(GTK_STACK(w->content_stack), scrolled_text, "text");

    GtkWidget *scrolled_render = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_render),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    w->render_vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(scrolled_render));
    g_signal_connect(w->render_vadj, "value-changed",
                     G_CALLBACK(nd_window_render_vadjustment_changed), w);
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
    g_signal_connect(w->drawing_area, "realize",
                     G_CALLBACK(nd_window_log_renderer), NULL);
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

    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(nd_on_drawing_scroll), w);
    gtk_widget_add_controller(w->drawing_area, scroll);

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
