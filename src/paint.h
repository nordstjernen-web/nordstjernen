/* Nordstjernen — Cairo paint API. */

#ifndef ND_PAINT_H
#define ND_PAINT_H

#include <cairo.h>
#include <glib.h>

#include "layout.h"

G_BEGIN_DECLS

void nd_paint(cairo_t *cr, const nd_box *root, const char *highlight_query);

G_END_DECLS

#endif
