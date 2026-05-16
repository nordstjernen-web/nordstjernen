/* Nordstjernen — per-window construction and lifecycle.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_WINDOW_H
#define ND_WINDOW_H

#include <gtk/gtk.h>

#include "bookmarks.h"
#include "csp.h"
#include "css.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "pdf.h"
#include "selection.h"
#include "video.h"
#include "audio.h"

G_BEGIN_DECLS

typedef enum nd_view_mode {
    ND_VIEW_RENDER,
    ND_VIEW_RAW,
    ND_VIEW_DOM,
    ND_VIEW_LAYOUT,
} nd_view_mode;

typedef struct nd_window {
    GtkWidget    *window;
    GtkWidget    *page_root;
    GtkWidget    *tab_button;
    GtkWidget    *tab_icon;
    GtkWidget    *tab_label;
    gboolean      favicon_loaded;
    GtkWidget    *url_entry;
    GtkWidget    *back_button;
    GtkWidget    *forward_button;
    GtkWidget    *home_button;
    GtkWidget    *reload_button;
    GtkWidget    *about_button;
    GtkWidget    *console_button;
    GtkWidget    *bookmark_button;
    GtkWidget    *bookmarks_button;
    GtkWidget    *go_button;
    GtkWidget    *stop_button;
    GtkWidget    *spinner;
    GtkWidget    *spinner_anim;
    GtkWidget    *content_stack;
    GtkWidget    *text_view;
    GtkWidget    *drawing_area;
    GtkAdjustment *render_vadj;
    nd_box       *layout_tree;
    GHashTable   *style_table;
    nd_node      *parsed_doc;
    nd_node      *focused_input;
    char         *focused_input_initial;
    gsize         caret_byte;
    GtkIMContext *im_context;
    guint         caret_blink_source;
    guint         refresh_source;
    guint         video_tick_source;
    gboolean      caret_blink_on;
    GtkWidget    *status_label;
    GCancellable *current_fetch;
    nd_view_mode  mode;

    GPtrArray    *history;
    int           cursor;

    char         *last_body;
    gsize         last_body_len;
    char         *last_content_type;
    char         *pending_fragment;
    nd_csp       *csp;

    double        zoom;

    GtkWidget    *search_revealer;
    GtkWidget    *search_entry;
    GtkWidget    *search_count_label;
    char         *search_query;

    nd_image_cache *images;
    nd_video_cache *videos;
    nd_audio_cache *audios;
    nd_js          *js;

    nd_pdf       *pdf;

    nd_selection  selection;
    double        drag_start_x;
    double        drag_start_y;

    GPtrArray    *external_stylesheets;
    GHashTable   *external_css_seen;
    GCancellable *css_cancellable;
    int          css_inflight;
    gboolean     first_paint_done;
    gint64       last_render_us;
    guint        js_relayout_idle_id;
    gboolean     layout_dirty;
    double       last_viewport_w;

    struct {
        GtkWidget     *window;
        GtkTextBuffer *buffer;
        GtkWidget     *entry;
    } console;
} nd_window;

void nd_window_build_toolbar     (nd_window *w, GtkWidget *header,
                                  const char *home_url);
void nd_window_build_search_bar  (nd_window *w, GtkWidget *vbox);
void nd_window_build_content     (nd_window *w, GtkWidget *vbox);
void nd_window_build_status_bar  (nd_window *w, GtkWidget *vbox);

void on_back_clicked        (GtkButton *b, gpointer ud);
void on_forward_clicked     (GtkButton *b, gpointer ud);
void on_home_clicked        (GtkButton *b, gpointer ud);
void on_reload_clicked      (GtkButton *b, gpointer ud);
void on_about_clicked       (GtkButton *b, gpointer ud);
void on_win_open_console    (GSimpleAction *a, GVariant *p, gpointer ud);
void on_bookmark_clicked    (GtkButton *b, gpointer ud);
void on_bookmarks_clicked   (GtkButton *b, gpointer ud);
void on_entry_activate      (GtkEntry  *e, gpointer ud);
void on_go_clicked          (GtkButton *b, gpointer ud);
void on_stop_clicked        (GtkButton *b, gpointer ud);
void on_search_changed      (GtkEditable *e, gpointer ud);
void on_search_activate     (GtkEntry *e, gpointer ud);
void on_drawing_motion      (GtkEventControllerMotion *c, double x, double y, gpointer ud);
void nd_draw_render         (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud);
void nd_on_drawing_pressed  (GtkGestureClick *g, int n, double x, double y, gpointer ud);
void nd_on_drawing_pressed_middle(GtkGestureClick *g, int n, double x, double y, gpointer ud);
void nd_on_drawing_right_pressed(GtkGestureClick *g, int n, double x, double y, gpointer ud);
void nd_on_drawing_drag_begin (GtkGestureDrag *g, double x, double y, gpointer ud);
void nd_on_drawing_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer ud);
void nd_on_drawing_drag_end   (GtkGestureDrag *g, double dx, double dy, gpointer ud);
gboolean nd_on_drawing_key_pressed (GtkEventControllerKey *c, guint kv, guint kc, GdkModifierType st, gpointer ud);
void     nd_on_drawing_key_released(GtkEventControllerKey *c, guint kv, guint kc, GdkModifierType st, gpointer ud);
gboolean nd_window_raf_tick        (GtkWidget *widget, GdkFrameClock *clock, gpointer ud);

G_END_DECLS

#endif
