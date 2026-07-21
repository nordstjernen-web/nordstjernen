/* Nordstjernen — synchronous fetch/cascade/layout/capture pipeline shared by drivers.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_ENGINE_H
#define NS_ENGINE_H

#include <glib.h>

#include "anim.h"
#include "dom.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"

G_BEGIN_DECLS

ns_response *ns_engine_fetch_blocking(const char *url, const char *top_url,
                                      GError **error);

ns_response *ns_engine_navigate_blocking(const char *url, const char *top_url,
                                         GError **error);

gboolean ns_engine_in_blocking_fetch(void);

ns_response *ns_engine_post_blocking(const char *url, const char *top_url,
                                     const void *body, gsize body_len,
                                     const char *content_type, GError **error);

ns_response *ns_engine_navigate_post_blocking(
    const char *url, const char *top_url,
    const void *body, gsize body_len,
    const char *content_type, GError **error);

void ns_engine_collect_stylesheets(ns_node *doc, const char *base_url,
                                   GPtrArray *out, GHashTable *css_cache);

void ns_engine_speculative_preload(ns_node *doc, const char *base_url,
                                   gboolean include_images);

GHashTable *ns_engine_compute_cascade(ns_node *doc, const char *base_url,
                                      GHashTable *css_cache);

GHashTable *ns_engine_relayout(ns_node *doc, const char *base_url,
                               int viewport_width, double viewport_height,
                               ns_image_cache *images, ns_anim *anim,
                               ns_js *js, GHashTable *css_cache,
                               const ns_node *focused, const ns_node *hover,
                               gsize caret_byte,
                               gsize sel_anchor_byte, ns_box **out_layout);

void ns_engine_layout_perf(guint64 *relayouts, double *total_ms);

void ns_engine_load_keyframes(ns_anim *anim, ns_node *doc, const char *base_url,
                              GHashTable *css_cache);

void ns_engine_anim_observe(ns_anim *anim, GHashTable *styles, gint64 now_us);

void ns_engine_fetch_images(ns_box *root, const char *base_url,
                            ns_image_cache *cache);

typedef struct ns_engine_img_session ns_engine_img_session;

ns_engine_img_session *ns_engine_fetch_images_start(
    ns_box *root, const char *base_url, ns_image_cache *cache,
    GHashTable *requested, double scroll_y, double viewport_h,
    gboolean *deferred_any,
    void (*arrived_cb)(gpointer user_data), gpointer user_data);
int  ns_engine_img_session_outstanding(const ns_engine_img_session *s);
void ns_engine_img_session_close(ns_engine_img_session *s);

int ns_engine_write_png(const ns_box *root, const char *path);
int ns_engine_write_pdf(const ns_box *root, const char *path);

void ns_engine_dump_text(const ns_box *root, GString *out);
void ns_engine_dump_layout(const ns_box *root, int indent, GString *out);

char *ns_engine_suffix_before_ext(const char *path, const char *suffix);

G_END_DECLS

#endif
