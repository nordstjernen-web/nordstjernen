/* Nordstjernen — page export: screenshot (PNG), save (PDF/HTML), and printing. */

#ifndef ND_EXPORT_H
#define ND_EXPORT_H

#include <gtk/gtk.h>

#include "window.h"

G_BEGIN_DECLS

gboolean nd_window_screenshot_to(nd_window *w, const char *path);

void on_win_screenshot(GSimpleAction *action, GVariant *parameter, gpointer ud);
void on_win_save_pdf  (GSimpleAction *action, GVariant *parameter, gpointer ud);
void on_win_save_html (GSimpleAction *action, GVariant *parameter, gpointer ud);
void on_win_print     (GSimpleAction *action, GVariant *parameter, gpointer ud);

G_END_DECLS

#endif
