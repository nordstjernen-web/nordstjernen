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

nd_js *nd_js_new(nd_js_log_cb      log_cb,  gpointer log_user_data,
                 nd_js_mutated_cb  mut_cb,  gpointer mut_user_data,
                 nd_js_navigate_cb nav_cb,  gpointer nav_user_data);

void   nd_js_set_csp(nd_js *js, const nd_csp *csp);

void   nd_js_set_scroll_to_cb(nd_js *js, nd_js_scroll_to_cb cb, gpointer user_data);
void   nd_js_set_form_submit_cb(nd_js *js, nd_js_form_submit_cb cb, gpointer user_data);
void   nd_js_set_soft_nav_cb(nd_js *js, nd_js_soft_nav_cb cb, gpointer user_data);
void   nd_js_update_current_url(nd_js *js, const char *new_url);
const char *nd_js_current_url(const nd_js *js);
void   nd_js_dispatch_popstate(nd_js *js);
void   nd_js_free(nd_js *js);

void     nd_js_run_scripts_in_doc(nd_js *js, nd_node *doc, const char *base_url);
gboolean nd_js_consume_mutated(nd_js *js);

char  *nd_js_eval_source(nd_js *js, const char *src, const char *origin);

gboolean nd_js_dispatch_event(nd_js *js, const nd_node *target, const char *type,
                              gboolean *default_prevented);

gboolean nd_js_run_animation_frame(nd_js *js);

void     nd_js_set_style_table(nd_js *js, GHashTable *styles);

struct nd_box;
void     nd_js_set_layout_root(nd_js *js, const struct nd_box *root);
void     nd_js_set_selection(nd_js *js, const char *text, gboolean has_range,
                             double x, double y, double w, double h);

cairo_surface_t *nd_js_canvas_surface(nd_js *js, const nd_node *n);

gboolean nd_js_dispatch_key_event(nd_js *js, const nd_node *target,
                                  const char *type,
                                  const char *key, const char *code, int key_code,
                                  gboolean shift, gboolean ctrl,
                                  gboolean alt,   gboolean meta,
                                  gboolean *default_prevented);

G_END_DECLS

#endif
