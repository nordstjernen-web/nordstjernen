/* Northstar — registered custom property <syntax> grammar and matching. */

#ifndef NS_CSS_PROP_SYNTAX_H
#define NS_CSS_PROP_SYNTAX_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ns_css_syntax_def ns_css_syntax_def;

ns_css_syntax_def *ns_css_syntax_def_parse(const char *text);
void ns_css_syntax_def_free(ns_css_syntax_def *syntax);
gboolean ns_css_syntax_def_universal(const ns_css_syntax_def *syntax);

gboolean ns_css_syntax_def_matches(const ns_css_syntax_def *syntax,
                                    const char *value);
gboolean ns_css_syntax_def_initial_valid(const ns_css_syntax_def *syntax,
                                          const char *value);

G_END_DECLS

#endif
