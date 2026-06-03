/* Nordstjernen — per-window construction and lifecycle.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
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

G_BEGIN_DECLS

typedef enum nd_view_mode {
    ND_VIEW_RENDER,
    ND_VIEW_RAW,
    ND_VIEW_DOM,
    ND_VIEW_LAYOUT,
} nd_view_mode;

typedef enum nd_load_stage {
    ND_STAGE_IDLE,
    ND_STAGE_CONNECTING,
    ND_STAGE_FETCHING,
    ND_STAGE_PARSING,
    ND_STAGE_STYLING,
    ND_STAGE_SCRIPTING,
    ND_STAGE_RENDERING,
    ND_STAGE_DONE,
} nd_load_stage;

typedef struct nd_window {
    guint         id;
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
    GtkWidget    *bookmarks_button;
    GtkWidget    *settings_button;
    GtkWidget    *go_button;
    GtkWidget    *stop_button;
    GtkWidget    *spinner;
    GtkWidget    *spinner_anim;
    GtkWidget    *stage_stack;
    nd_load_stage stage;
    guint         stage_done_source;
    GtkWidget    *logo_image;
    guint         logo_anim_source;
    int           logo_anim_index;
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
    gsize         sel_anchor_byte;
    GtkIMContext *im_context;
    guint         caret_blink_source;
    guint         refresh_source;
    guint         image_retry_source;
    gboolean      caret_blink_on;
    GCancellable *current_fetch;
    guint         fetch_gen;
    nd_view_mode  mode;

    GPtrArray    *history;
    int           cursor;

    char         *last_body;
    gsize         last_body_len;
    char         *last_content_type;
    gboolean      dom_mutated;
    char         *pending_fragment;
    nd_csp       *csp;

    double        zoom;

    GtkWidget    *search_revealer;
    GtkWidget    *search_entry;
    GtkWidget    *search_count_label;
    GtkWidget    *search_case_toggle;
    gboolean      search_case_sensitive;
    const nd_box *search_active_box;
    char         *search_query;

    nd_image_cache *images;
    nd_video_cache *videos;
    nd_js          *js;
    struct nd_anim *anim;

    nd_pdf       *pdf;

    nd_selection  selection;
    char         *context_menu_link;
    char         *context_menu_image;
    char         *context_menu_selection;
    char         *context_menu_media;
    gboolean      context_menu_media_is_video;
    gboolean      context_menu_media_stream;
    double        drag_start_x;
    double        drag_start_y;
    double        cursor_x;
    double        cursor_y;

    GPtrArray    *external_stylesheets;
    GHashTable   *external_css_seen;
    GCancellable *css_cancellable;
    int          css_inflight;
    gboolean     busy;
    gboolean     first_paint_done;
    gint64       last_render_us;
    guint        js_relayout_idle_id;
    gboolean     layout_dirty;
    double       last_viewport_w;

    struct {
        GtkWidget     *window;
        GtkWidget     *notebook;
        GtkTextBuffer *buffer;
        GtkWidget     *entry;
        GtkTextBuffer *profile_buffer;
        GtkWidget     *profile_start_btn;
        GtkWidget     *profile_samples_spin;
        GtkWidget     *profile_interval_spin;
        GtkWidget     *profile_progress_label;
        gboolean       profile_running;
        GtkTextBuffer *dlog_buffer;
        guint          dlog_sub_id;
    } console;
} nd_window;

void nd_window_build_toolbar     (nd_window *w, GtkWidget *header,
                                  const char *home_url);
void nd_window_update_logo_loading(nd_window *w, gboolean loading);
void nd_window_set_stage(nd_window *w, nd_load_stage stage);
GArray *nd_logo_anim_frames(void);
void nd_window_build_search_bar  (nd_window *w, GtkWidget *vbox);
void nd_window_build_content     (nd_window *w, GtkWidget *vbox);

typedef enum nd_load_source {
    ND_LOAD_USER,
    ND_LOAD_HISTORY,
} nd_load_source;

nd_window *nd_window_for_id       (guint id);
void       nd_window_ensure_js    (nd_window *w);
void       nd_window_js_mutated   (gpointer user_data);
void       nd_window_ensure_layout(nd_window *w, double viewport_width);
const char *nd_window_current_url (nd_window *w);
char       *nd_window_current_title(nd_window *w);
void        nd_window_set_status  (nd_window *w, const char *fmt, ...)
                                   G_GNUC_PRINTF(2, 3);
double      nd_layout_viewport    (void);
void        nd_window_load_url    (nd_window *w, const char *raw_url,
                                   nd_load_source src);
char       *nd_resolve_url        (const nd_window *w, const char *href);
gboolean    nd_window_media_target(nd_window *w, const struct nd_box *hit,
                                   char **out_url, gboolean *out_is_video,
                                   gboolean *out_stream);
void        nd_window_render      (nd_window *w);
void        nd_window_update_nav_state(nd_window *w);
void        nd_spawn_window       (GtkApplication *app, const char *url);
nd_window  *nd_browser_add_tab    (GtkWidget *toplevel, GtkApplication *app,
                                   const char *url);
void        nd_browser_set_active (GtkWidget *toplevel, nd_window *w);
nd_bookmarks *nd_app_bookmarks    (void);
const char   *nd_app_home_url     (void);
void          nd_app_set_home_url (const char *url);

void on_back_clicked        (GtkButton *b, gpointer ud);
void on_forward_clicked     (GtkButton *b, gpointer ud);
void on_home_clicked        (GtkButton *b, gpointer ud);
void on_reload_clicked      (GtkButton *b, gpointer ud);
void on_entry_activate      (GtkEntry  *e, gpointer ud);
void on_go_clicked          (GtkButton *b, gpointer ud);
void on_stop_clicked        (GtkButton *b, gpointer ud);
gboolean on_url_entry_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                                  guint keycode, GdkModifierType state,
                                  gpointer ud);
void on_drawing_motion      (GtkEventControllerMotion *c, double x, double y, gpointer ud);
gboolean nd_on_drawing_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud);
void nd_draw_render         (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer ud);
void nd_on_drawing_pressed  (GtkGestureClick *g, int n, double x, double y, gpointer ud);
void nd_on_drawing_pressed_middle(GtkGestureClick *g, int n, double x, double y, gpointer ud);
void nd_on_drawing_drag_begin (GtkGestureDrag *g, double x, double y, gpointer ud);
void nd_on_drawing_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer ud);
void nd_on_drawing_drag_end   (GtkGestureDrag *g, double dx, double dy, gpointer ud);
gboolean nd_on_drawing_key_pressed (GtkEventControllerKey *c, guint kv, guint kc, GdkModifierType st, gpointer ud);
void     nd_on_drawing_key_released(GtkEventControllerKey *c, guint kv, guint kc, GdkModifierType st, gpointer ud);
gboolean nd_window_raf_tick        (GtkWidget *widget, GdkFrameClock *clock, gpointer ud);
void     nd_window_render_vadjustment_changed(GtkAdjustment *adj, gpointer ud);

G_END_DECLS

#endif
