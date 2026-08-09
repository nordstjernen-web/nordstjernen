/* Northstar — registered custom property <syntax> grammar and matching. */

#include "css_prop_syntax.h"

#include "css.h"
#include "css_syntax.h"

#include <math.h>
#include <string.h>

typedef enum {
    NS_SYN_UNIVERSAL,
    NS_SYN_LENGTH,
    NS_SYN_NUMBER,
    NS_SYN_PERCENTAGE,
    NS_SYN_LENGTH_PERCENTAGE,
    NS_SYN_COLOR,
    NS_SYN_IMAGE,
    NS_SYN_URL,
    NS_SYN_INTEGER,
    NS_SYN_ANGLE,
    NS_SYN_TIME,
    NS_SYN_RESOLUTION,
    NS_SYN_TRANSFORM_FUNCTION,
    NS_SYN_TRANSFORM_LIST,
    NS_SYN_CUSTOM_IDENT,
    NS_SYN_STRING,
    NS_SYN_IDENT,
} ns_syn_kind;

typedef enum {
    NS_SYN_MULT_NONE,
    NS_SYN_MULT_SPACE,
    NS_SYN_MULT_COMMA,
} ns_syn_mult;

typedef struct {
    ns_syn_kind kind;
    ns_syn_mult mult;
    char *ident;
} ns_syn_component;

struct ns_css_syntax_def {
    gboolean universal;
    GArray *components;
};

typedef enum {
    NS_LEN_PX, NS_LEN_CM, NS_LEN_MM, NS_LEN_Q, NS_LEN_IN, NS_LEN_PT,
    NS_LEN_PC,
    NS_LEN_EM, NS_LEN_EX, NS_LEN_CH, NS_LEN_IC, NS_LEN_CAP, NS_LEN_LH,
    NS_LEN_REM, NS_LEN_REX, NS_LEN_RCH, NS_LEN_RIC, NS_LEN_RCAP, NS_LEN_RLH,
    NS_LEN_VW, NS_LEN_VH, NS_LEN_VI, NS_LEN_VB, NS_LEN_VMIN, NS_LEN_VMAX,
    NS_LEN_CQW, NS_LEN_CQH, NS_LEN_CQI, NS_LEN_CQB, NS_LEN_CQMIN, NS_LEN_CQMAX,
    NS_LEN_COUNT,
} ns_len_unit;

static const struct { const char *name; ns_len_unit unit; } kLengthUnits[] = {
    { "px", NS_LEN_PX }, { "cm", NS_LEN_CM }, { "mm", NS_LEN_MM },
    { "q", NS_LEN_Q }, { "in", NS_LEN_IN }, { "pt", NS_LEN_PT },
    { "pc", NS_LEN_PC },
    { "em", NS_LEN_EM }, { "ex", NS_LEN_EX }, { "ch", NS_LEN_CH },
    { "ic", NS_LEN_IC }, { "cap", NS_LEN_CAP }, { "lh", NS_LEN_LH },
    { "rem", NS_LEN_REM }, { "rex", NS_LEN_REX }, { "rch", NS_LEN_RCH },
    { "ric", NS_LEN_RIC }, { "rcap", NS_LEN_RCAP }, { "rlh", NS_LEN_RLH },
    { "vw", NS_LEN_VW }, { "vh", NS_LEN_VH }, { "vi", NS_LEN_VI },
    { "vb", NS_LEN_VB }, { "vmin", NS_LEN_VMIN }, { "vmax", NS_LEN_VMAX },
    { "svw", NS_LEN_VW }, { "svh", NS_LEN_VH }, { "svi", NS_LEN_VI },
    { "svb", NS_LEN_VB }, { "svmin", NS_LEN_VMIN }, { "svmax", NS_LEN_VMAX },
    { "lvw", NS_LEN_VW }, { "lvh", NS_LEN_VH }, { "lvi", NS_LEN_VI },
    { "lvb", NS_LEN_VB }, { "lvmin", NS_LEN_VMIN }, { "lvmax", NS_LEN_VMAX },
    { "dvw", NS_LEN_VW }, { "dvh", NS_LEN_VH }, { "dvi", NS_LEN_VI },
    { "dvb", NS_LEN_VB }, { "dvmin", NS_LEN_VMIN }, { "dvmax", NS_LEN_VMAX },
    { "cqw", NS_LEN_CQW }, { "cqh", NS_LEN_CQH }, { "cqi", NS_LEN_CQI },
    { "cqb", NS_LEN_CQB }, { "cqmin", NS_LEN_CQMIN },
    { "cqmax", NS_LEN_CQMAX },
};

static gboolean
len_unit_font_relative(ns_len_unit unit)
{
    switch (unit) {
    case NS_LEN_EM: case NS_LEN_EX: case NS_LEN_CH: case NS_LEN_IC:
    case NS_LEN_CAP: case NS_LEN_LH:
    case NS_LEN_REM: case NS_LEN_REX: case NS_LEN_RCH: case NS_LEN_RIC:
    case NS_LEN_RCAP: case NS_LEN_RLH:
        return TRUE;
    default:
        return FALSE;
    }
}

typedef enum {
    NS_MATH_INVALID,
    NS_MATH_NUMBER,
    NS_MATH_LENGTH,
    NS_MATH_PERCENT,
    NS_MATH_ANGLE,
    NS_MATH_TIME,
    NS_MATH_RESOLUTION,
    NS_MATH_FLEX,
} ns_math_kind;

typedef struct {
    ns_math_kind kind;
    double number;
    double percent;
    gboolean has_percent;
    double len[NS_LEN_COUNT];
} ns_math_value;

static gboolean
math_is_zero(const ns_math_value *v)
{
    if (v->has_percent && v->percent != 0) return FALSE;
    for (int i = 0; i < NS_LEN_COUNT; i++)
        if (v->len[i] != 0) return FALSE;
    return v->number == 0;
}

static gboolean
math_has_length_terms(const ns_math_value *v)
{
    for (int i = 0; i < NS_LEN_COUNT; i++)
        if (v->len[i] != 0) return TRUE;
    return FALSE;
}

static gboolean
math_font_relative(const ns_math_value *v)
{
    for (int i = 0; i < NS_LEN_COUNT; i++)
        if (v->len[i] != 0 && len_unit_font_relative((ns_len_unit)i))
            return TRUE;
    return FALSE;
}

static void
math_scale(ns_math_value *v, double f)
{
    v->number *= f;
    v->percent *= f;
    for (int i = 0; i < NS_LEN_COUNT; i++) v->len[i] *= f;
}

static gboolean
math_add(ns_math_value *a, const ns_math_value *b, double sign)
{
    if (a->kind == NS_MATH_INVALID || b->kind == NS_MATH_INVALID) return FALSE;
    ns_math_kind ka = a->kind, kb = b->kind;
    if (ka != kb) {
        gboolean a_zero = math_is_zero(a), b_zero = math_is_zero(b);
        if (ka == NS_MATH_LENGTH && kb == NS_MATH_PERCENT) {
        } else if (ka == NS_MATH_PERCENT && kb == NS_MATH_LENGTH) {
            a->kind = NS_MATH_LENGTH;
        } else if (a_zero && ka == NS_MATH_NUMBER) {
            a->kind = kb;
        } else if (b_zero && kb == NS_MATH_NUMBER) {
        } else {
            return FALSE;
        }
    }
    a->number += sign * b->number;
    a->percent += sign * b->percent;
    a->has_percent = a->has_percent || b->has_percent;
    for (int i = 0; i < NS_LEN_COUNT; i++) a->len[i] += sign * b->len[i];
    return TRUE;
}

static const char *g_math_input;

static char *
component_text(const ns_css_component *c)
{
    return g_strndup(g_math_input + c->start, c->end - c->start);
}

static gboolean
component_is_ws(const ns_css_component *c)
{
    return c->type == NS_CSS_COMPONENT_WHITESPACE;
}

static gboolean
math_function_name(const char *name)
{
    static const char *const fns[] = {
        "calc", "min", "max", "clamp", "round", "mod", "rem", "abs", "sign",
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt",
        "hypot", "log", "exp", "calc-size",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(fns); i++)
        if (g_ascii_strcasecmp(name, fns[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean math_eval_sum(GPtrArray *items, guint *pos, guint end,
                              ns_math_value *out, int depth);

static gboolean
math_eval_dimension(const ns_css_component *c, ns_math_value *out)
{
    memset(out, 0, sizeof(*out));
    if (c->type == NS_CSS_COMPONENT_NUMBER) {
        out->kind = NS_MATH_NUMBER;
        out->number = c->number;
        return TRUE;
    }
    if (c->type == NS_CSS_COMPONENT_PERCENTAGE) {
        out->kind = NS_MATH_PERCENT;
        out->percent = c->number;
        out->has_percent = TRUE;
        return TRUE;
    }
    if (c->type != NS_CSS_COMPONENT_DIMENSION) return FALSE;
    const char *unit = c->value ? c->value : "";
    for (gsize i = 0; i < G_N_ELEMENTS(kLengthUnits); i++) {
        if (g_ascii_strcasecmp(unit, kLengthUnits[i].name) == 0) {
            out->kind = NS_MATH_LENGTH;
            out->len[kLengthUnits[i].unit] = c->number;
            return TRUE;
        }
    }
    if (g_ascii_strcasecmp(unit, "deg") == 0 ||
        g_ascii_strcasecmp(unit, "grad") == 0 ||
        g_ascii_strcasecmp(unit, "rad") == 0 ||
        g_ascii_strcasecmp(unit, "turn") == 0) {
        double scale = g_ascii_strcasecmp(unit, "deg") == 0 ? 1.0
                     : g_ascii_strcasecmp(unit, "grad") == 0 ? 0.9
                     : g_ascii_strcasecmp(unit, "rad") == 0 ? 180.0 / G_PI
                     : 360.0;
        out->kind = NS_MATH_ANGLE;
        out->number = c->number * scale;
        return TRUE;
    }
    if (g_ascii_strcasecmp(unit, "s") == 0 ||
        g_ascii_strcasecmp(unit, "ms") == 0) {
        out->kind = NS_MATH_TIME;
        out->number = g_ascii_strcasecmp(unit, "s") == 0 ? c->number
                                                         : c->number / 1000.0;
        return TRUE;
    }
    if (g_ascii_strcasecmp(unit, "dppx") == 0 ||
        g_ascii_strcasecmp(unit, "x") == 0 ||
        g_ascii_strcasecmp(unit, "dpi") == 0 ||
        g_ascii_strcasecmp(unit, "dpcm") == 0) {
        double scale = g_ascii_strcasecmp(unit, "dpi") == 0 ? 1.0 / 96.0
                     : g_ascii_strcasecmp(unit, "dpcm") == 0 ? 2.54 / 96.0
                     : 1.0;
        out->kind = NS_MATH_RESOLUTION;
        out->number = c->number * scale;
        return TRUE;
    }
    if (g_ascii_strcasecmp(unit, "fr") == 0) {
        out->kind = NS_MATH_FLEX;
        out->number = c->number;
        return TRUE;
    }
    return FALSE;
}

typedef struct { guint lo, hi; } math_range;

static void
math_split_args(GPtrArray *children, GArray *ranges)
{
    guint start = 0;
    if (!children) return;
    for (guint i = 0; i < children->len; i++) {
        const ns_css_component *c = g_ptr_array_index(children, i);
        if (c->type != NS_CSS_COMPONENT_COMMA) continue;
        math_range r = { start, i };
        g_array_append_val(ranges, r);
        start = i + 1;
    }
    math_range tail = { start, children->len };
    g_array_append_val(ranges, tail);
}

static gboolean
math_eval_function(const ns_css_component *fn, ns_math_value *out, int depth)
{
    if (depth > 32) return FALSE;
    const char *name = fn->value ? fn->value : "";
    GPtrArray *children = fn->children;
    if (!children) return FALSE;
    GArray *ranges = g_array_new(FALSE, FALSE, sizeof(math_range));
    math_split_args(children, ranges);
    gboolean ok = FALSE;
    guint nargs = ranges->len;

    if (g_ascii_strcasecmp(name, "calc") == 0 && nargs == 1) {
        math_range r = g_array_index(ranges, math_range, 0);
        guint pos = r.lo;
        ok = math_eval_sum(children, &pos, r.hi, out, depth + 1);
    } else if ((g_ascii_strcasecmp(name, "min") == 0 ||
                g_ascii_strcasecmp(name, "max") == 0 ||
                g_ascii_strcasecmp(name, "clamp") == 0 ||
                g_ascii_strcasecmp(name, "hypot") == 0) && nargs >= 1) {
        ns_math_value acc;
        memset(&acc, 0, sizeof(acc));
        gboolean first = TRUE;
        ok = TRUE;
        for (guint i = 0; i < nargs && ok; i++) {
            math_range r = g_array_index(ranges, math_range, i);
            guint pos = r.lo;
            ns_math_value v;
            if (!math_eval_sum(children, &pos, r.hi, &v, depth + 1)) {
                ok = FALSE;
                break;
            }
            if (first) {
                acc = v;
                first = FALSE;
                continue;
            }
            ns_math_value probe = acc;
            if (!math_add(&probe, &v, 1.0)) { ok = FALSE; break; }
            acc.kind = probe.kind;
            if (math_has_length_terms(&v) || v.has_percent ||
                math_has_length_terms(&acc) || acc.has_percent) {
                for (int k = 0; k < NS_LEN_COUNT; k++)
                    if (v.len[k] != 0) acc.len[k] = v.len[k];
                if (v.has_percent) acc.has_percent = TRUE;
            } else if (g_ascii_strcasecmp(name, "min") == 0) {
                acc.number = MIN(acc.number, v.number);
            } else if (g_ascii_strcasecmp(name, "max") == 0) {
                acc.number = MAX(acc.number, v.number);
            } else if (g_ascii_strcasecmp(name, "hypot") == 0) {
                acc.number = sqrt(acc.number * acc.number +
                                  v.number * v.number);
            } else {
                acc.number = v.number;
            }
        }
        if (ok && !first) *out = acc;
        else ok = FALSE;
    } else if ((g_ascii_strcasecmp(name, "abs") == 0 ||
                g_ascii_strcasecmp(name, "sign") == 0) && nargs == 1) {
        math_range r = g_array_index(ranges, math_range, 0);
        guint pos = r.lo;
        ok = math_eval_sum(children, &pos, r.hi, out, depth + 1);
        if (ok && g_ascii_strcasecmp(name, "abs") == 0) {
            if (out->number < 0) math_scale(out, -1.0);
        } else if (ok) {
            double n = out->number;
            memset(out, 0, sizeof(*out));
            out->kind = NS_MATH_NUMBER;
            out->number = n > 0 ? 1 : n < 0 ? -1 : 0;
        }
    } else if ((g_ascii_strcasecmp(name, "round") == 0 ||
                g_ascii_strcasecmp(name, "mod") == 0 ||
                g_ascii_strcasecmp(name, "rem") == 0) && nargs >= 2) {
        ns_math_value a, b;
        math_range r0 = g_array_index(ranges, math_range, nargs - 2);
        math_range r1 = g_array_index(ranges, math_range, nargs - 1);
        guint p0 = r0.lo, p1 = r1.lo;
        ok = math_eval_sum(children, &p0, r0.hi, &a, depth + 1) &&
             math_eval_sum(children, &p1, r1.hi, &b, depth + 1);
        if (ok) {
            ns_math_value probe = a;
            ok = math_add(&probe, &b, 1.0);
            *out = a;
        }
    } else if ((g_ascii_strcasecmp(name, "sin") == 0 ||
                g_ascii_strcasecmp(name, "cos") == 0 ||
                g_ascii_strcasecmp(name, "tan") == 0 ||
                g_ascii_strcasecmp(name, "sqrt") == 0 ||
                g_ascii_strcasecmp(name, "exp") == 0 ||
                g_ascii_strcasecmp(name, "log") == 0 ||
                g_ascii_strcasecmp(name, "pow") == 0) && nargs >= 1) {
        double args[2] = { 0, 0 };
        ok = TRUE;
        for (guint i = 0; i < nargs && i < 2 && ok; i++) {
            math_range r = g_array_index(ranges, math_range, i);
            guint pos = r.lo;
            ns_math_value v;
            if (!math_eval_sum(children, &pos, r.hi, &v, depth + 1) ||
                math_has_length_terms(&v) || v.has_percent) {
                ok = FALSE;
                break;
            }
            args[i] = v.kind == NS_MATH_ANGLE ? v.number * G_PI / 180.0
                                              : v.number;
        }
        if (ok) {
            memset(out, 0, sizeof(*out));
            out->kind = NS_MATH_NUMBER;
            out->number =
                g_ascii_strcasecmp(name, "sin") == 0 ? sin(args[0]) :
                g_ascii_strcasecmp(name, "cos") == 0 ? cos(args[0]) :
                g_ascii_strcasecmp(name, "tan") == 0 ? tan(args[0]) :
                g_ascii_strcasecmp(name, "sqrt") == 0 ? sqrt(args[0]) :
                g_ascii_strcasecmp(name, "exp") == 0 ? exp(args[0]) :
                g_ascii_strcasecmp(name, "log") == 0
                    ? (nargs > 1 ? log(args[0]) / log(args[1]) : log(args[0]))
                    : pow(args[0], args[1]);
        }
    } else if ((g_ascii_strcasecmp(name, "asin") == 0 ||
                g_ascii_strcasecmp(name, "acos") == 0 ||
                g_ascii_strcasecmp(name, "atan") == 0 ||
                g_ascii_strcasecmp(name, "atan2") == 0) && nargs >= 1) {
        double args[2] = { 0, 0 };
        ok = TRUE;
        for (guint i = 0; i < nargs && i < 2 && ok; i++) {
            math_range r = g_array_index(ranges, math_range, i);
            guint pos = r.lo;
            ns_math_value v;
            if (!math_eval_sum(children, &pos, r.hi, &v, depth + 1) ||
                math_has_length_terms(&v) || v.has_percent) {
                ok = FALSE;
                break;
            }
            args[i] = v.number;
        }
        if (ok) {
            double radians =
                g_ascii_strcasecmp(name, "asin") == 0 ? asin(args[0]) :
                g_ascii_strcasecmp(name, "acos") == 0 ? acos(args[0]) :
                g_ascii_strcasecmp(name, "atan") == 0 ? atan(args[0])
                                                      : atan2(args[0], args[1]);
            memset(out, 0, sizeof(*out));
            out->kind = NS_MATH_ANGLE;
            out->number = radians * 180.0 / G_PI;
        }
    }
    g_array_free(ranges, TRUE);
    return ok;
}

static gboolean
math_eval_term(GPtrArray *items, guint *pos, guint end, ns_math_value *out,
               int depth)
{
    while (*pos < end && component_is_ws(g_ptr_array_index(items, *pos)))
        (*pos)++;
    if (*pos >= end) return FALSE;
    const ns_css_component *c = g_ptr_array_index(items, *pos);
    if (c->type == NS_CSS_COMPONENT_BLOCK && c->delimiter == ')') {
        guint inner = 0;
        gboolean ok = c->children &&
                      math_eval_sum(c->children, &inner, c->children->len,
                                    out, depth + 1);
        (*pos)++;
        return ok;
    }
    if (c->type == NS_CSS_COMPONENT_FUNCTION) {
        if (!math_function_name(c->value ? c->value : "")) return FALSE;
        gboolean ok = math_eval_function(c, out, depth);
        (*pos)++;
        return ok;
    }
    if (!math_eval_dimension(c, out)) return FALSE;
    (*pos)++;
    return TRUE;
}

static gboolean
math_eval_product(GPtrArray *items, guint *pos, guint end, ns_math_value *out,
                  int depth)
{
    if (!math_eval_term(items, pos, end, out, depth)) return FALSE;
    for (;;) {
        guint save = *pos;
        while (*pos < end && component_is_ws(g_ptr_array_index(items, *pos)))
            (*pos)++;
        if (*pos >= end) { *pos = save; return TRUE; }
        const ns_css_component *op = g_ptr_array_index(items, *pos);
        if (op->type != NS_CSS_COMPONENT_DELIM ||
            (op->delimiter != '*' && op->delimiter != '/')) {
            *pos = save;
            return TRUE;
        }
        char sign = op->delimiter;
        (*pos)++;
        ns_math_value rhs;
        if (!math_eval_term(items, pos, end, &rhs, depth)) return FALSE;
        if (sign == '*') {
            if (out->kind == NS_MATH_NUMBER &&
                !math_has_length_terms(out) && !out->has_percent) {
                double f = out->number;
                *out = rhs;
                math_scale(out, f);
            } else if (rhs.kind == NS_MATH_NUMBER &&
                       !math_has_length_terms(&rhs) && !rhs.has_percent) {
                math_scale(out, rhs.number);
            } else {
                return FALSE;
            }
        } else {
            if (rhs.kind != NS_MATH_NUMBER || math_has_length_terms(&rhs) ||
                rhs.has_percent || rhs.number == 0)
                return FALSE;
            math_scale(out, 1.0 / rhs.number);
        }
    }
}

static gboolean
math_eval_sum(GPtrArray *items, guint *pos, guint end, ns_math_value *out,
              int depth)
{
    if (depth > 32) return FALSE;
    if (!math_eval_product(items, pos, end, out, depth)) return FALSE;
    for (;;) {
        gboolean saw_ws = FALSE;
        while (*pos < end && component_is_ws(g_ptr_array_index(items, *pos))) {
            saw_ws = TRUE;
            (*pos)++;
        }
        if (*pos >= end) return TRUE;
        const ns_css_component *op = g_ptr_array_index(items, *pos);
        if (op->type != NS_CSS_COMPONENT_DELIM ||
            (op->delimiter != '+' && op->delimiter != '-'))
            return FALSE;
        if (!saw_ws) return FALSE;
        double sign = op->delimiter == '+' ? 1.0 : -1.0;
        (*pos)++;
        if (*pos >= end ||
            !component_is_ws(g_ptr_array_index(items, *pos)))
            return FALSE;
        ns_math_value rhs;
        if (!math_eval_product(items, pos, end, &rhs, depth)) return FALSE;
        if (!math_add(out, &rhs, sign)) return FALSE;
    }
}

static gboolean
math_eval_component(const ns_css_component *c, ns_math_value *out)
{
    if (c->type == NS_CSS_COMPONENT_FUNCTION) {
        if (!math_function_name(c->value ? c->value : "")) return FALSE;
        return math_eval_function(c, out, 0);
    }
    return math_eval_dimension(c, out);
}

static char *
syn_ident_unescape(const char *text, gssize len)
{
    GString *out = g_string_new(NULL);
    const char *p = text;
    const char *end = text + (len < 0 ? (gssize)strlen(text) : len);
    while (p < end) {
        if (*p == '\\') {
            const char *q = p;
            ns_css_append_unescaped(out, &q);
            if (q == p) { g_string_append_c(out, *p); p++; }
            else p = q;
            continue;
        }
        g_string_append_c(out, *p);
        p++;
    }
    return g_string_free(out, FALSE);
}

static gboolean
syn_css_wide_keyword(const char *ident)
{
    static const char *const kw[] = {
        "initial", "inherit", "unset", "revert", "revert-layer",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(kw); i++)
        if (g_ascii_strcasecmp(ident, kw[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
syn_reserved_ident(const char *ident)
{
    return syn_css_wide_keyword(ident) ||
           g_ascii_strcasecmp(ident, "default") == 0;
}

static const struct { const char *name; ns_syn_kind kind; } kSyntaxTypes[] = {
    { "length", NS_SYN_LENGTH },
    { "number", NS_SYN_NUMBER },
    { "percentage", NS_SYN_PERCENTAGE },
    { "length-percentage", NS_SYN_LENGTH_PERCENTAGE },
    { "color", NS_SYN_COLOR },
    { "image", NS_SYN_IMAGE },
    { "url", NS_SYN_URL },
    { "integer", NS_SYN_INTEGER },
    { "angle", NS_SYN_ANGLE },
    { "time", NS_SYN_TIME },
    { "resolution", NS_SYN_RESOLUTION },
    { "transform-function", NS_SYN_TRANSFORM_FUNCTION },
    { "transform-list", NS_SYN_TRANSFORM_LIST },
    { "custom-ident", NS_SYN_CUSTOM_IDENT },
    { "string", NS_SYN_STRING },
};

static gboolean
syn_ident_valid(const char *text, gsize len)
{
    if (len == 0) return FALSE;
    gboolean valid = FALSE;
    GPtrArray *items = ns_css_component_values_parse(text, (gssize)len, &valid);
    gboolean ok = valid && items->len == 1;
    if (ok) {
        const ns_css_component *c = g_ptr_array_index(items, 0);
        ok = c->type == NS_CSS_COMPONENT_IDENT;
    }
    g_ptr_array_free(items, TRUE);
    return ok;
}

static gboolean
syn_parse_component(const char *text, gsize len, ns_syn_component *out)
{
    while (len && g_ascii_isspace(text[len - 1])) len--;
    while (len && g_ascii_isspace(*text)) { text++; len--; }
    if (len == 0) return FALSE;

    out->mult = NS_SYN_MULT_NONE;
    if (text[len - 1] == '+') {
        out->mult = NS_SYN_MULT_SPACE;
        len--;
    } else if (text[len - 1] == '#') {
        out->mult = NS_SYN_MULT_COMMA;
        len--;
    }
    if (len == 0) return FALSE;

    if (text[0] == '<') {
        if (text[len - 1] != '>') return FALSE;
        const char *inner = text + 1;
        gsize inner_len = len - 2;
        for (gsize i = 0; i < G_N_ELEMENTS(kSyntaxTypes); i++) {
            const char *name = kSyntaxTypes[i].name;
            if (strlen(name) != inner_len ||
                strncmp(name, inner, inner_len) != 0)
                continue;
            out->kind = kSyntaxTypes[i].kind;
            if (out->kind == NS_SYN_TRANSFORM_LIST &&
                out->mult != NS_SYN_MULT_NONE)
                return FALSE;
            return TRUE;
        }
        return FALSE;
    }
    if (!syn_ident_valid(text, len)) return FALSE;
    char *ident = syn_ident_unescape(text, (gssize)len);
    if (syn_reserved_ident(ident)) {
        g_free(ident);
        return FALSE;
    }
    out->kind = NS_SYN_IDENT;
    out->ident = ident;
    return TRUE;
}

static void
syn_component_clear(gpointer data)
{
    ns_syn_component *c = data;
    g_free(c->ident);
}

ns_css_syntax_def *
ns_css_syntax_def_parse(const char *text)
{
    if (!text) return NULL;
    const char *p = text;
    const char *end = text + strlen(text);
    while (p < end && g_ascii_isspace(*p)) p++;
    while (end > p && g_ascii_isspace(end[-1])) end--;
    if (p == end) return NULL;

    ns_css_syntax_def *syntax = g_new0(ns_css_syntax_def, 1);
    if (end - p == 1 && *p == '*') {
        syntax->universal = TRUE;
        return syntax;
    }
    syntax->components = g_array_new(FALSE, TRUE, sizeof(ns_syn_component));
    g_array_set_clear_func(syntax->components, syn_component_clear);

    const char *seg = p;
    for (const char *q = p; q <= end; q++) {
        if (q < end && *q == '\\') {
            if (q + 1 < end) q++;
            continue;
        }
        if (q < end && *q != '|') continue;
        ns_syn_component comp = { 0 };
        if (!syn_parse_component(seg, (gsize)(q - seg), &comp)) {
            ns_css_syntax_def_free(syntax);
            return NULL;
        }
        g_array_append_val(syntax->components, comp);
        seg = q + 1;
    }
    if (syntax->components->len == 0) {
        ns_css_syntax_def_free(syntax);
        return NULL;
    }
    return syntax;
}

void
ns_css_syntax_def_free(ns_css_syntax_def *syntax)
{
    if (!syntax) return;
    if (syntax->components) g_array_free(syntax->components, TRUE);
    g_free(syntax);
}

gboolean
ns_css_syntax_def_universal(const ns_css_syntax_def *syntax)
{
    return syntax && syntax->universal;
}

typedef struct {
    GPtrArray *items;
    guint lo;
    guint hi;
} syn_chunk;

static void
chunk_trim(syn_chunk *chunk)
{
    while (chunk->lo < chunk->hi &&
           component_is_ws(g_ptr_array_index(chunk->items, chunk->lo)))
        chunk->lo++;
    while (chunk->hi > chunk->lo &&
           component_is_ws(g_ptr_array_index(chunk->items, chunk->hi - 1)))
        chunk->hi--;
}

static gboolean
syn_color_function(const char *name)
{
    static const char *const fns[] = {
        "rgb", "rgba", "hsl", "hsla", "hwb", "lab", "lch", "oklab", "oklch",
        "color", "color-mix", "light-dark", "contrast-color", "device-cmyk",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(fns); i++)
        if (g_ascii_strcasecmp(name, fns[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
syn_image_function(const char *name)
{
    static const char *const fns[] = {
        "url", "src", "linear-gradient", "radial-gradient", "conic-gradient",
        "repeating-linear-gradient", "repeating-radial-gradient",
        "repeating-conic-gradient", "image", "image-set", "-webkit-image-set",
        "cross-fade", "paint", "light-dark", "-webkit-linear-gradient",
        "-webkit-radial-gradient", "-webkit-gradient",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(fns); i++)
        if (g_ascii_strcasecmp(name, fns[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
syn_transform_function(const char *name, guint argc)
{
    static const struct { const char *name; guint min; guint max; } fns[] = {
        { "translate", 1, 2 }, { "translatex", 1, 1 }, { "translatey", 1, 1 },
        { "translatez", 1, 1 }, { "translate3d", 3, 3 },
        { "scale", 1, 2 }, { "scalex", 1, 1 }, { "scaley", 1, 1 },
        { "scalez", 1, 1 }, { "scale3d", 3, 3 },
        { "rotate", 1, 1 }, { "rotatex", 1, 1 }, { "rotatey", 1, 1 },
        { "rotatez", 1, 1 }, { "rotate3d", 4, 4 },
        { "skew", 1, 2 }, { "skewx", 1, 1 }, { "skewy", 1, 1 },
        { "matrix", 6, 6 }, { "matrix3d", 16, 16 },
        { "perspective", 1, 1 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(fns); i++)
        if (g_ascii_strcasecmp(name, fns[i].name) == 0)
            return argc >= fns[i].min && argc <= fns[i].max;
    return FALSE;
}

static gboolean
syn_number_is_integer(const ns_css_component *c)
{
    for (gsize i = c->start; i < c->end; i++) {
        char ch = g_math_input[i];
        if (ch == '.' || ch == 'e' || ch == 'E') return FALSE;
    }
    return TRUE;
}

static gboolean
syn_match_numeric(ns_syn_kind kind, const syn_chunk *chunk,
                  gboolean independent, ns_math_value *out)
{
    if (chunk->hi - chunk->lo != 1) return FALSE;
    const ns_css_component *c = g_ptr_array_index(chunk->items, chunk->lo);
    ns_math_value v;
    if (!math_eval_component(c, &v)) return FALSE;
    gboolean is_math = c->type == NS_CSS_COMPONENT_FUNCTION;
    if (independent && math_font_relative(&v)) return FALSE;

    switch (kind) {
    case NS_SYN_NUMBER:
        if (v.kind != NS_MATH_NUMBER || math_has_length_terms(&v) ||
            v.has_percent)
            return FALSE;
        break;
    case NS_SYN_INTEGER:
        if (v.kind != NS_MATH_NUMBER || math_has_length_terms(&v) ||
            v.has_percent)
            return FALSE;
        if (!is_math && !syn_number_is_integer(c)) return FALSE;
        break;
    case NS_SYN_LENGTH:
        if (v.kind == NS_MATH_NUMBER && !is_math && v.number == 0) break;
        if (v.kind != NS_MATH_LENGTH || v.has_percent) return FALSE;
        break;
    case NS_SYN_PERCENTAGE:
        if (v.kind != NS_MATH_PERCENT || math_has_length_terms(&v))
            return FALSE;
        break;
    case NS_SYN_LENGTH_PERCENTAGE:
        if (v.kind == NS_MATH_NUMBER && !is_math && v.number == 0) break;
        if (v.kind != NS_MATH_LENGTH && v.kind != NS_MATH_PERCENT)
            return FALSE;
        break;
    case NS_SYN_ANGLE:
        if (v.kind != NS_MATH_ANGLE) return FALSE;
        break;
    case NS_SYN_TIME:
        if (v.kind != NS_MATH_TIME) return FALSE;
        break;
    case NS_SYN_RESOLUTION:
        if (v.kind != NS_MATH_RESOLUTION || v.number < 0) return FALSE;
        break;
    default:
        return FALSE;
    }
    if (out) *out = v;
    return TRUE;
}

static const double kAbsoluteUnitPx[] = {
    [NS_LEN_PX] = 1.0,
    [NS_LEN_CM] = 96.0 / 2.54,
    [NS_LEN_MM] = 96.0 / 25.4,
    [NS_LEN_Q]  = 96.0 / 101.6,
    [NS_LEN_IN] = 96.0,
    [NS_LEN_PT] = 96.0 / 72.0,
    [NS_LEN_PC] = 16.0,
};

static double
len_unit_px(ns_len_unit unit, const ns_css_syntax_ctx *ctx)
{
    if (unit <= NS_LEN_PC) return kAbsoluteUnitPx[unit];
    if (!ctx) return 0;
    switch (unit) {
    case NS_LEN_EM:    return ctx->font_size;
    case NS_LEN_EX:    return ctx->ex_px;
    case NS_LEN_CH:    return ctx->ch_px;
    case NS_LEN_IC:    return ctx->ic_px;
    case NS_LEN_CAP:   return ctx->cap_px;
    case NS_LEN_LH:    return ctx->line_height;
    case NS_LEN_REM:   return ctx->root_font_size;
    case NS_LEN_REX:   return ctx->root_ex_px;
    case NS_LEN_RCH:   return ctx->root_ch_px;
    case NS_LEN_RIC:   return ctx->root_ic_px;
    case NS_LEN_RCAP:  return ctx->root_cap_px;
    case NS_LEN_RLH:   return ctx->root_line_height;
    case NS_LEN_VW:
    case NS_LEN_VI:    return ctx->viewport_w / 100.0;
    case NS_LEN_VH:
    case NS_LEN_VB:    return ctx->viewport_h / 100.0;
    case NS_LEN_VMIN:  return MIN(ctx->viewport_w, ctx->viewport_h) / 100.0;
    case NS_LEN_VMAX:  return MAX(ctx->viewport_w, ctx->viewport_h) / 100.0;
    case NS_LEN_CQW:
    case NS_LEN_CQI:   return ctx->container_w / 100.0;
    case NS_LEN_CQH:
    case NS_LEN_CQB:   return ctx->container_h / 100.0;
    case NS_LEN_CQMIN: return MIN(ctx->container_w, ctx->container_h) / 100.0;
    case NS_LEN_CQMAX: return MAX(ctx->container_w, ctx->container_h) / 100.0;
    default:           return 0;
    }
}

static double
math_length_px(const ns_math_value *v, const ns_css_syntax_ctx *ctx)
{
    double px = 0;
    for (int i = 0; i < NS_LEN_COUNT; i++)
        if (v->len[i] != 0) px += v->len[i] * len_unit_px((ns_len_unit)i, ctx);
    return px;
}

static void
syn_append_number(GString *out, double n)
{
    if (isnan(n)) { g_string_append(out, "NaN"); return; }
    if (isinf(n)) { g_string_append(out, n < 0 ? "-infinity" : "infinity"); return; }
    if (n == 0) n = 0;
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof buf, "%.6f", n);
    char *dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + strlen(buf);
        while (end > dot && end[-1] == '0') end--;
        if (end - 1 == dot) end--;
        *end = '\0';
    }
    g_string_append(out, buf[0] == '-' && buf[1] == '0' && !buf[2]
                         ? "0" : buf);
}

static void
syn_append_dimension(GString *out, double n, const char *unit)
{
    syn_append_number(out, n);
    g_string_append(out, unit);
}

static void
syn_append_string(GString *out, const char *text)
{
    g_string_append_c(out, '"');
    for (const char *p = text; p && *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_c(out, '\\');
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
}

static void
syn_append_color(GString *out, guint8 r, guint8 g, guint8 b, guint8 a)
{
    if (a == 255) {
        g_string_append_printf(out, "rgb(%u, %u, %u)", r, g, b);
        return;
    }
    g_string_append_printf(out, "rgba(%u, %u, %u, ", r, g, b);
    syn_append_number(out, ((int)(a * 100.0 / 255.0 + 0.5)) / 100.0);
    g_string_append_c(out, ')');
}

static void
syn_append_math(GString *out, ns_syn_kind kind, const ns_math_value *v,
                const ns_css_syntax_ctx *ctx)
{
    switch (kind) {
    case NS_SYN_INTEGER:
        syn_append_number(out, round(v->number));
        return;
    case NS_SYN_NUMBER:
        syn_append_number(out, v->number);
        return;
    case NS_SYN_ANGLE:
        syn_append_dimension(out, v->number, "deg");
        return;
    case NS_SYN_TIME:
        syn_append_dimension(out, v->number, "s");
        return;
    case NS_SYN_RESOLUTION:
        syn_append_dimension(out, v->number, "dppx");
        return;
    case NS_SYN_PERCENTAGE:
        syn_append_dimension(out, v->percent, "%");
        return;
    default:
        break;
    }
    gboolean has_len = math_has_length_terms(v);
    if (v->has_percent && has_len) {
        double px = math_length_px(v, ctx);
        g_string_append(out, "calc(");
        syn_append_dimension(out, v->percent, "%");
        g_string_append(out, px < 0 ? " - " : " + ");
        syn_append_dimension(out, px < 0 ? -px : px, "px");
        g_string_append_c(out, ')');
        return;
    }
    if (v->has_percent) {
        syn_append_dimension(out, v->percent, "%");
        return;
    }
    syn_append_dimension(out, math_length_px(v, ctx), "px");
}

typedef struct {
    GString *out;
    const ns_css_syntax_ctx *ctx;
} syn_emit;

static gboolean
syn_match_single(ns_syn_kind kind, const char *ident, syn_chunk chunk,
                 gboolean independent, const syn_emit *emit)
{
    chunk_trim(&chunk);
    if (chunk.lo >= chunk.hi) return FALSE;
    guint count = chunk.hi - chunk.lo;
    const ns_css_component *first = g_ptr_array_index(chunk.items, chunk.lo);

    switch (kind) {
    case NS_SYN_IDENT:
    case NS_SYN_CUSTOM_IDENT: {
        if (count != 1 || first->type != NS_CSS_COMPONENT_IDENT) return FALSE;
        char *got = syn_ident_unescape(g_math_input + first->start,
                                       (gssize)(first->end - first->start));
        gboolean ok = kind == NS_SYN_IDENT ? (ident && strcmp(got, ident) == 0)
                                           : !syn_reserved_ident(got);
        if (ok && emit) g_string_append(emit->out, got);
        g_free(got);
        return ok;
    }
    case NS_SYN_STRING:
        if (count != 1 || first->type != NS_CSS_COMPONENT_STRING) return FALSE;
        if (emit) syn_append_string(emit->out, first->value);
        return TRUE;
    case NS_SYN_URL:
    case NS_SYN_IMAGE: {
        if (count != 1 || first->type != NS_CSS_COMPONENT_FUNCTION ||
            !first->value)
            return FALSE;
        gboolean ok = kind == NS_SYN_URL
            ? (g_ascii_strcasecmp(first->value, "url") == 0 ||
               g_ascii_strcasecmp(first->value, "src") == 0)
            : syn_image_function(first->value);
        if (ok && emit) {
            char *text = component_text(first);
            g_string_append(emit->out, text);
            g_free(text);
        }
        return ok;
    }
    case NS_SYN_COLOR: {
        if (count != 1) return FALSE;
        if (first->type == NS_CSS_COMPONENT_FUNCTION &&
            (!first->value || !syn_color_function(first->value)))
            return FALSE;
        if (first->type != NS_CSS_COMPONENT_FUNCTION &&
            first->type != NS_CSS_COMPONENT_IDENT &&
            first->type != NS_CSS_COMPONENT_HASH)
            return FALSE;
        char *text = component_text(first);
        gboolean current = g_ascii_strcasecmp(text, "currentcolor") == 0;
        guint8 r = 0, g = 0, b = 0, a = 255;
        gboolean parsed = !current && ns_css_parse_color(text, &r, &g, &b, &a);
        gboolean ok = current || parsed ||
                      first->type == NS_CSS_COMPONENT_FUNCTION;
        if (ok && emit) {
            if (current && emit->ctx && emit->ctx->current_color)
                g_string_append(emit->out, emit->ctx->current_color);
            else if (parsed) syn_append_color(emit->out, r, g, b, a);
            else g_string_append(emit->out, text);
        }
        g_free(text);
        return ok;
    }
    case NS_SYN_TRANSFORM_FUNCTION: {
        if (count != 1 || first->type != NS_CSS_COMPONENT_FUNCTION)
            return FALSE;
        GArray *ranges = g_array_new(FALSE, FALSE, sizeof(math_range));
        math_split_args(first->children, ranges);
        guint argc = 0;
        gboolean ok = TRUE;
        GString *args = emit ? g_string_new(NULL) : NULL;
        for (guint i = 0; i < ranges->len; i++) {
            math_range r = g_array_index(ranges, math_range, i);
            syn_chunk arg = { first->children, r.lo, r.hi };
            chunk_trim(&arg);
            if (arg.lo >= arg.hi) {
                if (ranges->len == 1) break;
                ok = FALSE;
                break;
            }
            argc++;
            if (arg.hi - arg.lo != 1) { ok = FALSE; break; }
            const ns_css_component *a = g_ptr_array_index(first->children,
                                                          arg.lo);
            ns_math_value v;
            if (!math_eval_component(a, &v)) { ok = FALSE; break; }
            if (independent && math_font_relative(&v)) { ok = FALSE; break; }
            if (!args) continue;
            if (argc > 1) g_string_append(args, ", ");
            ns_syn_kind arg_kind =
                v.kind == NS_MATH_ANGLE      ? NS_SYN_ANGLE :
                v.kind == NS_MATH_NUMBER &&
                    !math_has_length_terms(&v) && !v.has_percent
                                             ? NS_SYN_NUMBER
                                             : NS_SYN_LENGTH_PERCENTAGE;
            syn_append_math(args, arg_kind, &v, emit->ctx);
        }
        g_array_free(ranges, TRUE);
        ok = ok && argc > 0 &&
             syn_transform_function(first->value ? first->value : "", argc);
        if (ok && emit)
            g_string_append_printf(emit->out, "%s(%s)", first->value,
                                   args->str);
        if (args) g_string_free(args, TRUE);
        return ok;
    }
    default:
        break;
    }
    ns_math_value v;
    if (!syn_match_numeric(kind, &chunk, independent, &v)) return FALSE;
    if (emit) syn_append_math(emit->out, kind, &v, emit->ctx);
    return TRUE;
}

static gboolean
syn_match_space_list(ns_syn_kind kind, const char *ident, syn_chunk chunk,
                     gboolean independent, const syn_emit *emit)
{
    chunk_trim(&chunk);
    if (chunk.lo >= chunk.hi) return FALSE;
    guint start = chunk.lo;
    guint count = 0;
    for (guint i = chunk.lo; i <= chunk.hi; i++) {
        gboolean at_end = i == chunk.hi;
        if (!at_end) {
            const ns_css_component *c = g_ptr_array_index(chunk.items, i);
            if (c->type == NS_CSS_COMPONENT_COMMA) return FALSE;
            if (!component_is_ws(c)) continue;
        }
        syn_chunk piece = { chunk.items, start, i };
        chunk_trim(&piece);
        if (piece.lo < piece.hi) {
            if (emit && count > 0) g_string_append_c(emit->out, ' ');
            if (!syn_match_single(kind, ident, piece, independent, emit))
                return FALSE;
            count++;
        }
        start = i + 1;
    }
    return count > 0;
}

static gboolean
syn_match_component(const ns_syn_component *comp, syn_chunk chunk,
                    gboolean independent, const syn_emit *emit)
{
    chunk_trim(&chunk);
    if (chunk.lo >= chunk.hi) return FALSE;

    if (comp->kind == NS_SYN_TRANSFORM_LIST)
        return syn_match_space_list(NS_SYN_TRANSFORM_FUNCTION, NULL, chunk,
                                    independent, emit);

    if (comp->mult == NS_SYN_MULT_SPACE)
        return syn_match_space_list(comp->kind, comp->ident, chunk,
                                    independent, emit);

    if (comp->mult == NS_SYN_MULT_COMMA) {
        guint start = chunk.lo;
        guint count = 0;
        for (guint i = chunk.lo; i <= chunk.hi; i++) {
            gboolean at_end = i == chunk.hi;
            if (!at_end) {
                const ns_css_component *c = g_ptr_array_index(chunk.items, i);
                if (c->type != NS_CSS_COMPONENT_COMMA) continue;
            }
            syn_chunk piece = { chunk.items, start, i };
            if (emit && count > 0) g_string_append(emit->out, ", ");
            if (!syn_match_single(comp->kind, comp->ident, piece, independent,
                                  emit))
                return FALSE;
            count++;
            start = i + 1;
        }
        return count > 0;
    }
    return syn_match_single(comp->kind, comp->ident, chunk, independent, emit);
}

static gboolean
syn_value_tokens_ok(GPtrArray *items, gboolean allow_var)
{
    for (guint i = 0; i < items->len; i++) {
        const ns_css_component *c = g_ptr_array_index(items, i);
        if (c->type == NS_CSS_COMPONENT_SEMICOLON) return FALSE;
        if (c->type == NS_CSS_COMPONENT_DELIM && c->delimiter == '!')
            return FALSE;
    }
    if (!allow_var) {
        GQueue queue = G_QUEUE_INIT;
        g_queue_push_tail(&queue, items);
        gboolean ok = TRUE;
        while (!g_queue_is_empty(&queue)) {
            GPtrArray *level = g_queue_pop_head(&queue);
            for (guint i = 0; i < level->len && ok; i++) {
                const ns_css_component *c = g_ptr_array_index(level, i);
                if (c->type == NS_CSS_COMPONENT_FUNCTION && c->value &&
                    (g_ascii_strcasecmp(c->value, "var") == 0 ||
                     g_ascii_strcasecmp(c->value, "env") == 0 ||
                     g_ascii_strcasecmp(c->value, "attr") == 0))
                    ok = FALSE;
                if (c->children) g_queue_push_tail(&queue, c->children);
            }
            if (!ok) break;
        }
        g_queue_clear(&queue);
        if (!ok) return FALSE;
    }
    return TRUE;
}

static gboolean
syn_bad_url(const char *text)
{
    const char *p = text;
    while ((p = strstr(p, "url(")) != NULL) {
        if (p != text) {
            char prev = p[-1];
            if (g_ascii_isalnum(prev) || prev == '-' || prev == '_') {
                p += 4;
                continue;
            }
        }
        const char *q = p + 4;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == '"' || *q == '\'') { p = q; continue; }
        while (*q && *q != ')') {
            if (*q == '"' || *q == '\'' || *q == '(') return TRUE;
            q++;
        }
        p = *q ? q + 1 : q;
    }
    return FALSE;
}

static gboolean
syn_match_value(const ns_css_syntax_def *syntax, const char *value,
                gboolean independent, const ns_css_syntax_ctx *ctx,
                GString *computed)
{
    if (!syntax || !value) return FALSE;
    const char *saved_input = g_math_input;
    g_math_input = value;

    gboolean tokens_valid = FALSE;
    GPtrArray *items = ns_css_component_values_parse(value, -1, &tokens_valid);
    gboolean ok = tokens_valid && syn_value_tokens_ok(items, FALSE) &&
                  !syn_bad_url(value);

    syn_chunk chunk = { items, 0, items->len };
    chunk_trim(&chunk);
    if (ok && chunk.lo >= chunk.hi) ok = FALSE;
    if (ok && chunk.hi - chunk.lo == 1) {
        const ns_css_component *c = g_ptr_array_index(items, chunk.lo);
        if (c->type == NS_CSS_COMPONENT_IDENT) {
            char *text = component_text(c);
            if (syn_css_wide_keyword(text)) ok = FALSE;
            g_free(text);
        }
    }
    if (ok && !syntax->universal) {
        syn_emit emit = { computed, ctx };
        gboolean matched = FALSE;
        for (guint i = 0; i < syntax->components->len && !matched; i++) {
            const ns_syn_component *comp =
                &g_array_index(syntax->components, ns_syn_component, i);
            if (computed) g_string_truncate(computed, 0);
            matched = syn_match_component(comp, chunk, independent,
                                          computed ? &emit : NULL);
        }
        ok = matched;
    }
    g_ptr_array_free(items, TRUE);
    g_math_input = saved_input;
    return ok;
}

gboolean
ns_css_syntax_def_matches(const ns_css_syntax_def *syntax, const char *value)
{
    return syn_match_value(syntax, value, FALSE, NULL, NULL);
}

gboolean
ns_css_syntax_def_initial_valid(const ns_css_syntax_def *syntax,
                                 const char *value)
{
    return syn_match_value(syntax, value, TRUE, NULL, NULL);
}

char *
ns_css_syntax_def_compute(const ns_css_syntax_def *syntax, const char *value,
                          const ns_css_syntax_ctx *ctx)
{
    if (!syntax || syntax->universal) return NULL;
    GString *out = g_string_new(NULL);
    if (!syn_match_value(syntax, value, FALSE, ctx, out)) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}
