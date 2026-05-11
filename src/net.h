/*
 * Nordstjernen — net.h
 *
 * Tiny libcurl-backed fetcher. One easy handle per call. TLS is delegated
 * to whatever curl was linked against (system OpenSSL on the target
 * platforms). Async API uses GTask so completion callbacks fire on the
 * GMainContext the request was issued from.
 */

#ifndef ND_NET_H
#define ND_NET_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

#define ND_MAX_REDIRECTS 10
#define ND_DEFAULT_TIMEOUT_S 30
#define ND_USER_AGENT "Nordstjernen/0.0.1 (+https://github.com/operativsystem42/nordstjernen)"

typedef struct nd_response {
    long  status;          /* HTTP status code, 0 on transport error */
    char *final_url;       /* effective URL after redirects, may be NULL */
    char *content_type;    /* dup of Content-Type header value, may be NULL */
    GByteArray *body;      /* raw response body bytes, never NULL on success */
    char *error;           /* curl error message, NULL on success */
} nd_response;

void nd_response_free(nd_response *resp);

/* Module init/shutdown. Call once at startup / shutdown. */
void nd_net_init(void);
void nd_net_shutdown(void);

/* Async fetch. callback receives a GAsyncResult; finish with
 * nd_net_fetch_finish() to get an owned nd_response*. */
void nd_net_fetch_async(const char        *url,
                        GCancellable      *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer            user_data);

nd_response *nd_net_fetch_finish(GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* ND_NET_H */
