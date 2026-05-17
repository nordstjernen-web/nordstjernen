/* Nordstjernen — flat key/value config loader.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "config.h"

#include <glib/gstdio.h>
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
    static const char *const truthy[] = { "true",  "yes", "on",  "1" };
    static const char *const falsy[]  = { "false", "no",  "off", "0" };
    if (!v || !*v) return dflt;
    for (gsize i = 0; i < G_N_ELEMENTS(truthy); i++)
        if (g_ascii_strcasecmp(v, truthy[i]) == 0) return TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(falsy); i++)
        if (g_ascii_strcasecmp(v, falsy[i]) == 0) return FALSE;
    return dflt;
}

static int
parse_int(const char *v, int dflt)
{
    if (!v || !*v) return dflt;
    char *end = NULL;
    gint64 n = g_ascii_strtoll(v, &end, 10);
    if (end == v) return dflt;
    if (n < (gint64)G_MININT) return G_MININT;
    if (n > (gint64)G_MAXINT) return G_MAXINT;
    return (int)n;
}

static nd_referer_policy
parse_referer_policy(const char *v, nd_referer_policy dflt)
{
    static const struct { const char *name; nd_referer_policy val; } map[] = {
        { "none",                            ND_REFERER_NO_REFERRER },
        { "no-referrer",                     ND_REFERER_NO_REFERRER },
        { "same-origin",                     ND_REFERER_SAME_ORIGIN },
        { "strict-origin-when-cross-origin", ND_REFERER_STRICT_ORIGIN_WHEN_CROSS },
        { "default",                         ND_REFERER_STRICT_ORIGIN_WHEN_CROSS },
        { "unsafe-url",                      ND_REFERER_UNSAFE_URL },
        { "full",                            ND_REFERER_UNSAFE_URL },
    };
    if (!v || !*v) return dflt;
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_ascii_strcasecmp(v, map[i].name) == 0) return map[i].val;
    return dflt;
}

static nd_cookie_policy
parse_cookie_policy(const char *v, nd_cookie_policy dflt)
{
    static const struct { const char *name; nd_cookie_policy val; } map[] = {
        { "always",           ND_COOKIE_ALWAYS },
        { "first-party",      ND_COOKIE_FIRST_PARTY },
        { "first-party-only", ND_COOKIE_FIRST_PARTY },
        { "never",            ND_COOKIE_NEVER },
        { "off",              ND_COOKIE_NEVER },
    };
    if (!v || !*v) return dflt;
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_ascii_strcasecmp(v, map[i].name) == 0) return map[i].val;
    return dflt;
}

static nd_color_scheme_pref
parse_color_scheme(const char *v, nd_color_scheme_pref dflt)
{
    static const struct { const char *name; nd_color_scheme_pref val; } map[] = {
        { "auto",   ND_COLOR_SCHEME_PREF_AUTO },
        { "system", ND_COLOR_SCHEME_PREF_AUTO },
        { "light",  ND_COLOR_SCHEME_PREF_LIGHT },
        { "dark",   ND_COLOR_SCHEME_PREF_DARK },
    };
    if (!v || !*v) return dflt;
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_ascii_strcasecmp(v, map[i].name) == 0) return map[i].val;
    return dflt;
}

static nd_reduced_motion_pref
parse_reduced_motion(const char *v, nd_reduced_motion_pref dflt)
{
    static const struct { const char *name; nd_reduced_motion_pref val; } map[] = {
        { "auto",          ND_REDUCED_MOTION_PREF_AUTO },
        { "system",        ND_REDUCED_MOTION_PREF_AUTO },
        { "no-preference", ND_REDUCED_MOTION_PREF_NO_PREFERENCE },
        { "off",           ND_REDUCED_MOTION_PREF_NO_PREFERENCE },
        { "reduce",        ND_REDUCED_MOTION_PREF_REDUCE },
        { "on",            ND_REDUCED_MOTION_PREF_REDUCE },
    };
    if (!v || !*v) return dflt;
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_ascii_strcasecmp(v, map[i].name) == 0) return map[i].val;
    return dflt;
}

typedef enum cfg_kind {
    CFG_STRING,
    CFG_BOOL,
    CFG_INT,
    CFG_REFERER,
    CFG_COOKIE,
    CFG_COLOR_SCHEME,
    CFG_REDUCED_MOTION,
} cfg_kind;

typedef struct cfg_field {
    const char *key;
    cfg_kind    kind;
    size_t      offset;
    const char *def_str;
    int         def_int;
} cfg_field;

#define FS(name, val)       { #name, CFG_STRING,       G_STRUCT_OFFSET(nd_config, name), val,   0 }
#define FB(name, val)       { #name, CFG_BOOL,         G_STRUCT_OFFSET(nd_config, name), NULL,  val }
#define FI(name, val)       { #name, CFG_INT,          G_STRUCT_OFFSET(nd_config, name), NULL,  val }
#define FE(name, kind, val) { #name, kind,             G_STRUCT_OFFSET(nd_config, name), NULL,  val }

static const cfg_field cfg_fields[] = {
    FS(home_url,              "about:start"),
    FS(user_agent,            ND_USER_AGENT),
    FS(accept_language,       ""),
    FS(search_engine,         "https://lite.duckduckgo.com/lite/?q=%s"),
    FS(http_proxy,            ""),
    FS(https_proxy,           ""),
    FS(no_proxy,              ""),
    FE(referer_policy,        CFG_REFERER,      ND_REFERER_STRICT_ORIGIN_WHEN_CROSS),
    FE(cookie_policy,         CFG_COOKIE,       ND_COOKIE_FIRST_PARTY),
    FE(color_scheme,          CFG_COLOR_SCHEME,    ND_COLOR_SCHEME_PREF_AUTO),
    FE(reduced_motion,        CFG_REDUCED_MOTION,  ND_REDUCED_MOTION_PREF_AUTO),
    FB(do_not_track,          TRUE),
    FB(javascript_enabled,    TRUE),
    FB(images_enabled,        TRUE),
    FB(local_storage_enabled, TRUE),
    FB(cache_enabled,         TRUE),
    FB(tls_allow_insecure_override, FALSE),
    FI(cache_cap_mb,          256),
    FI(default_font_size_px,  16),
    FI(js_eval_budget_ms,     5000),
    FI(js_memory_cap_mb,      128),
    FI(max_redirects,         ND_MAX_REDIRECTS),
    FI(window_width_px,       1280),
    FI(window_height_px,      800),
    FI(layout_viewport_px,    1000),
};

#undef FS
#undef FB
#undef FI
#undef FE

static void
apply_default(nd_config *c)
{
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const cfg_field *f = &cfg_fields[i];
        void *slot = (char *)c + f->offset;
        switch (f->kind) {
        case CFG_STRING:      set_string((char **)slot, f->def_str); break;
        case CFG_BOOL:        *(gboolean *)slot = (gboolean)f->def_int; break;
        case CFG_INT:         *(int *)slot      = f->def_int; break;
        case CFG_REFERER:     *(nd_referer_policy *)slot     = (nd_referer_policy)f->def_int; break;
        case CFG_COOKIE:      *(nd_cookie_policy *)slot      = (nd_cookie_policy)f->def_int; break;
        case CFG_COLOR_SCHEME:    *(nd_color_scheme_pref *)slot   = (nd_color_scheme_pref)f->def_int; break;
        case CFG_REDUCED_MOTION:  *(nd_reduced_motion_pref *)slot = (nd_reduced_motion_pref)f->def_int; break;
        }
    }
}

static void
apply_pair(nd_config *c, const char *key, const char *value)
{
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const cfg_field *f = &cfg_fields[i];
        if (strcmp(key, f->key) != 0) continue;
        void *slot = (char *)c + f->offset;
        switch (f->kind) {
        case CFG_STRING:       set_string((char **)slot, value); break;
        case CFG_BOOL:         *(gboolean *)slot = parse_bool(value, *(gboolean *)slot); break;
        case CFG_INT:          *(int *)slot      = parse_int(value, *(int *)slot); break;
        case CFG_REFERER:      *(nd_referer_policy *)slot     = parse_referer_policy(value, *(nd_referer_policy *)slot); break;
        case CFG_COOKIE:       *(nd_cookie_policy *)slot      = parse_cookie_policy(value, *(nd_cookie_policy *)slot); break;
        case CFG_COLOR_SCHEME:   *(nd_color_scheme_pref *)slot   = parse_color_scheme(value, *(nd_color_scheme_pref *)slot); break;
        case CFG_REDUCED_MOTION: *(nd_reduced_motion_pref *)slot = parse_reduced_motion(value, *(nd_reduced_motion_pref *)slot); break;
        }
        return;
    }
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

static const struct { const char *env; const char *key; } env_disable[] = {
    { "ND_NO_CACHE",         "cache_enabled"         },
    { "ND_NO_LOCAL_STORAGE", "local_storage_enabled" },
    { "ND_NO_JAVASCRIPT",    "javascript_enabled"    },
    { "ND_NO_JS",            "javascript_enabled"    },
    { "ND_NO_IMAGES",        "images_enabled"        },
};

static const struct { const char *env; const char *key; } env_value[] = {
    { "ND_HOME_URL",    "home_url"    },
    { "ND_USER_AGENT",  "user_agent"  },
    { "ND_HTTP_PROXY",  "http_proxy"  },
    { "ND_HTTPS_PROXY", "https_proxy" },
    { "ND_NO_PROXY",    "no_proxy"    },
};

static void
apply_env(nd_config *c)
{
    for (gsize i = 0; i < G_N_ELEMENTS(env_disable); i++)
        if (g_getenv(env_disable[i].env)) apply_pair(c, env_disable[i].key, "false");
    for (gsize i = 0; i < G_N_ELEMENTS(env_value); i++) {
        const char *v = g_getenv(env_value[i].env);
        if (v && *v) apply_pair(c, env_value[i].key, v);
    }
}

void
nd_config_init(void)
{
    apply_default(&g_cfg);
    g_cfg_path = g_build_filename(g_get_user_config_dir(),
                                  ND_APP_DIR_NAME, "nordstjernen.conf",
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
    g_free(g_cfg.http_proxy);
    g_free(g_cfg.https_proxy);
    g_free(g_cfg.no_proxy);
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_clear_pointer(&g_cfg_path, g_free);
}

const nd_config *
nd_config_get(void)
{
    return &g_cfg;
}

nd_config *
nd_config_mut(void)
{
    return &g_cfg;
}

const char *
nd_config_path(void)
{
    return g_cfg_path;
}

static const char *const referer_policy_names[] = {
    [ND_REFERER_NO_REFERRER]              = "none",
    [ND_REFERER_SAME_ORIGIN]              = "same-origin",
    [ND_REFERER_STRICT_ORIGIN_WHEN_CROSS] = "strict-origin-when-cross-origin",
    [ND_REFERER_UNSAFE_URL]               = "unsafe-url",
};

static const char *const cookie_policy_names[] = {
    [ND_COOKIE_ALWAYS]      = "always",
    [ND_COOKIE_FIRST_PARTY] = "first-party",
    [ND_COOKIE_NEVER]       = "never",
};

static const char *const color_scheme_names[] = {
    [ND_COLOR_SCHEME_PREF_AUTO]  = "auto",
    [ND_COLOR_SCHEME_PREF_LIGHT] = "light",
    [ND_COLOR_SCHEME_PREF_DARK]  = "dark",
};

static const char *const reduced_motion_names[] = {
    [ND_REDUCED_MOTION_PREF_AUTO]          = "auto",
    [ND_REDUCED_MOTION_PREF_NO_PREFERENCE] = "no-preference",
    [ND_REDUCED_MOTION_PREF_REDUCE]        = "reduce",
};

static const char *
referer_policy_name(nd_referer_policy p)
{
    if ((unsigned)p >= G_N_ELEMENTS(referer_policy_names) || !referer_policy_names[p])
        return "strict-origin-when-cross-origin";
    return referer_policy_names[p];
}

static const char *
cookie_policy_name(nd_cookie_policy p)
{
    if ((unsigned)p >= G_N_ELEMENTS(cookie_policy_names) || !cookie_policy_names[p])
        return "first-party";
    return cookie_policy_names[p];
}

static const char *
color_scheme_name(nd_color_scheme_pref p)
{
    if ((unsigned)p >= G_N_ELEMENTS(color_scheme_names) || !color_scheme_names[p])
        return "auto";
    return color_scheme_names[p];
}

static const char *
reduced_motion_name(nd_reduced_motion_pref p)
{
    if ((unsigned)p >= G_N_ELEMENTS(reduced_motion_names) || !reduced_motion_names[p])
        return "auto";
    return reduced_motion_names[p];
}

gboolean
nd_config_save(GError **error)
{
    if (!g_cfg_path) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                    "config path not initialized");
        return FALSE;
    }
    char *dir = g_path_get_dirname(g_cfg_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    const nd_config *c = &g_cfg;
    GString *s = g_string_new(NULL);
    g_string_append(s, "# nordstjernen configuration\n");
    for (gsize i = 0; i < G_N_ELEMENTS(cfg_fields); i++) {
        const cfg_field *f = &cfg_fields[i];
        const void *slot = (const char *)c + f->offset;
        switch (f->kind) {
        case CFG_STRING: {
            const char *v = *(const char *const *)slot;
            g_string_append_printf(s, "%s = %s\n", f->key, v ? v : "");
            break;
        }
        case CFG_BOOL:
            g_string_append_printf(s, "%s = %s\n", f->key,
                                   *(const gboolean *)slot ? "true" : "false");
            break;
        case CFG_INT:
            g_string_append_printf(s, "%s = %d\n", f->key,
                                   *(const int *)slot);
            break;
        case CFG_REFERER:
            g_string_append_printf(s, "%s = %s\n", f->key,
                referer_policy_name(*(const nd_referer_policy *)slot));
            break;
        case CFG_COOKIE:
            g_string_append_printf(s, "%s = %s\n", f->key,
                cookie_policy_name(*(const nd_cookie_policy *)slot));
            break;
        case CFG_COLOR_SCHEME:
            g_string_append_printf(s, "%s = %s\n", f->key,
                color_scheme_name(*(const nd_color_scheme_pref *)slot));
            break;
        case CFG_REDUCED_MOTION:
            g_string_append_printf(s, "%s = %s\n", f->key,
                reduced_motion_name(*(const nd_reduced_motion_pref *)slot));
            break;
        }
    }
    gboolean ok = g_file_set_contents(g_cfg_path, s->str, (gssize)s->len, error);
    g_string_free(s, TRUE);
    return ok;
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
    if (c->accept_language && *c->accept_language)
        g_string_append_printf(s, "accept_language       = %s\n",
                               c->accept_language);
    else
        g_string_append_printf(s, "accept_language       = (auto: %s)\n",
                               nd_net_default_accept_language());
    g_string_append_printf(s, "search_engine         = %s\n", c->search_engine);
    {
        char *hp  = nd_net_proxy_mask(c->http_proxy);
        char *hsp = nd_net_proxy_mask(c->https_proxy);
        g_string_append_printf(s, "http_proxy            = %s\n",
                               hp  && *hp  ? hp  : "(none)");
        g_string_append_printf(s, "https_proxy           = %s\n",
                               hsp && *hsp ? hsp : "(none)");
        g_string_append_printf(s, "no_proxy              = %s\n",
                               c->no_proxy && *c->no_proxy ? c->no_proxy : "(none)");
        g_free(hp);
        g_free(hsp);
    }
    g_string_append_printf(s, "referer_policy        = %s\n", referer_policy_name(c->referer_policy));
    g_string_append_printf(s, "cookie_policy         = %s\n", cookie_policy_name(c->cookie_policy));
    g_string_append_printf(s, "color_scheme          = %s\n", color_scheme_name(c->color_scheme));
    g_string_append_printf(s, "reduced_motion        = %s\n", reduced_motion_name(c->reduced_motion));
    g_string_append_printf(s, "do_not_track          = %s\n", c->do_not_track ? "true" : "false");
    g_string_append_printf(s, "javascript_enabled    = %s\n", c->javascript_enabled ? "true" : "false");
    g_string_append_printf(s, "images_enabled        = %s\n", c->images_enabled ? "true" : "false");
    g_string_append_printf(s, "local_storage_enabled = %s\n", c->local_storage_enabled ? "true" : "false");
    g_string_append_printf(s, "cache_enabled         = %s\n", c->cache_enabled ? "true" : "false");
    g_string_append_printf(s, "tls_allow_insecure_override = %s\n", c->tls_allow_insecure_override ? "true" : "false");
    g_string_append_printf(s, "cache_cap_mb          = %d\n", c->cache_cap_mb);
    g_string_append_printf(s, "default_font_size_px  = %d\n", c->default_font_size_px);
    g_string_append_printf(s, "js_eval_budget_ms     = %d\n", c->js_eval_budget_ms);
    g_string_append_printf(s, "js_memory_cap_mb      = %d\n", c->js_memory_cap_mb);
    g_string_append_printf(s, "max_redirects         = %d\n", c->max_redirects);
    g_string_append_printf(s, "window_width_px       = %d\n", c->window_width_px);
    g_string_append_printf(s, "window_height_px      = %d\n", c->window_height_px);
    g_string_append_printf(s, "layout_viewport_px    = %d\n", c->layout_viewport_px);
    return g_string_free(s, FALSE);
}
