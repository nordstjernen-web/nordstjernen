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
ns_css_register_defined_element(const char *tag)
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
ns_css_clear_defined_elements(void)
{
    if (g_defined_elements) {
        g_hash_table_destroy(g_defined_elements);
        g_defined_elements = NULL;
    }
}

static gboolean
ns_css_is_defined_element(const char *tag)
{
    if (!g_defined_elements || !tag) return FALSE;
    char *lower = g_ascii_strdown(tag, -1);
    gboolean ok = g_hash_table_contains(g_defined_elements, lower);
    g_free(lower);
    return ok;
}

void
ns_css_set_viewport(double vw_px, double vh_px)
{
    if (vw_px > 0) g_viewport_w = vw_px;
    if (vh_px > 0) g_viewport_h = vh_px;
}

double ns_css_viewport_w(void) { return g_viewport_w; }
double ns_css_viewport_h(void) { return g_viewport_h; }

static __thread double g_cq_unit_w = 0;
static __thread double g_cq_unit_h = 0;

void
ns_css_set_container_dims(double inline_px, double block_px)
{
    g_cq_unit_w = inline_px;
    g_cq_unit_h = block_px;
}

double ns_css_container_w(void) { return g_cq_unit_w; }
double ns_css_container_h(void) { return g_cq_unit_h; }

static double
viewport_resolve(double v, ns_css_unit unit)
{
    switch (unit) {
    case NS_CSS_UNIT_VW:  return v * g_viewport_w / 100.0;
    case NS_CSS_UNIT_VH:  return v * g_viewport_h / 100.0;
    case NS_CSS_UNIT_VMIN: {
        double m = g_viewport_w < g_viewport_h ? g_viewport_w : g_viewport_h;
        return v * m / 100.0;
    }
    case NS_CSS_UNIT_VMAX: {
        double m = g_viewport_w > g_viewport_h ? g_viewport_w : g_viewport_h;
        return v * m / 100.0;
    }
    default: return 0;
    }
}

static char *g_target_fragment = NULL;

void
ns_css_set_target_fragment(const char *fragment)
{
    g_free(g_target_fragment);
    g_target_fragment = (fragment && *fragment) ? g_strdup(fragment) : NULL;
}

static const ns_node *g_css_focus_node = NULL;

const ns_node *
ns_css_set_focus_node(const ns_node *node)
{
    const ns_node *prev = g_css_focus_node;
    g_css_focus_node = node;
    return prev;
}

static const ns_node *g_css_hover_node = NULL;

const ns_node *
ns_css_set_hover_node(const ns_node *node)
{
    const ns_node *prev = g_css_hover_node;
    g_css_hover_node = node;
    return prev;
}

static const ns_node *g_css_active_node = NULL;

const ns_node *
ns_css_set_active_node(const ns_node *node)
{
    const ns_node *prev = g_css_active_node;
    g_css_active_node = node;
    return prev;
}

static const char *kProp[NS_CSS_PROP_COUNT] = {
    [NS_CSS_DISPLAY]              = "display",
    [NS_CSS_COLOR]                = "color",
    [NS_CSS_BACKGROUND_COLOR]     = "background-color",
    [NS_CSS_FONT_SIZE]            = "font-size",
    [NS_CSS_FONT_WEIGHT]          = "font-weight",
    [NS_CSS_FONT_STYLE]           = "font-style",
    [NS_CSS_FONT_STRETCH]         = "font-stretch",
    [NS_CSS_FONT_KERNING]         = "font-kerning",
    [NS_CSS_FONT_VARIANT_LIGATURES] = "font-variant-ligatures",
    [NS_CSS_FONT_FEATURE_SETTINGS] = "font-feature-settings",
    [NS_CSS_FONT_VARIATION_SETTINGS] = "font-variation-settings",
    [NS_CSS_FONT_FAMILY]          = "font-family",
    [NS_CSS_TEXT_ALIGN]           = "text-align",
    [NS_CSS_MARGIN_TOP]           = "margin-top",
    [NS_CSS_MARGIN_RIGHT]         = "margin-right",
    [NS_CSS_MARGIN_BOTTOM]        = "margin-bottom",
    [NS_CSS_MARGIN_LEFT]          = "margin-left",
    [NS_CSS_PADDING_TOP]          = "padding-top",
    [NS_CSS_PADDING_RIGHT]        = "padding-right",
    [NS_CSS_PADDING_BOTTOM]       = "padding-bottom",
    [NS_CSS_PADDING_LEFT]         = "padding-left",
    [NS_CSS_BORDER_TOP_WIDTH]     = "border-top-width",
    [NS_CSS_BORDER_RIGHT_WIDTH]   = "border-right-width",
    [NS_CSS_BORDER_BOTTOM_WIDTH]  = "border-bottom-width",
    [NS_CSS_BORDER_LEFT_WIDTH]    = "border-left-width",
    [NS_CSS_BORDER_TOP_COLOR]     = "border-top-color",
    [NS_CSS_BORDER_RIGHT_COLOR]   = "border-right-color",
    [NS_CSS_BORDER_BOTTOM_COLOR]  = "border-bottom-color",
    [NS_CSS_BORDER_LEFT_COLOR]    = "border-left-color",
    [NS_CSS_BORDER_TOP_STYLE]     = "border-top-style",
    [NS_CSS_BORDER_RIGHT_STYLE]   = "border-right-style",
    [NS_CSS_BORDER_BOTTOM_STYLE]  = "border-bottom-style",
    [NS_CSS_BORDER_LEFT_STYLE]    = "border-left-style",
    [NS_CSS_WIDTH]                = "width",
    [NS_CSS_HEIGHT]               = "height",
    [NS_CSS_MAX_WIDTH]            = "max-width",
    [NS_CSS_MAX_HEIGHT]           = "max-height",
    [NS_CSS_MIN_WIDTH]            = "min-width",
    [NS_CSS_MIN_HEIGHT]           = "min-height",
    [NS_CSS_LINE_HEIGHT]          = "line-height",
    [NS_CSS_TEXT_DECORATION]      = "text-decoration",
    [NS_CSS_POSITION]             = "position",
    [NS_CSS_TOP]                  = "top",
    [NS_CSS_RIGHT]                = "right",
    [NS_CSS_BOTTOM]               = "bottom",
    [NS_CSS_LEFT]                 = "left",
    [NS_CSS_Z_INDEX]              = "z-index",
    [NS_CSS_OPACITY]              = "opacity",
    [NS_CSS_CURSOR]               = "cursor",
    [NS_CSS_POINTER_EVENTS]       = "pointer-events",
    [NS_CSS_LETTER_SPACING]       = "letter-spacing",
    [NS_CSS_WORD_SPACING]         = "word-spacing",
    [NS_CSS_WHITE_SPACE]          = "white-space",
    [NS_CSS_BOX_SIZING]           = "box-sizing",
    [NS_CSS_TEXT_INDENT]          = "text-indent",
    [NS_CSS_TEXT_TRANSFORM]       = "text-transform",
    [NS_CSS_LIST_STYLE_TYPE]      = "list-style-type",
    [NS_CSS_VERTICAL_ALIGN]       = "vertical-align",
    [NS_CSS_VISIBILITY]           = "visibility",
    [NS_CSS_OVERFLOW]             = "overflow",
    [NS_CSS_OVERFLOW_X]           = "overflow-x",
    [NS_CSS_OVERFLOW_Y]           = "overflow-y",
    [NS_CSS_FONT_VARIANT]         = "font-variant",
    [NS_CSS_BORDER_RADIUS]            = "border-radius",
    [NS_CSS_BORDER_TOP_LEFT_RADIUS]     = "border-top-left-radius",
    [NS_CSS_BORDER_TOP_RIGHT_RADIUS]    = "border-top-right-radius",
    [NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS] = "border-bottom-right-radius",
    [NS_CSS_BORDER_BOTTOM_LEFT_RADIUS]  = "border-bottom-left-radius",
    [NS_CSS_FLEX_DIRECTION]       = "flex-direction",
    [NS_CSS_FLEX_WRAP]            = "flex-wrap",
    [NS_CSS_JUSTIFY_CONTENT]      = "justify-content",
    [NS_CSS_ALIGN_ITEMS]          = "align-items",
    [NS_CSS_ALIGN_SELF]           = "align-self",
    [NS_CSS_GAP]                  = "gap",
    [NS_CSS_ROW_GAP]              = "row-gap",
    [NS_CSS_COLUMN_GAP]           = "column-gap",
    [NS_CSS_FLEX_GROW]            = "flex-grow",
    [NS_CSS_FLEX_SHRINK]          = "flex-shrink",
    [NS_CSS_FLEX_BASIS]           = "flex-basis",
    [NS_CSS_ORDER]                = "order",
    [NS_CSS_FLOAT]                = "float",
    [NS_CSS_CLEAR]                = "clear",
    [NS_CSS_BOX_SHADOW]           = "box-shadow",
    [NS_CSS_OUTLINE_WIDTH]        = "outline-width",
    [NS_CSS_OUTLINE_STYLE]        = "outline-style",
    [NS_CSS_OUTLINE_COLOR]        = "outline-color",
    [NS_CSS_OUTLINE_OFFSET]       = "outline-offset",
    [NS_CSS_BACKGROUND_IMAGE]     = "background-image",
    [NS_CSS_BACKGROUND_REPEAT]    = "background-repeat",
    [NS_CSS_BACKGROUND_POSITION_X]= "background-position-x",
    [NS_CSS_BACKGROUND_POSITION_Y]= "background-position-y",
    [NS_CSS_BACKGROUND_SIZE]      = "background-size",
    [NS_CSS_BACKGROUND_CLIP]      = "background-clip",
    [NS_CSS_BACKGROUND_ORIGIN]    = "background-origin",
    [NS_CSS_SCROLLBAR_WIDTH]      = "scrollbar-width",
    [NS_CSS_SCROLLBAR_COLOR]      = "scrollbar-color",
    [NS_CSS_IMAGE_RENDERING]      = "image-rendering",
    [NS_CSS_CONTENT]              = "content",
    [NS_CSS_CLIP]                 = "clip",
    [NS_CSS_CONTENT_VISIBILITY]   = "content-visibility",
    [NS_CSS_GRID_TEMPLATE_COLUMNS]= "grid-template-columns",
    [NS_CSS_GRID_TEMPLATE_ROWS]   = "grid-template-rows",
    [NS_CSS_GRID_TEMPLATE_AREAS]  = "grid-template-areas",
    [NS_CSS_GRID_COLUMN]          = "grid-column",
    [NS_CSS_GRID_ROW]             = "grid-row",
    [NS_CSS_GRID_COLUMN_START]    = "grid-column-start",
    [NS_CSS_GRID_COLUMN_END]      = "grid-column-end",
    [NS_CSS_GRID_ROW_START]       = "grid-row-start",
    [NS_CSS_GRID_ROW_END]         = "grid-row-end",
    [NS_CSS_GRID_AREA]            = "grid-area",
    [NS_CSS_GRID_AUTO_ROWS]       = "grid-auto-rows",
    [NS_CSS_GRID_AUTO_COLUMNS]    = "grid-auto-columns",
    [NS_CSS_GRID_AUTO_FLOW]       = "grid-auto-flow",
    [NS_CSS_TRANSFORM]            = "transform",
    [NS_CSS_TRANSFORM_ORIGIN]     = "transform-origin",
    [NS_CSS_TRANSITION]           = "transition",
    [NS_CSS_ANIMATION]            = "animation",
    [NS_CSS_ASPECT_RATIO]         = "aspect-ratio",
    [NS_CSS_TEXT_SHADOW]          = "text-shadow",
    [NS_CSS_OVERFLOW_WRAP]        = "overflow-wrap",
    [NS_CSS_WORD_BREAK]           = "word-break",
    [NS_CSS_HYPHENS]              = "hyphens",
    [NS_CSS_TEXT_OVERFLOW]        = "text-overflow",
    [NS_CSS_TEXT_DECORATION_COLOR]= "text-decoration-color",
    [NS_CSS_TEXT_DECORATION_STYLE]= "text-decoration-style",
    [NS_CSS_LIST_STYLE_POSITION]  = "list-style-position",
    [NS_CSS_LIST_STYLE_IMAGE]     = "list-style-image",
    [NS_CSS_USER_SELECT]          = "user-select",
    [NS_CSS_QUOTES]               = "quotes",
    [NS_CSS_COLUMN_COUNT]         = "column-count",
    [NS_CSS_COLUMN_WIDTH]         = "column-width",
    [NS_CSS_COLUMN_RULE_WIDTH]    = "column-rule-width",
    [NS_CSS_COLUMN_RULE_STYLE]    = "column-rule-style",
    [NS_CSS_COLUMN_RULE_COLOR]    = "column-rule-color",
    [NS_CSS_FILTER]               = "filter",
    [NS_CSS_CLIP_PATH]            = "clip-path",
    [NS_CSS_MIX_BLEND_MODE]       = "mix-blend-mode",
    [NS_CSS_ACCENT_COLOR]         = "accent-color",
    [NS_CSS_COUNTER_RESET]        = "counter-reset",
    [NS_CSS_COUNTER_INCREMENT]    = "counter-increment",
    [NS_CSS_LINE_CLAMP]           = "-webkit-line-clamp",
    [NS_CSS_OBJECT_FIT]           = "object-fit",
    [NS_CSS_OBJECT_POSITION_X]    = "object-position-x",
    [NS_CSS_OBJECT_POSITION_Y]    = "object-position-y",
    [NS_CSS_MASK_IMAGE]           = "mask-image",
    [NS_CSS_APPEARANCE]           = "appearance",
    [NS_CSS_TABLE_LAYOUT]         = "table-layout",
    [NS_CSS_CAPTION_SIDE]         = "caption-side",
    [NS_CSS_BORDER_COLLAPSE]      = "border-collapse",
    [NS_CSS_BORDER_SPACING]       = "border-spacing",
    [NS_CSS_CONTAINER_TYPE]       = "container-type",
    [NS_CSS_CONTAINER_NAME]       = "container-name",
    [NS_CSS_WRITING_MODE]         = "writing-mode",
    [NS_CSS_TEXT_ORIENTATION]     = "text-orientation",
    [NS_CSS_TRANSITION_DELAY]     = "transition-delay",
    [NS_CSS_TRANSITION_DURATION]  = "transition-duration",
    [NS_CSS_ANIMATION_DELAY]      = "animation-delay",
    [NS_CSS_ANIMATION_DURATION]   = "animation-duration",
    [NS_CSS_ORPHANS]              = "orphans",
    [NS_CSS_WIDOWS]               = "widows",
    [NS_CSS_MAX_LINES]            = "max-lines",
    [NS_CSS_HYPHENATE_LIMIT_LINES] = "hyphenate-limit-lines",
    [NS_CSS_COLUMN_SPAN]          = "column-span",
    [NS_CSS_CARET_COLOR]          = "caret-color",
    [NS_CSS_TAB_SIZE]             = "tab-size",
    [NS_CSS_JUSTIFY_ITEMS]        = "justify-items",
    [NS_CSS_JUSTIFY_SELF]         = "justify-self",
    [NS_CSS_ALIGN_CONTENT]        = "align-content",
    [NS_CSS_DIRECTION]            = "direction",
    [NS_CSS_UNICODE_BIDI]         = "unicode-bidi",
    [NS_CSS_TRANSLATE]            = "translate",
    [NS_CSS_ROTATE]               = "rotate",
    [NS_CSS_SCALE]                = "scale",
    [NS_CSS_PERSPECTIVE]          = "perspective",
    [NS_CSS_PERSPECTIVE_ORIGIN]   = "perspective-origin",
    [NS_CSS_TRANSFORM_STYLE]      = "transform-style",
    [NS_CSS_BACKFACE_VISIBILITY]  = "backface-visibility",
    [NS_CSS_ANIMATION_PLAY_STATE] = "animation-play-state",
};

static gboolean
prop_inherits(ns_css_prop p)
{
    switch (p) {
    case NS_CSS_COLOR:
    case NS_CSS_FONT_SIZE:
    case NS_CSS_FONT_WEIGHT:
    case NS_CSS_FONT_STYLE:
    case NS_CSS_FONT_STRETCH:
    case NS_CSS_FONT_KERNING:
    case NS_CSS_FONT_VARIANT_LIGATURES:
    case NS_CSS_FONT_FEATURE_SETTINGS:
    case NS_CSS_FONT_VARIATION_SETTINGS:
    case NS_CSS_FONT_FAMILY:
    case NS_CSS_FONT_VARIANT:
    case NS_CSS_LINE_HEIGHT:
    case NS_CSS_LETTER_SPACING:
    case NS_CSS_WORD_SPACING:
    case NS_CSS_WHITE_SPACE:
    case NS_CSS_HYPHENS:
    case NS_CSS_DIRECTION:
    case NS_CSS_WRITING_MODE:
    case NS_CSS_TEXT_ORIENTATION:
    case NS_CSS_CAPTION_SIDE:
    case NS_CSS_BORDER_COLLAPSE:
    case NS_CSS_BORDER_SPACING:
    case NS_CSS_TEXT_ALIGN:
    case NS_CSS_TEXT_INDENT:
    case NS_CSS_TEXT_TRANSFORM:
    case NS_CSS_LIST_STYLE_TYPE:
    case NS_CSS_LIST_STYLE_POSITION:
    case NS_CSS_LIST_STYLE_IMAGE:
    case NS_CSS_USER_SELECT:
    case NS_CSS_QUOTES:
    case NS_CSS_VISIBILITY:
    case NS_CSS_CURSOR:
    case NS_CSS_POINTER_EVENTS:
    case NS_CSS_SCROLLBAR_COLOR:
    case NS_CSS_IMAGE_RENDERING:
    case NS_CSS_TAB_SIZE:
    case NS_CSS_WORD_BREAK:
    case NS_CSS_OVERFLOW_WRAP:
    case NS_CSS_CARET_COLOR:
    case NS_CSS_ACCENT_COLOR:
        return TRUE;
    default:
        return FALSE;
    }
}

int
ns_css_writing_mode(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_WRITING_MODE] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return 0;
    const char *k = v->u.keyword;
    if (strcmp(k, "vertical-rl") == 0 || strcmp(k, "sideways-rl") == 0 ||
        strcmp(k, "tb-rl") == 0 || strcmp(k, "tb") == 0)
        return 1;
    if (strcmp(k, "vertical-lr") == 0 || strcmp(k, "sideways-lr") == 0)
        return 2;
    return 0;
}

int
ns_css_text_orientation(const ns_style *s)
{
    const ns_css_value *v = s ? s->values[NS_CSS_TEXT_ORIENTATION] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return 0;
    const char *k = v->u.keyword;
    if (strcmp(k, "upright") == 0) return 1;
    if (strcmp(k, "sideways") == 0 || strcmp(k, "sideways-right") == 0) return 2;
    return 0;
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
    if (p < end && is_ws(*p)) {
        gboolean cr = *p == '\r';
        p++;
        if (cr && p < end && *p == '\n') p++;
    }
    g_string_append_unichar(out, css_unescape_cp(cp));
    *pp = p;
}

void
ns_css_append_unescaped(GString *out, const char **pp)
{
    const char *p = *pp;
    if (*p == '\\' && p[1]) {
        p++;
        if (g_ascii_isxdigit(*p)) {
            gunichar cp = 0;
            int n = 0;
            while (n < 6 && g_ascii_isxdigit(*p)) {
                cp = cp * 16 + (gunichar)g_ascii_xdigit_value(*p);
                p++;
                n++;
            }
            if (is_ws(*p)) {
                gboolean cr = *p == '\r';
                p++;
                if (cr && *p == '\n') p++;
            }
            g_string_append_unichar(out, css_unescape_cp(cp));
        } else {
            g_string_append_c(out, *p++);
        }
    } else {
        g_string_append_c(out, *p++);
    }
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
static gboolean parse_color_depth(const char *s, guint8 *r, guint8 *g,
                                  guint8 *b, guint8 *a, int depth);

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

static ns_css_value *
parse_css_wide_keyword(const char *text)
{
    char *kw = ascii_lower(text, strlen(text));
    if (!css_wide_keyword_is(kw)) {
        g_free(kw);
        return NULL;
    }
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_KEYWORD;
    v->u.keyword = kw;
    return v;
}

static ns_css_value *
ns_css_value_dup(const ns_css_value *v)
{
    if (!v) return NULL;
    ((ns_css_value *)v)->ref++;
    return (ns_css_value *)v;
}

static void
ns_css_value_free(ns_css_value *v)
{
    while (v) {
        if (v->ref > 0) { v->ref--; return; }
        if (v->kind == NS_CSS_V_KEYWORD) g_free(v->u.keyword);
        else if (v->kind == NS_CSS_V_URL) g_free(v->u.url);
        else if (v->kind == NS_CSS_V_AREAS) {
            for (int i = 0; i < v->u.areas.n_rects; i++)
                g_free(v->u.areas.rects[i].name);
        }
        else if (v->kind == NS_CSS_V_ANIM) {
            for (int i = 0; i < v->u.anim.n; i++)
                g_free(v->u.anim.entries[i].name);
        }
        ns_css_value *next = v->next_layer;
        g_free(v);
        v = next;
    }
}

int
ns_css_value_layer_count(const ns_css_value *head)
{
    int n = 0;
    for (const ns_css_value *l = head; l; l = l->next_layer) n++;
    return n;
}

const ns_css_value *
ns_css_value_layer(const ns_css_value *head, int index)
{
    int n = ns_css_value_layer_count(head);
    if (n == 0) return NULL;
    index %= n;
    const ns_css_value *l = head;
    while (index-- > 0) l = l->next_layer;
    return l;
}

double
ns_css_length_or(const ns_css_value *v, double fallback)
{
    if (!v) return fallback;
    if (v->kind == NS_CSS_V_LENGTH &&
        (v->u.length.unit == NS_CSS_UNIT_PX ||
         v->u.length.unit == NS_CSS_UNIT_NUMBER))
        return v->u.length.v;
    if (v->kind == NS_CSS_V_CALC)
        return v->u.calc.px;
    return fallback;
}

static double
column_len_px(const ns_css_value *v, double basis, double fallback)
{
    if (!v) return fallback;
    if (v->kind == NS_CSS_V_CALC)
        return v->u.calc.pct / 100.0 * basis + v->u.calc.px;
    if (v->kind != NS_CSS_V_LENGTH) return fallback;
    switch (v->u.length.unit) {
    case NS_CSS_UNIT_PX:
    case NS_CSS_UNIT_NUMBER: return v->u.length.v;
    case NS_CSS_UNIT_EM:
    case NS_CSS_UNIT_REM:    return v->u.length.v * 16.0;
    case NS_CSS_UNIT_PERCENT: return v->u.length.v * basis / 100.0;
    case NS_CSS_UNIT_VW:     return v->u.length.v * ns_css_viewport_w() / 100.0;
    case NS_CSS_UNIT_VH:     return v->u.length.v * ns_css_viewport_h() / 100.0;
    default:                 return fallback;
    }
}

int
ns_css_used_column_count(const ns_style *s, double avail_w, double *out_gap)
{
    double gap = 16.0;
    if (s) {
        const ns_css_value *cg = s->values[NS_CSS_COLUMN_GAP];
        if (!cg || cg->kind != NS_CSS_V_LENGTH)
            cg = s->values[NS_CSS_GAP];
        if (cg) {
            double g = column_len_px(cg, avail_w, -1);
            if (g >= 0) gap = g;
        }
    }
    if (out_gap) *out_gap = gap;
    int n = 1;
    if (s && s->values[NS_CSS_COLUMN_COUNT] &&
        s->values[NS_CSS_COLUMN_COUNT]->kind == NS_CSS_V_LENGTH) {
        double v = s->values[NS_CSS_COLUMN_COUNT]->u.length.v;
        if (v >= 2) n = (int)(v + 0.5);
    }
    if (n == 1 && s && s->values[NS_CSS_COLUMN_WIDTH] &&
        s->values[NS_CSS_COLUMN_WIDTH]->kind == NS_CSS_V_LENGTH) {
        double colw = column_len_px(s->values[NS_CSS_COLUMN_WIDTH], avail_w, 0);
        if (colw > 1 && avail_w > colw + gap) {
            int fit = (int)((avail_w + gap) / (colw + gap));
            if (fit > 1) n = fit;
        }
    }
    return n;
}

gboolean
ns_css_keyword_is(const ns_css_value *v, const char *kw)
{
    return v && v->kind == NS_CSS_V_KEYWORD && kw &&
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
        strcmp(lo, "ui-rounded") == 0 ||
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
ns_css_set_font_available_cb(gboolean (*cb)(const char *family))
{
    g_font_available_cb = cb;
}

static void (*g_font_metrics_cb)(const char *family, double size_px,
                                 int weight, gboolean italic,
                                 ns_css_font_metrics *out);

void
ns_css_set_font_metrics_cb(
    void (*cb)(const char *family, double size_px, int weight,
               gboolean italic, ns_css_font_metrics *out))
{
    g_font_metrics_cb = cb;
}

static double
font_relative_unit_px(ns_css_unit unit, double font_px,
                      const char *family, int weight, gboolean italic)
{
    ns_css_font_metrics m = {
        .ex_px  = font_px * 0.5,
        .ch_px  = font_px * 0.5,
        .cap_px = font_px * 0.7,
        .ic_px  = font_px,
    };
    if (g_font_metrics_cb && font_px > 0)
        g_font_metrics_cb(family, font_px, weight, italic, &m);
    switch (unit) {
    case NS_CSS_UNIT_EX:  return m.ex_px;
    case NS_CSS_UNIT_CH:  return m.ch_px;
    case NS_CSS_UNIT_CAP: return m.cap_px;
    case NS_CSS_UNIT_IC:  return m.ic_px;
    default:              return font_px;
    }
}

static void
legacy_em_normalize(double *val, ns_css_unit *unit)
{
    switch (*unit) {
    case NS_CSS_UNIT_EX:  *val *= 0.5; *unit = NS_CSS_UNIT_EM; break;
    case NS_CSS_UNIT_CH:  *val *= 0.5; *unit = NS_CSS_UNIT_EM; break;
    case NS_CSS_UNIT_CAP: *val *= 0.7; *unit = NS_CSS_UNIT_EM; break;
    case NS_CSS_UNIT_IC:  *unit = NS_CSS_UNIT_EM; break;
    default: break;
    }
}

char *
ns_css_font_family_for_pango(const char *css_family)
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
ns_css_font_weight_number(const ns_css_value *v, int fallback)
{
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return fallback;
    const char *kw = v->u.keyword;
    if (strcmp(kw, "normal") == 0) return 400;
    if (strcmp(kw, "bold") == 0) return 700;
    if (strcmp(kw, "bolder") == 0) {
        int base = fallback > 0 ? fallback : 400;
        if (base < 400) return 400;
        if (base < 600) return 700;
        return 900;
    }
    if (strcmp(kw, "lighter") == 0) {
        int base = fallback > 0 ? fallback : 400;
        if (base < 600) return 100;
        if (base < 800) return 400;
        return 700;
    }
    if (g_ascii_isdigit(kw[0])) {
        return ns_parse_int(kw, fallback > 0 ? fallback : 400, 1, 1000);
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
        { "darkslategrey",   47,  79,  79  },
        { "darkturquoise",   0,   206, 209 },
        { "darkviolet",      148, 0,   211 },
        { "deeppink",        255, 20,  147 },
        { "deepskyblue",     0,   191, 255 },
        { "dimgray",         105, 105, 105 },
        { "dimgrey",         105, 105, 105 },
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
        { "lightslategrey",  119, 136, 153 },
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
        { "slategrey",       112, 128, 144 },
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
    gboolean alpha_pct = FALSE;
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
        } else if (count == 1 || count == 2) {
            if (*end != '%') return FALSE;   /* saturation/lightness need % */
            end++;
        } else if (*end == '%') {
            alpha_pct = TRUE;
            end++;
        }
        values[count++] = v;
        p = end;
    }
    if (count < 3) return FALSE;
    double h = values[0] / 360.0;
    h = isfinite(h) ? h - floor(h) : 0.0;
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
        if (alpha_pct) alpha /= 100.0;
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
    h = isfinite(h) ? h - floor(h) : 0.0;
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
srgb_decode_gamma(double c)
{
    if (c <= 0.04045) return c / 12.92;
    return pow((c + 0.055) / 1.055, 2.4);
}

static void
srgb_to_oklab(guint8 r, guint8 g, guint8 b, double *ol, double *oa, double *ob)
{
    double rl = srgb_decode_gamma(r / 255.0);
    double gl = srgb_decode_gamma(g / 255.0);
    double bl = srgb_decode_gamma(b / 255.0);
    double l = 0.4122214708 * rl + 0.5363325363 * gl + 0.0514459929 * bl;
    double m = 0.2119034982 * rl + 0.6806995451 * gl + 0.1073969566 * bl;
    double s = 0.0883024619 * rl + 0.2817188376 * gl + 0.6299787005 * bl;
    double lp = cbrt(l), mp = cbrt(m), sp = cbrt(s);
    *ol = 0.2104542553 * lp + 0.7936177850 * mp - 0.0040720468 * sp;
    *oa = 1.9779984951 * lp - 2.4285922050 * mp + 0.4505937099 * sp;
    *ob = 0.0259040371 * lp + 0.7827717662 * mp - 0.8086757660 * sp;
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
                     gboolean *has_pct, int depth)
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
        ok = parse_color_depth(tokens[0], &rgba[0], &rgba[1], &rgba[2],
                               &rgba[3], depth + 1);
    }
done:
    for (int i = 0; i < n; i++) g_free(tokens[i]);
    return ok;
}

static gboolean
parse_color_mix_func(const char *s, guint8 *r, guint8 *g, guint8 *b,
                     guint8 *a, int depth)
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
    gboolean in_oklab = FALSE;
    if (ok) {
        space += 2;
        while (*space && is_ws(*space)) space++;
        gsize sl = 0;
        while (space[sl] && !is_ws(space[sl])) sl++;
        in_oklab = (sl == 5 &&
                    (g_ascii_strncasecmp(space, "oklab", 5) == 0 ||
                     g_ascii_strncasecmp(space, "oklch", 5) == 0));
        ok = in_oklab ||
             (sl == 4 && g_ascii_strncasecmp(space, "srgb", 4) == 0) ||
             (sl == 11 && g_ascii_strncasecmp(space, "srgb-linear", 11) == 0) ||
             (sl == 3 && (g_ascii_strncasecmp(space, "hsl", 3) == 0 ||
                          g_ascii_strncasecmp(space, "hwb", 3) == 0 ||
                          g_ascii_strncasecmp(space, "lab", 3) == 0 ||
                          g_ascii_strncasecmp(space, "lch", 3) == 0 ||
                          g_ascii_strncasecmp(space, "xyz", 3) == 0));
    }
    guint8 c1[4] = {0}, c2[4] = {0};
    double p1 = 50, p2 = 50;
    gboolean h1 = FALSE, h2 = FALSE;
    if (ok)
        ok = parse_color_mix_stop(parts[1], c1, &p1, &h1, depth) &&
             parse_color_mix_stop(parts[2], c2, &p2, &h2, depth);
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
            if (in_oklab) {
                double l1, aa1, bb1, l2, aa2, bb2;
                srgb_to_oklab(c1[0], c1[1], c1[2], &l1, &aa1, &bb1);
                srgb_to_oklab(c2[0], c2[1], c2[2], &l2, &aa2, &bb2);
                double lo = 0, ao2 = 0, bo = 0;
                if (ao > 0) {
                    lo  = (l1 * a1 * w1 + l2 * a2 * w2) / ao;
                    ao2 = (aa1 * a1 * w1 + aa2 * a2 * w2) / ao;
                    bo  = (bb1 * a1 * w1 + bb2 * a2 * w2) / ao;
                }
                oklab_to_srgb(lo, ao2, bo, r, g, b);
            } else {
                double rr = 0, gg = 0, bb = 0;
                if (ao > 0) {
                    rr = (c1[0] * a1 * w1 + c2[0] * a2 * w2) / ao;
                    gg = (c1[1] * a1 * w1 + c2[1] * a2 * w2) / ao;
                    bb = (c1[2] * a1 * w1 + c2[2] * a2 * w2) / ao;
                }
                *r = (guint8)CLAMP((int)(rr + 0.5), 0, 255);
                *g = (guint8)CLAMP((int)(gg + 0.5), 0, 255);
                *b = (guint8)CLAMP((int)(bb + 0.5), 0, 255);
            }
            *a = (guint8)CLAMP((int)(ao * 255 + 0.5), 0, 255);
        }
    }
    for (int i = 0; i < n; i++) g_free(parts[i]);
    return ok;
}

typedef struct {
    double v;
    char unit[8];
} ns_color_calc_term;

#define NS_CALC_MAX_DEPTH 64

static gboolean color_calc_expr(const char **pp, const char *end,
                                ns_color_calc_term *out, int depth);

static gboolean
color_calc_factor(const char **pp, const char *end, ns_color_calc_term *out,
                  int depth)
{
    const char *p = *pp;
    if (depth > NS_CALC_MAX_DEPTH) return FALSE;
    while (p < end && is_ws(*p)) p++;
    if (p < end && *p == '(') {
        p++;
        if (!color_calc_expr(&p, end, out, depth + 1)) return FALSE;
        while (p < end && is_ws(*p)) p++;
        if (p >= end || *p != ')') return FALSE;
        *pp = p + 1;
        return TRUE;
    }
    if (p + 5 <= end && g_ascii_strncasecmp(p, "calc(", 5) == 0) {
        p += 5;
        if (!color_calc_expr(&p, end, out, depth + 1)) return FALSE;
        while (p < end && is_ws(*p)) p++;
        if (p >= end || *p != ')') return FALSE;
        *pp = p + 1;
        return TRUE;
    }
    char *num_end = NULL;
    double v = g_ascii_strtod(p, &num_end);
    if (!num_end || num_end == p || num_end > end) return FALSE;
    out->v = v;
    int ui = 0;
    p = num_end;
    while (p < end && (is_ident(*p) || *p == '%') &&
           ui < (int)sizeof out->unit - 1)
        out->unit[ui++] = *p++;
    out->unit[ui] = '\0';
    *pp = p;
    return TRUE;
}

static gboolean
color_calc_term_mul(const char **pp, const char *end, ns_color_calc_term *out,
                    int depth)
{
    if (!color_calc_factor(pp, end, out, depth)) return FALSE;
    for (;;) {
        const char *p = *pp;
        while (p < end && is_ws(*p)) p++;
        if (p >= end || (*p != '*' && *p != '/')) return TRUE;
        char op = *p++;
        ns_color_calc_term rhs;
        if (!color_calc_factor(&p, end, &rhs, depth)) return FALSE;
        if (op == '*') {
            if (out->unit[0] && rhs.unit[0]) return FALSE;
            out->v *= rhs.v;
            if (rhs.unit[0]) g_strlcpy(out->unit, rhs.unit, sizeof out->unit);
        } else {
            if (rhs.unit[0] || rhs.v == 0) return FALSE;
            out->v /= rhs.v;
        }
        *pp = p;
    }
}

static gboolean
color_calc_expr(const char **pp, const char *end, ns_color_calc_term *out,
                int depth)
{
    if (!color_calc_term_mul(pp, end, out, depth)) return FALSE;
    for (;;) {
        const char *p = *pp;
        while (p < end && is_ws(*p)) p++;
        if (p >= end || (*p != '+' && *p != '-')) return TRUE;
        char op = *p++;
        ns_color_calc_term rhs;
        if (!color_calc_term_mul(&p, end, &rhs, depth)) return FALSE;
        if (g_ascii_strcasecmp(out->unit, rhs.unit) != 0) {
            if (!out->unit[0] && out->v == 0)
                g_strlcpy(out->unit, rhs.unit, sizeof out->unit);
            else if (!(rhs.unit[0] == '\0' && rhs.v == 0))
                return FALSE;
        }
        out->v = op == '+' ? out->v + rhs.v : out->v - rhs.v;
        *pp = p;
    }
}

static char *
color_resolve_calcs(const char *s)
{
    const char *s_end = s + strlen(s);
    GString *out = g_string_new(NULL);
    const char *p = s;
    while (*p) {
        if (g_ascii_strncasecmp(p, "calc(", 5) == 0) {
            const char *body = p + 5;
            const char *close = match_close_paren(body, s_end);
            if (!close) { g_string_free(out, TRUE); return NULL; }
            const char *q = body;
            ns_color_calc_term t = { 0, "" };
            if (!color_calc_expr(&q, close, &t, 0)) {
                g_string_free(out, TRUE);
                return NULL;
            }
            g_string_append_printf(out, "%.6g%s", t.v, t.unit);
            p = close + 1;
        } else {
            g_string_append_c(out, *p++);
        }
    }
    return g_string_free(out, FALSE);
}

static gboolean
parse_color_depth(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *a,
                  int depth)
{
    *a = 255;
    if (!s || !*s) return FALSE;
    if (depth > 32) return FALSE;
    if (strstr(s, "calc(")) {
        char *flat = color_resolve_calcs(s);
        if (flat) {
            gboolean ok = parse_color_depth(flat, r, g, b, a, depth + 1);
            g_free(flat);
            return ok;
        }
        return FALSE;
    }
    if (g_ascii_strcasecmp(s, "transparent") == 0) {
        *r = 0; *g = 0; *b = 0; *a = 0;
        return TRUE;
    }
    if (parse_rgb_func(s, r, g, b, a)) return TRUE;
    if (parse_hsl_func(s, r, g, b, a)) return TRUE;
    if (parse_hwb_func(s, r, g, b, a)) return TRUE;
    if (parse_lab_func(s, r, g, b, a)) return TRUE;
    if (parse_oklab_func(s, r, g, b, a)) return TRUE;
    if (parse_color_mix_func(s, r, g, b, a, depth)) return TRUE;
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

static gboolean
parse_color(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
    return parse_color_depth(s, r, g, b, a, 0);
}

gboolean
ns_css_parse_color(const char *s, guint8 *r, guint8 *g, guint8 *b, guint8 *a)
{
    return parse_color(s, r, g, b, a);
}

static void
ns_attr_pred_clear(gpointer p)
{
    ns_css_attr_pred *a = p;
    g_free(a->name);
    g_free(a->value);
}

static void
matches_any_group_free(gpointer data)
{
    g_ptr_array_free((GPtrArray *)data, TRUE);
}

static void
ns_pseudo_pred_clear(gpointer p)
{
    ns_css_pseudo_pred *pc = p;
    g_free(pc->arg);
    if (pc->of_group) g_ptr_array_free(pc->of_group, TRUE);
}

static ns_css_simple *
ns_css_simple_new(void)
{
    ns_css_simple *s = g_new0(ns_css_simple, 1);
    s->classes = g_ptr_array_new_with_free_func(g_free);
    s->class_lens = g_array_new(FALSE, FALSE, sizeof(gsize));
    s->attrs   = g_array_new(FALSE, FALSE, sizeof(ns_css_attr_pred));
    g_array_set_clear_func(s->attrs, ns_attr_pred_clear);
    s->pseudos = g_array_new(FALSE, FALSE, sizeof(ns_css_pseudo_pred));
    g_array_set_clear_func(s->pseudos, ns_pseudo_pred_clear);
    return s;
}

static void
ns_css_simple_free(ns_css_simple *s)
{
    if (!s) return;
    g_free(s->type);
    g_free(s->id);
    g_ptr_array_free(s->classes, TRUE);
    g_array_free(s->class_lens, TRUE);
    if (s->attrs)   g_array_free(s->attrs,   TRUE);
    if (s->pseudos) g_array_free(s->pseudos, TRUE);
    if (s->matches_any)  g_ptr_array_free(s->matches_any,  TRUE);
    if (s->matches_none) g_ptr_array_free(s->matches_none, TRUE);
    if (s->has_groups)   g_ptr_array_free(s->has_groups,   TRUE);
    g_free(s);
}

static void
ns_css_selector_free(ns_css_selector *sel)
{
    if (!sel) return;
    for (guint i = 0; i < sel->compounds->len; i++)
        ns_css_simple_free(g_ptr_array_index(sel->compounds, i));
    g_ptr_array_free(sel->compounds, TRUE);
    g_array_free(sel->combinators, TRUE);
    g_free(sel);
}

typedef struct ns_css_scope {
    GPtrArray *roots;
    GPtrArray *limits;
} ns_css_scope;

typedef struct ns_css_scope_text {
    char *start;
    char *end;
} ns_css_scope_text;

#define NS_CSS_MAX_SELECTOR_NESTING 48
#define NS_CSS_MAX_AT_NESTING 32

static gboolean g_sel_parse_error;
static gboolean g_sel_ns_prefix;
static gboolean g_sel_has_hover;
static gboolean g_sel_has_active;
static gboolean g_sel_strict;
static int g_sel_has_depth;

static ns_css_selector *parse_one_selector_rel(const char **pp, const char *end,
                                               int depth, gboolean relative);
static ns_css_selector *parse_one_selector(const char **pp, const char *end,
                                           int depth);

static GPtrArray *
parse_selector_group_rel(const char *arg, gsize arg_n, int depth,
                         gboolean relative)
{
    GPtrArray *group = g_ptr_array_new_with_free_func(
        (GDestroyNotify)ns_css_selector_free);
    if (depth > NS_CSS_MAX_SELECTOR_NESTING)
        return group;
    const char *p = arg;
    const char *end = arg + arg_n;
    while (p < end) {
        const char *loop_start = p;
        p = css_skip_ws_comments(p, end);
        if (p >= end) break;
        ns_css_selector *sub = parse_one_selector_rel(&p, end, depth, relative);
        if (sub) g_ptr_array_add(group, sub);
        else if (g_sel_strict) g_sel_parse_error = TRUE;
        p = css_skip_ws_comments(p, end);
        if (p < end && *p == ',') { p++; continue; }
        if (p == loop_start) p++;
    }
    return group;
}

static GPtrArray *
parse_selector_group(const char *arg, gsize arg_n, int depth)
{
    return parse_selector_group_rel(arg, arg_n, depth, FALSE);
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
anb_int_strict(const char *str, int *out)
{
    const char *p = str;
    if (*p == '+' || *p == '-') p++;
    if (!g_ascii_isdigit(*p)) return FALSE;
    for (const char *q = p; *q; q++)
        if (!g_ascii_isdigit(*q)) return FALSE;
    *out = ns_parse_int(str, 0, -1000000, 1000000);
    return TRUE;
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
    gboolean ok = TRUE;
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
            const char *a_str = s;
            if (!*a_str || strcmp(a_str, "+") == 0) a = 1;
            else if (strcmp(a_str, "-") == 0) a = -1;
            else ok = anb_int_strict(a_str, &a);
            const char *b_str = n_pos + 1;
            if (*b_str) {
                if (*b_str != '+' && *b_str != '-') ok = FALSE;
                else ok = ok && anb_int_strict(b_str, &b);
            }
        } else {
            a = 0;
            ok = anb_int_strict(s, &b);
        }
    }
    g_free(s);
    g_free(raw);
    if (!ok) return FALSE;
    *out_a = a;
    *out_b = b;
    return TRUE;
}

static gboolean
css_pseudo_class_is_standard(const char *name, gsize n)
{
    static const char *known[] = {
        "default", "indeterminate", "in-range", "out-of-range",
        "fullscreen", "modal", "autofill", "blank",
        "user-valid", "user-invalid", "target-within", "focus-visible",
        "local-link", "current", "past", "future",
        "playing", "paused", "muted", "seeking", "buffering", "stalled",
        "picture-in-picture", "volume-locked",
        "host", "host-context", "nth-col", "nth-last-col", "state",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(known); i++)
        if (strlen(known[i]) == n && g_ascii_strncasecmp(name, known[i], n) == 0)
            return TRUE;
    return FALSE;
}

static gboolean
css_pseudo_element_is_standard(const char *name, gsize n)
{
    static const char *known[] = {
        "part", "slotted", "cue", "cue-region", "highlight",
        "target-text", "spelling-error", "grammar-error",
        "file-selector-button", "details-content",
        "view-transition", "view-transition-group",
        "view-transition-image-pair", "view-transition-old",
        "view-transition-new",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(known); i++)
        if (strlen(known[i]) == n && g_ascii_strncasecmp(name, known[i], n) == 0)
            return TRUE;
    return FALSE;
}

static gboolean
parse_pseudo_keyword(const char *name, gsize n,
                     const char *arg, gsize alen,
                     ns_css_pseudo_pred *out)
{
    struct { const char *k; ns_css_pseudo v; } table[] = {
        { "first-child",   NS_CSS_PC_FIRST_CHILD },
        { "last-child",    NS_CSS_PC_LAST_CHILD },
        { "only-child",    NS_CSS_PC_ONLY_CHILD },
        { "first-of-type", NS_CSS_PC_FIRST_OF_TYPE },
        { "last-of-type",  NS_CSS_PC_LAST_OF_TYPE },
        { "only-of-type",  NS_CSS_PC_ONLY_OF_TYPE },
        { "empty",         NS_CSS_PC_EMPTY },
        { "root",          NS_CSS_PC_ROOT },
        { "checked",       NS_CSS_PC_CHECKED },
        { "disabled",      NS_CSS_PC_DISABLED },
        { "enabled",       NS_CSS_PC_ENABLED },
        { "required",      NS_CSS_PC_REQUIRED },
        { "optional",      NS_CSS_PC_OPTIONAL },
        { "valid",         NS_CSS_PC_VALID },
        { "invalid",       NS_CSS_PC_INVALID },
        { "in-range",      NS_CSS_PC_IN_RANGE },
        { "out-of-range",  NS_CSS_PC_OUT_OF_RANGE },
        { "default",       NS_CSS_PC_DEFAULT },
        { "indeterminate", NS_CSS_PC_INDETERMINATE },
        { "link",          NS_CSS_PC_LINK },
        { "visited",       NS_CSS_PC_VISITED },
        { "any-link",      NS_CSS_PC_ANY_LINK },
        { "hover",         NS_CSS_PC_HOVER },
        { "active",        NS_CSS_PC_ACTIVE },
        { "focus",         NS_CSS_PC_FOCUS },
        { "focus-visible", NS_CSS_PC_FOCUS },
        { "focus-within",  NS_CSS_PC_FOCUS_WITHIN },
        { "target",        NS_CSS_PC_TARGET },
        { "target-within", NS_CSS_PC_TARGET_WITHIN },
        { "defined",       NS_CSS_PC_DEFINED },
        { "scope",         NS_CSS_PC_SCOPE },
        { "placeholder-shown", NS_CSS_PC_PLACEHOLDER_SHOWN },
        { "read-only",     NS_CSS_PC_READ_ONLY },
        { "read-write",    NS_CSS_PC_READ_WRITE },
        { "blank",         NS_CSS_PC_BLANK },
        { "open",          NS_CSS_PC_OPEN },
        { "popover-open",  NS_CSS_PC_POPOVER_OPEN },
        { "modal",         NS_CSS_PC_MODAL },
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
    if (n == 7 && g_ascii_strncasecmp(name, "heading", 7) == 0) {
        out->kind = NS_CSS_PC_HEADING;
        out->a = 0;
        out->b = 0;
        if (!arg) {
            out->arg = NULL;
            return TRUE;
        }
        char *raw = g_strndup(arg, alen);
        char **items = g_strsplit(raw, ",", -1);
        gboolean ok = items[0] != NULL;
        for (int i = 0; ok && items[i]; i++) {
            int v = 0;
            if (!anb_int_strict(g_strstrip(items[i]), &v)) ok = FALSE;
        }
        g_strfreev(items);
        if (!ok) {
            g_free(raw);
            return FALSE;
        }
        out->arg = raw;
        return TRUE;
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
        if (n == 9) out->kind = NS_CSS_PC_NTH_CHILD;
        else if (n == 14) out->kind = NS_CSS_PC_NTH_LAST_CHILD;
        else if (n == 11) out->kind = NS_CSS_PC_NTH_OF_TYPE;
        else out->kind = NS_CSS_PC_NTH_LAST_OF_TYPE;
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
        out->kind = NS_CSS_PC_LANG;
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
        out->kind = NS_CSS_PC_DIR;
        out->arg = lo;
        return TRUE;
    }
    return FALSE;
}

static void
selector_group_max_specificity(const GPtrArray *group, int *a, int *b, int *c)
{
    for (guint i = 0; group && i < group->len; i++) {
        const ns_css_selector *sub = g_ptr_array_index(group, i);
        if (sub->spec_a > *a ||
            (sub->spec_a == *a && sub->spec_b > *b) ||
            (sub->spec_a == *a && sub->spec_b == *b && sub->spec_c > *c)) {
            *a = sub->spec_a;
            *b = sub->spec_b;
            *c = sub->spec_c;
        }
    }
}

static ns_css_selector *
parse_one_selector(const char **pp, const char *end, int depth)
{
    return parse_one_selector_rel(pp, end, depth, FALSE);
}

static ns_css_selector *
parse_one_selector_rel(const char **pp, const char *end, int depth,
                       gboolean relative)
{
    ns_css_selector *sel = g_new0(ns_css_selector, 1);
    sel->compounds   = g_ptr_array_new();
    sel->combinators = g_array_new(FALSE, FALSE, sizeof(ns_css_comb));

    ns_css_comb pending = NS_CSS_COMB_NONE;
    gboolean expect_compound = TRUE;
    gboolean leading_comb_used = FALSE;
    const char *p = *pp;

    while (p < end) {

        gboolean had_ws = FALSE;
        const char *before_ws = p;
        p = css_skip_ws_comments(p, end);
        had_ws = p > before_ws;
        if (p >= end) break;
        char c = *p;

        if (c == ',' || c == '{') break;

        if (c == '>' || c == '+' || c == '~') {
            if (relative && sel->compounds->len == 0 && !leading_comb_used)
                leading_comb_used = TRUE;
            else if (expect_compound || sel->compounds->len == 0)
                g_sel_parse_error = TRUE;
            pending = c == '>' ? NS_CSS_COMB_CHILD
                    : c == '+' ? NS_CSS_COMB_ADJACENT
                    : NS_CSS_COMB_SIBLING;
            expect_compound = TRUE;
            p++;
            continue;
        }

        if (had_ws && !expect_compound)
            pending = NS_CSS_COMB_DESCENDANT;

        ns_css_simple *cmp = ns_css_simple_new();
        gboolean any = FALSE;
        while (p < end) {
            const char *tok_start = p;
            char cc = *p;
            if (cc == '*' || (cc == '|' && !(p + 1 < end && p[1] == '='))) {
                if (cc == '*') {
                    p++;
                }
                if (p < end && *p == '|' && !(p + 1 < end && p[1] == '=')) {
                    if (cc == '|')
                        cmp->ns_none = TRUE;
                    p++;
                    if (p < end && *p == '*') {
                        p++;
                        g_free(cmp->type);
                        cmp->type = g_strdup("*");
                    }
                    else {
                        char *type = read_css_ident(&p, end);
                        if (type && *type) {
                            if (!cmp->type) {
                                cmp->type = ascii_lower(type, strlen(type));
                                sel->spec_c += 1;
                            }
                        }
                        else {
                            g_sel_parse_error = TRUE;
                        }
                        g_free(type);
                    }
                }
                else {
                    if (cmp->type) {
                        g_sel_parse_error = TRUE;
                        cmp->never_match = TRUE;
                    }
                    g_free(cmp->type);
                    cmp->type = g_strdup("*");
                }
                any = TRUE;
            } else if (cc == '#') {
                p++;
                char *id_str = read_css_ident(&p, end);
                if (id_str && *id_str) {
                    g_free(cmp->id);
                    cmp->id = id_str;
                    sel->spec_a += 1;
                } else {
                    g_sel_parse_error = TRUE;
                    cmp->never_match = TRUE;
                    g_free(id_str);
                }
                any = TRUE;
            } else if (cc == '.') {
                p++;
                gboolean bad_start = FALSE;
                if (p < end) {
                    unsigned char nc = (unsigned char)*p;
                    if (g_ascii_isdigit(nc))
                        bad_start = TRUE;
                    else if (nc == '-' && p + 1 < end &&
                             g_ascii_isdigit((unsigned char)p[1]))
                        bad_start = TRUE;
                }
                char *cls = read_css_ident(&p, end);
                if (!bad_start && cls && *cls) {
                    gsize cls_len = strlen(cls);
                    g_ptr_array_add(cmp->classes, cls);
                    g_array_append_val(cmp->class_lens, cls_len);
                    sel->spec_b += 1;
                } else {
                    g_sel_parse_error = TRUE;
                    cmp->never_match = TRUE;
                    g_free(cls);
                }
                any = TRUE;
            } else if (is_ident_start(cc) || cc == '\\') {
                char *type = read_css_ident(&p, end);
                if (p < end && *p == '|' && !(p + 1 < end && p[1] == '=')) {
                    g_sel_ns_prefix = TRUE;
                    cmp->never_match = TRUE;
                    p++;
                    if (p < end && *p == '*') {
                        p++;
                    }
                    else {
                        char *unused = read_css_ident(&p, end);
                        g_free(unused);
                    }
                }
                else if (!cmp->type) {
                    cmp->type = ascii_lower(type, strlen(type));
                    sel->spec_c += 1;
                }
                else {
                    g_sel_parse_error = TRUE;
                    cmp->never_match = TRUE;
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
                if (name_n == 0) {
                    g_sel_parse_error = TRUE;
                    cmp->never_match = TRUE;
                    g_free(pseudo_name);
                    any = TRUE;
                    continue;
                }
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
                        sel->pseudo_element = NS_CSS_PE_BEFORE;
                        sel->spec_c += 1;
                    } else if (name_n == 5 && g_ascii_strncasecmp(name_s, "after", 5) == 0) {
                        sel->pseudo_element = NS_CSS_PE_AFTER;
                        sel->spec_c += 1;
                    } else if (name_n == 12 && g_ascii_strncasecmp(name_s, "first-letter", 12) == 0) {
                        sel->pseudo_element = NS_CSS_PE_FIRST_LETTER;
                        sel->spec_c += 1;
                    } else if (name_n == 10 && g_ascii_strncasecmp(name_s, "first-line", 10) == 0) {
                        sel->pseudo_element = NS_CSS_PE_FIRST_LINE;
                        sel->spec_c += 1;
                    } else if (name_n == 9 && g_ascii_strncasecmp(name_s, "selection", 9) == 0) {
                        sel->pseudo_element = NS_CSS_PE_SELECTION;
                        sel->spec_c += 1;
                    } else if (name_n == 6 && g_ascii_strncasecmp(name_s, "marker", 6) == 0) {
                        sel->pseudo_element = NS_CSS_PE_MARKER;
                        sel->spec_c += 1;
                    } else if (name_n == 8 && g_ascii_strncasecmp(name_s, "backdrop", 8) == 0) {
                        sel->pseudo_element = NS_CSS_PE_BACKDROP;
                        sel->spec_c += 1;
                    } else if ((name_n == 11 &&
                                g_ascii_strncasecmp(name_s, "placeholder", 11) == 0) ||
                               (name_n == 25 &&
                                g_ascii_strncasecmp(name_s, "-webkit-input-placeholder", 25) == 0) ||
                               (name_n == 21 &&
                                g_ascii_strncasecmp(name_s, "-ms-input-placeholder", 21) == 0) ||
                               (name_n == 16 &&
                                g_ascii_strncasecmp(name_s, "-moz-placeholder", 16) == 0)) {
                        sel->pseudo_element = NS_CSS_PE_PLACEHOLDER;
                        sel->spec_c += 1;
                    } else {
                        cmp->never_match = TRUE;
                        if (name_s[0] != '-'
                            && !css_pseudo_element_is_standard(name_s, name_n))
                            g_sel_parse_error = TRUE;
                    }
                } else if (name_n == 3 && arg_s &&
                           g_ascii_strncasecmp(name_s, "has", 3) == 0 &&
                           g_sel_has_depth > 0) {
                    cmp->never_match = TRUE;
                    g_sel_parse_error = TRUE;
                } else if (name_n == 3 && arg_s &&
                           g_ascii_strncasecmp(name_s, "has", 3) == 0) {
                    g_sel_has_depth++;
                    GPtrArray *group = parse_selector_group_rel(arg_s, arg_n,
                                                                depth + 1, TRUE);
                    g_sel_has_depth--;
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
                            const ns_css_selector *sub =
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
                    gboolean saved_err = g_sel_parse_error;
                    gboolean saved_ns = g_sel_ns_prefix;
                    GPtrArray *group = parse_selector_group(arg_s, arg_n, depth + 1);
                    if (!g_sel_strict) g_sel_parse_error = saved_err;
                    g_sel_ns_prefix = saved_ns;
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
                                const ns_css_selector *sub =
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
                            const ns_css_selector *sub =
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
                    ns_css_pseudo_pred pc = {0};
                    if (parse_pseudo_keyword(name_s, name_n, arg_s, arg_n, &pc)) {
                        g_array_append_val(cmp->pseudos, pc);
                        if (pc.kind == NS_CSS_PC_HOVER)
                            g_sel_has_hover = TRUE;
                        if (pc.kind == NS_CSS_PC_ACTIVE)
                            g_sel_has_active = TRUE;
                        sel->spec_b += 1;
                        int ma = 0, mb = 0, mc = 0;
                        selector_group_max_specificity(pc.of_group, &ma, &mb, &mc);
                        sel->spec_a += ma;
                        sel->spec_b += mb;
                        sel->spec_c += mc;
                    } else {
                        cmp->never_match = TRUE;
                        if (!css_pseudo_class_is_standard(name_s, name_n))
                            g_sel_parse_error = TRUE;
                    }
                } else {
                    cmp->never_match = TRUE;
                    g_sel_parse_error = TRUE;
                }
                g_free(pseudo_name);
                any = TRUE;
            } else if (cc == '[') {
                p++;
                p = css_skip_ws_comments(p, end);
                if (p + 1 < end && *p == '*' && p[1] == '|') {
                    p += 2;
                }
                else if (p < end && *p == '|' && !(p + 1 < end && p[1] == '=')) {
                    p++;
                }
                char *attr_name = read_css_ident(&p, end);
                if (attr_name && *attr_name && p < end && *p == '|'
                    && !(p + 1 < end && p[1] == '='))
                {
                    g_sel_ns_prefix = TRUE;
                    g_free(attr_name);
                    p++;
                    attr_name = read_css_ident(&p, end);
                    cmp->never_match = TRUE;
                }
                if (!attr_name || !*attr_name) {
                    g_free(attr_name);
                    char term = 0;
                    const char *close = css_scan_until(p, end, "]", &term);
                    p = term == ']' ? close + 1 : close;
                    continue;
                }
                ns_css_attr_pred ap = {0};
                ap.name = ascii_lower(attr_name, strlen(attr_name));
                ap.name_bit = ns_attr_name_bloom_bit(ap.name);
                g_free(attr_name);
                ap.op   = NS_CSS_ATTR_PRESENT;
                p = css_skip_ws_comments(p, end);
                if (p < end && (*p == '=' || *p == '^' || *p == '$' ||
                                *p == '*' || *p == '~' || *p == '|')) {
                    char op_c = *p;
                    if (op_c == '=')      ap.op = NS_CSS_ATTR_EQ;
                    else if (op_c == '^') { p++; if (p < end && *p == '=') ap.op = NS_CSS_ATTR_PREFIX; }
                    else if (op_c == '$') { p++; if (p < end && *p == '=') ap.op = NS_CSS_ATTR_SUFFIX; }
                    else if (op_c == '*') { p++; if (p < end && *p == '=') ap.op = NS_CSS_ATTR_SUBSTR; }
                    else if (op_c == '~') { p++; if (p < end && *p == '=') ap.op = NS_CSS_ATTR_WORD;   }
                    else if (op_c == '|') { p++; if (p < end && *p == '=') ap.op = NS_CSS_ATTR_HYPHEN; }
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
                if (p < end && *p != ']') {
                    const char *flag_start = p;
                    char *flag = read_css_ident(&p, end);
                    if (flag && g_ascii_strcasecmp(flag, "i") == 0) {
                        if (ap.op == NS_CSS_ATTR_PRESENT) g_sel_parse_error = TRUE;
                        ap.case_insensitive = TRUE;
                    } else if (flag && g_ascii_strcasecmp(flag, "s") == 0) {
                        if (ap.op == NS_CSS_ATTR_PRESENT) g_sel_parse_error = TRUE;
                        ap.case_sensitive = TRUE;
                    } else {
                        p = flag_start;
                        g_sel_parse_error = TRUE;
                    }
                    g_free(flag);
                }
                p = css_skip_ws_comments(p, end);
                if (p < end && *p != ']')
                    g_sel_parse_error = TRUE;
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
        if (!any) { ns_css_simple_free(cmp); break; }
        g_ptr_array_add(sel->compounds, cmp);
        g_array_append_val(sel->combinators, pending);
        pending = NS_CSS_COMB_NONE;
        expect_compound = FALSE;
    }
    *pp = p;
    if (pending != NS_CSS_COMB_NONE)
        g_sel_parse_error = TRUE;
    if (sel->compounds->len == 0) {
        ns_css_selector_free(sel);
        return NULL;
    }
    return sel;
}

static gboolean
parse_length(const char *text, double *out_v, ns_css_unit *out_unit)
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
    if (*end == '\0') { *out_unit = NS_CSS_UNIT_NUMBER; return TRUE; }
    if (g_ascii_strcasecmp(end, "px") == 0) { *out_unit = NS_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "em")  == 0) { *out_unit = NS_CSS_UNIT_EM;  return TRUE; }
    if (g_ascii_strcasecmp(end, "rem") == 0) { *out_unit = NS_CSS_UNIT_REM; return TRUE; }
    if (g_ascii_strcasecmp(end, "%")   == 0) { *out_unit = NS_CSS_UNIT_PERCENT; return TRUE; }
    if (g_ascii_strcasecmp(end, "vw") == 0) { *out_unit = NS_CSS_UNIT_VW; return TRUE; }
    if (g_ascii_strcasecmp(end, "vh") == 0) { *out_unit = NS_CSS_UNIT_VH; return TRUE; }
    if (g_ascii_strcasecmp(end, "dvw") == 0 || g_ascii_strcasecmp(end, "svw") == 0 ||
        g_ascii_strcasecmp(end, "lvw") == 0) { *out_unit = NS_CSS_UNIT_VW; return TRUE; }
    if (g_ascii_strcasecmp(end, "dvh") == 0 || g_ascii_strcasecmp(end, "svh") == 0 ||
        g_ascii_strcasecmp(end, "lvh") == 0) { *out_unit = NS_CSS_UNIT_VH; return TRUE; }
    if (g_ascii_strcasecmp(end, "cqi") == 0 || g_ascii_strcasecmp(end, "cqw") == 0) {
        *out_unit = NS_CSS_UNIT_CQW;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "cqb") == 0 || g_ascii_strcasecmp(end, "cqh") == 0) {
        *out_unit = NS_CSS_UNIT_CQH;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "vi") == 0 || g_ascii_strcasecmp(end, "dvi") == 0 ||
        g_ascii_strcasecmp(end, "svi") == 0 || g_ascii_strcasecmp(end, "lvi") == 0) {
        *out_unit = NS_CSS_UNIT_VW;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "vb") == 0 || g_ascii_strcasecmp(end, "dvb") == 0 ||
        g_ascii_strcasecmp(end, "svb") == 0 || g_ascii_strcasecmp(end, "lvb") == 0) {
        *out_unit = NS_CSS_UNIT_VH;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "vmin") == 0) { *out_unit = NS_CSS_UNIT_VMIN; return TRUE; }
    if (g_ascii_strcasecmp(end, "vmax") == 0) { *out_unit = NS_CSS_UNIT_VMAX; return TRUE; }
    if (g_ascii_strcasecmp(end, "dvmin") == 0 || g_ascii_strcasecmp(end, "svmin") == 0 ||
        g_ascii_strcasecmp(end, "lvmin") == 0) { *out_unit = NS_CSS_UNIT_VMIN; return TRUE; }
    if (g_ascii_strcasecmp(end, "dvmax") == 0 || g_ascii_strcasecmp(end, "svmax") == 0 ||
        g_ascii_strcasecmp(end, "lvmax") == 0) { *out_unit = NS_CSS_UNIT_VMAX; return TRUE; }
    if (g_ascii_strcasecmp(end, "cqmin") == 0) { *out_unit = NS_CSS_UNIT_CQMIN; return TRUE; }
    if (g_ascii_strcasecmp(end, "cqmax") == 0) { *out_unit = NS_CSS_UNIT_CQMAX; return TRUE; }
    if (g_ascii_strcasecmp(end, "pt")  == 0) {
        *out_v = v * (96.0 / 72.0);
        *out_unit = NS_CSS_UNIT_PX;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "pc")  == 0) {
        *out_v = v * 16.0;
        *out_unit = NS_CSS_UNIT_PX;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "ex")  == 0) { *out_unit = NS_CSS_UNIT_EX;  return TRUE; }
    if (g_ascii_strcasecmp(end, "ch")  == 0) { *out_unit = NS_CSS_UNIT_CH;  return TRUE; }
    if (g_ascii_strcasecmp(end, "cap") == 0) { *out_unit = NS_CSS_UNIT_CAP; return TRUE; }
    if (g_ascii_strcasecmp(end, "ic")  == 0) { *out_unit = NS_CSS_UNIT_IC;  return TRUE; }
    if (g_ascii_strcasecmp(end, "lh") == 0) {
        *out_unit = NS_CSS_UNIT_EM;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "rlh") == 0) {
        *out_unit = NS_CSS_UNIT_REM;
        return TRUE;
    }
    if (g_ascii_strcasecmp(end, "cm")  == 0) { *out_v = v * (96.0 / 2.54); *out_unit = NS_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "mm")  == 0) { *out_v = v * (96.0 / 25.4); *out_unit = NS_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "q")   == 0) { *out_v = v * (96.0 / 101.6); *out_unit = NS_CSS_UNIT_PX; return TRUE; }
    if (g_ascii_strcasecmp(end, "in")  == 0) { *out_v = v * 96.0;   *out_unit = NS_CSS_UNIT_PX; return TRUE; }
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
parse_font_size_token(const char *text, double *out_v, ns_css_unit *out_unit,
                      double *out_lh, ns_css_unit *out_lh_unit,
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
        *out_unit = NS_CSS_UNIT_PX;
        ok = TRUE;
    } else {
        ok = parse_length(s, out_v, out_unit) &&
             *out_unit != NS_CSS_UNIT_NUMBER;
    }
    if (ok && slash && slash[1] && out_lh && out_lh_unit &&
        parse_length(slash + 1, out_lh, out_lh_unit)) {
        if (out_has_lh) *out_has_lh = TRUE;
    }
    g_free(s);
    return ok;
}

static ns_css_value *parse_calc(const char *text);
static ns_css_value *parse_calc_inner(const char *text);
static char *angle_expr_rewrite(const char *s, gboolean to_radians);

static gboolean
resolve_to_px_pct(const char *text, gsize len, double *out_px, double *out_pct)
{
    char *s = g_strndup(text, len);
    g_strstrip(s);
    *out_px = 0;
    *out_pct = 0;
    ns_css_value *v = parse_calc(s);
    if (!v) {
        char *wrapped = g_strdup_printf("calc(%s)", s);
        v = parse_calc(wrapped);
        g_free(wrapped);
    }
    if (v && v->kind == NS_CSS_V_CALC) {
        double rel = (v->u.calc.em + v->u.calc.rem) * 16.0;
        *out_px = rel == 0 ? v->u.calc.px : v->u.calc.px + rel;
        *out_pct = v->u.calc.pct;
        ns_css_value_free(v);
        g_free(s);
        return TRUE;
    }
    if (v && v->kind == NS_CSS_V_LENGTH) {
        switch (v->u.length.unit) {
        case NS_CSS_UNIT_PERCENT:
            *out_pct = v->u.length.v;
            break;
        case NS_CSS_UNIT_EM:
        case NS_CSS_UNIT_REM:
            *out_px = v->u.length.v * 16.0;
            break;
        default:
            *out_px = v->u.length.v;
            break;
        }
        ns_css_value_free(v);
        g_free(s);
        return TRUE;
    }
    if (v) ns_css_value_free(v);
    double num;
    ns_css_unit u;
    if (parse_length(s, &num, &u)) {
        switch (u) {
        case NS_CSS_UNIT_PERCENT: *out_pct = num; break;
        case NS_CSS_UNIT_EM:
        case NS_CSS_UNIT_REM:     *out_px = num * 16.0; break;
        case NS_CSS_UNIT_VW:      *out_px = num * g_viewport_w / 100.0; break;
        case NS_CSS_UNIT_VH:      *out_px = num * g_viewport_h / 100.0; break;
        case NS_CSS_UNIT_VMIN:
            *out_px = num * (g_viewport_w < g_viewport_h ?
                             g_viewport_w : g_viewport_h) / 100.0;
            break;
        case NS_CSS_UNIT_VMAX:
            *out_px = num * (g_viewport_w > g_viewport_h ?
                             g_viewport_w : g_viewport_h) / 100.0;
            break;
        case NS_CSS_UNIT_CQW:     *out_px = num * g_viewport_w / 100.0; break;
        case NS_CSS_UNIT_CQH:     *out_px = num * g_viewport_h / 100.0; break;
        case NS_CSS_UNIT_CQMIN:
            *out_px = num * (g_viewport_w < g_viewport_h ?
                             g_viewport_w : g_viewport_h) / 100.0;
            break;
        case NS_CSS_UNIT_CQMAX:
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

typedef struct ns_calc_term {
    double px;
    double pct;
    double em;
    double rem;
    double num;
    gboolean is_number;
} ns_calc_term;

static void
calc_skip_ws(const char **pp, const char *end)
{
    const char *p = *pp;
    while (p < end && is_ws(*p)) p++;
    *pp = p;
}

static void
calc_term_scale(ns_calc_term *v, double m)
{
    if (v->is_number) {
        v->num *= m;
    } else if (isfinite(m)) {
        v->px *= m;
        v->pct *= m;
        v->em *= m;
        v->rem *= m;
    } else {
        if (v->px  != 0) v->px  *= m;
        if (v->pct != 0) v->pct *= m;
        if (v->em  != 0) v->em  *= m;
        if (v->rem != 0) v->rem *= m;
    }
}

static void
calc_term_lengthify(ns_calc_term *v)
{
    if (!v->is_number) return;
    v->px = v->num;
    v->pct = 0;
    v->is_number = FALSE;
}

static gboolean calc_expr_parse(const char **pp, const char *end,
                                ns_calc_term *out, int depth);

static gboolean
calc_unit_value(const char *unit, double num, ns_calc_term *out)
{
    memset(out, 0, sizeof(*out));
    if (!isfinite(num) && (!unit || !*unit)) {
        out->num = num;
        out->is_number = TRUE;
        return TRUE;
    }
    char *text = g_strdup_printf("%.17g%s", num, unit ? unit : "");
    double v = 0;
    ns_css_unit u = NS_CSS_UNIT_NUMBER;
    gboolean ok = parse_length(text, &v, &u);
    g_free(text);
    if (!ok) return FALSE;
    switch (u) {
    case NS_CSS_UNIT_NUMBER:
        out->num = v;
        out->is_number = TRUE;
        break;
    case NS_CSS_UNIT_PERCENT:
        out->pct = v;
        break;
    case NS_CSS_UNIT_EM:
        out->em = v;
        break;
    case NS_CSS_UNIT_EX:
    case NS_CSS_UNIT_CH:
        out->em = v * 0.5;
        break;
    case NS_CSS_UNIT_CAP:
        out->em = v * 0.7;
        break;
    case NS_CSS_UNIT_IC:
        out->em = v;
        break;
    case NS_CSS_UNIT_REM:
        out->rem = v;
        break;
    case NS_CSS_UNIT_VW:
        out->px = v * g_viewport_w / 100.0;
        break;
    case NS_CSS_UNIT_VH:
        out->px = v * g_viewport_h / 100.0;
        break;
    case NS_CSS_UNIT_VMIN:
        out->px = v * (g_viewport_w < g_viewport_h ?
                       g_viewport_w : g_viewport_h) / 100.0;
        break;
    case NS_CSS_UNIT_VMAX:
        out->px = v * (g_viewport_w > g_viewport_h ?
                       g_viewport_w : g_viewport_h) / 100.0;
        break;
    case NS_CSS_UNIT_CQW:
        out->px = v * g_viewport_w / 100.0;
        break;
    case NS_CSS_UNIT_CQH:
        out->px = v * g_viewport_h / 100.0;
        break;
    case NS_CSS_UNIT_CQMIN:
        out->px = v * (g_viewport_w < g_viewport_h ?
                       g_viewport_w : g_viewport_h) / 100.0;
        break;
    case NS_CSS_UNIT_CQMAX:
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
calc_primary_parse(const char **pp, const char *end, ns_calc_term *out,
                   int depth)
{
    if (depth > NS_CALC_MAX_DEPTH) return FALSE;
    const char *p = *pp;
    calc_skip_ws(&p, end);
    if (p >= end) return FALSE;
    if ((gsize)(end - p) > 4 && g_ascii_strncasecmp(p, "env(", 4) == 0) {
        const char *args = p + 4;
        const char *close = match_close_paren(args, end);
        if (!close) return FALSE;
        char *parts[2] = {0};
        int n = calc_split_args(args, close, parts, G_N_ELEMENTS(parts));
        memset(out, 0, sizeof(*out));
        if (n >= 2)
            resolve_to_px_pct(parts[1], strlen(parts[1]), &out->px, &out->pct);
        for (int i = 0; i < n; i++) g_free(parts[i]);
        *pp = close + 1;
        return TRUE;
    }
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
        { "hypot", 5 }, { "pow", 3 }, { "sqrt", 4 }, { "atan2", 5 },
        { "atan", 4 }, { "asin", 4 }, { "acos", 4 }, { "sign", 4 },
        { "sin", 3 }, { "cos", 3 }, { "tan", 3 }, { "exp", 3 },
        { "log", 3 }, { "progress", 8 },
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
        ns_css_value *v = parse_calc(frag);
        g_free(frag);
        if (!v) return FALSE;
        memset(out, 0, sizeof(*out));
        if (v->kind == NS_CSS_V_CALC) {
            out->px = v->u.calc.px;
            out->pct = v->u.calc.pct;
            out->em = v->u.calc.em;
            out->rem = v->u.calc.rem;
        } else if (v->kind == NS_CSS_V_LENGTH) {
            double num = v->u.length.v;
            switch (v->u.length.unit) {
            case NS_CSS_UNIT_PERCENT: out->pct = num; break;
            case NS_CSS_UNIT_EM:      out->em = num; break;
            case NS_CSS_UNIT_EX:      out->em = num * 0.5; break;
            case NS_CSS_UNIT_CH:      out->em = num * 0.5; break;
            case NS_CSS_UNIT_CAP:     out->em = num * 0.7; break;
            case NS_CSS_UNIT_IC:      out->em = num; break;
            case NS_CSS_UNIT_REM:     out->rem = num; break;
            case NS_CSS_UNIT_VW:      out->px = num * g_viewport_w / 100.0; break;
            case NS_CSS_UNIT_VH:      out->px = num * g_viewport_h / 100.0; break;
            case NS_CSS_UNIT_VMIN:
                out->px = num * (g_viewport_w < g_viewport_h ?
                                 g_viewport_w : g_viewport_h) / 100.0;
                break;
            case NS_CSS_UNIT_VMAX:
                out->px = num * (g_viewport_w > g_viewport_h ?
                                 g_viewport_w : g_viewport_h) / 100.0;
                break;
            case NS_CSS_UNIT_CQW:     out->px = num * g_viewport_w / 100.0; break;
            case NS_CSS_UNIT_CQH:     out->px = num * g_viewport_h / 100.0; break;
            case NS_CSS_UNIT_CQMIN:
                out->px = num * (g_viewport_w < g_viewport_h ?
                                 g_viewport_w : g_viewport_h) / 100.0;
                break;
            case NS_CSS_UNIT_CQMAX:
                out->px = num * (g_viewport_w > g_viewport_h ?
                                 g_viewport_w : g_viewport_h) / 100.0;
                break;
            case NS_CSS_UNIT_NUMBER:
                out->num = num;
                out->is_number = TRUE;
                break;
            default:                  out->px = num; break;
            }
        } else {
            ns_css_value_free(v);
            return FALSE;
        }
        ns_css_value_free(v);
        *pp = close + 1;
        return TRUE;
    }
    {
        static const struct { const char *name; gsize len; double val; } consts[] = {
            { "infinity", 8, INFINITY }, { "pi", 2, G_PI }, { "e", 1, G_E },
            { "nan", 3, NAN },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(consts); i++) {
            gsize L = consts[i].len;
            if ((gsize)(end - p) < L ||
                g_ascii_strncasecmp(p, consts[i].name, L) != 0)
                continue;
            const char *after = p + L;
            if (after < end && (g_ascii_isalnum(*after) || *after == '.' ||
                                *after == '%' || *after == '('))
                continue;
            memset(out, 0, sizeof(*out));
            out->num = consts[i].val;
            out->is_number = TRUE;
            *pp = after;
            return TRUE;
        }
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
calc_product_parse(const char **pp, const char *end, ns_calc_term *out,
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
        ns_calc_term rhs;
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
            if (!rhs.is_number) return FALSE;
            calc_term_scale(out, 1.0 / rhs.num);
        }
        *pp = p;
    }
}

static gboolean
calc_expr_parse(const char **pp, const char *end, ns_calc_term *out,
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
        ns_calc_term rhs;
        if (!calc_product_parse(&p, end, &rhs, depth)) return FALSE;
        if (out->is_number != rhs.is_number) return FALSE;
        if (out->is_number) {
            if (op == '+') out->num += rhs.num;
            else           out->num -= rhs.num;
            *pp = p;
            continue;
        }
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
    if (!resolve_to_px_pct(text, strlen(text), &px, &pct)) {
        char *w = g_strdup_printf("calc(%s)", text);
        gboolean ok = resolve_to_px_pct(w, strlen(w), &px, &pct);
        g_free(w);
        if (!ok) return FALSE;
    }
    double add = pct * 0.01 * g_viewport_w;
    *out = add == 0 ? px : px + add;
    return TRUE;
}

static gboolean
calc_token_sign(const char *text, double *out)
{
    static const char *const units[] = {
        "s", "ms", "deg", "grad", "rad", "turn", "hz", "khz",
        "dpi", "dpcm", "dppx", "x", "fr",
    };
    char *s = g_strdup(text);
    g_strstrip(s);
    const char *p = s;
    gboolean ok = FALSE;
    char *end = NULL;
    double num = g_ascii_strtod(p, &end);
    if (end && end != p) {
        const char *u = end;
        while (*u && g_ascii_isalpha((guchar)*u)) u++;
        gsize ulen = (gsize)(u - end);
        const char *rest = u;
        while (*rest && is_ws(*rest)) rest++;
        if (*rest == '\0' && ulen > 0) {
            for (gsize i = 0; i < G_N_ELEMENTS(units); i++)
                if (strlen(units[i]) == ulen &&
                    g_ascii_strncasecmp(end, units[i], ulen) == 0) {
                    *out = isnan(num) ? NAN : num > 0 ? 1 : num < 0 ? -1 : num;
                    ok = TRUE;
                    break;
                }
        }
    }
    g_free(s);
    return ok;
}

typedef enum {
    PT_INVALID, PT_NUMBER, PT_LENGTHPCT, PT_ANGLE
} ns_prog_type;

static ns_prog_type
progress_operand(const char *text, double *out)
{
    char *w = g_strdup_printf("calc(%s)", text);
    ns_css_value *v = parse_calc(w);
    g_free(w);
    ns_prog_type ty = PT_INVALID;
    if (v) {
        if (v->kind == NS_CSS_V_LENGTH) {
            if (v->u.length.unit == NS_CSS_UNIT_NUMBER) {
                ty = PT_NUMBER;
                *out = v->u.length.v;
            } else if (v->u.length.unit == NS_CSS_UNIT_PERCENT) {
                ty = PT_LENGTHPCT;
                *out = v->u.length.v * 0.01 * g_viewport_w;
            } else {
                ty = PT_LENGTHPCT;
                *out = v->u.length.v;
            }
        } else if (v->kind == NS_CSS_V_CALC) {
            ty = PT_LENGTHPCT;
            *out = v->u.calc.px + (v->u.calc.em + v->u.calc.rem) * 16.0 +
                   v->u.calc.pct * 0.01 * g_viewport_w;
        }
        ns_css_value_free(v);
        return ty;
    }
    static const struct { const char *u; double to_deg; } angs[] = {
        { "deg", 1.0 }, { "grad", 0.9 }, { "rad", 180.0 / G_PI },
        { "turn", 360.0 },
    };
    char *s = g_strdup(text);
    g_strstrip(s);
    char *end = NULL;
    double num = g_ascii_strtod(s, &end);
    ns_prog_type ty2 = PT_INVALID;
    if (end && end != s) {
        const char *u = end;
        while (*u && g_ascii_isalpha((guchar)*u)) u++;
        gsize ulen = (gsize)(u - end);
        if (*u == '\0' && ulen > 0)
            for (gsize i = 0; i < G_N_ELEMENTS(angs); i++)
                if (strlen(angs[i].u) == ulen &&
                    g_ascii_strncasecmp(end, angs[i].u, ulen) == 0) {
                    *out = num * angs[i].to_deg;
                    ty2 = PT_ANGLE;
                    break;
                }
    }
    g_free(s);
    return ty2;
}

static gboolean
calc_arg_is_number(const char *text)
{
    char *s = g_strdup(text);
    g_strstrip(s);
    char *endp = NULL;
    g_ascii_strtod(s, &endp);
    gboolean num = endp && endp != s && *endp == '\0';
    if (!num) {
        ns_css_value *v = parse_calc(s);
        if (!v) {
            char *wrapped = g_strdup_printf("calc(%s)", s);
            v = parse_calc(wrapped);
            g_free(wrapped);
        }
        num = v && v->kind == NS_CSS_V_LENGTH &&
              v->u.length.unit == NS_CSS_UNIT_NUMBER;
        if (v) ns_css_value_free(v);
    }
    g_free(s);
    return num;
}

static ns_css_value *
calc_px_value(double px)
{
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_LENGTH;
    v->u.length.v = px;
    v->u.length.unit = NS_CSS_UNIT_PX;
    return v;
}

static ns_css_value *
calc_num_value(double n)
{
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_LENGTH;
    v->u.length.v = n;
    v->u.length.unit = NS_CSS_UNIT_NUMBER;
    return v;
}

static double
css_round_step(int strategy, double a, double b)
{
    if (isnan(a) || isnan(b) || b == 0) return NAN;
    if (isinf(a)) return isinf(b) ? NAN : a;
    if (isinf(b)) {
        if (strategy == 1) return a > 0 ? INFINITY : 0.0;
        if (strategy == 2) return a < 0 ? -INFINITY : 0.0;
        return 0.0;
    }
    double q = a / fabs(b);
    double rq = strategy == 1 ? ceil(q) :
                strategy == 2 ? floor(q) :
                strategy == 3 ? trunc(q) : round(q);
    return rq * fabs(b);
}

static double
css_mod_rem(gboolean is_mod, double a, double b)
{
    if (isnan(a) || isnan(b) || b == 0 || isinf(a)) return NAN;
    if (isinf(b)) {
        if (!is_mod) return a;
        return signbit(a) == signbit(b) ? a : NAN;
    }
    double q = a / b;
    return is_mod ? a - b * floor(q) : a - b * trunc(q);
}

static ns_css_value *
parse_calc(const char *text)
{
    static __thread int depth;
    if (depth > NS_CALC_MAX_DEPTH) return NULL;
    depth++;
    ns_css_value *v = parse_calc_inner(text);
    depth--;
    return v;
}

static const char *
ns_css_unit_suffix(int unit)
{
    switch (unit) {
    case NS_CSS_UNIT_PX:      return "px";
    case NS_CSS_UNIT_EM:      return "em";
    case NS_CSS_UNIT_REM:     return "rem";
    case NS_CSS_UNIT_PERCENT: return "%";
    case NS_CSS_UNIT_NUMBER:  return "";
    case NS_CSS_UNIT_VW:      return "vw";
    case NS_CSS_UNIT_VH:      return "vh";
    case NS_CSS_UNIT_VMIN:    return "vmin";
    case NS_CSS_UNIT_VMAX:    return "vmax";
    case NS_CSS_UNIT_EX:      return "ex";
    case NS_CSS_UNIT_CH:      return "ch";
    default:                  return "px";
    }
}

static char *
ns_css_number_str(double n)
{
    if (isnan(n)) return g_strdup("NaN");
    if (isinf(n)) return g_strdup(n < 0 ? "-infinity" : "infinity");
    return g_strdup_printf("%g", n);
}

static gboolean
ns_value_has_relative_unit(const char *s)
{
    static const char *const rel[] = {
        "em", "rem", "ex", "rex", "ch", "rch", "cap", "rcap", "ic", "ric",
        "lh", "rlh", "vw", "vh", "vi", "vb", "vmin", "vmax",
        "svw", "svh", "svmin", "svmax", "lvw", "lvh", "lvmin", "lvmax",
        "dvw", "dvh", "dvmin", "dvmax",
        "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax",
    };
    while (*s) {
        if (g_ascii_isalpha(*s)) {
            const char *start = s;
            while (g_ascii_isalpha(*s) || *s == '-') s++;
            gsize len = (gsize)(s - start);
            if (*s == '(') continue;
            for (gsize i = 0; i < G_N_ELEMENTS(rel); i++)
                if (strlen(rel[i]) == len &&
                    g_ascii_strncasecmp(start, rel[i], len) == 0)
                    return TRUE;
        } else {
            s++;
        }
    }
    return FALSE;
}

static char *
serialize_nonfinite_length(double v, const char *unit)
{
    const char *nf = isnan(v) ? "NaN" : v < 0 ? "-infinity" : "infinity";
    if (!unit || !*unit)
        return g_strdup_printf("calc(%s)", nf);
    return g_strdup_printf("calc(%s * 1%s)", nf, unit);
}

char *
ns_css_math_canonical(const char *value)
{
    if (!value) return NULL;
    while (*value && is_ws(*value)) value++;
    if (ns_value_has_relative_unit(value)) return NULL;
    /* Only functions whose result parse_calc resolves to a single number or
       absolute length are canonicalized. A value carrying a percentage is
       only canonicalized when it resolves to a single non-finite component
       (calc(NaN * 1%), calc(infinity * 1px)); a percentage that stays finite
       — including a mixed comparison such as min(20px, 10%) — is left as
       authored, since resolving it needs layout. The arc functions return
       angles parse_calc reports as bare numbers, so they are left as
       authored. */
    static const char *const fns[] = {
        "calc(", "min(", "max(", "clamp(", "round(", "mod(", "rem(",
        "abs(", "hypot(", "pow(", "sqrt(", "sin(", "cos(", "tan(",
        "sign(", "exp(", "log(", "progress(",
    };
    gboolean is_math = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(fns); i++)
        if (g_ascii_strncasecmp(value, fns[i], strlen(fns[i])) == 0) {
            is_math = TRUE;
            break;
        }
    if (!is_math) return NULL;
    gboolean has_pct = strchr(value, '%') != NULL;
    ns_css_value *v = parse_calc(value);
    if (!v) return NULL;
    char *out = NULL;
    gboolean nonfinite = FALSE;
    gboolean number_result = v->kind == NS_CSS_V_LENGTH &&
                             v->u.length.unit == NS_CSS_UNIT_NUMBER &&
                             g_ascii_strncasecmp(value, "progress(", 9) == 0;
    if (v->kind == NS_CSS_V_LENGTH) {
        double lv = v->u.length.v;
        const char *suf = ns_css_unit_suffix(v->u.length.unit);
        if (!isfinite(lv)) {
            out = serialize_nonfinite_length(lv, suf);
            nonfinite = TRUE;
        } else {
            char *num = ns_css_number_str(lv);
            out = g_strdup_printf("calc(%s%s)", num, suf);
            g_free(num);
        }
    } else if (v->kind == NS_CSS_V_CALC) {
        int nonzero = 0;
        double val = 0;
        const char *unit = "px";
        if (v->u.calc.px != 0)  { nonzero++; val = v->u.calc.px;  unit = "px"; }
        if (v->u.calc.pct != 0) { nonzero++; val = v->u.calc.pct; unit = "%"; }
        if (v->u.calc.em != 0)  { nonzero++; val = v->u.calc.em;  unit = "em"; }
        if (v->u.calc.rem != 0) { nonzero++; val = v->u.calc.rem; unit = "rem"; }
        if (nonzero == 0) {
            out = g_strdup("calc(0px)");
        } else if (nonzero == 1) {
            if (!isfinite(val)) {
                out = serialize_nonfinite_length(val, unit);
                nonfinite = TRUE;
            } else {
                char *num = ns_css_number_str(val);
                out = g_strdup_printf("calc(%s%s)", num, unit);
                g_free(num);
            }
        }
    }
    ns_css_value_free(v);
    if (has_pct && !nonfinite && !number_result) {
        g_free(out);
        out = NULL;
    }
    return out;
}

static ns_css_value *
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
    else if (g_ascii_strncasecmp(text, "hypot(", 6) == 0) { fn = 8; args = text + 6; }
    else if (g_ascii_strncasecmp(text, "pow(",   4) == 0) { fn = 9; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "sqrt(",  5) == 0) { fn = 10; args = text + 5; }
    else if (g_ascii_strncasecmp(text, "sin(",   4) == 0) { fn = 11; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "cos(",   4) == 0) { fn = 12; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "tan(",   4) == 0) { fn = 13; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "atan2(", 6) == 0) { fn = 14; args = text + 6; }
    else if (g_ascii_strncasecmp(text, "atan(",  5) == 0) { fn = 15; args = text + 5; }
    else if (g_ascii_strncasecmp(text, "asin(",  5) == 0) { fn = 16; args = text + 5; }
    else if (g_ascii_strncasecmp(text, "acos(",  5) == 0) { fn = 17; args = text + 5; }
    else if (g_ascii_strncasecmp(text, "sign(",  5) == 0) { fn = 18; args = text + 5; }
    else if (g_ascii_strncasecmp(text, "exp(",   4) == 0) { fn = 19; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "log(",   4) == 0) { fn = 20; args = text + 4; }
    else if (g_ascii_strncasecmp(text, "progress(", 9) == 0) { fn = 21; args = text + 9; }
    else return NULL;
    const char *body_end = match_close_paren(args, args + strlen(args));
    if (!body_end) return NULL;
    for (const char *tail = body_end + 1; *tail; tail++)
        if (!is_ws(*tail)) return NULL;
    if (fn >= 4) {
        char *parts[4] = {0};
        int n = calc_split_args(args, body_end, parts, G_N_ELEMENTS(parts));
        ns_css_value *out = NULL;
        if (fn == 7 && n == 1) {
            double x = 0;
            if (calc_arg_key(parts[0], &x))
                out = calc_arg_is_number(parts[0])
                    ? calc_num_value(fabs(x)) : calc_px_value(fabs(x));
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
                    (vi + 1 >= n || calc_arg_key(parts[vi + 1], &step))) {
                    double r = css_round_step(strategy, x, step);
                    gboolean numeric = calc_arg_is_number(parts[vi]) &&
                        (vi + 1 >= n || calc_arg_is_number(parts[vi + 1]));
                    out = numeric ? calc_num_value(r) : calc_px_value(r);
                }
            }
        } else if ((fn == 5 || fn == 6) && n == 2) {
            double x = 0, y = 0;
            if (calc_arg_key(parts[0], &x) && calc_arg_key(parts[1], &y)) {
                double r = css_mod_rem(fn == 5, x, y);
                out = (calc_arg_is_number(parts[0]) &&
                       calc_arg_is_number(parts[1]))
                    ? calc_num_value(r) : calc_px_value(r);
            }
        } else if (fn == 8 && n >= 1) {
            double sum = 0;
            gboolean ok = TRUE;
            for (int i = 0; i < n && ok; i++) {
                double x = 0;
                ok = calc_arg_key(parts[i], &x);
                sum += x * x;
            }
            if (ok) out = calc_num_value(sqrt(sum));
        } else if (fn == 9 && n == 2) {
            double x = 0, y = 0;
            if (calc_arg_key(parts[0], &x) && calc_arg_key(parts[1], &y))
                out = calc_num_value(pow(x, y));
        } else if (fn == 10 && n == 1) {
            double x = 0;
            if (calc_arg_key(parts[0], &x))
                out = calc_num_value(sqrt(x));
        } else if (fn >= 11 && fn <= 13 && n == 1) {
            char *rad = angle_expr_rewrite(parts[0], TRUE);
            double x = 0;
            gboolean ok = calc_arg_key(rad, &x);
            g_free(rad);
            if (ok)
                out = calc_num_value(fn == 11 ? sin(x)
                                  : fn == 12 ? cos(x) : tan(x));
        } else if (fn == 14 && n == 2) {
            double y = 0, x = 0;
            if (calc_arg_key(parts[0], &y) && calc_arg_key(parts[1], &x))
                out = calc_num_value(atan2(y, x));
        } else if (fn >= 15 && fn <= 17 && n == 1) {
            double x = 0;
            if (calc_arg_key(parts[0], &x))
                out = calc_num_value(fn == 15 ? atan(x)
                                  : fn == 16 ? asin(x) : acos(x));
        } else if (fn == 18 && n == 1) {
            double x = 0;
            if (calc_arg_key(parts[0], &x))
                out = calc_num_value(isnan(x) ? NAN : x > 0 ? 1 : x < 0 ? -1 : x);
            else if (calc_token_sign(parts[0], &x))
                out = calc_num_value(x);
        } else if (fn == 19 && n == 1) {
            double x = 0;
            if (calc_arg_key(parts[0], &x))
                out = calc_num_value(exp(x));
        } else if (fn == 20 && n >= 1) {
            double x = 0, base = 0;
            if (calc_arg_key(parts[0], &x)) {
                if (n >= 2 && calc_arg_key(parts[1], &base))
                    out = calc_num_value(log(x) / log(base));
                else if (n == 1)
                    out = calc_num_value(log(x));
            }
        } else if (fn == 21 && n == 3) {
            const char *a = parts[0];
            while (*a && is_ws(*a)) a++;
            gboolean no_clamp = FALSE;
            if (g_ascii_strncasecmp(a, "no-clamp", 8) == 0 &&
                (a[8] == '\0' || is_ws(a[8]))) {
                no_clamp = TRUE;
                a += 8;
                while (*a && is_ws(*a)) a++;
            }
            double A = 0, B = 0, C = 0;
            ns_prog_type ta = progress_operand(a, &A);
            ns_prog_type tb = progress_operand(parts[1], &B);
            ns_prog_type tc = progress_operand(parts[2], &C);
            if (ta != PT_INVALID && ta == tb && tb == tc) {
                double den = C - B;
                double num = A - B;
                double p;
                if (den == 0) {
                    p = !no_clamp ? 0.0
                      : num > 0 ? INFINITY : num < 0 ? -INFINITY : NAN;
                } else {
                    p = num / den;
                    if (!no_clamp)
                        p = isnan(p) ? 0.0 : p < 0 ? 0.0 : p > 1 ? 1.0 : p;
                }
                out = calc_num_value(p);
            }
        }
        for (int i = 0; i < n; i++) g_free(parts[i]);
        return out;
    }
    if (fn != 0) {
        double values_px[8] = {0};
        double values_pct[8] = {0};
        gboolean is_none[8] = {0};
        int num_count = 0;
        int none_count = 0;
        gboolean ok = TRUE;
        int n = 0;
        const char *seg = args;
        int depth = 0;
        for (const char *q = args; q <= body_end; q++) {
            if (q < body_end && *q == '(') depth++;
            else if (q < body_end && *q == ')') depth--;
            if (q == body_end || (*q == ',' && depth == 0)) {
                int slot = n < 8 ? n : 7;
                char *part = g_strndup(seg, (gsize)(q - seg));
                g_strstrip(part);
                if (g_ascii_strcasecmp(part, "none") == 0) {
                    is_none[slot] = TRUE;
                    none_count++;
                    if (fn != 3) ok = FALSE;
                } else if (!resolve_to_px_pct(part, strlen(part),
                                              &values_px[slot],
                                              &values_pct[slot])) {
                    ok = FALSE;
                } else if (calc_arg_is_number(part)) {
                    num_count++;
                }
                g_free(part);
                n++;
                seg = q + 1;
            }
        }
        if (n == 0 || !ok) return NULL;
        int non_none = n - none_count;
        if (num_count != 0 && num_count != non_none) return NULL;
        if (fn == 3 && (n != 3 || is_none[1])) return NULL;
        gboolean all_numbers = non_none > 0 && num_count == non_none;
        if (n > 8) n = 8;
        double keys[8] = {0};
        for (int i = 0; i < n; i++)
            keys[i] = values_px[i] + values_pct[i] * 0.01 * g_viewport_w;
        double out_px;
        if (fn == 3) {
            double min_v = is_none[0] ? -HUGE_VAL : keys[0];
            double val_v = keys[1];
            double max_v = is_none[2] ? HUGE_VAL : keys[2];
            if (isnan(min_v) || isnan(val_v) || isnan(max_v)) {
                out_px = NAN;
            } else {
                out_px = val_v;
                if (out_px > max_v) out_px = max_v;
                if (out_px < min_v) out_px = min_v;
            }
        } else {
            out_px = keys[0];
            gboolean any_nan = isnan(keys[0]);
            for (int i = 1; i < n; i++) {
                if (isnan(keys[i])) any_nan = TRUE;
                if (fn == 1 && keys[i] < out_px) out_px = keys[i];
                if (fn == 2 && keys[i] > out_px) out_px = keys[i];
            }
            if (any_nan) out_px = NAN;
        }
        return all_numbers ? calc_num_value(out_px) : calc_px_value(out_px);
    }
    text = args;
    const char *end = body_end;
    double pct = 0;
    double px  = 0;
    double em  = 0;
    double rem = 0;
    const char *p = text;
    ns_calc_term term;
    gboolean parsed = FALSE;
    if (calc_expr_parse(&p, end, &term, 0)) {
        calc_skip_ws(&p, end);
        if (p == end) {
            if (term.is_number) return calc_num_value(term.num);
            calc_term_lengthify(&term);
            px = term.px;
            pct = term.pct;
            em = term.em;
            rem = term.rem;
            parsed = TRUE;
        }
    }
    if (!parsed) return NULL;
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_CALC;
    v->u.calc.pct = pct;
    v->u.calc.px  = px;
    v->u.calc.em  = em;
    v->u.calc.rem = rem;
    return v;
}

static int split_ws(const char *s, char *out[4]);

static gboolean
parse_bg_size_component(const char *tok, double *out_v, ns_css_unit *out_unit)
{
    if (parse_length(tok, out_v, out_unit)) return TRUE;
    ns_css_value *cv = parse_calc(tok);
    if (!cv) return FALSE;
    gboolean ok = TRUE;
    if (cv->kind == NS_CSS_V_LENGTH) {
        *out_v = cv->u.length.v;
        *out_unit = cv->u.length.unit;
    } else if (cv->kind == NS_CSS_V_CALC) {
        if (cv->u.calc.pct != 0 && cv->u.calc.px == 0 &&
            cv->u.calc.em == 0 && cv->u.calc.rem == 0) {
            *out_v = cv->u.calc.pct;
            *out_unit = NS_CSS_UNIT_PERCENT;
        } else {
            *out_v = cv->u.calc.px +
                     (cv->u.calc.em + cv->u.calc.rem) * 16.0;
            *out_unit = NS_CSS_UNIT_PX;
        }
    } else {
        ok = FALSE;
    }
    ns_css_value_free(cv);
    return ok;
}

static gboolean
parse_track_token(const char *tok, ns_css_track *out)
{
    if (!tok || !*tok) return FALSE;
    if (g_ascii_strcasecmp(tok, "auto") == 0 ||
        g_ascii_strcasecmp(tok, "min-content") == 0 ||
        g_ascii_strcasecmp(tok, "max-content") == 0) {
        out->kind = NS_CSS_TRACK_AUTO;
        out->v = 0;
        return TRUE;
    }
    char *endp = NULL;
    double v = g_ascii_strtod(tok, &endp);
    if (!endp || endp == tok) return FALSE;
    if (*endp == '\0' || g_ascii_strcasecmp(endp, "px") == 0) {
        out->kind = NS_CSS_TRACK_PX; out->v = v; return TRUE;
    }
    if (*endp == '%') { out->kind = NS_CSS_TRACK_PERCENT; out->v = v; return TRUE; }
    if (g_ascii_strcasecmp(endp, "fr") == 0) {
        out->kind = NS_CSS_TRACK_FR; out->v = v; return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "em") == 0) {
        out->kind = NS_CSS_TRACK_PX; out->v = v * 16; return TRUE;
    }
    if (g_ascii_strcasecmp(endp, "rem") == 0) {
        out->kind = NS_CSS_TRACK_PX; out->v = v * 16; return TRUE;
    }
    return FALSE;
}

#define NS_CSS_TRACK_MAX_DEPTH 32

static gboolean parse_one_track(const char *start, gsize len,
                                ns_css_track *out);
static gboolean parse_one_track_depth(const char *start, gsize len,
                                      ns_css_track *out, int depth);

static gboolean
parse_math_track(const char *start, gsize len, ns_css_track *out)
{
    char *tok = g_strndup(start, len);
    ns_css_value *cv = parse_calc(tok);
    g_free(tok);
    if (!cv) return FALSE;
    gboolean ok = FALSE;
    if (cv->kind == NS_CSS_V_LENGTH &&
        (cv->u.length.unit == NS_CSS_UNIT_PX ||
         cv->u.length.unit == NS_CSS_UNIT_NUMBER)) {
        out->kind = NS_CSS_TRACK_PX;
        out->v = cv->u.length.v;
        ok = TRUE;
    } else if (cv->kind == NS_CSS_V_CALC) {
        if (cv->u.calc.pct != 0 && cv->u.calc.px == 0 &&
            cv->u.calc.em == 0 && cv->u.calc.rem == 0) {
            out->kind = NS_CSS_TRACK_PERCENT;
            out->v = cv->u.calc.pct;
        } else {
            out->kind = NS_CSS_TRACK_PX;
            out->v = cv->u.calc.px +
                     (cv->u.calc.em + cv->u.calc.rem) * 16.0 +
                     cv->u.calc.pct / 100.0 * g_viewport_w;
        }
        ok = TRUE;
    }
    ns_css_value_free(cv);
    return ok;
}

static gboolean
parse_minmax_token(const char *body, gsize len, ns_css_track *out, int depth)
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
    ns_css_track mn = {0}, mx = {0};
    gboolean ok = parse_one_track_depth(as, alen, &mn, depth) &&
                  parse_one_track_depth(bs, blen, &mx, depth);
    if (!ok) return FALSE;
    *out = mx;
    out->min_kind = mn.kind;
    out->min_v    = mn.v;
    out->has_min  = TRUE;
    return TRUE;
}

static gboolean
parse_one_track_depth(const char *start, gsize len, ns_css_track *out, int depth)
{
    if (depth >= NS_CSS_TRACK_MAX_DEPTH) return FALSE;
    while (len > 0 && is_ws(*start)) { start++; len--; }
    while (len > 0 && is_ws(start[len - 1])) len--;
    if (len == 0) return FALSE;
    if (len > 7 && g_ascii_strncasecmp(start, "minmax(", 7) == 0 &&
        start[len - 1] == ')') {
        return parse_minmax_token(start + 7, len - 8, out, depth + 1);
    }
    if (len > 12 && g_ascii_strncasecmp(start, "fit-content(", 12) == 0 &&
        start[len - 1] == ')') {
        ns_css_track inner = {0};
        if (!parse_one_track_depth(start + 12, len - 13, &inner, depth + 1))
            return FALSE;
        *out = inner;
        out->min_kind = NS_CSS_TRACK_AUTO;
        out->min_v    = 0;
        out->has_min  = TRUE;
        return TRUE;
    }
    if (start[len - 1] == ')' &&
        (g_ascii_strncasecmp(start, "min(", 4) == 0 ||
         g_ascii_strncasecmp(start, "max(", 4) == 0 ||
         g_ascii_strncasecmp(start, "clamp(", 6) == 0 ||
         g_ascii_strncasecmp(start, "calc(", 5) == 0))
        return parse_math_track(start, len, out);
    char *tok = g_strndup(start, len);
    gboolean ok = parse_track_token(tok, out);
    g_free(tok);
    return ok;
}

static gboolean
parse_one_track(const char *start, gsize len, ns_css_track *out)
{
    return parse_one_track_depth(start, len, out, 0);
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

static ns_css_value *
parse_tracks(const char *text)
{
    if (!text || !*text) return NULL;
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_TRACKS;
    const char *p = text;
    const char *full_end = text + strlen(text);
    while (p < full_end && is_ws(*p)) p++;
    if (g_ascii_strncasecmp(p, "subgrid", 7) == 0 &&
        (p + 7 >= full_end || is_ws(p[7]) || p[7] == '[')) {
        v->u.tracks.subgrid = TRUE;
        return v;
    }
    while (p < full_end && v->u.tracks.n < NS_CSS_TRACKS_MAX) {
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
            ns_css_auto_repeat ar = NS_CSS_AUTO_REPEAT_NONE;
            long n = 0;
            if (count_len == 8 && g_ascii_strncasecmp(count_s, "auto-fit", 8) == 0)
                ar = NS_CSS_AUTO_REPEAT_FIT;
            else if (count_len == 9 && g_ascii_strncasecmp(count_s, "auto-fill", 9) == 0)
                ar = NS_CSS_AUTO_REPEAT_FILL;
            else {
                char *cstr = g_strndup(count_s, count_len);
                n = strtol(cstr, NULL, 10);
                g_free(cstr);
                if (n <= 0) continue;
                if (n > NS_CSS_TRACKS_MAX) n = NS_CSS_TRACKS_MAX;
            }
            const char *tstarts[16];
            gsize tlens[16];
            int nb = split_tracks_top(body, body_len, tstarts, tlens, 16);
            if (ar != NS_CSS_AUTO_REPEAT_NONE) {
                if (v->u.tracks.auto_repeat == NS_CSS_AUTO_REPEAT_NONE) {
                    v->u.tracks.auto_repeat = ar;
                    v->u.tracks.auto_repeat_start = v->u.tracks.n;
                    int cnt = 0;
                    for (int i = 0; i < nb && v->u.tracks.n < NS_CSS_TRACKS_MAX; i++) {
                        ns_css_track t = {0};
                        if (parse_one_track(tstarts[i], tlens[i], &t)) {
                            v->u.tracks.tracks[v->u.tracks.n++] = t;
                            cnt++;
                        }
                    }
                    v->u.tracks.auto_repeat_count = cnt;
                }
                continue;
            }
            for (long r = 0; r < n && v->u.tracks.n < NS_CSS_TRACKS_MAX; r++) {
                for (int i = 0; i < nb && v->u.tracks.n < NS_CSS_TRACKS_MAX; i++) {
                    ns_css_track t = {0};
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
        ns_css_track t = {0};
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

static ns_css_value *
parse_areas(const char *text)
{
    if (!text || !*text) return NULL;
    char *grid[NS_CSS_TRACKS_MAX][NS_CSS_TRACKS_MAX] = {{0}};
    int rows = 0;
    int cols = -1;
    const char *p = text;
    while (*p && rows < NS_CSS_TRACKS_MAX) {
        while (*p && is_ws(*p)) p++;
        if (!*p) break;
        if (*p != '"' && *p != '\'') {
            for (int r = 0; r < rows; r++)
                for (int k = 0; k < NS_CSS_TRACKS_MAX; k++)
                    g_free(grid[r][k]);
            return NULL;
        }
        const char *row_start = p;
        char *row = read_css_string(&p, p + strlen(p));
        if (p == row_start) {
            g_free(row);
            for (int r = 0; r < rows; r++)
                for (int k = 0; k < NS_CSS_TRACKS_MAX; k++)
                    g_free(grid[r][k]);
            return NULL;
        }
        char **toks = g_strsplit_set(row, " \t\r\n", -1);
        int c = 0;
        for (int i = 0; toks[i]; i++) {
            if (!*toks[i]) continue;
            if (c >= NS_CSS_TRACKS_MAX) break;
            grid[rows][c++] = g_strdup(toks[i]);
        }
        g_strfreev(toks);
        g_free(row);
        if (cols < 0) cols = c;
        else if (c != cols) {
            for (int r = 0; r <= rows; r++)
                for (int k = 0; k < NS_CSS_TRACKS_MAX; k++)
                    g_free(grid[r][k]);
            return NULL;
        }
        rows++;
    }
    if (rows == 0 || cols <= 0) {
        for (int r = 0; r < rows; r++)
            for (int k = 0; k < NS_CSS_TRACKS_MAX; k++)
                g_free(grid[r][k]);
        return NULL;
    }
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_AREAS;
    v->u.areas.n_rows = rows;
    v->u.areas.n_cols = cols;
    gboolean used[NS_CSS_TRACKS_MAX][NS_CSS_TRACKS_MAX] = {{0}};
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
            if (v->u.areas.n_rects < NS_CSS_AREAS_MAX) {
                ns_css_area_rect *rect = &v->u.areas.rects[v->u.areas.n_rects++];
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
        for (int k = 0; k < NS_CSS_TRACKS_MAX; k++)
            g_free(grid[r][k]);
    return v;
}

static gboolean
parse_one_shadow(const char *text, ns_css_shadow *out)
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
        ns_css_unit u;
        if (parse_color(tok, &r, &g, &b, &a)) {
            cr = r; cg = g; cb = b; ca = a; has_color = TRUE;
        } else if (parse_length(tok, &num, &u) && n_lens < 4) {
            if (u == NS_CSS_UNIT_EM)   num *= 16;
            if (u == NS_CSS_UNIT_REM)  num *= 16;
            if (u == NS_CSS_UNIT_EX || u == NS_CSS_UNIT_CH) num *= 8;
            if (u == NS_CSS_UNIT_CAP)  num *= 11.2;
            if (u == NS_CSS_UNIT_IC)   num *= 16;
            lens[n_lens++] = num;
        } else if (g_ascii_strcasecmp(tok, "inset") == 0) {
            inset = TRUE;
        }
        g_free(tok);
    }
    if (n_lens < 2) return FALSE;
    out->x = lens[0];
    out->y = lens[1];
    out->blur   = n_lens >= 3 ? CLAMP(lens[2], 0.0, 1000.0) : 0;
    out->spread = n_lens >= 4 ? CLAMP(lens[3], -1000.0, 1000.0) : 0;
    out->r = cr; out->g = cg; out->b = cb;
    out->a = has_color ? ca : 128;
    out->inset = inset;
    return TRUE;
}

static ns_css_value *
parse_box_shadow(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (!*text || g_ascii_strncasecmp(text, "none", 4) == 0) return NULL;
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_SHADOW;
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
            if (v->u.shadow.n < NS_CSS_SHADOWS_MAX) {
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
gradient_add_stop(ns_css_gradient *gr, guint8 r, guint8 g, guint8 b, guint8 a,
                  gboolean has_pos, gboolean is_px, double pos)
{
    if (gr->n_stops >= NS_CSS_GRADIENT_STOPS_MAX) return;
    ns_css_gradient_stop *s = &gr->stops[gr->n_stops++];
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
parse_gradient_stop_seg(ns_css_gradient *gr, const char *seg)
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

static ns_css_value *
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

    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_GRADIENT;
    v->u.gradient.angle_deg = angle;
    v->u.gradient.n_stops = 0;
    for (guint i = (guint)start_i;
         i < parts->len && v->u.gradient.n_stops < NS_CSS_GRADIENT_STOPS_MAX;
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

static ns_css_value *
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

    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_GRADIENT;
    v->u.gradient.angle_deg = 0;
    v->u.gradient.radial = TRUE;
    v->u.gradient.n_stops = 0;
    v->u.gradient.center_x = 0.5;
    v->u.gradient.center_y = 0.5;
    if (start_i == 1 && parts->len > 0)
        parse_gradient_at(parts->pdata[0], &v->u.gradient.center_x,
                          &v->u.gradient.center_y, &v->u.gradient.has_center);
    for (guint i = (guint)start_i;
         i < parts->len && v->u.gradient.n_stops < NS_CSS_GRADIENT_STOPS_MAX;
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

static ns_css_value *
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
        } else {
            char *ftoks[4] = {0};
            int fnt = split_ws(first, ftoks);
            gboolean first_is_color = fnt >= 1 &&
                parse_color(ftoks[0], &dummy_r, &dummy_g, &dummy_b, &dummy_a);
            for (int k = 0; k < fnt; k++) g_free(ftoks[k]);
            if (!first_is_color) start_i = 1;
        }
    }

    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_GRADIENT;
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
         i < parts->len && v->u.gradient.n_stops < NS_CSS_GRADIENT_STOPS_MAX;
         i++) {
        const char *stop_text = parts->pdata[i];
        char *tokens[4] = {0};
        int nt = split_ws(stop_text, tokens);
        if (nt < 1) { for (int k = 0; k < nt; k++) g_free(tokens[k]); continue; }
        guint8 r, g, b, a;
        if (parse_color(tokens[0], &r, &g, &b, &a)) {
            ns_css_gradient_stop *s =
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
    ns_css_gradient_stop *st = v->u.gradient.stops;
    if (!st[0].has_pos) { st[0].pos = 0.0; st[0].has_pos = TRUE; }
    if (!st[ns - 1].has_pos) { st[ns - 1].pos = 1.0; st[ns - 1].has_pos = TRUE; }
    for (int i = 1; i < ns; i++)
        if (st[i].has_pos && st[i].pos < st[i - 1].pos)
            st[i].pos = st[i - 1].pos;
    for (int i = 0; i < ns; ) {
        if (st[i].has_pos) { i++; continue; }
        int j = i;
        while (j < ns && !st[j].has_pos) j++;
        double lo = st[i - 1].pos;
        double hi = st[j].pos;
        for (int k = i; k < j; k++)
            st[k].pos = lo + (hi - lo) * (double)(k - i + 1) / (double)(j - i + 1);
        i = j;
    }
    return v;
}

static const char *
css_quoted_end(const char *u, char quote)
{
    const char *p = u;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == quote) return p;
        p++;
    }
    return NULL;
}

static char *
css_unescape_url(const char *u, gsize len)
{
    GString *out = g_string_sized_new(len);
    for (gsize i = 0; i < len; i++) {
        if (u[i] == '\\' && i + 1 < len) i++;
        g_string_append_c(out, u[i]);
    }
    return g_string_free(out, FALSE);
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
            if (q) end = css_quoted_end(u, q);
            else { end = u; while (*end && *end != ')' && !is_ws(*end)) end++; }
            if (end && end > u) url = css_unescape_url(u, (gsize)(end - u));
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

static ns_css_value *
parse_any_gradient(const char *t)
{
    const char *p = t;
    while (*p && is_ws(*p)) p++;
    gboolean rep = g_ascii_strncasecmp(p, "repeating-", 10) == 0;
    const char *g = rep ? p + 10 : p;
    ns_css_value *v = parse_linear_gradient(g);
    if (!v) v = parse_radial_gradient(g);
    if (!v) v = parse_conic_gradient(g);
    if (v && v->kind == NS_CSS_V_GRADIENT) v->u.gradient.repeating = rep;
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
    double px = 0, pct = 0;
    if (resolve_to_px_pct(s, strlen(s), &px, &pct)) {
        if (pct != 0 && px == 0) {
            *out = pct;
            *is_percent = TRUE;
        } else {
            *out = px;
            *is_percent = FALSE;
        }
        return TRUE;
    }
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (!end || end == s) return FALSE;
    while (*end && is_ws(*end)) end++;
    *out = v;
    *is_percent = (*end == '%');
    return TRUE;
}

static char *
angle_expr_rewrite(const char *s, gboolean to_radians)
{
    GString *out = g_string_new(NULL);
    const char *p = s;
    while (*p) {
        gboolean num_start = g_ascii_isdigit(*p) ||
            (*p == '.' && g_ascii_isdigit(p[1]));
        if (num_start) {
            char *end = NULL;
            double v = g_ascii_strtod(p, &end);
            const char *u = end;
            while (*u && g_ascii_isalpha(*u)) u++;
            gsize ul = (gsize)(u - end);
            double deg = v;
            gboolean is_angle = TRUE;
            if      (ul == 3 && g_ascii_strncasecmp(end, "deg",  3) == 0) deg = v;
            else if (ul == 4 && g_ascii_strncasecmp(end, "grad", 4) == 0) deg = v * 0.9;
            else if (ul == 3 && g_ascii_strncasecmp(end, "rad",  3) == 0) deg = v * 180.0 / G_PI;
            else if (ul == 4 && g_ascii_strncasecmp(end, "turn", 4) == 0) deg = v * 360.0;
            else is_angle = FALSE;
            if (is_angle) {
                g_string_append_printf(out, "%.17g",
                                       to_radians ? deg * G_PI / 180.0 : deg);
                p = u;
            } else {
                g_string_append_len(out, p, (gssize)(u - p));
                p = u;
            }
            continue;
        }
        g_string_append_c(out, *p++);
    }
    return g_string_free(out, FALSE);
}

static gboolean
parse_angle_any(const char *s, double *deg_out)
{
    if (!s) return FALSE;
    while (*s && is_ws(*s)) s++;
    if (!*s) return FALSE;
    if (g_ascii_strncasecmp(s, "atan2(", 6) == 0 ||
        g_ascii_strncasecmp(s, "atan(", 5) == 0 ||
        g_ascii_strncasecmp(s, "asin(", 5) == 0 ||
        g_ascii_strncasecmp(s, "acos(", 5) == 0) {
        double px = 0, pct = 0;
        if (!resolve_to_px_pct(s, strlen(s), &px, &pct)) return FALSE;
        *deg_out = px * 180.0 / G_PI;
        return TRUE;
    }
    {
        static const char *const fns[] = {
            "calc(", "min(", "max(", "clamp(", "round(", "mod(", "rem(",
            "abs(", "sign(", "hypot(", "pow(", "sqrt(", "sin(", "cos(",
            "tan(", "exp(", "log(",
        };
        for (gsize i = 0; i < G_N_ELEMENTS(fns); i++) {
            if (g_ascii_strncasecmp(s, fns[i], strlen(fns[i])) != 0) continue;
            char *rw = angle_expr_rewrite(s, FALSE);
            double px = 0, pct = 0;
            gboolean ok = resolve_to_px_pct(rw, strlen(rw), &px, &pct);
            g_free(rw);
            if (!ok) return FALSE;
            *deg_out = px;
            return TRUE;
        }
    }
    char *end = NULL;
    g_ascii_strtod(s, &end);
    if (!end || end == s) return FALSE;
    *deg_out = parse_angle_deg(s);
    return TRUE;
}

static gboolean parse_scale_number(const char *s, double *out);

static ns_css_value *
parse_transform(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (!*text) return NULL;
    if (g_ascii_strncasecmp(text, "none", 4) == 0) return NULL;
    ns_css_transform tf = {0};
    const char *p = text;
    while (*p && tf.n_ops < NS_CSS_TRANSFORM_OPS_MAX) {
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

        char *targs[16] = {0};
        int nt = 0;
        char *seg = args;
        int adepth = 0;
        for (char *q = args; ; q++) {
            if (*q == '(') adepth++;
            else if (*q == ')') adepth--;
            if ((*q == ',' && adepth == 0) || *q == '\0') {
                int saved = *q;
                *q = '\0';
                if (nt < 16) {
                    char *piece = g_strdup(seg);
                    g_strstrip(piece);
                    targs[nt++] = piece;
                }
                if (saved == '\0') break;
                seg = q + 1;
            }
        }

        ns_css_transform_op *op = &tf.ops[tf.n_ops];
        gboolean accept = FALSE;
        gboolean dummy_pct = FALSE;
        if (strcmp(fn_lc, "translate") == 0 ||
            strcmp(fn_lc, "translatex") == 0 ||
            strcmp(fn_lc, "translatey") == 0 ||
            strcmp(fn_lc, "translatez") == 0) {
            op->kind = NS_CSS_TFN_TRANSLATE;
            op->a = 0; op->b = 0; op->c = 0;
            op->a_is_percent = FALSE; op->b_is_percent = FALSE;
            if (strcmp(fn_lc, "translatey") == 0) {
                if (nt >= 1) parse_transform_len(targs[0], &op->b, &op->b_is_percent);
            } else if (strcmp(fn_lc, "translatez") == 0) {
                if (nt >= 1) parse_transform_len(targs[0], &op->c, &dummy_pct);
            } else {
                if (nt >= 1) parse_transform_len(targs[0], &op->a, &op->a_is_percent);
                if (nt >= 2) parse_transform_len(targs[1], &op->b, &op->b_is_percent);
            }
            accept = TRUE;
        } else if (strcmp(fn_lc, "rotate") == 0 ||
                   strcmp(fn_lc, "rotatez") == 0) {
            op->kind = NS_CSS_TFN_ROTATE;
            op->a = 0;
            op->b = 0;
            accept = nt == 1 && parse_angle_any(targs[0], &op->a);
        } else if (strcmp(fn_lc, "rotatex") == 0 ||
                   strcmp(fn_lc, "rotatey") == 0) {
            op->kind = NS_CSS_TFN_ROTATE3D;
            op->a = strcmp(fn_lc, "rotatex") == 0 ? 1 : 0;
            op->b = strcmp(fn_lc, "rotatey") == 0 ? 1 : 0;
            op->c = 0;
            op->d = 0;
            accept = nt == 1 && parse_angle_any(targs[0], &op->d);
        } else if (strcmp(fn_lc, "rotate3d") == 0 && nt == 4) {
            op->kind = NS_CSS_TFN_ROTATE3D;
            op->a = g_ascii_strtod(targs[0], NULL);
            op->b = g_ascii_strtod(targs[1], NULL);
            op->c = g_ascii_strtod(targs[2], NULL);
            op->d = 0;
            accept = parse_angle_any(targs[3], &op->d);
        } else if (strcmp(fn_lc, "perspective") == 0 && nt >= 1) {
            op->kind = NS_CSS_TFN_PERSPECTIVE;
            op->a = 0;
            parse_transform_len(targs[0], &op->a, &dummy_pct);
            accept = TRUE;
        } else if (strcmp(fn_lc, "scale") == 0 ||
                   strcmp(fn_lc, "scalex") == 0 ||
                   strcmp(fn_lc, "scaley") == 0 ||
                   strcmp(fn_lc, "scalez") == 0) {
            op->kind = NS_CSS_TFN_SCALE;
            double sa = 1, sb;
            if (nt >= 1 && !parse_scale_number(targs[0], &sa)) sa = 0;
            sb = sa;
            if (nt >= 2 && !parse_scale_number(targs[1], &sb)) sb = 0;
            op->c = 1;
            if (strcmp(fn_lc, "scalex") == 0) { op->a = sa; op->b = 1; }
            else if (strcmp(fn_lc, "scaley") == 0) { op->a = 1; op->b = sa; }
            else if (strcmp(fn_lc, "scalez") == 0) { op->a = 1; op->b = 1; op->c = sa; }
            else { op->a = sa; op->b = sb; }
            accept = TRUE;
        } else if (strcmp(fn_lc, "skew") == 0 ||
                   strcmp(fn_lc, "skewx") == 0 ||
                   strcmp(fn_lc, "skewy") == 0) {
            op->kind = NS_CSS_TFN_SKEW;
            double aa = 0, bb = 0;
            if (nt >= 1) parse_angle_any(targs[0], &aa);
            if (nt >= 2) parse_angle_any(targs[1], &bb);
            if (strcmp(fn_lc, "skewx") == 0) { op->a = aa; op->b = 0; }
            else if (strcmp(fn_lc, "skewy") == 0) { op->a = 0; op->b = aa; }
            else { op->a = aa; op->b = bb; }
            accept = TRUE;
        } else if (strcmp(fn_lc, "matrix") == 0 && nt == 6) {
            op->kind = NS_CSS_TFN_MATRIX;
            op->a = g_ascii_strtod(targs[0], NULL);
            op->b = g_ascii_strtod(targs[1], NULL);
            op->c = g_ascii_strtod(targs[2], NULL);
            op->d = g_ascii_strtod(targs[3], NULL);
            op->e = g_ascii_strtod(targs[4], NULL);
            op->f = g_ascii_strtod(targs[5], NULL);
            accept = TRUE;
        } else if (strcmp(fn_lc, "matrix3d") == 0 && nt == 16) {
            op->kind = NS_CSS_TFN_MATRIX3D;
            for (int k = 0; k < 16; k++)
                op->m3d[k] = g_ascii_strtod(targs[k], NULL);
            accept = TRUE;
        } else if ((strcmp(fn_lc, "translate3d") == 0 && nt >= 2)) {
            op->kind = NS_CSS_TFN_TRANSLATE;
            op->a = 0; op->b = 0; op->c = 0;
            op->a_is_percent = FALSE; op->b_is_percent = FALSE;
            parse_transform_len(targs[0], &op->a, &op->a_is_percent);
            parse_transform_len(targs[1], &op->b, &op->b_is_percent);
            if (nt >= 3) parse_transform_len(targs[2], &op->c, &dummy_pct);
            accept = TRUE;
        } else if (strcmp(fn_lc, "scale3d") == 0 && nt >= 2) {
            op->kind = NS_CSS_TFN_SCALE;
            op->a = 0; op->b = 0; op->c = 1;
            parse_scale_number(targs[0], &op->a);
            parse_scale_number(targs[1], &op->b);
            if (nt >= 3) parse_scale_number(targs[2], &op->c);
            accept = TRUE;
        }
        if (accept) tf.n_ops++;
        for (int k = 0; k < 16; k++) g_free(targs[k]);
        g_free(args);
        g_free(fn_lc);
    }
    if (tf.n_ops == 0) return NULL;
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_TRANSFORM;
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

static ns_css_value *
parse_transform_origin(const char *text)
{
    if (!text || !*text) return NULL;
    char *toks[4] = {0};
    int nt = split_ws_limit(text, toks, 3);
    char *a = nt >= 1 ? toks[0] : NULL;
    char *b = nt >= 2 ? toks[1] : NULL;
    char *zc = nt >= 3 ? toks[2] : NULL;
    ns_css_transform tf;
    memset(&tf, 0, sizeof(tf));
    tf.n_ops = 1;
    ns_css_transform_op *op = &tf.ops[0];
    op->kind = NS_CSS_TFN_TRANSLATE;
    op->a = 50; op->b = 50; op->c = 0;
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
    if (zc) {
        gboolean zpct = FALSE;
        parse_transform_len(zc, &op->c, &zpct);
        if (zpct) op->c = 0;
    }
    for (int i = 0; i < nt; i++) g_free(toks[i]);
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_TRANSFORM;
    v->u.transform = tf;
    return v;
}

static ns_css_value *
transform_value_one_op(const ns_css_transform_op *op)
{
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_TRANSFORM;
    v->u.transform.n_ops = 1;
    v->u.transform.ops[0] = *op;
    return v;
}

static ns_css_value *
parse_translate_prop(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (!*text || g_ascii_strncasecmp(text, "none", 4) == 0) return NULL;
    char *toks[4] = {0};
    int nt = split_ws_limit(text, toks, 3);
    if (nt == 0) return NULL;
    ns_css_transform_op op;
    memset(&op, 0, sizeof(op));
    op.kind = NS_CSS_TFN_TRANSLATE;
    gboolean dummy = FALSE;
    parse_transform_len(toks[0], &op.a, &op.a_is_percent);
    if (nt >= 2) parse_transform_len(toks[1], &op.b, &op.b_is_percent);
    if (nt >= 3) parse_transform_len(toks[2], &op.c, &dummy);
    for (int i = 0; i < nt; i++) g_free(toks[i]);
    return transform_value_one_op(&op);
}

static ns_css_value *
parse_rotate_prop(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (!*text || g_ascii_strncasecmp(text, "none", 4) == 0) return NULL;
    char *toks[5] = {0};
    int nt = split_ws_limit(text, toks, 4);
    ns_css_value *v = NULL;
    ns_css_transform_op op;
    memset(&op, 0, sizeof(op));
    if (nt == 1) {
        op.kind = NS_CSS_TFN_ROTATE;
        if (parse_angle_any(toks[0], &op.a)) v = transform_value_one_op(&op);
    } else if (nt == 2) {
        op.kind = NS_CSS_TFN_ROTATE3D;
        char *axis = ascii_lower(toks[0], strlen(toks[0]));
        gboolean ok = TRUE;
        if      (strcmp(axis, "x") == 0) op.a = 1;
        else if (strcmp(axis, "y") == 0) op.b = 1;
        else if (strcmp(axis, "z") == 0) op.c = 1;
        else ok = FALSE;
        g_free(axis);
        if (ok && parse_angle_any(toks[1], &op.d)) {
            if (op.c == 1 && op.a == 0 && op.b == 0) {
                op.kind = NS_CSS_TFN_ROTATE;
                op.a = op.d;
                op.b = 0; op.c = 0; op.d = 0;
            }
            v = transform_value_one_op(&op);
        }
    } else if (nt == 4) {
        op.kind = NS_CSS_TFN_ROTATE3D;
        op.a = g_ascii_strtod(toks[0], NULL);
        op.b = g_ascii_strtod(toks[1], NULL);
        op.c = g_ascii_strtod(toks[2], NULL);
        if (parse_angle_any(toks[3], &op.d)) v = transform_value_one_op(&op);
    }
    for (int i = 0; i < nt; i++) g_free(toks[i]);
    return v;
}

static gboolean
parse_scale_number(const char *s, double *out)
{
    if (!s) return FALSE;
    while (*s && is_ws(*s)) s++;
    if (!*s) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (end && end != s) {
        const char *p = end;
        while (*p && is_ws(*p)) p++;
        if (*p == '\0') { *out = v; return TRUE; }
        if (*p == '%') { *out = v / 100.0; return TRUE; }
    }
    double px = 0, pct = 0;
    if (resolve_to_px_pct(s, strlen(s), &px, &pct)) {
        *out = px + pct / 100.0;
        return TRUE;
    }
    ns_css_value *cv = parse_calc(s);
    if (!cv) {
        char *w = g_strdup_printf("calc(%s)", s);
        cv = parse_calc(w);
        g_free(w);
    }
    if (cv && cv->kind == NS_CSS_V_LENGTH) {
        double val = cv->u.length.unit == NS_CSS_UNIT_PERCENT
                   ? cv->u.length.v / 100.0 : cv->u.length.v;
        ns_css_value_free(cv);
        *out = isnan(val) ? 0.0 : val;
        return TRUE;
    }
    if (cv) ns_css_value_free(cv);
    return FALSE;
}

static ns_css_value *
parse_scale_prop(const char *text)
{
    while (*text && is_ws(*text)) text++;
    if (!*text || g_ascii_strncasecmp(text, "none", 4) == 0) return NULL;
    char *toks[4] = {0};
    int nt = split_ws_limit(text, toks, 3);
    if (nt == 0) return NULL;
    ns_css_transform_op op;
    memset(&op, 0, sizeof(op));
    op.kind = NS_CSS_TFN_SCALE;
    gboolean ok = parse_scale_number(toks[0], &op.a);
    op.b = op.a;
    op.c = 1;
    if (ok && nt >= 2) ok = parse_scale_number(toks[1], &op.b);
    if (ok && nt >= 3) ok = parse_scale_number(toks[2], &op.c);
    for (int i = 0; i < nt; i++) g_free(toks[i]);
    if (!ok) return NULL;
    return transform_value_one_op(&op);
}

static void
append_scale_number(GString *s, double n)
{
    if (isnan(n)) {
        g_string_append_c(s, '0');
        return;
    }
    if (!isfinite(n)) {
        g_string_append(s, n < 0 ? "calc(-infinity)" : "calc(infinity)");
        return;
    }
    char *t = ns_css_number_str(n);
    g_string_append(s, t);
    g_free(t);
}

static void
append_transform_length(GString *s, double v, gboolean is_percent)
{
    char *t = ns_css_number_str(v);
    g_string_append(s, t);
    g_free(t);
    g_string_append(s, is_percent ? "%" : "px");
}

char *
ns_css_individual_transform_serialize(const ns_css_value *v, int prop)
{
    if (!v) return NULL;
    if (v->kind == NS_CSS_V_KEYWORD)
        return g_strdup(v->u.keyword);
    if (v->kind != NS_CSS_V_TRANSFORM || v->u.transform.n_ops < 1)
        return NULL;
    const ns_css_transform_op *op = &v->u.transform.ops[0];
    GString *s = g_string_new(NULL);
    if (prop == NS_CSS_SCALE) {
        gboolean ab_same = (op->a == op->b) || (isnan(op->a) && isnan(op->b));
        append_scale_number(s, op->a);
        if (op->c != 1) {
            g_string_append_c(s, ' ');
            append_scale_number(s, op->b);
            g_string_append_c(s, ' ');
            append_scale_number(s, op->c);
        } else if (!ab_same) {
            g_string_append_c(s, ' ');
            append_scale_number(s, op->b);
        }
    } else if (prop == NS_CSS_ROTATE) {
        if (op->kind == NS_CSS_TFN_ROTATE3D) {
            const char *axis = NULL;
            if (op->a == 1 && op->b == 0 && op->c == 0) axis = "x";
            else if (op->a == 0 && op->b == 1 && op->c == 0) axis = "y";
            else if (op->a == 0 && op->b == 0 && op->c == 1) axis = NULL;
            if (axis) {
                g_string_append(s, axis);
                g_string_append_c(s, ' ');
            } else if (!(op->a == 0 && op->b == 0 && op->c == 1)) {
                append_scale_number(s, op->a);
                g_string_append_c(s, ' ');
                append_scale_number(s, op->b);
                g_string_append_c(s, ' ');
                append_scale_number(s, op->c);
                g_string_append_c(s, ' ');
            }
            append_scale_number(s, op->d);
            g_string_append(s, "deg");
        } else {
            append_scale_number(s, op->a);
            g_string_append(s, "deg");
        }
    } else if (prop == NS_CSS_TRANSLATE) {
        append_transform_length(s, op->a, op->a_is_percent);
        gboolean need_c = (op->c != 0);
        gboolean need_b = need_c || (op->b != 0) || op->b_is_percent;
        if (need_b) {
            g_string_append_c(s, ' ');
            append_transform_length(s, op->b, op->b_is_percent);
        }
        if (need_c) {
            g_string_append_c(s, ' ');
            append_transform_length(s, op->c, FALSE);
        }
    } else {
        g_string_free(s, TRUE);
        return NULL;
    }
    return g_string_free(s, FALSE);
}

gboolean
ns_css_transform_is_3d(const ns_css_transform *tf)
{
    if (!tf) return FALSE;
    for (int i = 0; i < tf->n_ops; i++) {
        const ns_css_transform_op *op = &tf->ops[i];
        switch (op->kind) {
        case NS_CSS_TFN_TRANSLATE:
            if (op->c != 0) return TRUE;
            break;
        case NS_CSS_TFN_SCALE:
            if (op->c != 0 && op->c != 1) return TRUE;
            break;
        case NS_CSS_TFN_ROTATE3D:
            if (fabs(op->d) > 1e-12 && (op->a != 0 || op->b != 0)) return TRUE;
            break;
        case NS_CSS_TFN_MATRIX3D:
        case NS_CSS_TFN_PERSPECTIVE:
            return TRUE;
        default:
            break;
        }
    }
    return FALSE;
}

void
ns_css_style_effective_transform(const ns_style *st,
                                 const ns_css_transform *transform_override,
                                 ns_css_transform *out)
{
    memset(out, 0, sizeof(*out));
    static const ns_css_prop independent[3] = {
        NS_CSS_TRANSLATE, NS_CSS_ROTATE, NS_CSS_SCALE,
    };
    for (int i = 0; i < 3; i++) {
        const ns_css_value *v = st ? st->values[independent[i]] : NULL;
        if (v && v->kind == NS_CSS_V_TRANSFORM && v->u.transform.n_ops > 0 &&
            out->n_ops < NS_CSS_TRANSFORM_OPS_MAX)
            out->ops[out->n_ops++] = v->u.transform.ops[0];
    }
    const ns_css_transform *tf = transform_override;
    if (!tf && st && st->values[NS_CSS_TRANSFORM] &&
        st->values[NS_CSS_TRANSFORM]->kind == NS_CSS_V_TRANSFORM)
        tf = &st->values[NS_CSS_TRANSFORM]->u.transform;
    if (tf)
        for (int i = 0; i < tf->n_ops && out->n_ops < NS_CSS_TRANSFORM_OPS_MAX; i++)
            out->ops[out->n_ops++] = tf->ops[i];
}

void
ns_css_transform_to_mat4(const ns_css_transform *tf,
                         double bw, double bh, ns_mat4 *out)
{
    ns_mat4_identity(out);
    if (!tf) return;
    for (int i = 0; i < tf->n_ops; i++) {
        ns_css_transform_op sane = tf->ops[i];
        double dflt = sane.kind == NS_CSS_TFN_SCALE ? 1.0 : 0.0;
        double *fields[] = { &sane.a, &sane.b, &sane.c, &sane.d,
                             &sane.e, &sane.f };
        for (gsize k = 0; k < G_N_ELEMENTS(fields); k++)
            if (!isfinite(*fields[k])) *fields[k] = dflt;
        const ns_css_transform_op *op = &sane;
        switch (op->kind) {
        case NS_CSS_TFN_TRANSLATE: {
            double dx = op->a_is_percent ? op->a / 100.0 * bw : op->a;
            double dy = op->b_is_percent ? op->b / 100.0 * bh : op->b;
            ns_mat4_translate(out, dx, dy, op->c);
            break;
        }
        case NS_CSS_TFN_ROTATE:
            ns_mat4_rotate_axis(out, 0, 0, 1, op->a);
            break;
        case NS_CSS_TFN_ROTATE3D:
            ns_mat4_rotate_axis(out, op->a, op->b, op->c, op->d);
            break;
        case NS_CSS_TFN_SCALE:
            ns_mat4_scale(out, op->a, op->b, op->c == 0 ? 1 : op->c);
            break;
        case NS_CSS_TFN_SKEW:
            ns_mat4_skew(out, op->a, op->b);
            break;
        case NS_CSS_TFN_MATRIX:
            ns_mat4_affine2d(out, op->a, op->b, op->c, op->d, op->e, op->f);
            break;
        case NS_CSS_TFN_MATRIX3D: {
            ns_mat4 t;
            for (int row = 0; row < 4; row++)
                for (int col = 0; col < 4; col++)
                    t.m[row * 4 + col] = op->m3d[col * 4 + row];
            ns_mat4_multiply(out, &t, out);
            break;
        }
        case NS_CSS_TFN_PERSPECTIVE:
            ns_mat4_perspective(out, op->a);
            break;
        }
    }
}

static ns_css_timing
parse_timing_keyword(const char *kw)
{
    ns_css_timing t = { .kind = NS_CSS_TIMING_EASE };
    if (!kw) return t;
    if (g_ascii_strcasecmp(kw, "linear") == 0)        t.kind = NS_CSS_TIMING_LINEAR;
    else if (g_ascii_strcasecmp(kw, "ease") == 0)     t.kind = NS_CSS_TIMING_EASE;
    else if (g_ascii_strcasecmp(kw, "ease-in") == 0)  t.kind = NS_CSS_TIMING_EASE_IN;
    else if (g_ascii_strcasecmp(kw, "ease-out") == 0) t.kind = NS_CSS_TIMING_EASE_OUT;
    else if (g_ascii_strcasecmp(kw, "ease-in-out") == 0) t.kind = NS_CSS_TIMING_EASE_IN_OUT;
    else if (g_ascii_strcasecmp(kw, "step-start") == 0) {
        t.kind = NS_CSS_TIMING_STEPS; t.steps = 1; t.step_pos = NS_CSS_STEP_JUMP_START;
    } else if (g_ascii_strcasecmp(kw, "step-end") == 0) {
        t.kind = NS_CSS_TIMING_STEPS; t.steps = 1; t.step_pos = NS_CSS_STEP_JUMP_END;
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

static ns_css_step_pos
parse_step_pos(const char *kw)
{
    if (g_ascii_strcasecmp(kw, "jump-start") == 0 ||
        g_ascii_strcasecmp(kw, "start") == 0)        return NS_CSS_STEP_JUMP_START;
    if (g_ascii_strcasecmp(kw, "jump-none") == 0)    return NS_CSS_STEP_JUMP_NONE;
    if (g_ascii_strcasecmp(kw, "jump-both") == 0)    return NS_CSS_STEP_JUMP_BOTH;
    return NS_CSS_STEP_JUMP_END;
}

static gboolean
extract_timing_function(char *item, ns_css_timing *out)
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
            out->kind = NS_CSS_TIMING_STEPS;
            out->steps = parts[0] ? (int)g_ascii_strtoll(g_strstrip(parts[0]), NULL, 10) : 1;
            if (out->steps < 1) out->steps = 1;
            out->step_pos = parts[0] && parts[1]
                ? parse_step_pos(g_strstrip(parts[1])) : NS_CSS_STEP_JUMP_END;
        } else {
            guint np = g_strv_length(parts);
            out->kind = NS_CSS_TIMING_CUBIC;
            for (int i = 0; i < 4; i++)
                out->cb[i] = (guint)i < np
                    ? g_ascii_strtod(g_strstrip(parts[i]), NULL) : 0.0;
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

static ns_css_value *
parse_anim_value(const char *text, gboolean is_animation)
{
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_ANIM;
    v->u.anim.n = 0;
    const char *p = text;
    const char *end = text + strlen(text);
    while (p < end && v->u.anim.n < NS_CSS_ANIM_ENTRIES_MAX) {
        char term = 0;
        const char *seg = css_scan_until(p, end, ",", &term);
        char *item_buf = css_trim_dup_range(p, seg);
        char *item = item_buf;
        if (!*item) {
            g_free(item_buf);
            p = term == ',' ? seg + 1 : seg;
            continue;
        }
        ns_css_anim_entry *e = &v->u.anim.entries[v->u.anim.n];
        e->target = is_animation ? NS_CSS_ANIM_TARGET_ALL : NS_CSS_ANIM_TARGET_NONE;
        e->name = NULL;
        e->duration_ms = 0;
        e->delay_ms = 0;
        e->timing = (ns_css_timing){ .kind = NS_CSS_TIMING_EASE };
        e->iter_count = 1;
        e->direction = NS_CSS_ANIM_DIR_NORMAL;
        e->fill = NS_CSS_ANIM_FILL_NONE;
        gboolean got_dur = FALSE;
        ns_css_timing fn_timing;
        if (extract_timing_function(item, &fn_timing))
            e->timing = fn_timing;
        char **toks = g_strsplit_set(item, " \t\n\r", -1);
        for (int j = 0; toks[j]; j++) {
            char *tok = g_strstrip(toks[j]);
            if (!*tok) continue;
            char *endp = NULL;
            double num = g_ascii_strtod(tok, &endp);
            gboolean bare_number = endp != tok && (*endp == '\0' || *endp == ' ');
            if (bare_number && is_animation && got_dur) {
                e->iter_count = num <= 0 ? 0 : (int)num;
                continue;
            }
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
            if (timing_keyword_matches(tok)) {
                e->timing = parse_timing_keyword(tok);
                continue;
            }
            if (is_animation) {
                if (g_ascii_strcasecmp(tok, "paused") == 0) {
                    e->paused = TRUE; continue;
                }
                if (g_ascii_strcasecmp(tok, "running") == 0) {
                    e->paused = FALSE; continue;
                }
                if (g_ascii_strcasecmp(tok, "reverse") == 0) {
                    e->direction = NS_CSS_ANIM_DIR_REVERSE; continue;
                }
                if (g_ascii_strcasecmp(tok, "alternate") == 0) {
                    e->direction = NS_CSS_ANIM_DIR_ALTERNATE; continue;
                }
                if (g_ascii_strcasecmp(tok, "alternate-reverse") == 0) {
                    e->direction = NS_CSS_ANIM_DIR_ALTERNATE_REVERSE; continue;
                }
                if (g_ascii_strcasecmp(tok, "forwards") == 0) {
                    e->fill = NS_CSS_ANIM_FILL_FORWARDS; continue;
                }
                if (g_ascii_strcasecmp(tok, "backwards") == 0) {
                    e->fill = NS_CSS_ANIM_FILL_BACKWARDS; continue;
                }
                if (g_ascii_strcasecmp(tok, "both") == 0) {
                    e->fill = NS_CSS_ANIM_FILL_BOTH; continue;
                }
            }
            if (g_ascii_strcasecmp(tok, "none") == 0) {
                continue;
            }
            if (!is_animation) {
                if (g_ascii_strcasecmp(tok, "opacity") == 0)
                    e->target = NS_CSS_ANIM_TARGET_OPACITY;
                else if (g_ascii_strcasecmp(tok, "transform") == 0)
                    e->target = NS_CSS_ANIM_TARGET_TRANSFORM;
                else if (g_ascii_strcasecmp(tok, "color") == 0)
                    e->target = NS_CSS_ANIM_TARGET_COLOR;
                else if (g_ascii_strcasecmp(tok, "background-color") == 0 ||
                         g_ascii_strcasecmp(tok, "background") == 0)
                    e->target = NS_CSS_ANIM_TARGET_BG_COLOR;
                else if (g_ascii_strcasecmp(tok, "all") == 0)
                    e->target = NS_CSS_ANIM_TARGET_ALL;
                else if (e->target == NS_CSS_ANIM_TARGET_NONE && !e->name &&
                         ns_css_prop_id(tok) >= 0) {
                    e->target = NS_CSS_ANIM_TARGET_OTHER;
                    e->name = ascii_lower(tok, strlen(tok));
                }
                continue;
            }
            if (is_animation && !e->name) {
                e->name = g_strdup(tok);
                continue;
            }
        }
        g_strfreev(toks);
        if (is_animation || e->target != NS_CSS_ANIM_TARGET_NONE)
            v->u.anim.n++;
        g_free(item_buf);
        p = term == ',' ? seg + 1 : seg;
    }
    if (v->u.anim.n == 0) {
        ns_css_value_free(v);
        return NULL;
    }
    return v;
}

static char *
normalize_display_short(const char *outside, const char *inside,
                        gboolean list_item, const char *single)
{
    if (single) {
        if (strcmp(single, "none") == 0 || strcmp(single, "contents") == 0 ||
            strcmp(single, "inline-block") == 0 ||
            strcmp(single, "inline-table") == 0 ||
            strcmp(single, "inline-flex") == 0 ||
            strcmp(single, "inline-grid") == 0 ||
            strncmp(single, "table-", 6) == 0 ||
            strcmp(single, "ruby-base") == 0 ||
            strcmp(single, "ruby-text") == 0)
            return g_strdup(single);
        if (strcmp(single, "block") == 0 || strcmp(single, "inline") == 0 ||
            strcmp(single, "run-in") == 0)
            outside = single;
        else if (strcmp(single, "list-item") == 0)
            list_item = TRUE;
        else
            inside = single;
    }

    if (!inside) inside = "flow";
    if (!outside) outside = strcmp(inside, "ruby") == 0 ? "inline" : "block";
    gboolean bl = strcmp(outside, "block") == 0;
    gboolean in_is = strcmp(outside, "inline") == 0;

    if (list_item) {
        gboolean froot = strcmp(inside, "flow-root") == 0;
        if (bl)
            return g_strdup(froot ? "flow-root list-item" : "list-item");
        if (in_is)
            return g_strdup(froot ? "inline flow-root list-item"
                                  : "inline list-item");
        return g_strdup(froot ? "run-in flow-root list-item"
                              : "run-in list-item");
    }

    if (bl) {
        if (strcmp(inside, "flow") == 0)      return g_strdup("block");
        if (strcmp(inside, "flow-root") == 0) return g_strdup("flow-root");
        if (strcmp(inside, "flex") == 0)      return g_strdup("flex");
        if (strcmp(inside, "grid") == 0)      return g_strdup("grid");
        if (strcmp(inside, "table") == 0)     return g_strdup("table");
        return g_strdup("block ruby");
    }
    if (in_is) {
        if (strcmp(inside, "flow") == 0)      return g_strdup("inline");
        if (strcmp(inside, "flow-root") == 0) return g_strdup("inline-block");
        if (strcmp(inside, "flex") == 0)      return g_strdup("inline-flex");
        if (strcmp(inside, "grid") == 0)      return g_strdup("inline-grid");
        if (strcmp(inside, "table") == 0)     return g_strdup("inline-table");
        return g_strdup("ruby");
    }
    if (strcmp(inside, "flow") == 0) return g_strdup("run-in");
    return g_strdup_printf("run-in %s", inside);
}

static char *
normalize_display_value(const char *text)
{
    char *kw = ascii_lower(text, strlen(text));
    if (strcmp(kw, "-webkit-flex") == 0 ||
        strcmp(kw, "-ms-flexbox") == 0) {
        g_free(kw);
        return g_strdup("flex");
    }
    if (strcmp(kw, "-webkit-inline-flex") == 0 ||
        strcmp(kw, "-ms-inline-flexbox") == 0) {
        g_free(kw);
        return g_strdup("inline-flex");
    }
    if (strcmp(kw, "-webkit-grid") == 0 ||
        strcmp(kw, "-ms-grid") == 0) {
        g_free(kw);
        return g_strdup("grid");
    }
    if (strcmp(kw, "-webkit-inline-box") == 0) {
        g_free(kw);
        return g_strdup("inline-block");
    }
    static const char *const reserved_single[] = {
        "none", "contents",
        "inline-block", "inline-table", "inline-flex", "inline-grid",
        "table-row-group", "table-header-group", "table-footer-group",
        "table-row", "table-cell", "table-column-group", "table-column",
        "table-caption", "ruby-base", "ruby-text",
        "block", "inline", "run-in", "list-item",
        "flow", "flow-root", "table", "flex", "grid", "ruby",
    };
    for (guint i = 0; i < G_N_ELEMENTS(reserved_single); i++) {
        if (strcmp(kw, reserved_single[i]) == 0)
            return normalize_display_short(NULL, NULL, FALSE, kw);
    }

    char *tokens[5] = {0};
    int n = split_ws_limit(kw, tokens, 5);
    const char *outside = NULL;
    const char *inside = NULL;
    gboolean list_item = FALSE;
    gboolean valid = n >= 2 && n <= 3;
    for (int i = 0; valid && i < n; i++) {
        const char *tok = tokens[i];
        if (strcmp(tok, "list-item") == 0) {
            if (list_item) valid = FALSE;
            list_item = TRUE;
        } else if (strcmp(tok, "block") == 0 ||
                   strcmp(tok, "inline") == 0 ||
                   strcmp(tok, "run-in") == 0) {
            if (outside) valid = FALSE;
            outside = tok;
        } else if (strcmp(tok, "flow") == 0 ||
                   strcmp(tok, "flow-root") == 0 ||
                   strcmp(tok, "flex") == 0 ||
                   strcmp(tok, "grid") == 0 ||
                   strcmp(tok, "table") == 0 ||
                   strcmp(tok, "ruby") == 0) {
            if (inside) valid = FALSE;
            inside = tok;
        } else {
            valid = FALSE;
        }
    }
    if (valid && list_item && inside &&
        strcmp(inside, "flow") != 0 && strcmp(inside, "flow-root") != 0)
        valid = FALSE;

    char *out;
    if (n <= 1) {
        out = g_strdup(kw);
    } else {
        out = valid
            ? normalize_display_short(outside, inside, list_item, NULL)
            : NULL;
    }
    for (int i = 0; i < n; i++) g_free(tokens[i]);
    g_free(kw);
    return out;
}

char *
ns_css_display_canonical(const char *value)
{
    if (!value) return NULL;
    while (*value && is_ws(*value)) value++;
    gsize n = strlen(value);
    while (n > 0 && is_ws(value[n - 1])) n--;
    char *t = g_strndup(value, n);
    char *r = normalize_display_value(t);
    g_free(t);
    return r;
}

char *
ns_css_display_blockify(const char *d)
{
    if (!d) return NULL;
    if (strcmp(d, "inline") == 0 || strcmp(d, "inline-block") == 0 ||
        strcmp(d, "table-row-group") == 0 || strcmp(d, "table-column") == 0 ||
        strcmp(d, "table-column-group") == 0 ||
        strcmp(d, "table-header-group") == 0 ||
        strcmp(d, "table-footer-group") == 0 || strcmp(d, "table-row") == 0 ||
        strcmp(d, "table-cell") == 0 || strcmp(d, "table-caption") == 0 ||
        strcmp(d, "ruby-base") == 0 || strcmp(d, "ruby-text") == 0 ||
        strcmp(d, "run-in") == 0)
        return g_strdup("block");
    if (strcmp(d, "inline-table") == 0) return g_strdup("table");
    if (strcmp(d, "inline-flex") == 0)  return g_strdup("flex");
    if (strcmp(d, "inline-grid") == 0)  return g_strdup("grid");
    if (strcmp(d, "ruby") == 0)         return g_strdup("block ruby");
    if (strcmp(d, "inline list-item") == 0)
        return g_strdup("list-item");
    if (strcmp(d, "inline flow-root list-item") == 0)
        return g_strdup("flow-root list-item");
    return NULL;
}

static gboolean
is_math_fn_start(const char *s)
{
    static const char *const fns[] = {
        "calc(", "min(", "max(", "clamp(", "round(", "mod(", "rem(",
        "abs(", "sign(", "hypot(", "pow(", "sqrt(", "sin(", "cos(",
        "tan(", "exp(", "log(", "atan2(", "atan(", "asin(", "acos(",
        "progress(",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(fns); i++)
        if (g_ascii_strncasecmp(s, fns[i], strlen(fns[i])) == 0)
            return TRUE;
    return FALSE;
}

static char *
serialize_calc_angle(double deg)
{
    if (isnan(deg)) return g_strdup("calc(NaN * 1deg)");
    if (isinf(deg)) return g_strdup(deg < 0 ? "calc(-infinity * 1deg)"
                                            : "calc(infinity * 1deg)");
    char *num = ns_css_number_str(deg);
    char *out = g_strdup_printf("calc(%sdeg)", num);
    g_free(num);
    return out;
}

static char *
serialize_calc_number(double n)
{
    if (isnan(n)) return g_strdup("calc(NaN)");
    if (isinf(n)) return g_strdup(n < 0 ? "calc(-infinity)" : "calc(infinity)");
    char *num = ns_css_number_str(n);
    char *out = g_strdup_printf("calc(%s)", num);
    g_free(num);
    return out;
}

static gboolean
eval_calc_number(const char *arg, double *out)
{
    ns_css_value *v = parse_calc(arg);
    if (!v) return FALSE;
    gboolean ok = v->kind == NS_CSS_V_LENGTH &&
                  v->u.length.unit == NS_CSS_UNIT_NUMBER;
    if (ok) *out = v->u.length.v;
    ns_css_value_free(v);
    return ok;
}

enum { TF_ARG_NONE = 0, TF_ARG_ANGLE, TF_ARG_NUMBER, TF_ARG_LENGTH };

static int
transform_arg_type(const char *fn_lc, int idx)
{
    if (!strcmp(fn_lc, "rotate") || !strcmp(fn_lc, "rotatex") ||
        !strcmp(fn_lc, "rotatey") || !strcmp(fn_lc, "rotatez") ||
        !strcmp(fn_lc, "skewx") || !strcmp(fn_lc, "skewy"))
        return idx == 0 ? TF_ARG_ANGLE : TF_ARG_NONE;
    if (!strcmp(fn_lc, "skew"))
        return idx < 2 ? TF_ARG_ANGLE : TF_ARG_NONE;
    if (!strcmp(fn_lc, "rotate3d"))
        return idx == 3 ? TF_ARG_ANGLE : idx < 3 ? TF_ARG_NUMBER : TF_ARG_NONE;
    if (!strcmp(fn_lc, "scale") || !strcmp(fn_lc, "scalex") ||
        !strcmp(fn_lc, "scaley") || !strcmp(fn_lc, "scalez") ||
        !strcmp(fn_lc, "scale3d") || !strcmp(fn_lc, "matrix") ||
        !strcmp(fn_lc, "matrix3d"))
        return TF_ARG_NUMBER;
    if (!strcmp(fn_lc, "translate") || !strcmp(fn_lc, "translatex") ||
        !strcmp(fn_lc, "translatey") || !strcmp(fn_lc, "translatez") ||
        !strcmp(fn_lc, "translate3d") || !strcmp(fn_lc, "perspective"))
        return TF_ARG_LENGTH;
    return TF_ARG_NONE;
}

static char *
canonicalize_transform_arg(const char *arg, int type)
{
    if (type == TF_ARG_ANGLE) {
        double deg = 0;
        if (!parse_angle_any(arg, &deg)) return NULL;
        return serialize_calc_angle(deg);
    }
    if (type == TF_ARG_NUMBER) {
        if (ns_value_has_relative_unit(arg)) return NULL;
        double n = 0;
        if (!eval_calc_number(arg, &n)) return NULL;
        return serialize_calc_number(n);
    }
    if (type == TF_ARG_LENGTH) {
        if (ns_value_has_relative_unit(arg)) return NULL;
        double px = 0, pct = 0;
        if (!resolve_to_px_pct(arg, strlen(arg), &px, &pct)) return NULL;
        if (!isfinite(px) || !isfinite(pct)) return NULL;
        if (px != 0 && pct != 0) return NULL;
        if (pct != 0) {
            char *num = ns_css_number_str(pct);
            char *out = g_strdup_printf("calc(%s%%)", num);
            g_free(num);
            return out;
        }
        char *num = ns_css_number_str(px);
        char *out = g_strdup_printf("calc(%spx)", num);
        g_free(num);
        return out;
    }
    return NULL;
}

char *
ns_css_transform_canonical(const char *value)
{
    if (!value) return NULL;
    const char *scan = value;
    while (*scan && is_ws(*scan)) scan++;
    if (!*scan || g_ascii_strncasecmp(scan, "none", 4) == 0) return NULL;

    GString *out = g_string_new(NULL);
    gboolean changed = FALSE;
    const char *p = value;
    while (*p) {
        if (is_ws(*p) || *p == ',') { g_string_append_c(out, *p); p++; continue; }
        const char *nstart = p;
        while (*p && *p != '(' && !is_ws(*p) && *p != ',') p++;
        gsize nlen = (gsize)(p - nstart);
        g_string_append_len(out, nstart, nlen);
        while (*p && is_ws(*p)) { g_string_append_c(out, *p); p++; }
        if (*p != '(') continue;
        char *fn_lc = g_ascii_strdown(nstart, nlen);
        g_string_append_c(out, '(');
        p++;
        const char *astart = p;
        int depth = 1;
        while (*p && depth > 0) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            if (depth > 0) p++;
        }
        const char *aend = p;
        const char *seg = astart;
        int adepth = 0, argidx = 0;
        for (const char *q = astart; q <= aend; q++) {
            if (q < aend && *q == '(') adepth++;
            else if (q < aend && *q == ')') adepth--;
            if (q == aend || (*q == ',' && adepth == 0)) {
                const char *s = seg;
                while (s < q && is_ws(*s)) s++;
                const char *e = q;
                while (e > s && is_ws(e[-1])) e--;
                char *argtxt = g_strndup(s, (gsize)(e - s));
                char *canon = NULL;
                int type = transform_arg_type(fn_lc, argidx);
                if (type != TF_ARG_NONE && is_math_fn_start(argtxt))
                    canon = canonicalize_transform_arg(argtxt, type);
                if (canon) {
                    g_string_append_len(out, seg, (gsize)(s - seg));
                    g_string_append(out, canon);
                    g_string_append_len(out, e, (gsize)(q - e));
                    g_free(canon);
                    changed = TRUE;
                } else {
                    g_string_append_len(out, seg, (gsize)(q - seg));
                }
                g_free(argtxt);
                if (q < aend) g_string_append_c(out, ',');
                argidx++;
                seg = q + 1;
            }
        }
        g_free(fn_lc);
        if (*p == ')') { g_string_append_c(out, ')'); p++; }
    }
    if (!changed) { g_string_free(out, TRUE); return NULL; }
    return g_string_free(out, FALSE);
}

char *
ns_css_specified_canonical(const char *prop, const char *value)
{
    if (prop && strcmp(prop, "display") == 0) {
        char *d = ns_css_display_canonical(value);
        if (d) return d;
    }
    if (prop && strcmp(prop, "transform") == 0) {
        char *t = ns_css_transform_canonical(value);
        if (t) return t;
    }
    if (prop && (strcmp(prop, "transition-delay") == 0 ||
                 strcmp(prop, "transition-duration") == 0 ||
                 strcmp(prop, "animation-delay") == 0 ||
                 strcmp(prop, "animation-duration") == 0)) {
        char *t = ns_css_time_specified(value);
        if (t) return t;
        return NULL;
    }
    return ns_css_math_canonical(value);
}

static ns_css_value *parse_value_for(ns_css_prop prop, const char *text);
static gboolean is_font_stretch_keyword(const char *s);
static gboolean is_font_ligatures_value(const char *s);
static gboolean is_font_feature_settings_value(const char *s);
static gboolean is_font_variation_settings_value(const char *s);

typedef enum { TVT_INVALID, TVT_NUMBER, TVT_TIME } ns_tval_type;

static ns_tval_type css_time_sum(const char *s, const char *e);
static ns_tval_type css_time_product(const char **pp, const char *e);
static ns_tval_type css_time_factor(const char **pp, const char *e);

static gboolean
css_tv_name_is(const char *s, gsize n, const char *lit)
{
    return strlen(lit) == n && g_ascii_strncasecmp(s, lit, n) == 0;
}

static ns_tval_type
css_time_func(const char *name, gsize nlen, const char *s, const char *e)
{
    const char *starts[8], *ends[8];
    int n = 0, depth = 0;
    const char *seg = s;
    for (const char *c = s; c <= e; c++) {
        if (c < e && *c == '(') depth++;
        else if (c < e && *c == ')') depth--;
        else if ((c == e || (*c == ',' && depth == 0))) {
            if (n < (int)G_N_ELEMENTS(starts)) { starts[n] = seg; ends[n] = c; n++; }
            else return TVT_INVALID;
            seg = c + 1;
        }
    }
    ns_tval_type at[8];
    for (int i = 0; i < n; i++) at[i] = css_time_sum(starts[i], ends[i]);

    if (css_tv_name_is(name, nlen, "calc"))
        return n == 1 ? at[0] : TVT_INVALID;
    if (css_tv_name_is(name, nlen, "min") ||
        css_tv_name_is(name, nlen, "max") ||
        css_tv_name_is(name, nlen, "hypot")) {
        if (n < 1) return TVT_INVALID;
        for (int i = 0; i < n; i++)
            if (at[i] == TVT_INVALID || at[i] != at[0]) return TVT_INVALID;
        return at[0];
    }
    if (css_tv_name_is(name, nlen, "clamp")) {
        if (n != 3) return TVT_INVALID;
        for (int i = 0; i < 3; i++)
            if (at[i] == TVT_INVALID || at[i] != at[0]) return TVT_INVALID;
        return at[0];
    }
    if (css_tv_name_is(name, nlen, "abs"))
        return n == 1 ? at[0] : TVT_INVALID;
    if (css_tv_name_is(name, nlen, "sign"))
        return (n == 1 && at[0] != TVT_INVALID) ? TVT_NUMBER : TVT_INVALID;
    if (css_tv_name_is(name, nlen, "mod") || css_tv_name_is(name, nlen, "rem")) {
        if (n != 2 || at[0] == TVT_INVALID || at[0] != at[1]) return TVT_INVALID;
        return at[0];
    }
    if (css_tv_name_is(name, nlen, "round")) {
        int base = 0;
        if (n >= 1 && (css_tv_name_is(starts[0], (gsize)(ends[0] - starts[0]), "nearest") ||
                       css_tv_name_is(starts[0], (gsize)(ends[0] - starts[0]), "up") ||
                       css_tv_name_is(starts[0], (gsize)(ends[0] - starts[0]), "down") ||
                       css_tv_name_is(starts[0], (gsize)(ends[0] - starts[0]), "to-zero")))
            base = 1;
        int cnt = n - base;
        if (cnt < 1 || cnt > 2) return TVT_INVALID;
        ns_tval_type t = at[base];
        if (t == TVT_INVALID) return TVT_INVALID;
        for (int i = base; i < n; i++)
            if (at[i] == TVT_INVALID || at[i] != t) return TVT_INVALID;
        return t;
    }
    if (css_tv_name_is(name, nlen, "sqrt") || css_tv_name_is(name, nlen, "exp") ||
        css_tv_name_is(name, nlen, "log") || css_tv_name_is(name, nlen, "pow") ||
        css_tv_name_is(name, nlen, "sin") || css_tv_name_is(name, nlen, "cos") ||
        css_tv_name_is(name, nlen, "tan") || css_tv_name_is(name, nlen, "asin") ||
        css_tv_name_is(name, nlen, "acos") || css_tv_name_is(name, nlen, "atan") ||
        css_tv_name_is(name, nlen, "atan2")) {
        for (int i = 0; i < n; i++)
            if (at[i] != TVT_NUMBER) return TVT_INVALID;
        return TVT_NUMBER;
    }
    return TVT_INVALID;
}

static ns_tval_type
css_time_factor(const char **pp, const char *e)
{
    const char *p = *pp;
    while (p < e && is_ws(*p)) p++;
    if (p >= e) { *pp = p; return TVT_INVALID; }
    if (*p == '(') {
        const char *close = match_close_paren(p + 1, e);
        if (!close) { *pp = e; return TVT_INVALID; }
        ns_tval_type t = css_time_sum(p + 1, close);
        *pp = close + 1;
        return t;
    }
    if (g_ascii_isalpha((guchar)*p)) {
        const char *id = p;
        while (p < e && (g_ascii_isalpha((guchar)*p) || *p == '-')) p++;
        gsize nlen = (gsize)(p - id);
        if (p < e && *p == '(') {
            const char *close = match_close_paren(p + 1, e);
            if (!close) { *pp = e; return TVT_INVALID; }
            ns_tval_type t = css_time_func(id, nlen, p + 1, close);
            *pp = close + 1;
            return t;
        }
        *pp = p;
        if (css_tv_name_is(id, nlen, "pi") || css_tv_name_is(id, nlen, "e") ||
            css_tv_name_is(id, nlen, "infinity") || css_tv_name_is(id, nlen, "nan"))
            return TVT_NUMBER;
        return TVT_INVALID;
    }
    char *endp = NULL;
    double num = g_ascii_strtod(p, &endp);
    (void)num;
    if (!endp || endp == p) { *pp = p; return TVT_INVALID; }
    const char *u = endp;
    if (u < e && *u == '%') { *pp = u + 1; return TVT_INVALID; }
    const char *us = u;
    while (u < e && g_ascii_isalpha((guchar)*u)) u++;
    gsize ulen = (gsize)(u - us);
    *pp = u;
    if (ulen == 0) return TVT_NUMBER;
    if (css_tv_name_is(us, ulen, "s") || css_tv_name_is(us, ulen, "ms"))
        return TVT_TIME;
    return TVT_INVALID;
}

static ns_tval_type
css_time_product(const char **pp, const char *e)
{
    const char *p = *pp;
    ns_tval_type acc = css_time_factor(&p, e);
    if (acc == TVT_INVALID) { *pp = p; return TVT_INVALID; }
    for (;;) {
        const char *q = p;
        while (q < e && is_ws(*q)) q++;
        if (q >= e || (*q != '*' && *q != '/')) { p = q; break; }
        char op = *q++;
        const char *r = q;
        ns_tval_type rhs = css_time_factor(&r, e);
        if (rhs == TVT_INVALID) { *pp = r; return TVT_INVALID; }
        if (op == '*') {
            if (acc == TVT_NUMBER && rhs == TVT_NUMBER) acc = TVT_NUMBER;
            else if (acc == TVT_NUMBER && rhs == TVT_TIME) acc = TVT_TIME;
            else if (acc == TVT_TIME && rhs == TVT_NUMBER) acc = TVT_TIME;
            else { *pp = r; return TVT_INVALID; }
        } else {
            if (rhs == TVT_NUMBER) { /* acc unchanged */ }
            else if (acc == TVT_TIME && rhs == TVT_TIME) acc = TVT_NUMBER;
            else { *pp = r; return TVT_INVALID; }
        }
        p = r;
    }
    *pp = p;
    return acc;
}

static ns_tval_type
css_time_sum(const char *s, const char *e)
{
    const char *p = s;
    while (p < e && is_ws(*p)) p++;
    if (p >= e) return TVT_INVALID;
    ns_tval_type acc = css_time_product(&p, e);
    if (acc == TVT_INVALID) return TVT_INVALID;
    for (;;) {
        while (p < e && is_ws(*p)) p++;
        if (p >= e) break;
        char op = *p;
        if (op != '+' && op != '-') return TVT_INVALID;
        p++;
        ns_tval_type rhs = css_time_product(&p, e);
        if (rhs == TVT_INVALID || rhs != acc) return TVT_INVALID;
    }
    return acc;
}

static ns_css_value *
parse_time_property(const char *t)
{
    const char *e = t + strlen(t);
    const char *seg = t;
    int depth = 0;
    gboolean any = FALSE;
    for (const char *c = t; c <= e; c++) {
        if (c < e && *c == '(') depth++;
        else if (c < e && *c == ')') depth--;
        else if (c == e || (*c == ',' && depth == 0)) {
            any = TRUE;
            if (css_time_sum(seg, c) != TVT_TIME) return NULL;
            seg = c + 1;
        }
    }
    if (!any) return NULL;
    ns_css_value *v = g_new0(ns_css_value, 1);
    v->kind = NS_CSS_V_KEYWORD;
    v->u.keyword = g_strdup(t);
    return v;
}

static char *
css_time_strip_units(const char *s, const char *e)
{
    GString *out = g_string_new(NULL);
    const char *p = s;
    while (p < e) {
        if (g_ascii_isalpha((guchar)*p)) {
            while (p < e && (g_ascii_isalpha((guchar)*p) || *p == '-'))
                g_string_append_c(out, *p++);
            continue;
        }
        gboolean num_start = g_ascii_isdigit((guchar)*p) || *p == '.' ||
            ((*p == '+' || *p == '-') && p + 1 < e &&
             (g_ascii_isdigit((guchar)p[1]) || p[1] == '.'));
        if (num_start) {
            char *endp = NULL;
            double num = g_ascii_strtod(p, &endp);
            if (!endp || endp == p) { g_string_append_c(out, *p++); continue; }
            const char *us = endp;
            const char *u = us;
            while (u < e && g_ascii_isalpha((guchar)*u)) u++;
            gsize ulen = (gsize)(u - us);
            if (ulen == 2 && g_ascii_strncasecmp(us, "ms", 2) == 0)
                g_string_append_printf(out, "(%.17g*0.001)", num);
            else if (ulen == 1 && (us[0] == 's' || us[0] == 'S'))
                g_string_append_len(out, p, (gsize)(endp - p));
            else {
                g_string_append_len(out, p, (gsize)(endp - p));
                g_string_append_len(out, us, ulen);
            }
            p = u;
            continue;
        }
        g_string_append_c(out, *p++);
    }
    return g_string_free(out, FALSE);
}

static gboolean
css_time_seconds(const char *s, const char *e, double *out)
{
    if (css_time_sum(s, e) != TVT_TIME) return FALSE;
    char *stripped = css_time_strip_units(s, e);
    ns_css_value *v = parse_calc(stripped);
    if (!v) {
        char *wrapped = g_strdup_printf("calc(%s)", stripped);
        v = parse_calc(wrapped);
        g_free(wrapped);
    }
    g_free(stripped);
    if (!v) return FALSE;
    gboolean ok = v->kind == NS_CSS_V_LENGTH;
    if (ok) *out = v->u.length.v;
    ns_css_value_free(v);
    return ok;
}

static gboolean
css_starts_math_fn(const char *s, const char *e)
{
    while (s < e && is_ws(*s)) s++;
    static const char *const fns[] = {
        "calc(", "min(", "max(", "clamp(", "round(", "mod(", "rem(",
        "abs(", "hypot(", "sign(",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(fns); i++) {
        gsize l = strlen(fns[i]);
        if ((gsize)(e - s) >= l && g_ascii_strncasecmp(s, fns[i], l) == 0)
            return TRUE;
    }
    return FALSE;
}

static char *
css_time_list_serialize(const char *value, gboolean computed)
{
    const char *e = value + strlen(value);
    GString *out = g_string_new(NULL);
    const char *seg = value;
    int depth = 0;
    gboolean changed = FALSE, first = TRUE;
    for (const char *c = value; c <= e; c++) {
        if (c < e && *c == '(') depth++;
        else if (c < e && *c == ')') depth--;
        else if (c == e || (*c == ',' && depth == 0)) {
            const char *is = seg, *ie = c;
            while (is < ie && is_ws(*is)) is++;
            while (ie > is && is_ws(ie[-1])) ie--;
            if (!first) g_string_append(out, ", ");
            first = FALSE;
            double sec = 0;
            gboolean did = FALSE;
            if ((computed || css_starts_math_fn(is, ie)) &&
                css_time_seconds(is, ie, &sec)) {
                if (computed) {
                    if (isfinite(sec)) {
                        char *num = ns_css_number_str(sec);
                        g_string_append_printf(out, "%ss", num);
                        g_free(num);
                        changed = did = TRUE;
                    }
                } else if (isfinite(sec)) {
                    char *num = ns_css_number_str(sec);
                    g_string_append_printf(out, "calc(%ss)", num);
                    g_free(num);
                    changed = did = TRUE;
                } else if (isnan(sec)) {
                    g_string_append(out, "calc(NaN * 1s)");
                    changed = did = TRUE;
                } else {
                    g_string_append(out, sec < 0 ? "calc(-infinity * 1s)"
                                                 : "calc(infinity * 1s)");
                    changed = did = TRUE;
                }
            }
            if (!did) g_string_append_len(out, is, (gsize)(ie - is));
            seg = c + 1;
        }
    }
    if (!changed) { g_string_free(out, TRUE); return NULL; }
    return g_string_free(out, FALSE);
}

char *
ns_css_time_specified(const char *value)
{
    return value ? css_time_list_serialize(value, FALSE) : NULL;
}

char *
ns_css_time_computed(const char *value)
{
    return value ? css_time_list_serialize(value, TRUE) : NULL;
}

static gboolean
css_is_integer_token(const char *s)
{
    if (!s) return FALSE;
    if (*s == '+' || *s == '-') s++;
    if (!*s) return FALSE;
    for (; *s; s++)
        if (!g_ascii_isdigit((guchar)*s)) return FALSE;
    return TRUE;
}

static const char *
integer_prop_keyword(ns_css_prop prop)
{
    switch (prop) {
    case NS_CSS_Z_INDEX:
    case NS_CSS_COLUMN_COUNT:          return "auto";
    case NS_CSS_MAX_LINES:             return "none";
    case NS_CSS_HYPHENATE_LIMIT_LINES: return "no-limit";
    default:                           return NULL;
    }
}

static ns_css_value *
parse_integer_property(ns_css_prop prop, const char *t)
{
    const char *kw = integer_prop_keyword(prop);
    if (kw && g_ascii_strcasecmp(t, kw) == 0) {
        ns_css_value *v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = g_strdup(kw);
        return v;
    }
    if (css_is_integer_token(t)) {
        ns_css_value *v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_LENGTH;
        v->u.length.v = (double)g_ascii_strtoll(t, NULL, 10);
        v->u.length.unit = NS_CSS_UNIT_NUMBER;
        return v;
    }
    ns_css_value *cv = parse_calc(t);
    if (cv) {
        if (cv->kind == NS_CSS_V_LENGTH &&
            cv->u.length.unit == NS_CSS_UNIT_NUMBER) {
            cv->u.length.v = round(cv->u.length.v);
            return cv;
        }
        ns_css_value_free(cv);
    }
    return NULL;
}

static gboolean
value_has_top_level_comma(const char *t)
{
    int depth = 0;
    for (const char *p = t; *p; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') { if (depth > 0) depth--; }
        else if (*p == ',' && depth == 0) return TRUE;
    }
    return FALSE;
}

static gboolean
prop_is_bg_layered(ns_css_prop prop)
{
    return prop == NS_CSS_BACKGROUND_IMAGE ||
           prop == NS_CSS_BACKGROUND_REPEAT ||
           prop == NS_CSS_BACKGROUND_SIZE ||
           prop == NS_CSS_BACKGROUND_POSITION_X ||
           prop == NS_CSS_BACKGROUND_POSITION_Y;
}

static ns_css_value *
parse_value_layer_list(ns_css_prop prop, const char *t)
{
    ns_css_value *head = NULL, *tail = NULL;
    int depth = 0;
    const char *seg = t;
    for (const char *p = t; ; p++) {
        if (*p == '(') depth++;
        else if (*p == ')') { if (depth > 0) depth--; }
        if ((*p == ',' && depth == 0) || !*p) {
            char *part = g_strndup(seg, (gsize)(p - seg));
            ns_css_value *lv = parse_value_for(prop, part);
            g_free(part);
            if (lv) {
                if (tail) tail->next_layer = lv;
                else head = lv;
                tail = lv;
            }
            if (!*p) break;
            seg = p + 1;
        }
    }
    return head;
}

static ns_css_value *
parse_value_for(ns_css_prop prop, const char *text)
{

    while (*text && is_ws(*text)) text++;
    gsize n = strlen(text);
    while (n > 0 && is_ws(text[n - 1])) n--;
    char *t = g_strndup(text, n);

    ns_css_value *v = NULL;

    v = parse_css_wide_keyword(t);
    if (v) {
        g_free(t);
        return v;
    }

    if (prop_is_bg_layered(prop) && value_has_top_level_comma(t)) {
        v = parse_value_layer_list(prop, t);
        g_free(t);
        return v;
    }

    switch (prop) {
    case NS_CSS_DISPLAY: {
        char *norm = normalize_display_value(t);
        if (!norm) break;
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = norm;
        break;
    }
    case NS_CSS_POSITION: {
        char *kw = ascii_lower(t, strlen(t));
        if (strcmp(kw, "-webkit-sticky") == 0) {
            g_free(kw);
            kw = g_strdup("sticky");
        }
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = kw;
        break;
    }
    case NS_CSS_OVERFLOW:
    case NS_CSS_OVERFLOW_X:
    case NS_CSS_OVERFLOW_Y: {
        char *kw = ascii_lower(t, strlen(t));
        if (strcmp(kw, "overlay") == 0) {
            g_free(kw);
            kw = g_strdup("auto");
        }
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = kw;
        break;
    }
    case NS_CSS_CLIP: {
        const char *open = strchr(t, '(');
        const char *close = open ? strrchr(t, ')') : NULL;
        if (!open || !close || close < open) break;
        char *inner = g_strndup(open + 1, close - open - 1);
        char **parts = g_strsplit_set(inner, ", \t", -1);
        double cv[4] = {0,0,0,0};
        ns_css_unit cu[4] = {NS_CSS_UNIT_PX, NS_CSS_UNIT_PX,
                             NS_CSS_UNIT_PX, NS_CSS_UNIT_PX};
        gboolean ca[4] = {TRUE, TRUE, TRUE, TRUE};
        int idx = 0;
        for (int i = 0; parts[i] && idx < 4; i++) {
            if (!parts[i][0]) continue;
            if (g_ascii_strcasecmp(parts[i], "auto") == 0) {
                ca[idx] = TRUE;
            } else if (parse_length(parts[i], &cv[idx], &cu[idx])) {
                ca[idx] = FALSE;
            }
            idx++;
        }
        g_strfreev(parts);
        g_free(inner);
        if (idx == 4) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_RECT;
            for (int i = 0; i < 4; i++) {
                v->u.rect.v[i] = cv[i];
                v->u.rect.unit[i] = cu[i];
                v->u.rect.is_auto[i] = ca[i];
            }
        }
        break;
    }
    case NS_CSS_COLOR:
    case NS_CSS_BACKGROUND_COLOR:
    case NS_CSS_BORDER_TOP_COLOR:
    case NS_CSS_BORDER_RIGHT_COLOR:
    case NS_CSS_BORDER_BOTTOM_COLOR:
    case NS_CSS_BORDER_LEFT_COLOR:
    case NS_CSS_OUTLINE_COLOR:
    case NS_CSS_TEXT_DECORATION_COLOR:
    case NS_CSS_COLUMN_RULE_COLOR:
    case NS_CSS_CARET_COLOR:
    case NS_CSS_ACCENT_COLOR: {
        guint8 r, g, b, a;
        if (parse_color(t, &r, &g, &b, &a)) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_COLOR;
            v->u.color.r = r; v->u.color.g = g; v->u.color.b = b; v->u.color.a = a;
        } else {
            char *kw = ascii_lower(t, strlen(t));
            if (kw && (strcmp(kw, "currentcolor") == 0 ||
                       strcmp(kw, "inherit") == 0 ||
                       strcmp(kw, "transparent") == 0)) {
                v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_KEYWORD;
                v->u.keyword = kw;
            } else {
                g_free(kw);
            }
        }
        break;
    }
    case NS_CSS_FONT_SIZE:
    case NS_CSS_MARGIN_TOP: case NS_CSS_MARGIN_RIGHT:
    case NS_CSS_MARGIN_BOTTOM: case NS_CSS_MARGIN_LEFT:
    case NS_CSS_PADDING_TOP: case NS_CSS_PADDING_RIGHT:
    case NS_CSS_PADDING_BOTTOM: case NS_CSS_PADDING_LEFT:
    case NS_CSS_BORDER_TOP_WIDTH: case NS_CSS_BORDER_RIGHT_WIDTH:
    case NS_CSS_BORDER_BOTTOM_WIDTH: case NS_CSS_BORDER_LEFT_WIDTH:
    case NS_CSS_WIDTH: case NS_CSS_HEIGHT:
    case NS_CSS_MAX_WIDTH: case NS_CSS_MAX_HEIGHT:
    case NS_CSS_MIN_WIDTH: case NS_CSS_MIN_HEIGHT:
    case NS_CSS_LETTER_SPACING: case NS_CSS_WORD_SPACING:
    case NS_CSS_TEXT_INDENT:
    case NS_CSS_OPACITY:
    case NS_CSS_BORDER_RADIUS:
    case NS_CSS_BORDER_TOP_LEFT_RADIUS:
    case NS_CSS_BORDER_TOP_RIGHT_RADIUS:
    case NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS:
    case NS_CSS_BORDER_BOTTOM_LEFT_RADIUS:
    case NS_CSS_GAP: case NS_CSS_ROW_GAP: case NS_CSS_COLUMN_GAP:
    case NS_CSS_FLEX_GROW: case NS_CSS_FLEX_SHRINK:
    case NS_CSS_FLEX_BASIS:
    case NS_CSS_LINE_CLAMP:
    case NS_CSS_LINE_HEIGHT:
    case NS_CSS_OUTLINE_WIDTH:
    case NS_CSS_OUTLINE_OFFSET:
    case NS_CSS_TOP: case NS_CSS_RIGHT:
    case NS_CSS_BOTTOM: case NS_CSS_LEFT:
    case NS_CSS_COLUMN_WIDTH:
    case NS_CSS_COLUMN_RULE_WIDTH: {
        if (prop == NS_CSS_FONT_SIZE) {
            double fs = font_size_keyword_px(t);
            if (fs > 0) {
                v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_LENGTH;
                v->u.length.v = fs;
                v->u.length.unit = NS_CSS_UNIT_PX;
                break;
            }
        }
        if (prop == NS_CSS_BORDER_TOP_WIDTH || prop == NS_CSS_BORDER_RIGHT_WIDTH ||
            prop == NS_CSS_BORDER_BOTTOM_WIDTH || prop == NS_CSS_BORDER_LEFT_WIDTH ||
            prop == NS_CSS_OUTLINE_WIDTH || prop == NS_CSS_COLUMN_RULE_WIDTH) {
            double bw = -1;
            if      (g_ascii_strcasecmp(t, "thin")   == 0) bw = 1;
            else if (g_ascii_strcasecmp(t, "medium") == 0) bw = 3;
            else if (g_ascii_strcasecmp(t, "thick")  == 0) bw = 5;
            if (bw >= 0) {
                v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_LENGTH;
                v->u.length.v = bw;
                v->u.length.unit = NS_CSS_UNIT_PX;
                break;
            }
        }
        gboolean sizing_prop = prop == NS_CSS_WIDTH || prop == NS_CSS_HEIGHT ||
            prop == NS_CSS_MIN_WIDTH || prop == NS_CSS_MAX_WIDTH ||
            prop == NS_CSS_MIN_HEIGHT || prop == NS_CSS_MAX_HEIGHT;
        if (g_ascii_strcasecmp(t, "auto") == 0 ||
            (prop == NS_CSS_LINE_HEIGHT &&
             g_ascii_strcasecmp(t, "normal") == 0) ||
            (sizing_prop &&
             (g_ascii_strcasecmp(t, "min-content") == 0 ||
              g_ascii_strcasecmp(t, "max-content") == 0 ||
              g_ascii_strcasecmp(t, "fit-content") == 0))) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = ascii_lower(t, strlen(t));
        } else if ((v = parse_calc(t))) {

        } else {
            double num;
            ns_css_unit u;
            if (parse_length(t, &num, &u)) {
                v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_LENGTH;
                v->u.length.v = num;
                v->u.length.unit = u;
            }
        }
        if (v && prop == NS_CSS_OPACITY) {
            if (v->kind == NS_CSS_V_LENGTH &&
                v->u.length.unit == NS_CSS_UNIT_PERCENT) {
                v->u.length.v /= 100.0;
                v->u.length.unit = NS_CSS_UNIT_NUMBER;
            } else if (v->kind == NS_CSS_V_CALC && v->u.calc.px == 0 &&
                       v->u.calc.em == 0 && v->u.calc.rem == 0) {
                double pnum = v->u.calc.pct / 100.0;
                ns_css_value_free(v);
                v = calc_num_value(pnum);
            }
        }
        break;
    }
    case NS_CSS_ORDER:
    case NS_CSS_Z_INDEX:
    case NS_CSS_COLUMN_COUNT:
    case NS_CSS_ORPHANS:
    case NS_CSS_WIDOWS:
    case NS_CSS_MAX_LINES:
    case NS_CSS_HYPHENATE_LIMIT_LINES:
        v = parse_integer_property(prop, t);
        break;
    case NS_CSS_COLUMN_SPAN:
        if (g_ascii_strcasecmp(t, "none") == 0 ||
            g_ascii_strcasecmp(t, "all") == 0) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = ascii_lower(t, strlen(t));
        }
        break;
    case NS_CSS_TRANSITION_DELAY:
    case NS_CSS_TRANSITION_DURATION:
    case NS_CSS_ANIMATION_DELAY:
    case NS_CSS_ANIMATION_DURATION:
        v = parse_time_property(t);
        break;
    case NS_CSS_BOX_SHADOW:
    case NS_CSS_TEXT_SHADOW: {
        v = parse_box_shadow(t);
        break;
    }
    case NS_CSS_GRID_TEMPLATE_COLUMNS:
    case NS_CSS_GRID_TEMPLATE_ROWS:
    case NS_CSS_GRID_AUTO_ROWS:
    case NS_CSS_GRID_AUTO_COLUMNS: {
        v = parse_tracks(t);
        if (!v) {
            char *kw = ascii_lower(t, strlen(t));
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
        }
        break;
    }
    case NS_CSS_GRID_TEMPLATE_AREAS: {
        v = parse_areas(t);
        if (!v) {
            char *kw = ascii_lower(t, strlen(t));
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
        }
        break;
    }
    case NS_CSS_BACKGROUND_POSITION_X:
    case NS_CSS_BACKGROUND_POSITION_Y:
    case NS_CSS_OBJECT_POSITION_X:
    case NS_CSS_OBJECT_POSITION_Y: {
        if ((v = parse_calc(t))) break;
        char *kw = ascii_lower(t, strlen(t));
        double pct = -1;
        if (kw) {
            if (prop == NS_CSS_BACKGROUND_POSITION_X ||
                prop == NS_CSS_OBJECT_POSITION_X) {
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
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_LENGTH;
            v->u.length.v = pct;
            v->u.length.unit = NS_CSS_UNIT_PERCENT;
        } else {
            g_free(kw);
            double num;
            ns_css_unit u;
            if (parse_length(t, &num, &u)) {
                v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_LENGTH;
                v->u.length.v = num;
                v->u.length.unit = u;
            }
        }
        break;
    }
    case NS_CSS_BACKGROUND_SIZE: {
        char *kw = ascii_lower(t, strlen(t));
        if (kw && (strcmp(kw, "cover") == 0 || strcmp(kw, "contain") == 0 ||
                   strcmp(kw, "auto") == 0)) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
        } else {
            g_free(kw);
            char *tokens[4] = {0};
            int nt = split_ws(t, tokens);
            double w = 0, h = 0;
            ns_css_unit wu = NS_CSS_UNIT_PX, hu = NS_CSS_UNIT_PX;
            gboolean w_auto = FALSE, h_auto = TRUE;
            gboolean ok = FALSE;
            if (nt == 1) {
                if (g_ascii_strcasecmp(tokens[0], "auto") == 0) {
                    ok = TRUE;
                    w_auto = TRUE;
                    h_auto = TRUE;
                } else if (parse_bg_size_component(tokens[0], &w, &wu)) {
                    ok = TRUE;
                }
            } else if (nt >= 2) {
                if (g_ascii_strcasecmp(tokens[0], "auto") == 0) {
                    w_auto = TRUE;
                    ok = TRUE;
                } else {
                    ok = parse_bg_size_component(tokens[0], &w, &wu);
                }
                if (ok) {
                    if (g_ascii_strcasecmp(tokens[1], "auto") == 0) {
                        h_auto = TRUE;
                    } else if (parse_bg_size_component(tokens[1], &h, &hu)) {
                        h_auto = FALSE;
                    } else {
                        ok = FALSE;
                    }
                }
            }
            if (ok) {
                legacy_em_normalize(&w, &wu);
                legacy_em_normalize(&h, &hu);
                v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_SIZE;
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
    case NS_CSS_BACKGROUND_REPEAT: {
        char *kw = ascii_lower(t, strlen(t));
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = kw;
        break;
    }
    case NS_CSS_CONTENT: {
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
            for (const char *p = raw; *p; )
                ns_css_append_unescaped(s, &p);
            g_free(raw);
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = g_string_free(s, FALSE);
        } else if (g_str_has_prefix(t, "counter(") ||
                   g_str_has_prefix(t, "counters(") ||
                   g_str_has_prefix(t, "attr(") ||
                   strchr(t, '"') || strchr(t, '\'')) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = g_strdup(t);
        } else {
            char *kw = ascii_lower(t, strlen(t));
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
        }
        break;
    }
    case NS_CSS_COUNTER_RESET:
    case NS_CSS_COUNTER_INCREMENT:
    case NS_CSS_QUOTES: {
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = g_strstrip(g_strdup(t));
        break;
    }
    case NS_CSS_MASK_IMAGE:
    case NS_CSS_LIST_STYLE_IMAGE:
    case NS_CSS_BACKGROUND_IMAGE: {
        v = parse_any_gradient(t);
        if (!v) {
            const char *p = t;
            while (*p && is_ws(*p)) p++;
            char *iset = pick_image_set_url(p);
            if (iset) {
                v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_URL;
                v->u.url = iset;
            } else if (g_ascii_strncasecmp(p, "url(", 4) == 0) {
                const char *u = p + 4;
                while (*u && is_ws(*u)) u++;
                char q = 0;
                if (*u == '"' || *u == '\'') { q = *u; u++; }
                const char *end;
                if (q) {
                    end = css_quoted_end(u, q);
                } else {
                    end = u;
                    while (*end && *end != ')' && !is_ws(*end)) end++;
                }
                if (end && end > u) {
                    char *url = css_unescape_url(u, (gsize)(end - u));
                    v = g_new0(ns_css_value, 1);
                    v->kind = NS_CSS_V_URL;
                    v->u.url = url;
                }
            }
        }
        if (!v) {
            char *kw = ascii_lower(t, strlen(t));
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
        }
        break;
    }
    case NS_CSS_TRANSFORM: {
        v = parse_transform(t);
        if (!v) {
            char *lc = g_ascii_strdown(t, -1);
            g_strstrip(lc);
            if (strcmp(lc, "none") == 0) {
                v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_KEYWORD;
                v->u.keyword = g_strdup("none");
            }
            g_free(lc);
        }
        break;
    }
    case NS_CSS_TRANSFORM_ORIGIN:
    case NS_CSS_PERSPECTIVE_ORIGIN: {
        v = parse_transform_origin(t);
        break;
    }
    case NS_CSS_TRANSLATE: {
        v = parse_translate_prop(t);
        if (!v) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = g_strdup("none");
        }
        break;
    }
    case NS_CSS_ROTATE: {
        v = parse_rotate_prop(t);
        if (!v) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = g_strdup("none");
        }
        break;
    }
    case NS_CSS_SCALE: {
        v = parse_scale_prop(t);
        if (!v) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = g_strdup("none");
        }
        break;
    }
    case NS_CSS_PERSPECTIVE: {
        double px = 0, pct = 0;
        if (g_ascii_strncasecmp(t, "none", 4) != 0 &&
            resolve_to_px_pct(t, strlen(t), &px, &pct) && px > 0) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_LENGTH;
            v->u.length.v = px;
            v->u.length.unit = NS_CSS_UNIT_PX;
        } else {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = g_strdup("none");
        }
        break;
    }
    case NS_CSS_TRANSFORM_STYLE:
    case NS_CSS_BACKFACE_VISIBILITY:
    case NS_CSS_ANIMATION_PLAY_STATE: {
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = ascii_lower(t, strlen(t));
        break;
    }
    case NS_CSS_TRANSITION:
        v = parse_anim_value(t, FALSE);
        break;
    case NS_CSS_ANIMATION:
        v = parse_anim_value(t, TRUE);
        break;
    case NS_CSS_ASPECT_RATIO: {
        if (g_ascii_strcasecmp(t, "auto") == 0) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
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
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_LENGTH;
        v->u.length.v = a / b;
        v->u.length.unit = NS_CSS_UNIT_NUMBER;
        break;
    }
    case NS_CSS_CONTAINER_NAME: {
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = g_strdup(t);
        break;
    }
    case NS_CSS_FONT_STRETCH: {
        char *kw = ascii_lower(t, strlen(t));
        if (is_font_stretch_keyword(kw)) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
        } else {
            g_free(kw);
            double num;
            ns_css_unit u;
            if (parse_length(t, &num, &u) &&
                u == NS_CSS_UNIT_PERCENT && num > 0) {
                v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_LENGTH;
                v->u.length.v = num;
                v->u.length.unit = u;
            }
        }
        break;
    }
    case NS_CSS_FONT_KERNING: {
        char *kw = ascii_lower(t, strlen(t));
        if (strcmp(kw, "auto") == 0 ||
            strcmp(kw, "normal") == 0 ||
            strcmp(kw, "none") == 0) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
        } else {
            g_free(kw);
        }
        break;
    }
    case NS_CSS_FONT_VARIANT_LIGATURES: {
        char *kw = ascii_lower(t, strlen(t));
        if (is_font_ligatures_value(kw)) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
        } else {
            g_free(kw);
        }
        break;
    }
    case NS_CSS_FONT_FEATURE_SETTINGS: {
        char *kw = ascii_lower(t, strlen(t));
        if (strcmp(kw, "normal") == 0 || is_font_feature_settings_value(t)) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = strcmp(kw, "normal") == 0 ? kw : g_strdup(t);
            if (v->u.keyword != kw) g_free(kw);
        } else {
            g_free(kw);
        }
        break;
    }
    case NS_CSS_FONT_VARIATION_SETTINGS: {
        char *kw = ascii_lower(t, strlen(t));
        if (strcmp(kw, "normal") == 0 || is_font_variation_settings_value(t)) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = strcmp(kw, "normal") == 0 ? kw : g_strdup(t);
            if (v->u.keyword != kw) g_free(kw);
        } else {
            g_free(kw);
        }
        break;
    }
    case NS_CSS_TAB_SIZE: {
        ns_css_value *cv = parse_calc(t);
        if (cv) {
            if (cv->kind == NS_CSS_V_CALC && cv->u.calc.pct == 0) {
                double px = cv->u.calc.px +
                            (cv->u.calc.em + cv->u.calc.rem) * 16.0;
                ns_css_value_free(cv);
                cv = calc_px_value(px);
            }
            if (cv->kind == NS_CSS_V_LENGTH &&
                cv->u.length.unit != NS_CSS_UNIT_PERCENT) {
                if (cv->u.length.v < 0) cv->u.length.v = 0;
                v = cv;
            } else {
                ns_css_value_free(cv);
            }
            break;
        }
        double len; ns_css_unit u;
        if (parse_length(t, &len, &u) && u != NS_CSS_UNIT_PERCENT &&
            len >= 0) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_LENGTH;
            v->u.length.v = len;
            v->u.length.unit = u;
        }
        break;
    }
    case NS_CSS_BORDER_SPACING: {
        char *tokens[4] = {0};
        int nt = split_ws(t, tokens);
        double w = 0, h = 0;
        ns_css_unit wu = NS_CSS_UNIT_PX, hu = NS_CSS_UNIT_PX;
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
            legacy_em_normalize(&w, &wu);
            legacy_em_normalize(&h, &hu);
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_SIZE;
            v->u.size.w = w;
            v->u.size.h = h;
            v->u.size.w_unit = wu;
            v->u.size.h_unit = hu;
        }
        for (int i = 0; i < nt; i++) g_free(tokens[i]);
        break;
    }
    case NS_CSS_WHITE_SPACE: {
        char *kw = ascii_lower(t, strlen(t));
        static const char *const ok[] = { "normal", "nowrap", "pre",
            "pre-wrap", "pre-line", "break-spaces" };
        gboolean valid = FALSE;
        for (gsize i = 0; i < G_N_ELEMENTS(ok); i++)
            if (strcmp(kw, ok[i]) == 0) { valid = TRUE; break; }
        if (!valid) { g_free(kw); g_free(t); return NULL; }
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = kw;
        break;
    }
    case NS_CSS_CURSOR: {
        if (strchr(t, '(')) {
            char *kw = ascii_lower(t, strlen(t));
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
            break;
        }
        char *kw = ascii_lower(t, strlen(t));
        static const char *const ok[] = {
            "auto", "default", "none", "context-menu", "help", "pointer",
            "progress", "wait", "cell", "crosshair", "text", "vertical-text",
            "alias", "copy", "move", "no-drop", "not-allowed", "grab",
            "grabbing", "e-resize", "n-resize", "ne-resize", "nw-resize",
            "s-resize", "se-resize", "sw-resize", "w-resize", "ew-resize",
            "ns-resize", "nesw-resize", "nwse-resize", "col-resize",
            "row-resize", "all-scroll", "zoom-in", "zoom-out" };
        gboolean valid = FALSE;
        for (gsize i = 0; i < G_N_ELEMENTS(ok); i++)
            if (strcmp(kw, ok[i]) == 0) { valid = TRUE; break; }
        if (!valid) { g_free(kw); g_free(t); return NULL; }
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
        v->u.keyword = kw;
        break;
    }
    case NS_CSS_FONT_WEIGHT: {
        char *kw = ascii_lower(t, strlen(t));
        if (strcmp(kw, "normal") == 0 || strcmp(kw, "bold") == 0 ||
            strcmp(kw, "bolder") == 0 || strcmp(kw, "lighter") == 0 ||
            strstr(kw, "var(") || strstr(kw, "attr(") ||
            strstr(kw, "env(") || strchr(kw, '"') || strchr(kw, '\'')) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = kw;
            break;
        }
        g_free(kw);
        double num = 0;
        gboolean got = FALSE;
        ns_css_value *cv = parse_calc(t);
        if (cv) {
            if (cv->kind == NS_CSS_V_LENGTH &&
                cv->u.length.unit == NS_CSS_UNIT_NUMBER) {
                num = cv->u.length.v;
                got = TRUE;
            }
            ns_css_value_free(cv);
        } else {
            char *end = NULL;
            double d = g_ascii_strtod(t, &end);
            while (end && *end && is_ws(*end)) end++;
            if (end && end != t && *end == '\0') { num = d; got = TRUE; }
        }
        if (got && isfinite(num) && num >= 1 && num <= 1000) {
            v = g_new0(ns_css_value, 1);
            v->kind = NS_CSS_V_KEYWORD;
            v->u.keyword = g_strdup_printf("%g", num);
        }
        break;
    }
    default: {

        char *kw = ascii_lower(t, strlen(t));
        v = g_new0(ns_css_value, 1);
        v->kind = NS_CSS_V_KEYWORD;
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
    for (int i = 0; i < NS_CSS_PROP_COUNT; i++) {
        if (g_ascii_strcasecmp(name, kProp[i]) == 0) return i;
    }
    if (g_ascii_strcasecmp(name, "word-wrap") == 0)
        return NS_CSS_OVERFLOW_WRAP;
    if (g_ascii_strcasecmp(name, "line-clamp") == 0)
        return NS_CSS_LINE_CLAMP;
    if (g_ascii_strcasecmp(name, "text-wrap") == 0 ||
        g_ascii_strcasecmp(name, "text-wrap-mode") == 0)
        return NS_CSS_WHITE_SPACE;
    if (g_ascii_strcasecmp(name, "-webkit-mask-image") == 0 ||
        g_ascii_strcasecmp(name, "-webkit-mask") == 0 ||
        g_ascii_strcasecmp(name, "mask") == 0)
        return NS_CSS_MASK_IMAGE;
    if (g_ascii_strcasecmp(name, "-webkit-background-clip") == 0)
        return NS_CSS_BACKGROUND_CLIP;
    if (g_ascii_strcasecmp(name, "-webkit-appearance") == 0 ||
        g_ascii_strcasecmp(name, "-moz-appearance") == 0)
        return NS_CSS_APPEARANCE;
    const char *phys = alias_logical(name);
    if (phys) {
        for (int i = 0; i < NS_CSS_PROP_COUNT; i++)
            if (g_ascii_strcasecmp(phys, kProp[i]) == 0) return i;
    }
    return -1;
}

int
ns_css_prop_id(const char *name)
{
    return name ? prop_id(name) : -1;
}

gboolean
ns_css_declaration_valid(int prop, const char *text)
{
    if (prop < 0 || !text || !*text) return TRUE;
    if (strstr(text, "var(")) return TRUE;
    ns_css_value *v = parse_value_for((ns_css_prop)prop, text);
    if (!v) return FALSE;
    ns_css_value_free(v);
    return TRUE;
}

int
ns_css_font_stretch_rank(const ns_css_value *v)
{
    if (!v) return 4;
    if (v->kind == NS_CSS_V_KEYWORD && v->u.keyword) {
        const char *kw = v->u.keyword;
        if (strcmp(kw, "ultra-condensed") == 0) return 0;
        if (strcmp(kw, "extra-condensed") == 0) return 1;
        if (strcmp(kw, "condensed") == 0) return 2;
        if (strcmp(kw, "semi-condensed") == 0) return 3;
        if (strcmp(kw, "normal") == 0) return 4;
        if (strcmp(kw, "semi-expanded") == 0) return 5;
        if (strcmp(kw, "expanded") == 0) return 6;
        if (strcmp(kw, "extra-expanded") == 0) return 7;
        if (strcmp(kw, "ultra-expanded") == 0) return 8;
    }
    if (v->kind == NS_CSS_V_LENGTH &&
        v->u.length.unit == NS_CSS_UNIT_PERCENT) {
        double p = v->u.length.v;
        if (p <= 56.25) return 0;
        if (p <= 68.75) return 1;
        if (p <= 81.25) return 2;
        if (p <= 93.75) return 3;
        if (p <= 106.25) return 4;
        if (p <= 118.75) return 5;
        if (p <= 137.5) return 6;
        if (p <= 175.0) return 7;
        return 8;
    }
    return 4;
}

static void
emit_quad(GArray *decls, ns_css_prop t, ns_css_prop r,
          ns_css_prop b, ns_css_prop l,
          char *vals[4], int n, gboolean important)
{
    const char *top    = vals[0];
    const char *right  = n >= 2 ? vals[1] : top;
    const char *bottom = n >= 3 ? vals[2] : top;
    const char *left   = n >= 4 ? vals[3] : right;
    const struct { ns_css_prop p; const char *v; } map[] = {
        { t, top }, { r, right }, { b, bottom }, { l, left },
    };
    for (int i = 0; i < 4; i++) {
        ns_css_value *vv = parse_value_for(map[i].p, map[i].v);
        if (!vv) continue;
        ns_css_decl d = { .prop = map[i].p, .value = vv, .important = important };
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
substitute_var_fallbacks(const char *vtext, int depth)
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
        if (comma_term == ',') {
            char *nested = css_trim_dup_range(comma + 1, args_end);
            char *sub = substitute_var_fallbacks(nested, depth + 1);
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

typedef struct ns_var_map {
    int ref;
    GHashTable *own;
    struct ns_var_map *parent;
} ns_var_map;

static ns_var_map *
ns_var_map_new(GHashTable *own, ns_var_map *parent)
{
    ns_var_map *m = g_new0(ns_var_map, 1);
    m->ref = 1;
    m->own = own;
    m->parent = parent;
    return m;
}

static ns_var_map *
ns_var_map_ref(ns_var_map *m)
{
    if (m) m->ref++;
    return m;
}

static void
ns_var_map_unref(ns_var_map *m)
{
    while (m && --m->ref <= 0) {
        ns_var_map *parent = m->parent;
        if (m->own) g_hash_table_destroy(m->own);
        g_free(m);
        m = parent;
    }
}

const char *
ns_var_map_lookup(const ns_var_map *m, const char *name)
{
    for (; m; m = m->parent) {
        if (m->own) {
            const char *v = g_hash_table_lookup(m->own, name);
            if (v) return v;
        }
    }
    return NULL;
}

#define NS_CSS_VAR_EXPAND_MAX   ((gsize)1024 * 1024)
#define NS_CSS_VAR_EXPAND_CALLS ((guint)100000)

typedef struct {
    gsize    out_bytes;
    guint    calls;
    gboolean overflow;
} ns_var_budget;

static gboolean
var_budget_take(ns_var_budget *b, gsize n, gboolean *valid)
{
    if (b->overflow) return FALSE;
    if (n > NS_CSS_VAR_EXPAND_MAX - b->out_bytes) {
        b->overflow = TRUE;
        if (valid) *valid = FALSE;
        return FALSE;
    }
    b->out_bytes += n;
    return TRUE;
}

static char *
substitute_vars_with_valid(const char *vtext, const ns_var_map *map, int depth,
                           gboolean *valid, ns_var_budget *b)
{
    if (!vtext) return NULL;
    if (depth > 16) return g_strdup(vtext);
    if (b->overflow || ++b->calls > NS_CSS_VAR_EXPAND_CALLS) {
        b->overflow = TRUE;
        if (valid) *valid = FALSE;
        return g_strdup("");
    }
    GString *out = g_string_new(NULL);
    const char *p = vtext;
    const char *end = vtext + strlen(vtext);
    while (p < end) {
        const char *fn = css_find_function(p, end, "var");
        if (!fn) {
            if (!var_budget_take(b, (gsize)(end - p), valid)) break;
            g_string_append_len(out, p, (gssize)(end - p));
            break;
        }
        if (!var_budget_take(b, (gsize)(fn - p), valid)) break;
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
            replacement = ns_var_map_lookup(map, name);
        if (replacement && *replacement &&
            !custom_prop_value_invalid(replacement)) {
            gboolean sub_valid = TRUE;
            char *sub = substitute_vars_with_valid(replacement, map,
                                                   depth + 1, &sub_valid, b);
            if (sub_valid) {
                if (sub) g_string_append(out, sub);
            } else if (comma_term == ',') {
                char *nested = css_trim_dup_range(comma + 1, args_end);
                gboolean nested_valid = TRUE;
                char *fallback = substitute_vars_with_valid(nested, map,
                                                            depth + 1,
                                                            &nested_valid, b);
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
                                                   &nested_valid, b);
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
substitute_vars_with(const char *vtext, const ns_var_map *map, int depth)
{
    gboolean valid = TRUE;
    ns_var_budget budget = { 0, 0, FALSE };
    char *out = substitute_vars_with_valid(vtext, map, depth, &valid, &budget);
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

static gboolean
is_font_stretch_keyword(const char *s)
{
    return s &&
        (g_ascii_strcasecmp(s, "ultra-condensed") == 0 ||
         g_ascii_strcasecmp(s, "extra-condensed") == 0 ||
         g_ascii_strcasecmp(s, "condensed") == 0 ||
         g_ascii_strcasecmp(s, "semi-condensed") == 0 ||
         g_ascii_strcasecmp(s, "normal") == 0 ||
         g_ascii_strcasecmp(s, "semi-expanded") == 0 ||
         g_ascii_strcasecmp(s, "expanded") == 0 ||
         g_ascii_strcasecmp(s, "extra-expanded") == 0 ||
         g_ascii_strcasecmp(s, "ultra-expanded") == 0);
}

static gboolean
is_font_ligature_token(const char *s)
{
    return s &&
        (strcmp(s, "common-ligatures") == 0 ||
         strcmp(s, "no-common-ligatures") == 0 ||
         strcmp(s, "discretionary-ligatures") == 0 ||
         strcmp(s, "no-discretionary-ligatures") == 0 ||
         strcmp(s, "historical-ligatures") == 0 ||
         strcmp(s, "no-historical-ligatures") == 0 ||
         strcmp(s, "contextual") == 0 ||
         strcmp(s, "no-contextual") == 0);
}

static gboolean
is_font_ligatures_value(const char *s)
{
    if (!s || !*s) return FALSE;
    if (strcmp(s, "normal") == 0 || strcmp(s, "none") == 0) return TRUE;
    char **tokens = g_strsplit_set(s, " \t\r\n\f", -1);
    gboolean any = FALSE;
    gboolean ok = TRUE;
    for (int i = 0; tokens[i]; i++) {
        if (!*tokens[i]) continue;
        any = TRUE;
        if (!is_font_ligature_token(tokens[i])) {
            ok = FALSE;
            break;
        }
    }
    g_strfreev(tokens);
    return any && ok;
}

static const char *
font_feature_skip_ws(const char *p)
{
    while (*p && g_ascii_isspace((unsigned char)*p)) p++;
    return p;
}

static gboolean
font_feature_read_tag(const char **pp, char tag[5])
{
    const char *p = font_feature_skip_ws(*pp);
    if (*p != '"' && *p != '\'') return FALSE;
    char quote = *p++;
    const char *s = p;
    while (*p && *p != quote) p++;
    if (*p != quote || p - s != 4) return FALSE;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e) return FALSE;
        tag[i] = (char)c;
    }
    tag[4] = '\0';
    *pp = p + 1;
    return TRUE;
}

static gboolean
font_feature_read_optional_value(const char **pp)
{
    const char *p = font_feature_skip_ws(*pp);
    if (!*p || *p == ',') {
        *pp = p;
        return TRUE;
    }
    if (g_ascii_isalpha((unsigned char)*p)) {
        const char *s = p;
        while (g_ascii_isalpha((unsigned char)*p) || *p == '-') p++;
        char *kw = ascii_lower(s, (gsize)(p - s));
        gboolean ok = strcmp(kw, "on") == 0 || strcmp(kw, "off") == 0;
        g_free(kw);
        if (!ok) return FALSE;
        *pp = font_feature_skip_ws(p);
        return TRUE;
    }
    if (!g_ascii_isdigit((unsigned char)*p)) return FALSE;
    char *endp = NULL;
    (void)g_ascii_strtoll(p, &endp, 10);
    if (!endp || endp == p) return FALSE;
    *pp = font_feature_skip_ws(endp);
    return TRUE;
}

static gboolean
is_font_feature_settings_value(const char *s)
{
    if (!s || !*s) return FALSE;
    const char *p = font_feature_skip_ws(s);
    if (!*p) return FALSE;
    while (*p) {
        char tag[5];
        if (!font_feature_read_tag(&p, tag)) return FALSE;
        if (!font_feature_read_optional_value(&p)) return FALSE;
        if (*p == ',') {
            p = font_feature_skip_ws(p + 1);
            if (!*p) return FALSE;
            continue;
        }
        return *p == '\0';
    }
    return FALSE;
}

static gboolean
font_variation_read_value(const char **pp)
{
    const char *p = font_feature_skip_ws(*pp);
    if (!*p || *p == ',') return FALSE;
    char *endp = NULL;
    double v = g_ascii_strtod(p, &endp);
    if (!endp || endp == p || !isfinite(v)) return FALSE;
    *pp = font_feature_skip_ws(endp);
    return TRUE;
}

static gboolean
is_font_variation_settings_value(const char *s)
{
    if (!s || !*s) return FALSE;
    const char *p = font_feature_skip_ws(s);
    if (!*p) return FALSE;
    while (*p) {
        char tag[5];
        if (!font_feature_read_tag(&p, tag)) return FALSE;
        if (!font_variation_read_value(&p)) return FALSE;
        if (*p == ',') {
            p = font_feature_skip_ws(p + 1);
            if (!*p) return FALSE;
            continue;
        }
        return *p == '\0';
    }
    return FALSE;
}

static void
parse_declaration_block(const char **pp, const char *end,
                        GArray *decls_out, ns_css_rule *capture)
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
        char *pname;
        if (name[0] == '-' && name[1] == '-') {
            pname = name;
        } else {
            pname = ascii_lower(name, strlen(name));
            g_free(name);
        }
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
                                               sizeof(ns_css_pending_decl));
                g_array_set_clear_func(capture->pending, pending_decl_clear);
            }
            gboolean is_important = FALSE;
            css_strip_important(raw_vtext, &is_important);
            ns_css_pending_decl pd = {
                .pname = pname,
                .raw_vtext = raw_vtext,
                .important = is_important,
            };
            g_array_append_val(capture->pending, pd);
            if (p < end && *p == ';') p++;
            continue;
        }

        char *vtext = substitute_var_fallbacks(raw_vtext, 0);
        g_free(raw_vtext);
        gboolean important = FALSE;
        css_strip_important(vtext, &important);

        static const struct { const char *name; ns_css_prop t,r,b,l; } border_sides[] = {
            { "border-top",    NS_CSS_BORDER_TOP_WIDTH,    NS_CSS_BORDER_TOP_COLOR,
                               NS_CSS_BORDER_TOP_STYLE,    NS_CSS_PROP_COUNT },
            { "border-right",  NS_CSS_BORDER_RIGHT_WIDTH,  NS_CSS_BORDER_RIGHT_COLOR,
                               NS_CSS_BORDER_RIGHT_STYLE,  NS_CSS_PROP_COUNT },
            { "border-bottom", NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_BOTTOM_COLOR,
                               NS_CSS_BORDER_BOTTOM_STYLE, NS_CSS_PROP_COUNT },
            { "border-left",   NS_CSS_BORDER_LEFT_WIDTH,   NS_CSS_BORDER_LEFT_COLOR,
                               NS_CSS_BORDER_LEFT_STYLE,   NS_CSS_PROP_COUNT },
            { "border-inline-start", NS_CSS_BORDER_LEFT_WIDTH,   NS_CSS_BORDER_LEFT_COLOR,
                                      NS_CSS_BORDER_LEFT_STYLE,   NS_CSS_PROP_COUNT },
            { "border-inline-end",   NS_CSS_BORDER_RIGHT_WIDTH,  NS_CSS_BORDER_RIGHT_COLOR,
                                      NS_CSS_BORDER_RIGHT_STYLE,  NS_CSS_PROP_COUNT },
            { "border-block-start",  NS_CSS_BORDER_TOP_WIDTH,    NS_CSS_BORDER_TOP_COLOR,
                                      NS_CSS_BORDER_TOP_STYLE,    NS_CSS_PROP_COUNT },
            { "border-block-end",    NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_BOTTOM_COLOR,
                                      NS_CSS_BORDER_BOTTOM_STYLE, NS_CSS_PROP_COUNT },
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
            gboolean saw_color = FALSE, saw_width = FALSE, saw_style = FALSE;
            for (int i = 0; i < n; i++) {
                guint8 r, g, b, a;
                double num; ns_css_unit u;
                if (parse_color(tokens[i], &r, &g, &b, &a) ||
                    is_color_keyword(tokens[i])) {
                    saw_color = TRUE;
                    if (is_border_side) {
                        ns_css_value *v = parse_value_for(border_sides[side_idx].r, tokens[i]);
                        if (v) {
                            ns_css_decl d = { .prop = border_sides[side_idx].r, .value = v, .important = important };
                            g_array_append_val(decls_out, d);
                        }
                    } else {
                        char *quad[4] = { tokens[i], tokens[i], tokens[i], tokens[i] };
                        emit_quad(decls_out,
                            NS_CSS_BORDER_TOP_COLOR, NS_CSS_BORDER_RIGHT_COLOR,
                            NS_CSS_BORDER_BOTTOM_COLOR, NS_CSS_BORDER_LEFT_COLOR,
                            quad, 4, important);
                    }
                } else if (parse_length(tokens[i], &num, &u) ||
                           g_ascii_strcasecmp(tokens[i], "thin") == 0 ||
                           g_ascii_strcasecmp(tokens[i], "medium") == 0 ||
                           g_ascii_strcasecmp(tokens[i], "thick") == 0) {
                    saw_width = TRUE;
                    if (is_border_side) {
                        ns_css_value *v = parse_value_for(border_sides[side_idx].t, tokens[i]);
                        if (v) {
                            ns_css_decl d = { .prop = border_sides[side_idx].t, .value = v, .important = important };
                            g_array_append_val(decls_out, d);
                        }
                    } else {
                        char *quad[4] = { tokens[i], tokens[i], tokens[i], tokens[i] };
                        emit_quad(decls_out,
                            NS_CSS_BORDER_TOP_WIDTH, NS_CSS_BORDER_RIGHT_WIDTH,
                            NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_LEFT_WIDTH,
                            quad, 4, important);
                    }
                } else {
                    saw_style = TRUE;
                    if (is_border_side) {
                        ns_css_value *v = parse_value_for(border_sides[side_idx].b, tokens[i]);
                        if (v) {
                            ns_css_decl d = { .prop = border_sides[side_idx].b, .value = v, .important = important };
                            g_array_append_val(decls_out, d);
                        }
                    } else {
                        char *quad[4] = { tokens[i], tokens[i], tokens[i], tokens[i] };
                        emit_quad(decls_out,
                            NS_CSS_BORDER_TOP_STYLE, NS_CSS_BORDER_RIGHT_STYLE,
                            NS_CSS_BORDER_BOTTOM_STYLE, NS_CSS_BORDER_LEFT_STYLE,
                            quad, 4, important);
                    }
                }
            }
            if (n > 0 && !(saw_color && saw_width && saw_style)) {
                static const struct { ns_css_prop t, r, b, l; const char *def; }
                    border_initials[3] = {
                    { NS_CSS_BORDER_TOP_COLOR, NS_CSS_BORDER_RIGHT_COLOR,
                      NS_CSS_BORDER_BOTTOM_COLOR, NS_CSS_BORDER_LEFT_COLOR,
                      "currentcolor" },
                    { NS_CSS_BORDER_TOP_WIDTH, NS_CSS_BORDER_RIGHT_WIDTH,
                      NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_LEFT_WIDTH,
                      "medium" },
                    { NS_CSS_BORDER_TOP_STYLE, NS_CSS_BORDER_RIGHT_STYLE,
                      NS_CSS_BORDER_BOTTOM_STYLE, NS_CSS_BORDER_LEFT_STYLE,
                      "none" },
                };
                gboolean seen[3] = { saw_color, saw_width, saw_style };
                for (int k = 0; k < 3; k++) {
                    if (seen[k]) continue;
                    if (is_border_side) {
                        ns_css_prop sp = k == 0 ? border_sides[side_idx].r
                                       : k == 1 ? border_sides[side_idx].t
                                                : border_sides[side_idx].b;
                        ns_css_value *v = parse_value_for(sp, border_initials[k].def);
                        if (v) {
                            ns_css_decl d = { .prop = sp, .value = v, .important = important };
                            g_array_append_val(decls_out, d);
                        }
                    } else {
                        char *q = (char *)border_initials[k].def;
                        char *quad[4] = { q, q, q, q };
                        emit_quad(decls_out, border_initials[k].t, border_initials[k].r,
                                  border_initials[k].b, border_initials[k].l,
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
            ns_css_prop w1 = is_block ? NS_CSS_BORDER_TOP_WIDTH : NS_CSS_BORDER_LEFT_WIDTH;
            ns_css_prop w2 = is_block ? NS_CSS_BORDER_BOTTOM_WIDTH : NS_CSS_BORDER_RIGHT_WIDTH;
            ns_css_prop c1 = is_block ? NS_CSS_BORDER_TOP_COLOR : NS_CSS_BORDER_LEFT_COLOR;
            ns_css_prop c2 = is_block ? NS_CSS_BORDER_BOTTOM_COLOR : NS_CSS_BORDER_RIGHT_COLOR;
            ns_css_prop s1 = is_block ? NS_CSS_BORDER_TOP_STYLE : NS_CSS_BORDER_LEFT_STYLE;
            ns_css_prop s2 = is_block ? NS_CSS_BORDER_BOTTOM_STYLE : NS_CSS_BORDER_RIGHT_STYLE;
            char *tokens[4] = {0};
            int n = split_ws_limit(vtext, tokens, G_N_ELEMENTS(tokens));
            for (int i = 0; i < n; i++) {
                guint8 r, g, b, a;
                double num; ns_css_unit u;
                ns_css_prop p1, p2;
                if (parse_color(tokens[i], &r, &g, &b, &a) ||
                    is_color_keyword(tokens[i])) {
                    p1 = c1; p2 = c2;
                } else if (parse_length(tokens[i], &num, &u)) {
                    p1 = w1; p2 = w2;
                } else {
                    p1 = s1; p2 = s2;
                }
                ns_css_value *v1 = parse_value_for(p1, tokens[i]);
                ns_css_value *v2 = parse_value_for(p2, tokens[i]);
                if (v1) {
                    ns_css_decl d = { .prop = p1, .value = v1, .important = important };
                    g_array_append_val(decls_out, d);
                }
                if (v2) {
                    ns_css_decl d = { .prop = p2, .value = v2, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        static const struct { const char *name; ns_css_prop a,b; } border_pair_props[] = {
            { "border-block-width",  NS_CSS_BORDER_TOP_WIDTH,    NS_CSS_BORDER_BOTTOM_WIDTH },
            { "border-inline-width", NS_CSS_BORDER_LEFT_WIDTH,   NS_CSS_BORDER_RIGHT_WIDTH },
            { "border-block-style",  NS_CSS_BORDER_TOP_STYLE,    NS_CSS_BORDER_BOTTOM_STYLE },
            { "border-inline-style", NS_CSS_BORDER_LEFT_STYLE,   NS_CSS_BORDER_RIGHT_STYLE },
            { "border-block-color",  NS_CSS_BORDER_TOP_COLOR,    NS_CSS_BORDER_BOTTOM_COLOR },
            { "border-inline-color", NS_CSS_BORDER_LEFT_COLOR,   NS_CSS_BORDER_RIGHT_COLOR },
            { NULL, NS_CSS_PROP_COUNT, NS_CSS_PROP_COUNT },
        };
        gboolean border_pair_prop = FALSE;
        for (int i = 0; border_pair_props[i].name; i++) {
            if (strcmp(pname, border_pair_props[i].name) != 0) continue;
            char *tokens[4] = {0};
            int n = split_ws(vtext, tokens);
            if (n > 0) {
                const char *a = tokens[0];
                const char *b = n >= 2 ? tokens[1] : a;
                ns_css_value *va = parse_value_for(border_pair_props[i].a, a);
                ns_css_value *vb = parse_value_for(border_pair_props[i].b, b);
                if (va) {
                    ns_css_decl d = { .prop = border_pair_props[i].a, .value = va, .important = important };
                    g_array_append_val(decls_out, d);
                }
                if (vb) {
                    ns_css_decl d = { .prop = border_pair_props[i].b, .value = vb, .important = important };
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

        if (strcmp(pname, "overflow") == 0) {
            char *tokens[3] = {0};
            int n = split_ws_limit(vtext, tokens, G_N_ELEMENTS(tokens));
            if (n == 2) {
                ns_css_value *vx = parse_value_for(NS_CSS_OVERFLOW_X, tokens[0]);
                ns_css_value *vy = parse_value_for(NS_CSS_OVERFLOW_Y, tokens[1]);
                if (vx) {
                    ns_css_decl d = { .prop = NS_CSS_OVERFLOW_X, .value = vx, .important = important };
                    g_array_append_val(decls_out, d);
                }
                if (vy) {
                    ns_css_decl d = { .prop = NS_CSS_OVERFLOW_Y, .value = vy, .important = important };
                    g_array_append_val(decls_out, d);
                }
                for (int i = 0; i < n; i++) g_free(tokens[i]);
                g_free(pname);
                g_free(vtext);
                if (p < end && *p == ';') p++;
                continue;
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
        }

        static const struct { const char *name; ns_css_prop prop; } prop_aliases[] = {
            { "grid-row-gap", NS_CSS_ROW_GAP },
            { "grid-column-gap", NS_CSS_COLUMN_GAP },
            { "-webkit-user-select", NS_CSS_USER_SELECT },
            { "-moz-user-select", NS_CSS_USER_SELECT },
            { "inline-size", NS_CSS_WIDTH },
            { "block-size", NS_CSS_HEIGHT },
            { "min-inline-size", NS_CSS_MIN_WIDTH },
            { "max-inline-size", NS_CSS_MAX_WIDTH },
            { "min-block-size", NS_CSS_MIN_HEIGHT },
            { "max-block-size", NS_CSS_MAX_HEIGHT },
            { "margin-inline-start", NS_CSS_MARGIN_LEFT },
            { "margin-inline-end", NS_CSS_MARGIN_RIGHT },
            { "margin-block-start", NS_CSS_MARGIN_TOP },
            { "margin-block-end", NS_CSS_MARGIN_BOTTOM },
            { "padding-inline-start", NS_CSS_PADDING_LEFT },
            { "padding-inline-end", NS_CSS_PADDING_RIGHT },
            { "padding-block-start", NS_CSS_PADDING_TOP },
            { "padding-block-end", NS_CSS_PADDING_BOTTOM },
            { "inset-inline-start", NS_CSS_LEFT },
            { "inset-inline-end", NS_CSS_RIGHT },
            { "inset-block-start", NS_CSS_TOP },
            { "inset-block-end", NS_CSS_BOTTOM },
            { "border-inline-start-width", NS_CSS_BORDER_LEFT_WIDTH },
            { "border-inline-end-width", NS_CSS_BORDER_RIGHT_WIDTH },
            { "border-block-start-width", NS_CSS_BORDER_TOP_WIDTH },
            { "border-block-end-width", NS_CSS_BORDER_BOTTOM_WIDTH },
            { "border-inline-start-style", NS_CSS_BORDER_LEFT_STYLE },
            { "border-inline-end-style", NS_CSS_BORDER_RIGHT_STYLE },
            { "border-block-start-style", NS_CSS_BORDER_TOP_STYLE },
            { "border-block-end-style", NS_CSS_BORDER_BOTTOM_STYLE },
            { "border-inline-start-color", NS_CSS_BORDER_LEFT_COLOR },
            { "border-inline-end-color", NS_CSS_BORDER_RIGHT_COLOR },
            { "border-block-start-color", NS_CSS_BORDER_TOP_COLOR },
            { "border-block-end-color", NS_CSS_BORDER_BOTTOM_COLOR },
            { "border-start-start-radius", NS_CSS_BORDER_TOP_LEFT_RADIUS },
            { "border-start-end-radius", NS_CSS_BORDER_TOP_RIGHT_RADIUS },
            { "border-end-start-radius", NS_CSS_BORDER_BOTTOM_LEFT_RADIUS },
            { "border-end-end-radius", NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS },
            { NULL, NS_CSS_PROP_COUNT },
        };
        gboolean aliased_prop = FALSE;
        for (int i = 0; prop_aliases[i].name; i++) {
            if (strcmp(pname, prop_aliases[i].name) != 0) continue;
            ns_css_value *vv = parse_value_for(prop_aliases[i].prop, vtext);
            if (vv) {
                ns_css_decl d = {
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
                ns_css_value *gv = parse_any_gradient(gtext);
                if (gv) {
                    ns_css_decl d = {
                        .prop = NS_CSS_BACKGROUND_IMAGE,
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
                    ns_css_value *uv = parse_value_for(NS_CSS_BACKGROUND_IMAGE, vu);
                    if (uv && uv->kind == NS_CSS_V_URL) {
                        ns_css_decl d = {
                            .prop = NS_CSS_BACKGROUND_IMAGE,
                            .value = uv,
                            .important = important,
                        };
                        g_array_append_val(decls_out, d);
                    } else {
                        ns_css_value_free(uv);
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
                        ns_css_value *v = g_new0(ns_css_value, 1);
                        v->kind = NS_CSS_V_COLOR;
                        v->u.color.r = r; v->u.color.g = g;
                        v->u.color.b = b; v->u.color.a = a;
                        ns_css_decl d = { .prop = NS_CSS_BACKGROUND_COLOR,
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
                    ns_css_value *v = g_new0(ns_css_value, 1);
                    v->kind = NS_CSS_V_COLOR;
                    v->u.color.r = r; v->u.color.g = g;
                    v->u.color.b = b; v->u.color.a = a;
                    ns_css_decl decl = {
                        .prop = NS_CSS_BACKGROUND_COLOR,
                        .value = v,
                        .important = important,
                    };
                    g_array_append_val(decls_out, decl);
                    break;
                }
                if (is_color_keyword(tokens[i])) {
                    ns_css_value *v = parse_value_for(NS_CSS_BACKGROUND_COLOR, tokens[i]);
                    if (v) {
                        ns_css_decl decl = {
                            .prop = NS_CSS_BACKGROUND_COLOR,
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
                        ns_css_value *pv = NULL;
                        if (i + 2 < n) {
                            pair = g_strdup_printf("%s %s", tokens[i + 1], tokens[i + 2]);
                            pv = parse_value_for(NS_CSS_BACKGROUND_SIZE, pair);
                        }
                        if (pv) {
                            ns_css_value_free(pv);
                            bg_size_text = pair;
                            bg_size_skip = i + 2;
                        } else {
                            g_free(pair);
                            bg_size_text = g_strdup(tokens[i + 1]);
                            bg_size_skip = i + 1;
                        }
                        i = bg_size_skip;
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
                            ns_css_value *v = parse_value_for(
                                pos_x ? NS_CSS_BACKGROUND_POSITION_Y
                                      : NS_CSS_BACKGROUND_POSITION_X,
                                pre);
                            if (v && v->kind == NS_CSS_V_LENGTH) {
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
                            ns_css_value_free(v);
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
                    ns_css_value *v = parse_value_for(NS_CSS_BACKGROUND_REPEAT, tk);
                    if (v) {
                        ns_css_decl d = { .prop = NS_CSS_BACKGROUND_REPEAT, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                } else if (g_ascii_strcasecmp(tk, "cover") == 0 ||
                           g_ascii_strcasecmp(tk, "contain") == 0) {
                    ns_css_value *v = parse_value_for(NS_CSS_BACKGROUND_SIZE, tk);
                    if (v) {
                        ns_css_decl d = { .prop = NS_CSS_BACKGROUND_SIZE, .value = v, .important = important };
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
                    ns_css_value *v = parse_value_for(
                        pos_x ? NS_CSS_BACKGROUND_POSITION_Y
                              : NS_CSS_BACKGROUND_POSITION_X,
                        tk);
                    if (v && v->kind == NS_CSS_V_LENGTH) {
                        if (!pos_x) pos_x = tk;
                        else if (!pos_y) pos_y = tk;
                    }
                    ns_css_value_free(v);
                }
            }
            if (bg_size_text) {
                ns_css_value *v = parse_value_for(NS_CSS_BACKGROUND_SIZE, bg_size_text);
                if (v) {
                    ns_css_decl d = { .prop = NS_CSS_BACKGROUND_SIZE, .value = v, .important = important };
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
                ns_css_value *vx =
                    parse_value_for(NS_CSS_BACKGROUND_POSITION_X, pos_x);
                if (vx) {
                    ns_css_decl d = { .prop = NS_CSS_BACKGROUND_POSITION_X,
                                      .value = vx, .important = important };
                    g_array_append_val(decls_out, d);
                }
                ns_css_value *vy =
                    parse_value_for(NS_CSS_BACKGROUND_POSITION_Y, pos_y);
                if (vy) {
                    ns_css_decl d = { .prop = NS_CSS_BACKGROUND_POSITION_Y,
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
            ns_css_value *vx_head = NULL, *vx_tail = NULL;
            ns_css_value *vy_head = NULL, *vy_tail = NULL;
            const char *sp = vtext;
            const char *send = vtext + strlen(vtext);
            while (sp < send) {
                char sterm = 0;
                const char *sseg = css_scan_until(sp, send, ",", &sterm);
                char *layer = css_trim_dup_range(sp, sseg);
                sp = sterm == ',' ? sseg + 1 : sseg;
                char *tokens[4] = {0};
                int n = split_ws(layer, tokens);
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
                    ns_css_value *v = parse_value_for(NS_CSS_BACKGROUND_POSITION_X, xs);
                    if (v) {
                        if (vx_tail) vx_tail->next_layer = v;
                        else vx_head = v;
                        vx_tail = v;
                    }
                }
                if (ys) {
                    ns_css_value *v = parse_value_for(NS_CSS_BACKGROUND_POSITION_Y, ys);
                    if (v) {
                        if (vy_tail) vy_tail->next_layer = v;
                        else vy_head = v;
                        vy_tail = v;
                    }
                }
                for (int i = 0; i < n; i++) g_free(tokens[i]);
                g_free(layer);
            }
            if (vx_head) {
                ns_css_decl d = { .prop = NS_CSS_BACKGROUND_POSITION_X, .value = vx_head, .important = important };
                g_array_append_val(decls_out, d);
            }
            if (vy_head) {
                ns_css_decl d = { .prop = NS_CSS_BACKGROUND_POSITION_Y, .value = vy_head, .important = important };
                g_array_append_val(decls_out, d);
            }
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
                ns_css_value *v = parse_value_for(NS_CSS_OBJECT_POSITION_X, xs);
                if (v) {
                    ns_css_decl d = { .prop = NS_CSS_OBJECT_POSITION_X, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (ys) {
                ns_css_value *v = parse_value_for(NS_CSS_OBJECT_POSITION_Y, ys);
                if (v) {
                    ns_css_decl d = { .prop = NS_CSS_OBJECT_POSITION_Y, .value = v, .important = important };
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
                ns_css_value *v = parse_value_for(NS_CSS_GRID_TEMPLATE_AREAS, areas_acc);
                if (v) {
                    ns_css_decl d = { .prop = NS_CSS_GRID_TEMPLATE_AREAS,
                                      .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (strcmp(pname, "grid") == 0) {
                char *cols_flow_probe = cols_trim
                    ? g_ascii_strdown(cols_trim, -1) : NULL;
                if (cols_trim && cols_flow_probe &&
                    strstr(cols_flow_probe, "auto-flow")) {
                    char *tokens[8] = {0};
                    int n = split_ws_limit(cols_trim, tokens, G_N_ELEMENTS(tokens));
                    GString *flow = g_string_new("column");
                    GString *tracks = g_string_new(NULL);
                    for (int i = 0; i < n; i++) {
                        if (g_ascii_strcasecmp(tokens[i], "auto-flow") == 0)
                            continue;
                        if (g_ascii_strcasecmp(tokens[i], "dense") == 0) {
                            g_string_append(flow, " dense");
                            continue;
                        }
                        if (tracks->len) g_string_append_c(tracks, ' ');
                        g_string_append(tracks, tokens[i]);
                    }
                    ns_css_value *fv = parse_value_for(NS_CSS_GRID_AUTO_FLOW,
                                                       flow->str);
                    if (fv) {
                        ns_css_decl d = { .prop = NS_CSS_GRID_AUTO_FLOW,
                                          .value = fv, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                    ns_css_value *tv = parse_value_for(NS_CSS_GRID_AUTO_COLUMNS,
                                                       tracks->len ? tracks->str : "auto");
                    if (tv) {
                        ns_css_decl d = { .prop = NS_CSS_GRID_AUTO_COLUMNS,
                                          .value = tv, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                    for (int i = 0; i < n; i++) g_free(tokens[i]);
                    g_string_free(flow, TRUE);
                    g_string_free(tracks, TRUE);
                    g_clear_pointer(&cols_trim, g_free);
                }
                g_free(cols_flow_probe);
                char *rows_flow_probe = rows_trim
                    ? g_ascii_strdown(rows_trim, -1) : NULL;
                if (rows_trim && rows_flow_probe &&
                    strstr(rows_flow_probe, "auto-flow")) {
                    char *tokens[8] = {0};
                    int n = split_ws_limit(rows_trim, tokens, G_N_ELEMENTS(tokens));
                    GString *flow = g_string_new("row");
                    GString *tracks = g_string_new(NULL);
                    for (int i = 0; i < n; i++) {
                        if (g_ascii_strcasecmp(tokens[i], "auto-flow") == 0)
                            continue;
                        if (g_ascii_strcasecmp(tokens[i], "dense") == 0) {
                            g_string_append(flow, " dense");
                            continue;
                        }
                        if (tracks->len) g_string_append_c(tracks, ' ');
                        g_string_append(tracks, tokens[i]);
                    }
                    ns_css_value *fv = parse_value_for(NS_CSS_GRID_AUTO_FLOW,
                                                       flow->str);
                    if (fv) {
                        ns_css_decl d = { .prop = NS_CSS_GRID_AUTO_FLOW,
                                          .value = fv, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                    ns_css_value *tv = parse_value_for(NS_CSS_GRID_AUTO_ROWS,
                                                       tracks->len ? tracks->str : "auto");
                    if (tv) {
                        ns_css_decl d = { .prop = NS_CSS_GRID_AUTO_ROWS,
                                          .value = tv, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                    for (int i = 0; i < n; i++) g_free(tokens[i]);
                    g_string_free(flow, TRUE);
                    g_string_free(tracks, TRUE);
                    g_clear_pointer(&rows_trim, g_free);
                }
                g_free(rows_flow_probe);
            }
            if (rows_trim && *rows_trim) {
                ns_css_value *v = parse_value_for(NS_CSS_GRID_TEMPLATE_ROWS, rows_trim);
                if (v) {
                    ns_css_decl d = { .prop = NS_CSS_GRID_TEMPLATE_ROWS,
                                      .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (cols_trim && *cols_trim) {
                ns_css_value *v = parse_value_for(NS_CSS_GRID_TEMPLATE_COLUMNS, cols_trim);
                if (v) {
                    ns_css_decl d = { .prop = NS_CSS_GRID_TEMPLATE_COLUMNS,
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
                ns_css_value *v = parse_value_for(NS_CSS_ROW_GAP, row);
                if (v) {
                    ns_css_decl d = { .prop = NS_CSS_ROW_GAP, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (col) {
                ns_css_value *v = parse_value_for(NS_CSS_COLUMN_GAP, col);
                if (v) {
                    ns_css_decl d = { .prop = NS_CSS_COLUMN_GAP, .value = v, .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
            g_free(pname);
            g_free(vtext);
            if (p < end && *p == ';') p++;
            continue;
        }

        if (strcmp(pname, "grid-column") == 0 ||
            strcmp(pname, "grid-row") == 0) {
            gboolean is_col = pname[5] == 'c';
            ns_css_prop sp_prop = is_col ? NS_CSS_GRID_COLUMN_START
                                         : NS_CSS_GRID_ROW_START;
            ns_css_prop ep_prop = is_col ? NS_CSS_GRID_COLUMN_END
                                         : NS_CSS_GRID_ROW_END;
            const char *gv_end = vtext + strlen(vtext);
            const char *slash = css_find_top_level_char(vtext, gv_end, '/');
            char *first = css_trim_dup_range(vtext, slash ? slash : gv_end);
            char *second = slash ? css_trim_dup_range(slash + 1, gv_end) : NULL;
            if (*first) {
                ns_css_value *v = parse_value_for(sp_prop, first);
                if (v) {
                    ns_css_decl d = { .prop = sp_prop, .value = v,
                                      .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            if (second && *second) {
                ns_css_value *v = parse_value_for(ep_prop, second);
                if (v) {
                    ns_css_decl d = { .prop = ep_prop, .value = v,
                                      .important = important };
                    g_array_append_val(decls_out, d);
                }
            }
            g_free(first);
            g_free(second);
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
                    ns_css_value *v = parse_value_for(NS_CSS_ALIGN_CONTENT, first);
                    if (v) {
                        ns_css_decl d = { .prop = NS_CSS_ALIGN_CONTENT, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                }
                if (second) {
                    ns_css_value *v = parse_value_for(NS_CSS_JUSTIFY_CONTENT, second);
                    if (v) {
                        ns_css_decl d = { .prop = NS_CSS_JUSTIFY_CONTENT, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                }
            } else {
                gboolean is_items = (strcmp(pname, "place-items") == 0);
                ns_css_prop ap = is_items ? NS_CSS_ALIGN_ITEMS : NS_CSS_ALIGN_SELF;
                ns_css_prop jp = is_items ? NS_CSS_JUSTIFY_ITEMS
                                          : NS_CSS_JUSTIFY_SELF;
                if (first) {
                    ns_css_value *v = parse_value_for(ap, first);
                    if (v) {
                        ns_css_decl d = { .prop = ap, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                }
                if (second) {
                    ns_css_value *v = parse_value_for(jp, second);
                    if (v) {
                        ns_css_decl d = { .prop = jp, .value = v, .important = important };
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
                double num; ns_css_unit u;
                if (parse_length(tokens[i], &num, &u)) {
                    ns_css_prop prop = (u == NS_CSS_UNIT_NUMBER)
                        ? NS_CSS_COLUMN_COUNT : NS_CSS_COLUMN_WIDTH;
                    ns_css_value *v = parse_value_for(prop, tokens[i]);
                    if (v) {
                        ns_css_decl d = { .prop = prop, .value = v,
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
            ns_css_prop p_w = is_outline ? NS_CSS_OUTLINE_WIDTH : NS_CSS_COLUMN_RULE_WIDTH;
            ns_css_prop p_s = is_outline ? NS_CSS_OUTLINE_STYLE : NS_CSS_COLUMN_RULE_STYLE;
            ns_css_prop p_c = is_outline ? NS_CSS_OUTLINE_COLOR : NS_CSS_COLUMN_RULE_COLOR;
            char *tokens[8] = {0};
            int n = split_ws(vtext, tokens);
            for (int i = 0; i < n; i++) {
                guint8 r, g, b, a;
                double num; ns_css_unit u;
                if (parse_color(tokens[i], &r, &g, &b, &a)) {
                    ns_css_value *v = parse_value_for(p_c, tokens[i]);
                    if (v) {
                        ns_css_decl d = { .prop = p_c, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                } else if (parse_length(tokens[i], &num, &u)) {
                    ns_css_value *v = parse_value_for(p_w, tokens[i]);
                    if (v) {
                        ns_css_decl d = { .prop = p_w, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                } else {
                    ns_css_value *v = parse_value_for(p_s, tokens[i]);
                    if (v) {
                        ns_css_decl d = { .prop = p_s, .value = v, .important = important };
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

        if (strcmp(pname, "text-decoration") == 0 ||
            strcmp(pname, "text-decoration-line") == 0) {
            gboolean line_only = strcmp(pname, "text-decoration-line") == 0;
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
                } else if (line_only) {
                    continue;
                } else if (g_ascii_strcasecmp(tk, "solid")  == 0 ||
                           g_ascii_strcasecmp(tk, "double") == 0 ||
                           g_ascii_strcasecmp(tk, "dotted") == 0 ||
                           g_ascii_strcasecmp(tk, "dashed") == 0 ||
                           g_ascii_strcasecmp(tk, "wavy")   == 0) {
                    ns_css_value *v = g_new0(ns_css_value, 1);
                    v->kind = NS_CSS_V_KEYWORD;
                    v->u.keyword = g_ascii_strdown(tk, -1);
                    ns_css_decl d = {
                        .prop = NS_CSS_TEXT_DECORATION_STYLE,
                        .value = v, .important = important
                    };
                    g_array_append_val(decls_out, d);
                } else if (parse_color(tk, &cr, &cg, &cb, &ca)) {
                    ns_css_value *v = g_new0(ns_css_value, 1);
                    v->kind = NS_CSS_V_COLOR;
                    v->u.color.r = cr; v->u.color.g = cg;
                    v->u.color.b = cb; v->u.color.a = ca;
                    ns_css_decl d = {
                        .prop = NS_CSS_TEXT_DECORATION_COLOR,
                        .value = v, .important = important
                    };
                    g_array_append_val(decls_out, d);
                }
            }
            if (lines->len > 0) {
                ns_css_value *v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_KEYWORD;
                v->u.keyword = g_string_free(lines, FALSE);
                lines = NULL;
                ns_css_decl d = {
                    .prop = NS_CSS_TEXT_DECORATION,
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
            ns_css_value *wide = parse_css_wide_keyword(vtext);
            if (wide) {
                const ns_css_prop props[] = {
                    NS_CSS_FONT_STYLE,
                    NS_CSS_FONT_VARIANT,
                    NS_CSS_FONT_WEIGHT,
                    NS_CSS_FONT_STRETCH,
                    NS_CSS_FONT_KERNING,
                    NS_CSS_FONT_VARIANT_LIGATURES,
                    NS_CSS_FONT_FEATURE_SETTINGS,
                    NS_CSS_FONT_VARIATION_SETTINGS,
                    NS_CSS_FONT_SIZE,
                    NS_CSS_LINE_HEIGHT,
                    NS_CSS_FONT_FAMILY,
                };
                for (gsize i = 0; i < G_N_ELEMENTS(props); i++) {
                    ns_css_decl d = {
                        .prop = props[i],
                        .value = ns_css_value_dup(wide),
                        .important = important
                    };
                    g_array_append_val(decls_out, d);
                }
                ns_css_value_free(wide);
                g_free(pname);
                g_free(vtext);
                if (p < end && *p == ';') p++;
                continue;
            }
            char *tokens[16] = {0};
            int n = split_ws_limit(vtext, tokens, (int)G_N_ELEMENTS(tokens));
            char *family_buf = NULL;
            int size_idx = -1;
            for (int i = 0; i < n; i++) {
                double num, lh;
                ns_css_unit u, lu;
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
                ns_css_prop prop = NS_CSS_PROP_COUNT;
                const char *kw = NULL;
                if (g_ascii_strcasecmp(t, "italic") == 0 ||
                    g_ascii_strcasecmp(t, "oblique") == 0) {
                    prop = NS_CSS_FONT_STYLE; kw = "italic";
                } else if (g_ascii_strcasecmp(t, "bold")    == 0 ||
                           g_ascii_strcasecmp(t, "bolder")  == 0 ||
                           g_ascii_strcasecmp(t, "lighter") == 0) {
                    prop = NS_CSS_FONT_WEIGHT; kw = t;
                } else if (g_ascii_isdigit(t[0])) {
                    double num; ns_css_unit u;
                    if (parse_length(t, &num, &u) &&
                        u == NS_CSS_UNIT_NUMBER &&
                        num >= 100 && num <= 900) {
                        prop = NS_CSS_FONT_WEIGHT; kw = t;
                    }
                } else if (g_ascii_strcasecmp(t, "small-caps") == 0) {
                    prop = NS_CSS_FONT_VARIANT; kw = "small-caps";
                } else if (is_font_stretch_keyword(t)) {
                    prop = NS_CSS_FONT_STRETCH; kw = t;
                }
                if (prop != NS_CSS_PROP_COUNT) {
                    ns_css_value *v = g_new0(ns_css_value, 1);
                    v->kind = NS_CSS_V_KEYWORD;
                    v->u.keyword = g_ascii_strdown(kw, -1);
                    ns_css_decl d = {
                        .prop = prop, .value = v, .important = important
                    };
                    g_array_append_val(decls_out, d);
                }
            }
            if (size_idx >= 0) {
                char *size_tok = tokens[size_idx];
                double num = 0, lh = 0;
                ns_css_unit u = NS_CSS_UNIT_PX, lu = NS_CSS_UNIT_NUMBER;
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
                ns_css_value *v = g_new0(ns_css_value, 1);
                v->kind = NS_CSS_V_LENGTH;
                v->u.length.v = num;
                v->u.length.unit = u;
                ns_css_decl d = {
                    .prop = NS_CSS_FONT_SIZE, .value = v,
                    .important = important
                };
                g_array_append_val(decls_out, d);
                if (has_lh) {
                    ns_css_value *lv = g_new0(ns_css_value, 1);
                    lv->kind = NS_CSS_V_LENGTH;
                    lv->u.length.v = lh;
                    lv->u.length.unit = lu;
                    ns_css_decl lhd = {
                        .prop = NS_CSS_LINE_HEIGHT,
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
                static const struct {
                    ns_css_prop prop;
                    const char *value;
                } reset_props[] = {
                    { NS_CSS_FONT_KERNING, "auto" },
                    { NS_CSS_FONT_VARIANT_LIGATURES, "normal" },
                    { NS_CSS_FONT_FEATURE_SETTINGS, "normal" },
                    { NS_CSS_FONT_VARIATION_SETTINGS, "normal" },
                };
                for (gsize j = 0; j < G_N_ELEMENTS(reset_props); j++) {
                    ns_css_value *rv = g_new0(ns_css_value, 1);
                    rv->kind = NS_CSS_V_KEYWORD;
                    rv->u.keyword = g_strdup(reset_props[j].value);
                    ns_css_decl rd = {
                        .prop = reset_props[j].prop,
                        .value = rv,
                        .important = important
                    };
                    g_array_append_val(decls_out, rd);
                }
                ns_css_value *fv = g_new0(ns_css_value, 1);
                fv->kind = NS_CSS_V_KEYWORD;
                fv->u.keyword = family_buf;
                ns_css_decl fd = {
                    .prop = NS_CSS_FONT_FAMILY, .value = fv,
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
                double num; ns_css_unit u;
                if (g_ascii_strcasecmp(t, "none") == 0) {
                    grow = 0; shrink = 0;
                    g_free(basis);
                    basis = g_strdup("auto"); basis_set = TRUE;
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
                    grow = 0; shrink = 1;
                    g_free(basis);
                    basis = g_strdup("auto"); basis_set = TRUE;
                    continue;
                }
                if (g_ascii_strncasecmp(t, "calc(", 5) == 0 ||
                    g_ascii_strncasecmp(t, "min(", 4) == 0 ||
                    g_ascii_strncasecmp(t, "max(", 4) == 0 ||
                    g_ascii_strncasecmp(t, "clamp(", 6) == 0) {
                    g_free(basis);
                    basis = g_strdup(t);
                    basis_set = TRUE;
                    continue;
                }
                if (parse_length(t, &num, &u) && u != NS_CSS_UNIT_NUMBER) {
                    g_free(basis);
                    basis = g_strdup(t);
                    basis_set = TRUE;
                    continue;
                }
                if (parse_length(t, &num, &u) && u == NS_CSS_UNIT_NUMBER) {
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
            ns_css_value *gv = parse_value_for(NS_CSS_FLEX_GROW, grow_buf);
            if (gv) {
                ns_css_decl d = { .prop = NS_CSS_FLEX_GROW, .value = gv, .important = important };
                g_array_append_val(decls_out, d);
            }
            ns_css_value *sv = parse_value_for(NS_CSS_FLEX_SHRINK, shrink_buf);
            if (sv) {
                ns_css_decl d = { .prop = NS_CSS_FLEX_SHRINK, .value = sv, .important = important };
                g_array_append_val(decls_out, d);
            }
            if (basis_set) {
                ns_css_value *bv = parse_value_for(NS_CSS_FLEX_BASIS, basis);
                if (bv) {
                    ns_css_decl d = { .prop = NS_CSS_FLEX_BASIS, .value = bv, .important = important };
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
                    ns_css_value *v = parse_value_for(NS_CSS_FLEX_DIRECTION, t);
                    if (v) {
                        ns_css_decl d = { .prop = NS_CSS_FLEX_DIRECTION, .value = v, .important = important };
                        g_array_append_val(decls_out, d);
                    }
                } else if (g_ascii_strcasecmp(t, "wrap") == 0 ||
                           g_ascii_strcasecmp(t, "nowrap") == 0 ||
                           g_ascii_strcasecmp(t, "wrap-reverse") == 0) {
                    ns_css_value *v = parse_value_for(NS_CSS_FLEX_WRAP, t);
                    if (v) {
                        ns_css_decl d = { .prop = NS_CSS_FLEX_WRAP, .value = v, .important = important };
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
                        ns_css_value *v = g_new0(ns_css_value, 1);
                        v->kind = NS_CSS_V_KEYWORD;
                        v->u.keyword = g_strdup(type_kws[k]);
                        ns_css_decl d = {
                            .prop = NS_CSS_LIST_STYLE_TYPE, .value = v,
                            .important = important
                        };
                        g_array_append_val(decls_out, d);
                        break;
                    }
                }
                if (g_ascii_strcasecmp(tokens[i], "inside") == 0 ||
                    g_ascii_strcasecmp(tokens[i], "outside") == 0) {
                    ns_css_value *v = g_new0(ns_css_value, 1);
                    v->kind = NS_CSS_V_KEYWORD;
                    v->u.keyword = g_ascii_strdown(tokens[i], -1);
                    ns_css_decl d = {
                        .prop = NS_CSS_LIST_STYLE_POSITION, .value = v,
                        .important = important
                    };
                    g_array_append_val(decls_out, d);
                }
                if (g_ascii_strncasecmp(tokens[i], "url(", 4) == 0 ||
                    g_ascii_strcasecmp(tokens[i], "none") == 0) {
                    ns_css_value *v = parse_value_for(NS_CSS_LIST_STYLE_IMAGE,
                                                      tokens[i]);
                    if (v) {
                        ns_css_decl d = {
                            .prop = NS_CSS_LIST_STYLE_IMAGE, .value = v,
                            .important = important
                        };
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
                const struct { ns_css_prop p; const char *v; } map[] = {
                    { NS_CSS_BORDER_TOP_LEFT_RADIUS,     tl },
                    { NS_CSS_BORDER_TOP_RIGHT_RADIUS,    tr },
                    { NS_CSS_BORDER_BOTTOM_RIGHT_RADIUS, br },
                    { NS_CSS_BORDER_BOTTOM_LEFT_RADIUS,  bl },
                };
                for (int i = 0; i < 4; i++) {
                    ns_css_value *vv = parse_value_for(map[i].p, map[i].v);
                    if (!vv) continue;
                    ns_css_decl d = { .prop = map[i].p, .value = vv, .important = important };
                    g_array_append_val(decls_out, d);
                }
                ns_css_value *legacy = parse_value_for(NS_CSS_BORDER_RADIUS, tl);
                if (legacy) {
                    ns_css_decl d = { .prop = NS_CSS_BORDER_RADIUS, .value = legacy, .important = important };
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
                ns_css_prop pa = NS_CSS_MARGIN_TOP, pb = NS_CSS_MARGIN_BOTTOM;
                if (strcmp(pname, "margin-block") == 0) {
                    pa = NS_CSS_MARGIN_TOP; pb = NS_CSS_MARGIN_BOTTOM;
                } else if (strcmp(pname, "margin-inline") == 0) {
                    pa = NS_CSS_MARGIN_LEFT; pb = NS_CSS_MARGIN_RIGHT;
                } else if (strcmp(pname, "padding-block") == 0) {
                    pa = NS_CSS_PADDING_TOP; pb = NS_CSS_PADDING_BOTTOM;
                } else if (strcmp(pname, "padding-inline") == 0) {
                    pa = NS_CSS_PADDING_LEFT; pb = NS_CSS_PADDING_RIGHT;
                } else if (strcmp(pname, "inset-block") == 0) {
                    pa = NS_CSS_TOP; pb = NS_CSS_BOTTOM;
                } else {
                    pa = NS_CSS_LEFT; pb = NS_CSS_RIGHT;
                }
                ns_css_value *va = parse_value_for(pa, a);
                ns_css_value *vb = parse_value_for(pb, b);
                if (va) {
                    ns_css_decl d = { .prop = pa, .value = va, .important = important };
                    g_array_append_val(decls_out, d);
                }
                if (vb) {
                    ns_css_decl d = { .prop = pb, .value = vb, .important = important };
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
                    NS_CSS_TOP, NS_CSS_RIGHT,
                    NS_CSS_BOTTOM, NS_CSS_LEFT,
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
                ns_css_value *vv = parse_value_for(NS_CSS_WHITE_SPACE, mapped);
                if (vv) {
                    ns_css_decl d = { .prop = NS_CSS_WHITE_SPACE, .value = vv, .important = important };
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
                        NS_CSS_MARGIN_TOP, NS_CSS_MARGIN_RIGHT,
                        NS_CSS_MARGIN_BOTTOM, NS_CSS_MARGIN_LEFT,
                        tokens, n, important);
                else if (strcmp(pname, "padding") == 0)
                    emit_quad(decls_out,
                        NS_CSS_PADDING_TOP, NS_CSS_PADDING_RIGHT,
                        NS_CSS_PADDING_BOTTOM, NS_CSS_PADDING_LEFT,
                        tokens, n, important);
                else if (strcmp(pname, "border-width") == 0)
                    emit_quad(decls_out,
                        NS_CSS_BORDER_TOP_WIDTH, NS_CSS_BORDER_RIGHT_WIDTH,
                        NS_CSS_BORDER_BOTTOM_WIDTH, NS_CSS_BORDER_LEFT_WIDTH,
                        tokens, n, important);
                else if (strcmp(pname, "border-color") == 0)
                    emit_quad(decls_out,
                        NS_CSS_BORDER_TOP_COLOR, NS_CSS_BORDER_RIGHT_COLOR,
                        NS_CSS_BORDER_BOTTOM_COLOR, NS_CSS_BORDER_LEFT_COLOR,
                        tokens, n, important);
                else
                    emit_quad(decls_out,
                        NS_CSS_BORDER_TOP_STYLE, NS_CSS_BORDER_RIGHT_STYLE,
                        NS_CSS_BORDER_BOTTOM_STYLE, NS_CSS_BORDER_LEFT_STYLE,
                        tokens, n, important);
            }
            for (int i = 0; i < n; i++) g_free(tokens[i]);
        } else if (strcmp(pname, "container") == 0) {
            char *slash = strchr(vtext, '/');
            char *name_part = slash ? g_strndup(vtext, (gsize)(slash - vtext))
                                    : g_strdup(vtext);
            g_strstrip(name_part);
            ns_css_value *nv = parse_value_for(NS_CSS_CONTAINER_NAME, name_part);
            if (nv) {
                ns_css_decl d = { .prop = NS_CSS_CONTAINER_NAME, .value = nv,
                                  .important = important };
                g_array_append_val(decls_out, d);
            }
            g_free(name_part);
            if (slash) {
                char *type_part = g_strstrip(g_strdup(slash + 1));
                ns_css_value *tv = *type_part
                    ? parse_value_for(NS_CSS_CONTAINER_TYPE, type_part) : NULL;
                if (tv) {
                    ns_css_decl d = { .prop = NS_CSS_CONTAINER_TYPE, .value = tv,
                                      .important = important };
                    g_array_append_val(decls_out, d);
                }
                g_free(type_part);
            }
        } else {
            int pid = prop_id(pname);
            if (pid >= 0) {
                ns_css_value *vv = parse_value_for((ns_css_prop)pid, vtext);
                if (vv) {
                    ns_css_decl d = { .prop = (ns_css_prop)pid, .value = vv, .important = important };
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
    ns_css_pending_decl *pd = data;
    g_free(pd->pname);
    g_free(pd->raw_vtext);
}

static void
ns_css_scope_free(ns_css_scope *s)
{
    if (!s) return;
    if (s->roots) g_ptr_array_free(s->roots, TRUE);
    if (s->limits) g_ptr_array_free(s->limits, TRUE);
    g_free(s);
}

static void
ns_css_rule_free(ns_css_rule *r)
{
    if (!r) return;
    for (guint i = 0; i < r->selectors->len; i++)
        ns_css_selector_free(g_ptr_array_index(r->selectors, i));
    g_ptr_array_free(r->selectors, TRUE);
    for (guint i = 0; i < r->decls->len; i++) {
        ns_css_decl *d = &g_array_index(r->decls, ns_css_decl, i);
        ns_css_value_free(d->value);
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
    ns_css_font_face *ff = data;
    g_free(ff->family);
    g_free(ff->src_url);
}

static void
property_rule_clear(gpointer data)
{
    ns_css_property_rule *pr = data;
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
    ns_css_keyframes *kf = data;
    g_free(kf->name);
    for (int i = 0; i < kf->n_stops; i++)
        g_free(kf->stops[i].raw_props);
    g_free(kf->stops);
}

ns_css_keyframes *
ns_css_keyframes_resolve(const ns_css_keyframes *kf,
                         const struct ns_var_map *vars)
{
    if (!kf) return NULL;
    gboolean any_raw = FALSE;
    for (int i = 0; i < kf->n_stops && !any_raw; i++)
        if (kf->stops[i].raw_props) any_raw = TRUE;
    if (!any_raw) return NULL;
    ns_css_keyframes *out = g_new0(ns_css_keyframes, 1);
    out->n_stops = kf->n_stops;
    out->stops = g_new(ns_css_keyframe_stop, (gsize)kf->n_stops);
    memcpy(out->stops, kf->stops,
           (gsize)kf->n_stops * sizeof(ns_css_keyframe_stop));
    for (int i = 0; i < out->n_stops; i++) {
        ns_css_keyframe_stop *s = &out->stops[i];
        const char *rawp = s->raw_props;
        s->raw_props = NULL;
        if (!rawp) continue;
        char *resolved = substitute_vars_with(rawp, vars, 0);
        if (!resolved) continue;
        ns_css_transform ind = { 0 };
        ns_css_transform list = s->has_transform ? s->transform
                                                 : (ns_css_transform){ 0 };
        char **decls = g_strsplit(resolved, ";", -1);
        for (int d = 0; decls[d]; d++) {
            char *colon = strchr(decls[d], ':');
            if (!colon) continue;
            *colon = '\0';
            char *prop = g_strstrip(decls[d]);
            char *val  = g_strstrip(colon + 1);
            ns_css_value *tv = NULL;
            if (g_ascii_strcasecmp(prop, "transform") == 0) {
                tv = parse_transform(val);
                if (tv) list = tv->u.transform;
            } else if (g_ascii_strcasecmp(prop, "translate") == 0) {
                tv = parse_translate_prop(val);
            } else if (g_ascii_strcasecmp(prop, "rotate") == 0) {
                tv = parse_rotate_prop(val);
            } else if (g_ascii_strcasecmp(prop, "scale") == 0) {
                tv = parse_scale_prop(val);
            }
            if (tv && g_ascii_strcasecmp(prop, "transform") != 0 &&
                ind.n_ops < NS_CSS_TRANSFORM_OPS_MAX)
                ind.ops[ind.n_ops++] = tv->u.transform.ops[0];
            if (tv) ns_css_value_free(tv);
        }
        g_strfreev(decls);
        g_free(resolved);
        ns_css_transform merged = ind;
        for (int k = 0; k < list.n_ops &&
                        merged.n_ops < NS_CSS_TRANSFORM_OPS_MAX; k++)
            merged.ops[merged.n_ops++] = list.ops[k];
        if (merged.n_ops > 0) {
            s->transform = merged;
            s->has_transform = TRUE;
        }
    }
    return out;
}

void
ns_css_keyframes_resolved_free(ns_css_keyframes *kf)
{
    if (!kf) return;
    g_free(kf->name);
    for (int i = 0; i < kf->n_stops; i++)
        g_free(kf->stops[i].raw_props);
    g_free(kf->stops);
    g_free(kf);
}

static int
keyframe_stop_cmp(gconstpointer a, gconstpointer b)
{
    double da = ((const ns_css_keyframe_stop *)a)->pct;
    double db = ((const ns_css_keyframe_stop *)b)->pct;
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

static ns_css_color_scheme g_color_scheme = NS_CSS_COLOR_SCHEME_LIGHT;
static ns_css_reduced_motion g_reduced_motion = NS_CSS_REDUCED_MOTION_NO_PREFERENCE;

ns_css_reduced_motion
ns_css_get_reduced_motion(void)
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
            return g_color_scheme == NS_CSS_COLOR_SCHEME_DARK;
        if (g_ascii_strcasecmp(value, "light") == 0)
            return g_color_scheme == NS_CSS_COLOR_SCHEME_LIGHT;
        return FALSE;
    }
    if (g_ascii_strcasecmp(name, "prefers-reduced-motion") == 0) {
        if (!value) return g_reduced_motion == NS_CSS_REDUCED_MOTION_REDUCE;
        if (g_ascii_strcasecmp(value, "reduce") == 0)
            return g_reduced_motion == NS_CSS_REDUCED_MOTION_REDUCE;
        if (g_ascii_strcasecmp(value, "no-preference") == 0)
            return g_reduced_motion == NS_CSS_REDUCED_MOTION_NO_PREFERENCE;
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
        return value && strcmp(value, "0") == 0;
    if (g_ascii_strcasecmp(name, "color") == 0 ||
        g_ascii_strcasecmp(name, "min-color") == 0 ||
        g_ascii_strcasecmp(name, "max-color") == 0) {
        const double depth = 8;
        if (!value) return depth != 0;
        char *e = NULL;
        double want = g_ascii_strtod(value, &e);
        if (e == value) return FALSE;
        if (g_ascii_strncasecmp(name, "min-", 4) == 0) return depth >= want;
        if (g_ascii_strncasecmp(name, "max-", 4) == 0) return depth <= want;
        return depth == want;
    }
    if (g_ascii_strcasecmp(name, "monochrome") == 0 ||
        g_ascii_strcasecmp(name, "min-monochrome") == 0 ||
        g_ascii_strcasecmp(name, "max-monochrome") == 0) {
        const double mono = 0;
        if (!value) return mono != 0;
        char *e = NULL;
        double want = g_ascii_strtod(value, &e);
        if (e == value) return FALSE;
        if (g_ascii_strncasecmp(name, "min-", 4) == 0) return mono >= want;
        if (g_ascii_strncasecmp(name, "max-", 4) == 0) return mono <= want;
        return mono == want;
    }
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
media_query_one_matches(const char *q, int depth)
{
    if (depth > NS_CSS_MAX_AT_NESTING) return FALSE;
    while (*q && is_ws(*q)) q++;
    gboolean invert = FALSE;
    if (g_ascii_strncasecmp(q, "not", 3) == 0 && is_ws(q[3])) {
        invert = TRUE; q += 3;
        while (*q && is_ws(*q)) q++;
    }
    if (g_ascii_strncasecmp(q, "only", 4) == 0 && is_ws(q[4])) {
        q += 4;
        while (*q && is_ws(*q)) q++;
    }
    const char *qe = q + strlen(q);
    const char *or_pos = media_find_top_level_or(q, qe);
    if (or_pos) {
        char *left = css_trim_dup_range(q, or_pos);
        char *right = css_trim_dup_range(or_pos + 2, qe);
        gboolean ok = media_query_one_matches(left, depth + 1) ||
                      media_query_one_matches(right, depth + 1);
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
        if (*alt && media_query_one_matches(alt, 0)) any = TRUE;
        g_free(alt);
        p = term == ',' ? seg + 1 : seg;
    }
    return any;
}

gboolean
ns_css_media_query_matches(const char *query)
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
ns_css_sizes_resolve(const char *sizes)
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
        gboolean cond_ok = (*cond == '\0') || ns_css_media_query_matches(cond);
        g_free(cond);
        if (!cond_ok) continue;
        double px = sizes_length_px(len_start, (gsize)(entry_end - len_start));
        if (px > 0) return px;
    }
    return g_viewport_w;
}

#define NS_CSS_LAYER_NONE INT_MAX

static gboolean supports_expr(const char **pp, const char *end, int depth);

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
    ns_css_value *v = parse_value_for((ns_css_prop)pid, value);
    gboolean ok = (v != NULL);
    if (v) ns_css_value_free(v);
    g_free(s);
    return ok;
}

static gboolean supports_selector_supported(const ns_css_selector *sel);

static gboolean
supports_simple_supported(const ns_css_simple *c)
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
supports_selector_supported(const ns_css_selector *sel)
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
    gboolean saved_strict = g_sel_strict;
    g_sel_strict = TRUE;
    gboolean valid = FALSE;
    GPtrArray *list = ns_css_parse_selector_list_checked(s, &valid);
    g_sel_strict = saved_strict;
    g_free(s);
    gboolean ok = valid;
    for (guint i = 0; ok && i < list->len; i++)
        if (!supports_selector_supported(g_ptr_array_index(list, i)))
            ok = FALSE;
    g_ptr_array_free(list, TRUE);
    return ok;
}

gboolean
ns_css_supports_selector(const char *text)
{
    if (!text) return FALSE;
    return supports_selector_matches(text, strlen(text));
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
supports_term(const char **pp, const char *end, int depth)
{
    if (depth > NS_CSS_MAX_AT_NESTING) { *pp = end; return FALSE; }
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
        result = supports_expr(&p, end, depth + 1);
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
supports_expr(const char **pp, const char *end, int depth)
{
    gboolean acc = supports_term(pp, end, depth);
    const char *p = *pp;
    while (1) {
        p = css_skip_ws_comments(p, end);
        if (match_kw(p, end, "and")) {
            p += 3;
            *pp = p;
            gboolean rhs = supports_term(pp, end, depth);
            p = *pp;
            acc = acc && rhs;
        } else if (match_kw(p, end, "or")) {
            p += 2;
            *pp = p;
            gboolean rhs = supports_term(pp, end, depth);
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
    return supports_expr(&p, end, 0);
}

/* Container query context: a stack of ancestor query containers, innermost
 * last, plus a node->info map populated from the laid-out box tree. */
#define NS_CQ_TYPE_INLINE 1
#define NS_CQ_TYPE_SIZE   2

typedef struct {
    char  *names;   /* space-separated container-name list, verbatim */
    double width;
    double height;
    int    type;    /* NS_CQ_TYPE_* */
} ns_cq_container;

static __thread GHashTable *g_cq_map;     /* ns_node* -> ns_cq_container* */
static __thread GArray     *g_cq_stack;   /* ns_cq_container (by value) */
static __thread GHashTable *g_registered_props; /* "--name" -> ns_css_property_rule* */
static __thread GHashTable *g_var_adjust_cache; /* parent ns_var_map* -> adjusted ns_var_map* */

void
ns_css_set_container_map(GHashTable *map)
{
    g_cq_map = map;
}

static void
cq_container_free(gpointer p)
{
    ns_cq_container *c = p;
    g_free(c->names);
    g_free(c);
}

GHashTable *
ns_css_container_map_new(void)
{
    return g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                 NULL, cq_container_free);
}

void
ns_css_container_map_add(GHashTable *map, const void *node,
                         const char *type_kw, const char *name_kw,
                         double w, double h)
{
    if (!map || !node || !type_kw) return;
    int type = g_ascii_strcasecmp(type_kw, "size") == 0
        ? NS_CQ_TYPE_SIZE : NS_CQ_TYPE_INLINE;
    ns_cq_container *c = g_new0(ns_cq_container, 1);
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
cq_feature_matches(const char *feat, const ns_cq_container *c)
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
        if ((vert && c->type != NS_CQ_TYPE_SIZE) || (!horiz && !vert))
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
        if (fi >= 0 && !(vert && c->type != NS_CQ_TYPE_SIZE)) {
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
cq_eval_expr(const char *q, const ns_cq_container *c, int rdepth)
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
static const ns_cq_container *
cq_select_container(const char *name, gsize nlen)
{
    if (!g_cq_stack || g_cq_stack->len == 0) return NULL;
    for (int i = (int)g_cq_stack->len - 1; i >= 0; i--) {
        const ns_cq_container *c = &g_array_index(g_cq_stack, ns_cq_container, i);
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
    const ns_cq_container *c = cq_select_container(name, nlen);
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
css_stylesheet_ensure_layers(ns_css_stylesheet *sh)
{
    if (!sh->layer_names)
        sh->layer_names = g_ptr_array_new_with_free_func(g_free);
    if (!sh->layers)
        sh->layers = g_hash_table_new(g_str_hash, g_str_equal);
}

static int
css_layer_register(ns_css_stylesheet *sh, const char *name)
{
    if (!sh || !name || !*name) return NS_CSS_LAYER_NONE;
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
css_layer_anonymous(ns_css_stylesheet *sh)
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
css_layer_name_from_range(ns_css_stylesheet *sh, const char *current_layer,
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
css_layer_register_list(ns_css_stylesheet *sh, const char *current_layer,
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
css_parse_layer_function(ns_css_stylesheet *sh, const char **pp,
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
    ns_css_import *im = data;
    g_free(im->url);
    g_free(im->layer_name);
    g_free(im->media);
}

static void
css_stylesheet_add_import(ns_css_stylesheet *sh, const char *url,
                          const char *layer_name, const char *media)
{
    if (!sh || !url || !*url) return;
    if (!sh->imports) {
        sh->imports = g_array_new(FALSE, FALSE, sizeof(ns_css_import));
        g_array_set_clear_func(sh->imports, css_import_clear);
    }
    if (layer_name) css_layer_register(sh, layer_name);
    ns_css_import im = {
        .url = g_strdup(url),
        .layer_name = layer_name ? g_strdup(layer_name) : NULL,
        .media = media && *media ? g_strdup(media) : NULL,
    };
    g_array_append_val(sh->imports, im);
}

static void
css_parse_import_prelude(ns_css_stylesheet *sh, const char *current_layer,
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
ns_css_scope_text_free(gpointer data)
{
    ns_css_scope_text *s = data;
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
        const ns_css_selector *sel = g_ptr_array_index(group, i);
        if (!sel || sel->pseudo_element != NS_CSS_PE_NONE) return FALSE;
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
css_scope_text_valid(const ns_css_scope_text *s)
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

static ns_css_scope_text *
css_scope_text_from_prelude(const char *start, const char *end)
{
    const char *p = css_skip_ws_comments(start, end);
    ns_css_scope_text *s = g_new0(ns_css_scope_text, 1);
    if (p < end && *p == '(') {
        char term = 0;
        const char *inner = p + 1;
        const char *close = css_scan_until(inner, end, ")", &term);
        if (term != ')') {
            ns_css_scope_text_free(s);
            return NULL;
        }
        s->start = css_trim_dup_range(inner, close);
        p = close + 1;
        if (!s->start || !*s->start) {
            ns_css_scope_text_free(s);
            return NULL;
        }
    }
    p = css_skip_ws_comments(p, end);
    if (css_scope_keyword_at(p, end, "to")) {
        p += 2;
        p = css_skip_ws_comments(p, end);
        if (p >= end || *p != '(') {
            ns_css_scope_text_free(s);
            return NULL;
        }
        char term = 0;
        const char *inner = p + 1;
        const char *close = css_scan_until(inner, end, ")", &term);
        if (term != ')') {
            ns_css_scope_text_free(s);
            return NULL;
        }
        s->end = css_trim_dup_range(inner, close);
        p = close + 1;
        if (!s->end || !*s->end) {
            ns_css_scope_text_free(s);
            return NULL;
        }
    }
    p = css_skip_ws_comments(p, end);
    if (p < end || !css_scope_text_valid(s)) {
        ns_css_scope_text_free(s);
        return NULL;
    }
    return s;
}

static ns_css_scope *
css_scope_from_text(const ns_css_scope_text *text)
{
    ns_css_scope *s = g_new0(ns_css_scope, 1);
    s->roots = css_scope_parse_selector_list(text && text->start
                                             ? text->start : ":root");
    if (!s->roots) {
        ns_css_scope_free(s);
        return NULL;
    }
    if (text && text->end) {
        s->limits = css_scope_parse_selector_list(text->end);
        if (!s->limits) {
            ns_css_scope_free(s);
            return NULL;
        }
    }
    return s;
}

static gboolean
css_scope_stack_apply_to_rule(ns_css_rule *rule, GPtrArray *scope_stack)
{
    if (!rule || !scope_stack || scope_stack->len == 0) return TRUE;
    rule->scopes = g_ptr_array_new_with_free_func((GDestroyNotify)ns_css_scope_free);
    for (guint i = 0; i < scope_stack->len; i++) {
        ns_css_scope_text *text = g_ptr_array_index(scope_stack, i);
        ns_css_scope *scope = css_scope_from_text(text);
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
                  ns_css_stylesheet *sh, int *source_order,
                  char close_at, const char *current_layer,
                  GPtrArray *scope_stack)
{
    static int at_depth;
    gboolean nested = close_at == '}';
    if (nested) {
        if (at_depth >= NS_CSS_MAX_AT_NESTING) {
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
                                                     sizeof(ns_css_font_face));
                        g_array_set_clear_func(sh->font_faces, font_face_clear);
                    }
                    if (family && *family && src_url && *src_url) {
                        ns_css_font_face ff = { family, src_url };
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
                                                sizeof(ns_css_keyframe_stop));
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
                        ns_css_transform tf = { 0 };
                        gboolean has_tf = FALSE;
                        ns_css_transform tf_ind = { 0 };
                        guint8 col[4] = { 0 }, bgcol[4] = { 0 };
                        gboolean has_col = FALSE, has_bgcol = FALSE;
                        GString *raw = NULL;
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
                            gboolean tf_prop =
                                g_ascii_strcasecmp(prop, "transform") == 0 ||
                                g_ascii_strcasecmp(prop, "translate") == 0 ||
                                g_ascii_strcasecmp(prop, "rotate") == 0 ||
                                g_ascii_strcasecmp(prop, "scale") == 0;
                            if (tf_prop && strstr(val, "var(")) {
                                if (!raw) raw = g_string_new(NULL);
                                if (raw->len) g_string_append_c(raw, ';');
                                g_string_append_printf(raw, "%s:%s", prop, val);
                            } else if (g_ascii_strcasecmp(prop, "opacity") == 0) {
                                op = g_ascii_strtod(val, NULL);
                                has_op = TRUE;
                            } else if (g_ascii_strcasecmp(prop, "transform") == 0) {
                                ns_css_value *tv = parse_transform(val);
                                if (tv) {
                                    tf = tv->u.transform;
                                    has_tf = TRUE;
                                    ns_css_value_free(tv);
                                }
                            } else if (g_ascii_strcasecmp(prop, "translate") == 0 ||
                                       g_ascii_strcasecmp(prop, "rotate") == 0 ||
                                       g_ascii_strcasecmp(prop, "scale") == 0) {
                                ns_css_value *tv =
                                    g_ascii_strcasecmp(prop, "translate") == 0
                                        ? parse_translate_prop(val)
                                    : g_ascii_strcasecmp(prop, "rotate") == 0
                                        ? parse_rotate_prop(val)
                                        : parse_scale_prop(val);
                                if (tv) {
                                    if (tf_ind.n_ops < NS_CSS_TRANSFORM_OPS_MAX)
                                        tf_ind.ops[tf_ind.n_ops++] =
                                            tv->u.transform.ops[0];
                                    ns_css_value_free(tv);
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
                        if (tf_ind.n_ops > 0) {
                            ns_css_transform merged = tf_ind;
                            for (int k = 0; k < tf.n_ops &&
                                            merged.n_ops < NS_CSS_TRANSFORM_OPS_MAX;
                                 k++)
                                merged.ops[merged.n_ops++] = tf.ops[k];
                            tf = merged;
                            has_tf = TRUE;
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
                                ns_css_keyframe_stop s = {
                                    .pct = pct,
                                    .opacity = op, .has_opacity = has_op,
                                    .transform = tf, .has_transform = has_tf,
                                    .has_color = has_col, .has_bg_color = has_bgcol,
                                    .raw_props = raw && raw->len
                                        ? g_strdup(raw->str) : NULL,
                                };
                                memcpy(s.color, col, 4);
                                memcpy(s.bg_color, bgcol, 4);
                                g_array_append_val(stops, s);
                            }
                            g_free(one);
                            sel_p = cterm == ',' ? one_end + 1 : one_end;
                        }
                        if (raw) g_string_free(raw, TRUE);
                        g_free(sel);
                    }
                    if (stops->len > 0 && kf_name && *kf_name) {
                        if (!sh->keyframes) {
                            sh->keyframes = g_array_new(FALSE, FALSE,
                                                        sizeof(ns_css_keyframes));
                            g_array_set_clear_func(sh->keyframes, keyframes_clear);
                        }
                        g_array_sort(stops, keyframe_stop_cmp);
                        ns_css_keyframes kf = {
                            .name = g_strdup(kf_name),
                            .n_stops = (int)stops->len,
                            .stops = (ns_css_keyframe_stop *)g_memdup2(
                                stops->data,
                                stops->len * sizeof(ns_css_keyframe_stop)),
                        };
                        g_array_append_val(sh->keyframes, kf);
                    } else {
                        for (guint i = 0; i < stops->len; i++)
                            g_free(g_array_index(stops, ns_css_keyframe_stop,
                                                 i).raw_props);
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
                        ns_css_rule *r = g_ptr_array_index(sh->rules, ri);
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
                    ns_css_scope_text *scope =
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
                            sizeof(ns_css_property_rule));
                        g_array_set_clear_func(sh->property_rules,
                                               property_rule_clear);
                    }
                    ns_css_property_rule pr = {
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

        ns_css_rule *rule = g_new0(ns_css_rule, 1);
        rule->selectors = g_ptr_array_new();
        rule->decls     = g_array_new(FALSE, FALSE, sizeof(ns_css_decl));
        rule->layer_name = current_layer ? g_strdup(current_layer) : NULL;
        rule->source_order = (*source_order)++;
        if (!css_scope_stack_apply_to_rule(rule, scope_stack)) {
            ns_css_rule_free(rule);
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
            ns_css_rule_free(rule);
            p = term == ';' ? sel_end + 1 : sel_end;
            continue;
        }
        char *scoped_sel = rule->scopes
            ? css_scope_selector_list_text(sel_start, sel_end) : NULL;
        const char *parse_p = scoped_sel ? scoped_sel : sel_start;
        const char *parse_end = scoped_sel ? scoped_sel + strlen(scoped_sel)
                                           : sel_end;

        gboolean ok = FALSE;
        g_sel_has_hover = FALSE;
        g_sel_has_active = FALSE;
        g_sel_parse_error = FALSE;
        while (parse_p < parse_end) {
            ns_css_selector *sel = parse_one_selector(&parse_p, parse_end, 0);
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
        if (g_sel_parse_error) ok = FALSE;
        if (ok && g_sel_has_hover)
            sh->has_hover_rules = TRUE;
        if (ok && g_sel_has_active)
            sh->has_active_rules = TRUE;
        g_free(scoped_sel);
        if (!ok) {
            ns_css_rule_free(rule);
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

#define NS_CSS_NEST_MAX_DEPTH 128

static void
css_trim_selector(char *sel)
{
    char *s = sel;
    while (*s && is_ws(*s)) s++;
    if (s != sel) memmove(sel, s, strlen(s) + 1);
    size_t n = strlen(sel);
    while (n > 0 && is_ws((unsigned char)sel[n - 1])) {
        size_t bs = 0, i = n - 1;
        while (i > 0 && sel[i - 1] == '\\') { bs++; i--; }
        if (bs % 2 == 1) break;
        n--;
    }
    sel[n] = '\0';
}

static void
css_flatten_rule_list(GString *out, const char *p, const char *end, int depth)
{
    if (depth > NS_CSS_NEST_MAX_DEPTH) return;
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
        css_trim_selector(sel);
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
    if (depth > NS_CSS_NEST_MAX_DEPTH) return;
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
            css_trim_selector(nsel);
            const char *nbody_s = seg_end + 1;
            const char *nblock_end = css_skip_to_block_end(seg_end, body_e);
            const char *nbody_e = css_block_body_end(nbody_s, nblock_end);
            if (nsel[0] == '@') {
                gboolean group =
                    g_ascii_strncasecmp(nsel, "@media", 6) == 0 ||
                    g_ascii_strncasecmp(nsel, "@supports", 9) == 0 ||
                    g_ascii_strncasecmp(nsel, "@container", 10) == 0;
                if (group) {
                    g_string_append(deferred, nsel);
                    g_string_append_c(deferred, '{');
                    css_flatten_style_rule(deferred, sel, nbody_s, nbody_e,
                                           depth + 1);
                    g_string_append_c(deferred, '}');
                }
            } else {
                char *combined = css_combine_selectors(sel, nsel);
                css_flatten_style_rule(deferred, combined, nbody_s, nbody_e,
                                       depth + 1);
                g_free(combined);
            }
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

ns_css_stylesheet *
ns_css_stylesheet_parse(const char *text, gssize len_in)
{
    ns_css_stylesheet *sh = g_new0(ns_css_stylesheet, 1);
    sh->rules = g_ptr_array_new_with_free_func((GDestroyNotify)ns_css_rule_free);
    if (!text) return sh;
    if (len_in < 0) len_in = (gssize)strlen(text);

    char *flattened = css_flatten_nesting(text, len_in);
    const char *p   = flattened;
    const char *end = flattened + strlen(flattened);
    int source_order = 0;
    GPtrArray *scope_stack =
        g_ptr_array_new_with_free_func(ns_css_scope_text_free);
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
css_value_resolve_url(ns_css_value *v, const char *base_url)
{
    if (!v || !base_url || v->kind != NS_CSS_V_URL)
        return;
    if (!css_url_should_resolve(v->u.url))
        return;
    char *abs_url = ns_url_resolve(base_url, v->u.url);
    if (!abs_url) return;
    g_free(v->u.url);
    v->u.url = abs_url;
}

static char *
css_raw_text_resolve_urls(const char *text, const char *base_url)
{
    if (!text || !strstr(text, "url(")) return NULL;
    GString *out = g_string_new(NULL);
    const char *p = text;
    gboolean changed = FALSE;
    while (*p) {
        const char *hit = strstr(p, "url(");
        if (!hit) { g_string_append(out, p); break; }
        const char *close = strchr(hit + 4, ')');
        if (!close) { g_string_append(out, p); break; }
        g_string_append_len(out, p, (gssize)(hit - p));
        const char *s = hit + 4;
        while (s < close && g_ascii_isspace(*s)) s++;
        const char *e = close;
        while (e > s && g_ascii_isspace(e[-1])) e--;
        if (e > s && (*s == '"' || *s == '\'')) {
            char q = *s;
            s++;
            if (e > s && e[-1] == q) e--;
        }
        char *rel = g_strndup(s, (gsize)(e - s));
        char *abs = css_url_should_resolve(rel)
            ? ns_url_resolve(base_url, rel) : NULL;
        if (abs && strcmp(abs, rel) != 0) {
            g_string_append(out, "url(\"");
            g_string_append(out, abs);
            g_string_append(out, "\")");
            changed = TRUE;
        } else {
            g_string_append_len(out, hit, (gssize)(close + 1 - hit));
        }
        g_free(abs);
        g_free(rel);
        p = close + 1;
    }
    if (!changed) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

void
ns_css_stylesheet_resolve_urls(ns_css_stylesheet *s, const char *base_url)
{
    if (!s || !base_url) return;
    if (s->rules) {
        for (guint ri = 0; ri < s->rules->len; ri++) {
            ns_css_rule *r = g_ptr_array_index(s->rules, ri);
            if (!r) continue;
            if (r->decls) {
                for (guint di = 0; di < r->decls->len; di++) {
                    ns_css_decl *d = &g_array_index(r->decls, ns_css_decl, di);
                    css_value_resolve_url(d->value, base_url);
                }
            }
            if (r->vars) {
                GHashTableIter it;
                gpointer k, v;
                g_hash_table_iter_init(&it, r->vars);
                while (g_hash_table_iter_next(&it, &k, &v)) {
                    char *resolved = css_raw_text_resolve_urls(v, base_url);
                    if (resolved) g_hash_table_iter_replace(&it, resolved);
                }
            }
            if (r->pending) {
                for (guint pi = 0; pi < r->pending->len; pi++) {
                    ns_css_pending_decl *pd =
                        &g_array_index(r->pending, ns_css_pending_decl, pi);
                    char *resolved =
                        css_raw_text_resolve_urls(pd->raw_vtext, base_url);
                    if (resolved) {
                        g_free(pd->raw_vtext);
                        pd->raw_vtext = resolved;
                    }
                }
            }
        }
    }
    if (s->font_faces) {
        for (guint i = 0; i < s->font_faces->len; i++) {
            ns_css_font_face *ff =
                &g_array_index(s->font_faces, ns_css_font_face, i);
            if (!css_url_should_resolve(ff->src_url)) continue;
            char *abs_url = ns_url_resolve(base_url, ff->src_url);
            if (!abs_url) continue;
            g_free(ff->src_url);
            ff->src_url = abs_url;
        }
    }
}

typedef struct css_candidate {
    guint rule_idx;
    guint selector_idx;
} css_candidate;

typedef enum css_index_kind {
    CSS_INDEX_NONE,
    CSS_INDEX_ID,
    CSS_INDEX_CLASS,
    CSS_INDEX_TAG,
    CSS_INDEX_ATTR,
} css_index_kind;

typedef struct css_index_counts {
    GHashTable *by_id;
    GHashTable *by_class;
    GHashTable *by_tag;
    GHashTable *by_attr;
} css_index_counts;

typedef struct ns_css_rule_index {
    GHashTable *by_id;
    GHashTable *by_class;
    GHashTable *by_tag;
    GHashTable *by_attr;
    GArray     *universal;
} ns_css_rule_index;

static void ns_css_rule_index_free(ns_css_rule_index *idx);

gboolean
ns_css_stylesheet_has_container_rules(const ns_css_stylesheet *sh)
{
    return sh && sh->has_container_rules;
}

gboolean
ns_css_stylesheet_has_hover_rules(const ns_css_stylesheet *sh)
{
    return sh && sh->has_hover_rules;
}

gboolean
ns_css_stylesheet_has_active_rules(const ns_css_stylesheet *sh)
{
    return sh && sh->has_active_rules;
}

void
ns_css_stylesheet_free(ns_css_stylesheet *s)
{
    if (!s || s->cached) return;
    if (s->rules) g_ptr_array_free(s->rules, TRUE);
    if (s->imports) g_array_free(s->imports, TRUE);
    if (s->layers) g_hash_table_destroy(s->layers);
    if (s->layer_names) g_ptr_array_free(s->layer_names, TRUE);
    if (s->font_faces) g_array_free(s->font_faces, TRUE);
    if (s->keyframes) g_array_free(s->keyframes, TRUE);
    if (s->property_rules) g_array_free(s->property_rules, TRUE);
    if (s->index) ns_css_rule_index_free(s->index);
    s->rules = NULL;
    s->imports = NULL;
    s->layers = NULL;
    s->layer_names = NULL;
    s->font_faces = NULL;
    s->keyframes = NULL;
    s->property_rules = NULL;
    s->index = NULL;
    g_free(s);
}

void
ns_css_stylesheet_force_layer(ns_css_stylesheet *s, const char *layer_name)
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
            ns_css_rule *r = g_ptr_array_index(s->rules, i);
            char *full = r->layer_name ? css_layer_join(layer_name, r->layer_name)
                                       : g_strdup(layer_name);
            g_free(r->layer_name);
            r->layer_name = full;
        }
    }
    if (s->imports) {
        for (guint i = 0; i < s->imports->len; i++) {
            ns_css_import *im = &g_array_index(s->imports, ns_css_import, i);
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
ns_css_rule_index_free(ns_css_rule_index *idx)
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
index_add_candidate_array(GArray *bucket, guint rule_idx, guint selector_idx)
{
    css_candidate cand = { rule_idx, selector_idx };
    if (bucket->len > 0) {
        css_candidate last =
            g_array_index(bucket, css_candidate, bucket->len - 1);
        if (last.rule_idx == rule_idx && last.selector_idx == selector_idx)
            return;
    }
    g_array_append_val(bucket, cand);
}

static void
index_add(GHashTable *table, const char *key, guint rule_idx, guint selector_idx)
{
    GArray *bucket = g_hash_table_lookup(table, key);
    if (!bucket) {
        bucket = g_array_new(FALSE, FALSE, sizeof(css_candidate));
        g_hash_table_insert(table, g_strdup(key), bucket);
    }
    index_add_candidate_array(bucket, rule_idx, selector_idx);
}

static void
index_add_lowercase(GHashTable *table, const char *key, guint rule_idx,
                    guint selector_idx)
{
    char *lk = g_ascii_strdown(key, -1);
    GArray *bucket = g_hash_table_lookup(table, lk);
    if (!bucket) {
        bucket = g_array_new(FALSE, FALSE, sizeof(css_candidate));
        g_hash_table_insert(table, lk, bucket);
        lk = NULL;
    }
    if (bucket) index_add_candidate_array(bucket, rule_idx, selector_idx);
    g_free(lk);
}

static void
index_count_inc(GHashTable *table, const char *key)
{
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(table, key));
    g_hash_table_replace(table, g_strdup(key), GUINT_TO_POINTER(n + 1));
}

static void
index_count_inc_lowercase(GHashTable *table, const char *key)
{
    char *lk = g_ascii_strdown(key, -1);
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(table, lk));
    g_hash_table_replace(table, lk, GUINT_TO_POINTER(n + 1));
}

static guint
index_count_lookup_lowercase(GHashTable *table, const char *key)
{
    char *lk = g_ascii_strdown(key, -1);
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(table, lk));
    g_free(lk);
    return n;
}

static css_index_counts
index_counts_new(void)
{
    css_index_counts counts = {
        .by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        .by_class = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        .by_tag = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        .by_attr = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
    };
    return counts;
}

static void
index_counts_free(css_index_counts *counts)
{
    g_hash_table_destroy(counts->by_id);
    g_hash_table_destroy(counts->by_class);
    g_hash_table_destroy(counts->by_tag);
    g_hash_table_destroy(counts->by_attr);
}

static void
index_counts_add_subject(css_index_counts *counts, const ns_css_simple *subj)
{
    if (!counts || !subj || subj->never_match) return;
    if (subj->id && *subj->id)
        index_count_inc(counts->by_id, subj->id);
    for (guint i = 0; subj->classes && i < subj->classes->len; i++) {
        const char *cls = g_ptr_array_index(subj->classes, i);
        if (cls && *cls) index_count_inc(counts->by_class, cls);
    }
    if (subj->type && *subj->type && strcmp(subj->type, "*") != 0)
        index_count_inc_lowercase(counts->by_tag, subj->type);
    for (guint i = 0; subj->attrs && i < subj->attrs->len; i++) {
        const ns_css_attr_pred *a =
            &g_array_index(subj->attrs, ns_css_attr_pred, i);
        if (a && a->name && *a->name)
            index_count_inc_lowercase(counts->by_attr, a->name);
    }
}

static gboolean
index_choice_take(guint count, guint *best)
{
    if (count == 0 || count >= *best) return FALSE;
    *best = count;
    return TRUE;
}

static css_index_kind
index_subject_kind(const css_index_counts *counts,
                   const ns_css_simple *subj,
                   guint *out_class_i,
                   guint *out_attr_i)
{
    css_index_kind kind = CSS_INDEX_NONE;
    guint best = G_MAXUINT;
    if (out_class_i) *out_class_i = 0;
    if (out_attr_i) *out_attr_i = 0;
    if (!counts || !subj || subj->never_match) return kind;
    if (subj->id && *subj->id &&
        index_choice_take(GPOINTER_TO_UINT(g_hash_table_lookup(counts->by_id,
                                                               subj->id)),
                          &best)) {
        kind = CSS_INDEX_ID;
    }
    for (guint i = 0; subj->classes && i < subj->classes->len; i++) {
        const char *cls = g_ptr_array_index(subj->classes, i);
        if (!cls || !*cls) continue;
        guint count = GPOINTER_TO_UINT(g_hash_table_lookup(counts->by_class,
                                                           cls));
        if (index_choice_take(count, &best)) {
            kind = CSS_INDEX_CLASS;
            if (out_class_i) *out_class_i = i;
        }
    }
    if (subj->type && *subj->type && strcmp(subj->type, "*") != 0 &&
        index_choice_take(index_count_lookup_lowercase(counts->by_tag,
                                                       subj->type),
                          &best)) {
        kind = CSS_INDEX_TAG;
    }
    for (guint i = 0; subj->attrs && i < subj->attrs->len; i++) {
        const ns_css_attr_pred *a =
            &g_array_index(subj->attrs, ns_css_attr_pred, i);
        if (!a || !a->name || !*a->name) continue;
        if (index_choice_take(index_count_lookup_lowercase(counts->by_attr,
                                                           a->name),
                              &best)) {
            kind = CSS_INDEX_ATTR;
            if (out_attr_i) *out_attr_i = i;
        }
    }
    return kind;
}

static ns_css_rule_index *
ns_css_rule_index_build(const ns_css_stylesheet *sheet)
{
    ns_css_rule_index *idx = g_new0(ns_css_rule_index, 1);
    idx->by_id    = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_bucket_array);
    idx->by_class = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_bucket_array);
    idx->by_tag   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_bucket_array);
    idx->by_attr  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_bucket_array);
    idx->universal = g_array_new(FALSE, FALSE, sizeof(css_candidate));

    css_index_counts counts = index_counts_new();
    for (guint ri = 0; ri < sheet->rules->len; ri++) {
        const ns_css_rule *r = g_ptr_array_index(sheet->rules, ri);
        for (guint si = 0; si < r->selectors->len; si++) {
            const ns_css_selector *sel = g_ptr_array_index(r->selectors, si);
            if (!sel || sel->compounds->len == 0) continue;
            const ns_css_simple *subj =
                g_ptr_array_index(sel->compounds, sel->compounds->len - 1);
            index_counts_add_subject(&counts, subj);
        }
    }

    for (guint ri = 0; ri < sheet->rules->len; ri++) {
        const ns_css_rule *r = g_ptr_array_index(sheet->rules, ri);
        gboolean had_matchable_selector = FALSE;
        for (guint si = 0; si < r->selectors->len; si++) {
            const ns_css_selector *sel = g_ptr_array_index(r->selectors, si);
            if (sel && sel->pseudo_element != NS_CSS_PE_NONE) {
                ((ns_css_stylesheet *)sheet)->pseudo_mask |=
                    (1u << sel->pseudo_element);
                ((ns_css_rule *)r)->pe_mask |= (1u << sel->pseudo_element);
            }
            if (!sel || sel->compounds->len == 0) {
                index_add_candidate_array(idx->universal, ri, si);
                had_matchable_selector = TRUE;
                continue;
            }
            const ns_css_simple *subj =
                g_ptr_array_index(sel->compounds, sel->compounds->len - 1);
            if (!subj || subj->never_match) continue;
            had_matchable_selector = TRUE;
            guint class_i = 0, attr_i = 0;
            switch (index_subject_kind(&counts, subj, &class_i, &attr_i)) {
            case CSS_INDEX_ID:
                index_add(idx->by_id, subj->id, ri, si);
                continue;
            case CSS_INDEX_CLASS: {
                const char *cls = g_ptr_array_index(subj->classes, class_i);
                if (cls && *cls) {
                    index_add(idx->by_class, cls, ri, si);
                    continue;
                }
                break;
            }
            case CSS_INDEX_TAG:
                index_add_lowercase(idx->by_tag, subj->type, ri, si);
                continue;
            case CSS_INDEX_ATTR: {
                const ns_css_attr_pred *a0 =
                    &g_array_index(subj->attrs, ns_css_attr_pred, attr_i);
                if (a0 && a0->name && *a0->name) {
                    index_add_lowercase(idx->by_attr, a0->name, ri, si);
                    continue;
                }
                break;
            }
            case CSS_INDEX_NONE:
                break;
            }
            index_add_candidate_array(idx->universal, ri, si);
        }
        if (!had_matchable_selector) continue;
    }
    index_counts_free(&counts);
    return idx;
}

static const ns_css_rule_index *
ns_css_rule_index_ensure(const ns_css_stylesheet *sheet)
{
    if (!sheet) return NULL;
    if (!sheet->index)
        ((ns_css_stylesheet *)sheet)->index = ns_css_rule_index_build(sheet);
    return sheet->index;
}

static gboolean match_selector(const ns_css_selector *sel, const ns_node *el);
static const ns_node *g_css_match_scope;

const ns_node *
ns_css_set_match_scope(const ns_node *scope)
{
    const ns_node *prev = g_css_match_scope;
    g_css_match_scope = scope;
    return prev;
}

static gboolean match_simple(const ns_css_simple *sel, const ns_node *el);

static gboolean
ns_input_is_text_entry(const ns_node *el)
{
    const char *type = ns_element_get_attr(el, "type");
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
ns_el_is_read_write(const ns_node *el)
{
    if (!el->name) return FALSE;
    if (strcmp(el->name, "input") == 0)
        return ns_input_type_supports_readonly(ns_element_get_attr(el, "type")) &&
               !ns_element_get_attr(el, "readonly") &&
               !ns_element_effectively_disabled(el);
    if (strcmp(el->name, "textarea") == 0)
        return !ns_element_get_attr(el, "readonly") &&
               !ns_element_effectively_disabled(el);
    const char *ce = ns_element_get_attr(el, "contenteditable");
    if (ce && (!*ce || g_ascii_strcasecmp(ce, "true") == 0 ||
               g_ascii_strcasecmp(ce, "plaintext-only") == 0))
        return TRUE;
    return FALSE;
}

static gboolean
ns_el_placeholder_shown(const ns_node *el)
{
    if (!el->name) return FALSE;
    const char *ph = ns_element_get_attr(el, "placeholder");
    if (!ph) return FALSE;
    if (strcmp(el->name, "input") == 0) {
        if (!ns_input_is_text_entry(el)) return FALSE;
        const char *v = ns_element_get_attr(el, "value");
        return !v || !*v;
    }
    if (strcmp(el->name, "textarea") == 0) {
        char *txt = ns_node_collect_text(el);
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
ns_el_is_checked(const ns_node *el)
{
    if (ns_node_is_element_named(el, "option")) {
        if (ns_element_get_attr(el, "selected")) return TRUE;
        const ns_node *sel = el->parent;
        if (ns_node_is_element_named(sel, "optgroup")) sel = sel->parent;
        return ns_node_is_element_named(sel, "select") &&
               !ns_element_get_attr(sel, "multiple") &&
               ns_select_chosen_option(sel) == el;
    }
    if (!ns_node_is_element_named(el, "input"))
        return FALSE;
    const char *type = ns_element_get_attr(el, "type");
    if (!type || (g_ascii_strcasecmp(type, "checkbox") != 0 &&
                  g_ascii_strcasecmp(type, "radio") != 0))
        return FALSE;
    return ns_input_is_checked(el);
}

static gboolean
ns_el_is_submit_button(const ns_node *el)
{
    if (ns_node_is_element_named(el, "input")) {
        const char *type = ns_element_get_attr(el, "type");
        return type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                        g_ascii_strcasecmp(type, "image") == 0);
    }
    if (!ns_node_is_element_named(el, "button")) return FALSE;
    const char *type = ns_element_get_attr(el, "type");
    return !type || !*type ||
           g_ascii_strcasecmp(type, "submit") == 0 ||
           g_ascii_strcasecmp(type, "auto") == 0;
}

static const ns_node *
ns_css_first_submit_button_for(const ns_node *scan, const ns_node *doc,
                               const ns_node *owner, int depth)
{
    if (!scan || depth >= 512) return NULL;
    if (scan->kind == NS_NODE_ELEMENT &&
        ns_el_is_submit_button(scan) &&
        !ns_element_effectively_disabled(scan) &&
        ns_form_owner(scan, doc) == owner)
        return scan;
    if (ns_node_is_element_named(scan, "template")) return NULL;
    for (const ns_node *c = scan->first_child; c; c = c->next_sibling) {
        const ns_node *hit =
            ns_css_first_submit_button_for(c, doc, owner, depth + 1);
        if (hit) return hit;
    }
    return NULL;
}

static gboolean
ns_el_is_default(const ns_node *el)
{
    if (ns_node_is_element_named(el, "option"))
        return ns_element_get_attr(el, "selected") != NULL;
    if (ns_node_is_element_named(el, "input")) {
        const char *type = ns_element_get_attr(el, "type");
        if (type && (g_ascii_strcasecmp(type, "checkbox") == 0 ||
                     g_ascii_strcasecmp(type, "radio") == 0))
            return ns_element_get_attr(el, "checked") != NULL;
    }
    if (!ns_el_is_submit_button(el)) return FALSE;
    const ns_node *doc = ns_node_root(el);
    const ns_node *owner = ns_form_owner(el, doc);
    if (!owner) return FALSE;
    return ns_css_first_submit_button_for(doc ? doc : owner, doc, owner, 0) == el;
}

static gboolean
ns_css_radio_group_has_checked(const ns_node *scan, const ns_node *doc,
                               const ns_node *owner, const char *name,
                               int depth)
{
    if (!scan || depth >= 512) return FALSE;
    if (ns_node_is_element_named(scan, "input")) {
        const char *type = ns_element_get_attr(scan, "type");
        if (type && g_ascii_strcasecmp(type, "radio") == 0) {
            const char *scan_name = ns_element_get_attr(scan, "name");
            if (!scan_name) scan_name = "";
            if (strcmp(scan_name, name) == 0 &&
                ns_form_owner(scan, doc) == owner &&
                ns_input_is_checked(scan))
                return TRUE;
        }
    }
    if (ns_node_is_element_named(scan, "template")) return FALSE;
    for (const ns_node *c = scan->first_child; c; c = c->next_sibling)
        if (ns_css_radio_group_has_checked(c, doc, owner, name, depth + 1))
            return TRUE;
    return FALSE;
}

static gboolean
ns_el_is_indeterminate(const ns_node *el)
{
    if (ns_node_is_element_named(el, "progress"))
        return ns_element_get_attr(el, "value") == NULL;
    if (!ns_node_is_element_named(el, "input")) return FALSE;
    const char *type = ns_element_get_attr(el, "type");
    if (!type || g_ascii_strcasecmp(type, "radio") != 0) return FALSE;
    const char *name = ns_element_get_attr(el, "name");
    if (!name) name = "";
    const ns_node *doc = ns_node_root(el);
    const ns_node *owner = ns_form_owner(el, doc);
    return !ns_css_radio_group_has_checked(doc ? doc : el, doc, owner, name, 0);
}

static gboolean
ns_el_range_state(const ns_node *el, gboolean *under, gboolean *over)
{
    if (under) *under = FALSE;
    if (over) *over = FALSE;
    if (!ns_node_is_element_named(el, "input")) return FALSE;
    const char *type = ns_element_get_attr(el, "type");
    if (!ns_input_type_has_number_value(type)) return FALSE;
    if (!ns_element_get_attr(el, "min") && !ns_element_get_attr(el, "max"))
        return FALSE;
    const char *value = ns_element_get_attr(el, "value");
    if (!value || !*value) return FALSE;
    return ns_input_value_range_state(el, value, under, over);
}

static gboolean
ns_el_is_blank(const ns_node *el)
{
    if (ns_node_is_element_named(el, "input")) {
        if (!ns_input_is_text_entry(el)) return FALSE;
        const char *value = ns_element_get_attr(el, "value");
        return !value || !*value;
    }
    if (!ns_node_is_element_named(el, "textarea")) return FALSE;
    char *txt = ns_node_collect_text(el);
    gboolean blank = TRUE;
    for (const char *p = txt ? txt : ""; *p; p++) {
        if (!is_ws(*p)) {
            blank = FALSE;
            break;
        }
    }
    g_free(txt);
    return blank;
}

static gboolean
ns_el_is_empty(const ns_node *el)
{
    for (const ns_node *c = el ? el->first_child : NULL; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT) return FALSE;
        if (c->kind == NS_NODE_TEXT && c->text && c->text[0] != '\0')
            return FALSE;
    }
    return TRUE;
}

static gboolean
ns_el_is_link(const ns_node *el)
{
    if (!ns_element_get_attr(el, "href")) return FALSE;
    return ns_node_is_element_named(el, "a") ||
           ns_node_is_element_named(el, "area");
}

static GHashTable *g_visited_urls = NULL;
static char       *g_css_doc_base = NULL;
static char       *g_css_doc_language = NULL;

void
ns_css_mark_visited(const char *abs_url)
{
    if (!abs_url || !*abs_url) return;
    if (!g_visited_urls)
        g_visited_urls = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    if (!g_hash_table_contains(g_visited_urls, abs_url))
        g_hash_table_add(g_visited_urls, g_strdup(abs_url));
}

void
ns_css_set_doc_base(const char *base_url)
{
    g_free(g_css_doc_base);
    g_css_doc_base = (base_url && *base_url) ? g_strdup(base_url) : NULL;
}

void
ns_css_set_doc_language(const char *lang)
{
    g_free(g_css_doc_language);
    g_css_doc_language = (lang && *lang) ? g_strdup(lang) : NULL;
}

static gboolean
ns_el_is_visited_link(const ns_node *el)
{
    if (!g_visited_urls || !g_css_doc_base || !ns_el_is_link(el)) return FALSE;
    const char *href = ns_element_get_attr(el, "href");
    if (!href || !*href) return FALSE;
    char *abs_url = ns_url_resolve(g_css_doc_base, href);
    if (!abs_url) return FALSE;
    gboolean v = g_hash_table_contains(g_visited_urls, abs_url);
    g_free(abs_url);
    return v;
}

static gboolean
selector_group_matches_element(const GPtrArray *group, const ns_node *el)
{
    for (guint i = 0; group && i < group->len; i++) {
        const ns_css_selector *sub = g_ptr_array_index(group, i);
        if (match_selector(sub, el)) return TRUE;
    }
    return FALSE;
}

static gboolean
ns_css_sibling_counts_for_nth(const ns_node *el, const ns_css_pseudo_pred *pc,
                              int *idx_out)
{
    int idx = 1;
    gboolean reverse = pc->kind == NS_CSS_PC_NTH_LAST_CHILD ||
                       pc->kind == NS_CSS_PC_NTH_LAST_OF_TYPE;
    gboolean typed = pc->kind == NS_CSS_PC_NTH_OF_TYPE ||
                     pc->kind == NS_CSS_PC_NTH_LAST_OF_TYPE;
    const ns_node *s = reverse ? el->next_sibling : el->prev_sibling;
    while (s) {
        if (s->kind == NS_NODE_ELEMENT &&
            (!typed || (el->name && ns_node_is_element_named(s, el->name))) &&
            (!pc->of_group || selector_group_matches_element(pc->of_group, s)))
            idx++;
        s = reverse ? s->next_sibling : s->prev_sibling;
    }
    if (pc->of_group && !selector_group_matches_element(pc->of_group, el))
        return FALSE;
    *idx_out = idx;
    return TRUE;
}

static __thread const ns_node *g_pragma_doc;
static __thread const char    *g_pragma_lang;
static __thread gboolean       g_pragma_valid;

static void
ns_css_pragma_language_scan(const ns_node *n, const char **found, int depth)
{
    if (!n || depth >= 512) return;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "meta") == 0) {
            const char *he = ns_element_get_attr(c, "http-equiv");
            const char *content = he &&
                g_ascii_strcasecmp(he, "content-language") == 0
                ? ns_element_get_attr(c, "content") : NULL;
            if (content && !strchr(content, ',')) {
                const char *s = content;
                while (*s && g_ascii_isspace((guchar)*s)) s++;
                const char *e = s;
                while (*e && !g_ascii_isspace((guchar)*e)) e++;
                if (e > s) {
                    static __thread char buf[128];
                    gsize len = (gsize)(e - s);
                    if (len >= sizeof buf) len = sizeof buf - 1;
                    memcpy(buf, s, len);
                    buf[len] = '\0';
                    *found = buf;
                }
            }
        }
        ns_css_pragma_language_scan(c, found, depth + 1);
    }
}

static const char *
ns_css_node_language(const ns_node *el)
{
    static const char xml_ns[] = "http://www.w3.org/XML/1998/namespace";
    for (const ns_node *n = el; n; n = n->parent) {
        if (n->kind != NS_NODE_ELEMENT) continue;
        const ns_attr *xa = ns_element_find_attr_ns(n, xml_ns, "lang");
        if (xa) return xa->value ? xa->value : "";
        const ns_attr *la = ns_element_find_attr_ns(n, NULL, "lang");
        if (la && !(n->flags & (NS_NODE_SVG_NS | NS_NODE_FOREIGN_NS)))
            return la->value ? la->value : "";
    }
    const ns_node *root = el;
    while (root && root->parent) root = root->parent;
    if (!g_pragma_valid || g_pragma_doc != root) {
        const char *found = NULL;
        if (root) ns_css_pragma_language_scan(root, &found, 0);
        g_pragma_doc = root;
        g_pragma_lang = found;
        g_pragma_valid = TRUE;
    }
    return g_pragma_lang ? g_pragma_lang : g_css_doc_language;
}

static gboolean
ns_css_lang_one_matches(const char *lang, const char *want)
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
ns_css_lang_matches(const ns_node *el, const char *arg)
{
    const char *lang = ns_css_node_language(el);
    if (!lang || !arg) return FALSE;
    const char *p = arg;
    const char *end = arg + strlen(arg);
    while (p < end) {
        char term = 0;
        const char *seg = css_scan_until(p, end, ",", &term);
        char *want = css_trim_dup_range(p, seg);
        gboolean ok = ns_css_lang_one_matches(lang, want);
        g_free(want);
        if (ok) return TRUE;
        p = term == ',' ? seg + 1 : seg;
    }
    return FALSE;
}

static gboolean
ns_dir_is_rtl_script(GUnicodeScript s)
{
    switch (s) {
    case G_UNICODE_SCRIPT_HEBREW:
    case G_UNICODE_SCRIPT_ARABIC:
    case G_UNICODE_SCRIPT_SYRIAC:
    case G_UNICODE_SCRIPT_THAANA:
    case G_UNICODE_SCRIPT_NKO:
    case G_UNICODE_SCRIPT_SAMARITAN:
    case G_UNICODE_SCRIPT_MANDAIC:
        return TRUE;
    default:
        return FALSE;
    }
}

static const char *
ns_dir_first_strong(const ns_node *n, int depth)
{
    if (!n || depth > 256) return NULL;
    if (n->kind == NS_NODE_TEXT && n->text) {
        for (const char *p = n->text; *p; p = g_utf8_next_char(p)) {
            gunichar c = g_utf8_get_char(p);
            if (ns_dir_is_rtl_script(g_unichar_get_script(c))) return "rtl";
            if (g_unichar_isalpha(c)) return "ltr";
        }
        return NULL;
    }
    if (n->kind != NS_NODE_ELEMENT) return NULL;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        gboolean html_ns = c->kind == NS_NODE_ELEMENT &&
            !(c->flags & (NS_NODE_SVG_NS | NS_NODE_FOREIGN_NS));
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            ((html_ns &&
              (g_ascii_strcasecmp(c->name, "script") == 0 ||
               g_ascii_strcasecmp(c->name, "style") == 0 ||
               g_ascii_strcasecmp(c->name, "textarea") == 0 ||
               g_ascii_strcasecmp(c->name, "bdi") == 0)) ||
             ns_element_get_attr(c, "dir")))
            continue;
        const char *d = ns_dir_first_strong(c, depth + 1);
        if (d) return d;
    }
    return NULL;
}

static const char *
ns_dir_first_strong_str(const char *s)
{
    if (!s) return NULL;
    for (const char *p = s; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (ns_dir_is_rtl_script(g_unichar_get_script(c))) return "rtl";
        if (g_unichar_isalpha(c)) return "ltr";
    }
    return NULL;
}

static const char *
ns_dir_form_control_value(const ns_node *n)
{
    if (!n->name) return NULL;
    if (g_ascii_strcasecmp(n->name, "textarea") == 0)
        return ns_node_editable_value(n);
    if (g_ascii_strcasecmp(n->name, "input") != 0) return NULL;
    const char *type = ns_element_get_attr(n, "type");
    if (type) {
        static const char *const uses[] = { "hidden", "text", "search", "tel",
            "url", "email", "password", "submit", "reset", "button", NULL };
        gboolean ok = FALSE;
        for (int i = 0; uses[i]; i++)
            if (g_ascii_strcasecmp(type, uses[i]) == 0) { ok = TRUE; break; }
        if (!ok) return NULL;
    }
    return ns_node_editable_value(n);
}

static const char *
ns_dir_auto_resolve(const ns_node *n)
{
    const char *val = ns_dir_form_control_value(n);
    if (val) {
        const char *d = ns_dir_first_strong_str(val);
        return d ? d : "ltr";
    }
    const char *d = ns_dir_first_strong(n, 0);
    return d ? d : "ltr";
}

const char *
ns_css_node_dir(const ns_node *el)
{
    for (const ns_node *n = el; n; n = n->parent) {
        if (n->kind != NS_NODE_ELEMENT) continue;
        const char *dir = ns_element_get_attr(n, "dir");
        gboolean is_bdi = n->name && g_ascii_strcasecmp(n->name, "bdi") == 0;
        if (dir) {
            if (g_ascii_strcasecmp(dir, "ltr") == 0) return "ltr";
            if (g_ascii_strcasecmp(dir, "rtl") == 0) return "rtl";
            if (g_ascii_strcasecmp(dir, "auto") == 0)
                return ns_dir_auto_resolve(n);
        } else if (is_bdi) {
            const char *d = ns_dir_first_strong(n, 0);
            return d ? d : "ltr";
        }
    }
    return "ltr";
}

static gboolean
ns_css_node_is_target(const ns_node *el)
{
    if (!g_target_fragment || !el) return FALSE;
    const char *eid = ns_element_get_attr(el, "id");
    if (eid && strcmp(eid, g_target_fragment) == 0) return TRUE;
    if (el->name && g_ascii_strcasecmp(el->name, "a") == 0) {
        const char *nm = ns_element_get_attr(el, "name");
        if (nm && strcmp(nm, g_target_fragment) == 0) return TRUE;
    }
    return FALSE;
}

static gboolean
ns_css_node_has_target_within(const ns_node *el, int depth)
{
    if (!el || depth >= 512) return FALSE;
    if (el->kind == NS_NODE_ELEMENT && ns_css_node_is_target(el))
        return TRUE;
    if (ns_node_is_element_named(el, "template")) return FALSE;
    for (const ns_node *c = el->first_child; c; c = c->next_sibling)
        if (ns_css_node_has_target_within(c, depth + 1))
            return TRUE;
    return FALSE;
}

static gboolean
ns_css_value_matches_pattern(const char *value, const char *pattern)
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
ns_css_node_will_validate(const ns_node *el)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !el->name) return FALSE;
    gboolean is_input = strcmp(el->name, "input") == 0;
    if (!is_input &&
        strcmp(el->name, "textarea") != 0 &&
        strcmp(el->name, "select") != 0)
        return FALSE;
    if (ns_element_effectively_disabled(el)) return FALSE;
    if (ns_form_control_readonly_bars_validation(el)) return FALSE;
    const char *type = is_input ? ns_element_get_attr(el, "type") : NULL;
    if (type && (g_ascii_strcasecmp(type, "submit") == 0 ||
                 g_ascii_strcasecmp(type, "button") == 0 ||
                 g_ascii_strcasecmp(type, "reset")  == 0 ||
                 g_ascii_strcasecmp(type, "image")  == 0 ||
                 g_ascii_strcasecmp(type, "hidden") == 0))
        return FALSE;
    return TRUE;
}

static char *
ns_css_control_value_dup(const ns_node *el)
{
    if (!el || !el->name) return g_strdup("");
    if (strcmp(el->name, "textarea") == 0)
        return ns_node_collect_text(el);
    if (strcmp(el->name, "select") == 0) {
        const ns_node *opt = ns_element_get_attr(el, "multiple")
            ? ns_select_first_selected_option(el)
            : ns_select_chosen_option(el);
        return opt ? ns_option_value_dup(opt) : g_strdup("");
    }
    return g_strdup(ns_element_get_attr(el, "value") ?
                    ns_element_get_attr(el, "value") : "");
}

static gboolean
ns_css_control_is_valid(const ns_node *el)
{
    if (!ns_css_node_will_validate(el)) return FALSE;
    const char *custom = ns_element_get_attr(el, NS_CUSTOM_VALIDITY_ATTR);
    if (custom && *custom) return FALSE;
    char *owned = ns_css_control_value_dup(el);
    const char *value = owned ? owned : "";
    gboolean valid = TRUE;
    const char *type = el->name && strcmp(el->name, "input") == 0
        ? ns_element_get_attr(el, "type") : NULL;
    if (ns_form_control_supports_required(el) &&
        ns_element_get_attr(el, "required") &&
        ns_form_control_value_missing(el, value, ns_node_root(el)))
        valid = FALSE;
    if (valid && *value && type) {
        if (g_ascii_strcasecmp(type, "email") == 0) {
            if (!ns_input_email_value_valid(el, value))
                valid = FALSE;
        } else if (g_ascii_strcasecmp(type, "url") == 0) {
            if (!ns_url_is_valid_absolute(value))
                valid = FALSE;
        } else if (ns_input_type_has_number_value(type)) {
            double parsed;
            if (!ns_input_value_to_number(type, value, &parsed)) valid = FALSE;
        }
        if (valid) {
            gboolean under = FALSE, over = FALSE;
            if (ns_input_value_range_state(el, value, &under, &over) &&
                (under || over))
                valid = FALSE;
        }
        if (valid && ns_input_value_step_mismatch(el, value))
            valid = FALSE;
    }
    if (valid && *value &&
        el->name && strcmp(el->name, "input") == 0 &&
        ns_input_type_supports_text_constraints(type) &&
        !ns_css_value_matches_pattern(value, ns_element_get_attr(el, "pattern")))
        valid = FALSE;
    if (valid && *value && ns_form_control_length_limits_apply(el)) {
        glong vlen = (glong)g_utf8_strlen(value, -1);
        const char *minlen = ns_element_get_attr(el, "minlength");
        const char *maxlen = ns_element_get_attr(el, "maxlength");
        if (minlen && vlen < (glong)ns_parse_int(minlen, 0, 0, 1000000))
            valid = FALSE;
        if (maxlen && vlen > (glong)ns_parse_int(maxlen, 0, 0, 1000000))
            valid = FALSE;
    }
    g_free(owned);
    return valid;
}

static gboolean
has_simple_scope_pseudo(const ns_css_simple *sel)
{
    for (guint i = 0; sel && sel->pseudos && i < sel->pseudos->len; i++) {
        const ns_css_pseudo_pred *pc =
            &g_array_index(sel->pseudos, ns_css_pseudo_pred, i);
        if (pc->kind == NS_CSS_PC_SCOPE) return TRUE;
    }
    return FALSE;
}

static const ns_node *
next_element_sibling(const ns_node *n)
{
    const ns_node *s = n ? n->next_sibling : NULL;
    while (s && s->kind != NS_NODE_ELEMENT) s = s->next_sibling;
    return s;
}

static gboolean
relative_chain_matches(const ns_css_selector *rel, const ns_node *anchor,
                       const ns_node *base, guint idx, int depth);

static gboolean
relative_try_candidate(const ns_css_selector *rel, const ns_node *anchor,
                       const ns_node *candidate, guint idx, int depth)
{
    if (!candidate || candidate->kind != NS_NODE_ELEMENT) return FALSE;
    const ns_css_simple *cmp = g_ptr_array_index(rel->compounds, idx);
    if (!match_simple(cmp, candidate)) return FALSE;
    return relative_chain_matches(rel, anchor, candidate, idx + 1, depth + 1);
}

static gboolean
relative_descendant_matches(const ns_css_selector *rel, const ns_node *anchor,
                            const ns_node *base, guint idx, int depth)
{
    if (depth >= 512) return FALSE;
    for (const ns_node *c = base ? base->first_child : NULL; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT) continue;
        if (relative_try_candidate(rel, anchor, c, idx, depth)) return TRUE;
        if (relative_descendant_matches(rel, anchor, c, idx, depth + 1))
            return TRUE;
    }
    return FALSE;
}

static gboolean
relative_chain_matches(const ns_css_selector *rel, const ns_node *anchor,
                       const ns_node *base, guint idx, int depth)
{
    if (depth >= 512) return FALSE;
    if (!rel || idx >= rel->compounds->len) return TRUE;
    const ns_css_simple *cmp = g_ptr_array_index(rel->compounds, idx);
    ns_css_comb comb = g_array_index(rel->combinators, ns_css_comb, idx);
    if (idx == 0 && (comb == NS_CSS_COMB_NONE ||
                     comb == NS_CSS_COMB_DESCENDANT)) {
        if (has_simple_scope_pseudo(cmp) &&
            relative_try_candidate(rel, anchor, anchor, idx, depth))
            return TRUE;
        return relative_descendant_matches(rel, anchor, anchor, idx, depth + 1);
    }
    if (comb == NS_CSS_COMB_CHILD) {
        for (const ns_node *c = base ? base->first_child : NULL; c; c = c->next_sibling)
            if (relative_try_candidate(rel, anchor, c, idx, depth))
                return TRUE;
        return FALSE;
    }
    if (comb == NS_CSS_COMB_ADJACENT)
        return relative_try_candidate(rel, anchor, next_element_sibling(base),
                                      idx, depth);
    if (comb == NS_CSS_COMB_SIBLING) {
        for (const ns_node *s = next_element_sibling(base); s; s = next_element_sibling(s))
            if (relative_try_candidate(rel, anchor, s, idx, depth))
                return TRUE;
        return FALSE;
    }
    return relative_descendant_matches(rel, anchor, base, idx, depth + 1);
}

static gboolean
has_relative_matches(const ns_css_selector *rel, const ns_node *anchor)
{
    if (!rel || rel->pseudo_element != NS_CSS_PE_NONE) return FALSE;
    const ns_node *prev_scope = g_css_match_scope;
    g_css_match_scope = anchor;
    gboolean matched = relative_chain_matches(rel, anchor, anchor, 0, 0);
    g_css_match_scope = prev_scope;
    return matched;
}

typedef struct has_memo_key {
    const void *group;
    const void *anchor;
} has_memo_key;

static guint
has_memo_hash(gconstpointer p)
{
    const has_memo_key *k = p;
    guintptr x = (guintptr)k->group * 2654435761u ^
                 (guintptr)k->anchor * 0x9E3779B9u;
    return (guint)(x ^ (x >> 16));
}

static gboolean
has_memo_equal(gconstpointer pa, gconstpointer pb)
{
    const has_memo_key *a = pa, *b = pb;
    return a->group == b->group && a->anchor == b->anchor;
}

static GHashTable *g_has_memo;

static gboolean
has_group_matches(const GPtrArray *group, const ns_node *anchor)
{
    has_memo_key probe = { group, anchor };
    gpointer cached;
    if (g_has_memo &&
        g_hash_table_lookup_extended(g_has_memo, &probe, NULL, &cached))
        return GPOINTER_TO_INT(cached) != 0;
    gboolean matched = FALSE;
    for (guint j = 0; j < group->len && !matched; j++) {
        const ns_css_selector *sub = g_ptr_array_index(group, j);
        if (has_relative_matches(sub, anchor)) matched = TRUE;
    }
    if (g_has_memo)
        g_hash_table_replace(g_has_memo,
                             g_memdup2(&probe, sizeof probe),
                             GINT_TO_POINTER(matched ? 1 : 0));
    return matched;
}

static gboolean
ns_css_html_ci_attr(const char *name)
{
    static const char *const list[] = {
        "accept", "accept-charset", "align", "alink", "axis", "bgcolor",
        "charset", "checked", "clear", "codetype", "color", "compact",
        "declare", "defer", "dir", "direction", "disabled", "enctype",
        "face", "frame", "hreflang", "http-equiv", "lang", "language",
        "link", "media", "method", "multiple", "nohref", "noresize",
        "noshade", "nowrap", "readonly", "rel", "rev", "rules", "scope",
        "scrolling", "selected", "shape", "target", "text", "type",
        "valign", "valuetype", "vlink",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(list); i++)
        if (g_ascii_strcasecmp(name, list[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
match_simple(const ns_css_simple *sel, const ns_node *el)
{
    if (sel->never_match) return FALSE;
    if (!el || el->kind != NS_NODE_ELEMENT) return FALSE;
    if (sel->ns_none) {
        gboolean null_ns = (el->flags & NS_NODE_FOREIGN_NS) &&
                           !(el->flags & NS_NODE_SVG_NS) &&
                           !ns_element_get_attr(el, "data-nd-ns-uri");
        if (!null_ns) return FALSE;
    }
    if (sel->type && strcmp(sel->type, "*") != 0) {
        if (!el->name) return FALSE;
        if (el->flags & (NS_NODE_SVG_NS | NS_NODE_FOREIGN_NS)) {
            if (strcmp(sel->type, el->name) != 0) return FALSE;
        }
        else if (g_ascii_tolower((unsigned char)el->name[0]) !=
                     g_ascii_tolower((unsigned char)sel->type[0]) ||
                 g_ascii_strcasecmp(sel->type, el->name) != 0) {
            return FALSE;
        }
    }
    if (sel->id) {
        const char *id = ns_element_get_attr(el, "id");
        if (!id || strcmp(id, sel->id) != 0) return FALSE;
    }
    if (sel->classes->len > 0) {
        for (guint i = 0; i < sel->classes->len; i++) {
            const char *want = g_ptr_array_index(sel->classes, i);
            gsize want_len = sel->class_lens && i < sel->class_lens->len
                ? g_array_index(sel->class_lens, gsize, i)
                : strlen(want);
            if (!ns_node_has_class(el, want, want_len))
                return FALSE;
        }
    }
    if (sel->attrs && sel->attrs->len > 0) {
        guint64 elbloom = ns_node_attr_bloom(el);
        gboolean html_doc = !(el->flags & NS_NODE_XML_DOC) &&
                             !(el->flags & (NS_NODE_FOREIGN_NS | NS_NODE_SVG_NS));
        for (guint i = 0; i < sel->attrs->len; i++) {
            const ns_css_attr_pred *a = &g_array_index(sel->attrs, ns_css_attr_pred, i);
            if (a->name_bit && (elbloom & a->name_bit) == 0) return FALSE;
            const char *v = ns_element_get_attr(el, a->name);
            if (a->op == NS_CSS_ATTR_PRESENT) {
                if (!v) return FALSE;
            } else {
                if (!v || !a->value) return FALSE;
                gsize vl = strlen(v), wl = strlen(a->value);
                gboolean ci = a->case_insensitive ||
                    (!a->case_sensitive && html_doc &&
                     ns_css_html_ci_attr(a->name));
                switch (a->op) {
                case NS_CSS_ATTR_EQ:
                    if (ci ? g_ascii_strcasecmp(v, a->value)
                           : strcmp(v, a->value)) return FALSE;
                    break;
                case NS_CSS_ATTR_PREFIX:
                    if (wl == 0 || vl < wl) return FALSE;
                    if (ci ? g_ascii_strncasecmp(v, a->value, wl)
                           : strncmp(v, a->value, wl)) return FALSE;
                    break;
                case NS_CSS_ATTR_SUFFIX:
                    if (wl == 0 || vl < wl) return FALSE;
                    if (ci ? g_ascii_strcasecmp(v + vl - wl, a->value)
                           : strcmp(v + vl - wl, a->value)) return FALSE;
                    break;
                case NS_CSS_ATTR_SUBSTR:
                    if (wl == 0) return FALSE;
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
                case NS_CSS_ATTR_WORD: {
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
                case NS_CSS_ATTR_HYPHEN: {
                    if (vl < wl) return FALSE;
                    if (ci ? g_ascii_strncasecmp(v, a->value, wl)
                           : strncmp(v, a->value, wl)) return FALSE;
                    if (vl > wl && v[wl] != '-') return FALSE;
                    break;
                }
                case NS_CSS_ATTR_PRESENT: break;
                }
            }
        }
    }
    if (sel->pseudos && sel->pseudos->len > 0) {
        for (guint i = 0; i < sel->pseudos->len; i++) {
            const ns_css_pseudo_pred *pc =
                &g_array_index(sel->pseudos, ns_css_pseudo_pred, i);
            switch (pc->kind) {
            case NS_CSS_PC_FIRST_CHILD: {
                const ns_node *s = el->prev_sibling;
                while (s && s->kind != NS_NODE_ELEMENT) s = s->prev_sibling;
                if (s) return FALSE;
                break;
            }
            case NS_CSS_PC_LAST_CHILD: {
                const ns_node *s = el->next_sibling;
                while (s && s->kind != NS_NODE_ELEMENT) s = s->next_sibling;
                if (s) return FALSE;
                break;
            }
            case NS_CSS_PC_ONLY_CHILD: {
                const ns_node *s = el->prev_sibling;
                while (s && s->kind != NS_NODE_ELEMENT) s = s->prev_sibling;
                if (s) return FALSE;
                s = el->next_sibling;
                while (s && s->kind != NS_NODE_ELEMENT) s = s->next_sibling;
                if (s) return FALSE;
                break;
            }
            case NS_CSS_PC_ONLY_OF_TYPE: {
                if (!el->name) return FALSE;
                for (const ns_node *s = el->prev_sibling; s; s = s->prev_sibling)
                    if (ns_node_is_element_named(s, el->name)) return FALSE;
                for (const ns_node *s = el->next_sibling; s; s = s->next_sibling)
                    if (ns_node_is_element_named(s, el->name)) return FALSE;
                break;
            }
            case NS_CSS_PC_FIRST_OF_TYPE: {
                if (!el->name) return FALSE;
                for (const ns_node *s = el->prev_sibling; s; s = s->prev_sibling)
                    if (ns_node_is_element_named(s, el->name)) return FALSE;
                break;
            }
            case NS_CSS_PC_LAST_OF_TYPE: {
                if (!el->name) return FALSE;
                for (const ns_node *s = el->next_sibling; s; s = s->next_sibling)
                    if (ns_node_is_element_named(s, el->name)) return FALSE;
                break;
            }
            case NS_CSS_PC_EMPTY:
                if (!ns_el_is_empty(el)) return FALSE;
                break;
            case NS_CSS_PC_ROOT:
                if (!el->parent || el->parent->kind != NS_NODE_DOCUMENT ||
                    (el->parent->flags & NS_NODE_FRAGMENT))
                    return FALSE;
                break;
            case NS_CSS_PC_SCOPE:
                if (g_css_match_scope) {
                    if (el != g_css_match_scope) return FALSE;
                } else if (el->parent && el->parent->kind == NS_NODE_ELEMENT) {
                    return FALSE;
                }
                break;
            case NS_CSS_PC_CHECKED:
                if (!ns_el_is_checked(el))
                    return FALSE;
                break;
            case NS_CSS_PC_DISABLED:
                if (!ns_element_supports_disabled(el) ||
                    !ns_element_effectively_disabled(el))
                    return FALSE;
                break;
            case NS_CSS_PC_ENABLED:
                if (!ns_element_supports_disabled(el) ||
                    ns_element_effectively_disabled(el))
                    return FALSE;
                break;
            case NS_CSS_PC_REQUIRED:
                if (!ns_form_control_supports_required(el) ||
                    !ns_element_get_attr(el, "required"))
                    return FALSE;
                break;
            case NS_CSS_PC_OPTIONAL:
                if (!ns_form_control_supports_required(el) ||
                    ns_element_get_attr(el, "required"))
                    return FALSE;
                break;
            case NS_CSS_PC_VALID:
                if (!ns_css_node_will_validate(el) ||
                    !ns_css_control_is_valid(el))
                    return FALSE;
                break;
            case NS_CSS_PC_INVALID:
                if (!ns_css_node_will_validate(el) ||
                    ns_css_control_is_valid(el))
                    return FALSE;
                break;
            case NS_CSS_PC_IN_RANGE: {
                gboolean under = FALSE, over = FALSE;
                if (!ns_el_range_state(el, &under, &over) || under || over)
                    return FALSE;
                break;
            }
            case NS_CSS_PC_OUT_OF_RANGE: {
                gboolean under = FALSE, over = FALSE;
                if (!ns_el_range_state(el, &under, &over) || (!under && !over))
                    return FALSE;
                break;
            }
            case NS_CSS_PC_DEFAULT:
                if (!ns_el_is_default(el)) return FALSE;
                break;
            case NS_CSS_PC_INDETERMINATE:
                if (!ns_el_is_indeterminate(el)) return FALSE;
                break;
            case NS_CSS_PC_NTH_CHILD:
            case NS_CSS_PC_NTH_LAST_CHILD:
            case NS_CSS_PC_NTH_LAST_OF_TYPE:
            case NS_CSS_PC_NTH_OF_TYPE: {
                int idx = 1;
                if (!ns_css_sibling_counts_for_nth(el, pc, &idx)) return FALSE;
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
            case NS_CSS_PC_ANY_LINK:
                if (!ns_el_is_link(el)) return FALSE;
                break;
            case NS_CSS_PC_LINK:
                if (!ns_el_is_link(el) || ns_el_is_visited_link(el))
                    return FALSE;
                break;
            case NS_CSS_PC_VISITED:
                if (!ns_el_is_visited_link(el)) return FALSE;
                break;
            case NS_CSS_PC_HOVER: {
                if (!g_css_hover_node) return FALSE;
                gboolean on = FALSE;
                for (const ns_node *h = g_css_hover_node; h; h = h->parent)
                    if (h == el) { on = TRUE; break; }
                if (!on) return FALSE;
                break;
            }
            case NS_CSS_PC_ACTIVE: {
                if (!g_css_active_node) return FALSE;
                gboolean pressed = FALSE;
                for (const ns_node *a = g_css_active_node; a; a = a->parent)
                    if (a == el) { pressed = TRUE; break; }
                if (!pressed) return FALSE;
                break;
            }
            case NS_CSS_PC_FOCUS:
                if (!g_css_focus_node || el != g_css_focus_node) return FALSE;
                break;
            case NS_CSS_PC_FOCUS_WITHIN: {
                if (!g_css_focus_node) return FALSE;
                const ns_node *f = g_css_focus_node;
                gboolean within = FALSE;
                for (; f; f = f->parent)
                    if (f == el) { within = TRUE; break; }
                if (!within) return FALSE;
                break;
            }
            case NS_CSS_PC_TARGET: {
                if (!ns_css_node_is_target(el)) return FALSE;
                break;
            }
            case NS_CSS_PC_TARGET_WITHIN:
                if (!g_target_fragment ||
                    !ns_css_node_has_target_within(el, 0))
                    return FALSE;
                break;
            case NS_CSS_PC_DEFINED:
                if (!el->name) return FALSE;
                if (!strchr(el->name, '-')) break;
                if (ns_css_is_defined_element(el->name)) break;
                return FALSE;
            case NS_CSS_PC_PLACEHOLDER_SHOWN:
                if (!ns_el_placeholder_shown(el)) return FALSE;
                break;
            case NS_CSS_PC_READ_WRITE:
                if (!ns_el_is_read_write(el)) return FALSE;
                break;
            case NS_CSS_PC_READ_ONLY:
                if (ns_el_is_read_write(el)) return FALSE;
                break;
            case NS_CSS_PC_BLANK:
                if (!ns_el_is_blank(el)) return FALSE;
                break;
            case NS_CSS_PC_LANG:
                if (!ns_css_lang_matches(el, pc->arg)) return FALSE;
                break;
            case NS_CSS_PC_DIR:
                if (!pc->arg || strcmp(ns_css_node_dir(el), pc->arg) != 0)
                    return FALSE;
                break;
            case NS_CSS_PC_OPEN:
                if ((!ns_node_is_element_named(el, "details") &&
                     !ns_node_is_element_named(el, "dialog")) ||
                    !ns_element_get_attr(el, "open"))
                    return FALSE;
                break;
            case NS_CSS_PC_POPOVER_OPEN:
                if (!ns_element_get_attr(el, "popover") ||
                    !ns_element_get_attr(el, "data-nd-popover-open"))
                    return FALSE;
                break;
            case NS_CSS_PC_MODAL:
                if (ns_dom_active_modal() != el) return FALSE;
                break;
            case NS_CSS_PC_HEADING: {
                int level = 0;
                if (el->kind == NS_NODE_ELEMENT && el->name &&
                    el->name[0] == 'h' && el->name[1] >= '1' &&
                    el->name[1] <= '6' && el->name[2] == '\0')
                    level = el->name[1] - '0';
                if (level == 0) return FALSE;
                if (pc->arg) {
                    char **items = g_strsplit(pc->arg, ",", -1);
                    gboolean any = FALSE;
                    for (int hi = 0; items[hi] && !any; hi++) {
                        int v = 0;
                        if (anb_int_strict(g_strstrip(items[hi]), &v) &&
                            level == v)
                            any = TRUE;
                    }
                    g_strfreev(items);
                    if (!any) return FALSE;
                }
                break;
            }
            }
        }
    }
    if (sel->matches_any) {
        for (guint i = 0; i < sel->matches_any->len; i++) {
            const GPtrArray *group = g_ptr_array_index(sel->matches_any, i);
            gboolean any = FALSE;
            for (guint j = 0; j < group->len; j++) {
                const ns_css_selector *sub = g_ptr_array_index(group, j);
                if (match_selector(sub, el)) { any = TRUE; break; }
            }
            if (!any) return FALSE;
        }
    }
    if (sel->matches_none) {
        for (guint i = 0; i < sel->matches_none->len; i++) {
            const GPtrArray *group = g_ptr_array_index(sel->matches_none, i);
            for (guint j = 0; j < group->len; j++) {
                const ns_css_selector *sub = g_ptr_array_index(group, j);
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


static char *
css_add_leading_zeros(char *v)
{
    if (!v) return NULL;
    gboolean needs = FALSE;
    for (const char *p = v; *p; p++) {
        if (*p != '.' || !g_ascii_isdigit((guchar)p[1])) continue;
        char prev = p == v ? 0 : p[-1];
        if (prev == 0 || prev == ' ' || prev == '\t' || prev == '(' ||
            prev == ',' || prev == '+' || prev == '-' || prev == '/' ||
            prev == '*') { needs = TRUE; break; }
    }
    if (!needs) return v;
    GString *out = g_string_new(NULL);
    for (const char *p = v; *p; p++) {
        if (*p == '.' && g_ascii_isdigit((guchar)p[1])) {
            char prev = p == v ? 0 : p[-1];
            if (prev == 0 || prev == ' ' || prev == '\t' || prev == '(' ||
                prev == ',' || prev == '+' || prev == '-' || prev == '/' ||
                prev == '*')
                g_string_append_c(out, '0');
        }
        g_string_append_c(out, *p);
    }
    g_free(v);
    return g_string_free(out, FALSE);
}

char *
ns_inline_style_get(const char *style, const char *prop)
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
        if (match) return css_add_leading_zeros(value);
        g_free(value);
        p = term == ';' ? vend + 1 : vend;
    }

    int pid = ns_css_prop_id(prop);
    if (pid >= 0) {
        char *wrapped = g_strconcat("*{", style, "}", NULL);
        ns_css_stylesheet *sheet = ns_css_stylesheet_parse(wrapped, -1);
        g_free(wrapped);
        if (sheet) {
            char *result = NULL;
            for (guint ri = 0; ri < sheet->rules->len && !result; ri++) {
                ns_css_rule *r = g_ptr_array_index(sheet->rules, ri);
                for (guint di = 0; di < r->decls->len; di++) {
                    ns_css_decl *d = &g_array_index(r->decls, ns_css_decl, di);
                    if ((int)d->prop == pid && d->value) {
                        result = ns_css_value_serialize(d->value);
                        break;
                    }
                }
            }
            ns_css_stylesheet_free(sheet);
            if (result) return result;
        }
    }
    return NULL;
}

gboolean
ns_inline_value_strip_important(char *value)
{
    gboolean important = FALSE;
    css_strip_important(value, &important);
    return important;
}

char *
ns_inline_style_set(const char *style, const char *prop, const char *value)
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
ns_css_parse_selector_list(const char *text)
{
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)ns_css_selector_free);
    if (!text) return out;
    const char *p = text;
    const char *end = text + strlen(text);
    gboolean expect_selector = TRUE;
    while (p < end) {
        while (p < end && is_ws(*p)) p++;
        if (p >= end) break;
        if (*p == ',') {
            g_sel_parse_error = TRUE;
            p++;
            expect_selector = TRUE;
            continue;
        }
        const char *iter_start = p;
        ns_css_selector *sel = parse_one_selector(&p, end, 0);
        if (sel) {
            g_ptr_array_add(out, sel);
            expect_selector = FALSE;
        }
        while (p < end && is_ws(*p)) p++;
        if (p < end && *p == ',') { p++; expect_selector = TRUE; }
        else if (p == iter_start) break;
    }
    if (expect_selector)
        g_sel_parse_error = TRUE;
    return out;
}

GPtrArray *
ns_css_parse_selector_list_checked(const char *text, gboolean *out_valid)
{
    g_sel_parse_error = FALSE;
    g_sel_ns_prefix = FALSE;
    GPtrArray *out = ns_css_parse_selector_list(text);
    if (out_valid)
        *out_valid = !g_sel_parse_error && !g_sel_ns_prefix && out->len > 0;
    g_sel_parse_error = FALSE;
    g_sel_ns_prefix = FALSE;
    return out;
}

gboolean
ns_css_selector_matches(const ns_css_selector *sel, const ns_node *el)
{
    return match_selector(sel, el);
}

static gboolean
match_complex_chain(const ns_css_selector *sel, int idx, const ns_node *cur)
{
    if (idx <= 0) return TRUE;
    ns_css_comb comb = g_array_index(sel->combinators, ns_css_comb, idx);
    const ns_css_simple *prev = g_ptr_array_index(sel->compounds, idx - 1);
    if (comb == NS_CSS_COMB_CHILD) {
        const ns_node *p = cur->parent;
        return p && match_simple(prev, p) &&
               match_complex_chain(sel, idx - 1, p);
    }
    if (comb == NS_CSS_COMB_ADJACENT) {
        const ns_node *s = cur->prev_sibling;
        while (s && s->kind != NS_NODE_ELEMENT) s = s->prev_sibling;
        return s && match_simple(prev, s) &&
               match_complex_chain(sel, idx - 1, s);
    }
    if (comb == NS_CSS_COMB_SIBLING) {
        for (const ns_node *s = cur->prev_sibling; s; s = s->prev_sibling)
            if (s->kind == NS_NODE_ELEMENT && match_simple(prev, s) &&
                match_complex_chain(sel, idx - 1, s))
                return TRUE;
        return FALSE;
    }
    for (const ns_node *p = cur->parent; p; p = p->parent) {
        if (p->kind == NS_NODE_DOCUMENT) break;
        if (match_simple(prev, p) && match_complex_chain(sel, idx - 1, p))
            return TRUE;
    }
    return FALSE;
}

static gboolean
match_selector_structural(const ns_css_selector *sel, const ns_node *el)
{
    if (!sel || sel->compounds->len == 0) return FALSE;
    int idx = (int)sel->compounds->len - 1;
    if (!match_simple(g_ptr_array_index(sel->compounds, idx), el)) return FALSE;
    return match_complex_chain(sel, idx, el);
}

static gboolean
match_selector(const ns_css_selector *sel, const ns_node *el)
{
    if (!sel) return FALSE;
    if (sel->pseudo_element != NS_CSS_PE_NONE) return FALSE;
    return match_selector_structural(sel, el);
}

static gboolean
match_selector_for_pe(const ns_css_selector *sel, const ns_node *el,
                      ns_css_pseudo_element pe)
{
    if (!sel) return FALSE;
    if (sel->pseudo_element != pe) return FALSE;
    return match_selector_structural(sel, el);
}

static gboolean
selector_group_matches_with_scope(const GPtrArray *group, const ns_node *el,
                                  const ns_node *scope)
{
    const ns_node *prev = g_css_match_scope;
    g_css_match_scope = scope;
    gboolean matched = FALSE;
    for (guint i = 0; group && i < group->len; i++) {
        const ns_css_selector *sel = g_ptr_array_index(group, i);
        if (match_selector(sel, el)) {
            matched = TRUE;
            break;
        }
    }
    g_css_match_scope = prev;
    return matched;
}

static gboolean
css_scope_root_matches(const ns_css_scope *scope, const ns_node *el)
{
    return selector_group_matches_with_scope(scope ? scope->roots : NULL,
                                            el, g_css_match_scope);
}

static int
css_scope_hops(const ns_node *root, const ns_node *el)
{
    int hops = 0;
    for (const ns_node *n = el; n; n = n->parent, hops++)
        if (n == root) return hops;
    return INT_MAX;
}

static gboolean
css_scope_limit_excludes(const ns_css_scope *scope, const ns_node *root,
                         const ns_node *el)
{
    if (!scope || !scope->limits) return FALSE;
    for (const ns_node *n = el; n; n = n->parent) {
        if (n->kind == NS_NODE_ELEMENT &&
            selector_group_matches_with_scope(scope->limits, n, root))
            return TRUE;
        if (n == root) break;
    }
    return FALSE;
}

static gboolean
css_scope_contains(const ns_css_scope *scope, const ns_node *root,
                   const ns_node *el)
{
    if (!scope || !root || !el) return FALSE;
    if (css_scope_hops(root, el) == INT_MAX) return FALSE;
    return !css_scope_limit_excludes(scope, root, el);
}

static gboolean
css_scope_applies_to(const ns_css_scope *scope, const ns_node *el)
{
    for (const ns_node *root = el; root; root = root->parent) {
        if (root->kind != NS_NODE_ELEMENT) continue;
        if (!css_scope_root_matches(scope, root)) continue;
        if (css_scope_contains(scope, root, el)) return TRUE;
    }
    return FALSE;
}

static gboolean
rule_outer_scopes_apply(const ns_css_rule *r, guint upto,
                        const ns_node *root, const ns_node *el)
{
    for (guint i = 0; r && r->scopes && i < upto; i++) {
        const ns_css_scope *scope = g_ptr_array_index(r->scopes, i);
        if (!css_scope_applies_to(scope, root)) return FALSE;
        if (!css_scope_applies_to(scope, el)) return FALSE;
    }
    return TRUE;
}

static gboolean
rule_selector_matches(const ns_css_rule *r, const ns_css_selector *sel,
                      const ns_node *el, ns_css_pseudo_element pe,
                      int *scope_order)
{
    if (scope_order) *scope_order = 0;
    if (!r || !r->scopes || r->scopes->len == 0) {
        return pe == NS_CSS_PE_NONE ? match_selector(sel, el)
                                    : match_selector_for_pe(sel, el, pe);
    }
    guint inner_i = r->scopes->len - 1;
    const ns_css_scope *inner = g_ptr_array_index(r->scopes, inner_i);
    int best = 0;
    for (const ns_node *root = el; root; root = root->parent) {
        if (root->kind != NS_NODE_ELEMENT) continue;
        if (!css_scope_root_matches(inner, root)) continue;
        if (!css_scope_contains(inner, root, el)) continue;
        if (!rule_outer_scopes_apply(r, inner_i, root, el)) continue;
        const ns_node *prev = g_css_match_scope;
        g_css_match_scope = root;
        gboolean matched = pe == NS_CSS_PE_NONE
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

static ns_style *g_style_pool[16384];
static int g_style_pool_n;

static ns_style *
ns_style_alloc(void)
{
    if (g_style_pool_n > 0) {
        ns_style *s = g_style_pool[--g_style_pool_n];
        memset(s, 0, sizeof(*s));
        return s;
    }
    return g_new0(ns_style, 1);
}

static void
ns_style_free(ns_style *s)
{
    if (!s) return;
    if (s->ref > 0) { s->ref--; return; }
    for (int i = 0; i < NS_CSS_PROP_COUNT; i++)
        if (s->values[i]) ns_css_value_free(s->values[i]);
    ns_style_free(s->before);
    ns_style_free(s->after);
    ns_style_free(s->first_letter);
    ns_style_free(s->first_line);
    ns_style_free(s->placeholder);
    ns_style_free(s->selection);
    ns_style_free(s->marker);
    ns_style_free(s->backdrop);
    if (s->vars) ns_var_map_unref(s->vars);
    if (g_style_pool_n < (int)G_N_ELEMENTS(g_style_pool))
        g_style_pool[g_style_pool_n++] = s;
    else
        g_free(s);
}

const char *
ns_style_keyword(const ns_style *s, ns_css_prop p)
{
    if (!s) return NULL;
    ns_css_value *v = s->values[p];
    if (!v || v->kind != NS_CSS_V_KEYWORD) return NULL;
    return v->u.keyword;
}

static void
ns_css_alpha_serialize(guint8 a, char *buf, gsize cap)
{
    double f = a / 255.0;
    for (int prec = 1; prec <= 5; prec++) {
        char fmt[8];
        g_snprintf(fmt, sizeof fmt, "%%.%df", prec);
        g_ascii_formatd(buf, (int)cap, fmt, f);
        if ((int)(g_ascii_strtod(buf, NULL) * 255.0 + 0.5) == a) break;
    }
    char *dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *end = '\0';
    }
}

char *
ns_css_value_serialize(const ns_css_value *v)
{
    if (!v) return g_strdup("");
    switch (v->kind) {
    case NS_CSS_V_KEYWORD:
        return g_strdup(v->u.keyword ? v->u.keyword : "");
    case NS_CSS_V_COLOR:
        if (v->u.color.a == 255)
            return g_strdup_printf("rgb(%u, %u, %u)",
                v->u.color.r, v->u.color.g, v->u.color.b);
        {
            char ab[16];
            ns_css_alpha_serialize(v->u.color.a, ab, sizeof ab);
            return g_strdup_printf("rgba(%u, %u, %u, %s)",
                v->u.color.r, v->u.color.g, v->u.color.b, ab);
        }
    case NS_CSS_V_LENGTH: {
        const char *unit = "";
        switch (v->u.length.unit) {
        case NS_CSS_UNIT_PX:      unit = "px"; break;
        case NS_CSS_UNIT_EM:      unit = "em"; break;
        case NS_CSS_UNIT_REM:     unit = "rem"; break;
        case NS_CSS_UNIT_PERCENT: unit = "%";  break;
        case NS_CSS_UNIT_NUMBER:  unit = "";   break;
        case NS_CSS_UNIT_VW:      unit = "vw"; break;
        case NS_CSS_UNIT_VH:      unit = "vh"; break;
        case NS_CSS_UNIT_VMIN:    unit = "vmin"; break;
        case NS_CSS_UNIT_VMAX:    unit = "vmax"; break;
        case NS_CSS_UNIT_CQW:     unit = "cqw"; break;
        case NS_CSS_UNIT_CQH:     unit = "cqh"; break;
        case NS_CSS_UNIT_CQMIN:   unit = "cqmin"; break;
        case NS_CSS_UNIT_CQMAX:   unit = "cqmax"; break;
        case NS_CSS_UNIT_EX:      unit = "ex";  break;
        case NS_CSS_UNIT_CH:      unit = "ch";  break;
        case NS_CSS_UNIT_CAP:     unit = "cap"; break;
        case NS_CSS_UNIT_IC:      unit = "ic";  break;
        }
        return g_strdup_printf("%g%s", v->u.length.v, unit);
    }
    case NS_CSS_V_SIZE: {
        GString *s = g_string_new(NULL);
        if (v->u.size.w_auto) {
            g_string_append(s, "auto");
        } else {
            const char *unit = "";
            switch (v->u.size.w_unit) {
            case NS_CSS_UNIT_PX:      unit = "px"; break;
            case NS_CSS_UNIT_EM:      unit = "em"; break;
            case NS_CSS_UNIT_REM:     unit = "rem"; break;
            case NS_CSS_UNIT_PERCENT: unit = "%";  break;
            case NS_CSS_UNIT_NUMBER:  unit = "";   break;
            case NS_CSS_UNIT_VW:      unit = "vw"; break;
            case NS_CSS_UNIT_VH:      unit = "vh"; break;
            case NS_CSS_UNIT_VMIN:    unit = "vmin"; break;
            case NS_CSS_UNIT_VMAX:    unit = "vmax"; break;
            case NS_CSS_UNIT_CQW:     unit = "cqw"; break;
            case NS_CSS_UNIT_CQH:     unit = "cqh"; break;
            case NS_CSS_UNIT_CQMIN:   unit = "cqmin"; break;
            case NS_CSS_UNIT_CQMAX:   unit = "cqmax"; break;
            case NS_CSS_UNIT_EX:      unit = "ex";  break;
            case NS_CSS_UNIT_CH:      unit = "ch";  break;
            case NS_CSS_UNIT_CAP:     unit = "cap"; break;
            case NS_CSS_UNIT_IC:      unit = "ic";  break;
            }
            g_string_append_printf(s, "%g%s", v->u.size.w, unit);
        }
        g_string_append_c(s, ' ');
        if (v->u.size.h_auto) {
            g_string_append(s, "auto");
        } else {
            const char *unit = "";
            switch (v->u.size.h_unit) {
            case NS_CSS_UNIT_PX:      unit = "px"; break;
            case NS_CSS_UNIT_EM:      unit = "em"; break;
            case NS_CSS_UNIT_REM:     unit = "rem"; break;
            case NS_CSS_UNIT_PERCENT: unit = "%";  break;
            case NS_CSS_UNIT_NUMBER:  unit = "";   break;
            case NS_CSS_UNIT_VW:      unit = "vw"; break;
            case NS_CSS_UNIT_VH:      unit = "vh"; break;
            case NS_CSS_UNIT_VMIN:    unit = "vmin"; break;
            case NS_CSS_UNIT_VMAX:    unit = "vmax"; break;
            case NS_CSS_UNIT_CQW:     unit = "cqw"; break;
            case NS_CSS_UNIT_CQH:     unit = "cqh"; break;
            case NS_CSS_UNIT_CQMIN:   unit = "cqmin"; break;
            case NS_CSS_UNIT_CQMAX:   unit = "cqmax"; break;
            case NS_CSS_UNIT_EX:      unit = "ex";  break;
            case NS_CSS_UNIT_CH:      unit = "ch";  break;
            case NS_CSS_UNIT_CAP:     unit = "cap"; break;
            case NS_CSS_UNIT_IC:      unit = "ic";  break;
            }
            g_string_append_printf(s, "%g%s", v->u.size.h, unit);
        }
        return g_string_free(s, FALSE);
    }
    case NS_CSS_V_CALC:
        if (v->u.calc.pct == 0)
            return g_strdup_printf("%gpx", v->u.calc.px);
        if (v->u.calc.px == 0)
            return g_strdup_printf("%g%%", v->u.calc.pct);
        return g_strdup_printf("calc(%gpx + %g%%)", v->u.calc.px, v->u.calc.pct);
    case NS_CSS_V_SHADOW: {
        GString *s = g_string_new(NULL);
        for (int i = 0; i < v->u.shadow.n; i++) {
            const ns_css_shadow *sh = &v->u.shadow.s[i];
            if (i > 0) g_string_append(s, ", ");
            g_string_append_printf(s,
                "%s%gpx %gpx %gpx %gpx rgba(%u,%u,%u,%g)",
                sh->inset ? "inset " : "",
                sh->x, sh->y, sh->blur, sh->spread,
                sh->r, sh->g, sh->b, sh->a / 255.0);
        }
        return g_string_free(s, FALSE);
    }
    case NS_CSS_V_GRADIENT: {
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
            const ns_css_gradient_stop *st = &v->u.gradient.stops[i];
            g_string_append_printf(s, ", rgba(%u,%u,%u,%g) %g%%",
                st->r, st->g, st->b, st->a / 255.0, st->pos * 100.0);
        }
        g_string_append_c(s, ')');
        return g_string_free(s, FALSE);
    }
    case NS_CSS_V_TRACKS: {
        GString *s = g_string_new(NULL);
        if (v->u.tracks.subgrid)
            return g_strdup("subgrid");
        for (int i = 0; i < v->u.tracks.n; i++) {
            if (i) g_string_append_c(s, ' ');
            const ns_css_track *t = &v->u.tracks.tracks[i];
            switch (t->kind) {
            case NS_CSS_TRACK_PX:      g_string_append_printf(s, "%gpx", t->v); break;
            case NS_CSS_TRACK_PERCENT: g_string_append_printf(s, "%g%%", t->v); break;
            case NS_CSS_TRACK_FR:      g_string_append_printf(s, "%gfr", t->v); break;
            case NS_CSS_TRACK_AUTO:    g_string_append(s, "auto"); break;
            }
        }
        return g_string_free(s, FALSE);
    }
    case NS_CSS_V_URL:
        return g_strdup_printf("url(\"%s\")", v->u.url ? v->u.url : "");
    case NS_CSS_V_AREAS: {
        GString *s = g_string_new(NULL);
        for (int r = 0; r < v->u.areas.n_rows; r++) {
            if (r) g_string_append_c(s, ' ');
            g_string_append_c(s, '"');
            for (int c = 0; c < v->u.areas.n_cols; c++) {
                const char *name = ".";
                for (int k = 0; k < v->u.areas.n_rects; k++) {
                    const ns_css_area_rect *rect = &v->u.areas.rects[k];
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
    case NS_CSS_V_ANIM: {
        GString *s = g_string_new(NULL);
        for (int i = 0; i < v->u.anim.n; i++) {
            if (i) g_string_append(s, ", ");
            const ns_css_anim_entry *e = &v->u.anim.entries[i];
            if (e->name) g_string_append_printf(s, "%s ", e->name);
            g_string_append_printf(s, "%gms", e->duration_ms);
            if (e->delay_ms != 0)
                g_string_append_printf(s, " %gms", e->delay_ms);
        }
        return g_string_free(s, FALSE);
    }
    case NS_CSS_V_TRANSFORM: {
        GString *s = g_string_new(NULL);
        for (int i = 0; i < v->u.transform.n_ops; i++) {
            const ns_css_transform_op *op = &v->u.transform.ops[i];
            if (i) g_string_append_c(s, ' ');
            switch (op->kind) {
            case NS_CSS_TFN_TRANSLATE:
                g_string_append_printf(s, "translate(%g%s, %g%s)",
                    op->a, op->a_is_percent ? "%" : "px",
                    op->b, op->b_is_percent ? "%" : "px");
                break;
            case NS_CSS_TFN_ROTATE:
                g_string_append_printf(s, "rotate(%gdeg)", op->a);
                break;
            case NS_CSS_TFN_SCALE:
                g_string_append_printf(s, "scale(%g, %g)", op->a, op->b);
                break;
            case NS_CSS_TFN_SKEW:
                g_string_append_printf(s, "skew(%gdeg, %gdeg)", op->a, op->b);
                break;
            case NS_CSS_TFN_MATRIX:
                g_string_append_printf(s, "matrix(%g, %g, %g, %g, %g, %g)",
                    op->a, op->b, op->c, op->d, op->e, op->f);
                break;
            case NS_CSS_TFN_MATRIX3D:
                g_string_append(s, "matrix3d(");
                for (int k = 0; k < 16; k++)
                    g_string_append_printf(s, "%s%g", k ? ", " : "",
                                           op->m3d[k]);
                g_string_append_c(s, ')');
                break;
            case NS_CSS_TFN_ROTATE3D:
                g_string_append_printf(s, "rotate3d(%g, %g, %g, %gdeg)",
                    op->a, op->b, op->c, op->d);
                break;
            case NS_CSS_TFN_PERSPECTIVE:
                g_string_append_printf(s, "perspective(%gpx)", op->a);
                break;
            }
        }
        return g_string_free(s, FALSE);
    }
    case NS_CSS_V_RECT: {
        GString *s = g_string_new("rect(");
        for (int i = 0; i < 4; i++) {
            if (i) g_string_append(s, ", ");
            if (v->u.rect.is_auto[i])
                g_string_append(s, "auto");
            else
                g_string_append_printf(s, "%gpx", v->u.rect.v[i]);
        }
        g_string_append_c(s, ')');
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
    ns_css_value *value;
    ns_css_prop  prop;
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
    ns_css_pending_decl *pd;
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
    if (!layer_name || !layer_ranks) return NS_CSS_LAYER_NONE;
    gpointer v = g_hash_table_lookup(layer_ranks, layer_name);
    return v ? GPOINTER_TO_INT(v) - 1 : NS_CSS_LAYER_NONE;
}

static void
css_layer_rank_add_sheet(GHashTable *layer_ranks,
                         const ns_css_stylesheet *sheet)
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
    if (a->origin    != b->origin)
        return a->important ? (a->origin > b->origin ? -1 : 1)
                            : (a->origin < b->origin ? -1 : 1);
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

typedef struct css_rule_match_accum {
    guint epoch;
    int layer_order;
    gboolean any[9];
    int spec_a[9];
    int spec_b[9];
    int spec_c[9];
    int scope_order[9];
} css_rule_match_accum;

static __thread css_candidate *g_cand_pool = NULL;
static __thread guint g_cand_pool_cap = 0;
static __thread css_rule_match_accum *g_rule_accum = NULL;
static __thread guint g_rule_accum_cap = 0;
static __thread guint *g_rule_matched = NULL;
static __thread guint g_rule_matched_cap = 0;
static __thread guint g_rule_match_epoch = 0;

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

typedef struct {
    ns_css_pseudo_element pe;
    GArray *out;
    GArray *var_out;
    GArray *pending_out;
} gather_dest;

static void
gather_matches_multi(const ns_css_stylesheet *sheet, int origin,
                     int sheet_index, const ns_node *el,
                     gather_dest *dests, guint n_dests,
                     GHashTable *layer_ranks)
{
    if (!sheet) return;
    const ns_css_rule_index *idx = ns_css_rule_index_ensure(sheet);
    if (!idx) return;

    css_candidate *cands = g_cand_pool;
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
                if (new_cap > G_MAXUINT / sizeof(css_candidate)) break; \
                cands = g_renew(css_candidate, cands, new_cap); \
                cand_cap = new_cap; \
                g_cand_pool = cands; \
                g_cand_pool_cap = cand_cap; \
            } \
            if (_n) memcpy(cands + cand_n, (_arr)->data, _n * sizeof(css_candidate)); \
            cand_n += _n; \
        } \
    } while (0)

    if (el && el->kind == NS_NODE_ELEMENT) {
        const char *id = ns_element_get_attr(el, "id");
        if (id && *id) {
            GArray *bucket = g_hash_table_lookup(idx->by_id, id);
            CAND_PUSH_ARR(bucket);
        }
        const char *cls = ns_element_get_attr(el, "class");
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
            for (const ns_attr *a = el->attrs; a; a = a->next) {
                if (!a->name) continue;
                CAND_PUSH_ARR(css_index_lookup_ci(idx->by_attr, a->name,
                                                  strlen(a->name)));
            }
        }
    }
    CAND_PUSH_ARR(idx->universal);
    #undef CAND_PUSH_ARR

    guint n_rules = sheet->rules ? sheet->rules->len : 0;
    if (g_rule_accum_cap < n_rules) {
        guint new_cap = g_rule_accum_cap < 64 ? 64 : g_rule_accum_cap;
        while (new_cap < n_rules) {
            if (new_cap > G_MAXUINT / 2) { new_cap = n_rules; break; }
            new_cap *= 2;
        }
        g_rule_accum = g_renew(css_rule_match_accum, g_rule_accum, new_cap);
        memset(g_rule_accum + g_rule_accum_cap, 0,
               (gsize)(new_cap - g_rule_accum_cap) * sizeof(css_rule_match_accum));
        g_rule_accum_cap = new_cap;
    }
    if (g_rule_matched_cap < n_rules) {
        guint new_cap = g_rule_matched_cap < 64 ? 64 : g_rule_matched_cap;
        while (new_cap < n_rules) {
            if (new_cap > G_MAXUINT / 2) { new_cap = n_rules; break; }
            new_cap *= 2;
        }
        g_rule_matched = g_renew(guint, g_rule_matched, new_cap);
        g_rule_matched_cap = new_cap;
    }
    if (++g_rule_match_epoch == 0) {
        memset(g_rule_accum, 0,
               (gsize)g_rule_accum_cap * sizeof(css_rule_match_accum));
        g_rule_match_epoch = 1;
    }

    guint matched_n = 0;
    for (guint ci = 0; ci < cand_n; ci++) {
        css_candidate cand = cands[ci];
        guint ri = cand.rule_idx;
        if (ri >= n_rules) continue;
        ns_css_rule *r = g_ptr_array_index(sheet->rules, ri);
        if (!r || cand.selector_idx >= r->selectors->len) continue;
        if (r->container_condition &&
            !container_cond_matches(r->container_condition))
            continue;
        for (guint dd = 0; dd < n_dests; dd++) {
            gather_dest *dst = &dests[dd];
            ns_css_pseudo_element pe = dst->pe;
            if (pe != NS_CSS_PE_NONE && !(r->pe_mask & (1u << pe)))
                continue;
            ns_css_selector *sel = g_ptr_array_index(r->selectors, cand.selector_idx);
            if (sel && sel->pseudo_element != pe) continue;
            int scope_order = 0;
            gboolean matched = rule_selector_matches(r, sel, el, pe,
                                                     &scope_order);
            if (!matched) continue;
            css_rule_match_accum *acc = &g_rule_accum[ri];
            if (acc->epoch != g_rule_match_epoch) {
                acc->epoch = g_rule_match_epoch;
                acc->layer_order = INT_MIN;
                memset(acc->any, 0, sizeof acc->any);
                if (matched_n < g_rule_matched_cap)
                    g_rule_matched[matched_n++] = ri;
            }
            if (!acc->any[dd] || sel->spec_a > acc->spec_a[dd] ||
                (sel->spec_a == acc->spec_a[dd] &&
                 sel->spec_b > acc->spec_b[dd]) ||
                (sel->spec_a == acc->spec_a[dd] &&
                 sel->spec_b == acc->spec_b[dd] &&
                 sel->spec_c > acc->spec_c[dd])) {
                acc->any[dd] = TRUE;
                acc->spec_a[dd] = sel->spec_a;
                acc->spec_b[dd] = sel->spec_b;
                acc->spec_c[dd] = sel->spec_c;
                acc->scope_order[dd] = scope_order;
            } else if (sel->spec_a == acc->spec_a[dd] &&
                       sel->spec_b == acc->spec_b[dd] &&
                       sel->spec_c == acc->spec_c[dd] &&
                       scope_order > acc->scope_order[dd]) {
                acc->scope_order[dd] = scope_order;
            }
        }
    }

    for (guint mi = 0; mi < matched_n; mi++) {
        guint ri = g_rule_matched[mi];
        ns_css_rule *r = g_ptr_array_index(sheet->rules, ri);
        css_rule_match_accum *acc = &g_rule_accum[ri];
        if (acc->layer_order == INT_MIN)
            acc->layer_order = css_layer_rank_for(layer_ranks, r->layer_name);
        for (guint dd = 0; dd < n_dests; dd++) {
            if (!acc->any[dd]) continue;
            gather_dest *dst = &dests[dd];
            for (guint di = 0; di < r->decls->len; di++) {
                ns_css_decl *d = &g_array_index(r->decls, ns_css_decl, di);
                match_entry e = {
                    .origin = origin,
                    .spec_a = acc->spec_a[dd],
                    .spec_b = acc->spec_b[dd],
                    .spec_c = acc->spec_c[dd],
                    .sheet_index = sheet_index,
                    .layer_order = acc->layer_order,
                    .scope_order = acc->scope_order[dd],
                    .source_order = r->source_order,
                    .decl_order = (int)di,
                    .important = d->important,
                    .value = d->value,
                    .prop  = d->prop,
                };
                g_array_append_val(dst->out, e);
            }
            if (dst->var_out && r->vars) {
                GHashTableIter it;
                gpointer k, v;
                int decl_i = 0;
                g_hash_table_iter_init(&it, r->vars);
                while (g_hash_table_iter_next(&it, &k, &v)) {
                    var_match vm = {
                        .origin = origin,
                        .spec_a = acc->spec_a[dd],
                        .spec_b = acc->spec_b[dd],
                        .spec_c = acc->spec_c[dd],
                        .sheet_index = sheet_index,
                        .layer_order = acc->layer_order,
                        .scope_order = acc->scope_order[dd],
                        .source_order = r->source_order,
                        .decl_order = decl_i++,
                        .important = r->var_important &&
                                     g_hash_table_contains(r->var_important, k),
                        .name = (const char *)k,
                        .text = (const char *)v,
                    };
                    g_array_append_val(dst->var_out, vm);
                }
            }
            if (dst->pending_out && r->pending) {
                for (guint pi = 0; pi < r->pending->len; pi++) {
                    ns_css_pending_decl *pd =
                        &g_array_index(r->pending, ns_css_pending_decl, pi);
                    pending_match pm = {
                        .origin = origin,
                        .spec_a = acc->spec_a[dd],
                        .spec_b = acc->spec_b[dd],
                        .spec_c = acc->spec_c[dd],
                        .sheet_index = sheet_index,
                        .layer_order = acc->layer_order,
                        .scope_order = acc->scope_order[dd],
                        .source_order = r->source_order,
                        .decl_order_base = (int)(r->decls->len + pi),
                        .pd = pd,
                    };
                    g_array_append_val(dst->pending_out, pm);
                }
            }
        }
    }
    (void)cands;
}

static int
var_match_cmp(gconstpointer a_, gconstpointer b_)
{
    const var_match *a = a_;
    const var_match *b = b_;
    if (a->important != b->important) return a->important ? 1 : -1;
    if (a->origin    != b->origin)
        return a->important ? (a->origin > b->origin ? -1 : 1)
                            : (a->origin < b->origin ? -1 : 1);
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
    gboolean ai = a->pd && a->pd->important;
    gboolean bi = b->pd && b->pd->important;
    if (ai != bi) return ai ? 1 : -1;
    if (a->origin    != b->origin)
        return ai ? (a->origin > b->origin ? -1 : 1)
                  : (a->origin < b->origin ? -1 : 1);
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
css_collect_property_rules(GHashTable *reg, const ns_css_stylesheet *sh)
{
    if (!reg || !sh || !sh->property_rules) return;
    for (guint i = 0; i < sh->property_rules->len; i++) {
        ns_css_property_rule *pr =
            &g_array_index(sh->property_rules, ns_css_property_rule, i);
        if (pr->name) g_hash_table_replace(reg, pr->name, pr);
    }
}

static GHashTable *
flatten_var_map(const ns_var_map *m)
{
    GHashTable *flat = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *chain = g_ptr_array_new();
    for (const ns_var_map *c = m; c; c = c->parent)
        g_ptr_array_add(chain, (gpointer)c);
    for (guint ci = chain->len; ci-- > 0; ) {
        const ns_var_map *c = g_ptr_array_index(chain, ci);
        if (!c->own) continue;
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, c->own);
        while (g_hash_table_iter_next(&it, &k, &v))
            g_hash_table_replace(flat, k, v);
    }
    g_ptr_array_free(chain, TRUE);
    return flat;
}

static ns_var_map *
build_vars_for_element(const ns_style *parent_style, GArray *var_matches)
{
    ns_var_map *parent = parent_style ? parent_style->vars : NULL;
    gboolean parent_has = parent != NULL;
    gboolean have_regs  = g_registered_props &&
                          g_hash_table_size(g_registered_props) > 0;
    gboolean have_local = var_matches && var_matches->len > 0;
    if (!parent_has && !have_regs && !have_local)
        return NULL;

    if (!have_regs) {
        if (!have_local)
            return ns_var_map_ref(parent);
        GHashTable *own = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_free);
        g_array_sort(var_matches, var_match_cmp);
        for (guint i = 0; i < var_matches->len; i++) {
            var_match *vm = &g_array_index(var_matches, var_match, i);
            if (!vm->name || !vm->text) continue;
            g_hash_table_replace(own, g_strdup(vm->name), g_strdup(vm->text));
        }
        return ns_var_map_new(own, ns_var_map_ref(parent));
    }

    if (parent_has && !have_local) {
        if (g_var_adjust_cache) {
            ns_var_map *hit = g_hash_table_lookup(g_var_adjust_cache, parent);
            if (hit) return ns_var_map_ref(hit);
        }
        gboolean same_as_parent = TRUE;
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_registered_props);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            ns_css_property_rule *pr = v;
            gboolean present = ns_var_map_lookup(parent, k) != NULL;
            if ((!pr->inherits && present) ||
                (pr->has_initial && !present)) {
                same_as_parent = FALSE;
                break;
            }
        }
        if (same_as_parent) {
            if (g_var_adjust_cache)
                g_hash_table_insert(g_var_adjust_cache,
                                    ns_var_map_ref(parent),
                                    ns_var_map_ref(parent));
            return ns_var_map_ref(parent);
        }
    }
    GHashTable *parent_flat = parent_has ? flatten_var_map(parent) : NULL;
    GHashTable *vars = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
    if (parent_flat) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, parent_flat);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            ns_css_property_rule *pr =
                g_hash_table_lookup(g_registered_props, k);
            if (pr && !pr->inherits) continue;
            g_hash_table_replace(vars, g_strdup(k), g_strdup(v));
        }
        g_hash_table_destroy(parent_flat);
    }
    {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_registered_props);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            ns_css_property_rule *pr = v;
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
    ns_var_map *built = ns_var_map_new(vars, NULL);
    if (parent_has && !have_local && g_var_adjust_cache)
        g_hash_table_insert(g_var_adjust_cache, ns_var_map_ref(parent),
                            ns_var_map_ref(built));
    return built;
}

static void
resolve_pending_into_matches(GArray *pending_matches,
                             const ns_var_map *vars,
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
        GArray *temp = g_array_new(FALSE, FALSE, sizeof(ns_css_decl));
        const char *sp = synth;
        const char *se = synth + strlen(synth);
        parse_declaration_block(&sp, se, temp, NULL);
        g_free(synth);
        for (guint i = 0; i < temp->len; i++) {
            ns_css_decl *d = &g_array_index(temp, ns_css_decl, i);
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
    "ul, ol, dl, dt, dd, blockquote, pre, address, "
    "hr, form, fieldset, figure, figcaption, center, "
    "legend, search, hgroup { display: block; }\n"
    "li { display: list-item; }\n"
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
    "bdo { unicode-bidi: bidi-override; }\n"
    "bdi { unicode-bidi: isolate; }\n"
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
    "pre { margin: 0.9em 0; line-height: 1.4; white-space: pre; }\n"
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
    "img { display: inline; }\n"
    "figure { margin: 0.6em 24px; }\n"
    "figcaption { font-style: italic; font-size: 0.9em; text-align: center; }\n"
    "input[type=\"radio\"], input[type=\"checkbox\"], input[type=\"reset\"], "
    "input[type=\"button\"], input[type=\"submit\"], input[type=\"color\"], "
    "input[type=\"search\"], select, button { box-sizing: border-box; }\n"
    "button { display: inline-block; padding: 4px 12px; background-color: #e6e6e6; "
    "border-top-width: 1px; border-right-width: 1px; "
    "border-bottom-width: 1px; border-left-width: 1px; "
    "border-top-style: solid; border-right-style: solid; "
    "border-bottom-style: solid; border-left-style: solid; "
    "border-top-color: #b8b8b8; border-right-color: #b8b8b8; "
    "border-bottom-color: #b8b8b8; border-left-color: #b8b8b8; }\n"
    "button, input, select, textarea { color: #1a1a1a; }\n"
    "input, textarea, select { display: inline-block; }\n"
    "input, textarea, select { padding: 0; background-color: transparent; "
    "border-top-width: 0; border-right-width: 0; "
    "border-bottom-width: 0; border-left-width: 0; }\n"
    "head, script, style, title, meta, link, noscript { display: none; }\n"
    "[data-nd-shadow-root] { display: block; }\n"
    "input[type=\"hidden\"] { display: none; }\n"
    "video { display: inline; }\n"
    "canvas { display: inline; }\n"
    "iframe, frame, frameset, object, embed { display: none !important; }\n"
    "iframe[data-nd-frame-loaded] { display: block !important; overflow: hidden; }\n"
    "audio, source, track, param { display: none; }\n"
    "audio[controls] { display: inline-block; }\n"
    "svg { display: inline; }\n"
    "noframes, frame, frameset, applet, basefont, marquee, "
    "noembed, isindex { display: none; }\n"
    "listing, xmp, plaintext { display: block; font-family: monospace; "
    "white-space: pre; margin: 0.9em 0; line-height: 1.4; }\n"
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
    "[hidden]:not([hidden=\"until-found\" i]) { display: none; }\n"
    "[hidden=\"until-found\" i] { content-visibility: hidden; }\n"
    "[popover]:not([data-nd-popover-open]) { display: none; }\n"
    "template { display: none; }\n";

static double
resolve_font_size_px(const ns_style *s, const ns_style *parent_style)
{
    double parent_px = 16;
    if (parent_style && parent_style->values[NS_CSS_FONT_SIZE] &&
        parent_style->values[NS_CSS_FONT_SIZE]->kind == NS_CSS_V_LENGTH &&
        parent_style->values[NS_CSS_FONT_SIZE]->u.length.unit == NS_CSS_UNIT_PX)
        parent_px = parent_style->values[NS_CSS_FONT_SIZE]->u.length.v;
    ns_css_value *fs = s ? s->values[NS_CSS_FONT_SIZE] : NULL;
    if (fs && fs->kind == NS_CSS_V_CALC)
        return fs->u.calc.px + fs->u.calc.em * parent_px +
               fs->u.calc.rem * parent_px +
               fs->u.calc.pct * parent_px / 100.0;
    if (!fs || fs->kind != NS_CSS_V_LENGTH) return parent_px;
    switch (fs->u.length.unit) {
    case NS_CSS_UNIT_PX:      return fs->u.length.v;
    case NS_CSS_UNIT_NUMBER:  return fs->u.length.v;
    case NS_CSS_UNIT_EM:      return fs->u.length.v * parent_px;
    case NS_CSS_UNIT_REM:     return fs->u.length.v * parent_px;
    case NS_CSS_UNIT_PERCENT: return fs->u.length.v * parent_px / 100.0;
    case NS_CSS_UNIT_EX:
    case NS_CSS_UNIT_CH:
    case NS_CSS_UNIT_CAP:
    case NS_CSS_UNIT_IC: {
        const char *pf =
            parent_style && parent_style->values[NS_CSS_FONT_FAMILY] &&
            parent_style->values[NS_CSS_FONT_FAMILY]->kind == NS_CSS_V_KEYWORD
            ? parent_style->values[NS_CSS_FONT_FAMILY]->u.keyword : NULL;
        int pw = parent_style
            ? ns_css_font_weight_number(parent_style->values[NS_CSS_FONT_WEIGHT], 400)
            : 400;
        gboolean pi = parent_style &&
            (ns_css_keyword_is(parent_style->values[NS_CSS_FONT_STYLE], "italic") ||
             ns_css_keyword_is(parent_style->values[NS_CSS_FONT_STYLE], "oblique"));
        return fs->u.length.v *
               font_relative_unit_px(fs->u.length.unit, parent_px, pf, pw, pi);
    }
    case NS_CSS_UNIT_VW:
    case NS_CSS_UNIT_VH:
    case NS_CSS_UNIT_VMIN:
    case NS_CSS_UNIT_VMAX:
        return viewport_resolve(fs->u.length.v, fs->u.length.unit);
    case NS_CSS_UNIT_CQW: {
        const ns_cq_container *c = cq_select_container(NULL, 0);
        double basis = c && c->width > 0 ? c->width : g_viewport_w;
        return fs->u.length.v * basis / 100.0;
    }
    case NS_CSS_UNIT_CQH: {
        const ns_cq_container *c = cq_select_container(NULL, 0);
        double basis = c && c->type == NS_CQ_TYPE_SIZE && c->height > 0
            ? c->height : g_viewport_h;
        return fs->u.length.v * basis / 100.0;
    }
    case NS_CSS_UNIT_CQMIN:
    case NS_CSS_UNIT_CQMAX: {
        const ns_cq_container *c = cq_select_container(NULL, 0);
        double w = c && c->width > 0 ? c->width : g_viewport_w;
        double h = c && c->type == NS_CQ_TYPE_SIZE && c->height > 0
            ? c->height : g_viewport_h;
        return fs->u.length.v *
               (fs->u.length.unit == NS_CSS_UNIT_CQMIN ? MIN(w, h)
                                                       : MAX(w, h)) / 100.0;
    }
    }
    return parent_px;
}

static ns_css_value *
ns_css_value_cow(ns_style *out, int prop)
{
    ns_css_value *v = out->values[prop];
    if (!v || v->ref == 0) return v;
    ns_css_value *copy = g_new0(ns_css_value, 1);
    *copy = *v;
    copy->ref = 0;
    if (copy->next_layer) copy->next_layer->ref++;
    v->ref--;
    out->values[prop] = copy;
    return copy;
}

static void
resolve_em_units(ns_style *out, const ns_style *parent_style, double root_px)
{
    double my_font_px = resolve_font_size_px(out, parent_style);
    if (root_px <= 0) root_px = my_font_px;
    if (out->values[NS_CSS_FONT_SIZE] &&
        out->values[NS_CSS_FONT_SIZE]->kind == NS_CSS_V_LENGTH &&
        out->values[NS_CSS_FONT_SIZE]->u.length.unit == NS_CSS_UNIT_REM) {
        my_font_px = out->values[NS_CSS_FONT_SIZE]->u.length.v * root_px;
    } else if (out->values[NS_CSS_FONT_SIZE] &&
               out->values[NS_CSS_FONT_SIZE]->kind == NS_CSS_V_CALC &&
               out->values[NS_CSS_FONT_SIZE]->u.calc.rem != 0) {
        const ns_css_value *fsv = out->values[NS_CSS_FONT_SIZE];
        double parent_px = 16;
        if (parent_style && parent_style->values[NS_CSS_FONT_SIZE] &&
            parent_style->values[NS_CSS_FONT_SIZE]->kind == NS_CSS_V_LENGTH &&
            parent_style->values[NS_CSS_FONT_SIZE]->u.length.unit ==
                NS_CSS_UNIT_PX)
            parent_px = parent_style->values[NS_CSS_FONT_SIZE]->u.length.v;
        my_font_px = fsv->u.calc.px + fsv->u.calc.em * parent_px +
                     fsv->u.calc.rem * root_px +
                     fsv->u.calc.pct * parent_px / 100.0;
    }
    if (out->values[NS_CSS_FONT_SIZE] &&
        out->values[NS_CSS_FONT_SIZE]->kind == NS_CSS_V_LENGTH) {
        ns_css_value *fs = ns_css_value_cow(out, NS_CSS_FONT_SIZE);
        fs->u.length.v = my_font_px;
        fs->u.length.unit = NS_CSS_UNIT_PX;
    } else {
        ns_css_value *fs = g_new0(ns_css_value, 1);
        fs->kind = NS_CSS_V_LENGTH;
        fs->u.length.v = my_font_px;
        fs->u.length.unit = NS_CSS_UNIT_PX;
        out->values[NS_CSS_FONT_SIZE] = fs;
    }
    const char *fr_family =
        out->values[NS_CSS_FONT_FAMILY] &&
        out->values[NS_CSS_FONT_FAMILY]->kind == NS_CSS_V_KEYWORD
        ? out->values[NS_CSS_FONT_FAMILY]->u.keyword : NULL;
    int fr_weight = ns_css_font_weight_number(out->values[NS_CSS_FONT_WEIGHT], 400);
    gboolean fr_italic =
        ns_css_keyword_is(out->values[NS_CSS_FONT_STYLE], "italic") ||
        ns_css_keyword_is(out->values[NS_CSS_FONT_STYLE], "oblique");
    for (int i = 0; i < NS_CSS_PROP_COUNT; i++) {
        if (i == NS_CSS_FONT_SIZE) continue;
        ns_css_value *v = out->values[i];
        if (!v) continue;
        if (v->kind == NS_CSS_V_CALC) {
            if (v->u.calc.em != 0 || v->u.calc.rem != 0)
                v = ns_css_value_cow(out, i);
            v->u.calc.px += v->u.calc.em * my_font_px +
                            v->u.calc.rem * root_px;
            v->u.calc.em = 0;
            v->u.calc.rem = 0;
            continue;
        }
        if (v->kind != NS_CSS_V_LENGTH) continue;
        switch (v->u.length.unit) {
        case NS_CSS_UNIT_EM:
            v = ns_css_value_cow(out, i);
            v->u.length.v *= my_font_px;
            v->u.length.unit = NS_CSS_UNIT_PX;
            break;
        case NS_CSS_UNIT_REM:
            v = ns_css_value_cow(out, i);
            v->u.length.v *= root_px;
            v->u.length.unit = NS_CSS_UNIT_PX;
            break;
        case NS_CSS_UNIT_VW:
        case NS_CSS_UNIT_VH:
        case NS_CSS_UNIT_VMIN:
        case NS_CSS_UNIT_VMAX:
            v = ns_css_value_cow(out, i);
            v->u.length.v = viewport_resolve(v->u.length.v, v->u.length.unit);
            v->u.length.unit = NS_CSS_UNIT_PX;
            break;
        case NS_CSS_UNIT_EX:
        case NS_CSS_UNIT_CH:
        case NS_CSS_UNIT_CAP:
        case NS_CSS_UNIT_IC:
            v = ns_css_value_cow(out, i);
            v->u.length.v *= font_relative_unit_px(v->u.length.unit, my_font_px,
                                                   fr_family, fr_weight,
                                                   fr_italic);
            v->u.length.unit = NS_CSS_UNIT_PX;
            break;
        default:
            break;
        }
    }
}

static gboolean
value_is_inherit(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "inherit") == 0;
}

static gboolean
value_is_initial(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "initial") == 0;
}

static gboolean
value_is_unset(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           (strcmp(v->u.keyword, "unset") == 0 ||
            strcmp(v->u.keyword, "revert-layer") == 0);
}

static gboolean
value_is_revert(const ns_css_value *v)
{
    return v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword &&
           strcmp(v->u.keyword, "revert") == 0;
}

static void
cascade_for(GArray *matches, ns_style *out, const ns_style *parent_style,
            double root_px)
{
    g_array_sort(matches, match_cmp);
    for (guint i = 0; i < matches->len; i++) {
        match_entry *m = &g_array_index(matches, match_entry, i);
        if (value_is_revert(m->value)) {
            ns_css_value *fallback = NULL;
            for (gint j = (gint)i - 1; j >= 0; j--) {
                match_entry *prev = &g_array_index(matches, match_entry, (guint)j);
                if (prev->prop != m->prop || prev->important != m->important)
                    continue;
                if (prev->origin >= m->origin) continue;
                if (value_is_revert(prev->value)) continue;
                fallback = prev->value;
                break;
            }
            ns_css_value_free(out->values[m->prop]);
            out->values[m->prop] = ns_css_value_dup(fallback);
            continue;
        }
        ns_css_value_free(out->values[m->prop]);
        out->values[m->prop] = ns_css_value_dup(m->value);
    }
    gboolean explicit_initial[NS_CSS_PROP_COUNT] = {0};
    for (int i = 0; i < NS_CSS_PROP_COUNT; i++) {
        if (value_is_inherit(out->values[i])) {
            ns_css_value_free(out->values[i]);
            out->values[i] = parent_style && parent_style->values[i]
                             ? ns_css_value_dup(parent_style->values[i])
                             : NULL;
        } else if (value_is_initial(out->values[i])) {
            ns_css_value_free(out->values[i]);
            out->values[i] = NULL;
            explicit_initial[i] = TRUE;
        } else if (value_is_unset(out->values[i])) {
            ns_css_value_free(out->values[i]);
            out->values[i] = NULL;
        }
    }
    if (parent_style) {
        for (int i = 0; i < NS_CSS_PROP_COUNT; i++) {
            if (out->values[i]) continue;
            if (explicit_initial[i]) continue;
            if (!prop_inherits((ns_css_prop)i)) continue;
            if (parent_style->values[i])
                out->values[i] = ns_css_value_dup(parent_style->values[i]);
        }
    }
    {
        const ns_css_prop color_props[] = {
            NS_CSS_BACKGROUND_COLOR,
            NS_CSS_BORDER_TOP_COLOR, NS_CSS_BORDER_RIGHT_COLOR,
            NS_CSS_BORDER_BOTTOM_COLOR, NS_CSS_BORDER_LEFT_COLOR,
            NS_CSS_OUTLINE_COLOR,
            NS_CSS_TEXT_DECORATION_COLOR,
            NS_CSS_COLUMN_RULE_COLOR,
            NS_CSS_ACCENT_COLOR,
            NS_CSS_CARET_COLOR,
        };
        for (gsize i = 0; i < G_N_ELEMENTS(color_props); i++) {
            ns_css_value *v = out->values[color_props[i]];
            if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) continue;
            if (strcmp(v->u.keyword, "currentcolor") == 0) {
                ns_css_value_free(out->values[color_props[i]]);
                out->values[color_props[i]] = out->values[NS_CSS_COLOR]
                    ? ns_css_value_dup(out->values[NS_CSS_COLOR])
                    : NULL;
            } else if (strcmp(v->u.keyword, "transparent") == 0) {
                ns_css_value_free(out->values[color_props[i]]);
                ns_css_value *t = g_new0(ns_css_value, 1);
                t->kind = NS_CSS_V_COLOR;
                t->u.color.r = t->u.color.g = t->u.color.b = 0;
                t->u.color.a = 0;
                out->values[color_props[i]] = t;
            }
        }
    }
    {
        ns_css_value *disp = out->values[NS_CSS_DISPLAY];
        if (disp && disp->kind == NS_CSS_V_KEYWORD && disp->u.keyword &&
            strcmp(disp->u.keyword, "none") != 0) {
            const ns_css_value *posv = out->values[NS_CSS_POSITION];
            const ns_css_value *flt = out->values[NS_CSS_FLOAT];
            gboolean out_of_flow =
                posv && posv->kind == NS_CSS_V_KEYWORD && posv->u.keyword &&
                (strcmp(posv->u.keyword, "absolute") == 0 ||
                 strcmp(posv->u.keyword, "fixed") == 0);
            if (!out_of_flow && flt && flt->kind == NS_CSS_V_KEYWORD &&
                flt->u.keyword && strcmp(flt->u.keyword, "none") != 0)
                out_of_flow = TRUE;
            if (out_of_flow) {
                const char *k = disp->u.keyword, *b = NULL;
                if (strcmp(k, "inline") == 0 || strcmp(k, "inline-block") == 0 ||
                    strcmp(k, "run-in") == 0)            b = "block";
                else if (strcmp(k, "inline-table") == 0) b = "table";
                else if (strcmp(k, "inline-flex") == 0)  b = "flex";
                else if (strcmp(k, "inline-grid") == 0)  b = "grid";
                if (b) {
                    ns_css_value *nv = g_new0(ns_css_value, 1);
                    nv->kind = NS_CSS_V_KEYWORD;
                    nv->u.keyword = g_strdup(b);
                    ns_css_value_free(out->values[NS_CSS_DISPLAY]);
                    out->values[NS_CSS_DISPLAY] = nv;
                }
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

static gboolean
is_presentational_attr_name(const char *n)
{
    if (!n || !*n) return FALSE;
    switch (g_ascii_tolower((guchar)n[0])) {
    case 'a': return g_ascii_strcasecmp(n, "align") == 0;
    case 'b': return g_ascii_strcasecmp(n, "bgcolor") == 0 ||
                     g_ascii_strcasecmp(n, "border") == 0;
    case 'c': return g_ascii_strcasecmp(n, "color") == 0 ||
                     g_ascii_strcasecmp(n, "cellspacing") == 0 ||
                     g_ascii_strcasecmp(n, "cellpadding") == 0;
    case 'f': return g_ascii_strcasecmp(n, "face") == 0 ||
                     g_ascii_strcasecmp(n, "frame") == 0;
    case 'h': return g_ascii_strcasecmp(n, "height") == 0 ||
                     g_ascii_strcasecmp(n, "hspace") == 0;
    case 'n': return g_ascii_strcasecmp(n, "nowrap") == 0 ||
                     g_ascii_strcasecmp(n, "noshade") == 0;
    case 'r': return g_ascii_strcasecmp(n, "rules") == 0;
    case 's': return g_ascii_strcasecmp(n, "size") == 0;
    case 't': return g_ascii_strcasecmp(n, "text") == 0 ||
                     g_ascii_strcasecmp(n, "type") == 0;
    case 'v': return g_ascii_strcasecmp(n, "valign") == 0 ||
                     g_ascii_strcasecmp(n, "vspace") == 0;
    case 'w': return g_ascii_strcasecmp(n, "width") == 0 ||
                     g_ascii_strcasecmp(n, "wrap") == 0;
    default:  return FALSE;
    }
}

static char *
presentational_hints_css(const ns_node *el)
{
    if (!el || el->kind != NS_NODE_ELEMENT || !el->name) return NULL;
    gboolean any = (strcmp(el->name, "td") == 0 || strcmp(el->name, "th") == 0);
    for (const ns_attr *a = el->attrs; !any && a; a = a->next)
        if (is_presentational_attr_name(a->name)) any = TRUE;
    if (!any) return NULL;
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
        const char *t = ns_element_get_attr(el, "type");
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
        const char *t = ns_element_get_attr(el, "type");
        const char *lst = NULL;
        if (t) {
            if (g_ascii_strcasecmp(t, "disc") == 0) lst = "disc";
            else if (g_ascii_strcasecmp(t, "circle") == 0) lst = "circle";
            else if (g_ascii_strcasecmp(t, "square") == 0) lst = "square";
        }
        if (lst) g_string_append_printf(out, "list-style-type: %s;", lst);
    }

    const char *bgcolor = ns_element_get_attr(el, "bgcolor");
    if (bgcolor && *bgcolor) {
        guint8 r, g, b, a;
        if (attr_is_color(bgcolor, &r, &g, &b, &a))
            g_string_append_printf(out, "background-color: rgba(%u,%u,%u,%g);",
                                   r, g, b, a / 255.0);
    }
    if (is_body) {
        const char *text = ns_element_get_attr(el, "text");
        if (text && *text) {
            guint8 r, g, b, a;
            if (attr_is_color(text, &r, &g, &b, &a))
                g_string_append_printf(out, "color: rgba(%u,%u,%u,%g);",
                                       r, g, b, a / 255.0);
        }
    }
    if (is_font) {
        const char *color = ns_element_get_attr(el, "color");
        if (color && *color) {
            guint8 r, g, b, a;
            if (attr_is_color(color, &r, &g, &b, &a))
                g_string_append_printf(out, "color: rgba(%u,%u,%u,%g);",
                                       r, g, b, a / 255.0);
        }
        const char *face = ns_element_get_attr(el, "face");
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
        const char *size = ns_element_get_attr(el, "size");
        if (size && *size) {
            int n = ns_parse_int(size, 0, 0, 100);
            if (n >= 1 && n <= 7) {
                static const double map[] = { 0.63, 0.82, 1.0, 1.13, 1.5, 2.0, 3.0 };
                g_string_append_printf(out, "font-size: %.2fem;", map[n - 1]);
            }
        }
    }

    const char *width = ns_element_get_attr(el, "width");
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
    const char *height = ns_element_get_attr(el, "height");
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
        const char *border = ns_element_get_attr(el, "border");
        if (border && *border) {
            int w = ns_parse_int(border, 0, 0, 100);
            if (w > 0) {
                g_string_append_printf(out,
                    "border: %dpx solid #888;", w);
            }
        }
        const char *cellspacing = ns_element_get_attr(el, "cellspacing");
        if (cellspacing) {
            int v = ns_parse_int(cellspacing, 0, 0, 1000);
            g_string_append_printf(out, "border-spacing: %dpx;", v);
        }
        const char *frame = ns_element_get_attr(el, "frame");
        if (frame && *frame) {
            char *lo = g_ascii_strdown(frame, -1);
            if (strcmp(lo, "void") == 0)
                g_string_append(out, "border-style: hidden;");
            else if (strcmp(lo, "above") == 0)
                g_string_append(out, "border-style: hidden;"
                                     "border-top: 1px solid #888;");
            else if (strcmp(lo, "below") == 0)
                g_string_append(out, "border-style: hidden;"
                                     "border-bottom: 1px solid #888;");
            else if (strcmp(lo, "hsides") == 0)
                g_string_append(out, "border-style: hidden;"
                                     "border-top: 1px solid #888;"
                                     "border-bottom: 1px solid #888;");
            else if (strcmp(lo, "vsides") == 0)
                g_string_append(out, "border-style: hidden;"
                                     "border-left: 1px solid #888;"
                                     "border-right: 1px solid #888;");
            else if (strcmp(lo, "lhs") == 0)
                g_string_append(out, "border-style: hidden;"
                                     "border-left: 1px solid #888;");
            else if (strcmp(lo, "rhs") == 0)
                g_string_append(out, "border-style: hidden;"
                                     "border-right: 1px solid #888;");
            else if (strcmp(lo, "box") == 0 || strcmp(lo, "border") == 0)
                g_string_append(out, "border: 1px solid #888;");
            g_free(lo);
        }
        const char *rules = ns_element_get_attr(el, "rules");
        if (rules && *rules)
            g_string_append(out, "border-collapse: collapse;");
    }
    if (is_cell) {
        const ns_node *tbl = el->parent;
        while (tbl && !(tbl->kind == NS_NODE_ELEMENT && tbl->name &&
                        g_ascii_strcasecmp(tbl->name, "table") == 0))
            tbl = tbl->parent;
        if (tbl) {
            const char *cellpadding = ns_element_get_attr(tbl, "cellpadding");
            if (cellpadding && *cellpadding) {
                int v = ns_parse_int(cellpadding, 0, 0, 1000);
                g_string_append_printf(out, "padding: %dpx;", v);
            }
            const char *tborder = ns_element_get_attr(tbl, "border");
            if (tborder && ns_parse_int(tborder, 0, 0, 100) > 0)
                g_string_append(out, "border: 1px solid #a0a0a0;");
            const char *rules = ns_element_get_attr(tbl, "rules");
            if (rules && *rules) {
                char *lo = g_ascii_strdown(rules, -1);
                if (strcmp(lo, "all") == 0 || strcmp(lo, "groups") == 0)
                    g_string_append(out, "border: 1px solid #a0a0a0;");
                else if (strcmp(lo, "cols") == 0)
                    g_string_append(out, "border-style: hidden;"
                                         "border-left: 1px solid #a0a0a0;"
                                         "border-right: 1px solid #a0a0a0;");
                else if (strcmp(lo, "rows") == 0)
                    g_string_append(out, "border-style: hidden;"
                                         "border-top: 1px solid #a0a0a0;"
                                         "border-bottom: 1px solid #a0a0a0;");
                else if (strcmp(lo, "none") == 0)
                    g_string_append(out, "border-style: hidden;");
                g_free(lo);
            }
        }
        if (ns_element_get_attr(el, "nowrap"))
            g_string_append(out, "white-space: nowrap;");
        const char *align = ns_element_get_attr(el, "align");
        if (align && *align) {
            char *lo = g_ascii_strdown(align, -1);
            if (strcmp(lo, "left") == 0 || strcmp(lo, "center") == 0 ||
                strcmp(lo, "right") == 0 || strcmp(lo, "justify") == 0)
                g_string_append_printf(out, "text-align: %s;", lo);
            g_free(lo);
        }
        const char *valign = ns_element_get_attr(el, "valign");
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
        const char *align = ns_element_get_attr(el, "align");
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
        const char *align = ns_element_get_attr(el, "align");
        if (align && *align) {
            char *lo = g_ascii_strdown(align, -1);
            if (strcmp(lo, "left") == 0 || strcmp(lo, "right") == 0)
                g_string_append_printf(out, "float: %s;", lo);
            else if (strcmp(lo, "top") == 0 || strcmp(lo, "bottom") == 0)
                g_string_append_printf(out, "vertical-align: %s;", lo);
            else if (strcmp(lo, "middle") == 0 ||
                     strcmp(lo, "center") == 0 ||
                     strcmp(lo, "absmiddle") == 0)
                g_string_append(out, "vertical-align: middle;");
            g_free(lo);
        }
        const char *hspace = ns_element_get_attr(el, "hspace");
        if (hspace && *hspace) {
            int v = ns_parse_int(hspace, 0, 0, 1000);
            g_string_append_printf(out, "margin-left: %dpx; margin-right: %dpx;", v, v);
        }
        const char *vspace = ns_element_get_attr(el, "vspace");
        if (vspace && *vspace) {
            int v = ns_parse_int(vspace, 0, 0, 1000);
            g_string_append_printf(out, "margin-top: %dpx; margin-bottom: %dpx;", v, v);
        }
        const char *iborder = ns_element_get_attr(el, "border");
        if (iborder && *iborder) {
            int v = ns_parse_int(iborder, 0, 0, 100);
            if (v > 0)
                g_string_append_printf(out, "border: %dpx solid;", v);
        }
    }
    if (is_hr) {
        const char *align = ns_element_get_attr(el, "align");
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
        const char *color = ns_element_get_attr(el, "color");
        if (color && *color) {
            guint8 r, g, b, a;
            if (attr_is_color(color, &r, &g, &b, &a))
                g_string_append_printf(out,
                    "color: rgba(%u,%u,%u,%g);"
                    "background-color: rgba(%u,%u,%u,%g);",
                    r, g, b, a / 255.0, r, g, b, a / 255.0);
        }
        const char *size = ns_element_get_attr(el, "size");
        if (size && *size) {
            int v = ns_parse_int(size, 0, 0, 1000);
            if (v > 0) g_string_append_printf(out, "height: %dpx;", v);
        }
        if (ns_element_get_attr(el, "noshade") && !(color && *color))
            g_string_append(out, "background-color: #808080;");
    }
    if (strcmp(tag, "textarea") == 0) {
        const char *wrap = ns_element_get_attr(el, "wrap");
        if (wrap && g_ascii_strcasecmp(wrap, "off") == 0)
            g_string_append(out, "white-space: pre;");
    }
    (void)is_marq;

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

#define NS_CSS_MAX_CASCADE_DEPTH 512

static GHashTable *g_decl_sheet_cache;

static const ns_css_stylesheet *
ns_css_cached_decl_sheet(const char *decls)
{
    if (!decls || !*decls) return NULL;
    if (!g_decl_sheet_cache)
        g_decl_sheet_cache = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free,
            (GDestroyNotify)ns_css_stylesheet_free);
    ns_css_stylesheet *s = g_hash_table_lookup(g_decl_sheet_cache, decls);
    if (s) return s;
    char *wrapped = g_strconcat("* { ", decls, " }", NULL);
    s = ns_css_stylesheet_parse(wrapped, -1);
    g_free(wrapped);
    if (s) g_hash_table_insert(g_decl_sheet_cache, g_strdup(decls), s);
    return s;
}

static void
cascade_walk(ns_node *node,
             const ns_css_stylesheet *ua,
             const ns_css_stylesheet *const *author, gsize n_author,
             const ns_style *parent_style,
             double *root_px,
             GHashTable *layer_ranks,
             GHashTable *out,
             gboolean under_dirty);

static GHashTable    *g_incr_prev_styles;
static ns_node       *g_incr_prev_doc;
static guint64        g_incr_prev_sig;
static const ns_node *g_incr_prev_focus;
static const ns_node *g_incr_prev_hover;
static const ns_node *g_incr_prev_active;
static GHashTable    *g_incr_dirty;
static gboolean       g_incr_pass_active;
static guint64        g_incr_has_sig;
static gboolean       g_incr_eligible;
static guint          g_incr_reused;
static guint          g_incr_recomputed;
static double         g_incr_zoom = 1.0;

static GHashTable    *g_struct_keys;
static GHashTable    *g_struct_anc_keys;
static GHashTable    *g_sib_keys;
static GHashTable    *g_sib_attrs;
static GHashTable    *g_attr_keys;
static GHashTable    *g_has_cq_keys;
static gboolean       g_has_cq_loose;
static gboolean       g_struct_loose;
static gboolean       g_sib_loose;
static guint64        g_struct_sig;
static gboolean       g_struct_ready;

void
ns_css_set_render_zoom(double zoom)
{
    g_incr_zoom = zoom > 0 ? zoom : 1.0;
}

void
ns_css_mark_restyle_dirty(ns_node *parent)
{
    if (!parent) return;
    if (!g_incr_dirty)
        g_incr_dirty = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_add(g_incr_dirty, parent);
}

static gboolean incr_pc_is_structural(ns_css_pseudo k)
{
    switch (k) {
    case NS_CSS_PC_FIRST_CHILD: case NS_CSS_PC_LAST_CHILD:
    case NS_CSS_PC_ONLY_CHILD:  case NS_CSS_PC_ONLY_OF_TYPE:
    case NS_CSS_PC_FIRST_OF_TYPE: case NS_CSS_PC_LAST_OF_TYPE:
    case NS_CSS_PC_EMPTY:
    case NS_CSS_PC_NTH_CHILD:   case NS_CSS_PC_NTH_LAST_CHILD:
    case NS_CSS_PC_NTH_OF_TYPE: case NS_CSS_PC_NTH_LAST_OF_TYPE:
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean incr_selector_has_structural(const ns_css_selector *sel, int d);

static gboolean
incr_simple_has_structural(const ns_css_simple *c, int d)
{
    if (!c) return FALSE;
    if (d > 6) return TRUE;
    if (c->pseudos)
        for (guint i = 0; i < c->pseudos->len; i++) {
            const ns_css_pseudo_pred *p =
                &g_array_index(c->pseudos, ns_css_pseudo_pred, i);
            if (incr_pc_is_structural(p->kind)) return TRUE;
            if (p->of_group)
                for (guint gi = 0; gi < p->of_group->len; gi++)
                    if (incr_selector_has_structural(
                            g_ptr_array_index(p->of_group, gi), d + 1))
                        return TRUE;
        }
    GPtrArray *gls[3] = { c->matches_any, c->matches_none, c->has_groups };
    for (int g = 0; g < 3; g++) {
        if (!gls[g]) continue;
        for (guint gi = 0; gi < gls[g]->len; gi++) {
            const GPtrArray *grp = g_ptr_array_index(gls[g], gi);
            for (guint si = 0; grp && si < grp->len; si++)
                if (incr_selector_has_structural(
                        g_ptr_array_index(grp, si), d + 1))
                    return TRUE;
        }
    }
    return FALSE;
}

static gboolean
incr_selector_has_structural(const ns_css_selector *sel, int d)
{
    if (!sel || !sel->compounds) return FALSE;
    if (d > 6) return TRUE;
    for (guint i = 0; i < sel->compounds->len; i++)
        if (incr_simple_has_structural(g_ptr_array_index(sel->compounds, i), d))
            return TRUE;
    return FALSE;
}

static gboolean
incr_add_compound_keys(GHashTable *keys, const ns_css_simple *c)
{
    gboolean any = FALSE;
    if (!c) return FALSE;
    if (c->id && *c->id) {
        g_hash_table_add(keys, g_strconcat("#", c->id, NULL));
        any = TRUE;
    }
    if (c->classes)
        for (guint i = 0; i < c->classes->len; i++) {
            const char *cls = g_ptr_array_index(c->classes, i);
            if (cls && *cls) {
                g_hash_table_add(keys, g_strconcat(".", cls, NULL));
                any = TRUE;
            }
        }
    if (c->type && *c->type && strcmp(c->type, "*") != 0) {
        char *t = g_ascii_strdown(c->type, -1);
        g_hash_table_add(keys, g_strconcat("%", t, NULL));
        g_free(t);
        any = TRUE;
    }
    return any;
}

static const char *
incr_state_pseudo_attr(ns_css_pseudo k)
{
    switch (k) {
    case NS_CSS_PC_DISABLED:
    case NS_CSS_PC_ENABLED:       return "disabled";
    case NS_CSS_PC_CHECKED:       return "data-nd-checked";
    case NS_CSS_PC_REQUIRED:
    case NS_CSS_PC_OPTIONAL:      return "required";
    case NS_CSS_PC_READ_ONLY:
    case NS_CSS_PC_READ_WRITE:    return "readonly";
    case NS_CSS_PC_LINK:
    case NS_CSS_PC_VISITED:
    case NS_CSS_PC_ANY_LINK:
    case NS_CSS_PC_HOVER:
    case NS_CSS_PC_ACTIVE:
    case NS_CSS_PC_FOCUS:
    case NS_CSS_PC_FOCUS_WITHIN:
    case NS_CSS_PC_TARGET:
    case NS_CSS_PC_TARGET_WITHIN:
    case NS_CSS_PC_FIRST_CHILD:
    case NS_CSS_PC_LAST_CHILD:
    case NS_CSS_PC_ONLY_CHILD:
    case NS_CSS_PC_ONLY_OF_TYPE:
    case NS_CSS_PC_FIRST_OF_TYPE:
    case NS_CSS_PC_LAST_OF_TYPE:
    case NS_CSS_PC_EMPTY:
    case NS_CSS_PC_NTH_CHILD:
    case NS_CSS_PC_NTH_LAST_CHILD:
    case NS_CSS_PC_NTH_OF_TYPE:
    case NS_CSS_PC_NTH_LAST_OF_TYPE:
    case NS_CSS_PC_ROOT:
    case NS_CSS_PC_SCOPE:
    case NS_CSS_PC_DEFINED:       return "";
    default:                      return NULL;
    }
}

static void
incr_collect_sib_left(const ns_css_simple *c)
{
    if (incr_add_compound_keys(g_sib_keys, c)) return;
    gboolean handled = FALSE;
    if (c->attrs)
        for (guint i = 0; i < c->attrs->len; i++) {
            const ns_css_attr_pred *a =
                &g_array_index(c->attrs, ns_css_attr_pred, i);
            if (a->name && *a->name) {
                char *low = g_ascii_strdown(a->name, -1);
                g_hash_table_add(g_sib_attrs, low);
                handled = TRUE;
            }
        }
    if (c->pseudos)
        for (guint i = 0; i < c->pseudos->len; i++) {
            const ns_css_pseudo_pred *p =
                &g_array_index(c->pseudos, ns_css_pseudo_pred, i);
            const char *attr = incr_state_pseudo_attr(p->kind);
            if (attr == NULL) { g_sib_loose = TRUE; }
            else if (*attr) { g_hash_table_add(g_sib_attrs, g_strdup(attr)); }
            handled = TRUE;
        }
    if (c->matches_any || c->matches_none || c->has_groups)
        g_sib_loose = TRUE;
    (void)handled;
}

static void incr_collect_attr_keys_selector(const ns_css_selector *sel, int depth);

static void
incr_collect_attr_keys_simple(const ns_css_simple *c, int depth)
{
    if (!c || depth > 6) return;
    if (c->attrs)
        for (guint i = 0; i < c->attrs->len; i++) {
            const ns_css_attr_pred *a =
                &g_array_index(c->attrs, ns_css_attr_pred, i);
            if (a->name && *a->name)
                g_hash_table_add(g_attr_keys, g_ascii_strdown(a->name, -1));
        }
    if (c->pseudos)
        for (guint i = 0; i < c->pseudos->len; i++) {
            const ns_css_pseudo_pred *p =
                &g_array_index(c->pseudos, ns_css_pseudo_pred, i);
            const char *attr = incr_state_pseudo_attr(p->kind);
            if (attr && *attr)
                g_hash_table_add(g_attr_keys, g_strdup(attr));
            if (p->kind == NS_CSS_PC_LANG) {
                g_hash_table_add(g_attr_keys, g_strdup("lang"));
                g_hash_table_add(g_attr_keys, g_strdup("xml:lang"));
            } else if (p->kind == NS_CSS_PC_DIR) {
                g_hash_table_add(g_attr_keys, g_strdup("dir"));
            } else if (p->kind == NS_CSS_PC_OPEN) {
                g_hash_table_add(g_attr_keys, g_strdup("open"));
            } else if (p->kind == NS_CSS_PC_POPOVER_OPEN) {
                g_hash_table_add(g_attr_keys, g_strdup("data-nd-popover-open"));
            }
            if (p->of_group)
                for (guint gi = 0; gi < p->of_group->len; gi++)
                    incr_collect_attr_keys_selector(
                        g_ptr_array_index(p->of_group, gi), depth + 1);
        }
    GPtrArray *groups[3] = { c->matches_any, c->matches_none, c->has_groups };
    for (guint i = 0; i < G_N_ELEMENTS(groups); i++)
        if (groups[i])
            for (guint gi = 0; gi < groups[i]->len; gi++) {
                const GPtrArray *group = g_ptr_array_index(groups[i], gi);
                for (guint si = 0; group && si < group->len; si++)
                    incr_collect_attr_keys_selector(
                        g_ptr_array_index(group, si), depth + 1);
            }
}

static void
incr_collect_attr_keys_selector(const ns_css_selector *sel, int depth)
{
    if (!sel || !sel->compounds || depth > 6) return;
    for (guint i = 0; i < sel->compounds->len; i++)
        incr_collect_attr_keys_simple(g_ptr_array_index(sel->compounds, i),
                                      depth);
}

static void
incr_collect_struct_keys(const ns_css_stylesheet *sh)
{
    if (!sh || !sh->rules) return;
    for (guint ri = 0; ri < sh->rules->len; ri++) {
        const ns_css_rule *r = g_ptr_array_index(sh->rules, ri);
        if (!r || !r->selectors) continue;
        for (guint si = 0; si < r->selectors->len; si++) {
            const ns_css_selector *sel = g_ptr_array_index(r->selectors, si);
            if (!sel || !sel->compounds) continue;
            incr_collect_attr_keys_selector(sel, 0);
            guint nc = sel->compounds->len;
            for (guint ci = 0; ci < nc; ci++) {
                const ns_css_simple *c = g_ptr_array_index(sel->compounds, ci);
                ns_css_comb left = NS_CSS_COMB_NONE;
                if (sel->combinators && ci < sel->combinators->len)
                    left = g_array_index(sel->combinators, ns_css_comb, ci);
                ns_css_comb right = NS_CSS_COMB_NONE;
                if (sel->combinators && ci + 1 < sel->combinators->len)
                    right = g_array_index(sel->combinators, ns_css_comb, ci + 1);
                if (right == NS_CSS_COMB_ADJACENT || right == NS_CSS_COMB_SIBLING)
                    incr_collect_sib_left(c);
                gboolean sib_subject = (left == NS_CSS_COMB_ADJACENT ||
                                        left == NS_CSS_COMB_SIBLING);
                if (!incr_simple_has_structural(c, 0) && !sib_subject)
                    continue;
                if (incr_add_compound_keys(g_struct_keys, c))
                    continue;
                gboolean sib_ctx = TRUE, found = FALSE;
                for (int j = (int)ci - 1; j >= 0; j--) {
                    ns_css_comb cb = NS_CSS_COMB_NONE;
                    if (sel->combinators &&
                        (guint)(j + 1) < sel->combinators->len)
                        cb = g_array_index(sel->combinators, ns_css_comb, j + 1);
                    const ns_css_simple *jc =
                        g_ptr_array_index(sel->compounds, j);
                    if (sib_ctx && (cb == NS_CSS_COMB_ADJACENT ||
                                    cb == NS_CSS_COMB_SIBLING)) {
                        if (incr_add_compound_keys(g_struct_keys, jc)) {
                            found = TRUE; break;
                        }
                    } else {
                        sib_ctx = FALSE;
                        if (incr_add_compound_keys(g_struct_anc_keys, jc)) {
                            found = TRUE; break;
                        }
                    }
                }
                if (!found) g_struct_loose = TRUE;
            }
        }
    }
}

static void
incr_ensure_struct_keys(const ns_css_stylesheet *ua,
                        const ns_css_stylesheet *const *author, gsize n,
                        guint64 sig)
{
    if (g_struct_ready && g_struct_sig == sig) return;
    if (g_struct_keys) g_hash_table_remove_all(g_struct_keys);
    else g_struct_keys = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    if (g_struct_anc_keys) g_hash_table_remove_all(g_struct_anc_keys);
    else g_struct_anc_keys = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);
    if (g_sib_keys) g_hash_table_remove_all(g_sib_keys);
    else g_sib_keys = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, NULL);
    if (g_sib_attrs) g_hash_table_remove_all(g_sib_attrs);
    else g_sib_attrs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    if (g_attr_keys) g_hash_table_remove_all(g_attr_keys);
    else g_attr_keys = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    g_struct_loose = FALSE;
    g_sib_loose = FALSE;
    incr_collect_struct_keys(ua);
    for (gsize i = 0; i < n; i++)
        incr_collect_struct_keys(author[i]);
    g_struct_sig = sig;
    g_struct_ready = TRUE;
}

static gboolean
incr_node_matches_keys(const ns_node *n, GHashTable *keyset)
{
    if (!n || n->kind != NS_NODE_ELEMENT || !keyset ||
        g_hash_table_size(keyset) == 0)
        return FALSE;
    GHashTableIter it;
    gpointer k;
    const char *id = ns_element_get_attr(n, "id");
    g_hash_table_iter_init(&it, keyset);
    while (g_hash_table_iter_next(&it, &k, NULL)) {
        const char *key = k;
        if (key[0] == '%') {
            if (n->name && g_ascii_strcasecmp(n->name, key + 1) == 0)
                return TRUE;
        } else if (key[0] == '#') {
            if (id && strcmp(id, key + 1) == 0) return TRUE;
        } else if (key[0] == '.') {
            if (ns_node_has_class(n, key + 1, strlen(key + 1))) return TRUE;
        }
    }
    return FALSE;
}

static gboolean
incr_childlist_needs_flood(const ns_node *parent)
{
    if (g_struct_loose) return TRUE;
    if (incr_node_matches_keys(parent, g_struct_keys)) return TRUE;
    int scanned = 0;
    for (const ns_node *c = parent->first_child; c; c = c->next_sibling) {
        if (++scanned > 64) return TRUE;
        if (c->kind == NS_NODE_ELEMENT &&
            incr_node_matches_keys(c, g_struct_keys))
            return TRUE;
    }
    for (const ns_node *a = parent; a; a = a->parent)
        if (incr_node_matches_keys(a, g_struct_anc_keys))
            return TRUE;
    return FALSE;
}

void
ns_css_mark_childlist_dirty(ns_node *parent, ns_node *added)
{
    if (!parent) return;
    if (!g_struct_ready || incr_childlist_needs_flood(parent))
        ns_css_mark_restyle_dirty(parent);
    else if (added)
        ns_css_mark_restyle_dirty(added);
}

static gboolean
incr_old_class_is_sib(const char *old_value)
{
    if (!old_value || !*old_value || !g_sib_keys ||
        g_hash_table_size(g_sib_keys) == 0)
        return FALSE;
    char **toks = g_strsplit_set(old_value, " \t\r\n\f", -1);
    gboolean hit = FALSE;
    for (int i = 0; toks && toks[i] && !hit; i++) {
        if (!*toks[i]) continue;
        char *key = g_strconcat(".", toks[i], NULL);
        if (g_hash_table_contains(g_sib_keys, key)) hit = TRUE;
        g_free(key);
    }
    g_strfreev(toks);
    return hit;
}

void
ns_css_mark_attr_dirty(ns_node *target, const char *name, const char *old_value)
{
    if (!target) return;
    if (!ns_css_attr_may_affect_style(target, name)) return;
    if (!g_struct_ready) {
        ns_css_mark_restyle_dirty(target->parent ? target->parent : target);
        return;
    }
    gboolean sib = g_sib_loose;
    if (!sib) sib = incr_node_matches_keys(target, g_sib_keys);
    if (!sib && name && g_sib_attrs) {
        char *low = g_ascii_strdown(name, -1);
        sib = g_hash_table_contains(g_sib_attrs, low);
        g_free(low);
    }
    if (!sib && name && g_ascii_strcasecmp(name, "class") == 0)
        sib = incr_old_class_is_sib(old_value);
    if (sib)
        ns_css_mark_restyle_dirty(target->parent ? target->parent : target);
    else
        ns_css_mark_restyle_dirty(target);
}

gboolean
ns_css_attr_may_affect_style(const ns_node *target, const char *name)
{
    (void)target;
    if (!name || !*name || !g_struct_ready || !g_attr_keys) return TRUE;
    if (is_presentational_attr_name(name)) return TRUE;
    char *low = g_ascii_strdown(name, -1);
    gboolean affects = g_hash_table_contains(g_attr_keys, low);
    static const char *const intrinsic[] = {
        "class", "id", "style", "hidden", "lang", "xml:lang", "dir",
        "width", "height", "src", "srcset", "sizes", "href", "type",
        "value", "checked", "selected", "open", "disabled", "readonly",
        "required", "placeholder", "multiple", "size", "rows", "cols",
        "rowspan", "colspan", "span", "start", "reversed", "wrap",
        "contenteditable", "inert", "popover", "popovertarget", "slot",
        "name", "form", "list", "min", "max", "step",
    };
    for (guint i = 0; !affects && i < G_N_ELEMENTS(intrinsic); i++)
        affects = strcmp(low, intrinsic[i]) == 0;
    g_free(low);
    return affects;
}

void
ns_css_restyle_invalidate(void)
{
    g_incr_prev_styles = NULL;
    g_incr_prev_doc = NULL;
}

static gboolean
incr_selector_uses_has(const ns_css_selector *sel)
{
    if (!sel || !sel->compounds) return FALSE;
    for (guint i = 0; i < sel->compounds->len; i++) {
        const ns_css_simple *c = g_ptr_array_index(sel->compounds, i);
        if (c && c->has_groups && c->has_groups->len > 0) return TRUE;
    }
    return FALSE;
}

static void
incr_collect_has_cq_keys(const ns_css_stylesheet *sh)
{
    if (!sh || !sh->rules) return;
    for (guint ri = 0; ri < sh->rules->len; ri++) {
        const ns_css_rule *r = g_ptr_array_index(sh->rules, ri);
        if (!r || !r->selectors) continue;
        gboolean cq = (r->container_condition != NULL);
        for (guint si = 0; si < r->selectors->len; si++) {
            const ns_css_selector *sel = g_ptr_array_index(r->selectors, si);
            if (!sel || !sel->compounds || sel->compounds->len == 0) continue;
            if (!cq && !incr_selector_uses_has(sel)) continue;
            const ns_css_simple *subj =
                g_ptr_array_index(sel->compounds, sel->compounds->len - 1);
            if (!incr_add_compound_keys(g_has_cq_keys, subj))
                g_has_cq_loose = TRUE;
        }
    }
}

static guint64
incr_sheet_sig(const ns_css_stylesheet *ua,
               const ns_css_stylesheet *const *author, gsize n)
{
    guint64 h = 1469598103934665603ULL;
    guint64 vals[2] = { (guint64)(gsize)(gconstpointer)ua, (guint64)n };
    for (int i = 0; i < 2; i++) { h ^= vals[i]; h *= 1099511628211ULL; }
    for (gsize i = 0; i < n; i++) {
        h ^= (guint64)(gsize)(gconstpointer)author[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static GHashTable *g_style_share;
static GByteArray *g_share_scratch;
static guint g_style_share_next_id;

typedef struct {
    guint32  hash;
    guint32  len;
    guint8  *data;
} share_key_t;

static guint
share_key_hash(gconstpointer p)
{
    return ((const share_key_t *)p)->hash;
}

static gboolean
share_key_equal(gconstpointer a, gconstpointer b)
{
    const share_key_t *x = a, *y = b;
    return x->hash == y->hash && x->len == y->len &&
           memcmp(x->data, y->data, x->len) == 0;
}

static void
share_key_free(gpointer p)
{
    share_key_t *k = p;
    g_free(k->data);
    g_free(k);
}

static guint32
share_key_djb2(const guint8 *d, guint32 n)
{
    guint32 h = 5381;
    for (guint32 i = 0; i < n; i++)
        h = ((h << 5) + h) ^ d[i];
    return h;
}

typedef struct {
    ns_css_pseudo_element pe;
    GArray *m;
    GArray *v;
    GArray *p;
} ns_pe_gather;

static ns_var_map *
ns_style_vars_clone(ns_var_map *vars)
{
    return ns_var_map_ref(vars);
}

static ns_style *
ns_style_clone_shared(const ns_style *s)
{
    if (!s) return NULL;
    ns_style *c = ns_style_alloc();
    c->share_id = s->share_id;
    for (int i = 0; i < NS_CSS_PROP_COUNT; i++) {
        c->values[i] = s->values[i];
        if (c->values[i]) c->values[i]->ref++;
    }
    c->before       = ns_style_clone_shared(s->before);
    c->after        = ns_style_clone_shared(s->after);
    c->first_letter = ns_style_clone_shared(s->first_letter);
    c->first_line   = ns_style_clone_shared(s->first_line);
    c->placeholder  = ns_style_clone_shared(s->placeholder);
    c->selection    = ns_style_clone_shared(s->selection);
    c->marker       = ns_style_clone_shared(s->marker);
    c->backdrop     = ns_style_clone_shared(s->backdrop);
    c->vars = ns_style_vars_clone(s->vars);
    return c;
}

static void
share_key_put_matches(GByteArray *b, const GArray *arr)
{
    guint n = arr ? arr->len : 0;
    g_byte_array_append(b, (const guint8 *)&n, sizeof n);
    for (guint i = 0; i < n; i++) {
        const match_entry *e = &g_array_index((GArray *)arr, match_entry, i);
        g_byte_array_append(b, (const guint8 *)&e->origin, sizeof(int) * 9);
        g_byte_array_append(b, (const guint8 *)&e->important, sizeof e->important);
        g_byte_array_append(b, (const guint8 *)&e->value, sizeof e->value);
        g_byte_array_append(b, (const guint8 *)&e->prop, sizeof e->prop);
    }
}

static void
share_key_put_vars(GByteArray *b, const GArray *arr)
{
    guint n = arr ? arr->len : 0;
    g_byte_array_append(b, (const guint8 *)&n, sizeof n);
    for (guint i = 0; i < n; i++) {
        const var_match *e = &g_array_index((GArray *)arr, var_match, i);
        g_byte_array_append(b, (const guint8 *)&e->origin, sizeof(int) * 9);
        g_byte_array_append(b, (const guint8 *)&e->important, sizeof e->important);
        g_byte_array_append(b, (const guint8 *)&e->name, sizeof e->name);
        g_byte_array_append(b, (const guint8 *)&e->text, sizeof e->text);
    }
}

static void
share_key_put_pending(GByteArray *b, const GArray *arr)
{
    guint n = arr ? arr->len : 0;
    g_byte_array_append(b, (const guint8 *)&n, sizeof n);
    for (guint i = 0; i < n; i++) {
        const pending_match *e = &g_array_index((GArray *)arr, pending_match, i);
        g_byte_array_append(b, (const guint8 *)&e->origin, sizeof(int) * 9);
        g_byte_array_append(b, (const guint8 *)&e->pd, sizeof e->pd);
    }
}

static void
style_share_key(GByteArray *b,
                const ns_style *parent_style, double root_px,
                const GArray *matches, const GArray *var_matches,
                const GArray *pending_matches,
                const ns_pe_gather *pe_g, int n_pe)
{
    g_byte_array_set_size(b, 0);
    guint parent_id = parent_style ? parent_style->share_id : 0;
    g_byte_array_append(b, (const guint8 *)&parent_id, sizeof parent_id);
    g_byte_array_append(b, (const guint8 *)&root_px, sizeof root_px);
    guint cq_len = g_cq_stack ? g_cq_stack->len : 0;
    g_byte_array_append(b, (const guint8 *)&cq_len, sizeof cq_len);
    if (cq_len)
        g_byte_array_append(b, (const guint8 *)g_cq_stack->data,
                            cq_len * (guint)sizeof(ns_cq_container));
    share_key_put_matches(b, matches);
    share_key_put_vars(b, var_matches);
    share_key_put_pending(b, pending_matches);
    for (int i = 0; i < n_pe; i++) {
        guint pe = (guint)pe_g[i].pe;
        g_byte_array_append(b, (const guint8 *)&pe, sizeof pe);
        share_key_put_matches(b, pe_g[i].m);
        share_key_put_vars(b, pe_g[i].v);
        share_key_put_pending(b, pe_g[i].p);
    }
}

static void
cascade_walk(ns_node *node,
             const ns_css_stylesheet *ua,
             const ns_css_stylesheet *const *author, gsize n_author,
             const ns_style *parent_style,
             double *root_px,
             GHashTable *layer_ranks,
             GHashTable *out,
             gboolean under_dirty)
{
    static int depth;
    if (depth >= NS_CSS_MAX_CASCADE_DEPTH) return;
    depth++;
    const ns_style *child_parent_style = parent_style;
    gboolean nd_recurse_dirty = under_dirty;
    if (node->kind == NS_NODE_ELEMENT) {
        gboolean nd_node_dirty = under_dirty ||
            (g_incr_dirty && g_hash_table_contains(g_incr_dirty, node)) ||
            (g_incr_pass_active && g_has_cq_keys &&
             incr_node_matches_keys(node, g_has_cq_keys));
        ns_style *nd_prev =
            (g_incr_pass_active && !nd_node_dirty && g_incr_prev_styles)
            ? g_hash_table_lookup(g_incr_prev_styles, node) : NULL;
        ns_style *s;
        if (nd_prev) {
            s = nd_prev;
            s->ref++;
            g_incr_reused++;
        } else {
        s = ns_style_alloc();
        g_incr_recomputed++;
        nd_node_dirty = TRUE;
        static GArray *sc_matches, *sc_var, *sc_pending;
        static GPtrArray *sc_owned;
        static GArray *sc_pe_m[8], *sc_pe_v[8], *sc_pe_p[8];
        if (!sc_matches) {
            sc_matches  = g_array_new(FALSE, FALSE, sizeof(match_entry));
            sc_var      = g_array_new(FALSE, FALSE, sizeof(var_match));
            sc_pending  = g_array_new(FALSE, FALSE, sizeof(pending_match));
            sc_owned    = g_ptr_array_new_with_free_func(
                              (GDestroyNotify)ns_css_value_free);
        }
        GArray *matches = sc_matches;
        GArray *var_matches = sc_var;
        GArray *pending_matches = sc_pending;
        GPtrArray *owned_values = sc_owned;
        g_array_set_size(matches, 0);
        g_array_set_size(var_matches, 0);
        g_array_set_size(pending_matches, 0);
        g_ptr_array_set_size(owned_values, 0);
        guint pe_mask = ua ? ua->pseudo_mask : 0;
        for (gsize i = 0; i < n_author; i++)
            if (author[i]) pe_mask |= author[i]->pseudo_mask;
        ns_pe_gather pe_g[8];
        int n_pe = 0;
        gather_dest dests[9];
        dests[0].pe = NS_CSS_PE_NONE;
        dests[0].out = matches;
        dests[0].var_out = var_matches;
        dests[0].pending_out = pending_matches;
        for (int pi = 0; pe_mask && pi < 8; pi++) {
            ns_css_pseudo_element pe = (pi == 0) ? NS_CSS_PE_BEFORE :
                                       (pi == 1) ? NS_CSS_PE_AFTER :
                                       (pi == 2) ? NS_CSS_PE_FIRST_LETTER :
                                       (pi == 3) ? NS_CSS_PE_FIRST_LINE :
                                       (pi == 4) ? NS_CSS_PE_SELECTION :
                                       (pi == 5) ? NS_CSS_PE_MARKER :
                                       (pi == 6) ? NS_CSS_PE_BACKDROP :
                                                   NS_CSS_PE_PLACEHOLDER;
            if (!(pe_mask & (1u << pe))) continue;
            ns_pe_gather *pg = &pe_g[n_pe];
            pg->pe = pe;
            if (!sc_pe_m[n_pe]) {
                sc_pe_m[n_pe] = g_array_new(FALSE, FALSE, sizeof(match_entry));
                sc_pe_v[n_pe] = g_array_new(FALSE, FALSE, sizeof(var_match));
                sc_pe_p[n_pe] = g_array_new(FALSE, FALSE, sizeof(pending_match));
            }
            pg->m = sc_pe_m[n_pe];
            pg->v = sc_pe_v[n_pe];
            pg->p = sc_pe_p[n_pe];
            g_array_set_size(pg->m, 0);
            g_array_set_size(pg->v, 0);
            g_array_set_size(pg->p, 0);
            dests[n_pe + 1].pe = pe;
            dests[n_pe + 1].out = pg->m;
            dests[n_pe + 1].var_out = pg->v;
            dests[n_pe + 1].pending_out = pg->p;
            n_pe++;
        }
        gather_matches_multi(ua, 0, 0, node, dests, (guint)n_pe + 1,
                             layer_ranks);
        for (gsize i = 0; i < n_author; i++)
            gather_matches_multi(author[i], 1, (int)(i + 1), node, dests,
                                 (guint)n_pe + 1, layer_ranks);

        char *pres_css = presentational_hints_css(node);
        const ns_css_stylesheet *pres_sheet = NULL;
        if (pres_css) {
            pres_sheet = ns_css_cached_decl_sheet(pres_css);
            g_free(pres_css);
        }
        if (pres_sheet) {
            for (guint ri = 0; ri < pres_sheet->rules->len; ri++) {
                ns_css_rule *r = g_ptr_array_index(pres_sheet->rules, ri);
                for (guint di = 0; di < r->decls->len; di++) {
                    ns_css_decl *d = &g_array_index(r->decls, ns_css_decl, di);
                    match_entry e = {
                        .origin = 1,
                        .spec_a = 0, .spec_b = 0, .spec_c = 0,
                        .layer_order = NS_CSS_LAYER_NONE,
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
                            .layer_order = NS_CSS_LAYER_NONE,
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
                        ns_css_pending_decl *pd =
                            &g_array_index(r->pending, ns_css_pending_decl, pi);
                        pending_match pm = {
                            .origin = 1, .spec_a = 0, .spec_b = 0, .spec_c = 0,
                            .sheet_index = 0,
                            .layer_order = NS_CSS_LAYER_NONE,
                            .source_order = INT_MIN,
                            .decl_order_base = (int)(r->decls->len + pi),
                            .pd = pd,
                        };
                        g_array_append_val(pending_matches, pm);
                    }
                }
            }
        }

        const char *inline_css = ns_element_get_attr(node, "style");
        const ns_css_stylesheet *inline_sheet = NULL;
        if (inline_css && *inline_css)
            inline_sheet = ns_css_cached_decl_sheet(inline_css);
        if (inline_sheet) {
            for (guint ri = 0; ri < inline_sheet->rules->len; ri++) {
                ns_css_rule *r = g_ptr_array_index(inline_sheet->rules, ri);
                for (guint di = 0; di < r->decls->len; di++) {
                    ns_css_decl *d = &g_array_index(r->decls, ns_css_decl, di);
                    match_entry e = {
                        .origin = 1,
                        .spec_a = 1000, .spec_b = 0, .spec_c = 0,
                        .layer_order = NS_CSS_LAYER_NONE,
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
                            .layer_order = NS_CSS_LAYER_NONE,
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
                        ns_css_pending_decl *pd =
                            &g_array_index(r->pending, ns_css_pending_decl, pi);
                        pending_match pm = {
                            .origin = 1, .spec_a = 1000, .spec_b = 0, .spec_c = 0,
                            .sheet_index = 0,
                            .layer_order = NS_CSS_LAYER_NONE,
                            .source_order = INT_MAX,
                            .decl_order_base = (int)(r->decls->len + pi),
                            .pd = pd,
                        };
                        g_array_append_val(pending_matches, pm);
                    }
                }
            }
        }

        share_key_t probe;
        gboolean have_key = FALSE;
        const ns_style *shared = NULL;
        if (g_style_share) {
            style_share_key(g_share_scratch, parent_style, *root_px, matches,
                            var_matches, pending_matches, pe_g, n_pe);
            probe.data = g_share_scratch->data;
            probe.len  = g_share_scratch->len;
            probe.hash = share_key_djb2(probe.data, probe.len);
            have_key = TRUE;
            shared = g_hash_table_lookup(g_style_share, &probe);
        }
        if (shared) {
            ns_style_free(s);
            s = ns_style_clone_shared(shared);
            g_array_set_size(matches, 0);
            g_array_set_size(var_matches, 0);
            g_array_set_size(pending_matches, 0);
            g_ptr_array_set_size(owned_values, 0);
        } else {
            s->vars = build_vars_for_element(parent_style, var_matches);
            resolve_pending_into_matches(pending_matches, s->vars,
                                         matches, owned_values);

            cascade_for(matches, s, parent_style, *root_px);
            g_array_set_size(matches, 0);
            g_array_set_size(var_matches, 0);
            g_array_set_size(pending_matches, 0);
            g_ptr_array_set_size(owned_values, 0);

            for (int gi = 0; gi < n_pe; gi++) {
                ns_css_pseudo_element pe = pe_g[gi].pe;
                GArray *pm = pe_g[gi].m;
                GArray *pe_vars = pe_g[gi].v;
                GArray *pe_pending = pe_g[gi].p;
                if (pm->len == 0 && pe_pending->len == 0) continue;
                GPtrArray *pe_owned =
                    g_ptr_array_new_with_free_func(
                        (GDestroyNotify)ns_css_value_free);
                ns_style *ps = ns_style_alloc();
                ps->vars = build_vars_for_element(s, pe_vars);
                resolve_pending_into_matches(pe_pending, ps->vars, pm, pe_owned);
                cascade_for(pm, ps, s, *root_px);
                gboolean keep = TRUE;
                if (pe == NS_CSS_PE_BEFORE || pe == NS_CSS_PE_AFTER)
                    keep = ps->values[NS_CSS_CONTENT] != NULL;
                if (keep) {
                    if (pe == NS_CSS_PE_BEFORE)            s->before       = ps;
                    else if (pe == NS_CSS_PE_AFTER)        s->after        = ps;
                    else if (pe == NS_CSS_PE_FIRST_LETTER) s->first_letter = ps;
                    else if (pe == NS_CSS_PE_FIRST_LINE)   s->first_line   = ps;
                    else if (pe == NS_CSS_PE_SELECTION)    s->selection    = ps;
                    else if (pe == NS_CSS_PE_MARKER)       s->marker       = ps;
                    else if (pe == NS_CSS_PE_BACKDROP)     s->backdrop     = ps;
                    else                                    s->placeholder  = ps;
                } else {
                    ns_style_free(ps);
                }
                g_ptr_array_free(pe_owned, TRUE);
            }
            if (have_key) {
                if (s->share_id == 0)
                    s->share_id = ++g_style_share_next_id;
                share_key_t *k = g_new(share_key_t, 1);
                k->len  = probe.len;
                k->hash = probe.hash;
                k->data = g_memdup2(probe.data, probe.len);
                g_hash_table_insert(g_style_share, k, s);
            }
        }
        }
        g_hash_table_insert(out, node, s);
        child_parent_style = s;
        if (*root_px <= 0 &&
            s->values[NS_CSS_FONT_SIZE] &&
            s->values[NS_CSS_FONT_SIZE]->kind == NS_CSS_V_LENGTH &&
            s->values[NS_CSS_FONT_SIZE]->u.length.unit == NS_CSS_UNIT_PX)
            *root_px = s->values[NS_CSS_FONT_SIZE]->u.length.v;
        nd_recurse_dirty = nd_node_dirty;
    }
    gboolean pushed = FALSE;
    if (g_cq_map && g_cq_stack) {
        ns_cq_container *info = g_hash_table_lookup(g_cq_map, node);
        if (info) {
            g_array_append_val(g_cq_stack, *info);
            pushed = TRUE;
        }
    }
    for (ns_node *c = node->first_child; c; c = c->next_sibling)
        cascade_walk(c, ua, author, n_author, child_parent_style, root_px,
                     layer_ranks, out, nd_recurse_dirty);
    if (pushed) g_array_set_size(g_cq_stack, g_cq_stack->len - 1);
    depth--;
}

static void
append_text_children(const ns_node *n, GString *out, int depth)
{
    if (depth >= 512) return;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == NS_NODE_TEXT && c->text)
            g_string_append(out, c->text);
        else if (c->kind == NS_NODE_ELEMENT)
            append_text_children(c, out, depth + 1);
    }
}

static int g_host_scope_counter;

static char *
style_host_scope_id(ns_node *style_el)
{
    ns_node *root = NULL;
    for (ns_node *a = style_el; a; a = a->parent) {
        if (a->kind == NS_NODE_ELEMENT &&
            ns_element_get_attr(a, NS_SHADOW_ATTR) != NULL) {
            root = a;
            break;
        }
    }
    if (!root || !root->parent) return NULL;
    ns_node *host = root->parent;
    const char *existing = ns_element_get_attr(host, NS_HOST_SCOPE_ATTR);
    if (existing) return g_strdup(existing);
    char buf[32];
    g_snprintf(buf, sizeof buf, "%d", ++g_host_scope_counter);
    ns_element_set_attr(host, NS_HOST_SCOPE_ATTR, buf);
    return g_strdup(buf);
}

static char *
style_iframe_scope_id(ns_node *style_el)
{
    ns_node *root = NULL;
    for (ns_node *a = style_el; a; a = a->parent) {
        if (a->kind == NS_NODE_ELEMENT && a->parent &&
            a->parent->kind == NS_NODE_DOCUMENT && a->parent->parent) {
            root = a;
            break;
        }
    }
    if (!root) return NULL;
    const char *existing = ns_element_get_attr(root, NS_HOST_SCOPE_ATTR);
    if (existing) return g_strdup(existing);
    char buf[32];
    g_snprintf(buf, sizeof buf, "%d", ++g_host_scope_counter);
    ns_element_set_attr(root, NS_HOST_SCOPE_ATTR, buf);
    return g_strdup(buf);
}

static char *
rewrite_host_selectors(const char *css, const char *host_id)
{
    GString *out = g_string_new(NULL);
    char marker[96];
    g_snprintf(marker, sizeof marker, "[" NS_HOST_SCOPE_ATTR "=\"%s\"]", host_id);
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

static gsize
selector_first_compound_len(const char *s)
{
    gsize i = 0;
    int depth = 0;
    while (s[i]) {
        char c = s[i];
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') { if (depth) depth--; }
        else if (!depth && (is_ws(c) || c == '>' || c == '+' || c == '~' ||
                            c == ','))
            break;
        i++;
    }
    return i;
}

static gboolean
selector_first_compound_targets_root(const char *s, gsize clen)
{
    if (clen >= 4 && g_ascii_strncasecmp(s, "html", 4) == 0 &&
        (clen == 4 || !is_ident(s[4])))
        return TRUE;
    if (clen >= 5 && g_ascii_strncasecmp(s, ":root", 5) == 0 &&
        (clen == 5 || !is_ident(s[5])))
        return TRUE;
    return FALSE;
}

static void
scope_one_selector(GString *out, const char *sel, gsize len,
                   const char *marker, const char *host_id)
{
    while (len && is_ws(*sel)) { sel++; len--; }
    while (len && is_ws(sel[len - 1])) len--;
    if (!len) return;
    char *s = g_strndup(sel, len);
    gsize clen = selector_first_compound_len(s);
    if (strstr(s, ":host") || strstr(s, "::slotted")) {
        char *r = rewrite_host_selectors(s, host_id);
        g_string_append(out, r);
        g_free(r);
    } else if (selector_first_compound_targets_root(s, clen)) {
        g_string_append_len(out, s, (gssize)clen);
        g_string_append(out, marker);
        g_string_append(out, s + clen);
    } else {
        g_string_append(out, marker);
        g_string_append_c(out, ' ');
        g_string_append(out, s);
    }
    g_free(s);
}

static void
scope_rule_list(GString *out, const char *p, const char *end,
                const char *marker, const char *host_id, int depth)
{
    if (depth >= NS_CSS_MAX_AT_NESTING) {
        g_string_append_len(out, p, (gssize)(end - p));
        return;
    }
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
                                    marker, host_id, depth + 1);
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
    g_snprintf(marker, sizeof marker, "[" NS_HOST_SCOPE_ATTR "=\"%s\"]", host_id);
    scope_rule_list(out, flat_css, flat_css + strlen(flat_css), marker, host_id, 0);
    return g_string_free(out, FALSE);
}

static char *
style_element_final_css(ns_node *style)
{
    if (!ns_node_is_element_named(style, "style")) return NULL;
    const char *media = ns_element_get_attr(style, "media");
    if (media && *media && !ns_css_media_query_matches(media)) return NULL;
    GString *buf = g_string_new(NULL);
    append_text_children(style, buf, 0);
    if (buf->len == 0) {
        g_string_free(buf, TRUE);
        return NULL;
    }
    char *host_id = style_host_scope_id(style);
    if (!host_id) host_id = style_iframe_scope_id(style);
    if (host_id) {
        char *flat = css_flatten_nesting(buf->str, (gssize)buf->len);
        char *rewritten = scope_shadow_css(flat, host_id);
        g_free(flat);
        g_free(host_id);
        g_string_free(buf, TRUE);
        return rewritten;
    }
    return g_string_free(buf, FALSE);
}

char *
ns_css_style_element_text(ns_node *style)
{
    return style_element_final_css(style);
}

typedef struct {
    char *css;
    ns_css_stylesheet *sheet;
} ns_style_el_cached;

static GHashTable *g_style_el_cache;
static GHashTable *g_merged_style_cache;
static GHashTable *g_link_sheet_cache;

static void
ns_style_el_cached_free(gpointer data)
{
    ns_style_el_cached *e = data;
    if (!e) return;
    g_free(e->css);
    if (e->sheet) {
        e->sheet->cached = FALSE;
        ns_css_stylesheet_free(e->sheet);
    }
    g_free(e);
}

static int g_css_relayout_depth;

void
ns_css_relayout_enter(void)
{
    g_css_relayout_depth++;
}

void
ns_css_relayout_leave(void)
{
    if (g_css_relayout_depth > 0) g_css_relayout_depth--;
}

void
ns_css_style_element_cache_begin(void)
{
    if (g_css_relayout_depth > 1) return;
    if (g_style_el_cache && g_hash_table_size(g_style_el_cache) > 2048)
        g_hash_table_remove_all(g_style_el_cache);
    if (g_merged_style_cache && g_hash_table_size(g_merged_style_cache) > 64)
        g_hash_table_remove_all(g_merged_style_cache);
    if (g_link_sheet_cache && g_hash_table_size(g_link_sheet_cache) > 256)
        g_hash_table_remove_all(g_link_sheet_cache);
}

static void
ns_merged_style_cached_free(gpointer data)
{
    ns_css_stylesheet *sh = data;
    if (!sh) return;
    sh->cached = FALSE;
    ns_css_stylesheet_free(sh);
}

ns_css_stylesheet *
ns_css_merged_styles_cached(const char *css, gssize len)
{
    if (!css || len == 0) return NULL;
    if (len < 0) len = (gssize)strlen(css);
    if (!g_merged_style_cache)
        g_merged_style_cache =
            g_hash_table_new_full(g_str_hash, g_str_equal,
                                  g_free, ns_merged_style_cached_free);
    ns_css_stylesheet *hit = g_hash_table_lookup(g_merged_style_cache, css);
    if (hit) return hit;
    ns_css_stylesheet *sh = ns_css_stylesheet_parse(css, len);
    if (!sh) return NULL;
    sh->cached = TRUE;
    g_hash_table_replace(g_merged_style_cache, g_strndup(css, (gsize)len), sh);
    return sh;
}

ns_css_stylesheet *
ns_css_stylesheet_parse_url_cached(const char *url, const char *css, gssize len)
{
    if (!css) return NULL;
    if (!url || !*url) return ns_css_stylesheet_parse(css, len);
    if (!g_link_sheet_cache)
        g_link_sheet_cache =
            g_hash_table_new_full(g_str_hash, g_str_equal,
                                  g_free, ns_merged_style_cached_free);
    ns_css_stylesheet *hit = g_hash_table_lookup(g_link_sheet_cache, url);
    if (hit) return hit;
    ns_css_stylesheet *sh = ns_css_stylesheet_parse(css, len);
    if (!sh) return NULL;
    sh->cached = TRUE;
    g_hash_table_replace(g_link_sheet_cache, g_strdup(url), sh);
    return sh;
}

ns_css_stylesheet *
ns_css_stylesheet_from_style_element_cached(ns_node *style)
{
    char *css = style_element_final_css(style);
    if (!css) return NULL;
    if (!g_style_el_cache)
        g_style_el_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                 NULL, ns_style_el_cached_free);
    ns_style_el_cached *e = g_hash_table_lookup(g_style_el_cache, style);
    if (e && strcmp(e->css, css) == 0) {
        g_free(css);
        return e->sheet;
    }
    ns_css_stylesheet *sh = ns_css_stylesheet_parse(css, -1);
    if (!sh) {
        g_free(css);
        return NULL;
    }
    sh->cached = TRUE;
    ns_style_el_cached *ne = g_new0(ns_style_el_cached, 1);
    ne->css = css;
    ne->sheet = sh;
    g_hash_table_replace(g_style_el_cache, style, ne);
    return sh;
}

GHashTable *
ns_css_compute(ns_node *doc,
               const ns_css_stylesheet *const *author_sheets,
               gsize n_sheets)
{
    GHashTable *out = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, (GDestroyNotify)ns_style_free);

    g_pragma_valid = FALSE;

    static ns_css_stylesheet *cached_ua = NULL;
    if (!cached_ua) cached_ua = ns_css_stylesheet_parse(kUa, -1);

    gboolean profile = g_getenv("NS_PROFILE") != NULL;
    gint64 t0 = profile ? g_get_monotonic_time() : 0;
    (void)ns_css_rule_index_ensure(cached_ua);
    for (gsize i = 0; i < n_sheets; i++)
        (void)ns_css_rule_index_ensure(author_sheets[i]);
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
    if (g_decl_sheet_cache && g_hash_table_size(g_decl_sheet_cache) >= 8192)
        g_hash_table_remove_all(g_decl_sheet_cache);
    if (!g_cq_stack)
        g_cq_stack = g_array_new(FALSE, FALSE, sizeof(ns_cq_container));
    g_array_set_size(g_cq_stack, 0);
    if (!g_share_scratch)
        g_share_scratch = g_byte_array_sized_new(512);
    g_style_share = g_hash_table_new_full(share_key_hash, share_key_equal,
                                          share_key_free, NULL);
    g_style_share_next_id = 0;
    g_var_adjust_cache = g_hash_table_new_full(
        g_direct_hash, g_direct_equal,
        (GDestroyNotify)ns_var_map_unref, (GDestroyNotify)ns_var_map_unref);
    g_has_memo = g_hash_table_new_full(has_memo_hash, has_memo_equal,
                                       g_free, NULL);

    guint64 sig = incr_sheet_sig(cached_ua, author_sheets, n_sheets);
    if (sig != g_incr_has_sig) {
        if (g_has_cq_keys) g_hash_table_remove_all(g_has_cq_keys);
        else g_has_cq_keys = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);
        g_has_cq_loose = FALSE;
        incr_collect_has_cq_keys(cached_ua);
        for (gsize i = 0; i < n_sheets; i++)
            incr_collect_has_cq_keys(author_sheets[i]);
        g_incr_eligible = !g_has_cq_loose;
        g_incr_has_sig = sig;
    }
    gboolean incr_want = g_getenv("NS_NO_INCR_RESTYLE") == NULL
        && g_incr_eligible && g_cq_map == NULL
        && fabs(g_incr_zoom - 1.0) <= 0.001;
    g_incr_pass_active = incr_want
        && g_incr_prev_styles != NULL
        && g_incr_prev_doc == doc
        && g_incr_prev_sig == sig
        && g_css_focus_node == g_incr_prev_focus
        && g_css_hover_node == g_incr_prev_hover
        && g_css_active_node == g_incr_prev_active;
    g_incr_reused = 0;
    g_incr_recomputed = 0;

    incr_ensure_struct_keys(cached_ua, author_sheets, n_sheets, sig);

    cascade_walk(doc, cached_ua, author_sheets, n_sheets, NULL, &root_px,
                 layer_ranks, out, FALSE);

    if (incr_want) {
        GHashTable *new_prev = g_hash_table_new_full(
            g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)ns_style_free);
        GHashTableIter pit; gpointer pk, pv;
        g_hash_table_iter_init(&pit, out);
        while (g_hash_table_iter_next(&pit, &pk, &pv)) {
            ((ns_style *)pv)->ref++;
            g_hash_table_insert(new_prev, pk, pv);
        }
        if (g_incr_prev_styles) g_hash_table_destroy(g_incr_prev_styles);
        g_incr_prev_styles = new_prev;
        g_incr_prev_doc = doc;
        g_incr_prev_sig = sig;
        g_incr_prev_focus = g_css_focus_node;
        g_incr_prev_hover = g_css_hover_node;
        g_incr_prev_active = g_css_active_node;
        if (g_getenv("NS_PROFILE"))
            g_printerr("[incr] active=%d reused=%u recomputed=%u\n",
                       g_incr_pass_active, g_incr_reused, g_incr_recomputed);
    } else if (g_incr_prev_styles) {
        g_hash_table_destroy(g_incr_prev_styles);
        g_incr_prev_styles = NULL;
        g_incr_prev_doc = NULL;
    }
    if (g_incr_dirty) g_hash_table_remove_all(g_incr_dirty);

    g_hash_table_destroy(g_has_memo);
    g_has_memo = NULL;
    g_hash_table_destroy(g_style_share);
    g_style_share = NULL;
    g_hash_table_destroy(g_var_adjust_cache);
    g_var_adjust_cache = NULL;
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
