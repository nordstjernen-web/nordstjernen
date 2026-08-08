/* Nordstjernen — shared style/layout pipeline used by GUI and headless.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_RENDER_H
#define NS_RENDER_H

#include <glib.h>

#include "anim.h"
#include "css.h"
#include "dom.h"
#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

struct ns_image_cache;

typedef struct ns_render_ctx {
    ns_node                        *doc;
    const ns_css_stylesheet *const *sheets;
    guint                           n_sheets;
    double                          viewport_width;
    double                          viewport_height;
    double                          zoom;
    struct ns_image_cache          *images;
    const char                     *base_url;
    ns_anim                        *anim;
    ns_js                          *js;
    const ns_node                  *focused_input;
    const ns_node                  *hover_node;
    gsize                           caret_byte;
    gsize                           sel_anchor_byte;
    char     *(*resolve_url)(const char *href, gpointer ud);
    gboolean  (*font_allowed)(const char *abs_url, gpointer ud);
    gpointer                        cb_ud;
} ns_render_ctx;

typedef struct ns_render_profile {
    gint64 css1_us;
    gint64 style1_us;
    gint64 layout1_us;
    gint64 container_us;
    gint64 css2_us;
    gint64 style2_us;
    gint64 layout2_us;
    guint  containers;
    guint  container_passes;
    gboolean container_pass;
} ns_render_profile;

/* Whether the most recently relaid-out page's stylesheets contain any :hover
 * selector. Lets a renderer skip hover-driven restyle/repaint work on pages
 * that have no hover styling. */
gboolean ns_render_page_uses_hover(void);
gboolean ns_render_page_uses_active(void);

/* The @page rule in force for the page most recently laid out, or NULL. */
const ns_css_page_rule *ns_render_page_rule(void);

GHashTable *ns_render_relayout(const ns_render_ctx *c, ns_box **out_layout);
GHashTable *ns_render_relayout_profile(const ns_render_ctx *c,
                                       ns_box **out_layout,
                                       ns_render_profile *profile);

G_END_DECLS

#endif
