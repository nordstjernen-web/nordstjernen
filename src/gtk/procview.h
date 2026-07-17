/* Nordstjernen — GTK view backed by the out-of-process renderer (thin client).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NORDSTJERNEN_GTK_PROCVIEW_H
#define NORDSTJERNEN_GTK_PROCVIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct NsProcView NsProcView;

const char *ns_app_self_exe(void);

typedef enum {
    NS_PROC_EVT_TITLE,
    NS_PROC_EVT_URL,
    NS_PROC_EVT_STATUS,
    NS_PROC_EVT_NEWTAB,
    NS_PROC_EVT_HISTORY,
    NS_PROC_EVT_LOADING,
    NS_PROC_EVT_DOWNLOAD,
    NS_PROC_EVT_FAVICON,
    NS_PROC_EVT_WEBGL
} NsProcEvent;

typedef void (*NsProcNotify)(NsProcView *view, NsProcEvent evt,
                             const char *text, gpointer user_data);

/* Resolved path to the renderer executable (NS_RENDERER, else discovered next
 * to the running binary). Newly allocated; free with g_free. */
char *ns_proc_renderer_path(void);

NsProcView *ns_proc_view_new(void);
GtkWidget  *ns_proc_view_widget(NsProcView *view);
void        ns_proc_view_set_notify(NsProcView *view, NsProcNotify cb,
                                    gpointer user_data);

/* Mark this tab's renderer as private/incognito. Must be set before the first
   load so the spawned renderer starts in ephemeral mode. */
void        ns_proc_view_set_private(NsProcView *view, gboolean private_mode);
gboolean    ns_proc_view_is_private(NsProcView *view);

void ns_proc_view_load(NsProcView *view, const char *url);
void ns_proc_view_back(NsProcView *view);
void ns_proc_view_forward(NsProcView *view);
void ns_proc_view_reload(NsProcView *view);
void ns_proc_view_toggle_console(NsProcView *view);

gboolean    ns_proc_view_can_back(NsProcView *view);
gboolean    ns_proc_view_can_forward(NsProcView *view);
const char *ns_proc_view_url(NsProcView *view);
const char *ns_proc_view_title(NsProcView *view);
gboolean    ns_proc_view_is_loading(NsProcView *view);

/* Connection security of the current page (ns_security in net.h:
   0 none, 1 secure, 2 invalid, 3 plain) and the server IP (owned by the view,
   or NULL). */
int         ns_proc_view_security(NsProcView *view);
const char *ns_proc_view_remote_ip(NsProcView *view);

/* The tab's current favicon as a paintable, or NULL. Owned by the view; ref it
 * if you keep it. Updated just before an NS_PROC_EVT_FAVICON notification. */
GdkPaintable *ns_proc_view_favicon(NsProcView *view);

/* Task-manager support: this tab's renderer OS pid (-1 if none), and a
 * forceful "End task" that kills the renderer (the tab respawns on next use). */
int         ns_proc_view_renderer_pid(NsProcView *view);
int         ns_proc_view_audio_pid(NsProcView *view);
int         ns_proc_view_video_pid(NsProcView *view);
void        ns_proc_view_end_task(NsProcView *view);
void        ns_proc_view_stop_video(NsProcView *view);
void        ns_proc_view_stop_audio(NsProcView *view);

void   ns_proc_view_zoom_in(NsProcView *view);
void   ns_proc_view_zoom_out(NsProcView *view);
void   ns_proc_view_zoom_reset(NsProcView *view);
double ns_proc_view_zoom(NsProcView *view);
void   ns_proc_view_focus(NsProcView *view);
void   ns_proc_view_find_open(NsProcView *view);

gboolean ns_proc_video_helper_available(void);

G_END_DECLS

#endif
