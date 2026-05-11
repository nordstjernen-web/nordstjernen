/* Nordstjernen — libcurl-backed async fetcher. */

#include "net.h"

#include <curl/curl.h>
#include <string.h>

#include <glib/gstdio.h>

static char *g_cookie_path;
static char *g_hsts_path;
static GHashTable *g_hsts_table;

typedef struct nd_hsts_entry {
    gint64    expiry;
    gboolean  include_subdomains;
} nd_hsts_entry;

static char *
nd_net_hsts_path(void)
{
    if (g_hsts_path) return g_hsts_path;
    const char *data = g_get_user_data_dir();
    char *dir = g_build_filename(data, "nordstjernen", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_hsts_path = g_build_filename(dir, "hsts.txt", NULL);
    g_free(dir);
    return g_hsts_path;
}

static void
nd_hsts_table_init(void)
{
    if (g_hsts_table) return;
    g_hsts_table = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, g_free);
    char *path = nd_net_hsts_path();
    char *content = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &content, &len, NULL)) return;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    char **lines = g_strsplit(content, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i]) continue;
        char **fields = g_strsplit(lines[i], "\t", -1);
        if (g_strv_length(fields) >= 3) {
            gint64 expiry = g_ascii_strtoll(fields[1], NULL, 10);
            int subs = (int)g_ascii_strtoll(fields[2], NULL, 10);
            if (expiry > now) {
                nd_hsts_entry *e = g_new0(nd_hsts_entry, 1);
                e->expiry = expiry;
                e->include_subdomains = subs != 0;
                g_hash_table_replace(g_hsts_table, g_strdup(fields[0]), e);
            }
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(content);
}

static void
nd_hsts_table_save(void)
{
    if (!g_hsts_table) return;
    char *path = nd_net_hsts_path();
    GString *out = g_string_new(NULL);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_hsts_table);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const char *host = k;
        const nd_hsts_entry *e = v;
        g_string_append_printf(out, "%s\t%" G_GINT64_FORMAT "\t%d\n",
                               host, e->expiry, e->include_subdomains ? 1 : 0);
    }
    g_file_set_contents(path, out->str, (gssize)out->len, NULL);
    g_chmod(path, 0600);
    g_string_free(out, TRUE);
}

static void
nd_hsts_record(const char *host, gint64 max_age, gboolean include_subs)
{
    if (!host || !*host || max_age <= 0) return;
    nd_hsts_table_init();
    nd_hsts_entry *e = g_new0(nd_hsts_entry, 1);
    e->expiry = g_get_real_time() / G_USEC_PER_SEC + max_age;
    e->include_subdomains = include_subs;
    char *lower = g_ascii_strdown(host, -1);
    g_hash_table_replace(g_hsts_table, lower, e);
    nd_hsts_table_save();
}

static char *
host_from_url(const char *url)
{
    if (!url) return NULL;
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return NULL;
    const char *host = scheme_end + 3;
    const char *host_end = host;
    while (*host_end && *host_end != '/' && *host_end != ':' && *host_end != '?' &&
           *host_end != '#')
        host_end++;
    return g_strndup(host, (gsize)(host_end - host));
}

gboolean
nd_net_hsts_should_upgrade(const char *host)
{
    if (!host || !*host) return FALSE;
    nd_hsts_table_init();
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    char *lower = g_ascii_strdown(host, -1);
    nd_hsts_entry *e = g_hash_table_lookup(g_hsts_table, lower);
    if (e && e->expiry > now) { g_free(lower); return TRUE; }
    const char *dot = lower;
    while ((dot = strchr(dot, '.')) != NULL) {
        const char *parent = dot + 1;
        e = g_hash_table_lookup(g_hsts_table, parent);
        if (e && e->expiry > now && e->include_subdomains) {
            g_free(lower);
            return TRUE;
        }
        dot = parent;
    }
    g_free(lower);
    return FALSE;
}

char *
nd_net_hsts_upgrade(const char *url)
{
    if (!url) return NULL;
    if (!g_str_has_prefix(url, "http://")) return NULL;
    char *host = host_from_url(url);
    if (!host) return NULL;
    gboolean upgrade = nd_net_hsts_should_upgrade(host);
    g_free(host);
    if (!upgrade) return NULL;
    return g_strconcat("https://", url + 7, NULL);
}

static const char *
nd_net_cookie_path(void)
{
    if (g_cookie_path) return g_cookie_path;
    const char *config = g_get_user_config_dir();
    char *dir = g_build_filename(config, "nordstjernen", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_cookie_path = g_build_filename(dir, "cookies.txt", NULL);
    g_free(dir);
    return g_cookie_path;
}

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
    nd_hsts_table_save();
    curl_global_cleanup();
    g_free(g_cookie_path);
    g_cookie_path = NULL;
    g_free(g_hsts_path);
    g_hsts_path = NULL;
    if (g_hsts_table) {
        g_hash_table_destroy(g_hsts_table);
        g_hsts_table = NULL;
    }
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

typedef struct nd_header_ctx {
    char **content_type_out;
    char  *sts_host;
    gint64 sts_max_age;
    gboolean sts_include_subs;
    gboolean sts_seen;
} nd_header_ctx;

static size_t
nd_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    nd_header_ctx *hc = userdata;
    size_t bytes = size * nitems;
    static const char ct_prefix[]  = "Content-Type:";
    static const char sts_prefix[] = "Strict-Transport-Security:";
    const size_t ct_len  = sizeof(ct_prefix)  - 1;
    const size_t sts_len = sizeof(sts_prefix) - 1;

    if (bytes >= ct_len && g_ascii_strncasecmp(buffer, ct_prefix, ct_len) == 0) {
        const char *v = buffer + ct_len;
        size_t vlen = bytes - ct_len;
        while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
        while (vlen > 0 &&
               (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' ||
                v[vlen - 1] == ' '  || v[vlen - 1] == '\t')) vlen--;
        g_free(*hc->content_type_out);
        *hc->content_type_out = g_strndup(v, vlen);
    } else if (bytes >= sts_len &&
               g_ascii_strncasecmp(buffer, sts_prefix, sts_len) == 0) {
        const char *v = buffer + sts_len;
        size_t vlen = bytes - sts_len;
        while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
        while (vlen > 0 &&
               (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' ||
                v[vlen - 1] == ' '  || v[vlen - 1] == '\t')) vlen--;
        char *line = g_strndup(v, vlen);
        char **toks = g_strsplit(line, ";", -1);
        for (int i = 0; toks[i]; i++) {
            char *t = g_strstrip(toks[i]);
            if (g_ascii_strncasecmp(t, "max-age", 7) == 0) {
                const char *eq = strchr(t, '=');
                if (eq) hc->sts_max_age = g_ascii_strtoll(eq + 1, NULL, 10);
            } else if (g_ascii_strcasecmp(t, "includeSubDomains") == 0) {
                hc->sts_include_subs = TRUE;
            }
        }
        g_strfreev(toks);
        g_free(line);
        hc->sts_seen = TRUE;
    }
    return bytes;
}

static const char k_about_mozilla[] =
    "<!doctype html><html><head><title>About Nordstjernen</title></head>"
    "<body>"
    "<h1>Nordstjernen</h1>"
    "<p>Nordstjernen is a small web browser written in C. It uses "
    "GTK 4 for the user interface and libcurl for networking. The "
    "rendering engine, HTML and CSS parsers, layout, and paint code "
    "are written from scratch in this repository — there is no "
    "third-party browser engine here.</p>"
    "<p>Design goals: minimal, compact, secure, English-only, "
    "no telemetry, no plugins, no DRM. HTML5 / modern CSS / modern "
    "JavaScript are supported pragmatically as far as is feasible "
    "without bloat. At most one video codec is active at a time.</p>"
    "<p>The typical user is a university student reading the web — "
    "Wikipedia, news, search results, documentation, simple forms.</p>"
    "<p>Developed by Andreas Røsdal, with extensive use of AI "
    "tooling. Copyright 2026, all rights reserved. Not open source.</p>"
    "<p><b>And the beast shall come forth surrounded by a roiling "
    "cloud of vengeance. The house of the unbelievers shall be "
    "razed and they shall be scorched to the earth. Their tags shall "
    "blink until the end of days.</b><br>— Mammon, 40:1-3</p>"
    "</body></html>";

static gboolean
synthesize_about_response(const char *url, nd_response *resp)
{
    if (!g_str_has_prefix(url, "about:")) return FALSE;
    const char *what = url + strlen("about:");
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = g_strdup("text/html; charset=utf-8");
    const char *body = NULL;
    if (g_str_equal(what, "mozilla") || g_str_equal(what, "blank") ||
        g_str_equal(what, "")) {
        body = (what[0] == 'b' || what[0] == '\0') ? "<!doctype html><title>Blank</title>" : k_about_mozilla;
    } else {
        body = k_about_mozilla;
    }
    g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
    return TRUE;
}

static nd_response *
nd_fetch_sync(const char *url, const char *method,
              const void *body, gsize body_len, const char *content_type,
              GCancellable *cancellable, GError **error)
{
    nd_response *resp = g_new0(nd_response, 1);
    resp->body = g_byte_array_new();

    if (synthesize_about_response(url, resp))
        return resp;

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

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,"
                                          "application/xml;q=0.9,image/avif,image/webp,"
                                          "image/png,image/*;q=0.8,*/*;q=0.5");
    headers = curl_slist_append(headers, "DNT: 1");

    if (method && g_ascii_strcasecmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (body && body_len > 0) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
        char *ct_hdr = g_strdup_printf("Content-Type: %s",
            content_type && *content_type ? content_type
                                          : "application/x-www-form-urlencoded");
        headers = curl_slist_append(headers, ct_hdr);
        g_free(ct_hdr);
    } else if (method && *method) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const char *cookie_path = nd_net_cookie_path();
    if (cookie_path) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_path);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR,  cookie_path);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nd_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp->body);
    nd_header_ctx header_ctx = {0};
    header_ctx.content_type_out = &resp->content_type;
    header_ctx.sts_host = host_from_url(url);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, nd_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_ctx);

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

    if (header_ctx.sts_seen && header_ctx.sts_host && eff_url &&
        g_str_has_prefix(eff_url, "https://")) {
        nd_hsts_record(header_ctx.sts_host, header_ctx.sts_max_age,
                       header_ctx.sts_include_subs);
    }
    g_free(header_ctx.sts_host);

    if (rc != CURLE_OK) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && cancellable &&
            g_cancellable_is_cancelled(cancellable)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "fetch cancelled");
            curl_easy_cleanup(curl);
            if (headers) curl_slist_free_all(headers);
            nd_response_free(resp);
            return NULL;
        }
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        resp->error = g_strdup(msg);
    }

    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    return resp;
}

nd_response *
nd_net_fetch_blocking(const char *url, GCancellable *cancellable, GError **error)
{
    return nd_fetch_sync(url, "GET", NULL, 0, NULL, cancellable, error);
}

typedef struct nd_fetch_ctx {
    char *url;
    char *method;
    char *content_type;
    guint8 *body;
    gsize body_len;
} nd_fetch_ctx;

static void
nd_fetch_ctx_free(gpointer data)
{
    nd_fetch_ctx *ctx = data;
    g_free(ctx->url);
    g_free(ctx->method);
    g_free(ctx->content_type);
    g_free(ctx->body);
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
    nd_response *resp = nd_fetch_sync(ctx->url, ctx->method,
                                      ctx->body, ctx->body_len, ctx->content_type,
                                      cancellable, &err);
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

void
nd_net_post_async(const char         *url,
                  const void         *body,
                  gsize               body_len,
                  const char         *content_type,
                  GCancellable       *cancellable,
                  GAsyncReadyCallback callback,
                  gpointer            user_data)
{
    g_return_if_fail(url != NULL);

    nd_fetch_ctx *ctx = g_new0(nd_fetch_ctx, 1);
    ctx->url = g_strdup(url);
    ctx->method = g_strdup("POST");
    ctx->content_type = g_strdup(content_type ? content_type
                                              : "application/x-www-form-urlencoded");
    if (body && body_len > 0) {
        ctx->body = g_memdup2(body, body_len);
        ctx->body_len = body_len;
    }

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, nd_net_post_async);
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
