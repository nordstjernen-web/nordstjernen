/* Nordstjernen — libcurl-backed async fetcher.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "net.h"
#include "cache.h"
#include "compatibility.h"
#include "config.h"
#include "csp.h"
#include "env.h"
#include "image.h"
#include "video.h"
#include "hsts_preload.h"

#include <curl/curl.h>
#include <time.h>
#include <string.h>

#include <glib/gstdio.h>

#include <lexbor/url/url.h>

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#endif

static char *g_cookie_dir;
static char *g_hsts_path;
static char *g_hsts_curl_path;
static char *g_altsvc_path;
static GHashTable *g_hsts_table;
static GMutex g_hsts_lock;
static char *g_ca_bundle;
static gboolean g_has_http3;
static CURLSH *g_share;
static GMutex g_share_locks[CURL_LOCK_DATA_LAST];

#define ND_NET_MAX_PER_ORIGIN 6

typedef struct nd_origin_slot {
    int   in_use;
    GCond cond;
} nd_origin_slot;

static GMutex      g_origin_slots_lock;
static GHashTable *g_origin_slots;

static void
nd_origin_slot_free(gpointer p)
{
    nd_origin_slot *s = p;
    g_cond_clear(&s->cond);
    g_free(s);
}

static gboolean
nd_net_acquire_origin_slot(const char *origin, GCancellable *cancellable)
{
    if (!origin || !*origin) return FALSE;
    char *key = g_ascii_strdown(origin, -1);
    g_mutex_lock(&g_origin_slots_lock);
    if (!g_origin_slots)
        g_origin_slots = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, nd_origin_slot_free);
    nd_origin_slot *s = g_hash_table_lookup(g_origin_slots, key);
    if (!s) {
        s = g_new0(nd_origin_slot, 1);
        g_cond_init(&s->cond);
        g_hash_table_insert(g_origin_slots, key, s);
        key = NULL;
    }
    while (s->in_use >= ND_NET_MAX_PER_ORIGIN) {
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            g_mutex_unlock(&g_origin_slots_lock);
            g_free(key);
            return FALSE;
        }
        gint64 wakeup = g_get_monotonic_time() + 250 * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&s->cond, &g_origin_slots_lock, wakeup);
    }
    s->in_use++;
    g_mutex_unlock(&g_origin_slots_lock);
    g_free(key);
    return TRUE;
}

static void
nd_net_release_origin_slot(const char *origin)
{
    if (!origin || !*origin) return;
    char *key = g_ascii_strdown(origin, -1);
    g_mutex_lock(&g_origin_slots_lock);
    if (g_origin_slots) {
        nd_origin_slot *s = g_hash_table_lookup(g_origin_slots, key);
        if (s && s->in_use > 0) {
            s->in_use--;
            g_cond_signal(&s->cond);
        }
    }
    g_mutex_unlock(&g_origin_slots_lock);
    g_free(key);
}

typedef struct nd_hsts_entry {
    gint64    expiry;
    gboolean  include_subdomains;
} nd_hsts_entry;

static char *
nd_net_data_path(char **slot, const char *basename)
{
    if (*slot) return *slot;
    char *dir = g_build_filename(g_get_user_data_dir(), ND_APP_DIR_NAME, NULL);
    g_mkdir_with_parents(dir, 0700);
    *slot = g_build_filename(dir, basename, NULL);
    g_free(dir);
    return *slot;
}

static char *
nd_net_hsts_path(void) { return nd_net_data_path(&g_hsts_path, "hsts.txt"); }

static char *
nd_net_hsts_curl_path(void) { return nd_net_data_path(&g_hsts_curl_path, "hsts-curl.txt"); }

static void
nd_hsts_format_expiry(gint64 unix_seconds, char out[24])
{
    time_t t = (time_t)unix_seconds;
    struct tm tm;
#ifdef G_OS_WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    g_snprintf(out, 24, "%04d%02d%02d %02d:%02d:%02d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void
nd_hsts_write_curl_file_locked(void)
{
    char *path = nd_net_hsts_curl_path();
    GString *out = g_string_new(
        "# nordstjernen HSTS cache for libcurl (regenerated at startup).\n");

    gint64 preload_expiry = 4102444800LL;
    char buf[24];
    nd_hsts_format_expiry(preload_expiry, buf);
    for (gsize i = 0; i < ND_HSTS_PRELOAD_COUNT; i++) {
        const nd_hsts_preload_entry *p = &nd_hsts_preload[i];
        g_string_append_printf(out, "%s%s \"%s\"\n",
            p->include_subdomains ? "." : "",
            p->host, buf);
    }

    if (g_hsts_table) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, g_hsts_table);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            const char *host = k;
            const nd_hsts_entry *e = v;
            if (e->expiry <= 0) continue;
            nd_hsts_format_expiry(e->expiry, buf);
            g_string_append_printf(out, "%s%s \"%s\"\n",
                e->include_subdomains ? "." : "",
                host, buf);
        }
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
nd_hsts_preload_seed_table_locked(void)
{
    if (!g_hsts_table) return;
    gint64 preload_expiry = 4102444800LL;
    for (gsize i = 0; i < ND_HSTS_PRELOAD_COUNT; i++) {
        const nd_hsts_preload_entry *p = &nd_hsts_preload[i];
        char *lower = g_ascii_strdown(p->host, -1);
        nd_hsts_entry *existing = g_hash_table_lookup(g_hsts_table, lower);
        if (existing && existing->expiry >= preload_expiry) {
            g_free(lower);
            continue;
        }
        nd_hsts_entry *e = g_new0(nd_hsts_entry, 1);
        e->expiry = preload_expiry;
        e->include_subdomains = p->include_subdomains != 0;
        g_hash_table_replace(g_hsts_table, lower, e);
    }
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
            gint64 subs   = g_ascii_strtoll(fields[2], NULL, 10);
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
    g_mutex_lock(&g_hsts_lock);
    nd_hsts_table_init();
    nd_hsts_entry *e = g_new0(nd_hsts_entry, 1);
    e->expiry = g_get_real_time() / G_USEC_PER_SEC + max_age;
    e->include_subdomains = include_subs;
    char *lower = g_ascii_strdown(host, -1);
    g_hash_table_replace(g_hsts_table, lower, e);
    nd_hsts_table_save();
    nd_hsts_write_curl_file_locked();
    g_mutex_unlock(&g_hsts_lock);
}

gboolean
nd_url_is_http_or_https(const char *url)
{
    return url && (g_str_has_prefix(url, "http://") ||
                   g_str_has_prefix(url, "https://"));
}

static char *
build_accept_language_from_locales(void)
{
    const char *const *langs = g_get_language_names();
    if (!langs || !langs[0]) return NULL;
    GString *out = g_string_new(NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    int n = 0;
    for (int i = 0; langs[i] && n < 6; i++) {
        char *tag = g_strdup(langs[i]);
        char *dot = strchr(tag, '.');
        if (dot) *dot = '\0';
        char *at = strchr(tag, '@');
        if (at) *at = '\0';
        for (char *p = tag; *p; p++) if (*p == '_') *p = '-';
        if (!*tag ||
            g_ascii_strcasecmp(tag, "C") == 0 ||
            g_ascii_strcasecmp(tag, "POSIX") == 0) {
            g_free(tag); continue;
        }
        char *lower = g_ascii_strdown(tag, -1);
        if (g_hash_table_contains(seen, lower)) {
            g_free(tag); g_free(lower); continue;
        }
        g_hash_table_insert(seen, lower, NULL);
        if (n == 0) {
            g_string_append(out, tag);
        } else {
            double q = 1.0 - (double)n * 0.1;
            if (q < 0.1) q = 0.1;
            g_string_append_printf(out, ",%s;q=%.1f", tag, q);
        }
        n++;
        g_free(tag);
    }
    g_hash_table_destroy(seen);
    if (n == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }
    return g_string_free(out, FALSE);
}

const char *
nd_net_default_accept_language(void)
{
    static char *cached;
    static gboolean tried;
    if (!tried) {
        tried = TRUE;
        cached = build_accept_language_from_locales();
        if (!cached) cached = g_strdup("en-US,en;q=0.9");
    }
    return cached;
}

static lxb_status_t
nd_url_str_append_cb(const lxb_char_t *data, size_t length, void *ctx)
{
    g_string_append_len((GString *)ctx, (const char *)data, (gssize)length);
    return LXB_STATUS_OK;
}

static void
nd_url_parser_destroy_tls(gpointer p)
{
    lxb_url_parser_t *parser = p;
    if (!parser) return;
    lxb_url_parser_memory_destroy(parser);
    lxb_url_parser_destroy(parser, true);
}

static GPrivate g_url_parser_tls = G_PRIVATE_INIT(nd_url_parser_destroy_tls);

static lxb_url_parser_t *
nd_url_parser_open(void)
{
    lxb_url_parser_t *parser = g_private_get(&g_url_parser_tls);
    if (parser) {
        lxb_url_parser_clean(parser);
        return parser;
    }
    parser = lxb_url_parser_create();
    if (!parser) return NULL;
    if (lxb_url_parser_init(parser, NULL) != LXB_STATUS_OK) {
        lxb_url_parser_destroy(parser, true);
        return NULL;
    }
    g_private_set(&g_url_parser_tls, parser);
    return parser;
}

static void
nd_url_parser_close(lxb_url_parser_t *parser)
{
    if (!parser) return;
    lxb_url_parser_clean(parser);
}

char *
nd_url_resolve(const char *base, const char *href)
{
    if (!href || !*href) return NULL;
    if (g_str_has_prefix(href, "data:") || g_str_has_prefix(href, "about:"))
        return g_strdup(href);

    lxb_url_parser_t *parser = nd_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *base_url = NULL;
    if (base && *base) {
        base_url = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)base, strlen(base));
        lxb_url_parser_clean(parser);
    }
    lxb_url_t *resolved = lxb_url_parse(parser, base_url,
                                        (const lxb_char_t *)href, strlen(href));
    char *out = NULL;
    if (resolved) {
        GString *s = g_string_new(NULL);
        if (lxb_url_serialize(resolved, nd_url_str_append_cb, s, false)
            == LXB_STATUS_OK && s->len > 0)
            out = g_string_free(s, FALSE);
        else
            g_string_free(s, TRUE);
    }
    nd_url_parser_close(parser);
    return out;
}

static lxb_url_t *
nd_url_parse_with_host(lxb_url_parser_t *parser, const char *url)
{
    lxb_url_t *u = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)url, strlen(url));
    if (!u) return NULL;
    if (u->host.type == LXB_URL_HOST_TYPE__UNDEF ||
        u->host.type == LXB_URL_HOST_TYPE_EMPTY)
        return NULL;
    return u;
}

char *
nd_url_origin_from(const char *url)
{
    if (!url || !*url) return NULL;
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        return NULL;

    lxb_url_parser_t *parser = nd_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = nd_url_parse_with_host(parser, url);
    char *out = NULL;
    if (u) {
        GString *s = g_string_new(NULL);
        g_string_append_len(s, (const char *)u->scheme.name.data,
                            (gssize)u->scheme.name.length);
        g_string_append(s, "://");
        if (lxb_url_serialize_host(&u->host, nd_url_str_append_cb, s)
            == LXB_STATUS_OK) {
            if (u->has_port)
                g_string_append_printf(s, ":%u", (unsigned)u->port);
            out = g_string_free(s, FALSE);
        } else {
            g_string_free(s, TRUE);
        }
    }
    nd_url_parser_close(parser);
    return out;
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

gboolean
nd_url_is_same_site(const char *a, const char *b)
{
    if (!a || !b) return FALSE;
    char *ha = nd_url_host_from(a);
    char *hb = nd_url_host_from(b);
    gboolean same = FALSE;
    if (ha && hb) {
        if (g_ascii_strcasecmp(ha, hb) == 0) {
            same = TRUE;
        } else {
            gsize la = strlen(ha), lb = strlen(hb);
            if (la > lb + 1 && ha[la - lb - 1] == '.' &&
                g_ascii_strncasecmp(ha + la - lb, hb, lb) == 0)
                same = TRUE;
            else if (lb > la + 1 && hb[lb - la - 1] == '.' &&
                     g_ascii_strncasecmp(hb + lb - la, ha, la) == 0)
                same = TRUE;
        }
    }
    g_free(ha);
    g_free(hb);
    return same;
}

static gboolean
xfo_token_is(const char *value, const char *want)
{
    if (!value) return FALSE;
    while (*value == ' ' || *value == '\t') value++;
    const char *end = value;
    while (*end && *end != ' ' && *end != '\t' && *end != ',' && *end != ';')
        end++;
    gsize len = (gsize)(end - value);
    gsize wl  = strlen(want);
    return len == wl && g_ascii_strncasecmp(value, want, wl) == 0;
}

gboolean
nd_response_allows_framing(const char *xframe_options,
                           const char *csp_header,
                           const char *parent_url,
                           const char *document_url)
{
    if (!parent_url) return TRUE;

    if (csp_header && *csp_header) {
        nd_csp *csp = nd_csp_parse(csp_header);
        if (csp && nd_csp_has_frame_ancestors(csp)) {
            gboolean allowed =
                nd_csp_frame_ancestors_allows(csp, parent_url, document_url);
            nd_csp_free(csp);
            return allowed;
        }
        nd_csp_free(csp);
    }

    if (xframe_options && *xframe_options) {
        if (xfo_token_is(xframe_options, "DENY")) return FALSE;
        if (xfo_token_is(xframe_options, "SAMEORIGIN"))
            return nd_url_same_origin(parent_url, document_url);
    }
    return TRUE;
}

char *
nd_url_host_from(const char *url)
{
    if (!url) return NULL;

    lxb_url_parser_t *parser = nd_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = nd_url_parse_with_host(parser, url);
    char *out = NULL;
    if (u) {
        GString *s = g_string_new(NULL);
        if (lxb_url_serialize_host(&u->host, nd_url_str_append_cb, s)
            == LXB_STATUS_OK && s->len > 0)
            out = g_string_free(s, FALSE);
        else
            g_string_free(s, TRUE);
    }
    nd_url_parser_close(parser);
    return out;
}

gboolean
nd_net_hsts_should_upgrade(const char *host)
{
    if (!host || !*host) return FALSE;
    g_mutex_lock(&g_hsts_lock);
    nd_hsts_table_init();
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    char *lower = g_ascii_strdown(host, -1);
    nd_hsts_entry *e = g_hash_table_lookup(g_hsts_table, lower);
    if (e && e->expiry > now) {
        g_free(lower);
        g_mutex_unlock(&g_hsts_lock);
        return TRUE;
    }
    const char *dot = lower;
    while ((dot = strchr(dot, '.')) != NULL) {
        const char *parent = dot + 1;
        e = g_hash_table_lookup(g_hsts_table, parent);
        if (e && e->expiry > now && e->include_subdomains) {
            g_free(lower);
            g_mutex_unlock(&g_hsts_lock);
            return TRUE;
        }
        dot = parent;
    }
    g_free(lower);
    g_mutex_unlock(&g_hsts_lock);
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
nd_net_cookie_dir(void)
{
    if (g_cookie_dir) return g_cookie_dir;
    const char *config = g_get_user_config_dir();
    g_cookie_dir = g_build_filename(config, ND_APP_DIR_NAME, "cookies", NULL);
    g_mkdir_with_parents(g_cookie_dir, 0700);
    return g_cookie_dir;
}

static char *
nd_net_cookie_path_for_partition(const char *top_origin)
{
    const char *dir = nd_net_cookie_dir();
    const char *key = (top_origin && *top_origin) ? top_origin : "default";
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
    char short_hex[33];
    g_strlcpy(short_hex, digest, sizeof(short_hex));
    g_free(digest);
    char *fname = g_strdup_printf("%s.txt", short_hex);
    char *path = g_build_filename(dir, fname, NULL);
    g_free(fname);
    return path;
}

void
nd_net_clear_cookies(void)
{
    const char *dir = nd_net_cookie_dir();
    if (!dir) return;
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd) return;
    const char *name;
    while ((name = g_dir_read_name(gd))) {
        if (!g_str_has_suffix(name, ".txt")) continue;
        char *full = g_build_filename(dir, name, NULL);
        g_unlink(full);
        g_free(full);
    }
    g_dir_close(gd);
}

static const char *
nd_net_altsvc_path(void)
{
    return nd_net_data_path(&g_altsvc_path, "altsvc.txt");
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
    DWORD cap = MAX_PATH;
    wchar_t *buf = g_new(wchar_t, cap);
    DWORD n = GetModuleFileNameW(NULL, buf, cap);
    while (n >= cap && cap < 32768) {
        cap *= 2;
        wchar_t *bigger = g_renew(wchar_t, buf, cap);
        buf = bigger;
        n = GetModuleFileNameW(NULL, buf, cap);
    }
    char *utf8 = NULL;
    if (n > 0 && n < cap)
        utf8 = g_utf16_to_utf8((gunichar2 *)buf, -1, NULL, NULL, NULL);
    g_free(buf);
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

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    const char *unix_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/var/lib/ca-certificates/ca-bundle.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/cert.pem",
        "/usr/local/share/certs/ca-root-nss.crt",
        NULL,
    };
    for (int i = 0; unix_paths[i]; i++)
        if (nd_net_try_ca_bundle(unix_paths[i])) return;
#endif

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

#ifdef G_OS_WIN32
    const char *win_paths[] = {
        "C:/msys64/mingw64/etc/ssl/certs/ca-bundle.crt",
        "C:/msys64/mingw64/etc/ssl/cert.pem",
        "C:/msys64/ucrt64/etc/ssl/certs/ca-bundle.crt",
        "C:/msys64/clang64/etc/ssl/certs/ca-bundle.crt",
        NULL,
    };
    for (int i = 0; win_paths[i]; i++)
        if (nd_net_try_ca_bundle(win_paths[i])) return;

    g_info("nd_net: no CA bundle file found; relying on "
           "CURLSSLOPT_NATIVE_CA via the Windows certificate store. "
           "If HTTPS fails, install mingw-w64-x86_64-ca-certificates or "
           "set CURL_CA_BUNDLE.");
#endif
}

static void
nd_share_lock(CURL *handle, curl_lock_data data,
              curl_lock_access access, void *user_data)
{
    (void)handle; (void)access; (void)user_data;
    if (data < CURL_LOCK_DATA_LAST)
        g_mutex_lock(&g_share_locks[data]);
}

static void
nd_share_unlock(CURL *handle, curl_lock_data data, void *user_data)
{
    (void)handle; (void)user_data;
    if (data < CURL_LOCK_DATA_LAST)
        g_mutex_unlock(&g_share_locks[data]);
}

void
nd_net_init(void)
{
    nd_net_resolve_ca_bundle();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
    g_has_http3 = vi && (vi->features & CURL_VERSION_HTTP3) != 0;

    g_mutex_lock(&g_hsts_lock);
    nd_hsts_table_init();
    nd_hsts_preload_seed_table_locked();
    nd_hsts_write_curl_file_locked();
    g_mutex_unlock(&g_hsts_lock);

    g_share = curl_share_init();
    if (g_share) {
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
#ifdef CURL_LOCK_DATA_PSL
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_PSL);
#endif
#ifdef CURL_LOCK_DATA_HSTS
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_HSTS);
#endif
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC,   nd_share_lock);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, nd_share_unlock);
    }
}

void
nd_net_shutdown(void)
{
    nd_hsts_table_save();
    if (g_share) { curl_share_cleanup(g_share); g_share = NULL; }
    curl_global_cleanup();
    g_free(g_cookie_dir);
    g_cookie_dir = NULL;
    g_free(g_hsts_path);
    g_hsts_path = NULL;
    g_free(g_hsts_curl_path);
    g_hsts_curl_path = NULL;
    g_free(g_altsvc_path);
    g_altsvc_path = NULL;
    g_free(g_ca_bundle);
    g_ca_bundle = NULL;
    if (g_hsts_table) {
        g_hash_table_destroy(g_hsts_table);
        g_hsts_table = NULL;
    }
    if (g_origin_slots) {
        g_hash_table_destroy(g_origin_slots);
        g_origin_slots = NULL;
    }
}

void
nd_response_free(nd_response *resp)
{
    if (!resp)
        return;
    g_free(resp->final_url);
    g_free(resp->content_type);
    g_free(resp->content_disposition);
    g_free(resp->csp_header);
    g_free(resp->xframe_options);
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
    char **content_disposition_out;
    char **csp_out;
    char **xframe_options_out;
    char **cors_allow_origin_out;
    char  *sts_host;
    gint64 sts_max_age;
    gboolean sts_include_subs;
    gboolean sts_seen;
    char  *etag;
    char  *last_modified;
    char  *cache_control;
    char  *expires;
    gboolean set_cookie_seen;
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

static gboolean
header_capture(const char *buffer, size_t bytes,
               const char *name, char **slot)
{
    size_t name_len = strlen(name);
    if (bytes < name_len ||
        g_ascii_strncasecmp(buffer, name, name_len) != 0)
        return FALSE;
    if (slot) {
        g_free(*slot);
        *slot = header_value_dup(buffer, bytes, name_len);
    }
    return TRUE;
}

static void
header_parse_sts(const char *buffer, size_t bytes, nd_header_ctx *hc)
{
    char *line = header_value_dup(buffer, bytes,
                                  strlen("Strict-Transport-Security:"));
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

static size_t
nd_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    nd_header_ctx *hc = userdata;
    size_t bytes = size * nitems;

    if      (header_capture(buffer, bytes, "Content-Type:",    hc->content_type_out))         {}
    else if (header_capture(buffer, bytes, "ETag:",            &hc->etag))                    {}
    else if (header_capture(buffer, bytes, "Last-Modified:",   &hc->last_modified))           {}
    else if (header_capture(buffer, bytes, "Cache-Control:",   &hc->cache_control))           {}
    else if (header_capture(buffer, bytes, "Expires:",         &hc->expires))                 {}
    else if (header_capture(buffer, bytes, "Content-Security-Policy:",
                            hc->csp_out))                                                     {}
    else if (header_capture(buffer, bytes, "X-Frame-Options:", hc->xframe_options_out))       {}
    else if (header_capture(buffer, bytes, "Access-Control-Allow-Origin:",
                            hc->cors_allow_origin_out))                                       {}
    else if (header_capture(buffer, bytes, "Content-Disposition:",
                            hc->content_disposition_out))                                     {}
    else if (header_capture(buffer, bytes, "Set-Cookie:", NULL))
        hc->set_cookie_seen = TRUE;
    else if (bytes >= strlen("Strict-Transport-Security:") &&
             g_ascii_strncasecmp(buffer, "Strict-Transport-Security:",
                                 strlen("Strict-Transport-Security:")) == 0)
        header_parse_sts(buffer, bytes, hc);

    return bytes;
}

static const char k_about_nordstjernen_prefix[] =
    "<!doctype html><html><head><title>About Nordstjernen</title>"
    "<style>"
    ".poem{font-style:italic;text-align:center;color:#444;"
    "margin:1.5em 0 2em 0}"
    "table.env{border-collapse:collapse;margin:0.5em 0}"
    "table.env th{text-align:left;font-weight:normal;color:#555;"
    "padding:0.1em 1em 0.1em 0;white-space:nowrap}"
    "table.env td{font-family:monospace}"
    "</style></head>"
    "<body>"
    "<p style=\"text-align:center\">"
    "<img alt=\"Nordstjernen\" width=\"96\" height=\"96\" "
    "src=\"data:image/svg+xml;utf8,"
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 96 96'>"
    "<rect width='96' height='96' fill='%23000026'/>"
    "<polygon points='48,8 52,44 88,48 52,52 48,88 44,52 8,48 44,44' "
    "fill='%23ffffff'/></svg>\">"
    "</p>"
    "<h1 style=\"text-align:center\">Nordstjernen " ND_VERSION "</h1>"
    "<p class=\"poem\">"
    "A north star, small and faithful — light enough to read by,<br>"
    "slow enough to think with; built one line at a time."
    "</p>"
    "<p>Nordstjernen is a clean-room web browser for people who want "
    "to read the web — Wikipedia, news, documentation, search "
    "results, light forms — without donating a few hundred million "
    "lines of someone else's engine and a few hundred kilobytes of "
    "telemetry to the experience. It is around 27,000 lines of C, "
    "small enough that a single human can read the whole thing in a "
    "long afternoon. It uses GTK 4 for the user interface and "
    "libcurl for networking; the HTML / CSS parsers, layout, paint, "
    "and JavaScript bindings are written from scratch in this "
    "repository — there is no Blink, no Gecko, no WebKit, no JIT.</p>"
    "<p>What you get in return for the smaller feature set: "
    "auditability (one person can read the engine), privacy by "
    "construction (no telemetry, no update pinger, no crash "
    "reporter, no \"studies,\" DuckDuckGo Lite as the default "
    "search), and a much smaller attack surface (no WebGL, no "
    "WebGPU, no service workers, no MSE, no extensions, no plugins, "
    "no DRM). The JavaScript engine is QuickJS — an interpreter, "
    "so the most prolific category of in-the-wild browser RCEs "
    "(V8 / SpiderMonkey JIT bugs) is foreclosed entirely.</p>"
    "<p>Network defaults are conservative: TLS verification on, "
    "HTTP / HTTPS only, built-in HSTS preload list, libcurl-enforced "
    "HSTS across the whole redirect chain, max 10 redirects per "
    "request, 60 seconds per HTTP request, 60 seconds per JS "
    "execution slice. The HTTP cache and connections are "
    "partitioned by top-level site, so the same CDN URL cannot "
    "track you across sites via cache timing.</p>"
    "<p>On Linux the renderer runs under a Landlock sandbox that "
    "removes read access to <code>~/.ssh</code>, <code>~/.gnupg</code>, "
    "other browsers' profiles, and arbitrary dotfiles; only the "
    "nordstjernen-specific subdirs under <code>$XDG_*_HOME</code> "
    "are writable. The browser refuses to run as root on Linux / "
    "macOS or as Administrator on Windows. The binary is compiled "
    "with PIE, full RELRO, stack-protector-strong, stack-clash "
    "protection, Intel CET, <code>_FORTIFY_SOURCE=2</code>, and "
    "a no-exec stack.</p>"
    "<p>This is not a Chromium-grade adversary-resistant browser "
    "and it does not pretend to be. It will not match Chrome or "
    "Firefox feature-for-feature; the threat model is "
    "<em>sloppy or mildly hostile websites running on top of a "
    "trusted operating-system user</em>, and the design is kept "
    "small enough to defend honestly.</p>"
    "<p>Source-available under the "
    "<a href=\"https://fsl.software/\">Functional Source License v1.1</a> "
    "(<code>FSL-1.1-MIT</code>): free for any non-competing use, and each "
    "release converts to the MIT license ten years after publication. "
    "See <code>LICENSE</code> in the source tree for the full text.</p>"
    "<p>Developed by Andreas Røsdal, with extensive use of AI tooling. "
    "Copyright 2026.</p>"
    "<p>Project home: <a href=\"https://nordstjernen.org\">nordstjernen.org</a>. "
    "Source code: <a href=\"https://github.com/operativsystem42/nordstjernen\">"
    "github.com/operativsystem42/nordstjernen</a>.</p>"
    "<h2>Environment</h2>"
    "<table class=\"env\">";

static const char k_about_nordstjernen_credits[] =
    "</table>"
    "<h2>Credits and third-party software</h2>"
    "<p>Nordstjernen is built on top of these libraries. Their "
    "copyright notices and license texts are reproduced in "
    "<code>THIRD-PARTY-LICENSES.md</code> shipped with every binary "
    "release; per the LGPL terms below, you are entitled to replace "
    "the dynamically-linked libraries with modified versions.</p>"
    "<table class=\"env\">"
    "<tr><th>lexbor</th><td>HTML / CSS / WHATWG URL parser — "
    "Apache-2.0 — <a href=\"https://github.com/lexbor/lexbor\">"
    "github.com/lexbor/lexbor</a></td></tr>"
    "<tr><th>Wuffs</th><td>memory-safe PNG / GIF / BMP / JPEG decoders — "
    "Apache-2.0 — <a href=\"https://github.com/google/wuffs\">"
    "github.com/google/wuffs</a></td></tr>"
    "<tr><th>quickjs-ng</th><td>JavaScript engine — MIT — "
    "<a href=\"https://github.com/quickjs-ng/quickjs\">"
    "github.com/quickjs-ng/quickjs</a></td></tr>"
    "<tr><th>libcurl</th><td>HTTP/TLS client — curl license "
    "(MIT-like) — <a href=\"https://curl.se\">curl.se</a></td></tr>"
    "<tr><th>libuchardet</th><td>charset detection — "
    "MPL-1.1 / LGPL-2.1+ — "
    "<a href=\"https://www.freedesktop.org/wiki/Software/uchardet/\">"
    "freedesktop.org/wiki/Software/uchardet</a></td></tr>"
    "<tr><th>libvpx</th><td>VP8/VP9 decoder (optional) — "
    "BSD-3-Clause — <a href=\"https://github.com/webmproject/libvpx\">"
    "github.com/webmproject/libvpx</a></td></tr>"
    "<tr><th>GTK 4</th><td>UI toolkit — LGPL-2.1+ — "
    "<a href=\"https://www.gtk.org\">gtk.org</a></td></tr>"
    "<tr><th>GLib</th><td>core utilities — LGPL-2.1+ — "
    "<a href=\"https://gitlab.gnome.org/GNOME/glib\">"
    "gitlab.gnome.org/GNOME/glib</a></td></tr>"
    "<tr><th>Pango</th><td>text shaping — LGPL-2.0+ — "
    "<a href=\"https://gitlab.gnome.org/GNOME/pango\">"
    "gitlab.gnome.org/GNOME/pango</a></td></tr>"
    "<tr><th>Cairo</th><td>2D drawing — LGPL-2.1 / MPL-1.1 — "
    "<a href=\"https://www.cairographics.org\">cairographics.org</a></td></tr>"
    "<tr><th>gdk-pixbuf</th><td>image loaders — LGPL-2.1+ — "
    "<a href=\"https://gitlab.gnome.org/GNOME/gdk-pixbuf\">"
    "gitlab.gnome.org/GNOME/gdk-pixbuf</a></td></tr>"
    "<tr><th>librsvg</th><td>SVG renderer — LGPL-2.1+ — "
    "<a href=\"https://gitlab.gnome.org/GNOME/librsvg\">"
    "gitlab.gnome.org/GNOME/librsvg</a></td></tr>"
    "</table>";

static const char k_about_nordstjernen_suffix[] =
    "</table>"
    "<p id=\"js-status\" style=\"color:#888;font-size:0.9em;"
    "margin-top:2em\"></p>"
    "<script>\n"
    "const status = document.getElementById('js-status');\n"
    "const same = (document.getElementById('js-status') === status);\n"
    "const paragraphs = document.querySelectorAll('p').length;\n"
    "setTimeout(function () {\n"
    "  status.textContent =\n"
    "    'JS: ' + (same ? 'OK' : 'BAD') +\n"
    "    ' \\u2014 ' + paragraphs + ' paragraphs rendered.';\n"
    "}, 0);\n"
    "</script>"
    "</body></html>";

static void
about_emit_env_row(const char *label, const char *value, gpointer user_data)
{
    GString *s = user_data;
    char *esc_label = g_markup_escape_text(label, -1);
    char *esc_value = g_markup_escape_text(value, -1);
    g_string_append_printf(s, "<tr><th>%s</th><td>%s</td></tr>",
                           esc_label, esc_value);
    g_free(esc_label);
    g_free(esc_value);
}

static char *
build_about_nordstjernen(void)
{
    GString *s = g_string_sized_new(4096);
    g_string_append(s, k_about_nordstjernen_prefix);
    nd_env_each(about_emit_env_row, s);
    g_string_append(s, k_about_nordstjernen_credits);
    g_string_append(s, k_about_nordstjernen_suffix);
    return g_string_free(s, FALSE);
}

static gboolean
synthesize_data_response(const char *url, nd_response *resp)
{
    if (!url || !g_str_has_prefix(url, "data:")) return FALSE;
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
        if (raw && out_len > 0 && out_len <= G_MAXUINT)
            g_byte_array_append(resp->body, raw, (guint)out_len);
        g_free(raw);
    } else {
        char *decoded = g_uri_unescape_string(data, NULL);
        if (decoded) {
            gsize dlen = strlen(decoded);
            if (dlen <= G_MAXUINT)
                g_byte_array_append(resp->body, (const guint8 *)decoded,
                                    (guint)dlen);
            g_free(decoded);
        }
    }
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = ct;
    return TRUE;
}

static const char k_about_start_body[] =
    "<!doctype html><html><head>"
    "<meta charset=\"utf-8\">"
    "<title>Nordstjernen</title>"
    "<style>\n"
    "html, body { background:#ffffff; color:#111111;"
    " font-family: system-ui, -apple-system, \"Segoe UI\","
    " Helvetica, Arial, sans-serif; margin:0; padding:0; min-height:100%; }\n"
    ".wrap { max-width: 720px; margin: 9vh auto 0 auto;"
    " padding: 0 24px 48px 24px; text-align:center; }\n"
    ".logo { margin: 0 auto 1.0em auto; width: 96px; height: 96px;"
    " display:block; }\n"
    ".title { font-size: 2.2em; font-weight: 600; margin: 0.1em 0 0.1em 0;"
    " letter-spacing: 0.5px; color:#111111; }\n"
    ".tagline { color:#666666; font-style: italic; margin: 0 0 1.6em 0;"
    " font-size: 1.0em; }\n"
    "form.search { margin: 0 auto 2.6em auto; display:flex; gap: 18px;"
    " max-width: 560px; align-items: center; justify-content: center;"
    " font-size: 1.15em; }\n"
    "form.search input { border: 1px solid #cccccc; background:#f4f6fb;"
    " padding: 7px 14px; font-size: 1.0em; line-height: 1.4;"
    " border-radius: 6px; }\n"
    "form.search button { border: 1px solid #cccccc; background:#e6e6e6;"
    " padding: 7px 18px; font-weight: 600; font-size: 1.0em;"
    " line-height: 1.4; border-radius: 6px; }\n"
    ".tiles { display:grid; grid-template-columns: repeat(3, 1fr);"
    " gap: 12px; margin: 0 auto; max-width: 600px; }\n"
    ".tile { display:block; padding: 18px 10px; background:#ffffff;"
    " border:1px solid #dddddd; border-radius:8px;"
    " color:#111111; text-decoration:none; }\n"
    ".tile:hover { background:#f4f6fb; border-color:#3a63d0; color:#111111; }\n"
    ".tile .name { display:block; font-size:1.0em; font-weight:600;"
    " color:#111111; }\n"
    ".tile .host { display:block; color:#666666; font-size:0.85em;"
    " margin-top:3px; }\n"
    ".footer { color:#888888; font-size:0.85em; margin-top:3em; }\n"
    ".footer a { color:#3a63d0; }\n"
    "</style></head>"
    "<body>"
    "<div class=\"wrap\">"
    "<img class=\"logo\" alt=\"Nordstjernen\""
    " src=\"data:image/svg+xml;utf8,"
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 96 96'>"
    "<circle cx='48' cy='48' r='46' fill='%23ffffff'"
    " stroke='%23111111' stroke-width='2'/>"
    "<polygon points='48,10 52,44 86,48 52,52 48,86 44,52 10,48 44,44'"
    " fill='%23111111'/>"
    "<circle cx='48' cy='48' r='4' fill='%233a63d0'/>"
    "</svg>\">"
    "<div class=\"title\">Nordstjernen</div>"
    "<div class=\"tagline\">Read the web. Slowly. Honestly.</div>"
    "<form class=\"search\" action=\"https://html.duckduckgo.com/html/\""
    " method=\"get\">"
    "<input type=\"text\" name=\"q\" size=\"24\" autofocus"
    " placeholder=\"Search DuckDuckGo\">"
    "<button type=\"submit\">Search</button>"
    "</form>"
    "<div class=\"tiles\">"
    "<a class=\"tile\" href=\"https://en.wikipedia.org/wiki/Main_Page\">"
    "<span class=\"name\">Wikipedia</span>"
    "<span class=\"host\">en.wikipedia.org</span></a>"
    "<a class=\"tile\" href=\"https://lite.cnn.com\">"
    "<span class=\"name\">CNN Lite</span>"
    "<span class=\"host\">lite.cnn.com</span></a>"
    "<a class=\"tile\" href=\"https://news.ycombinator.com\">"
    "<span class=\"name\">Hacker News</span>"
    "<span class=\"host\">news.ycombinator.com</span></a>"
    "<a class=\"tile\" href=\"https://old.reddit.com\">"
    "<span class=\"name\">Reddit</span>"
    "<span class=\"host\">old.reddit.com</span></a>"
    "<a class=\"tile\" href=\"https://www.google.com\">"
    "<span class=\"name\">Google</span>"
    "<span class=\"host\">www.google.com</span></a>"
    "<a class=\"tile\" href=\"https://text.npr.org\">"
    "<span class=\"name\">NPR Text</span>"
    "<span class=\"host\">text.npr.org</span></a>"
    "<a class=\"tile\" href=\"https://www.bbc.com/news\">"
    "<span class=\"name\">BBC News</span>"
    "<span class=\"host\">www.bbc.com</span></a>"
    "<a class=\"tile\" href=\"https://arxiv.org\">"
    "<span class=\"name\">arXiv</span>"
    "<span class=\"host\">arxiv.org</span></a>"
    "<a class=\"tile\" href=\"https://github.com\">"
    "<span class=\"name\">GitHub</span>"
    "<span class=\"host\">github.com</span></a>"
    "</div>"
    "<p class=\"footer\">"
    "<a href=\"about:nordstjernen\">About Nordstjernen</a>"
    "</p>"
    "</div>"
    "</body></html>";

static gboolean
synthesize_about_response(const char *url, nd_response *resp)
{
    if (!g_str_has_prefix(url, "about:")) return FALSE;
    const char *what = url + strlen("about:");
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = g_strdup("text/html; charset=utf-8");
    if (g_str_equal(what, "blank") || g_str_equal(what, "")) {
        const char *body = "<!doctype html><title>Blank</title>";
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
    } else if (g_str_equal(what, "start") || g_str_equal(what, "home") ||
               g_str_equal(what, "newtab")) {
        g_byte_array_append(resp->body, (const guint8 *)k_about_start_body,
                            (guint)strlen(k_about_start_body));
    } else {
        char *body = build_about_nordstjernen();
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    }
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
nd_fetch_sync(const char *url, const char *top_url, const char *method,
              const void *body, gsize body_len, const char *content_type,
              GPtrArray *extra_headers,
              GCancellable *cancellable, GError **error)
{
    nd_response *resp = g_new0(nd_response, 1);
    resp->body = g_byte_array_new();

    if (synthesize_about_response(url, resp))
        return resp;
    if (synthesize_data_response(url, resp))
        return resp;

    char *hsts_upgraded = nd_net_hsts_upgrade(url);
    if (hsts_upgraded) url = hsts_upgraded;

    char *url_host = nd_url_host_from(url);
    gboolean yt_host = nd_youtube_host_needs_browser_ua(url_host);
    const nd_config *cfg = nd_config_get();
    const char *configured_ua =
        (cfg && cfg->user_agent && *cfg->user_agent) ? cfg->user_agent
                                                     : ND_USER_AGENT;
    const char *compat_ua = nd_compat_user_agent_for_host(url_host);
    const char *effective_ua = yt_host ? nd_youtube_browser_user_agent()
                              : compat_ua ? compat_ua
                              : configured_ua;
    const char *accept_language =
        (cfg && cfg->accept_language && *cfg->accept_language)
            ? cfg->accept_language : nd_net_default_accept_language();
    const char *effective_top_url = top_url ? top_url : url;
    char *top_origin = nd_url_origin_from(effective_top_url);
    char *cache_partition = g_strdup_printf("top=%s|ua=%s|al=%s",
                                            top_origin ? top_origin : "",
                                            effective_ua, accept_language);
    nd_cookie_policy cookie_policy = cfg ? cfg->cookie_policy : ND_COOKIE_FIRST_PARTY;
    gboolean cookies_allowed = (cookie_policy != ND_COOKIE_NEVER);
    if (cookies_allowed && cookie_policy == ND_COOKIE_FIRST_PARTY &&
        top_url && !nd_url_is_same_site(url, effective_top_url))
        cookies_allowed = FALSE;
    char *cookie_partition_path = cookies_allowed
        ? nd_net_cookie_path_for_partition(top_origin) : NULL;
    g_free(top_origin);

    nd_cache_entry *cached = NULL;
    if (is_simple_get(method)) {
        cached = nd_cache_get(url, cache_partition);
        if (cached && nd_cache_is_fresh(cached)) {
            nd_response_free(resp);
            nd_response *from_cache = response_from_cache_entry(cached);
            nd_cache_entry_free(cached);
            g_free(cache_partition);
            g_free(cookie_partition_path);
            return from_cache;
        }
    }

    char *origin_slot = nd_url_origin_from(url);
    gboolean origin_held = FALSE;
    if (origin_slot) {
        origin_held = nd_net_acquire_origin_slot(origin_slot, cancellable);
        if (!origin_held) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "fetch cancelled");
            g_free(origin_slot);
            g_free(cache_partition);
            g_free(cookie_partition_path);
            nd_cache_entry_free(cached);
            nd_response_free(resp);
            return NULL;
        }
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        g_set_error_literal(error, ND_NET_DOMAIN, 1, "curl_easy_init failed");
        if (origin_held) nd_net_release_origin_slot(origin_slot);
        g_free(origin_slot);
        g_free(cache_partition);
        g_free(cookie_partition_path);
        nd_response_free(resp);
        return NULL;
    }
    if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    long max_redirs = cfg ? (long)cfg->max_redirects : (long)ND_MAX_REDIRECTS;
    if (max_redirs < 0)                       max_redirs = 0;
    if (max_redirs > (long)ND_MAX_REDIRECTS)  max_redirs = (long)ND_MAX_REDIRECTS;
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, max_redirs);

    long fetch_timeout = (long)ND_DEFAULT_TIMEOUT_S;
    if (yt_host) fetch_timeout = ND_MAX_TIMEOUT_S;
    if (fetch_timeout > (long)ND_MAX_TIMEOUT_S) fetch_timeout = (long)ND_MAX_TIMEOUT_S;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, fetch_timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, effective_ua);
    g_free(url_host);
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
        char *h = g_strdup_printf("Accept-Language: %s", accept_language);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }
    {
        char *accept = g_strdup_printf(
            "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
            "%s,*/*;q=0.5",
            nd_image_accept_header_fragment());
        headers = curl_slist_append(headers, accept);
        g_free(accept);
    }
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

    if (extra_headers) {
        for (guint i = 0; i < extra_headers->len; i++) {
            const char *h = g_ptr_array_index(extra_headers, i);
            if (h && *h) headers = curl_slist_append(headers, h);
        }
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

    if (cookie_partition_path) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_partition_path);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR,  cookie_partition_path);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nd_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp->body);
    nd_header_ctx header_ctx = {0};
    header_ctx.content_type_out = &resp->content_type;
    header_ctx.content_disposition_out = &resp->content_disposition;
    header_ctx.csp_out          = &resp->csp_header;
    header_ctx.xframe_options_out = &resp->xframe_options;
    header_ctx.cors_allow_origin_out = &resp->cors_allow_origin;
    header_ctx.sts_host = nd_url_host_from(url);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, nd_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_ctx);

    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    gboolean initial_https = g_str_has_prefix(url, "https://");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                     initial_https ? "https" : "http,https");

    const char *hsts_curl = nd_net_hsts_curl_path();
    if (hsts_curl) {
        curl_easy_setopt(curl, CURLOPT_HSTS_CTRL, (long)CURLHSTS_ENABLE);
        curl_easy_setopt(curl, CURLOPT_HSTS, hsts_curl);
    }

    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    const char *altsvc = nd_net_altsvc_path();
    if (altsvc)
        curl_easy_setopt(curl, CURLOPT_ALTSVC, altsvc);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, nd_xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancellable);

    CURLcode rc = curl_easy_perform(curl);

    if ((rc == CURLE_PEER_FAILED_VERIFICATION ||
         rc == CURLE_SSL_CACERT_BADFILE) &&
        g_str_has_prefix(url, "https://")) {
        gboolean opt_in = cfg && cfg->tls_allow_insecure_override;
        char *fb_host = nd_url_host_from(url);
        gboolean hsts_pinned = nd_net_hsts_should_upgrade(fb_host);
        if (opt_in && !hsts_pinned) {
            char *warn = g_strdup_printf(
                "Insecure: TLS certificate not trusted (%s)",
                errbuf[0] ? errbuf : curl_easy_strerror(rc));
            g_byte_array_set_size(resp->body, 0);
            errbuf[0] = '\0';
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
            curl_easy_setopt(curl, CURLOPT_COOKIEJAR,  NULL);
            curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
            rc = curl_easy_perform(curl);
            if (rc == CURLE_OK)
                resp->tls_warning = warn;
            else
                g_free(warn);
        }
        g_free(fb_host);
    }

    long status = 0;
    char *eff_url = NULL;
    long redirect_count = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url);
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirect_count);
    resp->status = status;
    resp->final_url = g_strdup(eff_url ? eff_url : url);
    resp->redirect_count = (int)redirect_count;

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
            if (origin_held) nd_net_release_origin_slot(origin_slot);
            g_free(origin_slot);
            return NULL;
        }
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        resp->error = g_strdup(msg);
    }

    if (rc == CURLE_OK && is_simple_get(method) && !header_ctx.set_cookie_seen) {
        if (resp->status == 304 && cached) {
            nd_cache_promote_304(url, cache_partition,
                                 header_ctx.cache_control, header_ctx.expires);
            g_byte_array_set_size(resp->body, 0);
            g_byte_array_append(resp->body, cached->body->data, cached->body->len);
            resp->status = cached->status;
            g_free(resp->content_type);
            resp->content_type = g_strdup(cached->content_type);
        } else if (resp->status > 0 && resp->body && resp->body->len > 0) {
            nd_cache_put(url, cache_partition,
                         resp->final_url, resp->status,
                         resp->content_type,
                         header_ctx.etag, header_ctx.last_modified,
                         header_ctx.cache_control, header_ctx.expires,
                         resp->body->data, resp->body->len);
        }
    }
    g_free(cache_partition);
    g_free(cookie_partition_path);

    g_free(header_ctx.etag);
    g_free(header_ctx.last_modified);
    g_free(header_ctx.cache_control);
    g_free(header_ctx.expires);
    nd_cache_entry_free(cached);

    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    if (origin_held) nd_net_release_origin_slot(origin_slot);
    g_free(origin_slot);
    g_free(hsts_upgraded);
    return resp;
}

nd_response *
nd_net_fetch_blocking(const char *url, GCancellable *cancellable, GError **error)
{
    return nd_fetch_sync(url, NULL, "GET", NULL, 0, NULL, NULL,
                         cancellable, error);
}

nd_response *
nd_net_request_blocking(const char        *url,
                        const char        *top_url,
                        const char        *method,
                        const void        *body,
                        gsize              body_len,
                        const char        *content_type,
                        const char *const *extra_headers,
                        GCancellable      *cancellable,
                        GError           **error)
{
    GPtrArray *hdrs = NULL;
    if (extra_headers) {
        hdrs = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; extra_headers[i]; i++)
            g_ptr_array_add(hdrs, g_strdup(extra_headers[i]));
    }
    nd_response *resp = nd_fetch_sync(url, top_url, method,
                                      body, body_len, content_type,
                                      hdrs, cancellable, error);
    if (hdrs) g_ptr_array_free(hdrs, TRUE);
    return resp;
}

typedef struct nd_fetch_ctx {
    char *url;
    char *top_url;
    char *method;
    char *content_type;
    guint8 *body;
    gsize body_len;
    GPtrArray *extra_headers;
} nd_fetch_ctx;

static void
nd_fetch_ctx_free(gpointer data)
{
    nd_fetch_ctx *ctx = data;
    g_free(ctx->url);
    g_free(ctx->top_url);
    g_free(ctx->method);
    g_free(ctx->content_type);
    g_free(ctx->body);
    if (ctx->extra_headers) g_ptr_array_free(ctx->extra_headers, TRUE);
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
    nd_response *resp = nd_fetch_sync(ctx->url, ctx->top_url, ctx->method,
                                      ctx->body, ctx->body_len, ctx->content_type,
                                      ctx->extra_headers,
                                      cancellable, &err);
    if (!resp) {
        g_task_return_error(task, err);
        return;
    }
    g_task_return_pointer(task, resp, (GDestroyNotify)nd_response_free);
}

void
nd_net_fetch_async(const char        *url,
                   const char        *top_url,
                   GCancellable      *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    g_return_if_fail(url != NULL);

    nd_fetch_ctx *ctx = g_new0(nd_fetch_ctx, 1);
    ctx->url = g_strdup(url);
    ctx->top_url = top_url ? g_strdup(top_url) : NULL;

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, nd_net_fetch_async);
    g_task_set_task_data(task, ctx, nd_fetch_ctx_free);
    g_task_run_in_thread(task, nd_fetch_thread);
    g_object_unref(task);
}

void
nd_net_post_async(const char         *url,
                  const char         *top_url,
                  const void         *body,
                  gsize               body_len,
                  const char         *content_type,
                  GCancellable       *cancellable,
                  GAsyncReadyCallback callback,
                  gpointer            user_data)
{
    nd_net_request_async(url, top_url, "POST", body, body_len, content_type, NULL,
                         cancellable, callback, user_data);
}

void
nd_net_request_async(const char         *url,
                     const char         *top_url,
                     const char         *method,
                     const void         *body,
                     gsize               body_len,
                     const char         *content_type,
                     const char *const  *extra_headers,
                     GCancellable       *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer            user_data)
{
    g_return_if_fail(url != NULL);

    nd_fetch_ctx *ctx = g_new0(nd_fetch_ctx, 1);
    ctx->url = g_strdup(url);
    ctx->top_url = top_url ? g_strdup(top_url) : NULL;
    if (method && *method) ctx->method = g_strdup(method);
    if (content_type && *content_type) ctx->content_type = g_strdup(content_type);
    if (body && body_len > 0) {
        ctx->body = g_memdup2(body, body_len);
        ctx->body_len = body_len;
    }
    if (extra_headers && extra_headers[0]) {
        ctx->extra_headers = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; extra_headers[i]; i++)
            g_ptr_array_add(ctx->extra_headers, g_strdup(extra_headers[i]));
    }

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, nd_net_request_async);
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
