/* Nordstjernen — developer console window (JS REPL, profiler, debug log). */

#ifndef ND_CONSOLE_H
#define ND_CONSOLE_H

#include <gtk/gtk.h>

#include "window.h"

G_BEGIN_DECLS

void nd_window_console_append(nd_window *w, const char *line);
void nd_window_open_console(nd_window *w);
void nd_window_console_close(nd_window *w);
void on_win_open_console(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data);

G_END_DECLS

#endif
