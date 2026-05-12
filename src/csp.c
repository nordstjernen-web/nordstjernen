/* Nordstjernen — Content-Security-Policy parser + check (CSP1+CSP2 subset). */

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
    return ND_CSP_KIND_COUNT;
}

nd_csp *
nd_csp_parse(const char *header_value)
{
    if (!header_value || !*header_value) return NULL;
    nd_csp *csp = g_new0(nd_csp, 1);
    for (int i = 0; i < ND_CSP_KIND_COUNT; i++)
        csp->sources[i] = g_ptr_array_new_with_free_func(g_free);
    char **clauses = g_strsplit(header_value, ";", -1);
    for (int i = 0; clauses[i]; i++) {
        char *clause = g_strstrip(clauses[i]);
        if (!*clause) continue;
        char **toks = g_strsplit_set(clause, " \t", -1);
        if (!toks || !toks[0]) { g_strfreev(toks); continue; }
        nd_csp_kind k = directive_kind(toks[0]);
        if (k == ND_CSP_KIND_COUNT) { g_strfreev(toks); continue; }
        csp->set[k] = TRUE;
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
same_origin(const char *a, const char *b)
{
    if (!a || !b) return FALSE;
    const char *as = strstr(a, "://");
    const char *bs = strstr(b, "://");
    if (!as || !bs) return FALSE;
    if ((as - a) != (bs - b) || g_ascii_strncasecmp(a, b, (gsize)(as - a)) != 0)
        return FALSE;
    char *ah = nd_url_host_from(a);
    char *bh = nd_url_host_from(b);
    gboolean eq = (ah && bh && g_ascii_strcasecmp(ah, bh) == 0);
    g_free(ah); g_free(bh);
    return eq;
}

static gboolean
url_scheme_is(const char *url, const char *scheme)
{
    gsize n = strlen(scheme);
    if (g_ascii_strncasecmp(url, scheme, n) != 0) return FALSE;
    return url[n] == ':';
}

static gboolean
source_matches(const char *src, const char *resource_url, const char *doc_url)
{
    if (!src || !*src) return FALSE;
    if (strcmp(src, "'none'") == 0) return FALSE;
    if (strcmp(src, "*") == 0)      return TRUE;
    if (strcmp(src, "'self'") == 0) return same_origin(resource_url, doc_url);
    if (strcmp(src, "'unsafe-inline'") == 0 ||
        strcmp(src, "'unsafe-eval'") == 0   ||
        strcmp(src, "'strict-dynamic'") == 0)
        return FALSE;

    if (g_ascii_strcasecmp(src, "https:") == 0 ||
        g_ascii_strcasecmp(src, "http:")  == 0 ||
        g_ascii_strcasecmp(src, "data:")  == 0 ||
        g_ascii_strcasecmp(src, "blob:")  == 0)
        return url_scheme_is(resource_url, (char[]){src[0], src[1], src[2], src[3], src[4] == ':' ? 0 : src[4], 0});

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
    if (kind < 0 || kind >= ND_CSP_KIND_COUNT) return TRUE;

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
