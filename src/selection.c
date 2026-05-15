/* Nordstjernen — text selection on the rendered page. */

#include "selection.h"

#include <pango/pangocairo.h>
#include <string.h>

#include "paint.h"

void
nd_selection_clear(nd_selection *sel)
{
    if (!sel) return;
    sel->anchor_box = NULL;
    sel->focus_box  = NULL;
    sel->anchor_byte = 0;
    sel->focus_byte  = 0;
    sel->active = FALSE;
}

gboolean
nd_selection_has_range(const nd_selection *sel)
{
    if (!sel || !sel->active) return FALSE;
    if (!sel->anchor_box || !sel->focus_box) return FALSE;
    if (sel->anchor_box == sel->focus_box &&
        sel->anchor_byte == sel->focus_byte) return FALSE;
    return TRUE;
}

static gboolean
box_xy_inside(const nd_box *b, double x, double y)
{
    if (b->content_width <= 0 || b->content_height <= 0) return FALSE;
    return x >= b->x && x <= b->x + b->content_width &&
           y >= b->y && y <= b->y + b->content_height;
}

static const nd_box *
inline_at_xy_walk(const nd_box *root, double x, double y)
{
    if (!root) return NULL;
    for (const nd_box *c = root->first_child; c; c = c->next_sibling) {
        const nd_box *m = inline_at_xy_walk(c, x, y);
        if (m) return m;
    }
    if (root->kind == ND_BOX_INLINE && root->text && *root->text &&
        box_xy_inside(root, x, y))
        return root;
    return NULL;
}

static const nd_box *
nearest_inline_walk(const nd_box *root, double x, double y,
                    double *best_dist, const nd_box *best)
{
    if (!root) return best;
    if (root->kind == ND_BOX_INLINE && root->text && *root->text &&
        root->content_width > 0 && root->content_height > 0) {
        double cx = root->x + root->content_width / 2.0;
        double cy = root->y + root->content_height / 2.0;
        double dx = x - cx, dy = y - cy;
        double d2 = dx * dx + dy * dy;
        if (d2 < *best_dist) { *best_dist = d2; best = root; }
    }
    for (const nd_box *c = root->first_child; c; c = c->next_sibling)
        best = nearest_inline_walk(c, x, y, best_dist, best);
    return best;
}

static const nd_box *
find_inline_for_point(const nd_box *root, double x, double y,
                      double *out_local_x, double *out_local_y)
{
    const nd_box *hit = inline_at_xy_walk(root, x, y);
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
resolve_point(const nd_box *root, double x, double y,
              const nd_box **out_box, gsize *out_byte)
{
    double lx, ly;
    const nd_box *b = find_inline_for_point(root, x, y, &lx, &ly);
    if (!b) return FALSE;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx > b->content_width)  lx = b->content_width;
    if (ly > b->content_height) ly = b->content_height;
    gsize byte = 0;
    nd_paint_inline_xy_to_byte(b, lx, ly, &byte);
    if (b->text) {
        gsize tlen = strlen(b->text);
        if (byte > tlen) byte = tlen;
    }
    if (out_box)  *out_box = b;
    if (out_byte) *out_byte = byte;
    return TRUE;
}

gboolean
nd_selection_anchor_at(nd_selection *sel, const nd_box *root, double x, double y)
{
    if (!sel) return FALSE;
    const nd_box *b = NULL;
    gsize byte = 0;
    if (!resolve_point(root, x, y, &b, &byte)) {
        nd_selection_clear(sel);
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
nd_selection_extend_to(nd_selection *sel, const nd_box *root, double x, double y)
{
    if (!sel || !sel->active || !sel->anchor_box) return FALSE;
    const nd_box *b = NULL;
    gsize byte = 0;
    if (!resolve_point(root, x, y, &b, &byte)) return FALSE;
    sel->focus_box = b;
    sel->focus_byte = byte;
    return TRUE;
}

typedef struct find_endpoints_ctx {
    const nd_box *a;
    const nd_box *b;
    const nd_box *first;
    const nd_box *last;
    int           seen;
} find_endpoints_ctx;

static void
walk_inline_pre(const nd_box *root, void (*cb)(const nd_box *, gpointer),
                gpointer ud)
{
    if (!root) return;
    if (root->kind == ND_BOX_INLINE) cb(root, ud);
    for (const nd_box *c = root->first_child; c; c = c->next_sibling)
        walk_inline_pre(c, cb, ud);
}

static void
find_endpoints_cb(const nd_box *b, gpointer ud)
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
order_endpoints(const nd_box *root, nd_selection sel,
                const nd_box **first_box, gsize *first_byte,
                const nd_box **last_box,  gsize *last_byte)
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
    const nd_box  *first_box;
    const nd_box  *last_box;
    gsize          first_byte;
    gsize          last_byte;
    int            state;
} paint_ctx;

static void
paint_box_highlight(cairo_t *cr, const nd_box *b, gsize start_b, gsize end_b)
{
    if (!b->text) return;
    gsize tlen = strlen(b->text);
    if (start_b > tlen) start_b = tlen;
    if (end_b   > tlen) end_b   = tlen;
    if (start_b >= end_b) return;

    PangoLayout *layout = nd_paint_build_inline_layout(cr, b);
    if (!layout) return;

    int n_lines = pango_layout_get_line_count(layout);
    for (int i = 0; i < n_lines; i++) {
        PangoLayoutLine *line = pango_layout_get_line_readonly(layout, i);
        if (!line) continue;
        int line_start = line->start_index;
        int line_end   = line_start + line->length;
        int sel_s = (int)start_b > line_start ? (int)start_b : line_start;
        int sel_e = (int)end_b   < line_end   ? (int)end_b   : line_end;
        if (sel_s >= sel_e) continue;
        int x0_p = 0, x1_p = 0;
        pango_layout_line_index_to_x(line, sel_s, FALSE, &x0_p);
        pango_layout_line_index_to_x(line, sel_e, FALSE, &x1_p);
        if (x1_p < x0_p) { int t = x0_p; x0_p = x1_p; x1_p = t; }
        PangoRectangle ext;
        pango_layout_line_get_pixel_extents(line, NULL, &ext);
        double rx = b->x + (double)x0_p / PANGO_SCALE;
        double ry = b->y + (double)ext.y;
        double rw = (double)(x1_p - x0_p) / PANGO_SCALE;
        double rh = (double)ext.height;
        if (rw < 1.0) rw = 1.0;
        cairo_set_source_rgba(cr, 0.20, 0.40, 0.85, 0.30);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_fill(cr);
    }
    g_object_unref(layout);
}

static void
paint_walk_cb(const nd_box *b, gpointer ud)
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
nd_selection_paint(cairo_t *cr, const nd_box *root, const nd_selection *sel)
{
    if (!cr || !root || !nd_selection_has_range(sel)) return;
    const nd_box *fb = NULL, *lb = NULL;
    gsize fy = 0, ly = 0;
    order_endpoints(root, *sel, &fb, &fy, &lb, &ly);
    paint_ctx ctx = { cr, fb, lb, fy, ly, 0 };
    walk_inline_pre(root, paint_walk_cb, &ctx);
}

typedef struct collect_ctx {
    GString       *out;
    const nd_box  *first_box;
    const nd_box  *last_box;
    gsize          first_byte;
    gsize          last_byte;
    int            state;
} collect_ctx;

static void
collect_walk_cb(const nd_box *b, gpointer ud)
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
        g_string_append_len(ctx->out, b->text, (gssize)tlen);
        g_string_append_c(ctx->out, '\n');
    }
}

char *
nd_selection_collect_text(const nd_box *root, const nd_selection *sel)
{
    if (!root || !nd_selection_has_range(sel)) return NULL;
    const nd_box *fb = NULL, *lb = NULL;
    gsize fy = 0, ly = 0;
    order_endpoints(root, *sel, &fb, &fy, &lb, &ly);
    GString *out = g_string_new(NULL);
    collect_ctx ctx = { out, fb, lb, fy, ly, 0 };
    walk_inline_pre(root, collect_walk_cb, &ctx);
    return g_string_free(out, FALSE);
}

typedef struct edge_ctx {
    const nd_box *first;
    const nd_box *last;
} edge_ctx;

static void
edge_walk_cb(const nd_box *b, gpointer ud)
{
    edge_ctx *ctx = ud;
    if (b->kind != ND_BOX_INLINE) return;
    if (!b->text || !*b->text) return;
    if (!ctx->first) ctx->first = b;
    ctx->last = b;
}

gboolean
nd_selection_select_all(nd_selection *sel, const nd_box *root)
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
