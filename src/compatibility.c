/* Nordstjernen — per-site compatibility rules, UA spoofs and CSS overrides.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "compatibility.h"

#include <string.h>

#include "net.h"

typedef gboolean (*compat_host_match_fn)(const char *host);
typedef void     (*compat_rewrite_fn)(nd_node *root);

typedef struct {
    const char           *id;
    compat_host_match_fn  matches;
    const char           *css_file;
    compat_rewrite_fn     rewrite;
} compat_rule;

static const char *
strip_www(const char *host)
{
    if (!host) return NULL;
    if (g_ascii_strncasecmp(host, "www.", 4) == 0) return host + 4;
    return host;
}

static gboolean
host_eq_or_subdomain(const char *host, const char *base)
{
    if (!host || !base) return FALSE;
    host = strip_www(host);
    if (g_ascii_strcasecmp(host, base) == 0) return TRUE;
    size_t hl = strlen(host), bl = strlen(base);
    return hl > bl + 1 &&
           host[hl - bl - 1] == '.' &&
           g_ascii_strcasecmp(host + hl - bl, base) == 0;
}

static gboolean
google_host_is_google(const char *host)
{
    if (!host) return FALSE;
    host = strip_www(host);
    if (g_ascii_strncasecmp(host, "google.", 7) != 0) return FALSE;
    const char *tld = host + 7;
    if (!*tld) return FALSE;
    for (const char *c = tld; *c; c++)
        if (!g_ascii_isalnum(*c) && *c != '.') return FALSE;
    return TRUE;
}

static gboolean match_google     (const char *h) { return google_host_is_google(h); }
static gboolean match_gmail      (const char *h) { return host_eq_or_subdomain(h, "mail.google.com") ||
                                                          host_eq_or_subdomain(h, "gmail.com"); }
static gboolean match_gaccounts  (const char *h) { return host_eq_or_subdomain(h, "accounts.google.com"); }
static gboolean match_duckduckgo (const char *h) { return host_eq_or_subdomain(h, "duckduckgo.com"); }
static gboolean match_wikipedia  (const char *h) { return host_eq_or_subdomain(h, "wikipedia.org"); }
static gboolean match_aftenposten(const char *h) { return host_eq_or_subdomain(h, "aftenposten.no"); }
static gboolean match_reddit     (const char *h) { return host_eq_or_subdomain(h, "reddit.com"); }
static gboolean match_xkcd       (const char *h) { return host_eq_or_subdomain(h, "xkcd.com"); }
static gboolean match_hn         (const char *h) { return host_eq_or_subdomain(h, "news.ycombinator.com"); }

static char *
google_query_param_decode(const char *url, const char *name)
{
    if (!url || !name) return NULL;
    const char *q = strchr(url, '?');
    if (!q) return NULL;
    q++;
    const char *frag = strchr(q, '#');
    size_t qlen = frag ? (size_t)(frag - q) : strlen(q);
    size_t name_len = strlen(name);
    const char *end = q + qlen;
    const char *p = q;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        size_t pair_len = amp ? (size_t)(amp - p) : (size_t)(end - p);
        if (pair_len > name_len &&
            strncmp(p, name, name_len) == 0 &&
            p[name_len] == '=') {
            const char *v = p + name_len + 1;
            size_t vlen = pair_len - name_len - 1;
            char *enc = g_strndup(v, vlen);
            for (size_t i = 0; i < vlen; i++)
                if (enc[i] == '+') enc[i] = ' ';
            char *dec = g_uri_unescape_string(enc, NULL);
            g_free(enc);
            return dec;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return NULL;
}

char *
nd_google_unwrap_consent_url(const char *url)
{
    if (!url || !nd_url_is_http_or_https(url)) return NULL;
    char *host = nd_url_host_from(url);
    if (!host) return NULL;
    gboolean is_consent = g_ascii_strcasecmp(host, "consent.google.com") == 0;
    g_free(host);
    if (!is_consent) return NULL;
    char *cont = google_query_param_decode(url, "continue");
    if (cont && nd_url_is_http_or_https(cont)) return cont;
    g_free(cont);
    return NULL;
}

static gboolean
url_has_query_key(const char *url, const char *name)
{
    if (!url || !name) return FALSE;
    const char *q = strchr(url, '?');
    if (!q) return FALSE;
    q++;
    const char *frag = strchr(q, '#');
    size_t qlen = frag ? (size_t)(frag - q) : strlen(q);
    size_t nl = strlen(name);
    const char *p = q;
    const char *end = q + qlen;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        size_t pair = amp ? (size_t)(amp - p) : (size_t)(end - p);
        if (pair >= nl && strncmp(p, name, nl) == 0 &&
            (pair == nl || p[nl] == '='))
            return TRUE;
        if (!amp) break;
        p = amp + 1;
    }
    return FALSE;
}

static char *
url_append_query(const char *url, const char *kv)
{
    if (!url || !kv) return NULL;
    const char *frag = strchr(url, '#');
    size_t base_len = frag ? (size_t)(frag - url) : strlen(url);
    gboolean has_q = memchr(url, '?', base_len) != NULL;
    return g_strdup_printf("%.*s%c%s%s",
                           (int)base_len, url,
                           has_q ? '&' : '?', kv,
                           frag ? frag : "");
}

static gboolean
path_is_search(const char *path)
{
    return path && (g_str_has_prefix(path, "/search") &&
                    (path[7] == '\0' || path[7] == '?' ||
                     path[7] == '#'  || path[7] == '/'));
}

char *
nd_google_rewrite_url(const char *url)
{
    if (!url || !nd_url_is_http_or_https(url)) return NULL;
    char *host = nd_url_host_from(url);
    if (!host) return NULL;

    if (google_host_is_google(host)) {
        g_free(host);
        const char *p = strstr(url, "://");
        if (!p) return NULL;
        p = strchr(p + 3, '/');
        if (!path_is_search(p ? p : "/")) return NULL;
        if (url_has_query_key(url, "udm") ||
            url_has_query_key(url, "tbm")) return NULL;
        return url_append_query(url, "udm=14");
    }

    g_free(host);
    return NULL;
}

static char *
google_unwrap_redirect_href(const char *href)
{
    if (!href || !*href) return NULL;
    const char *path = NULL;
    if (g_str_has_prefix(href, "/url?")) {
        path = href;
    } else if (nd_url_is_http_or_https(href)) {
        const char *slash = strchr(strstr(href, "://") + 3, '/');
        if (!slash || !g_str_has_prefix(slash, "/url?")) return NULL;
        char *host = nd_url_host_from(href);
        gboolean google = google_host_is_google(host);
        g_free(host);
        if (!google) return NULL;
        path = slash;
    } else {
        return NULL;
    }
    char *target = google_query_param_decode(path, "q");
    if (!target || !nd_url_is_http_or_https(target)) {
        g_free(target);
        target = google_query_param_decode(path, "url");
    }
    if (target && !nd_url_is_http_or_https(target)) {
        g_free(target);
        return NULL;
    }
    return target;
}

static gboolean
class_attr_has_token(const char *cls, const char *token)
{
    if (!cls || !token || !*token) return FALSE;
    size_t tl = strlen(token);
    const char *p = cls;
    while (*p) {
        while (*p && g_ascii_isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !g_ascii_isspace((unsigned char)*p)) p++;
        if ((size_t)(p - s) == tl && strncmp(s, token, tl) == 0) return TRUE;
    }
    return FALSE;
}

static void
hn_rewrite_doc(nd_node *node)
{
    if (!node) return;
    if (nd_node_is_element_named(node, "div")) {
        const char *cls = nd_element_get_attr(node, "class");
        if (class_attr_has_token(cls, "votearrow")) {
            gboolean down = class_attr_has_token(cls, "rotate180");
            g_free(node->name);
            node->name = g_strdup("span");
            while (node->first_child) {
                nd_node *c = node->first_child;
                nd_node_remove(c);
                nd_node_free(c);
            }
            nd_node_append_child(node,
                nd_node_new_text(g_strdup(down ? "\xe2\x96\xbc"
                                               : "\xe2\x96\xb2")));
        }
    }
    for (nd_node *c = node->first_child; c; c = c->next_sibling)
        hn_rewrite_doc(c);
}

static void
google_rewrite_doc(nd_node *node)
{
    if (!node) return;
    if (nd_node_is_element_named(node, "a")) {
        const char *href = nd_element_get_attr(node, "href");
        if (href) {
            char *target = google_unwrap_redirect_href(href);
            if (target) {
                nd_element_set_attr(node, "href", target);
                nd_element_remove_attr(node, "ping");
                nd_element_remove_attr(node, "data-ved");
                nd_element_remove_attr(node, "onmousedown");
                g_free(target);
            }
        }
    }
    for (nd_node *c = node->first_child; c; c = c->next_sibling)
        google_rewrite_doc(c);
}

static const compat_rule k_rules[] = {
    { "google",      match_google,      "google.css",      google_rewrite_doc },
    { "gmail",       match_gmail,       "gmail.css",       NULL },
    { "accounts",    match_gaccounts,   "accounts.css",    NULL },
    { "duckduckgo",  match_duckduckgo,  "duckduckgo.css",  NULL },
    { "wikipedia",   match_wikipedia,   "wikipedia.css",   NULL },
    { "aftenposten", match_aftenposten, "aftenposten.css", NULL },
    { "reddit",      match_reddit,      "reddit.css",      NULL },
    { "xkcd",        match_xkcd,        "xkcd.css",        NULL },
    { "hn",          match_hn,          "hn.css",          hn_rewrite_doc },
};

static const compat_rule *
compat_rule_for_host(const char *host)
{
    if (!host || !*host) return NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(k_rules); i++)
        if (k_rules[i].matches && k_rules[i].matches(host))
            return &k_rules[i];
    return NULL;
}

static const compat_rule *
compat_rule_for_url(const char *url)
{
    if (!url) return NULL;
    char *host = nd_url_host_from(url);
    const compat_rule *rule = compat_rule_for_host(host);
    g_free(host);
    return rule;
}

static const char *const k_css_dir_candidates[] = {
    "data/compatibility-css",
    "../share/nordstjernen/compatibility-css",
    "/usr/local/share/nordstjernen/compatibility-css",
    "/usr/share/nordstjernen/compatibility-css",
    NULL,
};

static GKeyFile *g_ua_keyfile;
static gboolean  g_ua_keyfile_loaded;

static char *
compat_data_locate(const char *basename);

static void
compat_ua_load(void)
{
    if (g_ua_keyfile_loaded) return;
    g_ua_keyfile_loaded = TRUE;
    char *path = compat_data_locate("user-agents.conf");
    if (!path) return;
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    if (!g_key_file_load_from_file(kf, path,
                                   G_KEY_FILE_NONE, &err)) {
        g_clear_error(&err);
        g_key_file_free(kf);
        g_free(path);
        return;
    }
    g_free(path);
    g_ua_keyfile = kf;
}

static const char *
compat_ua_lookup(const char *rule_id)
{
    if (!rule_id) return NULL;
    compat_ua_load();
    if (!g_ua_keyfile) return NULL;
    static GHashTable *cache;
    if (!cache)
        cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const char *cached = g_hash_table_lookup(cache, rule_id);
    if (cached) return *cached ? cached : NULL;
    char *ua = g_key_file_get_string(g_ua_keyfile,
                                     "user-agents", rule_id, NULL);
    if (ua) g_strstrip(ua);
    g_hash_table_replace(cache, g_strdup(rule_id), ua ? ua : g_strdup(""));
    cached = g_hash_table_lookup(cache, rule_id);
    return (cached && *cached) ? cached : NULL;
}

const char *
nd_compat_user_agent_for_host(const char *host)
{
    const compat_rule *r = compat_rule_for_host(host);
    return r ? compat_ua_lookup(r->id) : NULL;
}

const char *
nd_compat_user_agent_for_url(const char *url)
{
    const compat_rule *r = compat_rule_for_url(url);
    return r ? compat_ua_lookup(r->id) : NULL;
}

static char *
compat_user_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), "nordstjernen",
                            "compatibility-css", NULL);
}

static char *
compat_data_locate(const char *basename)
{
    if (!basename || !*basename) return NULL;
    char *user_dir = compat_user_dir();
    char *user_path = g_build_filename(user_dir, basename, NULL);
    g_free(user_dir);
    if (g_file_test(user_path, G_FILE_TEST_IS_REGULAR)) return user_path;
    g_free(user_path);
    for (gsize i = 0; k_css_dir_candidates[i]; i++) {
        char *p = g_build_filename(k_css_dir_candidates[i], basename, NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) return p;
        g_free(p);
    }
    return NULL;
}

static nd_css_stylesheet *
compat_load_stylesheet(const char *basename)
{
    char *path = compat_data_locate(basename);
    if (!path) return NULL;
    char *body = NULL;
    gsize len = 0;
    GError *err = NULL;
    gboolean ok = g_file_get_contents(path, &body, &len, &err);
    g_free(path);
    if (!ok) {
        g_clear_error(&err);
        g_free(body);
        return NULL;
    }
    nd_css_stylesheet *sheet = nd_css_stylesheet_parse(body, (gssize)len);
    g_free(body);
    return sheet;
}

nd_css_stylesheet *
nd_compat_stylesheet_for_host(const char *host)
{
    const compat_rule *r = compat_rule_for_host(host);
    if (!r || !r->css_file) return NULL;
    return compat_load_stylesheet(r->css_file);
}

nd_css_stylesheet *
nd_compat_stylesheet_for_url(const char *url)
{
    const compat_rule *r = compat_rule_for_url(url);
    if (!r || !r->css_file) return NULL;
    return compat_load_stylesheet(r->css_file);
}

void
nd_compat_rewrite_doc(nd_node *root, const char *page_url)
{
    if (!root || !page_url) return;
    const compat_rule *r = compat_rule_for_url(page_url);
    if (r && r->rewrite) r->rewrite(root);
}
