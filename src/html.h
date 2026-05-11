/* Nordstjernen — HTML parser API. */

#ifndef ND_HTML_H
#define ND_HTML_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

nd_node *nd_html_parse(const char *input, gssize len);

G_END_DECLS

#endif
