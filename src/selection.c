/* Nordstjernen — text selection on the rendered page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "selection.h"

#include "ns_pango.h"
#include <string.h>

#include "paint.h"

void
ns_selection_clear(ns_selection *sel)
{
    if (!sel) return;
    sel->anchor_box = NULL;
    sel->focus_box  = NULL;
    sel->anchor_byte = 0;
    sel->focus_byte  = 0;
    sel->active = FALSE;
}

gboolean
ns_selection_has_range(const ns_selection *sel)
{
    if (!sel || !sel->active) return FALSE;
    if (!sel->anchor_box || !sel->focus_box) return FALSE;
    if (sel->anchor_box == sel->focus_box &&
        sel->anchor_byte == sel->focus_byte) return FALSE;
    return TRUE;
}

static gboolean
box_user_selectable(const ns_box *b)
{
    return !(b->style &&
             ns_css_keyword_is(b->style->values[NS_CSS_USER_SELECT], "none"));
}

static gboolean
box_xy_inside(const ns_box *b, double x, double y)
{
    if (b->content_width <= 0 || b->content_height <= 0) return FALSE;
    return x >= b->x && x <= b->x + b->content_width &&
           y >= b->y && y <= b->y + b->content_height;
}

static const ns_box *
inline_at_xy_walk(const ns_box *root, double x, double y)
{
    if (!root) return NULL;
    for (const ns_box *c = root->first_child; c; c = c->next_sibling) {
        const ns_box *m = inline_at_xy_walk(c, x, y);
        if (m) return m;
    }
    if (root->kind == NS_BOX_INLINE && root->text && *root->text &&
        box_user_selectable(root) && box_xy_inside(root, x, y))
        return root;
    return NULL;
}

gboolean
ns_selection_text_at(const ns_box *root, double x, double y)
{
    return inline_at_xy_walk(root, x, y) != NULL;
}

static const ns_box *
nearest_inline_walk(const ns_box *root, double x, double y,
                    double *best_dist, const ns_box *best)
{
    if (!root) return best;
    if (root->kind == NS_BOX_INLINE && root->text && *root->text &&
        box_user_selectable(root) &&
        root->content_width > 0 && root->content_height > 0) {
        double cx = root->x + root->content_width / 2.0;
        double cy = root->y + root->content_height / 2.0;
        double dx = x - cx, dy = y - cy;
        double d2 = dx * dx + dy * dy;
        if (d2 < *best_dist) { *best_dist = d2; best = root; }
    }
    for (const ns_box *c = root->first_child; c; c = c->next_sibling)
        best = nearest_inline_walk(c, x, y, best_dist, best);
    return best;
}

static const ns_box *
find_inline_for_point(const ns_box *root, double x, double y,
                      double *out_local_x, double *out_local_y)
{
    const ns_box *hit = inline_at_xy_walk(root, x, y);
    if (!hit) {
        double best = 1e18;
        hit = nearest_inline_walk(root, x, y, &best, NULL);
    }
    if (!hit) return NULL;
    if (out_local_x) *out_local_x = x - hit->x;
    if (out_local_y) *out_local_y = y - hit->y;
    return hit;
}

static gboolean
resolve_point(const ns_box *root, double x, double y,
              const ns_box **out_box, gsize *out_byte)
{
    double local_x, local_y;
    const ns_box *b = find_inline_for_point(root, x, y, &local_x, &local_y);
    if (!b) return FALSE;
    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    if (local_x > b->content_width)  local_x = b->content_width;
    if (local_y > b->content_height) local_y = b->content_height;
    gsize byte = 0;
    ns_paint_inline_xy_to_byte(b, local_x, local_y, &byte);
    if (b->text) {
        gsize tlen = strlen(b->text);
        if (byte > tlen) byte = tlen;
    }
    if (out_box)  *out_box = b;
    if (out_byte) *out_byte = byte;
    return TRUE;
}

gboolean
ns_selection_anchor_at(ns_selection *sel, const ns_box *root, double x, double y)
{
    if (!sel) return FALSE;
    const ns_box *b = NULL;
    gsize byte = 0;
    if (!resolve_point(root, x, y, &b, &byte)) {
        ns_selection_clear(sel);
        return FALSE;
    }
    sel->anchor_box = b;
    sel->anchor_byte = byte;
    sel->focus_box = b;
    sel->focus_byte = byte;
    sel->active = TRUE;
    return TRUE;
}

gboolean
ns_selection_extend_to(ns_selection *sel, const ns_box *root, double x, double y)
{
    if (!sel || !sel->active || !sel->anchor_box) return FALSE;
    const ns_box *b = NULL;
    gsize byte = 0;
    if (!resolve_point(root, x, y, &b, &byte)) return FALSE;
    sel->focus_box = b;
    sel->focus_byte = byte;
    return TRUE;
}

typedef struct find_endpoints_ctx {
    const ns_box *a;
    const ns_box *b;
    const ns_box *first;
    const ns_box *last;
    int           seen;
} find_endpoints_ctx;

static void
walk_inline_pre(const ns_box *root, void (*cb)(const ns_box *, gpointer),
                gpointer ud)
{
    if (!root) return;
    if (root->kind == NS_BOX_INLINE) cb(root, ud);
    for (const ns_box *c = root->first_child; c; c = c->next_sibling)
        walk_inline_pre(c, cb, ud);
}

static void
find_endpoints_cb(const ns_box *b, gpointer ud)
{
    find_endpoints_ctx *ctx = ud;
    if (ctx->seen == 2) return;
    if (b == ctx->a || b == ctx->b) {
        if (ctx->seen == 0) {
            ctx->first = b;
            ctx->seen = (ctx->a == ctx->b) ? 2 : 1;
            if (ctx->seen == 2) ctx->last = b;
        } else {
            ctx->last = b;
            ctx->seen = 2;
        }
    }
}

static void
order_endpoints(const ns_box *root, ns_selection sel,
                const ns_box **first_box, gsize *first_byte,
                const ns_box **last_box,  gsize *last_byte)
{
    if (sel.anchor_box == sel.focus_box) {
        *first_box = sel.anchor_box;
        *last_box  = sel.anchor_box;
        if (sel.anchor_byte <= sel.focus_byte) {
            *first_byte = sel.anchor_byte;
            *last_byte  = sel.focus_byte;
        } else {
            *first_byte = sel.focus_byte;
            *last_byte  = sel.anchor_byte;
        }
        return;
    }
    find_endpoints_ctx ctx = { sel.anchor_box, sel.focus_box, NULL, NULL, 0 };
    walk_inline_pre(root, find_endpoints_cb, &ctx);
    if (ctx.first == sel.anchor_box) {
        *first_box = sel.anchor_box;
        *first_byte = sel.anchor_byte;
        *last_box = sel.focus_box;
        *last_byte = sel.focus_byte;
    } else {
        *first_box = sel.focus_box;
        *first_byte = sel.focus_byte;
        *last_box = sel.anchor_box;
        *last_byte = sel.anchor_byte;
    }
}

typedef struct paint_ctx {
    cairo_t       *cr;
    const ns_box  *first_box;
    const ns_box  *last_box;
    gsize          first_byte;
    gsize          last_byte;
    int            state;
} paint_ctx;

static gboolean
selection_color(const ns_style *s, ns_css_prop prop,
                double *r, double *g, double *b, double *a)
{
    if (!s) return FALSE;
    const ns_css_value *v = s->values[prop];
    if (!v || v->kind != NS_CSS_V_COLOR) return FALSE;
    *r = v->u.color.r / 255.0;
    *g = v->u.color.g / 255.0;
    *b = v->u.color.b / 255.0;
    *a = v->u.color.a / 255.0;
    return TRUE;
}

static void
paint_box_highlight(cairo_t *cr, const ns_box *b, gsize start_b, gsize end_b)
{
    if (!b->text) return;
    if (!box_user_selectable(b)) return;
    gsize tlen = strlen(b->text);
    if (start_b > tlen) start_b = tlen;
    if (end_b   > tlen) end_b   = tlen;
    if (start_b >= end_b) return;

    NsPangoLayout *layout = ns_paint_build_inline_layout(cr, b);
    if (!layout) return;

    double y_offset = ns_paint_inline_y_offset_for_layout(b, layout);

    double bg_r = 0.20, bg_g = 0.40, bg_b = 0.85, bg_a = 0.30;
    const ns_style *ssel = b->style ? b->style->selection : NULL;
    selection_color(ssel, NS_CSS_BACKGROUND_COLOR, &bg_r, &bg_g, &bg_b, &bg_a);

    double fg_r, fg_g, fg_b, fg_a;
    gboolean recolor = selection_color(ssel, NS_CSS_COLOR,
                                       &fg_r, &fg_g, &fg_b, &fg_a);
    if (!recolor && ssel && bg_a >= 0.999) {
        recolor = selection_color(b->style, NS_CSS_COLOR,
                                  &fg_r, &fg_g, &fg_b, &fg_a);
        if (!recolor) { fg_r = fg_g = fg_b = 0.1; fg_a = 1.0; recolor = TRUE; }
    }

    NsPangoLayoutIter *iter = ns_pango_layout_get_iter(layout);
    do {
        NsPangoLayoutLine *line = ns_pango_layout_iter_get_line_readonly(iter);
        if (!line) continue;
        int line_start = line->start_index;
        int line_end   = line_start + line->length;
        int sel_s = (int)start_b > line_start ? (int)start_b : line_start;
        int sel_e = (int)end_b   < line_end   ? (int)end_b   : line_end;
        if (sel_s >= sel_e) continue;
        NsPangoRectangle ext;
        ns_pango_layout_iter_get_line_extents(iter, NULL, &ext);
        double ry = b->y + y_offset + (double)ext.y / NS_PANGO_SCALE;
        double rh = (double)ext.height / NS_PANGO_SCALE;
        if (rh < 1.0) rh = 1.0;
        int *ranges = NULL;
        int n_ranges = 0;
        ns_pango_layout_line_get_x_ranges(line, sel_s, sel_e, &ranges, &n_ranges);
        for (int r = 0; r < n_ranges; r++) {
            int x0_p = ranges[r * 2];
            int x1_p = ranges[r * 2 + 1];
            if (x1_p < x0_p) { int t = x0_p; x0_p = x1_p; x1_p = t; }
            double rx = b->x + (double)x0_p / NS_PANGO_SCALE;
            double rw = (double)(x1_p - x0_p) / NS_PANGO_SCALE;
            if (rw < 1.0) rw = 1.0;
            cairo_set_source_rgba(cr, bg_r, bg_g, bg_b, bg_a);
            cairo_rectangle(cr, rx, ry, rw, rh);
            cairo_fill(cr);
            if (recolor) {
                cairo_save(cr);
                cairo_rectangle(cr, rx, ry, rw, rh);
                cairo_clip(cr);
                cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, fg_a);
                cairo_move_to(cr, b->x, b->y + y_offset);
                ns_pango_cairo_show_layout(cr, layout);
                cairo_restore(cr);
            }
        }
        g_free(ranges);
    } while (ns_pango_layout_iter_next_line(iter));
    ns_pango_layout_iter_free(iter);
    g_object_unref(layout);
}

static void
paint_walk_cb(const ns_box *b, gpointer ud)
{
    paint_ctx *ctx = ud;
    if (ctx->state == 2) return;
    if (ctx->first_box == ctx->last_box) {
        if (b == ctx->first_box) {
            paint_box_highlight(ctx->cr, b, ctx->first_byte, ctx->last_byte);
            ctx->state = 2;
        }
        return;
    }
    if (ctx->state == 0) {
        if (b == ctx->first_box) {
            gsize end = b->text ? strlen(b->text) : 0;
            paint_box_highlight(ctx->cr, b, ctx->first_byte, end);
            ctx->state = 1;
        }
        return;
    }
    if (ctx->state == 1) {
        if (b == ctx->last_box) {
            paint_box_highlight(ctx->cr, b, 0, ctx->last_byte);
            ctx->state = 2;
            return;
        }
        if (b->text && *b->text) {
            paint_box_highlight(ctx->cr, b, 0, strlen(b->text));
        }
    }
}

void
ns_selection_paint(cairo_t *cr, const ns_box *root, const ns_selection *sel)
{
    if (!cr || !root || !ns_selection_has_range(sel)) return;
    const ns_box *first_box = NULL, *last_box = NULL;
    gsize first_byte = 0, last_byte = 0;
    order_endpoints(root, *sel, &first_box, &first_byte, &last_box, &last_byte);
    paint_ctx ctx = { cr, first_box, last_box, first_byte, last_byte, 0 };
    walk_inline_pre(root, paint_walk_cb, &ctx);
}

typedef struct collect_ctx {
    GString       *out;
    const ns_box  *first_box;
    const ns_box  *last_box;
    gsize          first_byte;
    gsize          last_byte;
    int            state;
} collect_ctx;

static void
collect_walk_cb(const ns_box *b, gpointer ud)
{
    collect_ctx *ctx = ud;
    if (ctx->state == 2) return;
    if (!b->text || !*b->text) {
        if (ctx->state == 1 && b == ctx->last_box) {
            ctx->state = 2;
        }
        return;
    }
    gsize tlen = strlen(b->text);
    if (ctx->first_box == ctx->last_box) {
        if (b == ctx->first_box) {
            gsize s = ctx->first_byte > tlen ? tlen : ctx->first_byte;
            gsize e = ctx->last_byte  > tlen ? tlen : ctx->last_byte;
            if (e > s) g_string_append_len(ctx->out, b->text + s, (gssize)(e - s));
            ctx->state = 2;
        }
        return;
    }
    if (ctx->state == 0) {
        if (b == ctx->first_box) {
            gsize s = ctx->first_byte > tlen ? tlen : ctx->first_byte;
            g_string_append_len(ctx->out, b->text + s, (gssize)(tlen - s));
            g_string_append_c(ctx->out, '\n');
            ctx->state = 1;
        }
        return;
    }
    if (ctx->state == 1) {
        if (b == ctx->last_box) {
            gsize e = ctx->last_byte > tlen ? tlen : ctx->last_byte;
            g_string_append_len(ctx->out, b->text, (gssize)e);
            ctx->state = 2;
            return;
        }
        if (box_user_selectable(b)) {
            g_string_append_len(ctx->out, b->text, (gssize)tlen);
            g_string_append_c(ctx->out, '\n');
        }
    }
}

char *
ns_selection_collect_text(const ns_box *root, const ns_selection *sel)
{
    if (!root || !ns_selection_has_range(sel)) return NULL;
    const ns_box *first_box = NULL, *last_box = NULL;
    gsize first_byte = 0, last_byte = 0;
    order_endpoints(root, *sel, &first_box, &first_byte, &last_box, &last_byte);
    GString *out = g_string_new(NULL);
    collect_ctx ctx = { out, first_box, last_box, first_byte, last_byte, 0 };
    walk_inline_pre(root, collect_walk_cb, &ctx);
    return g_string_free(out, FALSE);
}

typedef struct edge_ctx {
    const ns_box *first;
    const ns_box *last;
} edge_ctx;

static void
edge_walk_cb(const ns_box *b, gpointer ud)
{
    edge_ctx *ctx = ud;
    if (b->kind != NS_BOX_INLINE) return;
    if (!b->text || !*b->text) return;
    if (!box_user_selectable(b)) return;
    if (!ctx->first) ctx->first = b;
    ctx->last = b;
}

gboolean
ns_selection_select_all(ns_selection *sel, const ns_box *root)
{
    if (!sel || !root) return FALSE;
    edge_ctx ec = { NULL, NULL };
    walk_inline_pre(root, edge_walk_cb, &ec);
    if (!ec.first || !ec.last) return FALSE;
    sel->anchor_box = ec.first;
    sel->anchor_byte = 0;
    sel->focus_box = ec.last;
    sel->focus_byte = ec.last->text ? strlen(ec.last->text) : 0;
    sel->active = TRUE;
    return TRUE;
}

