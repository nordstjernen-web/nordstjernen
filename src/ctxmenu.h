/* Nordstjernen — right-click context menu over the rendered document. */

#ifndef ND_CTXMENU_H
#define ND_CTXMENU_H

#include <gtk/gtk.h>

#include "window.h"

G_BEGIN_DECLS

void nd_install_ctx_actions(nd_window *w);
void nd_on_drawing_right_pressed(GtkGestureClick *gesture, int n_press,
                                 double x, double y, gpointer user_data);

G_END_DECLS

#endif
