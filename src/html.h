/* Nordstjernen — HTML parser API (lexbor).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_HTML_H
#define ND_HTML_H

#include <glib.h>

#include "dom.h"

G_BEGIN_DECLS

nd_node *nd_html_parse(const char *input, gssize len);

nd_node *nd_html_parse_fragment_in(const char *context_tag,
                                   const char *input, gssize len);

gboolean nd_html_is_void(const char *tag);

void nd_html_escape_append(GString *out, const char *s, gboolean escape_quotes);

char *nd_html_escape_text(const char *s);

char *nd_html_decode_body(const char *body, gsize len);

const char *nd_html_engine_name(void);

const char *nd_html_engine_version(void);

G_END_DECLS

#endif
