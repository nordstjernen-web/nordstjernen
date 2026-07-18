/* Nordstjernen — libcurl-backed async fetcher API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_NET_H
#define NS_NET_H

#include <gio/gio.h>
#include <glib.h>

#include "version.h"

G_BEGIN_DECLS

#define NS_MAX_REDIRECTS 10
#define NS_DEFAULT_TIMEOUT_S 30
#define NS_MAX_TIMEOUT_S 60
#define NS_CHROME_MAJOR   "146"
#define NS_CHROME_VERSION NS_CHROME_MAJOR ".0.0.0"

#if defined(__ANDROID__)
#  define NS_NAV_PLATFORM        "Linux armv8l"
#  define NS_UA_HINT_PLATFORM    "Android"
#  define NS_UA_HINT_MOBILE      1
#  define NS_SEC_CH_UA_MOBILE    "?1"
#  define NS_USER_AGENT \
       "Mozilla/5.0 (Linux; Android 14; K) Nordstjernen/1.0 " \
       "Chrome/" NS_CHROME_VERSION " AppleWebKit/537.36 Mobile Safari/537.36"
#else
#  if defined(_WIN32)
#    define NS_UA_PLATFORM_TOKEN "Windows NT 10.0; Win64; x64"
#    define NS_NAV_PLATFORM      "Win32"
#    define NS_UA_HINT_PLATFORM  "Windows"
#  elif defined(__APPLE__)
#    define NS_UA_PLATFORM_TOKEN "Macintosh; Intel Mac OS X 10_15_7"
#    define NS_NAV_PLATFORM      "MacIntel"
#    define NS_UA_HINT_PLATFORM  "macOS"
#  else
#    define NS_UA_PLATFORM_TOKEN "X11; Linux x86_64"
#    define NS_NAV_PLATFORM      "Linux x86_64"
#    define NS_UA_HINT_PLATFORM  "Linux"
#  endif
#  define NS_UA_HINT_MOBILE      0
#  define NS_SEC_CH_UA_MOBILE    "?0"
#  define NS_USER_AGENT \
       "Mozilla/5.0 (" NS_UA_PLATFORM_TOKEN ") Nordstjernen/1.0 " \
       "Chrome/" NS_CHROME_VERSION " AppleWebKit/537.36 Safari/537.36"
#  define NS_UA_LADYBIRD \
       "Mozilla/5.0 (" NS_UA_PLATFORM_TOKEN ") Ladybird/1.0 " \
       "Chrome/146.0.0.0 AppleWebKit/537.36 Safari/537.36"
#  define NS_UA_FIREFOX \
       "Mozilla/5.0 (" NS_UA_PLATFORM_TOKEN "; rv:143.0) " \
       "Gecko/20100101 Firefox/143.0"
#endif
#ifndef NS_UA_LADYBIRD
#  define NS_UA_LADYBIRD NS_USER_AGENT
#endif
#ifndef NS_UA_FIREFOX
#  define NS_UA_FIREFOX  NS_USER_AGENT
#endif

const char *ns_user_agent_for_mode(const char *compat_mode);
gboolean    ns_user_agent_has_client_hints(const char *user_agent);

typedef enum {
    NS_SEC_NONE = 0,
    NS_SEC_SECURE,
    NS_SEC_INVALID,
    NS_SEC_PLAIN
} ns_security;

typedef struct ns_response {
    long  status;
    char *final_url;
    char *content_type;
    char *content_disposition;
    char *csp_header;
    char *xframe_options;
    char *x_content_type_options;
    char *cors_allow_origin;
    char *refresh;
    char *content_language;
    char *raw_headers;
    GByteArray *body;
    char *error;
    char *tls_warning;
    char *remote_ip;
    gint64 request_start_us;
    double request_start_real_ms;
    double domain_lookup_ms;
    double connect_ms;
    double tls_ms;
    double pretransfer_ms;
    double response_start_ms;
    double response_end_ms;
    int   security;
    int   redirect_count;
} ns_response;


void ns_response_free(ns_response *resp);

char *ns_build_error_page(const char *url, long status,
                          const char *transport_error);

void ns_net_init(void);
void ns_net_shutdown(void);

/* Marks the next request as a top-level document navigation, so its
 * Sec-Fetch-Mode/Dest/User reflect a navigation even when a referrer
 * (top_url) is supplied. Set around the main document fetch and cleared
 * after; subresource fetches must run with it off. */
void ns_net_set_navigation_fetch(gboolean navigation);
gboolean ns_net_idle(void);

const char *ns_net_default_accept_language(void);
const char *ns_net_effective_accept_language(void);
char      **ns_net_navigator_languages(void);

typedef GBytes *(*ns_net_blob_resolver)(const char *url, char **out_type,
                                        gpointer user_data);
void ns_net_set_blob_resolver(ns_net_blob_resolver resolver, gpointer user_data);

void ns_net_fetch_async(const char        *url,
                        const char        *top_url,
                        GCancellable      *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer            user_data);

void ns_net_preconnect_async(const char *url);

void ns_net_post_async(const char         *url,
                       const char         *top_url,
                       const void         *body,
                       gsize               body_len,
                       const char         *content_type,
                       GCancellable       *cancellable,
                       GAsyncReadyCallback callback,
                       gpointer            user_data);

void ns_net_request_async(const char         *url,
                          const char         *top_url,
                          const char         *method,
                          const void         *body,
                          gsize               body_len,
                          const char         *content_type,
                          const char *const  *extra_headers,
                          GCancellable       *cancellable,
                          GAsyncReadyCallback callback,
                          gpointer            user_data);

ns_response *ns_net_fetch_finish(GAsyncResult *result, GError **error);

ns_response *ns_net_fetch_blocking(const char   *url,
                                   GCancellable *cancellable,
                                   GError      **error);

ns_response *ns_net_request_blocking(const char        *url,
                                     const char        *top_url,
                                     const char        *method,
                                     const void        *body,
                                     gsize              body_len,
                                     const char        *content_type,
                                     const char *const *extra_headers,
                                     GCancellable      *cancellable,
                                     GError           **error);

char    *ns_net_hsts_upgrade(const char *url);
gboolean ns_net_hsts_should_upgrade(const char *host);
char    *ns_net_https_first_upgrade(const char *url);
gboolean ns_net_header_is_nosniff(const char *value);

void  ns_net_log_clear(void);
char *ns_net_log_dump(void);

char *ns_url_host_from(const char *url);
char *ns_url_origin_from(const char *url);
gboolean ns_url_same_origin(const char *a, const char *b);
gboolean ns_url_is_http_or_https(const char *url);

gboolean ns_net_parse_refresh(const char *input, double *time_out,
                              char **url_out);

gboolean ns_data_url_decode(const char *url, GByteArray *out, guint64 budget,
                            char **out_content_type, gboolean *too_large);
gboolean ns_url_is_valid_absolute(const char *url);
char    *ns_url_strip_tracking_params(const char *url);
char    *ns_url_resolve(const char *base, const char *href);
char    *ns_url_resolve_len(const char *base, const char *href, size_t href_len);
char    *ns_url_set_component_len(const char *href, const char *component,
                                  const char *value, size_t value_len);


typedef struct ns_url_parts {
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
} ns_url_parts;

ns_url_parts *ns_url_parts_new(const char *url);
void          ns_url_parts_free(ns_url_parts *parts);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(ns_url_parts, ns_url_parts_free)

char *ns_net_cookies_for_js(const char *url);
void  ns_net_cookie_store_from_js(const char *url, const char *cookie);
void  ns_net_cookies_clear(void);
void  ns_net_site_storage_clear(void);

void  ns_net_set_proxy_override(const char *proxy_url);
void  ns_net_set_allow_file_urls(gboolean allow);
void  ns_net_set_log_fetches(gboolean on);
void  ns_net_perf_snapshot(guint64 *fetches, guint64 *bytes,
                           double *sum_ms, double *span_ms);
char *ns_net_proxy_mask(const char *proxy_url);
void  ns_net_apply_curl_proxy(void *curl_handle, const char *url);
void  ns_net_apply_curl_tls(void *curl_handle);
const char *ns_net_proxy_override(void);
const char *ns_net_http_proxy(void);
const char *ns_net_https_proxy(void);
const char *ns_net_no_proxy(void);
const char *ns_net_ca_bundle_path(void);

gboolean ns_address_is_search(const char *s);
char *ns_search_url_for(const char *query);
char *ns_url_from_local_path(const char *path);

char *ns_multipart_boundary(void);
void  ns_multipart_quote_field(GString *out, const char *s);

void  ns_form_urlencoded_append(GString *out, const char *s);
void  ns_form_set_submission_charset(const char *charset);
void  ns_form_urlencoded_append_pair(GString *out, gboolean *first,
                                     const char *name, const char *value);

G_END_DECLS

#endif
