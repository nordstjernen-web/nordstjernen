/* Nordstjernen — text selection on the rendered page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_SELECTION_H
#define ND_SELECTION_H

#include <cairo.h>
#include <glib.h>

#include "layout.h"

G_BEGIN_DECLS

typedef struct nd_selection {
    const nd_box *anchor_box;
    gsize         anchor_byte;
    const nd_box *focus_box;
    gsize         focus_byte;
    gboolean      active;
} nd_selection;

void nd_selection_clear(nd_selection *sel);
gboolean nd_selection_has_range(const nd_selection *sel);

gboolean nd_selection_anchor_at(nd_selection *sel, const nd_box *root,
                                double x, double y);
gboolean nd_selection_extend_to(nd_selection *sel, const nd_box *root,
                                double x, double y);
gboolean nd_selection_select_all(nd_selection *sel, const nd_box *root);

void nd_selection_paint(cairo_t *cr, const nd_box *root,
                        const nd_selection *sel);

char *nd_selection_collect_text(const nd_box *root, const nd_selection *sel);

gboolean nd_selection_bounds(const nd_box *root, const nd_selection *sel,
                             double *out_x, double *out_y,
                             double *out_w, double *out_h);

G_END_DECLS

#endif
