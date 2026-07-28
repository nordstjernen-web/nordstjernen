/* Northstar — in-engine SVG rendering API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NS_SVG_H
#define NS_SVG_H

#include <cairo.h>
#include <glib.h>

#include "dom.h"
#include "texture.h"

G_BEGIN_DECLS

struct ns_style;

typedef struct ns_svg_size {
    double   width;
    double   height;
    double   ratio;
    gboolean has_width;
    gboolean has_height;
    gboolean has_ratio;
} ns_svg_size;

gboolean ns_svg_node_is_root(const ns_node *n);

void ns_svg_intrinsic_size(const ns_node *svg, ns_svg_size *out);

void ns_svg_render_node(cairo_t          *cr,
                        const ns_node    *svg,
                        double            width,
                        double            height,
                        GHashTable       *styles,
                        const struct ns_style *inherited);

ns_texture *ns_svg_decode_bytes(const guchar *data, gsize len,
                                int *out_w, int *out_h);

gboolean ns_svg_bytes_look_like_svg(const guchar *data, gsize len);

G_END_DECLS

#endif
