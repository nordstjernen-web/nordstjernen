/* Nordstjernen — Cairo paint API. */

#ifndef ND_PAINT_H
#define ND_PAINT_H

#include <cairo.h>
#include <glib.h>

#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

struct nd_selection;
void nd_paint(cairo_t *cr, const nd_box *root, const char *highlight_query);
void nd_paint_with_selection(cairo_t *cr, const nd_box *root,
                             const char *highlight_query,
                             const struct nd_selection *sel);
void nd_paint_set_js(nd_js *js);

void nd_paint_set_caret_visible(gboolean visible);

gboolean nd_paint_inline_xy_to_byte(const nd_box *b,
                                    double rel_x, double rel_y,
                                    gsize *out_byte);

#include <pango/pangocairo.h>
PangoLayout *nd_paint_build_inline_layout(cairo_t *cr, const nd_box *b);

G_END_DECLS

#endif
