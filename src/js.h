/* Nordstjernen — JavaScript engine binding (QuickJS).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_JS_H
#define ND_JS_H

#include <glib.h>

#include <cairo.h>

#include "csp.h"
#include "dom.h"

G_BEGIN_DECLS

#define ND_JS_EVAL_BUDGET_MAX_MS 60000

typedef struct nd_js nd_js;

typedef void (*nd_js_log_cb)(const char *line, gpointer user_data);
typedef void (*nd_js_mutated_cb)(gpointer user_data);
typedef void (*nd_js_navigate_cb)(const char *url, gboolean reload, gpointer user_data);
typedef void (*nd_js_scroll_to_cb)(const nd_node *target, gpointer user_data);
typedef void (*nd_js_form_submit_cb)(const nd_node *form, const nd_node *submitter,
                                     gpointer user_data);
typedef void (*nd_js_soft_nav_cb)(const char *url, gboolean replace, gpointer user_data);
typedef void (*nd_js_repaint_cb)(gpointer user_data);
typedef void (*nd_js_layout_flush_cb)(gpointer user_data);
typedef gboolean (*nd_js_clipboard_write_cb)(const char *text, gpointer user_data);

nd_js *nd_js_new(nd_js_log_cb      log_cb,  gpointer log_user_data,
                 nd_js_mutated_cb  mut_cb,  gpointer mut_user_data,
                 nd_js_navigate_cb nav_cb,  gpointer nav_user_data);

void   nd_js_set_csp(nd_js *js, const nd_csp *csp);

const char *nd_js_engine_name(void);

void   nd_js_set_scroll_to_cb(nd_js *js, nd_js_scroll_to_cb cb, gpointer user_data);
void   nd_js_set_form_submit_cb(nd_js *js, nd_js_form_submit_cb cb, gpointer user_data);
void   nd_js_set_soft_nav_cb(nd_js *js, nd_js_soft_nav_cb cb, gpointer user_data);
void   nd_js_set_repaint_cb(nd_js *js, nd_js_repaint_cb cb, gpointer user_data);
void   nd_js_set_layout_flush_cb(nd_js *js, nd_js_layout_flush_cb cb, gpointer user_data);
void   nd_js_set_clipboard_write_cb(nd_js *js, nd_js_clipboard_write_cb cb,
                                    gpointer user_data);
const char *nd_js_current_url(const nd_js *js);
const char *nd_js_storage_partition(const nd_js *js);
void   nd_js_dispatch_hashchange(nd_js *js,
                                 const char *old_url, const char *new_url);
void   nd_js_free(nd_js *js);

void     nd_js_halt(nd_js *js);
gboolean nd_js_is_halted(const nd_js *js);

void     nd_js_run_scripts_in_doc(nd_js *js, nd_node *doc, const char *base_url);

gboolean nd_js_consume_mutated(nd_js *js);

char  *nd_js_eval_source(nd_js *js, const char *src, const char *origin);

gboolean nd_js_dispatch_event(nd_js *js, const nd_node *target, const char *type,
                              gboolean *default_prevented);
gboolean nd_js_dispatch_submit_event(nd_js *js, const nd_node *form,
                                     const nd_node *submitter,
                                     gboolean *default_prevented);

void nd_js_dialog_close(nd_js *js, nd_node *dialog, const char *return_value);
gboolean nd_js_close_topmost_modal_dialog(nd_js *js);

void           nd_js_set_focus(nd_js *js, const nd_node *el);
void           nd_js_set_focused_node(nd_js *js, const nd_node *el);
const nd_node *nd_js_focused_node(const nd_js *js);
const nd_node *nd_js_sequential_focus_target(nd_js *js, gboolean backward);
gboolean       nd_node_is_focusable(const nd_node *el);
void           nd_js_refresh_top_layer(nd_js *js);

void nd_js_details_toggle_open(nd_js *js, nd_node *details, gboolean open);

gboolean nd_js_run_animation_frame(nd_js *js);

void     nd_js_set_style_table(nd_js *js, GHashTable *styles);

struct nd_box;
void     nd_js_set_layout_root(nd_js *js, const struct nd_box *root);
void     nd_js_fire_media_load_events(nd_js *js, const struct nd_box *layout);
void     nd_js_set_selection(nd_js *js, const char *text, gboolean has_range,
                             double x, double y, double w, double h);

cairo_surface_t *nd_js_canvas_surface(nd_js *js, const nd_node *n);

struct nd_image_cache;
void nd_js_set_image_cache(nd_js *js, struct nd_image_cache *cache);

gboolean nd_js_dispatch_key_event(nd_js *js, const nd_node *target,
                                  const char *type,
                                  const char *key, const char *code, int key_code,
                                  gboolean shift, gboolean ctrl,
                                  gboolean alt,   gboolean meta,
                                  gboolean *default_prevented);

G_END_DECLS

#endif
