/* Nordstjernen — renderer request dispatch over the HTTP/JSON IPC protocol,
   shared by nordstjernen-renderer and the single-process in-process host. */

#define _GNU_SOURCE
#include "renderer_serve.h"
#include "libnordstjernen.h"
#include "net.h"
#include "image.h"
#include "texture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_BFCACHE_MAX 4

struct ns_renderer_session {
    int            ctrl_w;
    unsigned char *fb;
    int            max_w;
    int            max_h;
    int            shm_mode;
    ns_browser    *cur;
    ns_browser    *bf[NS_BFCACHE_MAX];
    int            bf_n;
    int            tick_budget_ms;
    int            frame_valid;
    long           frame_sx;
    long           frame_sy;
    int            frame_w;
    int            frame_h;
    double         frame_scale;
    char          *post_url;
    char          *post_body;
    size_t         post_len;
    char          *post_ct;
};

static void
session_bfcache_park_or_close(ns_renderer_session *s, ns_browser *b)
{
    if (!b)
        return;
    if (!ns_browser_bfcache_eligible(b)) {
        ns_browser_close(b);
        return;
    }
    ns_browser_bfcache_park(b);
    if (s->bf_n >= NS_BFCACHE_MAX) {
        ns_browser_close(s->bf[0]);
        for (int i = 1; i < s->bf_n; i++)
            s->bf[i - 1] = s->bf[i];
        s->bf_n--;
    }
    s->bf[s->bf_n++] = b;
}

static ns_browser *
session_bfcache_take(ns_renderer_session *s, const char *url)
{
    if (!url)
        return NULL;
    for (int i = s->bf_n - 1; i >= 0; i--) {
        char *u = ns_browser_url(s->bf[i]);
        int match = u && strcmp(u, url) == 0;
        free(u);
        if (!match)
            continue;
        ns_browser *b = s->bf[i];
        for (int j = i + 1; j < s->bf_n; j++)
            s->bf[j - 1] = s->bf[j];
        s->bf_n--;
        return b;
    }
    return NULL;
}

static void
session_bfcache_clear(ns_renderer_session *s)
{
    for (int i = 0; i < s->bf_n; i++)
        ns_browser_close(s->bf[i]);
    s->bf_n = 0;
}

static void
session_stash_post(ns_renderer_session *s, const char *href)
{
    if (!s || !s->cur)
        return;
    size_t len = 0;
    char *ct = NULL;
    char *pb = ns_browser_take_post(s->cur, &len, &ct);
    if (!pb)
        return;
    free(s->post_url);
    free(s->post_body);
    free(s->post_ct);
    s->post_url = (href && *href) ? strdup(href) : NULL;
    s->post_body = pb;
    s->post_len = len;
    s->post_ct = ct;
}

static void
session_clear_post(ns_renderer_session *s)
{
    if (!s)
        return;
    free(s->post_url);
    free(s->post_body);
    free(s->post_ct);
    s->post_url = NULL;
    s->post_body = NULL;
    s->post_ct = NULL;
    s->post_len = 0;
}

static int
clamp(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static void
reply_str(int fd, const char *key, const char *val)
{
    char *e = json_escape(val ? val : "");
    char *json = NULL;
    int n = asprintf(&json, "{\"%s\":\"%s\"}", key, e ? e : "");
    if (n > 0)
        http_write_response(fd, 200, "application/json", NULL, json,
                            (size_t)n);
    free(json);
    free(e);
}

static void
reply_str2(int fd, const char *key1, const char *val1,
           const char *key2, const char *val2)
{
    char *escaped1 = json_escape(val1 ? val1 : "");
    char *escaped2 = json_escape(val2 ? val2 : "");
    char *json = NULL;
    int n = asprintf(&json, "{\"%s\":\"%s\",\"%s\":\"%s\"}",
                     key1, escaped1 ? escaped1 : "",
                     key2, escaped2 ? escaped2 : "");
    if (n > 0)
        http_write_response(fd, 200, "application/json", NULL, json,
                            (size_t)n);
    free(json);
    free(escaped1);
    free(escaped2);
}

static void
reply_href_changed(int fd, const char *href, int changed)
{
    char *e = json_escape(href ? href : "");
    char *json = NULL;
    int n = asprintf(&json, "{\"href\":\"%s\",\"changed\":%d}",
                     e ? e : "", changed ? 1 : 0);
    if (n > 0)
        http_write_response(fd, 200, "application/json", NULL, json,
                            (size_t)n);
    free(json);
    free(e);
}

static unsigned char *
favicon_bgra(ns_browser *b, int *out_w, int *out_h)
{
    char *url = ns_browser_favicon_url(b);
    if (!url || !*url) {
        free(url);
        return NULL;
    }
    ns_response *resp = ns_net_fetch_blocking(url, NULL, NULL);
    free(url);
    if (!resp || resp->status >= 400 || !resp->body || resp->body->len == 0) {
        if (resp)
            ns_response_free(resp);
        return NULL;
    }
    int w = 0, h = 0;
    ns_texture *tex =
        ns_image_decode_bytes(resp->body->data, resp->body->len, &w, &h);
    ns_response_free(resp);
    if (!tex)
        return NULL;
    if (w < 1 || h < 1 || w > 512 || h > 512) {
        ns_texture_unref(tex);
        return NULL;
    }
    gsize stride = (gsize)w * 4u;
    unsigned char *px = malloc(stride * (gsize)h);
    if (!px) {
        ns_texture_unref(tex);
        return NULL;
    }
    ns_texture_download(tex, px, stride);
    ns_texture_unref(tex);
    *out_w = w;
    *out_h = h;
    return px;
}

ns_renderer_session *
ns_renderer_session_new(int ctrl_w, unsigned char *fb, int max_w, int max_h,
                        int shm_mode)
{
    if (!fb || max_w <= 0 || max_h <= 0)
        return NULL;
    ns_renderer_session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->ctrl_w = ctrl_w;
    s->fb = fb;
    s->max_w = max_w;
    s->max_h = max_h;
    s->shm_mode = shm_mode;
    s->tick_budget_ms = 16;
    s->frame_scale = 1.0;
    return s;
}

int
ns_renderer_session_busy(const ns_renderer_session *s)
{
    return s && ns_browser_busy(s->cur);
}

void
ns_renderer_session_free(ns_renderer_session *s)
{
    if (!s)
        return;
    session_clear_post(s);
    session_bfcache_clear(s);
    if (s->cur)
        ns_browser_close(s->cur);
    free(s);
}

static int
serve_append_hdr(char *buf, int pos, size_t cap, const char *name,
                 const char *value, int maxval)
{
    if (!value || !*value || pos < 0 || (size_t)pos >= cap) return pos;
    int need = snprintf(NULL, 0, "%s: %.*s\r\n", name, maxval, value);
    if (need <= 0 || (size_t)pos + (size_t)need >= cap) return pos;
    return pos + snprintf(buf + pos, cap - (size_t)pos, "%s: %.*s\r\n",
                          name, maxval, value);
}

int
ns_renderer_session_handle(ns_renderer_session *s, const http_head *head,
                           const char *body)
{
    int ctrl_w = s->ctrl_w;

    if (strcmp(head->path, "/quit") == 0) {
        http_write_response(ctrl_w, 200, "text/plain", NULL, NULL, 0);
        return 1;
    }

    if (strcmp(head->path, "/webgl") == 0) {
        char *origin = json_get_str(body, "origin");
        long allow = 0;
        json_get_long(body, "allow", &allow);
        if (s->cur && origin)
            ns_browser_resolve_webgl(s->cur, origin, (int)allow);
        s->frame_valid = 0;
        http_write_response(ctrl_w, 200, "text/plain", NULL, NULL, 0);
        free(origin);
        return 0;
    }

    if (strcmp(head->path, "/camera") == 0) {
        char *origin = json_get_str(body, "origin");
        long allow = 0;
        json_get_long(body, "allow", &allow);
        if (s->cur && origin)
            ns_browser_resolve_camera(s->cur, origin, (int)allow);
        s->frame_valid = 0;
        http_write_response(ctrl_w, 200, "text/plain", NULL, NULL, 0);
        free(origin);
        return 0;
    }

    if (strcmp(head->path, "/open") == 0) {
        char *url = json_get_str(body, "url");
        long w = 0, h = 0, settle = 0, history = 0;
        json_get_long(body, "width", &w);
        json_get_long(body, "height", &h);
        json_get_long(body, "settle_ms", &settle);
        json_get_long(body, "history", &history);
        int vw = clamp((int)w, 1, s->max_w);
        int vh = clamp((int)h, 1, s->max_h);
        ns_browser *restored = (history && url)
            ? session_bfcache_take(s, url) : NULL;
        char *referrer = (!history && s->cur) ? ns_browser_url(s->cur) : NULL;
        if (s->cur) {
            session_bfcache_park_or_close(s, s->cur);
            s->cur = NULL;
        }
        if (referrer && !restored && url && ns_url_same_origin(referrer, url))
            ns_browser_set_next_referrer(referrer);
        g_free(referrer);
        s->frame_valid = 0;
        ns_net_log_clear();
        if (restored) {
            ns_browser_bfcache_restore(restored, vw, (double)vh);
            s->cur = restored;
        } else if (url && s->post_body && s->post_url &&
            strcmp(url, s->post_url) == 0)
            s->cur = ns_browser_open_post_viewport(url, vw, vh, (int)settle,
                                                   s->post_body, s->post_len,
                                                   s->post_ct);
        else
            s->cur = url ? ns_browser_open_viewport(url, vw, vh, (int)settle)
                         : NULL;
        session_clear_post(s);
        int pw = 0, ph = 0, ok = s->cur != NULL;
        int security = 0;
        const char *ip = NULL;
        char *title = NULL, *final_url = NULL, *nav = NULL;
        char *title_escaped = NULL, *url_escaped = NULL, *nav_escaped = NULL;
        char *ip_escaped = NULL;
        if (s->cur) {
            ns_browser_page_size(s->cur, &pw, &ph);
            title = ns_browser_title(s->cur);
            final_url = ns_browser_url(s->cur);
            nav = ns_browser_take_pending_nav(s->cur);
            security = ns_browser_security(s->cur, &ip);
        }
        title_escaped = json_escape(title ? title : "");
        url_escaped = json_escape(final_url ? final_url : (url ? url : ""));
        nav_escaped = json_escape(nav ? nav : "");
        ip_escaped = json_escape(ip ? ip : "");
        char *json = NULL;
        int n = asprintf(&json,
                         "{\"ok\":%d,\"page_width\":%d,\"page_height\":%d,"
                         "\"title\":\"%s\",\"url\":\"%s\",\"nav\":\"%s\","
                         "\"security\":%d,\"ip\":\"%s\"}",
                         ok, pw, ph,
                         title_escaped ? title_escaped : "",
                         url_escaped ? url_escaped : "",
                         nav_escaped ? nav_escaped : "",
                         security, ip_escaped ? ip_escaped : "");
        if (n >= 0)
            http_write_response(ctrl_w, 200, "application/json", NULL, json,
                                (size_t)n);
        free(json);
        free(title_escaped);
        free(url_escaped);
        free(nav_escaped);
        free(ip_escaped);
        free(title);
        free(final_url);
        free(nav);
        free(url);
        return 0;
    }

    if (strcmp(head->path, "/render") == 0) {
        long w = 0, h = 0, sx = 0, sy = 0;
        double scale = 1.0;
        json_get_long(body, "width", &w);
        json_get_long(body, "height", &h);
        json_get_long(body, "scroll_x", &sx);
        json_get_long(body, "scroll_y", &sy);
        json_get_double(body, "scale", &scale);
        int vw = clamp((int)w, 1, s->max_w);
        int vh = clamp((int)h, 1, s->max_h);
        int stride = vw * 4;
        if (!s->cur) {
            http_write_response(ctrl_w, 200, "application/octet-stream",
                                "X-W: 0\r\nX-H: 0\r\nX-Stride: 0\r\n"
                                "X-Anim: 0\r\n", NULL, 0);
            return 0;
        }
        int ticked = ns_browser_tick(s->cur, s->tick_budget_ms);
        int unchanged = s->frame_valid && ticked == 0 &&
                        sx == s->frame_sx && sy == s->frame_sy &&
                        vw == s->frame_w && vh == s->frame_h &&
                        scale == s->frame_scale;
        int render_rc = 0;
        if (!unchanged) {
            render_rc = ns_browser_render_argb32(s->cur, (int)sx, (int)sy,
                                                 vw, vh, scale, s->fb,
                                                 stride);
            if (render_rc == 0) {
                s->frame_valid = 1;
                s->frame_sx = sx;
                s->frame_sy = sy;
                s->frame_w = vw;
                s->frame_h = vh;
                s->frame_scale = scale;
            } else {
                memset(s->fb, 0xff, (size_t)stride * (size_t)vh);
                s->frame_valid = 0;
            }
        }
        char *nav = ns_browser_take_pending_nav(s->cur);
        if (nav)
            for (char *p = nav; *p; p++)
                if (*p == '\r' || *p == '\n') *p = ' ';
        if (nav)
            session_stash_post(s, nav);
        char *webgl = ns_browser_take_pending_webgl(s->cur);
        if (webgl)
            for (char *p = webgl; *p; p++)
                if (*p == '\r' || *p == '\n') *p = ' ';
        char *camera = ns_browser_take_pending_camera(s->cur);
        if (camera)
            for (char *p = camera; *p; p++)
                if (*p == '\r' || *p == '\n') *p = ' ';
        char *download = ns_browser_take_pending_download(s->cur);
        if (download)
            for (char *p = download; *p; p++)
                if (*p == '\r' || *p == '\n') *p = ' ';
        char *audio = ns_browser_take_pending_audio(s->cur);
        if (audio)
            for (char *p = audio; *p; p++) {
                if (*p == '\r') *p = ' ';
                else if (*p == '\n') *p = '\x1f';
            }
        int page_w = 0, page_h = 0;
        ns_browser_page_size(s->cur, &page_w, &page_h);
        char hdrs[32768];
        int hn = snprintf(hdrs, sizeof hdrs,
                 "X-W: %d\r\nX-H: %d\r\nX-Stride: %d\r\nX-Anim: %d\r\n"
                 "X-PageW: %d\r\nX-PageH: %d\r\n"
                 "X-Render-RC: %d\r\n%s",
                 vw, vh, stride, ns_browser_animating(s->cur) ? 1 : 0,
                 page_w, page_h,
                 render_rc, unchanged ? "X-Unchanged: 1\r\n" : "");
        hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Nav", nav, 2000);
        hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-WebGL", webgl, 2000);
        hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Camera", camera, 2000);
        hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Download", download,
                              3000);
        hn = serve_append_hdr(hdrs, hn, sizeof hdrs, "X-Audio", audio, 16000);
        free(nav);
        free(webgl);
        free(camera);
        free(download);
        free(audio);
        if (s->shm_mode || unchanged)
            http_write_response(ctrl_w, 200, "application/octet-stream",
                                hdrs, NULL, 0);
        else
            http_write_response(ctrl_w, 200, "application/octet-stream",
                                hdrs, s->fb, (size_t)stride * (size_t)vh);
        return 0;
    }

    if (strcmp(head->path, "/favicon") == 0) {
        int fw = 0, fh = 0;
        unsigned char *px = s->cur ? favicon_bgra(s->cur, &fw, &fh) : NULL;
        if (px) {
            char hdrs[96];
            int hn = snprintf(hdrs, sizeof hdrs,
                              "X-W: %d\r\nX-H: %d\r\nX-Stride: %d\r\n",
                              fw, fh, fw * 4);
            http_write_response(ctrl_w, 200, "application/octet-stream",
                                hn > 0 ? hdrs : NULL, px,
                                (size_t)fw * (size_t)fh * 4u);
            free(px);
        } else {
            http_write_response(ctrl_w, 200, "application/octet-stream",
                                "X-W: 0\r\nX-H: 0\r\nX-Stride: 0\r\n", NULL, 0);
        }
        return 0;
    }

    if (strcmp(head->path, "/link") == 0 ||
        strcmp(head->path, "/click") == 0 ||
        strcmp(head->path, "/select") == 0) {
        long x = 0, y = 0, mods = 0, kind = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        json_get_long(body, "mods", &mods);
        json_get_long(body, "kind", &kind);
        char *href = NULL;
        if (s->cur && strcmp(head->path, "/link") == 0) {
            href = ns_browser_link_at(s->cur, (int)x, (int)y);
            char *cursor = ns_browser_cursor_at(s->cur, (int)x, (int)y);
            reply_str2(ctrl_w, "href", href, "cursor", cursor);
            free(cursor);
            free(href);
            return 0;
        }
        s->frame_valid = 0;
        if (s->cur && strcmp(head->path, "/click") == 0) {
            href = ns_browser_press(s->cur, (int)x, (int)y, (int)mods);
            session_stash_post(s, href);
        } else if (s->cur)
            href = ns_browser_select(s->cur, (int)kind, (int)x, (int)y);
        reply_str(ctrl_w, "href", href);
        free(href);
        return 0;
    }

    if (strcmp(head->path, "/key") == 0) {
        long kind = 0, keycode = 0, mods = 0;
        json_get_long(body, "kind", &kind);
        json_get_long(body, "keycode", &keycode);
        json_get_long(body, "mods", &mods);
        char *key = json_get_str(body, "key");
        char *code = json_get_str(body, "code");
        s->frame_valid = 0;
        int prevented = 0;
        char *href = s->cur ? ns_browser_key_full(s->cur, (int)kind,
                                                  key ? key : "",
                                                  code ? code : "",
                                                  (int)keycode, (int)mods,
                                                  &prevented)
                            : NULL;
        session_stash_post(s, href);
        char *e = json_escape(href ? href : "");
        char *json = NULL;
        int n = asprintf(&json, "{\"href\":\"%s\",\"prevented\":%d}",
                         e ? e : "", prevented ? 1 : 0);
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL, json,
                                (size_t)n);
        free(json);
        free(e);
        free(href);
        free(key);
        free(code);
        return 0;
    }

    if (strcmp(head->path, "/release") == 0) {
        s->frame_valid = 0;
        int changed = 0;
        char *href = s->cur ? ns_browser_release_click(s->cur, &changed) : NULL;
        session_stash_post(s, href);
        reply_href_changed(ctrl_w, href, changed > 0);
        free(href);
        return 0;
    }

    if (strcmp(head->path, "/dropfiles") == 0) {
        long x = 0, y = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        char *paths = json_get_str(body, "paths");
        int changed = 0;
        if (s->cur && paths && *paths) {
            char **list = g_strsplit(paths, "\n", -1);
            guint count = list ? g_strv_length(list) : 0;
            if (count > 0)
                changed = ns_browser_drop_files(s->cur, (int)x, (int)y,
                                                (const char *const *)list,
                                                (int)count);
            g_strfreev(list);
        }
        if (changed > 0) s->frame_valid = 0;
        char *json = NULL;
        int n = asprintf(&json, "{\"changed\":%d}", changed > 0 ? 1 : 0);
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        free(paths);
        return 0;
    }

    if (strcmp(head->path, "/hover") == 0) {
        long x = 0, y = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        int changed = s->cur ? ns_browser_hover(s->cur, (int)x, (int)y) : 0;
        if (changed > 0)
            s->frame_valid = 0;
        char *href = s->cur ? ns_browser_link_at(s->cur, (int)x, (int)y)
                            : NULL;
        char *cursor = s->cur ? ns_browser_cursor_at(s->cur, (int)x, (int)y)
                              : NULL;
        char *href_escaped = json_escape(href ? href : "");
        char *cursor_escaped = json_escape(cursor ? cursor : "");
        char *json = NULL;
        int n = asprintf(&json,
                         "{\"changed\":%d,\"href\":\"%s\",\"cursor\":\"%s\"}",
                         changed > 0 ? 1 : 0,
                         href_escaped ? href_escaped : "",
                         cursor_escaped ? cursor_escaped : "");
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        free(href_escaped);
        free(cursor_escaped);
        free(href);
        free(cursor);
        return 0;
    }

    if (strcmp(head->path, "/scroll") == 0) {
        long x = 0, y = 0, dx = 0, dy = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        json_get_long(body, "dx", &dx);
        json_get_long(body, "dy", &dy);
        int consumed = s->cur ? ns_browser_scroll_at(s->cur, (int)x, (int)y,
                                                      (int)dx, (int)dy) : 0;
        if (consumed)
            s->frame_valid = 0;
        char json[32];
        int n = snprintf(json, sizeof json, "{\"consumed\":%d}", consumed ? 1 : 0);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/scrollbar-press") == 0 ||
        strcmp(head->path, "/scrollbar-drag") == 0) {
        long x = 0, y = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        int hit = 0;
        if (s->cur) {
            hit = strcmp(head->path, "/scrollbar-press") == 0
                ? ns_browser_scrollbar_press(s->cur, (int)x, (int)y)
                : ns_browser_scrollbar_drag(s->cur, (int)x, (int)y);
        }
        if (hit)
            s->frame_valid = 0;
        char json[32];
        int n = snprintf(json, sizeof json, "{\"hit\":%d}", hit ? 1 : 0);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/scrollbar-release") == 0) {
        if (s->cur)
            ns_browser_scrollbar_release(s->cur);
        http_write_response(ctrl_w, 200, "application/json", NULL,
                            "{\"ok\":1}", 8);
        return 0;
    }

    if (strcmp(head->path, "/focused-editable") == 0) {
        char json[32];
        int active = s->cur ? ns_browser_focused_editable(s->cur) : 0;
        int n = snprintf(json, sizeof json, "{\"active\":%d}", active);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/focused-editable-state") == 0) {
        size_t caret = 0, anchor = 0;
        char *value = s->cur ? ns_browser_focused_editable_value(s->cur,
                                                                 &caret,
                                                                 &anchor)
                             : NULL;
        char *e = json_escape(value ? value : "");
        char *json = NULL;
        int n = asprintf(&json,
                         "{\"active\":%d,\"caret\":%zu,\"anchor\":%zu,"
                         "\"value\":\"%s\"}",
                         value ? 1 : 0, caret, anchor, e ? e : "");
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        free(e);
        free(value);
        return 0;
    }

    if (strcmp(head->path, "/focused-editable-selection") == 0) {
        long caret = 0, anchor = 0;
        json_get_long(body, "caret", &caret);
        json_get_long(body, "anchor", &anchor);
        int ok = s->cur ? ns_browser_set_focused_editable_selection(
                              s->cur,
                              caret > 0 ? (size_t)caret : 0,
                              anchor > 0 ? (size_t)anchor : 0)
                        : 0;
        if (ok)
            s->frame_valid = 0;
        char json[16];
        int n = snprintf(json, sizeof json, "{\"ok\":%d}", ok ? 1 : 0);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/find") == 0) {
        long cs = 0, dir = 0, from_y = 0;
        json_get_long(body, "case_sensitive", &cs);
        json_get_long(body, "direction", &dir);
        json_get_long(body, "from_y", &from_y);
        char *q = json_get_str(body, "query");
        int total = 0, current = 0, scroll_y = 0;
        s->frame_valid = 0;
        if (s->cur)
            ns_browser_find(s->cur, q ? q : "", (int)cs, (int)dir,
                            (int)from_y, &total, &current, &scroll_y);
        char json[96];
        int n = snprintf(json, sizeof json,
                         "{\"total\":%d,\"current\":%d,\"scroll_y\":%d}",
                         total, current, scroll_y);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        free(q);
        return 0;
    }

    if (strcmp(head->path, "/viewport") == 0) {
        long w = 0, h = 0;
        json_get_long(body, "width", &w);
        json_get_long(body, "height", &h);
        int vw = clamp((int)w, 1, s->max_w);
        int vh = clamp((int)h, 1, s->max_h);
        int pw = 0, ph = 0, ok = 0;
        s->frame_valid = 0;
        if (s->cur && ns_browser_set_viewport(s->cur, vw, vh) == 0) {
            ns_browser_page_size(s->cur, &pw, &ph);
            ok = 1;
        }
        char json[96];
        int n = snprintf(json, sizeof json,
                         "{\"ok\":%d,\"page_width\":%d,\"page_height\":%d}",
                         ok, pw, ph);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        return 0;
    }

    if (strcmp(head->path, "/eval") == 0) {
        char *src = json_get_str(body, "src");
        s->frame_valid = 0;
        char *res = s->cur ? ns_browser_eval(s->cur, src ? src : "") : NULL;
        reply_str(ctrl_w, "text", res);
        free(res);
        free(src);
        return 0;
    }

    if (strcmp(head->path, "/dump") == 0) {
        char *kind = json_get_str(body, "kind");
        char *res = NULL;
        if (s->cur && kind) {
            if (strcmp(kind, "dom") == 0)
                res = ns_browser_dump_dom(s->cur);
            else if (strcmp(kind, "layout") == 0)
                res = ns_browser_dump_layout(s->cur);
            else if (strcmp(kind, "text") == 0)
                res = ns_browser_render_text(s->cur);
            else if (strcmp(kind, "performance") == 0)
                res = ns_browser_dump_performance(s->cur);
        }
        if (!res && kind && strcmp(kind, "network") == 0)
            res = ns_net_log_dump();
        reply_str(ctrl_w, "text", res);
        free(res);
        free(kind);
        return 0;
    }

    if (strcmp(head->path, "/console") == 0) {
        char *log = s->cur ? ns_browser_console_drain(s->cur) : NULL;
        reply_str(ctrl_w, "text", log);
        free(log);
        return 0;
    }

    if (strcmp(head->path, "/media") == 0) {
        long x = 0, y = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        int is_video = 0, stream = 0;
        char *url = s->cur ? ns_browser_media_at(s->cur, (int)x, (int)y,
                                                 &is_video, &stream)
                           : NULL;
        char *e = json_escape(url ? url : "");
        char *json = NULL;
        int n = asprintf(&json,
                         "{\"url\":\"%s\",\"is_video\":%d,\"stream\":%d}",
                         e ? e : "", is_video, stream);
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        free(e);
        free(url);
        return 0;
    }

    if (strcmp(head->path, "/contextmenu") == 0) {
        long x = 0, y = 0;
        json_get_long(body, "x", &x);
        json_get_long(body, "y", &y);
        int prevented = s->cur
            ? ns_browser_contextmenu(s->cur, (int)x, (int)y) : 0;
        char *json = NULL;
        int n = asprintf(&json, "{\"prevented\":%d}", prevented);
        if (n > 0)
            http_write_response(ctrl_w, 200, "application/json", NULL,
                                json, (size_t)n);
        free(json);
        return 0;
    }

    if (strcmp(head->path, "/export") == 0) {
        char *path = json_get_str(body, "path");
        s->frame_valid = 0;
        int rc = (s->cur && path) ? ns_browser_render_image(s->cur, path)
                                  : -1;
        char json[24];
        int n = snprintf(json, sizeof json, "{\"ok\":%d}", rc);
        http_write_response(ctrl_w, 200, "application/json", NULL, json,
                            (size_t)n);
        free(path);
        return 0;
    }

    http_write_response(ctrl_w, 404, "text/plain", NULL, NULL, 0);
    return 0;
}
