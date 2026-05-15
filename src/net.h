/* Nordstjernen — libcurl-backed async fetcher API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_NET_H
#define ND_NET_H

#include <gio/gio.h>
#include <glib.h>

#include "version.h"

G_BEGIN_DECLS

#define ND_MAX_REDIRECTS 10
#define ND_DEFAULT_TIMEOUT_S 30
#define ND_MAX_TIMEOUT_S 60
#define ND_USER_AGENT "Nordstjernen/" ND_VERSION " (+https://nordstjernen.org)"

typedef struct nd_response {
    long  status;
    char *final_url;
    char *content_type;
    char *content_disposition;
    char *csp_header;
    char *xframe_options;
    char *cors_allow_origin;
    GByteArray *body;
    char *error;
    char *tls_warning;
    int   redirect_count;
} nd_response;

gboolean nd_response_allows_framing(const char *xframe_options,
                                    const char *csp_header,
                                    const char *parent_url,
                                    const char *document_url);

void nd_response_free(nd_response *resp);

void nd_net_init(void);
void nd_net_shutdown(void);

const char *nd_net_default_accept_language(void);

void nd_net_fetch_async(const char        *url,
                        const char        *top_url,
                        GCancellable      *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer            user_data);

void nd_net_post_async(const char         *url,
                       const char         *top_url,
                       const void         *body,
                       gsize               body_len,
                       const char         *content_type,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data);

void nd_net_request_async(const char         *url,
                          const char         *top_url,
                          const char         *method,
                          const void         *body,
                          gsize               body_len,
                          const char         *content_type,
                          const char *const  *extra_headers,
                          GCancellable       *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer            user_data);

nd_response *nd_net_fetch_finish(GAsyncResult *result, GError **error);

nd_response *nd_net_fetch_blocking(const char   *url,
                                   GCancellable *cancellable,
                                   GError      **error);

char    *nd_net_hsts_upgrade(const char *url);
gboolean nd_net_hsts_should_upgrade(const char *host);

char *nd_url_host_from(const char *url);
char *nd_url_origin_from(const char *url);
gboolean nd_url_same_origin(const char *a, const char *b);
gboolean nd_url_is_same_site(const char *a, const char *b);
gboolean nd_url_is_http_or_https(const char *url);
char    *nd_url_resolve(const char *base, const char *href);

void nd_net_clear_cookies(void);

G_END_DECLS

#endif
