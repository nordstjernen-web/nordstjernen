/* Nordstjernen — HTTP renderer IPC client (experiment; mirrors rproc.c). */

#define _GNU_SOURCE
#include "rproc_http.h"
#include "ipc_http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#if !defined(__ANDROID__)
static int
open_fb_fd(size_t size)
{
    int fd = -1;
#if defined(__linux__)
    fd = memfd_create("nshttp-fb", MFD_CLOEXEC);
#endif
    if (fd < 0) {
        static unsigned counter = 0;
        char name[64];
        snprintf(name, sizeof name, "/nshttp-%d-%u", (int)getpid(),
                 counter++);
        fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0)
            shm_unlink(name);
    }
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif
#endif

struct ns_rproc_http {
#ifdef _WIN32
    HANDLE         process;
    HANDLE         mapping;
#else
    pid_t          pid;
#endif
    int            inproc;
    int            sock;
    int            wfd;
    http_conn      conn;
    unsigned char *rxbuf;
    size_t         rxcap;
    int            shm;
    unsigned char *map;
    size_t         map_size;
    int            max_w;
    int            max_h;
};

static ns_rproc_inproc_attach_fn g_inproc_attach;

void
ns_rproc_http_set_inproc(ns_rproc_inproc_attach_fn attach)
{
    g_inproc_attach = attach;
}

#ifndef _WIN32
static void
wait_child(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
}
#endif

#ifndef _WIN32
#if defined(__ANDROID__)
static ns_rproc_http *
spawn_common(const char *renderer_path, int max_width, int max_height, int shm,
             int priv)
{
    (void)renderer_path;
    (void)max_width;
    (void)max_height;
    (void)shm;
    (void)priv;
    return NULL;
}
#else
static ns_rproc_http *
spawn_common(const char *renderer_path, int max_width, int max_height, int shm,
             int priv)
{
    if (!renderer_path || max_width <= 0 || max_height <= 0 ||
        max_width > 32768 || max_height > 32768)
        return NULL;
    signal(SIGPIPE, SIG_IGN);

    size_t size = (size_t)max_width * (size_t)max_height * 4u;
    unsigned char *rxbuf = NULL;
    unsigned char *map = NULL;
    int mapfd = -1;
    if (shm) {
        mapfd = open_fb_fd(size);
        if (mapfd < 0)
            return NULL;
        map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mapfd, 0);
        if (map == MAP_FAILED) {
            close(mapfd);
            return NULL;
        }
    } else {
        rxbuf = malloc(size);
        if (!rxbuf)
            return NULL;
    }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        goto fail;
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        goto fail;
    }
    if (pid == 0) {
        close(sv[0]);
        if (sv[1] != 3) {
            dup2(sv[1], 3);
            close(sv[1]);
        }
        char wbuf[16], hbuf[16];
        snprintf(wbuf, sizeof wbuf, "%d", max_width);
        snprintf(hbuf, sizeof hbuf, "%d", max_height);
        char *argv[6];
        int ai = 0;
        argv[ai++] = (char *)renderer_path;
        argv[ai++] = wbuf;
        argv[ai++] = hbuf;
        if (shm) argv[ai++] = (char *)"shm";
        if (priv) argv[ai++] = (char *)"private";
        argv[ai] = NULL;
        execv(renderer_path, argv);
        _exit(127);
    }
    close(sv[1]);
    if (shm) {
        http_send_fd(sv[0], mapfd);
        close(mapfd);
        mapfd = -1;
    }
    size_t bufsz = size + 65536;
    if (bufsz > 0x7fffffff) bufsz = 0x7fffffff;
    http_set_bufsize(sv[0], (int)bufsz);
    http_set_read_timeout(sv[0], 30);

    ns_rproc_http *r = calloc(1, sizeof *r);
    if (!r) {
        close(sv[0]);
        kill(pid, SIGKILL);
        wait_child(pid);
        goto fail;
    }
    r->pid = pid;
    r->sock = sv[0];
    r->wfd = sv[0];
    http_conn_init(&r->conn, sv[0]);
    r->rxbuf = rxbuf;
    r->rxcap = size;
    r->shm = shm;
    r->map = map;
    r->map_size = size;
    r->max_w = max_width;
    r->max_h = max_height;
    return r;

fail:
    if (map)
        munmap(map, size);
    if (mapfd >= 0)
        close(mapfd);
    free(rxbuf);
    return NULL;
}
#endif

#else /* _WIN32 */

static char *
win_dirname_dup(const char *path)
{
    if (!path || !*path)
        return NULL;
    const char *slash = strrchr(path, '\\');
    const char *alt = strrchr(path, '/');
    if (!slash || (alt && alt > slash))
        slash = alt;
    if (!slash)
        return NULL;
    size_t n = (size_t)(slash - path);
    if (n == 0)
        n = 1;
    if (n == 2 && path[1] == ':')
        n = 3;
    char *dir = malloc(n + 1);
    if (!dir)
        return NULL;
    memcpy(dir, path, n);
    dir[n] = '\0';
    return dir;
}

static ns_rproc_http *
spawn_common(const char *renderer_path, int max_width, int max_height, int shm,
             int priv)
{
    if (!renderer_path || max_width <= 0 || max_height <= 0 ||
        max_width > 32768 || max_height > 32768)
        return NULL;
    size_t size = (size_t)max_width * (size_t)max_height * 4u;

    HANDLE mapping = NULL;
    unsigned char *map = NULL;
    unsigned char *rxbuf = NULL;
    if (shm) {
        SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
        mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                     (DWORD)((uint64_t)size >> 32),
                                     (DWORD)(size & 0xffffffffu), NULL);
        if (!mapping)
            return NULL;
        map = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                            size);
        if (!map) {
            CloseHandle(mapping);
            return NULL;
        }
    } else {
        rxbuf = malloc(size);
        if (!rxbuf)
            return NULL;
    }

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE child_in = NULL, parent_in = NULL, parent_out = NULL,
           child_out = NULL;
    if (!CreatePipe(&child_in, &parent_in, &sa, 0) ||
        !SetHandleInformation(parent_in, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&parent_out, &child_out, &sa, 0) ||
        !SetHandleInformation(parent_out, HANDLE_FLAG_INHERIT, 0))
        goto winfail;

    char cmd[1024];
    const char *priv_arg = priv ? " private" : "";
    if (shm)
        snprintf(cmd, sizeof cmd, "\"%s\" %d %d shm %llu%s", renderer_path,
                 max_width, max_height,
                 (unsigned long long)(uintptr_t)mapping, priv_arg);
    else
        snprintf(cmd, sizeof cmd, "\"%s\" %d %d%s", renderer_path, max_width,
                 max_height, priv_arg);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_in;
    si.hStdOutput = child_out;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    char *workdir = win_dirname_dup(renderer_path);
    BOOL ok = CreateProcessA(renderer_path, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, workdir, &si, &pi);
    free(workdir);
    CloseHandle(child_in);
    CloseHandle(child_out);
    if (!ok)
        goto winfail2;
    CloseHandle(pi.hThread);

    ns_rproc_http *r = calloc(1, sizeof *r);
    if (!r) {
        CloseHandle(parent_in);
        CloseHandle(parent_out);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        goto winfail_nomap;
    }
    r->process = pi.hProcess;
    r->mapping = mapping;
    r->wfd = _open_osfhandle((intptr_t)parent_in, _O_BINARY);
    r->sock = _open_osfhandle((intptr_t)parent_out, _O_BINARY);
    http_conn_init(&r->conn, r->sock);
    r->rxbuf = rxbuf;
    r->rxcap = size;
    r->shm = shm;
    r->map = map;
    r->map_size = size;
    r->max_w = max_width;
    r->max_h = max_height;
    return r;

winfail2:
    if (parent_in)
        CloseHandle(parent_in);
    if (parent_out)
        CloseHandle(parent_out);
    goto winfail_nomap;
winfail:
    if (child_in) CloseHandle(child_in);
    if (parent_in) CloseHandle(parent_in);
    if (parent_out) CloseHandle(parent_out);
    if (child_out) CloseHandle(child_out);
winfail_nomap:
    if (map)
        UnmapViewOfFile(map);
    if (mapping)
        CloseHandle(mapping);
    free(rxbuf);
    return NULL;
}

#endif

static ns_rproc_http *
spawn_inproc(int max_width, int max_height)
{
    if (max_width <= 0 || max_height <= 0 ||
        max_width > 32768 || max_height > 32768)
        return NULL;
    size_t size = (size_t)max_width * (size_t)max_height * 4u;
    unsigned char *fb = malloc(size);
    if (!fb)
        return NULL;
    memset(fb, 0xff, size);
    ns_rproc_http *r = calloc(1, sizeof *r);
    if (!r) {
        free(fb);
        return NULL;
    }

#ifdef _WIN32
    int req[2] = { -1, -1 }, resp[2] = { -1, -1 };
    if (_pipe(req, 1 << 20, _O_BINARY) != 0) {
        free(r);
        free(fb);
        return NULL;
    }
    if (_pipe(resp, 1 << 20, _O_BINARY) != 0) {
        _close(req[0]);
        _close(req[1]);
        free(r);
        free(fb);
        return NULL;
    }
    if (g_inproc_attach(req[0], resp[1], fb, max_width, max_height) != 0) {
        _close(req[0]);
        _close(req[1]);
        _close(resp[0]);
        _close(resp[1]);
        free(r);
        free(fb);
        return NULL;
    }
    int client_r = resp[0], client_w = req[1];
#else
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        free(r);
        free(fb);
        return NULL;
    }
    http_set_bufsize(sv[0], 1 << 20);
    http_set_bufsize(sv[1], 1 << 20);
    if (g_inproc_attach(sv[1], sv[1], fb, max_width, max_height) != 0) {
        close(sv[0]);
        close(sv[1]);
        free(r);
        free(fb);
        return NULL;
    }
    int client_r = sv[0], client_w = sv[0];
#endif

#ifndef _WIN32
    r->pid = -1;
#endif
    r->inproc = 1;
    r->sock = client_r;
    r->wfd = client_w;
    http_conn_init(&r->conn, client_r);
    r->shm = 1;
    r->map = fb;
    r->map_size = size;
    r->max_w = max_width;
    r->max_h = max_height;
    return r;
}

ns_rproc_http *
ns_rproc_http_spawn(const char *renderer_path, int max_width, int max_height)
{
    if (g_inproc_attach)
        return spawn_inproc(max_width, max_height);
    return spawn_common(renderer_path, max_width, max_height, 0, 0);
}

ns_rproc_http *
ns_rproc_http_spawn_shm(const char *renderer_path, int max_width,
                        int max_height)
{
    if (g_inproc_attach)
        return spawn_inproc(max_width, max_height);
    return spawn_common(renderer_path, max_width, max_height, 1, 0);
}

ns_rproc_http *
ns_rproc_http_spawn_shm_ex(const char *renderer_path, int max_width,
                           int max_height, int private_mode)
{
    if (g_inproc_attach)
        return spawn_inproc(max_width, max_height);
    return spawn_common(renderer_path, max_width, max_height, 1, private_mode);
}

int
ns_rproc_http_open(ns_rproc_http *r, const char *url, int viewport_width,
                   int viewport_height, int settle_ms, ns_rproc_http_page *out)
{
    return ns_rproc_http_open_ex(r, url, viewport_width, viewport_height,
                                 settle_ms, 0, 1, out);
}

int
ns_rproc_http_open_ex(ns_rproc_http *r, const char *url, int viewport_width,
                      int viewport_height, int settle_ms, int history,
                      int user_activated,
                      ns_rproc_http_page *out)
{
    if (!r || !url || !out)
        return -1;
    memset(out, 0, sizeof *out);
    char *ue = json_escape(url);
    if (!ue)
        return -1;
    char *json = NULL;
    int jn = asprintf(&json,
                      "{\"url\":\"%s\",\"width\":%d,\"height\":%d,"
                      "\"settle_ms\":%d,\"history\":%d,"
                      "\"user_activated\":%d}",
                      ue, viewport_width, viewport_height, settle_ms,
                      history ? 1 : 0, user_activated ? 1 : 0);
    free(ue);
    if (jn < 0)
        return -1;
    int rc = http_write_request(r->wfd, "POST", "/open", "application/json",
                                json, (size_t)jn);
    free(json);
    if (rc != 0)
        return -1;

    http_head head;
    if (http_read_head(&r->conn, &head) != 0 || head.content_length < 0 ||
        head.content_length > NS_HTTP_MAX_REPLY)
        return -1;
    char *body = malloc((size_t)head.content_length + 1);
    if (!body)
        return -1;
    if (head.content_length &&
        http_read_body(&r->conn, head.content_length, body) != 0) {
        free(body);
        return -1;
    }
    body[head.content_length] = '\0';

    long ok = 0, pw = 0, ph = 0;
    json_get_long(body, "ok", &ok);
    json_get_long(body, "page_width", &pw);
    json_get_long(body, "page_height", &ph);
    out->ok = ok != 0;
    out->page_width = (int)pw;
    out->page_height = (int)ph;
    out->title = json_get_str(body, "title");
    out->url = json_get_str(body, "url");
    out->nav = json_get_str(body, "nav");
    if (out->nav && !*out->nav) {
        free(out->nav);
        out->nav = NULL;
    }
    long sec = 0;
    json_get_long(body, "security", &sec);
    out->security = (int)sec;
    out->remote_ip = json_get_str(body, "ip");
    if (out->remote_ip && !*out->remote_ip) {
        free(out->remote_ip);
        out->remote_ip = NULL;
    }
    free(body);
    return 0;
}

int
ns_rproc_http_render(ns_rproc_http *r, int width, int height, int scroll_x,
                     int scroll_y, double scale, int caret_active,
                     ns_rproc_http_frame *out)
{
    if (!r || !out)
        return -1;
    memset(out, 0, sizeof *out);
    out->scroll_y = -1;
    if (width > r->max_w)
        width = r->max_w;
    if (height > r->max_h)
        height = r->max_h;
    if (!(scale > 0))
        scale = 1.0;
    int scale_milli = (int)(scale * 1000.0 + 0.5);
    char json[192];
    int jn = snprintf(json, sizeof json,
                      "{\"width\":%d,\"height\":%d,\"scroll_x\":%d,"
                      "\"scroll_y\":%d,\"scale\":%d.%03d,\"caret\":%d}",
                      width, height, scroll_x, scroll_y,
                      scale_milli / 1000, scale_milli % 1000,
                      caret_active ? 1 : 0);
    if (http_write_request(r->wfd, "POST", "/render", "application/json",
                           json, (size_t)jn) != 0)
        return -1;

    http_head head;
    if (http_read_head(&r->conn, &head) != 0)
        return -1;
    if (head.content_length < 0 || (size_t)head.content_length > r->rxcap) {
        if (head.content_length > 0)
            http_skip_body(&r->conn, head.content_length);
        return -1;
    }
    if (head.content_length &&
        http_read_body(&r->conn, head.content_length, r->rxbuf) != 0)
        return -1;
    if (head.x_unchanged > 0) {
        out->ok = 1;
        out->unchanged = 1;
        out->animating = (head.x_anim & 1) != 0;
        out->caret_blinking = (head.x_anim & 2) != 0;
        out->page_w = (int)head.x_page_w;
        out->page_h = (int)head.x_page_h;
        out->scroll_y = (int)head.x_scroll_y;
        out->render_rc = (int)head.x_render_rc;
        out->nav = head.x_nav[0] ? strdup(head.x_nav) : NULL;
        out->webgl = head.x_webgl[0] ? strdup(head.x_webgl) : NULL;
        out->camera = head.x_camera[0] ? strdup(head.x_camera) : NULL;
        out->download = head.x_download[0] ? strdup(head.x_download) : NULL;
        out->audio = head.x_audio[0] ? strdup(head.x_audio) : NULL;
        out->window_action = head.x_window_action[0]
            ? strdup(head.x_window_action) : NULL;
        return 0;
    }
    if (head.x_w < 1 || head.x_w > r->max_w ||
        head.x_h < 1 || head.x_h > r->max_h ||
        head.x_stride < head.x_w * 4 || head.x_stride > (long)r->max_w * 4)
        return -1;
    if (!r->shm && (uint64_t)head.content_length <
            (uint64_t)head.x_stride * (uint64_t)head.x_h)
        return -1;
    out->ok = 1;
    out->width = (int)head.x_w;
    out->height = (int)head.x_h;
    out->stride = (int)head.x_stride;
    out->animating = (head.x_anim & 1) != 0;
    out->caret_blinking = (head.x_anim & 2) != 0;
    out->page_w = (int)head.x_page_w;
    out->page_h = (int)head.x_page_h;
    out->scroll_y = (int)head.x_scroll_y;
    out->render_rc = (int)head.x_render_rc;
    out->pixels = r->shm ? r->map : r->rxbuf;
    out->nav = head.x_nav[0] ? strdup(head.x_nav) : NULL;
    out->webgl = head.x_webgl[0] ? strdup(head.x_webgl) : NULL;
    out->camera = head.x_camera[0] ? strdup(head.x_camera) : NULL;
    out->download = head.x_download[0] ? strdup(head.x_download) : NULL;
    out->audio = head.x_audio[0] ? strdup(head.x_audio) : NULL;
    out->window_action = head.x_window_action[0]
        ? strdup(head.x_window_action) : NULL;
    return 0;
}

static char *
request(ns_rproc_http *r, const char *path, const char *json_body)
{
    if (http_write_request(r->wfd, "POST", path, "application/json",
                           json_body, json_body ? strlen(json_body) : 0) != 0)
        return NULL;
    http_head head;
    if (http_read_head(&r->conn, &head) != 0 ||
        head.content_length < 0 || head.content_length > NS_HTTP_MAX_REPLY)
        return NULL;
    char *body = malloc((size_t)head.content_length + 1);
    if (!body)
        return NULL;
    if (head.content_length &&
        http_read_body(&r->conn, head.content_length, body) != 0) {
        free(body);
        return NULL;
    }
    body[head.content_length] = '\0';
    return body;
}

int
ns_rproc_http_tick_page(ns_rproc_http *r, ns_rproc_http_tick *out)
{
    if (!r || !out) return -1;
    memset(out, 0, sizeof *out);
    char *body = request(r, "/tick", "{}");
    if (!body) return -1;
    long ok = 0, changed = 0, animating = 0, page_w = 0, page_h = 0;
    json_get_long(body, "ok", &ok);
    json_get_long(body, "changed", &changed);
    json_get_long(body, "animating", &animating);
    json_get_long(body, "page_width", &page_w);
    json_get_long(body, "page_height", &page_h);
    out->ok = ok != 0;
    out->changed = changed != 0;
    out->animating = animating != 0;
    out->page_w = (int)page_w;
    out->page_h = (int)page_h;
    out->nav = json_get_str(body, "nav");
    out->webgl = json_get_str(body, "webgl");
    out->camera = json_get_str(body, "camera");
    out->download = json_get_str(body, "download");
    out->audio = json_get_str(body, "audio");
    out->window_action = json_get_str(body, "window_action");
    free(body);
    return 0;
}

void
ns_rproc_http_tick_clear(ns_rproc_http_tick *out)
{
    if (!out) return;
    free(out->nav);
    free(out->webgl);
    free(out->camera);
    free(out->download);
    free(out->audio);
    free(out->window_action);
    memset(out, 0, sizeof *out);
}

static char *
request_xy_str(ns_rproc_http *r, const char *path, int x, int y, int extra_key,
               int extra_val, const char *result_key)
{
    char json[160];
    if (extra_key)
        snprintf(json, sizeof json, "{\"x\":%d,\"y\":%d,\"%s\":%d}", x, y,
                 extra_key == 1 ? "mods" : "kind", extra_val);
    else
        snprintf(json, sizeof json, "{\"x\":%d,\"y\":%d}", x, y);
    char *body = request(r, path, json);
    if (!body)
        return NULL;
    char *res = json_get_str(body, result_key);
    free(body);
    return res;
}

char *
ns_rproc_http_link_at(ns_rproc_http *r, int x, int y)
{
    return r ? request_xy_str(r, "/link", x, y, 0, 0, "href") : NULL;
}

char *
ns_rproc_http_link_cursor_at(ns_rproc_http *r, int x, int y, char **out_cursor)
{
    if (out_cursor)
        *out_cursor = NULL;
    if (!r)
        return NULL;
    char json[160];
    snprintf(json, sizeof json, "{\"x\":%d,\"y\":%d}", x, y);
    char *body = request(r, "/link", json);
    if (!body)
        return NULL;
    char *href = json_get_str(body, "href");
    if (out_cursor) {
        char *cursor = json_get_str(body, "cursor");
        if (cursor && !*cursor) {
            free(cursor);
            cursor = NULL;
        }
        *out_cursor = cursor;
    }
    free(body);
    return href;
}

char *
ns_rproc_http_click(ns_rproc_http *r, int x, int y, int mods)
{
    return r ? request_xy_str(r, "/click", x, y, 1, mods, "href") : NULL;
}

char *
ns_rproc_http_select(ns_rproc_http *r, int kind, int x, int y)
{
    return r ? request_xy_str(r, "/select", x, y, 2, kind, "href") : NULL;
}

char *
ns_rproc_http_key_full(ns_rproc_http *r, int kind, const char *key,
                       const char *code, int keycode, int mods,
                       int *out_prevented)
{
    if (out_prevented) *out_prevented = 0;
    if (!r)
        return NULL;
    char *ke = json_escape(key ? key : "");
    char *ce = json_escape(code ? code : "");
    char *json = NULL;
    if (asprintf(&json,
                 "{\"kind\":%d,\"key\":\"%s\",\"code\":\"%s\","
                 "\"keycode\":%d,\"mods\":%d}",
                 kind, ke ? ke : "", ce ? ce : "", keycode, mods) < 0)
        json = NULL;
    free(ke);
    free(ce);
    char *body = json ? request(r, "/key", json) : NULL;
    free(json);
    if (!body)
        return NULL;
    char *href = json_get_str(body, "href");
    long prevented = 0;
    json_get_long(body, "prevented", &prevented);
    if (out_prevented) *out_prevented = prevented != 0;
    free(body);
    return href;
}

char *
ns_rproc_http_key(ns_rproc_http *r, int kind, const char *key,
                  const char *code, int keycode, int mods)
{
    return ns_rproc_http_key_full(r, kind, key, code, keycode, mods, NULL);
}

char *
ns_rproc_http_release_full(ns_rproc_http *r, int *out_changed)
{
    if (out_changed) *out_changed = 0;
    if (!r)
        return NULL;
    char *body = request(r, "/release", "{}");
    if (!body)
        return NULL;
    long changed = 0;
    json_get_long(body, "changed", &changed);
    char *href = json_get_str(body, "href");
    free(body);
    if (out_changed) *out_changed = changed != 0 ? 1 : 0;
    return href;
}

int
ns_rproc_http_focused_editable(ns_rproc_http *r)
{
    if (!r)
        return 0;
    char *body = request(r, "/focused-editable", "{}");
    if (!body)
        return 0;
    long active = 0;
    json_get_long(body, "active", &active);
    free(body);
    return active != 0 ? 1 : 0;
}

char *
ns_rproc_http_focused_editable_value(ns_rproc_http *r, size_t *out_caret,
                                     size_t *out_anchor)
{
    if (out_caret)
        *out_caret = 0;
    if (out_anchor)
        *out_anchor = 0;
    if (!r)
        return NULL;
    char *body = request(r, "/focused-editable-state", "{}");
    if (!body)
        return NULL;
    long active = 0, caret = 0, anchor = 0;
    json_get_long(body, "active", &active);
    json_get_long(body, "caret", &caret);
    json_get_long(body, "anchor", &anchor);
    char *value = active != 0 ? json_get_str(body, "value") : NULL;
    free(body);
    if (!value && active != 0)
        value = strdup("");
    if (out_caret && caret > 0)
        *out_caret = (size_t)caret;
    if (out_anchor && anchor > 0)
        *out_anchor = (size_t)anchor;
    return active != 0 ? value : NULL;
}

int
ns_rproc_http_set_focused_editable_selection(ns_rproc_http *r, size_t caret,
                                             size_t anchor)
{
    if (!r)
        return 0;
    char json[96];
    int n = snprintf(json, sizeof json, "{\"caret\":%zu,\"anchor\":%zu}",
                     caret, anchor);
    if (n < 0 || (size_t)n >= sizeof json)
        return 0;
    char *body = request(r, "/focused-editable-selection", json);
    if (!body)
        return 0;
    long ok = 0;
    json_get_long(body, "ok", &ok);
    free(body);
    return ok != 0 ? 1 : 0;
}

int
ns_rproc_http_hover_full(ns_rproc_http *r, int x, int y, char **out_href,
                         char **out_cursor)
{
    if (out_href)
        *out_href = NULL;
    if (out_cursor)
        *out_cursor = NULL;
    if (!r)
        return -1;
    char json[48];
    snprintf(json, sizeof json, "{\"x\":%d,\"y\":%d}", x, y);
    char *body = request(r, "/hover", json);
    if (!body)
        return -1;
    long changed = 0;
    json_get_long(body, "changed", &changed);
    if (out_href) {
        char *href = json_get_str(body, "href");
        if (href && !*href) {
            free(href);
            href = NULL;
        }
        *out_href = href;
    }
    if (out_cursor) {
        char *cursor = json_get_str(body, "cursor");
        if (cursor && !*cursor) {
            free(cursor);
            cursor = NULL;
        }
        *out_cursor = cursor;
    }
    free(body);
    return changed != 0 ? 1 : 0;
}

int
ns_rproc_http_scroll(ns_rproc_http *r, int x, int y, int dx, int dy)
{
    if (!r)
        return 0;
    char json[80];
    snprintf(json, sizeof json, "{\"x\":%d,\"y\":%d,\"dx\":%d,\"dy\":%d}",
             x, y, dx, dy);
    char *body = request(r, "/scroll", json);
    if (!body)
        return 0;
    long consumed = 0;
    json_get_long(body, "consumed", &consumed);
    free(body);
    return consumed != 0 ? 1 : 0;
}

int
ns_rproc_http_scrollbar(ns_rproc_http *r, int kind, int x, int y)
{
    if (!r)
        return 0;
    const char *path = kind == 0 ? "/scrollbar-press"
                     : kind == 1 ? "/scrollbar-drag"
                                 : "/scrollbar-release";
    char json[48];
    snprintf(json, sizeof json, "{\"x\":%d,\"y\":%d}", x, y);
    char *body = request(r, path, json);
    if (!body)
        return 0;
    long hit = 0;
    json_get_long(body, "hit", &hit);
    free(body);
    return hit != 0 ? 1 : 0;
}

int
ns_rproc_http_find(ns_rproc_http *r, const char *query, int case_sensitive,
                   int direction, int from_y, int *out_total, int *out_current,
                   int *out_scroll_y)
{
    if (out_total) *out_total = 0;
    if (out_current) *out_current = 0;
    if (out_scroll_y) *out_scroll_y = 0;
    if (!r)
        return -1;
    char *qe = json_escape(query ? query : "");
    char *json = NULL;
    if (asprintf(&json,
                 "{\"query\":\"%s\",\"case_sensitive\":%d,\"direction\":%d,"
                 "\"from_y\":%d}",
                 qe ? qe : "", case_sensitive ? 1 : 0, direction, from_y) < 0)
        json = NULL;
    free(qe);
    char *body = json ? request(r, "/find", json) : NULL;
    free(json);
    if (!body)
        return -1;
    long total = 0, current = 0, scroll_y = 0;
    json_get_long(body, "total", &total);
    json_get_long(body, "current", &current);
    json_get_long(body, "scroll_y", &scroll_y);
    free(body);
    if (out_total) *out_total = (int)total;
    if (out_current) *out_current = (int)current;
    if (out_scroll_y) *out_scroll_y = (int)scroll_y;
    return 0;
}

int
ns_rproc_http_drop_files(ns_rproc_http *r, int x, int y,
                         const char *const *paths, int n_paths)
{
    if (!r || !paths || n_paths <= 0)
        return 0;
    size_t total = 1;
    for (int i = 0; i < n_paths; i++) {
        size_t len = paths[i] ? strlen(paths[i]) : 0;
        if (len > SIZE_MAX - total - 1)
            return 0;
        total += len + 1;
    }
    char *joined = malloc(total);
    if (!joined)
        return 0;
    char *dst = joined;
    for (int i = 0; i < n_paths; i++) {
        if (i)
            *dst++ = '\n';
        if (paths[i]) {
            size_t len = strlen(paths[i]);
            memcpy(dst, paths[i], len);
            dst += len;
        }
    }
    *dst = '\0';
    char *pe = json_escape(joined);
    free(joined);
    char *json = NULL;
    if (asprintf(&json, "{\"x\":%d,\"y\":%d,\"paths\":\"%s\"}", x, y,
                 pe ? pe : "") < 0)
        json = NULL;
    free(pe);
    char *body = json ? request(r, "/dropfiles", json) : NULL;
    free(json);
    if (!body)
        return 0;
    long changed = 0;
    json_get_long(body, "changed", &changed);
    free(body);
    return (int)changed;
}

int
ns_rproc_http_set_viewport(ns_rproc_http *r, int width, int height,
                           ns_rproc_http_page *out)
{
    if (out)
        memset(out, 0, sizeof *out);
    if (!r)
        return -1;
    char json[64];
    snprintf(json, sizeof json, "{\"width\":%d,\"height\":%d}", width, height);
    char *body = request(r, "/viewport", json);
    if (!body)
        return -1;
    long ok = 0, pw = 0, ph = 0;
    json_get_long(body, "ok", &ok);
    json_get_long(body, "page_width", &pw);
    json_get_long(body, "page_height", &ph);
    free(body);
    if (out) {
        out->ok = ok != 0;
        out->page_width = (int)pw;
        out->page_height = (int)ph;
    }
    return ok ? 0 : -1;
}

int
ns_rproc_http_resolve_webgl(ns_rproc_http *r, const char *origin, int allow)
{
    if (!r || !origin)
        return -1;
    char *oe = json_escape(origin);
    char *json = NULL;
    if (asprintf(&json, "{\"origin\":\"%s\",\"allow\":%d}",
                 oe ? oe : "", allow ? 1 : 0) < 0)
        json = NULL;
    free(oe);
    char *body = json ? request(r, "/webgl", json) : NULL;
    free(json);
    int ok = body != NULL;
    free(body);
    return ok ? 0 : -1;
}

int
ns_rproc_http_resolve_camera(ns_rproc_http *r, const char *origin, int allow)
{
    if (!r || !origin)
        return -1;
    char *oe = json_escape(origin);
    char *json = NULL;
    if (asprintf(&json, "{\"origin\":\"%s\",\"allow\":%d}",
                 oe ? oe : "", allow ? 1 : 0) < 0)
        json = NULL;
    free(oe);
    char *body = json ? request(r, "/camera", json) : NULL;
    free(json);
    int ok = body != NULL;
    free(body);
    return ok ? 0 : -1;
}

int
ns_rproc_http_video_event(ns_rproc_http *r, const char *token,
                          const char *kind)
{
    if (!r || !token || !kind) return -1;
    char *te = json_escape(token);
    char *ke = json_escape(kind);
    char *json = NULL;
    if (asprintf(&json, "{\"token\":\"%s\",\"kind\":\"%s\"}",
                 te ? te : "", ke ? ke : "") < 0)
        json = NULL;
    free(te);
    free(ke);
    char *body = json ? request(r, "/video-event", json) : NULL;
    free(json);
    int ok = body != NULL;
    free(body);
    return ok ? 0 : -1;
}

char *
ns_rproc_http_eval(ns_rproc_http *r, const char *src)
{
    if (!r)
        return NULL;
    char *se = json_escape(src ? src : "");
    char *json = NULL;
    if (asprintf(&json, "{\"src\":\"%s\"}", se ? se : "") < 0)
        json = NULL;
    free(se);
    char *body = json ? request(r, "/eval", json) : NULL;
    free(json);
    if (!body)
        return NULL;
    char *text = json_get_str(body, "text");
    free(body);
    return text;
}

char *
ns_rproc_http_dump(ns_rproc_http *r, const char *kind)
{
    if (!r || !kind)
        return NULL;
    char *ke = json_escape(kind);
    char *json = NULL;
    if (asprintf(&json, "{\"kind\":\"%s\"}", ke ? ke : "") < 0)
        json = NULL;
    free(ke);
    char *body = json ? request(r, "/dump", json) : NULL;
    free(json);
    if (!body)
        return NULL;
    char *text = json_get_str(body, "text");
    free(body);
    return text;
}

char *
ns_rproc_http_console_poll(ns_rproc_http *r)
{
    if (!r)
        return NULL;
    char *body = request(r, "/console", "{}");
    if (!body)
        return NULL;
    char *text = json_get_str(body, "text");
    free(body);
    return text;
}

char *
ns_rproc_http_media_at(ns_rproc_http *r, int x, int y, int *out_is_video,
                       int *out_stream)
{
    if (out_is_video) *out_is_video = 0;
    if (out_stream) *out_stream = 0;
    if (!r)
        return NULL;
    char json[48];
    snprintf(json, sizeof json, "{\"x\":%d,\"y\":%d}", x, y);
    char *body = request(r, "/media", json);
    if (!body)
        return NULL;
    long iv = 0, st = 0;
    json_get_long(body, "is_video", &iv);
    json_get_long(body, "stream", &st);
    if (out_is_video) *out_is_video = (int)iv;
    if (out_stream) *out_stream = (int)st;
    char *url = json_get_str(body, "url");
    free(body);
    if (url && !*url) {
        free(url);
        url = NULL;
    }
    return url;
}

void
ns_rproc_http_contextmenu(ns_rproc_http *r, int x, int y, int *out_prevented)
{
    if (out_prevented) *out_prevented = 0;
    if (!r)
        return;
    char json[48];
    snprintf(json, sizeof json, "{\"x\":%d,\"y\":%d}", x, y);
    char *body = request(r, "/contextmenu", json);
    if (!body)
        return;
    long pv = 0;
    json_get_long(body, "prevented", &pv);
    if (out_prevented) *out_prevented = (int)pv;
    free(body);
}

int
ns_rproc_http_export(ns_rproc_http *r, const char *path)
{
    if (!r || !path)
        return -1;
    char *pe = json_escape(path);
    char *json = NULL;
    if (pe && asprintf(&json, "{\"path\":\"%s\"}", pe) < 0)
        json = NULL;
    free(pe);
    char *body = json ? request(r, "/export", json) : NULL;
    free(json);
    if (!body)
        return -1;
    long ok = -1;
    json_get_long(body, "ok", &ok);
    free(body);
    return ok == 0 ? 0 : -1;
}

unsigned char *
ns_rproc_http_favicon(ns_rproc_http *r, int *out_w, int *out_h, int *out_stride)
{
    *out_w = 0;
    *out_h = 0;
    *out_stride = 0;
    if (!r)
        return NULL;
    if (http_write_request(r->wfd, "POST", "/favicon", "application/json", "",
                           0) != 0)
        return NULL;
    http_head head;
    if (http_read_head(&r->conn, &head) != 0)
        return NULL;
    if (head.content_length <= 0 || head.content_length > NS_HTTP_MAX_REPLY)
        return NULL;
    unsigned char *body = malloc((size_t)head.content_length);
    if (!body)
        return NULL;
    if (http_read_body(&r->conn, head.content_length, body) != 0) {
        free(body);
        return NULL;
    }
    if (head.x_w < 1 || head.x_w > 1024 || head.x_h < 1 || head.x_h > 1024 ||
        head.x_stride < head.x_w * 4 || head.x_stride > (long)1024 * 4 ||
        (uint64_t)head.x_stride * (uint64_t)head.x_h >
            (uint64_t)head.content_length) {
        free(body);
        return NULL;
    }
    *out_w = (int)head.x_w;
    *out_h = (int)head.x_h;
    *out_stride = (int)head.x_stride;
    return body;
}

void
ns_rproc_http_page_clear(ns_rproc_http_page *out)
{
    if (!out)
        return;
    free(out->title);
    free(out->url);
    free(out->nav);
    free(out->remote_ip);
    out->title = NULL;
    out->url = NULL;
    out->nav = NULL;
    out->remote_ip = NULL;
}

void
ns_rproc_http_interrupt(ns_rproc_http *r)
{
    if (!r)
        return;
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(r->sock);
    if (h != INVALID_HANDLE_VALUE)
        CancelIoEx(h, NULL);
#else
    shutdown(r->sock, SHUT_RDWR);
#endif
}

int
ns_rproc_http_pid(ns_rproc_http *r)
{
    if (!r)
        return -1;
#ifdef _WIN32
    return r->process ? (int)GetProcessId(r->process) : -1;
#else
    return (int)r->pid;
#endif
}

void
ns_rproc_http_terminate(ns_rproc_http *r)
{
    if (!r)
        return;
    if (r->inproc) {
        ns_rproc_http_interrupt(r);
        return;
    }
#ifdef _WIN32
    if (r->process)
        TerminateProcess(r->process, 1);
#else
    if (r->pid > 0)
        kill(r->pid, SIGKILL);
#endif
}

int
ns_rproc_self_pid(void)
{
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

int
ns_rproc_http_proc_info(int pid, char *state, int state_sz, long *rss_kb)
{
    if (state && state_sz > 0)
        snprintf(state, (size_t)state_sz, "%s", "running");
    if (rss_kb)
        *rss_kb = -1;
    if (pid <= 0)
        return 0;
#if defined(__linux__)
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        if (state && state_sz > 0)
            snprintf(state, (size_t)state_sz, "%s", "terminated");
        return 0;
    }
    char buf[512];
    if (fgets(buf, sizeof buf, f)) {
        char *rp = strrchr(buf, ')');
        if (rp && rp[1] == ' ' && rp[2] && state && state_sz > 0) {
            const char *s = "running";
            switch (rp[2]) {
            case 'R': s = "running";    break;
            case 'S': case 'D': s = "sleeping"; break;
            case 'T': case 't': s = "stopped";  break;
            case 'Z': case 'X': s = "terminated"; break;
            default:  s = "running";    break;
            }
            snprintf(state, (size_t)state_sz, "%s", s);
        }
    }
    fclose(f);
    snprintf(path, sizeof path, "/proc/%d/statm", pid);
    f = fopen(path, "r");
    if (f) {
        long total = 0, resident = 0;
        if (fscanf(f, "%ld %ld", &total, &resident) == 2 && rss_kb) {
            long pg = sysconf(_SC_PAGESIZE);
            *rss_kb = resident * (pg > 0 ? pg / 1024 : 4);
        }
        fclose(f);
    }
    return 1;
#elif defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        if (state && state_sz > 0)
            snprintf(state, (size_t)state_sz, "%s", "terminated");
        return 0;
    }
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    if (rss_kb) {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(h, &pmc, sizeof pmc))
            *rss_kb = (long)(pmc.WorkingSetSize / 1024);
    }
    CloseHandle(h);
    if (!alive && state && state_sz > 0)
        snprintf(state, (size_t)state_sz, "%s", "terminated");
    return alive;
#else
    if (kill((pid_t)pid, 0) != 0) {
        if (state && state_sz > 0)
            snprintf(state, (size_t)state_sz, "%s", "terminated");
        return 0;
    }
    return 1;
#endif
}

double
ns_rproc_http_proc_cpu(int pid)
{
    if (pid <= 0) return -1.0;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return -1.0;
    FILETIME cre, ex, ker, usr;
    double sec = -1.0;
    if (GetProcessTimes(h, &cre, &ex, &ker, &usr)) {
        ULONGLONG k = ((ULONGLONG)ker.dwHighDateTime << 32) | ker.dwLowDateTime;
        ULONGLONG u = ((ULONGLONG)usr.dwHighDateTime << 32) | usr.dwLowDateTime;
        sec = (double)(k + u) / 1e7;
    }
    CloseHandle(h);
    return sec;
#elif defined(__linux__)
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1.0;
    char buf[1024];
    double sec = -1.0;
    if (fgets(buf, sizeof buf, f)) {
        char *rp = strrchr(buf, ')');
        if (rp && rp[1]) {
            long tick = sysconf(_SC_CLK_TCK);
            if (tick <= 0) tick = 100;
            long utime = 0, stime = 0;
            int idx = 0;
            for (char *tok = strtok(rp + 2, " "); tok;
                 tok = strtok(NULL, " "), idx++) {
                if (idx == 11) utime = atol(tok);
                else if (idx == 12) { stime = atol(tok); break; }
            }
            sec = (double)(utime + stime) / (double)tick;
        }
    }
    fclose(f);
    return sec;
#else
    return -1.0;
#endif
}

int
ns_rproc_http_proc_threads(int pid)
{
    if (pid <= 0) return -1;
#if defined(__linux__)
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[1024];
    int threads = -1;
    if (fgets(buf, sizeof buf, f)) {
        char *rp = strrchr(buf, ')');
        if (rp && rp[1]) {
            int idx = 0;
            for (char *tok = strtok(rp + 2, " "); tok;
                 tok = strtok(NULL, " "), idx++) {
                if (idx == 17) { threads = atoi(tok); break; }
            }
        }
    }
    fclose(f);
    return threads;
#elif defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    int n = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == (DWORD)pid) n++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
#else
    return -1;
#endif
}

void
ns_rproc_http_close(ns_rproc_http *r)
{
    if (!r)
        return;
    http_write_request(r->wfd, "POST", "/quit", "text/plain", NULL, 0);
    if (r->inproc) {
#ifdef _WIN32
        _close(r->wfd);
        _close(r->sock);
#else
        shutdown(r->sock, SHUT_RDWR);
        close(r->sock);
#endif
        free(r->rxbuf);
        free(r->map);
        free(r);
        return;
    }
#ifdef _WIN32
    _close(r->wfd);
    WaitForSingleObject(r->process, 2000);
    TerminateProcess(r->process, 0);
    CloseHandle(r->process);
    _close(r->sock);
    if (r->map)
        UnmapViewOfFile(r->map);
    if (r->mapping)
        CloseHandle(r->mapping);
#else
    shutdown(r->sock, SHUT_WR);
    int reaped = 0;
    for (int i = 0; i < 200; i++) {
        pid_t w = waitpid(r->pid, NULL, WNOHANG);
        if (w == r->pid || (w < 0 && errno != EINTR)) {
            reaped = 1;
            break;
        }
        if (w == 0)
            usleep(10000);
    }
    if (!reaped) {
        kill(r->pid, SIGKILL);
        wait_child(r->pid);
    }
    close(r->sock);
    if (r->map)
        munmap(r->map, r->map_size);
#endif
    free(r->rxbuf);
    free(r);
}
