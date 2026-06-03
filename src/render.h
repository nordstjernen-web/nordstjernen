/* Nordstjernen — shared style/layout pipeline used by GUI and headless.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_RENDER_H
#define ND_RENDER_H

#include <glib.h>

#include "anim.h"
#include "css.h"
#include "dom.h"
#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

struct nd_image_cache;

typedef struct nd_render_ctx {
    nd_node                        *doc;
    const nd_css_stylesheet *const *sheets;
    guint                           n_sheets;
    double                          viewport_width;
    double                          viewport_height;
    double                          zoom;
    struct nd_image_cache          *images;
    const char                     *base_url;
    nd_anim                        *anim;
    nd_js                          *js;
    const nd_node                  *focused_input;
    gsize                           caret_byte;
    gsize                           sel_anchor_byte;
    char     *(*resolve_url)(const char *href, gpointer ud);
    gboolean  (*font_allowed)(const char *abs_url, gpointer ud);
    gpointer                        cb_ud;
} nd_render_ctx;

GHashTable *nd_render_relayout(const nd_render_ctx *c, nd_box **out_layout);

G_END_DECLS

#endif
