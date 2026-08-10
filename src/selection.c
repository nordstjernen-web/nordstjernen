/* Nordstjernen — text selection on the rendered page.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "selection.h"

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
    for (const ns_box *p = b; p; p = p->parent) {
        if (!p->style) continue;
        const ns_css_value *v = p->style->values[NS_CSS_USER_SELECT];
        if (!v) continue;
        return !ns_css_keyword_is(v, "none");
    }
    return TRUE;
}

static const ns_box *
containing_block(const ns_box *b)
{
    const ns_box *p = b ? b->parent : NULL;
    while (p && p->kind == NS_BOX_INLINE) p = p->parent;
    return p;
}

static gboolean
box_xy_inside(const ns_box *b, double x, double y)
{
    if (b->content_width <= 0 || b->content_height <= 0) return FALSE;
    return x >= b->x && x <= b->x + b->content_width &&
           y >= b->y && y <= b->y + b->content_height;
}

static const ns_box *
inline_at_xy_walk(const ns_box *root, double x, double y,
                  double *out_x, double *out_y)
{
    if (!root) return NULL;
    if (ns_box_clips_out_point(root, x, y)) return NULL;
    double cx = x + root->scroll_x;
    double cy = y + root->scroll_y;
    for (const ns_box *c = root->first_child; c; c = c->next_sibling) {
        const ns_box *m = inline_at_xy_walk(c, cx, cy, out_x, out_y);
        if (m) return m;
    }
    if (root->kind == NS_BOX_INLINE && root->text && *root->text &&
        box_user_selectable(root) && box_xy_inside(root, x, y)) {
        if (out_x) *out_x = x;
        if (out_y) *out_y = y;
        return root;
    }
    return NULL;
}

gboolean
ns_selection_text_at(const ns_box *root, double x, double y)
{
    return inline_at_xy_walk(root, x, y, NULL, NULL) != NULL;
}

typedef struct nearest_ctx {
    const ns_box *best;
    double        best_x, best_y;
    double        best_gap_y;
    double        best_gap_x;
} nearest_ctx;

static void
nearest_consider(nearest_ctx *n, const ns_box *b, double x, double y)
{
    double top = b->y, bottom = b->y + b->content_height;
    double left = b->x, right = b->x + b->content_width;
    double gap_y = y < top ? top - y : (y > bottom ? y - bottom : 0);
    double gap_x = x < left ? left - x : (x > right ? x - right : 0);
    if (!n->best || gap_y < n->best_gap_y - 0.01 ||
        (gap_y <= n->best_gap_y + 0.01 && gap_x < n->best_gap_x)) {
        n->best = b;
        n->best_x = x;
        n->best_y = y;
        n->best_gap_y = gap_y;
        n->best_gap_x = gap_x;
    }
}

static void
nearest_inline_walk(const ns_box *root, double x, double y, nearest_ctx *n)
{
    if (!root) return;
    if (root->kind == NS_BOX_INLINE && root->text && *root->text &&
        box_user_selectable(root) &&
        root->content_width > 0 && root->content_height > 0)
        nearest_consider(n, root, x, y);
    if (ns_box_clips_out_point(root, x, y)) return;
    double cx = x + root->scroll_x;
    double cy = y + root->scroll_y;
    for (const ns_box *c = root->first_child; c; c = c->next_sibling)
        nearest_inline_walk(c, cx, cy, n);
}

static const ns_box *
find_inline_for_point(const ns_box *root, double x, double y,
                      double *out_local_x, double *out_local_y)
{
    double hx = x, hy = y;
    const ns_box *hit = inline_at_xy_walk(root, x, y, &hx, &hy);
    if (!hit) {
        nearest_ctx n = { NULL, 0, 0, 0, 0 };
        nearest_inline_walk(root, x, y, &n);
        hit = n.best;
        hx = n.best_x;
        hy = n.best_y;
    }
    if (!hit) return NULL;
    if (out_local_x) *out_local_x = hx - hit->x;
    if (out_local_y) *out_local_y = hy - hit->y;
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

gboolean
ns_selection_select_word_at(ns_selection *sel, const ns_box *root,
                            double x, double y)
{
    if (!sel) return FALSE;
    const ns_box *b = NULL;
    gsize byte = 0;
    if (!resolve_point(root, x, y, &b, &byte)) return FALSE;
    gsize start = 0, end = 0;
    if (!ns_paint_inline_word_range(b, byte, &start, &end)) return FALSE;
    sel->anchor_box = b;
    sel->anchor_byte = start;
    sel->focus_box = b;
    sel->focus_byte = end;
    sel->active = TRUE;
    return TRUE;
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

static void
walk_inline_pre(const ns_box *root, void (*cb)(const ns_box *, gpointer),
                gpointer ud)
{
    if (!root) return;
    if (root->kind == NS_BOX_INLINE) cb(root, ud);
    for (const ns_box *c = root->first_child; c; c = c->next_sibling)
        walk_inline_pre(c, cb, ud);
}

gboolean
ns_selection_select_block_at(ns_selection *sel, const ns_box *root,
                             double x, double y)
{
    if (!sel) return FALSE;
    const ns_box *b = NULL;
    gsize byte = 0;
    if (!resolve_point(root, x, y, &b, &byte)) return FALSE;
    const ns_box *block = containing_block(b);
    if (!block) block = b;
    edge_ctx ec = { NULL, NULL };
    walk_inline_pre(block, edge_walk_cb, &ec);
    if (!ec.first || !ec.last) return FALSE;
    sel->anchor_box = ec.first;
    sel->anchor_byte = 0;
    sel->focus_box = ec.last;
    sel->focus_byte = ec.last->text ? strlen(ec.last->text) : 0;
    sel->active = TRUE;
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

typedef struct ranges_ctx {
    GHashTable   *table;
    const ns_box *first_box;
    const ns_box *last_box;
    gsize         first_byte;
    gsize         last_byte;
    int           state;
} ranges_ctx;

static void
ranges_record(ranges_ctx *ctx, const ns_box *b, gsize start, gsize end)
{
    if (!b->text || !box_user_selectable(b)) return;
    gsize tlen = strlen(b->text);
    if (start > tlen) start = tlen;
    if (end   > tlen) end   = tlen;
    if (start >= end) return;
    ns_selection_run *run = g_new(ns_selection_run, 1);
    run->start = start;
    run->end   = end;
    g_hash_table_insert(ctx->table, (gpointer)b, run);
}

static void
ranges_walk_cb(const ns_box *b, gpointer ud)
{
    ranges_ctx *ctx = ud;
    if (ctx->state == 2) return;
    if (ctx->first_box == ctx->last_box) {
        if (b == ctx->first_box) {
            ranges_record(ctx, b, ctx->first_byte, ctx->last_byte);
            ctx->state = 2;
        }
        return;
    }
    if (ctx->state == 0) {
        if (b == ctx->first_box) {
            ranges_record(ctx, b, ctx->first_byte,
                          b->text ? strlen(b->text) : 0);
            ctx->state = 1;
        }
        return;
    }
    if (b == ctx->last_box) {
        ranges_record(ctx, b, 0, ctx->last_byte);
        ctx->state = 2;
        return;
    }
    if (b->text && *b->text)
        ranges_record(ctx, b, 0, strlen(b->text));
}

GHashTable *
ns_selection_ranges(const ns_box *root, const ns_selection *sel)
{
    if (!root || !ns_selection_has_range(sel)) return NULL;
    const ns_box *first_box = NULL, *last_box = NULL;
    gsize first_byte = 0, last_byte = 0;
    order_endpoints(root, *sel, &first_box, &first_byte, &last_box, &last_byte);
    ranges_ctx ctx = {
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free),
        first_box, last_box, first_byte, last_byte, 0
    };
    walk_inline_pre(root, ranges_walk_cb, &ctx);
    if (g_hash_table_size(ctx.table) == 0) {
        g_hash_table_destroy(ctx.table);
        return NULL;
    }
    return ctx.table;
}

gboolean
ns_selection_bounds(const ns_box *root, const ns_selection *sel,
                    double *out_x, double *out_y, double *out_w, double *out_h)
{
    GHashTable *runs = ns_selection_ranges(root, sel);
    if (!runs) return FALSE;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    gboolean any = FALSE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, runs);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        const ns_box *b = key;
        const ns_selection_run *run = val;
        double rx = b->x, ry = b->y;
        double rw = b->content_width, rh = b->content_height;
        gboolean whole = run->start == 0 && b->text &&
                         run->end == strlen(b->text);
        if (!whole) {
            if (!ns_paint_inline_range_extents(b, run->start,
                                               run->end - run->start,
                                               &rx, &ry, &rw, &rh))
                continue;
            rx += b->x;
            ry += b->y;
        }
        if (!any) {
            x0 = rx; y0 = ry; x1 = rx + rw; y1 = ry + rh;
            any = TRUE;
        } else {
            if (rx < x0) x0 = rx;
            if (ry < y0) y0 = ry;
            if (rx + rw > x1) x1 = rx + rw;
            if (ry + rh > y1) y1 = ry + rh;
        }
    }
    g_hash_table_destroy(runs);
    if (!any) return FALSE;
    if (out_x) *out_x = x0;
    if (out_y) *out_y = y0;
    if (out_w) *out_w = x1 - x0;
    if (out_h) *out_h = y1 - y0;
    return TRUE;
}

typedef struct collect_ctx {
    GString       *out;
    const ns_box  *first_box;
    const ns_box  *last_box;
    gsize          first_byte;
    gsize          last_byte;
    int            state;
    const ns_box  *prev_block;
} collect_ctx;

static void
append_copied_run(GString *out, const char *text, gsize start, gsize end)
{
    for (gsize i = start; i < end; ) {
        const char *p = text + i;
        gunichar u = g_utf8_get_char_validated(p, (gssize)(end - i));
        if (u == (gunichar)-1 || u == (gunichar)-2) {
            g_string_append_c(out, text[i]);
            i++;
            continue;
        }
        gsize step = (gsize)(g_utf8_next_char(p) - p);
        if (u == 0x2028 || u == 0x2029)
            g_string_append_c(out, '\n');
        else if (u != 0x200b && u != 0xfeff)
            g_string_append_len(out, p, (gssize)step);
        i += step;
    }
}

static void
collect_append(collect_ctx *ctx, const ns_box *b, gsize start, gsize end)
{
    if (end <= start || !b->text) return;
    const ns_box *block = containing_block(b);
    if (ctx->out->len > 0 && block != ctx->prev_block)
        g_string_append_c(ctx->out, '\n');
    ctx->prev_block = block;
    append_copied_run(ctx->out, b->text, start, end);
}

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
            if (box_user_selectable(b)) collect_append(ctx, b, s, e);
            ctx->state = 2;
        }
        return;
    }
    if (ctx->state == 0) {
        if (b == ctx->first_box) {
            gsize s = ctx->first_byte > tlen ? tlen : ctx->first_byte;
            if (box_user_selectable(b)) collect_append(ctx, b, s, tlen);
            ctx->state = 1;
        }
        return;
    }
    if (ctx->state == 1) {
        if (b == ctx->last_box) {
            gsize e = ctx->last_byte > tlen ? tlen : ctx->last_byte;
            if (box_user_selectable(b)) collect_append(ctx, b, 0, e);
            ctx->state = 2;
            return;
        }
        if (box_user_selectable(b))
            collect_append(ctx, b, 0, tlen);
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
    collect_ctx ctx = { out, first_box, last_box, first_byte, last_byte, 0,
                        NULL };
    walk_inline_pre(root, collect_walk_cb, &ctx);
    return g_string_free(out, FALSE);
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

