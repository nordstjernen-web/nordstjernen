/* Nordstjernen — runtime config (flat key/value file). */

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

typedef enum nd_html_parser_choice {
    ND_HTML_PARSER_PRIMARY = 0,
    ND_HTML_PARSER_GUMBO,
} nd_html_parser_choice;

typedef struct nd_config {
    char  *home_url;
    char  *user_agent;
    char  *accept_language;
    char  *search_engine;
    nd_referer_policy referer_policy;
    nd_cookie_policy  cookie_policy;
    nd_html_parser_choice html_parser;
    gboolean do_not_track;
    gboolean javascript_enabled;
    gboolean images_enabled;
    gboolean local_storage_enabled;
    gboolean cache_enabled;
    int      cache_cap_mb;
    int      default_font_size_px;
    int      js_eval_budget_ms;
    int      js_memory_cap_mb;
    int      window_width_px;
    int      window_height_px;
    int      layout_viewport_px;
} nd_config;

void             nd_config_init(void);
void             nd_config_shutdown(void);
const nd_config *nd_config_get(void);
char            *nd_config_dump(void);
const char      *nd_config_path(void);

G_END_DECLS

#endif
