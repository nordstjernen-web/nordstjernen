/* Nordstjernen — find-in-page (the search bar over the rendered document). */

#include <gtk/gtk.h>

#include "find.h"
#include "layout.h"
#include "paint.h"
#include "window.h"

void
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
