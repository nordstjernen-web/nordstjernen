/* Northstar — SVG rendering onto Cairo, sharing the CSS cascade with HTML.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "svg.h"

#include <math.h>
#include <string.h>

#include <pango/pangocairo.h>

#include "css.h"
#include "html.h"

enum {
    NS_SVG_MAX_DEPTH        = 24,
    NS_SVG_MAX_NODES        = 60000,
    NS_SVG_MAX_INPUT_BYTES  = 8 * 1024 * 1024,
    NS_SVG_MAX_DIM_PX       = 8192,
    NS_SVG_MAX_PIXELS       = 4096 * 4096,
    NS_SVG_DEFAULT_DIM_PX   = 512,
    NS_SVG_MAX_HREF_CHAIN   = 16,
    NS_SVG_MAX_DASHES       = 256,
};

typedef enum {
    SVG_PAINT_NONE,
    SVG_PAINT_COLOR,
    SVG_PAINT_REF,
} svg_paint_kind;

typedef struct {
    svg_paint_kind kind;
    double         r, g, b, a;
    char          *ref;
    gboolean       have_fallback;
    double         fr, fg, fb, fa;
} svg_paint;

typedef struct {
    svg_paint         fill;
    svg_paint         stroke;
    double            fill_opacity;
    double            stroke_opacity;
    double            stroke_width;
    double            dash_offset;
    double            miter_limit;
    cairo_line_cap_t  line_cap;
    cairo_line_join_t line_join;
    cairo_fill_rule_t fill_rule;
    cairo_fill_rule_t clip_rule;
    GArray           *dashes;
    double            color_r, color_g, color_b, color_a;
    char             *font_family;
    double            font_size;
    int               font_weight;
    gboolean          font_italic;
    int               text_anchor;
    gboolean          hidden;
    gboolean          stroke_first;
    gboolean          non_scaling_stroke;
} svg_state;

typedef struct {
    cairo_t       *cr;
    const ns_node *root;
    GHashTable    *styles;
    GHashTable    *ids;
    int            depth;
    int            nodes;
    double         vw, vh;
} svg_ctx;

static void svg_render_node(svg_ctx *ctx, const ns_node *n,
                            const svg_state *parent);

static gboolean
svg_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static void
svg_skip_sep(const char **pp)
{
    const char *p = *pp;
    while (*p && (svg_is_ws(*p) || *p == ',')) p++;
    *pp = p;
}

static gboolean
svg_num(const char **pp, double *out)
{
    svg_skip_sep(pp);
    const char *p = *pp;
    if (!*p) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(p, &end);
    if (!end || end == p) return FALSE;
    if (!isfinite(v)) return FALSE;
    *out = v;
    *pp = end;
    return TRUE;
}

static gboolean
svg_flag(const char **pp, gboolean *out)
{
    svg_skip_sep(pp);
    const char *p = *pp;
    if (*p == '0') { *out = FALSE; *pp = p + 1; return TRUE; }
    if (*p == '1') { *out = TRUE;  *pp = p + 1; return TRUE; }
    return FALSE;
}

static double
svg_length_str(const char *s, double pct_basis, double font_size, double fallback)
{
    if (!s) return fallback;
    while (*s && svg_is_ws(*s)) s++;
    if (!*s) return fallback;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (!end || end == s || !isfinite(v)) return fallback;
    while (*end && svg_is_ws(*end)) end++;
    if (*end == '%')                        return v / 100.0 * pct_basis;
    if (g_ascii_strncasecmp(end, "px", 2) == 0) return v;
    if (g_ascii_strncasecmp(end, "pt", 2) == 0) return v * 4.0 / 3.0;
    if (g_ascii_strncasecmp(end, "pc", 2) == 0) return v * 16.0;
    if (g_ascii_strncasecmp(end, "mm", 2) == 0) return v * 96.0 / 25.4;
    if (g_ascii_strncasecmp(end, "cm", 2) == 0) return v * 96.0 / 2.54;
    if (g_ascii_strncasecmp(end, "in", 2) == 0) return v * 96.0;
    if (g_ascii_strncasecmp(end, "em", 2) == 0) return v * font_size;
    if (g_ascii_strncasecmp(end, "ex", 2) == 0) return v * font_size * 0.5;
    if (g_ascii_strncasecmp(end, "rem", 3) == 0) return v * 16.0;
    return v;
}

static double
svg_attr_length(const ns_node *n, const char *name, double pct_basis,
                double font_size, double fallback)
{
    const char *s = ns_element_get_attr(n, name);
    return s ? svg_length_str(s, pct_basis, font_size, fallback) : fallback;
}

static double
svg_diag_basis(double w, double h)
{
    return sqrt(w * w + h * h) / G_SQRT2;
}

static void
svg_paint_clear(svg_paint *p)
{
    g_clear_pointer(&p->ref, g_free);
}

static char *
svg_url_id(const char *s, const char **rest)
{
    if (!s) return NULL;
    while (*s && svg_is_ws(*s)) s++;
    if (g_ascii_strncasecmp(s, "url(", 4) != 0) return NULL;
    const char *p = s + 4;
    while (*p && svg_is_ws(*p)) p++;
    char quote = 0;
    if (*p == '"' || *p == '\'') { quote = *p; p++; }
    const char *start = p;
    while (*p && *p != ')' && (!quote || *p != quote)) p++;
    const char *end = p;
    if (quote && *p == quote) p++;
    while (*p && *p != ')') p++;
    if (*p == ')') p++;
    if (rest) *rest = p;
    while (end > start && svg_is_ws(end[-1])) end--;
    if (end > start && *start == '#') start++;
    if (end <= start) return NULL;
    char *raw = g_strndup(start, (gsize)(end - start));
    if (!strchr(raw, '%')) return raw;
    char *dec = g_uri_unescape_string(raw, NULL);
    if (!dec) return raw;
    g_free(raw);
    return dec;
}

static gboolean
svg_parse_paint(const char *text, const svg_state *st, svg_paint *out)
{
    if (!text) return FALSE;
    while (*text && svg_is_ws(*text)) text++;
    if (!*text) return FALSE;

    if (g_ascii_strncasecmp(text, "url(", 4) == 0) {
        const char *rest = NULL;
        char *id = svg_url_id(text, &rest);
        if (!id) return FALSE;
        svg_paint_clear(out);
        out->kind = SVG_PAINT_REF;
        out->ref = id;
        out->have_fallback = FALSE;
        if (rest) {
            while (*rest && svg_is_ws(*rest)) rest++;
            if (*rest) {
                guint8 r, g, b, a;
                if (g_ascii_strncasecmp(rest, "none", 4) == 0) {
                    out->have_fallback = TRUE;
                    out->fa = 0;
                } else if (g_ascii_strncasecmp(rest, "currentcolor", 12) == 0) {
                    out->have_fallback = TRUE;
                    out->fr = st->color_r; out->fg = st->color_g;
                    out->fb = st->color_b; out->fa = st->color_a;
                } else if (ns_css_parse_color(rest, &r, &g, &b, &a)) {
                    out->have_fallback = TRUE;
                    out->fr = r / 255.0; out->fg = g / 255.0;
                    out->fb = b / 255.0; out->fa = a / 255.0;
                }
            }
        }
        return TRUE;
    }
    if (g_ascii_strcasecmp(text, "none") == 0) {
        svg_paint_clear(out);
        out->kind = SVG_PAINT_NONE;
        return TRUE;
    }
    if (g_ascii_strcasecmp(text, "currentcolor") == 0) {
        svg_paint_clear(out);
        out->kind = SVG_PAINT_COLOR;
        out->r = st->color_r; out->g = st->color_g;
        out->b = st->color_b; out->a = st->color_a;
        return TRUE;
    }
    guint8 r, g, b, a;
    if (ns_css_parse_color(text, &r, &g, &b, &a)) {
        svg_paint_clear(out);
        out->kind = SVG_PAINT_COLOR;
        out->r = r / 255.0; out->g = g / 255.0;
        out->b = b / 255.0; out->a = a / 255.0;
        return TRUE;
    }
    return FALSE;
}

static void
svg_state_init(svg_state *st)
{
    memset(st, 0, sizeof *st);
    st->fill.kind = SVG_PAINT_COLOR;
    st->fill.a = 1.0;
    st->stroke.kind = SVG_PAINT_NONE;
    st->fill_opacity = 1.0;
    st->stroke_opacity = 1.0;
    st->stroke_width = 1.0;
    st->miter_limit = 4.0;
    st->line_cap = CAIRO_LINE_CAP_BUTT;
    st->line_join = CAIRO_LINE_JOIN_MITER;
    st->fill_rule = CAIRO_FILL_RULE_WINDING;
    st->clip_rule = CAIRO_FILL_RULE_WINDING;
    st->color_a = 1.0;
    st->font_size = 16.0;
    st->font_weight = 400;
}

static void
svg_state_copy(svg_state *dst, const svg_state *src)
{
    *dst = *src;
    dst->fill.ref = src->fill.ref ? g_strdup(src->fill.ref) : NULL;
    dst->stroke.ref = src->stroke.ref ? g_strdup(src->stroke.ref) : NULL;
    dst->font_family = src->font_family ? g_strdup(src->font_family) : NULL;
    if (src->dashes) {
        dst->dashes = g_array_sized_new(FALSE, FALSE, sizeof(double),
                                        src->dashes->len);
        g_array_append_vals(dst->dashes, src->dashes->data, src->dashes->len);
    }
}

static void
svg_state_clear(svg_state *st)
{
    svg_paint_clear(&st->fill);
    svg_paint_clear(&st->stroke);
    g_clear_pointer(&st->font_family, g_free);
    if (st->dashes) { g_array_free(st->dashes, TRUE); st->dashes = NULL; }
}

static void
svg_set_dashes(svg_state *st, const char *text, double basis, double font_size)
{
    if (st->dashes) { g_array_free(st->dashes, TRUE); st->dashes = NULL; }
    if (!text || g_ascii_strcasecmp(text, "none") == 0) return;
    GArray *out = g_array_new(FALSE, FALSE, sizeof(double));
    const char *p = text;
    double v;
    gboolean any_positive = FALSE;
    while (svg_num(&p, &v) && out->len < NS_SVG_MAX_DASHES) {
        while (*p && !svg_is_ws(*p) && *p != ',' && *p != '-' && *p != '+' &&
               !g_ascii_isdigit(*p) && *p != '.')
            p++;
        if (v < 0) { g_array_free(out, TRUE); return; }
        if (v > 0) any_positive = TRUE;
        g_array_append_val(out, v);
    }
    if (out->len == 0 || !any_positive) { g_array_free(out, TRUE); return; }
    if (out->len % 2) {
        guint n = out->len;
        for (guint i = 0; i < n; i++) {
            double d = g_array_index(out, double, i);
            g_array_append_val(out, d);
        }
    }
    (void)basis;
    (void)font_size;
    st->dashes = out;
}

static cairo_line_cap_t
svg_cap_of(const char *s)
{
    if (!s) return CAIRO_LINE_CAP_BUTT;
    if (g_ascii_strcasecmp(s, "round") == 0)  return CAIRO_LINE_CAP_ROUND;
    if (g_ascii_strcasecmp(s, "square") == 0) return CAIRO_LINE_CAP_SQUARE;
    return CAIRO_LINE_CAP_BUTT;
}

static cairo_line_join_t
svg_join_of(const char *s)
{
    if (!s) return CAIRO_LINE_JOIN_MITER;
    if (g_ascii_strcasecmp(s, "round") == 0) return CAIRO_LINE_JOIN_ROUND;
    if (g_ascii_strcasecmp(s, "bevel") == 0) return CAIRO_LINE_JOIN_BEVEL;
    return CAIRO_LINE_JOIN_MITER;
}

static int
svg_anchor_of(const char *s)
{
    if (!s) return 0;
    if (g_ascii_strcasecmp(s, "middle") == 0) return 1;
    if (g_ascii_strcasecmp(s, "end") == 0)    return 2;
    return 0;
}

static const char *
svg_style_decl(const ns_node *n, const char *prop)
{
    static char buf[256];
    const char *style = ns_element_get_attr(n, "style");
    if (!style) return NULL;
    gsize plen = strlen(prop);
    const char *p = style;
    const char *found = NULL;
    gsize found_len = 0;
    while (*p) {
        while (*p && (svg_is_ws(*p) || *p == ';')) p++;
        const char *name = p;
        while (*p && *p != ':' && *p != ';') p++;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }
        const char *name_end = p;
        while (name_end > name && svg_is_ws(name_end[-1])) name_end--;
        p++;
        while (*p && svg_is_ws(*p)) p++;
        const char *val = p;
        while (*p && *p != ';') p++;
        const char *val_end = p;
        while (val_end > val && svg_is_ws(val_end[-1])) val_end--;
        if ((gsize)(name_end - name) == plen &&
            g_ascii_strncasecmp(name, prop, plen) == 0) {
            found = val;
            found_len = (gsize)(val_end - val);
        }
    }
    if (!found || found_len == 0 || found_len >= sizeof buf) return NULL;
    memcpy(buf, found, found_len);
    buf[found_len] = '\0';
    return buf;
}

static const char *
svg_prop(const ns_node *n, const char *name)
{
    const char *v = svg_style_decl(n, name);
    if (v) return v;
    return ns_element_get_attr(n, name);
}

static void
svg_apply_css_paint(svg_state *st, const ns_style *s, ns_css_prop prop,
                    svg_paint *out)
{
    const ns_css_value *v = s->values[prop];
    if (!v) return;
    if (v->kind == NS_CSS_V_COLOR) {
        svg_paint_clear(out);
        out->kind = SVG_PAINT_COLOR;
        out->r = v->u.color.r / 255.0;
        out->g = v->u.color.g / 255.0;
        out->b = v->u.color.b / 255.0;
        out->a = v->u.color.a / 255.0;
    } else if (v->kind == NS_CSS_V_KEYWORD && v->u.keyword) {
        svg_parse_paint(v->u.keyword, st, out);
    }
}

static double
svg_css_number(const ns_style *s, ns_css_prop prop, double basis,
               double fallback)
{
    const ns_css_value *v = s->values[prop];
    if (!v) return fallback;
    if (v->kind == NS_CSS_V_LENGTH) {
        if (v->u.length.unit == NS_CSS_UNIT_PERCENT)
            return v->u.length.v / 100.0 * basis;
        return v->u.length.v;
    }
    return ns_css_length_or(v, fallback);
}

static void
svg_state_apply_node(svg_ctx *ctx, svg_state *st, const ns_node *n)
{
    const ns_style *s = ctx->styles
        ? g_hash_table_lookup(ctx->styles, (gpointer)n) : NULL;
    double basis = svg_diag_basis(ctx->vw, ctx->vh);

    if (s && s->values[NS_CSS_COLOR] &&
        s->values[NS_CSS_COLOR]->kind == NS_CSS_V_COLOR) {
        st->color_r = s->values[NS_CSS_COLOR]->u.color.r / 255.0;
        st->color_g = s->values[NS_CSS_COLOR]->u.color.g / 255.0;
        st->color_b = s->values[NS_CSS_COLOR]->u.color.b / 255.0;
        st->color_a = s->values[NS_CSS_COLOR]->u.color.a / 255.0;
    } else {
        const char *col = svg_prop(n, "color");
        guint8 r, g, b, a;
        if (col && ns_css_parse_color(col, &r, &g, &b, &a)) {
            st->color_r = r / 255.0; st->color_g = g / 255.0;
            st->color_b = b / 255.0; st->color_a = a / 255.0;
        }
    }

    const char *v;
    if ((v = svg_prop(n, "fill")))   svg_parse_paint(v, st, &st->fill);
    if ((v = svg_prop(n, "stroke"))) svg_parse_paint(v, st, &st->stroke);
    if ((v = svg_prop(n, "fill-opacity")))
        st->fill_opacity = CLAMP(svg_length_str(v, 1.0, st->font_size, 1.0), 0.0, 1.0);
    if ((v = svg_prop(n, "stroke-opacity")))
        st->stroke_opacity = CLAMP(svg_length_str(v, 1.0, st->font_size, 1.0), 0.0, 1.0);
    if ((v = svg_prop(n, "stroke-width")))
        st->stroke_width = MAX(0.0, svg_length_str(v, basis, st->font_size, 1.0));
    if ((v = svg_prop(n, "stroke-linecap")))    st->line_cap = svg_cap_of(v);
    if ((v = svg_prop(n, "stroke-linejoin")))   st->line_join = svg_join_of(v);
    if ((v = svg_prop(n, "stroke-miterlimit")))
        st->miter_limit = MAX(1.0, svg_length_str(v, 1.0, st->font_size, 4.0));
    if ((v = svg_prop(n, "stroke-dashoffset")))
        st->dash_offset = svg_length_str(v, basis, st->font_size, 0.0);
    if ((v = svg_prop(n, "stroke-dasharray")))
        svg_set_dashes(st, v, basis, st->font_size);
    if ((v = svg_prop(n, "fill-rule")))
        st->fill_rule = (g_ascii_strcasecmp(v, "evenodd") == 0)
            ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING;
    if ((v = svg_prop(n, "clip-rule")))
        st->clip_rule = (g_ascii_strcasecmp(v, "evenodd") == 0)
            ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING;
    if ((v = svg_prop(n, "text-anchor"))) st->text_anchor = svg_anchor_of(v);
    if ((v = svg_prop(n, "font-size")))
        st->font_size = MAX(0.0, svg_length_str(v, st->font_size, st->font_size, 16.0));
    if ((v = svg_prop(n, "font-family"))) {
        g_free(st->font_family);
        st->font_family = g_strdup(v);
    }
    if ((v = svg_prop(n, "font-weight"))) {
        if (g_ascii_strcasecmp(v, "bold") == 0) st->font_weight = 700;
        else if (g_ascii_strcasecmp(v, "normal") == 0) st->font_weight = 400;
        else {
            int w = atoi(v);
            if (w >= 1 && w <= 1000) st->font_weight = w;
        }
    }
    if ((v = svg_prop(n, "font-style")))
        st->font_italic = g_ascii_strcasecmp(v, "normal") != 0;
    if ((v = svg_prop(n, "paint-order")))
        st->stroke_first = g_ascii_strncasecmp(v, "stroke", 6) == 0;
    if ((v = svg_prop(n, "visibility")))
        st->hidden = (g_ascii_strcasecmp(v, "hidden") == 0 ||
                      g_ascii_strcasecmp(v, "collapse") == 0);
    if ((v = svg_prop(n, "vector-effect")))
        st->non_scaling_stroke = g_ascii_strcasecmp(v, "non-scaling-stroke") == 0;

    if (!s) return;

    svg_apply_css_paint(st, s, NS_CSS_FILL, &st->fill);
    svg_apply_css_paint(st, s, NS_CSS_STROKE, &st->stroke);
    if (s->values[NS_CSS_FILL_OPACITY])
        st->fill_opacity = CLAMP(svg_css_number(s, NS_CSS_FILL_OPACITY, 1.0, 1.0), 0.0, 1.0);
    if (s->values[NS_CSS_STROKE_OPACITY])
        st->stroke_opacity = CLAMP(svg_css_number(s, NS_CSS_STROKE_OPACITY, 1.0, 1.0), 0.0, 1.0);
    if (s->values[NS_CSS_STROKE_WIDTH])
        st->stroke_width = MAX(0.0, svg_css_number(s, NS_CSS_STROKE_WIDTH, basis, 1.0));
    if (s->values[NS_CSS_STROKE_MITERLIMIT])
        st->miter_limit = MAX(1.0, svg_css_number(s, NS_CSS_STROKE_MITERLIMIT, 1.0, 4.0));
    if (s->values[NS_CSS_STROKE_DASHOFFSET])
        st->dash_offset = svg_css_number(s, NS_CSS_STROKE_DASHOFFSET, basis, 0.0);
    const char *kw;
    if ((kw = ns_style_keyword(s, NS_CSS_STROKE_LINECAP)))  st->line_cap = svg_cap_of(kw);
    if ((kw = ns_style_keyword(s, NS_CSS_STROKE_LINEJOIN))) st->line_join = svg_join_of(kw);
    if ((kw = ns_style_keyword(s, NS_CSS_FILL_RULE)))
        st->fill_rule = (g_ascii_strcasecmp(kw, "evenodd") == 0)
            ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING;
    if ((kw = ns_style_keyword(s, NS_CSS_CLIP_RULE)))
        st->clip_rule = (g_ascii_strcasecmp(kw, "evenodd") == 0)
            ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING;
    if ((kw = ns_style_keyword(s, NS_CSS_TEXT_ANCHOR)))  st->text_anchor = svg_anchor_of(kw);
    if ((kw = ns_style_keyword(s, NS_CSS_PAINT_ORDER)))
        st->stroke_first = g_ascii_strncasecmp(kw, "stroke", 6) == 0;
    if ((kw = ns_style_keyword(s, NS_CSS_STROKE_DASHARRAY)))
        svg_set_dashes(st, kw, basis, st->font_size);
    if ((kw = ns_style_keyword(s, NS_CSS_VECTOR_EFFECT)))
        st->non_scaling_stroke = g_ascii_strcasecmp(kw, "non-scaling-stroke") == 0;
    if ((kw = ns_style_keyword(s, NS_CSS_VISIBILITY)))
        st->hidden = (g_ascii_strcasecmp(kw, "hidden") == 0 ||
                      g_ascii_strcasecmp(kw, "collapse") == 0);
    if (s->values[NS_CSS_FONT_SIZE])
        st->font_size = MAX(0.0, svg_css_number(s, NS_CSS_FONT_SIZE, st->font_size, st->font_size));
    if (s->values[NS_CSS_FONT_WEIGHT])
        st->font_weight = ns_css_font_weight_number(s->values[NS_CSS_FONT_WEIGHT],
                                                    st->font_weight);
    if ((kw = ns_style_keyword(s, NS_CSS_FONT_STYLE)))
        st->font_italic = g_ascii_strcasecmp(kw, "normal") != 0;
    if (s->values[NS_CSS_FONT_FAMILY] &&
        s->values[NS_CSS_FONT_FAMILY]->kind == NS_CSS_V_KEYWORD &&
        s->values[NS_CSS_FONT_FAMILY]->u.keyword) {
        g_free(st->font_family);
        st->font_family = g_strdup(s->values[NS_CSS_FONT_FAMILY]->u.keyword);
    }
}

static gboolean
svg_parse_transform(const char *s, cairo_matrix_t *out)
{
    cairo_matrix_init_identity(out);
    if (!s) return FALSE;
    gboolean any = FALSE;
    const char *p = s;
    while (*p) {
        while (*p && (svg_is_ws(*p) || *p == ',')) p++;
        if (!*p) break;
        const char *name = p;
        while (g_ascii_isalpha(*p)) p++;
        gsize nlen = (gsize)(p - name);
        if (nlen == 0) break;
        while (*p && svg_is_ws(*p)) p++;
        if (*p != '(') break;
        p++;
        double a[6] = {0};
        int n = 0;
        while (n < 6 && svg_num(&p, &a[n])) n++;
        while (*p && *p != ')') p++;
        if (*p == ')') p++;

        cairo_matrix_t m;
        cairo_matrix_init_identity(&m);
        if (nlen == 6 && g_ascii_strncasecmp(name, "matrix", 6) == 0 && n == 6) {
            cairo_matrix_init(&m, a[0], a[1], a[2], a[3], a[4], a[5]);
        } else if (nlen == 9 && g_ascii_strncasecmp(name, "translate", 9) == 0 && n >= 1) {
            cairo_matrix_init_translate(&m, a[0], n >= 2 ? a[1] : 0.0);
        } else if (nlen == 5 && g_ascii_strncasecmp(name, "scale", 5) == 0 && n >= 1) {
            cairo_matrix_init_scale(&m, a[0], n >= 2 ? a[1] : a[0]);
        } else if (nlen == 6 && g_ascii_strncasecmp(name, "rotate", 6) == 0 && n >= 1) {
            if (n >= 3) {
                cairo_matrix_init_translate(&m, a[1], a[2]);
                cairo_matrix_rotate(&m, a[0] * G_PI / 180.0);
                cairo_matrix_translate(&m, -a[1], -a[2]);
            } else {
                cairo_matrix_init_rotate(&m, a[0] * G_PI / 180.0);
            }
        } else if (nlen == 5 && g_ascii_strncasecmp(name, "skewX", 5) == 0 && n >= 1) {
            cairo_matrix_init(&m, 1, 0, tan(a[0] * G_PI / 180.0), 1, 0, 0);
        } else if (nlen == 5 && g_ascii_strncasecmp(name, "skewY", 5) == 0 && n >= 1) {
            cairo_matrix_init(&m, 1, tan(a[0] * G_PI / 180.0), 0, 1, 0, 0);
        } else {
            continue;
        }
        cairo_matrix_multiply(out, &m, out);
        any = TRUE;
    }
    return any;
}

static void
svg_apply_transform_attr(svg_ctx *ctx, const ns_node *n, const char *attr)
{
    const char *t = ns_element_get_attr(n, attr);
    if (!t) return;
    cairo_matrix_t m;
    if (!svg_parse_transform(t, &m)) return;
    cairo_transform(ctx->cr, &m);
}

static void
svg_arc_to(cairo_t *cr, double x1, double y1, double rx, double ry,
           double phi_deg, gboolean large_arc, gboolean sweep,
           double x2, double y2)
{
    if (rx == 0.0 || ry == 0.0) { cairo_line_to(cr, x2, y2); return; }
    rx = fabs(rx);
    ry = fabs(ry);
    double phi = phi_deg * G_PI / 180.0;
    double cosp = cos(phi), sinp = sin(phi);
    double dx2 = (x1 - x2) / 2.0, dy2 = (y1 - y2) / 2.0;
    double x1p =  cosp * dx2 + sinp * dy2;
    double y1p = -sinp * dx2 + cosp * dy2;

    double lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lam > 1.0) {
        double k = sqrt(lam);
        rx *= k;
        ry *= k;
    }

    double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    double num = rx * rx * ry * ry - den;
    double co = 0.0;
    if (den > 0.0 && num > 0.0) co = sqrt(num / den);
    if (large_arc == sweep) co = -co;

    double cxp =  co * rx * y1p / ry;
    double cyp = -co * ry * x1p / rx;
    double cx = cosp * cxp - sinp * cyp + (x1 + x2) / 2.0;
    double cy = sinp * cxp + cosp * cyp + (y1 + y2) / 2.0;

    double t1 = atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double t2 = atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
    double dt = t2 - t1;
    if (!sweep && dt > 0.0)      dt -= 2.0 * G_PI;
    else if (sweep && dt < 0.0)  dt += 2.0 * G_PI;

    int segs = (int)ceil(fabs(dt) / (G_PI / 2.0) - 1e-9);
    if (segs < 1) segs = 1;
    if (segs > 8) segs = 8;
    double seg = dt / segs;
    double alpha = 4.0 / 3.0 * tan(seg / 4.0);

    for (int i = 0; i < segs; i++) {
        double a1 = t1 + i * seg;
        double a2 = a1 + seg;
        double ca1 = cos(a1), sa1 = sin(a1);
        double ca2 = cos(a2), sa2 = sin(a2);
        double px1 = cx + rx * cosp * ca1 - ry * sinp * sa1;
        double py1 = cy + rx * sinp * ca1 + ry * cosp * sa1;
        double px2 = cx + rx * cosp * ca2 - ry * sinp * sa2;
        double py2 = cy + rx * sinp * ca2 + ry * cosp * sa2;
        double d1x = -rx * cosp * sa1 - ry * sinp * ca1;
        double d1y = -rx * sinp * sa1 + ry * cosp * ca1;
        double d2x = -rx * cosp * sa2 - ry * sinp * ca2;
        double d2y = -rx * sinp * sa2 + ry * cosp * ca2;
        cairo_curve_to(cr,
                       px1 + alpha * d1x, py1 + alpha * d1y,
                       px2 - alpha * d2x, py2 - alpha * d2y,
                       px2, py2);
    }
}

static void
svg_path_data(cairo_t *cr, const char *d)
{
    if (!d) return;
    const char *p = d;
    double cx = 0, cy = 0, sx = 0, sy = 0;
    double ctrl_x = 0, ctrl_y = 0;
    char cmd = 0, prev = 0;
    gboolean started = FALSE;

    for (;;) {
        svg_skip_sep(&p);
        if (!*p) break;
        if (g_ascii_isalpha(*p)) {
            cmd = *p;
            p++;
        } else if (cmd == 'M') {
            cmd = 'L';
        } else if (cmd == 'm') {
            cmd = 'l';
        } else if (!cmd) {
            break;
        }

        gboolean rel = g_ascii_islower(cmd);
        char c = g_ascii_toupper(cmd);
        double a[7];

        switch (c) {
        case 'M':
            if (!svg_num(&p, &a[0]) || !svg_num(&p, &a[1])) return;
            if (rel && started) { a[0] += cx; a[1] += cy; }
            cx = a[0]; cy = a[1];
            sx = cx; sy = cy;
            cairo_move_to(cr, cx, cy);
            started = TRUE;
            break;
        case 'L':
            if (!svg_num(&p, &a[0]) || !svg_num(&p, &a[1])) return;
            if (rel) { a[0] += cx; a[1] += cy; }
            cx = a[0]; cy = a[1];
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            else cairo_line_to(cr, cx, cy);
            break;
        case 'H':
            if (!svg_num(&p, &a[0])) return;
            if (rel) a[0] += cx;
            cx = a[0];
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            else cairo_line_to(cr, cx, cy);
            break;
        case 'V':
            if (!svg_num(&p, &a[0])) return;
            if (rel) a[0] += cy;
            cy = a[0];
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            else cairo_line_to(cr, cx, cy);
            break;
        case 'C':
            for (int i = 0; i < 6; i++) if (!svg_num(&p, &a[i])) return;
            if (rel) { a[0] += cx; a[1] += cy; a[2] += cx; a[3] += cy; a[4] += cx; a[5] += cy; }
            if (!started) { cairo_move_to(cr, a[0], a[1]); sx = a[0]; sy = a[1]; started = TRUE; }
            cairo_curve_to(cr, a[0], a[1], a[2], a[3], a[4], a[5]);
            ctrl_x = a[2]; ctrl_y = a[3];
            cx = a[4]; cy = a[5];
            break;
        case 'S': {
            for (int i = 0; i < 4; i++) if (!svg_num(&p, &a[i])) return;
            if (rel) { a[0] += cx; a[1] += cy; a[2] += cx; a[3] += cy; }
            char pc = g_ascii_toupper(prev);
            double rx = (pc == 'C' || pc == 'S') ? 2 * cx - ctrl_x : cx;
            double ry = (pc == 'C' || pc == 'S') ? 2 * cy - ctrl_y : cy;
            if (!started) { cairo_move_to(cr, rx, ry); sx = rx; sy = ry; started = TRUE; }
            cairo_curve_to(cr, rx, ry, a[0], a[1], a[2], a[3]);
            ctrl_x = a[0]; ctrl_y = a[1];
            cx = a[2]; cy = a[3];
            break;
        }
        case 'Q': {
            for (int i = 0; i < 4; i++) if (!svg_num(&p, &a[i])) return;
            if (rel) { a[0] += cx; a[1] += cy; a[2] += cx; a[3] += cy; }
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            double c1x = cx + 2.0 / 3.0 * (a[0] - cx);
            double c1y = cy + 2.0 / 3.0 * (a[1] - cy);
            double c2x = a[2] + 2.0 / 3.0 * (a[0] - a[2]);
            double c2y = a[3] + 2.0 / 3.0 * (a[1] - a[3]);
            cairo_curve_to(cr, c1x, c1y, c2x, c2y, a[2], a[3]);
            ctrl_x = a[0]; ctrl_y = a[1];
            cx = a[2]; cy = a[3];
            break;
        }
        case 'T': {
            for (int i = 0; i < 2; i++) if (!svg_num(&p, &a[i])) return;
            if (rel) { a[0] += cx; a[1] += cy; }
            char pc = g_ascii_toupper(prev);
            double qx = (pc == 'Q' || pc == 'T') ? 2 * cx - ctrl_x : cx;
            double qy = (pc == 'Q' || pc == 'T') ? 2 * cy - ctrl_y : cy;
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            double c1x = cx + 2.0 / 3.0 * (qx - cx);
            double c1y = cy + 2.0 / 3.0 * (qy - cy);
            double c2x = a[0] + 2.0 / 3.0 * (qx - a[0]);
            double c2y = a[1] + 2.0 / 3.0 * (qy - a[1]);
            cairo_curve_to(cr, c1x, c1y, c2x, c2y, a[0], a[1]);
            ctrl_x = qx; ctrl_y = qy;
            cx = a[0]; cy = a[1];
            break;
        }
        case 'A': {
            gboolean large = FALSE, sweep = FALSE;
            if (!svg_num(&p, &a[0]) || !svg_num(&p, &a[1]) || !svg_num(&p, &a[2]))
                return;
            if (!svg_flag(&p, &large) || !svg_flag(&p, &sweep)) return;
            if (!svg_num(&p, &a[5]) || !svg_num(&p, &a[6])) return;
            if (rel) { a[5] += cx; a[6] += cy; }
            if (!started) { cairo_move_to(cr, cx, cy); sx = cx; sy = cy; started = TRUE; }
            svg_arc_to(cr, cx, cy, a[0], a[1], a[2], large, sweep, a[5], a[6]);
            cx = a[5]; cy = a[6];
            break;
        }
        case 'Z':
            if (started) cairo_close_path(cr);
            cx = sx; cy = sy;
            break;
        default:
            return;
        }
        prev = cmd;
    }
}

static double
svg_geom(svg_ctx *ctx, const ns_node *n, const char *attr, ns_css_prop prop,
         double basis, double fs, double fallback, gboolean *out_auto)
{
    if (out_auto) *out_auto = FALSE;
    const ns_style *s = ctx->styles
        ? g_hash_table_lookup(ctx->styles, (gpointer)n) : NULL;
    const ns_css_value *v = s ? s->values[prop] : NULL;
    if (v) {
        if (v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
            g_ascii_strcasecmp(v->u.keyword, "auto") == 0) {
            if (out_auto) *out_auto = TRUE;
            return fallback;
        }
        if (v->kind == NS_CSS_V_LENGTH || v->kind == NS_CSS_V_CALC)
            return svg_css_number(s, prop, basis, fallback);
    }
    const char *a = svg_prop(n, attr);
    if (!a) return fallback;
    if (g_ascii_strcasecmp(a, "auto") == 0) {
        if (out_auto) *out_auto = TRUE;
        return fallback;
    }
    return svg_length_str(a, basis, fs, fallback);
}

static gboolean
svg_shape_path(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    cairo_t *cr = ctx->cr;
    const char *tag = n->name;
    double fs = st->font_size;

    if (strcmp(tag, "path") == 0) {
        const char *d = ns_element_get_attr(n, "d");
        if (!d || !*d) return FALSE;
        svg_path_data(cr, d);
        return TRUE;
    }
    if (strcmp(tag, "rect") == 0) {
        double x = svg_geom(ctx, n, "x", NS_CSS_SVG_X, ctx->vw, fs, 0, NULL);
        double y = svg_geom(ctx, n, "y", NS_CSS_SVG_Y, ctx->vh, fs, 0, NULL);
        double w = svg_geom(ctx, n, "width", NS_CSS_WIDTH, ctx->vw, fs, 0, NULL);
        double h = svg_geom(ctx, n, "height", NS_CSS_HEIGHT, ctx->vh, fs, 0, NULL);
        if (w <= 0 || h <= 0) return FALSE;
        gboolean rx_auto = FALSE, ry_auto = FALSE;
        double rx = svg_geom(ctx, n, "rx", NS_CSS_RX, ctx->vw, fs, -1, &rx_auto);
        double ry = svg_geom(ctx, n, "ry", NS_CSS_RY, ctx->vh, fs, -1, &ry_auto);
        if (rx_auto) rx = -1;
        if (ry_auto) ry = -1;
        if (rx < 0 && ry < 0) { rx = ry = 0; }
        else if (rx < 0) rx = ry;
        else if (ry < 0) ry = rx;
        rx = CLAMP(rx, 0, w / 2);
        ry = CLAMP(ry, 0, h / 2);
        if (rx <= 0 || ry <= 0) {
            cairo_rectangle(cr, x, y, w, h);
            return TRUE;
        }
        cairo_save(cr);
        cairo_new_sub_path(cr);
        cairo_translate(cr, x + rx, y + ry);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, G_PI, 1.5 * G_PI);
        cairo_restore(cr);
        cairo_save(cr);
        cairo_translate(cr, x + w - rx, y + ry);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, 1.5 * G_PI, 2.0 * G_PI);
        cairo_restore(cr);
        cairo_save(cr);
        cairo_translate(cr, x + w - rx, y + h - ry);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, 0, 0.5 * G_PI);
        cairo_restore(cr);
        cairo_save(cr);
        cairo_translate(cr, x + rx, y + h - ry);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, 0.5 * G_PI, G_PI);
        cairo_restore(cr);
        cairo_close_path(cr);
        return TRUE;
    }
    if (strcmp(tag, "circle") == 0) {
        double cx = svg_geom(ctx, n, "cx", NS_CSS_CX, ctx->vw, fs, 0, NULL);
        double cy = svg_geom(ctx, n, "cy", NS_CSS_CY, ctx->vh, fs, 0, NULL);
        double r  = svg_geom(ctx, n, "r", NS_CSS_R,
                             svg_diag_basis(ctx->vw, ctx->vh), fs, 0, NULL);
        if (r <= 0) return FALSE;
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
        cairo_close_path(cr);
        return TRUE;
    }
    if (strcmp(tag, "ellipse") == 0) {
        double cx = svg_geom(ctx, n, "cx", NS_CSS_CX, ctx->vw, fs, 0, NULL);
        double cy = svg_geom(ctx, n, "cy", NS_CSS_CY, ctx->vh, fs, 0, NULL);
        gboolean rx_auto = FALSE, ry_auto = FALSE;
        double rx = svg_geom(ctx, n, "rx", NS_CSS_RX, ctx->vw, fs, -1, &rx_auto);
        double ry = svg_geom(ctx, n, "ry", NS_CSS_RY, ctx->vh, fs, -1, &ry_auto);
        if (rx_auto) rx = -1;
        if (ry_auto) ry = -1;
        if (rx < 0 && ry < 0) return FALSE;
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx <= 0 || ry <= 0) return FALSE;
        cairo_save(cr);
        cairo_new_sub_path(cr);
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, rx, ry);
        cairo_arc(cr, 0, 0, 1.0, 0, 2 * G_PI);
        cairo_restore(cr);
        cairo_close_path(cr);
        return TRUE;
    }
    if (strcmp(tag, "line") == 0) {
        double x1 = svg_attr_length(n, "x1", ctx->vw, fs, 0);
        double y1 = svg_attr_length(n, "y1", ctx->vh, fs, 0);
        double x2 = svg_attr_length(n, "x2", ctx->vw, fs, 0);
        double y2 = svg_attr_length(n, "y2", ctx->vh, fs, 0);
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
        return TRUE;
    }
    if (strcmp(tag, "polyline") == 0 || strcmp(tag, "polygon") == 0) {
        const char *pts = ns_element_get_attr(n, "points");
        if (!pts) return FALSE;
        const char *p = pts;
        double x, y;
        gboolean first = TRUE;
        while (svg_num(&p, &x) && svg_num(&p, &y)) {
            if (first) { cairo_move_to(cr, x, y); first = FALSE; }
            else cairo_line_to(cr, x, y);
        }
        if (first) return FALSE;
        if (strcmp(tag, "polygon") == 0) cairo_close_path(cr);
        return TRUE;
    }
    return FALSE;
}

static void
svg_index_ids(svg_ctx *ctx, const ns_node *n)
{
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT) continue;
        const char *id = ns_element_get_attr(c, "id");
        if (id && *id && !g_hash_table_contains(ctx->ids, id))
            g_hash_table_insert(ctx->ids, (gpointer)id, (gpointer)c);
        svg_index_ids(ctx, c);
    }
}

static const ns_node *
svg_by_id(svg_ctx *ctx, const char *id)
{
    if (!id || !*id) return NULL;
    if (!ctx->ids) {
        ctx->ids = g_hash_table_new(g_str_hash, g_str_equal);
        const char *rid = ns_element_get_attr(ctx->root, "id");
        if (rid && *rid) g_hash_table_insert(ctx->ids, (gpointer)rid,
                                             (gpointer)ctx->root);
        svg_index_ids(ctx, ctx->root);
    }
    return g_hash_table_lookup(ctx->ids, id);
}

static const char *
svg_href(const ns_node *n)
{
    const char *h = ns_element_get_attr(n, "href");
    if (!h) h = ns_element_get_attr(n, "xlink:href");
    return h;
}

static const ns_node *
svg_href_target(svg_ctx *ctx, const ns_node *n)
{
    const char *h = svg_href(n);
    if (!h) return NULL;
    while (*h && svg_is_ws(*h)) h++;
    if (*h != '#') return NULL;
    return svg_by_id(ctx, h + 1);
}

static const char *
svg_inherited_attr(svg_ctx *ctx, const ns_node *n, const char *name)
{
    const ns_node *cur = n;
    for (int i = 0; cur && i < NS_SVG_MAX_HREF_CHAIN; i++) {
        const char *v = ns_element_get_attr(cur, name);
        if (v) return v;
        cur = svg_href_target(ctx, cur);
    }
    return NULL;
}

static const ns_node *
svg_stops_owner(svg_ctx *ctx, const ns_node *n)
{
    const ns_node *cur = n;
    for (int i = 0; cur && i < NS_SVG_MAX_HREF_CHAIN; i++) {
        for (const ns_node *c = cur->first_child; c; c = c->next_sibling)
            if (c->kind == NS_NODE_ELEMENT && c->name &&
                strcmp(c->name, "stop") == 0)
                return cur;
        cur = svg_href_target(ctx, cur);
    }
    return NULL;
}

static void
svg_add_stops(svg_ctx *ctx, cairo_pattern_t *pat, const ns_node *owner,
              const svg_state *st, double alpha)
{
    double last = 0.0;
    for (const ns_node *c = owner->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name || strcmp(c->name, "stop") != 0)
            continue;
        const char *os = svg_prop(c, "offset");
        double off = os ? svg_length_str(os, 1.0, st->font_size, 0.0) : 0.0;
        if (os && strchr(os, '%') == NULL && off > 1.0) off = 1.0;
        off = CLAMP(off, 0.0, 1.0);
        if (off < last) off = last;
        last = off;

        double r = 0, g = 0, b = 0, a = 1.0;
        const ns_style *cs = ctx->styles
            ? g_hash_table_lookup(ctx->styles, (gpointer)c) : NULL;
        const ns_css_value *sv = cs ? cs->values[NS_CSS_STOP_COLOR] : NULL;
        gboolean got = FALSE;
        if (sv && sv->kind == NS_CSS_V_COLOR) {
            r = sv->u.color.r / 255.0; g = sv->u.color.g / 255.0;
            b = sv->u.color.b / 255.0; a = sv->u.color.a / 255.0;
            got = TRUE;
        }
        if (!got) {
            const char *sc = svg_prop(c, "stop-color");
            guint8 cr8, cg8, cb8, ca8;
            if (sc && g_ascii_strcasecmp(sc, "currentcolor") == 0) {
                r = st->color_r; g = st->color_g; b = st->color_b; a = st->color_a;
            } else if (sc && ns_css_parse_color(sc, &cr8, &cg8, &cb8, &ca8)) {
                r = cr8 / 255.0; g = cg8 / 255.0; b = cb8 / 255.0; a = ca8 / 255.0;
            }
        }
        double so = 1.0;
        const ns_css_value *ov = cs ? cs->values[NS_CSS_STOP_OPACITY] : NULL;
        if (ov) so = CLAMP(svg_css_number(cs, NS_CSS_STOP_OPACITY, 1.0, 1.0), 0.0, 1.0);
        else {
            const char *sos = svg_prop(c, "stop-opacity");
            if (sos) so = CLAMP(svg_length_str(sos, 1.0, st->font_size, 1.0), 0.0, 1.0);
        }
        cairo_pattern_add_color_stop_rgba(pat, off, r, g, b, a * so * alpha);
    }
}

static cairo_extend_t
svg_extend_of(const char *s)
{
    if (!s) return CAIRO_EXTEND_PAD;
    if (g_ascii_strcasecmp(s, "reflect") == 0) return CAIRO_EXTEND_REFLECT;
    if (g_ascii_strcasecmp(s, "repeat") == 0)  return CAIRO_EXTEND_REPEAT;
    return CAIRO_EXTEND_PAD;
}

static cairo_pattern_t *
svg_gradient_pattern(svg_ctx *ctx, const ns_node *g, const svg_state *st,
                     double alpha, double bx, double by, double bw, double bh)
{
    gboolean linear = strcmp(g->name, "linearGradient") == 0;
    const char *units = svg_inherited_attr(ctx, g, "gradientUnits");
    gboolean obb = !units || g_ascii_strcasecmp(units, "userSpaceOnUse") != 0;
    double bw_ref = obb ? 1.0 : ctx->vw;
    double bh_ref = obb ? 1.0 : ctx->vh;
    double dref   = obb ? 1.0 : svg_diag_basis(ctx->vw, ctx->vh);

    cairo_pattern_t *pat = NULL;
    if (linear) {
        const char *s;
        double x1 = (s = svg_inherited_attr(ctx, g, "x1"))
            ? svg_length_str(s, bw_ref, st->font_size, 0) : 0.0;
        double y1 = (s = svg_inherited_attr(ctx, g, "y1"))
            ? svg_length_str(s, bh_ref, st->font_size, 0) : 0.0;
        double x2 = (s = svg_inherited_attr(ctx, g, "x2"))
            ? svg_length_str(s, bw_ref, st->font_size, bw_ref) : bw_ref;
        double y2 = (s = svg_inherited_attr(ctx, g, "y2"))
            ? svg_length_str(s, bh_ref, st->font_size, 0) : 0.0;
        pat = cairo_pattern_create_linear(x1, y1, x2, y2);
    } else {
        const char *s;
        double cx = (s = svg_inherited_attr(ctx, g, "cx"))
            ? svg_length_str(s, bw_ref, st->font_size, bw_ref * 0.5) : bw_ref * 0.5;
        double cy = (s = svg_inherited_attr(ctx, g, "cy"))
            ? svg_length_str(s, bh_ref, st->font_size, bh_ref * 0.5) : bh_ref * 0.5;
        double r  = (s = svg_inherited_attr(ctx, g, "r"))
            ? svg_length_str(s, dref, st->font_size, dref * 0.5) : dref * 0.5;
        double fx = (s = svg_inherited_attr(ctx, g, "fx"))
            ? svg_length_str(s, bw_ref, st->font_size, cx) : cx;
        double fy = (s = svg_inherited_attr(ctx, g, "fy"))
            ? svg_length_str(s, bh_ref, st->font_size, cy) : cy;
        if (r <= 0) return NULL;
        double dx = fx - cx, dy = fy - cy;
        double dist = sqrt(dx * dx + dy * dy);
        if (dist > r * 0.999) {
            double k = r * 0.999 / dist;
            fx = cx + dx * k;
            fy = cy + dy * k;
        }
        pat = cairo_pattern_create_radial(fx, fy, 0.0, cx, cy, r);
    }
    if (!pat) return NULL;

    const ns_node *owner = svg_stops_owner(ctx, g);
    if (!owner) {
        cairo_pattern_destroy(pat);
        return NULL;
    }
    svg_add_stops(ctx, pat, owner, st, alpha);
    cairo_pattern_set_extend(pat, svg_extend_of(
        svg_inherited_attr(ctx, g, "spreadMethod")));

    cairo_matrix_t m;
    cairo_matrix_init_identity(&m);
    if (obb) {
        if (bw <= 0 || bh <= 0) { cairo_pattern_destroy(pat); return NULL; }
        cairo_matrix_init_translate(&m, bx, by);
        cairo_matrix_scale(&m, bw, bh);
    }
    const char *gt = svg_inherited_attr(ctx, g, "gradientTransform");
    if (gt) {
        cairo_matrix_t t;
        if (svg_parse_transform(gt, &t)) cairo_matrix_multiply(&m, &t, &m);
    }
    if (cairo_matrix_invert(&m) != CAIRO_STATUS_SUCCESS) {
        cairo_pattern_destroy(pat);
        return NULL;
    }
    cairo_pattern_set_matrix(pat, &m);
    return pat;
}

static gboolean
svg_set_paint(svg_ctx *ctx, const svg_paint *paint, const svg_state *st,
              double opacity)
{
    cairo_t *cr = ctx->cr;
    if (paint->kind == SVG_PAINT_NONE) return FALSE;
    if (paint->kind == SVG_PAINT_COLOR) {
        if (paint->a * opacity <= 0.0) return FALSE;
        cairo_set_source_rgba(cr, paint->r, paint->g, paint->b,
                              paint->a * opacity);
        return TRUE;
    }

    const ns_node *g = svg_by_id(ctx, paint->ref);
    if (g && g->name &&
        (strcmp(g->name, "linearGradient") == 0 ||
         strcmp(g->name, "radialGradient") == 0)) {
        double bx, by, bx2, by2;
        cairo_path_extents(cr, &bx, &by, &bx2, &by2);
        cairo_pattern_t *pat = svg_gradient_pattern(ctx, g, st, opacity,
                                                    bx, by, bx2 - bx, by2 - by);
        if (pat) {
            cairo_set_source(cr, pat);
            cairo_pattern_destroy(pat);
            return TRUE;
        }
    }
    if (paint->have_fallback) {
        if (paint->fa * opacity <= 0.0) return FALSE;
        cairo_set_source_rgba(cr, paint->fr, paint->fg, paint->fb,
                              paint->fa * opacity);
        return TRUE;
    }
    return FALSE;
}

static void
svg_apply_stroke_params(svg_ctx *ctx, const svg_state *st)
{
    cairo_t *cr = ctx->cr;
    cairo_set_line_width(cr, st->stroke_width);
    cairo_set_line_cap(cr, st->line_cap);
    cairo_set_line_join(cr, st->line_join);
    cairo_set_miter_limit(cr, st->miter_limit);
    if (st->dashes && st->dashes->len > 0)
        cairo_set_dash(cr, &g_array_index(st->dashes, double, 0),
                       (int)st->dashes->len, st->dash_offset);
    else
        cairo_set_dash(cr, NULL, 0, 0.0);
}

static void
svg_paint_current_path(svg_ctx *ctx, const svg_state *st)
{
    cairo_t *cr = ctx->cr;
    gboolean do_stroke = st->stroke.kind != SVG_PAINT_NONE && st->stroke_width > 0;

    for (int pass = 0; pass < 2; pass++) {
        gboolean stroking = st->stroke_first ? (pass == 0) : (pass == 1);
        if (stroking) {
            if (!do_stroke) continue;
            if (!svg_set_paint(ctx, &st->stroke, st, st->stroke_opacity)) continue;
            if (st->non_scaling_stroke) {
                cairo_matrix_t ctm;
                cairo_get_matrix(cr, &ctm);
                cairo_identity_matrix(cr);
                svg_apply_stroke_params(ctx, st);
                cairo_stroke_preserve(cr);
                cairo_set_matrix(cr, &ctm);
            } else {
                svg_apply_stroke_params(ctx, st);
                cairo_stroke_preserve(cr);
            }
        } else {
            cairo_set_fill_rule(cr, st->fill_rule);
            if (!svg_set_paint(ctx, &st->fill, st, st->fill_opacity)) continue;
            cairo_fill_preserve(cr);
        }
    }
    cairo_new_path(cr);
}

static void
svg_clip_path_children(svg_ctx *ctx, const ns_node *clip, const svg_state *st)
{
    for (const ns_node *c = clip->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "use") == 0) {
            const ns_node *t = svg_href_target(ctx, c);
            if (t && ctx->depth < NS_SVG_MAX_DEPTH) {
                ctx->depth++;
                svg_clip_path_children(ctx, c, st);
                cairo_save(ctx->cr);
                svg_apply_transform_attr(ctx, c, "transform");
                if (svg_shape_path(ctx, t, st)) { }
                cairo_restore(ctx->cr);
                ctx->depth--;
            }
            continue;
        }
        cairo_save(ctx->cr);
        svg_apply_transform_attr(ctx, c, "transform");
        svg_shape_path(ctx, c, st);
        cairo_restore(ctx->cr);
    }
}

static cairo_surface_t *
svg_mask_surface(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    const char *mv = svg_prop(n, "mask");
    if (!mv) return NULL;
    char *id = svg_url_id(mv, NULL);
    if (!id) return NULL;
    const ns_node *mask = svg_by_id(ctx, id);
    g_free(id);
    if (!mask || !mask->name || strcmp(mask->name, "mask") != 0) return NULL;
    if (ctx->depth >= NS_SVG_MAX_DEPTH) return NULL;

    cairo_surface_t *target = cairo_get_target(ctx->cr);
    double ox = 0, oy = 0;
    cairo_surface_get_device_offset(target, &ox, &oy);
    double cx1, cy1, cx2, cy2;
    cairo_clip_extents(ctx->cr, &cx1, &cy1, &cx2, &cy2);
    cairo_matrix_t ctm;
    cairo_get_matrix(ctx->cr, &ctm);
    cairo_matrix_transform_point(&ctm, &cx1, &cy1);
    cairo_matrix_transform_point(&ctm, &cx2, &cy2);
    int w = (int)ceil(MAX(cx1, cx2) + ox);
    int h = (int)ceil(MAX(cy1, cy2) + oy);
    if (w <= 0 || h <= 0 || (double)w * h > (double)NS_SVG_MAX_PIXELS) return NULL;

    cairo_surface_t *rgb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(rgb) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(rgb);
        return NULL;
    }
    cairo_t *mcr = cairo_create(rgb);
    cairo_set_matrix(mcr, &ctm);

    cairo_t *saved = ctx->cr;
    ctx->cr = mcr;
    ctx->depth++;
    svg_state ms;
    svg_state_copy(&ms, st);
    svg_paint_clear(&ms.fill);
    ms.fill.kind = SVG_PAINT_COLOR;
    ms.fill.r = ms.fill.g = ms.fill.b = ms.fill.a = 1.0;
    for (const ns_node *c = mask->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT) svg_render_node(ctx, c, &ms);
    svg_state_clear(&ms);
    ctx->depth--;
    ctx->cr = saved;
    cairo_destroy(mcr);
    cairo_surface_flush(rgb);

    cairo_surface_t *a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    if (cairo_surface_status(a8) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(rgb);
        cairo_surface_destroy(a8);
        return NULL;
    }
    const unsigned char *src = cairo_image_surface_get_data(rgb);
    unsigned char *dst = cairo_image_surface_get_data(a8);
    int sstride = cairo_image_surface_get_stride(rgb);
    int dstride = cairo_image_surface_get_stride(a8);
    for (int y = 0; y < h; y++) {
        const guint32 *srow = (const guint32 *)(gconstpointer)(src + (gsize)y * sstride);
        unsigned char *drow = dst + (gsize)y * dstride;
        for (int x = 0; x < w; x++) {
            guint32 px = srow[x];
            double r = ((px >> 16) & 0xff) / 255.0;
            double g = ((px >> 8) & 0xff) / 255.0;
            double b = (px & 0xff) / 255.0;
            double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            drow[x] = (unsigned char)CLAMP(lum * 255.0, 0.0, 255.0);
        }
    }
    cairo_surface_mark_dirty(a8);
    cairo_surface_destroy(rgb);
    return a8;
}

static void
svg_apply_clip(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    const char *cp = svg_prop(n, "clip-path");
    if (!cp) return;
    char *id = svg_url_id(cp, NULL);
    if (!id) return;
    const ns_node *clip = svg_by_id(ctx, id);
    g_free(id);
    if (!clip || !clip->name || strcmp(clip->name, "clipPath") != 0) return;
    if (ctx->depth >= NS_SVG_MAX_DEPTH) return;

    ctx->depth++;
    cairo_save(ctx->cr);
    const char *units = ns_element_get_attr(clip, "clipPathUnits");
    gboolean obb = units && g_ascii_strcasecmp(units, "objectBoundingBox") == 0;
    if (obb) {
        cairo_matrix_t m;
        cairo_matrix_init_scale(&m, ctx->vw, ctx->vh);
        cairo_transform(ctx->cr, &m);
    }
    cairo_new_path(ctx->cr);
    svg_clip_path_children(ctx, clip, st);
    cairo_restore(ctx->cr);
    cairo_set_fill_rule(ctx->cr, st->clip_rule);
    cairo_clip(ctx->cr);
    cairo_new_path(ctx->cr);
    ctx->depth--;
}

static void
svg_viewbox_matrix(const ns_node *n, double vw, double vh, cairo_matrix_t *out)
{
    cairo_matrix_init_identity(out);
    const char *vb = ns_element_get_attr(n, "viewBox");
    if (!vb) return;
    const char *p = vb;
    double x, y, w, h;
    if (!svg_num(&p, &x) || !svg_num(&p, &y) ||
        !svg_num(&p, &w) || !svg_num(&p, &h)) return;
    if (w <= 0 || h <= 0 || vw <= 0 || vh <= 0) return;

    const char *par = ns_element_get_attr(n, "preserveAspectRatio");
    gboolean slice = FALSE;
    gboolean none = FALSE;
    double ax = 0.5, ay = 0.5;
    if (par) {
        char **parts = g_strsplit_set(par, " \t\r\n", -1);
        for (int i = 0; parts[i]; i++) {
            if (!*parts[i]) continue;
            if (g_ascii_strcasecmp(parts[i], "none") == 0) none = TRUE;
            else if (g_ascii_strcasecmp(parts[i], "slice") == 0) slice = TRUE;
            else if (g_ascii_strncasecmp(parts[i], "x", 1) == 0) {
                if (strstr(parts[i], "xMin")) ax = 0.0;
                else if (strstr(parts[i], "xMax")) ax = 1.0;
                if (strstr(parts[i], "YMin")) ay = 0.0;
                else if (strstr(parts[i], "YMax")) ay = 1.0;
            }
        }
        g_strfreev(parts);
    }

    double sx = vw / w, sy = vh / h;
    if (!none) {
        double s = slice ? MAX(sx, sy) : MIN(sx, sy);
        sx = sy = s;
    }
    double tx = (vw - w * sx) * ax;
    double ty = (vh - h * sy) * ay;
    cairo_matrix_init_translate(out, tx, ty);
    cairo_matrix_scale(out, sx, sy);
    cairo_matrix_translate(out, -x, -y);
}

static void
svg_render_text(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    char *text = ns_node_collect_text(n);
    if (!text || !*text) { g_free(text); return; }

    gsize len = strlen(text);
    GString *flat = g_string_sized_new(len);
    gboolean prev_ws = TRUE;
    for (gsize i = 0; i < len; i++) {
        char c = text[i];
        if (svg_is_ws(c)) {
            if (!prev_ws) g_string_append_c(flat, ' ');
            prev_ws = TRUE;
        } else {
            g_string_append_c(flat, c);
            prev_ws = FALSE;
        }
    }
    g_free(text);
    while (flat->len > 0 && flat->str[flat->len - 1] == ' ')
        g_string_truncate(flat, flat->len - 1);
    if (flat->len == 0) { g_string_free(flat, TRUE); return; }

    double x = svg_attr_length(n, "x", ctx->vw, st->font_size, 0);
    double y = svg_attr_length(n, "y", ctx->vh, st->font_size, 0);
    x += svg_attr_length(n, "dx", ctx->vw, st->font_size, 0);
    y += svg_attr_length(n, "dy", ctx->vh, st->font_size, 0);

    cairo_t *cr = ctx->cr;
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();
    char *fam = st->font_family
        ? ns_css_font_family_for_pango(st->font_family) : NULL;
    pango_font_description_set_family(desc, fam ? fam : "sans-serif");
    g_free(fam);
    pango_font_description_set_absolute_size(desc, st->font_size * PANGO_SCALE);
    pango_font_description_set_weight(desc, (PangoWeight)st->font_weight);
    if (st->font_italic)
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_text(layout, flat->str, (int)flat->len);
    g_string_free(flat, TRUE);

    int wpx = 0, hpx = 0;
    pango_layout_get_pixel_size(layout, &wpx, &hpx);
    if (st->text_anchor == 1)      x -= wpx / 2.0;
    else if (st->text_anchor == 2) x -= wpx;
    int baseline = pango_layout_get_baseline(layout) / PANGO_SCALE;

    cairo_move_to(cr, x, y - baseline);
    pango_cairo_layout_path(cr, layout);
    g_object_unref(layout);
    svg_paint_current_path(ctx, st);
}

typedef struct {
    double x, y;
    double in_angle, out_angle;
    gboolean has_in, has_out;
} svg_vertex;

static void
svg_vertex_add(GArray *out, double x, double y)
{
    svg_vertex v = { .x = x, .y = y };
    g_array_append_val(out, v);
}

static void
svg_vertex_link(GArray *out, double fromx, double fromy, double tox, double toy)
{
    if (out->len == 0) return;
    svg_vertex *prev = &g_array_index(out, svg_vertex, out->len - 1);
    double dx = tox - fromx, dy = toy - fromy;
    if (dx == 0 && dy == 0) return;
    double a = atan2(dy, dx);
    if (!prev->has_out) { prev->out_angle = a; prev->has_out = TRUE; }
}

static GArray *
svg_path_vertices(cairo_t *cr)
{
    cairo_path_t *path = cairo_copy_path(cr);
    if (!path) return NULL;
    if (path->status != CAIRO_STATUS_SUCCESS) {
        cairo_path_destroy(path);
        return NULL;
    }
    GArray *out = g_array_new(FALSE, TRUE, sizeof(svg_vertex));
    double cx = 0, cy = 0, sx = 0, sy = 0;
    for (int i = 0; i < path->num_data; i += path->data[i].header.length) {
        cairo_path_data_t *d = &path->data[i];
        switch (d->header.type) {
        case CAIRO_PATH_MOVE_TO:
            cx = sx = d[1].point.x;
            cy = sy = d[1].point.y;
            svg_vertex_add(out, cx, cy);
            break;
        case CAIRO_PATH_LINE_TO:
            svg_vertex_link(out, cx, cy, d[1].point.x, d[1].point.y);
            svg_vertex_add(out, d[1].point.x, d[1].point.y);
            if (out->len >= 2) {
                svg_vertex *v = &g_array_index(out, svg_vertex, out->len - 1);
                if (d[1].point.x != cx || d[1].point.y != cy) {
                    v->in_angle = atan2(d[1].point.y - cy, d[1].point.x - cx);
                    v->has_in = TRUE;
                }
            }
            cx = d[1].point.x; cy = d[1].point.y;
            break;
        case CAIRO_PATH_CURVE_TO: {
            double c1x = d[1].point.x, c1y = d[1].point.y;
            double c2x = d[2].point.x, c2y = d[2].point.y;
            double ex = d[3].point.x, ey = d[3].point.y;
            svg_vertex_link(out, cx, cy, c1x, c1y);
            svg_vertex_add(out, ex, ey);
            svg_vertex *v = &g_array_index(out, svg_vertex, out->len - 1);
            double tx = ex - c2x, ty = ey - c2y;
            if (tx == 0 && ty == 0) { tx = ex - c1x; ty = ey - c1y; }
            if (tx == 0 && ty == 0) { tx = ex - cx;  ty = ey - cy;  }
            if (tx != 0 || ty != 0) { v->in_angle = atan2(ty, tx); v->has_in = TRUE; }
            cx = ex; cy = ey;
            break;
        }
        case CAIRO_PATH_CLOSE_PATH:
            svg_vertex_link(out, cx, cy, sx, sy);
            svg_vertex_add(out, sx, sy);
            if (sx != cx || sy != cy) {
                svg_vertex *v = &g_array_index(out, svg_vertex, out->len - 1);
                v->in_angle = atan2(sy - cy, sx - cx);
                v->has_in = TRUE;
            }
            cx = sx; cy = sy;
            break;
        default:
            break;
        }
    }
    cairo_path_destroy(path);
    return out;
}

static const ns_node *
svg_marker_ref(svg_ctx *ctx, const ns_node *n, const char *prop)
{
    const char *v = svg_prop(n, prop);
    if (!v) v = svg_prop(n, "marker");
    if (!v) return NULL;
    char *id = svg_url_id(v, NULL);
    if (!id) return NULL;
    const ns_node *m = svg_by_id(ctx, id);
    g_free(id);
    if (!m || !m->name || strcmp(m->name, "marker") != 0) return NULL;
    return m;
}

static void
svg_draw_marker(svg_ctx *ctx, const ns_node *marker, const svg_state *st,
                double x, double y, double angle)
{
    if (ctx->depth >= NS_SVG_MAX_DEPTH) return;
    cairo_t *cr = ctx->cr;
    double fs = st->font_size;
    double mw = svg_attr_length(marker, "markerWidth", ctx->vw, fs, 3);
    double mh = svg_attr_length(marker, "markerHeight", ctx->vh, fs, 3);
    if (mw <= 0 || mh <= 0) return;

    const char *units = ns_element_get_attr(marker, "markerUnits");
    gboolean stroke_units = !units || g_ascii_strcasecmp(units, "strokeWidth") == 0;

    cairo_save(cr);
    cairo_translate(cr, x, y);
    const char *orient = ns_element_get_attr(marker, "orient");
    if (!orient || g_ascii_strcasecmp(orient, "auto") == 0 ||
        g_ascii_strcasecmp(orient, "auto-start-reverse") == 0)
        cairo_rotate(cr, angle);
    else
        cairo_rotate(cr, svg_length_str(orient, 0, fs, 0) * G_PI / 180.0);
    if (stroke_units && st->stroke_width > 0)
        cairo_scale(cr, st->stroke_width, st->stroke_width);

    cairo_matrix_t vm;
    svg_viewbox_matrix(marker, mw, mh, &vm);

    double rx = svg_attr_length(marker, "refX", ctx->vw, fs, 0);
    double ry = svg_attr_length(marker, "refY", ctx->vh, fs, 0);
    cairo_matrix_transform_point(&vm, &rx, &ry);
    cairo_translate(cr, -rx, -ry);

    const char *ov = svg_prop(marker, "overflow");
    gboolean clip = !ov || (g_ascii_strcasecmp(ov, "visible") != 0 &&
                            g_ascii_strcasecmp(ov, "auto") != 0);
    if (clip) {
        cairo_rectangle(cr, 0, 0, mw, mh);
        cairo_clip(cr);
        cairo_new_path(cr);
    }

    cairo_transform(cr, &vm);

    double ow = ctx->vw, oh = ctx->vh;
    const char *vb = ns_element_get_attr(marker, "viewBox");
    if (vb) {
        const char *q = vb;
        double a, b, c, dd;
        if (svg_num(&q, &a) && svg_num(&q, &b) &&
            svg_num(&q, &c) && svg_num(&q, &dd) && c > 0 && dd > 0) {
            ctx->vw = c; ctx->vh = dd;
        }
    }

    svg_state ms;
    svg_state_init(&ms);
    ms.color_r = st->color_r; ms.color_g = st->color_g;
    ms.color_b = st->color_b; ms.color_a = st->color_a;
    ms.font_size = st->font_size;
    ctx->depth++;
    for (const ns_node *c = marker->first_child; c; c = c->next_sibling)
        if (c->kind == NS_NODE_ELEMENT) svg_render_node(ctx, c, &ms);
    ctx->depth--;
    svg_state_clear(&ms);

    ctx->vw = ow; ctx->vh = oh;
    cairo_restore(cr);
}

static void
svg_render_markers(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    const ns_node *ms = svg_marker_ref(ctx, n, "marker-start");
    const ns_node *mm = svg_marker_ref(ctx, n, "marker-mid");
    const ns_node *me = svg_marker_ref(ctx, n, "marker-end");
    if (!ms && !mm && !me) return;

    GArray *verts = svg_path_vertices(ctx->cr);
    if (!verts) return;
    if (verts->len == 0) { g_array_free(verts, TRUE); return; }

    for (guint i = 0; i < verts->len; i++) {
        const svg_vertex *v = &g_array_index(verts, svg_vertex, i);
        const ns_node *m = (i == 0) ? ms
                         : (i == verts->len - 1) ? me : mm;
        if (!m) continue;
        double a;
        if (v->has_in && v->has_out) {
            double dx = cos(v->in_angle) + cos(v->out_angle);
            double dy = sin(v->in_angle) + sin(v->out_angle);
            a = (dx == 0 && dy == 0) ? v->in_angle : atan2(dy, dx);
        } else if (v->has_in) {
            a = v->in_angle;
        } else if (v->has_out) {
            a = v->out_angle;
        } else {
            a = 0;
        }
        if (i == 0 && m == ms) {
            const char *o = ns_element_get_attr(m, "orient");
            if (o && g_ascii_strcasecmp(o, "auto-start-reverse") == 0)
                a += G_PI;
        }
        svg_draw_marker(ctx, m, st, v->x, v->y, a);
    }
    g_array_free(verts, TRUE);
}

static void
svg_render_children(svg_ctx *ctx, const ns_node *n, const svg_state *st)
{
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT) continue;
        svg_render_node(ctx, c, st);
    }
}

static gboolean
svg_is_hidden(svg_ctx *ctx, const ns_node *n)
{
    const ns_style *s = ctx->styles
        ? g_hash_table_lookup(ctx->styles, (gpointer)n) : NULL;
    if (s) {
        const char *d = ns_style_keyword(s, NS_CSS_DISPLAY);
        if (d && strcmp(d, "none") == 0) return TRUE;
    }
    const char *d = svg_prop(n, "display");
    return d && g_ascii_strcasecmp(d, "none") == 0;
}

static void
svg_render_node(svg_ctx *ctx, const ns_node *n, const svg_state *parent)
{
    if (!n->name) return;
    if (ctx->depth >= NS_SVG_MAX_DEPTH) return;
    if (++ctx->nodes > NS_SVG_MAX_NODES) return;

    const char *tag = n->name;
    if (strcmp(tag, "defs") == 0 || strcmp(tag, "symbol") == 0 ||
        strcmp(tag, "title") == 0 || strcmp(tag, "desc") == 0 ||
        strcmp(tag, "metadata") == 0 || strcmp(tag, "style") == 0 ||
        strcmp(tag, "script") == 0 || strcmp(tag, "clipPath") == 0 ||
        strcmp(tag, "mask") == 0 || strcmp(tag, "marker") == 0 ||
        strcmp(tag, "pattern") == 0 || strcmp(tag, "filter") == 0 ||
        strcmp(tag, "linearGradient") == 0 || strcmp(tag, "radialGradient") == 0)
        return;

    if (svg_is_hidden(ctx, n)) return;

    svg_state st;
    svg_state_copy(&st, parent);
    svg_state_apply_node(ctx, &st, n);

    double opacity = 1.0;
    const ns_style *s = ctx->styles
        ? g_hash_table_lookup(ctx->styles, (gpointer)n) : NULL;
    if (s && s->values[NS_CSS_OPACITY])
        opacity = CLAMP(svg_css_number(s, NS_CSS_OPACITY, 1.0, 1.0), 0.0, 1.0);
    else {
        const char *o = svg_prop(n, "opacity");
        if (o) opacity = CLAMP(svg_length_str(o, 1.0, st.font_size, 1.0), 0.0, 1.0);
    }

    if (st.hidden && strcmp(tag, "g") != 0 && strcmp(tag, "svg") != 0) {
        svg_state_clear(&st);
        return;
    }
    if (opacity <= 0.0) {
        svg_state_clear(&st);
        return;
    }

    cairo_t *cr = ctx->cr;
    cairo_save(cr);
    svg_apply_transform_attr(ctx, n, "transform");

    cairo_surface_t *mask = svg_mask_surface(ctx, n, &st);
    gboolean grouped = opacity < 1.0 || mask != NULL;
    if (grouped) cairo_push_group(cr);

    svg_apply_clip(ctx, n, &st);

    if (strcmp(tag, "g") == 0 || strcmp(tag, "a") == 0) {
        svg_render_children(ctx, n, &st);
    } else if (strcmp(tag, "switch") == 0) {
        for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
            if (c->kind != NS_NODE_ELEMENT) continue;
            if (ns_element_get_attr(c, "requiredExtensions") ||
                ns_element_get_attr(c, "requiredFeatures")) continue;
            svg_render_node(ctx, c, &st);
            break;
        }
    } else if (strcmp(tag, "svg") == 0) {
        double x = svg_attr_length(n, "x", ctx->vw, st.font_size, 0);
        double y = svg_attr_length(n, "y", ctx->vh, st.font_size, 0);
        double w = svg_attr_length(n, "width", ctx->vw, st.font_size, ctx->vw);
        double h = svg_attr_length(n, "height", ctx->vh, st.font_size, ctx->vh);
        cairo_translate(cr, x, y);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_clip(cr);
        cairo_new_path(cr);
        cairo_matrix_t vm;
        svg_viewbox_matrix(n, w, h, &vm);
        cairo_transform(cr, &vm);
        double ow = ctx->vw, oh = ctx->vh;
        const char *vb = ns_element_get_attr(n, "viewBox");
        if (vb) {
            const char *p = vb;
            double a, b, c, d;
            if (svg_num(&p, &a) && svg_num(&p, &b) &&
                svg_num(&p, &c) && svg_num(&p, &d) && c > 0 && d > 0) {
                ctx->vw = c; ctx->vh = d;
            }
        } else {
            ctx->vw = w; ctx->vh = h;
        }
        svg_render_children(ctx, n, &st);
        ctx->vw = ow; ctx->vh = oh;
    } else if (strcmp(tag, "use") == 0) {
        const ns_node *t = svg_href_target(ctx, n);
        if (t && t != n) {
            double x = svg_attr_length(n, "x", ctx->vw, st.font_size, 0);
            double y = svg_attr_length(n, "y", ctx->vh, st.font_size, 0);
            cairo_translate(cr, x, y);
            ctx->depth++;
            if (t->name && strcmp(t->name, "symbol") == 0) {
                double w = svg_attr_length(n, "width", ctx->vw, st.font_size, ctx->vw);
                double h = svg_attr_length(n, "height", ctx->vh, st.font_size, ctx->vh);
                cairo_rectangle(cr, 0, 0, w, h);
                cairo_clip(cr);
                cairo_new_path(cr);
                cairo_matrix_t vm;
                svg_viewbox_matrix(t, w, h, &vm);
                cairo_transform(cr, &vm);
                svg_render_children(ctx, t, &st);
            } else {
                svg_render_node(ctx, t, &st);
            }
            ctx->depth--;
        }
    } else if (strcmp(tag, "text") == 0) {
        svg_render_text(ctx, n, &st);
    } else if (strcmp(tag, "tspan") == 0) {
        svg_render_text(ctx, n, &st);
    } else {
        cairo_new_path(cr);
        if (svg_shape_path(ctx, n, &st)) {
            gboolean markable = strcmp(tag, "path") == 0 ||
                                strcmp(tag, "line") == 0 ||
                                strcmp(tag, "polyline") == 0 ||
                                strcmp(tag, "polygon") == 0;
            cairo_path_t *kept = markable ? cairo_copy_path(cr) : NULL;
            svg_paint_current_path(ctx, &st);
            if (kept) {
                cairo_new_path(cr);
                cairo_append_path(cr, kept);
                cairo_path_destroy(kept);
                svg_render_markers(ctx, n, &st);
            }
        }
        cairo_new_path(cr);
    }

    if (grouped) {
        cairo_pop_group_to_source(cr);
        if (mask) {
            cairo_save(cr);
            cairo_identity_matrix(cr);
            if (opacity < 1.0) {
                cairo_push_group(cr);
                cairo_mask_surface(cr, mask, 0, 0);
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, opacity);
            } else {
                cairo_mask_surface(cr, mask, 0, 0);
            }
            cairo_restore(cr);
        } else {
            cairo_paint_with_alpha(cr, opacity);
        }
    }
    if (mask) cairo_surface_destroy(mask);
    cairo_restore(cr);
    svg_state_clear(&st);
}

gboolean
ns_svg_node_is_root(const ns_node *n)
{
    return n && n->kind == NS_NODE_ELEMENT && n->name &&
           strcmp(n->name, "svg") == 0;
}

void
ns_svg_intrinsic_size(const ns_node *svg, ns_svg_size *out)
{
    memset(out, 0, sizeof *out);
    if (!svg) return;

    const char *ws = ns_element_get_attr(svg, "width");
    const char *hs = ns_element_get_attr(svg, "height");
    if (ws && !strchr(ws, '%')) {
        double w = svg_length_str(ws, 0, 16.0, 0);
        if (w > 0) { out->width = w; out->has_width = TRUE; }
    }
    if (hs && !strchr(hs, '%')) {
        double h = svg_length_str(hs, 0, 16.0, 0);
        if (h > 0) { out->height = h; out->has_height = TRUE; }
    }

    const char *vb = ns_element_get_attr(svg, "viewBox");
    if (vb) {
        const char *p = vb;
        double x, y, w, h;
        if (svg_num(&p, &x) && svg_num(&p, &y) &&
            svg_num(&p, &w) && svg_num(&p, &h) && w > 0 && h > 0) {
            out->ratio = w / h;
            out->has_ratio = TRUE;
            if (!out->has_width && !out->has_height) {
                out->width = w;  out->has_width = TRUE;
                out->height = h; out->has_height = TRUE;
            }
        }
    }
    if (out->has_width && out->has_height && !out->has_ratio && out->height > 0) {
        out->ratio = out->width / out->height;
        out->has_ratio = TRUE;
    }
    if (out->has_width && !out->has_height && out->has_ratio) {
        out->height = out->width / out->ratio;
        out->has_height = TRUE;
    } else if (out->has_height && !out->has_width && out->has_ratio) {
        out->width = out->height * out->ratio;
        out->has_width = TRUE;
    }
}

void
ns_svg_render_node(cairo_t *cr, const ns_node *svg, double width, double height,
                   GHashTable *styles, const struct ns_style *inherited)
{
    if (!cr || !svg || width <= 0 || height <= 0) return;

    svg_ctx ctx = {
        .cr = cr,
        .root = svg,
        .styles = styles,
        .ids = NULL,
        .depth = 0,
        .nodes = 0,
        .vw = width,
        .vh = height,
    };

    svg_state st;
    svg_state_init(&st);
    if (inherited) {
        const ns_css_value *c = inherited->values[NS_CSS_COLOR];
        if (c && c->kind == NS_CSS_V_COLOR) {
            st.color_r = c->u.color.r / 255.0;
            st.color_g = c->u.color.g / 255.0;
            st.color_b = c->u.color.b / 255.0;
            st.color_a = c->u.color.a / 255.0;
        }
        if (inherited->values[NS_CSS_FONT_SIZE])
            st.font_size = MAX(1.0, ns_css_length_or(
                inherited->values[NS_CSS_FONT_SIZE], 16.0));
    }

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    cairo_new_path(cr);

    cairo_matrix_t vm;
    svg_viewbox_matrix(svg, width, height, &vm);
    cairo_transform(cr, &vm);

    const char *vb = ns_element_get_attr(svg, "viewBox");
    if (vb) {
        const char *p = vb;
        double a, b, c, d;
        if (svg_num(&p, &a) && svg_num(&p, &b) &&
            svg_num(&p, &c) && svg_num(&p, &d) && c > 0 && d > 0) {
            ctx.vw = c;
            ctx.vh = d;
        }
    }

    svg_state_apply_node(&ctx, &st, svg);
    svg_render_children(&ctx, svg, &st);

    cairo_restore(cr);
    svg_state_clear(&st);
    if (ctx.ids) g_hash_table_destroy(ctx.ids);
}

gboolean
ns_svg_bytes_look_like_svg(const guchar *data, gsize len)
{
    if (!data || len < 4) return FALSE;
    gsize scan = MIN(len, (gsize)1024);
    for (gsize i = 0; i + 4 < scan; i++) {
        if (data[i] == '<' &&
            g_ascii_strncasecmp((const char *)data + i, "<svg", 4) == 0)
            return TRUE;
    }
    return FALSE;
}

static const ns_node *
svg_find_root(const ns_node *n)
{
    if (!n) return NULL;
    if (ns_svg_node_is_root(n)) return n;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *r = svg_find_root(c);
        if (r) return r;
    }
    return NULL;
}

gboolean
ns_svg_render_bytes(cairo_t *cr, const guchar *data, gsize len,
                    double width, double height)
{
    if (!cr || !data || len == 0 || len > NS_SVG_MAX_INPUT_BYTES) return FALSE;
    if (width <= 0 || height <= 0) return FALSE;
    if (!ns_svg_bytes_look_like_svg(data, len)) return FALSE;

    ns_node *doc = ns_html_parse((const char *)data, (gssize)len);
    if (!doc) return FALSE;
    const ns_node *root = svg_find_root(doc);
    if (!root) {
        ns_node_free(doc);
        return FALSE;
    }
    ns_svg_render_node(cr, root, width, height, NULL, NULL);
    ns_node_free(doc);
    return TRUE;
}

ns_texture *
ns_svg_decode_bytes(const guchar *data, gsize len, int *out_w, int *out_h)
{
    if (!data || len == 0 || len > NS_SVG_MAX_INPUT_BYTES) return NULL;
    if (!ns_svg_bytes_look_like_svg(data, len)) return NULL;

    ns_node *doc = ns_html_parse((const char *)data, (gssize)len);
    if (!doc) return NULL;
    const ns_node *root = svg_find_root(doc);
    if (!root) {
        ns_node_free(doc);
        return NULL;
    }

    ns_svg_size size;
    ns_svg_intrinsic_size(root, &size);
    double w = size.has_width  ? size.width  : NS_SVG_DEFAULT_DIM_PX;
    double h = size.has_height ? size.height : NS_SVG_DEFAULT_DIM_PX;
    if (w <= 0) w = NS_SVG_DEFAULT_DIM_PX;
    if (h <= 0) h = NS_SVG_DEFAULT_DIM_PX;
    if (w > NS_SVG_MAX_DIM_PX || h > NS_SVG_MAX_DIM_PX) {
        double s = (double)NS_SVG_MAX_DIM_PX / MAX(w, h);
        w *= s; h *= s;
    }
    if (w * h > (double)NS_SVG_MAX_PIXELS) {
        double s = sqrt((double)NS_SVG_MAX_PIXELS / (w * h));
        w *= s; h *= s;
    }
    int iw = (int)CLAMP(ceil(w - 0.001), 1.0, (double)NS_SVG_MAX_DIM_PX);
    int ih = (int)CLAMP(ceil(h - 0.001), 1.0, (double)NS_SVG_MAX_DIM_PX);

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        ns_node_free(doc);
        return NULL;
    }
    cairo_t *cr = cairo_create(surf);
    ns_svg_render_node(cr, root, iw, ih, NULL, NULL);
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    ns_node_free(doc);

    int stride = cairo_image_surface_get_stride(surf);
    const guchar *pixels = cairo_image_surface_get_data(surf);
    if (!pixels) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    GBytes *bytes = g_bytes_new_with_free_func(
        pixels, (gsize)stride * (gsize)ih,
        (GDestroyNotify)cairo_surface_destroy, surf);
    ns_texture *tex = ns_texture_new(iw, ih, NS_TEXTURE_DEFAULT, bytes,
                                     (gsize)stride);
    g_bytes_unref(bytes);
    if (!tex) return NULL;
    if (out_w) *out_w = iw;
    if (out_h) *out_h = ih;
    return tex;
}
