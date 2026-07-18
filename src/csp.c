/* Nordstjernen — Content-Security-Policy parser + check (CSP1+CSP2 subset).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "csp.h"

#include <string.h>

#include "net.h"

typedef struct {
    GPtrArray *sources[NS_CSP_KIND_COUNT];
    gboolean   set[NS_CSP_KIND_COUNT];
} ns_csp_policy;

struct ns_csp {
    GPtrArray *policies;
};

static gboolean
policy_frame_ancestors_allows(const ns_csp_policy *p,
                              const char *parent_url,
                              const char *document_url);

static ns_csp_kind
directive_kind(const char *name)
{
    static const struct { const char *name; ns_csp_kind kind; } map[] = {
        { "default-src",     NS_CSP_DEFAULT },
        { "script-src",      NS_CSP_SCRIPT },
        { "style-src",       NS_CSP_STYLE },
        { "img-src",         NS_CSP_IMG },
        { "media-src",       NS_CSP_MEDIA },
        { "connect-src",     NS_CSP_CONNECT },
        { "font-src",        NS_CSP_FONT },
        { "frame-src",       NS_CSP_FRAME },
        { "child-src",       NS_CSP_CHILD },
        { "worker-src",      NS_CSP_WORKER },
        { "frame-ancestors", NS_CSP_FRAME_ANCESTORS },
        { "object-src",      NS_CSP_OBJECT },
        { "base-uri",        NS_CSP_BASE_URI },
        { "form-action",     NS_CSP_FORM_ACTION },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_ascii_strcasecmp(name, map[i].name) == 0) return map[i].kind;
    return NS_CSP_KIND_COUNT;
}

static ns_csp_policy *
policy_parse(const char *policy_text)
{
    ns_csp_policy *p = g_new0(ns_csp_policy, 1);
    char **clauses = g_strsplit(policy_text, ";", -1);
    for (int i = 0; clauses[i]; i++) {
        char *clause = g_strstrip(clauses[i]);
        if (!*clause) continue;
        char **toks = g_strsplit_set(clause, " \t", -1);
        if (!toks || !toks[0]) { g_strfreev(toks); continue; }
        ns_csp_kind k = directive_kind(toks[0]);
        if (k == NS_CSP_KIND_COUNT || p->set[k]) { g_strfreev(toks); continue; }
        p->set[k] = TRUE;
        if (!p->sources[k])
            p->sources[k] = g_ptr_array_new_with_free_func(g_free);
        for (int j = 1; toks[j]; j++) {
            char *t = g_strstrip(toks[j]);
            if (!*t) continue;
            g_ptr_array_add(p->sources[k], g_strdup(t));
        }
        g_strfreev(toks);
    }
    g_strfreev(clauses);
    return p;
}

static void
policy_free(ns_csp_policy *p)
{
    if (!p) return;
    for (int i = 0; i < NS_CSP_KIND_COUNT; i++)
        if (p->sources[i]) g_ptr_array_free(p->sources[i], TRUE);
    g_free(p);
}

ns_csp *
ns_csp_parse(const char *header_value)
{
    if (!header_value || !*header_value) return NULL;
    ns_csp *csp = g_new0(ns_csp, 1);
    csp->policies = g_ptr_array_new_with_free_func((GDestroyNotify)policy_free);
    char **policies = g_strsplit(header_value, ",", -1);
    for (int i = 0; policies[i]; i++) {
        char *ptext = g_strstrip(policies[i]);
        if (!*ptext) continue;
        g_ptr_array_add(csp->policies, policy_parse(ptext));
    }
    g_strfreev(policies);
    if (csp->policies->len == 0) {
        g_ptr_array_free(csp->policies, TRUE);
        g_free(csp);
        return NULL;
    }
    return csp;
}

void
ns_csp_free(ns_csp *csp)
{
    if (!csp) return;
    g_ptr_array_free(csp->policies, TRUE);
    g_free(csp);
}

void
ns_csp_merge(ns_csp *dst, ns_csp *src)
{
    if (!dst || !src || !dst->policies || !src->policies) return;
    g_ptr_array_set_free_func(src->policies, NULL);
    for (guint i = 0; i < src->policies->len; i++)
        g_ptr_array_add(dst->policies, g_ptr_array_index(src->policies, i));
    g_ptr_array_set_size(src->policies, 0);
}


static gboolean
url_scheme_matches(const char *url, const char *scheme_with_colon)
{
    gsize n = strlen(scheme_with_colon);
    if (n == 0) return FALSE;
    gsize cmp_len = scheme_with_colon[n - 1] == ':' ? n - 1 : n;
    if (g_ascii_strncasecmp(url, scheme_with_colon, cmp_len) != 0) return FALSE;
    return url[cmp_len] == ':';
}

static gboolean
scheme_part_matches(const char *scheme, gsize n, const char *url)
{
    if (g_ascii_strncasecmp(url, scheme, n) == 0 && url[n] == ':') return TRUE;
    if (n == 4 && g_ascii_strncasecmp(scheme, "http", 4) == 0)
        return g_ascii_strncasecmp(url, "https:", 6) == 0;
    if (n == 2 && g_ascii_strncasecmp(scheme, "ws", 2) == 0)
        return g_ascii_strncasecmp(url, "wss:", 4) == 0;
    return FALSE;
}

static gboolean
is_network_scheme_url(const char *url)
{
    static const char *const ok[] = {
        "http:", "https:", "ws:", "wss:", "ftp:", NULL,
    };
    for (gsize i = 0; ok[i]; i++)
        if (url_scheme_matches(url, ok[i]))
            return TRUE;
    return FALSE;
}

static const char *
default_port_for_scheme(const char *scheme)
{
    if (!scheme) return "";
    gsize n = strlen(scheme);
    if (n > 0 && scheme[n - 1] == ':') n--;
    if ((n == 5 && g_ascii_strncasecmp(scheme, "https", 5) == 0) ||
        (n == 3 && g_ascii_strncasecmp(scheme, "wss",   3) == 0)) return "443";
    if ((n == 4 && g_ascii_strncasecmp(scheme, "http",  4) == 0) ||
        (n == 2 && g_ascii_strncasecmp(scheme, "ws",    2) == 0)) return "80";
    if  (n == 3 && g_ascii_strncasecmp(scheme, "ftp",   3) == 0)  return "21";
    return "";
}

static gboolean
csp_port_matches(const char *src_port, const char *res_port,
                 const char *res_scheme)
{
    if (src_port && strcmp(src_port, "*") == 0) return TRUE;
    if (!src_port || !*src_port) {
        if (!res_port || !*res_port) return TRUE;
        const char *d = default_port_for_scheme(res_scheme);
        return *d && strcmp(res_port, d) == 0;
    }
    const char *rp = (res_port && *res_port)
                     ? res_port : default_port_for_scheme(res_scheme);
    return *rp && strcmp(src_port, rp) == 0;
}

static gboolean
csp_path_matches(const char *src_path, const char *res_path)
{
    if (!src_path || !*src_path || strcmp(src_path, "/") == 0) return TRUE;
    const char *rp = (res_path && *res_path) ? res_path : "/";
    gsize slen = strlen(src_path);
    if (src_path[slen - 1] == '/') {
        return strncmp(rp, src_path, slen) == 0;
    }
    return strcmp(rp, src_path) == 0;
}

static gboolean
source_matches(const char *src, const char *resource_url, const char *doc_url)
{
    if (!src || !*src) return FALSE;
    if (strcmp(src, "'none'") == 0) return FALSE;
    if (strcmp(src, "*") == 0)      return is_network_scheme_url(resource_url);
    if (strcmp(src, "'self'") == 0) return ns_url_same_origin(resource_url, doc_url);
    if (strcmp(src, "'unsafe-inline'") == 0 ||
        strcmp(src, "'unsafe-eval'") == 0   ||
        strcmp(src, "'strict-dynamic'") == 0)
        return FALSE;

    if (g_ascii_strcasecmp(src, "https:") == 0 ||
        g_ascii_strcasecmp(src, "http:")  == 0 ||
        g_ascii_strcasecmp(src, "wss:")   == 0 ||
        g_ascii_strcasecmp(src, "ws:")    == 0 ||
        g_ascii_strcasecmp(src, "data:")  == 0 ||
        g_ascii_strcasecmp(src, "blob:")  == 0)
        return url_scheme_matches(resource_url, src);

    const char *scheme_sep = strstr(src, "://");
    const char *src_host_start = scheme_sep ? scheme_sep + 3 : src;
    if (scheme_sep) {
        gsize scheme_len = (gsize)(scheme_sep - src);
        if (!scheme_part_matches(src, scheme_len, resource_url))
            return FALSE;
    }

    g_autoptr(ns_url_parts) res = ns_url_parts_new(resource_url);
    if (!res || !res->hostname) return FALSE;
    const char *res_scheme = res->protocol ? res->protocol : "";

    if (!scheme_sep) {
        g_autoptr(ns_url_parts) docp = doc_url ? ns_url_parts_new(doc_url) : NULL;
        const char *doc_scheme = (docp && docp->protocol) ? docp->protocol : NULL;
        if (doc_scheme && *doc_scheme) {
            gboolean scheme_ok =
                g_ascii_strcasecmp(res_scheme, doc_scheme) == 0 ||
                (g_ascii_strcasecmp(doc_scheme, "http:") == 0 &&
                 g_ascii_strcasecmp(res_scheme, "https:") == 0) ||
                (g_ascii_strcasecmp(doc_scheme, "ws:") == 0 &&
                 g_ascii_strcasecmp(res_scheme, "wss:") == 0);
            if (!scheme_ok) return FALSE;
        } else if (!is_network_scheme_url(resource_url)) {
            return FALSE;
        }
    }

    const char *port_p = strchr(src_host_start, ':');
    const char *path_p = strchr(src_host_start, '/');
    const char *host_end = src_host_start + strlen(src_host_start);
    if (port_p && (!path_p || port_p < path_p)) host_end = port_p;
    else if (path_p)                            host_end = path_p;

    gsize host_len = (gsize)(host_end - src_host_start);
    gboolean host_ok;
    if (host_len >= 2 && strncmp(src_host_start, "*.", 2) == 0) {
        const char *suffix = src_host_start + 1;
        gsize sfx_len = host_len - 1;
        gsize rh_len  = strlen(res->hostname);
        host_ok = rh_len > sfx_len &&
             g_ascii_strncasecmp(res->hostname + rh_len - sfx_len, suffix,
                                 sfx_len) == 0;
    } else {
        host_ok = strlen(res->hostname) == host_len &&
             g_ascii_strncasecmp(res->hostname, src_host_start, host_len) == 0;
    }
    if (!host_ok) return FALSE;

    g_autofree char *src_port = NULL;
    if (port_p && (!path_p || port_p < path_p)) {
        const char *pe = path_p ? path_p : src + strlen(src);
        src_port = g_strndup(port_p + 1, (gsize)(pe - port_p - 1));
    }
    if (!csp_port_matches(src_port, res->port, res_scheme))
        return FALSE;

    const char *src_path = path_p ? path_p : NULL;
    if (!csp_path_matches(src_path, res->pathname)) return FALSE;

    return TRUE;
}

static gboolean list_has_token(const GPtrArray *list, const char *tok);

static gboolean
policy_allows_with_nonce(const ns_csp_policy *p, ns_csp_kind kind,
                         const char *resource_url, const char *document_url,
                         const char *nonce, gboolean parser_inserted)
{
    if (kind == NS_CSP_FRAME_ANCESTORS)
        return policy_frame_ancestors_allows(p, resource_url, document_url);

    ns_csp_kind eff = kind;
    if (!p->set[eff]) {
        if (kind == NS_CSP_BASE_URI || kind == NS_CSP_FORM_ACTION)
            return TRUE;
        else if (kind == NS_CSP_WORKER && p->set[NS_CSP_CHILD])
            eff = NS_CSP_CHILD;
        else if (kind == NS_CSP_WORKER && p->set[NS_CSP_SCRIPT])
            eff = NS_CSP_SCRIPT;
        else if (kind == NS_CSP_FRAME && p->set[NS_CSP_CHILD])
            eff = NS_CSP_CHILD;
        else if (p->set[NS_CSP_DEFAULT])
            eff = NS_CSP_DEFAULT;
        else
            return TRUE;
    }
    GPtrArray *list = p->sources[eff];
    if (!list || list->len == 0) return FALSE;
    gboolean script_like = (kind == NS_CSP_SCRIPT ||
                            (kind == NS_CSP_DEFAULT && eff == NS_CSP_DEFAULT));
    gboolean strict_dynamic = script_like &&
                              list_has_token(list, "'strict-dynamic'");
    for (guint i = 0; i < list->len; i++) {
        const char *s = g_ptr_array_index(list, i);
        if (strcmp(s, "'strict-dynamic'") == 0) continue;
        if (nonce && g_str_has_prefix(s, "'nonce-")) {
            gsize slen = strlen(s);
            if (slen > 8 && s[slen - 1] == '\'') {
                gsize want_len = slen - 8;
                if (strlen(nonce) == want_len &&
                    strncmp(s + 7, nonce, want_len) == 0)
                    return TRUE;
            }
            continue;
        }
        if (strict_dynamic) continue;
        if (source_matches(s, resource_url, document_url)) return TRUE;
    }
    if (strict_dynamic && !parser_inserted)
        return TRUE;
    return FALSE;
}

gboolean
ns_csp_allows(const ns_csp *csp, ns_csp_kind kind,
              const char *resource_url, const char *document_url)
{
    return ns_csp_allows_with_nonce(csp, kind, resource_url, document_url,
                                    NULL, TRUE);
}

gboolean
ns_csp_allows_with_nonce(const ns_csp *csp, ns_csp_kind kind,
                         const char *resource_url, const char *document_url,
                         const char *nonce, gboolean parser_inserted)
{
    if (!csp || !resource_url) return TRUE;
    if (kind >= NS_CSP_KIND_COUNT) return TRUE;
    for (guint i = 0; i < csp->policies->len; i++) {
        const ns_csp_policy *p = g_ptr_array_index(csp->policies, i);
        if (!policy_allows_with_nonce(p, kind, resource_url, document_url,
                                      nonce, parser_inserted))
            return FALSE;
    }
    return TRUE;
}

static ns_csp_kind
inline_script_kind(const ns_csp_policy *p)
{
    if (p->set[NS_CSP_SCRIPT])  return NS_CSP_SCRIPT;
    if (p->set[NS_CSP_DEFAULT]) return NS_CSP_DEFAULT;
    return NS_CSP_KIND_COUNT;
}

static gboolean
list_has_token(const GPtrArray *list, const char *tok)
{
    if (!list) return FALSE;
    for (guint i = 0; i < list->len; i++)
        if (strcmp(g_ptr_array_index(list, i), tok) == 0) return TRUE;
    return FALSE;
}

static gboolean
list_has_nonce_or_hash(const GPtrArray *list)
{
    if (!list) return FALSE;
    for (guint i = 0; i < list->len; i++) {
        const char *s = g_ptr_array_index(list, i);
        if (g_str_has_prefix(s, "'nonce-") ||
            g_str_has_prefix(s, "'sha256-") ||
            g_str_has_prefix(s, "'sha384-") ||
            g_str_has_prefix(s, "'sha512-"))
            return TRUE;
    }
    return FALSE;
}

static gboolean
hash_token_matches(const char *src, const char *body, gsize body_len)
{
    GChecksumType type;
    const char *b64;
    if (g_str_has_prefix(src, "'sha256-")) { type = G_CHECKSUM_SHA256; b64 = src + 8; }
    else if (g_str_has_prefix(src, "'sha384-")) { type = G_CHECKSUM_SHA384; b64 = src + 8; }
    else if (g_str_has_prefix(src, "'sha512-")) { type = G_CHECKSUM_SHA512; b64 = src + 8; }
    else return FALSE;
    gsize b64_len = strlen(b64);
    if (b64_len < 2 || b64[b64_len - 1] != '\'') return FALSE;
    char *want = g_strndup(b64, b64_len - 1);
    GChecksum *cs = g_checksum_new(type);
    g_checksum_update(cs, (const guchar *)body, (gssize)body_len);
    guint8 raw[64];
    gsize  raw_len = sizeof raw;
    g_checksum_get_digest(cs, raw, &raw_len);
    char *got = g_base64_encode(raw, raw_len);
    char *got_alt = g_strdup(got);
    for (char *p = got_alt; *p; p++) {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
    }
    gboolean ok = strcmp(want, got) == 0 || strcmp(want, got_alt) == 0;
    g_free(got);
    g_free(got_alt);
    g_free(want);
    g_checksum_free(cs);
    return ok;
}

static gboolean
policy_inline_script_allowed(const ns_csp_policy *p,
                             const char *body, gsize body_len,
                             const char *nonce)
{
    ns_csp_kind k = inline_script_kind(p);
    if (k == NS_CSP_KIND_COUNT) return TRUE;
    const GPtrArray *list = p->sources[k];
    if (!list) return FALSE;

    if (nonce && *nonce) {
        char *want = g_strdup_printf("'nonce-%s'", nonce);
        gboolean ok = list_has_token(list, want);
        g_free(want);
        if (ok) return TRUE;
    }
    if (body && body_len > 0) {
        for (guint i = 0; i < list->len; i++) {
            const char *s = g_ptr_array_index(list, i);
            if (hash_token_matches(s, body, body_len)) return TRUE;
        }
    }
    if (list_has_token(list, "'strict-dynamic'")) return FALSE;
    if (list_has_nonce_or_hash(list)) return FALSE;
    return list_has_token(list, "'unsafe-inline'");
}

gboolean
ns_csp_inline_script_allowed(const ns_csp *csp,
                             const char *body, gsize body_len,
                             const char *nonce)
{
    if (!csp) return TRUE;
    for (guint i = 0; i < csp->policies->len; i++) {
        const ns_csp_policy *p = g_ptr_array_index(csp->policies, i);
        if (!policy_inline_script_allowed(p, body, body_len, nonce))
            return FALSE;
    }
    return TRUE;
}

static gboolean
policy_inline_event_handler_allowed(const ns_csp_policy *p)
{
    ns_csp_kind k = inline_script_kind(p);
    if (k == NS_CSP_KIND_COUNT) return TRUE;
    const GPtrArray *list = p->sources[k];
    if (!list) return FALSE;
    if (list_has_token(list, "'strict-dynamic'")) return FALSE;
    if (list_has_nonce_or_hash(list)) return FALSE;
    return list_has_token(list, "'unsafe-inline'");
}

gboolean
ns_csp_inline_event_handler_allowed(const ns_csp *csp)
{
    if (!csp) return TRUE;
    for (guint i = 0; i < csp->policies->len; i++) {
        const ns_csp_policy *p = g_ptr_array_index(csp->policies, i);
        if (!policy_inline_event_handler_allowed(p))
            return FALSE;
    }
    return TRUE;
}

static gboolean
policy_frame_ancestors_allows(const ns_csp_policy *p,
                              const char *parent_url,
                              const char *document_url)
{
    if (!p || !p->set[NS_CSP_FRAME_ANCESTORS]) return TRUE;
    GPtrArray *list = p->sources[NS_CSP_FRAME_ANCESTORS];
    if (!list || list->len == 0) return FALSE;
    if (!parent_url) return TRUE;
    for (guint i = 0; i < list->len; i++) {
        const char *s = g_ptr_array_index(list, i);
        if (source_matches(s, parent_url, document_url)) return TRUE;
    }
    return FALSE;
}
