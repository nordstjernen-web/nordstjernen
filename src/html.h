/* Nordstjernen — HTML parser API (lexbor default when available, gumbo fallback). */

#ifndef ND_HTML_H
#define ND_HTML_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

typedef enum nd_html_engine {
    ND_HTML_ENGINE_GUMBO = 0,
    ND_HTML_ENGINE_LEXBOR,
} nd_html_engine;

nd_html_engine nd_html_engine_default(void);

void nd_html_engine_set_default(nd_html_engine engine);

gboolean nd_html_engine_lexbor_available(void);

const char *nd_html_engine_name(nd_html_engine engine);

nd_node *nd_html_parse(const char *input, gssize len);

nd_node *nd_html_parse_with(nd_html_engine engine,
                            const char *input, gssize len);

nd_node *nd_html_parse_for_url(const char *url,
                               const char *input, gssize len);

nd_node *nd_html_parse_for_page(const char *input, gssize len);

nd_node *nd_html_parse_fragment(const char *input, gssize len);

nd_node *nd_html_parse_fragment_in(const char *context_tag,
                                   const char *input, gssize len);

nd_node *nd_html_parse_fragment_with(nd_html_engine engine,
                                     const char *context_tag,
                                     const char *input, gssize len);

gboolean nd_html_is_void(const char *tag);

char *nd_html_decode_body(const char *body, gsize len, const char *content_type);

nd_node *nd_html_parse_gumbo(const char *input, gssize len);

nd_node *nd_html_parse_fragment_gumbo(const char *context_tag,
                                      const char *input, gssize len);

#ifdef ND_HAVE_LEXBOR
nd_node *nd_html_parse_lexbor(const char *input, gssize len);

nd_node *nd_html_parse_fragment_lexbor(const char *context_tag,
                                       const char *input, gssize len);
#endif

G_END_DECLS

#endif
