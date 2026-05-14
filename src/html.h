/* Nordstjernen — HTML parser API (lexbor). */

#ifndef ND_HTML_H
#define ND_HTML_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

nd_node *nd_html_parse(const char *input, gssize len);

nd_node *nd_html_parse_for_url(const char *url,
                               const char *input, gssize len);

nd_node *nd_html_parse_for_page(const char *input, gssize len);

nd_node *nd_html_parse_fragment(const char *input, gssize len);

nd_node *nd_html_parse_fragment_in(const char *context_tag,
                                   const char *input, gssize len);

gboolean nd_html_is_void(const char *tag);

char *nd_html_decode_body(const char *body, gsize len);

nd_node *nd_html_parse_lexbor(const char *input, gssize len);

nd_node *nd_html_parse_fragment_lexbor(const char *context_tag,
                                       const char *input, gssize len);

const char *nd_html_engine_name(void);

const char *nd_html_engine_version(void);

G_END_DECLS

#endif
