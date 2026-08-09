/* Northstar — CSS Syntax token and component-value API. */

#ifndef NS_CSS_SYNTAX_H
#define NS_CSS_SYNTAX_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum ns_css_component_type {
    NS_CSS_COMPONENT_WHITESPACE,
    NS_CSS_COMPONENT_IDENT,
    NS_CSS_COMPONENT_FUNCTION,
    NS_CSS_COMPONENT_STRING,
    NS_CSS_COMPONENT_NUMBER,
    NS_CSS_COMPONENT_PERCENTAGE,
    NS_CSS_COMPONENT_DIMENSION,
    NS_CSS_COMPONENT_HASH,
    NS_CSS_COMPONENT_AT_KEYWORD,
    NS_CSS_COMPONENT_COLON,
    NS_CSS_COMPONENT_SEMICOLON,
    NS_CSS_COMPONENT_COMMA,
    NS_CSS_COMPONENT_DELIM,
    NS_CSS_COMPONENT_BLOCK,
} ns_css_component_type;

typedef struct ns_css_component {
    ns_css_component_type type;
    gsize start;
    gsize end;
    char *value;
    double number;
    GPtrArray *children;
    char delimiter;
} ns_css_component;

GPtrArray *ns_css_component_values_parse(const char *input, gssize len,
                                         gboolean *valid);
void ns_css_component_free(ns_css_component *component);
const char *ns_css_syntax_scan(const char *input, const char *end,
                               const char *terminators, char *terminator);
gboolean ns_css_component_value_valid(const char *input);

G_END_DECLS

#endif
