/* Nordstjernen — text selection on the rendered page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_SELECTION_H
#define NS_SELECTION_H

#include <cairo.h>
#include <glib.h>

#include "layout.h"

G_BEGIN_DECLS

typedef struct ns_selection {
    const ns_box *anchor_box;
    gsize         anchor_byte;
    const ns_box *focus_box;
    gsize         focus_byte;
    gboolean      active;
} ns_selection;

void ns_selection_clear(ns_selection *sel);
gboolean ns_selection_has_range(const ns_selection *sel);

gboolean ns_selection_text_at(const ns_box *root, double x, double y);

gboolean ns_selection_anchor_at(ns_selection *sel, const ns_box *root,
                                double x, double y);
gboolean ns_selection_extend_to(ns_selection *sel, const ns_box *root,
                                double x, double y);
gboolean ns_selection_select_all(ns_selection *sel, const ns_box *root);
gboolean ns_selection_select_word_at(ns_selection *sel, const ns_box *root,
                                     double x, double y);
gboolean ns_selection_select_block_at(ns_selection *sel, const ns_box *root,
                                      double x, double y);

typedef struct ns_selection_run {
    gsize start;
    gsize end;
} ns_selection_run;

GHashTable *ns_selection_ranges(const ns_box *root, const ns_selection *sel);

gboolean ns_selection_bounds(const ns_box *root, const ns_selection *sel,
                             double *out_x, double *out_y,
                             double *out_w, double *out_h);

char *ns_selection_collect_text(const ns_box *root, const ns_selection *sel);


G_END_DECLS

#endif
