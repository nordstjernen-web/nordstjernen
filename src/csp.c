/* Nordstjernen — Content-Security-Policy parser + check (CSP1+CSP2 subset).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "csp.h"

#include <string.h>

#include "net.h"

struct nd_csp {
    GPtrArray *sources[ND_CSP_KIND_COUNT];
    gboolean   set[ND_CSP_KIND_COUNT];
};

static gboolean
nd_csp_frame_ancestors_allows(const nd_csp *csp,
                              const char *parent_url,
                              const char *document_url);

static nd_csp_kind
directive_kind(const char *name)
{
    static const struct { const char *name; nd_csp_kind kind; } map[] = {
        { "default-src",     ND_CSP_DEFAULT },
        { "script-src",      ND_CSP_SCRIPT },
        { "style-src",       ND_CSP_STYLE },
        { "img-src",         ND_CSP_IMG },
        { "media-src",       ND_CSP_MEDIA },
        { "connect-src",     ND_CSP_CONNECT },
        { "font-src",        ND_CSP_FONT },
        { "frame-src",       ND_CSP_FRAME },
        { "child-src",       ND_CSP_CHILD },
        { "frame-ancestors", ND_CSP_FRAME_ANCESTORS },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_ascii_strcasecmp(name, map[i].name) == 0) return map[i].kind;
    return ND_CSP_KIND_COUNT;
}

nd_csp *
nd_csp_parse(const char *header_value)
{
    if (!header_value || !*header_value) return NULL;
    nd_csp *csp = g_new0(nd_csp, 1);
    char **clauses = g_strsplit(header_value, ";", -1);
    for (int i = 0; clauses[i]; i++) {
        char *clause = g_strstrip(clauses[i]);
        if (!*clause) continue;
        char **toks = g_strsplit_set(clause, " \t", -1);
        if (!toks || !toks[0]) { g_strfreev(toks); continue; }
        nd_csp_kind k = directive_kind(toks[0]);
        if (k == ND_CSP_KIND_COUNT) { g_strfreev(toks); continue; }
        csp->set[k] = TRUE;
        if (!csp->sources[k])
            csp->sources[k] = g_ptr_array_new_with_free_func(g_free);
        for (int j = 1; toks[j]; j++) {
            char *t = g_strstrip(toks[j]);
            if (!*t) continue;
            g_ptr_array_add(csp->sources[k], g_strdup(t));
        }
        g_strfreev(toks);
    }
    g_strfreev(clauses);
    return csp;
}

void
nd_csp_free(nd_csp *csp)
{
    if (!csp) return;
    for (int i = 0; i < ND_CSP_KIND_COUNT; i++)
        if (csp->sources[i]) g_ptr_array_free(csp->sources[i], TRUE);
    g_free(csp);
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
csp_port_matches(const char *src_port, const char *src_scheme,
                 const char *res_port, const char *res_scheme)
{
    if (src_port && strcmp(src_port, "*") == 0) return TRUE;
    const char *sp = (src_port && *src_port)
                     ? src_port
                     : (src_scheme && *src_scheme
                          ? default_port_for_scheme(src_scheme)
                          : default_port_for_scheme(res_scheme));
    const char *rp = (res_port && *res_port)
                     ? res_port : default_port_for_scheme(res_scheme);
    if (!*sp || !*rp) return FALSE;
    return strcmp(sp, rp) == 0;
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
    if (strcmp(src, "'self'") == 0) return nd_url_same_origin(resource_url, doc_url);
    if (strcmp(src, "'unsafe-inline'") == 0 ||
        strcmp(src, "'unsafe-eval'") == 0   ||
        strcmp(src, "'strict-dynamic'") == 0)
        return FALSE;

    if (g_ascii_strcasecmp(src, "https:") == 0 ||
        g_ascii_strcasecmp(src, "http:")  == 0 ||
        g_ascii_strcasecmp(src, "data:")  == 0 ||
        g_ascii_strcasecmp(src, "blob:")  == 0)
        return url_scheme_matches(resource_url, src);

    const char *scheme_sep = strstr(src, "://");
    const char *src_host_start = scheme_sep ? scheme_sep + 3 : src;
    g_autofree char *src_scheme = NULL;
    if (scheme_sep) {
        gsize scheme_len = (gsize)(scheme_sep - src);
        if (g_ascii_strncasecmp(resource_url, src, scheme_len) != 0 ||
            resource_url[scheme_len] != ':')
            return FALSE;
        src_scheme = g_ascii_strdown(src, scheme_len);
    }

    g_autoptr(nd_url_parts) res = nd_url_parts_new(resource_url);
    if (!res || !res->hostname) return FALSE;
    const char *res_scheme = res->protocol ? res->protocol : "";

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
             g_ascii_strcasecmp(res->hostname + rh_len - sfx_len, suffix) == 0;
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
    if (!csp_port_matches(src_port, src_scheme, res->port, res_scheme))
        return FALSE;

    const char *src_path = path_p ? path_p : NULL;
    if (!csp_path_matches(src_path, res->pathname)) return FALSE;

    return TRUE;
}

gboolean
nd_csp_allows(const nd_csp *csp, nd_csp_kind kind,
              const char *resource_url, const char *document_url)
{
    return nd_csp_allows_with_nonce(csp, kind, resource_url, document_url, NULL);
}

gboolean
nd_csp_allows_with_nonce(const nd_csp *csp, nd_csp_kind kind,
                         const char *resource_url, const char *document_url,
                         const char *nonce)
{
    if (!csp || !resource_url) return TRUE;
    if (kind >= ND_CSP_KIND_COUNT) return TRUE;
    if (kind == ND_CSP_FRAME_ANCESTORS)
        return nd_csp_frame_ancestors_allows(csp, resource_url, document_url);

    nd_csp_kind eff = kind;
    if (!csp->set[eff]) {
        if (kind == ND_CSP_FRAME && csp->set[ND_CSP_CHILD])
            eff = ND_CSP_CHILD;
        else if (csp->set[ND_CSP_DEFAULT])
            eff = ND_CSP_DEFAULT;
        else
            return TRUE;
    }
    GPtrArray *list = csp->sources[eff];
    if (!list || list->len == 0) return FALSE;
    gboolean strict_dynamic = FALSE;
    for (guint i = 0; i < list->len; i++) {
        const char *s = g_ptr_array_index(list, i);
        if (strcmp(s, "'strict-dynamic'") == 0) { strict_dynamic = TRUE; continue; }
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
        if (source_matches(s, resource_url, document_url)) return TRUE;
    }
    if (strict_dynamic && (kind == ND_CSP_SCRIPT ||
                           (kind == ND_CSP_DEFAULT && eff == ND_CSP_DEFAULT)))
        return TRUE;
    return FALSE;
}

static nd_csp_kind
inline_script_kind(const nd_csp *csp)
{
    if (csp->set[ND_CSP_SCRIPT])  return ND_CSP_SCRIPT;
    if (csp->set[ND_CSP_DEFAULT]) return ND_CSP_DEFAULT;
    return ND_CSP_KIND_COUNT;
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
hash_token_matches(const char *src, const char *body, gsize body_len)
{
    GChecksumType type;
    const char *b64;
    if (g_str_has_prefix(src, "'sha256-")) { type = G_CHECKSUM_SHA256; b64 = src + 8; }
    else if (g_str_has_prefix(src, "'sha384-")) { type = G_CHECKSUM_SHA384; b64 = src + 8; }
    else if (g_str_has_prefix(src, "'sha512-")) { type = G_CHECKSUM_SHA512; b64 = src + 8; }
    else return FALSE;
    gsize blen = strlen(b64);
    if (blen < 2 || b64[blen - 1] != '\'') return FALSE;
    char *want = g_strndup(b64, blen - 1);
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

gboolean
nd_csp_inline_script_allowed(const nd_csp *csp,
                             const char *body, gsize body_len,
                             const char *nonce)
{
    if (!csp) return TRUE;
    nd_csp_kind k = inline_script_kind(csp);
    if (k == ND_CSP_KIND_COUNT) return TRUE;
    const GPtrArray *list = csp->sources[k];
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
    return list_has_token(list, "'unsafe-inline'");
}

gboolean
nd_csp_inline_event_handler_allowed(const nd_csp *csp)
{
    if (!csp) return TRUE;
    nd_csp_kind k = inline_script_kind(csp);
    if (k == ND_CSP_KIND_COUNT) return TRUE;
    const GPtrArray *list = csp->sources[k];
    if (!list) return FALSE;
    if (list_has_token(list, "'strict-dynamic'")) return FALSE;
    return list_has_token(list, "'unsafe-inline'");
}

gboolean
nd_csp_javascript_url_allowed(const nd_csp *csp)
{
    return nd_csp_inline_event_handler_allowed(csp);
}

static gboolean
nd_csp_frame_ancestors_allows(const nd_csp *csp,
                              const char *parent_url,
                              const char *document_url)
{
    if (!csp || !csp->set[ND_CSP_FRAME_ANCESTORS]) return TRUE;
    GPtrArray *list = csp->sources[ND_CSP_FRAME_ANCESTORS];
    if (!list || list->len == 0) return FALSE;
    if (!parent_url) return TRUE;
    for (guint i = 0; i < list->len; i++) {
        const char *s = g_ptr_array_index(list, i);
        if (source_matches(s, parent_url, document_url)) return TRUE;
    }
    return FALSE;
}
