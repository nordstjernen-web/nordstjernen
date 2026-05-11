/*
 * Nordstjernen Web Navigator
 * Copyright 2026 Andreas Røsdal
 */

#include <gtk/gtk.h>

#define ND_APP_ID  "com.nordstjernen.Browser"
#define ND_TITLE   "Nordstjernen"
#define ND_DEFAULT_W 1024
#define ND_DEFAULT_H 720

static void
on_activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), ND_TITLE);
    gtk_window_set_default_size(GTK_WINDOW(window), ND_DEFAULT_W, ND_DEFAULT_H);
    gtk_window_present(GTK_WINDOW(window));
}

int
main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new(ND_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
