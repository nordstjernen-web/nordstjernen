/* Nordstjernen — paginates a laid-out page onto sheets of paper.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "print.h"

#include <string.h>

#include "paint.h"

#define NS_PRINT_A4_WIDTH  (210.0 * 96.0 / 25.4)
#define NS_PRINT_A4_HEIGHT (297.0 * 96.0 / 25.4)
#define NS_PRINT_MARGIN    (0.5 * 96.0)

typedef struct { double top, bottom; } ns_print_span;

void
ns_print_setup_default(ns_print_setup *setup)
{
    setup->width = NS_PRINT_A4_WIDTH;
    setup->height = NS_PRINT_A4_HEIGHT;
    setup->margin_top = NS_PRINT_MARGIN;
    setup->margin_right = NS_PRINT_MARGIN;
    setup->margin_bottom = NS_PRINT_MARGIN;
    setup->margin_left = NS_PRINT_MARGIN;
}

void
ns_print_setup_apply_page_rule(ns_print_setup *setup,
                               const ns_css_page_rule *rule)
{
    if (!rule) return;
    if (rule->has_size && rule->width > 1 && rule->height > 1) {
        setup->width = rule->width;
        setup->height = rule->height;
    }
    if (rule->has_margin[0]) setup->margin_top = rule->margin[0];
    if (rule->has_margin[1]) setup->margin_right = rule->margin[1];
    if (rule->has_margin[2]) setup->margin_bottom = rule->margin[2];
    if (rule->has_margin[3]) setup->margin_left = rule->margin[3];
}

static const char *
break_keyword(const ns_box *b, ns_css_prop prop)
{
    return b->style ? ns_style_keyword(b->style, prop) : NULL;
}

static gboolean
forces_page_break(const char *kw)
{
    return kw && (strcmp(kw, "page") == 0 || strcmp(kw, "always") == 0 ||
                  strcmp(kw, "all") == 0 || strcmp(kw, "left") == 0 ||
                  strcmp(kw, "right") == 0 || strcmp(kw, "recto") == 0 ||
                  strcmp(kw, "verso") == 0);
}

static gboolean
avoids_page_break(const char *kw)
{
    return kw && (strcmp(kw, "avoid") == 0 || strcmp(kw, "avoid-page") == 0);
}

static void
box_page_extent(const ns_box *b, double *top, double *bottom)
{
    *top = b->y + b->margin.top;
    *bottom = *top + b->content_height + b->padding.top + b->padding.bottom +
              b->border.top + b->border.bottom;
}

static void
add_span(GArray *spans, double top, double bottom, double page_h)
{
    if (!(bottom > top) || bottom - top > page_h) return;
    ns_print_span s = { top, bottom };
    g_array_append_val(spans, s);
}

static void
add_text_line_spans(GArray *spans, const ns_box *b, double top, double bottom,
                    double page_h)
{
    double line_h = b->style ? ns_paint_css_line_height_px(b->style) : 0;
    double height = bottom - top;
    if (!(line_h > 1) || height <= line_h * 1.5) {
        add_span(spans, top, bottom, page_h);
        return;
    }
    for (double y = top; y < bottom - 0.5; y += line_h)
        add_span(spans, y, MIN(y + line_h, bottom), page_h);
}

static void
collect_breaks(const ns_box *b, GArray *forced, GArray *spans, double page_h)
{
    if (!b) return;
    double top, bottom;
    box_page_extent(b, &top, &bottom);

    if (b->parent) {
        if (forces_page_break(break_keyword(b, NS_CSS_BREAK_BEFORE)))
            g_array_append_val(forced, top);
        if (forces_page_break(break_keyword(b, NS_CSS_BREAK_AFTER)))
            g_array_append_val(forced, bottom);
        if (avoids_page_break(break_keyword(b, NS_CSS_BREAK_INSIDE))) {
            add_span(spans, top, bottom, page_h);
            return;
        }
    }

    if (!b->first_child) {
        if (b->kind == NS_BOX_INLINE && b->text && *b->text)
            add_text_line_spans(spans, b, top, bottom, page_h);
        else
            add_span(spans, top, bottom, page_h);
        return;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        collect_breaks(c, forced, spans, page_h);
}

static int
cmp_double(gconstpointer a, gconstpointer b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static double
next_forced_break(const GArray *forced, double after, double limit)
{
    for (guint i = 0; i < forced->len; i++) {
        double f = g_array_index(forced, double, i);
        if (f > after + 0.5 && f <= limit + 0.5) return f;
    }
    return -1;
}

static double
pull_above_spans(const GArray *spans, double start, double limit)
{
    double cut = limit;
    for (int pass = 0; pass < 8; pass++) {
        double moved = cut;
        for (guint i = 0; i < spans->len; i++) {
            const ns_print_span *s = &g_array_index(spans, ns_print_span, i);
            if (s->top > start + 0.5 && s->top < moved - 0.5 &&
                s->bottom > moved + 0.5)
                moved = s->top;
        }
        if (moved >= cut - 0.01) break;
        cut = moved;
    }
    return cut > start + 1 ? cut : limit;
}

GArray *
ns_print_page_offsets(const ns_box *root, double page_content_height)
{
    GArray *offsets = g_array_new(FALSE, FALSE, sizeof(double));
    double zero = 0;
    g_array_append_val(offsets, zero);
    if (!root || !(page_content_height > 1)) return offsets;

    GArray *forced = g_array_new(FALSE, FALSE, sizeof(double));
    GArray *spans = g_array_new(FALSE, FALSE, sizeof(ns_print_span));
    collect_breaks(root, forced, spans, page_content_height);
    g_array_sort(forced, cmp_double);

    double doc_bottom = ns_box_max_bottom(root, 0);
    double cur = 0;
    while (cur + page_content_height < doc_bottom - 0.5 &&
           offsets->len < 4096) {
        double limit = cur + page_content_height;
        double cut = next_forced_break(forced, cur, limit);
        if (cut < 0) cut = pull_above_spans(spans, cur, limit);
        if (cut <= cur + 1) cut = limit;
        g_array_append_val(offsets, cut);
        cur = cut;
    }

    g_array_free(forced, TRUE);
    g_array_free(spans, TRUE);
    return offsets;
}

double
ns_print_page_bottom(const GArray *offsets, guint i, double page_content_height)
{
    if (!offsets || i >= offsets->len) return 0;
    if (i + 1 < offsets->len) return g_array_index(offsets, double, i + 1);
    return g_array_index(offsets, double, i) + page_content_height;
}

void
ns_print_draw_page(cairo_t *cr, const ns_box *root,
                   const ns_print_setup *setup, double scale,
                   double page_top, double page_bottom)
{
    if (!cr || !root || !setup) return;
    double content_w = setup->width - setup->margin_left - setup->margin_right;
    double content_h = page_bottom - page_top;
    if (!(content_w > 0) || !(content_h > 0)) return;
    cairo_save(cr);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, setup->margin_left, setup->margin_top);
    cairo_rectangle(cr, 0, 0, content_w, content_h);
    cairo_clip(cr);
    cairo_translate(cr, 0, -page_top);
    ns_paint(cr, root, NULL);
    cairo_restore(cr);
}
