/* Nordstjernen — Cairo paint API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_PAINT_H
#define NS_PAINT_H

#include <cairo.h>
#include <glib.h>
#include <pango/pangocairo.h>

#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

struct ns_selection;
struct ns_anim;
typedef struct ns_paint_stats {
    guint boxes_seen;
    guint hidden;
    guint skipped_top;
    guint culled_bounds;
    guint offscreen;
    guint grouped;
    guint overflow_clips;
    guint sorted_parents;
    guint sorted_children;
    guint blocks;
    guint inlines;
    guint images;
    guint videos;
    guint canvases;
} ns_paint_stats;

void ns_paint(cairo_t *cr, const ns_box *root, const char *highlight_query);
void ns_paint_with_selection(cairo_t *cr, const ns_box *root,
                             const char *highlight_query,
                             const struct ns_selection *sel);
void ns_paint_set_js(ns_js *js);
void ns_paint_set_anim(struct ns_anim *anim);

void ns_paint_3d_invalidate(void);
gboolean ns_paint_3d_registered(const ns_box *b);
const ns_box *ns_paint_3d_pick(const ns_box *root3d, double x, double y);


void ns_paint_set_search(gboolean case_sensitive, const ns_box *active);

gboolean ns_paint_inline_range_extents(const ns_box *b, gsize start, gsize len,
                                       double *out_x, double *out_y,
                                       double *out_w, double *out_h);
gboolean ns_paint_inline_xy_to_byte(const ns_box *b,
                                    double rel_x, double rel_y,
                                    gsize *out_byte);
double ns_paint_inline_y_offset_for_layout(const ns_box *b,
                                           PangoLayout *layout);

PangoLayout *ns_paint_build_inline_layout(cairo_t *cr, const ns_box *b);

void ns_paint_register_font_oracle(void);

void ns_paint_apply_inline_font(PangoLayout *layout, const ns_style *style);

void ns_paint_apply_i18n(PangoLayout *layout, PangoAttrList *attrs,
                         const ns_box *box);
void ns_paint_apply_font_features(PangoAttrList *attrs, const ns_style *style,
                                  guint start, guint end);
PangoAttribute *ns_paint_font_features_attr_from_values(int kerning,
                                                        const char *ligatures,
                                                        const char *settings);
PangoAttribute *ns_paint_font_variations_attr_from_values(const char *settings);

PangoWrapMode ns_paint_wrap_mode_for(const ns_style *style);

double ns_paint_css_line_height_px(const ns_style *style);
void ns_paint_apply_css_line_spacing(PangoLayout *layout,
                                     const ns_style *style);

gboolean ns_paint_li_is_inside(const ns_style *li_style);
gboolean ns_paint_li_marker_text(const ns_node *li, const ns_style *li_style,
                                 char *out, gsize out_sz);

G_END_DECLS

#endif
