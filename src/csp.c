/* Nordstjernen — Content-Security-Policy parser + check (CSP1+CSP2 subset).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "csp.h"

#include <string.h>

#include "net.h"

struct nd_csp {
    GPtrArray *sources[ND_CSP_KIND_COUNT];
    gboolean   set[ND_CSP_KIND_COUNT];
};

static nd_csp_kind
directive_kind(const char *name)
{
    if (g_ascii_strcasecmp(name, "default-src") == 0) return ND_CSP_DEFAULT;
    if (g_ascii_strcasecmp(name, "script-src") == 0)  return ND_CSP_SCRIPT;
    if (g_ascii_strcasecmp(name, "style-src") == 0)   return ND_CSP_STYLE;
    if (g_ascii_strcasecmp(name, "img-src") == 0)     return ND_CSP_IMG;
    if (g_ascii_strcasecmp(name, "media-src") == 0)   return ND_CSP_MEDIA;
    if (g_ascii_strcasecmp(name, "connect-src") == 0) return ND_CSP_CONNECT;
    if (g_ascii_strcasecmp(name, "font-src") == 0)    return ND_CSP_FONT;
    if (g_ascii_strcasecmp(name, "frame-src") == 0 ||
        g_ascii_strcasecmp(name, "child-src") == 0)   return ND_CSP_FRAME;
    if (g_ascii_strcasecmp(name, "frame-ancestors") == 0)
        return ND_CSP_FRAME_ANCESTORS;
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
source_matches(const char *src, const char *resource_url, const char *doc_url)
{
    if (!src || !*src) return FALSE;
    if (strcmp(src, "'none'") == 0) return FALSE;
    if (strcmp(src, "*") == 0)      return TRUE;
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
    if (scheme_sep) {
        gsize scheme_len = (gsize)(scheme_sep - src);
        if (g_ascii_strncasecmp(resource_url, src, scheme_len) != 0 ||
            resource_url[scheme_len] != ':')
            return FALSE;
    }

    char *res_host = nd_url_host_from(resource_url);
    if (!res_host) return FALSE;

    const char *port_p = strchr(src_host_start, ':');
    const char *path_p = strchr(src_host_start, '/');
    const char *host_end = src_host_start + strlen(src_host_start);
    if (port_p && (!path_p || port_p < path_p)) host_end = port_p;
    else if (path_p)                            host_end = path_p;

    gsize host_len = (gsize)(host_end - src_host_start);
    gboolean ok;
    if (host_len >= 2 && strncmp(src_host_start, "*.", 2) == 0) {
        const char *suffix = src_host_start + 1;
        gsize sfx_len = host_len - 1;
        gsize rh_len  = strlen(res_host);
        ok = rh_len > sfx_len &&
             g_ascii_strcasecmp(res_host + rh_len - sfx_len, suffix) == 0;
    } else {
        ok = strlen(res_host) == host_len &&
             g_ascii_strncasecmp(res_host, src_host_start, host_len) == 0;
    }
    g_free(res_host);
    return ok;
}

gboolean
nd_csp_allows(const nd_csp *csp, nd_csp_kind kind,
              const char *resource_url, const char *document_url)
{
    if (!csp || !resource_url) return TRUE;
    if (kind >= ND_CSP_KIND_COUNT) return TRUE;
    if (kind == ND_CSP_FRAME_ANCESTORS)
        return nd_csp_frame_ancestors_allows(csp, resource_url, document_url);

    nd_csp_kind eff = kind;
    if (!csp->set[eff]) {
        if (!csp->set[ND_CSP_DEFAULT]) return TRUE;
        eff = ND_CSP_DEFAULT;
    }
    GPtrArray *list = csp->sources[eff];
    if (!list || list->len == 0) return FALSE;
    for (guint i = 0; i < list->len; i++) {
        const char *s = g_ptr_array_index(list, i);
        if (source_matches(s, resource_url, document_url)) return TRUE;
    }
    return FALSE;
}

gboolean
nd_csp_has_frame_ancestors(const nd_csp *csp)
{
    return csp && csp->set[ND_CSP_FRAME_ANCESTORS];
}

gboolean
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
