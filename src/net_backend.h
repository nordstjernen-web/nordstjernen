/* Nordstjernen — HTTP transport backend seam (libcurl or libnghttp2). */

#ifndef NS_NET_BACKEND_H
#define NS_NET_BACKEND_H

#include <glib.h>
#include <curl/curl.h>

G_BEGIN_DECLS

typedef struct ns_write_ctx {
    GByteArray *body;
    guint64     total;
    guint64     budget;
    guint64     next_recheck;
    gboolean    exceeded;
} ns_write_ctx;

typedef struct ns_header_ctx {
    char   **content_type_out;
    char   **content_disposition_out;
    char   **csp_out;
    char   **xframe_options_out;
    char   **x_content_type_options_out;
    char   **cors_allow_origin_out;
    char   **refresh_out;
    char   **content_language_out;
    char    *etag;
    char    *last_modified;
    char    *cache_control;
    char    *expires;
    char    *location;
    GString *raw;
    gboolean set_cookie_seen;
} ns_header_ctx;

typedef struct ns_hop_req {
    const char        *url;
    const char        *method;
    const void        *body;
    gsize              body_len;
    struct curl_slist *headers;
    const char        *user_agent;
    const char        *referer;
    int                referer_policy;
    const char        *accept_encoding;
    long               timeout_s;
    long               connect_timeout_s;
    const char        *proxy;
    const char        *no_proxy;
    const char        *cookie_jar_path;
    const char        *cookie_js_path;
    gboolean           follow_redirects;
    long               max_redirs;
    gboolean           is_navigation;
    gboolean           request_ftp;
    gboolean           initial_https;
    long               http_version_pref;
} ns_hop_req;

typedef struct ns_hop_out {
    long     status;
    char    *effective_url;
    char    *remote_ip;
    long     http_version;
    long     num_connects;
    double   t_namelookup_ms;
    double   t_connect_ms;
    double   t_appconnect_ms;
    double   t_pretransfer_ms;
    double   t_starttransfer_ms;
    double   t_total_ms;
    gboolean ok;
    gboolean cancelled;
    gboolean tls_verify_failed;
    gboolean connect_failed;
    char    *tls_warning;
    char    *error_message;
} ns_hop_out;

void ns_hop_out_clear(ns_hop_out *out);

gboolean ns_hop_transport(const ns_hop_req *req, ns_write_ctx *wctx,
                          ns_header_ctx *hctx, ns_hop_out *out,
                          GCancellable *cancellable);

gboolean ns_hop_transport_curl(const ns_hop_req *req, ns_write_ctx *wctx,
                               ns_header_ctx *hctx, ns_hop_out *out,
                               GCancellable *cancellable);

void     ns_body_sink_init(ns_write_ctx *ctx, GByteArray *body);
gboolean ns_body_sink_write(ns_write_ctx *ctx, const void *data, size_t len);
void     ns_header_sink_feed(ns_header_ctx *ctx, const char *line, size_t len);

const char *ns_net_ec_curves(void);
gboolean    ns_net_aborting(void);
void        ns_net_store_set_cookie(const char *url, const char *set_cookie_value);
char       *ns_net_cookies_for_request(const char *url);

G_END_DECLS

#endif
