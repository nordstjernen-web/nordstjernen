/* Northstar — registered custom property <syntax> grammar and matching. */

#ifndef NS_CSS_PROP_SYNTAX_H
#define NS_CSS_PROP_SYNTAX_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct ns_css_syntax_def ns_css_syntax_def;

typedef struct ns_css_syntax_ctx {
    double font_size;
    double root_font_size;
    double line_height;
    double root_line_height;
    double ex_px;
    double ch_px;
    double cap_px;
    double ic_px;
    double root_ex_px;
    double root_ch_px;
    double root_cap_px;
    double root_ic_px;
    double viewport_w;
    double viewport_h;
    double container_w;
    double container_h;
    const char *current_color;
} ns_css_syntax_ctx;

ns_css_syntax_def *ns_css_syntax_def_parse(const char *text);
void ns_css_syntax_def_free(ns_css_syntax_def *syntax);
gboolean ns_css_syntax_def_universal(const ns_css_syntax_def *syntax);

gboolean ns_css_syntax_def_matches(const ns_css_syntax_def *syntax,
                                    const char *value);
gboolean ns_css_syntax_def_initial_valid(const ns_css_syntax_def *syntax,
                                          const char *value);
char *ns_css_syntax_def_compute(const ns_css_syntax_def *syntax,
                                const char *value,
                                const ns_css_syntax_ctx *ctx);

G_END_DECLS

#endif
