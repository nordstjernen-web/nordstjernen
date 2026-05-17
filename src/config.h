/* Nordstjernen — runtime config (flat key/value file).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_CONFIG_H
#define ND_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum nd_referer_policy {
    ND_REFERER_NO_REFERRER = 0,
    ND_REFERER_SAME_ORIGIN,
    ND_REFERER_STRICT_ORIGIN_WHEN_CROSS,
    ND_REFERER_UNSAFE_URL,
} nd_referer_policy;

typedef enum nd_cookie_policy {
    ND_COOKIE_ALWAYS = 0,
    ND_COOKIE_FIRST_PARTY,
    ND_COOKIE_NEVER,
} nd_cookie_policy;

typedef enum nd_color_scheme_pref {
    ND_COLOR_SCHEME_PREF_AUTO = 0,
    ND_COLOR_SCHEME_PREF_LIGHT,
    ND_COLOR_SCHEME_PREF_DARK,
} nd_color_scheme_pref;

typedef enum nd_reduced_motion_pref {
    ND_REDUCED_MOTION_PREF_AUTO = 0,
    ND_REDUCED_MOTION_PREF_NO_PREFERENCE,
    ND_REDUCED_MOTION_PREF_REDUCE,
} nd_reduced_motion_pref;

typedef struct nd_config {
    char  *home_url;
    char  *user_agent;
    char  *accept_language;
    char  *search_engine;
    char  *http_proxy;
    char  *https_proxy;
    char  *no_proxy;
    nd_referer_policy      referer_policy;
    nd_cookie_policy       cookie_policy;
    nd_color_scheme_pref   color_scheme;
    nd_reduced_motion_pref reduced_motion;
    gboolean do_not_track;
    gboolean images_enabled;
    gboolean local_storage_enabled;
    gboolean cache_enabled;
    gboolean tls_allow_insecure_override;
    int      cache_cap_mb;
    int      default_font_size_px;
    int      js_eval_budget_ms;
    int      js_memory_cap_mb;
    int      max_redirects;
    int      window_width_px;
    int      window_height_px;
    int      layout_viewport_px;
} nd_config;

void             nd_config_init(void);
void             nd_config_shutdown(void);
const nd_config *nd_config_get(void);
nd_config       *nd_config_mut(void);
char            *nd_config_dump(void);
const char      *nd_config_path(void);
gboolean         nd_config_save(GError **error);

#define ND_APP_DIR_NAME "nordstjernen"

G_END_DECLS

#endif
