/* Nordstjernen — Cairo paint API. */

#ifndef ND_PAINT_H
#define ND_PAINT_H

#include <cairo.h>
#include <glib.h>

#include "js.h"
#include "layout.h"

G_BEGIN_DECLS

void nd_paint(cairo_t *cr, const nd_box *root, const char *highlight_query);
void nd_paint_set_js(nd_js *js);

void nd_paint_set_caret_visible(gboolean visible);

gboolean nd_paint_inline_xy_to_byte(const nd_box *b,
                                    double rel_x, double rel_y,
                                    gsize *out_byte);

G_END_DECLS

#endif
