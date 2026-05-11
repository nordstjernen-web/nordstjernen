/* Nordstjernen — CSS parser, selectors, cascade. */

#include "css.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static const char *kProp[ND_CSS_PROP_COUNT] = {
    [ND_CSS_DISPLAY]              = "display",
    [ND_CSS_COLOR]                = "color",
    [ND_CSS_BACKGROUND_COLOR]     = "background-color",
    [ND_CSS_FONT_SIZE]            = "font-size",
    [ND_CSS_FONT_WEIGHT]          = "font-weight",
    [ND_CSS_FONT_STYLE]           = "font-style",
    [ND_CSS_FONT_FAMILY]          = "font-family",
    [ND_CSS_TEXT_ALIGN]           = "text-align",
    [ND_CSS_MARGIN_TOP]           = "margin-top",
    [ND_CSS_MARGIN_RIGHT]         = "margin-right",
    [ND_CSS_MARGIN_BOTTOM]        = "margin-bottom",
    [ND_CSS_MARGIN_LEFT]          = "margin-left",
    [ND_CSS_PADDING_TOP]          = "padding-top",
    [ND_CSS_PADDING_RIGHT]        = "padding-right",
    [ND_CSS_PADDING_BOTTOM]       = "padding-bottom",
    [ND_CSS_PADDING_LEFT]         = "padding-left",
    [ND_CSS_BORDER_TOP_WIDTH]     = "border-top-width",
    [ND_CSS_BORDER_RIGHT_WIDTH]   = "border-right-width",
    [ND_CSS_BORDER_BOTTOM_WIDTH]  = "border-bottom-width",
    [ND_CSS_BORDER_LEFT_WIDTH]    = "border-left-width",
    [ND_CSS_BORDER_TOP_COLOR]     = "border-top-color",
    [ND_CSS_BORDER_RIGHT_COLOR]   = "border-right-color",
    [ND_CSS_BORDER_BOTTOM_COLOR]  = "border-bottom-color",
    [ND_CSS_BORDER_LEFT_COLOR]    = "border-left-color",
    [ND_CSS_BORDER_TOP_STYLE]     = "border-top-style",
    [ND_CSS_BORDER_RIGHT_STYLE]   = "border-right-style",
    [ND_CSS_BORDER_BOTTOM_STYLE]  = "border-bottom-style",
    [ND_CSS_BORDER_LEFT_STYLE]    = "border-left-style",
    [ND_CSS_WIDTH]                = "width",
    [ND_CSS_HEIGHT]               = "height",
    [ND_CSS_MAX_WIDTH]            = "max-width",
    [ND_CSS_MAX_HEIGHT]           = "max-height",
    [ND_CSS_LINE_HEIGHT]          = "line-height",
    [ND_CSS_TEXT_DECORATION]      = "text-decoration",
};

const char *
nd_css_prop_name(nd_css_prop p)
{
    if (p < 0 || p >= ND_CSS_PROP_COUNT) return "?";
    return kProp[p];
}

static gboolean
prop_inherits(nd_css_prop p)
{
    switch (p) {
    case ND_CSS_COLOR:
    case ND_CSS_FONT_SIZE:
    case ND_CSS_FONT_WEIGHT:
    case ND_CSS_FONT_STYLE:
    case ND_CSS_FONT_FAMILY:
    case ND_CSS_TEXT_ALIGN:
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean
is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static gboolean
is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || (unsigned char)c >= 128;
}

static gboolean
is_ident(char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static char *
ascii_lower(const char *s, gsize len)
{
    char *r = g_malloc(len + 1);
    for (gsize i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        r[i] = c;
    }
    r[len] = '\0';
    return r;
}

nd_css_value *
nd_css_value_dup(const nd_css_value *v)
{
    if (!v) return NULL;
    nd_css_value *o = g_new0(nd_css_value, 1);
    o->kind = v->kind;
    switch (v->kind) {
    case ND_CSS_V_KEYWORD: o->u.keyword = g_strdup(v->u.keyword); break;
    case ND_CSS_V_LENGTH:  o->u.length = v->u.length; break;
    case ND_CSS_V_COLOR:   o->u.color = v->u.color; break;
    }
    return o;
}

void
nd_css_value_free(nd_css_value *v)
{
    if (!v) return;
    if (v->kind == ND_CSS_V_KEYWORD) g_free(v->u.keyword);
    g_free(v);
}

static gboolean
named_color(const char *name, guint8 *r, guint8 *g, guint8 *b)
{
    static const struct { const char *n; guint8 r, g, b; } table[] = {
        { "aliceblue",       240, 248, 255 },
        { "antiquewhite",    250, 235, 215 },
        { "aqua",            0,   255, 255 },
        { "aquamarine",      127, 255, 212 },
        { "azure",           240, 255, 255 },
        { "beige",           245, 245, 220 },
        { "bisque",          255, 228, 196 },
        { "black",           0,   0,   0   },
        { "blanchedalmond",  255, 235, 205 },
        { "blue",            0,   0,   255 },
        { "blueviolet",      138, 43,  226 },
        { "brown",           165, 42,  42  },
        { "burlywood",       222, 184, 135 },
        { "cadetblue",       95,  158, 160 },
        { "chartreuse",      127, 255, 0   },
        { "chocolate",       210, 105, 30  },
        { "coral",           255, 127, 80  },
        { "cornflowerblue",  100, 149, 237 },
        { "cornsilk",        255, 248, 220 },
        { "crimson",         220, 20,  60  },
        { "cyan",            0,   255, 255 },
        { "darkblue",        0,   0,   139 },
        { "darkcyan",        0,   139, 139 },
        { "darkgoldenrod",   184, 134, 11  },
        { "darkgray",        169, 169, 169 },
        { "darkgrey",        169, 169, 169 },
        { "darkgreen",       0,   100, 0   },
        { "darkkhaki",       189, 183, 107 },
        { "darkmagenta",     139, 0,   139 },
        { "darkolivegreen",  85,  107, 47  },
        { "darkorange",      255, 140, 0   },
        { "darkorchid",      153, 50,  204 },
        { "darkred",         139, 0,   0   },
        { "darksalmon",      233, 150, 122 },
        { "darkseagreen",    143, 188, 143 },
        { "darkslateblue",   72,  61,  139 },
        { "darkslategray",   47,  79,  79  },
        { "darkturquoise",   0,   206, 209 },
        { "darkviolet",      148, 0,   211 },
        { "deeppink",        255, 20,  147 },
        { "deepskyblue",     0,   191, 255 },
        { "dimgray",         105, 105, 105 },
        { "dodgerblue",      30,  144, 255 },
        { "firebrick",       178, 34,  34  },
        { "floralwhite",     255, 250, 240 },
        { "forestgreen",     34,  139, 34  },
        { "fuchsia",         255, 0,   255 },
        { "gainsboro",       220, 220, 220 },
        { "ghostwhite",      248, 248, 255 },
        { "gold",            255, 215, 0   },
        { "goldenrod",       218, 165, 32  },
        { "gray",            128, 128, 128 },
        { "grey",            128, 128, 128 },
        { "green",           0,   128, 0   },
        { "greenyellow",     173, 255, 47  },
        { "honeydew",        240, 255, 240 },
        { "hotpink",         255, 105, 180 },
        { "indianred",       205, 92,  92  },
        { "indigo",          75,  0,   130 },
        { "ivory",           255, 255, 240 },
        { "khaki",           240, 230, 140 },
        { "lavender",        230, 230, 250 },
        { "lavenderblush",   255, 240, 245 },
        { "lawngreen",       124, 252, 0   },
        { "lemonchiffon",    255, 250, 205 },
        { "lightblue",       173, 216, 230 },
        { "lightcoral",      240, 128, 128 },
        { "lightcyan",       224, 255, 255 },
        { "lightgoldenrodyellow", 250, 250, 210 },
        { "lightgray",       211, 211, 211 },
        { "lightgrey",       211, 211, 211 },
        { "lightgreen",      144, 238, 144 },
        { "lightpink",       255, 182, 193 },
        { "lightsalmon",     255, 160, 122 },
        { "lightseagreen",   32,  178, 170 },
        { "lightskyblue",    135, 206, 250 },
        { "lightslategray",  119, 136, 153 },
        { "lightsteelblue",  176, 196, 222 },
        { "lightyellow",     255, 255, 224 },
        { "lime",            0,   255, 0   },
        { "limegreen",       50,  205, 50  },
        { "linen",           250, 240, 230 },
        { "magenta",         255, 0,   255 },
        { "maroon",          128, 0,   0   },
        { "mediumaquamarine",102, 205, 170 },
        { "mediumblue",      0,   0,   205 },
        { "mediumorchid",    186, 85,  211 },
        { "mediumpurple",    147, 112, 219 },
        { "mediumseagreen",  60,  179, 113 },
        { "mediumslateblue", 123, 104, 238 },
        { "mediumspringgreen",0,  250, 154 },
        { "mediumturquoise", 72,  209, 204 },
        { "mediumvioletred", 199, 21,  133 },
        { "midnightblue",    25,  25,  112 },
        { "mintcream",       245, 255, 250 },
        { "mistyrose",       255, 228, 225 },
        { "moccasin",        255, 228, 181 },
        { "navajowhite",     255, 222, 173 },
        { "navy",            0,   0,   128 },
        { "oldlace",         253, 245, 230 },
        { "olive",           128, 128, 0   },
        { "olivedrab",       107, 142, 35  },
        { "orange",          255, 165, 0   },
        { "orangered",       255, 69,  0   },
        { "orchid",          218, 112, 214 },
        { "palegoldenrod",   238, 232, 170 },
        { "palegreen",       152, 251, 152 },
        { "paleturquoise",   175, 238, 238 },
        { "palevioletred",   219, 112, 147 },
        { "papayawhip",      255, 239, 213 },
        { "peachpuff",       255, 218, 185 },
        { "peru",            205, 133, 63  },
        { "pink",            255, 192, 203 },
        { "plum",            221, 160, 221 },
        { "powderblue",      176, 224, 230 },
        { "purple",          128, 0,   128 },
        { "rebeccapurple",   102, 51,  153 },
        { "red",             255, 0,   0   },
        { "rosybrown",       188, 143, 143 },
        { "royalblue",       65,  105, 225 },
        { "saddlebrown",     139, 69,  19  },
        { "salmon",          250, 128, 114 },
        { "sandybrown",      244, 164, 96  },
        { "seagreen",        46,  139, 87  },
        { "seashell",        255, 245, 238 },
        { "sienna",          160, 82,  45  },
        { "silver",          192, 192, 192 },
        { "skyblue",         135, 206, 235 },
        { "slateblue",       106, 90,  205 },
        { "slategray",       112, 128, 144 },
        { "snow",            255, 250, 250 },
        { "springgreen",     0,   255, 127 },
        { "steelblue",       70,  130, 180 },
        { "tan",             210, 180, 140 },
        { "teal",            0,   128, 128 },
        { "thistle",         216, 191, 216 },
        { "tomato",          255, 99,  71  },
        { "turquoise",       64,  224, 208 },
        { "violet",          238, 130, 238 },
        { "wheat",           245, 222, 179 },
        { "white",           255, 255, 255 },
        { "whitesmoke",      245, 245, 245 },
        { "yellow",          255, 255, 0   },
        { "yellowgreen",     154, 205, 50  },
        { "transparent",     0,   0,   0   },
        { NULL, 0, 0, 0 },
    };
    for (int i = 0; table[i].n; i++) {
        if (g_ascii_strcasecmp(table[i].n, name) == 0) {
            *r = table[i].r; *g = table[i].g; *b = table[i].b;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
parse_rgb_func(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
    gboolean is_rgba = g_ascii_strncasecmp(s, "rgba(", 5) == 0;
    gboolean is_rgb  = !is_rgba && g_ascii_strncasecmp(s, "rgb(", 4) == 0;
    if (!is_rgb && !is_rgba) return FALSE;
    const char *p = strchr(s, '(');
    if (!p) return FALSE;
    p++;
    double values[4] = { 0, 0, 0, 1 };
    int count = 0;
    while (*p && *p != ')' && count < 4) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p || *p == ')') break;
        char *end = NULL;
        double v = g_ascii_strtod(p, &end);
        if (!end || end == p) return FALSE;
        if (*end == '%') { v = v * 255.0 / 100.0; end++; }
        values[count++] = v;
        p = end;
    }
    if (count < 3) return FALSE;
    *r = (guint8)CLAMP((int)(values[0] + 0.5), 0, 255);
    *g = (guint8)CLAMP((int)(values[1] + 0.5), 0, 255);
    *b = (guint8)CLAMP((int)(values[2] + 0.5), 0, 255);
    *a = is_rgba ? (guint8)CLAMP((int)(values[3] * 255 + 0.5), 0, 255) : 255;
    return TRUE;
}

static gboolean
parse_color(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
    *a = 255;
    if (!s || !*s) return FALSE;
    if (g_ascii_strcasecmp(s, "transparent") == 0) {
        *r = 0; *g = 0; *b = 0; *a = 0;
        return TRUE;
    }
    if (parse_rgb_func(s, r, g, b, a)) return TRUE;
    if (s[0] == '#') {
        gsize n = strlen(s + 1);
        if (n == 3) {
            int rr = g_ascii_xdigit_value(s[1]);
            int gg = g_ascii_xdigit_value(s[2]);
            int bb = g_ascii_xdigit_value(s[3]);
            if (rr < 0 || gg < 0 || bb < 0) return FALSE;
            *r = (guint8)(rr * 17); *g = (guint8)(gg * 17); *b = (guint8)(bb * 17);
            return TRUE;
        }
        if (n == 6) {
            int v[6];
            for (int i = 0; i < 6; i++) {
                v[i] = g_ascii_xdigit_value(s[1 + i]);
                if (v[i] < 0) return FALSE;
            }
            *r = (guint8)(v[0] * 16 + v[1]);
            *g = (guint8)(v[2] * 16 + v[3]);
            *b = (guint8)(v[4] * 16 + v[5]);
            return TRUE;
        }
        return FALSE;
    }
    return named_color(s, r, g, b);
}

static void
nd_attr_pred_clear(gpointer p)
{
    nd_css_attr_pred *a = p;
    g_free(a->name);
    g_free(a->value);
}

static nd_css_simple *
nd_css_simple_new(void)
{
    nd_css_simple *s = g_new0(nd_css_simple, 1);
    s->classes = g_ptr_array_new_with_free_func(g_free);
    s->attrs   = g_array_new(FALSE, FALSE, sizeof(nd_css_attr_pred));
    g_array_set_clear_func(s->attrs, nd_attr_pred_clear);
    return s;
}

static void
nd_css_simple_free(nd_css_simple *s)
{
    if (!s) return;
    g_free(s->type);
    g_free(s->id);
    g_ptr_array_free(s->classes, TRUE);
    if (s->attrs) g_array_free(s->attrs, TRUE);
    g_free(s);
}

void
nd_css_selector_free(nd_css_selector *sel)
{
    if (!sel) return;
    for (guint i = 0; i < sel->compounds->len; i++)
        nd_css_simple_free(g_ptr_array_index(sel->compounds, i));
    g_ptr_array_free(sel->compounds, TRUE);
    g_array_free(sel->combinators, TRUE);
    g_free(sel);
}

static nd_css_selector *
parse_one_selector(const char **pp, const char *end)
{
    nd_css_selector *sel = g_new0(nd_css_selector, 1);
    sel->compounds   = g_ptr_array_new();
    sel->combinators = g_array_new(FALSE, FALSE, sizeof(nd_css_comb));

    nd_css_comb pending = ND_CSS_COMB_NONE;
    gboolean expect_compound = TRUE;
    const char *p = *pp;

    while (p < end) {

        gboolean had_ws = FALSE;
        while (p < end && is_ws(*p)) { p++; had_ws = TRUE; }
        if (p >= end) break;
        char c = *p;

        if (c == ',' || c == '{') break;

        if (c == '>') {
            pending = ND_CSS_COMB_CHILD;
            expect_compound = TRUE;
            p++;
            continue;
        }

        if (had_ws && !expect_compound)
            pending = ND_CSS_COMB_DESCENDANT;

        nd_css_simple *cmp = nd_css_simple_new();
        gboolean any = FALSE;
        while (p < end) {
            char cc = *p;
            if (cc == '*') {
                g_free(cmp->type);
                cmp->type = g_strdup("*");
                p++;
                sel->spec_c += 0;
                any = TRUE;
            } else if (cc == '#') {
                p++;
                const char *s = p;
                while (p < end && is_ident(*p)) p++;
                g_free(cmp->id);
                cmp->id = ascii_lower(s, (gsize)(p - s));
                sel->spec_a += 1;
                any = TRUE;
            } else if (cc == '.') {
                p++;
                const char *s = p;
                while (p < end && is_ident(*p)) p++;
                g_ptr_array_add(cmp->classes, ascii_lower(s, (gsize)(p - s)));
                sel->spec_b += 1;
                any = TRUE;
            } else if (is_ident_start(cc)) {
                const char *s = p;
                while (p < end && is_ident(*p)) p++;
                if (!cmp->type) {
                    cmp->type = ascii_lower(s, (gsize)(p - s));
                    sel->spec_c += 1;
                }
                any = TRUE;
            } else if (cc == '[') {
                p++;
                while (p < end && is_ws(*p)) p++;
                const char *ns = p;
                while (p < end && (is_ident(*p) || *p == '-')) p++;
                gsize nlen = (gsize)(p - ns);
                if (nlen == 0) {
                    while (p < end && *p != ']') p++;
                    if (p < end) p++;
                    continue;
                }
                nd_css_attr_pred ap = {0};
                ap.name = ascii_lower(ns, nlen);
                ap.op   = ND_CSS_ATTR_PRESENT;
                while (p < end && is_ws(*p)) p++;
                if (p < end && (*p == '=' || *p == '^' || *p == '$' ||
                                *p == '*' || *p == '~')) {
                    char op_c = *p;
                    if (op_c == '=')      ap.op = ND_CSS_ATTR_EQ;
                    else if (op_c == '^') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_PREFIX; }
                    else if (op_c == '$') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_SUFFIX; }
                    else if (op_c == '*') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_SUBSTR; }
                    else if (op_c == '~') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_WORD;   }
                    if (p < end && *p == '=') p++;
                    while (p < end && is_ws(*p)) p++;
                    char q = (p < end) ? *p : 0;
                    const char *vstart;
                    gsize vlen;
                    if (q == '"' || q == '\'') {
                        p++;
                        vstart = p;
                        while (p < end && *p != q) p++;
                        vlen = (gsize)(p - vstart);
                        if (p < end) p++;
                    } else {
                        vstart = p;
                        while (p < end && *p != ']' && !is_ws(*p)) p++;
                        vlen = (gsize)(p - vstart);
                    }
                    ap.value = g_strndup(vstart, vlen);
                }
                while (p < end && *p != ']') p++;
                if (p < end) p++;
                g_array_append_val(cmp->attrs, ap);
                sel->spec_b += 1;
                any = TRUE;
            } else {
                break;
            }
        }
        if (!any) { nd_css_simple_free(cmp); break; }
        g_ptr_array_add(sel->compounds, cmp);
        g_array_append_val(sel->combinators, pending);
        pending = ND_CSS_COMB_NONE;
        expect_compound = FALSE;
    }
    *pp = p;
    if (sel->compounds->len == 0) {
        nd_css_selector_free(sel);
        return NULL;
    }
    return sel;
}

static gboolean
parse_length(const char *text, double *out_v, nd_css_unit *out_unit)
{
    if (!text || !*text) return FALSE;
    const char *p = text;
    if (*p == '-' || *p == '+') p++;
    const char *num_start = p;
    while (*p && (g_ascii_isdigit(*p) || *p == '.')) p++;
    if (p == num_start) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(text, &end);
    if (!end || end == text) return FALSE;
    *out_v = v;
    if (*end == '\0') { *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "px") == 0) { *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "em")  == 0 ||
        g_ascii_strcasecmp(end, "rem") == 0) { *out_unit = ND_CSS_UNIT_EM; return TRUE; }
    if (g_ascii_strcasecmp(end, "%")   == 0) { *out_unit = ND_CSS_UNIT_PERCENT; return TRUE; }
    if (g_ascii_strcasecmp(end, "pt")  == 0) {
        *out_v = v * 1.333;
        *out_unit = ND_CSS_UNIT_PX;
        return TRUE;
    }
    return FALSE;
}

static nd_css_value *
parse_value_for(nd_css_prop prop, const char *text)
{

    while (*text && is_ws(*text)) text++;
    gsize n = strlen(text);
    while (n > 0 && is_ws(text[n - 1])) n--;
    char *t = g_strndup(text, n);

    nd_css_value *v = NULL;

    switch (prop) {
    case ND_CSS_COLOR:
    case ND_CSS_BACKGROUND_COLOR:
    case ND_CSS_BORDER_TOP_COLOR:
    case ND_CSS_BORDER_RIGHT_COLOR:
    case ND_CSS_BORDER_BOTTOM_COLOR:
    case ND_CSS_BORDER_LEFT_COLOR: {
        guint8 r, g, b, a;
        if (parse_color(t, &r, &g, &b, &a)) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_COLOR;
            v->u.color.r = r; v->u.color.g = g; v->u.color.b = b; v->u.color.a = a;
        }
        break;
    }
    case ND_CSS_FONT_SIZE:
    case ND_CSS_MARGIN_TOP: case ND_CSS_MARGIN_RIGHT:
    case ND_CSS_MARGIN_BOTTOM: case ND_CSS_MARGIN_LEFT:
    case ND_CSS_PADDING_TOP: case ND_CSS_PADDING_RIGHT:
    case ND_CSS_PADDING_BOTTOM: case ND_CSS_PADDING_LEFT:
    case ND_CSS_BORDER_TOP_WIDTH: case ND_CSS_BORDER_RIGHT_WIDTH:
    case ND_CSS_BORDER_BOTTOM_WIDTH: case ND_CSS_BORDER_LEFT_WIDTH:
    case ND_CSS_WIDTH: case ND_CSS_HEIGHT:
    case ND_CSS_MAX_WIDTH: case ND_CSS_MAX_HEIGHT:
    case ND_CSS_LINE_HEIGHT: {
        if (g_ascii_strcasecmp(t, "auto") == 0) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = g_strdup("auto");
        } else {
            double num;
            nd_css_unit u;
            if (parse_length(t, &num, &u)) {
                v = g_new0(nd_css_value, 1);
                v->kind = ND_CSS_V_LENGTH;
                v->u.length.v = num;
                v->u.length.unit = u;
            }
        }
        break;
    }
    default: {

        char *kw = ascii_lower(t, strlen(t));
        v = g_new0(nd_css_value, 1);
        v->kind = ND_CSS_V_KEYWORD;
        v->u.keyword = kw;
        break;
    }
    }
    g_free(t);
    return v;
}

static int
prop_id(const char *name)
{
    for (int i = 0; i < ND_CSS_PROP_COUNT; i++) {
        if (g_ascii_strcasecmp(name, kProp[i]) == 0) return i;
    }
    return -1;
}

static void
emit_quad(GArray *decls, nd_css_prop t, nd_css_prop r,
          nd_css_prop b, nd_css_prop l,
          char *vals[4], int n, gboolean important)
{
    const char *top    = vals[0];
    const char *right  = n >= 2 ? vals[1] : top;
    const char *bottom = n >= 3 ? vals[2] : top;
    const char *left   = n >= 4 ? vals[3] : right;
    const struct { nd_css_prop p; const char *v; } map[] = {
        { t, top }, { r, right }, { b, bottom }, { l, left },
    };
    for (int i = 0; i < 4; i++) {
        nd_css_value *vv = parse_value_for(map[i].p, map[i].v);
        if (!vv) continue;
        nd_css_decl d = { .prop = map[i].p, .value = vv, .important = important };
        g_array_append_val(decls, d);
    }
}

static int
split_ws(const char *s, char *out[4])
{
    int n = 0;
    while (*s && n < 4) {
        while (*s && is_ws(*s)) s++;
        if (!*s) break;
        const char *start = s;

        while (*s && !is_ws(*s)) s++;
        out[n++] = g_strndup(start, (gsize)(s - start));
    }
    return n;
}

static void
parse_declaration_block(const char **pp, const char *end, GArray *decls_out)
{

    const char *p = *pp;
    while (p < end && *p != '}') {
        while (p < end && (is_ws(*p) || *p == ';')) p++;
        if (p >= end || *p == '}') break;

        const char *nstart = p;
        while (p < end && (is_ident(*p) || *p == '-')) p++;
        if (p == nstart) { p++; continue; }
        char *pname = ascii_lower(nstart, (gsize)(p - nstart));
        while (p < end && is_ws(*p)) p++;
        if (p >= end || *p != ':') { g_free(pname);
            while (p < end && *p != ';' && *p != '}') p++;
            continue;
        }
        p++;

        const char *vstart = p;
        while (p < end && *p != ';' && *p != '}') p++;
        char *vtext = g_strndup(vstart, (gsize)(p - vstart));
        gboolean important = FALSE;
        char *bang = g_strrstr(vtext, "!");
        if (bang) {
            char *tail = bang + 1;
            while (*tail && is_ws(*tail)) tail++;
            if (g_ascii_strncasecmp(tail, "important", 9) == 0) {
                important = TRUE;
                *bang = '\0';
            }
        }

        static const struct { const char *name; nd_css_prop t,r,b,l; } border_sides[] = {
            { "border-top",    ND_CSS_BORDER_TOP_WIDTH,    ND_CSS_BORDER_TOP_COLOR,
                               ND_CSS_BORDER_TOP_STYLE,    ND_CSS_PROP_COUNT },
            { "border-right",  ND_CSS_BORDER_RIGHT_WIDTH,  ND_CSS_BORDER_RIGHT_COLOR,
                               ND_CSS_BORDER_RIGHT_STYLE,  ND_CSS_PROP_COUNT },
            { "border-bottom", ND_CSS_BORDER_BOTTOM_WIDTH, ND_CSS_BORDER_BOTTOM_COLOR,
                               ND_CSS_BORDER_BOTTOM_STYLE, ND_CSS_PROP_COUNT },
            { "border-left",   ND_CSS_BORDER_LEFT_WIDTH,   ND_CSS_BORDER_LEFT_COLOR,
                               ND_CSS_BORDER_LEFT_STYLE,   ND_CSS_PROP_COUNT },
            { NULL, 0, 0, 0, 0 },
        };

        gboolean is_border_side = FALSE;
        int side_idx = -1;
        for (int i = 0; border_sides[i].name; i++) {
            if (strcmp(pname, border_sides[i].name) == 0) {
                is_border_side = TRUE; side_idx = i; break;
            }
        }
        if (strcmp(pname, "border") == 0 || is_border_side) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            for (int i = 0; i < n; i++) {
                guint8 r, g, b, a;
                double num; nd_css_unit u;
                if (parse_color(tokens[i], &r, &g, &b, &a)) {
                    if (is_border_side) {
                        nd_css_value *v = parse_value_for(border_sides[side_idx].r, tokens[i]);
                        if (v) {
                            nd_css_decl d = { .prop = border_sides[side_idx].r, .value = v, .important = important };
                            g_array_append_val(decls_out, d);
                        }
                    } else {
                        char *quad[4] = { tokens[i], tokens[i], tokens[i], tokens[i] };
                        emit_quad(decls_out,
                            ND_CSS_BORDER_TOP_COLOR, ND_CSS_BORDER_RIGHT_COLOR,
                            ND_CSS_BORDER_BOTTOM_COLOR, ND_CSS_BORDER_LEFT_COLOR,
                            quad, 4, important);
                    }
                } else if (parse_length(tokens[i], &num, &u)) {
                    if (is_border_side) {
                        nd_css_value *v = parse_value_for(border_sides[side_idx].t, tokens[i]);
                        if (v) {
                            nd_css_decl d = { .prop = border_sides[side_idx].t, .value = v, .important = important };
                            g_array_append_val(decls_out, d);
                        }
                    } else {
                        char *quad[4] = { tokens[i], tokens[i], tokens[i], tokens[i] };
                        emit_quad(decls_out,
                            ND_CSS_BORDER_TOP_WIDTH, ND_CSS_BORDER_RIGHT_WIDTH,
                            ND_CSS_BORDER_BOTTOM_WIDTH, ND_CSS_BORDER_LEFT_WIDTH,
                            quad, 4, important);
                    }
                } else {
                    if (is_border_side) {
                        nd_css_value *v = parse_value_for(border_sides[side_idx].b, tokens[i]);
                        if (v) {
                            nd_css_decl d = { .prop = border_sides[side_idx].b, .value = v, .important = important };
                            g_array_append_val(decls_out, d);
                        }
                    } else {
                        char *quad[4] = { tokens[i], tokens[i], tokens[i], tokens[i] };
                        emit_quad(decls_out,
                            ND_CSS_BORDER_TOP_STYLE, ND_CSS_BORDER_RIGHT_STYLE,
                            ND_CSS_BORDER_BOTTOM_STYLE, ND_CSS_BORDER_LEFT_STYLE,
                            quad, 4, important);
                    }
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "margin") == 0 ||
            strcmp(pname, "padding") == 0 ||
            strcmp(pname, "border-width") == 0 ||
            strcmp(pname, "border-color") == 0 ||
            strcmp(pname, "border-style") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            if (n > 0) {
                if (strcmp(pname, "margin") == 0)
                    emit_quad(decls_out,
                        ND_CSS_MARGIN_TOP, ND_CSS_MARGIN_RIGHT,
                        ND_CSS_MARGIN_BOTTOM, ND_CSS_MARGIN_LEFT,
                        tokens, n, important);
                else if (strcmp(pname, "padding") == 0)
                    emit_quad(decls_out,
                        ND_CSS_PADDING_TOP, ND_CSS_PADDING_RIGHT,
                        ND_CSS_PADDING_BOTTOM, ND_CSS_PADDING_LEFT,
                        tokens, n, important);
                else if (strcmp(pname, "border-width") == 0)
                    emit_quad(decls_out,
                        ND_CSS_BORDER_TOP_WIDTH, ND_CSS_BORDER_RIGHT_WIDTH,
                        ND_CSS_BORDER_BOTTOM_WIDTH, ND_CSS_BORDER_LEFT_WIDTH,
                        tokens, n, important);
                else if (strcmp(pname, "border-color") == 0)
                    emit_quad(decls_out,
                        ND_CSS_BORDER_TOP_COLOR, ND_CSS_BORDER_RIGHT_COLOR,
                        ND_CSS_BORDER_BOTTOM_COLOR, ND_CSS_BORDER_LEFT_COLOR,
                        tokens, n, important);
                else
                    emit_quad(decls_out,
                        ND_CSS_BORDER_TOP_STYLE, ND_CSS_BORDER_RIGHT_STYLE,
                        ND_CSS_BORDER_BOTTOM_STYLE, ND_CSS_BORDER_LEFT_STYLE,
                        tokens, n, important);
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
        } else {
            int pid = prop_id(pname);
            if (pid >= 0) {
                nd_css_value *vv = parse_value_for((nd_css_prop)pid, vtext);
                if (vv) {
                    nd_css_decl d = { .prop = (nd_css_prop)pid, .value = vv, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
        }
        g_free(pname);
        g_free(vtext);
        if (p < end && *p == ';') p++;
    }
    if (p < end && *p == '}') p++;
    *pp = p;
}

static void
nd_css_rule_free(nd_css_rule *r)
{
    if (!r) return;
    for (guint i = 0; i < r->selectors->len; i++)
        nd_css_selector_free(g_ptr_array_index(r->selectors, i));
    g_ptr_array_free(r->selectors, TRUE);
    for (guint i = 0; i < r->decls->len; i++) {
        nd_css_decl *d = &g_array_index(r->decls, nd_css_decl, i);
        nd_css_value_free(d->value);
    }
    g_array_free(r->decls, TRUE);
    g_free(r);
}

static void
skip_block(const char **pp, const char *end)
{
    int depth = 0;
    const char *p = *pp;
    while (p < end) {
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
        p++;
    }
    *pp = p;
}

static void
skip_at_rule(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && *p != ';' && *p != '{') p++;
    if (p >= end) { *pp = p; return; }
    if (*p == ';') { *pp = p + 1; return; }
    *pp = p;

    skip_block(pp, end);
}

nd_css_stylesheet *
nd_css_stylesheet_parse(const char *text, gssize len_in)
{
    nd_css_stylesheet *sh = g_new0(nd_css_stylesheet, 1);
    sh->rules = g_ptr_array_new_with_free_func((GDestroyNotify)nd_css_rule_free);
    if (!text) return sh;
    if (len_in < 0) len_in = (gssize)strlen(text);

    const char *p   = text;
    const char *end = text + len_in;
    int source_order = 0;

    while (p < end) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;

        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < end) p += 2;
            continue;
        }
        if (*p == '@') { skip_at_rule(&p, end); continue; }
        if (*p == '}') { p++; continue; }

        nd_css_rule *rule = g_new0(nd_css_rule, 1);
        rule->selectors = g_ptr_array_new();
        rule->decls     = g_array_new(FALSE, FALSE, sizeof(nd_css_decl));
        rule->source_order = source_order++;

        gboolean ok = FALSE;
        while (p < end && *p != '{') {
            nd_css_selector *sel = parse_one_selector(&p, end);
            if (sel) {
                g_ptr_array_add(rule->selectors, sel);
                ok = TRUE;
            }
            while (p < end && is_ws(*p)) p++;
            if (p < end && *p == ',') { p++; continue; }
            else break;
        }
        if (!ok || p >= end || *p != '{') {
            nd_css_rule_free(rule);

            while (p < end && *p != '}' && *p != ';') p++;
            if (p < end) p++;
            continue;
        }
        p++;
        parse_declaration_block(&p, end, rule->decls);
        g_ptr_array_add(sh->rules, rule);
    }
    return sh;
}

void
nd_css_stylesheet_free(nd_css_stylesheet *s)
{
    if (!s) return;
    g_ptr_array_free(s->rules, TRUE);
    g_free(s);
}

static gboolean
match_simple(const nd_css_simple *sel, const nd_node *el)
{
    if (!el || el->kind != ND_NODE_ELEMENT) return FALSE;
    if (sel->type && strcmp(sel->type, "*") != 0) {
        if (!el->name || strcmp(sel->type, el->name) != 0) return FALSE;
    }
    if (sel->id) {
        const char *id = nd_element_get_attr(el, "id");
        if (!id || strcmp(id, sel->id) != 0) return FALSE;
    }
    if (sel->classes->len > 0) {
        const char *cls = nd_element_get_attr(el, "class");
        if (!cls) return FALSE;

        for (guint i = 0; i < sel->classes->len; i++) {
            const char *want = g_ptr_array_index(sel->classes, i);
            gboolean found = FALSE;
            const char *s = cls;
            while (*s) {
                while (*s && is_ws(*s)) s++;
                const char *tok = s;
                while (*s && !is_ws(*s)) s++;
                if (s - tok == (gssize)strlen(want) &&
                    strncmp(tok, want, (gsize)(s - tok)) == 0) {
                    found = TRUE; break;
                }
            }
            if (!found) return FALSE;
        }
    }
    if (sel->attrs && sel->attrs->len > 0) {
        for (guint i = 0; i < sel->attrs->len; i++) {
            const nd_css_attr_pred *a = &g_array_index(sel->attrs, nd_css_attr_pred, i);
            const char *v = nd_element_get_attr(el, a->name);
            if (a->op == ND_CSS_ATTR_PRESENT) {
                if (!v) return FALSE;
            } else {
                if (!v || !a->value) return FALSE;
                gsize vl = strlen(v), wl = strlen(a->value);
                switch (a->op) {
                case ND_CSS_ATTR_EQ:
                    if (strcmp(v, a->value) != 0) return FALSE;
                    break;
                case ND_CSS_ATTR_PREFIX:
                    if (vl < wl || strncmp(v, a->value, wl) != 0) return FALSE;
                    break;
                case ND_CSS_ATTR_SUFFIX:
                    if (vl < wl || strcmp(v + vl - wl, a->value) != 0) return FALSE;
                    break;
                case ND_CSS_ATTR_SUBSTR:
                    if (!strstr(v, a->value)) return FALSE;
                    break;
                case ND_CSS_ATTR_WORD: {
                    gboolean found = FALSE;
                    const char *s = v;
                    while (*s) {
                        while (*s && is_ws(*s)) s++;
                        const char *tok = s;
                        while (*s && !is_ws(*s)) s++;
                        if ((gsize)(s - tok) == wl &&
                            strncmp(tok, a->value, wl) == 0) {
                            found = TRUE; break;
                        }
                    }
                    if (!found) return FALSE;
                    break;
                }
                case ND_CSS_ATTR_PRESENT: break;
                }
            }
        }
    }
    return TRUE;
}

static gboolean match_selector(const nd_css_selector *sel, const nd_node *el);

char *
nd_inline_style_get(const char *style, const char *prop)
{
    if (!style || !prop) return NULL;
    gsize plen = strlen(prop);
    const char *p = style;
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        const char *kstart = p;
        while (*p && *p != ':' && *p != ';') p++;
        gsize klen = (gsize)(p - kstart);
        while (klen > 0 && (kstart[klen-1] == ' ' || kstart[klen-1] == '\t'))
            klen--;
        if (*p != ':') {
            while (*p && *p != ';') p++;
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t') p++;
        const char *vstart = p;
        while (*p && *p != ';') p++;
        gsize vlen = (gsize)(p - vstart);
        while (vlen > 0 && (vstart[vlen-1] == ' ' || vstart[vlen-1] == '\t'))
            vlen--;
        if (klen == plen && g_ascii_strncasecmp(kstart, prop, klen) == 0)
            return g_strndup(vstart, vlen);
    }
    return NULL;
}

char *
nd_inline_style_set(const char *style, const char *prop, const char *value)
{
    GString *out = g_string_new(NULL);
    gboolean found = FALSE;
    gsize plen = prop ? strlen(prop) : 0;
    const char *p = style ? style : "";
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        const char *kstart = p;
        while (*p && *p != ':' && *p != ';') p++;
        gsize klen = (gsize)(p - kstart);
        while (klen > 0 && (kstart[klen-1] == ' ' || kstart[klen-1] == '\t'))
            klen--;
        if (*p != ':') {
            while (*p && *p != ';') p++;
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t') p++;
        const char *vstart = p;
        while (*p && *p != ';') p++;
        gsize vlen = (gsize)(p - vstart);
        while (vlen > 0 && (vstart[vlen-1] == ' ' || vstart[vlen-1] == '\t'))
            vlen--;
        gboolean match = klen == plen && prop &&
                         g_ascii_strncasecmp(kstart, prop, klen) == 0;
        if (match) {
            if (!value || !*value) { found = TRUE; continue; }
            if (out->len > 0) g_string_append(out, "; ");
            g_string_append_len(out, kstart, klen);
            g_string_append(out, ": ");
            g_string_append(out, value);
            found = TRUE;
        } else {
            if (out->len > 0) g_string_append(out, "; ");
            g_string_append_len(out, kstart, klen);
            g_string_append(out, ": ");
            g_string_append_len(out, vstart, vlen);
        }
    }
    if (!found && value && *value) {
        if (out->len > 0) g_string_append(out, "; ");
        g_string_append(out, prop);
        g_string_append(out, ": ");
        g_string_append(out, value);
    }
    return g_string_free(out, FALSE);
}

GPtrArray *
nd_css_parse_selector_list(const char *text)
{
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)nd_css_selector_free);
    if (!text) return out;
    const char *p = text;
    const char *end = text + strlen(text);
    while (p < end) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;
        nd_css_selector *sel = parse_one_selector(&p, end);
        if (sel) g_ptr_array_add(out, sel);
        while (p < end && is_ws(*p)) p++;
        if (p < end && *p == ',') p++;
    }
    return out;
}

gboolean nd_css_selector_matches(const nd_css_selector *sel, const nd_node *el);

gboolean
nd_css_selector_matches(const nd_css_selector *sel, const nd_node *el)
{
    return match_selector(sel, el);
}

static gboolean
match_selector(const nd_css_selector *sel, const nd_node *el)
{
    if (!sel || sel->compounds->len == 0) return FALSE;
    int idx = (int)sel->compounds->len - 1;
    const nd_node *cur = el;
    if (!match_simple(g_ptr_array_index(sel->compounds, idx), cur)) return FALSE;
    while (idx > 0) {
        nd_css_comb comb = g_array_index(sel->combinators, nd_css_comb, idx);
        const nd_css_simple *prev = g_ptr_array_index(sel->compounds, idx - 1);
        if (comb == ND_CSS_COMB_CHILD) {
            cur = cur->parent;
            if (!cur || !match_simple(prev, cur)) return FALSE;
        } else {
            const nd_node *p = cur->parent;
            gboolean ok = FALSE;
            while (p) {
                if (match_simple(prev, p)) { cur = p; ok = TRUE; break; }
                p = p->parent;
            }
            if (!ok) return FALSE;
        }
        idx--;
    }
    return TRUE;
}

void
nd_style_free(nd_style *s)
{
    if (!s) return;
    for (int i = 0; i < ND_CSS_PROP_COUNT; i++)
        nd_css_value_free(s->values[i]);
    g_free(s);
}

const char *
nd_style_keyword(const nd_style *s, nd_css_prop p)
{
    if (!s) return NULL;
    nd_css_value *v = s->values[p];
    if (!v || v->kind != ND_CSS_V_KEYWORD) return NULL;
    return v->u.keyword;
}

typedef struct match_entry {
    int          origin;
    int          spec_a, spec_b, spec_c;
    int          source_order;
    int          decl_order;
    gboolean     important;
    nd_css_value *value;
    nd_css_prop  prop;
} match_entry;

static int
match_cmp(gconstpointer a_, gconstpointer b_)
{
    const match_entry *a = a_;
    const match_entry *b = b_;
    if (a->important != b->important) return a->important ? 1 : -1;
    if (a->origin    != b->origin)    return a->origin < b->origin ? -1 : 1;
    if (a->spec_a    != b->spec_a)    return a->spec_a < b->spec_a ? -1 : 1;
    if (a->spec_b    != b->spec_b)    return a->spec_b < b->spec_b ? -1 : 1;
    if (a->spec_c    != b->spec_c)    return a->spec_c < b->spec_c ? -1 : 1;
    if (a->source_order != b->source_order)
        return a->source_order < b->source_order ? -1 : 1;
    return a->decl_order < b->decl_order ? -1 : 1;
}

static void
gather_matches(const nd_css_stylesheet *sheet, int origin,
               const nd_node *el, GArray *out)
{
    if (!sheet) return;
    for (guint ri = 0; ri < sheet->rules->len; ri++) {
        nd_css_rule *r = g_ptr_array_index(sheet->rules, ri);
        gboolean any = FALSE;
        int best_a = 0, best_b = 0, best_c = 0;
        for (guint si = 0; si < r->selectors->len; si++) {
            nd_css_selector *sel = g_ptr_array_index(r->selectors, si);
            if (!match_selector(sel, el)) continue;
            if (!any || sel->spec_a > best_a ||
                (sel->spec_a == best_a && sel->spec_b > best_b) ||
                (sel->spec_a == best_a && sel->spec_b == best_b && sel->spec_c > best_c)) {
                best_a = sel->spec_a; best_b = sel->spec_b; best_c = sel->spec_c;
            }
            any = TRUE;
        }
        if (!any) continue;
        for (guint di = 0; di < r->decls->len; di++) {
            nd_css_decl *d = &g_array_index(r->decls, nd_css_decl, di);
            match_entry e = {
                .origin = origin,
                .spec_a = best_a, .spec_b = best_b, .spec_c = best_c,
                .source_order = r->source_order,
                .decl_order = (int)di,
                .important = d->important,
                .value = d->value,
                .prop  = d->prop,
            };
            g_array_append_val(out, e);
        }
    }
}

static const char *kUa =
    "html, body { display: block; color: #1a1a1a; background-color: #fefefe; "
    "font-family: sans-serif; font-size: 16px; line-height: 24px; }\n"
    "body { padding: 8px 16px; }\n"
    "div, p, section, article, header, footer, nav, main, aside, "
    "ul, ol, li, dl, dt, dd, blockquote, pre, address, "
    "hr, form, fieldset, figure, figcaption, center { display: block; }\n"
    "center { text-align: center; }\n"
    "h1, h2, h3, h4, h5, h6 { display: block; font-weight: bold; "
    "font-family: sans-serif; color: #111; }\n"
    "span, a, b, i, em, strong, code, small, u, s, del, ins, mark, "
    "tt, kbd, samp, var, cite, dfn, abbr, sub, sup, q, time, "
    "button, label { display: inline; }\n"
    "h1 { font-size: 2.0em;  margin: 0.67em 0; }\n"
    "h2 { font-size: 1.55em; margin: 0.75em 0; }\n"
    "h3 { font-size: 1.30em; margin: 0.83em 0; }\n"
    "h4 { font-size: 1.10em; margin: 1.10em 0; }\n"
    "h5 { font-size: 0.95em; margin: 1.50em 0; }\n"
    "h6 { font-size: 0.85em; margin: 1.65em 0; }\n"
    "p { margin: 0.9em 0; }\n"
    "blockquote { margin: 1em 24px; }\n"
    "hr { margin: 12px 0; height: 1px; }\n"
    "ul, ol { padding-left: 32px; margin: 0.6em 0; }\n"
    "li { margin: 2px 0; }\n"
    "dl { margin: 0.6em 0; } dt { font-weight: bold; } dd { margin-left: 24px; }\n"
    "a { color: #0645ad; }\n"
    "b, strong { font-weight: bold; }\n"
    "i, em, cite, dfn { font-style: italic; }\n"
    "code, pre, kbd, samp, tt { font-family: monospace; }\n"
    "pre { margin: 0.9em 0; padding: 6px; background-color: #f4f4f4; }\n"
    "code { background-color: #f4f4f4; padding: 1px 4px; }\n"
    "mark { background-color: #ffff00; }\n"
    "small { font-size: 0.85em; }\n"
    "sub, sup { font-size: 0.75em; }\n"
    "table { display: block; margin: 0.6em 0; }\n"
    "caption { display: block; font-weight: bold; padding-bottom: 4px; }\n"
    "tbody, thead, tfoot, colgroup, col { display: block; }\n"
    "tr { display: block; padding: 2px 0; }\n"
    "td, th { display: inline; padding: 2px 8px; }\n"
    "th { font-weight: bold; }\n"
    "img { display: inline; }\n"
    "figure { margin: 0.6em 24px; }\n"
    "figcaption { font-style: italic; font-size: 0.9em; }\n"
    "button { padding: 4px 12px; background-color: #e6e6e6; "
    "border-top-width: 1px; border-right-width: 1px; "
    "border-bottom-width: 1px; border-left-width: 1px; "
    "border-top-color: #b8b8b8; border-right-color: #b8b8b8; "
    "border-bottom-color: #b8b8b8; border-left-color: #b8b8b8; }\n"
    "input, textarea, select { padding: 2px 6px; background-color: #fff; "
    "border-top-width: 1px; border-right-width: 1px; "
    "border-bottom-width: 1px; border-left-width: 1px; "
    "border-top-color: #b8b8b8; border-right-color: #b8b8b8; "
    "border-bottom-color: #b8b8b8; border-left-color: #b8b8b8; }\n"
    "head, script, style, title, meta, link, noscript { display: none; }\n";

static double
resolve_font_size_px(const nd_style *s, const nd_style *parent_style)
{
    double parent_px = 16;
    if (parent_style && parent_style->values[ND_CSS_FONT_SIZE] &&
        parent_style->values[ND_CSS_FONT_SIZE]->kind == ND_CSS_V_LENGTH &&
        parent_style->values[ND_CSS_FONT_SIZE]->u.length.unit == ND_CSS_UNIT_PX)
        parent_px = parent_style->values[ND_CSS_FONT_SIZE]->u.length.v;
    nd_css_value *fs = s ? s->values[ND_CSS_FONT_SIZE] : NULL;
    if (!fs || fs->kind != ND_CSS_V_LENGTH) return parent_px;
    switch (fs->u.length.unit) {
    case ND_CSS_UNIT_PX:      return fs->u.length.v;
    case ND_CSS_UNIT_EM:      return fs->u.length.v * parent_px;
    case ND_CSS_UNIT_PERCENT: return fs->u.length.v * parent_px / 100.0;
    }
    return parent_px;
}

static void
resolve_em_units(nd_style *out, const nd_style *parent_style)
{
    double my_font_px = resolve_font_size_px(out, parent_style);
    if (out->values[ND_CSS_FONT_SIZE] &&
        out->values[ND_CSS_FONT_SIZE]->kind == ND_CSS_V_LENGTH) {
        out->values[ND_CSS_FONT_SIZE]->u.length.v = my_font_px;
        out->values[ND_CSS_FONT_SIZE]->u.length.unit = ND_CSS_UNIT_PX;
    } else {
        nd_css_value *fs = g_new0(nd_css_value, 1);
        fs->kind = ND_CSS_V_LENGTH;
        fs->u.length.v = my_font_px;
        fs->u.length.unit = ND_CSS_UNIT_PX;
        out->values[ND_CSS_FONT_SIZE] = fs;
    }
    for (int i = 0; i < ND_CSS_PROP_COUNT; i++) {
        if (i == ND_CSS_FONT_SIZE) continue;
        nd_css_value *v = out->values[i];
        if (!v || v->kind != ND_CSS_V_LENGTH) continue;
        if (v->u.length.unit == ND_CSS_UNIT_EM) {
            v->u.length.v *= my_font_px;
            v->u.length.unit = ND_CSS_UNIT_PX;
        }
    }
}

static void
cascade_for(const nd_node *el, GArray *matches, nd_style *out, const nd_style *parent_style)
{
    g_array_sort(matches, match_cmp);
    for (guint i = 0; i < matches->len; i++) {
        match_entry *m = &g_array_index(matches, match_entry, i);
        nd_css_value_free(out->values[m->prop]);
        out->values[m->prop] = nd_css_value_dup(m->value);
    }
    if (parent_style) {
        for (int i = 0; i < ND_CSS_PROP_COUNT; i++) {
            if (out->values[i]) continue;
            if (!prop_inherits((nd_css_prop)i)) continue;
            if (parent_style->values[i])
                out->values[i] = nd_css_value_dup(parent_style->values[i]);
        }
    }
    resolve_em_units(out, parent_style);
    (void)el;
}

static void
cascade_walk(const nd_node *node,
             const nd_css_stylesheet *ua,
             const nd_css_stylesheet *const *author, gsize n_author,
             const nd_style *parent_style,
             GHashTable *out)
{
    const nd_style *child_parent_style = parent_style;
    if (node->kind == ND_NODE_ELEMENT) {
        nd_style *s = g_new0(nd_style, 1);
        GArray *matches = g_array_new(FALSE, FALSE, sizeof(match_entry));
        gather_matches(ua, 0, node, matches);
        for (gsize i = 0; i < n_author; i++)
            gather_matches(author[i], 1, node, matches);

        const char *inline_css = nd_element_get_attr(node, "style");
        nd_css_stylesheet *inline_sheet = NULL;
        if (inline_css && *inline_css) {
            char *wrapped = g_strconcat("* { ", inline_css, " }", NULL);
            inline_sheet = nd_css_stylesheet_parse(wrapped, -1);
            g_free(wrapped);
            for (guint ri = 0; ri < inline_sheet->rules->len; ri++) {
                nd_css_rule *r = g_ptr_array_index(inline_sheet->rules, ri);
                for (guint di = 0; di < r->decls->len; di++) {
                    nd_css_decl *d = &g_array_index(r->decls, nd_css_decl, di);
                    match_entry e = {
                        .origin = 1,
                        .spec_a = 1000, .spec_b = 0, .spec_c = 0,
                        .source_order = INT_MAX,
                        .decl_order = (int)di,
                        .important = d->important,
                        .value = d->value,
                        .prop  = d->prop,
                    };
                    g_array_append_val(matches, e);
                }
            }
        }

        cascade_for(node, matches, s, parent_style);
        g_array_free(matches, TRUE);
        if (inline_sheet) nd_css_stylesheet_free(inline_sheet);
        g_hash_table_insert(out, (gpointer)node, s);
        child_parent_style = s;
    }
    for (const nd_node *c = node->first_child; c; c = c->next_sibling)
        cascade_walk(c, ua, author, n_author, child_parent_style, out);
}

GHashTable *
nd_css_compute(nd_node *doc,
               const nd_css_stylesheet *const *author_sheets,
               gsize n_sheets)
{
    GHashTable *out = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, (GDestroyNotify)nd_style_free);
    nd_css_stylesheet *ua = nd_css_stylesheet_parse(kUa, -1);
    cascade_walk(doc, ua, author_sheets, n_sheets, NULL, out);
    nd_css_stylesheet_free(ua);
    return out;
}
