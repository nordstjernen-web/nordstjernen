/* Nordstjernen — Content-Security-Policy parser + check.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_CSP_H
#define NS_CSP_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum ns_csp_kind {
    NS_CSP_DEFAULT,
    NS_CSP_SCRIPT,
    NS_CSP_STYLE,
    NS_CSP_IMG,
    NS_CSP_MEDIA,
    NS_CSP_CONNECT,
    NS_CSP_FONT,
    NS_CSP_FRAME,
    NS_CSP_CHILD,
    NS_CSP_WORKER,
    NS_CSP_FRAME_ANCESTORS,
    NS_CSP_OBJECT,
    NS_CSP_BASE_URI,
    NS_CSP_FORM_ACTION,
    NS_CSP_KIND_COUNT,
} ns_csp_kind;

typedef struct ns_csp ns_csp;

ns_csp *ns_csp_parse(const char *header_value);
void    ns_csp_free(ns_csp *csp);
void    ns_csp_merge(ns_csp *dst, ns_csp *src);

gboolean ns_csp_allows(const ns_csp *csp, ns_csp_kind kind,
                       const char *resource_url,
                       const char *document_url);

gboolean ns_csp_allows_with_nonce(const ns_csp *csp, ns_csp_kind kind,
                                  const char *resource_url,
                                  const char *document_url,
                                  const char *nonce,
                                  gboolean parser_inserted);

gboolean ns_csp_inline_script_allowed(const ns_csp *csp,
                                      const char *body, gsize body_len,
                                      const char *nonce);

gboolean ns_csp_inline_event_handler_allowed(const ns_csp *csp);


G_END_DECLS

#endif
