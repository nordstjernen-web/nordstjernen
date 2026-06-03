/* Nordstjernen — libcurl-backed async fetcher API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
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
    char *refresh;
    char *raw_headers;
    GByteArray *body;
    char *error;
    char *tls_warning;
    int   redirect_count;
} nd_response;


void nd_response_free(nd_response *resp);

char *nd_build_error_page(const char *url, long status,
                          const char *transport_error);

void nd_net_init(void);
void nd_net_shutdown(void);

const char *nd_net_default_accept_language(void);
const char *nd_net_supported_encodings(void);

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

nd_response *nd_net_request_blocking(const char        *url,
                                     const char        *top_url,
                                     const char        *method,
                                     const void        *body,
                                     gsize              body_len,
                                     const char        *content_type,
                                     const char *const *extra_headers,
                                     GCancellable      *cancellable,
                                     GError           **error);

char    *nd_net_hsts_upgrade(const char *url);
gboolean nd_net_hsts_should_upgrade(const char *host);

char *nd_url_host_from(const char *url);
char *nd_url_origin_from(const char *url);
gboolean nd_url_same_origin(const char *a, const char *b);
gboolean nd_url_is_http_or_https(const char *url);

gboolean nd_data_url_decode(const char *url, GByteArray *out, guint64 budget,
                            char **out_content_type, gboolean *too_large);
gboolean nd_url_is_valid_absolute(const char *url);
char    *nd_url_resolve(const char *base, const char *href);

char    *nd_url_to_display(const char *url);

typedef struct nd_url_parts {
    char *href;
    char *protocol;
    char *origin;
    char *host;
    char *hostname;
    char *port;
    char *pathname;
    char *search;
    char *hash;
    char *username;
    char *password;
} nd_url_parts;

nd_url_parts *nd_url_parts_new(const char *url);
void          nd_url_parts_free(nd_url_parts *parts);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(nd_url_parts, nd_url_parts_free)

char *nd_net_cookies_for_js(const char *url);

void  nd_net_set_proxy_override(const char *proxy_url);
void  nd_net_set_allow_file_urls(gboolean allow);
void  nd_net_set_log_fetches(gboolean on);
char *nd_net_proxy_mask(const char *proxy_url);
char *nd_net_effective_proxy_for(const char *url);

char *nd_multipart_boundary(void);
void  nd_multipart_quote_field(GString *out, const char *s);

void  nd_form_urlencoded_append(GString *out, const char *s);
void  nd_form_urlencoded_append_pair(GString *out, gboolean *first,
                                     const char *name, const char *value);

G_END_DECLS

#endif
