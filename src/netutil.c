/* Nordstjernen — curl-free network helpers shared by the engine and the
 * thin browser shells: Accept-Language, search-URL building, proxy masking. */

#include "net.h"
#include "config.h"

#include <glib/gstdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#endif

static char *
build_accept_language_from_locales(void)
{
#ifdef G_OS_WIN32
    WCHAR locale_name[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
        char *locale = g_utf16_to_utf8((const gunichar2 *)locale_name,
                                       -1, NULL, NULL, NULL);
        if (locale && *locale) {
            char *dash = strchr(locale, '-');
            char *base = dash ? g_strndup(locale, dash - locale) : NULL;
            GString *windows_languages = g_string_new(locale);
            if (base && g_ascii_strcasecmp(base, locale) != 0)
                g_string_append_printf(windows_languages, ",%s;q=0.9", base);
            if (base && g_ascii_strcasecmp(base, "nb") == 0)
                g_string_append(windows_languages, ",no;q=0.8,nn;q=0.7");
            if (g_ascii_strncasecmp(locale, "en", 2) != 0)
                g_string_append(windows_languages,
                    base && g_ascii_strcasecmp(base, "nb") == 0
                        ? ",en-US;q=0.6,en;q=0.5"
                        : ",en-US;q=0.8,en;q=0.7");
            g_free(base);
            g_free(locale);
            return g_string_free(windows_languages, FALSE);
        }
        g_free(locale);
    }
#endif
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
ns_net_default_accept_language(void)
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

const char *
ns_net_effective_accept_language(void)
{
    const ns_config *cfg = ns_config_get();
    return cfg && cfg->accept_language && *cfg->accept_language
        ? cfg->accept_language : ns_net_default_accept_language();
}

char **
ns_net_navigator_languages(void)
{
    char **parts = g_strsplit(ns_net_effective_accept_language(), ",", -1);
    GPtrArray *langs = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    for (guint i = 0; parts && parts[i] && langs->len < 20; i++) {
        char *part = g_strstrip(parts[i]);
        char *semi = strchr(part, ';');
        if (semi) *semi = '\0';
        part = g_strstrip(part);
        if (!*part || strcmp(part, "*") == 0) continue;
        gboolean valid = TRUE;
        for (const char *p = part; *p; p++) {
            if (!g_ascii_isalnum(*p) && *p != '-') {
                valid = FALSE;
                break;
            }
        }
        if (!valid) continue;
        char *lower = g_ascii_strdown(part, -1);
        if (g_hash_table_contains(seen, lower)) {
            g_free(lower);
            continue;
        }
        g_hash_table_add(seen, lower);
        g_ptr_array_add(langs, g_strdup(part));
    }
    g_strfreev(parts);
    g_hash_table_destroy(seen);
    if (langs->len == 0)
        g_ptr_array_add(langs, g_strdup("en-US"));
    g_ptr_array_add(langs, NULL);
    return (char **)g_ptr_array_free(langs, FALSE);
}

gboolean
ns_address_is_search(const char *s)
{
    if (!s || !*s) return FALSE;
    if (g_str_has_prefix(s, "about:") || g_str_has_prefix(s, "file:") ||
        g_str_has_prefix(s, "data:") || strstr(s, "://"))
        return FALSE;
    for (const char *p = s; *p; p++)
        if (*p == ' ' || *p == '\t')
            return TRUE;
    if (strstr(s, "\xe3\x80\x80"))
        return TRUE;
    if (g_str_has_prefix(s, "localhost") &&
        (s[9] == '\0' || s[9] == ':' || s[9] == '/'))
        return FALSE;
    g_autofree char *local = ns_url_from_local_path(s);
    if (local)
        return FALSE;
    if (strchr(s, '.') || strchr(s, ':'))
        return FALSE;
    return TRUE;
}

char *
ns_search_url_for(const char *query)
{
    const ns_config *cfg = ns_config_get();
    const char *engine = (cfg && cfg->search_engine && *cfg->search_engine)
        ? cfg->search_engine : "https://lite.duckduckgo.com/lite/?q=%s";
    char *enc = g_uri_escape_string(query ? query : "", NULL, TRUE);
    const char *pct = strstr(engine, "%s");
    char *out;
    if (pct) {
        char *prefix = g_strndup(engine, (gsize)(pct - engine));
        out = g_strconcat(prefix, enc, pct + 2, NULL);
        g_free(prefix);
    } else {
        out = g_strconcat(engine, enc, NULL);
    }
    g_free(enc);
    return out;
}

char *
ns_url_from_local_path(const char *path)
{
    if (!path || !*path) return NULL;
    if (g_str_has_prefix(path, "about:") || g_str_has_prefix(path, "file:") ||
        g_str_has_prefix(path, "data:") || strstr(path, "://"))
        return NULL;

    char *candidate = NULL;
#ifdef G_OS_WIN32
    if (g_ascii_isalpha((guchar)path[0]) && path[1] == ':' && path[2] == '\0') {
        char root[4] = { path[0], ':', G_DIR_SEPARATOR, '\0' };
        candidate = g_strdup(root);
    }
#endif

    if (!candidate) {
        if (!g_file_test(path, G_FILE_TEST_EXISTS))
            return NULL;
        candidate = g_canonicalize_filename(path, NULL);
    }
    if (!candidate)
        return NULL;

    if (g_file_test(candidate, G_FILE_TEST_IS_DIR) &&
        !g_str_has_suffix(candidate, G_DIR_SEPARATOR_S)) {
        char *with_sep = g_strconcat(candidate, G_DIR_SEPARATOR_S, NULL);
        g_free(candidate);
        candidate = with_sep;
    }

    char *uri = g_filename_to_uri(candidate, NULL, NULL);
    g_free(candidate);
    return uri;
}

char *
ns_net_proxy_mask(const char *proxy_url)
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
