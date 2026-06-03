/* Nordstjernen — synchronous fetch/cascade/layout/capture pipeline shared by drivers.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_ENGINE_H
#define ND_ENGINE_H

#include <glib.h>

#include "anim.h"
#include "dom.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"

G_BEGIN_DECLS

nd_response *nd_engine_fetch_blocking(const char *url, const char *top_url,
                                      GError **error);

void nd_engine_collect_stylesheets(nd_node *doc, const char *base_url,
                                   GPtrArray *out, GHashTable *css_cache);

GHashTable *nd_engine_compute_cascade(nd_node *doc, const char *base_url,
                                      GHashTable *css_cache);

GHashTable *nd_engine_relayout(nd_node *doc, const char *base_url,
                               int viewport_width, double viewport_height,
                               nd_image_cache *images, nd_anim *anim,
                               nd_js *js, GHashTable *css_cache,
                               const nd_node *focused, gsize caret_byte,
                               gsize sel_anchor_byte, nd_box **out_layout);

void nd_engine_load_keyframes(nd_anim *anim, nd_node *doc, const char *base_url,
                              GHashTable *css_cache);

void nd_engine_anim_observe(nd_anim *anim, GHashTable *styles, gint64 now_us);

void nd_engine_fetch_images(nd_box *root, const char *base_url,
                            nd_image_cache *cache);

int nd_engine_write_png(const nd_box *root, const char *path);
int nd_engine_write_pdf(const nd_box *root, const char *path);

void nd_engine_dump_text(const nd_box *root, GString *out);
void nd_engine_dump_layout(const nd_box *root, int indent, GString *out);

char *nd_engine_suffix_before_ext(const char *path, const char *suffix);

G_END_DECLS

#endif
