/* Nordstjernen — google.com compatibility shims. */

#include "google.h"

#include <string.h>

#include "net.h"

gboolean
nd_google_host_is_google(const char *host)
{
    if (!host) return FALSE;
    if (g_ascii_strncasecmp(host, "www.", 4) == 0) host += 4;
    if (g_ascii_strncasecmp(host, "google.", 7) != 0) return FALSE;
    const char *tld = host + 7;
    if (!*tld) return FALSE;
    for (const char *c = tld; *c; c++)
        if (!g_ascii_isalnum(*c) && *c != '.') return FALSE;
    return TRUE;
}

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

char *
nd_google_unwrap_redirect_href(const char *href)
{
    if (!href || !*href) return NULL;
    const char *path = NULL;
    if (g_str_has_prefix(href, "/url?")) {
        path = href;
    } else if (g_str_has_prefix(href, "http://") ||
               g_str_has_prefix(href, "https://")) {
        const char *scheme_end = strstr(href, "://");
        if (!scheme_end) return NULL;
        const char *slash = strchr(scheme_end + 3, '/');
        if (!slash || !g_str_has_prefix(slash, "/url?")) return NULL;
        char *host = nd_url_host_from(href);
        gboolean google = nd_google_host_is_google(host);
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

void
nd_google_rewrite_doc(nd_node *node)
{
    if (!node) return;
    if (node->kind == ND_NODE_ELEMENT && node->name &&
        g_ascii_strcasecmp(node->name, "a") == 0) {
        const char *href = nd_element_get_attr(node, "href");
        if (href) {
            char *target = nd_google_unwrap_redirect_href(href);
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
        nd_google_rewrite_doc(c);
}

void
nd_google_rewrite_if_google_host(nd_node *root, const char *page_url)
{
    if (!root || !page_url) return;
    char *host = nd_url_host_from(page_url);
    if (nd_google_host_is_google(host))
        nd_google_rewrite_doc(root);
    g_free(host);
}
