/* Nordstjernen — embedded PDF rendering via poppler-glib.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_PDF_H
#define ND_PDF_H

#include <cairo.h>
#include <glib.h>

G_BEGIN_DECLS

typedef struct nd_pdf nd_pdf;

gboolean nd_pdf_available(void);

nd_pdf *nd_pdf_new_from_bytes(const guint8 *data, gsize len);
void    nd_pdf_free(nd_pdf *pdf);

int     nd_pdf_n_pages(const nd_pdf *pdf);

void    nd_pdf_paint(nd_pdf *pdf, cairo_t *cr, double viewport_w,
                     double *out_total_height);

G_END_DECLS

#endif
