/* Nordstjernen — CSS parser, selectors, cascade. */

#include "css.h"

#include "config.h"

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
    [ND_CSS_MIN_WIDTH]            = "min-width",
    [ND_CSS_MIN_HEIGHT]           = "min-height",
    [ND_CSS_LINE_HEIGHT]          = "line-height",
    [ND_CSS_TEXT_DECORATION]      = "text-decoration",
    [ND_CSS_POSITION]             = "position",
    [ND_CSS_TOP]                  = "top",
    [ND_CSS_RIGHT]                = "right",
    [ND_CSS_BOTTOM]               = "bottom",
    [ND_CSS_LEFT]                 = "left",
    [ND_CSS_Z_INDEX]              = "z-index",
    [ND_CSS_OPACITY]              = "opacity",
    [ND_CSS_CURSOR]               = "cursor",
    [ND_CSS_LETTER_SPACING]       = "letter-spacing",
    [ND_CSS_WORD_SPACING]         = "word-spacing",
    [ND_CSS_WHITE_SPACE]          = "white-space",
    [ND_CSS_BOX_SIZING]           = "box-sizing",
    [ND_CSS_TEXT_INDENT]          = "text-indent",
    [ND_CSS_TEXT_TRANSFORM]       = "text-transform",
    [ND_CSS_LIST_STYLE_TYPE]      = "list-style-type",
    [ND_CSS_VERTICAL_ALIGN]       = "vertical-align",
    [ND_CSS_VISIBILITY]           = "visibility",
    [ND_CSS_OVERFLOW]             = "overflow",
    [ND_CSS_FONT_VARIANT]         = "font-variant",
    [ND_CSS_BORDER_RADIUS]        = "border-radius",
    [ND_CSS_FLEX_DIRECTION]       = "flex-direction",
    [ND_CSS_FLEX_WRAP]            = "flex-wrap",
    [ND_CSS_JUSTIFY_CONTENT]      = "justify-content",
    [ND_CSS_ALIGN_ITEMS]          = "align-items",
    [ND_CSS_ALIGN_SELF]           = "align-self",
    [ND_CSS_GAP]                  = "gap",
    [ND_CSS_ROW_GAP]              = "row-gap",
    [ND_CSS_COLUMN_GAP]           = "column-gap",
    [ND_CSS_FLEX_GROW]            = "flex-grow",
    [ND_CSS_FLEX_SHRINK]          = "flex-shrink",
    [ND_CSS_FLEX_BASIS]           = "flex-basis",
    [ND_CSS_ORDER]                = "order",
    [ND_CSS_FLOAT]                = "float",
    [ND_CSS_CLEAR]                = "clear",
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
    case ND_CSS_TEXT_TRANSFORM:
    case ND_CSS_LIST_STYLE_TYPE:
    case ND_CSS_VISIBILITY:
    case ND_CSS_FONT_VARIANT:
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
    case ND_CSS_V_CALC:    o->u.calc = v->u.calc; break;
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

double
nd_css_length_or(const nd_css_value *v, double fallback)
{
    if (!v) return fallback;
    if (v->kind == ND_CSS_V_LENGTH &&
        (v->u.length.unit == ND_CSS_UNIT_PX ||
         v->u.length.unit == ND_CSS_UNIT_NUMBER))
        return v->u.length.v;
    if (v->kind == ND_CSS_V_CALC)
        return v->u.calc.px;
    return fallback;
}

gboolean
nd_css_keyword_is(const nd_css_value *v, const char *kw)
{
    return v && v->kind == ND_CSS_V_KEYWORD && kw &&
           v->u.keyword && strcmp(v->u.keyword, kw) == 0;
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

static double
hsl_hue_to_rgb(double p, double q, double t)
{
    if (t < 0) t += 1.0;
    if (t > 1) t -= 1.0;
    if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5)     return q;
    if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
    return p;
}

static gboolean
parse_hsl_func(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
    gboolean is_hsla = g_ascii_strncasecmp(s, "hsla(", 5) == 0;
    gboolean is_hsl  = !is_hsla && g_ascii_strncasecmp(s, "hsl(", 4) == 0;
    if (!is_hsl && !is_hsla) return FALSE;
    const char *p = strchr(s, '(');
    if (!p) return FALSE;
    p++;
    double values[4] = { 0, 0, 0, 1 };
    int count = 0;
    while (*p && *p != ')' && count < 4) {
        while (*p == ' ' || *p == ',' || *p == '/') p++;
        if (!*p || *p == ')') break;
        char *end = NULL;
        double v = g_ascii_strtod(p, &end);
        if (!end || end == p) return FALSE;
        if (*end == '%') end++;
        if (g_ascii_strncasecmp(end, "deg", 3) == 0) end += 3;
        values[count++] = v;
        p = end;
    }
    if (count < 3) return FALSE;
    double h = values[0] / 360.0;
    while (h < 0) h += 1.0;
    while (h > 1) h -= 1.0;
    double sat = values[1] / 100.0;
    if (sat < 0) sat = 0;
    if (sat > 1) sat = 1;
    double lig = values[2] / 100.0;
    if (lig < 0) lig = 0;
    if (lig > 1) lig = 1;
    double rr, gg, bb;
    if (sat == 0) {
        rr = gg = bb = lig;
    } else {
        double q = lig < 0.5 ? lig * (1 + sat) : lig + sat - lig * sat;
        double pp = 2 * lig - q;
        rr = hsl_hue_to_rgb(pp, q, h + 1.0/3.0);
        gg = hsl_hue_to_rgb(pp, q, h);
        bb = hsl_hue_to_rgb(pp, q, h - 1.0/3.0);
    }
    *r = (guint8)CLAMP((int)(rr * 255 + 0.5), 0, 255);
    *g = (guint8)CLAMP((int)(gg * 255 + 0.5), 0, 255);
    *b = (guint8)CLAMP((int)(bb * 255 + 0.5), 0, 255);
    if (is_hsla) {
        double alpha = values[3];
        if (alpha > 1) alpha /= 100.0;
        *a = (guint8)CLAMP((int)(alpha * 255 + 0.5), 0, 255);
    } else {
        *a = 255;
    }
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
    if (parse_hsl_func(s, r, g, b, a)) return TRUE;
    if (s[0] == '#') {
        gsize n = strlen(s + 1);
        if (n == 3 || n == 4) {
            int rr = g_ascii_xdigit_value(s[1]);
            int gg = g_ascii_xdigit_value(s[2]);
            int bb = g_ascii_xdigit_value(s[3]);
            if (rr < 0 || gg < 0 || bb < 0) return FALSE;
            *r = (guint8)(rr * 17); *g = (guint8)(gg * 17); *b = (guint8)(bb * 17);
            if (n == 4) {
                int aa = g_ascii_xdigit_value(s[4]);
                if (aa < 0) return FALSE;
                *a = (guint8)(aa * 17);
            }
            return TRUE;
        }
        if (n == 6 || n == 8) {
            int v[8];
            for (gsize i = 0; i < n; i++) {
                v[i] = g_ascii_xdigit_value(s[1 + i]);
                if (v[i] < 0) return FALSE;
            }
            *r = (guint8)(v[0] * 16 + v[1]);
            *g = (guint8)(v[2] * 16 + v[3]);
            *b = (guint8)(v[4] * 16 + v[5]);
            if (n == 8) *a = (guint8)(v[6] * 16 + v[7]);
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
    s->pseudos = g_array_new(FALSE, FALSE, sizeof(nd_css_pseudo_pred));
    return s;
}

static void
nd_css_simple_free(nd_css_simple *s)
{
    if (!s) return;
    g_free(s->type);
    g_free(s->id);
    g_ptr_array_free(s->classes, TRUE);
    if (s->attrs)   g_array_free(s->attrs,   TRUE);
    if (s->pseudos) g_array_free(s->pseudos, TRUE);
    g_free(s);
}

static gboolean
parse_pseudo_keyword(const char *name, gsize n,
                     const char *arg, gsize alen,
                     nd_css_pseudo_pred *out)
{
    struct { const char *k; nd_css_pseudo v; } table[] = {
        { "first-child",   ND_CSS_PC_FIRST_CHILD },
        { "last-child",    ND_CSS_PC_LAST_CHILD },
        { "only-child",    ND_CSS_PC_ONLY_CHILD },
        { "first-of-type", ND_CSS_PC_FIRST_OF_TYPE },
        { "last-of-type",  ND_CSS_PC_LAST_OF_TYPE },
        { "empty",         ND_CSS_PC_EMPTY },
        { "root",          ND_CSS_PC_ROOT },
        { "checked",       ND_CSS_PC_CHECKED },
        { "disabled",      ND_CSS_PC_DISABLED },
        { "enabled",       ND_CSS_PC_ENABLED },
        { "required",      ND_CSS_PC_REQUIRED },
        { "optional",      ND_CSS_PC_OPTIONAL },
        { "link",          ND_CSS_PC_LINK },
        { "visited",       ND_CSS_PC_VISITED },
        { "hover",         ND_CSS_PC_HOVER },
        { "active",        ND_CSS_PC_ACTIVE },
        { "focus",         ND_CSS_PC_FOCUS },
        { "focus-visible", ND_CSS_PC_FOCUS },
        { "focus-within",  ND_CSS_PC_FOCUS },
        { "target",        ND_CSS_PC_TARGET },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(table); i++) {
        gsize klen = strlen(table[i].k);
        if (klen == n && g_ascii_strncasecmp(name, table[i].k, n) == 0) {
            out->kind = table[i].v;
            out->a = 0;
            out->b = 0;
            return TRUE;
        }
    }
    if (arg && ((n == 9 && g_ascii_strncasecmp(name, "nth-child",   9) == 0) ||
                (n == 11 && g_ascii_strncasecmp(name, "nth-of-type", 11) == 0))) {
        char *s = g_strndup(arg, alen);
        g_strstrip(s);
        int a = 0, b = 0;
        if (g_ascii_strcasecmp(s, "odd") == 0) { a = 2; b = 1; }
        else if (g_ascii_strcasecmp(s, "even") == 0) { a = 2; b = 0; }
        else {
            char *n_pos = strchr(s, 'n');
            if (!n_pos) n_pos = strchr(s, 'N');
            if (n_pos) {
                *n_pos = '\0';
                char *a_str = s;
                while (*a_str == ' ') a_str++;
                if (!*a_str || strcmp(a_str, "+") == 0) a = 1;
                else if (strcmp(a_str, "-") == 0) a = -1;
                else a = atoi(a_str);
                char *b_str = n_pos + 1;
                while (*b_str == ' ') b_str++;
                if (*b_str) b = atoi(b_str);
            } else {
                a = 0;
                b = atoi(s);
            }
        }
        g_free(s);
        out->kind = (n == 9) ? ND_CSS_PC_NTH_CHILD : ND_CSS_PC_NTH_OF_TYPE;
        out->a = a;
        out->b = b;
        return TRUE;
    }
    return FALSE;
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

        if (c == '+') {
            pending = ND_CSS_COMB_ADJACENT;
            expect_compound = TRUE;
            p++;
            continue;
        }

        if (c == '~') {
            pending = ND_CSS_COMB_SIBLING;
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
            } else if (cc == ':') {
                p++;
                gboolean is_element = (p < end && *p == ':');
                if (is_element) p++;
                const char *name_s = p;
                while (p < end && (is_ident(*p) || *p == '-')) p++;
                gsize name_n = (gsize)(p - name_s);
                const char *arg_s = NULL;
                gsize arg_n = 0;
                if (p < end && *p == '(') {
                    int depth = 1;
                    p++;
                    arg_s = p;
                    while (p < end && depth > 0) {
                        if (*p == '(') depth++;
                        else if (*p == ')') { depth--; if (depth == 0) break; }
                        p++;
                    }
                    arg_n = (gsize)(p - arg_s);
                    if (p < end && *p == ')') p++;
                }
                if (!is_element && name_n > 0) {
                    nd_css_pseudo_pred pc = {0};
                    if (parse_pseudo_keyword(name_s, name_n, arg_s, arg_n, &pc)) {
                        g_array_append_val(cmp->pseudos, pc);
                        sel->spec_b += 1;
                    }
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
                    else if (op_c == '|') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_HYPHEN; }
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
    if (*end == '\0') { *out_unit = ND_CSS_UNIT_NUMBER; return TRUE; }
    if (g_ascii_strcasecmp(end, "px") == 0) { *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "em")  == 0 ||
        g_ascii_strcasecmp(end, "rem") == 0) { *out_unit = ND_CSS_UNIT_EM; return TRUE; }
    if (g_ascii_strcasecmp(end, "%")   == 0) { *out_unit = ND_CSS_UNIT_PERCENT; return TRUE; }
    if (g_ascii_strcasecmp(end, "vw") == 0 ||
        g_ascii_strcasecmp(end, "vh") == 0 ||
        g_ascii_strcasecmp(end, "vmin") == 0 ||
        g_ascii_strcasecmp(end, "vmax") == 0) {
        *out_unit = ND_CSS_UNIT_PERCENT;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "pt")  == 0) {
        *out_v = v * 1.333;
        *out_unit = ND_CSS_UNIT_PX;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "pc")  == 0) {
        *out_v = v * 16.0;
        *out_unit = ND_CSS_UNIT_PX;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "ex")  == 0 ||
        g_ascii_strcasecmp(end, "ch")  == 0) {
        *out_unit = ND_CSS_UNIT_EM;
        *out_v = v * 0.5;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "cm")  == 0) { *out_v = v * 37.795; *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "mm")  == 0) { *out_v = v * 3.7795; *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "in")  == 0) { *out_v = v * 96.0;   *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    return FALSE;
}

static nd_css_value *
parse_calc(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (g_ascii_strncasecmp(text, "calc(", 5) != 0) return NULL;
    text += 5;
    const char *end = strrchr(text, ')');
    if (!end) return NULL;
    double pct = 0;
    double px  = 0;
    double sign = 1;
    const char *p = text;
    while (p < end) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;
        if (*p == '+') { sign = 1; p++; continue; }
        if (*p == '-' && (p == text || !g_ascii_isdigit(*(p - 1)))) {
            sign = -1; p++; continue;
        }
        char *tend = NULL;
        double num = g_ascii_strtod(p, &tend);
        if (!tend || tend == p) break;
        p = tend;
        if (*p == '%') {
            pct += num * sign;
            p++;
        } else if (g_ascii_strncasecmp(p, "px", 2) == 0) {
            px += num * sign;
            p += 2;
        } else if (g_ascii_strncasecmp(p, "em", 2) == 0 ||
                   g_ascii_strncasecmp(p, "rem", 3) == 0) {
            px += num * sign * 16.0;
            p += (g_ascii_strncasecmp(p, "rem", 3) == 0) ? 3 : 2;
        } else if (g_ascii_strncasecmp(p, "vh", 2) == 0 ||
                   g_ascii_strncasecmp(p, "vw", 2) == 0) {
            pct += num * sign;
            p += 2;
        } else if (g_ascii_strncasecmp(p, "pt", 2) == 0) {
            px += num * sign * (96.0 / 72.0);
            p += 2;
        } else {
            px += num * sign;
        }
        sign = 1;
        while (p < end && is_ws(*p)) p++;
        if (p < end && *p == '*') {
            p++;
            while (p < end && is_ws(*p)) p++;
            double m = g_ascii_strtod(p, &tend);
            if (tend && tend > p) {
                pct *= m;
                px  *= m;
                p = tend;
            }
        }
    }
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_CALC;
    v->u.calc.pct = pct;
    v->u.calc.px  = px;
    return v;
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
        } else {
            char *kw = ascii_lower(t, strlen(t));
            if (kw && (strcmp(kw, "currentcolor") == 0 ||
                       strcmp(kw, "inherit") == 0 ||
                       strcmp(kw, "transparent") == 0)) {
                v = g_new0(nd_css_value, 1);
                v->kind = ND_CSS_V_KEYWORD;
                v->u.keyword = kw;
            } else {
                g_free(kw);
            }
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
    case ND_CSS_MIN_WIDTH: case ND_CSS_MIN_HEIGHT:
    case ND_CSS_LETTER_SPACING: case ND_CSS_WORD_SPACING:
    case ND_CSS_TEXT_INDENT:
    case ND_CSS_OPACITY:
    case ND_CSS_BORDER_RADIUS:
    case ND_CSS_GAP: case ND_CSS_ROW_GAP: case ND_CSS_COLUMN_GAP:
    case ND_CSS_FLEX_GROW: case ND_CSS_FLEX_SHRINK:
    case ND_CSS_FLEX_BASIS:
    case ND_CSS_ORDER:
    case ND_CSS_LINE_HEIGHT:
    case ND_CSS_TOP: case ND_CSS_RIGHT:
    case ND_CSS_BOTTOM: case ND_CSS_LEFT: {
        if (g_ascii_strcasecmp(t, "auto") == 0) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = g_strdup("auto");
        } else if ((v = parse_calc(t))) {

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

static char *
substitute_var_fallbacks(const char *vtext)
{
    if (!vtext) return NULL;
    GString *out = g_string_new(NULL);
    const char *p = vtext;
    while (*p) {
        if (g_ascii_strncasecmp(p, "var(", 4) == 0) {
            p += 4;
            int depth = 1;
            const char *fallback_start = NULL;
            const char *fallback_end = NULL;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') {
                    depth--;
                    if (depth == 0) { fallback_end = p; p++; break; }
                } else if (*p == ',' && depth == 1 && !fallback_start) {
                    fallback_start = p + 1;
                }
                p++;
            }
            if (fallback_start) {
                if (!fallback_end) fallback_end = p;
                while (fallback_start < fallback_end && is_ws(*fallback_start))
                    fallback_start++;
                while (fallback_end > fallback_start && is_ws(*(fallback_end - 1)))
                    fallback_end--;
                char *nested = g_strndup(fallback_start,
                                         (gsize)(fallback_end - fallback_start));
                char *sub = substitute_var_fallbacks(nested);
                if (sub) g_string_append(out, sub);
                g_free(nested); g_free(sub);
            }
        } else {
            g_string_append_c(out, *p);
            p++;
        }
    }
    return g_string_free(out, FALSE);
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
        char *raw_vtext = g_strndup(vstart, (gsize)(p - vstart));
        char *vtext = substitute_var_fallbacks(raw_vtext);
        g_free(raw_vtext);
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

        if (strcmp(pname, "background") == 0) {
            char *tokens[16] = {0};
            int n = split_ws(vtext, tokens);
            for (int i = 0; i < n; i++) {
                guint8 r, g, b, a;
                if (parse_color(tokens[i], &r, &g, &b, &a)) {
                    nd_css_value *v = g_new0(nd_css_value, 1);
                    v->kind = ND_CSS_V_COLOR;
                    v->u.color.r = r; v->u.color.g = g;
                    v->u.color.b = b; v->u.color.a = a;
                    nd_css_decl decl = {
                        .prop = ND_CSS_BACKGROUND_COLOR,
                        .value = v,
                        .important = important,
                    };
                    g_array_append_val(decls_out, decl);
                    break;
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "font") == 0) {
            char *tokens[16] = {0};
            int n = split_ws(vtext, tokens);
            char *family_buf = NULL;
            for (int i = 0; i < n; i++) {
                double num; nd_css_unit u;
                if (parse_length(tokens[i], &num, &u)) {
                    char *slash = strchr(tokens[i], '/');
                    nd_css_value *v = g_new0(nd_css_value, 1);
                    v->kind = ND_CSS_V_LENGTH;
                    v->u.length.v = num;
                    v->u.length.unit = u;
                    nd_css_decl d = {
                        .prop = ND_CSS_FONT_SIZE, .value = v,
                        .important = important
                    };
                    g_array_append_val(decls_out, d);
                    if (slash) {
                        double lh; nd_css_unit lu;
                        if (parse_length(slash + 1, &lh, &lu)) {
                            nd_css_value *lv = g_new0(nd_css_value, 1);
                            lv->kind = ND_CSS_V_LENGTH;
                            lv->u.length.v = lh;
                            lv->u.length.unit = lu;
                            nd_css_decl lhd = {
                                .prop = ND_CSS_LINE_HEIGHT,
                                .value = lv,
                                .important = important
                            };
                            g_array_append_val(decls_out, lhd);
                        }
                    }
                    if (i + 1 < n) {
                        GString *fam = g_string_new(NULL);
                        for (int j = i + 1; j < n; j++) {
                            if (j > i + 1) g_string_append_c(fam, ' ');
                            g_string_append(fam, tokens[j]);
                        }
                        family_buf = g_string_free(fam, FALSE);
                    }
                    break;
                }
            }
            if (family_buf) {
                nd_css_value *fv = g_new0(nd_css_value, 1);
                fv->kind = ND_CSS_V_KEYWORD;
                fv->u.keyword = family_buf;
                nd_css_decl fd = {
                    .prop = ND_CSS_FONT_FAMILY, .value = fv,
                    .important = important
                };
                g_array_append_val(decls_out, fd);
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "flex") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            double grow = 0, shrink = 1;
            char *basis = NULL;
            gboolean basis_set = FALSE;
            int numerics = 0;
            for (int i = 0; i < n; i++) {
                char *t = tokens[i];
                double num; nd_css_unit u;
                if (g_ascii_strcasecmp(t, "none") == 0) {
                    grow = 0; shrink = 0; basis = g_strdup("auto"); basis_set = TRUE;
                    break;
                }
                if (g_ascii_strcasecmp(t, "auto") == 0) {
                    grow = 1; shrink = 1; basis = g_strdup("auto"); basis_set = TRUE;
                    continue;
                }
                if (g_ascii_strcasecmp(t, "initial") == 0) {
                    grow = 0; shrink = 1; basis = g_strdup("auto"); basis_set = TRUE;
                    continue;
                }
                if (parse_length(t, &num, &u) && u != ND_CSS_UNIT_NUMBER) {
                    g_free(basis);
                    basis = g_strdup(t);
                    basis_set = TRUE;
                    continue;
                }
                if (parse_length(t, &num, &u) && u == ND_CSS_UNIT_NUMBER) {
                    if (numerics == 0)      grow = num;
                    else if (numerics == 1) shrink = num;
                    else if (numerics == 2) {
                        g_free(basis);
                        basis = g_strdup_printf("%g", num);
                        basis_set = TRUE;
                    }
                    numerics++;
                }
            }
            char grow_buf[32];
            g_snprintf(grow_buf, sizeof grow_buf, "%g", grow);
            char shrink_buf[32];
            g_snprintf(shrink_buf, sizeof shrink_buf, "%g", shrink);
            nd_css_value *gv = parse_value_for(ND_CSS_FLEX_GROW, grow_buf);
            if (gv) {
                nd_css_decl d = { .prop = ND_CSS_FLEX_GROW, .value = gv, .important = important };
                g_array_append_val(decls_out, d);
            }
            nd_css_value *sv = parse_value_for(ND_CSS_FLEX_SHRINK, shrink_buf);
            if (sv) {
                nd_css_decl d = { .prop = ND_CSS_FLEX_SHRINK, .value = sv, .important = important };
                g_array_append_val(decls_out, d);
            }
            if (basis_set) {
                nd_css_value *bv = parse_value_for(ND_CSS_FLEX_BASIS, basis);
                if (bv) {
                    nd_css_decl d = { .prop = ND_CSS_FLEX_BASIS, .value = bv, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            g_free(basis);
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "flex-flow") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            for (int i = 0; i < n; i++) {
                char *t = tokens[i];
                if (g_ascii_strcasecmp(t, "row") == 0 ||
                    g_ascii_strcasecmp(t, "row-reverse") == 0 ||
                    g_ascii_strcasecmp(t, "column") == 0 ||
                    g_ascii_strcasecmp(t, "column-reverse") == 0) {
                    nd_css_value *v = parse_value_for(ND_CSS_FLEX_DIRECTION, t);
                    if (v) {
                        nd_css_decl d = { .prop = ND_CSS_FLEX_DIRECTION, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                } else if (g_ascii_strcasecmp(t, "wrap") == 0 ||
                           g_ascii_strcasecmp(t, "nowrap") == 0 ||
                           g_ascii_strcasecmp(t, "wrap-reverse") == 0) {
                    nd_css_value *v = parse_value_for(ND_CSS_FLEX_WRAP, t);
                    if (v) {
                        nd_css_decl d = { .prop = ND_CSS_FLEX_WRAP, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "list-style") == 0) {
            char *tokens[8] = {0};
            int n = split_ws(vtext, tokens);
            const char *type_kws[] = {
                "none", "disc", "circle", "square",
                "decimal", "decimal-leading-zero",
                "lower-alpha", "upper-alpha", "lower-latin", "upper-latin",
                "lower-roman", "upper-roman",
                NULL
            };
            for (int i = 0; i < n; i++) {
                for (int k = 0; type_kws[k]; k++) {
                    if (g_ascii_strcasecmp(tokens[i], type_kws[k]) == 0) {
                        nd_css_value *v = g_new0(nd_css_value, 1);
                        v->kind = ND_CSS_V_KEYWORD;
                        v->u.keyword = g_strdup(type_kws[k]);
                        nd_css_decl d = {
                            .prop = ND_CSS_LIST_STYLE_TYPE, .value = v,
                            .important = important
                        };
                        g_array_append_val(decls_out, d);
                        break;
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

#define ND_MQ_VIEWPORT 1000

static gboolean
media_feature_matches(const char *name, const char *value)
{
    if (!name) return FALSE;
    int n = value ? atoi(value) : 0;
    if (g_ascii_strcasecmp(name, "max-width") == 0 ||
        g_ascii_strcasecmp(name, "max-device-width") == 0)
        return n >= ND_MQ_VIEWPORT;
    if (g_ascii_strcasecmp(name, "min-width") == 0 ||
        g_ascii_strcasecmp(name, "min-device-width") == 0)
        return n <= ND_MQ_VIEWPORT;
    if (g_ascii_strcasecmp(name, "orientation") == 0)
        return !value || g_ascii_strcasecmp(value, "landscape") == 0;
    return TRUE;
}

static gboolean
media_query_one_matches(const char *q)
{
    while (*q && is_ws(*q)) q++;
    gboolean invert = FALSE;
    if (g_str_has_prefix(q, "not ") || g_str_has_prefix(q, "NOT ")) {
        invert = TRUE; q += 4;
        while (*q && is_ws(*q)) q++;
    }
    if (g_str_has_prefix(q, "only ") || g_str_has_prefix(q, "ONLY ")) {
        q += 5;
        while (*q && is_ws(*q)) q++;
    }
    gboolean match = TRUE;
    while (*q) {
        while (*q && is_ws(*q)) q++;
        if (!*q) break;
        if (*q == '(') {
            q++;
            const char *ns = q;
            while (*q && *q != ':' && *q != ')') q++;
            char *name = g_strndup(ns, (gsize)(q - ns));
            g_strstrip(name);
            char *value = NULL;
            if (*q == ':') {
                q++;
                while (*q && is_ws(*q)) q++;
                const char *vs = q;
                while (*q && *q != ')') q++;
                value = g_strndup(vs, (gsize)(q - vs));
                g_strstrip(value);
            }
            if (*q == ')') q++;
            if (!media_feature_matches(name, value)) match = FALSE;
            g_free(name); g_free(value);
        } else if (g_ascii_isalpha(*q)) {
            const char *ts = q;
            while (g_ascii_isalpha(*q) || *q == '-') q++;
            gsize tlen = (gsize)(q - ts);
            char *type = g_strndup(ts, tlen);
            if (g_ascii_strcasecmp(type, "screen") != 0 &&
                g_ascii_strcasecmp(type, "all")    != 0 &&
                g_ascii_strcasecmp(type, "and")    != 0)
                match = FALSE;
            g_free(type);
        } else {
            q++;
        }
    }
    return invert ? !match : match;
}

static gboolean
media_query_matches(const char *query)
{
    if (!query || !*query) return TRUE;
    char **alts = g_strsplit(query, ",", -1);
    gboolean any = FALSE;
    for (int i = 0; alts[i] && !any; i++) {
        if (media_query_one_matches(alts[i])) any = TRUE;
    }
    g_strfreev(alts);
    return any;
}

gboolean
nd_css_media_query_matches(const char *query)
{
    return media_query_matches(query);
}

static void
parse_rules_until(const char **pp, const char *end,
                  nd_css_stylesheet *sh, int *source_order,
                  char close_at)
{
    const char *p = *pp;
    while (p < end) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;

        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < end) p += 2;
            continue;
        }
        if (*p == close_at) { p++; break; }
        if (*p == '@') {
            const char *at_start = p;
            p++;
            const char *name_start = p;
            while (p < end && (g_ascii_isalpha(*p) || *p == '-')) p++;
            gsize name_len = (gsize)(p - name_start);
            if (name_len == 8 && g_ascii_strncasecmp(name_start, "supports", 8) == 0) {
                while (p < end && *p != '{' && *p != ';') p++;
                if (p < end && *p == '{') {
                    p++;
                    parse_rules_until(&p, end, sh, source_order, '}');
                } else if (p < end && *p == ';') p++;
                continue;
            }
            if (name_len == 5 && g_ascii_strncasecmp(name_start, "media", 5) == 0) {
                const char *cond_start = p;
                while (p < end && *p != '{' && *p != ';') p++;
                gsize cond_len = (gsize)(p - cond_start);
                char *cond = g_strndup(cond_start, cond_len);
                g_strstrip(cond);
                if (p < end && *p == '{') {
                    p++;
                    if (media_query_matches(cond)) {
                        parse_rules_until(&p, end, sh, source_order, '}');
                    } else {
                        int depth = 1;
                        while (p < end && depth > 0) {
                            if (*p == '{') depth++;
                            else if (*p == '}') depth--;
                            p++;
                        }
                    }
                } else if (p < end && *p == ';') p++;
                g_free(cond);
                continue;
            }
            p = at_start;
            skip_at_rule(&p, end);
            continue;
        }

        nd_css_rule *rule = g_new0(nd_css_rule, 1);
        rule->selectors = g_ptr_array_new();
        rule->decls     = g_array_new(FALSE, FALSE, sizeof(nd_css_decl));
        rule->source_order = (*source_order)++;

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
    *pp = p;
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
    parse_rules_until(&p, end, sh, &source_order, 0);
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
        if (!el->name || g_ascii_strcasecmp(sel->type, el->name) != 0) return FALSE;
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
                case ND_CSS_ATTR_HYPHEN: {
                    if (vl < wl) return FALSE;
                    if (strncmp(v, a->value, wl) != 0) return FALSE;
                    if (vl > wl && v[wl] != '-') return FALSE;
                    break;
                }
                case ND_CSS_ATTR_PRESENT: break;
                }
            }
        }
    }
    if (sel->pseudos && sel->pseudos->len > 0) {
        for (guint i = 0; i < sel->pseudos->len; i++) {
            const nd_css_pseudo_pred *pc =
                &g_array_index(sel->pseudos, nd_css_pseudo_pred, i);
            switch (pc->kind) {
            case ND_CSS_PC_FIRST_CHILD: {
                const nd_node *s = el->prev_sibling;
                while (s && s->kind != ND_NODE_ELEMENT) s = s->prev_sibling;
                if (s) return FALSE;
                break;
            }
            case ND_CSS_PC_LAST_CHILD: {
                const nd_node *s = el->next_sibling;
                while (s && s->kind != ND_NODE_ELEMENT) s = s->next_sibling;
                if (s) return FALSE;
                break;
            }
            case ND_CSS_PC_ONLY_CHILD: {
                const nd_node *s = el->prev_sibling;
                while (s && s->kind != ND_NODE_ELEMENT) s = s->prev_sibling;
                if (s) return FALSE;
                s = el->next_sibling;
                while (s && s->kind != ND_NODE_ELEMENT) s = s->next_sibling;
                if (s) return FALSE;
                break;
            }
            case ND_CSS_PC_FIRST_OF_TYPE: {
                if (!el->name) return FALSE;
                const nd_node *s = el->prev_sibling;
                while (s) {
                    if (s->kind == ND_NODE_ELEMENT && s->name &&
                        g_ascii_strcasecmp(s->name, el->name) == 0)
                        return FALSE;
                    s = s->prev_sibling;
                }
                break;
            }
            case ND_CSS_PC_LAST_OF_TYPE: {
                if (!el->name) return FALSE;
                const nd_node *s = el->next_sibling;
                while (s) {
                    if (s->kind == ND_NODE_ELEMENT && s->name &&
                        g_ascii_strcasecmp(s->name, el->name) == 0)
                        return FALSE;
                    s = s->next_sibling;
                }
                break;
            }
            case ND_CSS_PC_EMPTY:
                if (el->first_child) return FALSE;
                break;
            case ND_CSS_PC_ROOT:
                if (el->parent && el->parent->kind == ND_NODE_ELEMENT) return FALSE;
                break;
            case ND_CSS_PC_CHECKED:
                if (!nd_element_get_attr(el, "checked") &&
                    !nd_element_get_attr(el, "selected"))
                    return FALSE;
                break;
            case ND_CSS_PC_DISABLED:
                if (!nd_element_get_attr(el, "disabled")) return FALSE;
                break;
            case ND_CSS_PC_ENABLED:
                if (nd_element_get_attr(el, "disabled")) return FALSE;
                break;
            case ND_CSS_PC_REQUIRED:
                if (!nd_element_get_attr(el, "required")) return FALSE;
                break;
            case ND_CSS_PC_OPTIONAL:
                if (nd_element_get_attr(el, "required")) return FALSE;
                break;
            case ND_CSS_PC_NTH_CHILD:
            case ND_CSS_PC_NTH_OF_TYPE: {
                int idx = 1;
                if (pc->kind == ND_CSS_PC_NTH_OF_TYPE && el->name) {
                    for (const nd_node *s = el->prev_sibling; s; s = s->prev_sibling)
                        if (s->kind == ND_NODE_ELEMENT && s->name &&
                            g_ascii_strcasecmp(s->name, el->name) == 0)
                            idx++;
                } else {
                    for (const nd_node *s = el->prev_sibling; s; s = s->prev_sibling)
                        if (s->kind == ND_NODE_ELEMENT) idx++;
                }
                int a = pc->a, b = pc->b;
                if (a == 0) {
                    if (idx != b) return FALSE;
                } else {
                    int diff = idx - b;
                    if ((diff % a) != 0) return FALSE;
                    if ((diff / a) < 0) return FALSE;
                }
                break;
            }
            case ND_CSS_PC_LINK:
            case ND_CSS_PC_VISITED:
                if (!el->name || g_ascii_strcasecmp(el->name, "a") != 0)
                    return FALSE;
                if (!nd_element_get_attr(el, "href")) return FALSE;
                break;
            case ND_CSS_PC_HOVER:
            case ND_CSS_PC_ACTIVE:
            case ND_CSS_PC_FOCUS:
            case ND_CSS_PC_TARGET:
                return FALSE;
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
        } else if (comb == ND_CSS_COMB_ADJACENT) {
            const nd_node *s = cur->prev_sibling;
            while (s && s->kind != ND_NODE_ELEMENT) s = s->prev_sibling;
            if (!s || !match_simple(prev, s)) return FALSE;
            cur = s;
        } else if (comb == ND_CSS_COMB_SIBLING) {
            const nd_node *s = cur->prev_sibling;
            gboolean ok = FALSE;
            while (s) {
                if (s->kind == ND_NODE_ELEMENT && match_simple(prev, s)) {
                    cur = s;
                    ok = TRUE;
                    break;
                }
                s = s->prev_sibling;
            }
            if (!ok) return FALSE;
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
    "font-family: system-ui, sans-serif; font-size: 16px; line-height: 24px; }\n"
    "body { padding: 8px 16px; }\n"
    "div, p, section, article, header, footer, nav, main, aside, "
    "ul, ol, li, dl, dt, dd, blockquote, pre, address, "
    "hr, form, fieldset, figure, figcaption, center, "
    "legend, search, hgroup { display: block; }\n"
    "address { font-style: italic; }\n"
    "fieldset { margin: 0.5em 8px; padding: 0.35em 8px 0.6em; }\n"
    "legend { padding: 0 4px; font-weight: bold; }\n"
    "center { text-align: center; }\n"
    "h1, h2, h3, h4, h5, h6 { display: block; font-weight: bold; "
    "font-family: sans-serif; color: #111; line-height: 1.2; }\n"
    "span, a, b, i, em, strong, code, small, big, u, s, del, ins, mark, "
    "tt, kbd, samp, var, cite, dfn, abbr, acronym, sub, sup, q, time, "
    "bdi, bdo, ruby, rb, rt, output, "
    "button, label { display: inline; }\n"
    "var { font-style: italic; }\n"
    "rt { font-size: 0.7em; }\n"
    "abbr, acronym { text-decoration: underline; }\n"
    "rp, datalist { display: none; }\n"
    "menu { display: block; padding-left: 32px; margin: 0.6em 0; }\n"
    "h1 { font-size: 2.0em;  margin: 0.67em 0; }\n"
    "h2 { font-size: 1.55em; margin: 0.75em 0; }\n"
    "h3 { font-size: 1.30em; margin: 0.83em 0; }\n"
    "h4 { font-size: 1.10em; margin: 1.10em 0; }\n"
    "h5 { font-size: 0.95em; margin: 1.50em 0; }\n"
    "h6 { font-size: 0.85em; margin: 1.65em 0; }\n"
    "p { margin: 0.9em 0; }\n"
    "address { color: #555; }\n"
    "blockquote { margin: 1em 24px; border-left-width: 4px; "
    "border-left-color: #dddddd; padding-left: 12px; }\n"
    "hr { margin: 12px 0; height: 1px; }\n"
    "ul, ol { padding-left: 32px; margin: 0.6em 0; }\n"
    "li { margin: 2px 0; }\n"
    "dl { margin: 0.6em 0; } dt { font-weight: bold; } dd { margin-left: 24px; }\n"
    "dl > dt { margin-top: 0.3em; }\n"
    "a { color: #0645ad; }\n"
    "b, strong { font-weight: bold; }\n"
    "i, em, cite, dfn { font-style: italic; }\n"
    "ins { color: #006400; }\n"
    "del, s, strike { color: #8b0000; }\n"
    "big { font-size: 1.17em; }\n"
    "code, pre, kbd, samp, tt { font-family: monospace; }\n"
    "code, kbd, samp { white-space: pre-wrap; }\n"
    "pre { margin: 0.9em 0; padding: 6px; background-color: #f4f4f4; "
    "line-height: 1.4; }\n"
    "code { background-color: #f4f4f4; padding: 1px 4px; font-size: 0.93em; }\n"
    "samp { background-color: #f4f4f4; padding: 1px 4px; }\n"
    "kbd { background-color: #eeeeee; padding: 1px 4px; font-size: 0.9em; "
    "border-top-width: 1px; border-right-width: 1px; "
    "border-bottom-width: 1px; border-left-width: 1px; "
    "border-top-color: #aaaaaa; border-right-color: #aaaaaa; "
    "border-bottom-color: #aaaaaa; border-left-color: #aaaaaa; }\n"
    "mark { background-color: #ffff00; color: #000000; }\n"
    "small { font-size: 0.85em; }\n"
    "sub, sup { font-size: 0.75em; }\n"
    "table { display: block; margin: 0.6em 0; }\n"
    "caption { display: block; font-weight: bold; padding-bottom: 4px; "
    "text-align: center; }\n"
    "tbody, thead, tfoot, colgroup, col { display: block; }\n"
    "tr { display: block; padding: 2px 0; }\n"
    "td, th { display: inline; padding: 2px 8px; text-align: left; }\n"
    "th { font-weight: bold; text-align: center; background-color: #f0f0f0; }\n"
    "table[border] td, table[border] th { "
    "border-top-width: 1px; border-right-width: 1px; "
    "border-bottom-width: 1px; border-left-width: 1px; "
    "border-top-color: #888888; border-right-color: #888888; "
    "border-bottom-color: #888888; border-left-color: #888888; }\n"
    "table[border=\"0\"], table[border=\"0\"] td, table[border=\"0\"] th { "
    "border-top-width: 0; border-right-width: 0; "
    "border-bottom-width: 0; border-left-width: 0; }\n"
    "img { display: inline; }\n"
    "figure { margin: 0.6em 24px; }\n"
    "figcaption { font-style: italic; font-size: 0.9em; text-align: center; }\n"
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
    "head, script, style, title, meta, link, noscript { display: none; }\n"
    "input[type=\"hidden\"] { display: none; }\n"
    "video { display: block; }\n"
    "svg, canvas, iframe, object, embed, audio, source, track, param { display: none; }\n"
    "noframes, frame, frameset, applet, basefont, marquee, "
    "noembed, isindex, xmp, plaintext { display: none; }\n"
    "details, summary { display: block; }\n"
    "summary { list-style-type: none; }\n"
    "details p, details div, details ul, details ol, details pre, "
    "details blockquote, details table, details section, details article, "
    "details h1, details h2, details h3, details h4, details h5, details h6, "
    "details figure, details dl, details address { margin-left: 16px; }\n"
    "dialog { display: none; }\n"
    "dialog[open] { display: block; margin: auto; padding: 16px; "
    "border: 1px solid #888; }\n"
    "summary { font-weight: bold; cursor: pointer; }\n"
    "picture { display: inline; }\n"
    "[hidden] { display: none; }\n"
    "template { display: none; }\n";

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
    case ND_CSS_UNIT_NUMBER:  return fs->u.length.v;
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

static gboolean
value_is_inherit(const nd_css_value *v)
{
    return v && v->kind == ND_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "inherit") == 0;
}

static gboolean
value_is_initial(const nd_css_value *v)
{
    return v && v->kind == ND_CSS_V_KEYWORD && v->u.keyword &&
           (strcmp(v->u.keyword, "initial") == 0 ||
            strcmp(v->u.keyword, "unset")   == 0 ||
            strcmp(v->u.keyword, "revert")  == 0);
}

static void
cascade_for(GArray *matches, nd_style *out, const nd_style *parent_style)
{
    g_array_sort(matches, match_cmp);
    for (guint i = 0; i < matches->len; i++) {
        match_entry *m = &g_array_index(matches, match_entry, i);
        nd_css_value_free(out->values[m->prop]);
        out->values[m->prop] = nd_css_value_dup(m->value);
    }
    for (int i = 0; i < ND_CSS_PROP_COUNT; i++) {
        if (value_is_inherit(out->values[i])) {
            nd_css_value_free(out->values[i]);
            out->values[i] = parent_style && parent_style->values[i]
                             ? nd_css_value_dup(parent_style->values[i])
                             : NULL;
        } else if (value_is_initial(out->values[i])) {
            nd_css_value_free(out->values[i]);
            out->values[i] = NULL;
        }
    }
    if (parent_style) {
        for (int i = 0; i < ND_CSS_PROP_COUNT; i++) {
            if (out->values[i]) continue;
            if (!prop_inherits((nd_css_prop)i)) continue;
            if (parent_style->values[i])
                out->values[i] = nd_css_value_dup(parent_style->values[i]);
        }
    }
    {
        const nd_css_prop color_props[] = {
            ND_CSS_BACKGROUND_COLOR,
            ND_CSS_BORDER_TOP_COLOR, ND_CSS_BORDER_RIGHT_COLOR,
            ND_CSS_BORDER_BOTTOM_COLOR, ND_CSS_BORDER_LEFT_COLOR,
        };
        for (gsize i = 0; i < G_N_ELEMENTS(color_props); i++) {
            nd_css_value *v = out->values[color_props[i]];
            if (!v || v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) continue;
            if (strcmp(v->u.keyword, "currentcolor") != 0) continue;
            nd_css_value_free(out->values[color_props[i]]);
            out->values[color_props[i]] = out->values[ND_CSS_COLOR]
                ? nd_css_value_dup(out->values[ND_CSS_COLOR])
                : NULL;
        }
    }
    resolve_em_units(out, parent_style);
}

static gboolean
attr_is_color(const char *v, guint8 *r_out, guint8 *g_out, guint8 *b_out, guint8 *a_out)
{
    if (!v || !*v) return FALSE;
    if (parse_color(v, r_out, g_out, b_out, a_out)) return TRUE;
    if (v[0] != '#' && g_ascii_isxdigit(v[0])) {
        char *with_hash = g_strconcat("#", v, NULL);
        gboolean ok = parse_color(with_hash, r_out, g_out, b_out, a_out);
        g_free(with_hash);
        if (ok) return TRUE;
    }
    return FALSE;
}

static char *
presentational_hints_css(const nd_node *el)
{
    if (!el || el->kind != ND_NODE_ELEMENT || !el->name) return NULL;
    GString *out = g_string_new(NULL);
    const char *tag = el->name;
    gboolean is_table = strcmp(tag, "table") == 0;
    gboolean is_cell  = strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0;
    gboolean is_row   = strcmp(tag, "tr") == 0;
    gboolean is_img   = strcmp(tag, "img") == 0;
    gboolean is_hr    = strcmp(tag, "hr") == 0;
    gboolean is_body  = strcmp(tag, "body") == 0;
    gboolean is_font  = strcmp(tag, "font") == 0;
    gboolean is_marq  = strcmp(tag, "marquee") == 0;

    const char *bgcolor = nd_element_get_attr(el, "bgcolor");
    if (bgcolor && *bgcolor) {
        guint8 r, g, b, a;
        if (attr_is_color(bgcolor, &r, &g, &b, &a))
            g_string_append_printf(out, "background-color: rgba(%u,%u,%u,%g);",
                                   r, g, b, a / 255.0);
    }
    if (is_body) {
        const char *text = nd_element_get_attr(el, "text");
        if (text && *text) {
            guint8 r, g, b, a;
            if (attr_is_color(text, &r, &g, &b, &a))
                g_string_append_printf(out, "color: rgba(%u,%u,%u,%g);",
                                       r, g, b, a / 255.0);
        }
    }
    if (is_font) {
        const char *color = nd_element_get_attr(el, "color");
        if (color && *color) {
            guint8 r, g, b, a;
            if (attr_is_color(color, &r, &g, &b, &a))
                g_string_append_printf(out, "color: rgba(%u,%u,%u,%g);",
                                       r, g, b, a / 255.0);
        }
        const char *face = nd_element_get_attr(el, "face");
        if (face && *face)
            g_string_append_printf(out, "font-family: %s;", face);
        const char *size = nd_element_get_attr(el, "size");
        if (size && *size) {
            int n = atoi(size);
            if (n >= 1 && n <= 7) {
                static const double map[] = { 0.63, 0.82, 1.0, 1.13, 1.5, 2.0, 3.0 };
                g_string_append_printf(out, "font-size: %.2fem;", map[n - 1]);
            }
        }
    }

    const char *width = nd_element_get_attr(el, "width");
    if (width && *width && (is_table || is_cell || is_img || is_hr ||
                            strcmp(tag, "col") == 0 ||
                            strcmp(tag, "colgroup") == 0 ||
                            strcmp(tag, "iframe") == 0 ||
                            strcmp(tag, "video") == 0 ||
                            strcmp(tag, "canvas") == 0 ||
                            strcmp(tag, "object") == 0 ||
                            strcmp(tag, "embed") == 0 ||
                            strcmp(tag, "pre") == 0)) {
        char *end = NULL;
        double v = g_ascii_strtod(width, &end);
        if (end && end != width) {
            if (*end == '%')
                g_string_append_printf(out, "width: %g%%;", v);
            else
                g_string_append_printf(out, "width: %gpx;", v);
        }
    }
    const char *height = nd_element_get_attr(el, "height");
    if (height && *height && (is_table || is_cell || is_img || is_row ||
                              strcmp(tag, "iframe") == 0 ||
                              strcmp(tag, "video") == 0 ||
                              strcmp(tag, "canvas") == 0 ||
                              strcmp(tag, "object") == 0 ||
                              strcmp(tag, "embed") == 0)) {
        char *end = NULL;
        double v = g_ascii_strtod(height, &end);
        if (end && end != height) {
            if (*end == '%')
                g_string_append_printf(out, "height: %g%%;", v);
            else
                g_string_append_printf(out, "height: %gpx;", v);
        }
    }
    if (is_table) {
        const char *border = nd_element_get_attr(el, "border");
        if (border && *border) {
            int w = atoi(border);
            if (w < 0) w = 0;
            if (w > 0) {
                g_string_append_printf(out,
                    "border: %dpx solid #888;", w);
            }
        }
        const char *cellspacing = nd_element_get_attr(el, "cellspacing");
        if (cellspacing) {
            int v = atoi(cellspacing);
            g_string_append_printf(out, "border-spacing: %dpx;", v);
        }
    }
    if (is_cell) {
        const char *align = nd_element_get_attr(el, "align");
        if (align && *align) {
            char *lo = g_ascii_strdown(align, -1);
            if (strcmp(lo, "left") == 0 || strcmp(lo, "center") == 0 ||
                strcmp(lo, "right") == 0 || strcmp(lo, "justify") == 0)
                g_string_append_printf(out, "text-align: %s;", lo);
            g_free(lo);
        }
        const char *valign = nd_element_get_attr(el, "valign");
        if (valign && *valign) {
            char *lo = g_ascii_strdown(valign, -1);
            if (strcmp(lo, "top") == 0 || strcmp(lo, "middle") == 0 ||
                strcmp(lo, "bottom") == 0 || strcmp(lo, "baseline") == 0) {
                const char *css = strcmp(lo, "middle") == 0 ? "middle" : lo;
                g_string_append_printf(out, "vertical-align: %s;", css);
            }
            g_free(lo);
        }
    }
    if (strcmp(tag, "p") == 0 ||
        strcmp(tag, "div") == 0 ||
        strcmp(tag, "h1") == 0 || strcmp(tag, "h2") == 0 ||
        strcmp(tag, "h3") == 0 || strcmp(tag, "h4") == 0 ||
        strcmp(tag, "h5") == 0 || strcmp(tag, "h6") == 0 ||
        is_table) {
        const char *align = nd_element_get_attr(el, "align");
        if (align && *align) {
            char *lo = g_ascii_strdown(align, -1);
            if (is_table && (strcmp(lo, "left") == 0 ||
                             strcmp(lo, "right") == 0))
                g_string_append_printf(out, "float: %s;", lo);
            else if (strcmp(lo, "left") == 0 || strcmp(lo, "center") == 0 ||
                     strcmp(lo, "right") == 0 || strcmp(lo, "justify") == 0)
                g_string_append_printf(out, "text-align: %s;", lo);
            g_free(lo);
        }
    }
    if (is_img) {
        const char *align = nd_element_get_attr(el, "align");
        if (align && *align) {
            char *lo = g_ascii_strdown(align, -1);
            if (strcmp(lo, "left") == 0 || strcmp(lo, "right") == 0)
                g_string_append_printf(out, "float: %s;", lo);
            g_free(lo);
        }
        const char *hspace = nd_element_get_attr(el, "hspace");
        if (hspace && *hspace) {
            int v = atoi(hspace);
            g_string_append_printf(out, "margin-left: %dpx; margin-right: %dpx;", v, v);
        }
        const char *vspace = nd_element_get_attr(el, "vspace");
        if (vspace && *vspace) {
            int v = atoi(vspace);
            g_string_append_printf(out, "margin-top: %dpx; margin-bottom: %dpx;", v, v);
        }
        const char *iborder = nd_element_get_attr(el, "border");
        if (iborder && *iborder) {
            int v = atoi(iborder);
            if (v > 0)
                g_string_append_printf(out, "border: %dpx solid;", v);
        }
    }
    if (is_hr) {
        const char *align = nd_element_get_attr(el, "align");
        if (align && *align) {
            char *lo = g_ascii_strdown(align, -1);
            if (strcmp(lo, "center") == 0)
                g_string_append(out, "margin-left: auto; margin-right: auto;");
            else if (strcmp(lo, "left") == 0)
                g_string_append(out, "margin-left: 0; margin-right: auto;");
            else if (strcmp(lo, "right") == 0)
                g_string_append(out, "margin-left: auto; margin-right: 0;");
            g_free(lo);
        }
        const char *color = nd_element_get_attr(el, "color");
        if (color && *color) {
            guint8 r, g, b, a;
            if (attr_is_color(color, &r, &g, &b, &a))
                g_string_append_printf(out, "color: rgba(%u,%u,%u,%g);",
                                       r, g, b, a / 255.0);
        }
        const char *size = nd_element_get_attr(el, "size");
        if (size && *size) {
            int v = atoi(size);
            if (v > 0) g_string_append_printf(out, "height: %dpx;", v);
        }
    }
    (void)is_marq;

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

static void
cascade_walk(nd_node *node,
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

        char *pres_css = presentational_hints_css(node);
        nd_css_stylesheet *pres_sheet = NULL;
        if (pres_css) {
            char *wrapped = g_strconcat("* { ", pres_css, " }", NULL);
            pres_sheet = nd_css_stylesheet_parse(wrapped, -1);
            g_free(wrapped);
            g_free(pres_css);
            for (guint ri = 0; ri < pres_sheet->rules->len; ri++) {
                nd_css_rule *r = g_ptr_array_index(pres_sheet->rules, ri);
                for (guint di = 0; di < r->decls->len; di++) {
                    nd_css_decl *d = &g_array_index(r->decls, nd_css_decl, di);
                    match_entry e = {
                        .origin = 1,
                        .spec_a = 0, .spec_b = 0, .spec_c = 0,
                        .source_order = INT_MIN,
                        .decl_order = (int)di,
                        .important = d->important,
                        .value = d->value,
                        .prop  = d->prop,
                    };
                    g_array_append_val(matches, e);
                }
            }
        }

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

        cascade_for(matches, s, parent_style);
        g_array_free(matches, TRUE);
        if (inline_sheet) nd_css_stylesheet_free(inline_sheet);
        if (pres_sheet) nd_css_stylesheet_free(pres_sheet);
        g_hash_table_insert(out, node, s);
        child_parent_style = s;
    }
    for (nd_node *c = node->first_child; c; c = c->next_sibling)
        cascade_walk(c, ua, author, n_author, child_parent_style, out);
}

static void
append_text_children(const nd_node *n, GString *out)
{
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == ND_NODE_TEXT && c->text)
            g_string_append(out, c->text);
        else if (c->kind == ND_NODE_ELEMENT)
            append_text_children(c, out);
    }
}

void
nd_collect_inline_stylesheets(nd_node *doc, GPtrArray *out)
{
    if (!doc || !out) return;
    GQueue queue = G_QUEUE_INIT;
    g_queue_push_tail(&queue, doc);
    while (!g_queue_is_empty(&queue)) {
        nd_node *n = g_queue_pop_head(&queue);
        if (n->kind == ND_NODE_ELEMENT && n->name &&
            strcmp(n->name, "style") == 0) {
            GString *buf = g_string_new(NULL);
            append_text_children(n, buf);
            if (buf->len > 0) {
                nd_css_stylesheet *sh = nd_css_stylesheet_parse(buf->str, (gssize)buf->len);
                g_ptr_array_add(out, sh);
            }
            g_string_free(buf, TRUE);
        }
        for (nd_node *c = n->first_child; c; c = c->next_sibling)
            g_queue_push_tail(&queue, c);
    }
}

GHashTable *
nd_css_compute(nd_node *doc,
               const nd_css_stylesheet *const *author_sheets,
               gsize n_sheets)
{
    GHashTable *out = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, (GDestroyNotify)nd_style_free);
    const nd_config *cfg = nd_config_get();
    gboolean js_on = !cfg || cfg->javascript_enabled;
    char *full_ua = g_strconcat(kUa,
        js_on ? "" : "noscript { display: block; }\n",
        NULL);
    nd_css_stylesheet *ua = nd_css_stylesheet_parse(full_ua, -1);
    g_free(full_ua);
    cascade_walk(doc, ua, author_sheets, n_sheets, NULL, out);
    nd_css_stylesheet_free(ua);
    return out;
}
