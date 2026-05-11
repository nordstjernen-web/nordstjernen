/* Nordstjernen — libcurl-backed async fetcher API. */

#ifndef ND_NET_H
#define ND_NET_H

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

#define ND_MAX_REDIRECTS 10
#define ND_DEFAULT_TIMEOUT_S 30
#define ND_USER_AGENT "Nordstjernen/0.0.1 (+https://github.com/operativsystem42/nordstjernen)"

typedef struct nd_response {
    long  status;
    char *final_url;
    char *content_type;
    GByteArray *body;
    char *error;
} nd_response;

void nd_response_free(nd_response *resp);

void nd_net_init(void);
void nd_net_shutdown(void);

void nd_net_fetch_async(const char        *url,
                        GCancellable      *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer            user_data);

nd_response *nd_net_fetch_finish(GAsyncResult *result, GError **error);

G_END_DECLS

#endif
