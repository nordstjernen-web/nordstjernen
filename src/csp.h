/* Nordstjernen — Content-Security-Policy parser + check.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef ND_CSP_H
#define ND_CSP_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum nd_csp_kind {
    ND_CSP_DEFAULT,
    ND_CSP_SCRIPT,
    ND_CSP_STYLE,
    ND_CSP_IMG,
    ND_CSP_MEDIA,
    ND_CSP_CONNECT,
    ND_CSP_FONT,
    ND_CSP_FRAME,
    ND_CSP_CHILD,
    ND_CSP_FRAME_ANCESTORS,
    ND_CSP_KIND_COUNT,
} nd_csp_kind;

typedef struct nd_csp nd_csp;

nd_csp *nd_csp_parse(const char *header_value);
void    nd_csp_free(nd_csp *csp);

gboolean nd_csp_allows(const nd_csp *csp, nd_csp_kind kind,
                       const char *resource_url,
                       const char *document_url);

gboolean nd_csp_allows_with_nonce(const nd_csp *csp, nd_csp_kind kind,
                                  const char *resource_url,
                                  const char *document_url,
                                  const char *nonce);

gboolean nd_csp_inline_script_allowed(const nd_csp *csp,
                                      const char *body, gsize body_len,
                                      const char *nonce);

gboolean nd_csp_inline_event_handler_allowed(const nd_csp *csp);

gboolean nd_csp_javascript_url_allowed(const nd_csp *csp);

G_END_DECLS

#endif
