/*
 * Nordstjernen — html.h
 *
 * Pragmatic HTML5 parser. Not spec-complete. Handles the cases that
 * appear in well-formed real-world HTML: elements, attributes, text,
 * comments, doctype, void elements, rawtext for <script>/<style>,
 * common entity references, self-closing markup in foreign content.
 *
 * Returns a document node owning the entire parsed tree.
 */

#ifndef ND_HTML_H
#define ND_HTML_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

/* Parse UTF-8 HTML. `len < 0` means strlen(input). */
nd_node *nd_html_parse(const char *input, gssize len);

G_END_DECLS

#endif /* ND_HTML_H */
