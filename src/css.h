/* Nordstjernen — CSS engine API. */

#ifndef ND_CSS_H
#define ND_CSS_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

typedef enum nd_css_prop {
    ND_CSS_DISPLAY,
    ND_CSS_COLOR,
    ND_CSS_BACKGROUND_COLOR,
    ND_CSS_FONT_SIZE,
    ND_CSS_FONT_WEIGHT,
    ND_CSS_FONT_STYLE,
    ND_CSS_FONT_FAMILY,
    ND_CSS_TEXT_ALIGN,
    ND_CSS_MARGIN_TOP,
    ND_CSS_MARGIN_RIGHT,
    ND_CSS_MARGIN_BOTTOM,
    ND_CSS_MARGIN_LEFT,
    ND_CSS_PADDING_TOP,
    ND_CSS_PADDING_RIGHT,
    ND_CSS_PADDING_BOTTOM,
    ND_CSS_PADDING_LEFT,
    ND_CSS_BORDER_TOP_WIDTH,
    ND_CSS_BORDER_RIGHT_WIDTH,
    ND_CSS_BORDER_BOTTOM_WIDTH,
    ND_CSS_BORDER_LEFT_WIDTH,
    ND_CSS_BORDER_TOP_COLOR,
    ND_CSS_BORDER_RIGHT_COLOR,
    ND_CSS_BORDER_BOTTOM_COLOR,
    ND_CSS_BORDER_LEFT_COLOR,
    ND_CSS_BORDER_TOP_STYLE,
    ND_CSS_BORDER_RIGHT_STYLE,
    ND_CSS_BORDER_BOTTOM_STYLE,
    ND_CSS_BORDER_LEFT_STYLE,
    ND_CSS_WIDTH,
    ND_CSS_HEIGHT,
    ND_CSS_MAX_WIDTH,
    ND_CSS_MAX_HEIGHT,
    ND_CSS_MIN_WIDTH,
    ND_CSS_MIN_HEIGHT,
    ND_CSS_LINE_HEIGHT,
    ND_CSS_TEXT_DECORATION,
    ND_CSS_POSITION,
    ND_CSS_TOP,
    ND_CSS_RIGHT,
    ND_CSS_BOTTOM,
    ND_CSS_LEFT,
    ND_CSS_Z_INDEX,
    ND_CSS_OPACITY,
    ND_CSS_CURSOR,
    ND_CSS_LETTER_SPACING,
    ND_CSS_WORD_SPACING,
    ND_CSS_WHITE_SPACE,
    ND_CSS_BOX_SIZING,
    ND_CSS_TEXT_INDENT,
    ND_CSS_PROP_COUNT,
} nd_css_prop;

const char *nd_css_prop_name(nd_css_prop p);

typedef enum nd_css_value_kind {
    ND_CSS_V_KEYWORD,
    ND_CSS_V_LENGTH,
    ND_CSS_V_COLOR,
    ND_CSS_V_CALC,
} nd_css_value_kind;

typedef enum nd_css_unit {
    ND_CSS_UNIT_PX,
    ND_CSS_UNIT_EM,
    ND_CSS_UNIT_PERCENT,
    ND_CSS_UNIT_NUMBER,
} nd_css_unit;

typedef struct nd_css_value {
    nd_css_value_kind kind;
    union {
        char *keyword;
        struct { double v; nd_css_unit unit; } length;
        struct { guint8 r, g, b, a; } color;
        struct { double pct; double px; } calc;
    } u;
} nd_css_value;

nd_css_value *nd_css_value_dup(const nd_css_value *v);
void          nd_css_value_free(nd_css_value *v);

typedef enum nd_css_attr_op {
    ND_CSS_ATTR_PRESENT,
    ND_CSS_ATTR_EQ,
    ND_CSS_ATTR_PREFIX,
    ND_CSS_ATTR_SUFFIX,
    ND_CSS_ATTR_SUBSTR,
    ND_CSS_ATTR_WORD,
    ND_CSS_ATTR_HYPHEN,
} nd_css_attr_op;

typedef struct nd_css_attr_pred {
    char *name;
    nd_css_attr_op op;
    char *value;
} nd_css_attr_pred;

typedef struct nd_css_simple {
    char *type;
    char *id;
    GPtrArray *classes;
    GArray    *attrs;
} nd_css_simple;

typedef enum nd_css_comb {
    ND_CSS_COMB_NONE,
    ND_CSS_COMB_DESCENDANT,
    ND_CSS_COMB_CHILD,
} nd_css_comb;

typedef struct nd_css_selector {

    GPtrArray *compounds;
    GArray    *combinators;

    int spec_a, spec_b, spec_c;
} nd_css_selector;

void nd_css_selector_free(nd_css_selector *sel);

GPtrArray *nd_css_parse_selector_list(const char *text);
gboolean   nd_css_selector_matches(const nd_css_selector *sel, const nd_node *el);

char *nd_inline_style_get(const char *style_text, const char *prop_name);
char *nd_inline_style_set(const char *style_text, const char *prop_name, const char *value);

typedef struct nd_css_decl {
    nd_css_prop prop;
    nd_css_value *value;
    gboolean important;
} nd_css_decl;

typedef struct nd_css_rule {
    GPtrArray *selectors;
    GArray    *decls;
    int        source_order;
} nd_css_rule;

typedef struct nd_css_stylesheet {
    GPtrArray *rules;
} nd_css_stylesheet;

nd_css_stylesheet *nd_css_stylesheet_parse(const char *text, gssize len);
void               nd_css_stylesheet_free(nd_css_stylesheet *s);

typedef struct nd_style {
    nd_css_value *values[ND_CSS_PROP_COUNT];
} nd_style;

void nd_style_free(nd_style *s);

GHashTable *nd_css_compute(nd_node                 *doc,
                           const nd_css_stylesheet *const *author_sheets,
                           gsize                     n_sheets);

const char *nd_style_keyword(const nd_style *s, nd_css_prop p);

G_END_DECLS

#endif
