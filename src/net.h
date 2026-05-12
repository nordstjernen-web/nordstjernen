/* Nordstjernen — libcurl-backed async fetcher API. */

#ifndef ND_NET_H
#define ND_NET_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

#define ND_MAX_REDIRECTS 10
#define ND_DEFAULT_TIMEOUT_S 30
#define ND_USER_AGENT "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"

typedef struct nd_response {
    long  status;
    char *final_url;
    char *content_type;
    char *csp_header;
    char *cors_allow_origin;
    GByteArray *body;
    char *error;
    char *tls_warning;
} nd_response;

void nd_response_free(nd_response *resp);

void nd_net_init(void);
void nd_net_shutdown(void);

void nd_net_fetch_async(const char        *url,
                        GCancellable      *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer            user_data);

void nd_net_post_async(const char         *url,
                       const void         *body,
                       gsize               body_len,
                       const char         *content_type,
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
gboolean nd_url_is_http_or_https(const char *url);
char    *nd_url_resolve(const char *base, const char *href);

G_END_DECLS

#endif
