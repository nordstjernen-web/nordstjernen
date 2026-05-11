/*
 * Nordstjernen — css.h
 *
 * A small CSS engine: stylesheet parser, selector matcher, cascade,
 * inheritance. Covers a deliberate subset of CSS properties chosen so
 * the layout engine can do its job without drowning in feature work.
 * Anything not listed here is parsed and silently ignored.
 *
 * The cascade pipeline:
 *
 *     nd_css_stylesheet_parse(text)  ->  nd_css_stylesheet*
 *     nd_css_compute(doc, sheets, ...) attaches an nd_style* to every
 *         element node by way of a GHashTable<nd_node*, nd_style*>.
 *
 * Memory: the returned stylesheet and style table are owned by the
 * caller, who must call the matching _free routines.
 */

#ifndef ND_CSS_H
#define ND_CSS_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

/* ---------- properties we model ---------- */

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
    ND_CSS_PROP_COUNT,
} nd_css_prop;

const char *nd_css_prop_name(nd_css_prop p);

/* ---------- values ---------- */

typedef enum nd_css_value_kind {
    ND_CSS_V_KEYWORD,  /* identifiers like block/inline/auto/none */
    ND_CSS_V_LENGTH,   /* px (only; em/% to come) */
    ND_CSS_V_COLOR,    /* rgba */
} nd_css_value_kind;

typedef struct nd_css_value {
    nd_css_value_kind kind;
    union {
        char *keyword;
        struct { double v; } length;  /* always in px for now */
        struct { guint8 r, g, b, a; } color;
    } u;
} nd_css_value;

nd_css_value *nd_css_value_dup(const nd_css_value *v);
void          nd_css_value_free(nd_css_value *v);

/* ---------- selectors ---------- */

typedef struct nd_css_simple {
    char *type;     /* lowercase tag name; "*" or NULL for universal */
    char *id;       /* optional */
    GPtrArray *classes; /* of char* */
} nd_css_simple;

typedef enum nd_css_comb {
    ND_CSS_COMB_NONE,        /* leftmost simple selector */
    ND_CSS_COMB_DESCENDANT,  /* space */
    ND_CSS_COMB_CHILD,       /* > */
} nd_css_comb;

typedef struct nd_css_selector {
    /* A selector is a list of compounds linked by combinators, stored
     * leftmost first. e.g. "div > p .em" yields three compounds with
     * combinators NONE, CHILD, DESCENDANT. */
    GPtrArray *compounds;   /* of nd_css_simple* */
    GArray    *combinators; /* of nd_css_comb (one per compound) */
    /* Specificity (a, b, c) packed by the parser. */
    int spec_a, spec_b, spec_c;
} nd_css_selector;

void nd_css_selector_free(nd_css_selector *sel);

/* ---------- stylesheet ---------- */

typedef struct nd_css_decl {
    nd_css_prop prop;
    nd_css_value *value;
    gboolean important;
} nd_css_decl;

typedef struct nd_css_rule {
    GPtrArray *selectors; /* of nd_css_selector* */
    GArray    *decls;     /* of nd_css_decl */
    int        source_order;
} nd_css_rule;

typedef struct nd_css_stylesheet {
    GPtrArray *rules; /* of nd_css_rule* */
} nd_css_stylesheet;

nd_css_stylesheet *nd_css_stylesheet_parse(const char *text, gssize len);
void               nd_css_stylesheet_free(nd_css_stylesheet *s);

/* ---------- computed style ---------- */

typedef struct nd_style {
    nd_css_value *values[ND_CSS_PROP_COUNT];
} nd_style;

void nd_style_free(nd_style *s);

/* Run the cascade. Returns a GHashTable mapping every element nd_node*
 * in the tree to an nd_style*. Caller frees the table; the table's
 * value_destroy is set to nd_style_free.
 *
 * The user-agent default stylesheet is applied first (built into css.c),
 * then any supplied author sheets in order. */
GHashTable *nd_css_compute(nd_node                 *doc,
                           const nd_css_stylesheet *const *author_sheets,
                           gsize                     n_sheets);

/* Convenience: return the keyword value of a property, or NULL. */
const char *nd_style_keyword(const nd_style *s, nd_css_prop p);

G_END_DECLS

#endif /* ND_CSS_H */
