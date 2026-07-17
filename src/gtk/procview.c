/* Nordstjernen — GTK thin client over the out-of-process renderer (rproc). */

#include "procview.h"
#include "i18n.h"

#include "proc_limits.h"
#include "rproc_http.h"
#include "rproc_inproc.h"
#include "net.h"

#include <cairo.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <windows.h>
#endif

#ifndef G_OS_WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static int
pv_settle_ms(void)
{
    const char *e = g_getenv(NS_PROC_SETTLE_ENV);
    if (e && *e) {
        int v = atoi(e);
        if (v >= 0 && v <= 10000)
            return v;
    }
    return NS_PROC_SETTLE_MS;
}

typedef enum {
    REQ_LOAD, REQ_RENDER, REQ_LINK, REQ_CLICK, REQ_VIEWPORT, REQ_KEY,
    REQ_SELECT, REQ_HOVER, REQ_RELEASE, REQ_FIND, REQ_EXPORT, REQ_CONSOLE,
    REQ_EVAL, REQ_DUMP, REQ_DROPFILES, REQ_SCROLL, REQ_SCROLLBAR,
    REQ_WEBGL, REQ_CAMERA, REQ_FAVICON, REQ_QUIT
} ReqType;
typedef enum { ACT_HOVER, ACT_NAVIGATE, ACT_NEWTAB, ACT_CONTEXT } LinkAct;

enum {
    DEV_TAB_CONSOLE = 0,
    DEV_TAB_NETWORK,
    DEV_TAB_PERFORMANCE,
    DEV_TAB_LAYOUT,
    DEV_TAB_ELEMENTS
};

typedef struct {
    ReqType type;
    int     seq;
    char   *url;
    int     vw;
    int     vh;
    int     w, h, sx, sy;
    double  scale;
    int     x, y;
    int     dx, dy;
    int     mods;
    int     kind;
    int     keycode;
    int     fallback_scroll;
    double  fallback_x;
    double  fallback_y;
    char   *key;
    char   *code;
    LinkAct action;
    char   *query;
    int     find_dir;
    int     find_from_y;
    int     find_case;
    char   *export_dest;
    char   *paths;
    int     dump_tab;
    gboolean inspect;
    gboolean history;
} Req;

typedef enum {
    RES_PAGE, RES_FRAME, RES_LINK, RES_CLICK, RES_VIEWPORT, RES_KEY,
    RES_SELECT, RES_COPY, RES_HOVER, RES_RELEASE, RES_FIND, RES_EXPORT,
    RES_CONSOLE, RES_EVAL, RES_DUMP, RES_FAVICON, RES_DROPFILES, RES_SCROLL,
    RES_SCROLLBAR
} ResType;

typedef struct {
    NsProcView      *view;
    ResType          type;
    int              seq;
    gboolean         ok;
    int              pw, ph;
    char            *title;
    char            *url;
    char            *nav;
    int              security;
    char            *remote_ip;
    char            *webgl;
    char            *camera;
    char            *download;
    char            *audio;
    cairo_surface_t *surface;
    gboolean         surface_borrowed;
    char            *href;
    char            *cursor;
    LinkAct          action;
    int              kind;
    int              prevented;
    int              fallback_scroll;
    double           fallback_x;
    double           fallback_y;
    gboolean         animating;
    gboolean         frame_unchanged;
    int              find_total, find_current, find_scroll_y;
    char            *media_url;
    int              media_is_video, media_stream;
    unsigned char   *favicon_data;
    int              favicon_w, favicon_h, favicon_stride;
    int              dump_tab;
    gboolean         inspect;
} Res;

struct NsProcView {
    grefcount   rc;

    GtkWidget     *root;
    GtkWidget     *area;
    GtkWidget     *hscroll;
    GtkWidget     *vscroll;
    GtkIMContext  *im;
    GtkAdjustment *hadj;
    GtkAdjustment *vadj;
    gboolean    closed;

    GThread    *thread;
    GAsyncQueue *queue;
    ns_rproc_http *proc;
    GMutex      proc_lock;
    char       *renderer_path;
    gboolean    private_mode;

    GSubprocess  *audio_proc;
    GOutputStream *audio_in;

    GSubprocess      *video_proc;
    GOutputStream    *video_in;
    GDataInputStream *video_out;
    void             *vring;
    void             *vring_map;
    gsize             vring_bytes;
    char              vid_token[64];
    char              vid_shm[64];
    double            vid_x, vid_y, vid_w, vid_h;
    gboolean          vid_rect_valid;
    gboolean          vid_playing;
    guint             vid_tick_id;
    guint             vid_tick_count;
    gint64            vid_resync_us;

    NsProcNotify notify;
    gpointer     notify_ud;

    char       *current_url;
    char       *current_title;
    int         security;
    char       *remote_ip;
    int         page_w, page_h;
    int         scroll_x, scroll_y;
    gboolean    opened;

    cairo_surface_t *frame;
    cairo_surface_t *stage[2];
    int              stage_next;

    GdkPaintable    *favicon;

    gboolean    render_inflight;
    gboolean    render_pending;
    int         render_restarts;

    gboolean    link_inflight;
    gboolean    link_pending;
    int         link_pending_x, link_pending_y;
    LinkAct     link_pending_action;

    gboolean    hover_inflight;
    gboolean    hover_pending;
    int         hover_pending_x, hover_pending_y;

    gboolean    has_selection;
    double      ctx_x, ctx_y;
    char       *ctx_link;
    GtkWidget  *ctx_popover;
    GSimpleActionGroup *ctx_actions;

    GtkWidget  *search_revealer;
    GtkWidget  *search_entry;
    GtkWidget  *search_label;
    int         find_seq;
    gboolean    find_case;

    GtkWidget  *overlay;
    GtkWidget  *perm_revealer;
    GtkWidget  *perm_label;
    ReqType     perm_kind;
    gboolean    perm_pending;
    char       *perm_origin;

    GtkWidget    *console_window;
    GtkWidget    *console_notebook;
    GtkWidget    *console_entry;
    GtkWidget    *console_view;
    GtkTextBuffer *console_buffer;
    GtkWidget    *net_view;
    GtkTextBuffer *net_buffer;
    GtkWidget    *perf_view;
    GtkTextBuffer *perf_buffer;
    GtkWidget    *layout_view;
    GtkTextBuffer *layout_buffer;
    GtkWidget    *elements_view;
    GtkTextBuffer *elements_buffer;
    GtkWidget    *inspect_entry;
    gboolean      console_open;
    guint         console_poll_id;

    GPtrArray  *history;
    int         hist_index;
    gboolean    pending_record;

    char       *deferred_url;
    gboolean    deferred_record;
    gboolean    deferred_history;

    int         js_redirects;

    double      scale;
    gboolean    loading;
    gboolean    busy_cursor;
    GdkCursor  *hourglass_cursor;

    int         load_seq, render_seq, link_seq, click_seq, viewport_seq;
    int         key_seq, select_seq, hover_seq;
    int         last_vp_w, last_vp_h;
    double      drag_start_x, drag_start_y;
    double      pointer_x, pointer_y;
    gboolean    drag_anchored;
    gboolean    sb_probe, sb_dragging, sb_have_last;
    double      sb_last_x, sb_last_y;

    guint       anim_tick_id;
    gint64      last_anim_frame_us;
};

enum {
    NS_PV_ZOOM_MIN_PERMILLE = (int)(NS_PROC_ZOOM_MIN * 1000.0 + 0.5),
    NS_PV_ZOOM_MAX_PERMILLE = (int)(NS_PROC_ZOOM_MAX * 1000.0 + 0.5)
};

static NsProcView *pv_ref(NsProcView *v) { g_ref_count_inc(&v->rc); return v; }

static void
set_accessible_label(GtkWidget *w, const char *label)
{
    gtk_accessible_update_property(GTK_ACCESSIBLE(w),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

/* Closed silhouette of both glass bulbs, drawn around centre (16,16) so the
   whole glyph fits a compact 32px cursor with rounded bulbs. */
static void
hourglass_bulbs_path(cairo_t *cr, double ox, double oy)
{
    cairo_move_to(cr, 14.6 + ox, 16.0 + oy);
    cairo_curve_to(cr, 12.0 + ox, 13.2 + oy, 9.9 + ox, 10.6 + oy, 10.1 + ox, 8.0 + oy);
    cairo_curve_to(cr, 10.0 + ox, 6.8 + oy, 10.8 + ox, 6.1 + oy, 12.2 + ox, 6.1 + oy);
    cairo_curve_to(cr, 14.2 + ox, 5.6 + oy, 17.8 + ox, 5.6 + oy, 19.8 + ox, 6.1 + oy);
    cairo_curve_to(cr, 21.2 + ox, 6.1 + oy, 22.0 + ox, 6.8 + oy, 21.9 + ox, 8.0 + oy);
    cairo_curve_to(cr, 22.1 + ox, 10.6 + oy, 20.0 + ox, 13.2 + oy, 17.4 + ox, 16.0 + oy);
    cairo_close_path(cr);
    cairo_move_to(cr, 14.6 + ox, 16.0 + oy);
    cairo_curve_to(cr, 12.0 + ox, 18.8 + oy, 9.9 + ox, 21.4 + oy, 10.1 + ox, 24.0 + oy);
    cairo_curve_to(cr, 10.0 + ox, 25.2 + oy, 10.8 + ox, 25.9 + oy, 12.2 + ox, 25.9 + oy);
    cairo_curve_to(cr, 14.2 + ox, 26.4 + oy, 17.8 + ox, 26.4 + oy, 19.8 + ox, 25.9 + oy);
    cairo_curve_to(cr, 21.2 + ox, 25.9 + oy, 22.0 + ox, 25.2 + oy, 21.9 + ox, 24.0 + oy);
    cairo_curve_to(cr, 22.1 + ox, 21.4 + oy, 20.0 + ox, 18.8 + oy, 17.4 + ox, 16.0 + oy);
    cairo_close_path(cr);
}

static void
hourglass_caps_path(cairo_t *cr, double ox, double oy)
{
    cairo_move_to(cr, 9.7 + ox, 6.2 + oy);
    cairo_line_to(cr, 22.3 + ox, 6.2 + oy);
    cairo_move_to(cr, 9.7 + ox, 25.8 + oy);
    cairo_line_to(cr, 22.3 + ox, 25.8 + oy);
}

static GdkTexture *
hourglass_texture(void)
{
    const int size = 32;
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_t *cr = cairo_create(surface);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.22);
    cairo_set_line_width(cr, 3.0);
    hourglass_caps_path(cr, 0.7, 0.9);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 2.6);
    hourglass_bulbs_path(cr, 0.7, 0.9);
    cairo_stroke(cr);

    cairo_pattern_t *glass = cairo_pattern_create_linear(16.0, 7.0, 16.0, 26.0);
    cairo_pattern_add_color_stop_rgba(glass, 0.0, 0.97, 1.0, 1.0, 0.55);
    cairo_pattern_add_color_stop_rgba(glass, 0.5, 0.62, 0.80, 0.95, 0.30);
    cairo_pattern_add_color_stop_rgba(glass, 1.0, 0.93, 0.98, 1.0, 0.52);
    cairo_set_source(cr, glass);
    hourglass_bulbs_path(cr, 0.0, 0.0);
    cairo_fill(cr);
    cairo_pattern_destroy(glass);

    cairo_pattern_t *sand = cairo_pattern_create_linear(16.0, 7.0, 16.0, 25.0);
    cairo_pattern_add_color_stop_rgba(sand, 0.0, 1.0, 0.85, 0.40, 0.98);
    cairo_pattern_add_color_stop_rgba(sand, 1.0, 0.90, 0.52, 0.14, 0.98);
    cairo_set_source(cr, sand);
    cairo_move_to(cr, 11.7, 8.0);
    cairo_curve_to(cr, 14.0, 7.6, 18.0, 7.6, 20.3, 8.0);
    cairo_curve_to(cr, 18.9, 10.9, 17.4, 13.3, 16.0, 15.3);
    cairo_curve_to(cr, 14.6, 13.3, 13.1, 10.9, 11.7, 8.0);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_move_to(cr, 11.7, 24.0);
    cairo_curve_to(cr, 13.5, 21.6, 14.8, 20.9, 16.0, 20.9);
    cairo_curve_to(cr, 17.2, 20.9, 18.5, 21.6, 20.3, 24.0);
    cairo_curve_to(cr, 17.6, 24.7, 14.4, 24.7, 11.7, 24.0);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.1);
    cairo_move_to(cr, 16.0, 15.3);
    cairo_line_to(cr, 16.0, 20.9);
    cairo_stroke(cr);
    cairo_pattern_destroy(sand);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.80);
    cairo_set_line_width(cr, 0.9);
    cairo_move_to(cr, 12.6, 8.2);
    cairo_curve_to(cr, 13.7, 10.4, 14.6, 12.2, 15.3, 13.8);
    cairo_stroke(cr);
    cairo_move_to(cr, 16.7, 18.4);
    cairo_curve_to(cr, 17.6, 20.2, 18.7, 22.2, 19.6, 24.0);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.10, 0.12, 0.14, 0.98);
    cairo_set_line_width(cr, 2.0);
    hourglass_caps_path(cr, 0.0, 0.0);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1.5);
    hourglass_bulbs_path(cr, 0.0, 0.0);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.74, 0.82, 0.90, 0.85);
    cairo_set_line_width(cr, 0.6);
    hourglass_bulbs_path(cr, 0.0, 0.0);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_flush(surface);

    int stride = cairo_image_surface_get_stride(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    GBytes *bytes = g_bytes_new(data, (gsize)stride * (gsize)size);
    GdkTexture *texture =
        gdk_memory_texture_new(size, size, GDK_MEMORY_DEFAULT, bytes,
                               (gsize)stride);
    g_bytes_unref(bytes);
    cairo_surface_destroy(surface);
    return texture;
}

static GdkCursor *
busy_hourglass_cursor(NsProcView *v)
{
    if (v->hourglass_cursor)
        return v->hourglass_cursor;

    GdkTexture *texture = hourglass_texture();
    if (texture) {
        GdkCursor *fallback = gdk_cursor_new_from_name("wait", NULL);
        v->hourglass_cursor =
            gdk_cursor_new_from_texture(texture, 16, 16, fallback);
        if (fallback)
            g_object_unref(fallback);
        g_object_unref(texture);
    }
    if (!v->hourglass_cursor)
        v->hourglass_cursor = gdk_cursor_new_from_name("wait", NULL);
    return v->hourglass_cursor;
}

static void
pv_set_named_cursor(GtkWidget *w, const char *name)
{
    if (!name || !*name) {
        gtk_widget_set_cursor(w, NULL);
        return;
    }
    static const struct { const char *name; const char *fallback; } fb[] = {
        { "context-menu",  "default" },
        { "help",          "default" },
        { "progress",      "wait" },
        { "cell",          "crosshair" },
        { "vertical-text", "text" },
        { "alias",         "copy" },
        { "copy",          "default" },
        { "move",          "all-scroll" },
        { "no-drop",       "not-allowed" },
        { "not-allowed",   "default" },
        { "grab",          "all-scroll" },
        { "grabbing",      "all-scroll" },
        { "all-scroll",    "move" },
        { "col-resize",    "ew-resize" },
        { "row-resize",    "ns-resize" },
        { "ne-resize",     "nesw-resize" },
        { "sw-resize",     "nesw-resize" },
        { "nw-resize",     "nwse-resize" },
        { "se-resize",     "nwse-resize" },
        { "nesw-resize",   "crosshair" },
        { "nwse-resize",   "crosshair" },
        { "zoom-in",       "crosshair" },
        { "zoom-out",      "crosshair" },
    };
    const char *fallback = NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(fb); i++)
        if (strcmp(name, fb[i].name) == 0) { fallback = fb[i].fallback; break; }
    GdkCursor *fb_cur = fallback ? gdk_cursor_new_from_name(fallback, NULL) : NULL;
    GdkCursor *cur = gdk_cursor_new_from_name(name, fb_cur);
    gtk_widget_set_cursor(w, cur ? cur : fb_cur);
    if (cur)
        g_object_unref(cur);
    if (fb_cur)
        g_object_unref(fb_cur);
}

static void
set_busy_cursor(NsProcView *v)
{
    v->busy_cursor = TRUE;
    if (!v->area)
        return;

    GdkCursor *cursor = busy_hourglass_cursor(v);
    if (cursor)
        gtk_widget_set_cursor(v->area, cursor);
    else
        gtk_widget_set_cursor_from_name(v->area, "wait");
}

static void pv_audio_shutdown(NsProcView *v);
static void pv_video_shutdown(NsProcView *v);

static void
pv_free(NsProcView *v)
{
    pv_audio_shutdown(v);
    pv_video_shutdown(v);
    if (v->queue) {
        Req *r;
        while ((r = g_async_queue_try_pop(v->queue))) {
            g_free(r->url);
            g_free(r->key);
            g_free(r->code);
            g_free(r->query);
            g_free(r->export_dest);
            g_free(r->paths);
            g_free(r);
        }
        g_async_queue_unref(v->queue);
    }
    if (v->frame)
        cairo_surface_destroy(v->frame);
    v->frame = NULL;
    if (v->favicon)
        g_object_unref(v->favicon);
    v->favicon = NULL;
    if (v->ctx_popover)
        gtk_widget_unparent(v->ctx_popover);
    if (v->ctx_actions)
        g_object_unref(v->ctx_actions);
    g_free(v->ctx_link);
    if (v->history)
        g_ptr_array_unref(v->history);
    g_free(v->renderer_path);
    g_free(v->current_url);
    g_free(v->current_title);
    g_free(v->remote_ip);
    g_free(v->deferred_url);
    g_free(v->perm_origin);
    if (v->hourglass_cursor)
        g_object_unref(v->hourglass_cursor);
    g_mutex_clear(&v->proc_lock);
    g_free(v);
}

static void pv_unref(NsProcView *v) { if (g_ref_count_dec(&v->rc)) pv_free(v); }

/* Atomically install a new renderer handle (worker thread only) and return the
   previous one for the caller to close outside the lock. The lock serialises
   the worker's reassignments against the main thread's close-time interrupt so
   it can never touch a freed handle. */
static ns_rproc_http *
pv_swap_proc(NsProcView *v, ns_rproc_http *newp)
{
    g_mutex_lock(&v->proc_lock);
    ns_rproc_http *old = v->proc;
    v->proc = newp;
    g_mutex_unlock(&v->proc_lock);
    return old;
}

char *
ns_proc_renderer_path(void)
{
    const char *env = g_getenv(NS_PROC_RENDERER_ENV);
    if (env && *env)
        return g_strdup(env);
#ifdef G_OS_WIN32
    const char *name = NS_PROC_RENDERER_NAME ".exe";
#else
    const char *name = NS_PROC_RENDERER_NAME;
#endif
    const char *exe = ns_app_self_exe();
    if (exe) {
        char *dir = g_path_get_dirname(exe);
        char *parent = g_build_filename("..", name, NULL);
        const char *rel[] = { name, parent, NULL };
        for (int i = 0; rel[i]; i++) {
            char *cand = g_build_filename(dir, rel[i], NULL);
            if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) {
                g_free(parent);
                g_free(dir);
                return cand;
            }
            g_free(cand);
        }
        g_free(parent);
        g_free(dir);
    }
    return g_strdup(name);
}

static char *
ns_proc_audio_helper_path(void)
{
#ifdef G_OS_WIN32
    const char *name = "nordstjernen-audio.exe";
#else
    const char *name = "nordstjernen-audio";
#endif
    const char *exe = ns_app_self_exe();
    if (exe) {
        char *dir = g_path_get_dirname(exe);
        char *parent = g_build_filename("..", name, NULL);
        const char *rel[] = { name, parent, NULL };
        for (int i = 0; rel[i]; i++) {
            char *cand = g_build_filename(dir, rel[i], NULL);
            if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) {
                g_free(parent);
                g_free(dir);
                return cand;
            }
            g_free(cand);
        }
        g_free(parent);
        g_free(dir);
    }
    return g_strdup(name);
}

static void
ns_proc_audio_apply_proxy_env(GSubprocessLauncher *launcher)
{
    const char *override = ns_net_proxy_override();
    if (override && *override) {
        g_subprocess_launcher_setenv(launcher, "all_proxy", override, TRUE);
        g_subprocess_launcher_setenv(launcher, "http_proxy", override, TRUE);
        g_subprocess_launcher_setenv(launcher, "https_proxy", override, TRUE);
    } else {
        const char *http_proxy = ns_net_http_proxy();
        const char *https_proxy = ns_net_https_proxy();
        if (http_proxy && *http_proxy)
            g_subprocess_launcher_setenv(launcher, "http_proxy", http_proxy, TRUE);
        if (https_proxy && *https_proxy)
            g_subprocess_launcher_setenv(launcher, "https_proxy", https_proxy, TRUE);
    }
    const char *no_proxy = ns_net_no_proxy();
    if (no_proxy && *no_proxy)
        g_subprocess_launcher_setenv(launcher, "no_proxy", no_proxy, TRUE);
    const char *ca = ns_net_ca_bundle_path();
    if (ca && *ca)
        g_subprocess_launcher_setenv(launcher, "CURL_CA_BUNDLE", ca, TRUE);
}

static void pv_video_send(NsProcView *v, const char *cmd);

static void
pv_audio_feedback_line(GObject *src, GAsyncResult *res, gpointer user_data)
{
    NsProcView *v = user_data;
    GDataInputStream *in = G_DATA_INPUT_STREAM(src);
    char *line = g_data_input_stream_read_line_finish(in, res, NULL, NULL);
    if (!line) {
        g_object_unref(in);
        if (v) pv_unref(v);
        return;
    }
    if (g_str_has_prefix(line, "error ") || g_getenv("NS_DBG_AUDIO"))
        g_printerr("[audio-helper] %s\n", line);
    if (v && v->video_proc && v->vid_playing && v->vid_token[0] &&
        g_str_has_prefix(line, "pos ")) {
        char tok[64], valstr[64];
        if (sscanf(line + 4, "%63s %63s", tok, valstr) == 2 &&
            strcmp(tok, v->vid_token) == 0) {
            double sec = g_ascii_strtod(valstr, NULL);
            gint64 nowu = g_get_monotonic_time();
            if (sec >= 0.0 && nowu - v->vid_resync_us > 950000) {
                v->vid_resync_us = nowu;
                char valbuf[G_ASCII_DTOSTR_BUF_SIZE];
                g_ascii_formatd(valbuf, sizeof valbuf, "%.3f", sec);
                char cmd[96];
                g_snprintf(cmd, sizeof cmd, "resync %s %s", tok, valbuf);
                pv_video_send(v, cmd);
            }
        }
    }
    g_free(line);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL,
                                        pv_audio_feedback_line, v);
}

static void
pv_audio_pump(NsProcView *v, const char *commands)
{
    if (!commands || !*commands) return;
    if (!v->audio_proc) {
        char *path = ns_proc_audio_helper_path();
        GError *err = NULL;
        GSubprocessLauncher *launcher = g_subprocess_launcher_new(
            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_SILENCE);
        ns_proc_audio_apply_proxy_env(launcher);
        v->audio_proc = g_subprocess_launcher_spawn(launcher, &err, path, NULL);
        g_object_unref(launcher);
        if (!v->audio_proc) {
            g_printerr("nordstjernen: audio helper %s failed to start: %s\n",
                       path, err ? err->message : "unknown error");
            g_free(path);
            g_clear_error(&err);
            return;
        }
        if (g_getenv("NS_DBG_AUDIO"))
            g_printerr("[audio-pump] spawn %s -> ok\n", path);
        g_free(path);
        v->audio_in = g_subprocess_get_stdin_pipe(v->audio_proc);
        GInputStream *feedback = g_subprocess_get_stdout_pipe(v->audio_proc);
        if (feedback)
            g_data_input_stream_read_line_async(
                g_data_input_stream_new(feedback), G_PRIORITY_DEFAULT, NULL,
                pv_audio_feedback_line, pv_ref(v));
    }
    if (!v->audio_in) return;

    char **lines = g_strsplit(commands, "\x1f", -1);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i]) continue;
        char *line = g_strconcat(lines[i], "\n", NULL);
        if (g_getenv("NS_DBG_AUDIO"))
            g_printerr("[audio-pump] cmd: %s", line);
        g_output_stream_write_all(v->audio_in, line, strlen(line),
                                  NULL, NULL, NULL);
        g_free(line);
    }
    g_output_stream_flush(v->audio_in, NULL, NULL);
    g_strfreev(lines);
}

static void
pv_audio_shutdown(NsProcView *v)
{
    if (!v->audio_proc) return;
    if (v->audio_in) {
        g_output_stream_write_all(v->audio_in, "quit\n", 5, NULL, NULL, NULL);
        g_output_stream_flush(v->audio_in, NULL, NULL);
    }
    g_subprocess_force_exit(v->audio_proc);
    g_clear_object(&v->audio_proc);
    v->audio_in = NULL;
}

typedef struct {
    guint32 magic;
    guint32 width;
    guint32 height;
    guint32 stride;
    guint32 nslots;
    volatile guint32 latest;
    volatile guint32 writing;
    guint32 frame_bytes;
    double  pts[3];
} ns_vring_hdr;

#define NS_VRING_MAGIC 0x4e535646u

static char *
ns_proc_video_helper_path(void)
{
#ifdef G_OS_WIN32
    const char *name = "nordstjernen-video.exe";
#else
    const char *name = "nordstjernen-video";
#endif
    const char *exe = ns_app_self_exe();
    if (exe) {
        char *dir = g_path_get_dirname(exe);
        char *parent = g_build_filename("..", name, NULL);
        const char *rel[] = { name, parent, NULL };
        for (int i = 0; rel[i]; i++) {
            char *cand = g_build_filename(dir, rel[i], NULL);
            if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) {
                g_free(parent);
                g_free(dir);
                return cand;
            }
            g_free(cand);
        }
        g_free(parent);
        g_free(dir);
    }
    return g_strdup(name);
}

gboolean
ns_proc_video_helper_available(void)
{
    char *path = ns_proc_video_helper_path();
    gboolean ok = g_path_is_absolute(path)
        ? g_file_test(path, G_FILE_TEST_IS_EXECUTABLE)
        : FALSE;
    if (!ok && !g_path_is_absolute(path)) {
        char *found = g_find_program_in_path(path);
        ok = found != NULL;
        g_free(found);
    }
    g_free(path);
    return ok;
}

static void
pv_vring_unmap(NsProcView *v)
{
#ifdef G_OS_WIN32
    if (v->vring) UnmapViewOfFile(v->vring);
    if (v->vring_map) CloseHandle(v->vring_map);
    v->vring_map = NULL;
#else
    if (v->vring) munmap(v->vring, v->vring_bytes);
#endif
    v->vring = NULL;
    v->vring_bytes = 0;
    v->vid_shm[0] = '\0';
    v->vid_rect_valid = FALSE;
    v->vid_playing = FALSE;
}

static void request_render(NsProcView *v);

static gboolean
pv_video_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)clock;
    NsProcView *v = data;
    if (!v->vring || !v->vid_playing) {
        v->vid_tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    gtk_widget_queue_draw(widget);
    if (++v->vid_tick_count % 60 == 0)
        request_render(v);
    if (v->vid_tick_count % 60 == 0 && v->audio_in) {
        g_output_stream_write_all(v->audio_in, "poll\n", 5, NULL, NULL, NULL);
        g_output_stream_flush(v->audio_in, NULL, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static void
pv_video_ensure_tick(NsProcView *v)
{
    if (!v->vid_tick_id && v->vring && v->vid_playing)
        v->vid_tick_id = gtk_widget_add_tick_callback(v->area, pv_video_tick,
                                                      v, NULL);
}

static void
pv_video_handle_line(NsProcView *v, const char *line)
{
    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[video-helper] %s\n", line);
    char **tok = g_strsplit(line, " ", 8);
    guint n = g_strv_length(tok);
    if (n >= 6 && strcmp(tok[0], "shm") == 0 &&
        strcmp(tok[1], v->vid_token) != 0) {
        if (g_getenv("NS_DBG_AUDIO"))
            g_printerr("[shm-reject] line-tok=%s cur-tok=%s\n",
                       tok[1], v->vid_token);
    } else if (n >= 6 && strcmp(tok[0], "shm") == 0) {
#ifdef G_OS_WIN32
        pv_vring_unmap(v);
        HANDLE hm = OpenFileMappingA(FILE_MAP_READ, FALSE, tok[2]);
        if (hm) {
            void *map = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
            MEMORY_BASIC_INFORMATION mbi;
            gsize view_bytes = (map && VirtualQuery(map, &mbi, sizeof mbi))
                                   ? (gsize)mbi.RegionSize : 0;
            if (map && view_bytes > sizeof(ns_vring_hdr) &&
                ((ns_vring_hdr *)map)->magic == NS_VRING_MAGIC) {
                v->vring = map;
                v->vring_map = hm;
                v->vring_bytes = view_bytes;
                g_strlcpy(v->vid_shm, tok[2], sizeof v->vid_shm);
                g_strlcpy(v->vid_token, tok[1], sizeof v->vid_token);
            } else {
                if (map) UnmapViewOfFile(map);
                CloseHandle(hm);
            }
        }
        pv_video_ensure_tick(v);
#else
        if (g_getenv("NS_DBG_AUDIO"))
            g_printerr("[shm-adopt] %s %s\n", tok[1], tok[2]);
        pv_vring_unmap(v);
        int fd = shm_open(tok[2], O_RDONLY, 0);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size > (off_t)sizeof(ns_vring_hdr)) {
                void *map = mmap(NULL, (size_t)st.st_size, PROT_READ,
                                 MAP_SHARED, fd, 0);
                if (map != MAP_FAILED) {
                    ns_vring_hdr *hdr = map;
                    if (hdr->magic == NS_VRING_MAGIC) {
                        v->vring = map;
                        v->vring_bytes = (gsize)st.st_size;
                        g_strlcpy(v->vid_shm, tok[2], sizeof v->vid_shm);
                        g_strlcpy(v->vid_token, tok[1], sizeof v->vid_token);
                    } else {
                        munmap(map, (size_t)st.st_size);
                    }
                }
            }
            close(fd);
        }
        pv_video_ensure_tick(v);
#endif
    } else if (n >= 2 && strcmp(tok[0], "closed") == 0) {
        if (strcmp(tok[1], v->vid_token) == 0)
            pv_vring_unmap(v);
    } else if (n >= 2 && strcmp(tok[0], "playing") == 0) {
        if (strcmp(tok[1], v->vid_token) == 0) {
            v->vid_playing = TRUE;
            pv_video_ensure_tick(v);
        }
    } else if (n >= 2 && (strcmp(tok[0], "paused") == 0 ||
                          strcmp(tok[0], "ended") == 0)) {
        if (strcmp(tok[1], v->vid_token) == 0) {
            v->vid_playing = FALSE;
            gtk_widget_queue_draw(v->area);
        }
    }
    g_strfreev(tok);
}

static void
pv_video_read_line(GObject *src, GAsyncResult *res, gpointer data);

static void
pv_video_read_next(NsProcView *v)
{
    if (!v->video_out) return;
    g_data_input_stream_read_line_async(v->video_out, G_PRIORITY_DEFAULT,
                                        NULL, pv_video_read_line, pv_ref(v));
}

static void
pv_video_read_line(GObject *src, GAsyncResult *res, gpointer data)
{
    NsProcView *v = data;
    char *line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(src), res, NULL, NULL);
    if (line && v->video_out) {
        pv_video_handle_line(v, line);
        g_free(line);
        pv_video_read_next(v);
    } else {
        g_free(line);
    }
    pv_unref(v);
}

static void
pv_video_send(NsProcView *v, const char *cmd)
{
    if (!v->video_proc) {
        char *path = ns_proc_video_helper_path();
        GError *err = NULL;
        GSubprocessLauncher *launcher = g_subprocess_launcher_new(
            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_SILENCE);
        v->video_proc = g_subprocess_launcher_spawn(launcher, &err, path, NULL);
        g_object_unref(launcher);
        if (g_getenv("NS_DBG_AUDIO"))
            g_printerr("[video-pump] spawn %s -> %s (%s)\n", path,
                       v->video_proc ? "ok" : "FAIL",
                       err ? err->message : "-");
        g_free(path);
        if (!v->video_proc) {
            g_clear_error(&err);
            return;
        }
        v->video_in = g_subprocess_get_stdin_pipe(v->video_proc);
        GInputStream *out = g_subprocess_get_stdout_pipe(v->video_proc);
        v->video_out = g_data_input_stream_new(out);
        pv_video_read_next(v);
    }
    if (!v->video_in) return;
    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[video-pump] cmd: %s\n", cmd);
    char *line = g_strconcat(cmd, "\n", NULL);
    g_output_stream_write_all(v->video_in, line, strlen(line),
                              NULL, NULL, NULL);
    g_output_stream_flush(v->video_in, NULL, NULL);
    g_free(line);
}

static void
pv_video_dispatch(NsProcView *v, const char *cmd)
{
    if (g_str_has_prefix(cmd, "rect ")) {
        char token[64];
        int x, y, w, h;
        if (sscanf(cmd + 5, "%63s %d %d %d %d",
                   token, &x, &y, &w, &h) == 5 &&
            (strcmp(token, v->vid_token) == 0 || !v->vid_token[0])) {
            v->vid_x = x;
            v->vid_y = y;
            v->vid_w = w;
            v->vid_h = h;
            v->vid_rect_valid = w > 0 && h > 0;
            if (g_getenv("NS_DBG_AUDIO"))
                g_printerr("[video-rect] %d,%d %dx%d valid=%d\n",
                           x, y, w, h, v->vid_rect_valid);
            gtk_widget_queue_draw(v->area);
        }
        return;
    }
    char cmd_tok[64] = "";
    if (g_str_has_prefix(cmd, "open ")) {
        sscanf(cmd + 5, "%63s", v->vid_token);
        v->vid_rect_valid = FALSE;
    } else if (g_str_has_prefix(cmd, "play ")) {
        sscanf(cmd + 5, "%63s", cmd_tok);
        if (strcmp(cmd_tok, v->vid_token) == 0)
            v->vid_playing = TRUE;
    } else if (g_str_has_prefix(cmd, "pause ") ||
               g_str_has_prefix(cmd, "stop ")) {
        sscanf(cmd + (cmd[0] == 'p' ? 6 : 5), "%63s", cmd_tok);
        if (strcmp(cmd_tok, v->vid_token) == 0)
            v->vid_playing = FALSE;
    }
    pv_video_send(v, cmd);
    pv_video_ensure_tick(v);
}

static void
pv_video_shutdown(NsProcView *v)
{
    pv_vring_unmap(v);
    if (!v->video_proc) return;
    if (v->video_in) {
        g_output_stream_write_all(v->video_in, "quit\n", 5, NULL, NULL, NULL);
        g_output_stream_flush(v->video_in, NULL, NULL);
    }
    g_clear_object(&v->video_out);
    g_subprocess_force_exit(v->video_proc);
    g_clear_object(&v->video_proc);
    v->video_in = NULL;
}

static void
pv_append_proc_threads(GString *out, int pid)
{
#ifdef __linux__
    char *taskdir = g_strdup_printf("/proc/%d/task", pid);
    GDir *dir = g_dir_open(taskdir, 0, NULL);
    if (dir) {
        long hz = sysconf(_SC_CLK_TCK);
        if (hz <= 0) hz = 100;
        const char *tid;
        while ((tid = g_dir_read_name(dir))) {
            char *stat_path = g_strdup_printf("%s/%s/stat", taskdir, tid);
            char *stat = NULL;
            if (g_file_get_contents(stat_path, &stat, NULL, NULL)) {
                char *close = strrchr(stat, ')');
                char *open = strchr(stat, '(');
                if (open && close && close > open) {
                    char name[32] = {0};
                    g_strlcpy(name, open + 1,
                              MIN((gsize)(close - open), sizeof name));
                    char state = 0;
                    unsigned long utime = 0, stime = 0;
                    if (sscanf(close + 1,
                               " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                               "%lu %lu", &state, &utime, &stime) >= 1)
                        g_string_append_printf(
                            out, "    tid %-7s %-16s %c  cpu %.2fs\n",
                            tid, name, state,
                            (double)(utime + stime) / (double)hz);
                }
                g_free(stat);
            }
            g_free(stat_path);
        }
        g_dir_close(dir);
    }
    g_free(taskdir);
#else
    (void)out; (void)pid;
#endif
}

static void
pv_append_media_process_stats(NsProcView *v, GString *out)
{
    struct { const char *label; int pid; } procs[] = {
        { "audio helper (nordstjernen-audio)", ns_proc_view_audio_pid(v) },
        { "video helper (nordstjernen-video)", ns_proc_view_video_pid(v) },
    };
    gboolean any = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(procs); i++) {
        if (procs[i].pid <= 0) continue;
        if (!any) {
            g_string_append(out, "\n\n== Media helper processes ==\n");
            any = TRUE;
        }
        char state[32] = "";
        long rss = -1;
        ns_rproc_http_proc_info(procs[i].pid, state, sizeof state, &rss);
        g_string_append_printf(out, "%s  pid %d  %s  rss %.1f MB\n",
                               procs[i].label, procs[i].pid, state,
                               rss >= 0 ? rss / 1024.0 : 0.0);
        pv_append_proc_threads(out, procs[i].pid);
    }
    if (any && v->vring) {
        ns_vring_hdr *r = v->vring;
        g_string_append_printf(out,
                               "video ring %ux%u stride %u slot %d pts %.2fs\n",
                               r->width, r->height, r->stride,
                               r->latest == G_MAXUINT32 ? -1 : (int)r->latest,
                               r->latest < G_N_ELEMENTS(r->pts) ? r->pts[r->latest]
                                                                : 0.0);
    }
}

static void
pv_media_pump(NsProcView *v, const char *commands)
{
    if (!commands || !*commands) return;
    char **lines = g_strsplit(commands, "\x1f", -1);
    GString *audio = g_string_new(NULL);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i]) continue;
        if (g_str_has_prefix(lines[i], "video "))
            pv_video_dispatch(v, lines[i] + 6);
        else {
            g_string_append(audio, lines[i]);
            g_string_append_c(audio, '\x1f');
        }
    }
    g_strfreev(lines);
    if (audio->len)
        pv_audio_pump(v, audio->str);
    g_string_free(audio, TRUE);
}

static cairo_surface_t *
stage_fill(NsProcView *v, const unsigned char *px, int w, int h, int stride)
{
    (void)v;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    cairo_surface_flush(s);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int dstride = cairo_image_surface_get_stride(s);
    size_t row = (size_t)w * 4u;
    if (dstride == stride && (size_t)stride == row) {
        memcpy(dst, px, row * (size_t)h);
    } else {
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * dstride, px + (size_t)y * stride, row);
    }
    cairo_surface_mark_dirty(s);
    return s;
}

static void
post_emit(NsProcView *v, NsProcEvent evt, const char *text)
{
    if (v->notify)
        v->notify(v, evt, text, v->notify_ud);
}

static void
clear_busy_cursor(NsProcView *v)
{
    if (!v->busy_cursor)
        return;
    v->busy_cursor = FALSE;
    if (v->area)
        gtk_widget_set_cursor_from_name(v->area, NULL);
}

static void
finish_loading(NsProcView *v)
{
    if (v->loading) {
        v->loading = FALSE;
        post_emit(v, NS_PROC_EVT_LOADING, "0");
    }
}

static gboolean on_result(gpointer data);

static void
post(Res *res)
{
    g_idle_add(on_result, res);
}

static gpointer
worker_main(gpointer data)
{
    NsProcView *v = data;
    for (;;) {
        Req *req = g_async_queue_pop(v->queue);
        if (req->type == REQ_QUIT) {
            g_free(req->url);
            g_free(req);
            break;
        }
        if (!v->proc && !v->closed)
            pv_swap_proc(v, ns_rproc_http_spawn_shm_ex(v->renderer_path,
                                     NS_PROC_MAX_WIDTH, NS_PROC_MAX_HEIGHT,
                                     v->private_mode));

        if (req->type == REQ_LOAD) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_PAGE;
            res->seq = req->seq;
            ns_rproc_http_page pg;
            int settle = pv_settle_ms();
            int rc = v->proc ? ns_rproc_http_open_ex(v->proc, req->url, req->vw,
                                             req->vh, settle, req->history, &pg)
                             : -1;
            if (rc != 0 && v->proc && !v->closed) {
                ns_rproc_http_close(pv_swap_proc(v, NULL));
                pv_swap_proc(v, ns_rproc_http_spawn_shm_ex(v->renderer_path,
                                         NS_PROC_MAX_WIDTH, NS_PROC_MAX_HEIGHT,
                                         v->private_mode));
                rc = v->proc ? ns_rproc_http_open_ex(v->proc, req->url, req->vw,
                                             req->vh, settle, req->history, &pg)
                             : -1;
            }
            if (rc == 0 && pg.ok) {
                res->ok = TRUE;
                res->pw = pg.page_width;
                res->ph = pg.page_height;
                res->title = g_strdup(pg.title ? pg.title : "");
                res->url = g_strdup(pg.url ? pg.url : req->url);
                res->nav = pg.nav ? g_strdup(pg.nav) : NULL;
                res->security = pg.security;
                res->remote_ip = pg.remote_ip ? g_strdup(pg.remote_ip) : NULL;
            }
            if (rc == 0)
                ns_rproc_http_page_clear(&pg);
            post(res);
        } else if (req->type == REQ_RENDER) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_FRAME;
            res->seq = req->seq;
            ns_rproc_http_frame fr;
            gboolean rendered = v->proc &&
                ns_rproc_http_render(v->proc, req->w, req->h, req->sx, req->sy,
                                req->scale, &fr) == 0 && fr.ok;
            if (rendered) {
                res->ok = TRUE;
                res->animating = fr.animating ? TRUE : FALSE;
                res->pw = fr.page_w;
                res->ph = fr.page_h;
                res->frame_unchanged = fr.unchanged ? TRUE : FALSE;
                if (!fr.unchanged) {
                    res->surface = stage_fill(v, fr.pixels, fr.width,
                                              fr.height, fr.stride);
                    res->surface_borrowed = FALSE;
                }
                if (fr.nav) {
                    res->nav = g_strdup(fr.nav);
                    free(fr.nav);
                }
                if (fr.webgl) {
                    res->webgl = g_strdup(fr.webgl);
                    free(fr.webgl);
                }
                if (fr.camera) {
                    res->camera = g_strdup(fr.camera);
                    free(fr.camera);
                }
                if (fr.download) {
                    res->download = g_strdup(fr.download);
                    free(fr.download);
                }
                if (fr.audio) {
                    res->audio = g_strdup(fr.audio);
                    free(fr.audio);
                }
            } else if (v->proc) {
                ns_rproc_http_close(pv_swap_proc(v, NULL));
            }
            post(res);
        } else if (req->type == REQ_LINK) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_LINK;
            res->seq = req->seq;
            res->action = req->action;
            if (v->proc && req->action == ACT_HOVER)
                res->href = ns_rproc_http_link_cursor_at(v->proc, req->x,
                                                         req->y, &res->cursor);
            else if (v->proc && req->action == ACT_CONTEXT) {
                int prevented = 0;
                ns_rproc_http_contextmenu(v->proc, req->x, req->y, &prevented);
                res->prevented = prevented;
                if (!prevented)
                    res->href = ns_rproc_http_link_at(v->proc, req->x, req->y);
            }
            else if (v->proc)
                res->href = ns_rproc_http_link_at(v->proc, req->x, req->y);
            post(res);
        } else if (req->type == REQ_CLICK) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_CLICK;
            res->seq = req->seq;
            res->href = v->proc
                ? ns_rproc_http_click(v->proc, req->x, req->y, req->mods)
                : NULL;
            post(res);
        } else if (req->type == REQ_VIEWPORT) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_VIEWPORT;
            res->seq = req->seq;
            ns_rproc_http_page pg;
            if (v->proc &&
                ns_rproc_http_set_viewport(v->proc, req->vw, req->vh, &pg) == 0) {
                res->ok = pg.ok;
                res->pw = pg.page_width;
                res->ph = pg.page_height;
                ns_rproc_http_page_clear(&pg);
            }
            post(res);
        } else if (req->type == REQ_KEY) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_KEY;
            res->seq = req->seq;
            res->kind = req->kind;
            res->fallback_scroll = req->fallback_scroll;
            res->fallback_x = req->fallback_x;
            res->fallback_y = req->fallback_y;
            res->href = v->proc
                ? ns_rproc_http_key_full(v->proc, req->kind, req->key,
                               req->code, req->keycode, req->mods,
                               &res->prevented)
                : NULL;
            post(res);
        } else if (req->type == REQ_SELECT) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = (req->kind == 4) ? RES_COPY : RES_SELECT;
            res->seq = req->seq;
            res->href = v->proc
                ? ns_rproc_http_select(v->proc, req->kind, req->x, req->y)
                : NULL;
            post(res);
        } else if (req->type == REQ_HOVER) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_HOVER;
            res->seq = req->seq;
            if (v->proc)
                res->ok = ns_rproc_http_hover_full(v->proc, req->x, req->y,
                                                   &res->href,
                                                   &res->cursor) == 1;
            post(res);
        } else if (req->type == REQ_SCROLL) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_SCROLL;
            res->seq = req->seq;
            res->ok = v->proc
                ? ns_rproc_http_scroll(v->proc, req->x, req->y,
                                       req->dx, req->dy)
                : 0;
            res->fallback_x = req->fallback_x;
            res->fallback_y = req->fallback_y;
            post(res);
        } else if (req->type == REQ_SCROLLBAR) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_SCROLLBAR;
            res->seq = req->seq;
            res->kind = req->kind;
            res->ok = v->proc
                ? ns_rproc_http_scrollbar(v->proc, req->kind, req->x, req->y)
                : 0;
            post(res);
        } else if (req->type == REQ_DROPFILES) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_DROPFILES;
            res->seq = req->seq;
            if (v->proc && req->paths && *req->paths) {
                char **list = g_strsplit(req->paths, "\n", -1);
                guint count = list ? g_strv_length(list) : 0;
                if (count > 0)
                    res->ok = ns_rproc_http_drop_files(
                        v->proc, req->x, req->y,
                        (const char *const *)list, (int)count) == 1;
                g_strfreev(list);
            }
            post(res);
        } else if (req->type == REQ_RELEASE) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_RELEASE;
            res->seq = req->seq;
            res->href = v->proc
                ? ns_rproc_http_release_full(v->proc, &res->ok)
                : NULL;
            if (v->proc && (!res->href || !*res->href))
                res->media_url = ns_rproc_http_media_at(v->proc, req->x, req->y,
                                                   &res->media_is_video,
                                                   &res->media_stream);
            post(res);
        } else if (req->type == REQ_FIND) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_FIND;
            res->seq = req->seq;
            if (v->proc)
                ns_rproc_http_find(v->proc, req->query, req->find_case,
                              req->find_dir, req->find_from_y,
                              &res->find_total, &res->find_current,
                              &res->find_scroll_y);
            post(res);
        } else if (req->type == REQ_EXPORT) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_EXPORT;
            res->seq = req->seq;
            gboolean ok = FALSE;
            if (v->proc && req->url && req->export_dest &&
                ns_rproc_http_export(v->proc, req->url) == 0) {
                GFile *src = g_file_new_for_path(req->url);
                GFile *dst = g_file_new_for_path(req->export_dest);
                ok = g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL,
                                 NULL, NULL, NULL);
                g_object_unref(src);
                g_object_unref(dst);
            }
            if (req->url)
                g_unlink(req->url);
            res->ok = ok;
            res->url = g_strdup(req->export_dest ? req->export_dest : "");
            post(res);
        } else if (req->type == REQ_CONSOLE) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_CONSOLE;
            res->seq = req->seq;
            res->href = v->proc ? ns_rproc_http_console_poll(v->proc) : NULL;
            post(res);
        } else if (req->type == REQ_EVAL) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_EVAL;
            res->seq = req->seq;
            res->dump_tab = req->dump_tab;
            res->inspect = req->inspect;
            res->href = v->proc ? ns_rproc_http_eval(v->proc, req->query) : NULL;
            post(res);
        } else if (req->type == REQ_DUMP) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_DUMP;
            res->seq = req->seq;
            res->dump_tab = req->dump_tab;
            res->href = v->proc ? ns_rproc_http_dump(v->proc, req->query) : NULL;
            post(res);
        } else if (req->type == REQ_WEBGL) {
            if (v->proc)
                ns_rproc_http_resolve_webgl(v->proc, req->url, req->mods);
        } else if (req->type == REQ_CAMERA) {
            if (v->proc)
                ns_rproc_http_resolve_camera(v->proc, req->url, req->mods);
        } else if (req->type == REQ_FAVICON) {
            Res *res = g_new0(Res, 1);
            res->view = pv_ref(v);
            res->type = RES_FAVICON;
            res->seq = req->seq;
            if (v->proc)
                res->favicon_data = ns_rproc_http_favicon(
                    v->proc, &res->favicon_w, &res->favicon_h,
                    &res->favicon_stride);
            post(res);
        }
        g_free(req->url);
        g_free(req->key);
        g_free(req->code);
        g_free(req->query);
        g_free(req->export_dest);
        g_free(req->paths);
        g_free(req);
    }
    if (v->proc)
        ns_rproc_http_close(pv_swap_proc(v, NULL));
    pv_unref(v);
    return NULL;
}

static void
push_req(NsProcView *v, Req *req)
{
    g_async_queue_push(v->queue, req);
}

static void
request_favicon(NsProcView *v)
{
    Req *req = g_new0(Req, 1);
    req->type = REQ_FAVICON;
    req->seq = v->load_seq;
    push_req(v, req);
}

static int
viewport_w(NsProcView *v)
{
    int w = v->area ? gtk_widget_get_width(v->area) : 0;
    return w > 0 ? w : 1;
}

static int
viewport_h(NsProcView *v)
{
    int h = v->area ? gtk_widget_get_height(v->area) : 0;
    return h > 0 ? h : 1;
}

static double
cur_scale(NsProcView *v)
{
    return v->scale > 0.0 ? v->scale : 1.0;
}


static void
configure_adjustments(NsProcView *v)
{
    double s = cur_scale(v);
    double cw = viewport_w(v) / s;
    double ch = viewport_h(v) / s;
    gboolean can_scroll_x = v->page_w > cw + 0.5;
    gboolean can_scroll_y = v->page_h > ch + 0.5;
    if (!can_scroll_x) v->scroll_x = 0;
    if (!can_scroll_y) v->scroll_y = 0;
    if (v->hscroll) gtk_widget_set_visible(v->hscroll, can_scroll_x);
    if (v->vscroll) gtk_widget_set_visible(v->vscroll, can_scroll_y);
    double upper_w = v->page_w > cw ? v->page_w : cw;
    double upper_h = v->page_h > ch ? v->page_h : ch;
    gtk_adjustment_configure(v->hadj, v->scroll_x, 0, upper_w, 60, cw, cw);
    gtk_adjustment_configure(v->vadj, v->scroll_y, 0, upper_h, 60, ch, ch);
    v->scroll_x = (int)gtk_adjustment_get_value(v->hadj);
    v->scroll_y = (int)gtk_adjustment_get_value(v->vadj);
}

static void
on_adj_changed(GtkAdjustment *adj, gpointer data)
{
    (void)adj;
    NsProcView *v = data;
    if (v->closed)
        return;
    v->scroll_x = (int)gtk_adjustment_get_value(v->hadj);
    v->scroll_y = (int)gtk_adjustment_get_value(v->vadj);
    if (v->opened)
        request_render(v);
}

static void start_render(NsProcView *v);

static void
request_render(NsProcView *v)
{
    if (!v->opened)
        return;
    if (v->render_inflight) {
        v->render_pending = TRUE;
        return;
    }
    start_render(v);
}

static void
start_render(NsProcView *v)
{
    if (!v->opened)
        return;
    v->render_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_RENDER;
    req->seq = ++v->render_seq;
    req->w = viewport_w(v);
    req->h = viewport_h(v);
    req->sx = v->scroll_x;
    req->sy = v->scroll_y;
    req->scale = cur_scale(v);
    push_req(v, req);
}

static gboolean
anim_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    (void)widget;
    (void)clock;
    NsProcView *v = data;
    if (v->closed || !v->opened) {
        v->anim_tick_id = 0;
        v->last_anim_frame_us = 0;
        return G_SOURCE_REMOVE;
    }
    gint64 now = clock ? gdk_frame_clock_get_frame_time(clock) : 0;
    if (now <= 0) now = g_get_monotonic_time();
    if (v->last_anim_frame_us > 0 &&
        now - v->last_anim_frame_us < G_USEC_PER_SEC / 60)
        return G_SOURCE_CONTINUE;
    v->last_anim_frame_us = now;
    request_render(v);
    return G_SOURCE_CONTINUE;
}

static void
arm_anim(NsProcView *v)
{
    if (v->anim_tick_id || v->closed || !v->area)
        return;
    v->last_anim_frame_us = 0;
    v->anim_tick_id = gtk_widget_add_tick_callback(v->area, anim_tick, v, NULL);
}

static void
disarm_anim(NsProcView *v)
{
    if (v->anim_tick_id && v->area)
        gtk_widget_remove_tick_callback(v->area, v->anim_tick_id);
    v->anim_tick_id = 0;
    v->last_anim_frame_us = 0;
}

static void start_link(NsProcView *v, int x, int y, LinkAct action);
static void show_context_menu(NsProcView *v, const char *href);
static void build_search_bar(NsProcView *v);
static void console_append(NsProcView *v, const char *text);
static void console_set_open(NsProcView *v, gboolean open);

static void
request_link(NsProcView *v, int x, int y, LinkAct action)
{
    if (!v->opened)
        return;
    if (v->link_inflight) {
        if (action != ACT_HOVER || v->link_pending_action == ACT_HOVER) {
            v->link_pending_x = x;
            v->link_pending_y = y;
            v->link_pending_action = action;
        }
        v->link_pending = TRUE;
        return;
    }
    start_link(v, x, y, action);
}

static void
start_link(NsProcView *v, int x, int y, LinkAct action)
{
    if (!v->opened)
        return;
    v->link_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_LINK;
    req->seq = ++v->link_seq;
    req->x = x;
    req->y = y;
    req->action = action;
    push_req(v, req);
}

static void start_hover(NsProcView *v, int x, int y);

static void
request_hover(NsProcView *v, int x, int y)
{
    if (!v->opened)
        return;
    if (v->hover_inflight) {
        v->hover_pending_x = x;
        v->hover_pending_y = y;
        v->hover_pending = TRUE;
        return;
    }
    start_hover(v, x, y);
}

static void
start_hover(NsProcView *v, int x, int y)
{
    if (!v->opened)
        return;
    v->hover_inflight = TRUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_HOVER;
    req->seq = ++v->hover_seq;
    req->x = x;
    req->y = y;
    push_req(v, req);
}

static const char *
keyval_js_key(guint keyval, char *buf, size_t bufsz)
{
    gunichar uc = gdk_keyval_to_unicode(keyval);
    if (uc >= 0x20 && uc != 0x7f) {
        int len = g_unichar_to_utf8(uc, buf);
        if (len >= (int)bufsz) len = (int)bufsz - 1;
        buf[len] = '\0';
        return buf;
    }
    switch (keyval) {
    case GDK_KEY_Up:         return "ArrowUp";
    case GDK_KEY_Down:       return "ArrowDown";
    case GDK_KEY_Left:       return "ArrowLeft";
    case GDK_KEY_Right:      return "ArrowRight";
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:   return "Enter";
    case GDK_KEY_Escape:     return "Escape";
    case GDK_KEY_BackSpace:  return "Backspace";
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: return "Tab";
    case GDK_KEY_Delete:     return "Delete";
    case GDK_KEY_Insert:     return "Insert";
    case GDK_KEY_Home:       return "Home";
    case GDK_KEY_End:        return "End";
    case GDK_KEY_Page_Up:    return "PageUp";
    case GDK_KEY_Page_Down:  return "PageDown";
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:    return "Shift";
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:  return "Control";
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:      return "Alt";
    default: { const char *n = gdk_keyval_name(keyval); return n ? n : ""; }
    }
}

static const char *
keyval_js_code(guint keyval, char *buf, size_t bufsz)
{
    if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z) {
        g_snprintf(buf, bufsz, "Key%c", 'A' + (int)(keyval - GDK_KEY_a));
        return buf;
    }
    if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z) {
        g_snprintf(buf, bufsz, "Key%c", 'A' + (int)(keyval - GDK_KEY_A));
        return buf;
    }
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) {
        g_snprintf(buf, bufsz, "Digit%c", '0' + (int)(keyval - GDK_KEY_0));
        return buf;
    }
    switch (keyval) {
    case GDK_KEY_Up:         return "ArrowUp";
    case GDK_KEY_Down:       return "ArrowDown";
    case GDK_KEY_Left:       return "ArrowLeft";
    case GDK_KEY_Right:      return "ArrowRight";
    case GDK_KEY_Return:     return "Enter";
    case GDK_KEY_KP_Enter:   return "NumpadEnter";
    case GDK_KEY_Escape:     return "Escape";
    case GDK_KEY_BackSpace:  return "Backspace";
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: return "Tab";
    case GDK_KEY_Delete:     return "Delete";
    case GDK_KEY_Home:       return "Home";
    case GDK_KEY_End:        return "End";
    case GDK_KEY_Page_Up:    return "PageUp";
    case GDK_KEY_Page_Down:  return "PageDown";
    case GDK_KEY_space:      return "Space";
    default:                 return "";
    }
}

static int
keyval_js_keycode(guint keyval)
{
    if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_z)
        return 65 + (int)(keyval - GDK_KEY_a);
    if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_Z)
        return 65 + (int)(keyval - GDK_KEY_A);
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9)
        return 48 + (int)(keyval - GDK_KEY_0);
    switch (keyval) {
    case GDK_KEY_BackSpace:  return 8;
    case GDK_KEY_Tab:        return 9;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:   return 13;
    case GDK_KEY_Escape:     return 27;
    case GDK_KEY_space:      return 32;
    case GDK_KEY_Page_Up:    return 33;
    case GDK_KEY_Page_Down:  return 34;
    case GDK_KEY_End:        return 35;
    case GDK_KEY_Home:       return 36;
    case GDK_KEY_Left:       return 37;
    case GDK_KEY_Up:         return 38;
    case GDK_KEY_Right:      return 39;
    case GDK_KEY_Down:       return 40;
    case GDK_KEY_Delete:     return 46;
    default:                 return 0;
    }
}

static void
start_key_full(NsProcView *v, int kind, guint keyval, GdkModifierType state,
               int fallback_scroll, double fallback_x, double fallback_y)
{
    if (!v->opened)
        return;
    char keybuf[8] = {0}, codebuf[16] = {0};
    Req *req = g_new0(Req, 1);
    req->type = REQ_KEY;
    req->seq = kind == 0 ? ++v->key_seq : v->key_seq;
    req->kind = kind;
    req->keycode = keyval_js_keycode(keyval);
    req->fallback_scroll = fallback_scroll;
    req->fallback_x = fallback_x;
    req->fallback_y = fallback_y;
    req->mods = ((state & GDK_SHIFT_MASK)   ? 1 : 0) |
                ((state & GDK_CONTROL_MASK) ? 2 : 0) |
                ((state & GDK_ALT_MASK)     ? 4 : 0) |
                ((state & GDK_META_MASK)    ? 8 : 0);
    req->key = g_strdup(keyval_js_key(keyval, keybuf, sizeof keybuf));
    req->code = g_strdup(keyval_js_code(keyval, codebuf, sizeof codebuf));
    push_req(v, req);
}

static void
start_key(NsProcView *v, int kind, guint keyval, GdkModifierType state)
{
    start_key_full(v, kind, keyval, state, 0, 0, 0);
}

static void
start_key_text(NsProcView *v, int kind, const char *text)
{
    if (!v->opened || !text || !*text)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_KEY;
    req->seq = ++v->key_seq;
    req->kind = kind;
    req->keycode = 0;
    req->mods = 0;
    req->key = g_strdup(text);
    req->code = g_strdup("");
    push_req(v, req);
}

static void
on_im_commit(GtkIMContext *im, const char *text, gpointer data)
{
    (void)im;
    NsProcView *v = data;
    if (!v || !v->opened || !text || !*text) return;
    start_key_text(v, 2, text);
}

static void
start_release(NsProcView *v, int x, int y)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_RELEASE;
    req->x = x;
    req->y = y;
    push_req(v, req);
}

static void
start_select(NsProcView *v, int kind, int x, int y)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_SELECT;
    req->seq = ++v->select_seq;
    req->kind = kind;
    req->x = x;
    req->y = y;
    push_req(v, req);
}

static void
start_click(NsProcView *v, int x, int y, int mods)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_CLICK;
    req->seq = ++v->click_seq;
    req->x = x;
    req->y = y;
    req->mods = mods;
    push_req(v, req);
}

static void
start_dropfiles(NsProcView *v, int x, int y, char *paths)
{
    if (!v->opened) {
        g_free(paths);
        return;
    }
    Req *req = g_new0(Req, 1);
    req->type = REQ_DROPFILES;
    req->x = x;
    req->y = y;
    req->paths = paths;
    push_req(v, req);
}

static void
start_viewport(NsProcView *v, int width, int height)
{
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_VIEWPORT;
    req->seq = ++v->viewport_seq;
    req->vw = width;
    req->vh = height;
    push_req(v, req);
}

static gboolean
maybe_update_viewport(NsProcView *v)
{
    if (!v->opened)
        return FALSE;
    int w = viewport_w(v);
    int h = viewport_h(v);
    if (w <= 1 || h <= 1)
        return FALSE;
    if (w == v->last_vp_w && h == v->last_vp_h)
        return FALSE;
    v->last_vp_w = w;
    v->last_vp_h = h;
    start_viewport(v, w, h);
    return TRUE;
}

static void
push_history(NsProcView *v, const char *url)
{
    if (!url || !*url)
        return;
    if (v->hist_index >= 0 &&
        g_strcmp0(g_ptr_array_index(v->history, v->hist_index), url) == 0)
        return;
    while ((int)v->history->len > v->hist_index + 1)
        g_ptr_array_remove_index(v->history, v->history->len - 1);
    g_ptr_array_add(v->history, g_strdup(url));
    v->hist_index = (int)v->history->len - 1;
    post_emit(v, NS_PROC_EVT_HISTORY, NULL);
}

static void pv_perm_resolve(NsProcView *v, gboolean allow);

static void
do_load(NsProcView *v, const char *url, gboolean record, gboolean history)
{
    if (!url || !*url)
        return;
    pv_perm_resolve(v, FALSE);
    pv_audio_shutdown(v);
    pv_video_shutdown(v);
    v->pending_record = record;
    int seq = ++v->load_seq;
    ++v->render_seq;
    ++v->link_seq;
    ++v->click_seq;
    ++v->viewport_seq;
    ++v->key_seq;
    ++v->select_seq;
    ++v->hover_seq;
    v->render_pending = FALSE;
    v->render_inflight = FALSE;
    v->link_inflight = FALSE;
    v->link_pending = FALSE;
    v->link_pending_action = ACT_HOVER;
    v->hover_inflight = FALSE;
    v->hover_pending = FALSE;
    v->has_selection = FALSE;
    ++v->find_seq;
    if (v->search_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(v->search_revealer), FALSE);
    if (v->search_label)
        gtk_label_set_text(GTK_LABEL(v->search_label), "");
    v->opened = FALSE;
    disarm_anim(v);
    if (v->frame)
        cairo_surface_destroy(v->frame);
    v->frame = NULL;
    gtk_widget_queue_draw(v->area);
    if (!v->loading) {
        v->loading = TRUE;
        post_emit(v, NS_PROC_EVT_LOADING, "1");
    }
    set_busy_cursor(v);
    post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Loading…"));

    int vw = gtk_widget_get_width(v->area);
    int vh = gtk_widget_get_height(v->area);
    if (vw <= 1 || vh <= 1) {
        g_free(v->deferred_url);
        v->deferred_url = g_strdup(url);
        v->deferred_record = record;
        v->deferred_history = history;
        return;
    }
    v->last_vp_w = vw;
    v->last_vp_h = vh;

    Req *req = g_new0(Req, 1);
    req->type = REQ_LOAD;
    req->seq = seq;
    req->url = g_strdup(url);
    req->vw = vw;
    req->vh = vh;
    req->history = history;
    push_req(v, req);
}

void
ns_proc_view_load(NsProcView *v, const char *url)
{
    v->render_restarts = 0;
    do_load(v, url, TRUE, FALSE);
}

gboolean ns_proc_view_can_back(NsProcView *v) { return v->hist_index > 0; }

gboolean
ns_proc_view_can_forward(NsProcView *v)
{
    return v->hist_index >= 0 && v->hist_index < (int)v->history->len - 1;
}

void
ns_proc_view_back(NsProcView *v)
{
    if (!ns_proc_view_can_back(v))
        return;
    v->hist_index--;
    v->render_restarts = 0;
    post_emit(v, NS_PROC_EVT_HISTORY, NULL);
    do_load(v, g_ptr_array_index(v->history, v->hist_index), FALSE, TRUE);
}

void
ns_proc_view_forward(NsProcView *v)
{
    if (!ns_proc_view_can_forward(v))
        return;
    v->hist_index++;
    v->render_restarts = 0;
    post_emit(v, NS_PROC_EVT_HISTORY, NULL);
    do_load(v, g_ptr_array_index(v->history, v->hist_index), FALSE, TRUE);
}

void
ns_proc_view_reload(NsProcView *v)
{
    v->render_restarts = 0;
    if (v->hist_index >= 0 && v->hist_index < (int)v->history->len)
        do_load(v, g_ptr_array_index(v->history, v->hist_index), FALSE, FALSE);
    else if (v->current_url)
        do_load(v, v->current_url, FALSE, FALSE);
}

void
ns_proc_view_toggle_console(NsProcView *v)
{
    if (v->opened)
        console_set_open(v, !v->console_open);
}

const char *ns_proc_view_url(NsProcView *v) { return v->current_url; }
const char *ns_proc_view_title(NsProcView *v) { return v->current_title; }
int ns_proc_view_security(NsProcView *v) { return v ? v->security : 0; }
const char *ns_proc_view_remote_ip(NsProcView *v) { return v ? v->remote_ip : NULL; }
gboolean ns_proc_view_is_loading(NsProcView *v) { return v->loading; }
GdkPaintable *ns_proc_view_favicon(NsProcView *v) { return v ? v->favicon : NULL; }

int
ns_proc_view_renderer_pid(NsProcView *v)
{
    if (!v) return -1;
    g_mutex_lock(&v->proc_lock);
    int pid = v->proc ? ns_rproc_http_pid(v->proc) : -1;
    g_mutex_unlock(&v->proc_lock);
    return pid;
}

static int
pv_subprocess_pid(GSubprocess *proc)
{
    if (!proc) return -1;
    const char *id = g_subprocess_get_identifier(proc);
    return id ? atoi(id) : -1;
}

int
ns_proc_view_audio_pid(NsProcView *v)
{
    return v ? pv_subprocess_pid(v->audio_proc) : -1;
}

int
ns_proc_view_video_pid(NsProcView *v)
{
    return v ? pv_subprocess_pid(v->video_proc) : -1;
}

void
ns_proc_view_end_task(NsProcView *v)
{
    if (!v) return;
    g_mutex_lock(&v->proc_lock);
    if (v->proc) {
        ns_rproc_http_interrupt(v->proc);
        ns_rproc_http_terminate(v->proc);
    }
    g_mutex_unlock(&v->proc_lock);
}

void
ns_proc_view_stop_video(NsProcView *v)
{
    if (v) pv_video_shutdown(v);
}

void
ns_proc_view_stop_audio(NsProcView *v)
{
    if (v) pv_audio_shutdown(v);
}
double ns_proc_view_zoom(NsProcView *v) { return cur_scale(v); }

void ns_proc_view_focus(NsProcView *v)
{
    if (v->area)
        gtk_widget_grab_focus(v->area);
}

static void
set_zoom(NsProcView *v, double scale)
{
    int permille = (int)(scale * 1000.0 + 0.5);
    if (permille < NS_PV_ZOOM_MIN_PERMILLE)
        permille = NS_PV_ZOOM_MIN_PERMILLE;
    if (permille > NS_PV_ZOOM_MAX_PERMILLE)
        permille = NS_PV_ZOOM_MAX_PERMILLE;
    double clamped = permille / 1000.0;
    if (clamped == cur_scale(v))
        return;
    v->scale = clamped;
    char status[32];
    g_snprintf(status, sizeof status, "%s %d%%", ns_i18n("Zoom"),
               permille / 10);
    post_emit(v, NS_PROC_EVT_STATUS, status);
    if (v->opened) {
        configure_adjustments(v);
        request_render(v);
    }
}

void ns_proc_view_zoom_in(NsProcView *v)  { set_zoom(v, cur_scale(v) * NS_PROC_ZOOM_STEP); }
void ns_proc_view_zoom_out(NsProcView *v) { set_zoom(v, cur_scale(v) / NS_PROC_ZOOM_STEP); }
void ns_proc_view_zoom_reset(NsProcView *v) { set_zoom(v, 1.0); }

static void
pv_perm_resolve(NsProcView *v, gboolean allow)
{
    if (!v->perm_pending)
        return;
    v->perm_pending = FALSE;
    ReqType kind = v->perm_kind;
    char *origin = v->perm_origin;
    v->perm_origin = NULL;
    if (v->perm_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(v->perm_revealer), FALSE);
    if (!v->closed) {
        Req *req = g_new0(Req, 1);
        req->type = kind;
        req->url = g_strdup(origin);
        req->mods = allow ? 1 : 0;
        push_req(v, req);
        if (kind == REQ_WEBGL && allow && v->current_url)
            do_load(v, v->current_url, FALSE, FALSE);
    }
    g_free(origin);
}

static void
on_perm_allow(GtkButton *btn, gpointer data)
{
    (void)btn;
    pv_perm_resolve(data, TRUE);
}

static void
on_perm_deny(GtkButton *btn, gpointer data)
{
    (void)btn;
    pv_perm_resolve(data, FALSE);
}

static void
pv_perm_bar_show(NsProcView *v, ReqType kind, const char *origin)
{
    if (v->perm_pending && v->perm_kind == kind &&
        g_strcmp0(v->perm_origin, origin) == 0)
        return;
    if (v->perm_pending)
        pv_perm_resolve(v, FALSE);
    v->perm_pending = TRUE;
    v->perm_kind = kind;
    v->perm_origin = g_strdup(origin);
    const char *what =
        kind == REQ_WEBGL
            ? ns_i18n("This site wants to use WebGL (3D graphics)")
            : ns_i18n("This site wants to use your camera and microphone");
    char *text = g_strdup_printf("%s — %s", what, origin);
    gtk_label_set_text(GTK_LABEL(v->perm_label), text);
    g_free(text);
    gtk_revealer_set_reveal_child(GTK_REVEALER(v->perm_revealer), TRUE);
}

static gboolean
on_result(gpointer data)
{
    Res *res = data;
    NsProcView *v = res->view;

    if (v->closed)
        goto done;

    if (res->type == RES_PAGE) {
        if (res->seq != v->load_seq)
            goto done;
        if (!res->ok) {
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Failed to load page"));
            finish_loading(v);
            clear_busy_cursor(v);
            goto done;
        }
        if (res->nav && *res->nav) {
            if (v->js_redirects < NS_PROC_MAX_JS_REDIRECTS) {
                v->js_redirects++;
                do_load(v, res->nav, v->pending_record, FALSE);
                goto done;
            }
            post_emit(v, NS_PROC_EVT_STATUS,
                      ns_i18n("Stopped after too many redirects"));
        }
        v->js_redirects = 0;
        g_free(v->current_url);
        v->current_url = g_strdup(res->url);
        g_free(v->current_title);
        v->current_title = g_strdup(res->title);
        v->security = res->security;
        g_free(v->remote_ip);
        v->remote_ip = res->remote_ip ? g_strdup(res->remote_ip) : NULL;
        v->page_w = res->pw;
        v->page_h = res->ph;
        v->scroll_x = 0;
        v->scroll_y = 0;
        v->opened = TRUE;
        configure_adjustments(v);
        if (v->pending_record)
            push_history(v, v->current_url);
        post_emit(v, NS_PROC_EVT_URL, v->current_url);
        post_emit(v, NS_PROC_EVT_TITLE, v->current_title);
        post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Done"));
        finish_loading(v);
        request_render(v);
        request_favicon(v);
    } else if (res->type == RES_FRAME) {
        gboolean current = res->seq == v->render_seq;
        if (current && res->ok) {
            if (res->animating)
                arm_anim(v);
            else
                disarm_anim(v);
            if (res->ph > 0 && res->ph != v->page_h) {
                v->page_h = res->ph;
                if (res->pw > 0) v->page_w = res->pw;
                gtk_widget_queue_draw(v->area);
            }
        }
        if (current && res->ok && res->surface) {
            if (v->frame)
                cairo_surface_destroy(v->frame);
            v->frame = res->surface;
            res->surface = NULL;
            v->render_restarts = 0;
            gtk_widget_queue_draw(v->area);
            clear_busy_cursor(v);
        } else if (current && res->ok && res->frame_unchanged) {
            v->render_restarts = 0;
        }
        if (current && res->ok && res->nav && *res->nav &&
            v->js_redirects < NS_PROC_MAX_JS_REDIRECTS) {
            v->js_redirects++;
            do_load(v, res->nav, FALSE, FALSE);
        }
        if (res->ok && res->webgl && *res->webgl)
            post_emit(v, NS_PROC_EVT_WEBGL, "1");
        if (res->ok && res->camera && *res->camera)
            pv_perm_bar_show(v, REQ_CAMERA, res->camera);
        if (res->ok && res->download && *res->download)
            post_emit(v, NS_PROC_EVT_DOWNLOAD, res->download);
        if (res->ok && res->audio && *res->audio)
            pv_media_pump(v, res->audio);
        v->render_inflight = FALSE;
        if (v->render_pending) {
            v->render_pending = FALSE;
            start_render(v);
        } else if (current && !res->ok && v->current_url) {
            if (v->render_restarts < NS_PROC_MAX_RESTARTS) {
                v->render_restarts++;
                post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Renderer restarted"));
                do_load(v, v->current_url, FALSE, FALSE);
            } else {
                post_emit(v, NS_PROC_EVT_STATUS,
                          ns_i18n("This tab's renderer keeps failing — "
                                  "reload to retry"));
                finish_loading(v);
                clear_busy_cursor(v);
            }
        }
    } else if (res->type == RES_VIEWPORT) {
        if (res->seq != v->viewport_seq)
            goto done;
        if (res->ok) {
            v->page_w = res->pw;
            v->page_h = res->ph;
            configure_adjustments(v);
            request_render(v);
        }
    } else if (res->type == RES_SELECT) {
        if (res->seq == v->select_seq)
            request_render(v);
    } else if (res->type == RES_COPY) {
        if (res->href && *res->href && v->area) {
            gdk_clipboard_set_text(gtk_widget_get_clipboard(v->area),
                                   res->href);
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Copied selection"));
        }
    } else if (res->type == RES_KEY) {
        if (res->seq != v->key_seq)
            goto done;
        if (res->kind == 0 && res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            ns_proc_view_load(v, res->href);
        } else {
            if (res->fallback_scroll && !res->prevented) {
                gtk_adjustment_set_value(v->hadj, res->fallback_x);
                gtk_adjustment_set_value(v->vadj, res->fallback_y);
            }
            request_render(v);
        }
    } else if (res->type == RES_CLICK) {
        if (res->seq != v->click_seq)
            goto done;
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            ns_proc_view_load(v, res->href);
        } else {
            request_render(v);
        }
    } else if (res->type == RES_LINK) {
        if (res->seq != v->link_seq)
            goto done;
        v->link_inflight = FALSE;
        gboolean navigated = FALSE;
        GtkWidget *area = v->area;
        if (res->action == ACT_CONTEXT) {
            if (res->prevented)
                request_render(v);
            else
                show_context_menu(v, res->href);
            if (v->link_pending) {
                v->link_pending = FALSE;
                LinkAct a = v->link_pending_action;
                v->link_pending_action = ACT_HOVER;
                start_link(v, v->link_pending_x, v->link_pending_y, a);
            }
            goto done;
        }
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            if (!v->busy_cursor)
                pv_set_named_cursor(area, res->cursor ? res->cursor : "pointer");
            if (res->action == ACT_NAVIGATE) {
                navigated = TRUE;
                ns_proc_view_load(v, res->href);
            } else if (res->action == ACT_NEWTAB) {
                post_emit(v, NS_PROC_EVT_NEWTAB, res->href);
            }
        } else if (!v->busy_cursor) {
            pv_set_named_cursor(area, res->cursor);
        }
        if (!navigated && v->link_pending) {
            v->link_pending = FALSE;
            LinkAct a = v->link_pending_action;
            v->link_pending_action = ACT_HOVER;
            start_link(v, v->link_pending_x, v->link_pending_y, a);
        }
    } else if (res->type == RES_HOVER) {
        if (res->seq != v->hover_seq)
            goto done;
        v->hover_inflight = FALSE;
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            if (!v->busy_cursor)
                pv_set_named_cursor(v->area, res->cursor ? res->cursor : "pointer");
        } else if (!v->busy_cursor) {
            pv_set_named_cursor(v->area, res->cursor);
        }
        if (res->ok)
            request_render(v);
        if (v->hover_pending) {
            v->hover_pending = FALSE;
            start_hover(v, v->hover_pending_x, v->hover_pending_y);
        }
    } else if (res->type == RES_DROPFILES) {
        if (res->ok)
            request_render(v);
    } else if (res->type == RES_SCROLL) {
        if (res->ok)
            request_render(v);
        else {
            gtk_adjustment_set_value(
                v->hadj,
                gtk_adjustment_get_value(v->hadj) + res->fallback_x * 60.0);
            gtk_adjustment_set_value(
                v->vadj,
                gtk_adjustment_get_value(v->vadj) + res->fallback_y * 60.0);
        }
    } else if (res->type == RES_SCROLLBAR) {
        if (res->kind == 0) {
            gboolean still = v->sb_probe;
            v->sb_probe = FALSE;
            if (res->ok) {
                request_render(v);
                if (still) {
                    v->sb_dragging = TRUE;
                    if (v->sb_have_last) {
                        double s = cur_scale(v);
                        Req *req = g_new0(Req, 1);
                        req->type = REQ_SCROLLBAR;
                        req->kind = 1;
                        req->x = v->scroll_x + (int)(v->sb_last_x / s);
                        req->y = v->scroll_y + (int)(v->sb_last_y / s);
                        push_req(v, req);
                    }
                }
            } else if (still && v->sb_have_last) {
                double s = cur_scale(v);
                start_select(v, 0, v->scroll_x + (int)(v->drag_start_x / s),
                             v->scroll_y + (int)(v->drag_start_y / s));
                v->drag_anchored = TRUE;
                start_select(v, 1, v->scroll_x + (int)(v->sb_last_x / s),
                             v->scroll_y + (int)(v->sb_last_y / s));
                v->has_selection = TRUE;
            }
        } else if (res->kind == 1) {
            if (res->ok)
                request_render(v);
        }
    } else if (res->type == RES_RELEASE) {
        if (res->href && *res->href) {
            post_emit(v, NS_PROC_EVT_STATUS, res->href);
            ns_proc_view_load(v, res->href);
        } else if (res->ok) {
            request_render(v);
        }
    } else if (res->type == RES_FIND) {
        if (res->seq != v->find_seq)
            goto done;
        if (v->search_label) {
            const char *q = v->search_entry
                ? gtk_editable_get_text(GTK_EDITABLE(v->search_entry)) : NULL;
            if (res->find_total > 0) {
                char buf[64];
                g_snprintf(buf, sizeof buf, "%d/%d", res->find_current,
                           res->find_total);
                gtk_label_set_text(GTK_LABEL(v->search_label), buf);
            } else {
                gtk_label_set_text(GTK_LABEL(v->search_label),
                                   (q && *q) ? ns_i18n("No results") : "");
            }
        }
        if (res->find_total > 0) {
            double target = res->find_scroll_y > 40 ? res->find_scroll_y - 40
                                                    : 0;
            gtk_adjustment_set_value(v->vadj, target);
        }
        request_render(v);
    } else if (res->type == RES_EXPORT) {
        if (res->ok && res->url)
            post_emit(v, NS_PROC_EVT_STATUS, res->url);
        else
            post_emit(v, NS_PROC_EVT_STATUS, ns_i18n("Could not save page"));
    } else if (res->type == RES_CONSOLE) {
        if (res->href && *res->href)
            console_append(v, res->href);
    } else if (res->type == RES_EVAL) {
        if (res->inspect) {
            if (v->elements_buffer)
                gtk_text_buffer_set_text(
                    v->elements_buffer,
                    (res->href && *res->href) ? res->href
                                              : ns_i18n("No matching element"),
                    -1);
        } else if (res->href && *res->href) {
            console_append(v, res->href);
            console_append(v, "\n");
        } else {
            console_append(v, "undefined\n");
        }
        request_render(v);
    } else if (res->type == RES_DUMP) {
        GtkTextBuffer *buf = NULL;
        switch (res->dump_tab) {
        case DEV_TAB_NETWORK:     buf = v->net_buffer;      break;
        case DEV_TAB_PERFORMANCE: buf = v->perf_buffer;     break;
        case DEV_TAB_LAYOUT:      buf = v->layout_buffer;   break;
        case DEV_TAB_ELEMENTS:    buf = v->elements_buffer; break;
        default: break;
        }
        if (buf == v->perf_buffer) {
            GString *text = g_string_new(
                (res->href && *res->href) ? res->href : ns_i18n("(empty)"));
            pv_append_media_process_stats(v, text);
            gtk_text_buffer_set_text(buf, text->str, -1);
            g_string_free(text, TRUE);
        } else if (buf) {
            gtk_text_buffer_set_text(
                buf, (res->href && *res->href) ? res->href : ns_i18n("(empty)"),
                -1);
        }
    } else if (res->type == RES_FAVICON) {
        if (res->seq != v->load_seq)
            goto done;
        g_clear_object(&v->favicon);
        if (res->favicon_data && res->favicon_w > 0 && res->favicon_h > 0) {
            gsize len = (gsize)res->favicon_stride * (gsize)res->favicon_h;
            GBytes *bytes = g_bytes_new(res->favicon_data, len);
            GdkTexture *tex = gdk_memory_texture_new(
                res->favicon_w, res->favicon_h,
                GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, res->favicon_stride);
            g_bytes_unref(bytes);
            v->favicon = GDK_PAINTABLE(tex);
        }
        post_emit(v, NS_PROC_EVT_FAVICON, NULL);
    }

done:
    if (res->surface && !res->surface_borrowed)
        cairo_surface_destroy(res->surface);
    g_free(res->title);
    g_free(res->url);
    g_free(res->nav);
    g_free(res->remote_ip);
    g_free(res->webgl);
    g_free(res->camera);
    g_free(res->download);
    g_free(res->audio);
    free(res->href);
    free(res->cursor);
    free(res->media_url);
    free(res->favicon_data);
    pv_unref(res->view);
    g_free(res);
    return G_SOURCE_REMOVE;
}

static void
on_draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
        gpointer data)
{
    (void)area;
    NsProcView *v = data;
    gboolean covers = v->frame &&
        cairo_image_surface_get_width(v->frame) >= width &&
        cairo_image_surface_get_height(v->frame) >= height;
    if (!covers) {
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_rectangle(cr, 0, 0, width, height);
        cairo_fill(cr);
    }
    if (v->vring && v->vid_rect_valid) {
        gboolean frame_drawn = FALSE;
        ns_vring_hdr *r = v->vring;
        guint32 magic   = r->magic;
        guint32 slot    = r->latest;
        guint32 nslots  = r->nslots;
        guint32 fw      = r->width;
        guint32 fh      = r->height;
        guint32 fstride = r->stride;
        guint32 fbytes  = r->frame_bytes;
        if (magic == NS_VRING_MAGIC && slot != G_MAXUINT32 && slot < nslots &&
            fw > 0 && fh > 0 &&
            (guint64)fstride >= (guint64)fw * 4 &&
            (guint64)fstride * fh <= fbytes &&
            sizeof(ns_vring_hdr) + (gsize)(slot + 1) * fbytes
                <= v->vring_bytes) {
            unsigned char *px = (unsigned char *)r + sizeof(ns_vring_hdr) +
                                (gsize)slot * fbytes;
            cairo_surface_t *s = cairo_image_surface_create_for_data(
                px, CAIRO_FORMAT_RGB24, (int)fw, (int)fh, (int)fstride);
            if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) {
                cairo_save(cr);
                cairo_rectangle(cr, v->vid_x, v->vid_y, v->vid_w, v->vid_h);
                cairo_clip(cr);
                cairo_translate(cr, v->vid_x, v->vid_y);
                cairo_scale(cr, v->vid_w / (double)fw,
                            v->vid_h / (double)fh);
                cairo_set_source_surface(cr, s, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
                frame_drawn = TRUE;
            }
            cairo_surface_destroy(s);
        }
        if (!frame_drawn) {
            cairo_set_source_rgb(cr, 0.10, 0.10, 0.10);
            cairo_rectangle(cr, v->vid_x, v->vid_y, v->vid_w, v->vid_h);
            cairo_fill(cr);
        }
        if (g_getenv("NS_DBG_COMPOSITE")) {
            static gint64 last_us;
            static int cdrawn, cblack;
            if (frame_drawn) cdrawn++; else cblack++;
            gint64 nowu = g_get_monotonic_time();
            if (nowu - last_us > 1000000) {
                last_us = nowu;
                g_printerr("[composite] drawn=%d/s black=%d/s slot=%u magic=%s "
                           "%ux%u rect=%.0f,%.0f %.0fx%.0f\n",
                           cdrawn, cblack, slot,
                           magic == NS_VRING_MAGIC ? "ok" : "BAD",
                           fw, fh, v->vid_x, v->vid_y, v->vid_w, v->vid_h);
                cdrawn = 0; cblack = 0;
            }
        }
    }
    if (v->frame) {
        cairo_set_source_surface(cr, v->frame, 0, 0);
        cairo_paint(cr);
    }
}

static void
on_resize(GtkDrawingArea *area, int width, int height, gpointer data)
{
    (void)area;
    NsProcView *v = data;
    if (v->deferred_url && width > 1 && height > 1) {
        char *u = v->deferred_url;
        gboolean rec = v->deferred_record;
        gboolean hist = v->deferred_history;
        v->deferred_url = NULL;
        do_load(v, u, rec, hist);
        g_free(u);
        return;
    }
    if (v->opened) {
        if (maybe_update_viewport(v))
            return;
        configure_adjustments(v);
        request_render(v);
    }
}

static gboolean
on_scroll(GtkEventControllerScroll *ctrl, double dx, double dy, gpointer data)
{
    NsProcView *v = data;
    if (!v->opened)
        return FALSE;
    GdkModifierType mods =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(ctrl));
    GdkEvent *ev =
        gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(ctrl));
    if (ev)
        mods |= gdk_event_get_modifier_state(ev);
    if (mods & GDK_CONTROL_MASK) {
        double delta = dy != 0.0 ? dy : dx;
        if (delta < 0)
            ns_proc_view_zoom_in(v);
        else if (delta > 0)
            ns_proc_view_zoom_out(v);
        return TRUE;
    }
    if (v->proc) {
        double s = cur_scale(v);
        Req *req = g_new0(Req, 1);
        req->type = REQ_SCROLL;
        req->x = v->scroll_x + (int)(v->pointer_x / s);
        req->y = v->scroll_y + (int)(v->pointer_y / s);
        req->dx = (int)(dx * 60.0 / s);
        req->dy = (int)(dy * 60.0 / s);
        req->fallback_x = dx;
        req->fallback_y = dy;
        push_req(v, req);
        return TRUE;
    }
    gtk_adjustment_set_value(v->hadj,
                             gtk_adjustment_get_value(v->hadj) + dx * 60.0);
    gtk_adjustment_set_value(v->vadj,
                             gtk_adjustment_get_value(v->vadj) + dy * 60.0);
    return TRUE;
}

typedef struct { NsProcView *view; gboolean pdf; } ExportCtx;

static void
on_save_dialog_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    ExportCtx *c = ud;
    NsProcView *v = c->view;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
    if (file) {
        char *dest = g_file_get_path(file);
        if (dest && v->opened) {
            static int export_counter = 0;
            char *base = g_strdup_printf(
                "nordstjernen-export-%" G_GINT64_FORMAT "-%d.%s",
                g_get_monotonic_time(), ++export_counter,
                c->pdf ? "pdf" : "png");
            Req *req = g_new0(Req, 1);
            req->type = REQ_EXPORT;
            req->url = g_build_filename(g_get_user_runtime_dir(), base, NULL);
            req->export_dest = g_strdup(dest);
            push_req(v, req);
            g_free(base);
        }
        g_free(dest);
        g_object_unref(file);
    }
    g_clear_error(&err);
    pv_unref(v);
    g_free(c);
}

static void
view_save(NsProcView *v, gboolean pdf)
{
    if (!v->opened || !v->area)
        return;
    GtkRoot *root = gtk_widget_get_root(v->area);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog,
        pdf ? ns_i18n("Save page as PDF")
                                      : ns_i18n("Save page as PNG"));
    const char *t = (v->current_title && *v->current_title)
        ? v->current_title : "page";
    char *name = g_strdup_printf("%s.%s", t, pdf ? "pdf" : "png");
    g_strdelimit(name, "/", '_');
    gtk_file_dialog_set_initial_name(dialog, name);
    ExportCtx *c = g_new0(ExportCtx, 1);
    c->view = pv_ref(v);
    c->pdf = pdf;
    gtk_file_dialog_save(dialog, GTK_WINDOW(root), NULL,
                         on_save_dialog_done, c);
    g_object_unref(dialog);
    g_free(name);
}

static void
ctx_set_clipboard(NsProcView *v, const char *text, const char *status)
{
    if (!text || !*text || !v->area)
        return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(v->area), text);
    post_emit(v, NS_PROC_EVT_STATUS, status);
}

static void
on_ctx_back(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; ns_proc_view_back(ud); }

static void
on_ctx_forward(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; ns_proc_view_forward(ud); }

static void
on_ctx_reload(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; ns_proc_view_reload(ud); }

static void
on_ctx_copy_url(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    ctx_set_clipboard(v, v->current_url, ns_i18n("Copied page address"));
}

static void
on_ctx_open_newtab(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    if (v->ctx_link && *v->ctx_link)
        post_emit(v, NS_PROC_EVT_NEWTAB, v->ctx_link);
}

static void
on_ctx_open_link(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    if (v->ctx_link && *v->ctx_link)
        ns_proc_view_load(v, v->ctx_link);
}

static void
on_ctx_copy_link(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    ctx_set_clipboard(v, v->ctx_link, ns_i18n("Copied link address"));
}

static void
on_ctx_copy_sel(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; start_select(ud, 4, 0, 0); }

static void
on_ctx_select_all(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    NsProcView *v = ud;
    v->has_selection = TRUE;
    start_select(v, 3, 0, 0);
}

static void
on_ctx_save_pdf(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; view_save(ud, TRUE); }

static void
on_ctx_save_png(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; view_save(ud, FALSE); }

static void
ctx_action_enable(NsProcView *v, const char *name, gboolean on)
{
    GAction *act = g_action_map_lookup_action(G_ACTION_MAP(v->ctx_actions),
                                              name);
    if (act)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(act), on);
}

static void
ctx_install_actions(NsProcView *v)
{
    static const GActionEntry entries[] = {
        { "back",        on_ctx_back,        NULL, NULL, NULL, {0} },
        { "forward",     on_ctx_forward,     NULL, NULL, NULL, {0} },
        { "reload",      on_ctx_reload,      NULL, NULL, NULL, {0} },
        { "copy-url",    on_ctx_copy_url,    NULL, NULL, NULL, {0} },
        { "open-link",   on_ctx_open_link,   NULL, NULL, NULL, {0} },
        { "open-newtab", on_ctx_open_newtab, NULL, NULL, NULL, {0} },
        { "copy-link",   on_ctx_copy_link,   NULL, NULL, NULL, {0} },
        { "copy-sel",    on_ctx_copy_sel,    NULL, NULL, NULL, {0} },
        { "select-all",  on_ctx_select_all,  NULL, NULL, NULL, {0} },
        { "save-pdf",    on_ctx_save_pdf,    NULL, NULL, NULL, {0} },
        { "save-png",    on_ctx_save_png,    NULL, NULL, NULL, {0} },
    };
    v->ctx_actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(v->ctx_actions), entries,
                                    G_N_ELEMENTS(entries), v);
    gtk_widget_insert_action_group(v->area, "ctx",
                                   G_ACTION_GROUP(v->ctx_actions));
}

static void
show_context_menu(NsProcView *v, const char *href)
{
    g_free(v->ctx_link);
    v->ctx_link = (href && *href) ? g_strdup(href) : NULL;

    ctx_action_enable(v, "back", ns_proc_view_can_back(v));
    ctx_action_enable(v, "forward", ns_proc_view_can_forward(v));
    ctx_action_enable(v, "open-link", v->ctx_link != NULL);
    ctx_action_enable(v, "open-newtab", v->ctx_link != NULL);
    ctx_action_enable(v, "copy-link", v->ctx_link != NULL);
    ctx_action_enable(v, "copy-sel", v->has_selection);

    GMenu *menu = g_menu_new();
    if (v->ctx_link) {
        GMenu *s = g_menu_new();
        g_menu_append(s, ns_i18n("Open Link"), "ctx.open-link");
        if (!ns_rproc_single_process_enabled())
            g_menu_append(s, ns_i18n("Open Link in New Tab"), "ctx.open-newtab");
        g_menu_append(s, ns_i18n("Copy Link Address"), "ctx.copy-link");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
        g_object_unref(s);
    }
    if (v->has_selection) {
        GMenu *s = g_menu_new();
        g_menu_append(s, ns_i18n("Copy"), "ctx.copy-sel");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(s));
        g_object_unref(s);
    }
    GMenu *nav = g_menu_new();
    g_menu_append(nav, ns_i18n("Back"), "ctx.back");
    g_menu_append(nav, ns_i18n("Forward"), "ctx.forward");
    g_menu_append(nav, ns_i18n("Reload"), "ctx.reload");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(nav));
    g_object_unref(nav);
    GMenu *page = g_menu_new();
    g_menu_append(page, ns_i18n("Select All"), "ctx.select-all");
    g_menu_append(page, ns_i18n("Copy Page Address"), "ctx.copy-url");
    g_menu_append(page, ns_i18n("Save Page as PDF…"), "ctx.save-pdf");
    g_menu_append(page, ns_i18n("Save Page as Image…"), "ctx.save-png");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(page));
    g_object_unref(page);

    if (v->ctx_popover)
        gtk_widget_unparent(v->ctx_popover);
    v->ctx_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);
    gtk_widget_set_parent(v->ctx_popover, v->area);
    gtk_popover_set_has_arrow(GTK_POPOVER(v->ctx_popover), FALSE);
    gtk_popover_set_pointing_to(GTK_POPOVER(v->ctx_popover),
        &(GdkRectangle){ (int)v->ctx_x, (int)v->ctx_y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(v->ctx_popover));
}

static void
on_secondary_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
                     gpointer data)
{
    (void)n_press;
    NsProcView *v = data;
    if (!v->opened)
        return;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    v->ctx_x = x;
    v->ctx_y = y;
    double s = cur_scale(v);
    int px = v->scroll_x + (int)(x / s);
    int py = v->scroll_y + (int)(y / s);
    request_link(v, px, py, ACT_CONTEXT);
}

static void
request_find(NsProcView *v, int direction)
{
    if (!v->opened || !v->search_entry)
        return;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search_entry));
    Req *req = g_new0(Req, 1);
    req->type = REQ_FIND;
    req->seq = ++v->find_seq;
    req->query = g_strdup(q ? q : "");
    req->find_dir = direction;
    req->find_from_y = v->scroll_y;
    req->find_case = v->find_case;
    push_req(v, req);
}

static void
search_open(NsProcView *v)
{
    if (!v->search_revealer)
        return;
    gtk_revealer_set_reveal_child(GTK_REVEALER(v->search_revealer), TRUE);
    gtk_widget_grab_focus(v->search_entry);
    gtk_editable_select_region(GTK_EDITABLE(v->search_entry), 0, -1);
    const char *q = gtk_editable_get_text(GTK_EDITABLE(v->search_entry));
    if (q && *q)
        request_find(v, 0);
}

static void
search_close(NsProcView *v)
{
    if (!v->search_revealer)
        return;
    gtk_revealer_set_reveal_child(GTK_REVEALER(v->search_revealer), FALSE);
    gtk_label_set_text(GTK_LABEL(v->search_label), "");
    Req *req = g_new0(Req, 1);
    req->type = REQ_FIND;
    req->seq = ++v->find_seq;
    req->query = g_strdup("");
    req->find_from_y = v->scroll_y;
    push_req(v, req);
    gtk_widget_grab_focus(v->area);
}

void
ns_proc_view_find_open(NsProcView *v)
{
    search_open(v);
}

static void
on_search_changed(GtkSearchEntry *e, gpointer data)
{ (void)e; request_find(data, 0); }

static void
on_search_next(GtkWidget *w, gpointer data)
{ (void)w; request_find(data, 1); }

static void
on_search_prev(GtkWidget *w, gpointer data)
{ (void)w; request_find(data, 2); }

static void
on_search_stop(GtkSearchEntry *e, gpointer data)
{ (void)e; search_close(data); }

static void
on_search_close_clicked(GtkButton *b, gpointer data)
{ (void)b; search_close(data); }

static void
on_pressed(GtkGestureClick *gesture, int n_press, double x, double y,
           gpointer data)
{
    (void)n_press;
    NsProcView *v = data;
    if (!v->opened)
        return;
    GdkModifierType mods =
        gtk_event_controller_get_current_event_state(
            GTK_EVENT_CONTROLLER(gesture));
    guint button = gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(gesture));
    gtk_widget_grab_focus(v->area);
    double s = cur_scale(v);
    int px = v->scroll_x + (int)(x / s);
    int py = v->scroll_y + (int)(y / s);
    if (button == GDK_BUTTON_MIDDLE || (mods & GDK_CONTROL_MASK)) {
        request_link(v, px, py, ACT_NEWTAB);
        return;
    }
    int kmods = ((mods & GDK_SHIFT_MASK)   ? 1 : 0) |
                ((mods & GDK_CONTROL_MASK) ? 2 : 0) |
                ((mods & GDK_ALT_MASK)     ? 4 : 0) |
                ((mods & GDK_META_MASK)    ? 8 : 0);
    v->has_selection = FALSE;
    start_click(v, px, py, kmods);
}

static void
on_released(GtkGestureClick *gesture, int n_press, double x, double y,
            gpointer data)
{
    (void)gesture; (void)n_press;
    NsProcView *v = data;
    if (v->opened) {
        double s = cur_scale(v);
        start_release(v, v->scroll_x + (int)(x / s),
                      v->scroll_y + (int)(y / s));
    }
}

static void
on_motion(GtkEventControllerMotion *ctrl, double x, double y, gpointer data)
{
    (void)ctrl;
    NsProcView *v = data;
    v->pointer_x = x;
    v->pointer_y = y;
    if (v->opened) {
        double s = cur_scale(v);
        int px = v->scroll_x + (int)(x / s);
        int py = v->scroll_y + (int)(y / s);
        request_hover(v, px, py);
    }
}

static void
push_scrollbar(NsProcView *v, int kind, int px, int py)
{
    Req *req = g_new0(Req, 1);
    req->type = REQ_SCROLLBAR;
    req->kind = kind;
    req->x = px;
    req->y = py;
    push_req(v, req);
}

static void
on_drag_begin(GtkGestureDrag *g, double sx, double sy, gpointer data)
{
    (void)g;
    NsProcView *v = data;
    v->drag_start_x = sx;
    v->drag_start_y = sy;
    v->drag_anchored = FALSE;
    v->sb_dragging = FALSE;
    v->sb_have_last = FALSE;
    v->sb_probe = FALSE;
    if (v->opened) {
        double s = cur_scale(v);
        v->sb_probe = TRUE;
        push_scrollbar(v, 0, v->scroll_x + (int)(sx / s),
                       v->scroll_y + (int)(sy / s));
    }
}

static void
on_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    (void)g;
    NsProcView *v = data;
    if (!v->opened)
        return;
    double s = cur_scale(v);
    double wx = v->drag_start_x + ox;
    double wy = v->drag_start_y + oy;
    if (v->sb_dragging) {
        push_scrollbar(v, 1, v->scroll_x + (int)(wx / s),
                       v->scroll_y + (int)(wy / s));
        return;
    }
    if (v->sb_probe) {
        v->sb_last_x = wx;
        v->sb_last_y = wy;
        v->sb_have_last = TRUE;
        return;
    }
    if (!v->drag_anchored) {
        start_select(v, 0, v->scroll_x + (int)(v->drag_start_x / s),
                     v->scroll_y + (int)(v->drag_start_y / s));
        v->drag_anchored = TRUE;
    }
    start_select(v, 1, v->scroll_x + (int)(wx / s),
                 v->scroll_y + (int)(wy / s));
    v->has_selection = TRUE;
}

static void
on_drag_end(GtkGestureDrag *g, double ox, double oy, gpointer data)
{
    (void)g;
    (void)ox;
    (void)oy;
    NsProcView *v = data;
    if (v->sb_dragging || v->sb_probe) {
        push_scrollbar(v, 2, 0, 0);
        v->sb_dragging = FALSE;
        v->sb_probe = FALSE;
        v->sb_have_last = FALSE;
    }
}

static void
on_paste_text_ready(GObject *src, GAsyncResult *res, gpointer data)
{
    NsProcView *v = data;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (text && *text && v->opened)
        start_key_text(v, 2, text);
    g_free(text);
}

static gboolean
on_key(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
       GdkModifierType state, gpointer data)
{
    (void)ctrl;
    (void)keycode;
    NsProcView *v = data;
    if (!v->opened)
        return FALSE;
    if (keyval == GDK_KEY_F12) {
        console_set_open(v, !v->console_open);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) &&
        (keyval == GDK_KEY_j || keyval == GDK_KEY_J)) {
        console_set_open(v, !v->console_open);
        return TRUE;
    }
    gunichar uc = gdk_keyval_to_unicode(keyval);
    if (uc && uc != ' ' && !g_unichar_iscntrl(uc) &&
        !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_META_MASK))) {
        start_key(v, 0, keyval, state);
        start_key(v, 3, keyval, state);
        return FALSE;
    }
    if (state & GDK_CONTROL_MASK) {
        start_key(v, 0, keyval, state);
        switch (keyval) {
        case GDK_KEY_c:
        case GDK_KEY_C:          start_select(v, 4, 0, 0); return TRUE;
        case GDK_KEY_v:
        case GDK_KEY_V:
            gdk_clipboard_read_text_async(gtk_widget_get_clipboard(v->area),
                                          NULL, on_paste_text_ready, v);
            return TRUE;
        case GDK_KEY_a:
        case GDK_KEY_A:          start_select(v, 3, 0, 0); return TRUE;
        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add:      ns_proc_view_zoom_in(v); return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract: ns_proc_view_zoom_out(v); return TRUE;
        case GDK_KEY_0:
        case GDK_KEY_KP_0:        ns_proc_view_zoom_reset(v); return TRUE;
        case GDK_KEY_f:
        case GDK_KEY_F:           search_open(v); return TRUE;
        case GDK_KEY_g:
        case GDK_KEY_G:
            request_find(v, (state & GDK_SHIFT_MASK) ? 2 : 1);
            return TRUE;
        case GDK_KEY_p:
        case GDK_KEY_P:           view_save(v, TRUE); return TRUE;
        default: return FALSE;
        }
    }
    if ((state & GDK_ALT_MASK) && !(state & GDK_CONTROL_MASK) &&
        !(state & GDK_META_MASK)) {
        gunichar a = gdk_keyval_to_unicode(keyval);
        if (a && !g_unichar_iscntrl(a)) {
            start_key(v, 0, keyval, state);
            return TRUE;
        }
    }
    double line = 60.0;
    double page = viewport_h(v) / cur_scale(v) - line;
    if (page < line) page = line;
    double vy = gtk_adjustment_get_value(v->vadj);
    double vx = gtk_adjustment_get_value(v->hadj);
    double tx = vx, ty = vy;
    switch (keyval) {
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: start_key(v, 0, keyval, state); return TRUE;
    case GDK_KEY_Down:       ty = vy + line; break;
    case GDK_KEY_Up:         ty = vy - line; break;
    case GDK_KEY_Right:      tx = vx + line; break;
    case GDK_KEY_Left:       tx = vx - line; break;
    case GDK_KEY_Page_Down:
    case GDK_KEY_space:      ty = vy + page; break;
    case GDK_KEY_Page_Up:    ty = vy - page; break;
    case GDK_KEY_Home:       ty = 0; break;
    case GDK_KEY_End:        ty = gtk_adjustment_get_upper(v->vadj); break;
    default:                 start_key(v, 0, keyval, state); return FALSE;
    }
    start_key_full(v, 0, keyval, state, 1, tx, ty);
    return TRUE;
}

static void
on_key_released(GtkEventControllerKey *ctrl, guint keyval, guint keycode,
                GdkModifierType state, gpointer data)
{
    (void)ctrl;
    (void)keycode;
    NsProcView *v = data;
    if (v->opened)
        start_key(v, 1, keyval, state);
}

static void
on_area_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    NsProcView *v = data;
    v->closed = TRUE;
    g_clear_object(&v->im);
    disarm_anim(v);
    if (v->console_poll_id) {
        g_source_remove(v->console_poll_id);
        v->console_poll_id = 0;
    }
    if (v->console_window) {
        GtkWidget *win = v->console_window;
        v->console_window = NULL;
        v->console_notebook = NULL;
        v->console_entry = NULL;
        v->console_view = NULL;
        v->console_buffer = NULL;
        v->net_view = NULL;
        v->net_buffer = NULL;
        v->perf_view = NULL;
        v->perf_buffer = NULL;
        v->layout_view = NULL;
        v->layout_buffer = NULL;
        v->elements_view = NULL;
        v->elements_buffer = NULL;
        v->inspect_entry = NULL;
        gtk_window_destroy(GTK_WINDOW(win));
    }
    if (v->ctx_popover) {
        gtk_widget_unparent(v->ctx_popover);
        v->ctx_popover = NULL;
    }
    v->area = NULL;
    v->perm_revealer = NULL;
    v->perm_label = NULL;
    Req *req = g_new0(Req, 1);
    req->type = REQ_QUIT;
    push_req(v, req);
    /* Unblock the worker if it is mid-request to a wedged renderer, so the join
     * below can't stall the main loop for up to the 30 s IPC read timeout (which
     * would also trip the watchdog heartbeat and restart the whole shell). */
    g_mutex_lock(&v->proc_lock);
    if (v->proc)
        ns_rproc_http_interrupt(v->proc);
    g_mutex_unlock(&v->proc_lock);
    if (v->thread) {
        g_thread_join(v->thread);
        v->thread = NULL;
    }
    pv_unref(v);
}

static void
console_append(NsProcView *v, const char *text)
{
    if (!v->console_buffer || !text || !*text)
        return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(v->console_buffer, &end);
    gtk_text_buffer_insert(v->console_buffer, &end, text, -1);
    if (v->console_view) {
        gtk_text_buffer_get_end_iter(v->console_buffer, &end);
        GtkTextMark *m = gtk_text_buffer_create_mark(v->console_buffer, NULL,
                                                     &end, FALSE);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(v->console_view), m);
        gtk_text_buffer_delete_mark(v->console_buffer, m);
    }
}

static gboolean
console_poll_cb(gpointer data)
{
    NsProcView *v = data;
    if (!v->console_open || !v->opened)
        return G_SOURCE_CONTINUE;
    Req *req = g_new0(Req, 1);
    req->type = REQ_CONSOLE;
    push_req(v, req);
    return G_SOURCE_CONTINUE;
}

static void
on_console_eval(GtkEntry *entry, gpointer data)
{
    NsProcView *v = data;
    const char *src = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!src || !*src || !v->opened)
        return;
    char *echo = g_strdup_printf("> %s\n", src);
    console_append(v, echo);
    g_free(echo);
    Req *req = g_new0(Req, 1);
    req->type = REQ_EVAL;
    req->query = g_strdup(src);
    push_req(v, req);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void build_console_window(NsProcView *v);

static void
console_set_open(NsProcView *v, gboolean open)
{
    if (open && !v->console_window)
        build_console_window(v);
    if (!v->console_window)
        return;
    v->console_open = open;
    if (open) {
        GtkRoot *root = gtk_widget_get_root(v->area);
        if (GTK_IS_WINDOW(root))
            gtk_window_set_transient_for(GTK_WINDOW(v->console_window),
                                         GTK_WINDOW(root));
        gtk_window_present(GTK_WINDOW(v->console_window));
        if (!v->console_poll_id)
            v->console_poll_id = g_timeout_add(NS_PROC_CONSOLE_POLL_MS, console_poll_cb, v);
        gtk_widget_grab_focus(v->console_entry);
    } else {
        gtk_widget_set_visible(v->console_window, FALSE);
        if (v->console_poll_id) {
            g_source_remove(v->console_poll_id);
            v->console_poll_id = 0;
        }
        if (v->area)
            gtk_widget_grab_focus(v->area);
    }
}

static void
console_request_dump(NsProcView *v, int tab)
{
    const char *kind = NULL;
    switch (tab) {
    case DEV_TAB_NETWORK:     kind = "network";     break;
    case DEV_TAB_PERFORMANCE: kind = "performance"; break;
    case DEV_TAB_LAYOUT:      kind = "layout";      break;
    case DEV_TAB_ELEMENTS:    kind = "dom";         break;
    default: return;
    }
    if (!v->opened)
        return;
    Req *req = g_new0(Req, 1);
    req->type = REQ_DUMP;
    req->dump_tab = tab;
    req->query = g_strdup(kind);
    push_req(v, req);
}

static int
console_current_tab(NsProcView *v)
{
    if (!v->console_notebook)
        return DEV_TAB_CONSOLE;
    return gtk_notebook_get_current_page(GTK_NOTEBOOK(v->console_notebook));
}

static void
on_console_clear(GtkButton *b, gpointer data)
{
    (void)b;
    NsProcView *v = data;
    GtkTextBuffer *buf = NULL;
    switch (console_current_tab(v)) {
    case DEV_TAB_CONSOLE:     buf = v->console_buffer;  break;
    case DEV_TAB_NETWORK:     buf = v->net_buffer;      break;
    case DEV_TAB_PERFORMANCE: buf = v->perf_buffer;     break;
    case DEV_TAB_LAYOUT:      buf = v->layout_buffer;   break;
    case DEV_TAB_ELEMENTS:    buf = v->elements_buffer; break;
    default: break;
    }
    if (buf)
        gtk_text_buffer_set_text(buf, "", 0);
}

static void
on_console_refresh(GtkButton *b, gpointer data)
{
    (void)b;
    NsProcView *v = data;
    console_request_dump(v, console_current_tab(v));
}

static void
on_console_notebook_switch(GtkNotebook *nb, GtkWidget *page, guint num,
                           gpointer data)
{
    (void)nb;
    (void)page;
    NsProcView *v = data;
    console_request_dump(v, (int)num);
}

static void
on_inspect_activate(GtkEntry *entry, gpointer data)
{
    NsProcView *v = data;
    const char *sel = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!sel || !*sel || !v->opened)
        return;
    char *esc = g_strescape(sel, NULL);
    char *src = g_strdup_printf(
        "(function(){try{var e=document.querySelector(\"%s\");"
        "if(!e)return \"\";var s=e.outerHTML;"
        "return s.length>20000?s.slice(0,20000)+\"\\n\\u2026(truncated)\":s;}"
        "catch(err){return \"Error: \"+err;}})()",
        esc);
    g_free(esc);
    Req *req = g_new0(Req, 1);
    req->type = REQ_EVAL;
    req->inspect = TRUE;
    req->dump_tab = DEV_TAB_ELEMENTS;
    req->query = src;
    push_req(v, req);
}

static gboolean
on_console_close_request(GtkWindow *win, gpointer data)
{
    (void)win;
    console_set_open(data, FALSE);
    return TRUE;
}

static GtkWidget *
console_make_view(GtkWidget **out_view, GtkTextBuffer **out_buffer,
                  gboolean wrap)
{
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),
                                wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    *out_view = view;
    *out_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    return scroll;
}

static void
console_add_tab(NsProcView *v, GtkWidget *child, const char *title)
{
    gtk_notebook_append_page(GTK_NOTEBOOK(v->console_notebook), child,
                             gtk_label_new(ns_i18n(title)));
}

static void
build_console_window(NsProcView *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(header, "toolbar");
    GtkWidget *spacer = gtk_label_new("");
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, ns_i18n("Refresh"));
    set_accessible_label(refresh, ns_i18n("Refresh"));
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_console_refresh), v);
    GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(clear, ns_i18n("Clear"));
    set_accessible_label(clear, ns_i18n("Clear"));
    g_signal_connect(clear, "clicked", G_CALLBACK(on_console_clear), v);
    gtk_box_append(GTK_BOX(header), spacer);
    gtk_box_append(GTK_BOX(header), refresh);
    gtk_box_append(GTK_BOX(header), clear);

    v->console_notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(v->console_notebook, TRUE);

    GtkWidget *console_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *console_scroll =
        console_make_view(&v->console_view, &v->console_buffer, TRUE);
    v->console_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(v->console_entry),
                                   ns_i18n("Evaluate JavaScript and press Enter"));
    g_signal_connect(v->console_entry, "activate",
                     G_CALLBACK(on_console_eval), v);
    gtk_box_append(GTK_BOX(console_page), console_scroll);
    gtk_box_append(GTK_BOX(console_page), v->console_entry);
    console_add_tab(v, console_page, "Console");

    console_add_tab(v, console_make_view(&v->net_view, &v->net_buffer, FALSE),
                    "Network");
    console_add_tab(v, console_make_view(&v->perf_view, &v->perf_buffer, FALSE),
                    "Performance");
    console_add_tab(v,
                    console_make_view(&v->layout_view, &v->layout_buffer, FALSE),
                    "Layout");

    GtkWidget *elements_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *elements_scroll =
        console_make_view(&v->elements_view, &v->elements_buffer, FALSE);
    v->inspect_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(v->inspect_entry),
        ns_i18n("Inspect: CSS selector, then Enter"));
    g_signal_connect(v->inspect_entry, "activate",
                     G_CALLBACK(on_inspect_activate), v);
    gtk_box_append(GTK_BOX(elements_page), elements_scroll);
    gtk_box_append(GTK_BOX(elements_page), v->inspect_entry);
    console_add_tab(v, elements_page, "Elements");

    g_signal_connect(v->console_notebook, "switch-page",
                     G_CALLBACK(on_console_notebook_switch), v);

    gtk_box_append(GTK_BOX(box), header);
    gtk_box_append(GTK_BOX(box), v->console_notebook);

    v->console_window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(v->console_window),
                         ns_i18n("Developer Tools"));
    gtk_window_set_default_size(GTK_WINDOW(v->console_window), 720, 420);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(v->console_window), TRUE);
    gtk_window_set_child(GTK_WINDOW(v->console_window), box);
    g_signal_connect(v->console_window, "close-request",
                     G_CALLBACK(on_console_close_request), v);
}

static void
build_search_bar(NsProcView *v)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(box, "toolbar");
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_end(box, 6);

    v->search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(v->search_entry, 220, -1);
    set_accessible_label(v->search_entry, ns_i18n("Find in page"));
    g_signal_connect(v->search_entry, "search-changed",
                     G_CALLBACK(on_search_changed), v);
    g_signal_connect(v->search_entry, "activate",
                     G_CALLBACK(on_search_next), v);
    g_signal_connect(v->search_entry, "next-match",
                     G_CALLBACK(on_search_next), v);
    g_signal_connect(v->search_entry, "previous-match",
                     G_CALLBACK(on_search_prev), v);
    g_signal_connect(v->search_entry, "stop-search",
                     G_CALLBACK(on_search_stop), v);

    v->search_label = gtk_label_new("");
    gtk_widget_set_size_request(v->search_label, 56, -1);

    GtkWidget *prev = gtk_button_new_from_icon_name("go-up-symbolic");
    GtkWidget *next = gtk_button_new_from_icon_name("go-down-symbolic");
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(prev, ns_i18n("Previous match (Shift+Enter)"));
    gtk_widget_set_tooltip_text(next, ns_i18n("Next match (Enter)"));
    gtk_widget_set_tooltip_text(close, ns_i18n("Close (Esc)"));
    set_accessible_label(prev, ns_i18n("Previous match"));
    set_accessible_label(next, ns_i18n("Next match"));
    set_accessible_label(close, ns_i18n("Close search"));
    g_signal_connect(prev, "clicked", G_CALLBACK(on_search_prev), v);
    g_signal_connect(next, "clicked", G_CALLBACK(on_search_next), v);
    g_signal_connect(close, "clicked", G_CALLBACK(on_search_close_clicked), v);

    gtk_box_append(GTK_BOX(box), v->search_entry);
    gtk_box_append(GTK_BOX(box), v->search_label);
    gtk_box_append(GTK_BOX(box), prev);
    gtk_box_append(GTK_BOX(box), next);
    gtk_box_append(GTK_BOX(box), close);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), box);

    v->search_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(v->search_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_child(GTK_REVEALER(v->search_revealer), frame);
    gtk_widget_set_halign(v->search_revealer, GTK_ALIGN_END);
    gtk_widget_set_valign(v->search_revealer, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(v->overlay), v->search_revealer);
}

static void
build_perm_bar(NsProcView *v)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);

    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-question-symbolic");

    v->perm_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(v->perm_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(v->perm_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(v->perm_label, TRUE);

    GtkWidget *allow = gtk_button_new_with_label(ns_i18n("Allow"));
    gtk_widget_add_css_class(allow, "suggested-action");
    gtk_widget_set_tooltip_text(allow, ns_i18n("Allow and trust this site"));
    GtkWidget *deny = gtk_button_new_with_label(ns_i18n("Not now"));
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    set_accessible_label(close, ns_i18n("Dismiss"));
    g_signal_connect(allow, "clicked", G_CALLBACK(on_perm_allow), v);
    g_signal_connect(deny, "clicked", G_CALLBACK(on_perm_deny), v);
    g_signal_connect(close, "clicked", G_CALLBACK(on_perm_deny), v);

    gtk_box_append(GTK_BOX(bar), icon);
    gtk_box_append(GTK_BOX(bar), v->perm_label);
    gtk_box_append(GTK_BOX(bar), allow);
    gtk_box_append(GTK_BOX(bar), deny);
    gtk_box_append(GTK_BOX(bar), close);

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_child(GTK_FRAME(frame), bar);

    v->perm_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(v->perm_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_child(GTK_REVEALER(v->perm_revealer), frame);
}

static gboolean
on_file_drop(GtkDropTarget *target, const GValue *value, double x, double y,
             gpointer data)
{
    (void)target;
    NsProcView *v = data;
    if (!v->opened || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;
    GdkFileList *fl = g_value_get_boxed(value);
    if (!fl)
        return FALSE;
    GSList *files = gdk_file_list_get_files(fl);
    GString *paths = g_string_new(NULL);
    for (GSList *l = files; l; l = l->next) {
        char *p = g_file_get_path(G_FILE(l->data));
        if (!p)
            continue;
        if (paths->len)
            g_string_append_c(paths, '\n');
        g_string_append(paths, p);
        g_free(p);
    }
    g_slist_free(files);
    if (paths->len == 0) {
        g_string_free(paths, TRUE);
        return FALSE;
    }
    double s = cur_scale(v);
    int px = v->scroll_x + (int)(x / s);
    int py = v->scroll_y + (int)(y / s);
    start_dropfiles(v, px, py, g_string_free(paths, FALSE));
    return TRUE;
}

NsProcView *
ns_proc_view_new(void)
{
    NsProcView *v = g_new0(NsProcView, 1);
    g_ref_count_init(&v->rc);
    g_mutex_init(&v->proc_lock);
    v->renderer_path = ns_proc_renderer_path();
    v->queue = g_async_queue_new();
    v->history = g_ptr_array_new_with_free_func(g_free);
    v->hist_index = -1;
    v->link_pending_action = ACT_HOVER;
    v->pending_record = TRUE;
    v->scale = 1.0;

    v->hadj = gtk_adjustment_new(0, 0, 1, 60, 60, 1);
    v->vadj = gtk_adjustment_new(0, 0, 1, 60, 60, 1);
    g_signal_connect(v->hadj, "value-changed", G_CALLBACK(on_adj_changed), v);
    g_signal_connect(v->vadj, "value-changed", G_CALLBACK(on_adj_changed), v);

    v->area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(v->area, TRUE);
    gtk_widget_set_vexpand(v->area, TRUE);
    gtk_widget_set_focusable(v->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->area), on_draw, v, NULL);
    g_signal_connect(v->area, "resize", G_CALLBACK(on_resize), v);

    GtkWidget *grid = gtk_grid_new();
    v->vscroll =
        gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, v->vadj);
    v->hscroll =
        gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, v->hadj);
    gtk_grid_attach(GTK_GRID(grid), v->area, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v->vscroll, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), v->hscroll, 0, 1, 1, 1);
    gtk_widget_set_visible(v->vscroll, FALSE);
    gtk_widget_set_visible(v->hscroll, FALSE);

    v->overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(v->overlay), grid);
    gtk_widget_set_vexpand(v->overlay, TRUE);
    build_perm_bar(v);
    v->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(v->root), v->perm_revealer);
    gtk_box_append(GTK_BOX(v->root), v->overlay);
    gtk_widget_set_vexpand(v->root, TRUE);
    build_search_bar(v);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), v);
    g_signal_connect(click, "released", G_CALLBACK(on_released), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), v);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), v);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(drag));

    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
                                              GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", G_CALLBACK(on_file_drop), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(drop));

    GtkGesture *middle = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
    g_signal_connect(middle, "pressed", G_CALLBACK(on_pressed), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(middle));

    ctx_install_actions(v);
    GtkGesture *secondary = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(secondary, "pressed", G_CALLBACK(on_secondary_pressed), v);
    gtk_widget_add_controller(v->area, GTK_EVENT_CONTROLLER(secondary));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), v);
    gtk_widget_add_controller(v->area, motion);

    GtkEventController *scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), v);
    gtk_widget_add_controller(v->area, scroll);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), v);
    g_signal_connect(key, "key-released", G_CALLBACK(on_key_released), v);
    v->im = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(v->im, v->area);
    g_signal_connect(v->im, "commit", G_CALLBACK(on_im_commit), v);
    gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(key),
                                            v->im);
    gtk_widget_add_controller(v->area, key);

    g_signal_connect(v->area, "destroy", G_CALLBACK(on_area_destroy), v);

    v->thread = g_thread_new("ns-proc-view", worker_main, pv_ref(v));
    return v;
}

GtkWidget *ns_proc_view_widget(NsProcView *v) { return v->root; }

void
ns_proc_view_set_notify(NsProcView *v, NsProcNotify cb, gpointer ud)
{
    v->notify = cb;
    v->notify_ud = ud;
}

void
ns_proc_view_set_private(NsProcView *v, gboolean private_mode)
{
    if (v)
        v->private_mode = private_mode;
}

gboolean
ns_proc_view_is_private(NsProcView *v)
{
    return v && v->private_mode;
}
