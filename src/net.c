/* Nordstjernen — libcurl-backed async fetcher. */

#include "net.h"

#include <curl/curl.h>
#include <string.h>

#define ND_NET_DOMAIN nd_net_error_quark()

static GQuark
nd_net_error_quark(void)
{
    return g_quark_from_static_string("nd-net-error");
}

static int
nd_xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
               curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    GCancellable *c = clientp;
    return (c && g_cancellable_is_cancelled(c)) ? 1 : 0;
}

void
nd_net_init(void)
{

    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void
nd_net_shutdown(void)
{
    curl_global_cleanup();
}

void
nd_response_free(nd_response *resp)
{
    if (!resp)
        return;
    g_free(resp->final_url);
    g_free(resp->content_type);
    if (resp->body)
        g_byte_array_unref(resp->body);
    g_free(resp->error);
    g_free(resp);
}

static size_t
nd_write_cb(char *data, size_t size, size_t nmemb, void *userdata)
{
    GByteArray *body = userdata;
    size_t bytes = size * nmemb;

    if (bytes == 0)
        return 0;
    g_byte_array_append(body, (const guint8 *)data, bytes);
    return bytes;
}

static size_t
nd_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    char **content_type_out = userdata;
    size_t bytes = size * nitems;
    static const char prefix[] = "Content-Type:";
    const size_t prefix_len = sizeof(prefix) - 1;

    if (bytes >= prefix_len && g_ascii_strncasecmp(buffer, prefix, prefix_len) == 0) {
        const char *v = buffer + prefix_len;
        size_t vlen = bytes - prefix_len;

        while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }

        while (vlen > 0 &&
               (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' ||
                v[vlen - 1] == ' '  || v[vlen - 1] == '\t')) {
            vlen--;
        }
        g_free(*content_type_out);
        *content_type_out = g_strndup(v, vlen);
    }
    return bytes;
}

static nd_response *
nd_fetch_sync(const char *url, GCancellable *cancellable, GError **error)
{
    nd_response *resp = g_new0(nd_response, 1);
    resp->body = g_byte_array_new();

    CURL *curl = curl_easy_init();
    if (!curl) {
        g_set_error_literal(error, ND_NET_DOMAIN, 1, "curl_easy_init failed");
        nd_response_free(resp);
        return NULL;
    }

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)ND_MAX_REDIRECTS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)ND_DEFAULT_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ND_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nd_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp->body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, nd_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp->content_type);

    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, nd_xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancellable);

    CURLcode rc = curl_easy_perform(curl);

    long status = 0;
    char *eff_url = NULL;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url);
    resp->status = status;
    resp->final_url = g_strdup(eff_url ? eff_url : url);

    if (rc != CURLE_OK) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && cancellable &&
            g_cancellable_is_cancelled(cancellable)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "fetch cancelled");
            curl_easy_cleanup(curl);
            nd_response_free(resp);
            return NULL;
        }
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        resp->error = g_strdup(msg);
    }

    curl_easy_cleanup(curl);
    return resp;
}

typedef struct nd_fetch_ctx {
    char         *url;
} nd_fetch_ctx;

static void
nd_fetch_ctx_free(gpointer data)
{
    nd_fetch_ctx *ctx = data;
    g_free(ctx->url);
    g_free(ctx);
}

static void
nd_fetch_thread(GTask        *task,
                gpointer      source_object,
                gpointer      task_data,
                GCancellable *cancellable)
{
    (void)source_object;
    nd_fetch_ctx *ctx = task_data;
    GError *err = NULL;
    nd_response *resp = nd_fetch_sync(ctx->url, cancellable, &err);
    if (!resp) {
        g_task_return_error(task, err);
        return;
    }
    g_task_return_pointer(task, resp, (GDestroyNotify)nd_response_free);
}

void
nd_net_fetch_async(const char        *url,
                   GCancellable      *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    g_return_if_fail(url != NULL);

    nd_fetch_ctx *ctx = g_new0(nd_fetch_ctx, 1);
    ctx->url = g_strdup(url);

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, nd_net_fetch_async);
    g_task_set_task_data(task, ctx, nd_fetch_ctx_free);
    g_task_run_in_thread(task, nd_fetch_thread);
    g_object_unref(task);
}

nd_response *
nd_net_fetch_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}
