/* Nordstjernen — HTTP renderer IPC client (experiment; mirrors rproc.h). */

#ifndef NS_RPROC_HTTP_H
#define NS_RPROC_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ns_rproc_http ns_rproc_http;

typedef struct {
    int   ok;
    int   page_width;
    int   page_height;
    char *title;
    char *url;
    char *nav;
    int   security;
    char *remote_ip;
} ns_rproc_http_page;

typedef struct {
    int                  ok;
    int                  width;
    int                  height;
    int                  stride;
    int                  animating;
    int                  page_w;
    int                  page_h;
    int                  unchanged;
    int                  render_rc;
    const unsigned char *pixels;
    char                *nav;
    char                *webgl;
    char                *camera;
    char                *download;
    char                *audio;
    char                *window_action;
} ns_rproc_http_frame;

typedef struct {
    int   ok;
    int   changed;
    int   animating;
    int   page_w;
    int   page_h;
    char *nav;
    char *webgl;
    char *camera;
    char *download;
    char *audio;
    char *window_action;
} ns_rproc_http_tick;

ns_rproc_http *ns_rproc_http_spawn(const char *renderer_path, int max_width,
                                   int max_height);
ns_rproc_http *ns_rproc_http_spawn_shm(const char *renderer_path,
                                       int max_width, int max_height);

/* Like ns_rproc_http_spawn_shm, but when private_mode is non-zero the renderer
   is launched in private/incognito mode (ephemeral cookies/cache/storage). */
ns_rproc_http *ns_rproc_http_spawn_shm_ex(const char *renderer_path,
                                          int max_width, int max_height,
                                          int private_mode);

/* Single-process mode: when an attach hook is installed, spawn creates an
   in-process renderer connection (control channel + malloc'd framebuffer
   handed to the hook) instead of forking a renderer process. The hook
   returns 0 on success and takes ownership of the fds and framebuffer. */
typedef int (*ns_rproc_inproc_attach_fn)(int ctrl_r, int ctrl_w,
                                         unsigned char *fb, int max_w,
                                         int max_h);
void ns_rproc_http_set_inproc(ns_rproc_inproc_attach_fn attach);
int  ns_rproc_http_open(ns_rproc_http *r, const char *url, int viewport_width,
                        int viewport_height, int settle_ms,
                        ns_rproc_http_page *out);

/* Like ns_rproc_http_open, but when history is non-zero the open is a
   back/forward navigation, allowing the renderer to restore a parked
   back/forward-cache entry for url instead of refetching. */
int  ns_rproc_http_open_ex(ns_rproc_http *r, const char *url,
                           int viewport_width, int viewport_height,
                           int settle_ms, int history,
                           ns_rproc_http_page *out);
int  ns_rproc_http_render(ns_rproc_http *r, int width, int height,
                          int scroll_x, int scroll_y, double scale,
                          ns_rproc_http_frame *out);
int  ns_rproc_http_tick_page(ns_rproc_http *r, ns_rproc_http_tick *out);
void ns_rproc_http_tick_clear(ns_rproc_http_tick *out);
char *ns_rproc_http_link_at(ns_rproc_http *r, int x, int y);
char *ns_rproc_http_link_cursor_at(ns_rproc_http *r, int x, int y,
                                   char **out_cursor);
char *ns_rproc_http_click(ns_rproc_http *r, int x, int y, int mods);
char *ns_rproc_http_select(ns_rproc_http *r, int kind, int x, int y);
char *ns_rproc_http_key(ns_rproc_http *r, int kind, const char *key,
                        const char *code, int keycode, int mods);
char *ns_rproc_http_key_full(ns_rproc_http *r, int kind, const char *key,
                             const char *code, int keycode, int mods,
                             int *out_prevented);
int   ns_rproc_http_hover_full(ns_rproc_http *r, int x, int y,
                               char **out_href, char **out_cursor);
int   ns_rproc_http_scroll(ns_rproc_http *r, int x, int y, int dx, int dy);
int   ns_rproc_http_scrollbar(ns_rproc_http *r, int kind, int x, int y);
int   ns_rproc_http_drop_files(ns_rproc_http *r, int x, int y,
                               const char *const *paths, int n_paths);
int   ns_rproc_http_release(ns_rproc_http *r);
char *ns_rproc_http_release_full(ns_rproc_http *r, int *out_changed);
int   ns_rproc_http_focused_editable(ns_rproc_http *r);
char *ns_rproc_http_focused_editable_value(ns_rproc_http *r,
                                           size_t *out_caret,
                                           size_t *out_anchor);
int   ns_rproc_http_set_focused_editable_selection(ns_rproc_http *r,
                                                   size_t caret,
                                                   size_t anchor);
int   ns_rproc_http_find(ns_rproc_http *r, const char *query,
                         int case_sensitive, int direction, int from_y,
                         int *out_total, int *out_current, int *out_scroll_y);
int   ns_rproc_http_set_viewport(ns_rproc_http *r, int width, int height,
                                 ns_rproc_http_page *out);
int   ns_rproc_http_resolve_camera(ns_rproc_http *r, const char *origin,
                                   int allow);
int   ns_rproc_http_resolve_webgl(ns_rproc_http *r, const char *origin,
                                  int allow);
int   ns_rproc_http_video_event(ns_rproc_http *r, const char *token,
                                const char *kind);
char *ns_rproc_http_eval(ns_rproc_http *r, const char *src);
char *ns_rproc_http_dump(ns_rproc_http *r, const char *kind);
char *ns_rproc_http_console_poll(ns_rproc_http *r);
char *ns_rproc_http_media_at(ns_rproc_http *r, int x, int y, int *out_is_video,
                             int *out_stream);
void  ns_rproc_http_contextmenu(ns_rproc_http *r, int x, int y,
                                int *out_prevented);
int   ns_rproc_http_export(ns_rproc_http *r, const char *path);

/* Fetch and decode the current page's favicon to BGRA premultiplied pixels.
   Returns a malloc'd buffer (free() it) with *out_w x *out_h pixels at
   *out_stride bytes/row, or NULL if the page has no usable favicon. */
unsigned char *ns_rproc_http_favicon(ns_rproc_http *r, int *out_w, int *out_h,
                                     int *out_stride);

void ns_rproc_http_page_clear(ns_rproc_http_page *out);
void ns_rproc_http_close(ns_rproc_http *r);

/* Unblock a blocking IPC read in progress (called from another thread) so a
   wedged renderer cannot stall the caller; the in-flight request fails. */
void ns_rproc_http_interrupt(ns_rproc_http *r);

/* OS process id of the renderer (-1 if none), and forceful termination. */
int  ns_rproc_http_pid(ns_rproc_http *r);
void ns_rproc_http_terminate(ns_rproc_http *r);

/* Forcefully kill an OS process by pid (cross-platform). */

/* This process's own pid (for task-manager rows of in-process renderers). */
int  ns_rproc_self_pid(void);

/* Best-effort process introspection for a task-manager UI. Fills state with a
   short word ("running"/"sleeping"/"terminated"/...) and rss_kb with resident
   memory in KiB (-1 if unavailable). Returns non-zero if the process is alive.
   Full data on Linux; liveness/state only elsewhere. */
int  ns_rproc_http_proc_info(int pid, char *state, int state_sz, long *rss_kb);
double ns_rproc_http_proc_cpu(int pid);
int  ns_rproc_http_proc_threads(int pid);

#ifdef __cplusplus
}
#endif

#endif
