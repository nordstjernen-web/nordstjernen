/* Nordstjernen — flat key/value config loader. */

#include "config.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"

static nd_config g_cfg;
static char     *g_cfg_path;

static void
set_string(char **slot, const char *value)
{
    g_free(*slot);
    *slot = g_strdup(value ? value : "");
}

static gboolean
parse_bool(const char *v, gboolean dflt)
{
    if (!v || !*v) return dflt;
    if (g_ascii_strcasecmp(v, "true")  == 0 ||
        g_ascii_strcasecmp(v, "yes")   == 0 ||
        g_ascii_strcasecmp(v, "on")    == 0 ||
        strcmp(v, "1") == 0) return TRUE;
    if (g_ascii_strcasecmp(v, "false") == 0 ||
        g_ascii_strcasecmp(v, "no")    == 0 ||
        g_ascii_strcasecmp(v, "off")   == 0 ||
        strcmp(v, "0") == 0) return FALSE;
    return dflt;
}

static int
parse_int(const char *v, int dflt)
{
    if (!v || !*v) return dflt;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return dflt;
    return (int)n;
}

static nd_referer_policy
parse_referer_policy(const char *v, nd_referer_policy dflt)
{
    if (!v || !*v) return dflt;
    if (g_ascii_strcasecmp(v, "none") == 0 ||
        g_ascii_strcasecmp(v, "no-referrer") == 0)             return ND_REFERER_NO_REFERRER;
    if (g_ascii_strcasecmp(v, "same-origin") == 0)             return ND_REFERER_SAME_ORIGIN;
    if (g_ascii_strcasecmp(v, "strict-origin-when-cross-origin") == 0 ||
        g_ascii_strcasecmp(v, "default") == 0)                 return ND_REFERER_STRICT_ORIGIN_WHEN_CROSS;
    if (g_ascii_strcasecmp(v, "unsafe-url") == 0 ||
        g_ascii_strcasecmp(v, "full") == 0)                    return ND_REFERER_UNSAFE_URL;
    return dflt;
}

static nd_cookie_policy
parse_cookie_policy(const char *v, nd_cookie_policy dflt)
{
    if (!v || !*v) return dflt;
    if (g_ascii_strcasecmp(v, "always") == 0)      return ND_COOKIE_ALWAYS;
    if (g_ascii_strcasecmp(v, "first-party") == 0 ||
        g_ascii_strcasecmp(v, "first-party-only") == 0)
                                                   return ND_COOKIE_FIRST_PARTY;
    if (g_ascii_strcasecmp(v, "never") == 0 ||
        g_ascii_strcasecmp(v, "off") == 0)         return ND_COOKIE_NEVER;
    return dflt;
}

static nd_html_parser_choice
parse_html_parser(const char *v, nd_html_parser_choice dflt)
{
    if (!v || !*v) return dflt;
    if (g_ascii_strcasecmp(v, "primary") == 0 ||
        g_ascii_strcasecmp(v, "default") == 0 ||
        g_ascii_strcasecmp(v, "builtin") == 0)  return ND_HTML_PARSER_PRIMARY;
    if (g_ascii_strcasecmp(v, "gumbo") == 0)    return ND_HTML_PARSER_GUMBO;
    return dflt;
}

static void
apply_default(nd_config *c)
{
    set_string(&c->home_url,        "https://duckduckgo.com/lite/");
    set_string(&c->user_agent,      ND_USER_AGENT);
    set_string(&c->accept_language, "en-US,en;q=0.9");
    set_string(&c->search_engine,   "https://duckduckgo.com/lite/?q=%s");
    c->referer_policy        = ND_REFERER_STRICT_ORIGIN_WHEN_CROSS;
    c->cookie_policy         = ND_COOKIE_ALWAYS;
    c->html_parser           = ND_HTML_PARSER_PRIMARY;
    c->do_not_track          = TRUE;
    c->javascript_enabled    = TRUE;
    c->images_enabled        = TRUE;
    c->local_storage_enabled = TRUE;
    c->cache_enabled         = TRUE;
    c->cache_cap_mb          = 256;
    c->default_font_size_px  = 16;
    c->js_eval_budget_ms     = 5000;
    c->js_memory_cap_mb      = 128;
}

static void
apply_pair(nd_config *c, const char *key, const char *value)
{
    if      (strcmp(key, "home_url")              == 0) set_string(&c->home_url,        value);
    else if (strcmp(key, "user_agent")            == 0) set_string(&c->user_agent,      value);
    else if (strcmp(key, "accept_language")       == 0) set_string(&c->accept_language, value);
    else if (strcmp(key, "search_engine")         == 0) set_string(&c->search_engine,   value);
    else if (strcmp(key, "referer_policy")        == 0) c->referer_policy        = parse_referer_policy(value, c->referer_policy);
    else if (strcmp(key, "cookie_policy")         == 0) c->cookie_policy         = parse_cookie_policy(value, c->cookie_policy);
    else if (strcmp(key, "html_parser")           == 0) c->html_parser           = parse_html_parser(value, c->html_parser);
    else if (strcmp(key, "do_not_track")          == 0) c->do_not_track          = parse_bool(value, c->do_not_track);
    else if (strcmp(key, "javascript_enabled")    == 0) c->javascript_enabled    = parse_bool(value, c->javascript_enabled);
    else if (strcmp(key, "images_enabled")        == 0) c->images_enabled        = parse_bool(value, c->images_enabled);
    else if (strcmp(key, "local_storage_enabled") == 0) c->local_storage_enabled = parse_bool(value, c->local_storage_enabled);
    else if (strcmp(key, "cache_enabled")         == 0) c->cache_enabled         = parse_bool(value, c->cache_enabled);
    else if (strcmp(key, "cache_cap_mb")          == 0) c->cache_cap_mb          = parse_int (value, c->cache_cap_mb);
    else if (strcmp(key, "default_font_size_px")  == 0) c->default_font_size_px  = parse_int (value, c->default_font_size_px);
    else if (strcmp(key, "js_eval_budget_ms")     == 0) c->js_eval_budget_ms     = parse_int (value, c->js_eval_budget_ms);
    else if (strcmp(key, "js_memory_cap_mb")      == 0) c->js_memory_cap_mb      = parse_int (value, c->js_memory_cap_mb);
}

static void
load_file(nd_config *c, const char *path)
{
    char *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, NULL)) return;
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key   = g_strstrip(line);
        char *value = g_strstrip(eq + 1);
        if (*key) apply_pair(c, key, value);
    }
    g_strfreev(lines);
    g_free(contents);
}

static void
apply_env(nd_config *c)
{
    if (g_getenv("ND_NO_CACHE"))         c->cache_enabled         = FALSE;
    if (g_getenv("ND_NO_LOCAL_STORAGE")) c->local_storage_enabled = FALSE;
    if (g_getenv("ND_NO_JAVASCRIPT") ||
        g_getenv("ND_NO_JS"))            c->javascript_enabled    = FALSE;
    if (g_getenv("ND_NO_IMAGES"))        c->images_enabled        = FALSE;
    const char *parser = g_getenv("ND_HTML_PARSER");
    if (parser) c->html_parser = parse_html_parser(parser, c->html_parser);
    const char *home = g_getenv("ND_HOME_URL");
    if (home && *home) set_string(&c->home_url, home);
    const char *ua = g_getenv("ND_USER_AGENT");
    if (ua && *ua) set_string(&c->user_agent, ua);
}

void
nd_config_init(void)
{
    apply_default(&g_cfg);
    g_cfg_path = g_build_filename(g_get_user_config_dir(),
                                  "nordstjernen", "nordstjernen.conf",
                                  NULL);
    load_file(&g_cfg, g_cfg_path);
    apply_env(&g_cfg);
}

void
nd_config_shutdown(void)
{
    g_free(g_cfg.home_url);
    g_free(g_cfg.user_agent);
    g_free(g_cfg.accept_language);
    g_free(g_cfg.search_engine);
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_clear_pointer(&g_cfg_path, g_free);
}

const nd_config *
nd_config_get(void)
{
    return &g_cfg;
}

const char *
nd_config_path(void)
{
    return g_cfg_path;
}

static const char *
referer_policy_name(nd_referer_policy p)
{
    switch (p) {
    case ND_REFERER_NO_REFERRER:           return "none";
    case ND_REFERER_SAME_ORIGIN:           return "same-origin";
    case ND_REFERER_STRICT_ORIGIN_WHEN_CROSS: return "strict-origin-when-cross-origin";
    case ND_REFERER_UNSAFE_URL:            return "unsafe-url";
    }
    return "strict-origin-when-cross-origin";
}

static const char *
cookie_policy_name(nd_cookie_policy p)
{
    switch (p) {
    case ND_COOKIE_ALWAYS:      return "always";
    case ND_COOKIE_FIRST_PARTY: return "first-party";
    case ND_COOKIE_NEVER:       return "never";
    }
    return "always";
}

static const char *
html_parser_name(nd_html_parser_choice p)
{
    switch (p) {
    case ND_HTML_PARSER_PRIMARY: return "primary";
    case ND_HTML_PARSER_GUMBO:   return "gumbo";
    }
    return "primary";
}

char *
nd_config_dump(void)
{
    const nd_config *c = &g_cfg;
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "# nordstjernen effective config\n");
    g_string_append_printf(s, "# file: %s\n", g_cfg_path ? g_cfg_path : "(none)");
    g_string_append_printf(s, "home_url              = %s\n", c->home_url);
    g_string_append_printf(s, "user_agent            = %s\n", c->user_agent);
    g_string_append_printf(s, "accept_language       = %s\n", c->accept_language);
    g_string_append_printf(s, "search_engine         = %s\n", c->search_engine);
    g_string_append_printf(s, "referer_policy        = %s\n", referer_policy_name(c->referer_policy));
    g_string_append_printf(s, "cookie_policy         = %s\n", cookie_policy_name(c->cookie_policy));
    g_string_append_printf(s, "html_parser           = %s\n", html_parser_name(c->html_parser));
    g_string_append_printf(s, "do_not_track          = %s\n", c->do_not_track ? "true" : "false");
    g_string_append_printf(s, "javascript_enabled    = %s\n", c->javascript_enabled ? "true" : "false");
    g_string_append_printf(s, "images_enabled        = %s\n", c->images_enabled ? "true" : "false");
    g_string_append_printf(s, "local_storage_enabled = %s\n", c->local_storage_enabled ? "true" : "false");
    g_string_append_printf(s, "cache_enabled         = %s\n", c->cache_enabled ? "true" : "false");
    g_string_append_printf(s, "cache_cap_mb          = %d\n", c->cache_cap_mb);
    g_string_append_printf(s, "default_font_size_px  = %d\n", c->default_font_size_px);
    g_string_append_printf(s, "js_eval_budget_ms     = %d\n", c->js_eval_budget_ms);
    g_string_append_printf(s, "js_memory_cap_mb      = %d\n", c->js_memory_cap_mb);
    return g_string_free(s, FALSE);
}
