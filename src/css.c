/* Nordstjernen — CSS parser, selectors, cascade.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "css.h"

#include "config.h"
#include "net.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static double g_viewport_w = 1000;
static double g_viewport_h = 800;

static GHashTable *g_defined_elements;

void
nd_css_register_defined_element(const char *tag)
{
    if (!tag || !*tag) return;
    if (!g_defined_elements)
        g_defined_elements = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);
    char *lower = g_ascii_strdown(tag, -1);
    if (g_hash_table_contains(g_defined_elements, lower)) g_free(lower);
    else g_hash_table_add(g_defined_elements, lower);
}

void
nd_css_clear_defined_elements(void)
{
    if (g_defined_elements) {
        g_hash_table_destroy(g_defined_elements);
        g_defined_elements = NULL;
    }
}

static gboolean
nd_css_is_defined_element(const char *tag)
{
    if (!g_defined_elements || !tag) return FALSE;
    char *lower = g_ascii_strdown(tag, -1);
    gboolean ok = g_hash_table_contains(g_defined_elements, lower);
    g_free(lower);
    return ok;
}

void
nd_css_set_viewport(double vw_px, double vh_px)
{
    if (vw_px > 0) g_viewport_w = vw_px;
    if (vh_px > 0) g_viewport_h = vh_px;
}

double nd_css_viewport_w(void) { return g_viewport_w; }
double nd_css_viewport_h(void) { return g_viewport_h; }

static __thread double g_cq_unit_w = 0;
static __thread double g_cq_unit_h = 0;

void
nd_css_set_container_dims(double inline_px, double block_px)
{
    g_cq_unit_w = inline_px;
    g_cq_unit_h = block_px;
}

double nd_css_container_w(void) { return g_cq_unit_w; }
double nd_css_container_h(void) { return g_cq_unit_h; }

static double
viewport_resolve(double v, nd_css_unit unit)
{
    switch (unit) {
    case ND_CSS_UNIT_VW:  return v * g_viewport_w / 100.0;
    case ND_CSS_UNIT_VH:  return v * g_viewport_h / 100.0;
    case ND_CSS_UNIT_VMIN: {
        double m = g_viewport_w < g_viewport_h ? g_viewport_w : g_viewport_h;
        return v * m / 100.0;
    }
    case ND_CSS_UNIT_VMAX: {
        double m = g_viewport_w > g_viewport_h ? g_viewport_w : g_viewport_h;
        return v * m / 100.0;
    }
    default: return 0;
    }
}

static char *g_target_fragment = NULL;

void
nd_css_set_target_fragment(const char *fragment)
{
    g_free(g_target_fragment);
    g_target_fragment = (fragment && *fragment) ? g_strdup(fragment) : NULL;
}

static const nd_node *g_css_focus_node = NULL;

const nd_node *
nd_css_set_focus_node(const nd_node *node)
{
    const nd_node *prev = g_css_focus_node;
    g_css_focus_node = node;
    return prev;
}

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
    [ND_CSS_POINTER_EVENTS]       = "pointer-events",
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
    [ND_CSS_OVERFLOW_X]           = "overflow-x",
    [ND_CSS_OVERFLOW_Y]           = "overflow-y",
    [ND_CSS_FONT_VARIANT]         = "font-variant",
    [ND_CSS_BORDER_RADIUS]            = "border-radius",
    [ND_CSS_BORDER_TOP_LEFT_RADIUS]     = "border-top-left-radius",
    [ND_CSS_BORDER_TOP_RIGHT_RADIUS]    = "border-top-right-radius",
    [ND_CSS_BORDER_BOTTOM_RIGHT_RADIUS] = "border-bottom-right-radius",
    [ND_CSS_BORDER_BOTTOM_LEFT_RADIUS]  = "border-bottom-left-radius",
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
    [ND_CSS_BOX_SHADOW]           = "box-shadow",
    [ND_CSS_OUTLINE_WIDTH]        = "outline-width",
    [ND_CSS_OUTLINE_STYLE]        = "outline-style",
    [ND_CSS_OUTLINE_COLOR]        = "outline-color",
    [ND_CSS_OUTLINE_OFFSET]       = "outline-offset",
    [ND_CSS_BACKGROUND_IMAGE]     = "background-image",
    [ND_CSS_BACKGROUND_REPEAT]    = "background-repeat",
    [ND_CSS_BACKGROUND_POSITION_X]= "background-position-x",
    [ND_CSS_BACKGROUND_POSITION_Y]= "background-position-y",
    [ND_CSS_BACKGROUND_SIZE]      = "background-size",
    [ND_CSS_BACKGROUND_CLIP]      = "background-clip",
    [ND_CSS_SCROLLBAR_WIDTH]      = "scrollbar-width",
    [ND_CSS_SCROLLBAR_COLOR]      = "scrollbar-color",
    [ND_CSS_IMAGE_RENDERING]      = "image-rendering",
    [ND_CSS_CONTENT]              = "content",
    [ND_CSS_GRID_TEMPLATE_COLUMNS]= "grid-template-columns",
    [ND_CSS_GRID_TEMPLATE_ROWS]   = "grid-template-rows",
    [ND_CSS_GRID_TEMPLATE_AREAS]  = "grid-template-areas",
    [ND_CSS_GRID_COLUMN]          = "grid-column",
    [ND_CSS_GRID_ROW]             = "grid-row",
    [ND_CSS_GRID_COLUMN_START]    = "grid-column-start",
    [ND_CSS_GRID_COLUMN_END]      = "grid-column-end",
    [ND_CSS_GRID_ROW_START]       = "grid-row-start",
    [ND_CSS_GRID_ROW_END]         = "grid-row-end",
    [ND_CSS_GRID_AREA]            = "grid-area",
    [ND_CSS_GRID_AUTO_ROWS]       = "grid-auto-rows",
    [ND_CSS_TRANSFORM]            = "transform",
    [ND_CSS_TRANSFORM_ORIGIN]     = "transform-origin",
    [ND_CSS_TRANSITION]           = "transition",
    [ND_CSS_ANIMATION]            = "animation",
    [ND_CSS_ASPECT_RATIO]         = "aspect-ratio",
    [ND_CSS_TEXT_SHADOW]          = "text-shadow",
    [ND_CSS_OVERFLOW_WRAP]        = "overflow-wrap",
    [ND_CSS_WORD_BREAK]           = "word-break",
    [ND_CSS_TEXT_OVERFLOW]        = "text-overflow",
    [ND_CSS_TEXT_DECORATION_COLOR]= "text-decoration-color",
    [ND_CSS_TEXT_DECORATION_STYLE]= "text-decoration-style",
    [ND_CSS_LIST_STYLE_POSITION]  = "list-style-position",
    [ND_CSS_COLUMN_COUNT]         = "column-count",
    [ND_CSS_COLUMN_WIDTH]         = "column-width",
    [ND_CSS_COLUMN_RULE_WIDTH]    = "column-rule-width",
    [ND_CSS_COLUMN_RULE_STYLE]    = "column-rule-style",
    [ND_CSS_COLUMN_RULE_COLOR]    = "column-rule-color",
    [ND_CSS_FILTER]               = "filter",
    [ND_CSS_CLIP_PATH]            = "clip-path",
    [ND_CSS_MIX_BLEND_MODE]       = "mix-blend-mode",
    [ND_CSS_ACCENT_COLOR]         = "accent-color",
    [ND_CSS_COUNTER_RESET]        = "counter-reset",
    [ND_CSS_COUNTER_INCREMENT]    = "counter-increment",
    [ND_CSS_LINE_CLAMP]           = "-webkit-line-clamp",
    [ND_CSS_OBJECT_FIT]           = "object-fit",
    [ND_CSS_OBJECT_POSITION_X]    = "object-position-x",
    [ND_CSS_OBJECT_POSITION_Y]    = "object-position-y",
    [ND_CSS_MASK_IMAGE]           = "mask-image",
    [ND_CSS_APPEARANCE]           = "appearance",
    [ND_CSS_TABLE_LAYOUT]         = "table-layout",
    [ND_CSS_CAPTION_SIDE]         = "caption-side",
    [ND_CSS_BORDER_COLLAPSE]      = "border-collapse",
    [ND_CSS_BORDER_SPACING]       = "border-spacing",
    [ND_CSS_CONTAINER_TYPE]       = "container-type",
    [ND_CSS_CONTAINER_NAME]       = "container-name",
    [ND_CSS_CARET_COLOR]          = "caret-color",
    [ND_CSS_TAB_SIZE]             = "tab-size",
    [ND_CSS_JUSTIFY_ITEMS]        = "justify-items",
    [ND_CSS_JUSTIFY_SELF]         = "justify-self",
    [ND_CSS_ALIGN_CONTENT]        = "align-content",
};

static gboolean
prop_inherits(nd_css_prop p)
{
    switch (p) {
    case ND_CSS_COLOR:
    case ND_CSS_FONT_SIZE:
    case ND_CSS_FONT_WEIGHT:
    case ND_CSS_FONT_STYLE:
    case ND_CSS_FONT_FAMILY:
    case ND_CSS_FONT_VARIANT:
    case ND_CSS_LINE_HEIGHT:
    case ND_CSS_LETTER_SPACING:
    case ND_CSS_WORD_SPACING:
    case ND_CSS_WHITE_SPACE:
    case ND_CSS_CAPTION_SIDE:
    case ND_CSS_BORDER_COLLAPSE:
    case ND_CSS_BORDER_SPACING:
    case ND_CSS_TEXT_ALIGN:
    case ND_CSS_TEXT_INDENT:
    case ND_CSS_TEXT_TRANSFORM:
    case ND_CSS_LIST_STYLE_TYPE:
    case ND_CSS_LIST_STYLE_POSITION:
    case ND_CSS_VISIBILITY:
    case ND_CSS_CURSOR:
    case ND_CSS_POINTER_EVENTS:
    case ND_CSS_SCROLLBAR_COLOR:
    case ND_CSS_IMAGE_RENDERING:
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

static gunichar
css_unescape_cp(gunichar cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0xFFFD;
    return cp;
}

static void
css_append_hex_escape(GString *out, const char **pp, const char *end)
{
    const char *p = *pp;
    gunichar cp = 0;
    int n = 0;
    while (p < end && n < 6 && g_ascii_isxdigit(*p)) {
        cp = cp * 16 + (gunichar)g_ascii_xdigit_value(*p);
        p++;
        n++;
    }
    if (p < end && is_ws(*p)) p++;
    g_string_append_unichar(out, css_unescape_cp(cp));
    *pp = p;
}

static char *
read_css_ident(const char **pp, const char *end)
{
    GString *out = g_string_new(NULL);
    const char *p = *pp;
    while (p < end) {
        char c = *p;
        if (c == '\\') {
            char esc = p + 1 < end ? p[1] : '\0';
            if (p + 1 >= end || esc == '\n' || esc == '\r' || esc == '\f') {
                g_string_append_unichar(out, 0xFFFD);
                p++;
                break;
            }
            if (g_ascii_isxdigit(esc)) {
                p++;
                css_append_hex_escape(out, &p, end);
                continue;
            }
            g_string_append_c(out, esc);
            p += 2;
            continue;
        }
        if (is_ident(c)) {
            g_string_append_c(out, c);
            p++;
            continue;
        }
        break;
    }
    *pp = p;
    return g_string_free(out, FALSE);
}

static char *
read_css_string(const char **pp, const char *end)
{
    const char *p = *pp;
    if (p >= end || (*p != '"' && *p != '\'')) return g_strdup("");
    char quote = *p++;
    GString *out = g_string_new(NULL);
    while (p < end) {
        char c = *p;
        if (c == quote) {
            p++;
            break;
        }
        if (c == '\n' || c == '\r' || c == '\f') break;
        if (c == '\\' && p + 1 < end) {
            char esc = p[1];
            if (esc == '\n' || esc == '\r' || esc == '\f') {
                p += 2;
                continue;
            }
            if (g_ascii_isxdigit(esc)) {
                p++;
                css_append_hex_escape(out, &p, end);
                continue;
            }
            g_string_append_c(out, esc);
            p += 2;
            continue;
        }
        g_string_append_c(out, c);
        p++;
    }
    *pp = p;
    return g_string_free(out, FALSE);
}

static const char *css_skip_ws_comments(const char *p, const char *end);
static const char *css_scan_until(const char *p, const char *end,
                                  const char *terminators, char *terminator);
static const char *css_scan_segment(const char *p, const char *end,
                                    char *terminator);
static const char *css_scan_declaration_value(const char *p, const char *end,
                                              char *terminator);
static const char *css_skip_to_block_end(const char *p, const char *end);
static const char *css_block_body_end(const char *body_start,
                                      const char *block_end);
static const char *css_find_top_level_char(const char *p, const char *end,
                                           char needle);
static const char *css_find_function(const char *p, const char *end,
                                     const char *name);
static const char *css_skip_comment(const char *p, const char *end);
static void css_strip_important(char *text, gboolean *important);
static char *css_trim_dup_range(const char *start, const char *end);
static int split_ws_limit(const char *s, char *out[], int max);
static int calc_split_args(const char *args, const char *body_end,
                           char *out[], int max);
static const char *match_close_paren(const char *p, const char *end);
static gboolean parse_color(const char *s, guint8 *r, guint8 *g, guint8 *b,
                            guint8 *a);

static char *
ascii_lower(const char *s, gsize len)
{
    if (len == G_MAXSIZE) return g_strdup("");
    char *r = g_malloc(len + 1);
    for (gsize i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        r[i] = c;
    }
    r[len] = '\0';
    return r;
}

static gboolean
css_wide_keyword_is(const char *kw)
{
    return strcmp(kw, "inherit") == 0 ||
           strcmp(kw, "initial") == 0 ||
           strcmp(kw, "unset") == 0 ||
           strcmp(kw, "revert") == 0 ||
           strcmp(kw, "revert-layer") == 0;
}

static nd_css_value *
parse_css_wide_keyword(const char *text)
{
    char *kw = ascii_lower(text, strlen(text));
    if (!css_wide_keyword_is(kw)) {
        g_free(kw);
        return NULL;
    }
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_KEYWORD;
    v->u.keyword = kw;
    return v;
}

static nd_css_value *
nd_css_value_dup(const nd_css_value *v)
{
    if (!v) return NULL;
    ((nd_css_value *)v)->ref++;
    return (nd_css_value *)v;
}

static void
nd_css_value_free(nd_css_value *v)
{
    if (!v) return;
    if (v->ref > 0) { v->ref--; return; }
    if (v->kind == ND_CSS_V_KEYWORD) g_free(v->u.keyword);
    else if (v->kind == ND_CSS_V_URL) g_free(v->u.url);
    else if (v->kind == ND_CSS_V_AREAS) {
        for (int i = 0; i < v->u.areas.n_rects; i++)
            g_free(v->u.areas.rects[i].name);
    }
    else if (v->kind == ND_CSS_V_ANIM) {
        for (int i = 0; i < v->u.anim.n; i++)
            g_free(v->u.anim.entries[i].name);
    }
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

static double
column_len_px(const nd_css_value *v, double basis, double fallback)
{
    if (!v) return fallback;
    if (v->kind == ND_CSS_V_CALC)
        return v->u.calc.pct / 100.0 * basis + v->u.calc.px;
    if (v->kind != ND_CSS_V_LENGTH) return fallback;
    switch (v->u.length.unit) {
    case ND_CSS_UNIT_PX:
    case ND_CSS_UNIT_NUMBER: return v->u.length.v;
    case ND_CSS_UNIT_EM:
    case ND_CSS_UNIT_REM:    return v->u.length.v * 16.0;
    case ND_CSS_UNIT_PERCENT: return v->u.length.v * basis / 100.0;
    case ND_CSS_UNIT_VW:     return v->u.length.v * nd_css_viewport_w() / 100.0;
    case ND_CSS_UNIT_VH:     return v->u.length.v * nd_css_viewport_h() / 100.0;
    default:                 return fallback;
    }
}

int
nd_css_used_column_count(const nd_style *s, double avail_w, double *out_gap)
{
    double gap = 16.0;
    if (s) {
        const nd_css_value *cg = s->values[ND_CSS_COLUMN_GAP];
        if (!cg || cg->kind != ND_CSS_V_LENGTH)
            cg = s->values[ND_CSS_GAP];
        if (cg) {
            double g = column_len_px(cg, avail_w, -1);
            if (g >= 0) gap = g;
        }
    }
    if (out_gap) *out_gap = gap;
    int n = 1;
    if (s && s->values[ND_CSS_COLUMN_COUNT] &&
        s->values[ND_CSS_COLUMN_COUNT]->kind == ND_CSS_V_LENGTH) {
        double v = s->values[ND_CSS_COLUMN_COUNT]->u.length.v;
        if (v >= 2) n = (int)(v + 0.5);
    }
    if (n == 1 && s && s->values[ND_CSS_COLUMN_WIDTH] &&
        s->values[ND_CSS_COLUMN_WIDTH]->kind == ND_CSS_V_LENGTH) {
        double colw = column_len_px(s->values[ND_CSS_COLUMN_WIDTH], avail_w, 0);
        if (colw > 1 && avail_w > colw + gap) {
            int fit = (int)((avail_w + gap) / (colw + gap));
            if (fit > 1) n = fit;
        }
    }
    return n;
}

gboolean
nd_css_keyword_is(const nd_css_value *v, const char *kw)
{
    return v && v->kind == ND_CSS_V_KEYWORD && kw &&
           v->u.keyword && strcmp(v->u.keyword, kw) == 0;
}

static char *
font_family_token_clean(const char *start, gsize len)
{
    while (len > 0 && is_ws(*start)) {
        start++;
        len--;
    }
    while (len > 0 && is_ws(start[len - 1])) len--;
    if (len >= 2 &&
        ((start[0] == '"' && start[len - 1] == '"') ||
         (start[0] == '\'' && start[len - 1] == '\''))) {
        start++;
        len -= 2;
    }
    GString *out = g_string_new(NULL);
    gboolean pending_space = FALSE;
    for (gsize i = 0; i < len; i++) {
        char c = start[i];
        if (c == '\\' && i + 1 < len) {
            i++;
            c = start[i];
        }
        if (is_ws(c)) {
            pending_space = out->len > 0;
            continue;
        }
        if (pending_space) {
            g_string_append_c(out, ' ');
            pending_space = FALSE;
        }
        g_string_append_c(out, c);
    }
    char *ret = g_string_free(out, FALSE);
    g_strstrip(ret);
    return ret;
}

static char *
font_family_map_generic(const char *token)
{
    char *lo = g_ascii_strdown(token, -1);
    char *ret = NULL;
    if (strcmp(lo, "system-ui") == 0 ||
        strcmp(lo, "ui-sans-serif") == 0 ||
        strcmp(lo, "sans-serif") == 0 ||
        strcmp(lo, "arial") == 0 ||
        strcmp(lo, "helvetica") == 0 ||
        strcmp(lo, "segoe ui") == 0 ||
        g_str_has_prefix(lo, "roboto") ||
        g_str_has_prefix(lo, "sf pro") ||
        g_str_has_prefix(lo, "sfpro") ||
        g_str_has_prefix(lo, "optimistic text"))
        ret = g_strdup("sans-serif");
    else if (strcmp(lo, "ui-serif") == 0 ||
             strcmp(lo, "serif") == 0)
        ret = g_strdup("serif");
    else if (strcmp(lo, "ui-monospace") == 0 ||
             strcmp(lo, "monospace") == 0)
        ret = g_strdup("monospace");
    else if (strcmp(lo, "cursive") == 0 ||
             strcmp(lo, "fantasy") == 0 ||
             strcmp(lo, "emoji") == 0 ||
             strcmp(lo, "math") == 0 ||
             strcmp(lo, "fangsong") == 0)
        ret = g_strdup(lo);
    g_free(lo);
    return ret;
}

static gboolean (*g_font_available_cb)(const char *family);

void
nd_css_set_font_available_cb(gboolean (*cb)(const char *family))
{
    g_font_available_cb = cb;
}

char *
nd_css_font_family_for_pango(const char *css_family)
{
    if (!css_family || !*css_family) return g_strdup("sans-serif");
    char *fallback = NULL;
    const char *p = css_family;
    while (*p) {
        while (*p == ',') p++;
        const char *start = p;
        char quote = 0;
        while (*p) {
            if (quote) {
                if (*p == '\\' && p[1]) p++;
                else if (*p == quote) quote = 0;
            } else if (*p == '"' || *p == '\'') {
                quote = *p;
            } else if (*p == ',') {
                break;
            }
            p++;
        }
        char *token = font_family_token_clean(start, (gsize)(p - start));
        if (token && *token) {
            char *lo = g_ascii_strdown(token, -1);
            gboolean skip = strcmp(lo, "inherit") == 0 ||
                            strcmp(lo, "initial") == 0 ||
                            strcmp(lo, "unset") == 0 ||
                            strcmp(lo, "revert") == 0 ||
                            strcmp(lo, "revert-layer") == 0 ||
                            strstr(lo, "linux libertine") != NULL ||
                            g_str_has_prefix(lo, "libertinus") ||
                            g_str_has_prefix(lo, "var(");
            gboolean system_alias = strcmp(lo, "-apple-system") == 0 ||
                                    strcmp(lo, "blinkmacsystemfont") == 0;
            g_free(lo);
            if (system_alias) {
                if (!fallback) fallback = g_strdup("sans-serif");
            } else if (!skip) {
                char *mapped = font_family_map_generic(token);
                if (mapped) {
                    g_free(token);
                    g_free(fallback);
                    return mapped;
                }
                if (g_font_available_cb && !g_font_available_cb(token)) {
                    if (!fallback) fallback = g_strdup("sans-serif");
                } else {
                    g_free(fallback);
                    return token;
                }
            }
        }
        g_free(token);
        if (*p == ',') p++;
    }
    return fallback ? fallback : g_strdup("sans-serif");
}

int
nd_css_font_weight_number(const nd_css_value *v, int fallback)
{
    if (!v || v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) return fallback;
    const char *kw = v->u.keyword;
    if (strcmp(kw, "normal") == 0) return 400;
    if (strcmp(kw, "bold") == 0) return 700;
    if (strcmp(kw, "bolder") == 0) {
        int base = fallback > 0 ? fallback : 400;
        if (base < 400) return 400;
        if (base < 700) return 700;
        return 900;
    }
    if (strcmp(kw, "lighter") == 0) {
        int base = fallback > 0 ? fallback : 400;
        if (base > 700) return 700;
        if (base > 400) return 400;
        return 300;
    }
    if (g_ascii_isdigit(kw[0])) {
        int n = nd_parse_int(kw, fallback > 0 ? fallback : 400, 1, 1000);
        if (n < 100) n = 100;
        n = ((n + 50) / 100) * 100;
        if (n < 100) return 100;
        if (n > 1000) return 1000;
        return n;
    }
    return fallback;
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
    gboolean is_percent[4] = { FALSE, FALSE, FALSE, FALSE };
    int count = 0;
    while (*p && *p != ')' && count < 4) {
        while (*p == ' ' || *p == ',' || *p == '/') p++;
        if (!*p || *p == ')') break;
        if (g_ascii_strncasecmp(p, "none", 4) == 0 &&
            !is_ident(p[4])) {
            values[count] = count == 3 ? 1.0 : 0.0;
            count++;
            p += 4;
            continue;
        }
        char *end = NULL;
        double v = g_ascii_strtod(p, &end);
        if (!end || end == p) return FALSE;
        if (*end == '%') { is_percent[count] = TRUE; end++; }
        values[count++] = v;
        p = end;
    }
    if (count < 3) return FALSE;
    double rgb_scaled[3];
    for (int i = 0; i < 3; i++)
        rgb_scaled[i] = is_percent[i] ? values[i] * 255.0 / 100.0 : values[i];
    *r = (guint8)CLAMP((int)(rgb_scaled[0] + 0.5), 0, 255);
    *g = (guint8)CLAMP((int)(rgb_scaled[1] + 0.5), 0, 255);
    *b = (guint8)CLAMP((int)(rgb_scaled[2] + 0.5), 0, 255);
    if (count == 4) {
        double alpha = is_percent[3] ? values[3] / 100.0 : values[3];
        *a = (guint8)CLAMP((int)(alpha * 255 + 0.5), 0, 255);
    } else {
        *a = 255;
    }
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

static double
css_angle_value_degrees(double v, char **endp)
{
    char *end = *endp;
    if (g_ascii_strncasecmp(end, "deg", 3) == 0 && !is_ident(end[3])) {
        *endp = end + 3;
    } else if (g_ascii_strncasecmp(end, "turn", 4) == 0 &&
               !is_ident(end[4])) {
        v *= 360.0;
        *endp = end + 4;
    } else if (g_ascii_strncasecmp(end, "grad", 4) == 0 &&
               !is_ident(end[4])) {
        v *= 0.9;
        *endp = end + 4;
    } else if (g_ascii_strncasecmp(end, "rad", 3) == 0 &&
               !is_ident(end[3])) {
        v = v * 180.0 / G_PI;
        *endp = end + 3;
    }
    return v;
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
        if (g_ascii_strncasecmp(p, "none", 4) == 0 &&
            !is_ident(p[4])) {
            values[count] = count == 3 ? 1.0 : 0.0;
            count++;
            p += 4;
            continue;
        }
        char *end = NULL;
        double v = g_ascii_strtod(p, &end);
        if (!end || end == p) return FALSE;
        if (count == 0) {
            v = css_angle_value_degrees(v, &end);
            if (is_ident(*end)) return FALSE;
        } else if (*end == '%') {
            end++;
        }
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
    if (count >= 4) {
        double alpha = values[3];
        if (alpha > 1) alpha /= 100.0;
        *a = (guint8)CLAMP((int)(alpha * 255 + 0.5), 0, 255);
    } else {
        *a = 255;
    }
    return TRUE;
}

static gboolean
parse_hwb_func(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
    if (g_ascii_strncasecmp(s, "hwb(", 4) != 0) return FALSE;
    const char *p = strchr(s, '(');
    if (!p) return FALSE;
    p++;
    double values[4] = { 0, 0, 0, 1 };
    gboolean is_percent[4] = { FALSE, FALSE, FALSE, FALSE };
    int count = 0;
    while (*p && *p != ')' && count < 4) {
        while (*p == ' ' || *p == ',' || *p == '/') p++;
        if (!*p || *p == ')') break;
        if (g_ascii_strncasecmp(p, "none", 4) == 0 &&
            !is_ident(p[4])) {
            values[count] = count == 3 ? 1.0 : 0.0;
            count++;
            p += 4;
            continue;
        }
        char *end = NULL;
        double v = g_ascii_strtod(p, &end);
        if (!end || end == p) return FALSE;
        if (count == 0) {
            v = css_angle_value_degrees(v, &end);
            if (is_ident(*end)) return FALSE;
        } else if (*end == '%') {
            is_percent[count] = TRUE;
            end++;
        } else if (is_ident(*end)) {
            return FALSE;
        }
        values[count++] = v;
        p = end;
    }
    if (count < 3) return FALSE;
    double h = values[0] / 360.0;
    while (h < 0) h += 1.0;
    while (h > 1) h -= 1.0;
    double w = (is_percent[1] ? values[1] : values[1]) / 100.0;
    double bl = (is_percent[2] ? values[2] : values[2]) / 100.0;
    w = CLAMP(w, 0.0, 1.0);
    bl = CLAMP(bl, 0.0, 1.0);
    double rr = hsl_hue_to_rgb(0, 1, h + 1.0/3.0);
    double gg = hsl_hue_to_rgb(0, 1, h);
    double bb = hsl_hue_to_rgb(0, 1, h - 1.0/3.0);
    double sum = w + bl;
    if (sum >= 1.0) {
        rr = gg = bb = sum > 0 ? w / sum : 0;
    } else {
        double scale = 1.0 - w - bl;
        rr = rr * scale + w;
        gg = gg * scale + w;
        bb = bb * scale + w;
    }
    *r = (guint8)CLAMP((int)(rr * 255 + 0.5), 0, 255);
    *g = (guint8)CLAMP((int)(gg * 255 + 0.5), 0, 255);
    *b = (guint8)CLAMP((int)(bb * 255 + 0.5), 0, 255);
    if (count >= 4) {
        double alpha = is_percent[3] ? values[3] / 100.0 : values[3];
        *a = (guint8)CLAMP((int)(alpha * 255 + 0.5), 0, 255);
    } else {
        *a = 255;
    }
    return TRUE;
}

static double
srgb_encode_linear(double c)
{
    if (c <= 0.0031308) return 12.92 * c;
    return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

static void
oklab_to_srgb(double l, double a, double b, guint8 *r, guint8 *g,
              guint8 *bl)
{
    double lp = l + 0.3963377774 * a + 0.2158037573 * b;
    double mp = l - 0.1055613458 * a - 0.0638541728 * b;
    double sp = l - 0.0894841775 * a - 1.2914855480 * b;
    double ll = lp * lp * lp;
    double mm = mp * mp * mp;
    double ss = sp * sp * sp;
    double rr =  4.0767416621 * ll - 3.3077115913 * mm + 0.2309699292 * ss;
    double gg = -1.2684380046 * ll + 2.6097574011 * mm - 0.3413193965 * ss;
    double bb = -0.0041960863 * ll - 0.7034186147 * mm + 1.7076147010 * ss;
    rr = srgb_encode_linear(rr);
    gg = srgb_encode_linear(gg);
    bb = srgb_encode_linear(bb);
    *r = (guint8)CLAMP((int)(rr * 255 + 0.5), 0, 255);
    *g = (guint8)CLAMP((int)(gg * 255 + 0.5), 0, 255);
    *bl = (guint8)CLAMP((int)(bb * 255 + 0.5), 0, 255);
}

static double
lab_inv_f(double t)
{
    double t3 = t * t * t;
    if (t3 > 0.008856451679) return t3;
    return (116.0 * t - 16.0) / 903.2962963;
}

static void
lab_to_srgb(double l, double a, double b, guint8 *r, guint8 *g, guint8 *bl)
{
    double fy = (l + 16.0) / 116.0;
    double fx = fy + a / 500.0;
    double fz = fy - b / 200.0;
    double x50 = 0.96422 * lab_inv_f(fx);
    double y50 = lab_inv_f(fy);
    double z50 = 0.82521 * lab_inv_f(fz);
    double x =  0.9555766 * x50 - 0.0230393 * y50 + 0.0631636 * z50;
    double y = -0.0282895 * x50 + 1.0099416 * y50 + 0.0210077 * z50;
    double z =  0.0122982 * x50 - 0.0204830 * y50 + 1.3299098 * z50;
    double rr =  3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    double gg = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    double bb =  0.0556434 * x - 0.2040259 * y + 1.0572252 * z;
    rr = srgb_encode_linear(rr);
    gg = srgb_encode_linear(gg);
    bb = srgb_encode_linear(bb);
    *r = (guint8)CLAMP((int)(rr * 255 + 0.5), 0, 255);
    *g = (guint8)CLAMP((int)(gg * 255 + 0.5), 0, 255);
    *bl = (guint8)CLAMP((int)(bb * 255 + 0.5), 0, 255);
}

static gboolean
parse_lab_func(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *alpha)
{
    gboolean is_lch = g_ascii_strncasecmp(s, "lch(", 4) == 0;
    gboolean is_lab = !is_lch && g_ascii_strncasecmp(s, "lab(", 4) == 0;
    if (!is_lch && !is_lab) return FALSE;
    if (strchr(s, ',')) return FALSE;
    const char *p = strchr(s, '(');
    if (!p) return FALSE;
    p++;
    double values[4] = { 0, 0, 0, 1 };
    int count = 0;
    while (*p && *p != ')' && count < 4) {
        while (*p == ' ' || *p == '/') p++;
        if (!*p || *p == ')') break;
        if (g_ascii_strncasecmp(p, "none", 4) == 0 &&
            !is_ident(p[4])) {
            values[count] = count == 3 ? 1.0 : 0.0;
            count++;
            p += 4;
            continue;
        }
        char *end = NULL;
        double v = g_ascii_strtod(p, &end);
        if (!end || end == p) return FALSE;
        if (count == 0) {
            if (*end == '%') end++;
        } else if (count == 1) {
            if (*end == '%') {
                v *= 1.25;
                end++;
            }
            if (is_lch && v < 0) v = 0;
        } else if (count == 2) {
            if (is_lch) {
                v = css_angle_value_degrees(v, &end);
                if (is_ident(*end)) return FALSE;
            } else if (*end == '%') {
                v *= 1.25;
                end++;
            }
        } else if (count == 3) {
            if (*end == '%') {
                v /= 100.0;
                end++;
            }
        }
        values[count++] = v;
        p = end;
    }
    if (count < 3) return FALSE;
    double l = CLAMP(values[0], 0.0, 100.0);
    double aa = values[1];
    double bb = values[2];
    if (is_lch) {
        double rad = values[2] * G_PI / 180.0;
        aa = values[1] * cos(rad);
        bb = values[1] * sin(rad);
    }
    lab_to_srgb(l, aa, bb, r, g, b);
    double av = count >= 4 ? values[3] : 1.0;
    *alpha = (guint8)CLAMP((int)(CLAMP(av, 0.0, 1.0) * 255 + 0.5), 0, 255);
    return TRUE;
}

static gboolean
parse_oklab_func(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *alpha)
{
    gboolean is_lch = g_ascii_strncasecmp(s, "oklch(", 6) == 0;
    gboolean is_lab = !is_lch && g_ascii_strncasecmp(s, "oklab(", 6) == 0;
    if (!is_lch && !is_lab) return FALSE;
    if (strchr(s, ',')) return FALSE;
    const char *p = strchr(s, '(');
    if (!p) return FALSE;
    p++;
    double values[4] = { 0, 0, 0, 1 };
    int count = 0;
    while (*p && *p != ')' && count < 4) {
        while (*p == ' ' || *p == '/') p++;
        if (!*p || *p == ')') break;
        if (g_ascii_strncasecmp(p, "none", 4) == 0 &&
            !is_ident(p[4])) {
            values[count] = count == 3 ? 1.0 : 0.0;
            count++;
            p += 4;
            continue;
        }
        char *end = NULL;
        double v = g_ascii_strtod(p, &end);
        if (!end || end == p) return FALSE;
        if (count == 0) {
            if (*end == '%') {
                v /= 100.0;
                end++;
            }
        } else if (count == 1) {
            if (*end == '%') {
                v *= 0.004;
                end++;
            }
            if (is_lch && v < 0) v = 0;
        } else if (count == 2) {
            if (is_lch) {
                v = css_angle_value_degrees(v, &end);
                if (is_ident(*end)) return FALSE;
            } else if (*end == '%') {
                v *= 0.004;
                end++;
            }
        } else if (count == 3) {
            if (*end == '%') {
                v /= 100.0;
                end++;
            }
        }
        values[count++] = v;
        p = end;
    }
    if (count < 3) return FALSE;
    double l = CLAMP(values[0], 0.0, 1.0);
    double aa = values[1];
    double bb = values[2];
    if (is_lch) {
        double rad = values[2] * G_PI / 180.0;
        aa = values[1] * cos(rad);
        bb = values[1] * sin(rad);
    }
    oklab_to_srgb(l, aa, bb, r, g, b);
    double av = count >= 4 ? values[3] : 1.0;
    *alpha = (guint8)CLAMP((int)(CLAMP(av, 0.0, 1.0) * 255 + 0.5), 0, 255);
    return TRUE;
}

static gboolean
color_mix_percent(const char *s, double *out)
{
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (!end || end == s) return FALSE;
    while (*end && is_ws(*end)) end++;
    if (*end != '%') return FALSE;
    end++;
    while (*end && is_ws(*end)) end++;
    if (*end) return FALSE;
    *out = CLAMP(v, 0.0, 100.0);
    return TRUE;
}

static gboolean
parse_color_mix_stop(const char *text, guint8 rgba[4], double *pct,
                     gboolean *has_pct)
{
    *has_pct = FALSE;
    char *tokens[3] = {0};
    int n = split_ws_limit(text, tokens, G_N_ELEMENTS(tokens));
    gboolean ok = FALSE;
    if (n == 1 || n == 2) {
        if (n == 2) {
            if (!color_mix_percent(tokens[1], pct)) goto done;
            *has_pct = TRUE;
        }
        ok = parse_color(tokens[0], &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
    }
done:
    for (int i = 0; i < n; i++) g_free(tokens[i]);
    return ok;
}

static gboolean
parse_color_mix_func(const char *s, guint8 *r, guint8 *g, guint8 *b,
                     guint8 *a)
{
    if (g_ascii_strncasecmp(s, "color-mix(", 10) != 0) return FALSE;
    const char *p = strchr(s, '(');
    if (!p) return FALSE;
    p++;
    const char *end = s + strlen(s);
    const char *body_end = match_close_paren(p, end);
    if (!body_end) return FALSE;
    char *parts[3] = {0};
    int n = calc_split_args(p, body_end, parts, G_N_ELEMENTS(parts));
    if (n != 3) {
        for (int i = 0; i < n; i++) g_free(parts[i]);
        return FALSE;
    }
    char *space = parts[0];
    while (*space && is_ws(*space)) space++;
    gboolean ok = g_ascii_strncasecmp(space, "in", 2) == 0 &&
                  is_ws(space[2]);
    if (ok) {
        space += 2;
        while (*space && is_ws(*space)) space++;
        ok = g_ascii_strncasecmp(space, "srgb", 4) == 0 &&
             (!space[4] || is_ws(space[4]));
    }
    guint8 c1[4] = {0}, c2[4] = {0};
    double p1 = 50, p2 = 50;
    gboolean h1 = FALSE, h2 = FALSE;
    if (ok)
        ok = parse_color_mix_stop(parts[1], c1, &p1, &h1) &&
             parse_color_mix_stop(parts[2], c2, &p2, &h2);
    if (ok) {
        if (h1 && !h2) p2 = 100.0 - p1;
        else if (!h1 && h2) p1 = 100.0 - p2;
        else if (!h1 && !h2) { p1 = 50.0; p2 = 50.0; }
        double sum = p1 + p2;
        if (sum <= 0) ok = FALSE;
        else {
            double w1 = p1 / sum;
            double w2 = p2 / sum;
            double a1 = c1[3] / 255.0;
            double a2 = c2[3] / 255.0;
            double ao = a1 * w1 + a2 * w2;
            double rr = 0, gg = 0, bb = 0;
            if (ao > 0) {
                rr = (c1[0] * a1 * w1 + c2[0] * a2 * w2) / ao;
                gg = (c1[1] * a1 * w1 + c2[1] * a2 * w2) / ao;
                bb = (c1[2] * a1 * w1 + c2[2] * a2 * w2) / ao;
            }
            *r = (guint8)CLAMP((int)(rr + 0.5), 0, 255);
            *g = (guint8)CLAMP((int)(gg + 0.5), 0, 255);
            *b = (guint8)CLAMP((int)(bb + 0.5), 0, 255);
            *a = (guint8)CLAMP((int)(ao * 255 + 0.5), 0, 255);
        }
    }
    for (int i = 0; i < n; i++) g_free(parts[i]);
    return ok;
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
    if (parse_hwb_func(s, r, g, b, a)) return TRUE;
    if (parse_lab_func(s, r, g, b, a)) return TRUE;
    if (parse_oklab_func(s, r, g, b, a)) return TRUE;
    if (parse_color_mix_func(s, r, g, b, a)) return TRUE;
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

gboolean
nd_css_parse_color(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
    return parse_color(s, r, g, b, a);
}

static void
nd_attr_pred_clear(gpointer p)
{
    nd_css_attr_pred *a = p;
    g_free(a->name);
    g_free(a->value);
}

static void
matches_any_group_free(gpointer data)
{
    g_ptr_array_free((GPtrArray *)data, TRUE);
}

static void
nd_pseudo_pred_clear(gpointer p)
{
    nd_css_pseudo_pred *pc = p;
    g_free(pc->arg);
    if (pc->of_group) g_ptr_array_free(pc->of_group, TRUE);
}

static nd_css_simple *
nd_css_simple_new(void)
{
    nd_css_simple *s = g_new0(nd_css_simple, 1);
    s->classes = g_ptr_array_new_with_free_func(g_free);
    s->attrs   = g_array_new(FALSE, FALSE, sizeof(nd_css_attr_pred));
    g_array_set_clear_func(s->attrs, nd_attr_pred_clear);
    s->pseudos = g_array_new(FALSE, FALSE, sizeof(nd_css_pseudo_pred));
    g_array_set_clear_func(s->pseudos, nd_pseudo_pred_clear);
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
    if (s->matches_any)  g_ptr_array_free(s->matches_any,  TRUE);
    if (s->matches_none) g_ptr_array_free(s->matches_none, TRUE);
    if (s->has_groups)   g_ptr_array_free(s->has_groups,   TRUE);
    g_free(s);
}

static void
nd_css_selector_free(nd_css_selector *sel)
{
    if (!sel) return;
    for (guint i = 0; i < sel->compounds->len; i++)
        nd_css_simple_free(g_ptr_array_index(sel->compounds, i));
    g_ptr_array_free(sel->compounds, TRUE);
    g_array_free(sel->combinators, TRUE);
    g_free(sel);
}

typedef struct nd_css_scope {
    GPtrArray *roots;
    GPtrArray *limits;
} nd_css_scope;

typedef struct nd_css_scope_text {
    char *start;
    char *end;
} nd_css_scope_text;

#define ND_CSS_MAX_SELECTOR_NESTING 48

static nd_css_selector *parse_one_selector(const char **pp, const char *end,
                                           int depth);

static GPtrArray *
parse_selector_group(const char *arg, gsize arg_n, int depth)
{
    GPtrArray *group = g_ptr_array_new_with_free_func(
        (GDestroyNotify)nd_css_selector_free);
    if (depth > ND_CSS_MAX_SELECTOR_NESTING)
        return group;
    const char *p = arg;
    const char *end = arg + arg_n;
    while (p < end) {
        const char *loop_start = p;
        p = css_skip_ws_comments(p, end);
        if (p >= end) break;
        nd_css_selector *sub = parse_one_selector(&p, end, depth);
        if (sub) g_ptr_array_add(group, sub);
        p = css_skip_ws_comments(p, end);
        if (p < end && *p == ',') { p++; continue; }
        if (p == loop_start) p++;
    }
    return group;
}

static const char *
css_find_nth_of(const char *s, const char *end)
{
    char quote = 0;
    int paren = 0, bracket = 0;
    const char *p = s;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p + 1 < end) p += 2;
            else {
                if (c == quote) quote = 0;
                p++;
            }
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            p = css_skip_comment(p, end);
            continue;
        }
        if (c == '\\' && p + 1 < end) {
            p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            p++;
            continue;
        }
        if (c == '[') bracket++;
        else if (c == ']' && bracket > 0) bracket--;
        else if (c == '(') paren++;
        else if (c == ')' && paren > 0) paren--;
        if (paren == 0 && bracket == 0 &&
            p + 2 <= end &&
            g_ascii_strncasecmp(p, "of", 2) == 0 &&
            (p == s || is_ws(p[-1])) &&
            (p + 2 == end || is_ws(p[2])))
            return p;
        p++;
    }
    return NULL;
}

static gboolean
parse_anb(const char *arg, gsize alen, int *out_a, int *out_b)
{
    char *raw = g_strndup(arg, alen);
    char *trimmed = g_strstrip(raw);
    char *s = g_malloc(strlen(trimmed) + 1);
    char *w = s;
    for (const char *r = trimmed; *r; r++)
        if (!is_ws(*r)) *w++ = *r;
    *w = '\0';
    int a = 0, b = 0;
    if (g_ascii_strcasecmp(s, "odd") == 0) {
        a = 2;
        b = 1;
    } else if (g_ascii_strcasecmp(s, "even") == 0) {
        a = 2;
        b = 0;
    } else {
        char *n_pos = strchr(s, 'n');
        if (!n_pos) n_pos = strchr(s, 'N');
        if (n_pos) {
            *n_pos = '\0';
            char *a_str = s;
            while (*a_str == ' ') a_str++;
            if (!*a_str || strcmp(a_str, "+") == 0) a = 1;
            else if (strcmp(a_str, "-") == 0) a = -1;
            else a = nd_parse_int(a_str, 0, -1000000, 1000000);
            char *b_str = n_pos + 1;
            while (*b_str == ' ') b_str++;
            if (*b_str) b = nd_parse_int(b_str, 0, -1000000, 1000000);
        } else {
            a = 0;
            b = nd_parse_int(s, 0, -1000000, 1000000);
        }
    }
    g_free(s);
    g_free(raw);
    *out_a = a;
    *out_b = b;
    return TRUE;
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
        { "only-of-type",  ND_CSS_PC_ONLY_OF_TYPE },
        { "empty",         ND_CSS_PC_EMPTY },
        { "root",          ND_CSS_PC_ROOT },
        { "checked",       ND_CSS_PC_CHECKED },
        { "disabled",      ND_CSS_PC_DISABLED },
        { "enabled",       ND_CSS_PC_ENABLED },
        { "required",      ND_CSS_PC_REQUIRED },
        { "optional",      ND_CSS_PC_OPTIONAL },
        { "valid",         ND_CSS_PC_VALID },
        { "invalid",       ND_CSS_PC_INVALID },
        { "link",          ND_CSS_PC_LINK },
        { "visited",       ND_CSS_PC_VISITED },
        { "any-link",      ND_CSS_PC_ANY_LINK },
        { "hover",         ND_CSS_PC_HOVER },
        { "active",        ND_CSS_PC_ACTIVE },
        { "focus",         ND_CSS_PC_FOCUS },
        { "focus-visible", ND_CSS_PC_FOCUS },
        { "focus-within",  ND_CSS_PC_FOCUS_WITHIN },
        { "target",        ND_CSS_PC_TARGET },
        { "defined",       ND_CSS_PC_DEFINED },
        { "scope",         ND_CSS_PC_SCOPE },
        { "placeholder-shown", ND_CSS_PC_PLACEHOLDER_SHOWN },
        { "read-only",     ND_CSS_PC_READ_ONLY },
        { "read-write",    ND_CSS_PC_READ_WRITE },
        { "open",          ND_CSS_PC_OPEN },
        { "popover-open",  ND_CSS_PC_POPOVER_OPEN },
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
    if (arg && ((n == 9 && g_ascii_strncasecmp(name, "nth-child", 9) == 0) ||
                (n == 14 && g_ascii_strncasecmp(name, "nth-last-child", 14) == 0) ||
                (n == 11 && g_ascii_strncasecmp(name, "nth-of-type", 11) == 0) ||
                (n == 16 && g_ascii_strncasecmp(name, "nth-last-of-type", 16) == 0))) {
        const char *as = arg;
        const char *ae = arg + alen;
        const char *of = (n == 9 || n == 14) ? css_find_nth_of(as, ae) : NULL;
        const char *anb_end = of ? of : ae;
        int a = 0, b = 0;
        if (!parse_anb(as, (gsize)(anb_end - as), &a, &b)) return FALSE;
        if (of) {
            const char *fs = css_skip_ws_comments(of + 2, ae);
            GPtrArray *group = parse_selector_group(fs, (gsize)(ae - fs), 1);
            if (!group || group->len == 0) {
                if (group) g_ptr_array_free(group, TRUE);
                return FALSE;
            }
            out->of_group = group;
        }
        if (n == 9) out->kind = ND_CSS_PC_NTH_CHILD;
        else if (n == 14) out->kind = ND_CSS_PC_NTH_LAST_CHILD;
        else if (n == 11) out->kind = ND_CSS_PC_NTH_OF_TYPE;
        else out->kind = ND_CSS_PC_NTH_LAST_OF_TYPE;
        out->a = a;
        out->b = b;
        return TRUE;
    }
    if (arg && n == 4 && g_ascii_strncasecmp(name, "lang", 4) == 0) {
        char *lang = css_trim_dup_range(arg, arg + alen);
        if (!lang || !*lang) {
            g_free(lang);
            return FALSE;
        }
        out->kind = ND_CSS_PC_LANG;
        out->arg = lang;
        return TRUE;
    }
    if (arg && n == 3 && g_ascii_strncasecmp(name, "dir", 3) == 0) {
        char *dir = css_trim_dup_range(arg, arg + alen);
        char *lo = g_ascii_strdown(dir ? dir : "", -1);
        g_free(dir);
        if (strcmp(lo, "ltr") != 0 && strcmp(lo, "rtl") != 0) {
            g_free(lo);
            return FALSE;
        }
        out->kind = ND_CSS_PC_DIR;
        out->arg = lo;
        return TRUE;
    }
    return FALSE;
}

static void
selector_group_max_specificity(const GPtrArray *group, int *a, int *b, int *c)
{
    for (guint i = 0; group && i < group->len; i++) {
        const nd_css_selector *sub = g_ptr_array_index(group, i);
        if (sub->spec_a > *a ||
            (sub->spec_a == *a && sub->spec_b > *b) ||
            (sub->spec_a == *a && sub->spec_b == *b && sub->spec_c > *c)) {
            *a = sub->spec_a;
            *b = sub->spec_b;
            *c = sub->spec_c;
        }
    }
}

static nd_css_selector *
parse_one_selector(const char **pp, const char *end, int depth)
{
    nd_css_selector *sel = g_new0(nd_css_selector, 1);
    sel->compounds   = g_ptr_array_new();
    sel->combinators = g_array_new(FALSE, FALSE, sizeof(nd_css_comb));

    nd_css_comb pending = ND_CSS_COMB_NONE;
    gboolean expect_compound = TRUE;
    const char *p = *pp;

    while (p < end) {

        gboolean had_ws = FALSE;
        const char *before_ws = p;
        p = css_skip_ws_comments(p, end);
        had_ws = p > before_ws;
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
            const char *tok_start = p;
            char cc = *p;
            if (cc == '*') {
                g_free(cmp->type);
                cmp->type = g_strdup("*");
                p++;
                sel->spec_c += 0;
                any = TRUE;
            } else if (cc == '#') {
                p++;
                char *id_str = read_css_ident(&p, end);
                g_free(cmp->id);
                cmp->id = id_str;
                sel->spec_a += 1;
                any = TRUE;
            } else if (cc == '.') {
                p++;
                char *cls = read_css_ident(&p, end);
                if (cls && *cls) {
                    g_ptr_array_add(cmp->classes, cls);
                    sel->spec_b += 1;
                    any = TRUE;
                } else {
                    g_free(cls);
                }
            } else if (is_ident_start(cc) || cc == '\\') {
                char *type = read_css_ident(&p, end);
                if (!cmp->type) {
                    cmp->type = ascii_lower(type, strlen(type));
                    sel->spec_c += 1;
                }
                g_free(type);
                any = TRUE;
            } else if (cc == ':') {
                p++;
                gboolean is_element = (p < end && *p == ':');
                if (is_element) p++;
                char *pseudo_name = read_css_ident(&p, end);
                const char *name_s = pseudo_name;
                gsize name_n = strlen(pseudo_name);
                const char *arg_s = NULL;
                gsize arg_n = 0;
                if (p < end && *p == '(') {
                    p++;
                    arg_s = p;
                    char term = 0;
                    const char *arg_end = css_scan_until(p, end, ")", &term);
                    arg_n = (gsize)(arg_end - arg_s);
                    p = term == ')' ? arg_end + 1 : arg_end;
                }
                if (is_element ||
                    (name_n == 6 && g_ascii_strncasecmp(name_s, "before", 6) == 0) ||
                    (name_n == 5 && g_ascii_strncasecmp(name_s, "after",  5) == 0) ||
                    (name_n == 10 && g_ascii_strncasecmp(name_s, "first-line", 10) == 0) ||
                    (name_n == 12 && g_ascii_strncasecmp(name_s, "first-letter", 12) == 0)) {
                    if (name_n == 6 && g_ascii_strncasecmp(name_s, "before", 6) == 0) {
                        sel->pseudo_element = ND_CSS_PE_BEFORE;
                        sel->spec_c += 1;
                    } else if (name_n == 5 && g_ascii_strncasecmp(name_s, "after", 5) == 0) {
                        sel->pseudo_element = ND_CSS_PE_AFTER;
                        sel->spec_c += 1;
                    } else if (name_n == 12 && g_ascii_strncasecmp(name_s, "first-letter", 12) == 0) {
                        sel->pseudo_element = ND_CSS_PE_FIRST_LETTER;
                        sel->spec_c += 1;
                    } else if (name_n == 10 && g_ascii_strncasecmp(name_s, "first-line", 10) == 0) {
                        sel->pseudo_element = ND_CSS_PE_FIRST_LINE;
                        sel->spec_c += 1;
                    } else if (name_n == 9 && g_ascii_strncasecmp(name_s, "selection", 9) == 0) {
                        sel->pseudo_element = ND_CSS_PE_SELECTION;
                        sel->spec_c += 1;
                    } else if (name_n == 6 && g_ascii_strncasecmp(name_s, "marker", 6) == 0) {
                        sel->pseudo_element = ND_CSS_PE_MARKER;
                        sel->spec_c += 1;
                    } else if (name_n == 8 && g_ascii_strncasecmp(name_s, "backdrop", 8) == 0) {
                        sel->pseudo_element = ND_CSS_PE_BACKDROP;
                        sel->spec_c += 1;
                    } else if ((name_n == 11 &&
                                g_ascii_strncasecmp(name_s, "placeholder", 11) == 0) ||
                               (name_n == 25 &&
                                g_ascii_strncasecmp(name_s, "-webkit-input-placeholder", 25) == 0) ||
                               (name_n == 21 &&
                                g_ascii_strncasecmp(name_s, "-ms-input-placeholder", 21) == 0) ||
                               (name_n == 16 &&
                                g_ascii_strncasecmp(name_s, "-moz-placeholder", 16) == 0)) {
                        sel->pseudo_element = ND_CSS_PE_PLACEHOLDER;
                        sel->spec_c += 1;
                    } else {
                        cmp->never_match = TRUE;
                    }
                } else if (name_n == 3 && arg_s &&
                           g_ascii_strncasecmp(name_s, "has", 3) == 0) {
                    GPtrArray *group = parse_selector_group(arg_s, arg_n, depth + 1);
                    if (group->len == 0) {
                        g_ptr_array_free(group, TRUE);
                        cmp->never_match = TRUE;
                    } else {
                        if (!cmp->has_groups)
                            cmp->has_groups = g_ptr_array_new_with_free_func(
                                matches_any_group_free);
                        g_ptr_array_add(cmp->has_groups, group);
                        int ma = 0, mb = 0, mc = 0;
                        for (guint gi = 0; gi < group->len; gi++) {
                            const nd_css_selector *sub =
                                g_ptr_array_index(group, gi);
                            if (sub->spec_a > ma ||
                                (sub->spec_a == ma && sub->spec_b > mb) ||
                                (sub->spec_a == ma && sub->spec_b == mb &&
                                 sub->spec_c > mc)) {
                                ma = sub->spec_a;
                                mb = sub->spec_b;
                                mc = sub->spec_c;
                            }
                        }
                        sel->spec_a += ma;
                        sel->spec_b += mb;
                        sel->spec_c += mc;
                    }
                } else if (name_n > 0 && arg_s &&
                           ((name_n == 2 && g_ascii_strncasecmp(name_s, "is",    2) == 0) ||
                            (name_n == 5 && g_ascii_strncasecmp(name_s, "where", 5) == 0))) {
                    gboolean is_where = (name_n == 5);
                    GPtrArray *group = parse_selector_group(arg_s, arg_n, depth + 1);
                    if (group->len == 0) {
                        g_ptr_array_free(group, TRUE);
                        cmp->never_match = TRUE;
                    } else {
                        if (!cmp->matches_any)
                            cmp->matches_any = g_ptr_array_new_with_free_func(
                                matches_any_group_free);
                        g_ptr_array_add(cmp->matches_any, group);
                        if (!is_where) {
                            int ma = 0, mb = 0, mc = 0;
                            for (guint gi = 0; gi < group->len; gi++) {
                                const nd_css_selector *sub =
                                    g_ptr_array_index(group, gi);
                                if (sub->spec_a > ma ||
                                    (sub->spec_a == ma && sub->spec_b > mb) ||
                                    (sub->spec_a == ma && sub->spec_b == mb &&
                                     sub->spec_c > mc)) {
                                    ma = sub->spec_a;
                                    mb = sub->spec_b;
                                    mc = sub->spec_c;
                                }
                            }
                            sel->spec_a += ma;
                            sel->spec_b += mb;
                            sel->spec_c += mc;
                        }
                    }
                } else if (name_n == 3 && arg_s &&
                           g_ascii_strncasecmp(name_s, "not", 3) == 0) {
                    GPtrArray *group = parse_selector_group(arg_s, arg_n, depth + 1);
                    if (group->len == 0) {
                        g_ptr_array_free(group, TRUE);
                    } else {
                        if (!cmp->matches_none)
                            cmp->matches_none = g_ptr_array_new_with_free_func(
                                matches_any_group_free);
                        g_ptr_array_add(cmp->matches_none, group);
                        int ma = 0, mb = 0, mc = 0;
                        for (guint gi = 0; gi < group->len; gi++) {
                            const nd_css_selector *sub =
                                g_ptr_array_index(group, gi);
                            if (sub->spec_a > ma ||
                                (sub->spec_a == ma && sub->spec_b > mb) ||
                                (sub->spec_a == ma && sub->spec_b == mb &&
                                 sub->spec_c > mc)) {
                                ma = sub->spec_a;
                                mb = sub->spec_b;
                                mc = sub->spec_c;
                            }
                        }
                        sel->spec_a += ma;
                        sel->spec_b += mb;
                        sel->spec_c += mc;
                    }
                } else if (name_n > 0) {
                    nd_css_pseudo_pred pc = {0};
                    if (parse_pseudo_keyword(name_s, name_n, arg_s, arg_n, &pc)) {
                        g_array_append_val(cmp->pseudos, pc);
                        sel->spec_b += 1;
                        int ma = 0, mb = 0, mc = 0;
                        selector_group_max_specificity(pc.of_group, &ma, &mb, &mc);
                        sel->spec_a += ma;
                        sel->spec_b += mb;
                        sel->spec_c += mc;
                    } else {
                        cmp->never_match = TRUE;
                    }
                } else {
                    cmp->never_match = TRUE;
                }
                g_free(pseudo_name);
                any = TRUE;
            } else if (cc == '[') {
                p++;
                p = css_skip_ws_comments(p, end);
                char *attr_name = read_css_ident(&p, end);
                if (!attr_name || !*attr_name) {
                    g_free(attr_name);
                    char term = 0;
                    const char *close = css_scan_until(p, end, "]", &term);
                    p = term == ']' ? close + 1 : close;
                    continue;
                }
                nd_css_attr_pred ap = {0};
                ap.name = ascii_lower(attr_name, strlen(attr_name));
                g_free(attr_name);
                ap.op   = ND_CSS_ATTR_PRESENT;
                p = css_skip_ws_comments(p, end);
                if (p < end && (*p == '=' || *p == '^' || *p == '$' ||
                                *p == '*' || *p == '~' || *p == '|')) {
                    char op_c = *p;
                    if (op_c == '=')      ap.op = ND_CSS_ATTR_EQ;
                    else if (op_c == '^') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_PREFIX; }
                    else if (op_c == '$') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_SUFFIX; }
                    else if (op_c == '*') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_SUBSTR; }
                    else if (op_c == '~') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_WORD;   }
                    else if (op_c == '|') { p++; if (p < end && *p == '=') ap.op = ND_CSS_ATTR_HYPHEN; }
                    if (p < end && *p == '=') p++;
                    p = css_skip_ws_comments(p, end);
                    char q = (p < end) ? *p : 0;
                    if (q == '"' || q == '\'') {
                        ap.value = read_css_string(&p, end);
                    } else {
                        ap.value = read_css_ident(&p, end);
                    }
                }
                p = css_skip_ws_comments(p, end);
                if (p < end && (*p == 'i' || *p == 'I')) {
                    ap.ci = TRUE;
                    p++;
                } else if (p < end && (*p == 's' || *p == 'S')) {
                    p++;
                }
                char term = 0;
                const char *close = css_scan_until(p, end, "]", &term);
                p = term == ']' ? close + 1 : close;
                g_array_append_val(cmp->attrs, ap);
                sel->spec_b += 1;
                any = TRUE;
            } else {
                break;
            }
            if (p == tok_start) break;
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
    if (g_ascii_strcasecmp(end, "em")  == 0) { *out_unit = ND_CSS_UNIT_EM;  return TRUE; }
    if (g_ascii_strcasecmp(end, "rem") == 0) { *out_unit = ND_CSS_UNIT_REM; return TRUE; }
    if (g_ascii_strcasecmp(end, "%")   == 0) { *out_unit = ND_CSS_UNIT_PERCENT; return TRUE; }
    if (g_ascii_strcasecmp(end, "vw") == 0) { *out_unit = ND_CSS_UNIT_VW; return TRUE; }
    if (g_ascii_strcasecmp(end, "vh") == 0) { *out_unit = ND_CSS_UNIT_VH; return TRUE; }
    if (g_ascii_strcasecmp(end, "dvw") == 0 || g_ascii_strcasecmp(end, "svw") == 0 ||
        g_ascii_strcasecmp(end, "lvw") == 0) { *out_unit = ND_CSS_UNIT_VW; return TRUE; }
    if (g_ascii_strcasecmp(end, "dvh") == 0 || g_ascii_strcasecmp(end, "svh") == 0 ||
        g_ascii_strcasecmp(end, "lvh") == 0) { *out_unit = ND_CSS_UNIT_VH; return TRUE; }
    if (g_ascii_strcasecmp(end, "cqi") == 0 || g_ascii_strcasecmp(end, "cqw") == 0) {
        *out_unit = ND_CSS_UNIT_CQW;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "cqb") == 0 || g_ascii_strcasecmp(end, "cqh") == 0) {
        *out_unit = ND_CSS_UNIT_CQH;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "vi") == 0 || g_ascii_strcasecmp(end, "dvi") == 0 ||
        g_ascii_strcasecmp(end, "svi") == 0 || g_ascii_strcasecmp(end, "lvi") == 0) {
        *out_unit = ND_CSS_UNIT_VW;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "vb") == 0 || g_ascii_strcasecmp(end, "dvb") == 0 ||
        g_ascii_strcasecmp(end, "svb") == 0 || g_ascii_strcasecmp(end, "lvb") == 0) {
        *out_unit = ND_CSS_UNIT_VH;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "vmin") == 0) { *out_unit = ND_CSS_UNIT_VMIN; return TRUE; }
    if (g_ascii_strcasecmp(end, "vmax") == 0) { *out_unit = ND_CSS_UNIT_VMAX; return TRUE; }
    if (g_ascii_strcasecmp(end, "dvmin") == 0 || g_ascii_strcasecmp(end, "svmin") == 0 ||
        g_ascii_strcasecmp(end, "lvmin") == 0) { *out_unit = ND_CSS_UNIT_VMIN; return TRUE; }
    if (g_ascii_strcasecmp(end, "dvmax") == 0 || g_ascii_strcasecmp(end, "svmax") == 0 ||
        g_ascii_strcasecmp(end, "lvmax") == 0) { *out_unit = ND_CSS_UNIT_VMAX; return TRUE; }
    if (g_ascii_strcasecmp(end, "cqmin") == 0) { *out_unit = ND_CSS_UNIT_CQMIN; return TRUE; }
    if (g_ascii_strcasecmp(end, "cqmax") == 0) { *out_unit = ND_CSS_UNIT_CQMAX; return TRUE; }
    if (g_ascii_strcasecmp(end, "pt")  == 0) {
        *out_v = v * (96.0 / 72.0);
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
    if (g_ascii_strcasecmp(end, "lh") == 0) {
        *out_unit = ND_CSS_UNIT_EM;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "ic") == 0) {
        *out_unit = ND_CSS_UNIT_EM;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "cap") == 0) {
        *out_unit = ND_CSS_UNIT_EM;
        *out_v = v * 0.7;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "rlh") == 0) {
        *out_unit = ND_CSS_UNIT_REM;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "cm")  == 0) { *out_v = v * (96.0 / 2.54); *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "mm")  == 0) { *out_v = v * (96.0 / 25.4); *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "q")   == 0) { *out_v = v * (96.0 / 101.6); *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "in")  == 0) { *out_v = v * 96.0;   *out_unit = ND_CSS_UNIT_PX; return TRUE; }
    return FALSE;
}

static double
font_size_keyword_px(const char *t)
{
    if (!t) return -1;
    if (g_ascii_strcasecmp(t, "xx-small")  == 0) return 9;
    if (g_ascii_strcasecmp(t, "x-small")   == 0) return 10;
    if (g_ascii_strcasecmp(t, "small")     == 0) return 13;
    if (g_ascii_strcasecmp(t, "medium")    == 0) return 16;
    if (g_ascii_strcasecmp(t, "large")     == 0) return 18;
    if (g_ascii_strcasecmp(t, "x-large")   == 0) return 24;
    if (g_ascii_strcasecmp(t, "xx-large")  == 0) return 32;
    if (g_ascii_strcasecmp(t, "xxx-large") == 0) return 48;
    return -1;
}

static gboolean
parse_font_size_token(const char *text, double *out_v, nd_css_unit *out_unit,
                      double *out_lh, nd_css_unit *out_lh_unit,
                      gboolean *out_has_lh)
{
    if (out_has_lh) *out_has_lh = FALSE;
    if (!text || !*text) return FALSE;
    char *s = g_strdup(text);
    char *slash = strchr(s, '/');
    if (slash) *slash = '\0';
    double kw = font_size_keyword_px(g_strstrip(s));
    gboolean ok;
    if (kw > 0) {
        *out_v = kw;
        *out_unit = ND_CSS_UNIT_PX;
        ok = TRUE;
    } else {
        ok = parse_length(s, out_v, out_unit) &&
             *out_unit != ND_CSS_UNIT_NUMBER;
    }
    if (ok && slash && slash[1] && out_lh && out_lh_unit &&
        parse_length(slash + 1, out_lh, out_lh_unit)) {
        if (out_has_lh) *out_has_lh = TRUE;
    }
    g_free(s);
    return ok;
}

static nd_css_value *parse_calc(const char *text);
static nd_css_value *parse_calc_inner(const char *text);

static gboolean
resolve_to_px_pct(const char *text, gsize len, double *out_px, double *out_pct)
{
    char *s = g_strndup(text, len);
    g_strstrip(s);
    *out_px = 0;
    *out_pct = 0;
    nd_css_value *v = parse_calc(s);
    if (v && v->kind == ND_CSS_V_CALC) {
        *out_px = v->u.calc.px + (v->u.calc.em + v->u.calc.rem) * 16.0;
        *out_pct = v->u.calc.pct;
        nd_css_value_free(v);
        g_free(s);
        return TRUE;
    }
    if (v) nd_css_value_free(v);
    double num;
    nd_css_unit u;
    if (parse_length(s, &num, &u)) {
        switch (u) {
        case ND_CSS_UNIT_PERCENT: *out_pct = num; break;
        case ND_CSS_UNIT_EM:
        case ND_CSS_UNIT_REM:     *out_px = num * 16.0; break;
        case ND_CSS_UNIT_VW:      *out_px = num * g_viewport_w / 100.0; break;
        case ND_CSS_UNIT_VH:      *out_px = num * g_viewport_h / 100.0; break;
        case ND_CSS_UNIT_VMIN:
            *out_px = num * (g_viewport_w < g_viewport_h ?
                             g_viewport_w : g_viewport_h) / 100.0;
            break;
        case ND_CSS_UNIT_VMAX:
            *out_px = num * (g_viewport_w > g_viewport_h ?
                             g_viewport_w : g_viewport_h) / 100.0;
            break;
        case ND_CSS_UNIT_CQW:     *out_px = num * g_viewport_w / 100.0; break;
        case ND_CSS_UNIT_CQH:     *out_px = num * g_viewport_h / 100.0; break;
        case ND_CSS_UNIT_CQMIN:
            *out_px = num * (g_viewport_w < g_viewport_h ?
                             g_viewport_w : g_viewport_h) / 100.0;
            break;
        case ND_CSS_UNIT_CQMAX:
            *out_px = num * (g_viewport_w > g_viewport_h ?
                             g_viewport_w : g_viewport_h) / 100.0;
            break;
        default:                  *out_px = num; break;
        }
        g_free(s);
        return TRUE;
    }
    g_free(s);
    return FALSE;
}

static const char *
match_close_paren(const char *p, const char *end)
{
    int depth = 1;
    while (p < end && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') { depth--; if (depth == 0) return p; }
        p++;
    }
    return NULL;
}

typedef struct nd_calc_term {
    double px;
    double pct;
    double em;
    double rem;
    double num;
    gboolean is_number;
} nd_calc_term;

static void
calc_skip_ws(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && is_ws(*p)) p++;
    *pp = p;
}

static void
calc_term_scale(nd_calc_term *v, double m)
{
    if (v->is_number) v->num *= m;
    else {
        v->px *= m;
        v->pct *= m;
        v->em *= m;
        v->rem *= m;
    }
}

static void
calc_term_lengthify(nd_calc_term *v)
{
    if (!v->is_number) return;
    v->px = v->num;
    v->pct = 0;
    v->is_number = FALSE;
}

static gboolean calc_expr_parse(const char **pp, const char *end,
                                nd_calc_term *out, int depth);

#define ND_CALC_MAX_DEPTH 64

static gboolean
calc_unit_value(const char *unit, double num, nd_calc_term *out)
{
    memset(out, 0, sizeof(*out));
    char *text = g_strdup_printf("%.17g%s", num, unit ? unit : "");
    double v = 0;
    nd_css_unit u = ND_CSS_UNIT_NUMBER;
    gboolean ok = parse_length(text, &v, &u);
    g_free(text);
    if (!ok) return FALSE;
    switch (u) {
    case ND_CSS_UNIT_NUMBER:
        out->num = v;
        out->is_number = TRUE;
        break;
    case ND_CSS_UNIT_PERCENT:
        out->pct = v;
        break;
    case ND_CSS_UNIT_EM:
        out->em = v;
        break;
    case ND_CSS_UNIT_REM:
        out->rem = v;
        break;
    case ND_CSS_UNIT_VW:
        out->px = v * g_viewport_w / 100.0;
        break;
    case ND_CSS_UNIT_VH:
        out->px = v * g_viewport_h / 100.0;
        break;
    case ND_CSS_UNIT_VMIN:
        out->px = v * (g_viewport_w < g_viewport_h ?
                       g_viewport_w : g_viewport_h) / 100.0;
        break;
    case ND_CSS_UNIT_VMAX:
        out->px = v * (g_viewport_w > g_viewport_h ?
                       g_viewport_w : g_viewport_h) / 100.0;
        break;
    case ND_CSS_UNIT_CQW:
        out->px = v * g_viewport_w / 100.0;
        break;
    case ND_CSS_UNIT_CQH:
        out->px = v * g_viewport_h / 100.0;
        break;
    case ND_CSS_UNIT_CQMIN:
        out->px = v * (g_viewport_w < g_viewport_h ?
                       g_viewport_w : g_viewport_h) / 100.0;
        break;
    case ND_CSS_UNIT_CQMAX:
        out->px = v * (g_viewport_w > g_viewport_h ?
                       g_viewport_w : g_viewport_h) / 100.0;
        break;
    default:
        out->px = v;
        break;
    }
    return TRUE;
}

static gboolean
calc_primary_parse(const char **pp, const char *end, nd_calc_term *out,
                   int depth)
{
    if (depth > ND_CALC_MAX_DEPTH) return FALSE;
    const char *p = *pp;
    calc_skip_ws(&p, end);
    if (p >= end) return FALSE;
    if (*p == '(') {
        p++;
        if (!calc_expr_parse(&p, end, out, depth + 1)) return FALSE;
        calc_skip_ws(&p, end);
        if (p >= end || *p != ')') return FALSE;
        p++;
        *pp = p;
        return TRUE;
    }
    static const struct { const char *name; gsize len; } funcs[] = {
        { "calc", 4 }, { "min", 3 }, { "max", 3 }, { "clamp", 5 },
        { "round", 5 }, { "mod", 3 }, { "rem", 3 }, { "abs", 3 },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(funcs); i++) {
        if ((gsize)(end - p) <= funcs[i].len + 1 ||
            g_ascii_strncasecmp(p, funcs[i].name, funcs[i].len) != 0 ||
            p[funcs[i].len] != '(')
            continue;
        const char *args = p + funcs[i].len + 1;
        const char *close = match_close_paren(args, end);
        if (!close) return FALSE;
        char *frag = g_strndup(p, (gsize)(close + 1 - p));
        nd_css_value *v = parse_calc(frag);
        g_free(frag);
        if (!v) return FALSE;
        memset(out, 0, sizeof(*out));
        if (v->kind == ND_CSS_V_CALC) {
            out->px = v->u.calc.px;
            out->pct = v->u.calc.pct;
            out->em = v->u.calc.em;
            out->rem = v->u.calc.rem;
        } else if (v->kind == ND_CSS_V_LENGTH) {
            double num = v->u.length.v;
            switch (v->u.length.unit) {
            case ND_CSS_UNIT_PERCENT: out->pct = num; break;
            case ND_CSS_UNIT_EM:      out->em = num; break;
            case ND_CSS_UNIT_REM:     out->rem = num; break;
            case ND_CSS_UNIT_VW:      out->px = num * g_viewport_w / 100.0; break;
            case ND_CSS_UNIT_VH:      out->px = num * g_viewport_h / 100.0; break;
            case ND_CSS_UNIT_VMIN:
                out->px = num * (g_viewport_w < g_viewport_h ?
                                 g_viewport_w : g_viewport_h) / 100.0;
                break;
            case ND_CSS_UNIT_VMAX:
                out->px = num * (g_viewport_w > g_viewport_h ?
                                 g_viewport_w : g_viewport_h) / 100.0;
                break;
            case ND_CSS_UNIT_CQW:     out->px = num * g_viewport_w / 100.0; break;
            case ND_CSS_UNIT_CQH:     out->px = num * g_viewport_h / 100.0; break;
            case ND_CSS_UNIT_CQMIN:
                out->px = num * (g_viewport_w < g_viewport_h ?
                                 g_viewport_w : g_viewport_h) / 100.0;
                break;
            case ND_CSS_UNIT_CQMAX:
                out->px = num * (g_viewport_w > g_viewport_h ?
                                 g_viewport_w : g_viewport_h) / 100.0;
                break;
            case ND_CSS_UNIT_NUMBER:
                out->num = num;
                out->is_number = TRUE;
                break;
            default:                  out->px = num; break;
            }
        } else {
            nd_css_value_free(v);
            return FALSE;
        }
        nd_css_value_free(v);
        *pp = close + 1;
        return TRUE;
    }
    char *num_end = NULL;
    double num = g_ascii_strtod(p, &num_end);
    if (!num_end || num_end == p) return FALSE;
    const char *u = num_end;
    while (u < end && (g_ascii_isalpha(*u) || *u == '%')) u++;
    char *unit = g_strndup(num_end, (gsize)(u - num_end));
    gboolean ok = calc_unit_value(unit, num, out);
    g_free(unit);
    if (!ok) return FALSE;
    *pp = u;
    return TRUE;
}

static gboolean
calc_product_parse(const char **pp, const char *end, nd_calc_term *out,
                   int depth)
{
    if (!calc_primary_parse(pp, end, out, depth)) return FALSE;
    while (1) {
        const char *p = *pp;
        calc_skip_ws(&p, end);
        if (p >= end || (*p != '*' && *p != '/')) {
            *pp = p;
            return TRUE;
        }
        char op = *p++;
        nd_calc_term rhs;
        if (!calc_primary_parse(&p, end, &rhs, depth)) return FALSE;
        if (op == '*') {
            if (out->is_number && rhs.is_number) {
                out->num *= rhs.num;
            } else if (out->is_number) {
                double m = out->num;
                *out = rhs;
                calc_term_scale(out, m);
            } else if (rhs.is_number) {
                calc_term_scale(out, rhs.num);
            } else {
                return FALSE;
            }
        } else {
            if (!rhs.is_number || rhs.num == 0) return FALSE;
            calc_term_scale(out, 1.0 / rhs.num);
        }
        *pp = p;
    }
}

static gboolean
calc_expr_parse(const char **pp, const char *end, nd_calc_term *out,
                int depth)
{
    if (!calc_product_parse(pp, end, out, depth)) return FALSE;
    while (1) {
        const char *p = *pp;
        calc_skip_ws(&p, end);
        if (p >= end || (*p != '+' && *p != '-')) {
            *pp = p;
            return TRUE;
        }
        char op = *p++;
        nd_calc_term rhs;
        if (!calc_product_parse(&p, end, &rhs, depth)) return FALSE;
        calc_term_lengthify(out);
        calc_term_lengthify(&rhs);
        if (op == '+') {
            out->px += rhs.px;
            out->pct += rhs.pct;
            out->em += rhs.em;
            out->rem += rhs.rem;
        } else {
            out->px -= rhs.px;
            out->pct -= rhs.pct;
            out->em -= rhs.em;
            out->rem -= rhs.rem;
        }
        *pp = p;
    }
}

static int
calc_split_args(const char *args, const char *body_end, char *out[], int max)
{
    int n = 0;
    const char *seg = args;
    while (seg < body_end && n < max) {
        char term = 0;
        const char *next = css_scan_until(seg, body_end, ",", &term);
        out[n++] = css_trim_dup_range(seg, next);
        seg = term == ',' ? next + 1 : next;
        if (term != ',') break;
    }
    return n;
}

static gboolean
calc_arg_key(const char *text, double *out)
{
    double px = 0, pct = 0;
    if (!resolve_to_px_pct(text, strlen(text), &px, &pct)) return FALSE;
    *out = px + pct * 0.01 * g_viewport_w;
    return TRUE;
}

static nd_css_value *
calc_px_value(double px)
{
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_LENGTH;
    v->u.length.v = px;
    v->u.length.unit = ND_CSS_UNIT_PX;
    return v;
}

static nd_css_value *
parse_calc(const char *text)
{
    static int depth;
    if (depth > ND_CALC_MAX_DEPTH) return NULL;
    depth++;
    nd_css_value *v = parse_calc_inner(text);
    depth--;
    return v;
}

static nd_css_value *
parse_calc_inner(const char *text)
{
    while (*text && is_ws(*text)) text++;
    int fn = -1;
    const char *args = NULL;
    if      (g_ascii_strncasecmp(text, "calc(",  5) == 0) { fn = 0; args = text + 5; }
    else if (g_ascii_strncasecmp(text, "clamp(", 6) == 0) { fn = 3; args = text + 6; }
    else if (g_ascii_strncasecmp(text, "min(",   4) == 0) { fn = 1; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "max(",   4) == 0) { fn = 2; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "round(", 6) == 0) { fn = 4; args = text + 6; }
    else if (g_ascii_strncasecmp(text, "mod(",   4) == 0) { fn = 5; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "rem(",   4) == 0) { fn = 6; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "abs(",   4) == 0) { fn = 7; args = text + 4; }
    else return NULL;
    const char *body_end = match_close_paren(args, args + strlen(args));
    if (!body_end) return NULL;
    if (fn >= 4) {
        char *parts[4] = {0};
        int n = calc_split_args(args, body_end, parts, G_N_ELEMENTS(parts));
        nd_css_value *out = NULL;
        if (fn == 7 && n == 1) {
            double x = 0;
            if (calc_arg_key(parts[0], &x))
                out = calc_px_value(fabs(x));
        } else if (fn == 4 && n >= 1) {
            int vi = 0;
            int strategy = 0;
            if (g_ascii_strcasecmp(parts[0], "nearest") == 0) {
                strategy = 0; vi = 1;
            } else if (g_ascii_strcasecmp(parts[0], "up") == 0) {
                strategy = 1; vi = 1;
            } else if (g_ascii_strcasecmp(parts[0], "down") == 0) {
                strategy = 2; vi = 1;
            } else if (g_ascii_strcasecmp(parts[0], "to-zero") == 0) {
                strategy = 3; vi = 1;
            }
            if (vi < n) {
                double x = 0, step = 1;
                if (calc_arg_key(parts[vi], &x) &&
                    (vi + 1 >= n || calc_arg_key(parts[vi + 1], &step)) &&
                    step != 0) {
                    double q = x / fabs(step);
                    double rq = strategy == 1 ? ceil(q) :
                                strategy == 2 ? floor(q) :
                                strategy == 3 ? trunc(q) : round(q);
                    out = calc_px_value(rq * fabs(step));
                }
            }
        } else if ((fn == 5 || fn == 6) && n == 2) {
            double x = 0, y = 0;
            if (calc_arg_key(parts[0], &x) && calc_arg_key(parts[1], &y) &&
                y != 0) {
                double q = x / y;
                double r = fn == 5 ? x - y * floor(q) : x - y * trunc(q);
                out = calc_px_value(r);
            }
        }
        for (int i = 0; i < n; i++) g_free(parts[i]);
        return out;
    }
    if (fn != 0) {
        double values_px[8] = {0};
        double values_pct[8] = {0};
        int n = 0;
        const char *seg = args;
        int depth = 0;
        for (const char *q = args; q <= body_end && n < 8; q++) {
            if (q < body_end && *q == '(') depth++;
            else if (q < body_end && *q == ')') depth--;
            if (q == body_end || (*q == ',' && depth == 0)) {
                resolve_to_px_pct(seg, (gsize)(q - seg),
                                  &values_px[n], &values_pct[n]);
                n++;
                seg = q + 1;
            }
        }
        if (n == 0) return NULL;
        double keys[8] = {0};
        for (int i = 0; i < n; i++)
            keys[i] = values_px[i] + values_pct[i] * 0.01 * g_viewport_w;
        double out_px;
        if (fn == 3) {
            double min_v = keys[0];
            double val_v = n > 1 ? keys[1] : min_v;
            double max_v = n > 2 ? keys[2] : val_v;
            out_px = val_v;
            if (out_px > max_v) out_px = max_v;
            if (out_px < min_v) out_px = min_v;
        } else {
            out_px = keys[0];
            for (int i = 1; i < n; i++) {
                if (fn == 1 && keys[i] < out_px) out_px = keys[i];
                if (fn == 2 && keys[i] > out_px) out_px = keys[i];
            }
        }
        nd_css_value *v = g_new0(nd_css_value, 1);
        v->kind = ND_CSS_V_LENGTH;
        v->u.length.v = out_px;
        v->u.length.unit = ND_CSS_UNIT_PX;
        return v;
    }
    text = args;
    const char *end = body_end;
    double pct = 0;
    double px  = 0;
    double em  = 0;
    double rem = 0;
    const char *p = text;
    nd_calc_term term;
    gboolean parsed = FALSE;
    if (calc_expr_parse(&p, end, &term, 0)) {
        calc_skip_ws(&p, end);
        if (p == end) {
            calc_term_lengthify(&term);
            px = term.px;
            pct = term.pct;
            em = term.em;
            rem = term.rem;
            parsed = TRUE;
        }
    }
    if (!parsed) return NULL;
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_CALC;
    v->u.calc.pct = pct;
    v->u.calc.px  = px;
    v->u.calc.em  = em;
    v->u.calc.rem = rem;
    return v;
}

static int split_ws(const char *s, char *out[4]);

static gboolean
parse_track_token(const char *tok, nd_css_track *out)
{
    if (!tok || !*tok) return FALSE;
    if (g_ascii_strcasecmp(tok, "auto") == 0 ||
        g_ascii_strcasecmp(tok, "min-content") == 0 ||
        g_ascii_strcasecmp(tok, "max-content") == 0) {
        out->kind = ND_CSS_TRACK_AUTO;
        out->v = 0;
        return TRUE;
    }
    char *endp = NULL;
    double v = g_ascii_strtod(tok, &endp);
    if (!endp || endp == tok) return FALSE;
    if (*endp == '\0' || g_ascii_strcasecmp(endp, "px") == 0) {
        out->kind = ND_CSS_TRACK_PX; out->v = v; return TRUE;
    }
    if (*endp == '%') { out->kind = ND_CSS_TRACK_PERCENT; out->v = v; return TRUE; }
    if (g_ascii_strcasecmp(endp, "fr") == 0) {
        out->kind = ND_CSS_TRACK_FR; out->v = v; return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "em") == 0) {
        out->kind = ND_CSS_TRACK_PX; out->v = v * 16; return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "rem") == 0) {
        out->kind = ND_CSS_TRACK_PX; out->v = v * 16; return TRUE;
    }
    return FALSE;
}

static gboolean
parse_minmax_token(const char *body, gsize len, nd_css_track *out)
{
    const char *p = body;
    const char *end = body + len;
    while (p < end && is_ws(*p)) p++;
    const char *as = p;
    char term = 0;
    const char *comma = css_scan_until(p, end, ",", &term);
    if (term != ',') return FALSE;
    p = comma;
    gsize alen = (gsize)(p - as);
    while (alen > 0 && is_ws(as[alen - 1])) alen--;
    p++;
    while (p < end && is_ws(*p)) p++;
    const char *bs = p;
    gsize blen = (gsize)(end - p);
    while (blen > 0 && is_ws(bs[blen - 1])) blen--;
    char *atok = g_strndup(as, alen);
    char *btok = g_strndup(bs, blen);
    nd_css_track mn = {0}, mx = {0};
    gboolean ok = parse_track_token(atok, &mn) && parse_track_token(btok, &mx);
    g_free(atok);
    g_free(btok);
    if (!ok) return FALSE;
    *out = mx;
    out->min_kind = mn.kind;
    out->min_v    = mn.v;
    out->has_min  = TRUE;
    return TRUE;
}

static gboolean
parse_one_track(const char *start, gsize len, nd_css_track *out)
{
    while (len > 0 && is_ws(*start)) { start++; len--; }
    while (len > 0 && is_ws(start[len - 1])) len--;
    if (len == 0) return FALSE;
    if (len > 7 && g_ascii_strncasecmp(start, "minmax(", 7) == 0 &&
        start[len - 1] == ')') {
        return parse_minmax_token(start + 7, len - 8, out);
    }
    char *tok = g_strndup(start, len);
    gboolean ok = parse_track_token(tok, out);
    g_free(tok);
    return ok;
}

static int
split_tracks_top(const char *text, gsize len, const char **starts, gsize *lens, int max)
{
    int n = 0;
    const char *p = text;
    const char *end = text + len;
    while (p < end && n < max) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;
        const char *tok_start = p;
        char term = 0;
        p = css_scan_until(p, end, " \t\n\r\f,", &term);
        starts[n] = tok_start;
        lens[n]   = (gsize)(p - tok_start);
        n++;
        if (p < end && *p == ',') p++;
    }
    return n;
}

static nd_css_value *
parse_tracks(const char *text)
{
    if (!text || !*text) return NULL;
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_TRACKS;
    const char *p = text;
    const char *full_end = text + strlen(text);
    while (p < full_end && is_ws(*p)) p++;
    if (g_ascii_strncasecmp(p, "subgrid", 7) == 0 &&
        (p + 7 >= full_end || is_ws(p[7]) || p[7] == '[')) {
        v->u.tracks.subgrid = TRUE;
        return v;
    }
    while (p < full_end && v->u.tracks.n < ND_CSS_TRACKS_MAX) {
        while (p < full_end && is_ws(*p)) p++;
        if (p >= full_end) break;
        if (g_ascii_strncasecmp(p, "repeat(", 7) == 0) {
            p += 7;
            while (p < full_end && is_ws(*p)) p++;
            const char *count_s = p;
            while (p < full_end && *p != ',' && *p != ')') p++;
            gsize count_len = (gsize)(p - count_s);
            while (count_len > 0 && is_ws(count_s[count_len - 1])) count_len--;
            if (p < full_end && *p == ',') p++;
            const char *body = p;
            int depth = 1;
            while (p < full_end && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') { depth--; if (depth == 0) break; }
                p++;
            }
            gsize body_len = (gsize)(p - body);
            if (p < full_end && *p == ')') p++;
            nd_css_auto_repeat ar = ND_CSS_AUTO_REPEAT_NONE;
            long n = 0;
            if (count_len == 8 && g_ascii_strncasecmp(count_s, "auto-fit", 8) == 0)
                ar = ND_CSS_AUTO_REPEAT_FIT;
            else if (count_len == 9 && g_ascii_strncasecmp(count_s, "auto-fill", 9) == 0)
                ar = ND_CSS_AUTO_REPEAT_FILL;
            else {
                char *cstr = g_strndup(count_s, count_len);
                n = strtol(cstr, NULL, 10);
                g_free(cstr);
                if (n <= 0) continue;
            }
            const char *tstarts[16];
            gsize tlens[16];
            int nb = split_tracks_top(body, body_len, tstarts, tlens, 16);
            if (ar != ND_CSS_AUTO_REPEAT_NONE) {
                if (v->u.tracks.auto_repeat == ND_CSS_AUTO_REPEAT_NONE) {
                    v->u.tracks.auto_repeat = ar;
                    v->u.tracks.auto_repeat_start = v->u.tracks.n;
                    int cnt = 0;
                    for (int i = 0; i < nb && v->u.tracks.n < ND_CSS_TRACKS_MAX; i++) {
                        nd_css_track t = {0};
                        if (parse_one_track(tstarts[i], tlens[i], &t)) {
                            v->u.tracks.tracks[v->u.tracks.n++] = t;
                            cnt++;
                        }
                    }
                    v->u.tracks.auto_repeat_count = cnt;
                }
                continue;
            }
            for (long r = 0; r < n && v->u.tracks.n < ND_CSS_TRACKS_MAX; r++) {
                for (int i = 0; i < nb && v->u.tracks.n < ND_CSS_TRACKS_MAX; i++) {
                    nd_css_track t = {0};
                    if (parse_one_track(tstarts[i], tlens[i], &t))
                        v->u.tracks.tracks[v->u.tracks.n++] = t;
                }
            }
            continue;
        }
        const char *tstarts[1];
        gsize tlens[1];
        int n = split_tracks_top(p, (gsize)(full_end - p), tstarts, tlens, 1);
        if (n == 0) break;
        nd_css_track t = {0};
        if (parse_one_track(tstarts[0], tlens[0], &t))
            v->u.tracks.tracks[v->u.tracks.n++] = t;
        const char *next = tstarts[0] + tlens[0];
        while (next < full_end && is_ws(*next)) next++;
        if (next < full_end && *next == ',') next++;
        if (next <= p) next = p + 1;
        p = next;
    }
    if (v->u.tracks.n == 0) { g_free(v); return NULL; }
    return v;
}

static nd_css_value *
parse_areas(const char *text)
{
    if (!text || !*text) return NULL;
    char *grid[ND_CSS_TRACKS_MAX][ND_CSS_TRACKS_MAX] = {{0}};
    int rows = 0;
    int cols = -1;
    const char *p = text;
    while (*p && rows < ND_CSS_TRACKS_MAX) {
        while (*p && is_ws(*p)) p++;
        if (!*p) break;
        if (*p != '"' && *p != '\'') return NULL;
        const char *row_start = p;
        char *row = read_css_string(&p, p + strlen(p));
        if (p == row_start) {
            g_free(row);
            return NULL;
        }
        char **toks = g_strsplit_set(row, " \t\r\n", -1);
        int c = 0;
        for (int i = 0; toks[i]; i++) {
            if (!*toks[i]) continue;
            if (c >= ND_CSS_TRACKS_MAX) break;
            grid[rows][c++] = g_strdup(toks[i]);
        }
        g_strfreev(toks);
        g_free(row);
        if (cols < 0) cols = c;
        else if (c != cols) {
            for (int r = 0; r <= rows; r++)
                for (int k = 0; k < ND_CSS_TRACKS_MAX; k++)
                    g_free(grid[r][k]);
            return NULL;
        }
        rows++;
    }
    if (rows == 0 || cols <= 0) {
        for (int r = 0; r < rows; r++)
            for (int k = 0; k < ND_CSS_TRACKS_MAX; k++)
                g_free(grid[r][k]);
        return NULL;
    }
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_AREAS;
    v->u.areas.n_rows = rows;
    v->u.areas.n_cols = cols;
    gboolean used[ND_CSS_TRACKS_MAX][ND_CSS_TRACKS_MAX] = {{0}};
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (used[r][c]) continue;
            const char *name = grid[r][c];
            if (!name || strcmp(name, ".") == 0) { used[r][c] = TRUE; continue; }
            int c1 = c;
            while (c1 + 1 < cols && grid[r][c1 + 1] &&
                   strcmp(grid[r][c1 + 1], name) == 0) c1++;
            int r1 = r;
            while (r1 + 1 < rows) {
                gboolean ok = TRUE;
                for (int k = c; k <= c1; k++) {
                    if (!grid[r1 + 1][k] || strcmp(grid[r1 + 1][k], name) != 0) {
                        ok = FALSE; break;
                    }
                }
                if (!ok) break;
                r1++;
            }
            if (v->u.areas.n_rects < ND_CSS_AREAS_MAX) {
                nd_css_area_rect *rect = &v->u.areas.rects[v->u.areas.n_rects++];
                rect->name = ascii_lower(name, strlen(name));
                rect->r0 = r; rect->r1 = r1;
                rect->c0 = c; rect->c1 = c1;
            }
            for (int rr = r; rr <= r1; rr++)
                for (int cc = c; cc <= c1; cc++)
                    used[rr][cc] = TRUE;
        }
    }
    for (int r = 0; r < rows; r++)
        for (int k = 0; k < ND_CSS_TRACKS_MAX; k++)
            g_free(grid[r][k]);
    return v;
}

static gboolean
parse_one_shadow(const char *text, nd_css_shadow *out)
{
    const char *p = text;
    while (*p && is_ws(*p)) p++;
    gboolean inset = FALSE;
    guint8 cr = 0, cg = 0, cb = 0, ca = 255;
    gboolean has_color = FALSE;
    double lens[4] = {0};
    int n_lens = 0;
    while (*p) {
        while (*p && is_ws(*p)) p++;
        if (!*p) break;
        const char *start = p;
        if (*p == '(') {
            int depth = 1;
            p++;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                p++;
            }
        } else {
            while (*p && !is_ws(*p)) p++;
        }
        gsize len = (gsize)(p - start);
        char *tok = g_strndup(start, len);
        guint8 r, g, b, a;
        double num;
        nd_css_unit u;
        if (parse_color(tok, &r, &g, &b, &a)) {
            cr = r; cg = g; cb = b; ca = a; has_color = TRUE;
        } else if (parse_length(tok, &num, &u) && n_lens < 4) {
            if (u == ND_CSS_UNIT_EM)   num *= 16;
            if (u == ND_CSS_UNIT_REM)  num *= 16;
            lens[n_lens++] = num;
        } else if (g_ascii_strcasecmp(tok, "inset") == 0) {
            inset = TRUE;
        }
        g_free(tok);
    }
    if (n_lens < 2) return FALSE;
    out->x = lens[0];
    out->y = lens[1];
    out->blur   = n_lens >= 3 ? lens[2] : 0;
    out->spread = n_lens >= 4 ? lens[3] : 0;
    out->r = cr; out->g = cg; out->b = cb;
    out->a = has_color ? ca : 128;
    out->inset = inset;
    return TRUE;
}

static nd_css_value *
parse_box_shadow(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (!*text || g_ascii_strncasecmp(text, "none", 4) == 0) return NULL;
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_SHADOW;
    char *copy = g_strdup(text);
    int depth = 0;
    char *seg = copy;
    for (char *q = copy; ; q++) {
        if (*q == '(') depth++;
        else if (*q == ')') { if (depth > 0) depth--; }
        gboolean at_end = (*q == '\0');
        if ((*q == ',' && depth == 0) || at_end) {
            char saved = *q;
            *q = '\0';
            if (v->u.shadow.n < ND_CSS_SHADOWS_MAX) {
                if (parse_one_shadow(seg, &v->u.shadow.s[v->u.shadow.n]))
                    v->u.shadow.n++;
            }
            if (at_end) break;
            *q = saved;
            seg = q + 1;
        }
    }
    g_free(copy);
    if (v->u.shadow.n == 0) { g_free(v); return NULL; }
    return v;
}

static void
gradient_add_stop(nd_css_gradient *gr, guint8 r, guint8 g, guint8 b, guint8 a,
                  gboolean has_pos, gboolean is_px, double pos)
{
    if (gr->n_stops >= ND_CSS_GRADIENT_STOPS_MAX) return;
    nd_css_gradient_stop *s = &gr->stops[gr->n_stops++];
    s->r = r; s->g = g; s->b = b; s->a = a;
    s->has_pos = has_pos;
    s->pos_is_px = is_px;
    s->pos = pos;
}

static gboolean
parse_stop_pos(const char *tok, gboolean *is_px, double *out)
{
    char *endp = NULL;
    double val = g_ascii_strtod(tok, &endp);
    if (!endp || endp == tok) return FALSE;
    while (*endp == ' ' || *endp == '\t') endp++;
    if (*endp == '%') { *is_px = FALSE; *out = val / 100.0; return TRUE; }
    if (g_ascii_strncasecmp(endp, "px", 2) == 0) { *is_px = TRUE; *out = val; return TRUE; }
    return FALSE;
}

static void
parse_gradient_stop_seg(nd_css_gradient *gr, const char *seg)
{
    char *tokens[4] = {0};
    int nt = split_ws(seg, tokens);
    if (nt >= 1) {
        guint8 r, g, b, a;
        if (parse_color(tokens[0], &r, &g, &b, &a)) {
            gboolean is_px = FALSE; double pos = 0; gboolean hp = FALSE;
            if (nt >= 2 && parse_stop_pos(tokens[1], &is_px, &pos)) hp = TRUE;
            gradient_add_stop(gr, r, g, b, a, hp, is_px, pos);
            if (nt >= 3) {
                gboolean is_px2 = FALSE; double pos2 = 0;
                if (parse_stop_pos(tokens[2], &is_px2, &pos2))
                    gradient_add_stop(gr, r, g, b, a, TRUE, is_px2, pos2);
            }
        }
    }
    for (int k = 0; k < nt; k++) g_free(tokens[k]);
}

static void
parse_gradient_at(const char *prelude, double *cx, double *cy, gboolean *has)
{
    if (!prelude) return;
    char **toks = g_strsplit_set(prelude, " \t", -1);
    int n = 0;
    while (toks[n]) n++;
    int ai = -1;
    for (int i = 0; i < n; i++)
        if (g_ascii_strcasecmp(g_strstrip(toks[i]), "at") == 0) { ai = i; break; }
    if (ai >= 0) {
        gboolean setx = FALSE, sety = FALSE;
        for (int i = ai + 1; i < n; i++) {
            char *t = g_strstrip(toks[i]);
            if (!*t) continue;
            if (g_ascii_strcasecmp(t, "left") == 0)        { *cx = 0;   setx = TRUE; }
            else if (g_ascii_strcasecmp(t, "right") == 0)  { *cx = 1;   setx = TRUE; }
            else if (g_ascii_strcasecmp(t, "top") == 0)    { *cy = 0;   sety = TRUE; }
            else if (g_ascii_strcasecmp(t, "bottom") == 0) { *cy = 1;   sety = TRUE; }
            else if (g_ascii_strcasecmp(t, "center") == 0) { /* axis-neutral */ }
            else {
                char *e = NULL;
                double pv = g_ascii_strtod(t, &e);
                if (e && e != t && *e == '%') {
                    if (!setx) { *cx = pv / 100.0; setx = TRUE; }
                    else       { *cy = pv / 100.0; sety = TRUE; }
                }
            }
        }
        if (setx || sety) *has = TRUE;
    }
    g_strfreev(toks);
}

static nd_css_value *
parse_linear_gradient(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (g_ascii_strncasecmp(text, "linear-gradient", 15) != 0) return NULL;
    text += 15;
    while (*text && is_ws(*text)) text++;
    if (*text != '(') return NULL;
    text++;
    const char *end = strrchr(text, ')');
    if (!end) return NULL;

    char *body = g_strndup(text, end - text);
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    int depth = 0;
    const char *seg = body;
    for (const char *p = body; ; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if ((*p == ',' && depth == 0) || *p == '\0') {
            gsize len = (gsize)(p - seg);
            char *piece = g_strndup(seg, len);
            g_strstrip(piece);
            g_ptr_array_add(parts, piece);
            if (*p == '\0') break;
            seg = p + 1;
        }
    }
    g_free(body);

    int angle = 180;
    int start_i = 0;
    if (parts->len > 0) {
        const char *first = parts->pdata[0];
        if (g_ascii_strncasecmp(first, "to ", 3) == 0) {
            const char *dir = first + 3;
            while (*dir && is_ws(*dir)) dir++;
            if (g_ascii_strncasecmp(dir, "bottom", 6) == 0) angle = 180;
            else if (g_ascii_strncasecmp(dir, "top", 3) == 0) angle = 0;
            else if (g_ascii_strncasecmp(dir, "left", 4) == 0) angle = 270;
            else if (g_ascii_strncasecmp(dir, "right", 5) == 0) angle = 90;
            start_i = 1;
        } else {
            char *endp = NULL;
            double a = g_ascii_strtod(first, &endp);
            if (endp && endp != first &&
                (g_ascii_strncasecmp(endp, "deg", 3) == 0 || *endp == '\0')) {
                angle = (int)a;
                start_i = 1;
            }
        }
    }

    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_GRADIENT;
    v->u.gradient.angle_deg = angle;
    v->u.gradient.n_stops = 0;
    for (guint i = (guint)start_i;
         i < parts->len && v->u.gradient.n_stops < ND_CSS_GRADIENT_STOPS_MAX;
         i++)
        parse_gradient_stop_seg(&v->u.gradient, parts->pdata[i]);
    g_ptr_array_free(parts, TRUE);
    if (v->u.gradient.n_stops < 2) {
        g_free(v);
        return NULL;
    }
    int n = v->u.gradient.n_stops;
    for (int i = 0; i < n; i++)
        if (!v->u.gradient.stops[i].has_pos)
            v->u.gradient.stops[i].pos = (n > 1) ? (double)i / (n - 1) : 0;
    return v;
}

static nd_css_value *
parse_radial_gradient(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (g_ascii_strncasecmp(text, "radial-gradient", 15) != 0) return NULL;
    text += 15;
    while (*text && is_ws(*text)) text++;
    if (*text != '(') return NULL;
    text++;
    const char *end = strrchr(text, ')');
    if (!end) return NULL;

    char *body = g_strndup(text, end - text);
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    int depth = 0;
    const char *seg = body;
    for (const char *p = body; ; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if ((*p == ',' && depth == 0) || *p == '\0') {
            gsize len = (gsize)(p - seg);
            char *piece = g_strndup(seg, len);
            g_strstrip(piece);
            g_ptr_array_add(parts, piece);
            if (*p == '\0') break;
            seg = p + 1;
        }
    }
    g_free(body);

    int start_i = 0;
    if (parts->len > 0) {
        const char *first = parts->pdata[0];
        guint8 dummy_r, dummy_g, dummy_b, dummy_a;
        if (!parse_color(first, &dummy_r, &dummy_g, &dummy_b, &dummy_a))
            start_i = 1;
    }

    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_GRADIENT;
    v->u.gradient.angle_deg = 0;
    v->u.gradient.radial = TRUE;
    v->u.gradient.n_stops = 0;
    v->u.gradient.center_x = 0.5;
    v->u.gradient.center_y = 0.5;
    if (start_i == 1 && parts->len > 0)
        parse_gradient_at(parts->pdata[0], &v->u.gradient.center_x,
                          &v->u.gradient.center_y, &v->u.gradient.has_center);
    for (guint i = (guint)start_i;
         i < parts->len && v->u.gradient.n_stops < ND_CSS_GRADIENT_STOPS_MAX;
         i++)
        parse_gradient_stop_seg(&v->u.gradient, parts->pdata[i]);
    g_ptr_array_free(parts, TRUE);
    if (v->u.gradient.n_stops < 2) {
        g_free(v);
        return NULL;
    }
    int n = v->u.gradient.n_stops;
    for (int i = 0; i < n; i++)
        if (!v->u.gradient.stops[i].has_pos)
            v->u.gradient.stops[i].pos = (n > 1) ? (double)i / (n - 1) : 0;
    return v;
}

static nd_css_value *
parse_conic_gradient(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (g_ascii_strncasecmp(text, "conic-gradient", 14) != 0) return NULL;
    text += 14;
    while (*text && is_ws(*text)) text++;
    if (*text != '(') return NULL;
    text++;
    const char *end = strrchr(text, ')');
    if (!end) return NULL;

    char *body = g_strndup(text, end - text);
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    int depth = 0;
    const char *seg = body;
    for (const char *p = body; ; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if ((*p == ',' && depth == 0) || *p == '\0') {
            gsize len = (gsize)(p - seg);
            char *piece = g_strndup(seg, len);
            g_strstrip(piece);
            g_ptr_array_add(parts, piece);
            if (*p == '\0') break;
            seg = p + 1;
        }
    }
    g_free(body);

    int from_deg = 0;
    int start_i = 0;
    if (parts->len > 0) {
        const char *first = parts->pdata[0];
        guint8 dummy_r, dummy_g, dummy_b, dummy_a;
        if (g_ascii_strncasecmp(first, "from ", 5) == 0) {
            char *endp = NULL;
            double d = g_ascii_strtod(first + 5, &endp);
            if (endp && endp != first + 5) from_deg = (int)(d + 0.5);
            start_i = 1;
        } else if (!parse_color(first, &dummy_r, &dummy_g, &dummy_b, &dummy_a)) {
            start_i = 1;
        }
    }

    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_GRADIENT;
    v->u.gradient.angle_deg = 0;
    v->u.gradient.radial = FALSE;
    v->u.gradient.conic = TRUE;
    v->u.gradient.from_deg = from_deg;
    v->u.gradient.n_stops = 0;
    v->u.gradient.center_x = 0.5;
    v->u.gradient.center_y = 0.5;
    if (start_i == 1 && parts->len > 0)
        parse_gradient_at(parts->pdata[0], &v->u.gradient.center_x,
                          &v->u.gradient.center_y, &v->u.gradient.has_center);
    for (guint i = (guint)start_i;
         i < parts->len && v->u.gradient.n_stops < ND_CSS_GRADIENT_STOPS_MAX;
         i++) {
        const char *stop_text = parts->pdata[i];
        char *tokens[4] = {0};
        int nt = split_ws(stop_text, tokens);
        if (nt < 1) { for (int k = 0; k < nt; k++) g_free(tokens[k]); continue; }
        guint8 r, g, b, a;
        if (parse_color(tokens[0], &r, &g, &b, &a)) {
            nd_css_gradient_stop *s =
                &v->u.gradient.stops[v->u.gradient.n_stops++];
            s->r = r; s->g = g; s->b = b; s->a = a;
            s->has_pos = FALSE;
            if (nt >= 2) {
                char *pos = tokens[1];
                char *pcend = strchr(pos, '%');
                if (pcend) {
                    char *endp = NULL;
                    double pct = g_ascii_strtod(pos, &endp);
                    if (endp && endp != pos) {
                        s->pos = pct / 100.0;
                        s->has_pos = TRUE;
                    }
                } else {
                    char *endp = NULL;
                    double deg = g_ascii_strtod(pos, &endp);
                    if (endp && endp != pos &&
                        g_ascii_strncasecmp(endp, "deg", 3) == 0) {
                        s->pos = deg / 360.0;
                        s->has_pos = TRUE;
                    }
                }
            }
        }
        for (int k = 0; k < nt; k++) g_free(tokens[k]);
    }
    g_ptr_array_free(parts, TRUE);
    if (v->u.gradient.n_stops < 2) {
        g_free(v);
        return NULL;
    }
    int ns = v->u.gradient.n_stops;
    for (int i = 0; i < ns; i++)
        if (!v->u.gradient.stops[i].has_pos)
            v->u.gradient.stops[i].pos = (ns > 1) ? (double)i / (ns - 1) : 0;
    return v;
}

static char *
pick_image_set_url(const char *t)
{
    const char *p = t;
    while (*p && is_ws(*p)) p++;
    if (g_ascii_strncasecmp(p, "-webkit-image-set(", 18) == 0)
        p += 18;
    else if (g_ascii_strncasecmp(p, "image-set(", 10) == 0)
        p += 10;
    else
        return NULL;

    const double target = 1.0;
    char *best = NULL;
    double best_res = 0;
    while (*p && *p != ')') {
        while (*p && (is_ws(*p) || *p == ',')) p++;
        if (!*p || *p == ')') break;
        char *url = NULL;
        if (g_ascii_strncasecmp(p, "url(", 4) == 0) {
            const char *u = p + 4;
            while (*u && is_ws(*u)) u++;
            char q = 0;
            if (*u == '"' || *u == '\'') { q = *u; u++; }
            const char *end;
            if (q) end = strchr(u, q);
            else { end = u; while (*end && *end != ')' && !is_ws(*end)) end++; }
            if (end && end > u) url = g_strndup(u, (gsize)(end - u));
            p = end ? end : p + 4;
            while (*p && *p != ')') p++;
            if (*p == ')') p++;
        }
        double res = 1.0;
        while (*p && is_ws(*p)) p++;
        if (*p && *p != ',' && *p != ')') {
            res = g_ascii_strtod(p, NULL);
            while (*p && *p != ',' && *p != ')') p++;
        }
        if (url) {
            if (!best || fabs(res - target) < fabs(best_res - target)) {
                g_free(best);
                best = url;
                best_res = res;
            } else {
                g_free(url);
            }
        } else {
            while (*p && *p != ',' && *p != ')') p++;
        }
    }
    return best;
}

static nd_css_value *
parse_any_gradient(const char *t)
{
    const char *p = t;
    while (*p && is_ws(*p)) p++;
    gboolean rep = g_ascii_strncasecmp(p, "repeating-", 10) == 0;
    const char *g = rep ? p + 10 : p;
    nd_css_value *v = parse_linear_gradient(g);
    if (!v) v = parse_radial_gradient(g);
    if (!v) v = parse_conic_gradient(g);
    if (v && v->kind == ND_CSS_V_GRADIENT) v->u.gradient.repeating = rep;
    return v;
}

static double
parse_angle_deg(const char *s)
{
    if (!s) return 0;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (!end || end == s) return 0;
    while (*end && is_ws(*end)) end++;
    if (g_ascii_strncasecmp(end, "rad", 3) == 0) return v * 180.0 / G_PI;
    if (g_ascii_strncasecmp(end, "turn", 4) == 0) return v * 360.0;
    if (g_ascii_strncasecmp(end, "grad", 4) == 0) return v * 0.9;
    return v;
}

static gboolean
parse_transform_len(const char *s, double *out, gboolean *is_percent)
{
    if (!s) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (!end || end == s) return FALSE;
    while (*end && is_ws(*end)) end++;
    *out = v;
    *is_percent = (*end == '%');
    return TRUE;
}

static nd_css_value *
parse_transform(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (!*text) return NULL;
    if (g_ascii_strncasecmp(text, "none", 4) == 0) return NULL;
    nd_css_transform tf = {0};
    const char *p = text;
    while (*p && tf.n_ops < ND_CSS_TRANSFORM_OPS_MAX) {
        while (*p && (is_ws(*p) || *p == ',')) p++;
        if (!*p) break;
        const char *name_start = p;
        while (*p && *p != '(') p++;
        if (*p != '(') break;
        gsize name_len = (gsize)(p - name_start);
        char *fn = g_strndup(name_start, name_len);
        g_strstrip(fn);
        char *fn_lc = g_ascii_strdown(fn, -1);
        g_free(fn);
        p++;
        const char *args_start = p;
        int depth = 1;
        while (*p && depth > 0) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            if (depth > 0) p++;
        }
        if (depth != 0) { g_free(fn_lc); break; }
        gsize args_len = (gsize)(p - args_start);
        char *args = g_strndup(args_start, args_len);
        if (*p == ')') p++;

        char *targs[6] = {0};
        int nt = 0;
        char *seg = args;
        for (char *q = args; ; q++) {
            if (*q == ',' || *q == '\0') {
                int saved = *q;
                *q = '\0';
                if (nt < 6) {
                    char *piece = g_strdup(seg);
                    g_strstrip(piece);
                    targs[nt++] = piece;
                }
                if (saved == '\0') break;
                seg = q + 1;
            }
        }

        nd_css_transform_op *op = &tf.ops[tf.n_ops];
        gboolean accept = FALSE;
        if (strcmp(fn_lc, "translate") == 0 ||
            strcmp(fn_lc, "translatex") == 0 ||
            strcmp(fn_lc, "translatey") == 0) {
            op->kind = ND_CSS_TFN_TRANSLATE;
            op->a = 0; op->b = 0;
            op->a_is_percent = FALSE; op->b_is_percent = FALSE;
            if (strcmp(fn_lc, "translatey") == 0) {
                if (nt >= 1) parse_transform_len(targs[0], &op->b, &op->b_is_percent);
            } else {
                if (nt >= 1) parse_transform_len(targs[0], &op->a, &op->a_is_percent);
                if (nt >= 2) parse_transform_len(targs[1], &op->b, &op->b_is_percent);
            }
            accept = TRUE;
        } else if (strcmp(fn_lc, "rotate") == 0 ||
                   strcmp(fn_lc, "rotatez") == 0) {
            op->kind = ND_CSS_TFN_ROTATE;
            op->a = nt >= 1 ? parse_angle_deg(targs[0]) : 0;
            op->b = 0;
            accept = TRUE;
        } else if (strcmp(fn_lc, "scale") == 0 ||
                   strcmp(fn_lc, "scalex") == 0 ||
                   strcmp(fn_lc, "scaley") == 0) {
            op->kind = ND_CSS_TFN_SCALE;
            double sa = nt >= 1 ? g_ascii_strtod(targs[0], NULL) : 1;
            double sb = nt >= 2 ? g_ascii_strtod(targs[1], NULL) : sa;
            if (strcmp(fn_lc, "scalex") == 0) { op->a = sa; op->b = 1; }
            else if (strcmp(fn_lc, "scaley") == 0) { op->a = 1; op->b = sa; }
            else { op->a = sa; op->b = sb; }
            accept = TRUE;
        } else if (strcmp(fn_lc, "skew") == 0 ||
                   strcmp(fn_lc, "skewx") == 0 ||
                   strcmp(fn_lc, "skewy") == 0) {
            op->kind = ND_CSS_TFN_SKEW;
            double aa = nt >= 1 ? parse_angle_deg(targs[0]) : 0;
            double bb = nt >= 2 ? parse_angle_deg(targs[1]) : 0;
            if (strcmp(fn_lc, "skewx") == 0) { op->a = aa; op->b = 0; }
            else if (strcmp(fn_lc, "skewy") == 0) { op->a = 0; op->b = aa; }
            else { op->a = aa; op->b = bb; }
            accept = TRUE;
        } else if (strcmp(fn_lc, "matrix") == 0 && nt == 6) {
            op->kind = ND_CSS_TFN_MATRIX;
            op->a = g_ascii_strtod(targs[0], NULL);
            op->b = g_ascii_strtod(targs[1], NULL);
            op->c = g_ascii_strtod(targs[2], NULL);
            op->d = g_ascii_strtod(targs[3], NULL);
            op->e = g_ascii_strtod(targs[4], NULL);
            op->f = g_ascii_strtod(targs[5], NULL);
            accept = TRUE;
        } else if ((strcmp(fn_lc, "translate3d") == 0 && nt >= 2)) {
            op->kind = ND_CSS_TFN_TRANSLATE;
            op->a = 0; op->b = 0;
            op->a_is_percent = FALSE; op->b_is_percent = FALSE;
            parse_transform_len(targs[0], &op->a, &op->a_is_percent);
            parse_transform_len(targs[1], &op->b, &op->b_is_percent);
            accept = TRUE;
        } else if (strcmp(fn_lc, "scale3d") == 0 && nt >= 2) {
            op->kind = ND_CSS_TFN_SCALE;
            op->a = g_ascii_strtod(targs[0], NULL);
            op->b = g_ascii_strtod(targs[1], NULL);
            accept = TRUE;
        }
        if (accept) tf.n_ops++;
        for (int k = 0; k < 6; k++) g_free(targs[k]);
        g_free(args);
        g_free(fn_lc);
    }
    if (tf.n_ops == 0) return NULL;
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_TRANSFORM;
    v->u.transform = tf;
    return v;
}

static gboolean
parse_origin_axis(const char *tok, gboolean is_y,
                  double *out, gboolean *is_percent)
{
    if (!tok || !*tok) return FALSE;
    char *lc = ascii_lower(tok, strlen(tok));
    gboolean ok = TRUE;
    if (strcmp(lc, "center") == 0)      { *out = 50; *is_percent = TRUE; }
    else if (!is_y && strcmp(lc, "left")  == 0)  { *out = 0;   *is_percent = TRUE; }
    else if (!is_y && strcmp(lc, "right") == 0)  { *out = 100; *is_percent = TRUE; }
    else if ( is_y && strcmp(lc, "top")   == 0)  { *out = 0;   *is_percent = TRUE; }
    else if ( is_y && strcmp(lc, "bottom") == 0) { *out = 100; *is_percent = TRUE; }
    else ok = parse_transform_len(tok, out, is_percent);
    g_free(lc);
    return ok;
}

static nd_css_value *
parse_transform_origin(const char *text)
{
    if (!text || !*text) return NULL;
    char **toks = g_strsplit_set(text, " \t\n\r", -1);
    char *a = NULL, *b = NULL;
    for (int i = 0; toks[i]; i++) {
        if (!*toks[i]) continue;
        if (!a) a = toks[i];
        else if (!b) b = toks[i];
    }
    nd_css_transform tf;
    memset(&tf, 0, sizeof(tf));
    tf.n_ops = 1;
    nd_css_transform_op *op = &tf.ops[0];
    op->kind = ND_CSS_TFN_TRANSLATE;
    op->a = 50; op->b = 50;
    op->a_is_percent = TRUE; op->b_is_percent = TRUE;
    gboolean swap = FALSE;
    if (a) {
        char *alc = ascii_lower(a, strlen(a));
        if (strcmp(alc, "top") == 0 || strcmp(alc, "bottom") == 0) swap = TRUE;
        g_free(alc);
    }
    if (swap) {
        if (a) parse_origin_axis(a, TRUE,  &op->b, &op->b_is_percent);
        if (b) parse_origin_axis(b, FALSE, &op->a, &op->a_is_percent);
    } else {
        if (a) parse_origin_axis(a, FALSE, &op->a, &op->a_is_percent);
        if (b) parse_origin_axis(b, TRUE,  &op->b, &op->b_is_percent);
        else if (a) {
            char *alc = ascii_lower(a, strlen(a));
            if (strcmp(alc, "left") == 0 || strcmp(alc, "right") == 0) {
                op->b = 50; op->b_is_percent = TRUE;
            }
            g_free(alc);
        }
    }
    g_strfreev(toks);
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_TRANSFORM;
    v->u.transform = tf;
    return v;
}

static nd_css_timing
parse_timing_keyword(const char *kw)
{
    nd_css_timing t = { .kind = ND_CSS_TIMING_EASE };
    if (!kw) return t;
    if (g_ascii_strcasecmp(kw, "linear") == 0)        t.kind = ND_CSS_TIMING_LINEAR;
    else if (g_ascii_strcasecmp(kw, "ease") == 0)     t.kind = ND_CSS_TIMING_EASE;
    else if (g_ascii_strcasecmp(kw, "ease-in") == 0)  t.kind = ND_CSS_TIMING_EASE_IN;
    else if (g_ascii_strcasecmp(kw, "ease-out") == 0) t.kind = ND_CSS_TIMING_EASE_OUT;
    else if (g_ascii_strcasecmp(kw, "ease-in-out") == 0) t.kind = ND_CSS_TIMING_EASE_IN_OUT;
    else if (g_ascii_strcasecmp(kw, "step-start") == 0) {
        t.kind = ND_CSS_TIMING_STEPS; t.steps = 1; t.step_pos = ND_CSS_STEP_JUMP_START;
    } else if (g_ascii_strcasecmp(kw, "step-end") == 0) {
        t.kind = ND_CSS_TIMING_STEPS; t.steps = 1; t.step_pos = ND_CSS_STEP_JUMP_END;
    }
    return t;
}

static gboolean
timing_keyword_matches(const char *kw)
{
    return g_ascii_strcasecmp(kw, "linear") == 0 ||
           g_ascii_strcasecmp(kw, "ease") == 0 ||
           g_ascii_strcasecmp(kw, "ease-in") == 0 ||
           g_ascii_strcasecmp(kw, "ease-out") == 0 ||
           g_ascii_strcasecmp(kw, "ease-in-out") == 0 ||
           g_ascii_strcasecmp(kw, "step-start") == 0 ||
           g_ascii_strcasecmp(kw, "step-end") == 0;
}

static nd_css_step_pos
parse_step_pos(const char *kw)
{
    if (g_ascii_strcasecmp(kw, "jump-start") == 0 ||
        g_ascii_strcasecmp(kw, "start") == 0)        return ND_CSS_STEP_JUMP_START;
    if (g_ascii_strcasecmp(kw, "jump-none") == 0)    return ND_CSS_STEP_JUMP_NONE;
    if (g_ascii_strcasecmp(kw, "jump-both") == 0)    return ND_CSS_STEP_JUMP_BOTH;
    return ND_CSS_STEP_JUMP_END;
}

static gboolean
extract_timing_function(char *item, nd_css_timing *out)
{
    static const struct { const char *name; gboolean is_steps; } fns[] = {
        { "steps(", TRUE }, { "cubic-bezier(", FALSE },
    };
    for (guint f = 0; f < G_N_ELEMENTS(fns); f++) {
        char *open = NULL;
        for (char *q = item; *q; q++) {
            if (g_ascii_strncasecmp(q, fns[f].name, strlen(fns[f].name)) == 0) {
                open = q;
                break;
            }
        }
        if (!open) continue;
        char *args = open + strlen(fns[f].name);
        char *close = strchr(args, ')');
        if (!close) continue;
        char *body = g_strndup(args, close - args);
        char **parts = g_strsplit(body, ",", -1);
        if (fns[f].is_steps) {
            out->kind = ND_CSS_TIMING_STEPS;
            out->steps = parts[0] ? (int)g_ascii_strtoll(g_strstrip(parts[0]), NULL, 10) : 1;
            if (out->steps < 1) out->steps = 1;
            out->step_pos = parts[0] && parts[1]
                ? parse_step_pos(g_strstrip(parts[1])) : ND_CSS_STEP_JUMP_END;
        } else {
            out->kind = ND_CSS_TIMING_CUBIC;
            for (int i = 0; i < 4; i++)
                out->cb[i] = parts[i] ? g_ascii_strtod(g_strstrip(parts[i]), NULL) : 0.0;
        }
        g_strfreev(parts);
        g_free(body);
        memset(open, ' ', (close - open) + 1);
        return TRUE;
    }
    return FALSE;
}

static gboolean
parse_time_ms(const char *tok, double *out_ms)
{
    if (!tok) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(tok, &end);
    if (end == tok) return FALSE;
    while (*end == ' ') end++;
    if (g_ascii_strcasecmp(end, "ms") == 0)      *out_ms = v;
    else if (g_ascii_strcasecmp(end, "s") == 0 ||
             *end == '\0')                       *out_ms = v * 1000.0;
    else return FALSE;
    return TRUE;
}

static nd_css_value *
parse_anim_value(const char *text, gboolean is_animation)
{
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_ANIM;
    v->u.anim.n = 0;
    const char *p = text;
    const char *end = text + strlen(text);
    while (p < end && v->u.anim.n < ND_CSS_ANIM_ENTRIES_MAX) {
        char term = 0;
        const char *seg = css_scan_until(p, end, ",", &term);
        char *item_buf = css_trim_dup_range(p, seg);
        char *item = item_buf;
        if (!*item) {
            g_free(item_buf);
            p = term == ',' ? seg + 1 : seg;
            continue;
        }
        nd_css_anim_entry *e = &v->u.anim.entries[v->u.anim.n];
        e->target = is_animation ? ND_CSS_ANIM_TARGET_ALL : ND_CSS_ANIM_TARGET_NONE;
        e->name = NULL;
        e->duration_ms = 0;
        e->delay_ms = 0;
        e->timing = (nd_css_timing){ .kind = ND_CSS_TIMING_EASE };
        e->iter_count = 1;
        e->direction = ND_CSS_ANIM_DIR_NORMAL;
        e->fill = ND_CSS_ANIM_FILL_NONE;
        gboolean got_dur = FALSE;
        nd_css_timing fn_timing;
        if (extract_timing_function(item, &fn_timing))
            e->timing = fn_timing;
        char **toks = g_strsplit_set(item, " \t\n\r", -1);
        for (int j = 0; toks[j]; j++) {
            char *tok = g_strstrip(toks[j]);
            if (!*tok) continue;
            double ms;
            if (parse_time_ms(tok, &ms)) {
                if (!got_dur) { e->duration_ms = ms; got_dur = TRUE; }
                else            { e->delay_ms = ms; }
                continue;
            }
            if (g_ascii_strcasecmp(tok, "infinite") == 0) {
                e->iter_count = -1;
                continue;
            }
            char *endp = NULL;
            double n = g_ascii_strtod(tok, &endp);
            if (endp != tok && (*endp == '\0' || *endp == ' ') && is_animation && got_dur) {
                e->iter_count = n <= 0 ? 0 : (int)n;
                continue;
            }
            if (timing_keyword_matches(tok)) {
                e->timing = parse_timing_keyword(tok);
                continue;
            }
            if (is_animation) {
                if (g_ascii_strcasecmp(tok, "reverse") == 0) {
                    e->direction = ND_CSS_ANIM_DIR_REVERSE; continue;
                }
                if (g_ascii_strcasecmp(tok, "alternate") == 0) {
                    e->direction = ND_CSS_ANIM_DIR_ALTERNATE; continue;
                }
                if (g_ascii_strcasecmp(tok, "alternate-reverse") == 0) {
                    e->direction = ND_CSS_ANIM_DIR_ALTERNATE_REVERSE; continue;
                }
                if (g_ascii_strcasecmp(tok, "forwards") == 0) {
                    e->fill = ND_CSS_ANIM_FILL_FORWARDS; continue;
                }
                if (g_ascii_strcasecmp(tok, "backwards") == 0) {
                    e->fill = ND_CSS_ANIM_FILL_BACKWARDS; continue;
                }
                if (g_ascii_strcasecmp(tok, "both") == 0) {
                    e->fill = ND_CSS_ANIM_FILL_BOTH; continue;
                }
            }
            if (g_ascii_strcasecmp(tok, "none") == 0) {
                continue;
            }
            if (!is_animation) {
                if (g_ascii_strcasecmp(tok, "opacity") == 0)
                    e->target = ND_CSS_ANIM_TARGET_OPACITY;
                else if (g_ascii_strcasecmp(tok, "transform") == 0)
                    e->target = ND_CSS_ANIM_TARGET_TRANSFORM;
                else if (g_ascii_strcasecmp(tok, "color") == 0)
                    e->target = ND_CSS_ANIM_TARGET_COLOR;
                else if (g_ascii_strcasecmp(tok, "background-color") == 0 ||
                         g_ascii_strcasecmp(tok, "background") == 0)
                    e->target = ND_CSS_ANIM_TARGET_BG_COLOR;
                else if (g_ascii_strcasecmp(tok, "all") == 0)
                    e->target = ND_CSS_ANIM_TARGET_ALL;
                continue;
            }
            if (is_animation && !e->name) {
                e->name = g_strdup(tok);
                continue;
            }
        }
        g_strfreev(toks);
        if (is_animation || e->target != ND_CSS_ANIM_TARGET_NONE)
            v->u.anim.n++;
        g_free(item_buf);
        p = term == ',' ? seg + 1 : seg;
    }
    if (v->u.anim.n == 0) {
        nd_css_value_free(v);
        return NULL;
    }
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

    v = parse_css_wide_keyword(t);
    if (v) {
        g_free(t);
        return v;
    }

    switch (prop) {
    case ND_CSS_COLOR:
    case ND_CSS_BACKGROUND_COLOR:
    case ND_CSS_BORDER_TOP_COLOR:
    case ND_CSS_BORDER_RIGHT_COLOR:
    case ND_CSS_BORDER_BOTTOM_COLOR:
    case ND_CSS_BORDER_LEFT_COLOR:
    case ND_CSS_OUTLINE_COLOR:
    case ND_CSS_TEXT_DECORATION_COLOR:
    case ND_CSS_COLUMN_RULE_COLOR:
    case ND_CSS_CARET_COLOR:
    case ND_CSS_ACCENT_COLOR: {
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
    case ND_CSS_BORDER_TOP_LEFT_RADIUS:
    case ND_CSS_BORDER_TOP_RIGHT_RADIUS:
    case ND_CSS_BORDER_BOTTOM_RIGHT_RADIUS:
    case ND_CSS_BORDER_BOTTOM_LEFT_RADIUS:
    case ND_CSS_GAP: case ND_CSS_ROW_GAP: case ND_CSS_COLUMN_GAP:
    case ND_CSS_FLEX_GROW: case ND_CSS_FLEX_SHRINK:
    case ND_CSS_FLEX_BASIS:
    case ND_CSS_ORDER:
    case ND_CSS_Z_INDEX:
    case ND_CSS_LINE_CLAMP:
    case ND_CSS_LINE_HEIGHT:
    case ND_CSS_OUTLINE_WIDTH:
    case ND_CSS_OUTLINE_OFFSET:
    case ND_CSS_TOP: case ND_CSS_RIGHT:
    case ND_CSS_BOTTOM: case ND_CSS_LEFT:
    case ND_CSS_COLUMN_COUNT: case ND_CSS_COLUMN_WIDTH:
    case ND_CSS_COLUMN_RULE_WIDTH: {
        if (prop == ND_CSS_FONT_SIZE) {
            double fs = font_size_keyword_px(t);
            if (fs > 0) {
                v = g_new0(nd_css_value, 1);
                v->kind = ND_CSS_V_LENGTH;
                v->u.length.v = fs;
                v->u.length.unit = ND_CSS_UNIT_PX;
                break;
            }
        }
        if (prop == ND_CSS_BORDER_TOP_WIDTH || prop == ND_CSS_BORDER_RIGHT_WIDTH ||
            prop == ND_CSS_BORDER_BOTTOM_WIDTH || prop == ND_CSS_BORDER_LEFT_WIDTH ||
            prop == ND_CSS_OUTLINE_WIDTH || prop == ND_CSS_COLUMN_RULE_WIDTH) {
            double bw = -1;
            if      (g_ascii_strcasecmp(t, "thin")   == 0) bw = 1;
            else if (g_ascii_strcasecmp(t, "medium") == 0) bw = 3;
            else if (g_ascii_strcasecmp(t, "thick")  == 0) bw = 5;
            if (bw >= 0) {
                v = g_new0(nd_css_value, 1);
                v->kind = ND_CSS_V_LENGTH;
                v->u.length.v = bw;
                v->u.length.unit = ND_CSS_UNIT_PX;
                break;
            }
        }
        gboolean sizing_prop = prop == ND_CSS_WIDTH || prop == ND_CSS_HEIGHT ||
            prop == ND_CSS_MIN_WIDTH || prop == ND_CSS_MAX_WIDTH ||
            prop == ND_CSS_MIN_HEIGHT || prop == ND_CSS_MAX_HEIGHT;
        if (g_ascii_strcasecmp(t, "auto") == 0 ||
            (sizing_prop &&
             (g_ascii_strcasecmp(t, "min-content") == 0 ||
              g_ascii_strcasecmp(t, "max-content") == 0 ||
              g_ascii_strcasecmp(t, "fit-content") == 0))) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = ascii_lower(t, strlen(t));
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
    case ND_CSS_BOX_SHADOW:
    case ND_CSS_TEXT_SHADOW: {
        v = parse_box_shadow(t);
        break;
    }
    case ND_CSS_GRID_TEMPLATE_COLUMNS:
    case ND_CSS_GRID_TEMPLATE_ROWS:
    case ND_CSS_GRID_AUTO_ROWS: {
        v = parse_tracks(t);
        if (!v) {
            char *kw = ascii_lower(t, strlen(t));
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = kw;
        }
        break;
    }
    case ND_CSS_GRID_TEMPLATE_AREAS: {
        v = parse_areas(t);
        if (!v) {
            char *kw = ascii_lower(t, strlen(t));
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = kw;
        }
        break;
    }
    case ND_CSS_BACKGROUND_POSITION_X:
    case ND_CSS_BACKGROUND_POSITION_Y:
    case ND_CSS_OBJECT_POSITION_X:
    case ND_CSS_OBJECT_POSITION_Y: {
        char *kw = ascii_lower(t, strlen(t));
        double pct = -1;
        if (kw) {
            if (prop == ND_CSS_BACKGROUND_POSITION_X ||
                prop == ND_CSS_OBJECT_POSITION_X) {
                if (strcmp(kw, "left") == 0)   pct = 0;
                else if (strcmp(kw, "center") == 0) pct = 50;
                else if (strcmp(kw, "right") == 0)  pct = 100;
            } else {
                if (strcmp(kw, "top") == 0)    pct = 0;
                else if (strcmp(kw, "center") == 0) pct = 50;
                else if (strcmp(kw, "bottom") == 0) pct = 100;
            }
        }
        if (pct >= 0) {
            g_free(kw);
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_LENGTH;
            v->u.length.v = pct;
            v->u.length.unit = ND_CSS_UNIT_PERCENT;
        } else {
            g_free(kw);
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
    case ND_CSS_BACKGROUND_SIZE: {
        char *kw = ascii_lower(t, strlen(t));
        if (kw && (strcmp(kw, "cover") == 0 || strcmp(kw, "contain") == 0 ||
                   strcmp(kw, "auto") == 0)) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = kw;
        } else {
            g_free(kw);
            char *tokens[4] = {0};
            int nt = split_ws(t, tokens);
            double w = 0, h = 0;
            nd_css_unit wu = ND_CSS_UNIT_PX, hu = ND_CSS_UNIT_PX;
            gboolean w_auto = FALSE, h_auto = TRUE;
            gboolean ok = FALSE;
            if (nt == 1) {
                if (g_ascii_strcasecmp(tokens[0], "auto") == 0) {
                    ok = TRUE;
                    w_auto = TRUE;
                    h_auto = TRUE;
                } else if (parse_length(tokens[0], &w, &wu)) {
                    ok = TRUE;
                }
            } else if (nt >= 2) {
                if (g_ascii_strcasecmp(tokens[0], "auto") == 0) {
                    w_auto = TRUE;
                    ok = TRUE;
                } else {
                    ok = parse_length(tokens[0], &w, &wu);
                }
                if (ok) {
                    if (g_ascii_strcasecmp(tokens[1], "auto") == 0) {
                        h_auto = TRUE;
                    } else if (parse_length(tokens[1], &h, &hu)) {
                        h_auto = FALSE;
                    } else {
                        ok = FALSE;
                    }
                }
            }
            if (ok) {
                v = g_new0(nd_css_value, 1);
                v->kind = ND_CSS_V_SIZE;
                v->u.size.w = w;
                v->u.size.h = h;
                v->u.size.w_unit = wu;
                v->u.size.h_unit = hu;
                v->u.size.w_auto = w_auto;
                v->u.size.h_auto = h_auto;
            }
            for (int i = 0; i < nt; i++) g_free(tokens[i]);
        }
        break;
    }
    case ND_CSS_BACKGROUND_REPEAT: {
        char *kw = ascii_lower(t, strlen(t));
        v = g_new0(nd_css_value, 1);
        v->kind = ND_CSS_V_KEYWORD;
        v->u.keyword = kw;
        break;
    }
    case ND_CSS_CONTENT: {
        gsize tl = strlen(t);
        gboolean single_string = FALSE;
        if (tl >= 2 && (t[0] == '"' || t[0] == '\'')) {
            char q = t[0];
            gsize i = 1;
            while (i < tl) {
                if (t[i] == '\\' && i + 1 < tl) { i += 2; continue; }
                if (t[i] == q) break;
                i++;
            }
            single_string = (i == tl - 1);
        }
        if (single_string) {
            char *raw = g_strndup(t + 1, tl - 2);
            GString *s = g_string_new(NULL);
            for (const char *p = raw; *p; ) {
                if (*p == '\\' && p[1]) {
                    p++;
                    if (g_ascii_isxdigit(*p)) {
                        char hex[8] = {0};
                        int hn = 0;
                        while (hn < 6 && g_ascii_isxdigit(*p)) hex[hn++] = *p++;
                        gunichar uc = (gunichar)g_ascii_strtoull(hex, NULL, 16);
                        if (*p == ' ') p++;
                        uc = css_unescape_cp(uc);
                        g_string_append_unichar(s, uc);
                    } else {
                        g_string_append_c(s, *p++);
                    }
                } else {
                    g_string_append_c(s, *p++);
                }
            }
            g_free(raw);
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = g_string_free(s, FALSE);
        } else if (g_str_has_prefix(t, "counter(") ||
                   g_str_has_prefix(t, "counters(") ||
                   g_str_has_prefix(t, "attr(") ||
                   strchr(t, '"') || strchr(t, '\'')) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = g_strdup(t);
        } else {
            char *kw = ascii_lower(t, strlen(t));
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = kw;
        }
        break;
    }
    case ND_CSS_COUNTER_RESET:
    case ND_CSS_COUNTER_INCREMENT: {
        v = g_new0(nd_css_value, 1);
        v->kind = ND_CSS_V_KEYWORD;
        v->u.keyword = g_strstrip(g_strdup(t));
        break;
    }
    case ND_CSS_MASK_IMAGE:
    case ND_CSS_BACKGROUND_IMAGE: {
        v = parse_any_gradient(t);
        if (!v) {
            const char *p = t;
            while (*p && is_ws(*p)) p++;
            char *iset = pick_image_set_url(p);
            if (iset) {
                v = g_new0(nd_css_value, 1);
                v->kind = ND_CSS_V_URL;
                v->u.url = iset;
            } else if (g_ascii_strncasecmp(p, "url(", 4) == 0) {
                const char *u = p + 4;
                while (*u && is_ws(*u)) u++;
                char q = 0;
                if (*u == '"' || *u == '\'') { q = *u; u++; }
                const char *end;
                if (q) {
                    end = strchr(u, q);
                } else {
                    end = u;
                    while (*end && *end != ')' && !is_ws(*end)) end++;
                }
                if (end && end > u) {
                    char *url = g_strndup(u, (gsize)(end - u));
                    v = g_new0(nd_css_value, 1);
                    v->kind = ND_CSS_V_URL;
                    v->u.url = url;
                }
            }
        }
        if (!v) {
            char *kw = ascii_lower(t, strlen(t));
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = kw;
        }
        break;
    }
    case ND_CSS_TRANSFORM: {
        v = parse_transform(t);
        if (!v) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = g_strdup("none");
        }
        break;
    }
    case ND_CSS_TRANSFORM_ORIGIN: {
        v = parse_transform_origin(t);
        break;
    }
    case ND_CSS_TRANSITION:
        v = parse_anim_value(t, FALSE);
        break;
    case ND_CSS_ANIMATION:
        v = parse_anim_value(t, TRUE);
        break;
    case ND_CSS_ASPECT_RATIO: {
        if (g_ascii_strcasecmp(t, "auto") == 0) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_KEYWORD;
            v->u.keyword = g_strdup("auto");
            break;
        }
        char *slash = strchr(t, '/');
        char *end_a = NULL;
        double a = g_ascii_strtod(t, &end_a);
        if (!end_a || end_a == t || a <= 0) break;
        double b = 1.0;
        if (slash) {
            char *end_b = NULL;
            const char *after = slash + 1;
            while (*after && is_ws(*after)) after++;
            b = g_ascii_strtod(after, &end_b);
            if (!end_b || end_b == after || b <= 0) break;
        }
        v = g_new0(nd_css_value, 1);
        v->kind = ND_CSS_V_LENGTH;
        v->u.length.v = a / b;
        v->u.length.unit = ND_CSS_UNIT_NUMBER;
        break;
    }
    case ND_CSS_CONTAINER_NAME: {
        v = g_new0(nd_css_value, 1);
        v->kind = ND_CSS_V_KEYWORD;
        v->u.keyword = g_strdup(t);
        break;
    }
    case ND_CSS_TAB_SIZE: {
        double len; nd_css_unit u;
        if (parse_length(t, &len, &u) && len >= 0) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_LENGTH;
            v->u.length.v = len;
            v->u.length.unit = u;
        }
        break;
    }
    case ND_CSS_BORDER_SPACING: {
        char *tokens[4] = {0};
        int nt = split_ws(t, tokens);
        double w = 0, h = 0;
        nd_css_unit wu = ND_CSS_UNIT_PX, hu = ND_CSS_UNIT_PX;
        gboolean ok = FALSE;
        if (nt >= 1 && parse_length(tokens[0], &w, &wu)) {
            ok = TRUE;
            if (nt >= 2) {
                if (!parse_length(tokens[1], &h, &hu)) ok = FALSE;
            } else {
                h = w; hu = wu;
            }
        }
        if (ok && w >= 0 && h >= 0) {
            v = g_new0(nd_css_value, 1);
            v->kind = ND_CSS_V_SIZE;
            v->u.size.w = w;
            v->u.size.h = h;
            v->u.size.w_unit = wu;
            v->u.size.h_unit = hu;
        }
        for (int i = 0; i < nt; i++) g_free(tokens[i]);
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

static const struct { const char *logical; const char *physical; } kLogicalAlias[] = {
    { "margin-block-start",        "margin-top" },
    { "margin-block-end",          "margin-bottom" },
    { "margin-inline-start",       "margin-left" },
    { "margin-inline-end",         "margin-right" },
    { "padding-block-start",       "padding-top" },
    { "padding-block-end",         "padding-bottom" },
    { "padding-inline-start",      "padding-left" },
    { "padding-inline-end",        "padding-right" },
    { "border-block-start-width",  "border-top-width" },
    { "border-block-end-width",    "border-bottom-width" },
    { "border-inline-start-width", "border-left-width" },
    { "border-inline-end-width",   "border-right-width" },
    { "border-block-start-style",  "border-top-style" },
    { "border-block-end-style",    "border-bottom-style" },
    { "border-inline-start-style", "border-left-style" },
    { "border-inline-end-style",   "border-right-style" },
    { "border-block-start-color",  "border-top-color" },
    { "border-block-end-color",    "border-bottom-color" },
    { "border-inline-start-color", "border-left-color" },
    { "border-inline-end-color",   "border-right-color" },
    { "border-start-start-radius", "border-top-left-radius" },
    { "border-start-end-radius",   "border-top-right-radius" },
    { "border-end-start-radius",   "border-bottom-left-radius" },
    { "border-end-end-radius",     "border-bottom-right-radius" },
    { "inset-block-start",         "top" },
    { "inset-block-end",           "bottom" },
    { "inset-inline-start",        "left" },
    { "inset-inline-end",          "right" },
    { "block-size",                "height" },
    { "inline-size",               "width" },
    { "min-block-size",            "min-height" },
    { "min-inline-size",           "min-width" },
    { "max-block-size",            "max-height" },
    { "max-inline-size",           "max-width" },
};

static const char *
alias_logical(const char *name)
{
    for (gsize i = 0; i < G_N_ELEMENTS(kLogicalAlias); i++)
        if (g_ascii_strcasecmp(name, kLogicalAlias[i].logical) == 0)
            return kLogicalAlias[i].physical;
    return NULL;
}

static int
prop_id(const char *name)
{
    for (int i = 0; i < ND_CSS_PROP_COUNT; i++) {
        if (g_ascii_strcasecmp(name, kProp[i]) == 0) return i;
    }
    if (g_ascii_strcasecmp(name, "word-wrap") == 0)
        return ND_CSS_OVERFLOW_WRAP;
    if (g_ascii_strcasecmp(name, "line-clamp") == 0)
        return ND_CSS_LINE_CLAMP;
    if (g_ascii_strcasecmp(name, "text-wrap") == 0 ||
        g_ascii_strcasecmp(name, "text-wrap-mode") == 0)
        return ND_CSS_WHITE_SPACE;
    if (g_ascii_strcasecmp(name, "-webkit-mask-image") == 0 ||
        g_ascii_strcasecmp(name, "-webkit-mask") == 0 ||
        g_ascii_strcasecmp(name, "mask") == 0)
        return ND_CSS_MASK_IMAGE;
    if (g_ascii_strcasecmp(name, "-webkit-background-clip") == 0)
        return ND_CSS_BACKGROUND_CLIP;
    if (g_ascii_strcasecmp(name, "-webkit-appearance") == 0 ||
        g_ascii_strcasecmp(name, "-moz-appearance") == 0)
        return ND_CSS_APPEARANCE;
    const char *phys = alias_logical(name);
    if (phys) {
        for (int i = 0; i < ND_CSS_PROP_COUNT; i++)
            if (g_ascii_strcasecmp(phys, kProp[i]) == 0) return i;
    }
    return -1;
}

int
nd_css_prop_id(const char *name)
{
    return name ? prop_id(name) : -1;
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
split_ws_limit(const char *s, char *out[], int max)
{
    int n = 0;
    const char *p = s;
    const char *end = s + strlen(s);
    while (p < end && n < max) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;
        const char *start = p;
        char term = 0;
        p = css_scan_until(p, end, " \t\n\r\f", &term);
        out[n++] = g_strndup(start, (gsize)(p - start));
    }
    return n;
}

static int
split_ws(const char *s, char *out[4])
{
    return split_ws_limit(s, out, 4);
}

static char *
substitute_var_fallbacks(const char *vtext)
{
    if (!vtext) return NULL;
    GString *out = g_string_new(NULL);
    const char *p = vtext;
    const char *end = vtext + strlen(vtext);
    while (p < end) {
        const char *fn = css_find_function(p, end, "var");
        if (!fn) {
            g_string_append_len(out, p, (gssize)(end - p));
            break;
        }
        g_string_append_len(out, p, (gssize)(fn - p));
        const char *args_start = fn + 4;
        char term = 0;
        const char *args_end = css_scan_until(args_start, end, ")", &term);
        if (term != ')') {
            p = end;
            break;
        }
        char comma_term = 0;
        const char *comma = css_scan_until(args_start, args_end, ",",
                                           &comma_term);
        if (comma_term == ',') {
            char *nested = css_trim_dup_range(comma + 1, args_end);
            char *sub = substitute_var_fallbacks(nested);
            if (sub) g_string_append(out, sub);
            g_free(nested);
            g_free(sub);
        }
        p = args_end + 1;
    }
    return g_string_free(out, FALSE);
}

static void pending_decl_clear(gpointer data);

static gboolean
custom_prop_value_invalid(const char *text)
{
    if (!text) return TRUE;
    char *trim = g_strstrip(g_strdup(text));
    char *kw = ascii_lower(trim, strlen(trim));
    gboolean invalid = css_wide_keyword_is(kw);
    g_free(kw);
    g_free(trim);
    return invalid;
}

static char *
substitute_vars_with_valid(const char *vtext, GHashTable *map, int depth,
                           gboolean *valid)
{
    if (!vtext) return NULL;
    if (depth > 16) return g_strdup(vtext);
    GString *out = g_string_new(NULL);
    const char *p = vtext;
    const char *end = vtext + strlen(vtext);
    while (p < end) {
        const char *fn = css_find_function(p, end, "var");
        if (!fn) {
            g_string_append_len(out, p, (gssize)(end - p));
            break;
        }
        g_string_append_len(out, p, (gssize)(fn - p));
        const char *args_start = fn + 4;
        char term = 0;
        const char *args_end = css_scan_until(args_start, end, ")", &term);
        if (term != ')') {
            p = end;
            break;
        }
        char comma_term = 0;
        const char *comma = css_scan_until(args_start, args_end, ",",
                                           &comma_term);
        const char *name_end = comma_term == ',' ? comma : args_end;
        char *name = css_trim_dup_range(args_start, name_end);
        const char *replacement = NULL;
        if (map && name[0] == '-' && name[1] == '-')
            replacement = g_hash_table_lookup(map, name);
        if (replacement && *replacement &&
            !custom_prop_value_invalid(replacement)) {
            gboolean sub_valid = TRUE;
            char *sub = substitute_vars_with_valid(replacement, map,
                                                   depth + 1, &sub_valid);
            if (sub_valid) {
                if (sub) g_string_append(out, sub);
            } else if (comma_term == ',') {
                char *nested = css_trim_dup_range(comma + 1, args_end);
                gboolean nested_valid = TRUE;
                char *fallback = substitute_vars_with_valid(nested, map,
                                                            depth + 1,
                                                            &nested_valid);
                if (nested_valid && fallback)
                    g_string_append(out, fallback);
                else if (valid)
                    *valid = FALSE;
                g_free(nested);
                g_free(fallback);
            } else if (valid) {
                *valid = FALSE;
            }
            g_free(sub);
        } else if (comma_term == ',') {
            char *nested = css_trim_dup_range(comma + 1, args_end);
            gboolean nested_valid = TRUE;
            char *sub = substitute_vars_with_valid(nested, map, depth + 1,
                                                   &nested_valid);
            if (nested_valid) {
                if (sub) g_string_append(out, sub);
            } else if (valid) {
                *valid = FALSE;
            }
            g_free(nested);
            g_free(sub);
        } else if (valid) {
            *valid = FALSE;
        }
        g_free(name);
        p = args_end + 1;
    }
    return g_string_free(out, FALSE);
}

static char *
substitute_vars_with(const char *vtext, GHashTable *map, int depth)
{
    gboolean valid = TRUE;
    char *out = substitute_vars_with_valid(vtext, map, depth, &valid);
    if (!valid) {
        g_free(out);
        return NULL;
    }
    return out;
}

static gboolean
is_color_keyword(const char *s)
{
    return s && (g_ascii_strcasecmp(s, "currentcolor") == 0 ||
                 g_ascii_strcasecmp(s, "transparent") == 0);
}

static void
parse_declaration_block(const char **pp, const char *end,
                        GArray *decls_out, nd_css_rule *capture)
{

    const char *p = *pp;
    while (p < end && *p != '}') {
        p = css_skip_ws_comments(p, end);
        while (p < end && *p == ';') {
            p++;
            p = css_skip_ws_comments(p, end);
        }
        if (p >= end || *p == '}') break;

        char *name = read_css_ident(&p, end);
        if (!name || !*name) {
            g_free(name);
            p++;
            continue;
        }
        char *pname = ascii_lower(name, strlen(name));
        g_free(name);
        p = css_skip_ws_comments(p, end);
        if (p >= end || *p != ':') { g_free(pname);
            char term = 0;
            const char *skip_to = css_scan_segment(p, end, &term);
            p = (term == ';') ? skip_to + 1 : skip_to;
            continue;
        }
        p++;

        const char *vstart = p;
        char term = 0;
        const char *vend = css_scan_declaration_value(p, end, &term);
        p = vend;
        char *raw_vtext = g_strndup(vstart, (gsize)(vend - vstart));

        if (capture && pname[0] == '-' && pname[1] == '-' && pname[2]) {
            char *trimmed = g_strstrip(g_strdup(raw_vtext));
            gboolean is_important = FALSE;
            css_strip_important(trimmed, &is_important);
            if (!capture->vars)
                capture->vars = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, g_free);
            g_hash_table_replace(capture->vars, g_strdup(pname), trimmed);
            if (is_important) {
                if (!capture->var_important)
                    capture->var_important = g_hash_table_new_full(
                        g_str_hash, g_str_equal, g_free, NULL);
                g_hash_table_add(capture->var_important, g_strdup(pname));
            } else if (capture->var_important) {
                g_hash_table_remove(capture->var_important, pname);
            }
            g_free(raw_vtext);
            g_free(pname);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (capture && strstr(raw_vtext, "var(")) {
            if (!capture->pending) {
                capture->pending = g_array_new(FALSE, FALSE,
                                               sizeof(nd_css_pending_decl));
                g_array_set_clear_func(capture->pending, pending_decl_clear);
            }
            gboolean is_important = FALSE;
            css_strip_important(raw_vtext, &is_important);
            nd_css_pending_decl pd = {
                .pname = pname,
                .raw_vtext = raw_vtext,
                .important = is_important,
            };
            g_array_append_val(capture->pending, pd);
            if (p < end && *p == ';') p++;
            continue;
        }

        char *vtext = substitute_var_fallbacks(raw_vtext);
        g_free(raw_vtext);
        gboolean important = FALSE;
        css_strip_important(vtext, &important);

        static const struct { const char *name; nd_css_prop t,r,b,l; } border_sides[] = {
            { "border-top",    ND_CSS_BORDER_TOP_WIDTH,    ND_CSS_BORDER_TOP_COLOR,
                               ND_CSS_BORDER_TOP_STYLE,    ND_CSS_PROP_COUNT },
            { "border-right",  ND_CSS_BORDER_RIGHT_WIDTH,  ND_CSS_BORDER_RIGHT_COLOR,
                               ND_CSS_BORDER_RIGHT_STYLE,  ND_CSS_PROP_COUNT },
            { "border-bottom", ND_CSS_BORDER_BOTTOM_WIDTH, ND_CSS_BORDER_BOTTOM_COLOR,
                               ND_CSS_BORDER_BOTTOM_STYLE, ND_CSS_PROP_COUNT },
            { "border-left",   ND_CSS_BORDER_LEFT_WIDTH,   ND_CSS_BORDER_LEFT_COLOR,
                               ND_CSS_BORDER_LEFT_STYLE,   ND_CSS_PROP_COUNT },
            { "border-inline-start", ND_CSS_BORDER_LEFT_WIDTH,   ND_CSS_BORDER_LEFT_COLOR,
                                      ND_CSS_BORDER_LEFT_STYLE,   ND_CSS_PROP_COUNT },
            { "border-inline-end",   ND_CSS_BORDER_RIGHT_WIDTH,  ND_CSS_BORDER_RIGHT_COLOR,
                                      ND_CSS_BORDER_RIGHT_STYLE,  ND_CSS_PROP_COUNT },
            { "border-block-start",  ND_CSS_BORDER_TOP_WIDTH,    ND_CSS_BORDER_TOP_COLOR,
                                      ND_CSS_BORDER_TOP_STYLE,    ND_CSS_PROP_COUNT },
            { "border-block-end",    ND_CSS_BORDER_BOTTOM_WIDTH, ND_CSS_BORDER_BOTTOM_COLOR,
                                      ND_CSS_BORDER_BOTTOM_STYLE, ND_CSS_PROP_COUNT },
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
            int n = split_ws_limit(vtext, tokens, G_N_ELEMENTS(tokens));
            for (int i = 0; i < n; i++) {
                guint8 r, g, b, a;
                double num; nd_css_unit u;
                if (parse_color(tokens[i], &r, &g, &b, &a) ||
                    is_color_keyword(tokens[i])) {
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
                } else if (parse_length(tokens[i], &num, &u) ||
                           g_ascii_strcasecmp(tokens[i], "thin") == 0 ||
                           g_ascii_strcasecmp(tokens[i], "medium") == 0 ||
                           g_ascii_strcasecmp(tokens[i], "thick") == 0) {
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

        if (strcmp(pname, "border-block") == 0 ||
            strcmp(pname, "border-inline") == 0) {
            gboolean is_block = strcmp(pname, "border-block") == 0;
            nd_css_prop w1 = is_block ? ND_CSS_BORDER_TOP_WIDTH : ND_CSS_BORDER_LEFT_WIDTH;
            nd_css_prop w2 = is_block ? ND_CSS_BORDER_BOTTOM_WIDTH : ND_CSS_BORDER_RIGHT_WIDTH;
            nd_css_prop c1 = is_block ? ND_CSS_BORDER_TOP_COLOR : ND_CSS_BORDER_LEFT_COLOR;
            nd_css_prop c2 = is_block ? ND_CSS_BORDER_BOTTOM_COLOR : ND_CSS_BORDER_RIGHT_COLOR;
            nd_css_prop s1 = is_block ? ND_CSS_BORDER_TOP_STYLE : ND_CSS_BORDER_LEFT_STYLE;
            nd_css_prop s2 = is_block ? ND_CSS_BORDER_BOTTOM_STYLE : ND_CSS_BORDER_RIGHT_STYLE;
            char *tokens[4] = {0};
            int n = split_ws_limit(vtext, tokens, G_N_ELEMENTS(tokens));
            for (int i = 0; i < n; i++) {
                guint8 r, g, b, a;
                double num; nd_css_unit u;
                nd_css_prop p1, p2;
                if (parse_color(tokens[i], &r, &g, &b, &a) ||
                    is_color_keyword(tokens[i])) {
                    p1 = c1; p2 = c2;
                } else if (parse_length(tokens[i], &num, &u)) {
                    p1 = w1; p2 = w2;
                } else {
                    p1 = s1; p2 = s2;
                }
                nd_css_value *v1 = parse_value_for(p1, tokens[i]);
                nd_css_value *v2 = parse_value_for(p2, tokens[i]);
                if (v1) {
                    nd_css_decl d = { .prop = p1, .value = v1, .important = important };
                    g_array_append_val(decls_out, d);
                }
                if (v2) {
                    nd_css_decl d = { .prop = p2, .value = v2, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        static const struct { const char *name; nd_css_prop a,b; } border_pair_props[] = {
            { "border-block-width",  ND_CSS_BORDER_TOP_WIDTH,    ND_CSS_BORDER_BOTTOM_WIDTH },
            { "border-inline-width", ND_CSS_BORDER_LEFT_WIDTH,   ND_CSS_BORDER_RIGHT_WIDTH },
            { "border-block-style",  ND_CSS_BORDER_TOP_STYLE,    ND_CSS_BORDER_BOTTOM_STYLE },
            { "border-inline-style", ND_CSS_BORDER_LEFT_STYLE,   ND_CSS_BORDER_RIGHT_STYLE },
            { "border-block-color",  ND_CSS_BORDER_TOP_COLOR,    ND_CSS_BORDER_BOTTOM_COLOR },
            { "border-inline-color", ND_CSS_BORDER_LEFT_COLOR,   ND_CSS_BORDER_RIGHT_COLOR },
            { NULL, ND_CSS_PROP_COUNT, ND_CSS_PROP_COUNT },
        };
        gboolean border_pair_prop = FALSE;
        for (int i = 0; border_pair_props[i].name; i++) {
            if (strcmp(pname, border_pair_props[i].name) != 0) continue;
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            if (n > 0) {
                const char *a = tokens[0];
                const char *b = n >= 2 ? tokens[1] : a;
                nd_css_value *va = parse_value_for(border_pair_props[i].a, a);
                nd_css_value *vb = parse_value_for(border_pair_props[i].b, b);
                if (va) {
                    nd_css_decl d = { .prop = border_pair_props[i].a, .value = va, .important = important };
                    g_array_append_val(decls_out, d);
                }
                if (vb) {
                    nd_css_decl d = { .prop = border_pair_props[i].b, .value = vb, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int j = 0; j < n; j++) g_free(tokens[j]);
            border_pair_prop = TRUE;
            break;
        }
        if (border_pair_prop) {
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        static const struct { const char *name; nd_css_prop prop; } prop_aliases[] = {
            { "inline-size", ND_CSS_WIDTH },
            { "block-size", ND_CSS_HEIGHT },
            { "min-inline-size", ND_CSS_MIN_WIDTH },
            { "max-inline-size", ND_CSS_MAX_WIDTH },
            { "min-block-size", ND_CSS_MIN_HEIGHT },
            { "max-block-size", ND_CSS_MAX_HEIGHT },
            { "margin-inline-start", ND_CSS_MARGIN_LEFT },
            { "margin-inline-end", ND_CSS_MARGIN_RIGHT },
            { "margin-block-start", ND_CSS_MARGIN_TOP },
            { "margin-block-end", ND_CSS_MARGIN_BOTTOM },
            { "padding-inline-start", ND_CSS_PADDING_LEFT },
            { "padding-inline-end", ND_CSS_PADDING_RIGHT },
            { "padding-block-start", ND_CSS_PADDING_TOP },
            { "padding-block-end", ND_CSS_PADDING_BOTTOM },
            { "inset-inline-start", ND_CSS_LEFT },
            { "inset-inline-end", ND_CSS_RIGHT },
            { "inset-block-start", ND_CSS_TOP },
            { "inset-block-end", ND_CSS_BOTTOM },
            { "border-inline-start-width", ND_CSS_BORDER_LEFT_WIDTH },
            { "border-inline-end-width", ND_CSS_BORDER_RIGHT_WIDTH },
            { "border-block-start-width", ND_CSS_BORDER_TOP_WIDTH },
            { "border-block-end-width", ND_CSS_BORDER_BOTTOM_WIDTH },
            { "border-inline-start-style", ND_CSS_BORDER_LEFT_STYLE },
            { "border-inline-end-style", ND_CSS_BORDER_RIGHT_STYLE },
            { "border-block-start-style", ND_CSS_BORDER_TOP_STYLE },
            { "border-block-end-style", ND_CSS_BORDER_BOTTOM_STYLE },
            { "border-inline-start-color", ND_CSS_BORDER_LEFT_COLOR },
            { "border-inline-end-color", ND_CSS_BORDER_RIGHT_COLOR },
            { "border-block-start-color", ND_CSS_BORDER_TOP_COLOR },
            { "border-block-end-color", ND_CSS_BORDER_BOTTOM_COLOR },
            { "border-start-start-radius", ND_CSS_BORDER_TOP_LEFT_RADIUS },
            { "border-start-end-radius", ND_CSS_BORDER_TOP_RIGHT_RADIUS },
            { "border-end-start-radius", ND_CSS_BORDER_BOTTOM_LEFT_RADIUS },
            { "border-end-end-radius", ND_CSS_BORDER_BOTTOM_RIGHT_RADIUS },
            { NULL, ND_CSS_PROP_COUNT },
        };
        gboolean aliased_prop = FALSE;
        for (int i = 0; prop_aliases[i].name; i++) {
            if (strcmp(pname, prop_aliases[i].name) != 0) continue;
            nd_css_value *vv = parse_value_for(prop_aliases[i].prop, vtext);
            if (vv) {
                nd_css_decl d = {
                    .prop = prop_aliases[i].prop,
                    .value = vv,
                    .important = important,
                };
                g_array_append_val(decls_out, d);
            }
            aliased_prop = TRUE;
            break;
        }
        if (aliased_prop) {
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "background") == 0) {
            char *vlower_grad = g_ascii_strdown(vtext, -1);
            gboolean has_linear = strstr(vlower_grad, "linear-gradient") != NULL;
            gboolean has_radial = strstr(vlower_grad, "radial-gradient") != NULL;
            gboolean has_conic  = strstr(vlower_grad, "conic-gradient")  != NULL;
            g_free(vlower_grad);
            if (has_linear || has_radial || has_conic) {
                const char *gtext = vtext;
                while (*gtext && is_ws(*gtext)) gtext++;
                nd_css_value *gv = parse_any_gradient(gtext);
                if (gv) {
                    nd_css_decl d = {
                        .prop = ND_CSS_BACKGROUND_IMAGE,
                        .value = gv,
                        .important = important,
                    };
                    g_array_append_val(decls_out, d);
                }
            } else {
                char *vlower = g_ascii_strdown(vtext, -1);
                const char *u = strstr(vlower, "url(");
                if (u) {
                    const char *vu = vtext + (u - vlower);
                    nd_css_value *uv = parse_value_for(ND_CSS_BACKGROUND_IMAGE, vu);
                    if (uv && uv->kind == ND_CSS_V_URL) {
                        nd_css_decl d = {
                            .prop = ND_CSS_BACKGROUND_IMAGE,
                            .value = uv,
                            .important = important,
                        };
                        g_array_append_val(decls_out, d);
                    } else {
                        nd_css_value_free(uv);
                    }
                }
                g_free(vlower);
            }
            if (has_linear || has_radial || has_conic) {
                int depth = 0;
                const char *last_comma = NULL;
                for (const char *q = vtext; *q; q++) {
                    if (*q == '(') depth++;
                    else if (*q == ')') { if (depth > 0) depth--; }
                    else if (*q == ',' && depth == 0) last_comma = q;
                }
                if (last_comma) {
                    const char *seg = last_comma + 1;
                    while (*seg && is_ws(*seg)) seg++;
                    char *segdup = g_strchomp(g_strdup(seg));
                    guint8 r, g, b, a;
                    if (parse_color(segdup, &r, &g, &b, &a)) {
                        nd_css_value *v = g_new0(nd_css_value, 1);
                        v->kind = ND_CSS_V_COLOR;
                        v->u.color.r = r; v->u.color.g = g;
                        v->u.color.b = b; v->u.color.a = a;
                        nd_css_decl d = { .prop = ND_CSS_BACKGROUND_COLOR,
                                          .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                    g_free(segdup);
                }
            }
            char *tokens[16] = {0};
            int n = split_ws_limit(vtext, tokens, G_N_ELEMENTS(tokens));
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
                if (is_color_keyword(tokens[i])) {
                    nd_css_value *v = parse_value_for(ND_CSS_BACKGROUND_COLOR, tokens[i]);
                    if (v) {
                        nd_css_decl decl = {
                            .prop = ND_CSS_BACKGROUND_COLOR,
                            .value = v,
                            .important = important,
                        };
                        g_array_append_val(decls_out, decl);
                    }
                    break;
                }
            }
            const char *pos_x = NULL;
            const char *pos_y = NULL;
            char *pos_x_owned = NULL;
            char *pos_y_owned = NULL;
            char *bg_size_text = NULL;
            int bg_size_skip = -1;
            for (int i = 0; i < n; i++) {
                const char *tk = tokens[i];
                if (!tk) continue;
                if (i == bg_size_skip) continue;
                if (g_ascii_strncasecmp(tk, "url(", 4) == 0 ||
                    g_ascii_strncasecmp(tk, "linear-gradient(", 16) == 0 ||
                    g_ascii_strncasecmp(tk, "radial-gradient(", 16) == 0 ||
                    g_ascii_strncasecmp(tk, "conic-gradient(", 15) == 0)
                    continue;
                if (strcmp(tk, "/") == 0) {
                    g_free(bg_size_text);
                    bg_size_text = NULL;
                    if (i + 1 < n) {
                        char *pair = NULL;
                        nd_css_value *pv = NULL;
                        if (i + 2 < n) {
                            pair = g_strdup_printf("%s %s", tokens[i + 1], tokens[i + 2]);
                            pv = parse_value_for(ND_CSS_BACKGROUND_SIZE, pair);
                        }
                        if (pv) {
                            nd_css_value_free(pv);
                            bg_size_text = pair;
                            bg_size_skip = i + 2;
                        } else {
                            g_free(pair);
                            bg_size_text = g_strdup(tokens[i + 1]);
                            bg_size_skip = i + 1;
                        }
                    }
                    continue;
                }
                const char *slash = strchr(tk, '/');
                if (slash) {
                    if (slash > tk) {
                        const char *before = NULL;
                        gsize blen = (gsize)(slash - tk);
                        if (blen == 6 && g_ascii_strncasecmp(tk, "center", blen) == 0)
                            before = "center";
                        else if (blen == 4 && g_ascii_strncasecmp(tk, "left", blen) == 0)
                            before = "left";
                        else if (blen == 5 && g_ascii_strncasecmp(tk, "right", blen) == 0)
                            before = "right";
                        else if (blen == 3 && g_ascii_strncasecmp(tk, "top", blen) == 0)
                            before = "top";
                        else if (blen == 6 && g_ascii_strncasecmp(tk, "bottom", blen) == 0)
                            before = "bottom";
                        if (before) {
                            if (!pos_x) pos_x = before;
                            else if (!pos_y) pos_y = before;
                        } else {
                            char *pre = g_strndup(tk, blen);
                            nd_css_value *v = parse_value_for(
                                pos_x ? ND_CSS_BACKGROUND_POSITION_Y
                                      : ND_CSS_BACKGROUND_POSITION_X,
                                pre);
                            if (v && v->kind == ND_CSS_V_LENGTH) {
                                if (!pos_x) {
                                    pos_x = pre;
                                    pos_x_owned = pre;
                                } else if (!pos_y) {
                                    pos_y = pre;
                                    pos_y_owned = pre;
                                } else {
                                    g_free(pre);
                                }
                            } else {
                                g_free(pre);
                            }
                            nd_css_value_free(v);
                        }
                    }
                    const char *after = slash + 1;
                    if (*after) {
                        g_free(bg_size_text);
                        if (i + 1 < n) {
                            bg_size_text = g_strdup_printf("%s %s", after, tokens[i + 1]);
                            bg_size_skip = i + 1;
                        } else {
                            bg_size_text = g_strdup(after);
                        }
                    }
                    continue;
                }
                if (g_ascii_strcasecmp(tk, "no-repeat") == 0 ||
                    g_ascii_strcasecmp(tk, "repeat") == 0 ||
                    g_ascii_strcasecmp(tk, "repeat-x") == 0 ||
                    g_ascii_strcasecmp(tk, "repeat-y") == 0 ||
                    g_ascii_strcasecmp(tk, "space") == 0 ||
                    g_ascii_strcasecmp(tk, "round") == 0) {
                    nd_css_value *v = parse_value_for(ND_CSS_BACKGROUND_REPEAT, tk);
                    if (v) {
                        nd_css_decl d = { .prop = ND_CSS_BACKGROUND_REPEAT, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                } else if (g_ascii_strcasecmp(tk, "cover") == 0 ||
                           g_ascii_strcasecmp(tk, "contain") == 0) {
                    nd_css_value *v = parse_value_for(ND_CSS_BACKGROUND_SIZE, tk);
                    if (v) {
                        nd_css_decl d = { .prop = ND_CSS_BACKGROUND_SIZE, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                } else if (g_ascii_strcasecmp(tk, "center") == 0 ||
                           g_ascii_strcasecmp(tk, "left")   == 0 ||
                           g_ascii_strcasecmp(tk, "right")  == 0 ||
                           g_ascii_strcasecmp(tk, "top")    == 0 ||
                           g_ascii_strcasecmp(tk, "bottom") == 0) {
                    if (!pos_x) pos_x = tk;
                    else if (!pos_y) pos_y = tk;
                } else {
                    nd_css_value *v = parse_value_for(
                        pos_x ? ND_CSS_BACKGROUND_POSITION_Y
                              : ND_CSS_BACKGROUND_POSITION_X,
                        tk);
                    if (v && v->kind == ND_CSS_V_LENGTH) {
                        if (!pos_x) pos_x = tk;
                        else if (!pos_y) pos_y = tk;
                    }
                    nd_css_value_free(v);
                }
            }
            if (bg_size_text) {
                nd_css_value *v = parse_value_for(ND_CSS_BACKGROUND_SIZE, bg_size_text);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_BACKGROUND_SIZE, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
                g_free(bg_size_text);
            }
            if (pos_x) {
                if (!pos_y) {
                    if (g_ascii_strcasecmp(pos_x, "top") == 0 ||
                        g_ascii_strcasecmp(pos_x, "bottom") == 0) {
                        pos_y = pos_x;
                        pos_x = "center";
                    } else {
                        pos_y = "center";
                    }
                } else {
                    gboolean first_is_v =
                        g_ascii_strcasecmp(pos_x, "top") == 0 ||
                        g_ascii_strcasecmp(pos_x, "bottom") == 0;
                    gboolean second_is_h =
                        g_ascii_strcasecmp(pos_y, "left") == 0 ||
                        g_ascii_strcasecmp(pos_y, "right") == 0;
                    if (first_is_v && second_is_h) {
                        const char *tmp = pos_x;
                        pos_x = pos_y;
                        pos_y = tmp;
                    }
                }
                nd_css_value *vx =
                    parse_value_for(ND_CSS_BACKGROUND_POSITION_X, pos_x);
                if (vx) {
                    nd_css_decl d = { .prop = ND_CSS_BACKGROUND_POSITION_X,
                                      .value = vx, .important = important };
                    g_array_append_val(decls_out, d);
                }
                nd_css_value *vy =
                    parse_value_for(ND_CSS_BACKGROUND_POSITION_Y, pos_y);
                if (vy) {
                    nd_css_decl d = { .prop = ND_CSS_BACKGROUND_POSITION_Y,
                                      .value = vy, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            g_free(pos_x_owned);
            g_free(pos_y_owned);
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "background-position") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            const char *xs = NULL, *ys = NULL;
            if (n == 1) {
                xs = tokens[0];
                ys = (g_ascii_strcasecmp(tokens[0], "top") == 0 ||
                      g_ascii_strcasecmp(tokens[0], "bottom") == 0) ? tokens[0] : "center";
                if (g_ascii_strcasecmp(tokens[0], "top") == 0 ||
                    g_ascii_strcasecmp(tokens[0], "bottom") == 0) xs = "center";
            } else if (n >= 2) {
                xs = tokens[0];
                ys = tokens[1];
            }
            if (xs) {
                nd_css_value *v = parse_value_for(ND_CSS_BACKGROUND_POSITION_X, xs);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_BACKGROUND_POSITION_X, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (ys) {
                nd_css_value *v = parse_value_for(ND_CSS_BACKGROUND_POSITION_Y, ys);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_BACKGROUND_POSITION_Y, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "object-position") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            const char *xs = NULL, *ys = NULL;
            if (n == 1) {
                xs = tokens[0];
                ys = (g_ascii_strcasecmp(tokens[0], "top") == 0 ||
                      g_ascii_strcasecmp(tokens[0], "bottom") == 0) ? tokens[0] : "center";
                if (g_ascii_strcasecmp(tokens[0], "top") == 0 ||
                    g_ascii_strcasecmp(tokens[0], "bottom") == 0) xs = "center";
            } else if (n >= 2) {
                xs = tokens[0];
                ys = tokens[1];
                gboolean first_is_v =
                    g_ascii_strcasecmp(xs, "top") == 0 ||
                    g_ascii_strcasecmp(xs, "bottom") == 0;
                gboolean second_is_h =
                    g_ascii_strcasecmp(ys, "left") == 0 ||
                    g_ascii_strcasecmp(ys, "right") == 0;
                if (first_is_v && second_is_h) {
                    const char *tmp = xs;
                    xs = ys;
                    ys = tmp;
                }
            }
            if (xs) {
                nd_css_value *v = parse_value_for(ND_CSS_OBJECT_POSITION_X, xs);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_OBJECT_POSITION_X, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (ys) {
                nd_css_value *v = parse_value_for(ND_CSS_OBJECT_POSITION_Y, ys);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_OBJECT_POSITION_Y, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "grid-template") == 0 ||
            strcmp(pname, "grid") == 0) {
            const char *slash = NULL;
            int depth = 0;
            for (const char *q = vtext; *q; q++) {
                if (*q == '(') depth++;
                else if (*q == ')') { if (depth > 0) depth--; }
                else if (*q == '/' && depth == 0) { slash = q; break; }
            }
            char *rows_part = NULL, *cols_part = NULL;
            if (slash) {
                rows_part = g_strndup(vtext, (gsize)(slash - vtext));
                cols_part = g_strdup(slash + 1);
            } else {
                rows_part = g_strdup(vtext);
            }
            char *rows_trim = rows_part ? g_strstrip(g_strdup(rows_part)) : NULL;
            char *cols_trim = cols_part ? g_strstrip(g_strdup(cols_part)) : NULL;
            char *areas_acc = NULL;
            if (rows_trim) {
                GString *areas = g_string_new(NULL);
                GString *rows_only = g_string_new(NULL);
                const char *q = rows_trim;
                while (*q) {
                    while (*q && is_ws(*q)) q++;
                    if (*q == '"' || *q == '\'') {
                        char qc = *q++;
                        const char *s = q;
                        while (*q && *q != qc) q++;
                        gsize slen = (gsize)(q - s);
                        if (areas->len) g_string_append_c(areas, ' ');
                        g_string_append_c(areas, '"');
                        g_string_append_len(areas, s, slen);
                        g_string_append_c(areas, '"');
                        if (*q == qc) q++;
                        while (*q && is_ws(*q)) q++;
                        const char *tstart = q;
                        while (*q && *q != '"' && *q != '\'' && *q != '/') q++;
                        gsize tlen = (gsize)(q - tstart);
                        while (tlen > 0 && is_ws(tstart[tlen - 1])) tlen--;
                        if (tlen > 0) {
                            if (rows_only->len) g_string_append_c(rows_only, ' ');
                            g_string_append_len(rows_only, tstart, tlen);
                        }
                    } else {
                        const char *tstart = q;
                        while (*q && *q != '"' && *q != '\'') q++;
                        gsize tlen = (gsize)(q - tstart);
                        while (tlen > 0 && is_ws(tstart[tlen - 1])) tlen--;
                        if (tlen > 0) {
                            if (rows_only->len) g_string_append_c(rows_only, ' ');
                            g_string_append_len(rows_only, tstart, tlen);
                        }
                    }
                }
                if (areas->len > 0) areas_acc = g_string_free(areas, FALSE);
                else g_string_free(areas, TRUE);
                g_free(rows_trim);
                rows_trim = g_string_free(rows_only, FALSE);
            }
            if (areas_acc && *areas_acc) {
                nd_css_value *v = parse_value_for(ND_CSS_GRID_TEMPLATE_AREAS, areas_acc);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_GRID_TEMPLATE_AREAS,
                                      .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (rows_trim && *rows_trim) {
                nd_css_value *v = parse_value_for(ND_CSS_GRID_TEMPLATE_ROWS, rows_trim);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_GRID_TEMPLATE_ROWS,
                                      .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (cols_trim && *cols_trim) {
                nd_css_value *v = parse_value_for(ND_CSS_GRID_TEMPLATE_COLUMNS, cols_trim);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_GRID_TEMPLATE_COLUMNS,
                                      .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            g_free(areas_acc);
            g_free(rows_trim);
            g_free(cols_trim);
            g_free(rows_part);
            g_free(cols_part);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "gap") == 0 || strcmp(pname, "grid-gap") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            const char *row = n >= 1 ? tokens[0] : NULL;
            const char *col = n >= 2 ? tokens[1] : row;
            if (row) {
                nd_css_value *v = parse_value_for(ND_CSS_ROW_GAP, row);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_ROW_GAP, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (col) {
                nd_css_value *v = parse_value_for(ND_CSS_COLUMN_GAP, col);
                if (v) {
                    nd_css_decl d = { .prop = ND_CSS_COLUMN_GAP, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "place-items") == 0 ||
            strcmp(pname, "place-self") == 0 ||
            strcmp(pname, "place-content") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            const char *first  = n >= 1 ? tokens[0] : NULL;
            const char *second = n >= 2 ? tokens[1] : first;
            if (strcmp(pname, "place-content") == 0) {
                if (first) {
                    nd_css_value *v = parse_value_for(ND_CSS_ALIGN_CONTENT, first);
                    if (v) {
                        nd_css_decl d = { .prop = ND_CSS_ALIGN_CONTENT, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                }
                if (second) {
                    nd_css_value *v = parse_value_for(ND_CSS_JUSTIFY_CONTENT, second);
                    if (v) {
                        nd_css_decl d = { .prop = ND_CSS_JUSTIFY_CONTENT, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                }
            } else {
                gboolean is_items = (strcmp(pname, "place-items") == 0);
                nd_css_prop ap = is_items ? ND_CSS_ALIGN_ITEMS : ND_CSS_ALIGN_SELF;
                nd_css_prop jp = is_items ? ND_CSS_JUSTIFY_ITEMS
                                          : ND_CSS_JUSTIFY_SELF;
                if (first) {
                    nd_css_value *v = parse_value_for(ap, first);
                    if (v) {
                        nd_css_decl d = { .prop = ap, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                }
                if (second) {
                    nd_css_value *v = parse_value_for(jp, second);
                    if (v) {
                        nd_css_decl d = { .prop = jp, .value = v, .important = important };
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

        if (strcmp(pname, "columns") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            for (int i = 0; i < n; i++) {
                double num; nd_css_unit u;
                if (parse_length(tokens[i], &num, &u)) {
                    nd_css_prop prop = (u == ND_CSS_UNIT_NUMBER)
                        ? ND_CSS_COLUMN_COUNT : ND_CSS_COLUMN_WIDTH;
                    nd_css_value *v = parse_value_for(prop, tokens[i]);
                    if (v) {
                        nd_css_decl d = { .prop = prop, .value = v,
                                          .important = important };
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

        if (strcmp(pname, "outline") == 0 ||
            strcmp(pname, "column-rule") == 0) {
            gboolean is_outline = (strcmp(pname, "outline") == 0);
            nd_css_prop p_w = is_outline ? ND_CSS_OUTLINE_WIDTH : ND_CSS_COLUMN_RULE_WIDTH;
            nd_css_prop p_s = is_outline ? ND_CSS_OUTLINE_STYLE : ND_CSS_COLUMN_RULE_STYLE;
            nd_css_prop p_c = is_outline ? ND_CSS_OUTLINE_COLOR : ND_CSS_COLUMN_RULE_COLOR;
            char *tokens[8] = {0};
            int n = split_ws(vtext, tokens);
            for (int i = 0; i < n; i++) {
                guint8 r, g, b, a;
                double num; nd_css_unit u;
                if (parse_color(tokens[i], &r, &g, &b, &a)) {
                    nd_css_value *v = parse_value_for(p_c, tokens[i]);
                    if (v) {
                        nd_css_decl d = { .prop = p_c, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                } else if (parse_length(tokens[i], &num, &u)) {
                    nd_css_value *v = parse_value_for(p_w, tokens[i]);
                    if (v) {
                        nd_css_decl d = { .prop = p_w, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                } else {
                    nd_css_value *v = parse_value_for(p_s, tokens[i]);
                    if (v) {
                        nd_css_decl d = { .prop = p_s, .value = v, .important = important };
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

        if (strcmp(pname, "text-decoration") == 0) {
            char *tokens[8] = {0};
            int n = split_ws(vtext, tokens);
            GString *lines = g_string_new(NULL);
            for (int i = 0; i < n; i++) {
                const char *tk = tokens[i];
                if (!tk) continue;
                guint8 cr, cg, cb, ca;
                if (g_ascii_strcasecmp(tk, "underline") == 0 ||
                    g_ascii_strcasecmp(tk, "overline")  == 0 ||
                    g_ascii_strcasecmp(tk, "line-through") == 0 ||
                    g_ascii_strcasecmp(tk, "none") == 0) {
                    if (lines->len > 0) g_string_append_c(lines, ' ');
                    char *low = g_ascii_strdown(tk, -1);
                    g_string_append(lines, low);
                    g_free(low);
                } else if (g_ascii_strcasecmp(tk, "solid")  == 0 ||
                           g_ascii_strcasecmp(tk, "double") == 0 ||
                           g_ascii_strcasecmp(tk, "dotted") == 0 ||
                           g_ascii_strcasecmp(tk, "dashed") == 0 ||
                           g_ascii_strcasecmp(tk, "wavy")   == 0) {
                    nd_css_value *v = g_new0(nd_css_value, 1);
                    v->kind = ND_CSS_V_KEYWORD;
                    v->u.keyword = g_ascii_strdown(tk, -1);
                    nd_css_decl d = {
                        .prop = ND_CSS_TEXT_DECORATION_STYLE,
                        .value = v, .important = important
                    };
                    g_array_append_val(decls_out, d);
                } else if (parse_color(tk, &cr, &cg, &cb, &ca)) {
                    nd_css_value *v = g_new0(nd_css_value, 1);
                    v->kind = ND_CSS_V_COLOR;
                    v->u.color.r = cr; v->u.color.g = cg;
                    v->u.color.b = cb; v->u.color.a = ca;
                    nd_css_decl d = {
                        .prop = ND_CSS_TEXT_DECORATION_COLOR,
                        .value = v, .important = important
                    };
                    g_array_append_val(decls_out, d);
                }
            }
            if (lines->len > 0) {
                nd_css_value *v = g_new0(nd_css_value, 1);
                v->kind = ND_CSS_V_KEYWORD;
                v->u.keyword = g_string_free(lines, FALSE);
                lines = NULL;
                nd_css_decl d = {
                    .prop = ND_CSS_TEXT_DECORATION,
                    .value = v, .important = important
                };
                g_array_append_val(decls_out, d);
            }
            if (lines) g_string_free(lines, TRUE);
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "font") == 0) {
            nd_css_value *wide = parse_css_wide_keyword(vtext);
            if (wide) {
                const nd_css_prop props[] = {
                    ND_CSS_FONT_STYLE,
                    ND_CSS_FONT_VARIANT,
                    ND_CSS_FONT_WEIGHT,
                    ND_CSS_FONT_SIZE,
                    ND_CSS_LINE_HEIGHT,
                    ND_CSS_FONT_FAMILY,
                };
                for (gsize i = 0; i < G_N_ELEMENTS(props); i++) {
                    nd_css_decl d = {
                        .prop = props[i],
                        .value = nd_css_value_dup(wide),
                        .important = important
                    };
                    g_array_append_val(decls_out, d);
                }
                nd_css_value_free(wide);
                g_free(pname);
                g_free(vtext);
                if (p < end && *p == ';') p++;
                continue;
            }
            char *tokens[16] = {0};
            int n = split_ws(vtext, tokens);
            char *family_buf = NULL;
            int size_idx = -1;
            for (int i = 0; i < n; i++) {
                double num, lh;
                nd_css_unit u, lu;
                gboolean has_lh = FALSE;
                if (parse_font_size_token(tokens[i], &num, &u,
                                          &lh, &lu, &has_lh)) {
                    size_idx = i;
                    break;
                }
            }
            int prefix_end = size_idx >= 0 ? size_idx : n;
            for (int i = 0; i < prefix_end; i++) {
                const char *t = tokens[i];
                nd_css_prop prop = ND_CSS_PROP_COUNT;
                const char *kw = NULL;
                if (g_ascii_strcasecmp(t, "italic") == 0 ||
                    g_ascii_strcasecmp(t, "oblique") == 0) {
                    prop = ND_CSS_FONT_STYLE; kw = "italic";
                } else if (g_ascii_strcasecmp(t, "bold")    == 0 ||
                           g_ascii_strcasecmp(t, "bolder")  == 0 ||
                           g_ascii_strcasecmp(t, "lighter") == 0) {
                    prop = ND_CSS_FONT_WEIGHT; kw = t;
                } else if (g_ascii_isdigit(t[0])) {
                    double num; nd_css_unit u;
                    if (parse_length(t, &num, &u) &&
                        u == ND_CSS_UNIT_NUMBER &&
                        num >= 100 && num <= 900) {
                        prop = ND_CSS_FONT_WEIGHT; kw = t;
                    }
                } else if (g_ascii_strcasecmp(t, "small-caps") == 0) {
                    prop = ND_CSS_FONT_VARIANT; kw = "small-caps";
                }
                if (prop != ND_CSS_PROP_COUNT) {
                    nd_css_value *v = g_new0(nd_css_value, 1);
                    v->kind = ND_CSS_V_KEYWORD;
                    v->u.keyword = g_ascii_strdown(kw, -1);
                    nd_css_decl d = {
                        .prop = prop, .value = v, .important = important
                    };
                    g_array_append_val(decls_out, d);
                }
            }
            if (size_idx >= 0) {
                char *size_tok = tokens[size_idx];
                double num = 0, lh = 0;
                nd_css_unit u = ND_CSS_UNIT_PX, lu = ND_CSS_UNIT_NUMBER;
                gboolean has_lh = FALSE;
                parse_font_size_token(size_tok, &num, &u, &lh, &lu, &has_lh);
                int family_start = size_idx + 1;
                char *slash = strchr(size_tok, '/');
                if (!has_lh && slash && !slash[1] && size_idx + 1 < n) {
                    if (parse_length(tokens[size_idx + 1], &lh, &lu)) {
                        has_lh = TRUE;
                        family_start = size_idx + 2;
                    }
                } else if (!has_lh && size_idx + 1 < n &&
                           tokens[size_idx + 1][0] == '/') {
                    const char *lh_text = tokens[size_idx + 1] + 1;
                    if (*lh_text && parse_length(lh_text, &lh, &lu)) {
                        has_lh = TRUE;
                        family_start = size_idx + 2;
                    } else if (!*lh_text && size_idx + 2 < n &&
                               parse_length(tokens[size_idx + 2], &lh, &lu)) {
                        has_lh = TRUE;
                        family_start = size_idx + 3;
                    }
                } else if (has_lh) {
                    family_start = size_idx + 1;
                }
                nd_css_value *v = g_new0(nd_css_value, 1);
                v->kind = ND_CSS_V_LENGTH;
                v->u.length.v = num;
                v->u.length.unit = u;
                nd_css_decl d = {
                    .prop = ND_CSS_FONT_SIZE, .value = v,
                    .important = important
                };
                g_array_append_val(decls_out, d);
                if (has_lh) {
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
                if (family_start < n) {
                    GString *fam = g_string_new(NULL);
                    for (int j = family_start; j < n; j++) {
                        if (j > family_start) g_string_append_c(fam, ' ');
                        g_string_append(fam, tokens[j]);
                    }
                    family_buf = g_string_free(fam, FALSE);
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
                    if (numerics == 0) {
                        grow = 1; shrink = 1;
                    }
                    g_free(basis);
                    basis = g_strdup("auto"); basis_set = TRUE;
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
            if (numerics >= 1 && !basis_set) {
                basis = g_strdup("0");
                basis_set = TRUE;
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
                "lower-roman", "upper-roman", "lower-greek",
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
                if (g_ascii_strcasecmp(tokens[i], "inside") == 0 ||
                    g_ascii_strcasecmp(tokens[i], "outside") == 0) {
                    nd_css_value *v = g_new0(nd_css_value, 1);
                    v->kind = ND_CSS_V_KEYWORD;
                    v->u.keyword = g_ascii_strdown(tokens[i], -1);
                    nd_css_decl d = {
                        .prop = ND_CSS_LIST_STYLE_POSITION, .value = v,
                        .important = important
                    };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "border-radius") == 0) {
            char *vtext_main = vtext;
            char *slash = strchr(vtext_main, '/');
            if (slash) *slash = '\0';
            char *tokens[4] = {0};
            int n = split_ws(vtext_main, tokens);
            if (n > 0) {
                const char *tl = tokens[0];
                const char *tr = n >= 2 ? tokens[1] : tl;
                const char *br = n >= 3 ? tokens[2] : tl;
                const char *bl = n >= 4 ? tokens[3] : tr;
                const struct { nd_css_prop p; const char *v; } map[] = {
                    { ND_CSS_BORDER_TOP_LEFT_RADIUS,     tl },
                    { ND_CSS_BORDER_TOP_RIGHT_RADIUS,    tr },
                    { ND_CSS_BORDER_BOTTOM_RIGHT_RADIUS, br },
                    { ND_CSS_BORDER_BOTTOM_LEFT_RADIUS,  bl },
                };
                for (int i = 0; i < 4; i++) {
                    nd_css_value *vv = parse_value_for(map[i].p, map[i].v);
                    if (!vv) continue;
                    nd_css_decl d = { .prop = map[i].p, .value = vv, .important = important };
                    g_array_append_val(decls_out, d);
                }
                nd_css_value *legacy = parse_value_for(ND_CSS_BORDER_RADIUS, tl);
                if (legacy) {
                    nd_css_decl d = { .prop = ND_CSS_BORDER_RADIUS, .value = legacy, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "margin-block") == 0 ||
            strcmp(pname, "margin-inline") == 0 ||
            strcmp(pname, "padding-block") == 0 ||
            strcmp(pname, "padding-inline") == 0 ||
            strcmp(pname, "inset-block") == 0 ||
            strcmp(pname, "inset-inline") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            if (n > 2) {
                for (int i = 2; i < n; i++) g_free(tokens[i]);
                n = 2;
            }
            if (n > 0) {
                const char *a = tokens[0];
                const char *b = n >= 2 ? tokens[1] : a;
                nd_css_prop pa = ND_CSS_MARGIN_TOP, pb = ND_CSS_MARGIN_BOTTOM;
                if (strcmp(pname, "margin-block") == 0) {
                    pa = ND_CSS_MARGIN_TOP; pb = ND_CSS_MARGIN_BOTTOM;
                } else if (strcmp(pname, "margin-inline") == 0) {
                    pa = ND_CSS_MARGIN_LEFT; pb = ND_CSS_MARGIN_RIGHT;
                } else if (strcmp(pname, "padding-block") == 0) {
                    pa = ND_CSS_PADDING_TOP; pb = ND_CSS_PADDING_BOTTOM;
                } else if (strcmp(pname, "padding-inline") == 0) {
                    pa = ND_CSS_PADDING_LEFT; pb = ND_CSS_PADDING_RIGHT;
                } else if (strcmp(pname, "inset-block") == 0) {
                    pa = ND_CSS_TOP; pb = ND_CSS_BOTTOM;
                } else {
                    pa = ND_CSS_LEFT; pb = ND_CSS_RIGHT;
                }
                nd_css_value *va = parse_value_for(pa, a);
                nd_css_value *vb = parse_value_for(pb, b);
                if (va) {
                    nd_css_decl d = { .prop = pa, .value = va, .important = important };
                    g_array_append_val(decls_out, d);
                }
                if (vb) {
                    nd_css_decl d = { .prop = pb, .value = vb, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "inset") == 0) {
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            if (n > 0) {
                emit_quad(decls_out,
                    ND_CSS_TOP, ND_CSS_RIGHT,
                    ND_CSS_BOTTOM, ND_CSS_LEFT,
                    tokens, n, important);
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "text-wrap") == 0 ||
            strcmp(pname, "text-wrap-mode") == 0) {
            const char *mapped = NULL;
            char *kw = g_ascii_strdown(vtext, -1);
            g_strstrip(kw);
            if (strcmp(kw, "nowrap") == 0)
                mapped = "nowrap";
            else if (strcmp(kw, "wrap") == 0 ||
                     strcmp(kw, "balance") == 0 ||
                     strcmp(kw, "pretty") == 0 ||
                     strcmp(kw, "stable") == 0)
                mapped = "normal";
            if (mapped) {
                nd_css_value *vv = parse_value_for(ND_CSS_WHITE_SPACE, mapped);
                if (vv) {
                    nd_css_decl d = { .prop = ND_CSS_WHITE_SPACE, .value = vv, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            g_free(kw);
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
        } else if (strcmp(pname, "container") == 0) {
            char *slash = strchr(vtext, '/');
            char *name_part = slash ? g_strndup(vtext, (gsize)(slash - vtext))
                                    : g_strdup(vtext);
            g_strstrip(name_part);
            nd_css_value *nv = parse_value_for(ND_CSS_CONTAINER_NAME, name_part);
            if (nv) {
                nd_css_decl d = { .prop = ND_CSS_CONTAINER_NAME, .value = nv,
                                  .important = important };
                g_array_append_val(decls_out, d);
            }
            g_free(name_part);
            if (slash) {
                char *type_part = g_strstrip(g_strdup(slash + 1));
                nd_css_value *tv = *type_part
                    ? parse_value_for(ND_CSS_CONTAINER_TYPE, type_part) : NULL;
                if (tv) {
                    nd_css_decl d = { .prop = ND_CSS_CONTAINER_TYPE, .value = tv,
                                      .important = important };
                    g_array_append_val(decls_out, d);
                }
                g_free(type_part);
            }
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
pending_decl_clear(gpointer data)
{
    nd_css_pending_decl *pd = data;
    g_free(pd->pname);
    g_free(pd->raw_vtext);
}

static void
nd_css_scope_free(nd_css_scope *s)
{
    if (!s) return;
    if (s->roots) g_ptr_array_free(s->roots, TRUE);
    if (s->limits) g_ptr_array_free(s->limits, TRUE);
    g_free(s);
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
    if (r->vars) g_hash_table_destroy(r->vars);
    if (r->var_important) g_hash_table_destroy(r->var_important);
    if (r->pending) g_array_free(r->pending, TRUE);
    g_free(r->layer_name);
    g_free(r->container_condition);
    if (r->scopes) g_ptr_array_free(r->scopes, TRUE);
    g_free(r);
}

static const char *
css_skip_comment(const char *p, const char *end)
{
    if (p + 1 >= end || p[0] != '/' || p[1] != '*') return p;
    p += 2;
    while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
    return p + 1 < end ? p + 2 : end;
}

static const char *
css_skip_ws_comments(const char *p, const char *end)
{
    for (;;) {
        while (p < end && is_ws(*p)) p++;
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p = css_skip_comment(p, end);
            continue;
        }
        return p;
    }
}

static const char *
css_scan_until(const char *p, const char *end,
               const char *terminators, char *terminator)
{
    char quote = 0;
    int paren = 0, bracket = 0, brace = 0;
    if (terminator) *terminator = 0;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (c == quote) quote = 0;
            else if (c == '\n' || c == '\r' || c == '\f') quote = 0;
            p++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            p = css_skip_comment(p, end);
            continue;
        }
        if (c == '\\' && p + 1 < end) {
            p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            p++;
            continue;
        }
        if (paren == 0 && bracket == 0 && brace == 0 &&
            strchr(terminators, c)) {
            if (terminator) *terminator = c;
            return p;
        }
        if (c == '(') paren++;
        else if (c == ')' && paren > 0) paren--;
        else if (c == '[') bracket++;
        else if (c == ']' && bracket > 0) bracket--;
        else if (c == '{') brace++;
        else if (c == '}' && brace > 0) brace--;
        p++;
    }
    return p;
}

static const char *
css_scan_segment(const char *p, const char *end, char *terminator)
{
    return css_scan_until(p, end, "{;}", terminator);
}

static const char *
css_scan_declaration_value(const char *p, const char *end, char *terminator)
{
    return css_scan_until(p, end, ";}", terminator);
}

static const char *
css_skip_to_block_end(const char *p, const char *end)
{
    int depth = 0;
    char quote = 0;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (c == quote) quote = 0;
            else if (c == '\n' || c == '\r' || c == '\f') quote = 0;
            p++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            p = css_skip_comment(p, end);
            continue;
        }
        if (c == '\\' && p + 1 < end) {
            p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            p++;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth <= 0) return p + 1;
        }
        p++;
    }
    return end;
}

static const char *
css_block_body_end(const char *body_start, const char *block_end)
{
    return block_end > body_start && block_end[-1] == '}' ? block_end - 1
                                                          : block_end;
}

static const char *
css_find_top_level_char(const char *p, const char *end, char needle)
{
    char terms[2] = { needle, 0 };
    char term = 0;
    const char *q = css_scan_until(p, end, terms, &term);
    return term == needle ? q : NULL;
}

static const char *
css_find_function(const char *p, const char *end, const char *name)
{
    gsize n = strlen(name);
    const char *start = p;
    char quote = 0;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (c == quote) quote = 0;
            else if (c == '\n' || c == '\r' || c == '\f') quote = 0;
            p++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            p = css_skip_comment(p, end);
            continue;
        }
        if (c == '\\' && p + 1 < end) {
            p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            p++;
            continue;
        }
        if ((gsize)(end - p) > n && p[n] == '(' &&
            g_ascii_strncasecmp(p, name, n) == 0 &&
            (p == start || !is_ident(p[-1])))
            return p;
        p++;
    }
    return NULL;
}

static void
css_strip_important(char *text, gboolean *important)
{
    if (important) *important = FALSE;
    if (!text) return;
    const char *start = text;
    const char *end = text + strlen(text);
    const char *p = start;
    const char *bang = NULL;
    while (p < end) {
        const char *q = css_find_top_level_char(p, end, '!');
        if (!q) break;
        bang = q;
        p = q + 1;
    }
    if (!bang) return;
    const char *tail = css_skip_ws_comments(bang + 1, end);
    if ((gsize)(end - tail) < 9 ||
        g_ascii_strncasecmp(tail, "important", 9) != 0)
        return;
    const char *after = tail + 9;
    if (after < end && is_ident(*after)) return;
    after = css_skip_ws_comments(after, end);
    if (after != end) return;
    *((char *)bang) = '\0';
    g_strchomp(text);
    if (important) *important = TRUE;
}

static void
font_face_clear(gpointer data)
{
    nd_css_font_face *ff = data;
    g_free(ff->family);
    g_free(ff->src_url);
}

static void
property_rule_clear(gpointer data)
{
    nd_css_property_rule *pr = data;
    g_free(pr->name);
    g_free(pr->initial_value);
}

static gboolean
font_url_suffix_eq(const char *url, const char *end, const char *suffix)
{
    gsize n = strlen(suffix);
    return (gsize)(end - url) >= n &&
           g_ascii_strncasecmp(end - n, suffix, n) == 0;
}

static int
font_src_score(const char *url)
{
    if (!url || !*url) return -1;
    if (g_str_has_prefix(url, "data:")) {
        if (strstr(url, "font/woff2")) return 80;
        if (strstr(url, "font/woff"))  return 70;
        if (strstr(url, "font/"))      return 40;
        return 20;
    }
    const char *end = url + strlen(url);
    const char *q = strchr(url, '?');
    const char *h = strchr(url, '#');
    if (q && q < end) end = q;
    if (h && h < end) end = h;
    if (font_url_suffix_eq(url, end, ".woff2")) return 80;
    if (font_url_suffix_eq(url, end, ".woff"))  return 70;
    if (font_url_suffix_eq(url, end, ".otf"))   return 60;
    if (font_url_suffix_eq(url, end, ".ttf"))   return 60;
    if (font_url_suffix_eq(url, end, ".ttc"))   return 60;
    if (font_url_suffix_eq(url, end, ".eot"))   return -1;
    if (font_url_suffix_eq(url, end, ".svg"))   return -1;
    return 10;
}

static void
font_src_consider(char **best, const char *start, gsize len)
{
    if (!best || !start || len == 0) return;
    char *candidate = g_strndup(start, len);
    int score = font_src_score(candidate);
    if (score < 0) {
        g_free(candidate);
        return;
    }
    int old_score = *best ? font_src_score(*best) : -1;
    if (!*best || score > old_score) {
        g_free(*best);
        *best = candidate;
    } else {
        g_free(candidate);
    }
}

static void
font_src_consider_urls(char **best, const char *value)
{
    const char *p = value;
    const char *end = value + strlen(value);
    while (p < end) {
        if (p + 4 <= end && g_ascii_strncasecmp(p, "url(", 4) == 0) {
            p += 4;
            p = css_skip_ws_comments(p, end);
            char quote = 0;
            if (p < end && (*p == '"' || *p == '\'')) {
                quote = *p;
                p++;
            }
            const char *start = p;
            if (quote) {
                while (p < end) {
                    if (*p == '\\' && p + 1 < end) p += 2;
                    else if (*p == quote) break;
                    else p++;
                }
            } else {
                while (p < end && *p != ')' && !is_ws(*p)) {
                    if (*p == '\\' && p + 1 < end) p += 2;
                    else p++;
                }
            }
            if (p > start) font_src_consider(best, start, (gsize)(p - start));
            while (p < end && *p != ')') p++;
            if (p < end) p++;
            continue;
        }
        if ((*p == '"' || *p == '\'')) {
            char q = *p++;
            while (p < end) {
                if (*p == '\\' && p + 1 < end) p += 2;
                else if (*p++ == q) break;
            }
        } else if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p = css_skip_comment(p, end);
        } else {
            p++;
        }
    }
}

static char *
css_keyframes_name_from_range(const char *start, const char *end)
{
    char *name = css_trim_dup_range(start, end);
    if (!name || !*name) return name;
    gsize n = strlen(name);
    if (n >= 2 && (name[0] == '"' || name[0] == '\'') && name[n - 1] == name[0]) {
        GString *out = g_string_new(NULL);
        for (gsize i = 1; i + 1 < n; i++) {
            if (name[i] == '\\' && i + 1 < n - 1) i++;
            g_string_append_c(out, name[i]);
        }
        g_free(name);
        name = g_string_free(out, FALSE);
    }
    return name;
}

static void
keyframes_clear(gpointer data)
{
    nd_css_keyframes *kf = data;
    g_free(kf->name);
    g_free(kf->stops);
}

static int
keyframe_stop_cmp(gconstpointer a, gconstpointer b)
{
    double da = ((const nd_css_keyframe_stop *)a)->pct;
    double db = ((const nd_css_keyframe_stop *)b)->pct;
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

static gboolean
parse_keyframe_stop_pct(const char *sel, double *out_pct)
{
    while (*sel == ' ') sel++;
    if (g_ascii_strcasecmp(sel, "from") == 0) { *out_pct = 0;   return TRUE; }
    if (g_ascii_strcasecmp(sel, "to")   == 0) { *out_pct = 100; return TRUE; }
    char *end = NULL;
    double v = g_ascii_strtod(sel, &end);
    if (end == sel) return FALSE;
    while (*end == ' ') end++;
    if (*end != '%' && *end != '\0') return FALSE;
    *out_pct = v;
    return TRUE;
}

static void
skip_at_rule(const char **pp, const char *end)
{
    const char *p = *pp;
    char term = 0;
    const char *seg = css_scan_segment(p, end, &term);
    if (term == ';') *pp = seg + 1;
    else if (term == '{') *pp = css_skip_to_block_end(seg, end);
    else *pp = seg;
}

static nd_css_color_scheme g_color_scheme = ND_CSS_COLOR_SCHEME_LIGHT;
static nd_css_reduced_motion g_reduced_motion = ND_CSS_REDUCED_MOTION_NO_PREFERENCE;

void
nd_css_set_color_scheme(nd_css_color_scheme s)
{
    g_color_scheme = s;
}

void
nd_css_set_reduced_motion(nd_css_reduced_motion m)
{
    g_reduced_motion = m;
}

nd_css_reduced_motion
nd_css_get_reduced_motion(void)
{
    return g_reduced_motion;
}

static double
media_length_px(const char *text, double pct_basis)
{
    if (!text) return -1;
    double px = 0, pct = 0;
    if (!resolve_to_px_pct(text, strlen(text), &px, &pct)) return -1;
    return px + pct * 0.01 * pct_basis;
}

static double
media_ratio_value(const char *text)
{
    if (!text) return -1;
    char *s = g_strstrip(g_strdup(text));
    char *slash = strchr(s, '/');
    char *end_a = NULL;
    double a = g_ascii_strtod(s, &end_a);
    if (!end_a || end_a == s || a <= 0) {
        g_free(s);
        return -1;
    }
    double b = 1.0;
    if (slash) {
        char *end_b = NULL;
        char *bs = slash + 1;
        while (*bs && is_ws(*bs)) bs++;
        b = g_ascii_strtod(bs, &end_b);
        if (!end_b || end_b == bs || b <= 0) {
            g_free(s);
            return -1;
        }
    }
    g_free(s);
    return a / b;
}

static double
media_resolution_dppx(const char *text)
{
    if (!text) return -1;
    char *s = g_strstrip(g_strdup(text));
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (!end || end == s || v < 0) {
        g_free(s);
        return -1;
    }
    while (*end && is_ws(*end)) end++;
    double out = -1;
    if (g_ascii_strcasecmp(end, "dppx") == 0 ||
        g_ascii_strcasecmp(end, "x") == 0)
        out = v;
    else if (g_ascii_strcasecmp(end, "dpi") == 0)
        out = v / 96.0;
    else if (g_ascii_strcasecmp(end, "dpcm") == 0)
        out = v * 2.54 / 96.0;
    g_free(s);
    return out;
}

typedef enum media_feature_kind {
    MEDIA_FEATURE_LENGTH,
    MEDIA_FEATURE_RATIO,
    MEDIA_FEATURE_RESOLUTION,
} media_feature_kind;

static gboolean
media_feature_value(const char *name, double *out, media_feature_kind *kind,
                    double *basis)
{
    if (!name || !out || !kind || !basis) return FALSE;
    if (g_ascii_strcasecmp(name, "width") == 0 ||
        g_ascii_strcasecmp(name, "device-width") == 0 ||
        g_ascii_strcasecmp(name, "inline-size") == 0) {
        *out = g_viewport_w;
        *basis = g_viewport_w;
        *kind = MEDIA_FEATURE_LENGTH;
        return TRUE;
    }
    if (g_ascii_strcasecmp(name, "height") == 0 ||
        g_ascii_strcasecmp(name, "device-height") == 0 ||
        g_ascii_strcasecmp(name, "block-size") == 0) {
        *out = g_viewport_h;
        *basis = g_viewport_h;
        *kind = MEDIA_FEATURE_LENGTH;
        return TRUE;
    }
    if (g_ascii_strcasecmp(name, "aspect-ratio") == 0) {
        *out = g_viewport_h > 0 ? g_viewport_w / g_viewport_h : 0;
        *basis = *out;
        *kind = MEDIA_FEATURE_RATIO;
        return TRUE;
    }
    if (g_ascii_strcasecmp(name, "resolution") == 0) {
        *out = 1.0;
        *basis = 1.0;
        *kind = MEDIA_FEATURE_RESOLUTION;
        return TRUE;
    }
    return FALSE;
}

static double
media_feature_compare_value(const char *text, media_feature_kind kind,
                            double basis)
{
    if (kind == MEDIA_FEATURE_RATIO) return media_ratio_value(text);
    if (kind == MEDIA_FEATURE_RESOLUTION) return media_resolution_dppx(text);
    return media_length_px(text, basis);
}

static gboolean
media_compare(double a, const char *op, double b)
{
    if (strcmp(op, "<") == 0)  return a < b;
    if (strcmp(op, "<=") == 0) return a <= b;
    if (strcmp(op, ">") == 0)  return a > b;
    if (strcmp(op, ">=") == 0) return a >= b;
    if (strcmp(op, "=") == 0)  return (int)(a + 0.5) == (int)(b + 0.5);
    return FALSE;
}

static const char *
media_find_cmp(const char *p, const char *end, char op[3])
{
    char term = 0;
    const char *q = css_scan_until(p, end, "<>=", &term);
    if (!term) return NULL;
    op[0] = *q;
    op[1] = '\0';
    op[2] = '\0';
    if ((*q == '<' || *q == '>') && q + 1 < end && q[1] == '=') {
        op[1] = '=';
        op[2] = '\0';
    }
    return q;
}

static gboolean
media_range_matches(const char *text)
{
    char *f = g_strdup(text);
    g_strstrip(f);
    const char *start = f;
    const char *end = f + strlen(f);
    char op1[3];
    const char *cmp1 = media_find_cmp(start, end, op1);
    if (!cmp1) { g_free(f); return FALSE; }
    const char *after1 = cmp1 + strlen(op1);
    char op2[3];
    const char *cmp2 = media_find_cmp(after1, end, op2);
    char *left = css_trim_dup_range(start, cmp1);
    gboolean ok = FALSE;
    double size = 0, basis = 0;
    media_feature_kind kind = MEDIA_FEATURE_LENGTH;
    if (cmp2) {
        char *middle = css_trim_dup_range(after1, cmp2);
        char *right = css_trim_dup_range(cmp2 + strlen(op2), end);
        if (media_feature_value(middle, &size, &kind, &basis)) {
            double a = media_feature_compare_value(left, kind, basis);
            double b = media_feature_compare_value(right, kind, basis);
            ok = a >= 0 && b >= 0 &&
                 media_compare(a, op1, size) &&
                 media_compare(size, op2, b);
        }
        g_free(middle);
        g_free(right);
    } else {
        char *right = css_trim_dup_range(after1, end);
        if (media_feature_value(left, &size, &kind, &basis)) {
            double v = media_feature_compare_value(right, kind, basis);
            ok = v >= 0 && media_compare(size, op1, v);
        } else if (media_feature_value(right, &size, &kind, &basis)) {
            double v = media_feature_compare_value(left, kind, basis);
            ok = v >= 0 && media_compare(v, op1, size);
        }
        g_free(right);
    }
    g_free(left);
    g_free(f);
    return ok;
}

static gboolean
media_feature_matches(const char *name, const char *value)
{
    if (!name) return FALSE;
    double vw = g_viewport_w;
    double vh = g_viewport_h;
    double n = value ? media_length_px(value, vw) : 0;
    if (g_ascii_strcasecmp(name, "max-width") == 0 ||
        g_ascii_strcasecmp(name, "max-device-width") == 0)
        return n >= 0 && vw <= n;
    if (g_ascii_strcasecmp(name, "min-width") == 0 ||
        g_ascii_strcasecmp(name, "min-device-width") == 0)
        return n >= 0 && vw >= n;
    n = value ? media_length_px(value, vh) : 0;
    if (g_ascii_strcasecmp(name, "max-height") == 0 ||
        g_ascii_strcasecmp(name, "max-device-height") == 0)
        return n >= 0 && vh <= n;
    if (g_ascii_strcasecmp(name, "min-height") == 0 ||
        g_ascii_strcasecmp(name, "min-device-height") == 0)
        return n >= 0 && vh >= n;
    if (g_ascii_strcasecmp(name, "aspect-ratio") == 0 ||
        g_ascii_strcasecmp(name, "min-aspect-ratio") == 0 ||
        g_ascii_strcasecmp(name, "max-aspect-ratio") == 0) {
        double current = vh > 0 ? vw / vh : 0;
        if (!value) return current > 0;
        double want = media_ratio_value(value);
        if (want < 0) return FALSE;
        if (g_ascii_strncasecmp(name, "min-", 4) == 0)
            return current >= want;
        if (g_ascii_strncasecmp(name, "max-", 4) == 0)
            return current <= want;
        return fabs(current - want) < 0.0001;
    }
    if (g_ascii_strcasecmp(name, "resolution") == 0 ||
        g_ascii_strcasecmp(name, "min-resolution") == 0 ||
        g_ascii_strcasecmp(name, "max-resolution") == 0) {
        double current = 1.0;
        if (!value) return TRUE;
        double want = media_resolution_dppx(value);
        if (want < 0) return FALSE;
        if (g_ascii_strncasecmp(name, "min-", 4) == 0)
            return current >= want;
        if (g_ascii_strncasecmp(name, "max-", 4) == 0)
            return current <= want;
        return fabs(current - want) < 0.0001;
    }
    if (g_ascii_strcasecmp(name, "orientation") == 0) {
        gboolean landscape = vw >= vh;
        if (!value) return TRUE;
        if (g_ascii_strcasecmp(value, "landscape") == 0) return landscape;
        if (g_ascii_strcasecmp(value, "portrait")  == 0) return !landscape;
        return FALSE;
    }
    if (g_ascii_strcasecmp(name, "prefers-color-scheme") == 0) {
        if (!value) return TRUE;
        if (g_ascii_strcasecmp(value, "dark") == 0)
            return g_color_scheme == ND_CSS_COLOR_SCHEME_DARK;
        if (g_ascii_strcasecmp(value, "light") == 0)
            return g_color_scheme == ND_CSS_COLOR_SCHEME_LIGHT;
        return FALSE;
    }
    if (g_ascii_strcasecmp(name, "prefers-reduced-motion") == 0) {
        if (!value) return g_reduced_motion == ND_CSS_REDUCED_MOTION_REDUCE;
        if (g_ascii_strcasecmp(value, "reduce") == 0)
            return g_reduced_motion == ND_CSS_REDUCED_MOTION_REDUCE;
        if (g_ascii_strcasecmp(value, "no-preference") == 0)
            return g_reduced_motion == ND_CSS_REDUCED_MOTION_NO_PREFERENCE;
        return FALSE;
    }
    if (g_ascii_strcasecmp(name, "hover") == 0)
        return !value || g_ascii_strcasecmp(value, "hover") == 0;
    if (g_ascii_strcasecmp(name, "any-hover") == 0)
        return !value || g_ascii_strcasecmp(value, "hover") == 0;
    if (g_ascii_strcasecmp(name, "pointer") == 0)
        return !value || g_ascii_strcasecmp(value, "fine") == 0;
    if (g_ascii_strcasecmp(name, "any-pointer") == 0)
        return !value || g_ascii_strcasecmp(value, "fine") == 0;
    if (g_ascii_strcasecmp(name, "prefers-contrast") == 0)
        return !value || g_ascii_strcasecmp(value, "no-preference") == 0;
    if (g_ascii_strcasecmp(name, "forced-colors") == 0)
        return !value || g_ascii_strcasecmp(value, "none") == 0;
    if (g_ascii_strcasecmp(name, "prefers-reduced-data") == 0) {
        if (!value) return FALSE;
        return g_ascii_strcasecmp(value, "no-preference") == 0;
    }
    if (g_ascii_strcasecmp(name, "inverted-colors") == 0) {
        if (!value) return FALSE;
        return g_ascii_strcasecmp(value, "none") == 0;
    }
    if (g_ascii_strcasecmp(name, "color-gamut") == 0) {
        if (!value) return TRUE;
        return g_ascii_strcasecmp(value, "srgb") == 0;
    }
    if (g_ascii_strcasecmp(name, "scripting") == 0) {
        if (!value) return TRUE;
        return g_ascii_strcasecmp(value, "enabled") == 0;
    }
    if (g_ascii_strcasecmp(name, "display-mode") == 0) {
        if (!value) return TRUE;
        return g_ascii_strcasecmp(value, "browser") == 0;
    }
    if (g_ascii_strcasecmp(name, "update") == 0) {
        if (!value) return TRUE;
        return g_ascii_strcasecmp(value, "fast") == 0;
    }
    if (g_ascii_strcasecmp(name, "dynamic-range") == 0 ||
        g_ascii_strcasecmp(name, "video-dynamic-range") == 0) {
        if (!value) return TRUE;
        return g_ascii_strcasecmp(value, "standard") == 0;
    }
    if (g_ascii_strcasecmp(name, "overflow-block") == 0 ||
        g_ascii_strcasecmp(name, "overflow-inline") == 0) {
        if (!value) return TRUE;
        return g_ascii_strcasecmp(value, "scroll") == 0;
    }
    if (g_ascii_strcasecmp(name, "grid") == 0)
        return !value || strcmp(value, "0") == 0;
    if (g_ascii_strcasecmp(name, "monochrome") == 0)
        return !value || strcmp(value, "0") == 0;
    if (g_ascii_strcasecmp(name, "color") == 0)
        return !value || strcmp(value, "0") != 0;
    return FALSE;
}

static gboolean
media_feature_expr_matches(const char *src, gsize len)
{
    char *s = g_strndup(src, len);
    g_strstrip(s);
    const char *end = s + strlen(s);
    char cmp[3];
    if (media_find_cmp(s, end, cmp)) {
        gboolean ok = media_range_matches(s);
        g_free(s);
        return ok;
    }
    char *colon = (char *)css_find_top_level_char(s, end, ':');
    if (colon) {
        *colon = '\0';
        char *name = g_strstrip(s);
        char *value = g_strstrip(colon + 1);
        gboolean ok = media_feature_matches(name, value);
        g_free(s);
        return ok;
    }
    gboolean ok = media_feature_matches(s, NULL);
    g_free(s);
    return ok;
}

static const char *
media_find_top_level_or(const char *p, const char *end)
{
    const char *start = p;
    char quote = 0;
    int depth = 0;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p + 1 < end) { p += 2; continue; }
            if (c == quote) quote = 0;
            p++;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; p++; continue; }
        if (c == '(') { depth++; p++; continue; }
        if (c == ')') { if (depth > 0) depth--; p++; continue; }
        if (depth == 0 && p + 2 <= end &&
            g_ascii_strncasecmp(p, "or", 2) == 0) {
            gboolean left_ok = p == start || is_ws(p[-1]);
            gboolean right_ok = p + 2 == end || is_ws(p[2]);
            if (left_ok && right_ok) return p;
        }
        p++;
    }
    return NULL;
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
    const char *qe = q + strlen(q);
    const char *or_pos = media_find_top_level_or(q, qe);
    if (or_pos) {
        char *left = css_trim_dup_range(q, or_pos);
        char *right = css_trim_dup_range(or_pos + 2, qe);
        gboolean ok = media_query_one_matches(left) ||
                      media_query_one_matches(right);
        g_free(left);
        g_free(right);
        return invert ? !ok : ok;
    }
    gboolean match = TRUE;
    while (*q) {
        while (*q && is_ws(*q)) q++;
        if (!*q) break;
        if (*q == '(') {
            const char *inner = q + 1;
            const char *close = match_close_paren(inner, q + strlen(q));
            if (!close) {
                match = FALSE;
                break;
            }
            if (!media_feature_expr_matches(inner, (gsize)(close - inner)))
                match = FALSE;
            q = close + 1;
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
    gboolean any = FALSE;
    const char *p = query;
    const char *end = query + strlen(query);
    while (p < end && !any) {
        char term = 0;
        const char *seg = css_scan_until(p, end, ",", &term);
        char *alt = css_trim_dup_range(p, seg);
        if (*alt && media_query_one_matches(alt)) any = TRUE;
        g_free(alt);
        p = term == ',' ? seg + 1 : seg;
    }
    return any;
}

gboolean
nd_css_media_query_matches(const char *query)
{
    return media_query_matches(query);
}

static gboolean
sizes_is_length_fn(const char *p)
{
    return g_ascii_strncasecmp(p, "calc(", 5) == 0 ||
           g_ascii_strncasecmp(p, "min(", 4) == 0 ||
           g_ascii_strncasecmp(p, "max(", 4) == 0 ||
           g_ascii_strncasecmp(p, "clamp(", 6) == 0;
}

static double
sizes_length_px(const char *len, gsize len_n)
{
    double px = 0, pct = 0;
    if (!resolve_to_px_pct(len, len_n, &px, &pct)) return -1;
    return px + pct * 0.01 * g_viewport_w;
}

double
nd_css_sizes_resolve(const char *sizes)
{
    if (!sizes || !*sizes) return g_viewport_w;
    const char *p = sizes;
    const char *end = sizes + strlen(sizes);
    while (p < end) {
        while (p < end && (is_ws(*p) || *p == ',')) p++;
        if (p >= end) break;
        const char *entry = p;
        while (p < end && *p != ',') {
            if (*p == '(') {
                const char *cp = match_close_paren(p + 1, end);
                p = cp ? cp + 1 : end;
            } else {
                p++;
            }
        }
        const char *entry_end = p;
        const char *q = entry;
        const char *len_start = NULL;
        while (q < entry_end) {
            while (q < entry_end && is_ws(*q)) q++;
            if (q >= entry_end) break;
            if (sizes_is_length_fn(q) || g_ascii_isdigit(*q) ||
                *q == '.' || *q == '+' || *q == '-') {
                len_start = q;
                break;
            }
            if (*q == '(') {
                const char *cp = match_close_paren(q + 1, entry_end);
                q = cp ? cp + 1 : entry_end;
            } else {
                while (q < entry_end && !is_ws(*q)) q++;
            }
        }
        if (!len_start) continue;
        char *cond = g_strndup(entry, (gsize)(len_start - entry));
        g_strstrip(cond);
        gboolean cond_ok = (*cond == '\0') || nd_css_media_query_matches(cond);
        g_free(cond);
        if (!cond_ok) continue;
        double px = sizes_length_px(len_start, (gsize)(entry_end - len_start));
        if (px > 0) return px;
    }
    return g_viewport_w;
}

#define ND_CSS_LAYER_NONE INT_MAX
#define ND_CSS_MAX_AT_NESTING 32

static gboolean supports_expr(const char **pp, const char *end);

static gboolean
supports_feature_matches(const char *src, gsize len)
{
    char *s = g_strndup(src, len);
    g_strstrip(s);
    char *colon = (char *)css_find_top_level_char(s, s + strlen(s), ':');
    if (!colon) { g_free(s); return FALSE; }
    *colon = '\0';
    char *prop  = g_strstrip(s);
    char *value = g_strstrip(colon + 1);
    int pid = prop_id(prop);
    if (pid < 0) { g_free(s); return FALSE; }
    nd_css_value *v = parse_value_for((nd_css_prop)pid, value);
    gboolean ok = (v != NULL);
    if (v) nd_css_value_free(v);
    g_free(s);
    return ok;
}

static gboolean supports_selector_supported(const nd_css_selector *sel);

static gboolean
supports_simple_supported(const nd_css_simple *c)
{
    if (c->never_match) return FALSE;
    GPtrArray *groups[3] = { c->matches_any, c->matches_none, c->has_groups };
    for (int g = 0; g < 3; g++) {
        if (!groups[g]) continue;
        for (guint i = 0; i < groups[g]->len; i++) {
            const GPtrArray *grp = g_ptr_array_index(groups[g], i);
            for (guint j = 0; j < grp->len; j++)
                if (!supports_selector_supported(g_ptr_array_index(grp, j)))
                    return FALSE;
        }
    }
    return TRUE;
}

static gboolean
supports_selector_supported(const nd_css_selector *sel)
{
    if (!sel || !sel->compounds || sel->compounds->len == 0) return FALSE;
    for (guint i = 0; i < sel->compounds->len; i++)
        if (!supports_simple_supported(g_ptr_array_index(sel->compounds, i)))
            return FALSE;
    return TRUE;
}

static gboolean
supports_selector_matches(const char *src, gsize len)
{
    char *s = g_strndup(src, len);
    GPtrArray *list = nd_css_parse_selector_list(s);
    g_free(s);
    gboolean ok = list->len > 0;
    for (guint i = 0; ok && i < list->len; i++)
        if (!supports_selector_supported(g_ptr_array_index(list, i)))
            ok = FALSE;
    g_ptr_array_free(list, TRUE);
    return ok;
}

static gboolean
match_kw(const char *p, const char *end, const char *kw)
{
    gsize n = strlen(kw);
    if ((gsize)(end - p) < n) return FALSE;
    if (g_ascii_strncasecmp(p, kw, n) != 0) return FALSE;
    if (p + n == end) return TRUE;
    char c = p[n];
    return is_ws(c) || c == '(';
}

static gboolean
supports_term(const char **pp, const char *end)
{
    const char *p = *pp;
    p = css_skip_ws_comments(p, end);
    gboolean negate = FALSE;
    if (match_kw(p, end, "not")) {
        negate = TRUE;
        p += 3;
        p = css_skip_ws_comments(p, end);
    }
    if ((gsize)(end - p) > 9 && g_ascii_strncasecmp(p, "selector(", 9) == 0) {
        p += 9;
        const char *sel_start = p;
        char term = 0;
        const char *sel_end = css_scan_until(p, end, ")", &term);
        gsize sel_len = (gsize)(sel_end - sel_start);
        p = term == ')' ? sel_end + 1 : sel_end;
        gboolean result = supports_selector_matches(sel_start, sel_len);
        if (negate) result = !result;
        *pp = p;
        return result;
    }
    if (p >= end || *p != '(') { *pp = p; return FALSE; }
    p++;
    p = css_skip_ws_comments(p, end);
    gboolean is_nested = (p < end && *p == '(') || match_kw(p, end, "not");
    gboolean result;
    if (is_nested) {
        result = supports_expr(&p, end);
        p = css_skip_ws_comments(p, end);
    } else {
        const char *fstart = p;
        char term = 0;
        const char *fend = css_scan_until(p, end, ")", &term);
        gsize flen = (gsize)(fend - fstart);
        p = fend;
        result = supports_feature_matches(fstart, flen);
    }
    if (p < end && *p == ')') p++;
    if (negate) result = !result;
    *pp = p;
    return result;
}

static gboolean
supports_expr(const char **pp, const char *end)
{
    gboolean acc = supports_term(pp, end);
    const char *p = *pp;
    while (1) {
        p = css_skip_ws_comments(p, end);
        if (match_kw(p, end, "and")) {
            p += 3;
            *pp = p;
            gboolean rhs = supports_term(pp, end);
            p = *pp;
            acc = acc && rhs;
        } else if (match_kw(p, end, "or")) {
            p += 2;
            *pp = p;
            gboolean rhs = supports_term(pp, end);
            p = *pp;
            acc = acc || rhs;
        } else {
            break;
        }
    }
    *pp = p;
    return acc;
}

static gboolean
supports_query_matches(const char *query)
{
    if (!query) return FALSE;
    const char *p = query;
    const char *end = query + strlen(query);
    return supports_expr(&p, end);
}

/* Container query context: a stack of ancestor query containers, innermost
 * last, plus a node->info map populated from the laid-out box tree. */
#define ND_CQ_TYPE_INLINE 1
#define ND_CQ_TYPE_SIZE   2

typedef struct {
    char  *names;   /* space-separated container-name list, verbatim */
    double width;
    double height;
    int    type;    /* ND_CQ_TYPE_* */
} nd_cq_container;

static __thread GHashTable *g_cq_map;     /* nd_node* -> nd_cq_container* */
static __thread GArray     *g_cq_stack;   /* nd_cq_container (by value) */
static __thread GHashTable *g_registered_props; /* "--name" -> nd_css_property_rule* */

void
nd_css_set_container_map(GHashTable *map)
{
    g_cq_map = map;
}

static void
cq_container_free(gpointer p)
{
    nd_cq_container *c = p;
    g_free(c->names);
    g_free(c);
}

GHashTable *
nd_css_container_map_new(void)
{
    return g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                 NULL, cq_container_free);
}

void
nd_css_container_map_add(GHashTable *map, const void *node,
                         const char *type_kw, const char *name_kw,
                         double w, double h)
{
    if (!map || !node || !type_kw) return;
    int type = g_ascii_strcasecmp(type_kw, "size") == 0
        ? ND_CQ_TYPE_SIZE : ND_CQ_TYPE_INLINE;
    nd_cq_container *c = g_new0(nd_cq_container, 1);
    c->names = (name_kw && g_ascii_strcasecmp(name_kw, "none") != 0)
        ? g_strdup(name_kw) : NULL;
    c->width = w;
    c->height = h;
    c->type = type;
    g_hash_table_insert(map, (gpointer)node, c);
}

static gboolean
cq_names_contain(const char *names, const char *name, gsize nlen)
{
    if (!names) return FALSE;
    const char *p = names;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((gsize)(p - tok) == nlen && strncmp(tok, name, nlen) == 0)
            return TRUE;
    }
    return FALSE;
}

/* Resolve a length token (px/em/rem/% of container axis) to px; -1 on failure. */
static double
cq_length_px(const char *s, double pct_basis)
{
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (end == s) return -1;
    while (*end == ' ') end++;
    if (g_ascii_strncasecmp(end, "px", 2) == 0) return v;
    if (g_ascii_strncasecmp(end, "rem", 3) == 0 ||
        g_ascii_strncasecmp(end, "em", 2) == 0)  return v * 16.0;
    if (*end == '%') return v / 100.0 * pct_basis;
    if (*end == '\0' || *end == ')') return v;
    return -1;
}

/* Normalize a range expression so comparison operators are whitespace-delimited
 * tokens regardless of how the author spaced them, and tabs/newlines collapse to
 * spaces. "width>=400px" -> " width >= 400px ". */
static char *
cq_spacify(const char *s)
{
    GString *o = g_string_new(NULL);
    for (const char *p = s; *p; ) {
        if (*p == '<' || *p == '>' || *p == '=') {
            g_string_append_c(o, ' ');
            while (*p == '<' || *p == '>' || *p == '=')
                g_string_append_c(o, *p++);
            g_string_append_c(o, ' ');
        } else if (is_ws(*p)) {
            g_string_append_c(o, ' ');
            p++;
        } else {
            g_string_append_c(o, *p++);
        }
    }
    return g_string_free(o, FALSE);
}

/* Evaluate "<feature> : <value>", "min-/max-<feature>: value", or
 * "<value> <op> <feature> <op> <value>" range syntax against the container. */
static gboolean
cq_feature_matches(const char *feat, const nd_cq_container *c)
{
    char *f = g_strstrip(g_strdup(feat));
    gboolean result = FALSE;

    if (strchr(f, ':')) {
        char *colon = strchr(f, ':');
        *colon = '\0';
        char *name = g_strstrip(f);
        gboolean is_min = g_str_has_prefix(name, "min-");
        gboolean is_max = g_str_has_prefix(name, "max-");
        const char *base = (is_min || is_max) ? name + 4 : name;
        gboolean horiz = g_ascii_strcasecmp(base, "width") == 0 ||
                         g_ascii_strcasecmp(base, "inline-size") == 0;
        gboolean vert  = g_ascii_strcasecmp(base, "height") == 0 ||
                         g_ascii_strcasecmp(base, "block-size") == 0;
        if ((vert && c->type != ND_CQ_TYPE_SIZE) || (!horiz && !vert))
            goto done;
        double size = horiz ? c->width : c->height;
        double val = cq_length_px(colon + 1, size);
        if (val < 0) goto done;
        if (is_min)      result = size >= val;
        else if (is_max) result = size <= val;
        else             result = (int)size == (int)val;
        goto done;
    }

    {
        char *norm = cq_spacify(f);
        char **tok = g_strsplit(norm, " ", -1);
        GPtrArray *parts = g_ptr_array_new();
        for (int i = 0; tok[i]; i++)
            if (*tok[i]) g_ptr_array_add(parts, tok[i]);
        int fi = -1; gboolean horiz = FALSE, vert = FALSE;
        for (guint i = 0; i < parts->len; i++) {
            const char *t = g_ptr_array_index(parts, i);
            if (g_ascii_strcasecmp(t, "width") == 0 ||
                g_ascii_strcasecmp(t, "inline-size") == 0) { fi = (int)i; horiz = TRUE; }
            else if (g_ascii_strcasecmp(t, "height") == 0 ||
                     g_ascii_strcasecmp(t, "block-size") == 0) { fi = (int)i; vert = TRUE; }
        }
        if (fi >= 0 && !(vert && c->type != ND_CQ_TYPE_SIZE)) {
            double size = horiz ? c->width : c->height;
            gboolean ok = TRUE, had = FALSE;
            if (fi >= 2) {
                double v = cq_length_px(g_ptr_array_index(parts, fi - 2), size);
                const char *op = g_ptr_array_index(parts, fi - 1);
                if (v < 0) ok = FALSE;
                else if (!strcmp(op, "<"))  { ok = ok && v <  size; had = TRUE; }
                else if (!strcmp(op, "<=")) { ok = ok && v <= size; had = TRUE; }
                else if (!strcmp(op, ">"))  { ok = ok && v >  size; had = TRUE; }
                else if (!strcmp(op, ">=")) { ok = ok && v >= size; had = TRUE; }
                else ok = FALSE;
            }
            if ((guint)fi + 2 < parts->len) {
                const char *op = g_ptr_array_index(parts, fi + 1);
                double v = cq_length_px(g_ptr_array_index(parts, fi + 2), size);
                if (v < 0) ok = FALSE;
                else if (!strcmp(op, "<"))  { ok = ok && size <  v; had = TRUE; }
                else if (!strcmp(op, "<=")) { ok = ok && size <= v; had = TRUE; }
                else if (!strcmp(op, ">"))  { ok = ok && size >  v; had = TRUE; }
                else if (!strcmp(op, ">=")) { ok = ok && size >= v; had = TRUE; }
                else ok = FALSE;
            }
            result = had ? ok : FALSE;
        }
        g_ptr_array_free(parts, TRUE);
        g_strfreev(tok);
        g_free(norm);
    }
done:
    g_free(f);
    return result;
}

/* Evaluate a container condition expression: parenthesized feature/range groups
 * joined by `and`/`or`, with optional `not`, and nested groups. */
static gboolean
cq_eval_expr(const char *q, const nd_cq_container *c, int rdepth)
{
    if (rdepth > 64) return FALSE;
    gboolean have_or = FALSE, any = FALSE, pending_not = FALSE;
    gboolean acc_and = TRUE, acc_or = FALSE;
    const char *p = q;
    while (*p) {
        while (*p && is_ws(*p)) p++;
        if (!*p) break;
        if (*p == '(') {
            int depth = 1;
            const char *start = ++p;
            while (*p && depth) {
                if (*p == '(') depth++;
                else if (*p == ')' && --depth == 0) break;
                p++;
            }
            char *inner = g_strndup(start, (gsize)(p - start));
            gboolean g = strchr(inner, '(')
                ? cq_eval_expr(inner, c, rdepth + 1)
                : cq_feature_matches(inner, c);
            g_free(inner);
            if (pending_not) { g = !g; pending_not = FALSE; }
            if (!any) { acc_and = g; acc_or = g; any = TRUE; }
            else { acc_and = acc_and && g; acc_or = acc_or || g; }
            if (*p == ')') p++;
        } else {
            const char *w = p;
            while (*p && !is_ws(*p) && *p != '(') p++;
            gsize wl = (gsize)(p - w);
            if (wl == 2 && g_ascii_strncasecmp(w, "or", 2) == 0) have_or = TRUE;
            else if (wl == 3 && g_ascii_strncasecmp(w, "not", 3) == 0)
                pending_not = !pending_not;
        }
    }
    if (!any) return TRUE;
    return have_or ? acc_or : acc_and;
}

/* Pick the query container for a query: nearest ancestor (innermost) that
 * matches the requested name (or any container, if unnamed). */
static const nd_cq_container *
cq_select_container(const char *name, gsize nlen)
{
    if (!g_cq_stack || g_cq_stack->len == 0) return NULL;
    for (int i = (int)g_cq_stack->len - 1; i >= 0; i--) {
        const nd_cq_container *c = &g_array_index(g_cq_stack, nd_cq_container, i);
        if (!name || cq_names_contain(c->names, name, nlen))
            return c;
    }
    return NULL;
}

static gboolean
container_cond_matches(const char *cond)
{
    const char *q = cond;
    while (*q && is_ws(*q)) q++;
    const char *name = NULL;
    gsize nlen = 0;
    if (*q && *q != '(') {
        const char *tok = q;
        while (*q && !is_ws(*q) && *q != '(') q++;
        gsize tlen = (gsize)(q - tok);
        if (tlen == 3 && g_ascii_strncasecmp(tok, "not", 3) == 0) {
            q = tok;
        } else {
            name = tok;
            nlen = tlen;
        }
        if (nlen == 0) name = NULL;
    }
    const nd_cq_container *c = cq_select_container(name, nlen);
    if (!c) return FALSE;
    while (*q && is_ws(*q)) q++;
    if (!*q) return TRUE;
    return cq_eval_expr(q, c, 0);
}

static char *
css_trim_dup_range(const char *start, const char *end)
{
    while (start < end && is_ws(*start)) start++;
    while (end > start && is_ws(end[-1])) end--;
    return g_strndup(start, (gsize)(end - start));
}

static void
css_stylesheet_ensure_layers(nd_css_stylesheet *sh)
{
    if (!sh->layer_names)
        sh->layer_names = g_ptr_array_new_with_free_func(g_free);
    if (!sh->layers)
        sh->layers = g_hash_table_new(g_str_hash, g_str_equal);
}

static int
css_layer_register(nd_css_stylesheet *sh, const char *name)
{
    if (!sh || !name || !*name) return ND_CSS_LAYER_NONE;
    css_stylesheet_ensure_layers(sh);
    gpointer existing = g_hash_table_lookup(sh->layers, name);
    if (existing) return GPOINTER_TO_INT(existing) - 1;
    int rank = (int)sh->layer_names->len;
    char *owned = g_strdup(name);
    g_ptr_array_add(sh->layer_names, owned);
    g_hash_table_insert(sh->layers, owned, GINT_TO_POINTER(rank + 1));
    return rank;
}

static char *
css_layer_anonymous(nd_css_stylesheet *sh)
{
    char *name = g_strdup_printf("@anon:%p:%u", (void *)sh,
                                 sh && sh->layer_names ? sh->layer_names->len : 0);
    css_layer_register(sh, name);
    return name;
}

static char *
css_layer_join(const char *parent, const char *child)
{
    if (!parent || !*parent) return g_strdup(child);
    if (!child || !*child) return g_strdup(parent);
    return g_strconcat(parent, ".", child, NULL);
}

static char *
css_layer_name_from_range(nd_css_stylesheet *sh, const char *current_layer,
                          const char *start, const char *end)
{
    char *name = css_trim_dup_range(start, end);
    if (!name || !*name) {
        g_free(name);
        return css_layer_anonymous(sh);
    }
    char *full = current_layer ? css_layer_join(current_layer, name)
                               : g_strdup(name);
    css_layer_register(sh, full);
    g_free(name);
    return full;
}

static void
css_layer_register_list(nd_css_stylesheet *sh, const char *current_layer,
                        const char *start, const char *end)
{
    const char *p = start;
    while (p < end) {
        char term = 0;
        const char *item_end = css_scan_until(p, end, ",", &term);
        char *name = css_trim_dup_range(p, item_end);
        if (name && *name) {
            char *full = current_layer ? css_layer_join(current_layer, name)
                                       : g_strdup(name);
            css_layer_register(sh, full);
            g_free(full);
        }
        g_free(name);
        p = term == ',' ? item_end + 1 : item_end;
    }
}

static gboolean
css_at_keyword(const char *p, const char *end, const char *kw)
{
    gsize len = strlen(kw);
    if ((gsize)(end - p) < len) return FALSE;
    if (g_ascii_strncasecmp(p, kw, len) != 0) return FALSE;
    if (p + len == end) return TRUE;
    char c = p[len];
    return is_ws(c) || c == '(' || c == ';' || c == ',';
}

static char *
css_parse_import_url(const char **pp, const char *end)
{
    const char *p = *pp;
    p = css_skip_ws_comments(p, end);
    char *url = NULL;
    if (p + 4 <= end && g_ascii_strncasecmp(p, "url(", 4) == 0) {
        p += 4;
        p = css_skip_ws_comments(p, end);
        char quote = 0;
        if (p < end && (*p == '"' || *p == '\'')) {
            quote = *p;
            p++;
        }
        const char *start = p;
        if (quote) {
            while (p < end) {
                if (*p == '\\' && p + 1 < end) p += 2;
                else if (*p == quote) break;
                else p++;
            }
        } else {
            while (p < end && *p != ')' && !is_ws(*p)) p++;
        }
        url = g_strndup(start, (gsize)(p - start));
        if (quote && p < end && *p == quote) p++;
        p = css_skip_ws_comments(p, end);
        if (p < end && *p == ')') p++;
    } else if (p < end && (*p == '"' || *p == '\'')) {
        char quote = *p++;
        const char *start = p;
        while (p < end) {
            if (*p == '\\' && p + 1 < end) p += 2;
            else if (*p == quote) break;
            else p++;
        }
        url = g_strndup(start, (gsize)(p - start));
        if (p < end && *p == quote) p++;
    }
    *pp = p;
    if (url) g_strstrip(url);
    return url;
}

static char *
css_parse_layer_function(nd_css_stylesheet *sh, const char **pp,
                         const char *end)
{
    const char *p = *pp;
    if (!css_at_keyword(p, end, "layer")) return NULL;
    p += 5;
    p = css_skip_ws_comments(p, end);
    if (p < end && *p == '(') {
        p++;
        const char *start = p;
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '(') depth++;
            else if (*p == ')') {
                depth--;
                if (depth == 0) break;
            }
            p++;
        }
        char *name = css_layer_name_from_range(sh, NULL, start, p);
        if (p < end && *p == ')') p++;
        *pp = p;
        return name;
    }
    *pp = p;
    return css_layer_anonymous(sh);
}

static void
css_import_clear(gpointer data)
{
    nd_css_import *im = data;
    g_free(im->url);
    g_free(im->layer_name);
    g_free(im->media);
}

static void
css_stylesheet_add_import(nd_css_stylesheet *sh, const char *url,
                          const char *layer_name, const char *media)
{
    if (!sh || !url || !*url) return;
    if (!sh->imports) {
        sh->imports = g_array_new(FALSE, FALSE, sizeof(nd_css_import));
        g_array_set_clear_func(sh->imports, css_import_clear);
    }
    if (layer_name) css_layer_register(sh, layer_name);
    nd_css_import im = {
        .url = g_strdup(url),
        .layer_name = layer_name ? g_strdup(layer_name) : NULL,
        .media = media && *media ? g_strdup(media) : NULL,
    };
    g_array_append_val(sh->imports, im);
}

static void
css_parse_import_prelude(nd_css_stylesheet *sh, const char *current_layer,
                         const char *start, const char *end)
{
    const char *p = start;
    char *url = css_parse_import_url(&p, end);
    if (!url || !*url) {
        g_free(url);
        return;
    }
    char *layer_name = NULL;
    while (p < end) {
        p = css_skip_ws_comments(p, end);
        if (!css_at_keyword(p, end, "layer")) break;
        char *parsed = css_parse_layer_function(sh, &p, end);
        if (parsed) {
            g_free(layer_name);
            layer_name = parsed;
        }
    }
    if (current_layer) {
        char *full = layer_name ? css_layer_join(current_layer, layer_name)
                                : g_strdup(current_layer);
        g_free(layer_name);
        layer_name = full;
    }
    char *media = css_trim_dup_range(p, end);
    css_stylesheet_add_import(sh, url, layer_name, media);
    g_free(media);
    g_free(layer_name);
    g_free(url);
}

static void
nd_css_scope_text_free(gpointer data)
{
    nd_css_scope_text *s = data;
    if (!s) return;
    g_free(s->start);
    g_free(s->end);
    g_free(s);
}

static gboolean
css_scope_keyword_at(const char *p, const char *end, const char *kw)
{
    gsize n = strlen(kw);
    if ((gsize)(end - p) < n) return FALSE;
    if (g_ascii_strncasecmp(p, kw, n) != 0) return FALSE;
    return p + n == end || !is_ident(p[n]);
}

static gboolean
css_scope_selector_group_valid(GPtrArray *group)
{
    if (!group || group->len == 0) return FALSE;
    for (guint i = 0; i < group->len; i++) {
        const nd_css_selector *sel = g_ptr_array_index(group, i);
        if (!sel || sel->pseudo_element != ND_CSS_PE_NONE) return FALSE;
    }
    return TRUE;
}

static GPtrArray *
css_scope_parse_selector_list(const char *text)
{
    GPtrArray *group = parse_selector_group(text, strlen(text), 0);
    if (!css_scope_selector_group_valid(group)) {
        g_ptr_array_free(group, TRUE);
        return NULL;
    }
    return group;
}

static gboolean
css_scope_text_valid(const nd_css_scope_text *s)
{
    GPtrArray *roots = css_scope_parse_selector_list(s && s->start
                                                     ? s->start : ":root");
    if (!roots) return FALSE;
    g_ptr_array_free(roots, TRUE);
    if (s && s->end) {
        GPtrArray *limits = css_scope_parse_selector_list(s->end);
        if (!limits) return FALSE;
        g_ptr_array_free(limits, TRUE);
    }
    return TRUE;
}

static nd_css_scope_text *
css_scope_text_from_prelude(const char *start, const char *end)
{
    const char *p = css_skip_ws_comments(start, end);
    nd_css_scope_text *s = g_new0(nd_css_scope_text, 1);
    if (p < end && *p == '(') {
        char term = 0;
        const char *inner = p + 1;
        const char *close = css_scan_until(inner, end, ")", &term);
        if (term != ')') {
            nd_css_scope_text_free(s);
            return NULL;
        }
        s->start = css_trim_dup_range(inner, close);
        p = close + 1;
        if (!s->start || !*s->start) {
            nd_css_scope_text_free(s);
            return NULL;
        }
    }
    p = css_skip_ws_comments(p, end);
    if (css_scope_keyword_at(p, end, "to")) {
        p += 2;
        p = css_skip_ws_comments(p, end);
        if (p >= end || *p != '(') {
            nd_css_scope_text_free(s);
            return NULL;
        }
        char term = 0;
        const char *inner = p + 1;
        const char *close = css_scan_until(inner, end, ")", &term);
        if (term != ')') {
            nd_css_scope_text_free(s);
            return NULL;
        }
        s->end = css_trim_dup_range(inner, close);
        p = close + 1;
        if (!s->end || !*s->end) {
            nd_css_scope_text_free(s);
            return NULL;
        }
    }
    p = css_skip_ws_comments(p, end);
    if (p < end || !css_scope_text_valid(s)) {
        nd_css_scope_text_free(s);
        return NULL;
    }
    return s;
}

static nd_css_scope *
css_scope_from_text(const nd_css_scope_text *text)
{
    nd_css_scope *s = g_new0(nd_css_scope, 1);
    s->roots = css_scope_parse_selector_list(text && text->start
                                             ? text->start : ":root");
    if (!s->roots) {
        nd_css_scope_free(s);
        return NULL;
    }
    if (text && text->end) {
        s->limits = css_scope_parse_selector_list(text->end);
        if (!s->limits) {
            nd_css_scope_free(s);
            return NULL;
        }
    }
    return s;
}

static gboolean
css_scope_stack_apply_to_rule(nd_css_rule *rule, GPtrArray *scope_stack)
{
    if (!rule || !scope_stack || scope_stack->len == 0) return TRUE;
    rule->scopes = g_ptr_array_new_with_free_func((GDestroyNotify)nd_css_scope_free);
    for (guint i = 0; i < scope_stack->len; i++) {
        nd_css_scope_text *text = g_ptr_array_index(scope_stack, i);
        nd_css_scope *scope = css_scope_from_text(text);
        if (!scope) return FALSE;
        g_ptr_array_add(rule->scopes, scope);
    }
    return TRUE;
}

static gboolean
css_selector_segment_has_scope_marker(const char *p, const char *end)
{
    char quote = 0;
    int paren = 0, bracket = 0;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p + 1 < end) p += 2;
            else {
                if (c == quote) quote = 0;
                p++;
            }
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            p = css_skip_comment(p, end);
            continue;
        }
        if (c == '\\' && p + 1 < end) {
            p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            p++;
            continue;
        }
        if (c == '[') bracket++;
        else if (c == ']' && bracket > 0) bracket--;
        else if (c == '(') paren++;
        else if (c == ')' && paren > 0) paren--;
        if (bracket == 0 && c == '&') return TRUE;
        if (bracket == 0 && c == ':' &&
            (gsize)(end - p) >= 6 &&
            g_ascii_strncasecmp(p + 1, "scope", 5) == 0 &&
            (p + 6 == end || !is_ident(p[6])))
            return TRUE;
        p++;
    }
    return FALSE;
}

static void
css_scope_append_amp_rewritten(GString *out, const char *p, const char *end)
{
    char quote = 0;
    int bracket = 0;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p + 1 < end) {
                g_string_append_len(out, p, 2);
                p += 2;
                continue;
            }
            g_string_append_c(out, c);
            if (c == quote) quote = 0;
            p++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            const char *q = css_skip_comment(p, end);
            g_string_append_len(out, p, (gssize)(q - p));
            p = q;
            continue;
        }
        if (c == '\\' && p + 1 < end) {
            g_string_append_len(out, p, 2);
            p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            g_string_append_c(out, c);
            p++;
            continue;
        }
        if (c == '[') bracket++;
        else if (c == ']' && bracket > 0) bracket--;
        if (c == '&' && bracket == 0) {
            g_string_append(out, ":scope");
            p++;
            continue;
        }
        g_string_append_c(out, c);
        p++;
    }
}

static char *
css_scope_selector_list_text(const char *start, const char *end)
{
    GString *out = g_string_new(NULL);
    const char *p = start;
    gboolean first = TRUE;
    while (p < end) {
        char term = 0;
        const char *seg_end = css_scan_until(p, end, ",", &term);
        const char *s = p;
        const char *e = seg_end;
        while (s < e && is_ws(*s)) s++;
        while (e > s && is_ws(e[-1])) e--;
        if (s < e) {
            if (!first) g_string_append(out, ", ");
            first = FALSE;
            gboolean has_scope = css_selector_segment_has_scope_marker(s, e);
            if (!has_scope) {
                g_string_append(out, ":where(:scope) ");
                g_string_append_len(out, s, (gssize)(e - s));
            } else {
                css_scope_append_amp_rewritten(out, s, e);
            }
        }
        p = term == ',' ? seg_end + 1 : seg_end;
    }
    return g_string_free(out, FALSE);
}

static void
parse_rules_until(const char **pp, const char *end,
                  nd_css_stylesheet *sh, int *source_order,
                  char close_at, const char *current_layer,
                  GPtrArray *scope_stack)
{
    static int at_depth;
    gboolean nested = close_at == '}';
    if (nested) {
        if (at_depth >= ND_CSS_MAX_AT_NESTING) {
            const char *p = *pp;
            char term = 0;
            const char *seg = css_scan_until(p, end, "}", &term);
            p = term == '}' ? seg + 1 : seg;
            *pp = p;
            return;
        }
        at_depth++;
    }
    const char *p = *pp;
    while (p < end) {
        p = css_skip_ws_comments(p, end);
        if (p >= end) break;

        if (!nested && p + 4 <= end && memcmp(p, "<!--", 4) == 0) {
            p += 4;
            continue;
        }
        if (!nested && p + 3 <= end && memcmp(p, "-->", 3) == 0) {
            p += 3;
            continue;
        }
        if (*p == '}') {
            p++;
            if (close_at == '}') break;
            continue;
        }
        if (*p == '@') {
            const char *at_start = p;
            p++;
            char *at_name = read_css_ident(&p, end);
            if (!at_name || !*at_name) {
                g_free(at_name);
                p = at_start;
                skip_at_rule(&p, end);
                continue;
            }
            if (g_ascii_strcasecmp(at_name, "import") == 0) {
                char term = 0;
                const char *prelude_start = p;
                const char *prelude_end = css_scan_segment(p, end, &term);
                if (term == ';') {
                    css_parse_import_prelude(sh, current_layer,
                                             prelude_start, prelude_end);
                    p = prelude_end + 1;
                } else {
                    p = at_start;
                    skip_at_rule(&p, end);
                }
                g_free(at_name);
                continue;
            }
            if (g_ascii_strcasecmp(at_name, "supports") == 0) {
                char term = 0;
                const char *cond_start = p;
                const char *cond_end = css_scan_segment(p, end, &term);
                p = cond_end;
                gsize cond_len = (gsize)(cond_end - cond_start);
                char *cond = g_strndup(cond_start, cond_len);
                g_strstrip(cond);
                if (p < end && *p == '{') {
                    p++;
                    if (supports_query_matches(cond)) {
                        parse_rules_until(&p, end, sh, source_order, '}',
                                          current_layer, scope_stack);
                    } else {
                        p = css_skip_to_block_end(p - 1, end);
                    }
                } else if (p < end && *p == ';') p++;
                g_free(cond);
                g_free(at_name);
                continue;
            }
            if (g_ascii_strcasecmp(at_name, "font-face") == 0) {
                char term = 0;
                const char *prelude_end = css_scan_segment(p, end, &term);
                p = prelude_end;
                if (term == '{') {
                    const char *block_end = css_skip_to_block_end(p, end);
                    const char *body_start = p + 1;
                    const char *body_end = css_block_body_end(body_start,
                                                              block_end);
                    char *family = NULL;
                    char *src_url = NULL;
                    const char *decl_p = body_start;
                    while (decl_p < body_end) {
                        char dterm = 0;
                        const char *decl_end =
                            css_scan_declaration_value(decl_p, body_end, &dterm);
                        char *decl = g_strndup(decl_p,
                                               (gsize)(decl_end - decl_p));
                        char *line = g_strstrip(decl);
                        char *colon = (char *)css_find_top_level_char(
                            line, line + strlen(line), ':');
                        if (!colon) {
                            g_free(decl);
                            if (!dterm) break;
                            decl_p = decl_end + 1;
                            continue;
                        }
                        *colon = '\0';
                        char *prop = g_strstrip(line);
                        char *val  = g_strstrip(colon + 1);
                        if (g_ascii_strcasecmp(prop, "font-family") == 0 && !family) {
                            char *v = val;
                            while (*v == ' ' || *v == '\'' || *v == '"') v++;
                            gsize vlen = strlen(v);
                            while (vlen > 0 && (v[vlen - 1] == ' ' ||
                                                v[vlen - 1] == '\'' ||
                                                v[vlen - 1] == '"')) vlen--;
                            if (vlen > 0) family = g_strndup(v, vlen);
                        } else if (g_ascii_strcasecmp(prop, "src") == 0) {
                            font_src_consider_urls(&src_url, val);
                        }
                        g_free(decl);
                        if (!dterm) break;
                        decl_p = decl_end + 1;
                    }
                    if (!sh->font_faces) {
                        sh->font_faces = g_array_new(FALSE, FALSE,
                                                     sizeof(nd_css_font_face));
                        g_array_set_clear_func(sh->font_faces, font_face_clear);
                    }
                    if (family && *family && src_url && *src_url) {
                        nd_css_font_face ff = { family, src_url };
                        g_array_append_val(sh->font_faces, ff);
                        family = NULL;
                        src_url = NULL;
                    }
                    g_free(family);
                    g_free(src_url);
                    p = block_end;
                } else if (term == ';') p++;
                g_free(at_name);
                continue;
            }
            if (g_ascii_strcasecmp(at_name, "keyframes") == 0 ||
                g_ascii_strcasecmp(at_name, "-webkit-keyframes") == 0) {
                char term = 0;
                const char *nm_start = p;
                const char *prelude_end = css_scan_segment(p, end, &term);
                char *kf_name = css_keyframes_name_from_range(nm_start,
                                                              prelude_end);
                p = prelude_end;
                if (term == '{') {
                    p++;
                    GArray *stops = g_array_new(FALSE, FALSE,
                                                sizeof(nd_css_keyframe_stop));
                    while (p < end) {
                        p = css_skip_ws_comments(p, end);
                        if (p < end && *p == '}') { p++; break; }
                        const char *sel_start = p;
                        char sel_term = 0;
                        const char *sel_end =
                            css_scan_segment(p, end, &sel_term);
                        if (sel_term != '{') break;
                        const char *body_start = sel_end + 1;
                        const char *block_end = css_skip_to_block_end(sel_end, end);
                        const char *body_end = css_block_body_end(body_start,
                                                                  block_end);
                        p = block_end;
                        gsize sel_len = (gsize)(sel_end - sel_start);
                        char *sel = g_strndup(sel_start, sel_len);
                        g_strstrip(sel);
                        double op = 0;
                        gboolean has_op = FALSE;
                        nd_css_transform tf = { 0 };
                        gboolean has_tf = FALSE;
                        guint8 col[4] = { 0 }, bgcol[4] = { 0 };
                        gboolean has_col = FALSE, has_bgcol = FALSE;
                        const char *decl_p = body_start;
                        while (decl_p < body_end) {
                            char dterm = 0;
                            const char *decl_end =
                                css_scan_declaration_value(decl_p, body_end, &dterm);
                            char *decl = g_strndup(decl_p,
                                                   (gsize)(decl_end - decl_p));
                            char *line = g_strstrip(decl);
                            char *colon = (char *)css_find_top_level_char(
                                line, line + strlen(line), ':');
                            if (!colon) {
                                g_free(decl);
                                if (!dterm) break;
                                decl_p = decl_end + 1;
                                continue;
                            }
                            *colon = '\0';
                            char *prop = g_strstrip(line);
                            char *val  = g_strstrip(colon + 1);
                            if (g_ascii_strcasecmp(prop, "opacity") == 0) {
                                op = g_ascii_strtod(val, NULL);
                                has_op = TRUE;
                            } else if (g_ascii_strcasecmp(prop, "transform") == 0) {
                                nd_css_value *tv = parse_transform(val);
                                if (tv) {
                                    tf = tv->u.transform;
                                    has_tf = TRUE;
                                    nd_css_value_free(tv);
                                }
                            } else if (g_ascii_strcasecmp(prop, "color") == 0) {
                                if (parse_color(val, &col[0], &col[1],
                                                &col[2], &col[3]))
                                    has_col = TRUE;
                            } else if (g_ascii_strcasecmp(prop, "background-color") == 0 ||
                                       g_ascii_strcasecmp(prop, "background") == 0) {
                                if (parse_color(val, &bgcol[0], &bgcol[1],
                                                &bgcol[2], &bgcol[3]))
                                    has_bgcol = TRUE;
                            }
                            g_free(decl);
                            if (!dterm) break;
                            decl_p = decl_end + 1;
                        }
                        const char *sel_p = sel;
                        const char *sel_all_end = sel + strlen(sel);
                        while (sel_p < sel_all_end) {
                            char cterm = 0;
                            const char *one_end =
                                css_scan_until(sel_p, sel_all_end, ",", &cterm);
                            char *one = css_trim_dup_range(sel_p, one_end);
                            double pct = 0;
                            if (parse_keyframe_stop_pct(one, &pct)) {
                                nd_css_keyframe_stop s = {
                                    .pct = pct,
                                    .opacity = op, .has_opacity = has_op,
                                    .transform = tf, .has_transform = has_tf,
                                    .has_color = has_col, .has_bg_color = has_bgcol,
                                };
                                memcpy(s.color, col, 4);
                                memcpy(s.bg_color, bgcol, 4);
                                g_array_append_val(stops, s);
                            }
                            g_free(one);
                            sel_p = cterm == ',' ? one_end + 1 : one_end;
                        }
                        g_free(sel);
                    }
                    if (stops->len > 0 && kf_name && *kf_name) {
                        if (!sh->keyframes) {
                            sh->keyframes = g_array_new(FALSE, FALSE,
                                                        sizeof(nd_css_keyframes));
                            g_array_set_clear_func(sh->keyframes, keyframes_clear);
                        }
                        g_array_sort(stops, keyframe_stop_cmp);
                        nd_css_keyframes kf = {
                            .name = g_strdup(kf_name),
                            .n_stops = (int)stops->len,
                            .stops = (nd_css_keyframe_stop *)g_memdup2(
                                stops->data,
                                stops->len * sizeof(nd_css_keyframe_stop)),
                        };
                        g_array_append_val(sh->keyframes, kf);
                    }
                    g_array_free(stops, TRUE);
                } else if (term == ';') p++;
                g_free(kf_name);
                g_free(at_name);
                continue;
            }
            if (g_ascii_strcasecmp(at_name, "media") == 0) {
                char term = 0;
                const char *cond_start = p;
                const char *cond_end = css_scan_segment(p, end, &term);
                p = cond_end;
                gsize cond_len = (gsize)(cond_end - cond_start);
                char *cond = g_strndup(cond_start, cond_len);
                g_strstrip(cond);
                if (p < end && *p == '{') {
                    p++;
                    if (media_query_matches(cond)) {
                        parse_rules_until(&p, end, sh, source_order, '}',
                                          current_layer, scope_stack);
                    } else {
                        p = css_skip_to_block_end(p - 1, end);
                    }
                } else if (p < end && *p == ';') p++;
                g_free(cond);
                g_free(at_name);
                continue;
            }
            if (g_ascii_strcasecmp(at_name, "container") == 0) {
                char term = 0;
                const char *cond_start = p;
                const char *cond_end = css_scan_segment(p, end, &term);
                p = cond_end;
                gsize cond_len = (gsize)(cond_end - cond_start);
                char *cond = g_strndup(cond_start, cond_len);
                g_strstrip(cond);
                if (p < end && *p == '{') {
                    p++;
                    guint before = sh->rules->len;
                    parse_rules_until(&p, end, sh, source_order, '}',
                                      current_layer, scope_stack);
                    sh->has_container_rules = TRUE;
                    for (guint ri = before; ri < sh->rules->len; ri++) {
                        nd_css_rule *r = g_ptr_array_index(sh->rules, ri);
                        if (r->container_condition) {
                            char *joined = g_strdup_printf("%s and %s",
                                cond, r->container_condition);
                            g_free(r->container_condition);
                            r->container_condition = joined;
                        } else {
                            r->container_condition = g_strdup(cond);
                        }
                    }
                } else if (p < end && *p == ';') p++;
                g_free(cond);
                g_free(at_name);
                continue;
            }
            if (g_ascii_strcasecmp(at_name, "scope") == 0) {
                char term = 0;
                const char *prelude_start = p;
                const char *prelude_end = css_scan_segment(p, end, &term);
                p = prelude_end;
                if (term == '{') {
                    nd_css_scope_text *scope =
                        css_scope_text_from_prelude(prelude_start, prelude_end);
                    p++;
                    if (scope) {
                        g_ptr_array_add(scope_stack, scope);
                        parse_rules_until(&p, end, sh, source_order, '}',
                                          current_layer, scope_stack);
                        g_ptr_array_remove_index(scope_stack,
                                                 scope_stack->len - 1);
                    } else {
                        p = css_skip_to_block_end(p - 1, end);
                    }
                } else if (term == ';') p++;
                g_free(at_name);
                continue;
            }
            if (g_ascii_strcasecmp(at_name, "layer") == 0) {
                char term = 0;
                const char *prelude_start = p;
                const char *prelude_end = css_scan_segment(p, end, &term);
                if (term == '{') {
                    char *layer_name = css_layer_name_from_range(
                        sh, current_layer, prelude_start, prelude_end);
                    p = prelude_end;
                    p++;
                    parse_rules_until(&p, end, sh, source_order, '}',
                                      layer_name, scope_stack);
                    g_free(layer_name);
                } else if (term == ';') {
                    css_layer_register_list(sh, current_layer,
                                            prelude_start, prelude_end);
                    p = prelude_end + 1;
                } else p = prelude_end;
                g_free(at_name);
                continue;
            }
            if (g_ascii_strcasecmp(at_name, "property") == 0) {
                char term = 0;
                const char *name_start = p;
                const char *name_end = css_scan_segment(p, end, &term);
                p = name_end;
                char *prop_name = css_trim_dup_range(name_start, name_end);
                if (term == '{' && prop_name &&
                    prop_name[0] == '-' && prop_name[1] == '-' && prop_name[2]) {
                    const char *block_end = css_skip_to_block_end(p, end);
                    const char *body_start = p + 1;
                    const char *body_end = css_block_body_end(body_start,
                                                              block_end);
                    char *initial_value = NULL;
                    gboolean inherits = TRUE;
                    gboolean has_initial = FALSE;
                    const char *decl_p = body_start;
                    while (decl_p < body_end) {
                        char dterm = 0;
                        const char *decl_end =
                            css_scan_declaration_value(decl_p, body_end, &dterm);
                        char *decl = g_strndup(decl_p,
                                               (gsize)(decl_end - decl_p));
                        char *line = g_strstrip(decl);
                        char *colon = (char *)css_find_top_level_char(
                            line, line + strlen(line), ':');
                        if (colon) {
                            *colon = '\0';
                            char *dprop = g_strstrip(line);
                            char *dval = g_strstrip(colon + 1);
                            if (g_ascii_strcasecmp(dprop, "inherits") == 0) {
                                inherits = g_ascii_strcasecmp(dval, "false") != 0;
                            } else if (g_ascii_strcasecmp(dprop,
                                                          "initial-value") == 0) {
                                g_free(initial_value);
                                initial_value = g_strdup(dval);
                                has_initial = TRUE;
                            }
                        }
                        g_free(decl);
                        if (!dterm) break;
                        decl_p = decl_end + 1;
                    }
                    if (!sh->property_rules) {
                        sh->property_rules = g_array_new(FALSE, FALSE,
                            sizeof(nd_css_property_rule));
                        g_array_set_clear_func(sh->property_rules,
                                               property_rule_clear);
                    }
                    nd_css_property_rule pr = {
                        .name = g_strdup(prop_name),
                        .initial_value = initial_value,
                        .inherits = inherits,
                        .has_initial = has_initial,
                    };
                    g_array_append_val(sh->property_rules, pr);
                    p = block_end;
                } else if (term == ';' && p < end) {
                    p++;
                } else if (term == '{') {
                    p = css_skip_to_block_end(p, end);
                }
                g_free(prop_name);
                g_free(at_name);
                continue;
            }
            g_free(at_name);
            p = at_start;
            skip_at_rule(&p, end);
            continue;
        }

        nd_css_rule *rule = g_new0(nd_css_rule, 1);
        rule->selectors = g_ptr_array_new();
        rule->decls     = g_array_new(FALSE, FALSE, sizeof(nd_css_decl));
        rule->layer_name = current_layer ? g_strdup(current_layer) : NULL;
        rule->source_order = (*source_order)++;
        if (!css_scope_stack_apply_to_rule(rule, scope_stack)) {
            nd_css_rule_free(rule);
            char term = 0;
            const char *skip_to = css_scan_segment(p, end, &term);
            if (term == '{') p = css_skip_to_block_end(skip_to, end);
            else p = term == ';' ? skip_to + 1 : skip_to;
            continue;
        }

        char term = 0;
        const char *sel_start = p;
        const char *sel_end = css_scan_segment(p, end, &term);
        if (term != '{') {
            nd_css_rule_free(rule);
            p = term == ';' ? sel_end + 1 : sel_end;
            continue;
        }
        char *scoped_sel = rule->scopes
            ? css_scope_selector_list_text(sel_start, sel_end) : NULL;
        const char *parse_p = scoped_sel ? scoped_sel : sel_start;
        const char *parse_end = scoped_sel ? scoped_sel + strlen(scoped_sel)
                                           : sel_end;

        gboolean ok = FALSE;
        while (parse_p < parse_end) {
            nd_css_selector *sel = parse_one_selector(&parse_p, parse_end, 0);
            if (sel) {
                g_ptr_array_add(rule->selectors, sel);
                ok = TRUE;
            }
            while (parse_p < parse_end && is_ws(*parse_p)) parse_p++;
            if (parse_p < parse_end && *parse_p == ',') {
                parse_p++;
                continue;
            }
            else break;
        }
        g_free(scoped_sel);
        if (!ok) {
            nd_css_rule_free(rule);
            p = css_skip_to_block_end(sel_end, end);
            continue;
        }
        p = sel_end + 1;
        parse_declaration_block(&p, end, rule->decls, rule);
        g_ptr_array_add(sh->rules, rule);
    }
    *pp = p;
    if (nested) at_depth--;
}

static gboolean
css_append_nested_selector(GString *out, const char *part,
                           const char *parent_expr)
{
    const char *p = part;
    const char *end = part + strlen(part);
    char quote = 0;
    int bracket = 0;
    gboolean replaced = FALSE;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p + 1 < end) {
                g_string_append_len(out, p, 2);
                p += 2;
                continue;
            }
            g_string_append_c(out, c);
            if (c == quote) quote = 0;
            p++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            const char *q = css_skip_comment(p, end);
            g_string_append_len(out, p, (gssize)(q - p));
            p = q;
            continue;
        }
        if (c == '\\' && p + 1 < end) {
            g_string_append_len(out, p, 2);
            p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            g_string_append_c(out, c);
            p++;
            continue;
        }
        if (c == '[') bracket++;
        else if (c == ']' && bracket > 0) bracket--;
        if (c == '&' && bracket == 0) {
            g_string_append(out, parent_expr);
            replaced = TRUE;
            p++;
            continue;
        }
        g_string_append_c(out, c);
        p++;
    }
    return replaced;
}

static char *
css_combine_selectors(const char *parent, const char *child)
{
    char *pc = g_strstrip(g_strdup(parent));
    char *cc = g_strstrip(g_strdup(child));
    GString *out = g_string_new(NULL);
    const char *p = cc;
    const char *end = cc + strlen(cc);
    while (p < end) {
        char term = 0;
        const char *seg = css_scan_until(p, end, ",", &term);
        char *part_buf = css_trim_dup_range(p, seg);
        char *part = part_buf;
        if (!*part) {
            g_free(part_buf);
            p = term == ',' ? seg + 1 : seg;
            continue;
        }
        if (out->len) g_string_append(out, ", ");
        char *isparent = g_strdup_printf(":is(%s)", pc);
        GString *piece = g_string_new(NULL);
        if (css_append_nested_selector(piece, part, isparent))
            g_string_append_len(out, piece->str, (gssize)piece->len);
        else
            g_string_append_printf(out, ":is(%s) %s", pc, part);
        g_string_free(piece, TRUE);
        g_free(isparent);
        g_free(part_buf);
        p = term == ',' ? seg + 1 : seg;
    }
    g_free(pc);
    g_free(cc);
    return g_string_free(out, FALSE);
}

static gboolean css_body_has_nested_rule(const char *s, const char *e);
static void css_flatten_style_rule(GString *out, const char *sel,
                                   const char *body_s, const char *body_e,
                                   int depth);

#define ND_CSS_NEST_MAX_DEPTH 128

static void
css_flatten_rule_list(GString *out, const char *p, const char *end, int depth)
{
    if (depth > ND_CSS_NEST_MAX_DEPTH) return;
    while (p < end) {
        p = css_skip_ws_comments(p, end);
        if (p >= end) break;
        if (p + 4 <= end && memcmp(p, "<!--", 4) == 0) {
            p += 4;
            continue;
        }
        if (p + 3 <= end && memcmp(p, "-->", 3) == 0) {
            p += 3;
            continue;
        }
        if (*p == '}') { p++; continue; }
        if (*p == '@') {
            const char *prelude = p;
            char term = 0;
            const char *seg_end = css_scan_segment(p, end, &term);
            if (term == '{') {
                gboolean group = (g_ascii_strncasecmp(prelude, "@media", 6) == 0 ||
                                  g_ascii_strncasecmp(prelude, "@supports", 9) == 0 ||
                                  g_ascii_strncasecmp(prelude, "@container", 10) == 0 ||
                                  g_ascii_strncasecmp(prelude, "@layer", 6) == 0 ||
                                  g_ascii_strncasecmp(prelude, "@scope", 6) == 0);
                const char *block_end = css_skip_to_block_end(seg_end, end);
                if (group) {
                    g_string_append_len(out, prelude, (gssize)(seg_end - prelude));
                    g_string_append_c(out, '{');
                    const char *body_s = seg_end + 1;
                    css_flatten_rule_list(out, body_s,
                                          css_block_body_end(body_s, block_end),
                                          depth + 1);
                    g_string_append_c(out, '}');
                } else {
                    g_string_append_len(out, prelude, (gssize)(block_end - prelude));
                }
                p = block_end;
            } else {
                g_string_append_len(out, prelude, (gssize)(seg_end - prelude));
                if (term == ';' && seg_end < end) { g_string_append_c(out, ';'); p = seg_end + 1; }
                else p = seg_end;
            }
            continue;
        }
        char term = 0;
        const char *seg_end = css_scan_segment(p, end, &term);
        if (term != '{') {
            p = (seg_end < end) ? seg_end + 1 : end;
            continue;
        }
        char *sel = g_strndup(p, (gsize)(seg_end - p));
        g_strstrip(sel);
        const char *body_s = seg_end + 1;
        const char *block_end = css_skip_to_block_end(seg_end, end);
        const char *body_e = css_block_body_end(body_s, block_end);
        css_flatten_style_rule(out, sel, body_s, body_e, depth + 1);
        g_free(sel);
        p = block_end;
    }
}

static gboolean
css_body_has_nested_rule(const char *s, const char *e)
{
    const char *p = s;
    while (p < e) {
        while (p < e && is_ws(*p)) p++;
        if (p >= e) break;
        if (p + 1 < e && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < e) p += 2;
            continue;
        }
        char term = 0;
        const char *seg_end = css_scan_segment(p, e, &term);
        if (term == '{') return TRUE;
        if (term == 0) break;
        p = seg_end + 1;
    }
    return FALSE;
}

static void
css_flatten_style_rule(GString *out, const char *sel,
                       const char *body_s, const char *body_e, int depth)
{
    if (depth > ND_CSS_NEST_MAX_DEPTH) return;
    if (!css_body_has_nested_rule(body_s, body_e)) {
        g_string_append(out, sel);
        g_string_append_c(out, '{');
        g_string_append_len(out, body_s, (gssize)(body_e - body_s));
        g_string_append_c(out, '}');
        return;
    }
    GString *decls = g_string_new(NULL);
    GString *deferred = g_string_new(NULL);
    const char *p = body_s;
    while (p < body_e) {
        while (p < body_e && is_ws(*p)) p++;
        if (p >= body_e) break;
        if (p + 1 < body_e && p[0] == '/' && p[1] == '*') {
            const char *cs = p;
            p += 2;
            while (p + 1 < body_e && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < body_e) p += 2;
            g_string_append_len(decls, cs, (gssize)(p - cs));
            continue;
        }
        char term = 0;
        const char *seg_end = css_scan_segment(p, body_e, &term);
        if (term == '{') {
            char *nsel = g_strndup(p, (gsize)(seg_end - p));
            g_strstrip(nsel);
            const char *nbody_s = seg_end + 1;
            const char *nblock_end = css_skip_to_block_end(seg_end, body_e);
            const char *nbody_e = css_block_body_end(nbody_s, nblock_end);
            char *combined = css_combine_selectors(sel, nsel);
            css_flatten_style_rule(deferred, combined, nbody_s, nbody_e,
                                   depth + 1);
            g_free(combined);
            g_free(nsel);
            p = nblock_end;
        } else {
            g_string_append_len(decls, p, (gssize)(seg_end - p));
            if (term == ';') g_string_append_c(decls, ';');
            p = (seg_end < body_e) ? seg_end + 1 : body_e;
        }
    }
    if (decls->len > 0) {
        g_string_append(out, sel);
        g_string_append_c(out, '{');
        g_string_append_len(out, decls->str, (gssize)decls->len);
        g_string_append_c(out, '}');
    }
    g_string_append_len(out, deferred->str, (gssize)deferred->len);
    g_string_free(decls, TRUE);
    g_string_free(deferred, TRUE);
}

static char *
css_flatten_nesting(const char *text, gssize len)
{
    if (!text) return NULL;
    if (len < 0) len = (gssize)strlen(text);
    GString *out = g_string_new(NULL);
    css_flatten_rule_list(out, text, text + len, 0);
    return g_string_free(out, FALSE);
}

nd_css_stylesheet *
nd_css_stylesheet_parse(const char *text, gssize len_in)
{
    nd_css_stylesheet *sh = g_new0(nd_css_stylesheet, 1);
    sh->rules = g_ptr_array_new_with_free_func((GDestroyNotify)nd_css_rule_free);
    if (!text) return sh;
    if (len_in < 0) len_in = (gssize)strlen(text);

    char *flattened = css_flatten_nesting(text, len_in);
    const char *p   = flattened;
    const char *end = flattened + strlen(flattened);
    int source_order = 0;
    GPtrArray *scope_stack =
        g_ptr_array_new_with_free_func(nd_css_scope_text_free);
    parse_rules_until(&p, end, sh, &source_order, 0, NULL, scope_stack);
    g_ptr_array_free(scope_stack, TRUE);
    g_free(flattened);
    return sh;
}

static gboolean
css_url_should_resolve(const char *url)
{
    if (!url || !*url) return FALSE;
    if (url[0] == '#') return FALSE;
    if (g_ascii_strncasecmp(url, "data:", 5) == 0) return FALSE;
    if (g_ascii_strncasecmp(url, "blob:", 5) == 0) return FALSE;
    return TRUE;
}

static void
css_value_resolve_url(nd_css_value *v, const char *base_url)
{
    if (!v || !base_url || v->kind != ND_CSS_V_URL)
        return;
    if (!css_url_should_resolve(v->u.url))
        return;
    char *abs = nd_url_resolve(base_url, v->u.url);
    if (!abs) return;
    g_free(v->u.url);
    v->u.url = abs;
}

void
nd_css_stylesheet_resolve_urls(nd_css_stylesheet *s, const char *base_url)
{
    if (!s || !base_url) return;
    if (s->rules) {
        for (guint ri = 0; ri < s->rules->len; ri++) {
            nd_css_rule *r = g_ptr_array_index(s->rules, ri);
            if (!r || !r->decls) continue;
            for (guint di = 0; di < r->decls->len; di++) {
                nd_css_decl *d = &g_array_index(r->decls, nd_css_decl, di);
                css_value_resolve_url(d->value, base_url);
            }
        }
    }
    if (s->font_faces) {
        for (guint i = 0; i < s->font_faces->len; i++) {
            nd_css_font_face *ff =
                &g_array_index(s->font_faces, nd_css_font_face, i);
            if (!css_url_should_resolve(ff->src_url)) continue;
            char *abs = nd_url_resolve(base_url, ff->src_url);
            if (!abs) continue;
            g_free(ff->src_url);
            ff->src_url = abs;
        }
    }
}

typedef struct nd_css_rule_index {
    GHashTable *by_id;
    GHashTable *by_class;
    GHashTable *by_tag;
    GHashTable *by_attr;
    GArray     *universal;
} nd_css_rule_index;

static void nd_css_rule_index_free(nd_css_rule_index *idx);

gboolean
nd_css_stylesheet_has_container_rules(const nd_css_stylesheet *sh)
{
    return sh && sh->has_container_rules;
}

void
nd_css_stylesheet_free(nd_css_stylesheet *s)
{
    if (!s) return;
    g_ptr_array_free(s->rules, TRUE);
    if (s->imports) g_array_free(s->imports, TRUE);
    if (s->layers) g_hash_table_destroy(s->layers);
    if (s->layer_names) g_ptr_array_free(s->layer_names, TRUE);
    if (s->font_faces) g_array_free(s->font_faces, TRUE);
    if (s->keyframes) g_array_free(s->keyframes, TRUE);
    if (s->property_rules) g_array_free(s->property_rules, TRUE);
    if (s->index) nd_css_rule_index_free(s->index);
    g_free(s);
}

void
nd_css_stylesheet_force_layer(nd_css_stylesheet *s, const char *layer_name)
{
    if (!s || !layer_name || !*layer_name) return;
    GPtrArray *old_names = s->layer_names;
    GHashTable *old_layers = s->layers;
    s->layer_names = NULL;
    s->layers = NULL;
    css_layer_register(s, layer_name);
    if (old_names) {
        for (guint i = 0; i < old_names->len; i++) {
            const char *old = g_ptr_array_index(old_names, i);
            char *full = css_layer_join(layer_name, old);
            css_layer_register(s, full);
            g_free(full);
        }
    }
    if (s->rules) {
        for (guint i = 0; i < s->rules->len; i++) {
            nd_css_rule *r = g_ptr_array_index(s->rules, i);
            char *full = r->layer_name ? css_layer_join(layer_name, r->layer_name)
                                       : g_strdup(layer_name);
            g_free(r->layer_name);
            r->layer_name = full;
        }
    }
    if (s->imports) {
        for (guint i = 0; i < s->imports->len; i++) {
            nd_css_import *im = &g_array_index(s->imports, nd_css_import, i);
            char *full = im->layer_name ? css_layer_join(layer_name, im->layer_name)
                                        : g_strdup(layer_name);
            g_free(im->layer_name);
            im->layer_name = full;
            css_layer_register(s, im->layer_name);
        }
    }
    if (old_layers) g_hash_table_destroy(old_layers);
    if (old_names) g_ptr_array_free(old_names, TRUE);
}

static void
free_bucket_array(gpointer data)
{
    g_array_free((GArray *)data, TRUE);
}

static void
nd_css_rule_index_free(nd_css_rule_index *idx)
{
    if (!idx) return;
    if (idx->by_id)    g_hash_table_destroy(idx->by_id);
    if (idx->by_class) g_hash_table_destroy(idx->by_class);
    if (idx->by_tag)   g_hash_table_destroy(idx->by_tag);
    if (idx->by_attr)  g_hash_table_destroy(idx->by_attr);
    if (idx->universal) g_array_free(idx->universal, TRUE);
    g_free(idx);
}

static void
index_add(GHashTable *table, const char *key, guint rule_idx)
{
    GArray *bucket = g_hash_table_lookup(table, key);
    if (!bucket) {
        bucket = g_array_new(FALSE, FALSE, sizeof(guint));
        g_hash_table_insert(table, g_strdup(key), bucket);
    }
    g_array_append_val(bucket, rule_idx);
}

static void
index_add_lowercase(GHashTable *table, const char *key, guint rule_idx)
{
    char *lk = g_ascii_strdown(key, -1);
    GArray *bucket = g_hash_table_lookup(table, lk);
    if (!bucket) {
        bucket = g_array_new(FALSE, FALSE, sizeof(guint));
        g_hash_table_insert(table, lk, bucket);
        lk = NULL;
    }
    if (bucket) {
        guint last = bucket->len > 0 ? g_array_index(bucket, guint, bucket->len - 1) : G_MAXUINT;
        if (last != rule_idx)
            g_array_append_val(bucket, rule_idx);
    }
    g_free(lk);
}

static nd_css_rule_index *
nd_css_rule_index_build(const nd_css_stylesheet *sheet)
{
    nd_css_rule_index *idx = g_new0(nd_css_rule_index, 1);
    idx->by_id    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_bucket_array);
    idx->by_class = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_bucket_array);
    idx->by_tag   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_bucket_array);
    idx->by_attr  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_bucket_array);
    idx->universal = g_array_new(FALSE, FALSE, sizeof(guint));

    for (guint ri = 0; ri < sheet->rules->len; ri++) {
        const nd_css_rule *r = g_ptr_array_index(sheet->rules, ri);
        gboolean placed_anywhere = FALSE;
        gboolean force_universal = FALSE;
        gboolean had_matchable_selector = FALSE;
        for (guint si = 0; si < r->selectors->len; si++) {
            const nd_css_selector *sel = g_ptr_array_index(r->selectors, si);
            if (sel && sel->pseudo_element != ND_CSS_PE_NONE)
                ((nd_css_stylesheet *)sheet)->pseudo_mask |=
                    (1u << sel->pseudo_element);
            if (!sel || sel->compounds->len == 0) {
                force_universal = TRUE; had_matchable_selector = TRUE; continue;
            }
            const nd_css_simple *subj =
                g_ptr_array_index(sel->compounds, sel->compounds->len - 1);
            if (!subj || subj->never_match) continue;
            had_matchable_selector = TRUE;
            if (subj->id) {
                index_add(idx->by_id, subj->id, ri);
                placed_anywhere = TRUE;
                continue;
            }
            if (subj->classes && subj->classes->len > 0) {
                const char *cls = g_ptr_array_index(subj->classes, 0);
                if (cls && *cls) {
                    index_add(idx->by_class, cls, ri);
                    placed_anywhere = TRUE;
                    continue;
                }
            }
            if (subj->type && strcmp(subj->type, "*") != 0) {
                index_add_lowercase(idx->by_tag, subj->type, ri);
                placed_anywhere = TRUE;
                continue;
            }
            if (subj->attrs && subj->attrs->len > 0) {
                const nd_css_attr_pred *a0 =
                    &g_array_index(subj->attrs, nd_css_attr_pred, 0);
                if (a0 && a0->name && *a0->name) {
                    index_add_lowercase(idx->by_attr, a0->name, ri);
                    placed_anywhere = TRUE;
                    continue;
                }
            }
            force_universal = TRUE;
        }
        if (!had_matchable_selector) continue;
        if (force_universal || !placed_anywhere) {
            guint last = idx->universal->len > 0
                ? g_array_index(idx->universal, guint, idx->universal->len - 1)
                : G_MAXUINT;
            if (last != ri) g_array_append_val(idx->universal, ri);
        }
    }
    return idx;
}

static const nd_css_rule_index *
nd_css_rule_index_ensure(const nd_css_stylesheet *sheet)
{
    if (!sheet) return NULL;
    if (!sheet->index)
        ((nd_css_stylesheet *)sheet)->index = nd_css_rule_index_build(sheet);
    return sheet->index;
}

static gboolean match_selector(const nd_css_selector *sel, const nd_node *el);
static const nd_node *g_css_match_scope;

const nd_node *
nd_css_set_match_scope(const nd_node *scope)
{
    const nd_node *prev = g_css_match_scope;
    g_css_match_scope = scope;
    return prev;
}

static gboolean match_simple(const nd_css_simple *sel, const nd_node *el);

static gboolean
nd_input_is_text_entry(const nd_node *el)
{
    const char *type = nd_element_get_attr(el, "type");
    if (!type || !*type) return TRUE;
    return g_ascii_strcasecmp(type, "text") == 0 ||
           g_ascii_strcasecmp(type, "search") == 0 ||
           g_ascii_strcasecmp(type, "url") == 0 ||
           g_ascii_strcasecmp(type, "tel") == 0 ||
           g_ascii_strcasecmp(type, "email") == 0 ||
           g_ascii_strcasecmp(type, "password") == 0 ||
           g_ascii_strcasecmp(type, "number") == 0;
}

static gboolean
nd_el_is_read_write(const nd_node *el)
{
    if (!el->name) return FALSE;
    if (strcmp(el->name, "input") == 0)
        return nd_input_type_supports_readonly(nd_element_get_attr(el, "type")) &&
               !nd_element_get_attr(el, "readonly") &&
               !nd_element_effectively_disabled(el);
    if (strcmp(el->name, "textarea") == 0)
        return !nd_element_get_attr(el, "readonly") &&
               !nd_element_effectively_disabled(el);
    const char *ce = nd_element_get_attr(el, "contenteditable");
    if (ce && (!*ce || g_ascii_strcasecmp(ce, "true") == 0 ||
               g_ascii_strcasecmp(ce, "plaintext-only") == 0))
        return TRUE;
    return FALSE;
}

static gboolean
nd_el_placeholder_shown(const nd_node *el)
{
    if (!el->name) return FALSE;
    const char *ph = nd_element_get_attr(el, "placeholder");
    if (!ph) return FALSE;
    if (strcmp(el->name, "input") == 0) {
        if (!nd_input_is_text_entry(el)) return FALSE;
        const char *v = nd_element_get_attr(el, "value");
        return !v || !*v;
    }
    if (strcmp(el->name, "textarea") == 0) {
        char *txt = nd_node_collect_text(el);
        gboolean empty = TRUE;
        if (txt) {
            for (const char *q = txt; *q; q++)
                if (!is_ws(*q)) { empty = FALSE; break; }
            g_free(txt);
        }
        return empty;
    }
    return FALSE;
}

static gboolean
nd_el_is_checked(const nd_node *el)
{
    if (nd_node_is_element_named(el, "option"))
        return nd_element_get_attr(el, "selected") != NULL;
    if (!nd_node_is_element_named(el, "input") ||
        !nd_element_get_attr(el, "checked"))
        return FALSE;
    const char *type = nd_element_get_attr(el, "type");
    return type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                    g_ascii_strcasecmp(type, "radio") == 0);
}

static gboolean
nd_el_is_link(const nd_node *el)
{
    if (!nd_element_get_attr(el, "href")) return FALSE;
    return nd_node_is_element_named(el, "a") ||
           nd_node_is_element_named(el, "area");
}

static gboolean
selector_group_matches_element(const GPtrArray *group, const nd_node *el)
{
    for (guint i = 0; group && i < group->len; i++) {
        const nd_css_selector *sub = g_ptr_array_index(group, i);
        if (match_selector(sub, el)) return TRUE;
    }
    return FALSE;
}

static gboolean
nd_css_sibling_counts_for_nth(const nd_node *el, const nd_css_pseudo_pred *pc,
                              int *idx_out)
{
    int idx = 1;
    gboolean reverse = pc->kind == ND_CSS_PC_NTH_LAST_CHILD ||
                       pc->kind == ND_CSS_PC_NTH_LAST_OF_TYPE;
    gboolean typed = pc->kind == ND_CSS_PC_NTH_OF_TYPE ||
                     pc->kind == ND_CSS_PC_NTH_LAST_OF_TYPE;
    const nd_node *s = reverse ? el->next_sibling : el->prev_sibling;
    while (s) {
        if (s->kind == ND_NODE_ELEMENT &&
            (!typed || (el->name && nd_node_is_element_named(s, el->name))) &&
            (!pc->of_group || selector_group_matches_element(pc->of_group, s)))
            idx++;
        s = reverse ? s->next_sibling : s->prev_sibling;
    }
    if (pc->of_group && !selector_group_matches_element(pc->of_group, el))
        return FALSE;
    *idx_out = idx;
    return TRUE;
}

static const char *
nd_css_node_language(const nd_node *el)
{
    for (const nd_node *n = el; n; n = n->parent) {
        if (n->kind != ND_NODE_ELEMENT) continue;
        const char *lang = nd_element_get_attr(n, "lang");
        if (lang && *lang) return lang;
        lang = nd_element_get_attr(n, "xml:lang");
        if (lang && *lang) return lang;
    }
    return NULL;
}

static gboolean
nd_css_lang_one_matches(const char *lang, const char *want)
{
    if (!lang || !want || !*want) return FALSE;
    while (*want == ' ' || *want == '\'' || *want == '"') want++;
    gsize wlen = strlen(want);
    while (wlen > 0 && (is_ws(want[wlen - 1]) ||
                        want[wlen - 1] == '\'' || want[wlen - 1] == '"'))
        wlen--;
    if (wlen == 0) return FALSE;
    if (wlen == 1 && want[0] == '*') return TRUE;
    if (want[0] == '*' && want[1] == '-') {
        const char *needle = want + 2;
        gsize nlen = wlen - 2;
        const char *p = lang;
        while ((p = strchr(p, '-')) != NULL) {
            p++;
            if (g_ascii_strncasecmp(p, needle, nlen) == 0 &&
                (p[nlen] == '\0' || p[nlen] == '-'))
                return TRUE;
        }
        return FALSE;
    }
    if (g_ascii_strncasecmp(lang, want, wlen) != 0) return FALSE;
    return lang[wlen] == '\0' || lang[wlen] == '-';
}

static gboolean
nd_css_lang_matches(const nd_node *el, const char *arg)
{
    const char *lang = nd_css_node_language(el);
    if (!lang || !arg) return FALSE;
    const char *p = arg;
    const char *end = arg + strlen(arg);
    while (p < end) {
        char term = 0;
        const char *seg = css_scan_until(p, end, ",", &term);
        char *want = css_trim_dup_range(p, seg);
        gboolean ok = nd_css_lang_one_matches(lang, want);
        g_free(want);
        if (ok) return TRUE;
        p = term == ',' ? seg + 1 : seg;
    }
    return FALSE;
}

static const char *
nd_css_node_dir(const nd_node *el)
{
    for (const nd_node *n = el; n; n = n->parent) {
        if (n->kind != ND_NODE_ELEMENT) continue;
        const char *dir = nd_element_get_attr(n, "dir");
        if (!dir) continue;
        if (g_ascii_strcasecmp(dir, "ltr") == 0) return "ltr";
        if (g_ascii_strcasecmp(dir, "rtl") == 0) return "rtl";
    }
    return "ltr";
}

static gboolean
nd_css_value_matches_pattern(const char *value, const char *pattern)
{
    if (!pattern || !*pattern) return TRUE;
    char *anchored = g_strdup_printf("^(?:%s)$", pattern);
    GError *err = NULL;
    GRegex *re = g_regex_new(anchored, 0, 0, &err);
    g_free(anchored);
    if (!re) { g_clear_error(&err); return TRUE; }
    gboolean ok = g_regex_match(re, value ? value : "", 0, NULL);
    g_regex_unref(re);
    return ok;
}

static gboolean
nd_css_node_will_validate(const nd_node *el)
{
    if (!el || el->kind != ND_NODE_ELEMENT || !el->name) return FALSE;
    gboolean is_input = strcmp(el->name, "input") == 0;
    if (!is_input &&
        strcmp(el->name, "textarea") != 0 &&
        strcmp(el->name, "select") != 0)
        return FALSE;
    if (nd_element_effectively_disabled(el)) return FALSE;
    if (nd_form_control_readonly_bars_validation(el)) return FALSE;
    const char *type = is_input ? nd_element_get_attr(el, "type") : NULL;
    if (type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                 g_ascii_strcasecmp(type, "button") == 0 ||
                 g_ascii_strcasecmp(type, "reset")  == 0 ||
                 g_ascii_strcasecmp(type, "image")  == 0 ||
                 g_ascii_strcasecmp(type, "hidden") == 0))
        return FALSE;
    return TRUE;
}

static char *
nd_css_control_value_dup(const nd_node *el)
{
    if (!el || !el->name) return g_strdup("");
    if (strcmp(el->name, "textarea") == 0)
        return nd_node_collect_text(el);
    if (strcmp(el->name, "select") == 0) {
        const nd_node *opt = nd_element_get_attr(el, "multiple")
            ? nd_select_first_selected_option(el)
            : nd_select_chosen_option(el);
        return opt ? nd_option_value_dup(opt) : g_strdup("");
    }
    return g_strdup(nd_element_get_attr(el, "value") ?
                    nd_element_get_attr(el, "value") : "");
}

static gboolean
nd_css_control_is_valid(const nd_node *el)
{
    if (!nd_css_node_will_validate(el)) return FALSE;
    const char *custom = nd_element_get_attr(el, ND_CUSTOM_VALIDITY_ATTR);
    if (custom && *custom) return FALSE;
    char *owned = nd_css_control_value_dup(el);
    const char *value = owned ? owned : "";
    gboolean valid = TRUE;
    const char *type = el->name && strcmp(el->name, "input") == 0
        ? nd_element_get_attr(el, "type") : NULL;
    if (nd_form_control_supports_required(el) &&
        nd_element_get_attr(el, "required") &&
        nd_form_control_value_missing(el, value, nd_node_root(el)))
        valid = FALSE;
    if (valid && *value && type) {
        if (g_ascii_strcasecmp(type, "email") == 0) {
            if (!nd_input_email_value_valid(el, value))
                valid = FALSE;
        } else if (g_ascii_strcasecmp(type, "url") == 0) {
            if (!nd_url_is_valid_absolute(value))
                valid = FALSE;
        } else if (nd_input_type_has_number_value(type)) {
            double parsed;
            if (!nd_input_value_to_number(type, value, &parsed)) valid = FALSE;
        }
        if (valid) {
            gboolean under = FALSE, over = FALSE;
            if (nd_input_value_range_state(el, value, &under, &over) &&
                (under || over))
                valid = FALSE;
        }
        if (valid && nd_input_value_step_mismatch(el, value))
            valid = FALSE;
    }
    if (valid && *value &&
        el->name && strcmp(el->name, "input") == 0 &&
        nd_input_type_supports_text_constraints(type) &&
        !nd_css_value_matches_pattern(value, nd_element_get_attr(el, "pattern")))
        valid = FALSE;
    if (valid && *value && nd_form_control_length_limits_apply(el)) {
        glong vlen = (glong)g_utf8_strlen(value, -1);
        const char *minlen = nd_element_get_attr(el, "minlength");
        const char *maxlen = nd_element_get_attr(el, "maxlength");
        if (minlen && vlen < (glong)nd_parse_int(minlen, 0, 0, 1000000))
            valid = FALSE;
        if (maxlen && vlen > (glong)nd_parse_int(maxlen, 0, 0, 1000000))
            valid = FALSE;
    }
    g_free(owned);
    return valid;
}

static gboolean
has_descendant_match(const nd_css_simple *cmp, const nd_node *anchor, int depth)
{
    if (depth >= 512) return FALSE;
    for (const nd_node *c = anchor->first_child; c; c = c->next_sibling) {
        if (c->kind != ND_NODE_ELEMENT) continue;
        if (match_simple(cmp, c)) return TRUE;
        if (has_descendant_match(cmp, c, depth + 1)) return TRUE;
    }
    return FALSE;
}

static gboolean
has_relative_matches(const nd_css_selector *rel, const nd_node *anchor)
{
    if (!rel || rel->pseudo_element != ND_CSS_PE_NONE) return FALSE;
    if (rel->compounds->len != 1) return FALSE;
    const nd_css_simple *cmp = g_ptr_array_index(rel->compounds, 0);
    nd_css_comb comb = g_array_index(rel->combinators, nd_css_comb, 0);
    if (comb == ND_CSS_COMB_CHILD) {
        for (const nd_node *c = anchor->first_child; c; c = c->next_sibling)
            if (c->kind == ND_NODE_ELEMENT && match_simple(cmp, c))
                return TRUE;
        return FALSE;
    }
    if (comb == ND_CSS_COMB_ADJACENT) {
        const nd_node *s = anchor->next_sibling;
        while (s && s->kind != ND_NODE_ELEMENT) s = s->next_sibling;
        return s && match_simple(cmp, s);
    }
    if (comb == ND_CSS_COMB_SIBLING) {
        for (const nd_node *s = anchor->next_sibling; s; s = s->next_sibling)
            if (s->kind == ND_NODE_ELEMENT && match_simple(cmp, s))
                return TRUE;
        return FALSE;
    }
    return has_descendant_match(cmp, anchor, 0);
}

static gboolean
has_group_matches(const GPtrArray *group, const nd_node *anchor)
{
    for (guint j = 0; j < group->len; j++) {
        const nd_css_selector *sub = g_ptr_array_index(group, j);
        if (has_relative_matches(sub, anchor)) return TRUE;
    }
    return FALSE;
}

static gboolean
match_simple(const nd_css_simple *sel, const nd_node *el)
{
    if (sel->never_match) return FALSE;
    if (!el || el->kind != ND_NODE_ELEMENT) return FALSE;
    if (sel->type && strcmp(sel->type, "*") != 0) {
        if (!el->name || strcmp(sel->type, el->name) != 0) return FALSE;
    }
    if (sel->id) {
        const char *id = nd_element_get_attr(el, "id");
        if (!id || strcmp(id, sel->id) != 0) return FALSE;
    }
    if (sel->classes->len > 0) {
        for (guint i = 0; i < sel->classes->len; i++) {
            const char *want = g_ptr_array_index(sel->classes, i);
            if (!nd_node_has_class(el, want, strlen(want)))
                return FALSE;
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
                gboolean ci = a->ci;
                switch (a->op) {
                case ND_CSS_ATTR_EQ:
                    if (ci ? g_ascii_strcasecmp(v, a->value)
                           : strcmp(v, a->value)) return FALSE;
                    break;
                case ND_CSS_ATTR_PREFIX:
                    if (vl < wl) return FALSE;
                    if (ci ? g_ascii_strncasecmp(v, a->value, wl)
                           : strncmp(v, a->value, wl)) return FALSE;
                    break;
                case ND_CSS_ATTR_SUFFIX:
                    if (vl < wl) return FALSE;
                    if (ci ? g_ascii_strcasecmp(v + vl - wl, a->value)
                           : strcmp(v + vl - wl, a->value)) return FALSE;
                    break;
                case ND_CSS_ATTR_SUBSTR:
                    if (ci) {
                        gboolean found = FALSE;
                        for (gsize i2 = 0; i2 + wl <= vl; i2++) {
                            if (g_ascii_strncasecmp(v + i2, a->value, wl) == 0) {
                                found = TRUE; break;
                            }
                        }
                        if (!found) return FALSE;
                    } else {
                        if (!strstr(v, a->value)) return FALSE;
                    }
                    break;
                case ND_CSS_ATTR_WORD: {
                    gboolean found = FALSE;
                    const char *s = v;
                    while (*s) {
                        while (*s && is_ws(*s)) s++;
                        const char *tok = s;
                        while (*s && !is_ws(*s)) s++;
                        if ((gsize)(s - tok) == wl &&
                            (ci ? g_ascii_strncasecmp(tok, a->value, wl)
                                : strncmp(tok, a->value, wl)) == 0) {
                            found = TRUE; break;
                        }
                    }
                    if (!found) return FALSE;
                    break;
                }
                case ND_CSS_ATTR_HYPHEN: {
                    if (vl < wl) return FALSE;
                    if (ci ? g_ascii_strncasecmp(v, a->value, wl)
                           : strncmp(v, a->value, wl)) return FALSE;
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
            case ND_CSS_PC_ONLY_OF_TYPE: {
                if (!el->name) return FALSE;
                for (const nd_node *s = el->prev_sibling; s; s = s->prev_sibling)
                    if (nd_node_is_element_named(s, el->name)) return FALSE;
                for (const nd_node *s = el->next_sibling; s; s = s->next_sibling)
                    if (nd_node_is_element_named(s, el->name)) return FALSE;
                break;
            }
            case ND_CSS_PC_FIRST_OF_TYPE: {
                if (!el->name) return FALSE;
                for (const nd_node *s = el->prev_sibling; s; s = s->prev_sibling)
                    if (nd_node_is_element_named(s, el->name)) return FALSE;
                break;
            }
            case ND_CSS_PC_LAST_OF_TYPE: {
                if (!el->name) return FALSE;
                for (const nd_node *s = el->next_sibling; s; s = s->next_sibling)
                    if (nd_node_is_element_named(s, el->name)) return FALSE;
                break;
            }
            case ND_CSS_PC_EMPTY:
                if (el->first_child) return FALSE;
                break;
            case ND_CSS_PC_ROOT:
                if (el->parent && el->parent->kind == ND_NODE_ELEMENT) return FALSE;
                break;
            case ND_CSS_PC_SCOPE:
                if (g_css_match_scope) {
                    if (el != g_css_match_scope) return FALSE;
                } else if (el->parent && el->parent->kind == ND_NODE_ELEMENT) {
                    return FALSE;
                }
                break;
            case ND_CSS_PC_CHECKED:
                if (!nd_el_is_checked(el))
                    return FALSE;
                break;
            case ND_CSS_PC_DISABLED:
                if (!nd_element_supports_disabled(el) ||
                    !nd_element_effectively_disabled(el))
                    return FALSE;
                break;
            case ND_CSS_PC_ENABLED:
                if (!nd_element_supports_disabled(el) ||
                    nd_element_effectively_disabled(el))
                    return FALSE;
                break;
            case ND_CSS_PC_REQUIRED:
                if (!nd_form_control_supports_required(el) ||
                    !nd_element_get_attr(el, "required"))
                    return FALSE;
                break;
            case ND_CSS_PC_OPTIONAL:
                if (!nd_form_control_supports_required(el) ||
                    nd_element_get_attr(el, "required"))
                    return FALSE;
                break;
            case ND_CSS_PC_VALID:
                if (!nd_css_node_will_validate(el) ||
                    !nd_css_control_is_valid(el))
                    return FALSE;
                break;
            case ND_CSS_PC_INVALID:
                if (!nd_css_node_will_validate(el) ||
                    nd_css_control_is_valid(el))
                    return FALSE;
                break;
            case ND_CSS_PC_NTH_CHILD:
            case ND_CSS_PC_NTH_LAST_CHILD:
            case ND_CSS_PC_NTH_LAST_OF_TYPE:
            case ND_CSS_PC_NTH_OF_TYPE: {
                int idx = 1;
                if (!nd_css_sibling_counts_for_nth(el, pc, &idx)) return FALSE;
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
            case ND_CSS_PC_ANY_LINK:
                if (!nd_el_is_link(el)) return FALSE;
                break;
            case ND_CSS_PC_VISITED:
                return FALSE;
            case ND_CSS_PC_HOVER:
            case ND_CSS_PC_ACTIVE:
                return FALSE;
            case ND_CSS_PC_FOCUS:
                if (!g_css_focus_node || el != g_css_focus_node) return FALSE;
                break;
            case ND_CSS_PC_FOCUS_WITHIN: {
                if (!g_css_focus_node) return FALSE;
                const nd_node *f = g_css_focus_node;
                gboolean within = FALSE;
                for (; f; f = f->parent)
                    if (f == el) { within = TRUE; break; }
                if (!within) return FALSE;
                break;
            }
            case ND_CSS_PC_TARGET: {
                if (!g_target_fragment) return FALSE;
                const char *eid = nd_element_get_attr(el, "id");
                if (eid && strcmp(eid, g_target_fragment) == 0) break;
                if (el->name && g_ascii_strcasecmp(el->name, "a") == 0) {
                    const char *nm = nd_element_get_attr(el, "name");
                    if (nm && strcmp(nm, g_target_fragment) == 0) break;
                }
                return FALSE;
            }
            case ND_CSS_PC_DEFINED:
                if (!el->name) return FALSE;
                if (!strchr(el->name, '-')) break;
                if (nd_css_is_defined_element(el->name)) break;
                return FALSE;
            case ND_CSS_PC_PLACEHOLDER_SHOWN:
                if (!nd_el_placeholder_shown(el)) return FALSE;
                break;
            case ND_CSS_PC_READ_WRITE:
                if (!nd_el_is_read_write(el)) return FALSE;
                break;
            case ND_CSS_PC_READ_ONLY:
                if (nd_el_is_read_write(el)) return FALSE;
                break;
            case ND_CSS_PC_LANG:
                if (!nd_css_lang_matches(el, pc->arg)) return FALSE;
                break;
            case ND_CSS_PC_DIR:
                if (!pc->arg || strcmp(nd_css_node_dir(el), pc->arg) != 0)
                    return FALSE;
                break;
            case ND_CSS_PC_OPEN:
                if ((!nd_node_is_element_named(el, "details") &&
                     !nd_node_is_element_named(el, "dialog")) ||
                    !nd_element_get_attr(el, "open"))
                    return FALSE;
                break;
            case ND_CSS_PC_POPOVER_OPEN:
                if (!nd_element_get_attr(el, "popover") ||
                    !nd_element_get_attr(el, "data-nd-popover-open"))
                    return FALSE;
                break;
            }
        }
    }
    if (sel->matches_any) {
        for (guint i = 0; i < sel->matches_any->len; i++) {
            const GPtrArray *group = g_ptr_array_index(sel->matches_any, i);
            gboolean any = FALSE;
            for (guint j = 0; j < group->len; j++) {
                const nd_css_selector *sub = g_ptr_array_index(group, j);
                if (match_selector(sub, el)) { any = TRUE; break; }
            }
            if (!any) return FALSE;
        }
    }
    if (sel->matches_none) {
        for (guint i = 0; i < sel->matches_none->len; i++) {
            const GPtrArray *group = g_ptr_array_index(sel->matches_none, i);
            for (guint j = 0; j < group->len; j++) {
                const nd_css_selector *sub = g_ptr_array_index(group, j);
                if (match_selector(sub, el)) return FALSE;
            }
        }
    }
    if (sel->has_groups) {
        for (guint i = 0; i < sel->has_groups->len; i++) {
            const GPtrArray *group = g_ptr_array_index(sel->has_groups, i);
            if (!has_group_matches(group, el)) return FALSE;
        }
    }
    return TRUE;
}


char *
nd_inline_style_get(const char *style, const char *prop)
{
    if (!style || !prop) return NULL;
    gsize plen = strlen(prop);
    const char *p = style;
    const char *end = style + strlen(style);
    while (p < end) {
        p = css_skip_ws_comments(p, end);
        while (p < end && *p == ';') {
            p++;
            p = css_skip_ws_comments(p, end);
        }
        if (p >= end) break;
        const char *kstart = p;
        char term = 0;
        const char *kend = css_scan_until(p, end, ":;", &term);
        char *key = css_trim_dup_range(kstart, kend);
        if (term != ':') {
            g_free(key);
            p = term == ';' ? kend + 1 : kend;
            continue;
        }
        p = css_skip_ws_comments(kend + 1, end);
        const char *vstart = p;
        const char *vend = css_scan_declaration_value(p, end, &term);
        char *value = css_trim_dup_range(vstart, vend);
        gboolean match = strlen(key) == plen &&
                         g_ascii_strcasecmp(key, prop) == 0;
        g_free(key);
        if (match) return value;
        g_free(value);
        p = term == ';' ? vend + 1 : vend;
    }
    return NULL;
}

char *
nd_inline_style_set(const char *style, const char *prop, const char *value)
{
    if (!prop) return g_strdup(style ? style : "");
    GString *out = g_string_new(NULL);
    gboolean found = FALSE;
    gsize plen = prop ? strlen(prop) : 0;
    const char *p = style ? style : "";
    const char *end = p + strlen(p);
    while (p < end) {
        p = css_skip_ws_comments(p, end);
        while (p < end && *p == ';') {
            p++;
            p = css_skip_ws_comments(p, end);
        }
        if (p >= end) break;
        const char *kstart = p;
        char term = 0;
        const char *kend = css_scan_until(p, end, ":;", &term);
        char *key = css_trim_dup_range(kstart, kend);
        if (term != ':') {
            g_free(key);
            p = term == ';' ? kend + 1 : kend;
            continue;
        }
        p = css_skip_ws_comments(kend + 1, end);
        const char *vstart = p;
        const char *vend = css_scan_declaration_value(p, end, &term);
        char *old_value = css_trim_dup_range(vstart, vend);
        gboolean match = strlen(key) == plen && prop &&
                         g_ascii_strcasecmp(key, prop) == 0;
        if (match) {
            if (!value || !*value) {
                found = TRUE;
                g_free(key);
                g_free(old_value);
                p = term == ';' ? vend + 1 : vend;
                continue;
            }
            if (out->len > 0) g_string_append(out, "; ");
            g_string_append(out, key);
            g_string_append(out, ": ");
            g_string_append(out, value);
            found = TRUE;
        } else {
            if (out->len > 0) g_string_append(out, "; ");
            g_string_append(out, key);
            g_string_append(out, ": ");
            g_string_append(out, old_value);
        }
        g_free(key);
        g_free(old_value);
        p = term == ';' ? vend + 1 : vend;
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
        const char *iter_start = p;
        nd_css_selector *sel = parse_one_selector(&p, end, 0);
        if (sel) g_ptr_array_add(out, sel);
        while (p < end && is_ws(*p)) p++;
        if (p < end && *p == ',') p++;
        else if (p == iter_start) break;
    }
    return out;
}

gboolean
nd_css_selector_matches(const nd_css_selector *sel, const nd_node *el)
{
    return match_selector(sel, el);
}

static gboolean
match_selector_structural(const nd_css_selector *sel, const nd_node *el)
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

static gboolean
match_selector(const nd_css_selector *sel, const nd_node *el)
{
    if (!sel) return FALSE;
    if (sel->pseudo_element != ND_CSS_PE_NONE) return FALSE;
    return match_selector_structural(sel, el);
}

static gboolean
match_selector_for_pe(const nd_css_selector *sel, const nd_node *el,
                      nd_css_pseudo_element pe)
{
    if (!sel) return FALSE;
    if (sel->pseudo_element != pe) return FALSE;
    return match_selector_structural(sel, el);
}

static gboolean
selector_group_matches_with_scope(const GPtrArray *group, const nd_node *el,
                                  const nd_node *scope)
{
    const nd_node *prev = g_css_match_scope;
    g_css_match_scope = scope;
    gboolean matched = FALSE;
    for (guint i = 0; group && i < group->len; i++) {
        const nd_css_selector *sel = g_ptr_array_index(group, i);
        if (match_selector(sel, el)) {
            matched = TRUE;
            break;
        }
    }
    g_css_match_scope = prev;
    return matched;
}

static gboolean
css_scope_root_matches(const nd_css_scope *scope, const nd_node *el)
{
    return selector_group_matches_with_scope(scope ? scope->roots : NULL,
                                            el, g_css_match_scope);
}

static int
css_scope_hops(const nd_node *root, const nd_node *el)
{
    int hops = 0;
    for (const nd_node *n = el; n; n = n->parent, hops++)
        if (n == root) return hops;
    return INT_MAX;
}

static gboolean
css_scope_limit_excludes(const nd_css_scope *scope, const nd_node *root,
                         const nd_node *el)
{
    if (!scope || !scope->limits) return FALSE;
    for (const nd_node *n = el; n; n = n->parent) {
        if (n->kind == ND_NODE_ELEMENT &&
            selector_group_matches_with_scope(scope->limits, n, root))
            return TRUE;
        if (n == root) break;
    }
    return FALSE;
}

static gboolean
css_scope_contains(const nd_css_scope *scope, const nd_node *root,
                   const nd_node *el)
{
    if (!scope || !root || !el) return FALSE;
    if (css_scope_hops(root, el) == INT_MAX) return FALSE;
    return !css_scope_limit_excludes(scope, root, el);
}

static gboolean
css_scope_applies_to(const nd_css_scope *scope, const nd_node *el)
{
    for (const nd_node *root = el; root; root = root->parent) {
        if (root->kind != ND_NODE_ELEMENT) continue;
        if (!css_scope_root_matches(scope, root)) continue;
        if (css_scope_contains(scope, root, el)) return TRUE;
    }
    return FALSE;
}

static gboolean
rule_outer_scopes_apply(const nd_css_rule *r, guint upto,
                        const nd_node *root, const nd_node *el)
{
    for (guint i = 0; r && r->scopes && i < upto; i++) {
        const nd_css_scope *scope = g_ptr_array_index(r->scopes, i);
        if (!css_scope_applies_to(scope, root)) return FALSE;
        if (!css_scope_applies_to(scope, el)) return FALSE;
    }
    return TRUE;
}

static gboolean
rule_selector_matches(const nd_css_rule *r, const nd_css_selector *sel,
                      const nd_node *el, nd_css_pseudo_element pe,
                      int *scope_order)
{
    if (scope_order) *scope_order = 0;
    if (!r || !r->scopes || r->scopes->len == 0) {
        return pe == ND_CSS_PE_NONE ? match_selector(sel, el)
                                    : match_selector_for_pe(sel, el, pe);
    }
    guint inner_i = r->scopes->len - 1;
    const nd_css_scope *inner = g_ptr_array_index(r->scopes, inner_i);
    int best = 0;
    for (const nd_node *root = el; root; root = root->parent) {
        if (root->kind != ND_NODE_ELEMENT) continue;
        if (!css_scope_root_matches(inner, root)) continue;
        if (!css_scope_contains(inner, root, el)) continue;
        if (!rule_outer_scopes_apply(r, inner_i, root, el)) continue;
        const nd_node *prev = g_css_match_scope;
        g_css_match_scope = root;
        gboolean matched = pe == ND_CSS_PE_NONE
            ? match_selector(sel, el)
            : match_selector_for_pe(sel, el, pe);
        g_css_match_scope = prev;
        if (!matched) continue;
        int hops = css_scope_hops(root, el);
        if (hops != INT_MAX) {
            int order = INT_MAX - hops;
            if (order > best) best = order;
        }
    }
    if (best <= 0) return FALSE;
    if (scope_order) *scope_order = best;
    return TRUE;
}

static nd_style *g_style_pool[16384];
static int g_style_pool_n;

static nd_style *
nd_style_alloc(void)
{
    if (g_style_pool_n > 0) {
        nd_style *s = g_style_pool[--g_style_pool_n];
        memset(s, 0, sizeof(*s));
        return s;
    }
    return g_new0(nd_style, 1);
}

static void
nd_style_free(nd_style *s)
{
    if (!s) return;
    for (int i = 0; i < ND_CSS_PROP_COUNT; i++)
        nd_css_value_free(s->values[i]);
    nd_style_free(s->before);
    nd_style_free(s->after);
    nd_style_free(s->first_letter);
    nd_style_free(s->first_line);
    nd_style_free(s->placeholder);
    nd_style_free(s->selection);
    nd_style_free(s->marker);
    nd_style_free(s->backdrop);
    if (s->vars) g_hash_table_destroy(s->vars);
    if (g_style_pool_n < (int)G_N_ELEMENTS(g_style_pool))
        g_style_pool[g_style_pool_n++] = s;
    else
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

char *
nd_css_value_serialize(const nd_css_value *v)
{
    if (!v) return g_strdup("");
    switch (v->kind) {
    case ND_CSS_V_KEYWORD:
        return g_strdup(v->u.keyword ? v->u.keyword : "");
    case ND_CSS_V_COLOR:
        if (v->u.color.a == 255)
            return g_strdup_printf("rgb(%u, %u, %u)",
                v->u.color.r, v->u.color.g, v->u.color.b);
        return g_strdup_printf("rgba(%u, %u, %u, %g)",
            v->u.color.r, v->u.color.g, v->u.color.b, v->u.color.a / 255.0);
    case ND_CSS_V_LENGTH: {
        const char *unit = "";
        switch (v->u.length.unit) {
        case ND_CSS_UNIT_PX:      unit = "px"; break;
        case ND_CSS_UNIT_EM:      unit = "em"; break;
        case ND_CSS_UNIT_REM:     unit = "rem"; break;
        case ND_CSS_UNIT_PERCENT: unit = "%";  break;
        case ND_CSS_UNIT_NUMBER:  unit = "";   break;
        case ND_CSS_UNIT_VW:      unit = "vw"; break;
        case ND_CSS_UNIT_VH:      unit = "vh"; break;
        case ND_CSS_UNIT_VMIN:    unit = "vmin"; break;
        case ND_CSS_UNIT_VMAX:    unit = "vmax"; break;
        case ND_CSS_UNIT_CQW:     unit = "cqw"; break;
        case ND_CSS_UNIT_CQH:     unit = "cqh"; break;
        case ND_CSS_UNIT_CQMIN:   unit = "cqmin"; break;
        case ND_CSS_UNIT_CQMAX:   unit = "cqmax"; break;
        }
        return g_strdup_printf("%g%s", v->u.length.v, unit);
    }
    case ND_CSS_V_SIZE: {
        GString *s = g_string_new(NULL);
        if (v->u.size.w_auto) {
            g_string_append(s, "auto");
        } else {
            const char *unit = "";
            switch (v->u.size.w_unit) {
            case ND_CSS_UNIT_PX:      unit = "px"; break;
            case ND_CSS_UNIT_EM:      unit = "em"; break;
            case ND_CSS_UNIT_REM:     unit = "rem"; break;
            case ND_CSS_UNIT_PERCENT: unit = "%";  break;
            case ND_CSS_UNIT_NUMBER:  unit = "";   break;
            case ND_CSS_UNIT_VW:      unit = "vw"; break;
            case ND_CSS_UNIT_VH:      unit = "vh"; break;
            case ND_CSS_UNIT_VMIN:    unit = "vmin"; break;
            case ND_CSS_UNIT_VMAX:    unit = "vmax"; break;
            case ND_CSS_UNIT_CQW:     unit = "cqw"; break;
            case ND_CSS_UNIT_CQH:     unit = "cqh"; break;
            case ND_CSS_UNIT_CQMIN:   unit = "cqmin"; break;
            case ND_CSS_UNIT_CQMAX:   unit = "cqmax"; break;
            }
            g_string_append_printf(s, "%g%s", v->u.size.w, unit);
        }
        g_string_append_c(s, ' ');
        if (v->u.size.h_auto) {
            g_string_append(s, "auto");
        } else {
            const char *unit = "";
            switch (v->u.size.h_unit) {
            case ND_CSS_UNIT_PX:      unit = "px"; break;
            case ND_CSS_UNIT_EM:      unit = "em"; break;
            case ND_CSS_UNIT_REM:     unit = "rem"; break;
            case ND_CSS_UNIT_PERCENT: unit = "%";  break;
            case ND_CSS_UNIT_NUMBER:  unit = "";   break;
            case ND_CSS_UNIT_VW:      unit = "vw"; break;
            case ND_CSS_UNIT_VH:      unit = "vh"; break;
            case ND_CSS_UNIT_VMIN:    unit = "vmin"; break;
            case ND_CSS_UNIT_VMAX:    unit = "vmax"; break;
            case ND_CSS_UNIT_CQW:     unit = "cqw"; break;
            case ND_CSS_UNIT_CQH:     unit = "cqh"; break;
            case ND_CSS_UNIT_CQMIN:   unit = "cqmin"; break;
            case ND_CSS_UNIT_CQMAX:   unit = "cqmax"; break;
            }
            g_string_append_printf(s, "%g%s", v->u.size.h, unit);
        }
        return g_string_free(s, FALSE);
    }
    case ND_CSS_V_CALC:
        if (v->u.calc.pct == 0)
            return g_strdup_printf("%gpx", v->u.calc.px);
        if (v->u.calc.px == 0)
            return g_strdup_printf("%g%%", v->u.calc.pct);
        return g_strdup_printf("calc(%gpx + %g%%)", v->u.calc.px, v->u.calc.pct);
    case ND_CSS_V_SHADOW: {
        GString *s = g_string_new(NULL);
        for (int i = 0; i < v->u.shadow.n; i++) {
            const nd_css_shadow *sh = &v->u.shadow.s[i];
            if (i > 0) g_string_append(s, ", ");
            g_string_append_printf(s,
                "%s%gpx %gpx %gpx %gpx rgba(%u,%u,%u,%g)",
                sh->inset ? "inset " : "",
                sh->x, sh->y, sh->blur, sh->spread,
                sh->r, sh->g, sh->b, sh->a / 255.0);
        }
        return g_string_free(s, FALSE);
    }
    case ND_CSS_V_GRADIENT: {
        GString *s = g_string_new(NULL);
        if (v->u.gradient.conic) {
            g_string_append_printf(s, "conic-gradient(from %ddeg",
                                   v->u.gradient.from_deg);
        } else if (v->u.gradient.radial) {
            g_string_append(s, "radial-gradient(circle");
        } else {
            g_string_append_printf(s, "linear-gradient(%ddeg",
                                   v->u.gradient.angle_deg);
        }
        for (int i = 0; i < v->u.gradient.n_stops; i++) {
            const nd_css_gradient_stop *st = &v->u.gradient.stops[i];
            g_string_append_printf(s, ", rgba(%u,%u,%u,%g) %g%%",
                st->r, st->g, st->b, st->a / 255.0, st->pos * 100.0);
        }
        g_string_append_c(s, ')');
        return g_string_free(s, FALSE);
    }
    case ND_CSS_V_TRACKS: {
        GString *s = g_string_new(NULL);
        if (v->u.tracks.subgrid)
            return g_strdup("subgrid");
        for (int i = 0; i < v->u.tracks.n; i++) {
            if (i) g_string_append_c(s, ' ');
            const nd_css_track *t = &v->u.tracks.tracks[i];
            switch (t->kind) {
            case ND_CSS_TRACK_PX:      g_string_append_printf(s, "%gpx", t->v); break;
            case ND_CSS_TRACK_PERCENT: g_string_append_printf(s, "%g%%", t->v); break;
            case ND_CSS_TRACK_FR:      g_string_append_printf(s, "%gfr", t->v); break;
            case ND_CSS_TRACK_AUTO:    g_string_append(s, "auto"); break;
            }
        }
        return g_string_free(s, FALSE);
    }
    case ND_CSS_V_URL:
        return g_strdup_printf("url(\"%s\")", v->u.url ? v->u.url : "");
    case ND_CSS_V_AREAS: {
        GString *s = g_string_new(NULL);
        for (int r = 0; r < v->u.areas.n_rows; r++) {
            if (r) g_string_append_c(s, ' ');
            g_string_append_c(s, '"');
            for (int c = 0; c < v->u.areas.n_cols; c++) {
                const char *name = ".";
                for (int k = 0; k < v->u.areas.n_rects; k++) {
                    const nd_css_area_rect *rect = &v->u.areas.rects[k];
                    if (r >= rect->r0 && r <= rect->r1 &&
                        c >= rect->c0 && c <= rect->c1) {
                        name = rect->name; break;
                    }
                }
                if (c) g_string_append_c(s, ' ');
                g_string_append(s, name);
            }
            g_string_append_c(s, '"');
        }
        return g_string_free(s, FALSE);
    }
    case ND_CSS_V_ANIM: {
        GString *s = g_string_new(NULL);
        for (int i = 0; i < v->u.anim.n; i++) {
            if (i) g_string_append(s, ", ");
            const nd_css_anim_entry *e = &v->u.anim.entries[i];
            if (e->name) g_string_append_printf(s, "%s ", e->name);
            g_string_append_printf(s, "%gms", e->duration_ms);
            if (e->delay_ms != 0)
                g_string_append_printf(s, " %gms", e->delay_ms);
        }
        return g_string_free(s, FALSE);
    }
    case ND_CSS_V_TRANSFORM: {
        GString *s = g_string_new(NULL);
        for (int i = 0; i < v->u.transform.n_ops; i++) {
            const nd_css_transform_op *op = &v->u.transform.ops[i];
            if (i) g_string_append_c(s, ' ');
            switch (op->kind) {
            case ND_CSS_TFN_TRANSLATE:
                g_string_append_printf(s, "translate(%g%s, %g%s)",
                    op->a, op->a_is_percent ? "%" : "px",
                    op->b, op->b_is_percent ? "%" : "px");
                break;
            case ND_CSS_TFN_ROTATE:
                g_string_append_printf(s, "rotate(%gdeg)", op->a);
                break;
            case ND_CSS_TFN_SCALE:
                g_string_append_printf(s, "scale(%g, %g)", op->a, op->b);
                break;
            case ND_CSS_TFN_SKEW:
                g_string_append_printf(s, "skew(%gdeg, %gdeg)", op->a, op->b);
                break;
            case ND_CSS_TFN_MATRIX:
                g_string_append_printf(s, "matrix(%g, %g, %g, %g, %g, %g)",
                    op->a, op->b, op->c, op->d, op->e, op->f);
                break;
            }
        }
        return g_string_free(s, FALSE);
    }
    }
    return g_strdup("");
}

typedef struct match_entry {
    int          origin;
    int          spec_a, spec_b, spec_c;
    int          sheet_index;
    int          layer_order;
    int          scope_order;
    int          source_order;
    int          decl_order;
    gboolean     important;
    nd_css_value *value;
    nd_css_prop  prop;
} match_entry;

typedef struct var_match {
    int origin;
    int spec_a, spec_b, spec_c;
    int sheet_index;
    int layer_order;
    int scope_order;
    int source_order;
    int decl_order;
    gboolean important;
    const char *name;
    const char *text;
} var_match;

typedef struct pending_match {
    int origin;
    int spec_a, spec_b, spec_c;
    int sheet_index;
    int layer_order;
    int scope_order;
    int source_order;
    int decl_order_base;
    nd_css_pending_decl *pd;
} pending_match;

static int
css_layer_cmp(int a, int b, gboolean important)
{
    if (a == b) return 0;
    if (important) return a > b ? -1 : 1;
    return a < b ? -1 : 1;
}

static int
css_layer_rank_for(GHashTable *layer_ranks, const char *layer_name)
{
    if (!layer_name || !layer_ranks) return ND_CSS_LAYER_NONE;
    gpointer v = g_hash_table_lookup(layer_ranks, layer_name);
    return v ? GPOINTER_TO_INT(v) - 1 : ND_CSS_LAYER_NONE;
}

static void
css_layer_rank_add_sheet(GHashTable *layer_ranks,
                         const nd_css_stylesheet *sheet)
{
    if (!layer_ranks || !sheet || !sheet->layer_names) return;
    for (guint i = 0; i < sheet->layer_names->len; i++) {
        const char *name = g_ptr_array_index(sheet->layer_names, i);
        if (!name || g_hash_table_contains(layer_ranks, name)) continue;
        int rank = (int)g_hash_table_size(layer_ranks);
        g_hash_table_insert(layer_ranks, (gpointer)name,
                            GINT_TO_POINTER(rank + 1));
    }
}

static int
match_cmp(gconstpointer a_, gconstpointer b_)
{
    const match_entry *a = a_;
    const match_entry *b = b_;
    if (a->important != b->important) return a->important ? 1 : -1;
    if (a->origin    != b->origin)    return a->origin < b->origin ? -1 : 1;
    int layer_cmp = css_layer_cmp(a->layer_order, b->layer_order, a->important);
    if (layer_cmp != 0) return layer_cmp;
    if (a->spec_a    != b->spec_a)    return a->spec_a < b->spec_a ? -1 : 1;
    if (a->spec_b    != b->spec_b)    return a->spec_b < b->spec_b ? -1 : 1;
    if (a->spec_c    != b->spec_c)    return a->spec_c < b->spec_c ? -1 : 1;
    if (a->scope_order != b->scope_order)
        return a->scope_order < b->scope_order ? -1 : 1;
    if (a->sheet_index  != b->sheet_index)
        return a->sheet_index < b->sheet_index ? -1 : 1;
    if (a->source_order != b->source_order)
        return a->source_order < b->source_order ? -1 : 1;
    return a->decl_order < b->decl_order ? -1 : 1;
}

static gboolean
class_token_hides(const char *tok, gsize len)
{
    return len > 7 && tok[len - 7] == ':' &&
           memcmp(tok + len - 6, "hidden", 6) == 0;
}

static gboolean
element_has_hidden_utility(const nd_node *el)
{
    const char *cls = nd_element_get_attr(el, "class");
    if (!cls || !*cls) return FALSE;
    const char *s = cls;
    while (*s) {
        while (*s && is_ws(*s)) s++;
        const char *tok = s;
        while (*s && !is_ws(*s)) s++;
        if (s > tok && class_token_hides(tok, (gsize)(s - tok)))
            return TRUE;
    }
    return FALSE;
}

static void
add_hidden_utility_match(const nd_node *el, GArray *matches,
                         GPtrArray *owned_values)
{
    if (!element_has_hidden_utility(el)) return;
    nd_css_value *v = g_new0(nd_css_value, 1);
    v->kind = ND_CSS_V_KEYWORD;
    v->u.keyword = g_strdup("none");
    g_ptr_array_add(owned_values, v);
    match_entry e = {
        .origin = 1,
        .spec_a = 0, .spec_b = 1, .spec_c = 0,
        .sheet_index = 0,
        .layer_order = ND_CSS_LAYER_NONE,
        .source_order = INT_MIN,
        .decl_order = 0,
        .important = FALSE,
        .value = v,
        .prop = ND_CSS_DISPLAY,
    };
    g_array_append_val(matches, e);
}

static __thread guint *g_cand_pool = NULL;
static __thread guint g_cand_pool_cap = 0;
static __thread guint *g_cand_seen = NULL;
static __thread guint g_cand_seen_cap = 0;
static __thread guint g_cand_seen_epoch = 0;

static GArray *
css_index_lookup_ci(GHashTable *table, const char *name, gsize nlen)
{
    for (gsize i = 0; i < nlen; i++)
        if (name[i] >= 'A' && name[i] <= 'Z') {
            char small[64];
            char *key;
            if (nlen < sizeof(small)) {
                for (gsize j = 0; j < nlen; j++) small[j] = g_ascii_tolower(name[j]);
                small[nlen] = '\0'; key = small;
            } else {
                key = g_ascii_strdown(name, (gssize)nlen);
            }
            GArray *bucket = g_hash_table_lookup(table, key);
            if (key != small) g_free(key);
            return bucket;
        }
    return g_hash_table_lookup(table, name);
}

static void
gather_matches_impl(const nd_css_stylesheet *sheet, int origin, int sheet_index,
                    const nd_node *el, nd_css_pseudo_element pe,
                    GArray *out, GArray *var_out, GArray *pending_out,
                    GHashTable *layer_ranks)
{
    if (!sheet) return;
    const nd_css_rule_index *idx = nd_css_rule_index_ensure(sheet);
    if (!idx) return;

    guint *cands = g_cand_pool;
    guint cand_cap = g_cand_pool_cap;
    guint cand_n = 0;
    #define CAND_PUSH_ARR(_arr) do { \
        if ((_arr)) { \
            guint _n = (_arr)->len; \
            if (cand_n > G_MAXUINT - _n) break; \
            if (cand_n + _n > cand_cap) { \
                guint new_cap = cand_cap < 64 ? 64 : cand_cap; \
                while (cand_n + _n > new_cap) { \
                    if (new_cap > G_MAXUINT / 2) { new_cap = G_MAXUINT; break; } \
                    new_cap *= 2; \
                } \
                if (new_cap > G_MAXUINT / sizeof(guint)) break; \
                cands = g_renew(guint, cands, new_cap); \
                cand_cap = new_cap; \
                g_cand_pool = cands; \
                g_cand_pool_cap = cand_cap; \
            } \
            if (_n) memcpy(cands + cand_n, (_arr)->data, _n * sizeof(guint)); \
            cand_n += _n; \
        } \
    } while (0)

    if (el && el->kind == ND_NODE_ELEMENT) {
        const char *id = nd_element_get_attr(el, "id");
        if (id && *id) {
            GArray *bucket = g_hash_table_lookup(idx->by_id, id);
            CAND_PUSH_ARR(bucket);
        }
        const char *cls = nd_element_get_attr(el, "class");
        if (cls && *cls) {
            const char *s = cls;
            while (*s) {
                while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f')) s++;
                const char *tok = s;
                while (*s && !(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f')) s++;
                if (s == tok) break;
                gsize tlen = (gsize)(s - tok);
                char small[64];
                char *key;
                if (tlen < sizeof(small)) {
                    memcpy(small, tok, tlen); small[tlen] = '\0'; key = small;
                } else {
                    key = g_strndup(tok, tlen);
                }
                GArray *bucket = g_hash_table_lookup(idx->by_class, key);
                if (key != small) g_free(key);
                CAND_PUSH_ARR(bucket);
            }
        }
        if (el->name && *el->name) {
            CAND_PUSH_ARR(css_index_lookup_ci(idx->by_tag, el->name,
                                              strlen(el->name)));
        }
        if (idx->by_attr && g_hash_table_size(idx->by_attr) > 0) {
            for (const nd_attr *a = el->attrs; a; a = a->next) {
                if (!a->name) continue;
                CAND_PUSH_ARR(css_index_lookup_ci(idx->by_attr, a->name,
                                                  strlen(a->name)));
            }
        }
    }
    CAND_PUSH_ARR(idx->universal);
    #undef CAND_PUSH_ARR

    guint n_rules = sheet->rules ? sheet->rules->len : 0;
    if (g_cand_seen_cap < n_rules) {
        guint new_cap = g_cand_seen_cap < 64 ? 64 : g_cand_seen_cap;
        while (new_cap < n_rules) {
            if (new_cap > G_MAXUINT / 2) { new_cap = n_rules; break; }
            new_cap *= 2;
        }
        g_cand_seen = g_renew(guint, g_cand_seen, new_cap);
        memset(g_cand_seen + g_cand_seen_cap, 0,
               (gsize)(new_cap - g_cand_seen_cap) * sizeof(guint));
        g_cand_seen_cap = new_cap;
    }
    if (++g_cand_seen_epoch == 0) {
        memset(g_cand_seen, 0, (gsize)g_cand_seen_cap * sizeof(guint));
        g_cand_seen_epoch = 1;
    }

    for (guint ci = 0; ci < cand_n; ci++) {
        guint ri = cands[ci];
        if (g_cand_seen[ri] == g_cand_seen_epoch) continue;
        g_cand_seen[ri] = g_cand_seen_epoch;
        nd_css_rule *r = g_ptr_array_index(sheet->rules, ri);
        if (r->container_condition &&
            !container_cond_matches(r->container_condition))
            continue;
        gboolean any = FALSE;
        int best_a = 0, best_b = 0, best_c = 0;
        int best_scope_order = 0;
        for (guint si = 0; si < r->selectors->len; si++) {
            nd_css_selector *sel = g_ptr_array_index(r->selectors, si);
            int scope_order = 0;
            gboolean matched = rule_selector_matches(r, sel, el, pe,
                                                     &scope_order);
            if (!matched) continue;
            if (!any || sel->spec_a > best_a ||
                (sel->spec_a == best_a && sel->spec_b > best_b) ||
                (sel->spec_a == best_a && sel->spec_b == best_b && sel->spec_c > best_c)) {
                best_a = sel->spec_a; best_b = sel->spec_b; best_c = sel->spec_c;
                best_scope_order = scope_order;
            } else if (sel->spec_a == best_a && sel->spec_b == best_b &&
                       sel->spec_c == best_c &&
                       scope_order > best_scope_order) {
                best_scope_order = scope_order;
            }
            any = TRUE;
        }
        if (!any) continue;
        int layer_order = css_layer_rank_for(layer_ranks, r->layer_name);
        for (guint di = 0; di < r->decls->len; di++) {
            nd_css_decl *d = &g_array_index(r->decls, nd_css_decl, di);
            match_entry e = {
                .origin = origin,
                .spec_a = best_a, .spec_b = best_b, .spec_c = best_c,
                .sheet_index = sheet_index,
                .layer_order = layer_order,
                .scope_order = best_scope_order,
                .source_order = r->source_order,
                .decl_order = (int)di,
                .important = d->important,
                .value = d->value,
                .prop  = d->prop,
            };
            g_array_append_val(out, e);
        }
        if (var_out && r->vars) {
            GHashTableIter it;
            gpointer k, v;
            int decl_i = 0;
            g_hash_table_iter_init(&it, r->vars);
            while (g_hash_table_iter_next(&it, &k, &v)) {
                var_match vm = {
                    .origin = origin,
                    .spec_a = best_a, .spec_b = best_b, .spec_c = best_c,
                    .sheet_index = sheet_index,
                    .layer_order = layer_order,
                    .scope_order = best_scope_order,
                    .source_order = r->source_order,
                    .decl_order = decl_i++,
                    .important = r->var_important &&
                                 g_hash_table_contains(r->var_important, k),
                    .name = (const char *)k,
                    .text = (const char *)v,
                };
                g_array_append_val(var_out, vm);
            }
        }
        if (pending_out && r->pending) {
            for (guint pi = 0; pi < r->pending->len; pi++) {
                nd_css_pending_decl *pd =
                    &g_array_index(r->pending, nd_css_pending_decl, pi);
                pending_match pm = {
                    .origin = origin,
                    .spec_a = best_a, .spec_b = best_b, .spec_c = best_c,
                    .sheet_index = sheet_index,
                    .layer_order = layer_order,
                    .scope_order = best_scope_order,
                    .source_order = r->source_order,
                    .decl_order_base = (int)(r->decls->len + pi),
                    .pd = pd,
                };
                g_array_append_val(pending_out, pm);
            }
        }
    }
    (void)cands;
}

static void
gather_matches(const nd_css_stylesheet *sheet, int origin, int sheet_index,
               const nd_node *el, GArray *out,
               GArray *var_out, GArray *pending_out,
               GHashTable *layer_ranks)
{
    gather_matches_impl(sheet, origin, sheet_index, el, ND_CSS_PE_NONE,
                        out, var_out, pending_out, layer_ranks);
}

static int
var_match_cmp(gconstpointer a_, gconstpointer b_)
{
    const var_match *a = a_;
    const var_match *b = b_;
    if (a->important != b->important) return a->important ? 1 : -1;
    if (a->origin    != b->origin)    return a->origin < b->origin ? -1 : 1;
    int layer_cmp = css_layer_cmp(a->layer_order, b->layer_order, a->important);
    if (layer_cmp != 0) return layer_cmp;
    if (a->spec_a    != b->spec_a)    return a->spec_a < b->spec_a ? -1 : 1;
    if (a->spec_b    != b->spec_b)    return a->spec_b < b->spec_b ? -1 : 1;
    if (a->spec_c    != b->spec_c)    return a->spec_c < b->spec_c ? -1 : 1;
    if (a->scope_order != b->scope_order)
        return a->scope_order < b->scope_order ? -1 : 1;
    if (a->sheet_index  != b->sheet_index)
        return a->sheet_index < b->sheet_index ? -1 : 1;
    if (a->source_order != b->source_order)
        return a->source_order < b->source_order ? -1 : 1;
    return a->decl_order < b->decl_order ? -1 : 1;
}

static int
pending_match_cmp(gconstpointer a_, gconstpointer b_)
{
    const pending_match *a = a_;
    const pending_match *b = b_;
    if (a->origin    != b->origin)    return a->origin < b->origin ? -1 : 1;
    gboolean ai = a->pd && a->pd->important;
    gboolean bi = b->pd && b->pd->important;
    if (ai != bi) return ai ? 1 : -1;
    int layer_cmp = css_layer_cmp(a->layer_order, b->layer_order, ai);
    if (layer_cmp != 0) return layer_cmp;
    if (a->spec_a    != b->spec_a)    return a->spec_a < b->spec_a ? -1 : 1;
    if (a->spec_b    != b->spec_b)    return a->spec_b < b->spec_b ? -1 : 1;
    if (a->spec_c    != b->spec_c)    return a->spec_c < b->spec_c ? -1 : 1;
    if (a->scope_order != b->scope_order)
        return a->scope_order < b->scope_order ? -1 : 1;
    if (a->sheet_index  != b->sheet_index)
        return a->sheet_index < b->sheet_index ? -1 : 1;
    return a->source_order < b->source_order ? -1 : 1;
}

static void
css_collect_property_rules(GHashTable *reg, const nd_css_stylesheet *sh)
{
    if (!reg || !sh || !sh->property_rules) return;
    for (guint i = 0; i < sh->property_rules->len; i++) {
        nd_css_property_rule *pr =
            &g_array_index(sh->property_rules, nd_css_property_rule, i);
        if (pr->name) g_hash_table_replace(reg, pr->name, pr);
    }
}

static GHashTable *
build_vars_for_element(const nd_style *parent_style, GArray *var_matches)
{
    gboolean parent_has = parent_style && parent_style->vars &&
                          g_hash_table_size(parent_style->vars) > 0;
    gboolean have_regs  = g_registered_props &&
                          g_hash_table_size(g_registered_props) > 0;
    gboolean have_local = var_matches && var_matches->len > 0;
    if (!parent_has && !have_regs && !have_local)
        return NULL;
    GHashTable *vars = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
    if (parent_style && parent_style->vars) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, parent_style->vars);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            if (g_registered_props) {
                nd_css_property_rule *pr =
                    g_hash_table_lookup(g_registered_props, k);
                if (pr && !pr->inherits) continue;
            }
            g_hash_table_replace(vars, g_strdup(k), g_strdup(v));
        }
    }
    if (g_registered_props) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_registered_props);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            nd_css_property_rule *pr = v;
            if (pr->has_initial && !g_hash_table_contains(vars, k))
                g_hash_table_replace(vars, g_strdup(k),
                                     g_strdup(pr->initial_value));
        }
    }
    g_array_sort(var_matches, var_match_cmp);
    for (guint i = 0; i < var_matches->len; i++) {
        var_match *vm = &g_array_index(var_matches, var_match, i);
        if (!vm->name || !vm->text) continue;
        g_hash_table_replace(vars, g_strdup(vm->name), g_strdup(vm->text));
    }
    return vars;
}

static void
resolve_pending_into_matches(GArray *pending_matches,
                             GHashTable *vars,
                             GArray *matches,
                             GPtrArray *owned_values)
{
    if (!pending_matches || pending_matches->len == 0) return;
    g_array_sort(pending_matches, pending_match_cmp);
    for (guint pmi = 0; pmi < pending_matches->len; pmi++) {
        pending_match *pm = &g_array_index(pending_matches, pending_match, pmi);
        if (!pm->pd || !pm->pd->pname || !pm->pd->raw_vtext) continue;
        char *substituted = substitute_vars_with(pm->pd->raw_vtext, vars, 0);
        if (!substituted) continue;
        gboolean ignored_important = FALSE;
        css_strip_important(substituted, &ignored_important);
        char *synth = g_strdup_printf("%s: %s;}", pm->pd->pname, substituted);
        g_free(substituted);
        GArray *temp = g_array_new(FALSE, FALSE, sizeof(nd_css_decl));
        const char *sp = synth;
        const char *se = synth + strlen(synth);
        parse_declaration_block(&sp, se, temp, NULL);
        g_free(synth);
        for (guint i = 0; i < temp->len; i++) {
            nd_css_decl *d = &g_array_index(temp, nd_css_decl, i);
            if (!d->value) continue;
            g_ptr_array_add(owned_values, d->value);
            match_entry me = {
                .origin = pm->origin,
                .spec_a = pm->spec_a, .spec_b = pm->spec_b, .spec_c = pm->spec_c,
                .sheet_index = pm->sheet_index,
                .layer_order = pm->layer_order,
                .scope_order = pm->scope_order,
                .source_order = pm->source_order,
                .decl_order = pm->decl_order_base + (int)i,
                .important = pm->pd->important || d->important,
                .value = d->value,
                .prop  = d->prop,
            };
            g_array_append_val(matches, me);
        }
        g_array_free(temp, TRUE);
    }
}

static const char *kUa =
    "html, body { display: block; color: #1a1a1a; "
    "font-family: system-ui, sans-serif; font-size: 16px; line-height: normal; }\n"
    "body { margin: 8px; }\n"
    "div, p, section, article, header, footer, nav, main, aside, "
    "ul, ol, li, dl, dt, dd, blockquote, pre, address, "
    "hr, form, fieldset, figure, figcaption, center, "
    "legend, search, hgroup { display: block; }\n"
    "address { font-style: italic; }\n"
    "fieldset { margin: 0.5em 8px; padding: 0.35em 8px 0.6em; "
    "border-top-width: 1px; border-right-width: 1px; "
    "border-bottom-width: 1px; border-left-width: 1px; "
    "border-top-style: solid; border-right-style: solid; "
    "border-bottom-style: solid; border-left-style: solid; "
    "border-top-color: #a0a0a0; border-right-color: #a0a0a0; "
    "border-bottom-color: #a0a0a0; border-left-color: #a0a0a0; }\n"
    "legend { padding: 0 4px; font-weight: bold; }\n"
    "center { text-align: center; }\n"
    "h1, h2, h3, h4, h5, h6 { display: block; font-weight: bold; "
    "font-family: sans-serif; line-height: 1.2; }\n"
    "span, a, b, i, em, strong, code, small, big, u, s, del, ins, mark, "
    "tt, kbd, samp, var, cite, dfn, abbr, acronym, sub, sup, q, time, "
    "bdi, bdo, ruby, rb, rt, output, "
    "button, label { display: inline; }\n"
    "var { font-style: italic; }\n"
    "rt { font-size: 0.7em; }\n"
    "abbr[title], acronym[title] { text-decoration: underline dotted; cursor: help; }\n"
    "rp, datalist { display: none; }\n"
    "menu { display: block; padding-left: 32px; margin: 0.6em 0; }\n"
    "h1 { font-size: 2.0em;  margin: 0.67em 0; }\n"
    "h2 { font-size: 1.5em;  margin: 0.83em 0; }\n"
    "h3 { font-size: 1.17em; margin: 1.00em 0; }\n"
    "h4 { font-size: 1.0em;  margin: 1.33em 0; }\n"
    "h5 { font-size: 0.83em; margin: 1.67em 0; }\n"
    "h6 { font-size: 0.67em; margin: 2.33em 0; }\n"
    "p { margin: 1em 0; }\n"
    "address { color: #555; }\n"
    "blockquote { margin: 1em 24px; border-left-width: 4px; "
    "border-left-style: solid; border-left-color: #dddddd; padding-left: 12px; }\n"
    "hr { margin: 12px 0; height: 1px; background-color: #888888; }\n"
    "ul, ol { padding-left: 40px; margin: 1em 0; }\n"
    "li { margin: 2px 0; }\n"
    "dl { margin: 0.6em 0; } dt { font-weight: bold; } dd { margin-left: 24px; }\n"
    "dl > dt { margin-top: 0.3em; }\n"
    "a:link, a:visited { color: #0645ad; text-decoration: underline; }\n"
    "b, strong { font-weight: bold; }\n"
    "i, em, cite, dfn { font-style: italic; }\n"
    "ins { color: #006400; }\n"
    "del, s, strike { color: #8b0000; }\n"
    "big { font-size: 1.17em; }\n"
    "code, pre, kbd, samp, tt { font-family: monospace; }\n"
    "code, kbd, samp { white-space: pre-wrap; }\n"
    "pre { margin: 0.9em 0; padding: 6px; background-color: #f4f4f4; "
    "line-height: 1.4; white-space: pre; }\n"
    "textarea { white-space: pre-wrap; }\n"
    "code { background-color: #f4f4f4; padding: 1px 4px; font-size: 0.93em; }\n"
    "samp { background-color: #f4f4f4; padding: 1px 4px; }\n"
    "kbd { background-color: #eeeeee; padding: 1px 4px; font-size: 0.9em; "
    "border-top-width: 1px; border-right-width: 1px; "
    "border-bottom-width: 1px; border-left-width: 1px; "
    "border-top-style: solid; border-right-style: solid; "
    "border-bottom-style: solid; border-left-style: solid; "
    "border-top-color: #aaaaaa; border-right-color: #aaaaaa; "
    "border-bottom-color: #aaaaaa; border-left-color: #aaaaaa; }\n"
    "mark { background-color: #ffff00; color: #000000; }\n"
    "small { font-size: 0.85em; }\n"
    "sub, sup { font-size: 0.75em; }\n"
    "table { display: table; border-collapse: separate; border-spacing: 2px; }\n"
    "caption { display: table-caption; font-weight: bold; padding-bottom: 4px; "
    "text-align: center; }\n"
    "thead { display: table-header-group; }\n"
    "tbody { display: table-row-group; }\n"
    "tfoot { display: table-footer-group; }\n"
    "colgroup { display: table-column-group; }\n"
    "col { display: table-column; }\n"
    "tr { display: table-row; }\n"
    "td, th { display: table-cell; padding: 1px; text-align: left; }\n"
    "th { font-weight: bold; text-align: center; background-color: #f0f0f0; }\n"
    "table[border] td, table[border] th { "
    "border-top-width: 1px; border-right-width: 1px; "
    "border-bottom-width: 1px; border-left-width: 1px; "
    "border-top-style: solid; border-right-style: solid; "
    "border-bottom-style: solid; border-left-style: solid; "
    "border-top-color: #888888; border-right-color: #888888; "
    "border-bottom-color: #888888; border-left-color: #888888; }\n"
    "table[border=\"0\"], table[border=\"0\"] td, table[border=\"0\"] th { "
    "border-top-width: 0; border-right-width: 0; "
    "border-bottom-width: 0; border-left-width: 0; }\n"
    "img { display: inline; max-width: 100%; }\n"
    "figure { margin: 0.6em 24px; }\n"
    "figcaption { font-style: italic; font-size: 0.9em; text-align: center; }\n"
    "button { display: inline-block; padding: 4px 12px; background-color: #e6e6e6; "
    "border-top-width: 1px; border-right-width: 1px; "
    "border-bottom-width: 1px; border-left-width: 1px; "
    "border-top-style: solid; border-right-style: solid; "
    "border-bottom-style: solid; border-left-style: solid; "
    "border-top-color: #b8b8b8; border-right-color: #b8b8b8; "
    "border-bottom-color: #b8b8b8; border-left-color: #b8b8b8; }\n"
    "button, input, select, textarea { color: #1a1a1a; }\n"
    "input, textarea, select { padding: 0; background-color: transparent; "
    "border-top-width: 0; border-right-width: 0; "
    "border-bottom-width: 0; border-left-width: 0; }\n"
    "head, script, style, title, meta, link, noscript { display: none; }\n"
    "[data-nd-shadow-root] { display: block; }\n"
    "input[type=\"hidden\"] { display: none; }\n"
    "video { display: block; }\n"
    "canvas { display: block; }\n"
    "iframe, frame, frameset, object, embed { display: none !important; }\n"
    "iframe[data-nd-frame-loaded] { display: block !important; overflow: hidden; }\n"
    "audio, source, track, param { display: none; }\n"
    "audio[controls] { display: inline-block; }\n"
    "svg { display: inline-block; }\n"
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
    "[popover]:not([data-nd-popover-open]) { display: none; }\n"
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
    case ND_CSS_UNIT_REM:     return fs->u.length.v * parent_px;
    case ND_CSS_UNIT_PERCENT: return fs->u.length.v * parent_px / 100.0;
    case ND_CSS_UNIT_VW:
    case ND_CSS_UNIT_VH:
    case ND_CSS_UNIT_VMIN:
    case ND_CSS_UNIT_VMAX:
        return viewport_resolve(fs->u.length.v, fs->u.length.unit);
    case ND_CSS_UNIT_CQW:   return fs->u.length.v * g_viewport_w / 100.0;
    case ND_CSS_UNIT_CQH:   return fs->u.length.v * g_viewport_h / 100.0;
    case ND_CSS_UNIT_CQMIN:
    case ND_CSS_UNIT_CQMAX:
        return viewport_resolve(fs->u.length.v,
            fs->u.length.unit == ND_CSS_UNIT_CQMIN ? ND_CSS_UNIT_VMIN
                                                   : ND_CSS_UNIT_VMAX);
    }
    return parent_px;
}

static void
resolve_em_units(nd_style *out, const nd_style *parent_style, double root_px)
{
    double my_font_px = resolve_font_size_px(out, parent_style);
    if (root_px <= 0) root_px = my_font_px;
    if (out->values[ND_CSS_FONT_SIZE] &&
        out->values[ND_CSS_FONT_SIZE]->u.length.unit == ND_CSS_UNIT_REM) {
        my_font_px = out->values[ND_CSS_FONT_SIZE]->u.length.v * root_px;
    }
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
        if (!v) continue;
        if (v->kind == ND_CSS_V_CALC) {
            v->u.calc.px += v->u.calc.em * my_font_px +
                            v->u.calc.rem * root_px;
            v->u.calc.em = 0;
            v->u.calc.rem = 0;
            continue;
        }
        if (v->kind != ND_CSS_V_LENGTH) continue;
        switch (v->u.length.unit) {
        case ND_CSS_UNIT_EM:
            v->u.length.v *= my_font_px;
            v->u.length.unit = ND_CSS_UNIT_PX;
            break;
        case ND_CSS_UNIT_REM:
            v->u.length.v *= root_px;
            v->u.length.unit = ND_CSS_UNIT_PX;
            break;
        case ND_CSS_UNIT_VW:
        case ND_CSS_UNIT_VH:
        case ND_CSS_UNIT_VMIN:
        case ND_CSS_UNIT_VMAX:
            v->u.length.v = viewport_resolve(v->u.length.v, v->u.length.unit);
            v->u.length.unit = ND_CSS_UNIT_PX;
            break;
        default:
            break;
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
            strcmp(v->u.keyword, "revert")  == 0 ||
            strcmp(v->u.keyword, "revert-layer") == 0);
}

static void
cascade_for(GArray *matches, nd_style *out, const nd_style *parent_style,
            double root_px)
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
            ND_CSS_OUTLINE_COLOR,
            ND_CSS_TEXT_DECORATION_COLOR,
            ND_CSS_COLUMN_RULE_COLOR,
            ND_CSS_ACCENT_COLOR,
        };
        for (gsize i = 0; i < G_N_ELEMENTS(color_props); i++) {
            nd_css_value *v = out->values[color_props[i]];
            if (!v || v->kind != ND_CSS_V_KEYWORD || !v->u.keyword) continue;
            if (strcmp(v->u.keyword, "currentcolor") == 0) {
                nd_css_value_free(out->values[color_props[i]]);
                out->values[color_props[i]] = out->values[ND_CSS_COLOR]
                    ? nd_css_value_dup(out->values[ND_CSS_COLOR])
                    : NULL;
            } else if (strcmp(v->u.keyword, "transparent") == 0) {
                nd_css_value_free(out->values[color_props[i]]);
                nd_css_value *t = g_new0(nd_css_value, 1);
                t->kind = ND_CSS_V_COLOR;
                t->u.color.r = t->u.color.g = t->u.color.b = 0;
                t->u.color.a = 0;
                out->values[color_props[i]] = t;
            }
        }
    }
    resolve_em_units(out, parent_style, root_px);
}

static gboolean
parse_legacy_color(const char *input, guint8 *r_out, guint8 *g_out, guint8 *b_out)
{
    if (!input || !*input) return FALSE;

    GString *s = g_string_new(NULL);
    for (const char *p = input; *p; ) {
        gunichar c = g_utf8_get_char(p);
        const char *next = g_utf8_next_char(p);
        if (c > 0xFFFF) g_string_append(s, "00");
        else            g_string_append_len(s, p, next - p);
        p = next;
    }

    glong m = g_utf8_strlen(s->str, -1);
    if (m > 128) m = 128;

    GString *hex = g_string_new(NULL);
    const char *p = s->str;
    for (glong i = 0; i < m; i++, p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (i == 0 && c == '#') continue;
        if (c < 128 && g_ascii_isxdigit((char)c))
            g_string_append_c(hex, (char)c);
        else
            g_string_append_c(hex, '0');
    }
    g_string_free(s, TRUE);

    if (hex->len == 0) g_string_append_c(hex, '0');
    while (hex->len % 3 != 0) g_string_append_c(hex, '0');

    gsize comp = hex->len / 3;
    const char *c0 = hex->str, *c1 = hex->str + comp, *c2 = hex->str + 2 * comp;
    gsize off = 0, len = comp;
    if (len > 8) { off = len - 8; len = 8; }
    while (len > 2 && c0[off] == '0' && c1[off] == '0' && c2[off] == '0') {
        off++; len--;
    }
    if (len > 2) len = 2;

    guint rv = 0, gv = 0, bv = 0;
    for (gsize i = 0; i < len; i++) {
        rv = rv * 16 + (guint)g_ascii_xdigit_value(c0[off + i]);
        gv = gv * 16 + (guint)g_ascii_xdigit_value(c1[off + i]);
        bv = bv * 16 + (guint)g_ascii_xdigit_value(c2[off + i]);
    }
    g_string_free(hex, TRUE);

    *r_out = (guint8)rv; *g_out = (guint8)gv; *b_out = (guint8)bv;
    return TRUE;
}

static gboolean
attr_is_color(const char *v, guint8 *r_out, guint8 *g_out, guint8 *b_out, guint8 *a_out)
{
    if (!v) return FALSE;
    while (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\f' || *v == '\r') v++;
    const char *end = v + strlen(v);
    while (end > v && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                       end[-1] == '\f' || end[-1] == '\r'))
        end--;
    if (end == v) return FALSE;
    char *stripped = g_strndup(v, (gsize)(end - v));
    gboolean ok = parse_color(stripped, r_out, g_out, b_out, a_out);
    if (!ok) {
        *a_out = 255;
        ok = parse_legacy_color(stripped, r_out, g_out, b_out);
    }
    g_free(stripped);
    return ok;
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

    if (strcmp(tag, "ol") == 0 || strcmp(tag, "li") == 0) {
        const char *t = nd_element_get_attr(el, "type");
        const char *lst = NULL;
        if (t) {
            if (strcmp(t, "1") == 0) lst = "decimal";
            else if (strcmp(t, "a") == 0) lst = "lower-alpha";
            else if (strcmp(t, "A") == 0) lst = "upper-alpha";
            else if (strcmp(t, "i") == 0) lst = "lower-roman";
            else if (strcmp(t, "I") == 0) lst = "upper-roman";
        }
        if (lst) g_string_append_printf(out, "list-style-type: %s;", lst);
    }
    if (strcmp(tag, "ul") == 0 || strcmp(tag, "li") == 0) {
        const char *t = nd_element_get_attr(el, "type");
        const char *lst = NULL;
        if (t) {
            if (g_ascii_strcasecmp(t, "disc") == 0) lst = "disc";
            else if (g_ascii_strcasecmp(t, "circle") == 0) lst = "circle";
            else if (g_ascii_strcasecmp(t, "square") == 0) lst = "square";
        }
        if (lst) g_string_append_printf(out, "list-style-type: %s;", lst);
    }

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
        if (face && *face) {
            static const char *const generics[] = {
                "serif", "sans-serif", "monospace", "cursive", "fantasy",
                "system-ui", "ui-serif", "ui-sans-serif", "ui-monospace",
                "ui-rounded", "math", "emoji", "fangsong",
            };
            gboolean is_generic = FALSE;
            for (gsize i = 0; i < G_N_ELEMENTS(generics); i++)
                if (g_ascii_strcasecmp(face, generics[i]) == 0) {
                    is_generic = TRUE;
                    break;
                }
            if (is_generic) {
                g_string_append_printf(out, "font-family: %s;", face);
            } else {
                g_string_append(out, "font-family: \"");
                for (const unsigned char *p = (const unsigned char *)face; *p; p++) {
                    unsigned char c = *p;
                    if (c == '\\' || c == '"')
                        g_string_append_printf(out, "\\%c", c);
                    else if (c < 0x20 || c == 0x7f)
                        g_string_append_printf(out, "\\%X ", c);
                    else
                        g_string_append_c(out, (char)c);
                }
                g_string_append(out, "\";");
            }
        }
        const char *size = nd_element_get_attr(el, "size");
        if (size && *size) {
            int n = nd_parse_int(size, 0, 0, 100);
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
                            strcmp(tag, "col") == 0 ||
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
            int w = nd_parse_int(border, 0, 0, 100);
            if (w > 0) {
                g_string_append_printf(out,
                    "border: %dpx solid #888;", w);
            }
        }
        const char *cellspacing = nd_element_get_attr(el, "cellspacing");
        if (cellspacing) {
            int v = nd_parse_int(cellspacing, 0, 0, 1000);
            g_string_append_printf(out, "border-spacing: %dpx;", v);
        }
    }
    if (is_cell) {
        const nd_node *tbl = el->parent;
        while (tbl && !(tbl->kind == ND_NODE_ELEMENT && tbl->name &&
                        g_ascii_strcasecmp(tbl->name, "table") == 0))
            tbl = tbl->parent;
        if (tbl) {
            const char *cellpadding = nd_element_get_attr(tbl, "cellpadding");
            if (cellpadding && *cellpadding) {
                int v = nd_parse_int(cellpadding, 0, 0, 1000);
                g_string_append_printf(out, "padding: %dpx;", v);
            }
            const char *tborder = nd_element_get_attr(tbl, "border");
            if (tborder && nd_parse_int(tborder, 0, 0, 100) > 0)
                g_string_append(out, "border: 1px solid #a0a0a0;");
        }
        if (nd_element_get_attr(el, "nowrap"))
            g_string_append(out, "white-space: nowrap;");
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
            else if (is_table && strcmp(lo, "center") == 0)
                g_string_append(out, "margin-left: auto; margin-right: auto;");
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
            int v = nd_parse_int(hspace, 0, 0, 1000);
            g_string_append_printf(out, "margin-left: %dpx; margin-right: %dpx;", v, v);
        }
        const char *vspace = nd_element_get_attr(el, "vspace");
        if (vspace && *vspace) {
            int v = nd_parse_int(vspace, 0, 0, 1000);
            g_string_append_printf(out, "margin-top: %dpx; margin-bottom: %dpx;", v, v);
        }
        const char *iborder = nd_element_get_attr(el, "border");
        if (iborder && *iborder) {
            int v = nd_parse_int(iborder, 0, 0, 100);
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
            int v = nd_parse_int(size, 0, 0, 1000);
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

#define ND_CSS_MAX_CASCADE_DEPTH 512

static GHashTable *g_decl_sheet_cache;

static const nd_css_stylesheet *
nd_css_cached_decl_sheet(const char *decls)
{
    if (!decls || !*decls) return NULL;
    if (!g_decl_sheet_cache)
        g_decl_sheet_cache = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free,
            (GDestroyNotify)nd_css_stylesheet_free);
    nd_css_stylesheet *s = g_hash_table_lookup(g_decl_sheet_cache, decls);
    if (s) return s;
    if (g_hash_table_size(g_decl_sheet_cache) >= 8192)
        g_hash_table_remove_all(g_decl_sheet_cache);
    char *wrapped = g_strconcat("* { ", decls, " }", NULL);
    s = nd_css_stylesheet_parse(wrapped, -1);
    g_free(wrapped);
    if (s) g_hash_table_insert(g_decl_sheet_cache, g_strdup(decls), s);
    return s;
}

static void
cascade_walk(nd_node *node,
             const nd_css_stylesheet *ua,
             const nd_css_stylesheet *const *author, gsize n_author,
             const nd_style *parent_style,
             double *root_px,
             GHashTable *layer_ranks,
             GHashTable *out)
{
    static int depth;
    if (depth >= ND_CSS_MAX_CASCADE_DEPTH) return;
    depth++;
    const nd_style *child_parent_style = parent_style;
    if (node->kind == ND_NODE_ELEMENT) {
        nd_style *s = nd_style_alloc();
        static GArray *sc_matches, *sc_var, *sc_pending;
        static GPtrArray *sc_owned;
        if (!sc_matches) {
            sc_matches  = g_array_new(FALSE, FALSE, sizeof(match_entry));
            sc_var      = g_array_new(FALSE, FALSE, sizeof(var_match));
            sc_pending  = g_array_new(FALSE, FALSE, sizeof(pending_match));
            sc_owned    = g_ptr_array_new_with_free_func(
                              (GDestroyNotify)nd_css_value_free);
        }
        GArray *matches = sc_matches;
        GArray *var_matches = sc_var;
        GArray *pending_matches = sc_pending;
        GPtrArray *owned_values = sc_owned;
        g_array_set_size(matches, 0);
        g_array_set_size(var_matches, 0);
        g_array_set_size(pending_matches, 0);
        g_ptr_array_set_size(owned_values, 0);
        gather_matches(ua, 0, 0, node, matches, var_matches, pending_matches,
                       layer_ranks);
        for (gsize i = 0; i < n_author; i++)
            gather_matches(author[i], 1, (int)(i + 1), node, matches,
                           var_matches, pending_matches, layer_ranks);
        add_hidden_utility_match(node, matches, owned_values);

        char *pres_css = presentational_hints_css(node);
        const nd_css_stylesheet *pres_sheet = NULL;
        if (pres_css) {
            pres_sheet = nd_css_cached_decl_sheet(pres_css);
            g_free(pres_css);
        }
        if (pres_sheet) {
            for (guint ri = 0; ri < pres_sheet->rules->len; ri++) {
                nd_css_rule *r = g_ptr_array_index(pres_sheet->rules, ri);
                for (guint di = 0; di < r->decls->len; di++) {
                    nd_css_decl *d = &g_array_index(r->decls, nd_css_decl, di);
                    match_entry e = {
                        .origin = 1,
                        .spec_a = 0, .spec_b = 0, .spec_c = 0,
                        .layer_order = ND_CSS_LAYER_NONE,
                        .source_order = INT_MIN,
                        .decl_order = (int)di,
                        .important = d->important,
                        .value = d->value,
                        .prop  = d->prop,
                    };
                    g_array_append_val(matches, e);
                }
                if (r->vars) {
                    GHashTableIter it; gpointer k, v; int di_v = 0;
                    g_hash_table_iter_init(&it, r->vars);
                    while (g_hash_table_iter_next(&it, &k, &v)) {
                        var_match vm = {
                            .origin = 1, .spec_a = 0, .spec_b = 0, .spec_c = 0,
                            .sheet_index = 0,
                            .layer_order = ND_CSS_LAYER_NONE,
                            .source_order = INT_MIN,
                            .decl_order = di_v++,
                            .important = r->var_important &&
                                g_hash_table_contains(r->var_important, k),
                            .name = (const char *)k,
                            .text = (const char *)v,
                        };
                        g_array_append_val(var_matches, vm);
                    }
                }
                if (r->pending) {
                    for (guint pi = 0; pi < r->pending->len; pi++) {
                        nd_css_pending_decl *pd =
                            &g_array_index(r->pending, nd_css_pending_decl, pi);
                        pending_match pm = {
                            .origin = 1, .spec_a = 0, .spec_b = 0, .spec_c = 0,
                            .sheet_index = 0,
                            .layer_order = ND_CSS_LAYER_NONE,
                            .source_order = INT_MIN,
                            .decl_order_base = (int)(r->decls->len + pi),
                            .pd = pd,
                        };
                        g_array_append_val(pending_matches, pm);
                    }
                }
            }
        }

        const char *inline_css = nd_element_get_attr(node, "style");
        const nd_css_stylesheet *inline_sheet = NULL;
        if (inline_css && *inline_css)
            inline_sheet = nd_css_cached_decl_sheet(inline_css);
        if (inline_sheet) {
            for (guint ri = 0; ri < inline_sheet->rules->len; ri++) {
                nd_css_rule *r = g_ptr_array_index(inline_sheet->rules, ri);
                for (guint di = 0; di < r->decls->len; di++) {
                    nd_css_decl *d = &g_array_index(r->decls, nd_css_decl, di);
                    match_entry e = {
                        .origin = 1,
                        .spec_a = 1000, .spec_b = 0, .spec_c = 0,
                        .layer_order = ND_CSS_LAYER_NONE,
                        .source_order = INT_MAX,
                        .decl_order = (int)di,
                        .important = d->important,
                        .value = d->value,
                        .prop  = d->prop,
                    };
                    g_array_append_val(matches, e);
                }
                if (r->vars) {
                    GHashTableIter it; gpointer k, v; int di_v = 0;
                    g_hash_table_iter_init(&it, r->vars);
                    while (g_hash_table_iter_next(&it, &k, &v)) {
                        var_match vm = {
                            .origin = 1, .spec_a = 1000, .spec_b = 0, .spec_c = 0,
                            .sheet_index = 0,
                            .layer_order = ND_CSS_LAYER_NONE,
                            .source_order = INT_MAX,
                            .decl_order = di_v++,
                            .important = r->var_important &&
                                g_hash_table_contains(r->var_important, k),
                            .name = (const char *)k,
                            .text = (const char *)v,
                        };
                        g_array_append_val(var_matches, vm);
                    }
                }
                if (r->pending) {
                    for (guint pi = 0; pi < r->pending->len; pi++) {
                        nd_css_pending_decl *pd =
                            &g_array_index(r->pending, nd_css_pending_decl, pi);
                        pending_match pm = {
                            .origin = 1, .spec_a = 1000, .spec_b = 0, .spec_c = 0,
                            .sheet_index = 0,
                            .layer_order = ND_CSS_LAYER_NONE,
                            .source_order = INT_MAX,
                            .decl_order_base = (int)(r->decls->len + pi),
                            .pd = pd,
                        };
                        g_array_append_val(pending_matches, pm);
                    }
                }
            }
        }

        s->vars = build_vars_for_element(parent_style, var_matches);
        resolve_pending_into_matches(pending_matches, s->vars,
                                     matches, owned_values);

        cascade_for(matches, s, parent_style, *root_px);
        g_array_set_size(matches, 0);
        g_array_set_size(var_matches, 0);
        g_array_set_size(pending_matches, 0);
        g_ptr_array_set_size(owned_values, 0);

        guint pe_mask = ua ? ua->pseudo_mask : 0;
        for (gsize i = 0; i < n_author; i++)
            if (author[i]) pe_mask |= author[i]->pseudo_mask;
        for (int pi = 0; pe_mask && pi < 8; pi++) {
            nd_css_pseudo_element pe = (pi == 0) ? ND_CSS_PE_BEFORE :
                                       (pi == 1) ? ND_CSS_PE_AFTER :
                                       (pi == 2) ? ND_CSS_PE_FIRST_LETTER :
                                       (pi == 3) ? ND_CSS_PE_FIRST_LINE :
                                       (pi == 4) ? ND_CSS_PE_SELECTION :
                                       (pi == 5) ? ND_CSS_PE_MARKER :
                                       (pi == 6) ? ND_CSS_PE_BACKDROP :
                                                   ND_CSS_PE_PLACEHOLDER;
            if (!(pe_mask & (1u << pe))) continue;
            GArray *pm = g_array_new(FALSE, FALSE, sizeof(match_entry));
            GArray *pe_vars    = g_array_new(FALSE, FALSE, sizeof(var_match));
            GArray *pe_pending = g_array_new(FALSE, FALSE, sizeof(pending_match));
            GPtrArray *pe_owned =
                g_ptr_array_new_with_free_func((GDestroyNotify)nd_css_value_free);
            gather_matches_impl(ua, 0, 0, node, pe, pm, pe_vars, pe_pending,
                                layer_ranks);
            for (gsize i = 0; i < n_author; i++)
                gather_matches_impl(author[i], 1, (int)(i + 1), node, pe, pm,
                                    pe_vars, pe_pending, layer_ranks);
            if (pm->len > 0 || pe_pending->len > 0) {
                nd_style *ps = nd_style_alloc();
                ps->vars = build_vars_for_element(s, pe_vars);
                resolve_pending_into_matches(pe_pending, ps->vars, pm, pe_owned);
                cascade_for(pm, ps, s, *root_px);
                gboolean keep = TRUE;
                if (pe == ND_CSS_PE_BEFORE || pe == ND_CSS_PE_AFTER)
                    keep = ps->values[ND_CSS_CONTENT] != NULL;
                if (keep) {
                    if (pe == ND_CSS_PE_BEFORE)            s->before       = ps;
                    else if (pe == ND_CSS_PE_AFTER)        s->after        = ps;
                    else if (pe == ND_CSS_PE_FIRST_LETTER) s->first_letter = ps;
                    else if (pe == ND_CSS_PE_FIRST_LINE)   s->first_line   = ps;
                    else if (pe == ND_CSS_PE_SELECTION)    s->selection    = ps;
                    else if (pe == ND_CSS_PE_MARKER)       s->marker       = ps;
                    else if (pe == ND_CSS_PE_BACKDROP)     s->backdrop     = ps;
                    else                                    s->placeholder  = ps;
                } else {
                    nd_style_free(ps);
                }
            }
            g_array_free(pm, TRUE);
            g_array_free(pe_vars, TRUE);
            g_array_free(pe_pending, TRUE);
            g_ptr_array_free(pe_owned, TRUE);
        }

        g_hash_table_insert(out, node, s);
        child_parent_style = s;
        if (*root_px <= 0 &&
            s->values[ND_CSS_FONT_SIZE] &&
            s->values[ND_CSS_FONT_SIZE]->kind == ND_CSS_V_LENGTH &&
            s->values[ND_CSS_FONT_SIZE]->u.length.unit == ND_CSS_UNIT_PX)
            *root_px = s->values[ND_CSS_FONT_SIZE]->u.length.v;
    }
    gboolean pushed = FALSE;
    if (g_cq_map && g_cq_stack) {
        nd_cq_container *info = g_hash_table_lookup(g_cq_map, node);
        if (info) {
            g_array_append_val(g_cq_stack, *info);
            pushed = TRUE;
        }
    }
    for (nd_node *c = node->first_child; c; c = c->next_sibling)
        cascade_walk(c, ua, author, n_author, child_parent_style, root_px,
                     layer_ranks, out);
    if (pushed) g_array_set_size(g_cq_stack, g_cq_stack->len - 1);
    depth--;
}

static void
append_text_children(const nd_node *n, GString *out, int depth)
{
    if (depth >= 512) return;
    for (const nd_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == ND_NODE_TEXT && c->text)
            g_string_append(out, c->text);
        else if (c->kind == ND_NODE_ELEMENT)
            append_text_children(c, out, depth + 1);
    }
}

static int g_host_scope_counter;

static char *
style_host_scope_id(nd_node *style_el)
{
    nd_node *root = NULL;
    for (nd_node *a = style_el; a; a = a->parent) {
        if (a->kind == ND_NODE_ELEMENT &&
            nd_element_get_attr(a, ND_SHADOW_ATTR) != NULL) {
            root = a;
            break;
        }
    }
    if (!root || !root->parent) return NULL;
    nd_node *host = root->parent;
    const char *existing = nd_element_get_attr(host, ND_HOST_SCOPE_ATTR);
    if (existing) return g_strdup(existing);
    char buf[32];
    g_snprintf(buf, sizeof buf, "%d", ++g_host_scope_counter);
    nd_element_set_attr(host, ND_HOST_SCOPE_ATTR, buf);
    return g_strdup(buf);
}

static char *
style_iframe_scope_id(nd_node *style_el)
{
    nd_node *frame = NULL;
    for (nd_node *a = style_el; a; a = a->parent) {
        if (nd_node_is_element_named(a, "iframe")) {
            frame = a;
            break;
        }
    }
    if (!frame) return NULL;
    const char *existing = nd_element_get_attr(frame, ND_HOST_SCOPE_ATTR);
    if (existing) return g_strdup(existing);
    char buf[32];
    g_snprintf(buf, sizeof buf, "%d", ++g_host_scope_counter);
    nd_element_set_attr(frame, ND_HOST_SCOPE_ATTR, buf);
    return g_strdup(buf);
}

static char *
rewrite_host_selectors(const char *css, const char *host_id)
{
    GString *out = g_string_new(NULL);
    char marker[96];
    g_snprintf(marker, sizeof marker, "[" ND_HOST_SCOPE_ATTR "=\"%s\"]", host_id);
    for (const char *p = css; *p; ) {
        if (p[0] == ':' && g_ascii_strncasecmp(p, "::slotted(", 10) == 0) {
            const char *inner = p + 10;
            const char *q = inner;
            int depth = 1;
            while (*q && depth) {
                if (*q == '(') depth++;
                else if (*q == ')') { depth--; if (!depth) break; }
                q++;
            }
            g_string_append(out, marker);
            g_string_append(out, " > ");
            g_string_append_len(out, inner, (gssize)(q - inner));
            p = (*q == ')') ? q + 1 : q;
            continue;
        }
        if (p[0] == ':' && g_ascii_strncasecmp(p, ":host", 5) == 0) {
            const char *after = p + 5;
            if (g_ascii_strncasecmp(after, "-context(", 9) == 0) {
                const char *q = after + 9;
                int depth = 1;
                while (*q && depth) {
                    if (*q == '(') depth++;
                    else if (*q == ')') depth--;
                    q++;
                }
                g_string_append(out, marker);
                p = q;
                continue;
            }
            if (*after == '(') {
                const char *inner = after + 1;
                const char *q = inner;
                int depth = 1;
                while (*q && depth) {
                    if (*q == '(') depth++;
                    else if (*q == ')') { depth--; if (!depth) break; }
                    q++;
                }
                g_string_append(out, marker);
                g_string_append_len(out, inner, (gssize)(q - inner));
                p = (*q == ')') ? q + 1 : q;
                continue;
            }
            if (!is_ident(*after) && *after != '-') {
                g_string_append(out, marker);
                p = after;
                continue;
            }
        }
        g_string_append_c(out, *p);
        p++;
    }
    return g_string_free(out, FALSE);
}

static void
scope_one_selector(GString *out, const char *sel, gsize len,
                   const char *marker, const char *host_id)
{
    while (len && is_ws(*sel)) { sel++; len--; }
    while (len && is_ws(sel[len - 1])) len--;
    if (!len) return;
    char *s = g_strndup(sel, len);
    if (strstr(s, ":host") || strstr(s, "::slotted")) {
        char *r = rewrite_host_selectors(s, host_id);
        g_string_append(out, r);
        g_free(r);
    } else {
        g_string_append(out, marker);
        g_string_append_c(out, ' ');
        g_string_append(out, s);
    }
    g_free(s);
}

static void
scope_rule_list(GString *out, const char *p, const char *end,
                const char *marker, const char *host_id)
{
    while (p < end) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            if (p + 1 < end) p += 2;
            continue;
        }
        if (*p == '}') { p++; continue; }
        if (*p == '@') {
            const char *prelude = p;
            char term = 0;
            const char *seg = css_scan_segment(p, end, &term);
            if (term == '{') {
                gboolean group =
                    g_ascii_strncasecmp(prelude, "@media", 6) == 0 ||
                    g_ascii_strncasecmp(prelude, "@supports", 9) == 0 ||
                    g_ascii_strncasecmp(prelude, "@container", 10) == 0 ||
                    g_ascii_strncasecmp(prelude, "@layer", 6) == 0 ||
                    g_ascii_strncasecmp(prelude, "@scope", 6) == 0;
                const char *be = css_skip_to_block_end(seg, end);
                if (group) {
                    g_string_append_len(out, prelude, (gssize)(seg - prelude));
                    g_string_append_c(out, '{');
                    const char *body_s = seg + 1;
                    scope_rule_list(out, body_s, css_block_body_end(body_s, be),
                                    marker, host_id);
                    g_string_append_c(out, '}');
                } else {
                    g_string_append_len(out, prelude, (gssize)(be - prelude));
                }
                p = be;
            } else {
                g_string_append_len(out, prelude, (gssize)(seg - prelude));
                if (term == ';' && seg < end) { g_string_append_c(out, ';'); p = seg + 1; }
                else p = seg;
            }
            continue;
        }
        char term = 0;
        const char *seg = css_scan_segment(p, end, &term);
        if (term != '{') { p = (seg < end) ? seg + 1 : end; continue; }
        const char *be = css_skip_to_block_end(seg, end);
        const char *selend = seg;
        const char *q = p, *segstart = p;
        char quote = 0;
        int paren = 0, bracket = 0;
        gboolean first = TRUE;
        for (; q <= selend; q++) {
            if (q == selend || (!quote && !paren && !bracket && *q == ',')) {
                if (!first) g_string_append(out, ", ");
                first = FALSE;
                scope_one_selector(out, segstart, (gsize)(q - segstart),
                                   marker, host_id);
                segstart = q + 1;
                if (q == selend) break;
            } else if (quote) {
                if (*q == '\\' && q + 1 < selend) q++;
                else if (*q == quote) quote = 0;
            } else if (*q == '\\' && q + 1 < selend) q++;
            else if (*q == '"' || *q == '\'') quote = *q;
            else if (*q == '(') paren++;
            else if (*q == ')') { if (paren) paren--; }
            else if (*q == '[') bracket++;
            else if (*q == ']') { if (bracket) bracket--; }
        }
        g_string_append_c(out, '{');
        const char *body_s = seg + 1;
        const char *body_e = css_block_body_end(body_s, be);
        g_string_append_len(out, body_s, (gssize)(body_e - body_s));
        g_string_append_c(out, '}');
        p = be;
    }
}

static char *
scope_shadow_css(const char *flat_css, const char *host_id)
{
    GString *out = g_string_new(NULL);
    char marker[96];
    g_snprintf(marker, sizeof marker, "[" ND_HOST_SCOPE_ATTR "=\"%s\"]", host_id);
    scope_rule_list(out, flat_css, flat_css + strlen(flat_css), marker, host_id);
    return g_string_free(out, FALSE);
}

char *
nd_css_assign_iframe_scope(nd_node *node)
{
    return style_iframe_scope_id(node);
}

char *
nd_css_scope_css(const char *css, gssize len, const char *scope_id)
{
    if (!css || !scope_id || !*scope_id) return NULL;
    char *flat = css_flatten_nesting(css, len);
    char *scoped = scope_shadow_css(flat, scope_id);
    g_free(flat);
    return scoped;
}

void
nd_collect_inline_stylesheets(nd_node *doc, GPtrArray *out)
{
    if (!doc || !out) return;
    GQueue queue = G_QUEUE_INIT;
    g_queue_push_tail(&queue, doc);
    while (!g_queue_is_empty(&queue)) {
        nd_node *n = g_queue_pop_head(&queue);
        if (nd_node_is_element_named(n, "noscript")) continue;
        if (nd_node_is_element_named(n, "style")) {
            const char *media = nd_element_get_attr(n, "media");
            if (!media || !*media || nd_css_media_query_matches(media)) {
                GString *buf = g_string_new(NULL);
                append_text_children(n, buf, 0);
                if (buf->len > 0) {
                    char *rewritten = NULL;
                    const char *text = buf->str;
                    char *host_id = style_host_scope_id(n);
                    if (!host_id) host_id = style_iframe_scope_id(n);
                    if (host_id) {
                        char *flat = css_flatten_nesting(buf->str, (gssize)buf->len);
                        rewritten = scope_shadow_css(flat, host_id);
                        g_free(flat);
                        text = rewritten;
                    }
                    g_free(host_id);
                    nd_css_stylesheet *sh =
                        nd_css_stylesheet_parse(text, (gssize)strlen(text));
                    g_ptr_array_add(out, sh);
                    g_free(rewritten);
                }
                g_string_free(buf, TRUE);
            }
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

    static nd_css_stylesheet *cached_ua = NULL;
    if (!cached_ua) cached_ua = nd_css_stylesheet_parse(kUa, -1);

    gboolean profile = g_getenv("ND_PROFILE") != NULL;
    gint64 t0 = profile ? g_get_monotonic_time() : 0;
    (void)nd_css_rule_index_ensure(cached_ua);
    for (gsize i = 0; i < n_sheets; i++)
        (void)nd_css_rule_index_ensure(author_sheets[i]);
    gint64 t_idx = profile ? g_get_monotonic_time() : 0;

    GHashTable *layer_ranks = g_hash_table_new(g_str_hash, g_str_equal);
    css_layer_rank_add_sheet(layer_ranks, cached_ua);
    for (gsize i = 0; i < n_sheets; i++)
        css_layer_rank_add_sheet(layer_ranks, author_sheets[i]);

    g_registered_props = g_hash_table_new(g_str_hash, g_str_equal);
    css_collect_property_rules(g_registered_props, cached_ua);
    for (gsize i = 0; i < n_sheets; i++)
        css_collect_property_rules(g_registered_props, author_sheets[i]);

    double root_px = 0;
    if (!g_cq_stack)
        g_cq_stack = g_array_new(FALSE, FALSE, sizeof(nd_cq_container));
    g_array_set_size(g_cq_stack, 0);
    cascade_walk(doc, cached_ua, author_sheets, n_sheets, NULL, &root_px,
                 layer_ranks, out);
    g_hash_table_destroy(layer_ranks);
    g_hash_table_destroy(g_registered_props);
    g_registered_props = NULL;
    gint64 t_cascade = profile ? g_get_monotonic_time() : 0;
    if (profile)
        g_printerr("[profile]   css.idx=%.1fms css.cascade=%.1fms\n",
                   (t_idx - t0) / 1000.0,
                   (t_cascade - t_idx) / 1000.0);
    return out;
}
