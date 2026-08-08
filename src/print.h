/* Nordstjernen — paginates a laid-out page onto sheets of paper.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_PRINT_H
#define NS_PRINT_H

#include <cairo.h>
#include <glib.h>

#include "css.h"
#include "layout.h"

G_BEGIN_DECLS

struct ns_browser;
struct ns_renderer_session;
struct ns_rproc_http;

typedef struct ns_print_setup {
    double width;
    double height;
    double margin_top;
    double margin_right;
    double margin_bottom;
    double margin_left;
} ns_print_setup;

void ns_print_setup_default(ns_print_setup *setup);
void ns_print_setup_apply_page_rule(ns_print_setup *setup,
                                    const ns_css_page_rule *rule);

/* Y offsets in root coordinates where each sheet starts; always at least one
   entry, the first being 0. */
GArray *ns_print_page_offsets(const ns_box *root, double page_content_height);

/* Where sheet i stops: the next sheet's offset, or a full sheet for the last. */
double ns_print_page_bottom(const GArray *offsets, guint i,
                            double page_content_height);

void ns_print_draw_page(cairo_t *cr, const ns_box *root,
                        const ns_print_setup *setup, double scale,
                        double page_top, double page_bottom);

/* Lays the page out for paper, renders one recording surface per sheet and
   restores the on-screen layout. The caller destroys every surface. */
GPtrArray *ns_browser_print_pages(struct ns_browser *browser,
                                  ns_print_setup *out_setup);

/* One cairo recording surface per sheet of paper. The caller destroys them. */
GPtrArray *ns_renderer_session_print(struct ns_renderer_session *s,
                                     ns_print_setup *out_setup);

/* Renders the open page onto sheets of paper. Only single-process mode can
   answer this: the sheets are cairo recording surfaces, which cross no
   process boundary. Returns NULL otherwise. */
typedef GPtrArray *(*ns_rproc_inproc_print_fn)(void *conn,
                                               ns_print_setup *out_setup);
void ns_rproc_http_set_inproc_print(ns_rproc_inproc_print_fn print);
GPtrArray *ns_rproc_http_print(struct ns_rproc_http *r,
                               ns_print_setup *out_setup);

G_END_DECLS

#endif
