/* Nordstjernen — find-in-page (the search bar over the rendered document). */

#ifndef ND_FIND_H
#define ND_FIND_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

void on_win_find            (GSimpleAction *action, GVariant *parameter,
                             gpointer user_data);
void on_search_changed      (GtkEditable *e, gpointer ud);
void on_search_activate     (GtkEntry *e, gpointer ud);
void on_search_case_toggled (GtkToggleButton *btn, gpointer ud);
void on_search_stop         (GtkSearchEntry *e, gpointer ud);
gboolean on_search_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer ud);

G_END_DECLS

#endif
