/* Nordstjernen — HTML parser API. */

#ifndef ND_HTML_H
#define ND_HTML_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

nd_node *nd_html_parse(const char *input, gssize len);

nd_node *nd_html_parse_for_page(const char *input, gssize len);

nd_node *nd_html_parse_gumbo(const char *input, gssize len);
gboolean nd_html_gumbo_available(void);

G_END_DECLS

#endif
