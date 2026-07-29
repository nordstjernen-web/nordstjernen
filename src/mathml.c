/* Nordstjernen — minimalist MathML presentation layout and paint.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "mathml.h"

#include <math.h>
#include "ns_pango.h"
#include <string.h>

#define NS_MATH_MAX_DEPTH 64
#define NS_MATH_MAX_CELLS 4096

static NsPangoContext *
math_pango_ctx(void)
{
    static NsPangoContext *ctx;
    if (!ctx) {
        NsPangoFontMap *fm = ns_pango_cairo_font_map_get_default();
        ctx = ns_pango_font_map_create_context(fm);
    }
    return ctx;
}

static gboolean
text_is_blank(const char *s)
{
    if (!s) return TRUE;
    for (; *s; s++)
        if (!g_ascii_isspace((guchar)*s)) return FALSE;
    return TRUE;
}

static const char *
tag_of(const ns_node *n)
{
    return n && n->kind == NS_NODE_ELEMENT && n->name ? n->name : "";
}

static gboolean
tag_is(const ns_node *n, const char *name)
{
    const char *t = tag_of(n);
    return t[0] && g_ascii_strcasecmp(t, name) == 0;
}

static gboolean
node_is_renderable(const ns_node *n)
{
    if (!n) return FALSE;
    if (n->kind == NS_NODE_ELEMENT) return TRUE;
    if (n->kind == NS_NODE_TEXT) return !text_is_blank(n->text);
    return FALSE;
}

static int
collect_args(const ns_node *n, const ns_node **out, int max)
{
    int c = 0;
    for (const ns_node *k = n->first_child; k; k = k->next_sibling) {
        if (!node_is_renderable(k)) continue;
        if (c < max) out[c] = k;
        c++;
    }
    return c;
}

static NsPangoLayout *
token_layout(const char *text, double fpx, gboolean italic)
{
    NsPangoLayout *l = ns_pango_layout_new(math_pango_ctx());
    NsPangoFontDescription *d = ns_pango_font_description_new();
    ns_pango_font_description_set_family(d, "serif");
    if (fpx < 1) fpx = 1;
    ns_pango_font_description_set_absolute_size(d, fpx * NS_PANGO_SCALE);
    ns_pango_font_description_set_style(d, italic ? NS_PANGO_STYLE_ITALIC
                                               : NS_PANGO_STYLE_NORMAL);
    ns_pango_layout_set_font_description(l, d);
    ns_pango_font_description_free(d);
    ns_pango_layout_set_text(l, text ? text : "", -1);
    return l;
}

static double
render_token(cairo_t *cr, const char *text, double fpx, gboolean italic,
             double x, double by, double *asc, double *desc)
{
    NsPangoLayout *l = token_layout(text, fpx, italic);
    int baseline = ns_pango_layout_get_baseline(l);
    NsPangoRectangle logical;
    ns_pango_layout_get_extents(l, NULL, &logical);
    double w = (double)logical.width / NS_PANGO_SCALE;
    double a = (double)baseline / NS_PANGO_SCALE;
    double total = (double)logical.height / NS_PANGO_SCALE;
    double dsc = total - a;
    if (asc) *asc = a;
    if (desc) *desc = dsc;
    if (cr && text && *text) {
        cairo_move_to(cr, x, by - a);
        ns_pango_cairo_show_layout(cr, l);
    }
    g_object_unref(l);
    return w;
}

static double mrender(cairo_t *cr, const ns_node *n, double fpx, int depth,
                      double x, double by, double *asc, double *desc);

static double
render_seq(cairo_t *cr, const ns_node *parent, double fpx, int depth,
           double x, double by, double *asc, double *desc)
{
    double cx = x, maxa = 0, maxd = 0;
    gboolean any = FALSE;
    for (const ns_node *k = parent->first_child; k; k = k->next_sibling) {
        if (!node_is_renderable(k)) continue;
        if (tag_is(k, "annotation") || tag_is(k, "annotation-xml")) continue;
        double a = 0, d = 0;
        double w = mrender(cr, k, fpx, depth + 1, cx, by, &a, &d);
        cx += w;
        if (a > maxa) maxa = a;
        if (d > maxd) maxd = d;
        any = TRUE;
    }
    if (!any) { maxa = fpx * 0.7; maxd = fpx * 0.2; }
    if (asc) *asc = maxa;
    if (desc) *desc = maxd;
    return cx - x;
}

static double
mo_spacing(const char *t, double fpx)
{
    if (!t || !*t) return 0;
    if (strcmp(t, "(") == 0 || strcmp(t, ")") == 0 ||
        strcmp(t, "[") == 0 || strcmp(t, "]") == 0 ||
        strcmp(t, "{") == 0 || strcmp(t, "}") == 0 ||
        strcmp(t, ",") == 0 || strcmp(t, "!") == 0)
        return fpx * 0.05;
    return fpx * 0.18;
}

static double
render_msup_msub(cairo_t *cr, const ns_node *n, double fpx, int depth,
                 double x, double by, double *asc, double *desc, gboolean sup)
{
    const ns_node *args[2];
    if (collect_args(n, args, 2) < 2)
        return render_seq(cr, n, fpx, depth, x, by, asc, desc);
    double ba = 0, bd = 0;
    double bw = mrender(cr, args[0], fpx, depth + 1, x, by, &ba, &bd);
    double sfpx = fpx * 0.72;
    double sa = 0, sd = 0;
    double sw = mrender(NULL, args[1], sfpx, depth + 1, 0, 0, &sa, &sd);
    if (sup) {
        double shift = ba * 0.5 + sfpx * 0.2;
        double sbase = by - shift;
        if (cr) mrender(cr, args[1], sfpx, depth + 1, x + bw, sbase, NULL, NULL);
        if (asc) *asc = MAX(ba, shift + sa);
        if (desc) *desc = bd;
    } else {
        double shift = MAX(bd * 0.5, sfpx * 0.3);
        double sbase = by + shift;
        if (cr) mrender(cr, args[1], sfpx, depth + 1, x + bw, sbase, NULL, NULL);
        if (asc) *asc = ba;
        if (desc) *desc = MAX(bd, shift + sd);
    }
    return bw + sw;
}

static double
render_msubsup(cairo_t *cr, const ns_node *n, double fpx, int depth,
               double x, double by, double *asc, double *desc)
{
    const ns_node *args[3];
    if (collect_args(n, args, 3) < 3)
        return render_seq(cr, n, fpx, depth, x, by, asc, desc);
    double ba = 0, bd = 0;
    double bw = mrender(cr, args[0], fpx, depth + 1, x, by, &ba, &bd);
    double sfpx = fpx * 0.72;
    double sba = 0, sbd = 0, spa = 0, spd = 0;
    double subw = mrender(NULL, args[1], sfpx, depth + 1, 0, 0, &sba, &sbd);
    double supw = mrender(NULL, args[2], sfpx, depth + 1, 0, 0, &spa, &spd);
    double up = ba * 0.5 + sfpx * 0.2;
    double down = MAX(bd * 0.5, sfpx * 0.3);
    if (cr) {
        mrender(cr, args[2], sfpx, depth + 1, x + bw, by - up, NULL, NULL);
        mrender(cr, args[1], sfpx, depth + 1, x + bw, by + down, NULL, NULL);
    }
    if (asc) *asc = MAX(ba, up + spa);
    if (desc) *desc = MAX(bd, down + sbd);
    return bw + MAX(subw, supw);
}

static double
render_mfrac(cairo_t *cr, const ns_node *n, double fpx, int depth,
             double x, double by, double *asc, double *desc)
{
    const ns_node *args[2];
    if (collect_args(n, args, 2) < 2)
        return render_seq(cr, n, fpx, depth, x, by, asc, desc);
    double na = 0, nd = 0, da = 0, dd = 0;
    double nw = mrender(NULL, args[0], fpx, depth + 1, 0, 0, &na, &nd);
    double dw = mrender(NULL, args[1], fpx, depth + 1, 0, 0, &da, &dd);
    double pad = fpx * 0.2;
    double width = MAX(nw, dw) + 2 * pad;
    double axis = fpx * 0.30;
    double rule = MAX(1.0, fpx * 0.055);
    double gap = fpx * 0.18;
    double bar_y = by - axis;
    if (cr) {
        double nx = x + (width - nw) / 2;
        double dx = x + (width - dw) / 2;
        mrender(cr, args[0], fpx, depth + 1, nx, bar_y - gap - nd, NULL, NULL);
        mrender(cr, args[1], fpx, depth + 1, dx, bar_y + gap + da, NULL, NULL);
        cairo_save(cr);
        cairo_set_line_width(cr, rule);
        cairo_move_to(cr, x + pad * 0.5, bar_y);
        cairo_line_to(cr, x + width - pad * 0.5, bar_y);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    if (asc) *asc = axis + gap + na + nd;
    double low = -axis + gap + da + dd;
    if (desc) *desc = low < 0 ? 0 : low;
    return width;
}

static double
render_radical(cairo_t *cr, double fpx, double rw, double ra, double rd,
               double x, double by, double *out_left)
{
    double surd = fpx * 0.6;
    double top_gap = fpx * 0.12;
    double pad = fpx * 0.12;
    if (cr) {
        double top = by - ra - top_gap;
        double bot = by + rd;
        cairo_save(cr);
        cairo_set_line_width(cr, MAX(1.0, fpx * 0.05));
        cairo_move_to(cr, x, by - ra * 0.35);
        cairo_line_to(cr, x + surd * 0.45, bot);
        cairo_line_to(cr, x + surd * 0.85, top);
        cairo_line_to(cr, x + surd + rw + pad, top);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
    if (out_left) *out_left = surd;
    return surd + rw + pad + top_gap;
}

static double
render_msqrt(cairo_t *cr, const ns_node *n, double fpx, int depth,
             double x, double by, double *asc, double *desc)
{
    double ra = 0, rd = 0;
    double rw = render_seq(NULL, n, fpx, depth, 0, 0, &ra, &rd);
    double surd = 0;
    double total = render_radical(cr, fpx, rw, ra, rd, x, by, &surd);
    if (cr) render_seq(cr, n, fpx, depth, x + surd, by, NULL, NULL);
    if (asc) *asc = ra + fpx * 0.12 + MAX(1.0, fpx * 0.05);
    if (desc) *desc = rd;
    return total;
}

static double
render_mroot(cairo_t *cr, const ns_node *n, double fpx, int depth,
             double x, double by, double *asc, double *desc)
{
    const ns_node *args[2];
    if (collect_args(n, args, 2) < 2)
        return render_msqrt(cr, n, fpx, depth, x, by, asc, desc);
    double ifpx = fpx * 0.55;
    double ia = 0, id = 0;
    double iw = mrender(NULL, args[1], ifpx, depth + 1, 0, 0, &ia, &id);
    double ra = 0, rd = 0;
    double rwid = mrender(NULL, args[0], fpx, depth + 1, 0, 0, &ra, &rd);
    double ox = x + iw * 0.7;
    double surd = 0;
    double total = render_radical(cr, fpx, rwid, ra, rd, ox, by, &surd);
    if (cr) {
        mrender(cr, args[0], fpx, depth + 1, ox + surd, by, NULL, NULL);
        mrender(cr, args[1], ifpx, depth + 1, x, by - ra * 0.6, NULL, NULL);
    }
    if (asc) *asc = MAX(ra + fpx * 0.12 + MAX(1.0, fpx * 0.05), ra * 0.6 + ia);
    if (desc) *desc = rd;
    return (ox - x) + total;
}

static double
render_under_over(cairo_t *cr, const ns_node *n, double fpx, int depth,
                  double x, double by, double *asc, double *desc,
                  gboolean over, gboolean under)
{
    const ns_node *args[3];
    int na = collect_args(n, args, 3);
    int need = (over && under) ? 3 : 2;
    if (na < need)
        return render_seq(cr, n, fpx, depth, x, by, asc, desc);
    double sfpx = fpx * 0.72;
    double ba = 0, bd = 0;
    double bw = mrender(NULL, args[0], fpx, depth + 1, 0, 0, &ba, &bd);
    double gap = fpx * 0.12;
    double topa = ba, botd = bd;
    double width = bw;
    const ns_node *over_n = NULL, *under_n = NULL;
    if (over && under) { under_n = args[1]; over_n = args[2]; }
    else if (over)     { over_n = args[1]; }
    else               { under_n = args[1]; }
    double oa = 0, od = 0, ua = 0, ud = 0, ow = 0, uw = 0;
    if (over_n)  ow = mrender(NULL, over_n, sfpx, depth + 1, 0, 0, &oa, &od);
    if (under_n) uw = mrender(NULL, under_n, sfpx, depth + 1, 0, 0, &ua, &ud);
    width = MAX(width, MAX(ow, uw));
    if (cr) {
        double bx = x + (width - bw) / 2;
        mrender(cr, args[0], fpx, depth + 1, bx, by, NULL, NULL);
        if (over_n) {
            double sx = x + (width - ow) / 2;
            mrender(cr, over_n, sfpx, depth + 1, sx, by - ba - gap - od, NULL, NULL);
        }
        if (under_n) {
            double sx = x + (width - uw) / 2;
            mrender(cr, under_n, sfpx, depth + 1, sx, by + bd + gap + ua, NULL, NULL);
        }
    }
    if (over_n)  topa = ba + gap + oa + od;
    if (under_n) botd = bd + gap + ua + ud;
    if (asc) *asc = topa;
    if (desc) *desc = botd;
    return width;
}

static double
render_mtable(cairo_t *cr, const ns_node *n, double fpx, int depth,
              double x, double by, double *asc, double *desc)
{
    GArray *colw = g_array_new(FALSE, TRUE, sizeof(double));
    GArray *rowa = g_array_new(FALSE, TRUE, sizeof(double));
    GArray *rowd = g_array_new(FALSE, TRUE, sizeof(double));
    int ncells = 0;
    for (const ns_node *r = n->first_child; r; r = r->next_sibling) {
        if (!tag_is(r, "mtr") && !tag_is(r, "mlabeledtr")) continue;
        double ra = fpx * 0.7, rd = fpx * 0.2;
        int col = 0;
        for (const ns_node *c = r->first_child; c; c = c->next_sibling) {
            if (!tag_is(c, "mtd")) continue;
            if (++ncells > NS_MATH_MAX_CELLS) break;
            double ca = 0, cd = 0;
            double cw = render_seq(NULL, c, fpx, depth + 1, 0, 0, &ca, &cd);
            if ((int)colw->len <= col) g_array_set_size(colw, col + 1);
            double *cwp = &g_array_index(colw, double, col);
            if (cw > *cwp) *cwp = cw;
            if (ca > ra) ra = ca;
            if (cd > rd) rd = cd;
            col++;
        }
        g_array_append_val(rowa, ra);
        g_array_append_val(rowd, rd);
    }
    double colgap = fpx * 0.6, rowgap = fpx * 0.35;
    double total_w = 0;
    for (guint i = 0; i < colw->len; i++) {
        total_w += g_array_index(colw, double, i);
        if (i + 1 < colw->len) total_w += colgap;
    }
    double total_h = 0;
    for (guint i = 0; i < rowa->len; i++) {
        total_h += g_array_index(rowa, double, i) + g_array_index(rowd, double, i);
        if (i + 1 < rowa->len) total_h += rowgap;
    }
    double axis = fpx * 0.30;
    double top = by - axis - total_h / 2;
    if (cr) {
        double cy = top;
        guint ri = 0;
        for (const ns_node *r = n->first_child; r; r = r->next_sibling) {
            if (!tag_is(r, "mtr") && !tag_is(r, "mlabeledtr")) continue;
            if (ri >= rowa->len) break;
            double ra = g_array_index(rowa, double, ri);
            double rd = g_array_index(rowd, double, ri);
            double row_base = cy + ra;
            double cx = x;
            int col = 0;
            for (const ns_node *c = r->first_child; c; c = c->next_sibling) {
                if (!tag_is(c, "mtd")) continue;
                if ((int)colw->len <= col) break;
                double cwidth = g_array_index(colw, double, col);
                double ca = 0, cd = 0;
                double cw = render_seq(NULL, c, fpx, depth + 1, 0, 0, &ca, &cd);
                render_seq(cr, c, fpx, depth + 1, cx + (cwidth - cw) / 2,
                           row_base, NULL, NULL);
                cx += cwidth + colgap;
                col++;
            }
            cy += ra + rd + rowgap;
            ri++;
        }
    }
    g_array_free(colw, TRUE);
    g_array_free(rowa, TRUE);
    g_array_free(rowd, TRUE);
    if (asc) *asc = axis + total_h / 2;
    double low = total_h / 2 - axis;
    if (desc) *desc = low < 0 ? 0 : low;
    return total_w;
}

static double
render_mfenced(cairo_t *cr, const ns_node *n, double fpx, int depth,
               double x, double by, double *asc, double *desc)
{
    const char *open = ns_element_get_attr(n, "open");
    const char *close = ns_element_get_attr(n, "close");
    const char *seps = ns_element_get_attr(n, "separators");
    if (!open)  open  = "(";
    if (!close) close = ")";
    if (!seps)  seps  = ",";
    const ns_node *kids[64];
    int nk = collect_args(n, kids, 64);
    if (nk > 64) nk = 64;
    double cx = x, maxa = fpx * 0.7, maxd = fpx * 0.2;
    double a = 0, d = 0, sp;
    if (*open) {
        sp = mo_spacing(open, fpx);
        cx += render_token(cr, open, fpx, FALSE, cx + sp, by, &a, &d) + 2 * sp;
        if (a > maxa) maxa = a;
        if (d > maxd) maxd = d;
    }
    gsize nsep = strlen(seps);
    for (int i = 0; i < nk; i++) {
        if (i > 0 && nsep > 0) {
            gsize si = (gsize)(i - 1) < nsep ? (gsize)(i - 1) : nsep - 1;
            char sepbuf[2] = { seps[si], 0 };
            if (!g_ascii_isspace((guchar)sepbuf[0])) {
                sp = mo_spacing(sepbuf, fpx);
                cx += render_token(cr, sepbuf, fpx, FALSE, cx + sp, by, &a, &d)
                      + 2 * sp;
                if (a > maxa) maxa = a;
                if (d > maxd) maxd = d;
            }
        }
        cx += mrender(cr, kids[i], fpx, depth + 1, cx, by, &a, &d);
        if (a > maxa) maxa = a;
        if (d > maxd) maxd = d;
    }
    if (*close) {
        sp = mo_spacing(close, fpx);
        cx += render_token(cr, close, fpx, FALSE, cx + sp, by, &a, &d) + 2 * sp;
        if (a > maxa) maxa = a;
        if (d > maxd) maxd = d;
    }
    if (asc) *asc = maxa;
    if (desc) *desc = maxd;
    return cx - x;
}

static double
mrender(cairo_t *cr, const ns_node *n, double fpx, int depth,
        double x, double by, double *asc, double *desc)
{
    if (asc) *asc = fpx * 0.7;
    if (desc) *desc = fpx * 0.2;
    if (!n || depth > NS_MATH_MAX_DEPTH) return 0;

    if (n->kind == NS_NODE_TEXT) {
        char *t = g_strstrip(g_strdup(n->text ? n->text : ""));
        double w = render_token(cr, t, fpx, FALSE, x, by, asc, desc);
        g_free(t);
        return w;
    }

    const char *tag = tag_of(n);
    if (tag_is(n, "mi")) {
        char *t = g_strstrip(ns_node_collect_text(n));
        gboolean italic = FALSE;
        if (t && t[0] && g_utf8_strlen(t, -1) == 1)
            italic = g_unichar_isalpha(g_utf8_get_char(t));
        double w = render_token(cr, t, fpx, italic, x, by, asc, desc);
        g_free(t);
        return w;
    }
    if (tag_is(n, "mn") || tag_is(n, "mtext") || tag_is(n, "ms")) {
        char *t = g_strstrip(ns_node_collect_text(n));
        double w = render_token(cr, t, fpx, FALSE, x, by, asc, desc);
        g_free(t);
        return w;
    }
    if (tag_is(n, "mo")) {
        char *t = g_strstrip(ns_node_collect_text(n));
        double sp = mo_spacing(t, fpx);
        double w = render_token(cr, t, fpx, FALSE, x + sp, by, asc, desc);
        g_free(t);
        return w + 2 * sp;
    }
    if (tag_is(n, "mphantom")) {
        double a = 0, d = 0;
        double w = render_seq(NULL, n, fpx, depth, 0, 0, &a, &d);
        if (asc) *asc = a;
        if (desc) *desc = d;
        return w;
    }
    if (tag_is(n, "mspace")) {
        if (asc) *asc = 0;
        if (desc) *desc = 0;
        return fpx * 0.4;
    }
    if (tag_is(n, "msup"))
        return render_msup_msub(cr, n, fpx, depth, x, by, asc, desc, TRUE);
    if (tag_is(n, "msub"))
        return render_msup_msub(cr, n, fpx, depth, x, by, asc, desc, FALSE);
    if (tag_is(n, "msubsup"))
        return render_msubsup(cr, n, fpx, depth, x, by, asc, desc);
    if (tag_is(n, "mfrac"))
        return render_mfrac(cr, n, fpx, depth, x, by, asc, desc);
    if (tag_is(n, "msqrt"))
        return render_msqrt(cr, n, fpx, depth, x, by, asc, desc);
    if (tag_is(n, "mroot"))
        return render_mroot(cr, n, fpx, depth, x, by, asc, desc);
    if (tag_is(n, "mover"))
        return render_under_over(cr, n, fpx, depth, x, by, asc, desc, TRUE, FALSE);
    if (tag_is(n, "munder"))
        return render_under_over(cr, n, fpx, depth, x, by, asc, desc, FALSE, TRUE);
    if (tag_is(n, "munderover"))
        return render_under_over(cr, n, fpx, depth, x, by, asc, desc, TRUE, TRUE);
    if (tag_is(n, "mtable"))
        return render_mtable(cr, n, fpx, depth, x, by, asc, desc);
    if (tag_is(n, "mfenced"))
        return render_mfenced(cr, n, fpx, depth, x, by, asc, desc);
    if (tag_is(n, "semantics")) {
        const ns_node *first = NULL;
        if (collect_args(n, &first, 1) >= 1)
            return mrender(cr, first, fpx, depth + 1, x, by, asc, desc);
        return 0;
    }
    (void)tag;
    return render_seq(cr, n, fpx, depth, x, by, asc, desc);
}

void
ns_math_measure(const ns_node *math, double font_px,
                double *out_w, double *out_ascent, double *out_descent)
{
    double a = 0, d = 0;
    double w = mrender(NULL, math, font_px, 0, 0, 0, &a, &d);
    if (out_w) *out_w = w;
    if (out_ascent) *out_ascent = a;
    if (out_descent) *out_descent = d;
}

void
ns_math_paint(cairo_t *cr, const ns_node *math, double x, double y,
              double font_px, double r, double g, double b, double a)
{
    if (!cr || !math) return;
    double asc = 0, desc = 0;
    mrender(NULL, math, font_px, 0, 0, 0, &asc, &desc);
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    mrender(cr, math, font_px, 0, x, y + asc, NULL, NULL);
    cairo_restore(cr);
}
