/* Northstar — Media Queries Level 4 parser, evaluator and serializer.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <math.h>
#include <string.h>
#include "css.h"

#define MQ_MAX_DEPTH 32
#define MQ_VIEWPORT_STACK 16

typedef enum { MQ_NO, MQ_YES, MQ_DUNNO } mq_tri;

static struct { double w, h; } g_mq_stack[MQ_VIEWPORT_STACK];
static int g_mq_stack_len;

static double g_mq_device_w = 1920;
static double g_mq_device_h = 1080;

void
ns_css_set_device_size(double w, double h)
{
    if (w > 0) g_mq_device_w = w;
    if (h > 0) g_mq_device_h = h;
}

void
ns_css_media_viewport_push(double w, double h)
{
    if (g_mq_stack_len >= MQ_VIEWPORT_STACK) return;
    g_mq_stack[g_mq_stack_len].w = w >= 0 ? w : 0;
    g_mq_stack[g_mq_stack_len].h = h >= 0 ? h : 0;
    g_mq_stack_len++;
}

void
ns_css_media_viewport_pop(void)
{
    if (g_mq_stack_len > 0) g_mq_stack_len--;
}

static double
mq_vw(void)
{
    if (g_mq_stack_len > 0) return g_mq_stack[g_mq_stack_len - 1].w;
    return ns_css_viewport_w();
}

static double
mq_vh(void)
{
    if (g_mq_stack_len > 0) return g_mq_stack[g_mq_stack_len - 1].h;
    return ns_css_viewport_h();
}

double
ns_css_media_viewport_current_w(void)
{
    return mq_vw();
}

double
ns_css_media_viewport_current_h(void)
{
    return mq_vh();
}

static mq_tri
mq_not(mq_tri v)
{
    if (v == MQ_YES) return MQ_NO;
    if (v == MQ_NO) return MQ_YES;
    return MQ_DUNNO;
}

static mq_tri
mq_and(mq_tri a, mq_tri b)
{
    if (a == MQ_NO || b == MQ_NO) return MQ_NO;
    if (a == MQ_YES && b == MQ_YES) return MQ_YES;
    return MQ_DUNNO;
}

static mq_tri
mq_or(mq_tri a, mq_tri b)
{
    if (a == MQ_YES || b == MQ_YES) return MQ_YES;
    if (a == MQ_NO && b == MQ_NO) return MQ_NO;
    return MQ_DUNNO;
}

typedef enum {
    MQF_LENGTH, MQF_RATIO, MQF_RESOLUTION, MQF_INTEGER, MQF_DISCRETE,
} mq_feature_type;

typedef struct {
    const char        *name;
    mq_feature_type    type;
    const char *const *kw;
    gsize              nkw;
} mq_feature_def;

static const char *const mqkw_orientation[]  = { "portrait", "landscape" };
static const char *const mqkw_scan[]         = { "interlace", "progressive" };
static const char *const mqkw_overflow_b[]   = { "none", "scroll", "paged" };
static const char *const mqkw_overflow_i[]   = { "none", "scroll" };
static const char *const mqkw_update[]       = { "none", "slow", "fast" };
static const char *const mqkw_hover[]        = { "none", "hover" };
static const char *const mqkw_pointer[]      = { "none", "coarse", "fine" };
static const char *const mqkw_scheme[]       = { "light", "dark" };
static const char *const mqkw_motion[]       = { "no-preference", "reduce" };
static const char *const mqkw_contrast[]     = { "no-preference", "less", "more", "custom" };
static const char *const mqkw_data[]         = { "no-preference", "reduce" };
static const char *const mqkw_forced[]       = { "none", "active" };
static const char *const mqkw_inverted[]     = { "none", "inverted" };
static const char *const mqkw_gamut[]        = { "srgb", "p3", "rec2020" };
static const char *const mqkw_scripting[]    = { "none", "initial-only", "enabled" };
static const char *const mqkw_display_mode[] = {
    "fullscreen", "standalone", "minimal-ui", "browser", "picture-in-picture",
    "window-controls-overlay",
};
static const char *const mqkw_dynrange[]     = { "standard", "high" };

static const mq_feature_def g_mq_features[] = {
    { "width",                MQF_LENGTH,     NULL, 0 },
    { "height",               MQF_LENGTH,     NULL, 0 },
    { "device-width",         MQF_LENGTH,     NULL, 0 },
    { "device-height",        MQF_LENGTH,     NULL, 0 },
    { "inline-size",          MQF_LENGTH,     NULL, 0 },
    { "block-size",           MQF_LENGTH,     NULL, 0 },
    { "aspect-ratio",         MQF_RATIO,      NULL, 0 },
    { "device-aspect-ratio",  MQF_RATIO,      NULL, 0 },
    { "resolution",           MQF_RESOLUTION, NULL, 0 },
    { "color",                MQF_INTEGER,    NULL, 0 },
    { "color-index",          MQF_INTEGER,    NULL, 0 },
    { "monochrome",           MQF_INTEGER,    NULL, 0 },
    { "grid",                 MQF_INTEGER,    NULL, 0 },
    { "orientation",          MQF_DISCRETE,   mqkw_orientation,  G_N_ELEMENTS(mqkw_orientation) },
    { "scan",                 MQF_DISCRETE,   mqkw_scan,         G_N_ELEMENTS(mqkw_scan) },
    { "overflow-block",       MQF_DISCRETE,   mqkw_overflow_b,   G_N_ELEMENTS(mqkw_overflow_b) },
    { "overflow-inline",      MQF_DISCRETE,   mqkw_overflow_i,   G_N_ELEMENTS(mqkw_overflow_i) },
    { "update",               MQF_DISCRETE,   mqkw_update,       G_N_ELEMENTS(mqkw_update) },
    { "hover",                MQF_DISCRETE,   mqkw_hover,        G_N_ELEMENTS(mqkw_hover) },
    { "any-hover",            MQF_DISCRETE,   mqkw_hover,        G_N_ELEMENTS(mqkw_hover) },
    { "pointer",              MQF_DISCRETE,   mqkw_pointer,      G_N_ELEMENTS(mqkw_pointer) },
    { "any-pointer",          MQF_DISCRETE,   mqkw_pointer,      G_N_ELEMENTS(mqkw_pointer) },
    { "prefers-color-scheme", MQF_DISCRETE,   mqkw_scheme,       G_N_ELEMENTS(mqkw_scheme) },
    { "prefers-reduced-motion", MQF_DISCRETE, mqkw_motion,       G_N_ELEMENTS(mqkw_motion) },
    { "prefers-reduced-transparency", MQF_DISCRETE, mqkw_motion, G_N_ELEMENTS(mqkw_motion) },
    { "prefers-contrast",     MQF_DISCRETE,   mqkw_contrast,     G_N_ELEMENTS(mqkw_contrast) },
    { "prefers-reduced-data", MQF_DISCRETE,   mqkw_data,         G_N_ELEMENTS(mqkw_data) },
    { "forced-colors",        MQF_DISCRETE,   mqkw_forced,       G_N_ELEMENTS(mqkw_forced) },
    { "inverted-colors",      MQF_DISCRETE,   mqkw_inverted,     G_N_ELEMENTS(mqkw_inverted) },
    { "color-gamut",          MQF_DISCRETE,   mqkw_gamut,        G_N_ELEMENTS(mqkw_gamut) },
    { "scripting",            MQF_DISCRETE,   mqkw_scripting,    G_N_ELEMENTS(mqkw_scripting) },
    { "display-mode",         MQF_DISCRETE,   mqkw_display_mode, G_N_ELEMENTS(mqkw_display_mode) },
    { "dynamic-range",        MQF_DISCRETE,   mqkw_dynrange,     G_N_ELEMENTS(mqkw_dynrange) },
    { "video-dynamic-range",  MQF_DISCRETE,   mqkw_dynrange,     G_N_ELEMENTS(mqkw_dynrange) },
};

static const mq_feature_def *
mq_feature_lookup(const char *name)
{
    for (gsize i = 0; i < G_N_ELEMENTS(g_mq_features); i++)
        if (g_ascii_strcasecmp(name, g_mq_features[i].name) == 0)
            return &g_mq_features[i];
    return NULL;
}

typedef enum { MQOP_EQ, MQOP_LT, MQOP_LE, MQOP_GT, MQOP_GE } mq_op;

typedef struct {
    mq_feature_type kind;
    double num;
    double denom;
    char   unit[8];
    char   ident[24];
    gboolean is_ident;
} mq_value;

typedef enum {
    MQN_TYPE, MQN_NOT, MQN_AND, MQN_OR, MQN_FEATURE, MQN_ENCLOSED,
} mq_node_kind;

typedef struct mq_node {
    mq_node_kind kind;
    GPtrArray *kids;
    char *text;
    const mq_feature_def *feature;
    int minmax;
    int nops;
    gboolean plain;
    mq_op op1, op2;
    mq_value v1, v2;
} mq_node;

static void
mq_node_free(mq_node *n)
{
    if (!n) return;
    if (n->kids) {
        for (guint i = 0; i < n->kids->len; i++)
            mq_node_free(g_ptr_array_index(n->kids, i));
        g_ptr_array_free(n->kids, TRUE);
    }
    g_free(n->text);
    g_free(n);
}

static mq_node *
mq_node_new(mq_node_kind kind)
{
    mq_node *n = g_new0(mq_node, 1);
    n->kind = kind;
    return n;
}

typedef struct {
    mq_node *cond;
    char *type;
    gboolean negated;
    gboolean only;
    gboolean valid;
} mq_query;

static void
mq_query_clear(mq_query *q)
{
    mq_node_free(q->cond);
    g_free(q->type);
    q->cond = NULL;
    q->type = NULL;
}

static gboolean
mq_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static const char *
mq_skip_ws(const char *p, const char *end)
{
    while (p < end && mq_is_ws(*p)) p++;
    return p;
}

static gboolean
mq_ident_char(char c)
{
    return g_ascii_isalnum(c) || c == '-' || c == '_';
}

static const char *
mq_read_ident(const char *p, const char *end, char *buf, gsize buflen)
{
    const char *s = p;
    while (p < end && mq_ident_char(*p)) p++;
    if (p == s) return NULL;
    gsize n = (gsize)(p - s);
    if (n >= buflen) return NULL;
    for (gsize i = 0; i < n; i++) buf[i] = g_ascii_tolower(s[i]);
    buf[n] = '\0';
    return p;
}

static gboolean
mq_kw_at(const char *p, const char *end, const char *kw)
{
    gsize n = strlen(kw);
    if ((gsize)(end - p) < n) return FALSE;
    if (g_ascii_strncasecmp(p, kw, n) != 0) return FALSE;
    const char *after = p + n;
    return after == end || mq_is_ws(*after) || *after == '(';
}

static const char *
mq_close_paren(const char *p, const char *end)
{
    int depth = 1;
    while (p < end) {
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) return p;
        p++;
    }
    return end;
}

static gboolean
mq_any_value_ok(const char *p, const char *end)
{
    int paren = 0, bracket = 0, brace = 0;
    while (p < end) {
        char c = *p;
        if (c == '(') paren++;
        else if (c == ')') { if (--paren < 0) return FALSE; }
        else if (c == '[') bracket++;
        else if (c == ']') { if (--bracket < 0) return FALSE; }
        else if (c == '{') brace++;
        else if (c == '}') { if (--brace < 0) return FALSE; }
        else if (c == ';' || c == '!') return FALSE;
        p++;
    }
    return TRUE;
}

static double
mq_length_unit_px(double v, const char *unit)
{
    if (!*unit || g_ascii_strcasecmp(unit, "px") == 0) return v;
    if (g_ascii_strcasecmp(unit, "em") == 0 ||
        g_ascii_strcasecmp(unit, "rem") == 0) return v * 16.0;
    if (g_ascii_strcasecmp(unit, "ex") == 0 ||
        g_ascii_strcasecmp(unit, "ch") == 0) return v * 8.0;
    if (g_ascii_strcasecmp(unit, "cm") == 0) return v * 96.0 / 2.54;
    if (g_ascii_strcasecmp(unit, "mm") == 0) return v * 96.0 / 25.4;
    if (g_ascii_strcasecmp(unit, "q") == 0)  return v * 96.0 / 101.6;
    if (g_ascii_strcasecmp(unit, "in") == 0) return v * 96.0;
    if (g_ascii_strcasecmp(unit, "pt") == 0) return v * 96.0 / 72.0;
    if (g_ascii_strcasecmp(unit, "pc") == 0) return v * 16.0;
    if (g_ascii_strcasecmp(unit, "vw") == 0) return v * mq_vw() / 100.0;
    if (g_ascii_strcasecmp(unit, "vh") == 0) return v * mq_vh() / 100.0;
    if (g_ascii_strcasecmp(unit, "vmin") == 0)
        return v * MIN(mq_vw(), mq_vh()) / 100.0;
    if (g_ascii_strcasecmp(unit, "vmax") == 0)
        return v * MAX(mq_vw(), mq_vh()) / 100.0;
    return NAN;
}

static double
mq_resolution_unit_dppx(double v, const char *unit)
{
    if (g_ascii_strcasecmp(unit, "dppx") == 0 ||
        g_ascii_strcasecmp(unit, "x") == 0) return v;
    if (g_ascii_strcasecmp(unit, "dpi") == 0)  return v / 96.0;
    if (g_ascii_strcasecmp(unit, "dpcm") == 0) return v * 2.54 / 96.0;
    return NAN;
}

typedef double (*mq_unit_fn)(double v, const char *unit);

static gboolean mq_calc_sum(const char **pp, const char *end, mq_unit_fn fn,
                            double *out, int depth);

static gboolean
mq_calc_primary(const char **pp, const char *end, mq_unit_fn fn,
                double *out, gboolean *is_number, int depth)
{
    if (depth > MQ_MAX_DEPTH) return FALSE;
    const char *p = mq_skip_ws(*pp, end);
    if (p < end && *p == '(') {
        p++;
        if (!mq_calc_sum(&p, end, fn, out, depth + 1)) return FALSE;
        p = mq_skip_ws(p, end);
        if (p < end && *p == ')') p++;
        *is_number = FALSE;
        *pp = p;
        return TRUE;
    }
    if (p < end && g_ascii_strncasecmp(p, "calc(", 5) == 0) {
        p += 5;
        if (!mq_calc_sum(&p, end, fn, out, depth + 1)) return FALSE;
        p = mq_skip_ws(p, end);
        if (p < end && *p == ')') p++;
        *is_number = FALSE;
        *pp = p;
        return TRUE;
    }
    char *num_end = NULL;
    double v = g_ascii_strtod(p, &num_end);
    if (!num_end || num_end == p || num_end > end) return FALSE;
    char unit[8];
    gsize u = 0;
    const char *q = num_end;
    while (q < end && (g_ascii_isalpha(*q) || *q == '%') && u < sizeof unit - 1)
        unit[u++] = *q++;
    unit[u] = '\0';
    if (u == 0) {
        *is_number = TRUE;
        *out = v;
    } else {
        double c = fn(v, unit);
        if (isnan(c)) return FALSE;
        *is_number = FALSE;
        *out = c;
    }
    *pp = q;
    return TRUE;
}

static gboolean
mq_calc_product(const char **pp, const char *end, mq_unit_fn fn,
                double *out, gboolean *is_number, int depth)
{
    if (!mq_calc_primary(pp, end, fn, out, is_number, depth)) return FALSE;
    for (;;) {
        const char *p = mq_skip_ws(*pp, end);
        if (p >= end || (*p != '*' && *p != '/')) return TRUE;
        char op = *p++;
        double rhs = 0;
        gboolean rhs_num = FALSE;
        if (!mq_calc_primary(&p, end, fn, &rhs, &rhs_num, depth)) return FALSE;
        if (op == '*') {
            if (!*is_number && !rhs_num) return FALSE;
            *out *= rhs;
            *is_number = *is_number && rhs_num;
        } else {
            if (!rhs_num || rhs == 0) return FALSE;
            *out /= rhs;
        }
        *pp = p;
    }
}

static gboolean
mq_calc_sum(const char **pp, const char *end, mq_unit_fn fn,
            double *out, int depth)
{
    if (depth > MQ_MAX_DEPTH) return FALSE;
    gboolean is_number = FALSE;
    if (!mq_calc_product(pp, end, fn, out, &is_number, depth)) return FALSE;
    for (;;) {
        const char *p = mq_skip_ws(*pp, end);
        if (p >= end || (*p != '+' && *p != '-')) return TRUE;
        if (!mq_is_ws(p[-1])) return FALSE;
        char op = *p++;
        if (p >= end || !mq_is_ws(*p)) return FALSE;
        double rhs = 0;
        gboolean rhs_num = FALSE;
        if (!mq_calc_product(&p, end, fn, &rhs, &rhs_num, depth)) return FALSE;
        if (rhs_num != is_number) return FALSE;
        *out += op == '+' ? rhs : -rhs;
        *pp = p;
    }
}

static gboolean
mq_parse_calc(const char *s, const char *end, mq_unit_fn fn, double *out)
{
    const char *p = s;
    if (g_ascii_strncasecmp(p, "calc(", 5) != 0) return FALSE;
    p += 5;
    if (!mq_calc_sum(&p, end, fn, out, 0)) return FALSE;
    p = mq_skip_ws(p, end);
    if (p < end && *p == ')') p++;
    return mq_skip_ws(p, end) == end;
}

static gboolean
mq_parse_number(const char *s, const char *end, double *out,
                const char **unit_start)
{
    char *num_end = NULL;
    double v = g_ascii_strtod(s, &num_end);
    if (!num_end || num_end == s || num_end > end) return FALSE;
    *out = v;
    *unit_start = num_end;
    return TRUE;
}

static gboolean
mq_value_parse(const char *s, const char *e, mq_feature_type type,
               const mq_feature_def *f, mq_value *out)
{
    s = mq_skip_ws(s, e);
    while (e > s && mq_is_ws(e[-1])) e--;
    if (s >= e) return FALSE;
    memset(out, 0, sizeof *out);
    out->kind = type;
    out->denom = 1;

    if (type == MQF_DISCRETE) {
        char ident[24];
        const char *p = mq_read_ident(s, e, ident, sizeof ident);
        if (!p || mq_skip_ws(p, e) != e) return FALSE;
        for (gsize i = 0; i < f->nkw; i++)
            if (strcmp(ident, f->kw[i]) == 0) {
                g_strlcpy(out->ident, f->kw[i], sizeof out->ident);
                out->is_ident = TRUE;
                return TRUE;
            }
        return FALSE;
    }

    if (type == MQF_INTEGER) {
        const char *p = s;
        if (*p == '+' || *p == '-') p++;
        if (p >= e) return FALSE;
        for (const char *q = p; q < e; q++)
            if (!g_ascii_isdigit(*q)) return FALSE;
        out->num = g_ascii_strtod(s, NULL);
        if (f && g_ascii_strcasecmp(f->name, "grid") == 0 &&
            out->num != 0 && out->num != 1)
            return FALSE;
        g_strlcpy(out->unit, "", sizeof out->unit);
        return TRUE;
    }

    if (type == MQF_RATIO) {
        double a = 0, b = 1;
        const char *p = NULL;
        if (!mq_parse_number(s, e, &a, &p)) return FALSE;
        if (a < 0) return FALSE;
        p = mq_skip_ws(p, e);
        if (p < e) {
            if (*p != '/') return FALSE;
            p = mq_skip_ws(p + 1, e);
            const char *q = NULL;
            if (!mq_parse_number(p, e, &b, &q)) return FALSE;
            if (b < 0) return FALSE;
            if (mq_skip_ws(q, e) != e) return FALSE;
        }
        out->num = a;
        out->denom = b;
        return TRUE;
    }

    if (type == MQF_RESOLUTION) {
        if (g_ascii_strncasecmp(s, "calc(", 5) == 0) {
            double v = 0;
            if (!mq_parse_calc(s, e, mq_resolution_unit_dppx, &v)) return FALSE;
            out->num = v;
            g_strlcpy(out->unit, "dppx", sizeof out->unit);
            return TRUE;
        }
        double v = 0;
        const char *p = NULL;
        if (!mq_parse_number(s, e, &v, &p)) return FALSE;
        if (v < 0) return FALSE;
        char unit[8];
        gsize u = 0;
        while (p < e && g_ascii_isalpha(*p) && u < sizeof unit - 1)
            unit[u++] = g_ascii_tolower(*p++);
        unit[u] = '\0';
        if (p != e || u == 0) return FALSE;
        double dppx = mq_resolution_unit_dppx(v, unit);
        if (isnan(dppx)) return FALSE;
        out->num = dppx;
        g_strlcpy(out->unit, unit, sizeof out->unit);
        return TRUE;
    }

    if (g_ascii_strncasecmp(s, "calc(", 5) == 0) {
        double v = 0;
        if (!mq_parse_calc(s, e, mq_length_unit_px, &v)) return FALSE;
        out->num = v;
        g_strlcpy(out->unit, "px", sizeof out->unit);
        return TRUE;
    }
    double v = 0;
    const char *p = NULL;
    if (!mq_parse_number(s, e, &v, &p)) return FALSE;
    char unit[8];
    gsize u = 0;
    while (p < e && g_ascii_isalpha(*p) && u < sizeof unit - 1)
        unit[u++] = g_ascii_tolower(*p++);
    unit[u] = '\0';
    if (p != e) return FALSE;
    if (u == 0) {
        if (v != 0) return FALSE;
        out->num = 0;
        g_strlcpy(out->unit, "px", sizeof out->unit);
        return TRUE;
    }
    double px = mq_length_unit_px(v, unit);
    if (isnan(px)) return FALSE;
    out->num = px;
    g_strlcpy(out->unit, unit, sizeof out->unit);
    return TRUE;
}

static const char *
mq_find_op(const char *p, const char *end, mq_op *op, int *oplen)
{
    int depth = 0;
    while (p < end) {
        char c = *p;
        if (c == '(') depth++;
        else if (c == ')') { if (depth > 0) depth--; }
        else if (depth == 0 && (c == '<' || c == '>' || c == '=')) {
            if (c == '<') {
                if (p + 1 < end && p[1] == '=') { *op = MQOP_LE; *oplen = 2; }
                else { *op = MQOP_LT; *oplen = 1; }
            } else if (c == '>') {
                if (p + 1 < end && p[1] == '=') { *op = MQOP_GE; *oplen = 2; }
                else { *op = MQOP_GT; *oplen = 1; }
            } else {
                *op = MQOP_EQ;
                *oplen = 1;
            }
            return p;
        }
        p++;
    }
    return NULL;
}

static gboolean
mq_side_is_feature(const char *s, const char *e, const mq_feature_def **out)
{
    s = mq_skip_ws(s, e);
    while (e > s && mq_is_ws(e[-1])) e--;
    char ident[40];
    const char *p = mq_read_ident(s, e, ident, sizeof ident);
    if (!p || p != e) return FALSE;
    const mq_feature_def *f = mq_feature_lookup(ident);
    if (!f) return FALSE;
    if (f->type == MQF_DISCRETE) return FALSE;
    if (g_ascii_strcasecmp(f->name, "grid") == 0) return FALSE;
    *out = f;
    return TRUE;
}

static mq_node *
mq_parse_feature(const char *s, const char *e)
{
    s = mq_skip_ws(s, e);
    while (e > s && mq_is_ws(e[-1])) e--;
    if (s >= e) return NULL;

    const char *colon = NULL;
    int depth = 0;
    for (const char *p = s; p < e; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') { if (depth > 0) depth--; }
        else if (*p == ':' && depth == 0) { colon = p; break; }
    }

    if (colon) {
        char name[40];
        const char *ne = mq_read_ident(s, colon, name, sizeof name);
        if (!ne || mq_skip_ws(ne, colon) != colon) return NULL;
        int minmax = 0;
        const char *base = name;
        if (g_str_has_prefix(name, "min-")) { minmax = 1; base = name + 4; }
        else if (g_str_has_prefix(name, "max-")) { minmax = 2; base = name + 4; }
        const mq_feature_def *f = mq_feature_lookup(base);
        if (!f) return NULL;
        if (minmax && (f->type == MQF_DISCRETE ||
                       g_ascii_strcasecmp(f->name, "grid") == 0))
            return NULL;
        mq_value v;
        if (!mq_value_parse(colon + 1, e, f->type, f, &v)) return NULL;
        mq_node *n = mq_node_new(MQN_FEATURE);
        n->feature = f;
        n->minmax = minmax;
        n->plain = TRUE;
        n->nops = 1;
        n->op1 = minmax == 1 ? MQOP_GE : minmax == 2 ? MQOP_LE : MQOP_EQ;
        n->v1 = v;
        return n;
    }

    mq_op op1, op2;
    int len1 = 0, len2 = 0;
    const char *p1 = mq_find_op(s, e, &op1, &len1);
    if (p1) {
        const char *after1 = p1 + len1;
        const char *p2 = mq_find_op(after1, e, &op2, &len2);
        const mq_feature_def *f = NULL;
        if (p2) {
            if (!mq_side_is_feature(after1, p2, &f)) return NULL;
            gboolean lt1 = op1 == MQOP_LT || op1 == MQOP_LE;
            gboolean lt2 = op2 == MQOP_LT || op2 == MQOP_LE;
            gboolean gt1 = op1 == MQOP_GT || op1 == MQOP_GE;
            gboolean gt2 = op2 == MQOP_GT || op2 == MQOP_GE;
            if (!((lt1 && lt2) || (gt1 && gt2))) return NULL;
            mq_value va, vb;
            if (!mq_value_parse(s, p1, f->type, f, &va)) return NULL;
            if (!mq_value_parse(p2 + len2, e, f->type, f, &vb)) return NULL;
            mq_node *n = mq_node_new(MQN_FEATURE);
            n->feature = f;
            n->nops = 2;
            n->op1 = op1;
            n->op2 = op2;
            n->v1 = va;
            n->v2 = vb;
            return n;
        }
        mq_value v;
        mq_node *n = NULL;
        if (mq_side_is_feature(s, p1, &f)) {
            if (!mq_value_parse(after1, e, f->type, f, &v)) return NULL;
            n = mq_node_new(MQN_FEATURE);
            n->feature = f;
            n->nops = 1;
            n->op1 = op1;
            n->v1 = v;
        } else if (mq_side_is_feature(after1, e, &f)) {
            if (!mq_value_parse(s, p1, f->type, f, &v)) return NULL;
            mq_op flipped = op1 == MQOP_LT ? MQOP_GT
                          : op1 == MQOP_LE ? MQOP_GE
                          : op1 == MQOP_GT ? MQOP_LT
                          : op1 == MQOP_GE ? MQOP_LE : MQOP_EQ;
            n = mq_node_new(MQN_FEATURE);
            n->feature = f;
            n->nops = 1;
            n->op1 = flipped;
            n->v1 = v;
        }
        return n;
    }

    char ident[40];
    const char *p = mq_read_ident(s, e, ident, sizeof ident);
    if (!p || mq_skip_ws(p, e) != e) return NULL;
    const mq_feature_def *f = mq_feature_lookup(ident);
    if (!f) return NULL;
    mq_node *n = mq_node_new(MQN_FEATURE);
    n->feature = f;
    n->nops = 0;
    return n;
}

static mq_node *mq_parse_condition(const char **pp, const char *end,
                                   gboolean allow_or, int depth);

static mq_node *
mq_parse_in_parens(const char **pp, const char *end, int depth)
{
    if (depth > MQ_MAX_DEPTH) return NULL;
    const char *p = mq_skip_ws(*pp, end);
    if (p < end && *p == '(') {
        const char *inner = p + 1;
        const char *close = mq_close_paren(inner, end);
        const char *cp = inner;
        mq_node *cond = mq_parse_condition(&cp, close, TRUE, depth + 1);
        if (cond && mq_skip_ws(cp, close) == close) {
            mq_node *wrap = mq_node_new(MQN_AND);
            wrap->kids = g_ptr_array_new();
            g_ptr_array_add(wrap->kids, cond);
            *pp = close < end ? close + 1 : end;
            return wrap;
        }
        mq_node_free(cond);
        mq_node *feat = mq_parse_feature(inner, close);
        if (feat) {
            *pp = close < end ? close + 1 : end;
            return feat;
        }
        if (!mq_any_value_ok(inner, close)) return NULL;
        mq_node *enc = mq_node_new(MQN_ENCLOSED);
        enc->text = g_strndup(inner, (gsize)(close - inner));
        g_strstrip(enc->text);
        *pp = close < end ? close + 1 : end;
        return enc;
    }
    char ident[40];
    const char *q = mq_read_ident(p, end, ident, sizeof ident);
    if (q && q < end && *q == '(') {
        const char *inner = q + 1;
        const char *close = mq_close_paren(inner, end);
        if (!mq_any_value_ok(inner, close)) return NULL;
        mq_node *enc = mq_node_new(MQN_ENCLOSED);
        enc->text = g_strndup(p, (gsize)((close < end ? close + 1 : end) - p));
        *pp = close < end ? close + 1 : end;
        return enc;
    }
    return NULL;
}

static mq_node *
mq_parse_condition(const char **pp, const char *end, gboolean allow_or,
                   int depth)
{
    if (depth > MQ_MAX_DEPTH) return NULL;
    const char *p = mq_skip_ws(*pp, end);
    if (mq_kw_at(p, end, "not")) {
        const char *q = p + 3;
        mq_node *child = mq_parse_in_parens(&q, end, depth + 1);
        if (!child) return NULL;
        mq_node *n = mq_node_new(MQN_NOT);
        n->kids = g_ptr_array_new();
        g_ptr_array_add(n->kids, child);
        *pp = q;
        return n;
    }
    mq_node *first = mq_parse_in_parens(&p, end, depth + 1);
    if (!first) return NULL;
    const char *q = mq_skip_ws(p, end);
    if (q >= end) {
        *pp = p;
        return first;
    }
    gboolean use_and;
    if (mq_kw_at(q, end, "and")) use_and = TRUE;
    else if (allow_or && mq_kw_at(q, end, "or")) use_and = FALSE;
    else {
        *pp = p;
        return first;
    }
    mq_node *n = mq_node_new(use_and ? MQN_AND : MQN_OR);
    n->kids = g_ptr_array_new();
    g_ptr_array_add(n->kids, first);
    for (;;) {
        q = mq_skip_ws(p, end);
        if (q >= end) break;
        if (use_and && mq_kw_at(q, end, "and")) q += 3;
        else if (!use_and && mq_kw_at(q, end, "or")) q += 2;
        else break;
        mq_node *next = mq_parse_in_parens(&q, end, depth + 1);
        if (!next) {
            mq_node_free(n);
            return NULL;
        }
        g_ptr_array_add(n->kids, next);
        p = q;
    }
    *pp = p;
    return n;
}

static gboolean
mq_type_reserved(const char *ident)
{
    return strcmp(ident, "not") == 0 || strcmp(ident, "only") == 0 ||
           strcmp(ident, "and") == 0 || strcmp(ident, "or") == 0 ||
           strcmp(ident, "layer") == 0;
}

static void
mq_parse_query(const char *s, const char *e, mq_query *out)
{
    memset(out, 0, sizeof *out);
    s = mq_skip_ws(s, e);
    while (e > s && mq_is_ws(e[-1])) e--;
    if (s >= e) return;

    const char *p = s;
    mq_node *cond = mq_parse_condition(&p, e, TRUE, 0);
    if (cond && mq_skip_ws(p, e) == e) {
        out->cond = cond;
        out->valid = TRUE;
        return;
    }
    mq_node_free(cond);

    p = s;
    gboolean negated = FALSE, only = FALSE;
    char ident[64];
    const char *q = mq_read_ident(p, e, ident, sizeof ident);
    if (!q) return;
    if (strcmp(ident, "not") == 0) { negated = TRUE; p = q; }
    else if (strcmp(ident, "only") == 0) { only = TRUE; p = q; }
    p = mq_skip_ws(p, e);
    q = mq_read_ident(p, e, ident, sizeof ident);
    if (!q || mq_type_reserved(ident)) return;
    char *type = g_strdup(ident);
    p = mq_skip_ws(q, e);
    mq_node *tail = NULL;
    if (p < e) {
        if (!mq_kw_at(p, e, "and")) {
            g_free(type);
            return;
        }
        p += 3;
        tail = mq_parse_condition(&p, e, FALSE, 0);
        if (!tail || mq_skip_ws(p, e) != e) {
            mq_node_free(tail);
            g_free(type);
            return;
        }
    }
    out->type = type;
    out->cond = tail;
    out->negated = negated;
    out->only = only;
    out->valid = TRUE;
}

static const char *
mq_discrete_current(const mq_feature_def *f)
{
    const char *n = f->name;
    if (strcmp(n, "orientation") == 0)
        return mq_vw() >= mq_vh() ? "landscape" : "portrait";
    if (strcmp(n, "hover") == 0 || strcmp(n, "any-hover") == 0) return "hover";
    if (strcmp(n, "pointer") == 0 || strcmp(n, "any-pointer") == 0)
        return "fine";
    if (strcmp(n, "update") == 0) return "fast";
    if (strcmp(n, "overflow-block") == 0) return "scroll";
    if (strcmp(n, "overflow-inline") == 0) return "scroll";
    if (strcmp(n, "scripting") == 0) return "enabled";
    if (strcmp(n, "display-mode") == 0) return "browser";
    if (strcmp(n, "forced-colors") == 0) return "none";
    if (strcmp(n, "inverted-colors") == 0) return "none";
    if (strcmp(n, "color-gamut") == 0) return "srgb";
    if (strcmp(n, "dynamic-range") == 0 ||
        strcmp(n, "video-dynamic-range") == 0) return "standard";
    if (strcmp(n, "prefers-color-scheme") == 0)
        return ns_css_get_color_scheme() == NS_CSS_COLOR_SCHEME_DARK
                   ? "dark" : "light";
    if (strcmp(n, "prefers-reduced-motion") == 0)
        return ns_css_get_reduced_motion() == NS_CSS_REDUCED_MOTION_REDUCE
                   ? "reduce" : "no-preference";
    if (strcmp(n, "prefers-reduced-transparency") == 0) return "no-preference";
    if (strcmp(n, "prefers-contrast") == 0) return "no-preference";
    if (strcmp(n, "prefers-reduced-data") == 0) return "no-preference";
    if (strcmp(n, "scan") == 0) return NULL;
    return NULL;
}

static gboolean
mq_feature_current(const mq_feature_def *f, double *num, double *denom)
{
    const char *n = f->name;
    *denom = 1;
    if (strcmp(n, "width") == 0 || strcmp(n, "inline-size") == 0)
        *num = mq_vw();
    else if (strcmp(n, "height") == 0 || strcmp(n, "block-size") == 0)
        *num = mq_vh();
    else if (strcmp(n, "device-width") == 0)
        *num = g_mq_device_w;
    else if (strcmp(n, "device-height") == 0)
        *num = g_mq_device_h;
    else if (strcmp(n, "aspect-ratio") == 0) {
        *num = mq_vw();
        *denom = mq_vh();
    } else if (strcmp(n, "device-aspect-ratio") == 0) {
        *num = g_mq_device_w;
        *denom = g_mq_device_h;
    } else if (strcmp(n, "resolution") == 0)
        *num = 1.0;
    else if (strcmp(n, "color") == 0)
        *num = 8;
    else if (strcmp(n, "monochrome") == 0 || strcmp(n, "color-index") == 0 ||
             strcmp(n, "grid") == 0)
        *num = 0;
    else
        return FALSE;
    return TRUE;
}

static mq_tri
mq_compare(double a, double ad, mq_op op, double b, double bd)
{
    double lhs = a * bd;
    double rhs = b * ad;
    switch (op) {
    case MQOP_EQ: return lhs == rhs ? MQ_YES : MQ_NO;
    case MQOP_LT: return lhs < rhs ? MQ_YES : MQ_NO;
    case MQOP_LE: return lhs <= rhs ? MQ_YES : MQ_NO;
    case MQOP_GT: return lhs > rhs ? MQ_YES : MQ_NO;
    case MQOP_GE: return lhs >= rhs ? MQ_YES : MQ_NO;
    }
    return MQ_DUNNO;
}

static mq_tri
mq_eval_feature(const mq_node *n)
{
    const mq_feature_def *f = n->feature;
    if (f->type == MQF_DISCRETE) {
        const char *cur = mq_discrete_current(f);
        if (n->nops == 0) {
            if (!cur) return MQ_NO;
            return strcmp(cur, "none") == 0 ||
                   strcmp(cur, "no-preference") == 0 ? MQ_NO : MQ_YES;
        }
        if (!cur) return MQ_NO;
        return strcmp(cur, n->v1.ident) == 0 ? MQ_YES : MQ_NO;
    }
    double cur = 0, curd = 1;
    if (!mq_feature_current(f, &cur, &curd)) return MQ_DUNNO;
    if (n->nops == 0) {
        if (n->minmax) return MQ_DUNNO;
        if (f->type == MQF_RATIO)
            return cur > 0 && curd > 0 ? MQ_YES : MQ_NO;
        return cur != 0 ? MQ_YES : MQ_NO;
    }
    if (n->nops == 2) {
        mq_op flip1 = n->op1 == MQOP_LT ? MQOP_GT
                    : n->op1 == MQOP_LE ? MQOP_GE
                    : n->op1 == MQOP_GT ? MQOP_LT
                    : n->op1 == MQOP_GE ? MQOP_LE : MQOP_EQ;
        mq_tri a = mq_compare(cur, curd, flip1, n->v1.num, n->v1.denom);
        mq_tri b = mq_compare(cur, curd, n->op2, n->v2.num, n->v2.denom);
        return mq_and(a, b);
    }
    return mq_compare(cur, curd, n->op1, n->v1.num, n->v1.denom);
}

static mq_tri
mq_eval_node(const mq_node *n, int depth)
{
    if (!n || depth > MQ_MAX_DEPTH) return MQ_DUNNO;
    switch (n->kind) {
    case MQN_ENCLOSED:
        return MQ_DUNNO;
    case MQN_FEATURE:
        return mq_eval_feature(n);
    case MQN_NOT:
        return mq_not(mq_eval_node(g_ptr_array_index(n->kids, 0), depth + 1));
    case MQN_AND: {
        mq_tri r = MQ_YES;
        for (guint i = 0; i < n->kids->len; i++)
            r = mq_and(r, mq_eval_node(g_ptr_array_index(n->kids, i),
                                       depth + 1));
        return r;
    }
    case MQN_OR: {
        mq_tri r = MQ_NO;
        for (guint i = 0; i < n->kids->len; i++)
            r = mq_or(r, mq_eval_node(g_ptr_array_index(n->kids, i),
                                      depth + 1));
        return r;
    }
    case MQN_TYPE:
        break;
    }
    return MQ_DUNNO;
}

static mq_tri
mq_eval_type(const char *type)
{
    if (!type) return MQ_YES;
    if (g_ascii_strcasecmp(type, "all") == 0 ||
        g_ascii_strcasecmp(type, "screen") == 0)
        return MQ_YES;
    return MQ_NO;
}

static mq_tri
mq_eval_query(const mq_query *q)
{
    if (!q->valid) return MQ_NO;
    mq_tri r = mq_eval_type(q->type);
    if (q->cond) r = mq_and(r, mq_eval_node(q->cond, 0));
    if (q->negated) r = mq_not(r);
    return r;
}

static const char *
mq_split_next(const char *p, const char *end, const char **seg_end)
{
    int paren = 0, bracket = 0;
    const char *q = p;
    while (q < end) {
        char c = *q;
        if (c == '(') paren++;
        else if (c == ')') { if (paren > 0) paren--; }
        else if (c == '[') bracket++;
        else if (c == ']') { if (bracket > 0) bracket--; }
        else if (c == ',' && paren == 0 && bracket == 0) {
            *seg_end = q;
            return q + 1;
        }
        q++;
    }
    *seg_end = end;
    return end;
}

gboolean
ns_css_media_query_matches(const char *query)
{
    if (!query) return TRUE;
    const char *end = query + strlen(query);
    const char *p = mq_skip_ws(query, end);
    if (p == end) return TRUE;
    gboolean any = FALSE;
    while (p < end && !any) {
        const char *seg_end = NULL;
        const char *next = mq_split_next(p, end, &seg_end);
        mq_query q;
        mq_parse_query(p, seg_end, &q);
        if (mq_eval_query(&q) == MQ_YES) any = TRUE;
        mq_query_clear(&q);
        p = next;
    }
    return any;
}

static void
mq_serialize_value(GString *out, const mq_value *v, mq_feature_type type)
{
    if (type == MQF_DISCRETE) {
        g_string_append(out, v->ident);
        return;
    }
    if (type == MQF_INTEGER) {
        g_string_append_printf(out, "%g", v->num);
        return;
    }
    if (type == MQF_RATIO) {
        g_string_append_printf(out, "%g / %g", v->num, v->denom);
        return;
    }
    if (type == MQF_RESOLUTION) {
        g_string_append_printf(out, "%gdppx", v->num);
        return;
    }
    g_string_append_printf(out, "%gpx", v->num);
}

static const char *
mq_op_str(mq_op op)
{
    switch (op) {
    case MQOP_EQ: return "=";
    case MQOP_LT: return "<";
    case MQOP_LE: return "<=";
    case MQOP_GT: return ">";
    case MQOP_GE: return ">=";
    }
    return "=";
}

static void
mq_serialize_node(GString *out, const mq_node *n, int depth)
{
    if (!n || depth > MQ_MAX_DEPTH) return;
    switch (n->kind) {
    case MQN_ENCLOSED:
        if (n->text && n->text[0] == '(')
            g_string_append(out, n->text);
        else if (n->text && strchr(n->text, '('))
            g_string_append(out, n->text);
        else {
            g_string_append_c(out, '(');
            if (n->text) g_string_append(out, n->text);
            g_string_append_c(out, ')');
        }
        break;
    case MQN_FEATURE:
        g_string_append_c(out, '(');
        if (n->nops == 0) {
            g_string_append(out, n->feature->name);
        } else if (n->plain) {
            if (n->minmax == 1) g_string_append(out, "min-");
            else if (n->minmax == 2) g_string_append(out, "max-");
            g_string_append(out, n->feature->name);
            g_string_append(out, ": ");
            mq_serialize_value(out, &n->v1, n->feature->type);
        } else if (n->nops == 2) {
            mq_serialize_value(out, &n->v1, n->feature->type);
            g_string_append_printf(out, " %s %s %s ", mq_op_str(n->op1),
                                   n->feature->name, mq_op_str(n->op2));
            mq_serialize_value(out, &n->v2, n->feature->type);
        } else {
            g_string_append(out, n->feature->name);
            g_string_append_printf(out, " %s ", mq_op_str(n->op1));
            mq_serialize_value(out, &n->v1, n->feature->type);
        }
        g_string_append_c(out, ')');
        break;
    case MQN_NOT:
        g_string_append(out, "not ");
        mq_serialize_node(out, g_ptr_array_index(n->kids, 0), depth + 1);
        break;
    case MQN_AND:
    case MQN_OR:
        if (n->kids->len == 1 && depth > 0) {
            g_string_append_c(out, '(');
            mq_serialize_node(out, g_ptr_array_index(n->kids, 0), depth + 1);
            g_string_append_c(out, ')');
            break;
        }
        for (guint i = 0; i < n->kids->len; i++) {
            if (i > 0)
                g_string_append(out, n->kind == MQN_AND ? " and " : " or ");
            mq_serialize_node(out, g_ptr_array_index(n->kids, i), depth + 1);
        }
        break;
    case MQN_TYPE:
        break;
    }
}

static void
mq_serialize_query(GString *out, const mq_query *q)
{
    if (!q->valid) {
        g_string_append(out, "not all");
        return;
    }
    if (q->type) {
        if (q->negated) g_string_append(out, "not ");
        else if (q->only) g_string_append(out, "only ");
        char *lower = g_ascii_strdown(q->type, -1);
        g_string_append(out, lower);
        g_free(lower);
        if (q->cond) {
            g_string_append(out, " and ");
            mq_serialize_node(out, q->cond, 1);
        }
        return;
    }
    mq_serialize_node(out, q->cond, 0);
}

char *
ns_css_media_list_serialize(const char *query)
{
    if (!query) return g_strdup("");
    const char *end = query + strlen(query);
    const char *p = mq_skip_ws(query, end);
    if (p == end) return g_strdup("");
    GString *out = g_string_new(NULL);
    gboolean first = TRUE;
    while (p < end) {
        const char *seg_end = NULL;
        const char *next = mq_split_next(p, end, &seg_end);
        mq_query q;
        mq_parse_query(p, seg_end, &q);
        if (!first) g_string_append(out, ", ");
        mq_serialize_query(out, &q);
        mq_query_clear(&q);
        first = FALSE;
        p = next;
    }
    return g_string_free(out, FALSE);
}
