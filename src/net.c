/* Nordstjernen — libcurl-backed async fetcher.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "net.h"
#include "ai.h"
#include "cache.h"
#include "config.h"
#include "history.h"
#include "mobile.h"
#include "csp.h"
#include "debuglog.h"
#include "ext.h"
#include "html.h"
#include "image.h"
#include "security.h"
#include "about_logo_gif.h"
#include "about_splash_gif.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <glib/gstdio.h>
#include <gmodule.h>

#include <lexbor/unicode/idna.h>
#include <lexbor/url/url.h>

#include <libpsl.h>
#include <sqlite3.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <webp/decode.h>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#ifdef NS_HAVE_LIBAV
#include <libavutil/avutil.h>
#endif

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#endif

static char *g_cookie_dir;
static char *g_private_root;
static char *g_hsts_curl_path;
static char *g_altsvc_path;
static GHashTable *g_hsts_cache;
static gint64      g_hsts_cache_mtime_us;
static GMutex      g_hsts_lock;
static char *g_ca_bundle;
static gboolean g_has_http3;
static const char *g_ec_curves = "X25519:P-256:P-384";
static char *g_accept_encoding;
static char *g_proxy_override;
static CURLSH *g_share;
static GMutex g_fetch_throttle_mutex;
static GCond  g_fetch_idle_cond;
static int    g_fetch_active;
static int    g_preconnect_active;
static gint   g_net_aborting;
static GQueue g_fetch_queue = G_QUEUE_INIT;
static GMutex g_share_locks[CURL_LOCK_DATA_LAST];
static GMutex      g_conn_stats_lock;
static GHashTable *g_conn_stats;

#define NS_DEAD_HOST_TTL_US ((gint64)120 * G_USEC_PER_SEC)
static GHashTable *g_dead_hosts;
static GMutex      g_dead_hosts_lock;

static gboolean
ns_net_host_recently_dead(const char *host)
{
    gboolean dead = FALSE;
    g_mutex_lock(&g_dead_hosts_lock);
    if (g_dead_hosts) {
        gint64 *expiry = g_hash_table_lookup(g_dead_hosts, host);
        if (expiry) {
            if (g_get_monotonic_time() < *expiry)
                dead = TRUE;
            else
                g_hash_table_remove(g_dead_hosts, host);
        }
    }
    g_mutex_unlock(&g_dead_hosts_lock);
    return dead;
}

static void
ns_net_host_mark_dead(const char *host)
{
    g_mutex_lock(&g_dead_hosts_lock);
    if (!g_dead_hosts)
        g_dead_hosts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
    gint64 *expiry = g_new(gint64, 1);
    *expiry = g_get_monotonic_time() + NS_DEAD_HOST_TTL_US;
    g_hash_table_replace(g_dead_hosts, g_strdup(host), expiry);
    g_mutex_unlock(&g_dead_hosts_lock);
}

static void
ns_net_host_mark_alive(const char *host)
{
    g_mutex_lock(&g_dead_hosts_lock);
    if (g_dead_hosts)
        g_hash_table_remove(g_dead_hosts, host);
    g_mutex_unlock(&g_dead_hosts_lock);
}

#define NS_NET_MAX_PER_ORIGIN 6

typedef struct ns_origin_slot {
    int   in_use;
    GCond cond;
} ns_origin_slot;

static GMutex      g_origin_slots_lock;
static GHashTable *g_origin_slots;

static void
ns_origin_slot_free(gpointer p)
{
    ns_origin_slot *s = p;
    g_cond_clear(&s->cond);
    g_free(s);
}

static char *
origin_slot_key(const char *origin)
{
    return (origin && *origin) ? g_ascii_strdown(origin, -1) : NULL;
}

static gboolean
ns_net_acquire_origin_slot(const char *origin, GCancellable *cancellable)
{
    char *key = origin_slot_key(origin);
    if (!key) return FALSE;
    g_mutex_lock(&g_origin_slots_lock);
    if (!g_origin_slots)
        g_origin_slots = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, ns_origin_slot_free);
    ns_origin_slot *s = g_hash_table_lookup(g_origin_slots, key);
    if (!s) {
        s = g_new0(ns_origin_slot, 1);
        g_cond_init(&s->cond);
        g_hash_table_insert(g_origin_slots, key, s);
        key = NULL;
    }
    while (s->in_use >= NS_NET_MAX_PER_ORIGIN) {
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
ns_net_release_origin_slot(const char *origin)
{
    char *key = origin_slot_key(origin);
    if (!key) return;
    g_mutex_lock(&g_origin_slots_lock);
    if (g_origin_slots) {
        ns_origin_slot *s = g_hash_table_lookup(g_origin_slots, key);
        if (s && s->in_use > 0) {
            s->in_use--;
            g_cond_signal(&s->cond);
        }
    }
    g_mutex_unlock(&g_origin_slots_lock);
    g_free(key);
}

typedef struct ns_multi_xfer {
    CURL     *easy;
    GCond     cond;
    gboolean  done;
    CURLcode  result;
} ns_multi_xfer;

static CURLM      *g_multi;
static GThread    *g_multi_thread;
static gboolean    g_multi_quit;
static GMutex      g_multi_lock;
static GQueue      g_multi_incoming = G_QUEUE_INIT;
static GHashTable *g_multi_active;

static void
ns_net_multi_finish_locked(ns_multi_xfer *x, CURLcode result)
{
    x->result = result;
    x->done = TRUE;
    g_cond_signal(&x->cond);
}

static gpointer
ns_net_multi_loop(gpointer data)
{
    (void)data;
    for (;;) {
        g_mutex_lock(&g_multi_lock);
        if (g_multi_quit) {
            GHashTableIter it;
            gpointer key, val;
            g_hash_table_iter_init(&it, g_multi_active);
            while (g_hash_table_iter_next(&it, &key, &val)) {
                ns_multi_xfer *x = val;
                curl_multi_remove_handle(g_multi, x->easy);
                ns_net_multi_finish_locked(x, CURLE_ABORTED_BY_CALLBACK);
            }
            g_hash_table_remove_all(g_multi_active);
            for (ns_multi_xfer *x; (x = g_queue_pop_head(&g_multi_incoming)); )
                ns_net_multi_finish_locked(x, CURLE_ABORTED_BY_CALLBACK);
            g_mutex_unlock(&g_multi_lock);
            break;
        }
        for (ns_multi_xfer *x; (x = g_queue_pop_head(&g_multi_incoming)); ) {
            if (curl_multi_add_handle(g_multi, x->easy) == CURLM_OK)
                g_hash_table_insert(g_multi_active, x->easy, x);
            else
                ns_net_multi_finish_locked(x, CURLE_FAILED_INIT);
        }
        g_mutex_unlock(&g_multi_lock);

        int running = 0;
        curl_multi_perform(g_multi, &running);

        int nmsgs = 0;
        CURLMsg *m;
        while ((m = curl_multi_info_read(g_multi, &nmsgs))) {
            if (m->msg != CURLMSG_DONE) continue;
            CURL *easy = m->easy_handle;
            CURLcode res = m->data.result;
            curl_multi_remove_handle(g_multi, easy);
            g_mutex_lock(&g_multi_lock);
            ns_multi_xfer *x = g_hash_table_lookup(g_multi_active, easy);
            if (x) {
                g_hash_table_remove(g_multi_active, easy);
                ns_net_multi_finish_locked(x, res);
            }
            g_mutex_unlock(&g_multi_lock);
        }

        long timeo = -1;
        curl_multi_timeout(g_multi, &timeo);
        int wait_ms = (timeo < 0 || timeo > 1000) ? 1000 : (int)timeo;
        curl_multi_poll(g_multi, NULL, 0, wait_ms, NULL);
    }
    return NULL;
}

static void
ns_net_multi_start(void)
{
    g_mutex_lock(&g_multi_lock);
    if (!g_multi_thread) {
        g_multi = curl_multi_init();
        if (g_multi) {
            curl_multi_setopt(g_multi, CURLMOPT_PIPELINING,
                              (long)CURLPIPE_MULTIPLEX);
            g_multi_active = g_hash_table_new(g_direct_hash, g_direct_equal);
            g_multi_thread = g_thread_new("ns-net-multi",
                                          ns_net_multi_loop, NULL);
        }
    }
    g_mutex_unlock(&g_multi_lock);
}

static CURLcode
ns_net_multi_perform(CURL *easy, GCancellable *cancellable)
{
    (void)cancellable;
    ns_net_multi_start();
    if (!g_multi) return curl_easy_perform(easy);

    ns_multi_xfer x = { .easy = easy, .done = FALSE, .result = CURLE_OK };
    g_cond_init(&x.cond);

    g_mutex_lock(&g_multi_lock);
    if (g_multi_quit) {
        g_mutex_unlock(&g_multi_lock);
        g_cond_clear(&x.cond);
        return curl_easy_perform(easy);
    }
    g_queue_push_tail(&g_multi_incoming, &x);
    curl_multi_wakeup(g_multi);
    while (!x.done) {
        gint64 wakeup = g_get_monotonic_time() + 250 * G_TIME_SPAN_MILLISECOND;
        g_cond_wait_until(&x.cond, &g_multi_lock, wakeup);
    }
    g_mutex_unlock(&g_multi_lock);

    g_cond_clear(&x.cond);
    return x.result;
}

static void
ns_net_multi_shutdown(void)
{
    g_mutex_lock(&g_multi_lock);
    GThread *t = g_multi_thread;
    if (t) {
        g_multi_quit = TRUE;
        curl_multi_wakeup(g_multi);
    }
    g_mutex_unlock(&g_multi_lock);
    if (t) {
        g_thread_join(t);
        g_multi_thread = NULL;
    }
    if (g_multi) { curl_multi_cleanup(g_multi); g_multi = NULL; }
    if (g_multi_active) {
        g_hash_table_destroy(g_multi_active);
        g_multi_active = NULL;
    }
    g_multi_quit = FALSE;
}

static char *
ns_net_data_path(char **slot, const char *basename)
{
    if (*slot) return *slot;
    char *dir = g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME, NULL);
    g_mkdir_with_parents(dir, 0700);
    *slot = g_build_filename(dir, basename, NULL);
    g_free(dir);
    return *slot;
}

static void
ns_rmrf(const char *path)
{
    if (!path) return;
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            char *child = g_build_filename(path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
                !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                ns_rmrf(child);
            else
                g_unlink(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_rmdir(path);
}

static const char *
ns_net_private_root(void)
{
    if (g_private_root) return g_private_root;
    g_private_root = g_dir_make_tmp("nordstjernen-private-XXXXXX", NULL);
    if (!g_private_root) {
        char *base = g_strdup_printf("nordstjernen-private-%u", g_random_int());
        g_private_root = g_build_filename(g_get_tmp_dir(), base, NULL);
        g_free(base);
        g_mkdir_with_parents(g_private_root, 0700);
    }
    g_chmod(g_private_root, 0700);
    return g_private_root;
}

static gboolean
ns_net_is_private(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg && cfg->private_mode;
}

static char *
ns_net_hsts_curl_path(void)
{
    if (g_hsts_curl_path) return g_hsts_curl_path;
    if (!ns_net_is_private())
        return ns_net_data_path(&g_hsts_curl_path, "hsts-curl.txt");
    const char *root = ns_net_private_root();
    if (!root) return NULL;
    g_hsts_curl_path = g_build_filename(root, "hsts-curl.txt", NULL);
    char *real_dir = g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME,
                                      NULL);
    char *real = g_build_filename(real_dir, "hsts-curl.txt", NULL);
    char *contents = NULL;
    gsize len = 0;
    if (g_file_get_contents(real, &contents, &len, NULL))
        g_file_set_contents(g_hsts_curl_path, contents, (gssize)len, NULL);
    g_free(contents);
    g_free(real);
    g_free(real_dir);
    return g_hsts_curl_path;
}

static void
ns_hsts_cache_reload_locked(const char *path)
{
    if (!g_hsts_cache)
        g_hsts_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    else
        g_hash_table_remove_all(g_hsts_cache);

    char *content = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(path, &content, &len, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_warning("hsts: failed to read %s: %s", path, err->message);
        g_clear_error(&err);
        return;
    }

    char **lines = g_strsplit(content, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') continue;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        gboolean subs = (*line == '.');
        const char *host = subs ? line + 1 : line;
        if (!*host) continue;
        g_hash_table_replace(g_hsts_cache,
                             g_ascii_strdown(host, -1),
                             GINT_TO_POINTER(subs ? 2 : 1));
    }
    g_strfreev(lines);
    g_free(content);
}

static gint64
file_mtime_us(const char *path)
{
    GStatBuf st;
    if (g_stat(path, &st) != 0) return 0;
    return (gint64)st.st_mtime * G_USEC_PER_SEC;
}

static gboolean
ns_hsts_lookup_locked(const char *lower_host)
{
    gpointer v = g_hash_table_lookup(g_hsts_cache, lower_host);
    if (v) return TRUE;
    const char *dot = lower_host;
    while ((dot = strchr(dot, '.')) != NULL) {
        const char *parent = dot + 1;
        v = g_hash_table_lookup(g_hsts_cache, parent);
        if (v && GPOINTER_TO_INT(v) == 2) return TRUE;
        dot = parent;
    }
    return FALSE;
}

gboolean
ns_url_is_http_or_https(const char *url)
{
    return url && (g_str_has_prefix(url, "http://") ||
                   g_str_has_prefix(url, "https://"));
}

static gboolean
ns_query_param_is_tracking(const char *key, size_t key_len)
{
    static const char *const exact[] = {
        "gclid", "dclid", "gbraid", "wbraid",
        "fbclid", "msclkid", "yclid", "twclid", "igshid",
        "mc_cid", "mc_eid",
        "_hsenc", "_hsmi", "__hssc", "__hstc", "__hsfp", "hsctatracking",
        "vero_id", "vero_conv",
        "oly_anon_id", "oly_enc_id",
        "_openstat", "wickedid", "rb_clickid", "s_cid",
        "ml_subscriber", "ml_subscriber_hash",
        "mtm_source", "mtm_medium", "mtm_campaign", "mtm_keyword",
        "mtm_cid", "mtm_content", "mtm_group", "mtm_placement",
        "pk_source", "pk_medium", "pk_campaign", "pk_keyword",
        "pk_cid", "pk_content",
    };
    if (key_len == 0)
        return FALSE;
    if (key_len >= 4 && g_ascii_strncasecmp(key, "utm_", 4) == 0)
        return TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(exact); i++)
        if (strlen(exact[i]) == key_len &&
            g_ascii_strncasecmp(key, exact[i], key_len) == 0)
            return TRUE;
    return FALSE;
}

static gboolean
ns_url_is_google_search(const char *url)
{
    char *host = ns_url_host_from(url);
    gboolean google_host = host &&
        (g_str_equal(host, "google.com") ||
         g_str_has_prefix(host, "www.google."));
    g_free(host);
    if (!google_host)
        return FALSE;

    const char *scheme = strstr(url, "://");
    const char *path = scheme ? strchr(scheme + 3, '/') : NULL;
    if (!path)
        return FALSE;
    const char *path_end = strpbrk(path, "?#");
    size_t path_len = path_end ? (size_t)(path_end - path) : strlen(path);
    return path_len == strlen("/search") &&
           memcmp(path, "/search", path_len) == 0;
}

static gboolean
ns_google_search_param_is_tracking(const char *key, size_t key_len)
{
    static const char *const exact[] = {
        "sca_esv", "source", "ei", "iflsig", "ved", "uact", "oq",
        "gs_lp", "sclient", "sei",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(exact); i++)
        if (strlen(exact[i]) == key_len &&
            g_ascii_strncasecmp(key, exact[i], key_len) == 0)
            return TRUE;
    return FALSE;
}

char *
ns_url_strip_tracking_params(const char *url)
{
    if (!ns_url_is_http_or_https(url))
        return NULL;
    const ns_config *cfg = ns_config_get();
    if (!cfg || !cfg->strip_tracking_params)
        return NULL;

    const char *frag = strchr(url, '#');
    const char *query_end = frag ? frag : url + strlen(url);
    const char *query = strchr(url, '?');
    if (!query || query >= query_end)
        return NULL;

    GString *kept = g_string_new(NULL);
    gboolean removed = FALSE;
    gboolean google_search = ns_url_is_google_search(url);
    for (const char *p = query + 1; ; ) {
        const char *amp = memchr(p, '&', (size_t)(query_end - p));
        const char *tok_end = amp ? amp : query_end;
        const char *eq = memchr(p, '=', (size_t)(tok_end - p));
        size_t key_len = (size_t)((eq ? eq : tok_end) - p);
        if (ns_query_param_is_tracking(p, key_len) ||
            (google_search &&
             ns_google_search_param_is_tracking(p, key_len))) {
            removed = TRUE;
        } else {
            if (kept->len)
                g_string_append_c(kept, '&');
            g_string_append_len(kept, p, (gssize)(tok_end - p));
        }
        if (!amp)
            break;
        p = amp + 1;
    }

    if (!removed) {
        g_string_free(kept, TRUE);
        return NULL;
    }

    GString *out = g_string_new_len(url, (gssize)(query - url));
    if (kept->len) {
        g_string_append_c(out, '?');
        g_string_append_len(out, kept->str, (gssize)kept->len);
    }
    if (frag)
        g_string_append(out, frag);
    g_string_free(kept, TRUE);
    return g_string_free(out, FALSE);
}

static gboolean
ns_url_is_ftp(const char *url)
{
    return url && g_str_has_prefix(url, "ftp://");
}

static gboolean
refresh_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

gboolean
ns_net_parse_refresh(const char *input, double *time_out, char **url_out)
{
    if (time_out) *time_out = 0.0;
    if (url_out) *url_out = NULL;
    if (!input) return FALSE;

    const char *p = input;
    while (refresh_is_space(*p)) p++;

    const char *digits = p;
    while (g_ascii_isdigit(*p)) p++;
    if (p == digits && *p != '.') return FALSE;

    double seconds = 0.0;
    for (const char *d = digits; d < p; d++) {
        seconds = seconds * 10.0 + (double)(*d - '0');
        if (seconds > 2147483647.0) { seconds = 2147483647.0; break; }
    }
    while (*p == '.' || g_ascii_isdigit(*p)) p++;

    if (*p) {
        if (*p != ';' && *p != ',' && !refresh_is_space(*p)) return FALSE;
        while (refresh_is_space(*p)) p++;
        if (*p == ';' || *p == ',') p++;
        while (refresh_is_space(*p)) p++;
    }

    if (*p == 'u' || *p == 'U') {
        const char *q = p + 1;
        if (*q == 'r' || *q == 'R') {
            q++;
            if (*q == 'l' || *q == 'L') {
                q++;
                while (refresh_is_space(*q)) q++;
                if (*q == '=') {
                    q++;
                    while (refresh_is_space(*q)) q++;
                    p = q;
                }
            }
        }
    }

    char *url = NULL;
    if (*p == '\'' || *p == '"') {
        char quote = *p++;
        const char *end = strchr(p, quote);
        url = end ? g_strndup(p, (gsize)(end - p)) : g_strdup(p);
    } else if (*p) {
        url = g_strdup(p);
        g_strchomp(url);
    }
    if (url && !*url) g_clear_pointer(&url, g_free);

    if (time_out) *time_out = seconds;
    if (url_out) *url_out = url;
    else g_free(url);
    return TRUE;
}

static lxb_status_t
ns_url_str_append_cb(const lxb_char_t *data, size_t length, void *ctx)
{
    g_string_append_len((GString *)ctx, (const char *)data, (gssize)length);
    return LXB_STATUS_OK;
}

static void
ns_url_parser_destroy_tls(gpointer p)
{
    lxb_url_parser_t *parser = p;
    if (!parser) return;
    lxb_url_parser_memory_destroy(parser);
    lxb_url_parser_destroy(parser, true);
}

static GPrivate g_url_parser_tls = G_PRIVATE_INIT(ns_url_parser_destroy_tls);

static lxb_url_parser_t *
ns_url_parser_open(void)
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
ns_url_parser_close(lxb_url_parser_t *parser)
{
    if (!parser) return;
    lxb_url_parser_clean(parser);
}

char *
ns_url_resolve_len(const char *base, const char *href, size_t href_len)
{
    if (!href) return NULL;
    if (href_len == 0 && !(base && *base)) return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *base_url = NULL;
    if (base && *base) {
        base_url = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)base, strlen(base));
        lxb_url_parser_clean(parser);
        if (!base_url) {
            ns_url_parser_close(parser);
            return NULL;
        }
    }
    lxb_url_t *resolved = lxb_url_parse(parser, base_url,
                                        (const lxb_char_t *)href, href_len);
    char *out = NULL;
    if (resolved) {
        GString *s = g_string_new(NULL);
        if (lxb_url_serialize(resolved, ns_url_str_append_cb, s, false)
            == LXB_STATUS_OK && s->len > 0)
            out = g_string_free(s, FALSE);
        else
            g_string_free(s, TRUE);
    }
    ns_url_parser_close(parser);
    return out;
}

char *
ns_url_resolve(const char *base, const char *href)
{
    return ns_url_resolve_len(base, href, href ? strlen(href) : 0);
}

char *
ns_url_set_component_len(const char *href, const char *component,
                         const char *value, size_t value_len)
{
    if (!href || !component || !value) return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)href, strlen(href));
    if (!u) {
        ns_url_parser_close(parser);
        return NULL;
    }

    const lxb_char_t *v = (const lxb_char_t *)value;
    size_t vlen = value_len;

    if (strcmp(component, "protocol") == 0)
        (void) lxb_url_api_protocol_set(u, parser, v, vlen);
    else if (strcmp(component, "username") == 0)
        (void) lxb_url_api_username_set(u, v, vlen);
    else if (strcmp(component, "password") == 0)
        (void) lxb_url_api_password_set(u, v, vlen);
    else if (strcmp(component, "host") == 0)
        (void) lxb_url_api_host_set(u, parser, v, vlen);
    else if (strcmp(component, "hostname") == 0)
        (void) lxb_url_api_hostname_set(u, parser, v, vlen);
    else if (strcmp(component, "port") == 0)
        (void) lxb_url_api_port_set(u, parser, v, vlen);
    else if (strcmp(component, "pathname") == 0)
        (void) lxb_url_api_pathname_set(u, parser, v, vlen);
    else if (strcmp(component, "search") == 0)
        (void) lxb_url_api_search_set(u, parser, v, vlen);
    else if (strcmp(component, "hash") == 0)
        (void) lxb_url_api_hash_set(u, parser, v, vlen);

    char *out = NULL;
    GString *s = g_string_new(NULL);
    if (lxb_url_serialize(u, ns_url_str_append_cb, s, false) == LXB_STATUS_OK
        && s->len > 0)
        out = g_string_free(s, FALSE);
    else
        g_string_free(s, TRUE);
    ns_url_parser_close(parser);
    return out;
}

static char *
ns_url_to_ascii(const char *url)
{
    if (!url || !*url) return NULL;
    if (g_str_has_prefix(url, "data:") || g_str_has_prefix(url, "about:") ||
        g_str_has_prefix(url, "file:"))
        return g_strdup(url);
    return ns_url_resolve(NULL, url);
}

static lxb_url_t *
ns_url_parse_with_host(lxb_url_parser_t *parser, const char *url)
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
ns_url_origin_from(const char *url)
{
    if (!url || !*url) return NULL;
    if (!ns_url_is_http_or_https(url))
        return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = ns_url_parse_with_host(parser, url);
    char *out = NULL;
    if (u) {
        GString *s = g_string_new(NULL);
        g_string_append_len(s, (const char *)u->scheme.name.data,
                            (gssize)u->scheme.name.length);
        g_string_append(s, "://");
        if (lxb_url_serialize_host(&u->host, ns_url_str_append_cb, s)
            == LXB_STATUS_OK) {
            if (u->has_port)
                g_string_append_printf(s, ":%u", (unsigned)u->port);
            out = g_string_free(s, FALSE);
        } else {
            g_string_free(s, TRUE);
        }
    }
    ns_url_parser_close(parser);
    return out;
}

gboolean
ns_url_same_origin(const char *a, const char *b)
{
    char *oa = ns_url_origin_from(a);
    char *ob = ns_url_origin_from(b);
    gboolean eq = oa && *oa && ob && *ob && g_ascii_strcasecmp(oa, ob) == 0;
    g_free(oa);
    g_free(ob);
    return eq;
}

static char *
ns_net_referer_for(const char *url, const char *top_url,
                   ns_referer_policy policy)
{
    if (!top_url || !*top_url || !ns_url_is_http_or_https(url) ||
        !ns_url_is_http_or_https(top_url) ||
        policy == NS_REFERER_NO_REFERRER)
        return NULL;
    if (g_str_has_prefix(top_url, "https://") &&
        g_str_has_prefix(url, "http://"))
        return NULL;
    gboolean same_origin = ns_url_same_origin(top_url, url);
    if (policy == NS_REFERER_SAME_ORIGIN && !same_origin)
        return NULL;
    if (policy == NS_REFERER_UNSAFE_URL || same_origin) {
        g_autoptr(ns_url_parts) p = ns_url_parts_new(top_url);
        if (!p || !p->origin || !*p->origin) return NULL;
        return g_strconcat(p->origin, p->pathname ? p->pathname : "",
                           p->search ? p->search : "", NULL);
    }
    char *origin = ns_url_origin_from(top_url);
    if (!origin) return NULL;
    char *out = g_strdup_printf("%s/", origin);
    g_free(origin);
    return out;
}

static char *
ns_url_site_from(const char *url)
{
    if (!url || !*url) return NULL;
    g_autoptr(ns_url_parts) p = ns_url_parts_new(url);
    if (!p || !p->protocol || !p->hostname) return NULL;
    g_autofree char *lower = g_ascii_strdown(p->hostname, -1);
    const psl_ctx_t *psl = psl_builtin();
    const char *reg = psl ? psl_registrable_domain(psl, lower) : NULL;
    const char *site_host = (reg && *reg) ? reg : p->hostname;
    if (p->port && *p->port)
        return g_strdup_printf("%s://%s:%s", p->protocol, site_host, p->port);
    return g_strdup_printf("%s://%s", p->protocol, site_host);
}

static gboolean
ns_url_is_same_site(const char *a, const char *b)
{
    if (!a || !b) return FALSE;
    g_autofree char *sa = ns_url_site_from(a);
    g_autofree char *sb = ns_url_site_from(b);
    if (sa && sb) return g_ascii_strcasecmp(sa, sb) == 0;

    g_autofree char *ha = ns_url_host_from(a);
    g_autofree char *hb = ns_url_host_from(b);
    if (!ha || !hb) return FALSE;
    if (g_ascii_strcasecmp(ha, hb) == 0) return TRUE;
    gsize la = strlen(ha), lb = strlen(hb);
    if (la > lb + 1 && ha[la - lb - 1] == '.' &&
        g_ascii_strncasecmp(ha + la - lb, hb, lb) == 0)
        return TRUE;
    if (lb > la + 1 && hb[lb - la - 1] == '.' &&
        g_ascii_strncasecmp(hb + lb - la, ha, la) == 0)
        return TRUE;
    return FALSE;
}

char *
ns_url_host_from(const char *url)
{
    if (!url) return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = ns_url_parse_with_host(parser, url);
    char *out = NULL;
    if (u) {
        GString *s = g_string_new(NULL);
        if (lxb_url_serialize_host(&u->host, ns_url_str_append_cb, s)
            == LXB_STATUS_OK && s->len > 0)
            out = g_string_free(s, FALSE);
        else
            g_string_free(s, TRUE);
    }
    ns_url_parser_close(parser);
    return out;
}

void
ns_url_parts_free(ns_url_parts *parts)
{
    if (!parts) return;
    g_free(parts->href);
    g_free(parts->protocol);
    g_free(parts->origin);
    g_free(parts->host);
    g_free(parts->hostname);
    g_free(parts->port);
    g_free(parts->pathname);
    g_free(parts->search);
    g_free(parts->hash);
    g_free(parts->username);
    g_free(parts->password);
    g_free(parts);
}

static char *
ns_url_take_serialized(GString *s, lxb_status_t status)
{
    if (status != LXB_STATUS_OK) {
        g_string_free(s, TRUE);
        return g_strdup("");
    }
    return g_string_free(s, FALSE);
}

static ns_url_parts *
ns_url_parts_new_depth(const char *url, int depth)
{
    if (!url) return NULL;

    lxb_url_parser_t *parser = ns_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)url, strlen(url));
    if (!u) {
        ns_url_parser_close(parser);
        return NULL;
    }

    ns_url_parts *p = g_new0(ns_url_parts, 1);

    GString *s = g_string_new(NULL);
    p->href = ns_url_take_serialized(s,
        lxb_url_serialize(u, ns_url_str_append_cb, s, false));
    if (!*p->href) {
        g_free(p->href);
        p->href = g_strdup(url);
    }

    s = g_string_new(NULL);
    char *scheme = ns_url_take_serialized(s,
        lxb_url_serialize_scheme(u, ns_url_str_append_cb, s));
    p->protocol = *scheme ? g_strconcat(scheme, ":", NULL) : g_strdup("");
    g_free(scheme);

    if (u->host.type == LXB_URL_HOST_TYPE__UNDEF ||
        u->host.type == LXB_URL_HOST_TYPE_EMPTY) {
        p->hostname = g_strdup("");
    } else {
        s = g_string_new(NULL);
        p->hostname = ns_url_take_serialized(s,
            lxb_url_serialize_host(&u->host, ns_url_str_append_cb, s));
    }

    p->port = u->has_port ? g_strdup_printf("%u", (unsigned)u->port)
                          : g_strdup("");

    p->host = (*p->hostname && *p->port)
        ? g_strconcat(p->hostname, ":", p->port, NULL)
        : g_strdup(p->hostname);

    gboolean tuple_origin = *p->hostname
        && (strcmp(p->protocol, "http:") == 0
            || strcmp(p->protocol, "https:") == 0
            || strcmp(p->protocol, "ws:") == 0
            || strcmp(p->protocol, "wss:") == 0
            || strcmp(p->protocol, "ftp:") == 0);
    p->origin = tuple_origin
        ? g_strconcat(p->protocol, "//", p->host, NULL)
        : g_strdup("null");

    s = g_string_new(NULL);
    p->pathname = ns_url_take_serialized(s,
        lxb_url_serialize_path(&u->path, ns_url_str_append_cb, s));

    if (depth == 0 && strcmp(p->protocol, "blob:") == 0 &&
        p->pathname && *p->pathname) {
        ns_url_parts *inner = ns_url_parts_new_depth(p->pathname, depth + 1);
        if (inner) {
            if (strcmp(inner->protocol, "http:") == 0 ||
                strcmp(inner->protocol, "https:") == 0) {
                g_free(p->origin);
                p->origin = g_strdup(inner->origin);
            }
            ns_url_parts_free(inner);
        }
    }

    if (u->query.length) {
        s = g_string_new("?");
        g_string_append_len(s, (const char *)u->query.data,
                            (gssize)u->query.length);
        p->search = g_string_free(s, FALSE);
    } else {
        p->search = g_strdup("");
    }

    if (u->fragment.length) {
        s = g_string_new("#");
        g_string_append_len(s, (const char *)u->fragment.data,
                            (gssize)u->fragment.length);
        p->hash = g_string_free(s, FALSE);
    } else {
        p->hash = g_strdup("");
    }

    p->username = u->username.length
        ? g_strndup((const char *)u->username.data, u->username.length)
        : g_strdup("");
    p->password = u->password.length
        ? g_strndup((const char *)u->password.data, u->password.length)
        : g_strdup("");

    ns_url_parser_close(parser);
    return p;
}

ns_url_parts *
ns_url_parts_new(const char *url)
{
    return ns_url_parts_new_depth(url, 0);
}

gboolean
ns_url_is_valid_absolute(const char *url)
{
    if (!url || !*url) return FALSE;
    for (const char *p = url; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '\n' ||
            *p == '\r' || *p == '\f')
            return FALSE;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(url);
    return parts && parts->protocol && *parts->protocol;
}

gboolean
ns_net_hsts_should_upgrade(const char *host)
{
    if (!host || !*host) return FALSE;
    char *path = ns_net_hsts_curl_path();
    if (!path) return FALSE;
    g_mutex_lock(&g_hsts_lock);
    gint64 mtime = file_mtime_us(path);
    if (!g_hsts_cache || mtime != g_hsts_cache_mtime_us) {
        ns_hsts_cache_reload_locked(path);
        g_hsts_cache_mtime_us = mtime;
    }
    char *lower = g_ascii_strdown(host, -1);
    for (gsize ln = strlen(lower); ln > 0 && lower[ln - 1] == '.'; ln--)
        lower[ln - 1] = '\0';
    gboolean hit = ns_hsts_lookup_locked(lower);
    g_free(lower);
    g_mutex_unlock(&g_hsts_lock);
    return hit;
}

char *
ns_net_hsts_upgrade(const char *url)
{
    if (!url) return NULL;
    if (!g_str_has_prefix(url, "http://")) return NULL;
    char *host = ns_url_host_from(url);
    if (!host) return NULL;
    gboolean upgrade = ns_net_hsts_should_upgrade(host);
    g_free(host);
    if (!upgrade) return NULL;
    return g_strconcat("https://", url + 7, NULL);
}

const char *
ns_user_agent_for_mode(const char *compat_mode)
{
    if (compat_mode && *compat_mode) {
        if (g_ascii_strcasecmp(compat_mode, "ladybird") == 0)
            return NS_UA_LADYBIRD;
        if (g_ascii_strcasecmp(compat_mode, "firefox") == 0)
            return NS_UA_FIREFOX;
    }
    return NS_USER_AGENT;
}

gboolean
ns_user_agent_has_client_hints(const char *user_agent)
{
    return user_agent && strstr(user_agent, "Chrome/") &&
           !strstr(user_agent, "Nordstjernen/") &&
           !strstr(user_agent, "Ladybird/");
}

static gboolean
ns_host_is_loopback(const char *host)
{
    return g_ascii_strcasecmp(host, "localhost") == 0 ||
           g_str_has_suffix(host, ".localhost") ||
           strcmp(host, "127.0.0.1") == 0 ||
           strcmp(host, "::1") == 0 ||
           strcmp(host, "[::1]") == 0;
}

char *
ns_net_https_first_upgrade(const char *url)
{
    const ns_config *cfg = ns_config_get();
    if (!cfg || !cfg->https_first) return NULL;
    if (!url || !g_str_has_prefix(url, "http://")) return NULL;
    char *host = ns_url_host_from(url);
    if (!host) return NULL;
    gboolean loopback = ns_host_is_loopback(host);
    g_free(host);
    if (loopback) return NULL;
    return g_strconcat("https://", url + 7, NULL);
}

gboolean
ns_net_header_is_nosniff(const char *value)
{
    if (!value || !*value) return FALSE;
    for (const char *p = value; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        if ((size_t)(p - start) == 7 &&
            g_ascii_strncasecmp(start, "nosniff", 7) == 0)
            return TRUE;
    }
    return FALSE;
}

static const char *
ns_net_cookie_dir(void)
{
    if (g_cookie_dir) return g_cookie_dir;
    if (ns_net_is_private()) {
        const char *root = ns_net_private_root();
        g_cookie_dir = g_build_filename(root ? root : g_get_tmp_dir(),
                                        "cookies", NULL);
    } else {
        const char *config = g_get_user_config_dir();
        g_cookie_dir = g_build_filename(config, NS_APP_DIR_NAME, "cookies",
                                        NULL);
    }
    g_mkdir_with_parents(g_cookie_dir, 0700);
    return g_cookie_dir;
}

static void
ns_net_empty_dir(const char *dir)
{
    if (!dir) return;
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        char *child = g_build_filename(dir, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
            !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
            ns_rmrf(child);
        else
            g_unlink(child);
        g_free(child);
    }
    g_dir_close(d);
}

void
ns_net_cookies_clear(void)
{
    ns_net_empty_dir(ns_net_cookie_dir());
}

void
ns_net_site_storage_clear(void)
{
    char *base = g_build_filename(g_get_user_data_dir(), NS_APP_DIR_NAME, NULL);
    char *localstorage = g_build_filename(base, "localstorage", NULL);
    char *indexeddb    = g_build_filename(base, "indexeddb", NULL);
    ns_rmrf(localstorage);
    ns_rmrf(indexeddb);
    g_free(localstorage);
    g_free(indexeddb);
    g_free(base);
}

static char *
ns_net_cookie_path_for_partition(const char *top_origin)
{
    const char *dir = ns_net_cookie_dir();
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

/* JS-set (document.cookie) cookies are persisted to a sibling ".js.txt" file
 * that curl reads as an additional CURLOPT_COOKIEFILE source but never writes
 * back to. This keeps them from being clobbered when a concurrent request's
 * curl handle flushes its own (older) in-memory jar to the main cookie file. */
static char *
ns_net_cookie_js_path_for_partition(const char *top_origin)
{
    const char *dir = ns_net_cookie_dir();
    const char *key = (top_origin && *top_origin) ? top_origin : "default";
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
    char short_hex[33];
    g_strlcpy(short_hex, digest, sizeof(short_hex));
    g_free(digest);
    char *fname = g_strdup_printf("%s.js.txt", short_hex);
    char *path = g_build_filename(dir, fname, NULL);
    g_free(fname);
    return path;
}

char *
ns_net_cookies_for_js(const char *url)
{
    if (!url || !*url) return NULL;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(url);
    if (!parts || !parts->hostname || !*parts->hostname) return NULL;
    const char *host = parts->hostname;
    const char *path = (parts->pathname && *parts->pathname)
                       ? parts->pathname : "/";
    gboolean is_https = parts->protocol &&
                        g_ascii_strcasecmp(parts->protocol, "https:") == 0;

    g_autofree char *site = ns_url_site_from(url);
    if (!site || !*site) return NULL;
    g_autofree char *jar_path = ns_net_cookie_path_for_partition(site);
    g_autofree char *js_path  = ns_net_cookie_js_path_for_partition(site);

    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gsize hl = strlen(host);
    GPtrArray *order = g_ptr_array_new_with_free_func(g_free);
    GHashTable *vals = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);

    for (int pass = 0; pass < 2; pass++) {
        const char *fp = pass == 0 ? jar_path : js_path;
        char *contents = NULL;
        if (!fp || !g_file_get_contents(fp, &contents, NULL, NULL)) {
            g_free(contents);
            continue;
        }
        char **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = g_strchomp(lines[i]);
            if (!*line || line[0] == '#') continue;
            char **f = g_strsplit(line, "\t", 7);
            int nf = 0;
            while (f[nf]) nf++;
            if (nf < 7) { g_strfreev(f); continue; }
            const char *cdomain = f[0];
            const char *cpath   = f[2];
            gboolean csecure = g_ascii_strcasecmp(f[3], "TRUE") == 0;
            gint64 cexpiry = g_ascii_strtoll(f[4], NULL, 10);
            const char *cname = f[5];
            const char *cval  = f[6];

            gboolean match;
            if (cdomain[0] == '.') {
                gsize dl = strlen(cdomain);
                match = (hl >= dl &&
                         g_ascii_strcasecmp(host + hl - dl, cdomain) == 0) ||
                        g_ascii_strcasecmp(host, cdomain + 1) == 0;
            } else {
                match = g_ascii_strcasecmp(host, cdomain) == 0;
            }
            if (match && cpath && *cpath) {
                gsize cl = strlen(cpath);
                if (!g_str_has_prefix(path, cpath))
                    match = FALSE;
                else if (path[cl] != '\0' && path[cl] != '/' &&
                         cpath[cl - 1] != '/')
                    match = FALSE;
            }
            if (match && csecure && !is_https) match = FALSE;
            if (match && cexpiry != 0 && cexpiry < now) match = FALSE;
            if (match && cname && *cname) {
                if (!g_hash_table_contains(vals, cname))
                    g_ptr_array_add(order, g_strdup(cname));
                g_hash_table_replace(vals, g_strdup(cname),
                                     g_strdup(cval ? cval : ""));
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(contents);
    }

    GString *out = g_string_new(NULL);
    for (guint i = 0; i < order->len; i++) {
        const char *nm = g_ptr_array_index(order, i);
        const char *vv = g_hash_table_lookup(vals, nm);
        if (out->len) g_string_append(out, "; ");
        g_string_append(out, nm);
        g_string_append_c(out, '=');
        g_string_append(out, vv ? vv : "");
    }
    g_ptr_array_free(order, TRUE);
    g_hash_table_destroy(vals);
    if (out->len == 0) return g_string_free(out, TRUE), NULL;
    return g_string_free(out, FALSE);
}

void
ns_net_cookie_store_from_js(const char *url, const char *cookie)
{
    if (!url || !*url || !cookie) return;
    if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://"))
        return;
    for (const char *c = cookie; *c; c++)
        if ((unsigned char)*c < 0x20) return;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(url);
    if (!parts || !parts->hostname || !*parts->hostname) return;

    const char *semi = strchr(cookie, ';');
    const char *pair_end = semi ? semi : cookie + strlen(cookie);
    const char *eq = memchr(cookie, '=', (gsize)(pair_end - cookie));
    if (!eq || eq == cookie) return;
    const char *name = cookie;
    gsize name_len = (gsize)(eq - cookie);
    const char *value = eq + 1;
    gsize value_len = (gsize)(pair_end - value);
    while (name_len && (name[0] == ' ' || name[0] == '\t')) { name++; name_len--; }
    while (name_len && (name[name_len - 1] == ' ' || name[name_len - 1] == '\t'))
        name_len--;
    while (value_len && (value[0] == ' ' || value[0] == '\t')) { value++; value_len--; }
    while (value_len && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t'))
        value_len--;
    if (name_len == 0) return;

    char *domain_attr = NULL, *path_attr = NULL;
    gboolean secure = FALSE, has_expiry = FALSE, expired = FALSE;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 expiry = 0;
    for (const char *p = semi; p && *p; ) {
        while (*p == ';' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *e = strchr(p, ';');
        gsize alen = e ? (gsize)(e - p) : strlen(p);
        while (alen && (p[alen - 1] == ' ' || p[alen - 1] == '\t')) alen--;
        const char *aeq = memchr(p, '=', alen);
        gsize klen = aeq ? (gsize)(aeq - p) : alen;
        const char *av = aeq ? aeq + 1 : NULL;
        gsize avlen = aeq ? (gsize)(p + alen - av) : 0;
        while (avlen && (av[0] == ' ' || av[0] == '\t')) { av++; avlen--; }
        if (klen == 6 && g_ascii_strncasecmp(p, "secure", 6) == 0) {
            secure = TRUE;
        } else if (klen == 7 && g_ascii_strncasecmp(p, "max-age", 7) == 0 && aeq) {
            g_autofree char *tmp = g_strndup(av, avlen);
            gint64 ma = g_ascii_strtoll(tmp, NULL, 10);
            has_expiry = TRUE;
            if (ma <= 0) expired = TRUE;
            else expiry = now + ma;
        } else if (klen == 7 && g_ascii_strncasecmp(p, "expires", 7) == 0 && aeq &&
                   !has_expiry) {
            g_autofree char *tmp = g_strndup(av, avlen);
            time_t t = curl_getdate(tmp, NULL);
            if (t != (time_t)-1) {
                has_expiry = TRUE;
                expiry = (gint64)t;
                if (expiry <= now) expired = TRUE;
            }
        } else if (klen == 6 && g_ascii_strncasecmp(p, "domain", 6) == 0 && aeq) {
            g_free(domain_attr);
            domain_attr = g_strndup(av, avlen);
        } else if (klen == 4 && g_ascii_strncasecmp(p, "path", 4) == 0 && aeq) {
            g_free(path_attr);
            path_attr = g_strndup(av, avlen);
        }
        if (!e) break;
        p = e + 1;
    }

    gboolean is_https = parts->protocol &&
                        g_ascii_strcasecmp(parts->protocol, "https:") == 0;
    if (secure && !is_https) { g_free(domain_attr); g_free(path_attr); return; }

    if (name_len >= 9 && g_ascii_strncasecmp(name, "__Secure-", 9) == 0 &&
        !(secure && is_https)) {
        g_free(domain_attr); g_free(path_attr); return;
    }
    if (name_len >= 7 && g_ascii_strncasecmp(name, "__Host-", 7) == 0 &&
        (!secure || !is_https || (domain_attr && *domain_attr) ||
         (path_attr && *path_attr && strcmp(path_attr, "/") != 0))) {
        g_free(domain_attr); g_free(path_attr); return;
    }

    const char *host = parts->hostname;
    char *file_domain;
    const char *tail;
    if (domain_attr && *domain_attr) {
        const char *d = domain_attr[0] == '.' ? domain_attr + 1 : domain_attr;
        gsize dl = strlen(d), hl = strlen(host);
        gboolean ok = g_ascii_strcasecmp(host, d) == 0 ||
                      (hl > dl && host[hl - dl - 1] == '.' &&
                       g_ascii_strcasecmp(host + hl - dl, d) == 0);
        if (!ok || !dl) { g_free(domain_attr); g_free(path_attr); return; }
        g_autofree char *d_lower = g_ascii_strdown(d, -1);
        const psl_ctx_t *psl = psl_builtin();
        if (psl && psl_is_public_suffix(psl, d_lower)) {
            g_free(domain_attr); g_free(path_attr); return;
        }
        file_domain = g_strconcat(".", d, NULL);
        tail = "TRUE";
    } else {
        file_domain = g_strdup(host);
        tail = "FALSE";
    }
    const char *path = (path_attr && *path_attr) ? path_attr : "/";

    g_autofree char *site = ns_url_site_from(url);
    if (!site || !*site) {
        g_free(file_domain); g_free(domain_attr); g_free(path_attr);
        return;
    }
    g_autofree char *jar_path = ns_net_cookie_js_path_for_partition(site);
    g_autofree char *name_dup = g_strndup(name, name_len);

    char *contents = NULL;
    g_file_get_contents(jar_path, &contents, NULL, NULL);
    GString *out = g_string_new(NULL);
    gboolean blocked_httponly = FALSE;
    if (contents) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char *line = lines[i];
            if (!*line) continue;
            if (line[0] == '#') {
                if (g_str_has_prefix(line, "#HttpOnly_")) {
                    char **hf = g_strsplit(line + 10, "\t", 7);
                    int hn = 0;
                    while (hf[hn]) hn++;
                    if (hn >= 7 &&
                        g_ascii_strcasecmp(hf[0], file_domain) == 0 &&
                        strcmp(hf[2], path) == 0 &&
                        strcmp(hf[5], name_dup) == 0)
                        blocked_httponly = TRUE;
                    g_strfreev(hf);
                }
                g_string_append(out, line);
                g_string_append_c(out, '\n');
                continue;
            }
            char **f = g_strsplit(line, "\t", 7);
            int nf = 0;
            while (f[nf]) nf++;
            if (nf < 7) { g_strfreev(f); continue; }
            gint64 cexp = g_ascii_strtoll(f[4], NULL, 10);
            gboolean dup = g_ascii_strcasecmp(f[0], file_domain) == 0 &&
                           strcmp(f[2], path) == 0 &&
                           strcmp(f[5], name_dup) == 0;
            gboolean dead = cexp != 0 && cexp < now;
            if (!dup && !dead) {
                g_string_append(out, line);
                g_string_append_c(out, '\n');
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    if (!expired && !blocked_httponly) {
        g_autofree char *vdup = g_strndup(value, value_len);
        g_string_append_printf(out,
            "%s\t%s\t%s\t%s\t%" G_GINT64_FORMAT "\t%s\t%s\n",
            file_domain, tail, path, secure ? "TRUE" : "FALSE",
            expiry, name_dup, vdup);
    }
    if (g_file_set_contents(jar_path, out->str, out->len, NULL))
        g_chmod(jar_path, 0600);
    g_string_free(out, TRUE);
    g_free(file_domain);
    g_free(domain_attr);
    g_free(path_attr);
}

static const char *
ns_net_altsvc_path(void)
{
    if (ns_net_is_private())
        return NULL;
    return ns_net_data_path(&g_altsvc_path, "altsvc.txt");
}

static long
ns_net_http_version(void)
{
#ifdef CURL_VERSION_HTTP3
    static gsize once = 0;
    static long version = CURL_HTTP_VERSION_2TLS;
    if (g_once_init_enter(&once)) {
        const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
        if (info && (info->features & CURL_VERSION_HTTP3))
            version = CURL_HTTP_VERSION_3;
        g_once_init_leave(&once, 1);
    }
    return version;
#else
    return CURL_HTTP_VERSION_2TLS;
#endif
}

#define NS_NET_DOMAIN ns_net_error_quark()

static GQuark
ns_net_error_quark(void)
{
    return g_quark_from_static_string("nd-net-error");
}

static int
ns_xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
               curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    if (g_atomic_int_get(&g_net_aborting)) return 1;
    GCancellable *c = clientp;
    return (c && g_cancellable_is_cancelled(c)) ? 1 : 0;
}

static char *
ns_net_exe_dir(void)
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
    if (size == 0 || size > 32768) return NULL;
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
ns_net_try_ca_bundle(const char *path)
{
    if (!path || !*path) return FALSE;
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;
    g_ca_bundle = g_strdup(path);
    return TRUE;
}

static void
ns_net_resolve_ca_bundle(void)
{
    if (g_ca_bundle) return;
    const char *env = g_getenv("CURL_CA_BUNDLE");
    if (!env) env = g_getenv("SSL_CERT_FILE");
    if (ns_net_try_ca_bundle(env)) return;

    char *dir = ns_net_exe_dir();
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
            gboolean ok = ns_net_try_ca_bundle(cand);
            g_free(cand);
            if (ok) break;
        }
        g_free(dir);
        if (g_ca_bundle) return;
    }

#if defined(__ANDROID__)
    const char *android_paths[] = {
        "/system/etc/security/cacerts.pem",
        "/apex/com.android.conscrypt/cacerts.pem",
        "/data/misc/keychain/cacerts-added/cacert.pem",
        NULL,
    };
    for (int i = 0; android_paths[i]; i++)
        if (ns_net_try_ca_bundle(android_paths[i])) return;
    g_info("ns_net: no CA bundle found; the Android host app should set "
           "CURL_CA_BUNDLE to an extracted cacert.pem before ns_browser_init().");
#endif

#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__FreeBSD__) || defined(__NetBSD__)
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
        if (ns_net_try_ca_bundle(unix_paths[i])) return;
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
        if (ns_net_try_ca_bundle(mac_paths[i])) return;
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
        if (ns_net_try_ca_bundle(win_paths[i])) return;

    g_info("ns_net: no CA bundle file found; relying on "
           "CURLSSLOPT_NATIVE_CA via the Windows certificate store. "
           "If HTTPS fails, install mingw-w64-x86_64-ca-certificates or "
           "set CURL_CA_BUNDLE.");
#endif
}

static void
ns_share_lock(CURL *handle, curl_lock_data data,
              curl_lock_access access, void *user_data)
{
    (void)handle; (void)access; (void)user_data;
    if (data < CURL_LOCK_DATA_LAST)
        g_mutex_lock(&g_share_locks[data]);
}

static void
ns_share_unlock(CURL *handle, curl_lock_data data, void *user_data)
{
    (void)handle; (void)user_data;
    if (data < CURL_LOCK_DATA_LAST)
        g_mutex_unlock(&g_share_locks[data]);
}

static gpointer
ns_rng_warmup_thread(gpointer data)
{
    (void)data;
    int (*rand_bytes)(unsigned char *, int) = NULL;
    GModule *self = g_module_open(NULL, G_MODULE_BIND_LAZY);
    if (self &&
        g_module_symbol(self, "RAND_bytes", (gpointer *)&rand_bytes) &&
        rand_bytes) {
        unsigned char buf[32];
        rand_bytes(buf, (int)sizeof buf);
    }
    if (self) g_module_close(self);
    return NULL;
}

static GThread *g_rng_warmup_thread;

static void
ns_net_warm_rng(void)
{
    if (!g_module_supported()) return;
    g_rng_warmup_thread = g_thread_try_new("nd-rng-warmup",
                                           ns_rng_warmup_thread, NULL, NULL);
}

static void
ns_net_join_rng(void)
{
    if (g_rng_warmup_thread) {
        g_thread_join(g_rng_warmup_thread);
        g_rng_warmup_thread = NULL;
    }
}

void
ns_net_init(void)
{
    ns_net_resolve_ca_bundle();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
    g_has_http3 = vi && (vi->features & CURL_VERSION_HTTP3) != 0;

    unsigned ossl_major = 0, ossl_minor = 0;
    if (vi && vi->ssl_version &&
        sscanf(vi->ssl_version, "OpenSSL/%u.%u", &ossl_major, &ossl_minor) == 2 &&
        (ossl_major > 3 || (ossl_major == 3 && ossl_minor >= 5)))
        g_ec_curves = "X25519MLKEM768:X25519:P-256:P-384";

    GString *enc = g_string_new(NULL);
    if (vi && (vi->features & CURL_VERSION_LIBZ) != 0)
        g_string_append(enc, "gzip, deflate");
#ifdef CURL_VERSION_BROTLI
    if (vi && (vi->features & CURL_VERSION_BROTLI) != 0) {
        if (enc->len) g_string_append(enc, ", ");
        g_string_append(enc, "br");
    }
#endif
#ifdef CURL_VERSION_ZSTD
    if (vi && (vi->features & CURL_VERSION_ZSTD) != 0) {
        if (enc->len) g_string_append(enc, ", ");
        g_string_append(enc, "zstd");
    }
#endif
    g_free(g_accept_encoding);
    g_accept_encoding = g_string_free(enc, FALSE);

    g_share = curl_share_init();
    if (g_share) {
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
#ifdef CURL_LOCK_DATA_CONNECT
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
#endif
#ifdef CURL_LOCK_DATA_PSL
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_PSL);
#endif
#ifdef CURL_LOCK_DATA_HSTS
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_HSTS);
#endif
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC,   ns_share_lock);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, ns_share_unlock);
    }

    ns_net_warm_rng();

    ns_net_hsts_curl_path();
    ns_net_altsvc_path();
    ns_net_cookie_dir();
}

gboolean
ns_net_idle(void)
{
    g_mutex_lock(&g_fetch_throttle_mutex);
    gboolean idle = g_fetch_active == 0 && g_preconnect_active == 0;
    g_mutex_unlock(&g_fetch_throttle_mutex);
    return idle;
}

static gboolean
ns_net_drain(int timeout_ms)
{
    g_atomic_int_set(&g_net_aborting, 1);
    g_mutex_lock(&g_fetch_throttle_mutex);
    for (GTask *t; (t = g_queue_pop_head(&g_fetch_queue)); ) {
        g_task_return_new_error(t, NS_NET_DOMAIN, 1, "shutting down");
        g_object_unref(t);
    }
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    gboolean drained = TRUE;
    while (g_fetch_active > 0 || g_preconnect_active > 0) {
        if (!g_cond_wait_until(&g_fetch_idle_cond, &g_fetch_throttle_mutex,
                               deadline)) {
            drained = g_fetch_active == 0 && g_preconnect_active == 0;
            break;
        }
    }
    g_mutex_unlock(&g_fetch_throttle_mutex);
    return drained;
}

void
ns_net_shutdown(void)
{
    ns_net_join_rng();
    ns_net_multi_shutdown();
    if (!ns_net_drain(3000))
        return;
    if (g_conn_stats) {
        g_hash_table_destroy(g_conn_stats);
        g_conn_stats = NULL;
    }
    if (g_share) { curl_share_cleanup(g_share); g_share = NULL; }
    curl_global_cleanup();
    g_free(g_accept_encoding);
    g_accept_encoding = NULL;
    g_free(g_proxy_override);
    g_proxy_override = NULL;
    g_free(g_cookie_dir);
    g_cookie_dir = NULL;
    g_free(g_hsts_curl_path);
    g_hsts_curl_path = NULL;
    g_free(g_altsvc_path);
    g_altsvc_path = NULL;
    if (g_private_root) {
        ns_rmrf(g_private_root);
        g_free(g_private_root);
        g_private_root = NULL;
    }
    g_free(g_ca_bundle);
    g_ca_bundle = NULL;
    if (g_hsts_cache) {
        g_hash_table_destroy(g_hsts_cache);
        g_hsts_cache = NULL;
    }
    if (g_origin_slots) {
        g_hash_table_destroy(g_origin_slots);
        g_origin_slots = NULL;
    }
}

void
ns_net_set_proxy_override(const char *proxy_url)
{
    g_free(g_proxy_override);
    g_proxy_override = (proxy_url && *proxy_url) ? g_strdup(proxy_url) : NULL;
}

static gboolean g_allow_file_urls = FALSE;

void
ns_net_set_allow_file_urls(gboolean allow)
{
    g_allow_file_urls = allow;
}

static gboolean g_log_fetches = FALSE;

void
ns_net_set_log_fetches(gboolean on)
{
    g_log_fetches = on;
}

typedef struct ns_conn_stat {
    guint64 requests;
    guint64 connections;
} ns_conn_stat;

static const char *
ns_net_http_version_name(long v)
{
    switch (v) {
    case CURL_HTTP_VERSION_1_0: return "http/1.0";
    case CURL_HTTP_VERSION_1_1: return "http/1.1";
    case CURL_HTTP_VERSION_2_0: return "h2";
#ifdef CURL_HTTP_VERSION_3
    case CURL_HTTP_VERSION_3:   return "h3";
#endif
    default:                    return "http/?";
    }
}

static GMutex   g_perf_lock;
static guint64  g_perf_fetch_count;
static guint64  g_perf_fetch_bytes;
static gint64   g_perf_fetch_sum_us;
static gint64   g_perf_fetch_first_us;
static gint64   g_perf_fetch_last_us;

static void
ns_net_perf_record(gint64 start_us, gint64 end_us, guint64 bytes)
{
    if (!g_log_fetches) return;
    g_mutex_lock(&g_perf_lock);
    if (g_perf_fetch_count == 0 || start_us < g_perf_fetch_first_us)
        g_perf_fetch_first_us = start_us;
    if (end_us > g_perf_fetch_last_us)
        g_perf_fetch_last_us = end_us;
    g_perf_fetch_count++;
    g_perf_fetch_bytes += bytes;
    g_perf_fetch_sum_us += end_us - start_us;
    g_mutex_unlock(&g_perf_lock);
}

void
ns_net_perf_snapshot(guint64 *fetches, guint64 *bytes,
                     double *sum_ms, double *span_ms)
{
    g_mutex_lock(&g_perf_lock);
    if (fetches) *fetches = g_perf_fetch_count;
    if (bytes)   *bytes   = g_perf_fetch_bytes;
    if (sum_ms)  *sum_ms  = g_perf_fetch_sum_us / 1000.0;
    if (span_ms) *span_ms = g_perf_fetch_count
                            ? (g_perf_fetch_last_us - g_perf_fetch_first_us) / 1000.0
                            : 0.0;
    g_mutex_unlock(&g_perf_lock);
}

static void
ns_net_conn_stat_record(const char *url, long http_version, long new_connections)
{
    if (!g_log_fetches) return;
    char *origin = ns_url_origin_from(url);
    if (!origin) return;

    g_mutex_lock(&g_conn_stats_lock);
    if (!g_conn_stats)
        g_conn_stats = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
    ns_conn_stat *s = g_hash_table_lookup(g_conn_stats, origin);
    if (!s) {
        s = g_new0(ns_conn_stat, 1);
        g_hash_table_insert(g_conn_stats, g_strdup(origin), s);
    }
    s->requests++;
    if (new_connections > 0)
        s->connections += (guint64)new_connections;
    guint64 reqs = s->requests, conns = s->connections;
    g_mutex_unlock(&g_conn_stats_lock);

    ns_debug_log_emit(NS_DLOG_NET, "conn", "%s new=%ld origin=%s reqs=%"
                      G_GUINT64_FORMAT " conns=%" G_GUINT64_FORMAT,
                      ns_net_http_version_name(http_version),
                      new_connections, origin, reqs, conns);
    g_free(origin);
}

static const char *
ns_net_pick_configured_proxy(const char *url)
{
    if (g_proxy_override && *g_proxy_override) return g_proxy_override;
    const ns_config *cfg = ns_config_get();
    if (!cfg) return NULL;
    gboolean https = g_str_has_prefix(url, "https://") ||
                     g_str_has_prefix(url, "wss://");
    if (https && cfg->https_proxy && *cfg->https_proxy) return cfg->https_proxy;
    if (cfg->http_proxy && *cfg->http_proxy)            return cfg->http_proxy;
    return NULL;
}

static const char *
ns_net_configured_no_proxy(void)
{
    const ns_config *cfg = ns_config_get();
    if (cfg && cfg->no_proxy && *cfg->no_proxy) return cfg->no_proxy;
    return NULL;
}

void
ns_net_apply_curl_proxy(void *curl_handle, const char *url)
{
    CURL *curl = curl_handle;
    const char *proxy = ns_net_pick_configured_proxy(url);
    if (proxy && *proxy)
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
    const char *no_proxy = ns_net_configured_no_proxy();
    if (no_proxy && *no_proxy)
        curl_easy_setopt(curl, CURLOPT_NOPROXY, no_proxy);
}

const char *
ns_net_proxy_override(void)
{
    return g_proxy_override;
}

const char *
ns_net_http_proxy(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg ? cfg->http_proxy : NULL;
}

const char *
ns_net_https_proxy(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg ? cfg->https_proxy : NULL;
}

const char *
ns_net_no_proxy(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg ? cfg->no_proxy : NULL;
}

const char *
ns_net_ca_bundle_path(void)
{
    return g_ca_bundle;
}

void
ns_net_apply_curl_tls(void *curl_handle)
{
    CURL *curl = curl_handle;
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST,
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:"
        "AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA:AES256-SHA");
#ifdef CURLOPT_TLS13_CIPHERS
    curl_easy_setopt(curl, CURLOPT_TLS13_CIPHERS,
        "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
        "TLS_CHACHA20_POLY1305_SHA256");
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_EC_CURVES, g_ec_curves);
#if LIBCURL_VERSION_NUM >= 0x080800
    {
        const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
        const char *const *feat = info ? info->feature_names : NULL;
        for (; feat && *feat; feat++) {
            if (!g_ascii_strcasecmp(*feat, "ECH")) {
                curl_easy_setopt(curl, CURLOPT_ECH, "true");
                break;
            }
        }
    }
#endif
    if (g_ca_bundle)
        curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_bundle);
#ifdef G_OS_WIN32
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
#ifdef CURLOPT_DOH_URL
    const ns_config *cfg = ns_config_get();
    if (cfg && cfg->doh_url && g_str_has_prefix(cfg->doh_url, "https://"))
        curl_easy_setopt(curl, CURLOPT_DOH_URL, cfg->doh_url);
#endif
}

void
ns_response_free(ns_response *resp)
{
    if (!resp)
        return;
    g_free(resp->final_url);
    g_free(resp->content_type);
    g_free(resp->content_disposition);
    g_free(resp->csp_header);
    g_free(resp->xframe_options);
    g_free(resp->x_content_type_options);
    g_free(resp->cors_allow_origin);
    g_free(resp->refresh);
    g_free(resp->content_language);
    g_free(resp->raw_headers);
    if (resp->body)
        g_byte_array_unref(resp->body);
    g_free(resp->error);
    g_free(resp->tls_warning);
    g_free(resp->remote_ip);
    g_free(resp);
}

typedef struct {
    char   *method;
    char   *url;
    long    status;
    char   *content_type;
    guint64 body_len;
    double  duration_ms;
    char   *req_headers;
    char   *resp_headers;
    char   *error;
} ns_net_log_entry;

#define NS_NET_LOG_CAP 256

static GMutex      ns_net_log_lock;
static GPtrArray  *ns_net_log;

static void
ns_net_log_entry_free(gpointer data)
{
    ns_net_log_entry *e = data;
    if (!e)
        return;
    g_free(e->method);
    g_free(e->url);
    g_free(e->content_type);
    g_free(e->req_headers);
    g_free(e->resp_headers);
    g_free(e->error);
    g_free(e);
}

static void
ns_net_log_record(const char *method, const char *url, long status,
                  const char *content_type, guint64 body_len,
                  double duration_ms, const char *req_headers,
                  const char *resp_headers, const char *error)
{
    if (!url || !*url)
        return;
    if (g_str_has_prefix(url, "data:") || g_str_has_prefix(url, "about:"))
        return;
    ns_net_log_entry *e = g_new0(ns_net_log_entry, 1);
    e->method = g_strdup(method && *method ? method : "GET");
    e->url = g_strdup(url);
    e->status = status;
    e->content_type = g_strdup(content_type ? content_type : "");
    e->body_len = body_len;
    e->duration_ms = duration_ms;
    e->req_headers = g_strdup(req_headers ? req_headers : "");
    e->resp_headers = g_strdup(resp_headers ? resp_headers : "");
    e->error = error && *error ? g_strdup(error) : NULL;

    g_mutex_lock(&ns_net_log_lock);
    if (!ns_net_log)
        ns_net_log = g_ptr_array_new_with_free_func(ns_net_log_entry_free);
    if (ns_net_log->len >= NS_NET_LOG_CAP)
        g_ptr_array_remove_index(ns_net_log, 0);
    g_ptr_array_add(ns_net_log, e);
    g_mutex_unlock(&ns_net_log_lock);
}

void
ns_net_log_clear(void)
{
    g_mutex_lock(&ns_net_log_lock);
    if (ns_net_log)
        g_ptr_array_set_size(ns_net_log, 0);
    g_mutex_unlock(&ns_net_log_lock);
}

static void
ns_net_log_append_headers(GString *out, const char *headers)
{
    if (!headers || !*headers)
        return;
    char **lines = g_strsplit(headers, "\n", -1);
    for (guint i = 0; lines && lines[i]; i++) {
        char *line = g_strchomp(lines[i]);
        if (*line)
            g_string_append_printf(out, "    %s\n", line);
    }
    g_strfreev(lines);
}

char *
ns_net_log_dump(void)
{
    GString *out = g_string_new(NULL);
    g_mutex_lock(&ns_net_log_lock);
    guint n = ns_net_log ? ns_net_log->len : 0;
    g_string_append_printf(out, "%u network request%s\n\n", n,
                           n == 1 ? "" : "s");
    for (guint i = 0; i < n; i++) {
        ns_net_log_entry *e = g_ptr_array_index(ns_net_log, i);
        if (e->status > 0)
            g_string_append_printf(out, "[%ld] %s %s\n", e->status,
                                   e->method, e->url);
        else
            g_string_append_printf(out, "[---] %s %s\n", e->method, e->url);
        g_string_append_printf(out, "    %.0f ms, %llu bytes",
                               e->duration_ms,
                               (unsigned long long)e->body_len);
        if (e->content_type && *e->content_type)
            g_string_append_printf(out, ", %s", e->content_type);
        g_string_append_c(out, '\n');
        if (e->error)
            g_string_append_printf(out, "    error: %s\n", e->error);
        if (e->req_headers && *e->req_headers) {
            g_string_append(out, "  Request headers:\n");
            ns_net_log_append_headers(out, e->req_headers);
        }
        if (e->resp_headers && *e->resp_headers) {
            g_string_append(out, "  Response headers:\n");
            ns_net_log_append_headers(out, e->resp_headers);
        }
        g_string_append_c(out, '\n');
    }
    g_mutex_unlock(&ns_net_log_lock);
    return g_string_free(out, FALSE);
}

static char *
ns_net_slist_serialize(struct curl_slist *list)
{
    if (!list)
        return NULL;
    GString *out = g_string_new(NULL);
    for (struct curl_slist *n = list; n; n = n->next) {
        if (!n->data)
            continue;
        if (g_str_has_prefix(n->data, "X-ND-"))
            continue;
        g_string_append(out, n->data);
        g_string_append_c(out, '\n');
    }
    return g_string_free(out, FALSE);
}

#define NS_NET_RESPONSE_MIN_BUDGET (64ULL * 1024ULL * 1024ULL)
#define NS_NET_RESPONSE_RECHECK_BYTES (16ULL * 1024ULL * 1024ULL)
#define NS_NET_MAX_RAW_HEADER_BYTES   (1ULL * 1024ULL * 1024ULL)

static guint64
ns_net_available_memory_bytes(void)
{
#if defined(G_OS_WIN32)
    MEMORYSTATUSEX m = { .dwLength = sizeof(m) };
    if (GlobalMemoryStatusEx(&m))
        return (guint64)m.ullAvailPhys;
#elif defined(__linux__)
    FILE *f = fopen("/proc/meminfo", "re");
    if (f) {
        char line[256];
        guint64 kb = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemAvailable: %" G_GUINT64_FORMAT " kB", &kb) == 1) {
                fclose(f);
                return kb * 1024ULL;
            }
        }
        fclose(f);
    }
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof mem;
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0 && mem > 0)
        return (guint64)mem;
#elif defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0)
        return (guint64)pages * (guint64)psize;
#endif
    return 0;
}

static guint64
ns_net_response_budget(void)
{
    guint64 avail = ns_net_available_memory_bytes();
    if (avail == 0) return NS_NET_RESPONSE_MIN_BUDGET;
    guint64 half = avail / 2;
    return half < NS_NET_RESPONSE_MIN_BUDGET ? NS_NET_RESPONSE_MIN_BUDGET : half;
}

typedef struct ns_write_ctx {
    GByteArray *body;
    guint64     total;
    guint64     budget;
    guint64     next_recheck;
    gboolean    exceeded;
} ns_write_ctx;

static size_t
ns_write_cb(char *data, size_t size, size_t nmemb, void *userdata)
{
    ns_write_ctx *ctx = userdata;
    if (size != 0 && nmemb > G_MAXSIZE / size)
        return 0;
    size_t bytes = size * nmemb;

    if (bytes == 0)
        return 0;
    if (bytes > G_MAXUINT)
        return 0;
    if (ctx->total >= ctx->next_recheck) {
        ctx->budget = ns_net_response_budget();
        ctx->next_recheck = ctx->total + NS_NET_RESPONSE_RECHECK_BYTES;
    }
    if (ctx->total + bytes > ctx->budget) {
        ctx->exceeded = TRUE;
        return 0;
    }
    if (ctx->total + bytes > G_MAXUINT) {
        ctx->exceeded = TRUE;
        return 0;
    }
    g_byte_array_append(ctx->body, (const guint8 *)data, bytes);
    ctx->total += bytes;
    return bytes;
}

typedef struct ns_header_ctx {
    char **content_type_out;
    char **content_disposition_out;
    char **csp_out;
    char **xframe_options_out;
    char **x_content_type_options_out;
    char **cors_allow_origin_out;
    char **refresh_out;
    char **content_language_out;
    char  *etag;
    char  *last_modified;
    char  *cache_control;
    char  *expires;
    char  *location;
    GString *raw;
    gboolean set_cookie_seen;
} ns_header_ctx;

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

static gboolean
header_append(const char *buffer, size_t bytes,
              const char *name, char **slot)
{
    size_t name_len = strlen(name);
    if (bytes < name_len ||
        g_ascii_strncasecmp(buffer, name, name_len) != 0)
        return FALSE;
    if (slot) {
        char *val = header_value_dup(buffer, bytes, name_len);
        if (*slot && **slot && val && *val) {
            char *joined = g_strconcat(*slot, ", ", val, NULL);
            g_free(*slot);
            g_free(val);
            *slot = joined;
        } else if (val && *val) {
            g_free(*slot);
            *slot = val;
        } else {
            g_free(val);
        }
    }
    return TRUE;
}

static size_t
ns_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    ns_header_ctx *hc = userdata;
    if (size != 0 && nitems > G_MAXSIZE / size)
        return 0;
    size_t bytes = size * nitems;

    if (bytes >= 5 && g_ascii_strncasecmp(buffer, "HTTP/", 5) == 0) {
        if (hc->raw) g_string_set_size(hc->raw, 0);
    } else if (bytes > 2) {
        gboolean set_cookie =
            (bytes >= 11 && g_ascii_strncasecmp(buffer, "Set-Cookie:", 11) == 0) ||
            (bytes >= 12 && g_ascii_strncasecmp(buffer, "Set-Cookie2:", 12) == 0);
        if (!set_cookie) {
            if (!hc->raw) hc->raw = g_string_new(NULL);
            if (hc->raw->len + bytes <= NS_NET_MAX_RAW_HEADER_BYTES)
                g_string_append_len(hc->raw, buffer, bytes);
        }
    }

    if      (header_capture(buffer, bytes, "Content-Type:",    hc->content_type_out))         {}
    else if (header_capture(buffer, bytes, "ETag:",            &hc->etag))                    {}
    else if (header_capture(buffer, bytes, "Last-Modified:",   &hc->last_modified))           {}
    else if (header_capture(buffer, bytes, "Cache-Control:",   &hc->cache_control))           {}
    else if (header_capture(buffer, bytes, "Expires:",         &hc->expires))                 {}
    else if (header_append(buffer, bytes, "Content-Security-Policy:",
                            hc->csp_out))                                                     {}
    else if (header_capture(buffer, bytes, "X-Frame-Options:", hc->xframe_options_out))       {}
    else if (header_capture(buffer, bytes, "X-Content-Type-Options:",
                            hc->x_content_type_options_out))                                  {}
    else if (header_capture(buffer, bytes, "Access-Control-Allow-Origin:",
                            hc->cors_allow_origin_out))                                       {}
    else if (header_capture(buffer, bytes, "Content-Disposition:",
                            hc->content_disposition_out))                                     {}
    else if (header_capture(buffer, bytes, "Content-Language:", hc->content_language_out))      {}
    else if (header_capture(buffer, bytes, "Refresh:", hc->refresh_out))                       {}
    else if (header_capture(buffer, bytes, "Location:", &hc->location))                        {}
    else if (header_capture(buffer, bytes, "Set-Cookie:", NULL))
        hc->set_cookie_seen = TRUE;

    return bytes;
}

extern const char *ns_app_self_exe(void);

static char *
about_read_first(const char *const *rel_paths, gsize *out_len)
{
    const char *exe = ns_app_self_exe();
    char *exe_dir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    const char *user_data_dir = g_get_user_data_dir();
    const char *const *system_data_dirs = g_get_system_data_dirs();
    char *contents = NULL;
    gsize len = 0;
    for (int i = 0; rel_paths[i]; i++) {
        char *path = g_build_filename(exe_dir, rel_paths[i], NULL);
        gboolean ok = g_file_get_contents(path, &contents, &len, NULL);
        g_free(path);
        if (ok) break;
        if (user_data_dir && *user_data_dir) {
            path = g_build_filename(user_data_dir, rel_paths[i], NULL);
            ok = g_file_get_contents(path, &contents, &len, NULL);
            g_free(path);
            if (ok) break;
        }
        for (int j = 0; system_data_dirs && system_data_dirs[j]; j++) {
            path = g_build_filename(system_data_dirs[j], rel_paths[i], NULL);
            ok = g_file_get_contents(path, &contents, &len, NULL);
            g_free(path);
            if (ok) break;
        }
        if (ok) break;
    }
    g_free(exe_dir);
    if (out_len) *out_len = contents ? len : 0;
    return contents;
}

static const char *
about_logo_data_uri(void)
{
    static char *cached = NULL;
    if (cached) return cached;

    static const char *const gif_paths[] = {
        "share/icons/hicolor/scalable/apps/nordstjernen.gif",
        "../share/icons/hicolor/scalable/apps/nordstjernen.gif",
        "../../data/icons/hicolor/scalable/apps/nordstjernen.gif",
        "data/icons/hicolor/scalable/apps/nordstjernen.gif",
        NULL,
    };
    gsize gif_len = 0;
    char *gif = about_read_first(gif_paths, &gif_len);
    if (gif) {
        gchar *b64 = g_base64_encode((const guchar *)gif, gif_len);
        g_free(gif);
        cached = g_strconcat("data:image/gif;base64,", b64, NULL);
        g_free(b64);
        return cached;
    }

    cached = g_strconcat("data:image/gif;base64,", about_logo_gif_b64, NULL);
    return cached;
}

static char *
about_logo_markup(void)
{
    return g_strdup_printf("<img class=\"mark-img\" src=\"%s\" alt=\"\" "
                           "aria-hidden=\"true\">",
                           about_logo_data_uri());
}

static char *
about_splash_markup(void)
{
    char *uri = g_strconcat("data:image/gif;base64,", about_splash_gif_b64, NULL);
    char *markup = g_strdup_printf(
        "<img class=\"splash\" src=\"%s\" "
        "alt=\"Nordstjernen " NS_VERSION " splash\" "
        "style=\"display:block;width:auto;max-width:96%%;height:auto;"
        "margin:2px auto 36px;border-radius:14px;\">",
        uri);
    g_free(uri);
    return markup;
}

static char *
about_substitute(const char *template_text,
                 const char *placeholder, const char *value)
{
    char **parts = g_strsplit(template_text, placeholder, -1);
    char *joined = g_strjoinv(value, parts);
    g_strfreev(parts);
    return joined;
}

static void
diag_kv(GString *s, const char *key, const char *value)
{
    char *ev = g_markup_escape_text((value && *value) ? value : "\xe2\x80\x94",
                                    -1);
    g_string_append_printf(s,
        "<div class=\"drow\"><span class=\"dk\">%s</span>"
        "<span class=\"dv\">%s</span></div>", key, ev);
    g_free(ev);
}

static void
diag_feature(GString *s, const char *key, gboolean on)
{
    g_string_append_printf(s,
        "<div class=\"drow\"><span class=\"dk\">%s</span>"
        "<span class=\"dv %s\">%s</span></div>",
        key, on ? "on" : "off", on ? "Enabled" : "Not built");
}

static char *
about_diagnostics_html(void)
{
    const char *platform =
#if defined(G_OS_WIN32)
        "Windows";
#elif defined(__APPLE__)
        "macOS";
#elif defined(__linux__)
        "Linux";
#else
        "Unknown";
#endif
    const char *arch =
#if defined(__x86_64__) || defined(_M_X64)
        "x86-64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        "arm64";
#elif defined(__i386__) || defined(_M_IX86)
        "x86";
#elif defined(__arm__)
        "arm";
#else
        "unknown";
#endif

    GString *s = g_string_new("<div class=\"diag\">");

    g_string_append(s, "<h3>System</h3>");
    char *os = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
    if (!os) os = g_get_os_info(G_OS_INFO_KEY_NAME);
    diag_kv(s, "Operating system", os ? os : platform);
    g_free(os);
    diag_kv(s, "Platform", platform);
    diag_kv(s, "Architecture", arch);
    char *cores = g_strdup_printf("%u", g_get_num_processors());
    diag_kv(s, "Logical CPUs", cores);
    g_free(cores);

    g_string_append(s, "<h3>Version &amp; libraries</h3>");
    diag_kv(s, "Nordstjernen", NS_VERSION " (built " NS_BUILD_DATE ")");
    diag_kv(s, "JavaScript (QuickJS)", JS_GetVersion());
#ifdef NS_LEXBOR_VERSION
    diag_kv(s, "HTML / CSS (lexbor)", NS_LEXBOR_VERSION);
#endif
    char *glibv = g_strdup_printf("%u.%u.%u", glib_major_version,
                                  glib_minor_version, glib_micro_version);
    diag_kv(s, "GLib", glibv);
    g_free(glibv);
    diag_kv(s, "Pango", pango_version_string());
    diag_kv(s, "Cairo", cairo_version_string());
    diag_kv(s, "SQLite", sqlite3_libversion());
    int wv = WebPGetDecoderVersion();
    char *webpv = g_strdup_printf("%d.%d.%d", (wv >> 16) & 0xff,
                                  (wv >> 8) & 0xff, wv & 0xff);
    diag_kv(s, "libwebp", webpv);
    g_free(webpv);
#ifdef NS_HAVE_LIBAV
    diag_kv(s, "Video (FFmpeg libav*)", av_version_info());
#endif
    diag_kv(s, "TLS / crypto", OpenSSL_version(OPENSSL_VERSION));
    diag_kv(s, "Networking", curl_version());

    g_string_append(s, "<h3>Features</h3>");
#ifdef NS_ENABLE_WEBGL
    diag_feature(s, "WebGL (3D canvas)", TRUE);
#else
    diag_feature(s, "WebGL (3D canvas)", FALSE);
#endif
#ifdef ND_HAVE_WEBGPU
    diag_kv(s, "WebGPU (experimental)",
            g_getenv("NS_WEBGPU_ALLOW")
                ? "Enabled (--enable-webgpu)"
                : "Built \xe2\x80\x94 start with --enable-webgpu");
#else
    diag_feature(s, "WebGPU (experimental)", FALSE);
#endif
#ifdef NS_HAVE_AI
    diag_feature(s, "Local AI assistant", TRUE);
#else
    diag_feature(s, "Local AI assistant", FALSE);
#endif
#ifdef NS_HAVE_LIBAV
    diag_feature(s, "WebM video (VP8 / VP9 / Opus)", TRUE);
#else
    diag_feature(s, "WebM video (VP8 / VP9 / Opus)", FALSE);
#endif
#ifdef NS_HAVE_AVIF
    diag_feature(s, "AVIF images", TRUE);
#else
    diag_feature(s, "AVIF images", FALSE);
#endif
#ifdef NS_HAVE_LIBRSVG
    diag_feature(s, "SVG images (librsvg)", TRUE);
#else
    diag_feature(s, "SVG images (librsvg)", FALSE);
#endif
#ifdef NS_HAVE_POPPLER
    diag_feature(s, "Inline PDF viewer", TRUE);
#else
    diag_feature(s, "Inline PDF viewer", FALSE);
#endif
#ifdef NS_HAVE_ENCHANT
    diag_feature(s, "Spell checking", TRUE);
#else
    diag_feature(s, "Spell checking", FALSE);
#endif
#ifdef NS_HAVE_SECCOMP
    diag_feature(s, "Seccomp sandbox", TRUE);
#else
    diag_feature(s, "Seccomp sandbox", FALSE);
#endif

    g_string_append(s, "</div>");
    return g_string_free(s, FALSE);
}

static char *
build_about_markdown_page(const char *const *paths, const char *title,
                          const char *back_href, const char *back_label,
                          const char *missing_file)
{
    char *text = about_read_first(paths, NULL);
    if (!text) {
        return g_strdup_printf(
            "<!doctype html><meta charset=utf-8>"
            "<title>%s</title>"
            "<p>%s is missing from the install \xe2\x80\x94 reinstall the "
            "package or copy <code>%s</code> next to the binary.</p>",
            title, missing_file, missing_file);
    }
    char *escaped = ns_html_escape_text(text);
    g_free(text);
    char *html = g_strconcat(
        "<!doctype html><html><head>"
        "<meta charset=\"utf-8\">"
        "<title>", title, "</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,\"Segoe UI\","
        "Helvetica,Arial,sans-serif;max-width:780px;margin:2em auto;"
        "padding:0 24px;color:#111;line-height:1.5}"
        "pre{white-space:pre-wrap;word-wrap:break-word;"
        "font-family:ui-monospace,\"SF Mono\",Menlo,Consolas,monospace;"
        "font-size:0.95em;background:#f7f7f9;border:1px solid #e3e3e8;"
        "border-radius:6px;padding:1em 1.2em}"
        ".nav{color:#666;font-size:0.9em;margin:0 0 1.5em 0}"
        ".nav a{color:#3a63d0}"
        "</style></head><body>"
        "<p class=\"nav\"><a href=\"", back_href, "\">"
        "&larr; ", back_label, "</a></p>"
        "<h1>", title, "</h1>"
        "<pre>", escaped, "</pre>"
        "</body></html>", NULL);
    g_free(escaped);
    return html;
}

static char *
build_about_license(void)
{
    static const char *const paths[] = {
        "nordstjernen/License.md",
        "share/nordstjernen/License.md",
        "../share/nordstjernen/License.md",
        "../../../License.md",
        "../../License.md",
        "License.md",
        NULL,
    };
    return build_about_markdown_page(paths, "Nordstjernen Source License",
                                     "about:nordstjernen", "About Nordstjernen",
                                     "License.md");
}

static char *
build_about_third_party(void)
{
    static const char *const paths[] = {
        "nordstjernen/THIRD-PARTY-LICENSES.md",
        "share/nordstjernen/THIRD-PARTY-LICENSES.md",
        "../share/nordstjernen/THIRD-PARTY-LICENSES.md",
        "../../../THIRD-PARTY-LICENSES.md",
        "../../THIRD-PARTY-LICENSES.md",
        "THIRD-PARTY-LICENSES.md",
        NULL,
    };
    return build_about_markdown_page(paths, "Third-party software notices",
                                     "about:nordstjernen", "About Nordstjernen",
                                     "THIRD-PARTY-LICENSES.md");
}

typedef struct ns_error_info {
    const char *icon;
    const char *title;
    const char *heading;
    const char *summary;
} ns_error_info;

static const ns_error_info *
classify_error(long status, const char *transport_error)
{
    static const ns_error_info NO_NETWORK = {
        "📡",
        "Can't reach the network",
        "Can't reach the network",
        "Nordstjernen couldn't connect to any server. Your device may be "
        "offline, or a firewall is blocking outbound traffic."
    };
    static const ns_error_info DNS = {
        "🔍",
        "Server address not found",
        "Server address not found",
        "Nordstjernen couldn't look up the host name. The address may be "
        "mistyped, or your DNS resolver isn't responding."
    };
    static const ns_error_info REFUSED = {
        "🚫",
        "Server refused the connection",
        "Server refused the connection",
        "The host is reachable but no service is listening on that port, "
        "or it actively closed the connection."
    };
    static const ns_error_info TIMEOUT = {
        "⌛",
        "The connection timed out",
        "The connection timed out",
        "The server didn't respond within the allowed time. It may be "
        "overloaded or temporarily unreachable."
    };
    static const ns_error_info TLS = {
        "🔒",
        "Secure connection failed",
        "Secure connection failed",
        "Nordstjernen couldn't establish a trustworthy TLS connection. "
        "The certificate may be invalid, expired, or self-signed."
    };
    static const ns_error_info BAD_URL = {
        "📝",
        "That address looks malformed",
        "That address looks malformed",
        "The URL couldn't be parsed. Check for typos, missing slashes, "
        "or an unsupported scheme."
    };
    static const ns_error_info HTTP_404 = {
        "🗺",
        "Page not found",
        "Page not found",
        "The server is reachable, but it has no resource at that URL. "
        "The link may be outdated or the page may have moved."
    };
    static const ns_error_info HTTP_410 = {
        "🪦",
        "This page is gone",
        "This page is gone",
        "The server is telling us the resource has been permanently removed."
    };
    static const ns_error_info HTTP_401 = {
        "🔐",
        "Authentication required",
        "Authentication required",
        "The server needs credentials Nordstjernen doesn't have. Sign in "
        "elsewhere first, or try a different URL."
    };
    static const ns_error_info HTTP_403 = {
        "🚪",
        "Access denied",
        "Access denied",
        "The server understood the request but refused to share this "
        "resource with us."
    };
    static const ns_error_info HTTP_429 = {
        "🌊",
        "Too many requests",
        "Too many requests",
        "The server is throttling us. Wait a moment and try again."
    };
    static const ns_error_info HTTP_500 = {
        "💥",
        "Server error",
        "Server error",
        "The server hit an internal error processing this request. "
        "Nothing to do on our end — try again later."
    };
    static const ns_error_info HTTP_502 = {
        "🪢",
        "Bad gateway",
        "Bad gateway",
        "An upstream server returned an invalid response. The site's "
        "infrastructure may be misconfigured."
    };
    static const ns_error_info HTTP_503 = {
        "🛠",
        "Service unavailable",
        "Service unavailable",
        "The server is temporarily refusing requests, usually because it "
        "is overloaded or down for maintenance."
    };
    static const ns_error_info HTTP_504 = {
        "⏱",
        "Gateway timeout",
        "Gateway timeout",
        "An upstream server didn't answer in time."
    };
    static const ns_error_info HTTP_GENERIC_4XX = {
        "⚠",
        "Request rejected",
        "Request rejected",
        "The server didn't accept this request."
    };
    static const ns_error_info HTTP_GENERIC_5XX = {
        "⚠",
        "Server error",
        "Server error",
        "The server reported a failure handling this request."
    };
    static const ns_error_info GENERIC = {
        "⚠",
        "Couldn't load page",
        "Couldn't load page",
        "Something went wrong fetching this URL."
    };

    if (transport_error && *transport_error) {
        const char *e = transport_error;
        if (g_strstr_len(e, -1, "Could not resolve") ||
            g_strstr_len(e, -1, "resolve host") ||
            g_strstr_len(e, -1, "name resolution"))
            return &DNS;
        if (g_strstr_len(e, -1, "Connection refused") ||
            g_strstr_len(e, -1, "refused"))
            return &REFUSED;
        if (g_strstr_len(e, -1, "imed out") ||
            g_strstr_len(e, -1, "Timeout"))
            return &TIMEOUT;
        if (g_strstr_len(e, -1, "SSL") ||
            g_strstr_len(e, -1, "TLS") ||
            g_strstr_len(e, -1, "certificate"))
            return &TLS;
        if (g_strstr_len(e, -1, "URL") ||
            g_strstr_len(e, -1, "Protocol") ||
            g_strstr_len(e, -1, "malformed"))
            return &BAD_URL;
        if (g_strstr_len(e, -1, "network") ||
            g_strstr_len(e, -1, "unreachable") ||
            g_strstr_len(e, -1, "No route"))
            return &NO_NETWORK;
        return &NO_NETWORK;
    }

    switch (status) {
    case 401: return &HTTP_401;
    case 403: return &HTTP_403;
    case 404: return &HTTP_404;
    case 410: return &HTTP_410;
    case 429: return &HTTP_429;
    case 500: return &HTTP_500;
    case 502: return &HTTP_502;
    case 503: return &HTTP_503;
    case 504: return &HTTP_504;
    }
    if (status >= 500 && status < 600) return &HTTP_GENERIC_5XX;
    if (status >= 400 && status < 500) return &HTTP_GENERIC_4XX;
    return &GENERIC;
}

char *
ns_build_error_page(const char *url, long status, const char *transport_error)
{
    const ns_error_info *info = classify_error(status, transport_error);
    const char *safe_url = url && *url ? url : "(no URL)";
    char *esc_url = ns_html_escape_text(safe_url);
    char *esc_title = ns_html_escape_text(info->title);
    char *esc_heading = ns_html_escape_text(info->heading);
    char *esc_summary = ns_html_escape_text(info->summary);
    char *esc_detail = NULL;
    if (transport_error && *transport_error) {
        char *esc_err = ns_html_escape_text(transport_error);
        esc_detail = g_strdup_printf("Technical detail: %s", esc_err);
        g_free(esc_err);
    } else if (status > 0) {
        esc_detail = g_strdup_printf("HTTP status: %ld", status);
    }

    char *retry_href = url && *url ? ns_html_escape_text(url) : g_strdup("");
    gboolean can_retry = url && *url && !g_str_has_prefix(url, "about:");

    GString *out = g_string_new(NULL);
    g_string_append(out,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>");
    g_string_append(out, esc_title);
    g_string_append(out, " — Nordstjernen</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,\"Segoe UI\","
        "Helvetica,Arial,sans-serif;background:#f5f5f8;color:#1b1b22;"
        "margin:0;padding:0;min-height:100vh;"
        "display:flex;align-items:center;justify-content:center}"
        ".card{background:#fff;border:1px solid #e3e3e8;border-radius:10px;"
        "box-shadow:0 4px 24px rgba(0,0,0,0.06);padding:36px 40px;"
        "max-width:640px;margin:32px 16px;line-height:1.5}"
        ".icon{font-size:48px;line-height:1;margin-bottom:14px}"
        "h1{font-size:22px;margin:0 0 12px 0;color:#1b1b22}"
        "p.summary{font-size:16px;color:#33333d;margin:0 0 16px 0}"
        ".url{font-family:ui-monospace,\"SF Mono\",Menlo,Consolas,monospace;"
        "background:#f0f0f4;border:1px solid #e3e3e8;border-radius:6px;"
        "padding:8px 12px;font-size:13px;color:#444;overflow-wrap:anywhere;"
        "margin:0 0 16px 0}"
        ".detail{font-family:ui-monospace,\"SF Mono\",Menlo,Consolas,monospace;"
        "font-size:12px;color:#666;margin:0 0 24px 0;"
        "overflow-wrap:anywhere}"
        ".actions{display:flex;gap:10px;flex-wrap:wrap}"
        ".btn{display:inline-block;padding:9px 16px;border-radius:6px;"
        "text-decoration:none;font-size:14px;font-weight:500;"
        "border:1px solid transparent;cursor:pointer;"
        "font-family:inherit}"
        ".btn.primary{background:#3a63d0;color:#fff;border-color:#3a63d0}"
        ".btn.primary:hover{background:#2f55c2}"
        ".btn.secondary{background:#fff;color:#1b1b22;"
        "border-color:#d0d0d8}"
        ".btn.secondary:hover{background:#f5f5f8}"
        ".tips{margin-top:24px;padding-top:18px;border-top:1px solid #ececf0;"
        "color:#555;font-size:13px}"
        ".tips ul{margin:8px 0 0 0;padding-left:20px}"
        ".tips li{margin:3px 0}"
        "</style></head><body>"
        "<div class=\"card\">"
        "<div class=\"icon\">");
    g_string_append(out, info->icon);
    g_string_append(out, "</div>"
        "<h1>");
    g_string_append(out, esc_heading);
    g_string_append(out, "</h1>"
        "<p class=\"summary\">");
    g_string_append(out, esc_summary);
    g_string_append(out, "</p>"
        "<p class=\"url\">");
    g_string_append(out, esc_url);
    g_string_append(out, "</p>");
    if (esc_detail) {
        g_string_append(out, "<p class=\"detail\">");
        g_string_append(out, esc_detail);
        g_string_append(out, "</p>");
    }
    g_string_append(out, "<div class=\"actions\">");
    if (can_retry) {
        g_string_append(out, "<a class=\"btn primary\" href=\"");
        g_string_append(out, retry_href);
        g_string_append(out, "\">Try again</a>");
    }
    g_string_append(out,
        "<button class=\"btn secondary\" "
        "onclick=\"history.back()\">Go back</button>"
        "<a class=\"btn secondary\" href=\"about:start\">Start page</a>"
        "</div>"
        "<div class=\"tips\">"
        "<strong>What to try:</strong>"
        "<ul>"
        "<li>Double-check the address bar for typos.</li>"
        "<li>Make sure your internet connection is working.</li>"
        "<li>Reload the page in a moment — temporary outages do happen.</li>"
        "</ul>"
        "</div>"
        "</div></body></html>");

    g_free(esc_url);
    g_free(esc_title);
    g_free(esc_heading);
    g_free(esc_summary);
    g_free(esc_detail);
    g_free(retry_href);

    return g_string_free(out, FALSE);
}

static gboolean
append_response_budgeted(GByteArray *body,
                         const guint8 *data,
                         gsize len,
                         guint64 *total,
                         guint64 budget)
{
    if (len == 0) return TRUE;
    if (len > G_MAXUINT) return FALSE;
    if (*total > budget) return FALSE;
    if ((guint64)len > budget - *total) return FALSE;
    if (*total + (guint64)len > G_MAXUINT) return FALSE;
    g_byte_array_append(body, data, (guint)len);
    *total += (guint64)len;
    return TRUE;
}

static char *
response_budget_error(guint64 stopped_at)
{
    return g_strdup_printf(
        "response would exhaust available memory (stopped at %llu MiB)",
        (unsigned long long)(stopped_at >> 20));
}

static int
base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static gboolean
decode_data_base64_budgeted(const char *data,
                            GByteArray *body,
                            guint64 budget,
                            gboolean *too_large)
{
    int q[4] = {0};
    int qn = 0;
    guint64 total = 0;
    gboolean ended = FALSE;
    if (too_large) *too_large = FALSE;
    for (const char *p = data; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '%' && g_ascii_isxdigit((guchar)p[1]) &&
            g_ascii_isxdigit((guchar)p[2])) {
            c = (unsigned char)((g_ascii_xdigit_value(p[1]) << 4) |
                                g_ascii_xdigit_value(p[2]));
            p += 2;
        }
        if (g_ascii_isspace(c)) continue;
        if (ended) return FALSE;
        if (c == '=') {
            q[qn++] = -2;
        } else {
            int v = base64_value((char)c);
            if (v < 0) return FALSE;
            q[qn++] = v;
        }
        if (qn < 4) continue;
        if (q[0] < 0 || q[1] < 0) return FALSE;
        guint8 out[3];
        gsize out_len = 1;
        out[0] = (guint8)((q[0] << 2) | (q[1] >> 4));
        if (q[2] == -2) {
            if (q[3] != -2) return FALSE;
            ended = TRUE;
        } else {
            out[1] = (guint8)(((q[1] & 15) << 4) | (q[2] >> 2));
            out_len = 2;
            if (q[3] == -2) {
                ended = TRUE;
            } else {
                if (q[3] < 0) return FALSE;
                out[2] = (guint8)(((q[2] & 3) << 6) | q[3]);
                out_len = 3;
            }
        }
        if (!append_response_budgeted(body, out, out_len, &total, budget)) {
            if (too_large) *too_large = TRUE;
            return FALSE;
        }
        qn = 0;
    }
    if (qn == 0) return TRUE;
    if (qn == 1 || q[0] < 0 || q[1] < 0) return FALSE;
    guint8 out[2];
    gsize out_len = 1;
    out[0] = (guint8)((q[0] << 2) | (q[1] >> 4));
    if (qn == 3) {
        if (q[2] < 0) return FALSE;
        out[1] = (guint8)(((q[1] & 15) << 4) | (q[2] >> 2));
        out_len = 2;
    }
    if (!append_response_budgeted(body, out, out_len, &total, budget)) {
        if (too_large) *too_large = TRUE;
        return FALSE;
    }
    return TRUE;
}

static gboolean
decode_data_uri_budgeted(const char *data,
                         GByteArray *body,
                         guint64 budget,
                         gboolean *too_large)
{
    guint8 buf[8192];
    gsize n = 0;
    guint64 total = 0;
    if (too_large) *too_large = FALSE;
    for (const char *p = data; *p; p++) {
        guint8 b;
        if (*p == '%') {
            if (!g_ascii_isxdigit((guchar)p[1]) ||
                !g_ascii_isxdigit((guchar)p[2]))
                return FALSE;
            b = (guint8)((g_ascii_xdigit_value(p[1]) << 4) |
                         g_ascii_xdigit_value(p[2]));
            p += 2;
        } else {
            b = (guint8)*p;
        }
        buf[n++] = b;
        if (n == sizeof(buf)) {
            if (!append_response_budgeted(body, buf, n, &total, budget)) {
                if (too_large) *too_large = TRUE;
                return FALSE;
            }
            n = 0;
        }
    }
    if (!append_response_budgeted(body, buf, n, &total, budget)) {
        if (too_large) *too_large = TRUE;
        return FALSE;
    }
    return TRUE;
}

gboolean
ns_data_url_decode(const char *url,
                   GByteArray *out,
                   guint64 budget,
                   char **out_content_type,
                   gboolean *too_large)
{
    if (too_large) *too_large = FALSE;
    if (out_content_type) *out_content_type = NULL;
    if (!url || !out || !g_str_has_prefix(url, "data:")) return FALSE;
    const char *p = url + 5;
    const char *comma = strchr(p, ',');
    if (!comma) return FALSE;
    char *meta = g_strndup(p, (gsize)(comma - p));
    g_strchomp(meta);
    gboolean base64 = FALSE;
    gsize meta_len = strlen(meta);
    if (meta_len >= 7 &&
        g_ascii_strcasecmp(meta + meta_len - 7, ";base64") == 0) {
        meta[meta_len - 7] = '\0';
        base64 = TRUE;
    }
    if (out_content_type)
        *out_content_type = (*meta) ? g_strdup(meta)
                                    : g_strdup("text/plain;charset=UTF-8");
    g_free(meta);
    const char *data = comma + 1;
    return base64
        ? decode_data_base64_budgeted(data, out, budget, too_large)
        : decode_data_uri_budgeted(data, out, budget, too_large);
}

static gboolean
read_file_budgeted(const char *path,
                   GByteArray *body,
                   guint64 budget,
                   char **error_out)
{
    GStatBuf st;
    if (g_stat(path, &st) == 0 && st.st_size > 0 &&
        ((guint64)st.st_size > budget || (guint64)st.st_size > G_MAXUINT)) {
        if (error_out) *error_out = response_budget_error((guint64)st.st_size);
        return FALSE;
    }
    FILE *f = g_fopen(path, "rb");
    if (!f) {
        if (error_out) *error_out = g_strdup(g_strerror(errno));
        return FALSE;
    }
    guint8 buf[65536];
    guint64 total = 0;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0) {
            if (!append_response_budgeted(body, buf, (gsize)n, &total, budget)) {
                if (error_out) *error_out = response_budget_error(total);
                fclose(f);
                return FALSE;
            }
        }
        if (n < sizeof(buf)) {
            if (ferror(f)) {
                if (error_out) *error_out = g_strdup(g_strerror(errno));
                fclose(f);
                return FALSE;
            }
            break;
        }
    }
    fclose(f);
    return TRUE;
}

static gboolean
synthesize_data_response(const char *url, ns_response *resp)
{
    if (!url || !g_str_has_prefix(url, "data:")) return FALSE;
    guint64 budget = ns_net_response_budget();
    gsize body_start = resp->body->len;
    char *ct = NULL;
    gboolean too_large = FALSE;
    if (!ns_data_url_decode(url, resp->body, budget, &ct, &too_large)) {
        if (!ct) return FALSE;
        g_byte_array_set_size(resp->body, body_start);
        if (too_large)
            resp->error = response_budget_error(budget);
        else
            resp->error = g_strdup("malformed data: URL");
    }
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = ct;
    return TRUE;
}

typedef struct ns_file_listing_entry {
    char     *name;
    char     *display;
    char     *uri;
    guint64   size;
    gint64    mtime;
    gboolean  is_dir;
    gboolean  have_stat;
} ns_file_listing_entry;

static void
file_listing_entry_free(gpointer data)
{
    ns_file_listing_entry *e = data;
    if (!e) return;
    g_free(e->name);
    g_free(e->display);
    g_free(e->uri);
    g_free(e);
}

static gint
file_listing_entry_compare(gconstpointer ap, gconstpointer bp)
{
    const ns_file_listing_entry *a =
        *(const ns_file_listing_entry * const *)ap;
    const ns_file_listing_entry *b =
        *(const ns_file_listing_entry * const *)bp;
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    return g_utf8_collate(a->display ? a->display : "",
                          b->display ? b->display : "");
}

static char *
file_uri_for_path(const char *path, gboolean is_dir)
{
    if (!path || !*path) return NULL;
    char *p = g_strdup(path);
    if (is_dir && !g_str_has_suffix(p, G_DIR_SEPARATOR_S)) {
        char *with_sep = g_strconcat(p, G_DIR_SEPARATOR_S, NULL);
        g_free(p);
        p = with_sep;
    }
    char *uri = g_filename_to_uri(p, NULL, NULL);
    g_free(p);
    return uri;
}

static char *
file_size_label(guint64 size)
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)size;
    guint unit = 0;
    while (v >= 1024.0 && unit + 1 < G_N_ELEMENTS(units)) {
        v /= 1024.0;
        unit++;
    }
    if (unit == 0)
        return g_strdup_printf("%" G_GUINT64_FORMAT " B", size);
    if (v >= 100.0)
        return g_strdup_printf("%.0f %s", v, units[unit]);
    return g_strdup_printf("%.1f %s", v, units[unit]);
}

static char *
file_mtime_label(gint64 mtime)
{
    if (mtime <= 0) return g_strdup("");
    GDateTime *dt = g_date_time_new_from_unix_local(mtime);
    if (!dt) return g_strdup("");
    char *out = g_date_time_format(dt, "%Y-%m-%d %H:%M");
    g_date_time_unref(dt);
    return out ? out : g_strdup("");
}

static char *
file_listing_parent_uri(const char *path)
{
    if (!path || !*path) return NULL;
    char *parent = g_path_get_dirname(path);
    if (!parent || !*parent || strcmp(parent, ".") == 0 ||
        strcmp(parent, path) == 0) {
        g_free(parent);
        return NULL;
    }
    char *uri = file_uri_for_path(parent, TRUE);
    g_free(parent);
    return uri;
}

static char *
file_directory_error_page(const char *url, long status, const char *error)
{
    char *html = ns_build_error_page(url, status, error);
    return html ? html : g_strdup("<!doctype html><meta charset=utf-8>"
                                  "<title>Cannot read folder</title>"
                                  "<p>Cannot read folder.</p>");
}

static char *
file_directory_listing_page(const char *path, const char *url, long *status_out)
{
    if (status_out) *status_out = 200;
    GError *err = NULL;
    GDir *dir = g_dir_open(path, 0, &err);
    if (!dir) {
        long status = g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_ACCES)
            ? 403 : 404;
        char *msg = g_strdup(err ? err->message : "cannot read folder");
        g_clear_error(&err);
        if (status_out) *status_out = status;
        char *html = file_directory_error_page(url, status, msg);
        g_free(msg);
        return html;
    }

    GPtrArray *entries = g_ptr_array_new_with_free_func(file_listing_entry_free);
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *full = g_build_filename(path, name, NULL);
        ns_file_listing_entry *e = g_new0(ns_file_listing_entry, 1);
        e->name = g_strdup(name);
        e->display = g_filename_display_name(name);
        e->is_dir = g_file_test(full, G_FILE_TEST_IS_DIR);
        GStatBuf st;
        if (g_stat(full, &st) == 0) {
            e->have_stat = TRUE;
            if (st.st_size > 0)
                e->size = (guint64)st.st_size;
            e->mtime = (gint64)st.st_mtime;
        }
        e->uri = file_uri_for_path(full, e->is_dir);
        if (e->uri)
            g_ptr_array_add(entries, e);
        else
            file_listing_entry_free(e);
        g_free(full);
    }
    g_dir_close(dir);
    g_ptr_array_sort(entries, file_listing_entry_compare);

    char *display_path = g_filename_display_name(path);
    char *esc_path = ns_html_escape_text(display_path ? display_path : path);
    char *parent_uri = file_listing_parent_uri(path);
    char *esc_parent = parent_uri ? ns_html_escape_text(parent_uri) : NULL;
    GString *out = g_string_new(NULL);
    g_string_append(out, "<!doctype html><html><head><meta charset=\"utf-8\">");
    g_string_append(out, "<title>Index of ");
    g_string_append(out, esc_path);
    g_string_append(out, "</title><style>"
        "body{font-family:serif;margin:8px;color:#000;background:#fff}"
        "h1{font-size:2em;margin:.4em 0}"
        "table{border-collapse:collapse}"
        "th{text-align:left;font-weight:bold;padding:0 3em .25em 0}"
        "td{padding:0 3em 0 0;white-space:nowrap}"
        "td.name{min-width:22em}"
        "td.size{text-align:right}"
        ".ico{display:inline-block;width:1.35em;margin-right:.35em;"
        "text-align:center;font-family:\"Segoe UI Emoji\",\"Apple Color Emoji\","
        "\"Noto Color Emoji\",sans-serif}"
        "a{color:#00e;text-decoration:underline}"
        "hr{border:0;border-top:1px solid #bbb;margin:.6em 0}"
        "</style></head><body><h1>Index of ");
    g_string_append(out, esc_path);
    g_string_append(out, "</h1><hr><table><thead><tr>"
        "<th>Name</th><th>Size</th><th>Date modified</th>"
        "</tr></thead><tbody>");
    if (parent_uri) {
        g_string_append(out, "<tr><td class=\"name\"><span class=\"ico dir\""
            " aria-hidden=\"true\">&#128193;</span><a href=\"");
        g_string_append(out, esc_parent);
        g_string_append(out, "\">../</a></td><td class=\"size\"></td>"
            "<td></td></tr>");
    }
    for (guint i = 0; i < entries->len; i++) {
        ns_file_listing_entry *e = g_ptr_array_index(entries, i);
        char *esc_uri = ns_html_escape_text(e->uri);
        char *esc_name = ns_html_escape_text(e->display ? e->display : e->name);
        char *size = e->is_dir ? g_strdup("") : file_size_label(e->size);
        char *date = e->have_stat ? file_mtime_label(e->mtime) : g_strdup("");
        char *esc_size = ns_html_escape_text(size);
        char *esc_date = ns_html_escape_text(date);
        g_string_append(out, "<tr><td class=\"name\"><span class=\"ico ");
        g_string_append(out, e->is_dir ? "dir" : "file");
        g_string_append(out, "\" aria-hidden=\"true\">");
        g_string_append(out, e->is_dir ? "&#128193;" : "&#128196;");
        g_string_append(out, "</span><a href=\"");
        g_string_append(out, esc_uri);
        g_string_append(out, "\">");
        g_string_append(out, esc_name);
        if (e->is_dir)
            g_string_append_c(out, '/');
        g_string_append(out, "</a></td><td class=\"size\">");
        g_string_append(out, esc_size);
        g_string_append(out, "</td><td>");
        g_string_append(out, esc_date);
        g_string_append(out, "</td></tr>");
        g_free(esc_uri);
        g_free(esc_name);
        g_free(size);
        g_free(date);
        g_free(esc_size);
        g_free(esc_date);
    }
    g_string_append(out, "</tbody></table><hr></body></html>");
    g_ptr_array_free(entries, TRUE);
    g_free(display_path);
    g_free(esc_path);
    g_free(parent_uri);
    g_free(esc_parent);
    return g_string_free(out, FALSE);
}

typedef struct ns_ftp_listing_entry {
    char     *name;
    char     *display;
    char     *uri;
    char     *date;
    guint64   size;
    gboolean  is_dir;
    gboolean  have_size;
} ns_ftp_listing_entry;

static void
ftp_listing_entry_free(gpointer data)
{
    ns_ftp_listing_entry *e = data;
    if (!e) return;
    g_free(e->name);
    g_free(e->display);
    g_free(e->uri);
    g_free(e->date);
    g_free(e);
}

static gint
ftp_listing_entry_compare(gconstpointer ap, gconstpointer bp)
{
    const ns_ftp_listing_entry *a =
        *(const ns_ftp_listing_entry * const *)ap;
    const ns_ftp_listing_entry *b =
        *(const ns_ftp_listing_entry * const *)bp;
    if (a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;
    return g_utf8_collate(a->display ? a->display : "",
                          b->display ? b->display : "");
}

static char **
ftp_split_fields(const char *line, int max_fields)
{
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    const char *p = line ? line : "";
    while (*p && (max_fields <= 0 ||
                  parts->len + 1 < (guint)max_fields)) {
        while (g_ascii_isspace(*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !g_ascii_isspace(*p)) p++;
        g_ptr_array_add(parts, g_strndup(start, (gsize)(p - start)));
    }
    while (g_ascii_isspace(*p)) p++;
    if (*p)
        g_ptr_array_add(parts, g_strdup(p));
    g_ptr_array_add(parts, NULL);
    return (char **)g_ptr_array_free(parts, FALSE);
}

static char *
ftp_listing_child_uri(const char *base_url, const char *name, gboolean is_dir)
{
    if (!base_url || !*base_url || !name || !*name) return NULL;
    char *base = g_strdup(base_url);
    char *hash = strchr(base, '#');
    if (hash) *hash = '\0';
    char *query = strchr(base, '?');
    if (query) *query = '\0';
    if (!g_str_has_suffix(base, "/")) {
        char *with_slash = g_strconcat(base, "/", NULL);
        g_free(base);
        base = with_slash;
    }
    char *seg = g_uri_escape_string(name, NULL, FALSE);
    char *out = g_strconcat(base, seg, is_dir ? "/" : "", NULL);
    g_free(seg);
    g_free(base);
    return out;
}

static char *
ftp_listing_parent_uri(const char *url)
{
    if (!url || !*url) return NULL;
    char *base = g_strdup(url);
    char *hash = strchr(base, '#');
    if (hash) *hash = '\0';
    char *query = strchr(base, '?');
    if (query) *query = '\0';
    gsize len = strlen(base);
    while (len > 0 && base[len - 1] == '/')
        base[--len] = '\0';
    char *after_scheme = strstr(base, "://");
    char *path = after_scheme ? strchr(after_scheme + 3, '/') : NULL;
    if (!path || !path[1]) {
        g_free(base);
        return NULL;
    }
    char *last = strrchr(path + 1, '/');
    if (!last) {
        path[1] = '\0';
    } else {
        last[1] = '\0';
    }
    char *out = g_strdup(base);
    g_free(base);
    return out;
}

static gboolean
ftp_parse_unix_listing(char *line, ns_ftp_listing_entry *e)
{
    if (!line || !*line || !e) return FALSE;
    if (line[0] != 'd' && line[0] != '-' && line[0] != 'l')
        return FALSE;
    char **tokens = ftp_split_fields(line, 9);
    int count = 0;
    while (tokens[count]) count++;
    if (count < 9 || !tokens[4] || !tokens[5] || !tokens[6] ||
        !tokens[7] || !tokens[8] || !*tokens[8]) {
        g_strfreev(tokens);
        return FALSE;
    }
    e->is_dir = line[0] == 'd';
    e->size = g_ascii_strtoull(tokens[4], NULL, 10);
    e->have_size = TRUE;
    e->date = g_strdup_printf("%s %s %s", tokens[5], tokens[6], tokens[7]);
    char *name = g_strdup(tokens[8]);
    if (line[0] == 'l') {
        char *arrow = strstr(name, " -> ");
        if (arrow) *arrow = '\0';
    }
    g_strstrip(name);
    if (!*name || g_str_equal(name, ".") || g_str_equal(name, "..")) {
        g_free(name);
        g_strfreev(tokens);
        return FALSE;
    }
    e->name = name;
    e->display = g_strdup(name);
    g_strfreev(tokens);
    return TRUE;
}

static gboolean
ftp_parse_dos_listing(char *line, ns_ftp_listing_entry *e)
{
    if (!line || !*line || !e) return FALSE;
    char **tokens = ftp_split_fields(line, 4);
    int count = 0;
    while (tokens[count]) count++;
    if (count < 4 || !tokens[0] || !tokens[1] || !tokens[2] ||
        !tokens[3] || !*tokens[3]) {
        g_strfreev(tokens);
        return FALSE;
    }
    if (!strchr(tokens[0], '-') || !strchr(tokens[1], ':')) {
        g_strfreev(tokens);
        return FALSE;
    }
    e->is_dir = g_ascii_strcasecmp(tokens[2], "<DIR>") == 0;
    if (!e->is_dir) {
        e->size = g_ascii_strtoull(tokens[2], NULL, 10);
        e->have_size = TRUE;
    }
    e->date = g_strdup_printf("%s %s", tokens[0], tokens[1]);
    char *name = g_strdup(tokens[3]);
    g_strstrip(name);
    if (!*name || g_str_equal(name, ".") || g_str_equal(name, "..")) {
        g_free(name);
        g_strfreev(tokens);
        return FALSE;
    }
    e->name = name;
    e->display = g_strdup(name);
    g_strfreev(tokens);
    return TRUE;
}

static ns_ftp_listing_entry *
ftp_listing_entry_from_line(const char *line)
{
    if (!line) return NULL;
    char *work = g_strdup(line);
    g_strstrip(work);
    if (!*work) {
        g_free(work);
        return NULL;
    }
    ns_ftp_listing_entry *e = g_new0(ns_ftp_listing_entry, 1);
    if (!ftp_parse_unix_listing(work, e) &&
        !ftp_parse_dos_listing(work, e)) {
        g_clear_pointer(&e->name, g_free);
        g_clear_pointer(&e->display, g_free);
        g_clear_pointer(&e->date, g_free);
        e->size = 0;
        e->have_size = FALSE;
        e->is_dir = FALSE;
        e->name = g_strdup(work);
        e->display = g_strdup(work);
    }
    g_free(work);
    if (!e->name || !*e->name) {
        ftp_listing_entry_free(e);
        return NULL;
    }
    return e;
}

static char *
ftp_directory_listing_page(const char *url, GByteArray *body)
{
    char *listing = (body && body->data && body->len > 0)
        ? g_strndup((const char *)body->data, body->len) : g_strdup("");
    GPtrArray *entries = g_ptr_array_new_with_free_func(ftp_listing_entry_free);
    char **lines = g_strsplit(listing, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        ns_ftp_listing_entry *e = ftp_listing_entry_from_line(lines[i]);
        if (!e) continue;
        e->uri = ftp_listing_child_uri(url, e->name, e->is_dir);
        if (e->uri)
            g_ptr_array_add(entries, e);
        else
            ftp_listing_entry_free(e);
    }
    g_strfreev(lines);
    g_free(listing);
    g_ptr_array_sort(entries, ftp_listing_entry_compare);

    char *esc_url = ns_html_escape_text(url ? url : "ftp://");
    char *parent_uri = ftp_listing_parent_uri(url);
    char *esc_parent = parent_uri ? ns_html_escape_text(parent_uri) : NULL;
    GString *out = g_string_new(NULL);
    g_string_append(out, "<!doctype html><html><head><meta charset=\"utf-8\">");
    g_string_append(out, "<title>Index of ");
    g_string_append(out, esc_url);
    g_string_append(out, "</title><style>"
        "body{font-family:serif;margin:8px;color:#000;background:#fff}"
        "h1{font-size:2em;margin:.4em 0}"
        "table{border-collapse:collapse}"
        "th{text-align:left;font-weight:bold;padding:0 3em .25em 0}"
        "td{padding:0 3em 0 0;white-space:nowrap}"
        "td.name{min-width:22em}"
        "td.size{text-align:right}"
        ".ico{display:inline-block;width:1.35em;margin-right:.35em;"
        "text-align:center;font-family:\"Segoe UI Emoji\",\"Apple Color Emoji\","
        "\"Noto Color Emoji\",sans-serif}"
        "a{color:#00e;text-decoration:underline}"
        "hr{border:0;border-top:1px solid #bbb;margin:.6em 0}"
        "</style></head><body><h1>Index of ");
    g_string_append(out, esc_url);
    g_string_append(out, "</h1><hr><table><thead><tr>"
        "<th>Name</th><th>Size</th><th>Date modified</th>"
        "</tr></thead><tbody>");
    if (parent_uri) {
        g_string_append(out, "<tr><td class=\"name\"><span class=\"ico dir\""
            " aria-hidden=\"true\">&#128193;</span><a href=\"");
        g_string_append(out, esc_parent);
        g_string_append(out, "\">../</a></td><td class=\"size\"></td>"
            "<td></td></tr>");
    }
    for (guint i = 0; i < entries->len; i++) {
        ns_ftp_listing_entry *e = g_ptr_array_index(entries, i);
        char *esc_uri = ns_html_escape_text(e->uri);
        char *esc_name = ns_html_escape_text(e->display ? e->display : e->name);
        char *size = (!e->is_dir && e->have_size) ? file_size_label(e->size)
                                                  : g_strdup("");
        char *esc_size = ns_html_escape_text(size);
        char *esc_date = ns_html_escape_text(e->date ? e->date : "");
        g_string_append(out, "<tr><td class=\"name\"><span class=\"ico ");
        g_string_append(out, e->is_dir ? "dir" : "file");
        g_string_append(out, "\" aria-hidden=\"true\">");
        g_string_append(out, e->is_dir ? "&#128193;" : "&#128196;");
        g_string_append(out, "</span><a href=\"");
        g_string_append(out, esc_uri);
        g_string_append(out, "\">");
        g_string_append(out, esc_name);
        if (e->is_dir)
            g_string_append_c(out, '/');
        g_string_append(out, "</a></td><td class=\"size\">");
        g_string_append(out, esc_size);
        g_string_append(out, "</td><td>");
        g_string_append(out, esc_date);
        g_string_append(out, "</td></tr>");
        g_free(esc_uri);
        g_free(esc_name);
        g_free(size);
        g_free(esc_size);
        g_free(esc_date);
    }
    g_string_append(out, "</tbody></table><hr></body></html>");
    g_ptr_array_free(entries, TRUE);
    g_free(esc_url);
    g_free(parent_uri);
    g_free(esc_parent);
    return g_string_free(out, FALSE);
}

static gboolean
ftp_url_looks_like_directory(const char *url)
{
    if (!ns_url_is_ftp(url)) return FALSE;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(url);
    if (!parts || !parts->pathname) return g_str_has_suffix(url, "/");
    return !*parts->pathname || g_str_has_suffix(parts->pathname, "/");
}

static void
maybe_synthesize_ftp_listing(ns_response *resp)
{
    if (!resp || !resp->body || !ftp_url_looks_like_directory(resp->final_url))
        return;
    char *html = ftp_directory_listing_page(resp->final_url, resp->body);
    if (!html) return;
    g_byte_array_set_size(resp->body, 0);
    g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
    g_free(html);
    g_free(resp->content_type);
    resp->content_type = g_strdup("text/html; charset=utf-8");
}

static void
maybe_guess_ftp_content_type(ns_response *resp)
{
    if (!resp || resp->content_type || !ns_url_is_ftp(resp->final_url))
        return;
    if (ftp_url_looks_like_directory(resp->final_url)) return;
    g_autoptr(ns_url_parts) parts = ns_url_parts_new(resp->final_url);
    const char *path = parts && parts->pathname ? parts->pathname
                                                : resp->final_url;
    gboolean uncertain = FALSE;
    char *ctype = g_content_type_guess(path,
                                       resp->body ? resp->body->data : NULL,
                                       resp->body ? resp->body->len : 0,
                                       &uncertain);
    char *mime = ctype ? g_content_type_get_mime_type(ctype) : NULL;
    resp->content_type = g_strdup(mime ? mime : "application/octet-stream");
    g_free(mime);
    g_free(ctype);
}

static gboolean
ns_file_access_allowed(const char *top_url)
{
    if (!g_allow_file_urls) return FALSE;
    if (!top_url || !*top_url) return TRUE;
    return g_str_has_prefix(top_url, "file:");
}

static gboolean
synthesize_file_response(const char *url, const char *top_url, ns_response *resp)
{
    if (!url || !g_str_has_prefix(url, "file:")) return FALSE;
    if (!ns_file_access_allowed(top_url)) {
        resp->final_url = g_strdup(url);
        resp->status = 0;
        resp->error = g_strdup("local file access is not allowed from a "
                               "remote page");
        return TRUE;
    }
    char *uri_path = g_filename_from_uri(url, NULL, NULL);
    char *path = uri_path ? g_canonicalize_filename(uri_path, NULL) : NULL;
    g_free(uri_path);
    resp->final_url = g_strdup(url);
    if (!path) {
        resp->status = 400;
        resp->error = g_strdup("invalid file URL");
        return TRUE;
    }

    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_free(resp->final_url);
        resp->final_url = file_uri_for_path(path, TRUE);
        if (!resp->final_url)
            resp->final_url = g_strdup(url);
        long status = 200;
        char *html = file_directory_listing_page(path, resp->final_url, &status);
        resp->status = status;
        resp->content_type = g_strdup("text/html; charset=utf-8");
        if (html) {
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
        }
        g_free(path);
        return TRUE;
    }

    guint64 budget = ns_net_response_budget();
    char *read_error = NULL;
    if (!read_file_budgeted(path, resp->body, budget, &read_error)) {
        resp->status = read_error &&
            g_str_has_prefix(read_error, "response would exhaust") ? 0 : 404;
        resp->error = read_error ? read_error : g_strdup("file not found");
    } else {
        resp->status = 200;
    }

    gboolean uncertain = FALSE;
    char *ctype = g_content_type_guess(path, NULL, 0, &uncertain);
    char *mime = ctype ? g_content_type_get_mime_type(ctype) : NULL;
    resp->content_type = g_strdup(mime ? mime : "application/octet-stream");
    g_free(mime);
    g_free(ctype);
    g_free(path);
    return TRUE;
}

#if defined(NS_HAVE_AI) && !defined(__APPLE__)
static const char k_about_start_template[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<meta name=\"color-scheme\" content=\"light\">"
    "<title>Nordstjernen</title>"
    "<style>\n"
    "html, body { background:#ffffff; color:#111418;"
    " font-family: system-ui, -apple-system, \"Segoe UI\","
    " Helvetica, Arial, sans-serif; margin:0; padding:0; min-height:100%; }\n"
    ".wrap { max-width: 760px; margin: 0 auto; padding: 34px 24px 32px;"
    " box-sizing:border-box; }\n"
    ".mark-img { display:block; width:58px; height:58px;"
    " margin:0 auto 12px; border-radius:14px; object-fit:cover; }\n"
    ".intro { color:#4b5563; text-align:center; line-height:1.5;"
    " margin:0 auto 22px; max-width:620px; }\n"
    ".search { display:flex; gap:10px; margin:0 0 16px; }\n"
    ".search input { flex:1 1 auto; border:1px solid #c9cfd9;"
    " background:#ffffff; color:#111418; border-radius:10px;"
    " padding:13px 16px; font:inherit; font-size:1.03em; outline:none; }\n"
    ".search input:focus { border-color:#2d6cf6; }\n"
    ".search button { border:0; background:#2d6cf6; color:#fff;"
    " border-radius:10px; padding:13px 22px; font:inherit; font-weight:600;"
    " cursor:pointer; }\n"
    ".search button:hover { background:#2560d8; }\n"
    ".aiwin { display:flex; align-items:center; gap:14px; width:100%;"
    " text-align:left; margin:0 0 22px; padding:15px 18px; cursor:pointer;"
    " border:1px solid #d9dee7; border-radius:12px; background:#f6f8fb;"
    " color:#111418; font:inherit; }\n"
    ".aiwin:hover { border-color:#2d6cf6; background:#eef3fe; }\n"
    ".aiwin .ico { flex:0 0 auto; width:38px; height:38px; line-height:38px;"
    " text-align:center; border-radius:10px; background:#2d6cf6; color:#fff;"
    " font-size:0.92em; font-weight:700; letter-spacing:0.02em; }\n"
    ".aiwin .txt { flex:1 1 auto; }\n"
    ".aiwin .txt b { display:block; font-size:1.02em; }\n"
    ".aiwin .txt span { color:#5b6470; font-size:0.9em; }\n"
    ".aiwin .arrow { flex:0 0 auto; color:#2d6cf6; font-size:1.35em;"
    " font-weight:700; }\n"
    ".links { text-align:center; color:#6b7280; font-size:0.86em;"
    " line-height:1.7; }\n"
    ".links a { color:#2d6cf6; margin:0 8px; }\n"
    "@media (max-width:560px) { .wrap { padding:24px 24px 28px; }"
    " .mark-img { width:48px; height:48px; margin-bottom:10px; }"
    " .intro { display:none; }"
    " .search { flex-direction:column; gap:8px; }"
    " .search button { width:100%; }"
    " .links a { display:inline-block; margin:0 5px 4px; } }\n"
    "</style></head>"
    "<body><main class=\"wrap\">"
    "__ND_SPLASH__"
    "<form class=\"search\" id=\"dsearch\">"
    "<input id=\"sq\" name=\"q\" autocomplete=\"off\" autofocus dir=\"auto\""
    " aria-label=\"Search the web\""
    " size=\"__ND_SEARCH_SIZE__\""
    " placeholder=\"Search (__ND_SEARCH_NAME__):\">"
    "<button type=\"submit\">Search</button>"
    "</form>"
    "<button class=\"aiwin\" id=\"aiwin\" type=\"button\">"
    "<span class=\"ico\" aria-hidden=\"true\">AI</span>"
    "<span class=\"txt\"><b>AI Window</b>"
    "<span>Chat with a small AI model that runs entirely on your "
    "computer \xe2\x80\x94 nothing leaves this machine.</span></span>"
    "<span class=\"arrow\" aria-hidden=\"true\">\xe2\x86\x92</span>"
    "</button>"
    "<p class=\"links\">"
    "<a href=\"about:history\">History</a>"
    "<a href=\"about:license\">License</a>"
    "<a href=\"https://nordstjernen.org/privacy\">Privacy</a>"
    "<a href=\"https://nordstjernen.org\">nordstjernen.org</a>"
    "</p>"
    "</main>"
    "<script>\n"
    "var searchUrl='__ND_SEARCH_URL__';\n"
    "var dsearch=document.getElementById('dsearch');\n"
    "var sq=document.getElementById('sq');\n"
    "dsearch.addEventListener('submit',function(e){e.preventDefault();\n"
    " var s=sq.value.trim(); if(!s) return;\n"
    " window.location.href=searchUrl.indexOf('%s')>=0\n"
    "  ? searchUrl.replace('%s',encodeURIComponent(s))\n"
    "  : searchUrl+encodeURIComponent(s);});\n"
    "var aiwin=document.getElementById('aiwin');\n"
    "if(aiwin) aiwin.addEventListener('click',function(){\n"
    " window.location.href='about:ai-window';});\n"
    "try{sq.focus();}catch(e){}\n"
    "</script></body></html>";

static const char k_about_ai_window_template[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<meta name=\"color-scheme\" content=\"light\">"
    "<title>AI Window \xe2\x80\x94 Nordstjernen</title>"
    "<style>\n"
    "html, body { background:#ffffff; color:#111418;"
    " font-family: system-ui, -apple-system, \"Segoe UI\","
    " Helvetica, Arial, sans-serif; margin:0; padding:0; height:100%; }\n"
    ".wrap { max-width: 920px; margin: 0 auto; padding: 14px 24px 35px 24px;"
    " display:flex; flex-direction:column; height:100%;"
    " box-sizing:border-box; }\n"
    ".head { display:block; text-align:center; margin-bottom:10px;"
    " flex:0 0 auto; }\n"
    ".mark { display:block; width:52px; height:52px; line-height:52px;"
    " margin:0 auto 10px; border:1px solid #d9dee7; border-radius:13px;"
    " background:#f6f8fb; color:#1b2a4a; font-weight:700;"
    " font-size:1.55em; text-align:center; }\n"
    ".mark-img { display:block; width:52px; height:52px;"
    " margin:0 auto 10px; border-radius:13px; object-fit:cover; }\n"
    ".hgroup { display:block; text-align:center; }\n"
    ".title { font-size: 1.45em; font-weight: 600; letter-spacing: 0;"
    " line-height:1.2; }\n"
    ".tagline { color:#5b6470; font-style: italic; font-size: 0.82em;"
    " margin:3px auto 0; line-height:1.25; max-width:36em; }\n"
    "#log { flex: 1 1 0; min-height:0; overflow-y:auto; padding: 6px 2px;"
    " display:flex; flex-direction:column; gap: 14px; }\n"
    ".msg { max-width: 94%; padding: 11px 14px; border-radius: 14px;"
    " line-height: 1.45; white-space: pre-wrap; overflow-wrap:break-word; }\n"
    ".user { align-self:flex-end; background:#2d6cf6; color:#fff;"
    " border-bottom-right-radius:4px; }\n"
    ".bot { align-self:flex-start; background:#f1f3f6; color:#111418;"
    " border-bottom-left-radius:4px; }\n"
    ".bot.think { color:#6b7280; font-style:italic; }\n"
    "form.ask { display:flex; gap:10px; padding-top:12px;"
    " flex:0 0 auto; margin-top:0; margin-bottom:8px; }\n"
    "form.ask input { flex:1 1 auto; border:1px solid #c9cfd9;"
    " background:#ffffff; color:#111418; border-radius:10px;"
    " padding:12px 14px; font:inherit; outline:none; }\n"
    "form.ask input:focus { border-color:#2d6cf6; }\n"
    "form.ask button { border:0; background:#2d6cf6; color:#fff;"
    " border-radius:10px; padding:12px 20px; font:inherit; font-weight:600;"
    " cursor:pointer; }\n"
    "form.ask button:disabled { opacity:0.5; }\n"
    ".footer { color:#6b7280; font-size:0.76em; text-align:center;"
    " padding-top:4px; margin-top:0; flex:0 0 auto; }\n"
    ".footer a { color:#2d6cf6; }\n"
    ".banner { align-self:flex-start; max-width:82%; background:#f1f3f6;"
    " color:#111418; padding:14px 16px; border-radius:14px;"
    " border-bottom-left-radius:4px; line-height:1.45; }\n"
    ".banner button { margin-top:12px; border:0; background:#2d6cf6;"
    " color:#fff; border-radius:9px; padding:9px 16px; font:inherit;"
    " font-weight:600; cursor:pointer; }\n"
    ".banner .hint { color:#6b7280; font-size:0.85em; margin-top:8px; }\n"
    ".opt { display:flex; align-items:center; gap:10px; margin-top:10px;"
    " padding:10px 12px; background:#ffffff; border:1px solid #c9cfd9;"
    " border-radius:10px; }\n"
    ".opt.active { border-color:#2d6cf6; }\n"
    ".opt .name { font-weight:600; }\n"
    ".opt .sz { color:#6b7280; font-size:0.85em; }\n"
    ".opt .spacer { flex:1 1 auto; }\n"
    ".opt button { margin:0; padding:7px 14px; font-size:0.9em; }\n"
    ".opt .badge { color:#2d6cf6; font-size:0.85em; font-weight:600; }\n"
    ".change { margin-left:8px; color:#111418; cursor:pointer; border:0;"
    " background:#e3e7ee; font:inherit; padding:2px 8px; border-radius:6px;"
    " position:relative; top:9px; }\n"
    ".bar { margin-top:12px; height:8px; border-radius:5px;"
    " background:#e3e7ee; overflow:hidden; }\n"
    ".bar > div { height:100%; width:0; background:#2d6cf6;"
    " transition:width 0.3s; }\n"
    ".msg a { color:#1a56d6; }\n"
    ".msg img.genimg { display:block; max-width:100%; height:auto;"
    " border-radius:10px; margin-top:8px; }\n"
    ".msg a img.genimg { cursor:pointer; }\n"
    ".search { display:flex; gap:10px;"
    " margin:8px 0 10px 0; flex:0 0 auto; }\n"
    ".search input { flex:1 1 auto; border:1px solid #c9cfd9;"
    " background:#ffffff; color:#111418; border-radius:10px;"
    " padding:13px 16px; font:inherit; font-size:1.02em; outline:none; }\n"
    ".search input:focus { border-color:#2d6cf6; }\n"
    ".search button { border:0; background:#2d6cf6; color:#fff;"
    " border-radius:10px; padding:13px 22px; font:inherit; font-weight:600;"
    " cursor:pointer; }\n"
    ".intro { color:#5b6470; text-align:left;"
    " margin:8px 2px 2px 2px; font-size:0.9em; line-height:1.45;"
    " flex:0 0 auto; }\n"
    ".search button:hover, form.ask button:not(:disabled):hover,"
    " .opt button:hover, .banner button:hover { background:#2560d8; }\n"
    ".search button:active, form.ask button:not(:disabled):active,"
    " .opt button:active, .banner button:active { background:#1f4fbf; }\n"
    "@media (max-width:560px) { .wrap { padding:18px 24px 24px; }"
    " .mark { width:44px; height:44px; line-height:44px; font-size:1.3em;"
    " margin-bottom:8px; }"
    " .mark-img { width:44px; height:44px; margin-bottom:8px; }"
    " .title { font-size:1.28em; }"
    " .tagline { display:none; }"
    " .intro { margin-top:10px; font-size:0.9em; }"
    " .search { flex-direction:column; gap:8px; }"
    " .search button { width:100%; } }\n"
    "</style></head>"
    "<body>"
    "<div class=\"wrap\">"
    "<div class=\"head\">"
    "__ND_LOGO_MARK__"
    "<div class=\"hgroup\"><div class=\"title\">AI Window</div>"
    "<div class=\"tagline\">A small AI model running entirely on your "
    "computer \xe2\x80\x94 nothing leaves this machine.</div></div></div>"
    "<div id=\"log\" role=\"log\" aria-live=\"polite\" aria-label=\"Chat\">"
    "<div class=\"msg bot\">Hi! I'm a small AI model running entirely on your "
    "computer \xe2\x80\x94 nothing leaves this machine. Ask me anything; I can "
    "also search the web or open pages for you.</div>"
    "</div>"
    "<form class=\"ask\" id=\"ask\">"
    "<input id=\"q\" name=\"q\" autocomplete=\"off\" size=\"48\" dir=\"auto\""
    " aria-label=\"Message the local model\""
    " placeholder=\"Message the local model…\" disabled>"
    "<button id=\"send\" type=\"submit\" disabled>Send</button>"
    "</form>"
    "<p class=\"footer\">"
    "<a href=\"about:start\">\xe2\x86\x90 Start</a> \xc2\xb7 "
    "<a href=\"#\" id=\"ainew\">New chat</a> \xc2\xb7 "
    "<span id=\"aifoot\"><span id=\"active\"></span>Local inference \xc2\xb7 </span>"
    "<a href=\"about:license\">License</a> \xc2\xb7 "
    "<a href=\"https://nordstjernen.org/privacy\">Privacy</a> \xc2\xb7 "
    "<a href=\"https://nordstjernen.org\">nordstjernen.org</a></p>"
    "</div>"
    "<script>\n"
    "var log=document.getElementById('log');\n"
    "var form=document.getElementById('ask');\n"
    "var input=document.getElementById('q');\n"
    "var send=document.getElementById('send');\n"
    "var foot=document.getElementById('active');\n"
    "var ready=false, banner=null, poll=null, picking=false, gpuShown=false;\n"
    "function bubble(cls,text){var d=document.createElement('div');\n"
    " d.className='msg '+cls; d.dir='auto'; d.textContent=text;\n"
    " log.appendChild(d);\n"
    " log.scrollTop=log.scrollHeight; return d;}\n"
    "function isImgUrl(u){return /[.](png|jpe?g|gif|webp|bmp|svg)([?#]|$)/i\n"
    "  .test(u);}\n"
    "function linkImg(img,url){ if(url.slice(0,5)==='data:') return img;\n"
    " var a=document.createElement('a'); a.href=url;\n"
    " a.title='Open image'; a.appendChild(img); return a;}\n"
    "function renderMsg(el,txt){ el.className='msg bot'; el.textContent='';\n"
    " var s=(txt||'').trim()||'(no response)';\n"
    " var re=/!\\[([^\\]]*)\\]\\(((?:https?:\\/\\/|data:)[^\\s)]+)\\)|"
    "\\[([^\\]]+)\\]\\((https?:\\/\\/[^\\s)]+)\\)|(https?:\\/\\/[^\\s<]+)/g;\n"
    " var last=0,m;\n"
    " while((m=re.exec(s))){\n"
    "  if(m.index>last) el.appendChild(document.createTextNode(\n"
    "    s.slice(last,m.index)));\n"
    "  if(m[2]!==undefined){var im=document.createElement('img');\n"
    "    im.className='genimg'; im.src=m[2]; if(m[1])im.alt=m[1];\n"
    "    el.appendChild(linkImg(im,m[2]));}\n"
    "  else if(m[4]!==undefined){var a=document.createElement('a');\n"
    "    a.href=m[4]; a.textContent=m[3]; el.appendChild(a);}\n"
    "  else {var u=m[5];\n"
    "    if(isImgUrl(u)){var ig=document.createElement('img');\n"
    "      ig.className='genimg'; ig.src=u; el.appendChild(linkImg(ig,u));}\n"
    "    else {var a2=document.createElement('a'); a2.href=u;\n"
    "      a2.textContent=u; el.appendChild(a2);}}\n"
    "  last=re.lastIndex;}\n"
    " if(last<s.length) el.appendChild(document.createTextNode(s.slice(last)));\n"
    " log.scrollTop=log.scrollHeight;}\n"
    "function ensureBanner(){ if(!banner){banner=document.createElement('div');\n"
    "  banner.className='banner'; log.appendChild(banner);} return banner;}\n"
    "function clearBanner(){ if(banner){banner.remove(); banner=null;} }\n"
    "function pick(id){ picking=false; fetch('about:ai-download?model='+\n"
    "  encodeURIComponent(id)).then(refresh,refresh); }\n"
    "function showPicker(st){ var b=ensureBanner(); ready=false; b.innerHTML='';\n"
    " var p=document.createElement('div');\n"
    " p.textContent='Choose a local model. Bigger models answer better but "
    "download more and run slower. Everything stays on your machine.';\n"
    " b.appendChild(p);\n"
    " (st.models||[]).forEach(function(m){ var row=document.createElement('div');\n"
    "  row.className='opt'+(m.id===st.active?' active':'');\n"
    "  var n=document.createElement('span'); n.className='name';\n"
    "  n.textContent=m.label; var s=document.createElement('span');\n"
    "  s.className='sz'; s.textContent='~'+(m.mb>=1000?(m.mb/1000).toFixed(1)+\n"
    "   ' GB':m.mb+' MB'); var sp=document.createElement('span');\n"
    "  sp.className='spacer';\n"
    "  row.appendChild(n); row.appendChild(s); row.appendChild(sp);\n"
    "  if(m.installed){ if(m.id===st.active){ var bd=document.createElement('span');\n"
    "    bd.className='badge'; bd.textContent='in use'; row.appendChild(bd); }\n"
    "   else { var u=document.createElement('button'); u.textContent='Use';\n"
    "    u.onclick=function(){pick(m.id);}; row.appendChild(u); } }\n"
    "  else { var dl=document.createElement('button'); dl.textContent='Download';\n"
    "   dl.onclick=function(){dl.disabled=true; dl.textContent='Starting…';\n"
    "    pick(m.id);}; row.appendChild(dl); }\n"
    "  b.appendChild(row); });\n"
    " if(st.message){var e=document.createElement('div'); e.className='hint';\n"
    "  e.textContent='Last download failed: '+st.message; b.appendChild(e);}\n"
    " log.scrollTop=log.scrollHeight;}\n"
    "function showDownloading(st){ var b=ensureBanner(); ready=false;\n"
    " var lab=''; (st.models||[]).forEach(function(m){if(m.id===st.downloading)lab=m.label;});\n"
    " var mb=st.total?(st.total/1048576).toFixed(0):'?';\n"
    " var got=st.received?(st.received/1048576).toFixed(0):'0';\n"
    " b.innerHTML='Downloading '+(lab||'model')+'\\u2026 '+got+' / '+mb+' MB ('+\n"
    "  (st.percent||0)+'%)<div class=\"bar\"><div style=\"width:'+\n"
    "  (st.percent||0)+'%\"></div></div>"
    "<div class=\"hint\">This happens once; afterwards everything is local.</div>';\n"
    " log.scrollTop=log.scrollHeight;}\n"
    "function enableChat(st){ready=true; clearBanner(); input.disabled=false;\n"
    " send.disabled=false;\n"
    " var lab=''; (st.models||[]).forEach(function(m){if(m.id===st.active)lab=m.label;});\n"
    " foot.innerHTML=''; if(lab){foot.appendChild(document.createTextNode(lab));\n"
    "  var c=document.createElement('button'); c.className='change';\n"
    "  c.type='button';\n"
    "  c.textContent='change'; c.onclick=function(){picking=true; refresh();};\n"
    "  foot.appendChild(c);\n"
    "  var acc=st.gpu?(' \\u00b7 GPU: '+st.gpu+' ('+st.gpu_layers+' layers)'):'';\n"
    "  foot.appendChild(document.createTextNode(acc+' \\u00b7 '));}\n"
    " input.focus();}\n"
    "function apply(st){\n"
    " if(st.state==='downloading'){showDownloading(st);\n"
    "  if(!poll) poll=setInterval(refresh,1000); return;}\n"
    " if(poll){clearInterval(poll); poll=null;}\n"
    " if(picking){showPicker(st); return;}\n"
    " if(st.state==='ready'){enableChat(st);}\n"
    " else { showPicker(st); }}\n"
    "function refresh(){return fetch('about:ai-status').then(function(r){\n"
    "  return r.json();}).then(apply).catch(function(){});}\n"
    "var phases={'starting':'thinking\\u2026','loading model':\n"
    " 'loading the model\\u2026','thinking':'thinking\\u2026',\n"
    " 'searching':'searching the web\\u2026'};\n"
    "function chatDone(t,txt){\n"
    " if(txt.indexOf('@@NAVIGATE@@')===0){var u=txt.slice(12).trim();\n"
    "   if(!/^https?:\\/\\//i.test(u)){renderMsg(t,u); return;}\n"
    "   t.className='msg bot';\n"
    "   t.textContent='Opening '+u+' \\u2026';\n"
    "   log.scrollTop=log.scrollHeight;\n"
    "   setTimeout(function(){window.location.href=u;},350); return;}\n"
    " renderMsg(t,txt);}\n"
    "function chatPoll(t){\n"
    " fetch('about:ai-poll').then(function(r){return r.json();})\n"
    " .then(function(j){\n"
    "   if(j.state==='running'){\n"
    "     if(j.text){renderMsg(t,j.text); t.className='msg bot';}\n"
    "     else {var ph=phases[j.phase]||'thinking\\u2026';\n"
    "       if(t.textContent!==ph){t.className='msg bot think';\n"
    "         t.textContent=ph;}}\n"
    "     setTimeout(function(){chatPoll(t);},250); return;}\n"
    "   if(j.state==='error'){t.className='msg bot';\n"
    "     t.textContent=j.message||'Something went wrong.';}\n"
    "   else chatDone(t,j.text||'');\n"
    "   input.disabled=false; send.disabled=false; input.focus();\n"
    "   if(!gpuShown){gpuShown=true; refresh();}\n"
    " }).catch(function(err){t.className='msg bot';\n"
    "   t.textContent='Error: '+err;\n"
    "   input.disabled=false; send.disabled=false;});}\n"
    "form.addEventListener('submit',function(e){e.preventDefault();\n"
    " if(!ready) return; var q=input.value.trim(); if(!q) return;\n"
    " bubble('user',q); input.value=''; input.disabled=true;\n"
    " send.disabled=true;\n"
    " var t=bubble('bot think','thinking…');\n"
    " fetch('about:ai?q='+encodeURIComponent(q)).then(function(r){\n"
    "   return r.json();}).then(function(j){\n"
    "   if(j.state==='error'){t.className='msg bot';\n"
    "     t.textContent=j.message||'Something went wrong.';\n"
    "     input.disabled=false; send.disabled=false; return;}\n"
    "   if(j.busy){t.className='msg bot';\n"
    "     t.textContent='I\\u2019m still answering the previous question "
    "\\u2014 try again in a moment.';\n"
    "     input.disabled=false; send.disabled=false; return;}\n"
    "   chatPoll(t);}).catch(function(err){\n"
    "   t.className='msg bot'; t.textContent='Error: '+err;\n"
    "   input.disabled=false; send.disabled=false;});\n"
    "});\n"
    "var ainew=document.getElementById('ainew');\n"
    "if(ainew) ainew.addEventListener('click',function(e){\n"
    " e.preventDefault();\n"
    " fetch('about:ai-reset').then(function(){\n"
    "  var ms=log.querySelectorAll('.msg');\n"
    "  for(var i=0;i<ms.length;i++) ms[i].remove();\n"
    "  input.focus();});});\n"
    "refresh();\n"
    "</script>"
    "</body></html>";
#else
static const char k_about_start_template[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<meta name=\"color-scheme\" content=\"light\">"
    "<title>Nordstjernen</title>"
    "<style>\n"
    "html, body { background:#ffffff; color:#111418;"
    " font-family: system-ui, -apple-system, \"Segoe UI\","
    " Helvetica, Arial, sans-serif; margin:0; padding:0; min-height:100%; }\n"
    ".wrap { max-width: 760px; margin: 0 auto; padding: 34px 24px 32px;"
    " box-sizing:border-box; }\n"
    ".head { text-align:center; margin-bottom:18px; }\n"
    ".mark { display:block; width:58px; height:58px; line-height:58px;"
    " margin:0 auto 12px; border:1px solid #d9dee7; border-radius:14px;"
    " background:#f6f8fb; color:#1b2a4a; font-weight:700;"
    " font-size:1.7em; text-align:center; }\n"
    ".mark-img { display:block; width:58px; height:58px;"
    " margin:0 auto 12px; border-radius:14px; object-fit:cover; }\n"
    ".hgroup { display:block; text-align:center; }\n"
    ".title { font-size: 1.5em; font-weight: 600; line-height:1.2; }\n"
    ".tagline { color:#5b6470; font-style: italic; font-size: 0.84em;"
    " margin:4px auto 0; line-height:1.25; max-width:36em; }\n"
    ".intro { color:#4b5563; text-align:center; line-height:1.5;"
    " margin:0 auto 22px; max-width:620px; }\n"
    ".search { display:flex; gap:10px; margin:0 0 18px; }\n"
    ".search input { flex:1 1 auto; border:1px solid #c9cfd9;"
    " background:#ffffff; color:#111418; border-radius:10px;"
    " padding:13px 16px; font:inherit; font-size:1.03em; outline:none; }\n"
    ".search input:focus { border-color:#2d6cf6; }\n"
    ".search button { border:0; background:#2d6cf6; color:#fff;"
    " border-radius:10px; padding:13px 22px; font:inherit; font-weight:600;"
    " cursor:pointer; }\n"
    ".search button:hover { background:#2560d8; }\n"
    ".links { text-align:center; color:#6b7280; font-size:0.86em;"
    " line-height:1.7; }\n"
    ".links a { color:#2d6cf6; margin:0 8px; }\n"
    "@media (max-width:560px) { .wrap { padding:24px 24px 28px; }"
    " .head { margin-bottom:18px; }"
    " .mark { width:48px; height:48px; line-height:48px; font-size:1.4em;"
    " margin-bottom:10px; }"
    " .mark-img { width:48px; height:48px; margin-bottom:10px; }"
    " .title { font-size:1.28em; }"
    " .tagline { display:none; }"
    " .intro { display:none; }"
    " .search { flex-direction:column; gap:8px; }"
    " .search button { width:100%; }"
    " .links a { display:inline-block; margin:0 5px 4px; } }\n"
    "</style></head>"
    "<body><main class=\"wrap\">"
    "__ND_SPLASH__"
    "<form class=\"search\" id=\"dsearch\">"
    "<input id=\"sq\" name=\"q\" autocomplete=\"off\" autofocus dir=\"auto\""
    " aria-label=\"Search the web\""
    " size=\"__ND_SEARCH_SIZE__\""
    " placeholder=\"Search (__ND_SEARCH_NAME__):\">"
    "<button type=\"submit\">Search</button>"
    "</form>"
    "<p class=\"links\">"
    "<a href=\"about:history\">History</a>"
    "<a href=\"about:license\">License</a>"
    "<a href=\"https://nordstjernen.org/privacy\">Privacy</a>"
    "<a href=\"https://nordstjernen.org\">nordstjernen.org</a>"
    "</p>"
    "</main>"
    "<script>\n"
    "var searchUrl='__ND_SEARCH_URL__';\n"
    "var dsearch=document.getElementById('dsearch');\n"
    "var sq=document.getElementById('sq');\n"
    "dsearch.addEventListener('submit',function(e){e.preventDefault();\n"
    " var s=sq.value.trim(); if(!s) return;\n"
    " window.location.href=searchUrl.indexOf('%s')>=0\n"
    "  ? searchUrl.replace('%s',encodeURIComponent(s))\n"
    "  : searchUrl+encodeURIComponent(s);});\n"
    "try{sq.focus();}catch(e){}\n"
    "</script></body></html>";
#endif

static const char k_about_nordstjernen_template[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<meta name=\"color-scheme\" content=\"light\">"
    "<title>About Nordstjernen</title>"
    "<style>\n"
    "html, body { background:#ffffff; color:#111418;"
    " font-family: system-ui, -apple-system, \"Segoe UI\","
    " Helvetica, Arial, sans-serif; margin:0; padding:0; min-height:100%; }\n"
    ".wrap { max-width: 640px; margin: 0 auto; padding: 40px 24px 32px;"
    " box-sizing:border-box; }\n"
    ".head { text-align:center; margin-bottom:22px; }\n"
    ".mark-img { display:block; width:104px; height:104px;"
    " margin:0 auto 16px; border-radius:22px; object-fit:cover; }\n"
    ".title { font-size: 1.7em; font-weight: 600; line-height:1.15; }\n"
    ".ver { color:#6b7280; font-size:0.92em; margin-top:5px; }\n"
    ".tagline { color:#5b6470; font-style: italic; font-size:0.9em;"
    " margin:6px auto 0; line-height:1.25; }\n"
    ".intro { color:#4b5563; text-align:center; line-height:1.55;"
    " margin:0 auto 18px; max-width:560px; }\n"
    ".intro b { color:#111418; }\n"
    ".license { max-width:560px; margin:24px auto 8px;"
    " border-top:1px solid #e6e9ef; padding-top:22px; }\n"
    ".license h2 { font-size:1.02em; font-weight:600; margin:0 0 8px;"
    " text-align:center; }\n"
    ".license p { color:#4b5563; line-height:1.55; font-size:0.92em;"
    " margin:0 auto 10px; text-align:center; }\n"
    ".license .third { color:#6b7280; font-size:0.86em; }\n"
    ".docs { list-style:none; margin:16px auto 0; padding:0;"
    " max-width:400px; }\n"
    ".docs li { margin:0 0 8px; }\n"
    ".docs a { color:#2d6cf6; font-weight:600; font-size:0.95em;"
    " text-decoration:none; }\n"
    ".docs a:hover { text-decoration:underline; }\n"
    ".diag { max-width:560px; margin:26px auto 0; text-align:left;"
    " border-top:1px solid #e6e9ef; padding-top:20px; }\n"
    ".diag h3 { font-size:0.72em; text-transform:uppercase;"
    " letter-spacing:0.05em; color:#8a94a3; font-weight:700;"
    " margin:18px 0 6px; }\n"
    ".diag h3:first-child { margin-top:0; }\n"
    ".drow { display:flex; justify-content:space-between; gap:18px;"
    " padding:5px 0; border-bottom:1px solid #f0f2f5; font-size:0.85em; }\n"
    ".dk { color:#4b5563; flex:0 0 auto; }\n"
    ".dv { color:#111418; text-align:right; word-break:break-word;"
    " font-family:ui-monospace,\"SF Mono\",Menlo,Consolas,monospace; }\n"
    ".dv.on { color:#137a3f; font-family:inherit; }\n"
    ".dv.off { color:#9aa3af; font-family:inherit; }\n"
    ".copy { text-align:center; color:#6b7280; font-size:0.85em;"
    " margin:26px 0 14px; }\n"
    ".links { text-align:center; color:#6b7280; font-size:0.86em;"
    " line-height:1.7; }\n"
    ".links a { color:#2d6cf6; margin:0 8px; }\n"
    "@media (max-width:560px) { .wrap { padding:28px 22px; }"
    " .mark-img { width:84px; height:84px; }"
    " .title { font-size:1.42em; }"
    " .drow { font-size:0.8em; }"
    " .links a { display:inline-block; margin:0 5px 4px; } }\n"
    "</style></head>"
    "<body><main class=\"wrap\">"
    "<div class=\"head\">"
    "__ND_LOGO_MARK__"
    "<div class=\"title\">Nordstjernen</div>"
    "<div class=\"ver\">Version " NS_VERSION "</div>"
    "<div class=\"tagline\">The unique, legendary web browser</div>"
    "</div>"
    "<p class=\"intro\">A web browser implemented in C.</p>"
    "<section class=\"license\">"
    "<h2>License</h2>"
    "<p>Nordstjernen is distributed under the <b>Nordstjernen Source "
    "License v1.0 (NSL-1.0)</b>, \xc2\xa9 2026 Andreas R\xc3\xb8sdal.</p>"
    "<p class=\"third\">It bundles third-party open-source software under "
    "the MIT, BSD, Apache\xc2\xa0" "2.0, LGPL, MPL and zlib licenses \xe2"
    "\x80\x94 including lexbor, QuickJS, Wuffs, GTK\xc2\xa0" "4, Cairo, "
    "libcurl, OpenSSL, uchardet, libwebp, libpsl, SQLite and zlib. Each "
    "component\xe2\x80\x99s copyright notice and full license text is "
    "reproduced in the notices below.</p>"
    "<ul class=\"docs\">"
    "<li><a href=\"about:license\">Nordstjernen Source License (NSL-1.0)"
    " \xe2\x86\x92</a></li>"
    "<li><a href=\"about:third-party\">Third-party software notices \xe2"
    "\x86\x92</a></li>"
    "</ul>"
    "</section>"
    "__ND_DIAG__"
    "<p class=\"copy\">Nordstjernen Web Browser \xc2\xa9 2026 "
    "Andreas R\xc3\xb8sdal</p>"
    "<p class=\"links\">"
    "<a href=\"about:start\">\xe2\x86\x90 Start</a>"
    "<a href=\"about:license\">License</a>"
    "<a href=\"https://nordstjernen.org/privacy\">Privacy</a>"
    "<a href=\"https://nordstjernen.org\">nordstjernen.org</a>"
    "</p>"
    "</main></body></html>";

static const char k_about_settings_html[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<meta name=\"color-scheme\" content=\"light\">\n"
"<title>Settings</title>\n"
"<style>\n"
"html,body{margin:0;background:#f4f6f9;color:#16202e;font-family:system-ui,"
"-apple-system,\"Segoe UI\",Helvetica,Arial,sans-serif;min-height:100%}\n"
".bar{display:flex;align-items:center;gap:12px;padding:11px 18px;"
"background:#1b2a4a;color:#fff;position:sticky;top:0;z-index:5}\n"
".brand{font-weight:700;font-size:1.12em}\n"
"main{max-width:680px;margin:0 auto;padding:18px}\n"
".card{background:#fff;border-radius:12px;padding:18px 20px;margin:0 0 14px;"
"box-shadow:0 1px 3px rgba(20,30,50,.08)}\n"
"h2{font-size:1.1em;margin:.1em 0 .7em}\n"
".field{display:flex;flex-direction:column;gap:6px;font-size:.84em;"
"color:#5b6470;font-weight:600;margin:0 0 14px}\n"
".field:last-child{margin-bottom:0}\n"
"input,select{font:inherit;color:#16202e;border:1px solid #ccd3de;"
"border-radius:8px;padding:9px 11px;background:#fff;font-weight:400}\n"
"input:focus,select:focus{outline:2px solid #2d6cf6;outline-offset:-1px;"
"border-color:#2d6cf6}\n"
".toggle{display:flex;align-items:center;gap:10px;font-size:.92em;"
"color:#27313f;font-weight:500;margin:0 0 12px;cursor:pointer}\n"
".toggle:last-child{margin-bottom:0}\n"
".toggle input{width:18px;height:18px;accent-color:#2d6cf6;margin:0}\n"
"button{font:inherit;border:0;border-radius:8px;padding:9px 18px;cursor:pointer;"
"background:#2d6cf6;color:#fff;font-weight:600}\n"
"button.ghost{background:#e7ebf2;color:#16202e}\n"
"button:hover{filter:brightness(1.05)}\n"
".row{display:flex;align-items:center;gap:12px;margin-top:6px}\n"
".spacer{flex:1 1 auto}\n"
".note{color:#8a93a3;font-size:.85em;text-align:center;margin-top:14px}\n"
".ok{color:#1a8a4a;font-weight:600}\n"
"#custom_wrap[hidden]{display:none}\n"
"</style></head><body>\n"
"<header class=\"bar\"><div class=\"brand\">\xe2\x9a\x99 Settings</div></header>\n"
"<main>\n"
"<section class=\"card\"><h2>General</h2>"
"<label class=\"field\">Home page<input id=\"home_url\" type=\"text\"></label>"
"<label class=\"field\">Search engine<select id=\"search_pick\"></select></label>"
"<label class=\"field\" id=\"custom_wrap\">Custom search URL"
"<input id=\"search_engine\" type=\"text\" "
"placeholder=\"https://example.com/?q=%s\"></label>"
"</section>\n"
"<section class=\"card\"><h2>Privacy</h2>"
"<label class=\"field\">Cookies<select id=\"cookie_policy\">"
"<option value=\"0\">Accept all cookies</option>"
"<option value=\"1\">Block third-party cookies</option>"
"<option value=\"2\">Block all cookies</option></select></label>"
"<label class=\"toggle\"><input type=\"checkbox\" id=\"do_not_track\">"
"Send \xe2\x80\x9c" "Do Not Track\xe2\x80\x9d</label>"
"<label class=\"toggle\"><input type=\"checkbox\" id=\"global_privacy_control\">"
"Send Global Privacy Control (GPC)</label>"
"<label class=\"toggle\"><input type=\"checkbox\" id=\"strip_tracking_params\">"
"Strip tracking parameters from links</label>"
"<label class=\"toggle\"><input type=\"checkbox\" id=\"https_first\">"
"Try HTTPS first on every site</label>"
"</section>\n"
"<section class=\"card\"><h2>Content</h2>"
"<label class=\"toggle\"><input type=\"checkbox\" id=\"images_enabled\">"
"Load images</label>"
"<label class=\"toggle\"><input type=\"checkbox\" id=\"webgl_enabled\">"
"Enable WebGL</label>"
"<label class=\"toggle\"><input type=\"checkbox\" id=\"local_storage_enabled\">"
"Enable local storage</label>"
"<label class=\"toggle\"><input type=\"checkbox\" id=\"cache_enabled\">"
"Enable cache</label>"
"</section>\n"
"<div class=\"row\">"
"<button id=\"clear\" class=\"ghost\">Clear all browsing data</button>"
"<span class=\"spacer\"></span><span id=\"status\"></span>"
"<button id=\"save\">Save</button></div>\n"
"<p class=\"note\">Changes apply to newly opened pages.</p>\n"
"</main>\n"
"<script>\n"
"function $(i){return document.getElementById(i);}\n"
"function enc(o){var p=[];for(var k in o)p.push(encodeURIComponent(k)+'='+"
"encodeURIComponent(o[k]==null?'':o[k]));return p.join('&');}\n"
"var engines=[{n:'DuckDuckGo',u:'https://lite.duckduckgo.com/lite/?q=%s'},"
"{n:'Startpage',u:'https://www.startpage.com/sp/search?query=%s'},"
"{n:'Google',u:'https://www.google.com/search?q=%s'},"
"{n:'Bing',u:'https://www.bing.com/search?q=%s'},"
"{n:'Brave',u:'https://search.brave.com/search?q=%s'},"
"{n:'Wikipedia',u:'https://en.wikipedia.org/w/index.php?search=%s'}];\n"
"function buildPick(cur){var sel=$('search_pick');sel.textContent='';"
"var matched=false;engines.forEach(function(e){var o=document.createElement("
"'option');o.value=e.u;o.textContent=e.n;if(e.u===cur){o.selected=true;"
"matched=true;}sel.appendChild(o);});"
"var oc=document.createElement('option');oc.value='custom';"
"oc.textContent='Custom\\u2026';if(!matched)oc.selected=true;"
"sel.appendChild(oc);"
"$('custom_wrap').hidden=matched;}\n"
"$('search_pick')&&($('search_pick').onchange=function(){"
"var v=this.value;if(v==='custom'){$('custom_wrap').hidden=false;}"
"else{$('custom_wrap').hidden=true;$('search_engine').value=v;}});\n"
"function load(){fetch('about:settings-data').then(function(r){"
"return r.json();}).then(function(c){"
"$('home_url').value=c.home_url||'';"
"$('search_engine').value=c.search_engine||'';buildPick(c.search_engine||'');"
"$('cookie_policy').value=''+(c.cookie_policy||0);"
"['do_not_track','global_privacy_control','strip_tracking_params',"
"'https_first','images_enabled','webgl_enabled','local_storage_enabled',"
"'cache_enabled'].forEach(function(k){$(k).checked=!!c[k];});});}\n"
"function bv(id){return $(id).checked?'1':'0';}\n"
"function save(){var se=$('search_pick').value;"
"if(se!=='custom')$('search_engine').value=se;"
"fetch('about:settings-save',{method:'POST',headers:{'Content-Type':"
"'application/x-www-form-urlencoded'},body:enc({"
"home_url:$('home_url').value,search_engine:$('search_engine').value,"
"cookie_policy:$('cookie_policy').value,do_not_track:bv('do_not_track'),"
"global_privacy_control:bv('global_privacy_control'),"
"strip_tracking_params:bv('strip_tracking_params'),"
"https_first:bv('https_first'),images_enabled:bv('images_enabled'),"
"webgl_enabled:bv('webgl_enabled'),"
"local_storage_enabled:bv('local_storage_enabled'),"
"cache_enabled:bv('cache_enabled')})}).then(function(){"
"$('status').textContent='Saved.';$('status').className='ok';"
"setTimeout(function(){$('status').textContent='';},2500);});}\n"
"function clearData(){var b=$('clear');b.disabled=true;"
"fetch('about:settings-clear',{method:'POST'}).then(function(){"
"b.textContent='Browsing data cleared';});}\n"
"$('save').onclick=save;$('clear').onclick=clearData;\n"
"load();\n"
"</script></body></html>";

static char *
about_request_form(const char *url, const char *method,
                   const void *body, gsize body_len)
{
    if (method && g_ascii_strcasecmp(method, "POST") == 0 && body && body_len)
        return g_strndup((const char *)body, body_len);
    const char *qs = strchr(url, '?');
    return g_strdup(qs ? qs + 1 : "");
}

static void
about_emit_json(ns_response *resp, char *json)
{
    g_free(resp->content_type);
    resp->content_type = g_strdup("application/json; charset=utf-8");
    g_byte_array_append(resp->body, (const guint8 *)json, (guint)strlen(json));
    g_free(json);
}

static char *
about_json_escape(const char *s)
{
    GString *o = g_string_new(NULL);
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') g_string_append_c(o, '\\');
        if ((guchar)*s < 0x20) { g_string_append_printf(o, "\\u%04x", *s); continue; }
        g_string_append_c(o, *s);
    }
    return g_string_free(o, FALSE);
}

static char *
about_settings_json(void)
{
    const ns_config *c = ns_config_get();
    char *home = about_json_escape(c && c->home_url ? c->home_url : "");
    char *eng = about_json_escape(c && c->search_engine ? c->search_engine : "");
    char *json = g_strdup_printf(
        "{\"home_url\":\"%s\",\"search_engine\":\"%s\",\"cookie_policy\":%d,"
        "\"do_not_track\":%s,\"global_privacy_control\":%s,"
        "\"strip_tracking_params\":%s,\"https_first\":%s,"
        "\"images_enabled\":%s,\"webgl_enabled\":%s,"
        "\"local_storage_enabled\":%s,\"cache_enabled\":%s}",
        home, eng, c ? (int)c->cookie_policy : 1,
        (c && c->do_not_track) ? "true" : "false",
        (c && c->global_privacy_control) ? "true" : "false",
        (c && c->strip_tracking_params) ? "true" : "false",
        (c && c->https_first) ? "true" : "false",
        (c && c->images_enabled) ? "true" : "false",
        (c && c->webgl_enabled) ? "true" : "false",
        (c && c->local_storage_enabled) ? "true" : "false",
        (c && c->cache_enabled) ? "true" : "false");
    g_free(home);
    g_free(eng);
    return json;
}

static void
about_settings_save(const char *form)
{
    GHashTable *q = form && *form
        ? g_uri_parse_params(form, -1, "&", G_URI_PARAMS_WWW_FORM, NULL)
        : NULL;
    if (!q) return;
    ns_config_lock();
    ns_config *c = ns_config_mut();
    const char *v;
    if ((v = g_hash_table_lookup(q, "home_url"))) {
        g_free(c->home_url); c->home_url = g_strdup(v);
    }
    if ((v = g_hash_table_lookup(q, "search_engine"))) {
        g_free(c->search_engine); c->search_engine = g_strdup(v);
    }
    if ((v = g_hash_table_lookup(q, "cookie_policy")))
        c->cookie_policy = (ns_cookie_policy)atoi(v);
    if ((v = g_hash_table_lookup(q, "do_not_track")))
        c->do_not_track = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "global_privacy_control")))
        c->global_privacy_control = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "strip_tracking_params")))
        c->strip_tracking_params = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "https_first")))
        c->https_first = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "images_enabled")))
        c->images_enabled = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "webgl_enabled")))
        c->webgl_enabled = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "local_storage_enabled")))
        c->local_storage_enabled = atoi(v) != 0;
    if ((v = g_hash_table_lookup(q, "cache_enabled")))
        c->cache_enabled = atoi(v) != 0;
    ns_config_save(NULL);
    ns_config_unlock();
    g_hash_table_destroy(q);
}

static void
about_settings_clear(void)
{
    ns_history_clear();
    ns_cache_clear();
    ns_net_cookies_clear();
    ns_net_site_storage_clear();
}

static const char *
about_start_tagline(void)
{
    static const char *const taglines[] = {
        "Nordstjernen the unique web browser",
        "Northstar the unique web browser",
        "Nordstjärnan the unique web browser",
        "Étoile du Nord the unique web browser",
        "Nordstern the unique web browser",
    };
    return taglines[g_random_int_range(0, G_N_ELEMENTS(taglines))];
}

static gboolean
about_request_from_chrome(const char *top_url)
{
    return !top_url || !*top_url || g_str_has_prefix(top_url, "about:");
}

static gboolean
synthesize_about_response(const char *url, const char *top_url,
                          const char *method, const void *req_body,
                          gsize req_body_len, ns_response *resp)
{
    if (!g_str_has_prefix(url, "about:")) return FALSE;
    const char *what = url + strlen("about:");
    if ((g_str_has_prefix(what, "ai") || g_str_equal(what, "history") ||
         g_str_has_prefix(what, "settings")) &&
        !about_request_from_chrome(top_url)) {
        resp->status = 403;
        resp->final_url = g_strdup(url);
        resp->content_type = g_strdup("text/plain; charset=utf-8");
        const char *body = "about: pages are not available to web content";
        g_byte_array_append(resp->body, (const guint8 *)body,
                            (guint)strlen(body));
        return TRUE;
    }
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = g_strdup("text/html; charset=utf-8");
    if (g_str_equal(what, "blank") || g_str_equal(what, "")) {
        const char *body = "<!doctype html><title>Blank</title>";
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
    } else if (g_str_equal(what, "start") || g_str_equal(what, "home") ||
               g_str_equal(what, "newtab")) {
        ns_config_lock();
        const ns_config *scfg = ns_config_get();
        g_autofree char *engine_dup =
            (scfg && scfg->search_engine && *scfg->search_engine)
            ? g_strdup(scfg->search_engine)
            : g_strdup("https://lite.duckduckgo.com/lite/?q=%s");
        ns_config_unlock();
        const char *engine = engine_dup;
        GString *esc_engine = g_string_new(NULL);
        for (const char *p = engine; *p; p++) {
            if (*p == '\'' || *p == '\\' || *p == '<' || *p == '"')
                g_string_append_printf(esc_engine, "\\u%04x", (guchar)*p);
            else
                g_string_append_c(esc_engine, *p);
        }
        char *engine_host = ns_url_host_from(engine);
        const char *search_name = "the web";
        g_autofree char *engine_host_lower = NULL;
        if (engine_host && *engine_host) {
            engine_host_lower = g_ascii_strdown(engine_host, -1);
            const psl_ctx_t *psl = psl_builtin();
            const char *reg =
                psl ? psl_registrable_domain(psl, engine_host_lower) : NULL;
            search_name = (reg && *reg) ? reg
                : (g_str_has_prefix(engine_host, "www.") ? engine_host + 4
                                                         : engine_host);
        }
        char *with_search = about_substitute(k_about_start_template,
                                             "__ND_SEARCH_URL__",
                                             esc_engine->str);
        char *esc_name = g_markup_escape_text(search_name, -1);
        char *with_name = about_substitute(with_search,
                                           "__ND_SEARCH_NAME__",
                                           esc_name);
        g_free(esc_name);
        g_free(with_search);
        glong ph_len = g_utf8_strlen(search_name, -1) + 10;
        if (ph_len < 20) ph_len = 20;
        if (ph_len > 80) ph_len = 80;
        char *size_val = g_strdup_printf("%ld", ph_len);
        char *with_size = about_substitute(with_name,
                                           "__ND_SEARCH_SIZE__",
                                           size_val);
        g_free(size_val);
        g_free(with_name);
        with_name = with_size;
        g_string_free(esc_engine, TRUE);
        g_free(engine_host);
        char *logo_markup = about_logo_markup();
        char *with_logo = about_substitute(with_name,
                                           "__ND_LOGO_MARK__",
                                           logo_markup);
        g_free(logo_markup);
        g_free(with_name);
        char *splash_markup = about_splash_markup();
        char *with_splash = about_substitute(with_logo, "__ND_SPLASH__",
                                             splash_markup);
        g_free(splash_markup);
        g_free(with_logo);
        char *body = about_substitute(with_splash, "__ND_TAGLINE__",
                                      about_start_tagline());
        g_free(with_splash);
        g_byte_array_append(resp->body, (const guint8 *)body,
                            (guint)strlen(body));
        g_free(body);
#if defined(NS_HAVE_AI) && !defined(__APPLE__)
    } else if (g_str_equal(what, "ai-window")) {
        char *logo_markup = about_logo_markup();
        char *with_logo = about_substitute(k_about_ai_window_template,
                                           "__ND_LOGO_MARK__", logo_markup);
        g_free(logo_markup);
        g_byte_array_append(resp->body, (const guint8 *)with_logo,
                            (guint)strlen(with_logo));
        g_free(with_logo);
#endif
    } else if (g_str_has_prefix(what, "ai-status")) {
        char *json = ns_ai_status_json();
        g_free(resp->content_type);
        resp->content_type = g_strdup("application/json; charset=utf-8");
        g_byte_array_append(resp->body, (const guint8 *)json,
                            (guint)strlen(json));
        g_free(json);
    } else if (g_str_has_prefix(what, "ai-download")) {
        const char *qs = strchr(url, '?');
        char *model = NULL;
        if (qs) {
            GHashTable *q = g_uri_parse_params(qs + 1, -1, "&",
                                               G_URI_PARAMS_WWW_FORM, NULL);
            if (q) {
                const char *raw = g_hash_table_lookup(q, "model");
                if (raw) model = g_strdup(raw);
                g_hash_table_destroy(q);
            }
        }
        ns_ai_select_download(model ? model : "");
        g_free(model);
        char *json = ns_ai_status_json();
        g_free(resp->content_type);
        resp->content_type = g_strdup("application/json; charset=utf-8");
        g_byte_array_append(resp->body, (const guint8 *)json,
                            (guint)strlen(json));
        g_free(json);
    } else if (g_str_has_prefix(what, "ai-poll")) {
        char *json = ns_ai_chat_poll();
        g_free(resp->content_type);
        resp->content_type = g_strdup("application/json; charset=utf-8");
        g_byte_array_append(resp->body, (const guint8 *)json,
                            (guint)strlen(json));
        g_free(json);
    } else if (g_str_has_prefix(what, "ai-reset")) {
        ns_ai_chat_reset();
        const char *json = "{\"ok\":true}";
        g_free(resp->content_type);
        resp->content_type = g_strdup("application/json; charset=utf-8");
        g_byte_array_append(resp->body, (const guint8 *)json,
                            (guint)strlen(json));
    } else if (g_str_has_prefix(what, "ai?") || g_str_equal(what, "ai")) {
        const char *qs = strchr(url, '?');
        char *msg = NULL;
        if (qs) {
            GHashTable *q = g_uri_parse_params(qs + 1, -1, "&",
                                               G_URI_PARAMS_WWW_FORM, NULL);
            if (q) {
                const char *raw = g_hash_table_lookup(q, "q");
                if (raw) msg = g_strdup(raw);
                g_hash_table_destroy(q);
            }
        }
        char *json = ns_ai_chat_start(msg ? msg : "");
        g_free(msg);
        g_free(resp->content_type);
        resp->content_type = g_strdup("application/json; charset=utf-8");
        g_byte_array_append(resp->body, (const guint8 *)json,
                            (guint)strlen(json));
        g_free(json);
    } else if (g_str_equal(what, "nordstjernen") || g_str_equal(what, "about")) {
        char *logo_markup = about_logo_markup();
        char *with_logo = about_substitute(k_about_nordstjernen_template,
                                           "__ND_LOGO_MARK__", logo_markup);
        g_free(logo_markup);
        char *diag = about_diagnostics_html();
        char *body = about_substitute(with_logo, "__ND_DIAG__", diag);
        g_free(diag);
        g_free(with_logo);
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "license") || g_str_equal(what, "licence")) {
        char *body = build_about_license();
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "third-party") ||
               g_str_equal(what, "third-party-licenses") ||
               g_str_equal(what, "credits")) {
        char *body = build_about_third_party();
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "history")) {
        char *body = ns_history_html_page();
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "settings")) {
        g_byte_array_append(resp->body, (const guint8 *)k_about_settings_html,
                            (guint)strlen(k_about_settings_html));
    } else if (g_str_has_prefix(what, "settings-data")) {
        about_emit_json(resp, about_settings_json());
    } else if (g_str_has_prefix(what, "settings-save")) {
        char *form = about_request_form(url, method, req_body, req_body_len);
        about_settings_save(form);
        g_free(form);
        about_emit_json(resp, g_strdup("{\"ok\":true}"));
    } else if (g_str_has_prefix(what, "settings-clear")) {
        about_settings_clear();
        about_emit_json(resp, g_strdup("{\"ok\":true}"));
    } else {
        const char *body = "<!doctype html><title>Nordstjernen</title>";
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
    }
    return TRUE;
}

static gboolean
is_simple_get(const char *method)
{
    return !method || !*method || g_ascii_strcasecmp(method, "GET") == 0;
}

static ns_response *
response_from_cache_entry(ns_cache_entry *e)
{
    ns_response *resp = g_new0(ns_response, 1);
    resp->status       = e->status;
    resp->final_url    = g_strdup(e->final_url);
    resp->content_type = g_strdup(e->content_type);
    resp->cors_allow_origin = g_strdup(e->cors_allow_origin);
    resp->body         = e->body;
    e->body = NULL;
    return resp;
}

static gboolean ns_fetch_is_navigation(const char *top_url,
                                       GPtrArray *extra_headers);

static gboolean g_navigation_fetch;

void
ns_net_set_navigation_fetch(gboolean navigation)
{
    g_navigation_fetch = navigation;
}

static ns_response *
ns_fetch_sync_hop(const char *url, const char *top_url, const char *method,
                  const void *body, gsize body_len, const char *content_type,
                  GPtrArray *extra_headers,
                  GCancellable *cancellable, GError **error,
                  gboolean follow_redirects, char **location_out)
{
    if (location_out) *location_out = NULL;
    gboolean is_navigation = ns_fetch_is_navigation(top_url, extra_headers)
                             || g_navigation_fetch;
    ns_response *resp = g_new0(ns_response, 1);
    resp->body = g_byte_array_new();

    if (!is_navigation) {
        char *dead_host = ns_url_host_from(url);
        gboolean dead = dead_host && *dead_host &&
                        ns_net_host_recently_dead(dead_host);
        g_free(dead_host);
        if (dead) {
            resp->error =
                g_strdup("host unreachable (recent connection failure)");
            resp->final_url = g_strdup(url);
            return resp;
        }
    }

    if (synthesize_about_response(url, top_url, method, body, body_len, resp))
        return resp;
    if (synthesize_data_response(url, resp))
        return resp;
    if (synthesize_file_response(url, top_url, resp))
        return resp;

    char *hsts_upgraded = ns_net_hsts_upgrade(url);
    if (hsts_upgraded) url = hsts_upgraded;

    char *idn_ascii = ns_url_to_ascii(url);
    if (idn_ascii && strcmp(idn_ascii, url) != 0) {
        g_free(hsts_upgraded);
        hsts_upgraded = idn_ascii;
        url = hsts_upgraded;
    } else {
        g_free(idn_ascii);
    }

    char *url_host = ns_url_host_from(url);
    gboolean mobile_ua = ns_mobile_force_host(url_host);
    g_free(url_host);
    gboolean request_http = ns_url_is_http_or_https(url);
    gboolean request_ftp = ns_url_is_ftp(url);
    if (request_ftp && top_url && *top_url && !is_navigation &&
        !ns_url_is_ftp(top_url)) {
        resp->final_url = g_strdup(url);
        resp->status = 0;
        resp->error = g_strdup("FTP access is not allowed from this page");
        g_free(hsts_upgraded);
        return resp;
    }
    const ns_config *cfg = ns_config_get();
    const char *configured_ua =
        (cfg && cfg->user_agent && *cfg->user_agent) ? cfg->user_agent
            : ns_user_agent_for_mode(cfg ? cfg->compat_mode : NULL);
    const char *effective_ua = mobile_ua ? ns_mobile_user_agent()
                                         : configured_ua;
    const char *accept_language = ns_net_effective_accept_language();
    const char *effective_top_url = top_url ? top_url : url;
    char *top_origin = ns_url_origin_from(effective_top_url);
    char *top_site   = ns_url_site_from(effective_top_url);
    const char *partition_key = (top_site && *top_site) ? top_site
                              : (top_origin ? top_origin : "");
    char *cache_partition = g_strdup_printf("top=%s\x1f" "ua=%s\x1f" "al=%s",
                                            partition_key,
                                            effective_ua, accept_language);
    ns_cookie_policy cookie_policy = cfg ? cfg->cookie_policy : NS_COOKIE_FIRST_PARTY;
    gboolean cookies_allowed = request_http &&
        (cookie_policy != NS_COOKIE_NEVER);
    if (cookies_allowed && cookie_policy == NS_COOKIE_FIRST_PARTY &&
        top_url && !ns_url_is_same_site(url, effective_top_url))
        cookies_allowed = FALSE;
    if (!*partition_key)
        cookies_allowed = FALSE;
    char *cookie_partition_path = cookies_allowed
        ? ns_net_cookie_path_for_partition(partition_key) : NULL;

    ns_cache_entry *cached = NULL;
    if (request_http && is_simple_get(method)) {
        cached = ns_cache_get(url, cache_partition);
        if (cached && ns_cache_is_fresh(cached)) {
            gboolean cache_has_cors =
                cached->cors_allow_origin ||
                ns_url_same_origin(effective_top_url, cached->final_url);
            if (!cache_has_cors) {
                ns_cache_entry_free(cached);
                cached = NULL;
            } else {
                ns_response_free(resp);
                ns_response *from_cache = response_from_cache_entry(cached);
                ns_cache_entry_free(cached);
                g_free(cache_partition);
                g_free(cookie_partition_path);
                g_free(top_origin);
                g_free(top_site);
                g_free(hsts_upgraded);
                return from_cache;
            }
        }
    }

    char *referer = ns_net_referer_for(url, top_url,
        cfg ? cfg->referer_policy : NS_REFERER_STRICT_ORIGIN_WHEN_CROSS);
    char *origin_slot = ns_url_origin_from(url);
    gboolean origin_held = FALSE;
    if (origin_slot) {
        origin_held = ns_net_acquire_origin_slot(origin_slot, cancellable);
        if (!origin_held) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "fetch cancelled");
            g_free(origin_slot);
            g_free(referer);
            g_free(cache_partition);
            g_free(cookie_partition_path);
            g_free(top_origin);
            g_free(top_site);
            g_free(hsts_upgraded);
            ns_cache_entry_free(cached);
            ns_response_free(resp);
            return NULL;
        }
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        g_set_error_literal(error, NS_NET_DOMAIN, 1, "curl_easy_init failed");
        if (origin_held) ns_net_release_origin_slot(origin_slot);
        g_free(origin_slot);
        g_free(referer);
        g_free(cache_partition);
        g_free(cookie_partition_path);
        g_free(top_origin);
        g_free(top_site);
        g_free(hsts_upgraded);
        ns_response_free(resp);
        return NULL;
    }
    if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (getenv("NS_NET_LOG"))
        fprintf(stderr, "NS_NET %s %s\n", method ? method : "GET", url);
    {
        const char *proxy = ns_net_pick_configured_proxy(url);
        if (proxy && *proxy)
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
        const char *no_proxy = ns_net_configured_no_proxy();
        if (no_proxy && *no_proxy)
            curl_easy_setopt(curl, CURLOPT_NOPROXY, no_proxy);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
    long max_redirs = cfg ? (long)cfg->max_redirects : (long)NS_MAX_REDIRECTS;
    if (max_redirs < 0)                       max_redirs = 0;
    if (max_redirs > (long)NS_MAX_REDIRECTS)  max_redirs = (long)NS_MAX_REDIRECTS;
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, max_redirs);

    long fetch_timeout = (long)NS_DEFAULT_TIMEOUT_S;
    if (mobile_ua) fetch_timeout = NS_MAX_TIMEOUT_S;
    if (extra_headers) {
        for (guint i = 0; i < extra_headers->len; i++) {
            const char *h = g_ptr_array_index(extra_headers, i);
            if (h && g_str_has_prefix(h, "X-ND-Timeout-Seconds:")) {
                fetch_timeout = (long)g_ascii_strtoll(
                    h + strlen("X-ND-Timeout-Seconds:"), NULL, 10);
                break;
            }
        }
    }
    if (fetch_timeout < 1) fetch_timeout = 1;
    if (fetch_timeout > (long)NS_MAX_TIMEOUT_S) fetch_timeout = (long)NS_MAX_TIMEOUT_S;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, fetch_timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, is_navigation ? 15L : 6L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, effective_ua);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING,
                     g_accept_encoding ? g_accept_encoding : "");
    switch (cfg ? cfg->referer_policy : NS_REFERER_STRICT_ORIGIN_WHEN_CROSS) {
    case NS_REFERER_NO_REFERRER:
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 0L);
        curl_easy_setopt(curl, CURLOPT_REFERER, "");
        break;
    case NS_REFERER_UNSAFE_URL:
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 1L);
        break;
    default:
        curl_easy_setopt(curl, CURLOPT_AUTOREFERER, 0L);
        break;
    }
    if (referer && *referer)
        curl_easy_setopt(curl, CURLOPT_REFERER, referer);

    gboolean caller_set_accept = FALSE;
    if (extra_headers) {
        for (guint i = 0; i < extra_headers->len; i++) {
            const char *h = g_ptr_array_index(extra_headers, i);
            if (h && g_ascii_strncasecmp(h, "Accept:", 7) == 0) {
                caller_set_accept = TRUE;
                break;
            }
        }
    }

    struct curl_slist *headers = NULL;
    {
        char *h = g_strdup_printf("Accept-Language: %s", accept_language);
        headers = curl_slist_append(headers, h);
        g_free(h);
    }
    if (!caller_set_accept) {
        headers = curl_slist_append(headers, is_navigation
            ? "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
            : "Accept: */*");
    }
    if (!cfg || cfg->do_not_track)
        headers = curl_slist_append(headers, "DNT: 1");

    {
        gboolean send_origin = FALSE;
        if (top_origin && *top_origin) {
            if (top_url && !ns_url_same_origin(top_url, url)) {
                send_origin = TRUE;
            } else if (method && *method &&
                       g_ascii_strcasecmp(method, "GET") != 0 &&
                       g_ascii_strcasecmp(method, "HEAD") != 0) {
                send_origin = TRUE;
            }
        }
        if (send_origin && top_origin && *top_origin &&
            !strpbrk(top_origin, "\r\n") &&
            strlen(top_origin) < 4096) {
            char *h = g_strdup_printf("Origin: %s", top_origin);
            headers = curl_slist_append(headers, h);
            g_free(h);
        }
    }

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

    if (request_http) {
        const char *fetch_site;
        if (!top_url || !*top_url) {
            fetch_site = "none";
        } else if (ns_url_same_origin(top_url, url)) {
            fetch_site = "same-origin";
        } else if (ns_url_is_same_site(top_url, url)) {
            fetch_site = "same-site";
        } else {
            fetch_site = "cross-site";
        }
        char *site_h = g_strdup_printf("Sec-Fetch-Site: %s", fetch_site);
        headers = curl_slist_append(headers, site_h);
        g_free(site_h);

        const char *fetch_mode;
        if (is_navigation) {
            fetch_mode = "navigate";
        } else if (method && *method &&
                   g_ascii_strcasecmp(method, "GET") != 0 &&
                   g_ascii_strcasecmp(method, "HEAD") != 0) {
            fetch_mode = "cors";
        } else {
            fetch_mode = "no-cors";
        }
        char *mode_h = g_strdup_printf("Sec-Fetch-Mode: %s", fetch_mode);
        headers = curl_slist_append(headers, mode_h);
        g_free(mode_h);

        const char *fetch_dest = is_navigation ? "document" : "empty";
        char *dest_h = g_strdup_printf("Sec-Fetch-Dest: %s", fetch_dest);
        headers = curl_slist_append(headers, dest_h);
        g_free(dest_h);

        if (is_navigation) {
            headers = curl_slist_append(headers, "Sec-Fetch-User: ?1");
            headers = curl_slist_append(headers,
                                        "Upgrade-Insecure-Requests: 1");
        }

        const char *platform = "\"" NS_UA_HINT_PLATFORM "\"";
        gboolean chromium_ua = ns_user_agent_has_client_hints(effective_ua);
        if (!mobile_ua && chromium_ua) {
            char chrome_major[16];
            const char *cp = strstr(effective_ua, "Chrome/");
            if (cp) {
                cp += strlen("Chrome/");
                size_t k = 0;
                while (cp[k] && cp[k] != '.' && k + 1 < sizeof chrome_major) {
                    chrome_major[k] = cp[k];
                    k++;
                }
                chrome_major[k] = '\0';
            } else {
                g_strlcpy(chrome_major, NS_CHROME_MAJOR, sizeof chrome_major);
            }
            char *ua_brand = g_strdup_printf(
                "Sec-CH-UA: \"Chromium\";v=\"%s\", "
                "\"Google Chrome\";v=\"%s\", \"Not=A?Brand\";v=\"24\"",
                chrome_major, chrome_major);
            headers = curl_slist_append(headers, ua_brand);
            g_free(ua_brand);
            headers = curl_slist_append(headers,
                                        "Sec-CH-UA-Mobile: " NS_SEC_CH_UA_MOBILE);
            char *ua_plat = g_strdup_printf("Sec-CH-UA-Platform: %s", platform);
            headers = curl_slist_append(headers, ua_plat);
            g_free(ua_plat);
        }

        if (!cfg || cfg->global_privacy_control) {
            headers = curl_slist_append(headers, "Sec-GPC: 1");
        }
    }

    gboolean method_is_post = method && g_ascii_strcasecmp(method, "POST") == 0;
    gboolean method_is_get  = !method || !*method ||
                              g_ascii_strcasecmp(method, "GET") == 0;
    gboolean has_body = body && body_len > 0;

    if (method_is_post)
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (has_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }
    if (!method_is_post && !method_is_get && !strpbrk(method, "\r\n"))
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

    if (has_body && (method_is_post || (!method_is_get && method))) {
        gboolean extra_has_ct = FALSE;
        if (extra_headers) {
            for (guint i = 0; i < extra_headers->len; i++) {
                const char *h = g_ptr_array_index(extra_headers, i);
                if (h && g_ascii_strncasecmp(h, "Content-Type:", 13) == 0) {
                    extra_has_ct = TRUE;
                    break;
                }
            }
        }
        if (!extra_has_ct) {
            char *ct_hdr = g_strdup_printf("Content-Type: %s",
                content_type && *content_type ? content_type
                                              : "application/x-www-form-urlencoded");
            headers = curl_slist_append(headers, ct_hdr);
            g_free(ct_hdr);
        }
    }

    if (extra_headers) {
        for (guint i = 0; i < extra_headers->len; i++) {
            const char *h = g_ptr_array_index(extra_headers, i);
            if (!h || !*h) continue;
            if (g_str_has_prefix(h, "X-ND-")) continue;
            if (strpbrk(h, "\r\n")) continue;
            headers = curl_slist_append(headers, h);
        }
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    ns_net_apply_curl_tls(curl);

    if (cookie_partition_path) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_partition_path);
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR,  cookie_partition_path);
        char *cookie_js_path = ns_net_cookie_js_path_for_partition(partition_key);
        if (cookie_js_path) {
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, cookie_js_path);
            g_free(cookie_js_path);
        }
    }

    ns_write_ctx write_ctx = {
        .body = resp->body,
        .total = 0,
        .budget = ns_net_response_budget(),
        .next_recheck = NS_NET_RESPONSE_RECHECK_BYTES,
        .exceeded = FALSE,
    };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ns_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     (curl_off_t)write_ctx.budget);
    ns_header_ctx header_ctx = {0};
    header_ctx.content_type_out = &resp->content_type;
    header_ctx.content_disposition_out = &resp->content_disposition;
    header_ctx.csp_out          = &resp->csp_header;
    header_ctx.xframe_options_out = &resp->xframe_options;
    header_ctx.x_content_type_options_out = &resp->x_content_type_options;
    header_ctx.cors_allow_origin_out = &resp->cors_allow_origin;
    header_ctx.refresh_out = &resp->refresh;
    header_ctx.content_language_out = &resp->content_language;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ns_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_ctx);

    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,ftp");
    gboolean initial_https = g_str_has_prefix(url, "https://");
    gboolean initial_ftp = request_ftp;
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                     initial_https ? "https" :
                     (initial_ftp ? "ftp" : "http,https"));

    const char *hsts_curl = ns_net_hsts_curl_path();
    if (hsts_curl) {
        curl_easy_setopt(curl, CURLOPT_HSTS_CTRL, (long)CURLHSTS_ENABLE);
        curl_easy_setopt(curl, CURLOPT_HSTS, hsts_curl);
    }

    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, ns_net_http_version());
    const char *altsvc = ns_net_altsvc_path();
    if (altsvc)
        curl_easy_setopt(curl, CURLOPT_ALTSVC, altsvc);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ns_xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancellable);

    gint64 fetch_start_us = g_get_monotonic_time();
    double fetch_start_real_ms = (double)g_get_real_time() / 1000.0;
    CURLcode rc = ns_net_multi_perform(curl, cancellable);

    if ((rc == CURLE_PEER_FAILED_VERIFICATION ||
         rc == CURLE_SSL_CACERT_BADFILE) &&
        g_str_has_prefix(url, "https://")) {
        gboolean opt_in = cfg && cfg->tls_allow_insecure_override;
        char *fb_host = ns_url_host_from(url);
        gboolean hsts_pinned = ns_net_hsts_should_upgrade(fb_host);
        if (opt_in && !hsts_pinned) {
            char *warn = g_strdup_printf(
                "Insecure: TLS certificate not trusted (%s)",
                errbuf[0] ? errbuf : curl_easy_strerror(rc));
            g_byte_array_set_size(resp->body, 0);
            write_ctx.total = 0;
            write_ctx.next_recheck = NS_NET_RESPONSE_RECHECK_BYTES;
            write_ctx.exceeded = FALSE;
            errbuf[0] = '\0';
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
            curl_easy_setopt(curl, CURLOPT_COOKIEJAR,  NULL);
            curl_easy_setopt(curl, CURLOPT_COOKIELIST, "ALL");
            rc = ns_net_multi_perform(curl, cancellable);
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
    resp->request_start_us = fetch_start_us;
    resp->request_start_real_ms = fetch_start_real_ms;
    {
        curl_off_t lookup_us = 0;
        curl_off_t connect_us = 0;
        curl_off_t tls_us = 0;
        curl_off_t pretransfer_us = 0;
        curl_off_t response_start_us = 0;
        curl_off_t response_end_us = 0;
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME_T, &lookup_us);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connect_us);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &tls_us);
        curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME_T, &pretransfer_us);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &response_start_us);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &response_end_us);
        resp->domain_lookup_ms = (double)lookup_us / 1000.0;
        resp->connect_ms = (double)connect_us / 1000.0;
        resp->tls_ms = (double)tls_us / 1000.0;
        resp->pretransfer_ms = (double)pretransfer_us / 1000.0;
        resp->response_start_ms = (double)response_start_us / 1000.0;
        resp->response_end_ms = (double)response_end_us / 1000.0;
    }
    {
        char *ip = NULL;
        curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip);
        if (ip && *ip)
            resp->remote_ip = g_strdup(ip);
        const char *sec_url = resp->final_url ? resp->final_url : url;
        gboolean tls_fail = rc == CURLE_PEER_FAILED_VERIFICATION ||
                            rc == CURLE_SSL_CACERT_BADFILE ||
                            rc == CURLE_SSL_ISSUER_ERROR;
        if (g_str_has_prefix(sec_url, "https://")) {
            if (tls_fail || resp->tls_warning)
                resp->security = NS_SEC_INVALID;
            else if (rc == CURLE_OK)
                resp->security = NS_SEC_SECURE;
        } else if (g_str_has_prefix(sec_url, "http://")) {
            resp->security = NS_SEC_PLAIN;
        }
    }
    if (g_log_fetches) {
        long http_version = 0, num_connects = 0;
        curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &http_version);
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &num_connects);
        ns_net_conn_stat_record(resp->final_url, http_version, num_connects);
    }
    if (rc == CURLE_OK && request_ftp) {
        maybe_synthesize_ftp_listing(resp);
        maybe_guess_ftp_content_type(resp);
    }

    {
        char *reach_host = ns_url_host_from(url);
        if (reach_host && *reach_host) {
            if (rc == CURLE_OK || status > 0)
                ns_net_host_mark_alive(reach_host);
            else if (status == 0 &&
                     (rc == CURLE_COULDNT_CONNECT ||
                      rc == CURLE_OPERATION_TIMEDOUT ||
                      rc == CURLE_COULDNT_RESOLVE_HOST))
                ns_net_host_mark_dead(reach_host);
        }
        g_free(reach_host);
    }

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
            g_free(header_ctx.location);
            if (header_ctx.raw) g_string_free(header_ctx.raw, TRUE);
            ns_cache_entry_free(cached);
            ns_response_free(resp);
            if (origin_held) ns_net_release_origin_slot(origin_slot);
            g_free(origin_slot);
            g_free(referer);
            g_free(cache_partition);
            g_free(cookie_partition_path);
            g_free(top_origin);
            g_free(top_site);
            g_free(hsts_upgraded);
            return NULL;
        }
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        if (write_ctx.exceeded || rc == CURLE_FILESIZE_EXCEEDED)
            resp->error = g_strdup_printf(
                "response would exhaust available memory (stopped at %llu MiB)",
                (unsigned long long)(write_ctx.total >> 20));
        else
            resp->error = g_strdup(msg);
    }

    if (rc == CURLE_OK && request_http && is_simple_get(method) &&
        !header_ctx.set_cookie_seen &&
        !resp->tls_warning) {
        if (resp->status == 304 && cached && cached->body) {
            ns_cache_promote_304(url, cache_partition,
                                 header_ctx.cache_control, header_ctx.expires);
            g_byte_array_set_size(resp->body, 0);
            g_byte_array_append(resp->body, cached->body->data, cached->body->len);
            resp->status = cached->status;
            g_free(resp->content_type);
            resp->content_type = g_strdup(cached->content_type);
            g_free(resp->cors_allow_origin);
            resp->cors_allow_origin = g_strdup(cached->cors_allow_origin);
        } else if (resp->status > 0 && resp->status < 300 &&
                   resp->body && resp->body->len > 0) {
            ns_cache_put(url, cache_partition,
                         resp->final_url, resp->status,
                         resp->content_type,
                         resp->cors_allow_origin,
                         header_ctx.etag, header_ctx.last_modified,
                         header_ctx.cache_control, header_ctx.expires,
                         resp->body->data, resp->body->len);
        }
    }
    g_free(referer);
    g_free(cache_partition);
    g_free(cookie_partition_path);
    g_free(top_origin);
    g_free(top_site);

    g_free(header_ctx.etag);
    g_free(header_ctx.last_modified);
    g_free(header_ctx.cache_control);
    g_free(header_ctx.expires);
    if (location_out)
        *location_out = header_ctx.location;
    else
        g_free(header_ctx.location);
    if (header_ctx.raw) {
        g_free(resp->raw_headers);
        resp->raw_headers = g_string_free(header_ctx.raw, FALSE);
    }
    ns_cache_entry_free(cached);

    {
        gint64 fetch_end_us = g_get_monotonic_time();
        ns_net_perf_record(fetch_start_us, fetch_end_us,
                           resp->body ? (guint64)resp->body->len : 0);
        char *req_hdrs = ns_net_slist_serialize(headers);
        ns_net_log_record(method, url, resp->status, resp->content_type,
                          resp->body ? (guint64)resp->body->len : 0,
                          (fetch_end_us - fetch_start_us) / 1000.0,
                          req_hdrs, resp->raw_headers, resp->error);
        g_free(req_hdrs);
    }

    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    if (origin_held) ns_net_release_origin_slot(origin_slot);
    g_free(origin_slot);
    g_free(hsts_upgraded);
    return resp;
}

static gboolean
ns_fetch_is_navigation(const char *top_url, GPtrArray *extra_headers)
{
    if (!top_url) return TRUE;
    if (!extra_headers) return FALSE;
    for (guint i = 0; i < extra_headers->len; i++) {
        const char *h = g_ptr_array_index(extra_headers, i);
        if (h && g_ascii_strncasecmp(h, "X-ND-Navigate:", 14) == 0)
            return TRUE;
    }
    return FALSE;
}

static ns_response *
ns_fetch_sync(const char *url, const char *top_url, const char *method,
              const void *body, gsize body_len, const char *content_type,
              GPtrArray *extra_headers,
              GCancellable *cancellable, GError **error)
{
    if (!ns_fetch_is_navigation(top_url, extra_headers) &&
        ns_ext_should_block(url, top_url)) {
        ns_response *blocked = g_new0(ns_response, 1);
        blocked->body = g_byte_array_new();
        blocked->final_url = g_strdup(url);
        blocked->status = 0;
        blocked->error = g_strdup("blocked by extension");
        return blocked;
    }

    const ns_config *cfg = ns_config_get();
    long max_redirs = cfg ? (long)cfg->max_redirects : (long)NS_MAX_REDIRECTS;
    if (max_redirs < 0)                       max_redirs = 0;
    if (max_redirs > (long)NS_MAX_REDIRECTS)  max_redirs = (long)NS_MAX_REDIRECTS;

    char *cur_url = g_strdup(url);
    char *cur_top = g_strdup(top_url);
    char *cur_method = g_strdup(method && *method ? method : "GET");
    const void *cur_body = body;
    gsize cur_len = body_len;
    const char *cur_ct = content_type;
    gboolean started_https = g_str_has_prefix(url, "https://");
    int hops = 0;
    ns_response *resp = NULL;
    for (;;) {
        char *location = NULL;
        resp = ns_fetch_sync_hop(cur_url, cur_top, cur_method,
                                 cur_body, cur_len, cur_ct,
                                 extra_headers, cancellable, error,
                                 FALSE, &location);
        if (!resp) {
            g_free(location);
            break;
        }
        gboolean is_redirect = resp->status == 301 || resp->status == 302 ||
                               resp->status == 303 || resp->status == 307 ||
                               resp->status == 308;
        if (!is_redirect || resp->error || !location || !*location) {
            g_free(location);
            break;
        }
        if (hops >= max_redirs) {
            g_free(resp->error);
            resp->error = g_strdup("too many redirects");
            g_free(location);
            break;
        }
        const char *base = resp->final_url ? resp->final_url : cur_url;
        char *next = ns_url_resolve(base, location);
        g_free(location);
        if (!next) break;
        if (!ns_url_is_http_or_https(next) ||
            (started_https && !g_str_has_prefix(next, "https://"))) {
            g_free(resp->error);
            resp->error = g_strdup("redirect to a disallowed URL blocked");
            g_free(next);
            break;
        }
        if (resp->status == 303 ||
            ((resp->status == 301 || resp->status == 302) &&
             g_ascii_strcasecmp(cur_method, "GET") != 0)) {
            g_free(cur_method);
            cur_method = g_strdup("GET");
            cur_body = NULL;
            cur_len = 0;
            cur_ct = NULL;
        }
        g_free(cur_top);
        cur_top = NULL;
        g_free(cur_url);
        cur_url = next;
        hops++;
        ns_response_free(resp);
        resp = NULL;
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "fetch cancelled");
            break;
        }
    }
    if (resp) resp->redirect_count = hops;
    g_free(cur_url);
    g_free(cur_top);
    g_free(cur_method);
    return resp;
}

ns_response *
ns_net_fetch_blocking(const char *url, GCancellable *cancellable, GError **error)
{
    return ns_fetch_sync(url, NULL, "GET", NULL, 0, NULL, NULL,
                         cancellable, error);
}

ns_response *
ns_net_request_blocking(const char        *url,
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
    ns_response *resp = ns_fetch_sync(url, top_url, method,
                                      body, body_len, content_type,
                                      hdrs, cancellable, error);
    if (hdrs) g_ptr_array_free(hdrs, TRUE);
    return resp;
}

typedef struct ns_fetch_ctx {
    char *url;
    char *top_url;
    char *method;
    char *content_type;
    guint8 *body;
    gsize body_len;
    GPtrArray *extra_headers;
} ns_fetch_ctx;

static void
ns_fetch_ctx_free(gpointer data)
{
    ns_fetch_ctx *ctx = data;
    g_free(ctx->url);
    g_free(ctx->top_url);
    g_free(ctx->method);
    g_free(ctx->content_type);
    g_free(ctx->body);
    if (ctx->extra_headers) g_ptr_array_free(ctx->extra_headers, TRUE);
    g_free(ctx);
}

#define NS_MAX_CONCURRENT_FETCHES 32
#define NS_MAX_FETCHES_PER_HOST   6

static GHashTable *g_fetch_host_active;

static void ns_fetch_thread(GTask *task, gpointer source_object,
                            gpointer task_data, GCancellable *cancellable);

static char *
ns_fetch_task_host(GTask *task)
{
    ns_fetch_ctx *ctx = task ? g_task_get_task_data(task) : NULL;
    return (ctx && ctx->url) ? ns_url_host_from(ctx->url) : NULL;
}

static int
ns_fetch_host_count_locked(const char *host)
{
    if (!host || !g_fetch_host_active) return 0;
    return GPOINTER_TO_INT(g_hash_table_lookup(g_fetch_host_active, host));
}

static void
ns_fetch_host_adjust_locked(const char *host, int delta)
{
    if (!host) return;
    if (!g_fetch_host_active)
        g_fetch_host_active = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                    g_free, NULL);
    int n = ns_fetch_host_count_locked(host) + delta;
    if (n <= 0)
        g_hash_table_remove(g_fetch_host_active, host);
    else
        g_hash_table_replace(g_fetch_host_active, g_strdup(host),
                             GINT_TO_POINTER(n));
}

static void
ns_fetch_throttle_dispatch(void)
{
    for (;;) {
        g_mutex_lock(&g_fetch_throttle_mutex);
        if (g_fetch_active >= NS_MAX_CONCURRENT_FETCHES ||
            g_queue_is_empty(&g_fetch_queue)) {
            g_mutex_unlock(&g_fetch_throttle_mutex);
            return;
        }
        GTask *chosen = NULL;
        char *chosen_host = NULL;
        for (GList *l = g_fetch_queue.head; l; l = l->next) {
            GTask *t = l->data;
            char *host = ns_fetch_task_host(t);
            if (!host ||
                ns_fetch_host_count_locked(host) < NS_MAX_FETCHES_PER_HOST) {
                chosen = t;
                chosen_host = host;
                g_queue_delete_link(&g_fetch_queue, l);
                break;
            }
            g_free(host);
        }
        if (!chosen) {
            g_mutex_unlock(&g_fetch_throttle_mutex);
            return;
        }
        g_fetch_active++;
        ns_fetch_host_adjust_locked(chosen_host, 1);
        g_free(chosen_host);
        g_mutex_unlock(&g_fetch_throttle_mutex);
        g_task_run_in_thread(chosen, ns_fetch_thread);
        g_object_unref(chosen);
    }
}

static void
ns_fetch_throttle_submit(GTask *task)
{
    g_mutex_lock(&g_fetch_throttle_mutex);
    g_queue_push_tail(&g_fetch_queue, task);
    g_mutex_unlock(&g_fetch_throttle_mutex);
    ns_fetch_throttle_dispatch();
}

static void
ns_fetch_thread(GTask        *task,
                gpointer      source_object,
                gpointer      task_data,
                GCancellable *cancellable)
{
    (void)source_object;
    ns_fetch_ctx *ctx = task_data;
    GError *err = NULL;
    ns_response *resp = ns_fetch_sync(ctx->url, ctx->top_url, ctx->method,
                                      ctx->body, ctx->body_len, ctx->content_type,
                                      ctx->extra_headers,
                                      cancellable, &err);
    if (!resp) {
        if (g_log_fetches)
            ns_debug_log_emit(NS_DLOG_NET, "fetch", "failed %s: %s",
                              ctx->url, err ? err->message : "unknown error");
        g_task_return_error(task, err);
    } else if (g_log_fetches) {
        if (resp->error)
            ns_debug_log_emit(NS_DLOG_NET, "fetch", "error %s: %s",
                              ctx->url, resp->error);
        else
            ns_debug_log_emit(NS_DLOG_NET, "fetch", "%ld %s (%u bytes)",
                              resp->status,
                              resp->final_url ? resp->final_url : ctx->url,
                              resp->body ? resp->body->len : 0u);
    }
    if (resp)
        g_task_return_pointer(task, resp, (GDestroyNotify)ns_response_free);
    {
        char *host = (ctx && ctx->url) ? ns_url_host_from(ctx->url) : NULL;
        g_mutex_lock(&g_fetch_throttle_mutex);
        if (g_fetch_active > 0) g_fetch_active--;
        ns_fetch_host_adjust_locked(host, -1);
        g_cond_broadcast(&g_fetch_idle_cond);
        g_mutex_unlock(&g_fetch_throttle_mutex);
        g_free(host);
    }
    ns_fetch_throttle_dispatch();
}

static void
ns_preconnect_thread(GTask *task, gpointer source_object, gpointer task_data,
                     GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    const char *url = task_data;
    g_mutex_lock(&g_fetch_throttle_mutex);
    g_preconnect_active++;
    g_mutex_unlock(&g_fetch_throttle_mutex);
    CURL *curl = g_atomic_int_get(&g_net_aborting) ? NULL : curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
        if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ns_xferinfo_cb);
        ns_net_apply_curl_proxy(curl, url);
        ns_net_apply_curl_tls(curl);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    g_mutex_lock(&g_fetch_throttle_mutex);
    if (g_preconnect_active > 0) g_preconnect_active--;
    g_cond_broadcast(&g_fetch_idle_cond);
    g_mutex_unlock(&g_fetch_throttle_mutex);
    g_task_return_boolean(task, TRUE);
}

void
ns_net_preconnect_async(const char *url)
{
    if (!url || !ns_url_is_http_or_https(url)) return;
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_set_task_data(task, g_strdup(url), g_free);
    g_task_run_in_thread(task, ns_preconnect_thread);
    g_object_unref(task);
}

static ns_net_blob_resolver g_blob_resolver = NULL;
static gpointer g_blob_resolver_ud = NULL;

void
ns_net_set_blob_resolver(ns_net_blob_resolver resolver, gpointer user_data)
{
    g_blob_resolver = resolver;
    g_blob_resolver_ud = user_data;
}

static gboolean
ns_net_complete_blob(const char *url, GCancellable *cancellable,
                     GAsyncReadyCallback callback, gpointer user_data)
{
    if (!g_blob_resolver || !g_str_has_prefix(url, "blob:")) return FALSE;
    char *type = NULL;
    GBytes *bytes = g_blob_resolver(url, &type, g_blob_resolver_ud);
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, ns_net_fetch_async);
    ns_response *resp = g_new0(ns_response, 1);
    resp->body = g_byte_array_new();
    resp->final_url = g_strdup(url);
    if (bytes) {
        gsize len = 0;
        const guint8 *data = g_bytes_get_data(bytes, &len);
        if (data && len) g_byte_array_append(resp->body, data, len);
        resp->status = 200;
        resp->content_type = type;
        g_bytes_unref(bytes);
    } else {
        resp->status = 404;
        resp->error = g_strdup("blob URL not found");
        g_free(type);
    }
    g_task_return_pointer(task, resp, (GDestroyNotify)ns_response_free);
    g_object_unref(task);
    return TRUE;
}

void
ns_net_fetch_async(const char        *url,
                   const char        *top_url,
                   GCancellable      *cancellable,
                   GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    g_return_if_fail(url != NULL);

    if (ns_net_complete_blob(url, cancellable, callback, user_data)) return;

    ns_fetch_ctx *ctx = g_new0(ns_fetch_ctx, 1);
    ctx->url = g_strdup(url);
    ctx->top_url = top_url ? g_strdup(top_url) : NULL;

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, ns_net_fetch_async);
    g_task_set_task_data(task, ctx, ns_fetch_ctx_free);
    ns_fetch_throttle_submit(task);
}

void
ns_net_post_async(const char         *url,
                  const char         *top_url,
                  const void         *body,
                  gsize               body_len,
                  const char         *content_type,
                  GCancellable       *cancellable,
                  GAsyncReadyCallback callback,
                  gpointer            user_data)
{
    ns_net_request_async(url, top_url, "POST", body, body_len, content_type, NULL,
                         cancellable, callback, user_data);
}

void
ns_net_request_async(const char         *url,
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

    if (ns_net_complete_blob(url, cancellable, callback, user_data)) return;

    ns_fetch_ctx *ctx = g_new0(ns_fetch_ctx, 1);
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
    g_task_set_source_tag(task, ns_net_request_async);
    g_task_set_task_data(task, ctx, ns_fetch_ctx_free);
    ns_fetch_throttle_submit(task);
}

ns_response *
ns_net_fetch_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

char *
ns_multipart_boundary(void)
{
    guint32 r[4];
    if (!ns_security_csprng_fill(r, sizeof r)) {
        r[0] = g_random_int(); r[1] = g_random_int();
        r[2] = g_random_int(); r[3] = g_random_int();
    }
    return g_strdup_printf("----NordstjernenFormBoundary%08x%08x%08x%08x",
                           r[0], r[1], r[2], r[3]);
}

void
ns_multipart_quote_field(GString *out, const char *s)
{
    if (!out || !s) return;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  g_string_append(out, "%22");
        else if (c == '\r') g_string_append(out, "%0D");
        else if (c == '\n') g_string_append(out, "%0A");
        else                g_string_append_c(out, (char)c);
    }
}

static char *g_form_submission_charset;

void
ns_form_set_submission_charset(const char *charset)
{
    g_free(g_form_submission_charset);
    g_form_submission_charset = NULL;
    if (!charset || !*charset) return;
    char *first = g_strdup(charset);
    g_strstrip(first);
    for (char *p = first; *p; p++)
        if (*p == ' ' || *p == ',' || *p == '\t') { *p = '\0'; break; }
    if (*first && g_ascii_strcasecmp(first, "UTF-8") != 0 &&
        g_ascii_strcasecmp(first, "UTF8") != 0 &&
        g_ascii_strcasecmp(first, "UTF-16LE") != 0 &&
        g_ascii_strcasecmp(first, "UTF-16BE") != 0)
        g_form_submission_charset = first;
    else
        g_free(first);
}

void
ns_form_urlencoded_append(GString *out, const char *s)
{
    if (!out || !s) return;
    char *converted = NULL;
    if (g_form_submission_charset) {
        converted = g_convert(s, -1, g_form_submission_charset, "UTF-8",
                              NULL, NULL, NULL);
        if (converted) s = converted;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (g_ascii_isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_')
            g_string_append_c(out, (char)c);
        else if (c == ' ')
            g_string_append_c(out, '+');
        else
            g_string_append_printf(out, "%%%02X", c);
    }
    g_free(converted);
}

void
ns_form_urlencoded_append_pair(GString *out, gboolean *first,
                               const char *name, const char *value)
{
    if (!out || !first || !name) return;
    if (!*first) g_string_append_c(out, '&');
    *first = FALSE;
    ns_form_urlencoded_append(out, name);
    g_string_append_c(out, '=');
    ns_form_urlencoded_append(out, value);
}
