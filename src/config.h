/* Nordstjernen — runtime config (flat key/value file).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#ifndef NS_CONFIG_H
#define NS_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum ns_referer_policy {
    NS_REFERER_NO_REFERRER = 0,
    NS_REFERER_SAME_ORIGIN,
    NS_REFERER_STRICT_ORIGIN_WHEN_CROSS,
    NS_REFERER_UNSAFE_URL,
} ns_referer_policy;

typedef enum ns_cookie_policy {
    NS_COOKIE_ALWAYS = 0,
    NS_COOKIE_FIRST_PARTY,
    NS_COOKIE_NEVER,
} ns_cookie_policy;

typedef enum ns_color_scheme_pref {
    NS_COLOR_SCHEME_PREF_AUTO = 0,
    NS_COLOR_SCHEME_PREF_LIGHT,
    NS_COLOR_SCHEME_PREF_DARK,
} ns_color_scheme_pref;

typedef enum ns_reduced_motion_pref {
    NS_REDUCED_MOTION_PREF_AUTO = 0,
    NS_REDUCED_MOTION_PREF_NO_PREFERENCE,
    NS_REDUCED_MOTION_PREF_REDUCE,
} ns_reduced_motion_pref;

typedef struct ns_config {
    char  *home_url;
    char  *user_agent;
    char  *compat_mode;
    char  *accept_language;
    char  *search_engine;
    char  *ai_model_mirror;
    char  *http_proxy;
    char  *https_proxy;
    char  *no_proxy;
    char  *doh_url;
    char  *gsk_renderer;
    ns_referer_policy      referer_policy;
    ns_cookie_policy       cookie_policy;
    ns_color_scheme_pref   color_scheme;
    ns_reduced_motion_pref reduced_motion;
    gboolean do_not_track;
    gboolean global_privacy_control;
    gboolean strip_tracking_params;
    gboolean https_first;
    gboolean harden_allocator;
    gboolean speculative_preload;
    gboolean async_image_decode;
    gboolean images_enabled;
    gboolean webgl_enabled;
    gboolean camera_enabled;
    gboolean microphone_enabled;
    gboolean local_storage_enabled;
    gboolean cache_enabled;
    gboolean tls_allow_insecure_override;
    gboolean watchdog_enabled;
    gboolean private_mode;
    int      cache_cap_mb;
    int      js_eval_budget_ms;
    int      js_memory_cap_mb;
    int      max_redirects;
    int      window_width_px;
    int      window_height_px;
    int      layout_viewport_px;
} ns_config;

void             ns_config_init(void);
void             ns_config_shutdown(void);
const ns_config *ns_config_get(void);
ns_config       *ns_config_mut(void);
char            *ns_config_dump(void);
gboolean         ns_config_save(GError **error);

void             ns_config_lock(void);
void             ns_config_unlock(void);

#define NS_APP_DIR_NAME "nordstjernen"

G_END_DECLS

#endif
