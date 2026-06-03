/* Nordstjernen - force the mobile variant of select sites. */

#include "mobile.h"

#include "net.h"

#include <string.h>

static const char k_mobile_ua[] =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 "
    "Mobile/15E148 Safari/604.1";

const char *
nd_mobile_user_agent(void)
{
    return k_mobile_ua;
}

static gboolean
host_eq(const char *host, const char *want)
{
    return host && g_ascii_strcasecmp(host, want) == 0;
}

static gboolean
facebook_host(const char *host)
{
    return host_eq(host, "facebook.com")        ||
           host_eq(host, "www.facebook.com")    ||
           host_eq(host, "web.facebook.com")    ||
           host_eq(host, "mobile.facebook.com") ||
           host_eq(host, "m.facebook.com");
}

static gboolean
youtube_site_host(const char *host)
{
    return host_eq(host, "youtube.com")              ||
           host_eq(host, "www.youtube.com")          ||
           host_eq(host, "m.youtube.com")            ||
           host_eq(host, "music.youtube.com")        ||
           host_eq(host, "youtube-nocookie.com")     ||
           host_eq(host, "www.youtube-nocookie.com") ||
           host_eq(host, "youtu.be");
}

static gboolean
youtube_media_host(const char *host)
{
    if (!host) return FALSE;
    return g_str_has_suffix(host, ".googlevideo.com") ||
           g_str_has_suffix(host, ".ytimg.com")       ||
           g_str_has_suffix(host, ".ggpht.com");
}

static gboolean
reddit_site_host(const char *host)
{
    return host_eq(host, "reddit.com")     ||
           host_eq(host, "www.reddit.com") ||
           host_eq(host, "m.reddit.com")   ||
           host_eq(host, "new.reddit.com") ||
           host_eq(host, "old.reddit.com");
}

gboolean
nd_mobile_force_host(const char *host)
{
    return facebook_host(host)     ||
           youtube_site_host(host) ||
           youtube_media_host(host);
}

static const char *
mobile_host_for(const char *host)
{
    if (facebook_host(host) && !host_eq(host, "m.facebook.com"))
        return "m.facebook.com";
    if (host_eq(host, "youtube.com") || host_eq(host, "www.youtube.com") ||
        host_eq(host, "music.youtube.com"))
        return "m.youtube.com";
    if (reddit_site_host(host) && !host_eq(host, "old.reddit.com"))
        return "old.reddit.com";
    return NULL;
}

char *
nd_mobile_rewrite_url(const char *url)
{
    if (!url) return NULL;
    g_autoptr(nd_url_parts) p = nd_url_parts_new(url);
    if (!p || !p->hostname) return NULL;
    const char *newhost = mobile_host_for(p->hostname);
    if (!newhost) return NULL;

    GString *s = g_string_new(p->protocol && *p->protocol ? p->protocol : "https:");
    g_string_append(s, "//");
    if (p->username && *p->username) {
        g_string_append(s, p->username);
        if (p->password && *p->password) {
            g_string_append_c(s, ':');
            g_string_append(s, p->password);
        }
        g_string_append_c(s, '@');
    }
    g_string_append(s, newhost);
    if (p->port && *p->port) {
        g_string_append_c(s, ':');
        g_string_append(s, p->port);
    }
    g_string_append(s, p->pathname && *p->pathname ? p->pathname : "/");
    if (p->search && *p->search) g_string_append(s, p->search);
    if (p->hash && *p->hash) g_string_append(s, p->hash);
    return g_string_free(s, FALSE);
}
