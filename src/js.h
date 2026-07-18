/* Nordstjernen — JavaScript engine binding (QuickJS).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_JS_H
#define NS_JS_H

#include <glib.h>

#include <cairo.h>

#include "csp.h"
#include "dom.h"

G_BEGIN_DECLS

#define NS_JS_EVAL_BUDGET_MAX_MS 60000

typedef struct ns_js ns_js;
typedef struct ns_js_drag_session ns_js_drag_session;

typedef void (*ns_js_log_cb)(const char *line, gpointer user_data);
typedef void (*ns_js_mutated_cb)(gpointer user_data);
typedef void (*ns_js_navigate_cb)(const char *url, gboolean reload, gpointer user_data);
typedef void (*ns_js_download_cb)(const char *url, const char *filename, gpointer user_data);
typedef void (*ns_js_audio_cb)(const char *command, gpointer user_data);
typedef gboolean (*ns_js_media_seek_cb)(const void *node, double seconds,
                                        gpointer user_data);
typedef void (*ns_js_media_play_cb)(const void *node, gboolean play,
                                    gpointer user_data);
typedef void (*ns_js_media_muted_cb)(const void *node, gboolean muted,
                                     gpointer user_data);
typedef gboolean (*ns_js_mse_cb)(guint stream_id, char kind,
                                 const guint8 *data, gsize len, gboolean eos,
                                 gpointer user_data);
typedef double (*ns_js_mse_buffered_cb)(guint stream_id, char kind,
                                        gpointer user_data);
typedef void (*ns_js_media_volume_cb)(const void *node, double volume,
                                      gpointer user_data);
typedef void (*ns_js_scroll_to_cb)(const ns_node *target, gpointer user_data);
typedef void (*ns_js_form_submit_cb)(const ns_node *form, const ns_node *submitter,
                                     gpointer user_data);
typedef void (*ns_js_soft_nav_cb)(const char *url, gboolean replace, gpointer user_data);
typedef void (*ns_js_repaint_cb)(gpointer user_data);
typedef void (*ns_js_layout_flush_cb)(gpointer user_data);
typedef gboolean (*ns_js_clipboard_write_cb)(const char *text, gpointer user_data);
typedef void (*ns_js_window_action_cb)(const char *action, gpointer user_data);

typedef struct {
    gint64 origin_us;
    double origin_real_ms;
    double domain_lookup_start_ms;
    double domain_lookup_end_ms;
    double connect_start_ms;
    double connect_end_ms;
    double secure_connection_start_ms;
    double request_start_ms;
    double response_start_ms;
    double response_end_ms;
    double dom_loading_ms;
    double dom_interactive_ms;
    double dom_content_loaded_event_start_ms;
    double dom_content_loaded_event_end_ms;
    double dom_complete_ms;
    double load_event_start_ms;
    double load_event_end_ms;
} ns_js_navigation_timing;

ns_js *ns_js_new(ns_js_log_cb      log_cb,  gpointer log_user_data,
                 ns_js_mutated_cb  mut_cb,  gpointer mut_user_data,
                 ns_js_navigate_cb nav_cb,  gpointer nav_user_data,
                 const ns_js_navigation_timing *navigation_timing);



void   ns_js_set_form_submit_cb(ns_js *js, ns_js_form_submit_cb cb, gpointer user_data);
void   ns_js_set_download_cb(ns_js *js, ns_js_download_cb cb, gpointer user_data);
void   ns_js_set_audio_cb(ns_js *js, ns_js_audio_cb cb, gpointer user_data);
void   ns_js_set_media_seek_cb(ns_js *js, ns_js_media_seek_cb cb,
                               gpointer user_data);
void   ns_js_set_media_play_cb(ns_js *js, ns_js_media_play_cb cb,
                               gpointer user_data);
void   ns_js_set_media_muted_cb(ns_js *js, ns_js_media_muted_cb cb,
                                gpointer user_data);
void   ns_js_set_mse_cb(ns_js *js, ns_js_mse_cb cb, gpointer user_data);
void   ns_js_set_mse_buffered_cb(ns_js *js, ns_js_mse_buffered_cb cb,
                                 gpointer user_data);
void   ns_js_set_media_volume_cb(ns_js *js, ns_js_media_volume_cb cb,
                                 gpointer user_data);
void   ns_js_video_event(ns_js *js, const void *node, const char *kind, double value);
void   ns_js_set_layout_flush_cb(ns_js *js, ns_js_layout_flush_cb cb, gpointer user_data);
void   ns_js_set_early_inject_src(ns_js *js, const char *src);
void   ns_js_add_csp_header(ns_js *js, const char *header_value);
gboolean ns_js_csp_form_action_allowed(const ns_js *js, const char *action_url);
const char *ns_js_current_url(const ns_js *js);
const char *ns_js_storage_partition(const ns_js *js);
void   ns_js_dispatch_hashchange(ns_js *js,
                                 const char *old_url, const char *new_url);
void   ns_js_free(ns_js *js);

gboolean ns_js_in_pump(const ns_js *js);

void     ns_js_run_scripts_in_doc(ns_js *js, ns_node *doc, const char *base_url);

gboolean ns_js_consume_mutated(ns_js *js);

char  *ns_js_eval_source(ns_js *js, const char *src, const char *origin);

gboolean ns_js_dispatch_event(ns_js *js, const ns_node *target, const char *type,
                              gboolean *default_prevented);
gboolean ns_js_click_activate(ns_js *js, const ns_node *node);
gboolean ns_js_node_has_click_handler(ns_js *js, const ns_node *target);
gboolean ns_js_select_choose_option(ns_js *js, ns_node *option);
gboolean ns_js_select_toggle_option(ns_js *js, ns_node *option);
gboolean ns_js_select_step(ns_js *js, ns_node *select, int dir);
gboolean ns_js_select_edge(ns_js *js, ns_node *select, gboolean last);
gboolean ns_js_select_typeahead(ns_js *js, ns_node *select, const char *key);
void     ns_js_activate_element(ns_js *js, const ns_node *el);
gboolean ns_js_dispatch_submit_event(ns_js *js, const ns_node *form,
                                     const ns_node *submitter,
                                     gboolean *default_prevented);
void     ns_js_form_reset(ns_js *js, ns_node *form);
gboolean ns_js_activate_summary(ns_js *js, const ns_node *el);

void ns_js_dialog_close(ns_js *js, ns_node *dialog, const char *return_value);

void           ns_js_set_focus(ns_js *js, const ns_node *el);
void           ns_js_set_focused_node(ns_js *js, const ns_node *el);
const ns_node *ns_js_focused_node(const ns_js *js);
const ns_node *ns_js_sequential_focus_target(ns_js *js, gboolean backward);
gboolean       ns_node_is_focusable(const ns_node *el);
void           ns_js_refresh_top_layer(ns_js *js);

void ns_js_details_toggle_open(ns_js *js, ns_node *details, gboolean open);

gboolean ns_js_run_animation_frame(ns_js *js);

gboolean ns_js_has_pending_animation_frame(const ns_js *js);
gboolean ns_js_has_pending_work(const ns_js *js);

void ns_js_dump_stats(ns_js *js, GString *out);

typedef struct ns_anim ns_anim;
void ns_js_dispatch_anim_events(ns_js *js, ns_anim *anim);

void     ns_js_set_style_table(ns_js *js, GHashTable *styles);
void     ns_js_sync_window_metrics(ns_js *js);
void     ns_js_dispatch_resize(ns_js *js);
void     ns_js_note_viewport_scroll(ns_js *js, double x, double y);

struct ns_box;
void     ns_js_set_layout_root(ns_js *js, const struct ns_box *root);
void     ns_js_fire_media_load_events(ns_js *js, const struct ns_box *layout);
void     ns_js_fire_page_transition(ns_js *js, const char *type,
                                    gboolean persisted);

cairo_surface_t *ns_js_canvas_surface(ns_js *js, const ns_node *n);

void ns_js_request_repaint(ns_js *js);

struct ns_image_cache;
struct ns_image;
void ns_js_set_image_cache(ns_js *js, struct ns_image_cache *cache);
const struct ns_image *ns_js_image_for_node(ns_js *js, const ns_node *el);

gboolean ns_js_dispatch_key_event(ns_js *js, const ns_node *target,
                                  const char *type,
                                  const char *key, const char *code, int key_code,
                                  gboolean shift, gboolean ctrl,
                                  gboolean alt,   gboolean meta,
                                  gboolean *default_prevented);
gboolean ns_js_dispatch_key_event_full(ns_js *js, const ns_node *target,
                                       const char *type,
                                       const char *key, const char *code,
                                       int key_code, int char_code,
                                       gboolean shift, gboolean ctrl,
                                       gboolean alt,   gboolean meta,
                                       gboolean *default_prevented);

gboolean ns_js_dispatch_mouse_event(ns_js *js, const ns_node *target,
                                    const char *type,
                                    double client_x, double client_y,
                                    double page_x, double page_y,
                                    int button, int buttons,
                                    gboolean shift, gboolean ctrl,
                                    gboolean alt,   gboolean meta,
                                    const ns_node *related,
                                    gboolean *default_prevented);

ns_js_drag_session *ns_js_drag_session_new(ns_js *js);
void                ns_js_drag_session_free(ns_js_drag_session *session);
void                ns_js_drag_session_set_data(ns_js_drag_session *session,
                                                const char *type,
                                                const char *data);
void                ns_js_drag_session_add_file(ns_js_drag_session *session,
                                                const char *path);
gboolean            ns_js_dispatch_drag_event(ns_js *js, ns_js_drag_session *session,
                                              const ns_node *target,
                                              const char *type,
                                              double client_x, double client_y,
                                              double page_x, double page_y,
                                              int button, int buttons,
                                              gboolean shift, gboolean ctrl,
                                              gboolean alt, gboolean meta,
                                              const ns_node *related,
                                              gboolean *default_prevented);


G_END_DECLS

#endif
