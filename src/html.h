/* Nordstjernen — HTML parser API (gumbo-parser backed). */

#ifndef ND_HTML_H
#define ND_HTML_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

nd_node *nd_html_parse(const char *input, gssize len);

nd_node *nd_html_parse_for_page(const char *input, gssize len);

gboolean nd_html_is_void(const char *tag);

char *nd_html_decode_body(const char *body, gsize len, const char *content_type);

G_END_DECLS

#endif
