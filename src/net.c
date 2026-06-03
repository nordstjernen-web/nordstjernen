/* Nordstjernen — libcurl-backed async fetcher.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "net.h"
#include "cache.h"
#include "config.h"
#include "mobile.h"
#include "csp.h"
#include "debuglog.h"
#include "env.h"
#include "html.h"
#include "image.h"
#include "security.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <glib/gstdio.h>
#include <gmodule.h>

#include <lexbor/unicode/idna.h>
#include <lexbor/url/url.h>

#include <libpsl.h>

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#endif

static char *g_cookie_dir;
static char *g_hsts_curl_path;
static char *g_altsvc_path;
static GHashTable *g_hsts_cache;
static gint64      g_hsts_cache_mtime_us;
static GMutex      g_hsts_lock;
static char *g_ca_bundle;
static gboolean g_has_http3;
static char *g_accept_encoding;
static char *g_proxy_override;
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

static char *
origin_slot_key(const char *origin)
{
    return (origin && *origin) ? g_ascii_strdown(origin, -1) : NULL;
}

static gboolean
nd_net_acquire_origin_slot(const char *origin, GCancellable *cancellable)
{
    char *key = origin_slot_key(origin);
    if (!key) return FALSE;
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
    char *key = origin_slot_key(origin);
    if (!key) return;
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
nd_net_hsts_curl_path(void) { return nd_net_data_path(&g_hsts_curl_path, "hsts-curl.txt"); }

static void
nd_hsts_cache_reload_locked(const char *path)
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
nd_hsts_lookup_locked(const char *lower_host)
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
nd_net_supported_encodings(void)
{
    return g_accept_encoding ? g_accept_encoding : "";
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

static char *
nd_url_to_ascii(const char *url)
{
    if (!url || !*url) return NULL;
    if (g_str_has_prefix(url, "data:") || g_str_has_prefix(url, "about:") ||
        g_str_has_prefix(url, "file:"))
        return g_strdup(url);
    return nd_url_resolve(NULL, url);
}

static gboolean
nd_idn_label_is_safe(const char *label, gsize len)
{
    gboolean has_latin = FALSE, has_han = FALSE;
    gboolean has_hira = FALSE, has_kata = FALSE;
    gboolean has_bopo = FALSE, has_hangul = FALSE;
    GUnicodeScript other_script = G_UNICODE_SCRIPT_INVALID_CODE;
    gboolean has_other = FALSE, mixed_other = FALSE;

    const char *p = label;
    const char *end = label + len;
    while (p < end) {
        gunichar c = g_utf8_get_char(p);
        p = g_utf8_next_char(p);
        if (c < 0x80) { has_latin = TRUE; continue; }
        GUnicodeScript s = g_unichar_get_script(c);
        if (s == G_UNICODE_SCRIPT_COMMON || s == G_UNICODE_SCRIPT_INHERITED)
            continue;
        switch (s) {
        case G_UNICODE_SCRIPT_LATIN:    has_latin = TRUE;  break;
        case G_UNICODE_SCRIPT_HAN:      has_han = TRUE;    break;
        case G_UNICODE_SCRIPT_HIRAGANA: has_hira = TRUE;   break;
        case G_UNICODE_SCRIPT_KATAKANA: has_kata = TRUE;   break;
        case G_UNICODE_SCRIPT_BOPOMOFO: has_bopo = TRUE;   break;
        case G_UNICODE_SCRIPT_HANGUL:   has_hangul = TRUE; break;
        default:
            if (!has_other) { other_script = s; has_other = TRUE; }
            else if (s != other_script) mixed_other = TRUE;
            break;
        }
    }
    if (mixed_other) return FALSE;
    if (has_other) {
        return !has_latin && !has_han && !has_hira && !has_kata &&
               !has_bopo && !has_hangul;
    }
    int cjk_groups = ((has_hira || has_kata) ? 1 : 0) +
                     (has_bopo ? 1 : 0) +
                     (has_hangul ? 1 : 0);
    return cjk_groups <= 1;
}

static gboolean
nd_idn_label_check_range(const char *host, gsize host_len)
{
    if (host_len == 0) return TRUE;
    const char *p = host;
    const char *host_end = host + host_len;
    while (p < host_end) {
        const char *dot = memchr(p, '.', (gsize)(host_end - p));
        gsize n = dot ? (gsize)(dot - p) : (gsize)(host_end - p);
        if (!nd_idn_label_is_safe(p, n)) return FALSE;
        if (!dot) break;
        p = dot + 1;
    }
    return TRUE;
}

char *
nd_url_to_display(const char *url)
{
    if (!url || !*url) return NULL;
    if (!nd_url_is_http_or_https(url))
        return g_strdup(url);

    lxb_url_parser_t *parser = nd_url_parser_open();
    if (!parser) return g_strdup(url);

    lxb_url_t *u = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)url, strlen(url));
    if (!u || u->host.type == LXB_URL_HOST_TYPE__UNDEF ||
        u->host.type == LXB_URL_HOST_TYPE_EMPTY) {
        nd_url_parser_close(parser);
        return g_strdup(url);
    }

    if (!parser->idna) {
        parser->idna = lxb_unicode_idna_create();
        if (parser->idna && lxb_unicode_idna_init(parser->idna) != LXB_STATUS_OK)
            parser->idna = lxb_unicode_idna_destroy(parser->idna, true);
    }

    char *out = NULL;
    if (parser->idna) {
        GString *full = g_string_new(NULL);
        if (lxb_url_serialize_idna(parser->idna, u, nd_url_str_append_cb,
                                   full, false) == LXB_STATUS_OK &&
            full->len > 0) {
            const char *p = strstr(full->str, "://");
            if (p) {
                p += 3;
                const char *at = strchr(p, '@');
                const char *slash = strchr(p, '/');
                if (at && (!slash || at < slash)) p = at + 1;
                const char *end = p;
                while (*end && *end != ':' && *end != '/' &&
                       *end != '?' && *end != '#')
                    end++;
                if (nd_idn_label_check_range(p, (gsize)(end - p))) {
                    out = g_string_free(full, FALSE);
                    full = NULL;
                }
            }
        }
        if (full) g_string_free(full, TRUE);
    }
    nd_url_parser_close(parser);
    if (out) return out;
    char *ascii = nd_url_to_ascii(url);
    return ascii ? ascii : g_strdup(url);
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
    if (!nd_url_is_http_or_https(url))
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
    gboolean eq = oa && *oa && ob && *ob && g_ascii_strcasecmp(oa, ob) == 0;
    g_free(oa);
    g_free(ob);
    return eq;
}

static char *
nd_net_referer_for(const char *url, const char *top_url,
                   nd_referer_policy policy)
{
    if (!top_url || !*top_url || !nd_url_is_http_or_https(url) ||
        !nd_url_is_http_or_https(top_url) ||
        policy == ND_REFERER_NO_REFERRER)
        return NULL;
    if (g_str_has_prefix(top_url, "https://") &&
        g_str_has_prefix(url, "http://"))
        return NULL;
    gboolean same_origin = nd_url_same_origin(top_url, url);
    if (policy == ND_REFERER_SAME_ORIGIN && !same_origin)
        return NULL;
    if (policy == ND_REFERER_UNSAFE_URL || same_origin) {
        char *out = g_strdup(top_url);
        char *hash = strchr(out, '#');
        if (hash) *hash = '\0';
        return out;
    }
    char *origin = nd_url_origin_from(top_url);
    if (!origin) return NULL;
    char *out = g_strdup_printf("%s/", origin);
    g_free(origin);
    return out;
}

static char *
nd_url_site_from(const char *url)
{
    if (!url || !*url) return NULL;
    g_autoptr(nd_url_parts) p = nd_url_parts_new(url);
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
nd_url_is_same_site(const char *a, const char *b)
{
    if (!a || !b) return FALSE;
    g_autofree char *sa = nd_url_site_from(a);
    g_autofree char *sb = nd_url_site_from(b);
    if (sa && sb) return g_ascii_strcasecmp(sa, sb) == 0;

    g_autofree char *ha = nd_url_host_from(a);
    g_autofree char *hb = nd_url_host_from(b);
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

void
nd_url_parts_free(nd_url_parts *parts)
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
nd_url_take_serialized(GString *s, lxb_status_t status)
{
    if (status != LXB_STATUS_OK) {
        g_string_free(s, TRUE);
        return g_strdup("");
    }
    return g_string_free(s, FALSE);
}

nd_url_parts *
nd_url_parts_new(const char *url)
{
    if (!url) return NULL;

    lxb_url_parser_t *parser = nd_url_parser_open();
    if (!parser) return NULL;

    lxb_url_t *u = lxb_url_parse(parser, NULL,
                                 (const lxb_char_t *)url, strlen(url));
    if (!u) {
        nd_url_parser_close(parser);
        return NULL;
    }

    nd_url_parts *p = g_new0(nd_url_parts, 1);

    GString *s = g_string_new(NULL);
    p->href = nd_url_take_serialized(s,
        lxb_url_serialize(u, nd_url_str_append_cb, s, false));
    if (!*p->href) {
        g_free(p->href);
        p->href = g_strdup(url);
    }

    s = g_string_new(NULL);
    char *scheme = nd_url_take_serialized(s,
        lxb_url_serialize_scheme(u, nd_url_str_append_cb, s));
    p->protocol = *scheme ? g_strconcat(scheme, ":", NULL) : g_strdup("");
    g_free(scheme);

    if (u->host.type == LXB_URL_HOST_TYPE__UNDEF ||
        u->host.type == LXB_URL_HOST_TYPE_EMPTY) {
        p->hostname = g_strdup("");
    } else {
        s = g_string_new(NULL);
        p->hostname = nd_url_take_serialized(s,
            lxb_url_serialize_host(&u->host, nd_url_str_append_cb, s));
    }

    p->port = u->has_port ? g_strdup_printf("%u", (unsigned)u->port)
                          : g_strdup("");

    p->host = (*p->hostname && *p->port)
        ? g_strconcat(p->hostname, ":", p->port, NULL)
        : g_strdup(p->hostname);

    p->origin = (*p->hostname && *p->protocol)
        ? g_strconcat(p->protocol, "//", p->host, NULL)
        : g_strdup("");

    s = g_string_new(NULL);
    p->pathname = nd_url_take_serialized(s,
        lxb_url_serialize_path(&u->path, nd_url_str_append_cb, s));

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

    nd_url_parser_close(parser);
    return p;
}

gboolean
nd_url_is_valid_absolute(const char *url)
{
    if (!url || !*url) return FALSE;
    for (const char *p = url; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '\n' ||
            *p == '\r' || *p == '\f')
            return FALSE;
    g_autoptr(nd_url_parts) parts = nd_url_parts_new(url);
    return parts && parts->protocol && *parts->protocol;
}

gboolean
nd_net_hsts_should_upgrade(const char *host)
{
    if (!host || !*host) return FALSE;
    char *path = nd_net_hsts_curl_path();
    if (!path) return FALSE;
    g_mutex_lock(&g_hsts_lock);
    gint64 mtime = file_mtime_us(path);
    if (!g_hsts_cache || mtime != g_hsts_cache_mtime_us) {
        nd_hsts_cache_reload_locked(path);
        g_hsts_cache_mtime_us = mtime;
    }
    char *lower = g_ascii_strdown(host, -1);
    gboolean hit = nd_hsts_lookup_locked(lower);
    g_free(lower);
    g_mutex_unlock(&g_hsts_lock);
    return hit;
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

char *
nd_net_cookies_for_js(const char *url)
{
    if (!url || !*url) return NULL;
    g_autoptr(nd_url_parts) parts = nd_url_parts_new(url);
    if (!parts || !parts->hostname || !*parts->hostname) return NULL;
    const char *host = parts->hostname;
    const char *path = (parts->pathname && *parts->pathname)
                       ? parts->pathname : "/";
    gboolean is_https = parts->protocol &&
                        g_ascii_strcasecmp(parts->protocol, "https:") == 0;

    g_autofree char *site = nd_url_site_from(url);
    if (!site || !*site) return NULL;
    char *jar_path = nd_net_cookie_path_for_partition(site);
    char *contents = NULL;
    gboolean ok = jar_path &&
                  g_file_get_contents(jar_path, &contents, NULL, NULL);
    g_free(jar_path);
    if (!ok || !contents) { g_free(contents); return NULL; }

    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gsize hl = strlen(host);
    GString *out = g_string_new(NULL);
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = lines[i];
        if (!*line) continue;
        if (line[0] == '#') continue;
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
            else if (path[cl] != '\0' && path[cl] != '/' && cpath[cl - 1] != '/')
                match = FALSE;
        }
        if (match && csecure && !is_https) match = FALSE;
        if (match && cexpiry != 0 && cexpiry < now) match = FALSE;
        if (match && cname && *cname) {
            if (out->len) g_string_append(out, "; ");
            g_string_append(out, cname);
            g_string_append_c(out, '=');
            g_string_append(out, cval ? cval : "");
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(contents);
    if (out->len == 0) return g_string_free(out, TRUE), NULL;
    return g_string_free(out, FALSE);
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

#if defined(__ANDROID__)
    const char *android_paths[] = {
        "/system/etc/security/cacerts.pem",
        "/apex/com.android.conscrypt/cacerts.pem",
        "/data/misc/keychain/cacerts-added/cacert.pem",
        NULL,
    };
    for (int i = 0; android_paths[i]; i++)
        if (nd_net_try_ca_bundle(android_paths[i])) return;
    g_info("nd_net: no CA bundle found; the Android host app should set "
           "CURL_CA_BUNDLE to an extracted cacert.pem before nd_browser_init().");
#endif

#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
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

static gpointer
nd_rng_warmup_thread(gpointer data)
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
nd_net_warm_rng(void)
{
    if (!g_module_supported()) return;
    g_rng_warmup_thread = g_thread_try_new("nd-rng-warmup",
                                           nd_rng_warmup_thread, NULL, NULL);
}

static void
nd_net_join_rng(void)
{
    if (g_rng_warmup_thread) {
        g_thread_join(g_rng_warmup_thread);
        g_rng_warmup_thread = NULL;
    }
}

void
nd_net_init(void)
{
    nd_net_resolve_ca_bundle();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
    g_has_http3 = vi && (vi->features & CURL_VERSION_HTTP3) != 0;

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
#ifdef CURL_LOCK_DATA_PSL
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_PSL);
#endif
#ifdef CURL_LOCK_DATA_HSTS
        curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_HSTS);
#endif
        curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC,   nd_share_lock);
        curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, nd_share_unlock);
    }

    nd_net_warm_rng();

    nd_net_hsts_curl_path();
    nd_net_altsvc_path();
    nd_net_cookie_dir();
}

void
nd_net_shutdown(void)
{
    nd_net_join_rng();
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
nd_net_set_proxy_override(const char *proxy_url)
{
    g_free(g_proxy_override);
    g_proxy_override = (proxy_url && *proxy_url) ? g_strdup(proxy_url) : NULL;
}

static gboolean g_allow_file_urls = FALSE;

void
nd_net_set_allow_file_urls(gboolean allow)
{
    g_allow_file_urls = allow;
}

static gboolean g_log_fetches = FALSE;

void
nd_net_set_log_fetches(gboolean on)
{
    g_log_fetches = on;
}

static const char *
nd_net_pick_configured_proxy(const char *url)
{
    if (g_proxy_override && *g_proxy_override) return g_proxy_override;
    const nd_config *cfg = nd_config_get();
    if (!cfg) return NULL;
    gboolean https = g_str_has_prefix(url, "https://");
    if (https && cfg->https_proxy && *cfg->https_proxy) return cfg->https_proxy;
    if (cfg->http_proxy && *cfg->http_proxy)            return cfg->http_proxy;
    return NULL;
}

static const char *
nd_net_configured_no_proxy(void)
{
    const nd_config *cfg = nd_config_get();
    if (cfg && cfg->no_proxy && *cfg->no_proxy) return cfg->no_proxy;
    return NULL;
}

char *
nd_net_proxy_mask(const char *proxy_url)
{
    if (!proxy_url || !*proxy_url) return g_strdup("");
    const char *scheme_sep = strstr(proxy_url, "://");
    const char *cursor = scheme_sep ? scheme_sep + 3 : proxy_url;
    const char *at = strchr(cursor, '@');
    if (!at) return g_strdup(proxy_url);
    const char *colon = memchr(cursor, ':', (gsize)(at - cursor));
    if (!colon) return g_strdup(proxy_url);
    GString *s = g_string_new(NULL);
    g_string_append_len(s, proxy_url, (gssize)(colon - proxy_url));
    g_string_append(s, ":***");
    g_string_append(s, at);
    return g_string_free(s, FALSE);
}

char *
nd_net_effective_proxy_for(const char *url)
{
    const char *p = nd_net_pick_configured_proxy(url);
    if (p && *p) return nd_net_proxy_mask(p);
    static const char *const env_keys[] = {
        "ND_HTTPS_PROXY", "ND_HTTP_PROXY",
        "https_proxy", "HTTPS_PROXY",
        "http_proxy",  "HTTP_PROXY",
        "all_proxy",   "ALL_PROXY",
    };
    gboolean https = url && g_str_has_prefix(url, "https://");
    gsize start = https ? 0 : 2;
    for (gsize i = start; i < G_N_ELEMENTS(env_keys); i++) {
        const char *v = g_getenv(env_keys[i]);
        if (v && *v) return nd_net_proxy_mask(v);
    }
    return g_strdup("");
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
    g_free(resp->refresh);
    g_free(resp->raw_headers);
    if (resp->body)
        g_byte_array_unref(resp->body);
    g_free(resp->error);
    g_free(resp->tls_warning);
    g_free(resp);
}

#define ND_NET_RESPONSE_MIN_BUDGET (64ULL * 1024ULL * 1024ULL)
#define ND_NET_RESPONSE_RECHECK_BYTES (16ULL * 1024ULL * 1024ULL)

static guint64
nd_net_available_memory_bytes(void)
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
#elif defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0)
        return (guint64)pages * (guint64)psize;
#endif
    return 0;
}

static guint64
nd_net_response_budget(void)
{
    guint64 avail = nd_net_available_memory_bytes();
    if (avail == 0) return ND_NET_RESPONSE_MIN_BUDGET;
    guint64 half = avail / 2;
    return half < ND_NET_RESPONSE_MIN_BUDGET ? ND_NET_RESPONSE_MIN_BUDGET : half;
}

typedef struct nd_write_ctx {
    GByteArray *body;
    guint64     total;
    guint64     budget;
    guint64     next_recheck;
    gboolean    exceeded;
} nd_write_ctx;

static size_t
nd_write_cb(char *data, size_t size, size_t nmemb, void *userdata)
{
    nd_write_ctx *ctx = userdata;
    if (size != 0 && nmemb > G_MAXSIZE / size)
        return 0;
    size_t bytes = size * nmemb;

    if (bytes == 0)
        return 0;
    if (bytes > G_MAXUINT)
        return 0;
    if (ctx->total >= ctx->next_recheck) {
        ctx->budget = nd_net_response_budget();
        ctx->next_recheck = ctx->total + ND_NET_RESPONSE_RECHECK_BYTES;
    }
    if (ctx->total + bytes > ctx->budget) {
        ctx->exceeded = TRUE;
        return 0;
    }
    g_byte_array_append(ctx->body, (const guint8 *)data, bytes);
    ctx->total += bytes;
    return bytes;
}

typedef struct nd_header_ctx {
    char **content_type_out;
    char **content_disposition_out;
    char **csp_out;
    char **xframe_options_out;
    char **cors_allow_origin_out;
    char **refresh_out;
    char  *etag;
    char  *last_modified;
    char  *cache_control;
    char  *expires;
    GString *raw;
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

static size_t
nd_header_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    nd_header_ctx *hc = userdata;
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
            g_string_append_len(hc->raw, buffer, bytes);
        }
    }

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
    else if (header_capture(buffer, bytes, "Refresh:", hc->refresh_out))                       {}
    else if (header_capture(buffer, bytes, "Set-Cookie:", NULL))
        hc->set_cookie_seen = TRUE;

    return bytes;
}

extern const char *nd_app_self_exe(void);

static char *
about_read_first(const char *const *rel_paths, gsize *out_len)
{
    const char *exe = nd_app_self_exe();
    char *exe_dir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    char *contents = NULL;
    gsize len = 0;
    for (int i = 0; rel_paths[i]; i++) {
        char *path = g_build_filename(exe_dir, rel_paths[i], NULL);
        gboolean ok = g_file_get_contents(path, &contents, &len, NULL);
        g_free(path);
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

    static const char *const svg_paths[] = {
        "share/icons/hicolor/scalable/apps/nordstjernen.svg",
        "../share/icons/hicolor/scalable/apps/nordstjernen.svg",
        "../../data/icons/hicolor/scalable/apps/nordstjernen.svg",
        "data/icons/hicolor/scalable/apps/nordstjernen.svg",
        NULL,
    };
    char *svg = about_read_first(svg_paths, NULL);
    if (!svg) {
        cached = g_strdup("data:image/svg+xml;utf8,"
                          "<svg xmlns='http://www.w3.org/2000/svg' "
                          "viewBox='0 0 16 16'><rect width='16' height='16' "
                          "fill='%23000026'/></svg>");
        return cached;
    }
    char *encoded = g_uri_escape_string(svg, NULL, FALSE);
    g_free(svg);
    cached = g_strconcat("data:image/svg+xml;utf8,", encoded, NULL);
    g_free(encoded);
    return cached;
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

static char *
build_about_license(void)
{
    static const char *const license_paths[] = {
        "share/nordstjernen/License.md",
        "../share/nordstjernen/License.md",
        "../../License.md",
        "License.md",
        NULL,
    };
    char *text = about_read_first(license_paths, NULL);
    if (!text) {
        return g_strdup("<!doctype html><meta charset=utf-8>"
                        "<title>Nordstjernen License</title>"
                        "<p>License.md is missing from the install — "
                        "reinstall the package or copy <code>License.md</code> "
                        "next to the binary.</p>");
    }
    char *escaped = nd_html_escape_text(text);
    g_free(text);
    char *html = g_strconcat(
        "<!doctype html><html><head>"
        "<meta charset=\"utf-8\">"
        "<title>Nordstjernen Source License</title>"
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
        "<p class=\"nav\"><a href=\"about:start\">"
        "&larr; Start page</a></p>"
        "<h1>Nordstjernen Source License</h1>"
        "<pre>", escaped, "</pre>"
        "</body></html>", NULL);
    g_free(escaped);
    return html;
}

typedef struct nd_error_info {
    const char *icon;
    const char *title;
    const char *heading;
    const char *summary;
} nd_error_info;

static const nd_error_info *
classify_error(long status, const char *transport_error)
{
    static const nd_error_info NO_NETWORK = {
        "📡",
        "Can't reach the network",
        "Can't reach the network",
        "Nordstjernen couldn't connect to any server. Your device may be "
        "offline, or a firewall is blocking outbound traffic."
    };
    static const nd_error_info DNS = {
        "🔍",
        "Server address not found",
        "Server address not found",
        "Nordstjernen couldn't look up the host name. The address may be "
        "mistyped, or your DNS resolver isn't responding."
    };
    static const nd_error_info REFUSED = {
        "🚫",
        "Server refused the connection",
        "Server refused the connection",
        "The host is reachable but no service is listening on that port, "
        "or it actively closed the connection."
    };
    static const nd_error_info TIMEOUT = {
        "⌛",
        "The connection timed out",
        "The connection timed out",
        "The server didn't respond within the allowed time. It may be "
        "overloaded or temporarily unreachable."
    };
    static const nd_error_info TLS = {
        "🔒",
        "Secure connection failed",
        "Secure connection failed",
        "Nordstjernen couldn't establish a trustworthy TLS connection. "
        "The certificate may be invalid, expired, or self-signed."
    };
    static const nd_error_info BAD_URL = {
        "📝",
        "That address looks malformed",
        "That address looks malformed",
        "The URL couldn't be parsed. Check for typos, missing slashes, "
        "or an unsupported scheme."
    };
    static const nd_error_info HTTP_404 = {
        "🗺",
        "Page not found",
        "Page not found",
        "The server is reachable, but it has no resource at that URL. "
        "The link may be outdated or the page may have moved."
    };
    static const nd_error_info HTTP_410 = {
        "🪦",
        "This page is gone",
        "This page is gone",
        "The server is telling us the resource has been permanently removed."
    };
    static const nd_error_info HTTP_401 = {
        "🔐",
        "Authentication required",
        "Authentication required",
        "The server needs credentials Nordstjernen doesn't have. Sign in "
        "elsewhere first, or try a different URL."
    };
    static const nd_error_info HTTP_403 = {
        "🚪",
        "Access denied",
        "Access denied",
        "The server understood the request but refused to share this "
        "resource with us."
    };
    static const nd_error_info HTTP_429 = {
        "🌊",
        "Too many requests",
        "Too many requests",
        "The server is throttling us. Wait a moment and try again."
    };
    static const nd_error_info HTTP_500 = {
        "💥",
        "Server error",
        "Server error",
        "The server hit an internal error processing this request. "
        "Nothing to do on our end — try again later."
    };
    static const nd_error_info HTTP_502 = {
        "🪢",
        "Bad gateway",
        "Bad gateway",
        "An upstream server returned an invalid response. The site's "
        "infrastructure may be misconfigured."
    };
    static const nd_error_info HTTP_503 = {
        "🛠",
        "Service unavailable",
        "Service unavailable",
        "The server is temporarily refusing requests, usually because it "
        "is overloaded or down for maintenance."
    };
    static const nd_error_info HTTP_504 = {
        "⏱",
        "Gateway timeout",
        "Gateway timeout",
        "An upstream server didn't answer in time."
    };
    static const nd_error_info HTTP_GENERIC_4XX = {
        "⚠",
        "Request rejected",
        "Request rejected",
        "The server didn't accept this request."
    };
    static const nd_error_info HTTP_GENERIC_5XX = {
        "⚠",
        "Server error",
        "Server error",
        "The server reported a failure handling this request."
    };
    static const nd_error_info GENERIC = {
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
nd_build_error_page(const char *url, long status, const char *transport_error)
{
    const nd_error_info *info = classify_error(status, transport_error);
    const char *safe_url = url && *url ? url : "(no URL)";
    char *esc_url = nd_html_escape_text(safe_url);
    char *esc_title = nd_html_escape_text(info->title);
    char *esc_heading = nd_html_escape_text(info->heading);
    char *esc_summary = nd_html_escape_text(info->summary);
    char *esc_detail = NULL;
    if (transport_error && *transport_error) {
        char *esc_err = nd_html_escape_text(transport_error);
        esc_detail = g_strdup_printf("Technical detail: %s", esc_err);
        g_free(esc_err);
    } else if (status > 0) {
        esc_detail = g_strdup_printf("HTTP status: %ld", status);
    }

    char *retry_href = url && *url ? nd_html_escape_text(url) : g_strdup("");
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
nd_data_url_decode(const char *url,
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
synthesize_data_response(const char *url, nd_response *resp)
{
    if (!url || !g_str_has_prefix(url, "data:")) return FALSE;
    guint64 budget = nd_net_response_budget();
    gsize body_start = resp->body->len;
    char *ct = NULL;
    gboolean too_large = FALSE;
    if (!nd_data_url_decode(url, resp->body, budget, &ct, &too_large)) {
        if (!ct) return FALSE;
        g_byte_array_set_size(resp->body, body_start);
        if (too_large)
            resp->error = response_budget_error(budget);
    }
    resp->status = 200;
    resp->final_url = g_strdup(url);
    resp->content_type = ct;
    return TRUE;
}

static gboolean
nd_file_access_allowed(const char *top_url)
{
    if (g_allow_file_urls) return TRUE;
    if (!top_url || !*top_url) return TRUE;
    if (g_str_has_prefix(top_url, "file:")) return TRUE;
    return FALSE;
}

static gboolean
synthesize_file_response(const char *url, const char *top_url, nd_response *resp)
{
    if (!url || !g_str_has_prefix(url, "file:")) return FALSE;
    if (!nd_file_access_allowed(top_url)) {
        resp->final_url = g_strdup(url);
        resp->status = 0;
        resp->error = g_strdup("local file access is not allowed from a "
                               "remote page");
        return TRUE;
    }
    char *path = g_filename_from_uri(url, NULL, NULL);
    resp->final_url = g_strdup(url);
    if (!path) {
        resp->status = 400;
        resp->error = g_strdup("invalid file URL");
        return TRUE;
    }
    guint64 budget = nd_net_response_budget();
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
    if (mime && g_str_has_prefix(mime, "text/"))
        resp->content_type = g_strdup_printf("%s; charset=utf-8", mime);
    else
        resp->content_type = g_strdup(mime ? mime : "application/octet-stream");
    g_free(mime);
    g_free(ctype);
    g_free(path);
    return TRUE;
}

static const char k_about_start_template[] =
    "<!doctype html><html><head>"
    "<meta charset=\"utf-8\">"
    "<title>Nordstjernen</title>"
    "<style>\n"
    "html, body { background:#ffffff; color:#111111;"
    " font-family: system-ui, -apple-system, \"Segoe UI\","
    " Helvetica, Arial, sans-serif; margin:0; padding:0; min-height:100%; }\n"
    ".wrap { max-width: 720px; margin: 9vh auto 0 auto;"
    " padding: 0 24px 48px 24px; text-align:center; }\n"
    ".logo { margin: 0 auto 1.0em auto; width: 128px; height: 128px;"
    " display:block; border-radius: 18px; }\n"
    ".title { font-size: 2.2em; font-weight: 600; margin: 0.1em 0 0.1em 0;"
    " letter-spacing: 0.5px; color:#111111; }\n"
    ".tagline { color:#666666; font-style: italic; margin: 0 0 3.4em 0;"
    " font-size: 1.0em; }\n"
    "form.search { margin: 0 auto 5.5em auto; display:flex; gap: 18px;"
    " max-width: 560px; align-items: center; justify-content: center;"
    " font-size: 1.15em; }\n"
    "form.search input, form.search button {"
    " display:inline; border:0; padding:0; margin:0;"
    " background: transparent; font: inherit; }\n"
    "form.search input { flex: 0 0 320px; text-align: left; }\n"
    ".footer { color:#888888; font-size:0.85em; margin-top:0;"
    " text-align:center; }\n"
    ".footer a { color:#3a63d0; }\n"
    "</style></head>"
    "<body>"
    "<div class=\"wrap\">"
    "<img class=\"logo\" alt=\"Nordstjernen\" src=\"__ND_LOGO_URI__\">"
    "<div class=\"title\">Nordstjernen</div>"
    "<div class=\"tagline\">Nordstjernen is a fine web browser</div>"
    "<form class=\"search\" action=\"https://html.duckduckgo.com/html/\""
    " method=\"get\">"
    "<input type=\"text\" name=\"q\" size=\"24\" autofocus"
    " placeholder=\"Search DuckDuckGo\">"
    "<button type=\"submit\">Search</button>"
    "</form>"
    "<p class=\"footer\">"
    "<a href=\"about:license\">License</a>"
    " &middot; "
    "<a href=\"https://nordstjernen.org\">nordstjernen.org</a>"
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
        char *body = about_substitute(k_about_start_template,
                                      "__ND_LOGO_URI__",
                                      about_logo_data_uri());
        g_byte_array_append(resp->body, (const guint8 *)body,
                            (guint)strlen(body));
        g_free(body);
    } else if (g_str_equal(what, "license") || g_str_equal(what, "licence")) {
        char *body = build_about_license();
        g_byte_array_append(resp->body, (const guint8 *)body, (guint)strlen(body));
        g_free(body);
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

static nd_response *
response_from_cache_entry(nd_cache_entry *e)
{
    nd_response *resp = g_new0(nd_response, 1);
    resp->status       = e->status;
    resp->final_url    = g_strdup(e->final_url);
    resp->content_type = g_strdup(e->content_type);
    resp->cors_allow_origin = g_strdup(e->cors_allow_origin);
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
    if (synthesize_file_response(url, top_url, resp))
        return resp;

    char *hsts_upgraded = nd_net_hsts_upgrade(url);
    if (hsts_upgraded) url = hsts_upgraded;

    char *idn_ascii = nd_url_to_ascii(url);
    if (idn_ascii && strcmp(idn_ascii, url) != 0) {
        g_free(hsts_upgraded);
        hsts_upgraded = idn_ascii;
        url = hsts_upgraded;
    } else {
        g_free(idn_ascii);
    }

    char *url_host = nd_url_host_from(url);
    gboolean mobile_ua = nd_mobile_force_host(url_host);
    const nd_config *cfg = nd_config_get();
    const char *configured_ua =
        (cfg && cfg->user_agent && *cfg->user_agent) ? cfg->user_agent
                                                     : ND_USER_AGENT;
    const char *effective_ua = mobile_ua ? nd_mobile_user_agent()
                                         : configured_ua;
    const char *accept_language =
        (cfg && cfg->accept_language && *cfg->accept_language)
            ? cfg->accept_language : nd_net_default_accept_language();
    const char *effective_top_url = top_url ? top_url : url;
    char *top_origin = nd_url_origin_from(effective_top_url);
    char *top_site   = nd_url_site_from(effective_top_url);
    const char *partition_key = (top_site && *top_site) ? top_site
                              : (top_origin ? top_origin : "");
    char *cache_partition = g_strdup_printf("top=%s\x1f" "ua=%s\x1f" "al=%s",
                                            partition_key,
                                            effective_ua, accept_language);
    nd_cookie_policy cookie_policy = cfg ? cfg->cookie_policy : ND_COOKIE_FIRST_PARTY;
    gboolean cookies_allowed = (cookie_policy != ND_COOKIE_NEVER);
    if (cookies_allowed && cookie_policy == ND_COOKIE_FIRST_PARTY &&
        top_url && !nd_url_is_same_site(url, effective_top_url))
        cookies_allowed = FALSE;
    if (!*partition_key)
        cookies_allowed = FALSE;
    char *cookie_partition_path = cookies_allowed
        ? nd_net_cookie_path_for_partition(partition_key) : NULL;

    nd_cache_entry *cached = NULL;
    if (is_simple_get(method)) {
        cached = nd_cache_get(url, cache_partition);
        if (cached && nd_cache_is_fresh(cached)) {
            gboolean cache_has_cors =
                cached->cors_allow_origin ||
                nd_url_same_origin(effective_top_url, cached->final_url);
            if (!cache_has_cors) {
                nd_cache_entry_free(cached);
                cached = NULL;
            } else {
                nd_response_free(resp);
                nd_response *from_cache = response_from_cache_entry(cached);
                nd_cache_entry_free(cached);
                g_free(cache_partition);
                g_free(cookie_partition_path);
                g_free(top_origin);
                g_free(top_site);
                g_free(hsts_upgraded);
                return from_cache;
            }
        }
    }

    char *referer = nd_net_referer_for(url, top_url,
        cfg ? cfg->referer_policy : ND_REFERER_STRICT_ORIGIN_WHEN_CROSS);
    char *origin_slot = nd_url_origin_from(url);
    gboolean origin_held = FALSE;
    if (origin_slot) {
        origin_held = nd_net_acquire_origin_slot(origin_slot, cancellable);
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
        g_free(referer);
        g_free(cache_partition);
        g_free(cookie_partition_path);
        g_free(top_origin);
        g_free(top_site);
        g_free(hsts_upgraded);
        nd_response_free(resp);
        return NULL;
    }
    if (g_share) curl_easy_setopt(curl, CURLOPT_SHARE, g_share);

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    {
        const char *proxy = nd_net_pick_configured_proxy(url);
        if (proxy && *proxy)
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
        const char *no_proxy = nd_net_configured_no_proxy();
        if (no_proxy && *no_proxy)
            curl_easy_setopt(curl, CURLOPT_NOPROXY, no_proxy);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    long max_redirs = cfg ? (long)cfg->max_redirects : (long)ND_MAX_REDIRECTS;
    if (max_redirs < 0)                       max_redirs = 0;
    if (max_redirs > (long)ND_MAX_REDIRECTS)  max_redirs = (long)ND_MAX_REDIRECTS;
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, max_redirs);

    long fetch_timeout = (long)ND_DEFAULT_TIMEOUT_S;
    if (mobile_ua) fetch_timeout = ND_MAX_TIMEOUT_S;
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
    if (fetch_timeout > (long)ND_MAX_TIMEOUT_S) fetch_timeout = (long)ND_MAX_TIMEOUT_S;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, fetch_timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, effective_ua);
    g_free(url_host);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING,
                     g_accept_encoding ? g_accept_encoding : "");
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
        char *accept = g_strdup_printf(
            "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
            "%s,*/*;q=0.5",
            nd_image_accept_header_fragment());
        headers = curl_slist_append(headers, accept);
        g_free(accept);
    }
    if (!cfg || cfg->do_not_track)
        headers = curl_slist_append(headers, "DNT: 1");

    {
        gboolean send_origin = FALSE;
        if (top_origin && *top_origin) {
            if (top_url && !nd_url_same_origin(top_url, url)) {
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

    if (nd_url_is_http_or_https(url)) {
        const char *fetch_site;
        if (!top_url || !*top_url) {
            fetch_site = "none";
        } else if (nd_url_same_origin(top_url, url)) {
            fetch_site = "same-origin";
        } else if (nd_url_is_same_site(top_url, url)) {
            fetch_site = "same-site";
        } else {
            fetch_site = "cross-site";
        }
        char *site_h = g_strdup_printf("Sec-Fetch-Site: %s", fetch_site);
        headers = curl_slist_append(headers, site_h);
        g_free(site_h);

        const char *fetch_mode;
        if (!top_url || !*top_url) {
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

        const char *fetch_dest = (!top_url || !*top_url) ? "document" : "empty";
        char *dest_h = g_strdup_printf("Sec-Fetch-Dest: %s", fetch_dest);
        headers = curl_slist_append(headers, dest_h);
        g_free(dest_h);

        if (!top_url || !*top_url) {
            headers = curl_slist_append(headers, "Sec-Fetch-User: ?1");
        }

        const char *platform =
#if defined(G_OS_WIN32)
            "\"Windows\"";
#elif defined(__APPLE__)
            "\"macOS\"";
#elif defined(__linux__)
            "\"Linux\"";
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
            "\"Unknown\"";
#else
            "\"Unknown\"";
#endif
        if (!mobile_ua) {
            char *ua_brand = g_strdup_printf(
                "Sec-CH-UA: \"Nordstjernen\";v=\"" ND_VERSION
                "\", \"Not.A/Brand\";v=\"99\"");
            headers = curl_slist_append(headers, ua_brand);
            g_free(ua_brand);
            headers = curl_slist_append(headers, "Sec-CH-UA-Mobile: ?0");
            char *ua_plat = g_strdup_printf("Sec-CH-UA-Platform: %s", platform);
            headers = curl_slist_append(headers, ua_plat);
            g_free(ua_plat);
        }

        if (cfg && cfg->do_not_track) {
            headers = curl_slist_append(headers, "Sec-GPC: 1");
        }
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
            if (!h || !*h) continue;
            if (g_str_has_prefix(h, "X-ND-")) continue;
            if (strpbrk(h, "\r\n")) continue;
            headers = curl_slist_append(headers, h);
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

    nd_write_ctx write_ctx = {
        .body = resp->body,
        .total = 0,
        .budget = nd_net_response_budget(),
        .next_recheck = ND_NET_RESPONSE_RECHECK_BYTES,
        .exceeded = FALSE,
    };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nd_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     (curl_off_t)write_ctx.budget);
    nd_header_ctx header_ctx = {0};
    header_ctx.content_type_out = &resp->content_type;
    header_ctx.content_disposition_out = &resp->content_disposition;
    header_ctx.csp_out          = &resp->csp_header;
    header_ctx.xframe_options_out = &resp->xframe_options;
    header_ctx.cors_allow_origin_out = &resp->cors_allow_origin;
    header_ctx.refresh_out = &resp->refresh;
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
            write_ctx.total = 0;
            write_ctx.next_recheck = ND_NET_RESPONSE_RECHECK_BYTES;
            write_ctx.exceeded = FALSE;
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
            if (header_ctx.raw) g_string_free(header_ctx.raw, TRUE);
            nd_cache_entry_free(cached);
            nd_response_free(resp);
            if (origin_held) nd_net_release_origin_slot(origin_slot);
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

    if (rc == CURLE_OK && is_simple_get(method) && !header_ctx.set_cookie_seen &&
        !resp->tls_warning) {
        if (resp->status == 304 && cached && cached->body) {
            nd_cache_promote_304(url, cache_partition,
                                 header_ctx.cache_control, header_ctx.expires);
            g_byte_array_set_size(resp->body, 0);
            g_byte_array_append(resp->body, cached->body->data, cached->body->len);
            resp->status = cached->status;
            g_free(resp->content_type);
            resp->content_type = g_strdup(cached->content_type);
            g_free(resp->cors_allow_origin);
            resp->cors_allow_origin = g_strdup(cached->cors_allow_origin);
        } else if (resp->status > 0 && resp->body && resp->body->len > 0) {
            nd_cache_put(url, cache_partition,
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
    if (header_ctx.raw) {
        g_free(resp->raw_headers);
        resp->raw_headers = g_string_free(header_ctx.raw, FALSE);
    }
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

#define ND_MAX_CONCURRENT_FETCHES 6
static GMutex g_fetch_throttle_mutex;
static int    g_fetch_active;
static GQueue g_fetch_queue = G_QUEUE_INIT;

static void nd_fetch_thread(GTask *task, gpointer source_object,
                            gpointer task_data, GCancellable *cancellable);

static void
nd_fetch_throttle_dispatch(void)
{
    for (;;) {
        g_mutex_lock(&g_fetch_throttle_mutex);
        if (g_fetch_active >= ND_MAX_CONCURRENT_FETCHES ||
            g_queue_is_empty(&g_fetch_queue)) {
            g_mutex_unlock(&g_fetch_throttle_mutex);
            return;
        }
        GTask *t = g_queue_pop_head(&g_fetch_queue);
        g_fetch_active++;
        g_mutex_unlock(&g_fetch_throttle_mutex);
        g_task_run_in_thread(t, nd_fetch_thread);
        g_object_unref(t);
    }
}

static void
nd_fetch_throttle_submit(GTask *task)
{
    g_mutex_lock(&g_fetch_throttle_mutex);
    g_queue_push_tail(&g_fetch_queue, task);
    g_mutex_unlock(&g_fetch_throttle_mutex);
    nd_fetch_throttle_dispatch();
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
        if (g_log_fetches)
            nd_debug_log_emit(ND_DLOG_NET, "fetch", "failed %s: %s",
                              ctx->url, err ? err->message : "unknown error");
        g_task_return_error(task, err);
    } else if (g_log_fetches) {
        if (resp->error)
            nd_debug_log_emit(ND_DLOG_NET, "fetch", "error %s: %s",
                              ctx->url, resp->error);
        else
            nd_debug_log_emit(ND_DLOG_NET, "fetch", "%ld %s (%u bytes)",
                              resp->status,
                              resp->final_url ? resp->final_url : ctx->url,
                              resp->body ? resp->body->len : 0u);
    }
    if (resp)
        g_task_return_pointer(task, resp, (GDestroyNotify)nd_response_free);
    g_mutex_lock(&g_fetch_throttle_mutex);
    if (g_fetch_active > 0) g_fetch_active--;
    g_mutex_unlock(&g_fetch_throttle_mutex);
    nd_fetch_throttle_dispatch();
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
    nd_fetch_throttle_submit(task);
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
    nd_fetch_throttle_submit(task);
}

nd_response *
nd_net_fetch_finish(GAsyncResult *result, GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

char *
nd_multipart_boundary(void)
{
    guint32 r[4];
    if (!nd_security_csprng_fill(r, sizeof r)) {
        r[0] = g_random_int(); r[1] = g_random_int();
        r[2] = g_random_int(); r[3] = g_random_int();
    }
    return g_strdup_printf("----NordstjernenFormBoundary%08x%08x%08x%08x",
                           r[0], r[1], r[2], r[3]);
}

void
nd_multipart_quote_field(GString *out, const char *s)
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

void
nd_form_urlencoded_append(GString *out, const char *s)
{
    if (!out || !s) return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (g_ascii_isalnum(c) || c == '*' || c == '-' || c == '.' || c == '_')
            g_string_append_c(out, (char)c);
        else if (c == ' ')
            g_string_append_c(out, '+');
        else
            g_string_append_printf(out, "%%%02X", c);
    }
}

void
nd_form_urlencoded_append_pair(GString *out, gboolean *first,
                               const char *name, const char *value)
{
    if (!out || !first || !name) return;
    if (!*first) g_string_append_c(out, '&');
    *first = FALSE;
    nd_form_urlencoded_append(out, name);
    g_string_append_c(out, '=');
    nd_form_urlencoded_append(out, value);
}
