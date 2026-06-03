/* Nordstjernen — Cairo paint API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_PAINT_H
#define ND_PAINT_H

#include <cairo.h>
#include <glib.h>
#include <pango/pangocairo.h>

#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

struct nd_selection;
struct nd_anim;
void nd_paint(cairo_t *cr, const nd_box *root, const char *highlight_query);
void nd_paint_with_selection(cairo_t *cr, const nd_box *root,
                             const char *highlight_query,
                             const struct nd_selection *sel);
void nd_paint_set_js(nd_js *js);
void nd_paint_set_anim(struct nd_anim *anim);

void nd_paint_set_caret_visible(gboolean visible);

void nd_paint_set_search(gboolean case_sensitive, const nd_box *active);

gboolean nd_paint_inline_xy_to_byte(const nd_box *b,
                                    double rel_x, double rel_y,
                                    gsize *out_byte);
double nd_paint_inline_y_offset_for_layout(const nd_box *b,
                                           PangoLayout *layout);

PangoLayout *nd_paint_build_inline_layout(cairo_t *cr, const nd_box *b);

void nd_paint_register_font_oracle(void);

void nd_paint_apply_inline_font(PangoLayout *layout, const nd_style *style);

void nd_paint_apply_i18n(PangoLayout *layout, PangoAttrList *attrs,
                         const nd_box *box);

PangoWrapMode nd_paint_wrap_mode_for(const nd_style *style);

gboolean nd_paint_li_is_inside(const nd_style *li_style);
gboolean nd_paint_li_marker_text(const nd_node *li, const nd_style *li_style,
                                 char *out, gsize out_sz);

G_END_DECLS

#endif
