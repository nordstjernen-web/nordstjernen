/* Nordstjernen — libcurl-backed async fetcher. */

#include "net.h"
#include "cache.h"
#include "config.h"

#include <curl/curl.h>
#include <string.h>

#include <glib/gstdio.h>

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#endif

static char *g_cookie_path;
static char *g_hsts_path;
static char *g_altsvc_path;
static GHashTable *g_hsts_table;
static char *g_ca_bundle;
static gboolean g_has_http3;

typedef struct nd_hsts_entry {
    gint64    expiry;
    gboolean  include_subdomains;
} nd_hsts_entry;

static char *
nd_net_hsts_path(void)
{
    if (g_hsts_path) return g_hsts_path;
    const char *data = g_get_user_data_dir();
    char *dir = g_build_filename(data, ND_APP_DIR_NAME, NULL);
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
    GError *err = NULL;
    if (!g_file_set_contents(path, out->str, (gssize)out->len, &err)) {
        g_warning("hsts: failed to write %s: %s", path, err->message);
        g_clear_error(&err);
    }
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

gboolean
nd_url_is_http_or_https(const char *url)
{
    return url && (g_str_has_prefix(url, "http://") ||
                   g_str_has_prefix(url, "https://"));
}

char *
nd_url_resolve(const char *base, const char *href)
{
    if (!href || !*href) return NULL;
    if (g_str_has_prefix(href, "http://") ||
        g_str_has_prefix(href, "https://") ||
        g_str_has_prefix(href, "data:") ||
        g_str_has_prefix(href, "about:"))
        return g_strdup(href);
    if (g_str_has_prefix(href, "//")) return g_strconcat("https:", href, NULL);
    if (!base || !*base) return NULL;
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end) return NULL;
    if (href[0] == '#') {
        const char *base_frag = strchr(base, '#');
        gsize keep = base_frag ? (gsize)(base_frag - base) : strlen(base);
        char *root = g_strndup(base, keep);
        char *full = g_strconcat(root, href, NULL);
        g_free(root);
        return full;
    }
    if (href[0] == '?') {
        const char *base_q = strpbrk(base, "?#");
        gsize keep = base_q ? (gsize)(base_q - base) : strlen(base);
        char *root = g_strndup(base, keep);
        char *full = g_strconcat(root, href, NULL);
        g_free(root);
        return full;
    }
    if (href[0] == '/') {
        const char *host_start = scheme_end + 3;
        const char *host_end = strchr(host_start, '/');
        gsize host_len = host_end ? (gsize)(host_end - base) : strlen(base);
        char *root = g_strndup(base, host_len);
        char *full = g_strconcat(root, href, NULL);
        g_free(root);
        return full;
    }
    const char *q = strrchr(base, '/');
    if (q && q > scheme_end + 2) {
        gsize prefix_len = (gsize)(q - base) + 1;
        char *prefix = g_strndup(base, prefix_len);
        char *full = g_strconcat(prefix, href, NULL);
        g_free(prefix);
        return full;
    }
    return g_strconcat(base, "/", href, NULL);
}

char *
nd_url_origin_from(const char *url)
{
    if (!url || !*url) return NULL;
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        return NULL;
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return NULL;
    const char *p = scheme_end + 3;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    return g_strndup(url, (gsize)(p - url));
}

gboolean
nd_url_same_origin(const char *a, const char *b)
{
    char *oa = nd_url_origin_from(a);
    char *ob = nd_url_origin_from(b);
    gboolean eq = oa && ob && g_ascii_strcasecmp(oa, ob) == 0;
    g_free(oa);
    g_free(ob);
    return eq;
}

char *
nd_url_host_from(const char *url)
{
    if (!url) return NULL;
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return NULL;
    const char *authority = scheme_end + 3;
    const char *authority_end = authority;
    while (*authority_end && *authority_end != '/' &&
           *authority_end != '?' && *authority_end != '#')
        authority_end++;
    const char *host = authority;
    for (const char *c = authority; c < authority_end; c++) {
        if (*c == '@') { host = c + 1; break; }
    }
    const char *host_end = host;
    while (host_end < authority_end && *host_end != ':')
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
    char *host = nd_url_host_from(url);
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
    char *dir = g_build_filename(config, ND_APP_DIR_NAME, NULL);
    g_mkdir_with_parents(dir, 0700);
    g_cookie_path = g_build_filename(dir, "cookies.txt", NULL);
    g_free(dir);
    return g_cookie_path;
}

static const char *
nd_net_altsvc_path(void)
{
    if (g_altsvc_path) return g_altsvc_path;
    const char *data = g_get_user_data_dir();
    char *dir = g_build_filename(data, ND_APP_DIR_NAME, NULL);
    g_mkdir_with_parents(dir, 0700);
    g_altsvc_path = g_build_filename(dir, "altsvc.txt", NULL);
    g_free(dir);
    return g_altsvc_path;
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

static char *
nd_net_exe_dir(void)
{
#ifdef G_OS_WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return NULL;
    char *utf8 = g_utf16_to_utf8((gunichar2 *)buf, -1, NULL, NULL, NULL);
    if (!utf8) return NULL;
    char *dir = g_path_get_dirname(utf8);
    g_free(utf8);
    return dir;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    if (size == 0) return NULL;
    char *raw = g_malloc(size);
    if (_NSGetExecutablePath(raw, &size) != 0) { g_free(raw); return NULL; }
    char *real = realpath(raw, NULL);
    char *dir = g_path_get_dirname(real ? real : raw);
    free(real);
    g_free(raw);
    return dir;
#elif defined(__linux__)
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe) return NULL;
    char *dir = g_path_get_dirname(exe);
    g_free(exe);
    return dir;
#else
    return NULL;
#endif
}

static gboolean
nd_net_try_ca_bundle(const char *path)
{
    if (!path || !*path) return FALSE;
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;
    g_ca_bundle = g_strdup(path);
    return TRUE;
}

static void
nd_net_resolve_ca_bundle(void)
{
    if (g_ca_bundle) return;
    const char *env = g_getenv("CURL_CA_BUNDLE");
    if (!env) env = g_getenv("SSL_CERT_FILE");
    if (nd_net_try_ca_bundle(env)) return;

    char *dir = nd_net_exe_dir();
    if (dir) {
        const char *rels[] = {
            "etc/ssl/certs/ca-bundle.crt",
            "ssl/certs/ca-bundle.crt",
            "ca-bundle.crt",
            "cert.pem",
            "../etc/ca-certificates/cert.pem",
            "../etc/openssl@3/cert.pem",
            "../etc/openssl/cert.pem",
            NULL,
        };
        for (int i = 0; rels[i]; i++) {
            char *cand = g_build_filename(dir, rels[i], NULL);
            gboolean ok = nd_net_try_ca_bundle(cand);
            g_free(cand);
            if (ok) break;
        }
        g_free(dir);
        if (g_ca_bundle) return;
    }

#ifdef __APPLE__
    const char *mac_paths[] = {
        "/opt/homebrew/etc/ca-certificates/cert.pem",
        "/opt/homebrew/etc/openssl@3/cert.pem",
        "/usr/local/etc/ca-certificates/cert.pem",
        "/usr/local/etc/openssl@3/cert.pem",
        "/usr/local/etc/openssl/cert.pem",
        "/etc/ssl/cert.pem",
        NULL,
    };
    for (int i = 0; mac_paths[i]; i++)
        if (nd_net_try_ca_bundle(mac_paths[i])) return;
#endif
}

void
nd_net_init(void)
{
    nd_net_resolve_ca_bundle();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
    g_has_http3 = vi && (vi->features & CURL_VERSION_HTTP3) != 0;
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
    g_free(g_altsvc_path);
    g_altsvc_path = NULL;
    g_free(g_ca_bundle);
    g_ca_bundle = NULL;
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
    g_free(resp->csp_header);
    g_free(resp->cors_allow_origin);
    if (resp->body)
        g_byte_array_unref(resp->body);
    g_free(resp->error);
    g_free(resp->tls_warning);
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
    char **csp_out;
    char **cors_allow_origin_out;
    char  *sts_host;
    gint64 sts_max_age;
    gboolean sts_include_subs;
    gboolean sts_seen;
    char  *etag;
    char  *last_modified;
    char  *cache_control;
    char  *expires;
} nd_header_ctx;

static char *
header_value_dup(const char *line, size_t bytes, size_t prefix_len)
{
    const char *v = line + prefix_len;
    size_t vlen = bytes - prefix_len;
    while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
    while (vlen > 0 &&
           (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' ||
            v[vlen - 1] == ' '  || v[vlen - 1] == '\t')) vlen--;
    return g_strndup(v, vlen);
}

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
        g_free(*hc->content_type_out);
        *hc->content_type_out = header_value_dup(buffer, bytes, ct_len);
    } else if (bytes >= sts_len &&
               g_ascii_strncasecmp(buffer, sts_prefix, sts_len) == 0) {
        char *line = header_value_dup(buffer, bytes, sts_len);
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
    } else if (bytes >= 5 && g_ascii_strncasecmp(buffer, "ETag:", 5) == 0) {
        g_free(hc->etag);
        hc->etag = header_value_dup(buffer, bytes, 5);
    } else if (bytes >= 14 && g_ascii_strncasecmp(buffer, "Last-Modified:", 14) == 0) {
        g_free(hc->last_modified);
        hc->last_modified = header_value_dup(buffer, bytes, 14);
    } else if (bytes >= 14 && g_ascii_strncasecmp(buffer, "Cache-Control:", 14) == 0) {
        g_free(hc->cache_control);
        hc->cache_control = header_value_dup(buffer, bytes, 14);
    } else if (bytes >= 8 && g_ascii_strncasecmp(buffer, "Expires:", 8) == 0) {
        g_free(hc->expires);
        hc->expires = header_value_dup(buffer, bytes, 8);
    } else if (bytes >= 24 &&
               g_ascii_strncasecmp(buffer, "Content-Security-Policy:", 24) == 0 &&
               hc->csp_out) {
        g_free(*hc->csp_out);
        *hc->csp_out = header_value_dup(buffer, bytes, 24);
    } else if (bytes >= 28 &&
               g_ascii_strncasecmp(buffer, "Access-Control-Allow-Origin:", 28) == 0 &&
               hc->cors_allow_origin_out) {
        g_free(*hc->cors_allow_origin_out);
        *hc->cors_allow_origin_out = header_value_dup(buffer, bytes, 28);
    }
    return bytes;
}

static const char k_about_nordstjernen[] =
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
    "<p>Distributed as shareware in the spirit of how Opera Software "
    "shipped its browser in the early years: free to download and use, "
    "with a polite nag asking the user to buy a license. The binary "
    "keeps working either way.</p>"
    "<p>Developed by Andreas Røsdal, with extensive use of AI tooling. "
    "Copyright 2026.</p>"
    "<p>Project home: <a href=\"https://nordstjernen.org\">nordstjernen.org</a>. "
    "Source code: <a href=\"https://github.com/operativsystem42/nordstjernen\">"
    "github.com/operativsystem42/nordstjernen</a>.</p>"
    "</body></html>";

static gboolean
synthesize_data_response(const char *url, nd_response *resp)
{
    if (!g_str_has_prefix(url, "data:")) return FALSE;
    const char *p = url + 5;
    const char *comma = strchr(p, ',');
    if (!comma) return FALSE;
    char *meta = g_strndup(p, (gsize)(comma - p));
    gboolean base64 = FALSE;
    char *semi = strstr(meta, ";base64");
    if (semi) { *semi = '\0'; base64 = TRUE; }
    char *ct = (*meta) ? g_strdup(meta) : g_strdup("text/plain;charset=US-ASCII");
    g_free(meta);

    const char *data = comma + 1;
    if (base64) {
        gsize out_len = 0;
        guchar *raw = g_base64_decode(data, &out_len);
        if (raw && out_len > 0)
            g_byte_array_append(resp->body, raw, (guint)out_len);
        g_free(raw);
    } else {
        char *decoded = g_uri_unescape_string(data, NULL);
        if (decoded) {
            g_byte_array_append(resp->body, (const guint8 *)decoded,
                                (guint)strlen(decoded));
            g_free(decoded);
        }
    }
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = ct;
    return TRUE;
}

static gboolean
synthesize_about_response(const char *url, nd_response *resp)
{
    if (!g_str_has_prefix(url, "about:")) return FALSE;
    const char *what = url + strlen("about:");
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = g_strdup("text/html; charset=utf-8");
    const char *body = NULL;
    if (g_str_equal(what, "blank") || g_str_equal(what, "")) {
        body = "<!doctype html><title>Blank</title>";
    } else {
        body = k_about_nordstjernen;
    }
    g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
    return TRUE;
}

static gboolean
is_simple_get(const char *method)
{
    return !method || !*method || g_ascii_strcasecmp(method, "GET") == 0;
}

static nd_response *
response_from_cache_entry(nd_cache_entry *e)
{
    nd_response *resp = g_new0(nd_response, 1);
    resp->status       = e->status;
    resp->final_url    = g_strdup(e->final_url);
    resp->content_type = g_strdup(e->content_type);
    resp->body         = e->body;
    e->body = NULL;
    return resp;
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
    if (synthesize_data_response(url, resp))
        return resp;

    nd_cache_entry *cached = NULL;
    if (is_simple_get(method)) {
        cached = nd_cache_get(url);
        if (cached && nd_cache_is_fresh(cached)) {
            nd_response_free(resp);
            nd_response *from_cache = response_from_cache_entry(cached);
            nd_cache_entry_free(cached);
            return from_cache;
        }
    }

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
    const nd_config *cfg = nd_config_get();
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        (cfg && cfg->user_agent && *cfg->user_agent) ? cfg->user_agent
                                                     : ND_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    switch (cfg ? cfg->referer_policy : ND_REFERER_STRICT_ORIGIN_WHEN_CROSS) {
    case ND_REFERER_NO_REFERRER:
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 0L);
        curl_easy_setopt(curl, CURLOPT_REFERER, "");
        break;
    case ND_REFERER_UNSAFE_URL:
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);
        break;
    default:
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);
        break;
    }

    struct curl_slist *headers = NULL;
    {
        const char *al = (cfg && cfg->accept_language && *cfg->accept_language)
                          ? cfg->accept_language : "en-US,en;q=0.9";
        char *h = g_strdup_printf("Accept-Language: %s", al);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }
    headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,"
                                          "application/xml;q=0.9,image/avif,image/webp,"
                                          "image/png,image/*;q=0.8,*/*;q=0.5");
    if (!cfg || cfg->do_not_track)
        headers = curl_slist_append(headers, "DNT: 1");

    if (cached && cached->etag) {
        char *h = g_strdup_printf("If-None-Match: %s", cached->etag);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }
    if (cached && cached->last_modified) {
        char *h = g_strdup_printf("If-Modified-Since: %s", cached->last_modified);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }

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
    if (g_ca_bundle)
        curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_bundle);
#ifdef G_OS_WIN32
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif

    const char *cookie_path = nd_net_cookie_path();
    if (cookie_path) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_path);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR,  cookie_path);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nd_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp->body);
    nd_header_ctx header_ctx = {0};
    header_ctx.content_type_out = &resp->content_type;
    header_ctx.csp_out          = &resp->csp_header;
    header_ctx.cors_allow_origin_out = &resp->cors_allow_origin;
    header_ctx.sts_host = nd_url_host_from(url);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, nd_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_ctx);

    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    const char *altsvc = nd_net_altsvc_path();
    if (altsvc)
        curl_easy_setopt(curl, CURLOPT_ALTSVC, altsvc);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, nd_xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancellable);

    CURLcode rc = curl_easy_perform(curl);

    if ((rc == CURLE_PEER_FAILED_VERIFICATION ||
         rc == CURLE_SSL_CACERT ||
         rc == CURLE_SSL_CACERT_BADFILE) &&
        g_str_has_prefix(url, "https://")) {
        char *warn = g_strdup_printf(
            "Insecure: TLS certificate not trusted (%s)",
            errbuf[0] ? errbuf : curl_easy_strerror(rc));
        g_byte_array_set_size(resp->body, 0);
        errbuf[0] = '\0';
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        rc = curl_easy_perform(curl);
        if (rc == CURLE_OK)
            resp->tls_warning = warn;
        else
            g_free(warn);
    }

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
            g_free(header_ctx.etag);
            g_free(header_ctx.last_modified);
            g_free(header_ctx.cache_control);
            g_free(header_ctx.expires);
            nd_cache_entry_free(cached);
            nd_response_free(resp);
            return NULL;
        }
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        resp->error = g_strdup(msg);
    }

    if (rc == CURLE_OK && is_simple_get(method)) {
        if (resp->status == 304 && cached) {
            nd_cache_promote_304(url, header_ctx.cache_control, header_ctx.expires);
            g_byte_array_set_size(resp->body, 0);
            g_byte_array_append(resp->body, cached->body->data, cached->body->len);
            resp->status = cached->status;
            g_free(resp->content_type);
            resp->content_type = g_strdup(cached->content_type);
        } else if (resp->status > 0 && resp->body && resp->body->len > 0) {
            nd_cache_put(url, resp->final_url, resp->status,
                         resp->content_type,
                         header_ctx.etag, header_ctx.last_modified,
                         header_ctx.cache_control, header_ctx.expires,
                         resp->body->data, resp->body->len);
        }
    }

    g_free(header_ctx.etag);
    g_free(header_ctx.last_modified);
    g_free(header_ctx.cache_control);
    g_free(header_ctx.expires);
    nd_cache_entry_free(cached);

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
