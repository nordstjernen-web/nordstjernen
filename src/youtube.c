/* Nordstjernen — YouTube watch page interception.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#include "youtube.h"

#include <string.h>

#include "net.h"

static const char k_browser_ua[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

const char *
nd_youtube_browser_user_agent(void)
{
    return k_browser_ua;
}

static gboolean
host_is_youtube_site(const char *host)
{
    if (!host) return FALSE;
    return g_ascii_strcasecmp(host, "www.youtube.com") == 0 ||
           g_ascii_strcasecmp(host, "youtube.com")     == 0 ||
           g_ascii_strcasecmp(host, "m.youtube.com")   == 0 ||
           g_ascii_strcasecmp(host, "youtu.be")        == 0;
}

static gboolean
host_is_youtube_media(const char *host)
{
    if (!host) return FALSE;
    if (g_str_has_suffix(host, ".googlevideo.com")) return TRUE;
    if (g_str_has_suffix(host, ".ytimg.com")) return TRUE;
    if (g_str_has_suffix(host, ".ggpht.com")) return TRUE;
    return FALSE;
}

gboolean
nd_youtube_host_needs_browser_ua(const char *host)
{
    return host_is_youtube_site(host) || host_is_youtube_media(host);
}

gboolean
nd_youtube_is_watch_url(const char *url)
{
    if (!url) return FALSE;
    char *host = nd_url_host_from(url);
    gboolean is_yt = host_is_youtube_site(host);
    g_free(host);
    if (!is_yt) return FALSE;
    const char *scheme = strstr(url, "://");
    if (!scheme) return FALSE;
    const char *path = strchr(scheme + 3, '/');
    if (!path) return FALSE;
    return g_str_has_prefix(path, "/watch?") ||
           g_str_has_prefix(path, "/watch/") ||
           strcmp(path, "/watch") == 0;
}

static const char *
skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static const char *
skip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"') return p;
    p++;
    while (p < end) {
        if (*p == '\\') {
            p++;
            if (p < end) p++;
        } else if (*p == '"') {
            return p + 1;
        } else {
            p++;
        }
    }
    return end;
}

static const char *
skip_value(const char *p, const char *end)
{
    p = skip_ws(p, end);
    if (p >= end) return p;
    if (*p == '"') return skip_string(p, end);
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (p < end && depth > 0) {
            if (*p == '"') p = skip_string(p, end);
            else if (*p == open)  { depth++; p++; }
            else if (*p == close) { depth--; p++; }
            else p++;
        }
        return p;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    return p;
}

static char *
parse_json_string(const char *p, const char *end, const char **out_after)
{
    if (p >= end || *p != '"') return NULL;
    p++;
    GString *s = g_string_new(NULL);
    while (p < end && *p != '"') {
        if (*p == '\\') {
            p++;
            if (p >= end) break;
            switch (*p) {
            case '"':  g_string_append_c(s, '"');  p++; break;
            case '\\': g_string_append_c(s, '\\'); p++; break;
            case '/':  g_string_append_c(s, '/');  p++; break;
            case 'b':  g_string_append_c(s, '\b'); p++; break;
            case 'f':  g_string_append_c(s, '\f'); p++; break;
            case 'n':  g_string_append_c(s, '\n'); p++; break;
            case 'r':  g_string_append_c(s, '\r'); p++; break;
            case 't':  g_string_append_c(s, '\t'); p++; break;
            case 'u':
                p++;
                if (p + 4 > end) goto done;
                {
                    char hex[5] = { p[0], p[1], p[2], p[3], 0 };
                    p += 4;
                    guint cp = (guint)g_ascii_strtoull(hex, NULL, 16);
                    gchar buf[8];
                    gint n = g_unichar_to_utf8(cp, buf);
                    if (n > 0) g_string_append_len(s, buf, n);
                }
                break;
            default:
                g_string_append_c(s, *p);
                p++;
                break;
            }
        } else {
            g_string_append_c(s, *p);
            p++;
        }
    }
done:
    if (p < end && *p == '"') p++;
    if (out_after) *out_after = p;
    return g_string_free(s, FALSE);
}

static const char *
find_key_in_object(const char *obj, const char *end, const char *key)
{
    if (!obj || obj >= end || *obj != '{') return NULL;
    gsize keylen = strlen(key);
    const char *p = obj + 1;
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end) return NULL;
        if (*p == '}') return NULL;
        if (*p != '"') return NULL;
        const char *key_after = skip_string(p, end);
        if (key_after <= p + 1) return NULL;
        gsize klen = (gsize)((key_after - 1) - (p + 1));
        gboolean match = (klen == keylen &&
                          strncmp(p + 1, key, keylen) == 0);
        p = skip_ws(key_after, end);
        if (p >= end || *p != ':') return NULL;
        p++;
        p = skip_ws(p, end);
        if (match) return p;
        p = skip_value(p, end);
        p = skip_ws(p, end);
        if (p < end && *p == ',') p++;
    }
    return NULL;
}

static char *
get_string_field(const char *obj, const char *end, const char *key)
{
    const char *v = find_key_in_object(obj, end, key);
    if (!v || v >= end || *v != '"') return NULL;
    return parse_json_string(v, end, NULL);
}

static gint64
get_int_field(const char *obj, const char *end, const char *key, gint64 dflt)
{
    const char *v = find_key_in_object(obj, end, key);
    if (!v || v >= end) return dflt;
    if (*v == '"') {
        char *s = parse_json_string(v, end, NULL);
        gint64 n = s ? g_ascii_strtoll(s, NULL, 10) : dflt;
        g_free(s);
        return n;
    }
    return g_ascii_strtoll(v, NULL, 10);
}

static const char *
find_player_response(const char *body, gsize len, const char **out_end)
{
    const char *needles[] = {
        "var ytInitialPlayerResponse =",
        "ytInitialPlayerResponse =",
        "\"ytInitialPlayerResponse\":",
        NULL
    };
    const char *end = body + len;
    for (int i = 0; needles[i]; i++) {
        const char *hit = g_strstr_len(body, (gssize)len, needles[i]);
        if (!hit) continue;
        const char *p = hit + strlen(needles[i]);
        p = skip_ws(p, end);
        if (p >= end || *p != '{') continue;
        const char *finish = skip_value(p, end);
        if (finish <= p) continue;
        if (out_end) *out_end = finish;
        return p;
    }
    return NULL;
}

typedef struct yt_format {
    char *mime_type;
    char *url;
    char *quality_label;
    int   width;
    int   height;
    gint64 bitrate;
} yt_format;

static void
yt_format_clear(yt_format *f)
{
    if (!f) return;
    g_free(f->mime_type);
    g_free(f->url);
    g_free(f->quality_label);
    memset(f, 0, sizeof(*f));
}

static gboolean
mime_is_webm_vp9(const char *m)
{
    if (!m) return FALSE;
    return g_ascii_strncasecmp(m, "video/webm", 10) == 0 &&
           (strstr(m, "vp9") != NULL || strstr(m, "vp09") != NULL);
}

static gboolean
mime_is_webm_vp8(const char *m)
{
    if (!m) return FALSE;
    return g_ascii_strncasecmp(m, "video/webm", 10) == 0 &&
           (strstr(m, "vp8") != NULL || strstr(m, "vp08") != NULL);
}

static int
score_format(const yt_format *f)
{
    if (!f || !f->url || !f->mime_type) return -1;
    int score;
    if (mime_is_webm_vp9(f->mime_type)) score = 1000;
    else if (mime_is_webm_vp8(f->mime_type)) score = 500;
    else return -1;
    int h = f->height > 0 ? f->height : 360;
    int dist = h - 480;
    if (dist < 0) dist = -dist;
    score -= dist;
    return score;
}

typedef struct pick_ctx {
    yt_format best;
    int       best_score;
} pick_ctx;

static void
consider_format(pick_ctx *ctx, const char *item, const char *item_end)
{
    yt_format f = {0};
    if (item >= item_end || *item != '{') return;
    f.mime_type     = get_string_field(item, item_end, "mimeType");
    f.url           = get_string_field(item, item_end, "url");
    f.quality_label = get_string_field(item, item_end, "qualityLabel");
    f.width         = (int)get_int_field(item, item_end, "width",  0);
    f.height        = (int)get_int_field(item, item_end, "height", 0);
    f.bitrate       = get_int_field(item, item_end, "bitrate", 0);

    int s = score_format(&f);
    if (s > ctx->best_score) {
        yt_format_clear(&ctx->best);
        ctx->best = f;
        ctx->best_score = s;
        return;
    }
    yt_format_clear(&f);
}

static void
walk_formats(const char *array_start, const char *end, pick_ctx *ctx)
{
    if (!array_start || array_start >= end || *array_start != '[') return;
    const char *p = array_start + 1;
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') return;
        const char *item = p;
        const char *after = skip_value(p, end);
        if (after <= item) return;
        consider_format(ctx, item, after);
        p = skip_ws(after, end);
        if (p < end && *p == ',') p++;
    }
}

static char *
escape_html(const char *s)
{
    if (!s) return g_strdup("");
    GString *out = g_string_new(NULL);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '&': g_string_append(out, "&amp;");  break;
        case '"': g_string_append(out, "&quot;"); break;
        case '<': g_string_append(out, "&lt;");   break;
        case '>': g_string_append(out, "&gt;");   break;
        default:  g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

static char *
pick_best_thumbnail(const char *details_obj, const char *end)
{
    const char *thumb = find_key_in_object(details_obj, end, "thumbnail");
    if (!thumb) return NULL;
    const char *arr = find_key_in_object(thumb, end, "thumbnails");
    if (!arr || *arr != '[') return NULL;
    const char *p = arr + 1;
    char *best = NULL;
    int best_w = 0;
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') break;
        const char *item = p;
        const char *after = skip_value(p, end);
        if (after <= item) break;
        if (*item == '{') {
            char *u = get_string_field(item, after, "url");
            int w = (int)get_int_field(item, after, "width", 0);
            if (u && w >= best_w) {
                g_free(best);
                best = u;
                best_w = w;
            } else {
                g_free(u);
            }
        }
        p = skip_ws(after, end);
        if (p < end && *p == ',') p++;
    }
    return best;
}

static char *
build_error_page(const char *original_url, const char *title,
                 const char *poster_url, const char *reason)
{
    char *t  = escape_html(title  ? title  : "YouTube video");
    char *u  = escape_html(original_url ? original_url : "");
    char *r  = escape_html(reason ? reason : "Playback unavailable.");
    char *poster_img = NULL;
    if (poster_url && *poster_url) {
        char *p = escape_html(poster_url);
        poster_img = g_strdup_printf("<img src=\"%s\" alt=\"\">", p);
        g_free(p);
    }
    char *page = g_strdup_printf(
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>%s</title>"
        "<style>"
        "body{margin:0;background:#101010;color:#eee;"
        "font:14px system-ui,-apple-system,sans-serif;}"
        "header{padding:10px 16px;background:#181818;}"
        "header h1{margin:0;font-size:16px;}"
        "main{padding:14px 16px;}"
        "img{display:block;max-width:100%%;height:auto;}"
        "p{color:#ccc;line-height:1.45;}"
        "a{color:#79b8ff;}"
        "</style></head><body>"
        "<header><h1>%s</h1></header>"
        "<main>%s<p>%s</p>"
        "<p>Original URL: <a href=\"%s\">%s</a></p></main>"
        "</body></html>",
        t, t,
        poster_img ? poster_img : "",
        r, u, u);
    g_free(t);
    g_free(u);
    g_free(r);
    g_free(poster_img);
    return page;
}

char *
nd_youtube_render_watch_page(const char *url, const char *body, gsize body_len)
{
    if (!nd_youtube_is_watch_url(url) || !body || body_len == 0) return NULL;

    const char *resp_end = NULL;
    const char *resp = find_player_response(body, body_len, &resp_end);
    if (!resp || !resp_end) {
        return build_error_page(url, "YouTube video", NULL,
            "Could not locate ytInitialPlayerResponse in the page. "
            "YouTube probably served a non-player page (consent / "
            "interstitial / older client). Nordstjernen does not "
            "currently follow consent redirects.");
    }

    char *title = NULL;
    char *poster_url = NULL;
    const char *details = find_key_in_object(resp, resp_end, "videoDetails");
    if (details && *details == '{') {
        title = get_string_field(details, resp_end, "title");
        poster_url = pick_best_thumbnail(details, resp_end);
    }

    pick_ctx ctx = { .best_score = -1 };
    const char *sd = find_key_in_object(resp, resp_end, "streamingData");
    if (sd && *sd == '{') {
        const char *adaptive = find_key_in_object(sd, resp_end, "adaptiveFormats");
        if (adaptive) walk_formats(adaptive, resp_end, &ctx);
        const char *progressive = find_key_in_object(sd, resp_end, "formats");
        if (progressive) walk_formats(progressive, resp_end, &ctx);
    }

    char *page = NULL;
    if (ctx.best.url && ctx.best.mime_type) {
        char *t   = escape_html(title ? title : "YouTube video");
        char *src = escape_html(ctx.best.url);
        char *ps  = escape_html(poster_url ? poster_url : "");
        int  w    = ctx.best.width  > 0 ? ctx.best.width  : 640;
        int  h    = ctx.best.height > 0 ? ctx.best.height : 360;
        char *ql  = escape_html(ctx.best.quality_label ? ctx.best.quality_label : "");
        char *mt  = escape_html(ctx.best.mime_type);
        page = g_strdup_printf(
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>%s</title>"
            "<style>"
            "body{margin:0;background:#000;color:#eee;"
            "font:14px system-ui,-apple-system,sans-serif;}"
            "header{padding:10px 16px;background:#181818;}"
            "header h1{margin:0;font-size:16px;}"
            "main{display:block;}"
            "video{display:block;margin:0 auto;max-width:100%%;background:#000;}"
            "footer{padding:8px 16px;color:#888;font-size:12px;background:#101010;}"
            "</style></head><body>"
            "<header><h1>%s</h1></header>"
            "<main><video src=\"%s\" poster=\"%s\" width=\"%d\" height=\"%d\"></video></main>"
            "<footer>Stream: %s &middot; %s &middot; native VP9/WebM</footer>"
            "</body></html>",
            t, t, src, ps, w, h, mt, ql);
        g_free(t);
        g_free(src);
        g_free(ps);
        g_free(ql);
        g_free(mt);
    } else {
        page = build_error_page(url, title, poster_url,
            "Could not find a WebM/VP9 stream with a direct URL. "
            "This video most likely requires player-script signature "
            "deciphering (only available to JavaScript-heavy browsers), "
            "or it is encrypted with a per-request token. "
            "Nordstjernen plays only progressive WebM/VP9 streams.");
    }

    yt_format_clear(&ctx.best);
    g_free(title);
    g_free(poster_url);
    return page;
}
