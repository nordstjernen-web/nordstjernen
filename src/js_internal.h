/* Nordstjernen — internal JS engine declarations shared between
 * js.c and js_canvas.c. Not a public API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_JS_INTERNAL_H
#define NS_JS_INTERNAL_H

#include <glib.h>
#include <cairo.h>
#include <pango/pango.h>
#include <quickjs.h>

#include "js.h"
#include "dom.h"
#include "image.h"
#include "layout.h"

typedef struct ns_worker_host ns_worker_host;

typedef struct ns_canvas_state {
    int w, h;
    cairo_surface_t *surf;
    cairo_t         *cr;
    double fill_r, fill_g, fill_b, fill_a;
    double stroke_r, stroke_g, stroke_b, stroke_a;
    double line_width;
    char  *font;
    cairo_pattern_t *fill_pattern;
    cairo_pattern_t *stroke_pattern;
    double shadow_r, shadow_g, shadow_b, shadow_a;
    double shadow_blur, shadow_ox, shadow_oy;
} ns_canvas_state;

typedef struct ns_path2d {
    cairo_surface_t *rs;
    cairo_t         *cr;
} ns_path2d;

typedef struct ns_image_bitmap {
    cairo_surface_t *surf;
    int w, h;
} ns_image_bitmap;

typedef struct ns_perf_observer {
    JSValue   cb;
    JSValue   wrapper;
    gboolean  disconnected;
    gboolean  pinned;
    GPtrArray *entry_types;
    GPtrArray *records;
} ns_perf_observer;

struct ns_js {
    JSRuntime    *rt;
    JSContext    *ctx;
    GPtrArray    *frame_ctxs;
    GHashTable   *frame_windows;
    ns_js_log_cb  log_cb;
    gpointer      log_user_data;
    ns_js_mutated_cb mut_cb;
    gpointer      mut_user_data;
    ns_js_navigate_cb nav_cb;
    gpointer      nav_user_data;
    ns_js_download_cb download_cb;
    gpointer      download_user_data;
    ns_js_audio_cb audio_cb;
    gpointer      audio_user_data;
    ns_js_media_seek_cb media_seek_cb;
    gpointer      media_seek_user_data;
    ns_js_media_play_cb media_play_cb;
    gpointer      media_play_user_data;
    ns_js_media_muted_cb media_muted_cb;
    gpointer      media_muted_user_data;
    ns_js_mse_cb  mse_cb;
    gpointer      mse_user_data;
    ns_js_mse_buffered_cb mse_buffered_cb;
    gpointer      mse_buffered_user_data;
    ns_js_media_volume_cb media_volume_cb;
    gpointer      media_volume_user_data;
    guint         next_audio_token;
    ns_js_scroll_to_cb scroll_to_cb;
    gpointer      scroll_to_user_data;
    ns_js_form_submit_cb form_submit_cb;
    gpointer      form_submit_user_data;
    ns_js_soft_nav_cb soft_nav_cb;
    gpointer      soft_nav_user_data;
    ns_js_repaint_cb repaint_cb;
    gpointer      repaint_user_data;
    ns_js_layout_flush_cb layout_flush_cb;
    gpointer      layout_flush_user_data;
    gboolean      in_layout_flush;
    ns_js_clipboard_write_cb clipboard_write_cb;
    gpointer      clipboard_write_user_data;
    ns_js_window_action_cb window_action_cb;
    gpointer      window_action_user_data;
    JSValue       history_state;
    int           history_length;
    GPtrArray    *history_entries;
    int           history_pos;
    guint64       nav_key_seq;
    JSValue       navigation;
    char         *current_url;
    ns_node       *current_doc;
    ns_node       *current_script;
    char         *early_inject_src;
    gboolean      mutated;
    GHashTable   *timers;
    GMainContext *main_context;
    GPtrArray    *workers;
    ns_worker_host *worker_host;
    int           next_timer_id;
    int           timer_nesting_level;
    int           n_immediate_timers;
    gboolean      running_due_timers;
    GArray       *raf_pending;
    int           next_raf_id;
    gint64        raf_last_us;
    ns_node      *raf_frame_ctx;
    JSValue       pristine_promise;
    GHashTable   *style_table;
    const struct ns_box *layout_root;
    GHashTable   *box_lookup_cache;
    const void   *box_lookup_cache_root;
    const void   *box_lookup_pending_root;
    int           box_lookup_pending_count;
    const ns_node *focused_node;
    const ns_node *active_modal;
    const ns_node *focus_before_modal;
    const ns_node *pointer_lock_element;
    double         last_mouse_x[2];
    double         last_mouse_y[2];
    gboolean       has_last_mouse[2];
    GHashTable   *canvas_states;
    ns_image_cache *image_cache;
    GHashTable   *js_image_loads;
    GHashTable   *orphan_nodes;
    GPtrArray    *listeners;
    GHashTable   *pinned_wrappers_set;
    GPtrArray    *pending_fetches;
    GHashTable   *fetch_states_by_id;
    guint         next_fetch_id;
    GPtrArray    *pending_xhrs;
    GPtrArray    *pending_ws;
    GPtrArray    *pending_aborts;
    GPtrArray    *filereader_idles;
    GHashTable   *local_storage;
    GHashTable   *session_storage;
    char         *local_storage_origin;
    char         *local_storage_path;
    gboolean      local_storage_dirty;
    guint         local_storage_flush_source;
    gboolean      local_storage_disabled;
    char         *cookie_value;
    GHashTable   *session_storage_buckets;
    GHashTable   *cookie_buckets;
    char         *partition_key;
    guint64       opaque_counter;
    char         *referrer;
    int           ready_state;
    gint64        eval_deadline_us;
    gint64        js_monitor_deadline_us;
    gboolean      halted;
    gboolean      in_pump;
    gboolean      in_scroll_dispatch;
    GPtrArray    *pending_scrollend;
    gboolean      pending_scrollend_doc;
    int           eval_depth;
    GString      *document_write_buffer;
    ns_node      *document_write_script;
    gboolean      document_write_parser_open;
    GPtrArray    *deferred_script_roots;
    GPtrArray    *async_script_roots;
    guint         async_script_source;
    GPtrArray    *pending_iframe_loads;
    GHashTable   *iframe_globals;
    int           iframe_load_depth;
    GArray       *pending_storage_events;
    gboolean      storage_events_draining;
    gint64        last_pump_us;
    gint64        last_orphan_sweep_us;
    int           dispatch_depth;
    int           callback_depth;
    GPtrArray    *mutation_observers;
    gboolean      mutation_drain_scheduled;
    GPtrArray    *intersection_observers;
    GPtrArray    *media_query_lists;
    GPtrArray    *resize_observers;
    guint         observer_tick_source;
    gboolean      observer_ticking;
    guint         raf_tick_source;
    GArray       *doc_stack;
    JSValue       iframe_doc;
    int           iframe_doc_set;
    ns_csp *csp;
    char         *selection_text;
    gboolean      selection_has_range;
    double        selection_x, selection_y, selection_w, selection_h;
    int           module_load_count;
    gsize         module_load_bytes;
    gint64        module_load_deadline_us;
    gboolean      module_load_capped;
    GPtrArray    *import_map;
    gint64        time_origin_us;
    double        time_origin_real_ms;
    ns_js_navigation_timing navigation_timing;
    GPtrArray    *perf_entries;
    GPtrArray    *perf_observers;
    gboolean      perf_drain_scheduled;
    GPtrArray    *node_iters;
    GHashTable   *console_counts;
    GHashTable   *console_timers;
    GHashTable   *blob_urls;
    GHashTable   *ce_registry;
    GHashTable   *ce_pending;
    ns_node      *ce_upgrading;
    void         *ce_upgrading_wrapper;
    int           ce_in_attr_callback;
    int           ce_defer_upgrades;
    int           ce_constructing;
    int           throw_on_dynamic_markup;
    int           ignore_destructive_writes;
    int           in_error_report;
    JSValue       nodelist_decorator;
    int           nodelist_decorator_set;
    JSValue       live_html_proto;
    JSValue       live_node_proto;
    int           live_protos_set;
    JSValue       computed_style_proxy;
    int           computed_style_proxy_set;
    JSValue       url_helper;
    int           url_helper_set;
    JSValue       search_params_helper;
    int           search_params_helper_set;
    JSValue       form_data_helper;
    int           form_data_helper_set;
    JSValue       body_consumer_helper;
    int           body_consumer_helper_set;
    JSAtom        atom_capture;
    JSAtom        atom_once;
    JSAtom        atom_signal;
    JSAtom        atom_passive;
    JSAtom        atom_aborted;
    JSAtom        atom_immediate_stopped;
    JSAtom        atom_propagation_stopped;
    int           listener_atoms_set;
    guint64       dom_gen;
    JSValue       proto_node;
    JSValue       proto_element;
    JSValue       proto_htmlelement;
    JSValue       proto_svgelement;
    JSValue       proto_chardata;
    JSValue       proto_text;
    JSValue       proto_comment;
    JSValue       proto_cdata;
    JSValue       proto_pi;
    JSValue       proto_doctype;
    JSValue       proto_docfrag;
    JSValue       proto_document;
    GHashTable   *per_tag_protos;
    int           dom_protos_set;
    struct {
        const void *root;
        char        kind;
        char       *key;
        guint64     gen;
        JSValue     value;
        int         set;
    } qcache[16];
    int           qcache_next;
};

static inline ns_js *
js_from_ctx(JSContext *ctx)
{
    return ctx ? (ns_js *)JS_GetContextOpaque(ctx) : NULL;
}

typedef void (*ns_ctx_drawfn)(cairo_t *cr, void *ud);

typedef struct ns_draw_rect_ud {
    double x, y, w, h, lw;
    JSContext *ctx;
    JSValueConst this_val;
    ns_canvas_state *st;
} ns_draw_rect_ud;

typedef struct ns_draw_path_ud {
    JSContext *ctx;
    JSValueConst this_val;
    ns_canvas_state *st;
    double lw;
    cairo_path_t *snapshot;
    cairo_fill_rule_t fill_rule;
} ns_draw_path_ud;

/* Helpers defined in js.c, used by js_canvas.c */
double ns_arg_d(JSContext *ctx, JSValueConst v);
void ns_bind_fn(JSContext *ctx, JSValueConst obj, const char *name, JSCFunction *fn, int argc);
const ns_box *ns_box_find_by_dom(const ns_box *root, const ns_node *target);
uint32_t ns_js_array_length(JSContext *ctx, JSValueConst arr);
void ns_js_promise_reject(JSContext *ctx, JSValue resolvers[2], const char *message);
JSValue ns_make_element(JSContext *ctx, const ns_node *cnode);
const ns_node *ns_unwrap_element(JSValueConst val);

/* Canvas API implemented in js_canvas.c */
void
ns_path2d_finalizer(JSRuntime *rt, JSValue val);
void
ns_image_bitmap_finalizer(JSRuntime *rt, JSValue val);
void
ns_canvas_state_free(gpointer data);
JSValue
ns_image_bitmap_close(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
JSValue
ns_image_bitmap_make(JSContext *ctx, cairo_surface_t *surf, int w, int h);
cairo_surface_t *
ns_image_bitmap_from_imagedata(JSContext *ctx, JSValueConst src,
                               int *out_w, int *out_h);
cairo_surface_t *
ns_image_bitmap_crop(cairo_surface_t *src, int sw, int sh,
                     int sx, int sy, int rw, int rh);
JSValue
ns_window_create_image_bitmap(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
JSValue
ns_offscreen_transferToImageBitmap(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv);
void
ns_dommatrix_read(JSContext *ctx, JSValueConst v, double *a, double *b,
                  double *c, double *d, double *e, double *f);
void
ns_dommatrix_write(JSContext *ctx, JSValueConst obj, double a, double b,
                   double c, double d, double e, double f);
JSValue
ns_dommatrix_multiply(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
JSValue
ns_dommatrix_multiplySelf(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv);
JSValue
ns_dommatrix_translate(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv);
JSValue
ns_dommatrix_scale(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv);
JSValue
ns_dommatrix_rotate(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_dommatrix_inverse(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue
ns_dommatrix_invertSelf(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv);
void
ns_obj_double(JSContext *ctx, JSValueConst obj, const char *key, double *out);
JSValue
ns_dommatrix_transformPoint(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);
JSValue
ns_dommatrix_toString(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
JSValue
ns_dommatrix_make(JSContext *ctx, double a, double b, double c, double d,
                  double e, double f, gboolean readonly);
JSValue
ns_dommatrix_ctor_impl(JSContext *ctx, int argc, JSValueConst *argv,
                       gboolean readonly);
JSValue
ns_window_dommatrix_ctor(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv);
JSValue
ns_window_dommatrix_readonly_ctor(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);
JSValue
ns_window_offscreen_canvas_ctor(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv);
int
ns_canvas_dim_from_attr(const ns_node *el, const char *name, int defv);
gboolean
ns_canvas_parse_color(const char *s, double *r, double *g, double *b, double *a);
ns_canvas_state *
ns_canvas_state_for(ns_js *js, const ns_node *el);
ns_canvas_state *
ns_ctx_state(JSContext *ctx, JSValueConst this_val);
cairo_pattern_t *
ns_ctx_build_pattern(JSContext *ctx, JSValueConst obj);
double
ns_ctx_global_alpha(JSContext *ctx, JSValueConst this_val);
cairo_operator_t
ns_ctx_parse_composite(const char *s);
void
ns_ctx_apply_composite(JSContext *ctx, JSValueConst this_val, cairo_t *cr);
gboolean
ns_ctx_image_smoothing(JSContext *ctx, JSValueConst this_val);
void
ns_ctx_sync_styles(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st);
gboolean
ns_ctx_has_shadow(const ns_canvas_state *st);
void
ns_box_blur_argb(uint8_t *data, int w, int h, int stride, int radius);
void
ns_ctx_with_shadow(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st,
                   ns_ctx_drawfn draw, void *ud);
void
ns_ctx_set_fill_source(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st);
void
ns_ctx_set_stroke_source(JSContext *ctx, JSValueConst this_val, ns_canvas_state *st);
void
ns_draw_fillrect(cairo_t *cr, void *vud);
void
ns_draw_strokerect(cairo_t *cr, void *vud);
JSValue
ns_ctx_fillRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_strokeRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_clearRect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_beginPath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_closePath(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_moveTo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_lineTo(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_arc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
gboolean
ns_value_is_path2d(JSValueConst v);
void
ns_replay_path2d(cairo_t *target, JSValueConst path_v);
cairo_fill_rule_t
ns_parse_fill_rule(const char *s);
cairo_path_t *
ns_ctx_prepare_path_and_rule(JSContext *ctx, cairo_t *cr,
                             int argc, JSValueConst *argv);
void
ns_ctx_restore_path(cairo_t *cr, cairo_path_t *saved);
void
ns_draw_fillpath(cairo_t *cr, void *vud);
void
ns_draw_strokepath(cairo_t *cr, void *vud);
JSValue
ns_ctx_fill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_stroke(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_restore(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_translate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_scale(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_rotate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
PangoFontDescription *
ns_canvas_font_desc(const char *css_font);
gboolean
ns_ctx_direction_is_rtl(JSContext *ctx, JSValueConst this_val);
void
ns_ctx_paint_text(JSContext *ctx, JSValueConst this_val,
                  ns_canvas_state *st, const char *text,
                  double x, double y, double max_width,
                  gboolean stroke);
JSValue
ns_ctx_fillText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_measureText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue
ns_ctx_quadraticCurveTo(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv);
JSValue
ns_ctx_bezierCurveTo(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue
ns_ctx_arcTo(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv);
JSValue
ns_ctx_ellipse(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv);
JSValue
ns_ctx_clip(JSContext *ctx, JSValueConst this_val,
            int argc, JSValueConst *argv);
gboolean
ns_matrix_from_obj(JSContext *ctx, JSValueConst v, cairo_matrix_t *m);
JSValue
ns_ctx_setTransform(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_ctx_transform(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_ctx_resetTransform(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
JSValue
ns_ctx_setLineDash(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv);
JSValue
ns_ctx_getLineDash(JSContext *ctx, JSValueConst this_val,
                   int argc, JSValueConst *argv);
JSValue
ns_ctx_gradient_addColorStop(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv);
cairo_surface_t *
ns_ctx_drawimage_source(JSContext *ctx, JSValueConst src, int *out_w, int *out_h);
JSValue
ns_ctx_drawImage(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_ctx_createPattern(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue
ns_ctx_createLinearGradient(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);
JSValue
ns_ctx_createRadialGradient(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);
JSValue
ns_ctx_createConicGradient(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
JSValue
ns_image_data_make(JSContext *ctx, int w, int h, const uint8_t *rgba);
JSValue
ns_ctx_createImageData(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv);
JSValue
ns_ctx_getImageData(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_ctx_putImageData(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_ctx_strokeText(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv);
void
ns_round_rect_subpath(cairo_t *cr, double x, double y, double w, double h,
                      double rtl, double rtr, double rbr, double rbl);
gboolean
ns_extract_radii(JSContext *ctx, JSValueConst v,
                 double *rtl, double *rtr, double *rbr, double *rbl);
JSValue
ns_ctx_roundRect(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_ctx_reset(JSContext *ctx, JSValueConst this_val,
             int argc, JSValueConst *argv);
JSValue
ns_ctx_getTransform(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_ctx_isPointInPath(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue
ns_ctx_isPointInStroke(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv);
JSValue
ns_path2d_get_cr(JSContext *ctx, JSValueConst this_val, cairo_t **out);
JSValue
ns_path2d_moveTo(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_path2d_lineTo(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_path2d_closePath(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_path2d_bezierCurveTo(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv);
JSValue
ns_path2d_quadraticCurveTo(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
JSValue
ns_path2d_arc(JSContext *ctx, JSValueConst this_val,
              int argc, JSValueConst *argv);
JSValue
ns_path2d_arcTo(JSContext *ctx, JSValueConst this_val,
                int argc, JSValueConst *argv);
JSValue
ns_path2d_ellipse(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv);
JSValue
ns_path2d_rect(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv);
JSValue
ns_path2d_roundRect(JSContext *ctx, JSValueConst this_val,
                    int argc, JSValueConst *argv);
JSValue
ns_path2d_addPath(JSContext *ctx, JSValueConst this_val,
                  int argc, JSValueConst *argv);
void
ns_path2d_attach_methods(JSContext *ctx, JSValueConst obj);
const char *
ns_svg_skip_ws(const char *p);
gboolean
ns_svg_read_number(const char **pp, double *out);
void
ns_path2d_arc_svg(cairo_t *cr, double x1, double y1,
                  double rx, double ry, double phi_deg,
                  gboolean large_arc, gboolean sweep,
                  double x2, double y2);
void
ns_path2d_parse_svg(cairo_t *cr, const char *d);
JSValue
ns_path2d_ctor(JSContext *ctx, JSValueConst this_val,
               int argc, JSValueConst *argv);
JSValue
ns_ctx_get_attrs(JSContext *ctx, JSValueConst this_val,
                 int argc, JSValueConst *argv);
JSValue
ns_ctx_is_context_lost(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv);
JSValue
ns_ctx_draw_focus_if_needed(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv);
JSValue
ns_element_getContext(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv);
cairo_status_t
ns_canvas_png_write(void *closure, const unsigned char *data, unsigned int length);
JSValue
ns_offscreen_convertToBlob(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);

void ns_canvas_register_image_bitmap_class(JSRuntime *rt);
void ns_canvas_register_path2d_class(JSRuntime *rt);

/* Performance API (js_perf.c) and the js.c helpers it shares. */
void ns_bind_fn_if_not_callable(JSContext *ctx, JSValueConst obj, const char *name,
                                JSCFunction *fn, int argc);
gboolean ns_js_get_bool_prop(JSContext *ctx, JSValueConst obj, const char *key,
                             gboolean *was_set);

double ns_perf_now_ms(const ns_js *js);
void ns_perf_add_resource(ns_js *js, const char *url, const char *initiator,
                          double start_ms, double duration_ms, gint64 size);
double ns_perf_clamp_ms(gint64 delta_us);
double ns_perf_relative_ms(gint64 now_us, gint64 origin_us);
void ns_perf_entry_free(gpointer p);
JSValue ns_perf_supported_entry_types(JSContext *ctx);
JSValue ns_perf_observer_ctor(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv);
JSValue ns_perf_observer_observe(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv);
JSValue ns_perf_observer_disconnect(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv);
JSValue ns_perf_observer_takeRecords(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv);
JSValue ns_window_performance_now(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv);
JSValue ns_window_performance_mark(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv);
JSValue ns_window_performance_measure(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv);
JSValue ns_window_performance_clearMarks(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
JSValue ns_window_performance_clearMeasures(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv);
JSValue ns_window_performance_getEntries(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);
JSValue ns_window_performance_getEntriesByName(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);
JSValue ns_window_performance_getEntriesByType(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv);
JSValue ns_window_performance_memory_get(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv);

#endif
