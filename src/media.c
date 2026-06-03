/* Nordstjernen — hands audio/video URLs off to an external player.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "media.h"

#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#endif

static nd_media_open_uri_fn nd_media_open_uri = NULL;

void
nd_media_set_open_uri_handler(nd_media_open_uri_fn fn)
{
    nd_media_open_uri = fn;
}

static gboolean
nd_media_url_is_safe(const char *u, gboolean allow_local)
{
    if (!u || !*u || u[0] == '-' || strlen(u) > 8192) return FALSE;
    for (const char *c = u; *c; c++)
        if ((guchar)*c < 0x20) return FALSE;
    static const char *const remote[] = {
        "http://", "https://", "ftp://", "rtsp://", "rtmp://", NULL,
    };
    for (int i = 0; remote[i]; i++)
        if (g_str_has_prefix(u, remote[i])) return TRUE;
    if (!allow_local) return FALSE;
    return g_str_has_prefix(u, "file://") || u[0] == '/';
}

static char *
nd_media_suggest_message(gboolean is_video, gboolean stream, const char *app)
{
    if (stream)
        return g_strdup_printf(
            "This looks like a streaming page.\n\n"
            "Nordstjernen plays it by handing the link to an external "
            "player. Install %s to play it.", app);
    return g_strdup_printf(
        "No media player was found.\n\n"
        "Nordstjernen plays %s by handing the link to an external player. "
        "Install %s to play this content.",
        is_video ? "video" : "audio", app);
}

typedef struct {
    GtkWindow *parent;
    char      *app_url;
} nd_media_suggest_ctx;

static void
nd_media_suggest_response(GObject *src, GAsyncResult *res, gpointer ud)
{
    nd_media_suggest_ctx *c = ud;
    int idx = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    if (idx == 0 && nd_media_open_uri && c->parent)
        nd_media_open_uri(c->parent, c->app_url);
    if (c->parent)
        g_object_remove_weak_pointer(G_OBJECT(c->parent), (gpointer *)&c->parent);
    g_free(c->app_url);
    g_free(c);
}

static void
nd_media_suggest(GtkWindow *parent, const char *message,
                 const char *app, const char *app_url)
{
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", message);
    char *get_label = g_strdup_printf("Get %s", app);
    const char *buttons[] = { get_label, "Close", NULL };
    gtk_alert_dialog_set_buttons(dlg, buttons);
    gtk_alert_dialog_set_default_button(dlg, 0);
    gtk_alert_dialog_set_cancel_button(dlg, 1);
    gtk_alert_dialog_set_modal(dlg, TRUE);

    nd_media_suggest_ctx *c = g_new0(nd_media_suggest_ctx, 1);
    c->parent = parent;
    c->app_url = g_strdup(app_url);
    if (parent)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&c->parent);
    gtk_alert_dialog_choose(dlg, parent, NULL, nd_media_suggest_response, c);

    g_object_unref(dlg);
    g_free(get_label);
}

#ifndef G_OS_WIN32

static gboolean
nd_media_spawnv(char **argv)
{
    GError *err = NULL;
    gboolean ok = g_spawn_async(NULL, argv, NULL,
                                G_SPAWN_SEARCH_PATH |
                                G_SPAWN_STDOUT_TO_DEV_NULL |
                                G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL, NULL, NULL, &err);
    if (!ok) {
        g_warning("nd_media: failed to launch %s: %s",
                  argv[0], err ? err->message : "unknown error");
        g_clear_error(&err);
    }
    return ok;
}

#ifndef __APPLE__
static const char *const nd_media_mime_types[] = {
    "video/mp4", "video/webm", "video/x-matroska", "audio/mpeg", NULL,
};

static char *
nd_media_find_player(void)
{
    static const char *const players[] = {
        "mpv", "vlc", "celluloid", "totem", "mplayer", "ffplay", NULL,
    };
    for (int i = 0; players[i]; i++) {
        char *path = g_find_program_in_path(players[i]);
        if (path) return path;
    }
    return NULL;
}

static gboolean
nd_media_ytdlp_available(void)
{
    char *p = g_find_program_in_path("yt-dlp");
    if (!p) p = g_find_program_in_path("youtube-dl");
    if (p) { g_free(p); return TRUE; }
    return FALSE;
}

static GAppInfo *
nd_media_default_handler(const char *url)
{
    char *base = g_path_get_basename(url);
    gboolean uncertain = TRUE;
    char *ctype = base ? g_content_type_guess(base, NULL, 0, &uncertain) : NULL;
    GAppInfo *app = NULL;
    if (ctype && !uncertain)
        app = g_app_info_get_default_for_type(ctype, FALSE);
    g_free(ctype);
    g_free(base);
    for (int i = 0; !app && nd_media_mime_types[i]; i++)
        app = g_app_info_get_default_for_type(nd_media_mime_types[i], FALSE);
    return app;
}

static gboolean
nd_media_player_available(const char *url, gboolean stream)
{
    char *path = nd_media_find_player();
    if (path) { g_free(path); return TRUE; }
    if (stream) return FALSE;
    GAppInfo *app = nd_media_default_handler(url);
    if (app) { g_object_unref(app); return TRUE; }
    return FALSE;
}

static gboolean
nd_media_run_player(const char *url)
{
    char *player = nd_media_find_player();
    if (player) {
        char *argv[] = { player, (char *)url, NULL };
        gboolean ok = nd_media_spawnv(argv);
        g_free(player);
        if (ok) return TRUE;
    }
    GAppInfo *app = nd_media_default_handler(url);
    if (!app) return FALSE;
    GList *uris = g_list_append(NULL, (gpointer)url);
    gboolean ok = g_app_info_launch_uris(app, uris, NULL, NULL);
    g_list_free(uris);
    g_object_unref(app);
    return ok;
}
#endif

#if defined(__linux__)
static int nd_media_broker_fd = -1;

static gboolean
read_all(int fd, void *buf, size_t n)
{
    guint8 *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return FALSE;
    }
    return TRUE;
}

static G_GNUC_NORETURN void
nd_media_broker_loop(int fd)
{
    signal(SIGCHLD, SIG_IGN);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    for (;;) {
        guint32 len;
        if (!read_all(fd, &len, sizeof len) || len == 0 || len > 65536)
            _exit(0);
        char *buf = g_malloc(len + 1);
        if (!read_all(fd, buf, len)) { g_free(buf); _exit(0); }
        buf[len] = '\0';
        if (nd_media_url_is_safe(buf, TRUE))
            nd_media_run_player(buf);
        g_free(buf);
    }
}

void
nd_media_broker_start(void)
{
    if (nd_media_broker_fd >= 0) return;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); return; }
    if (pid == 0) {
        close(sv[0]);
        nd_media_broker_loop(sv[1]);
    }
    close(sv[1]);
    nd_media_broker_fd = sv[0];
    fcntl(nd_media_broker_fd, F_SETFD, FD_CLOEXEC);
}

static gboolean
nd_media_broker_send(const char *url)
{
    if (nd_media_broker_fd < 0) return FALSE;
    guint32 len = (guint32)strlen(url);
    if (len == 0) return FALSE;
    return send(nd_media_broker_fd, &len, sizeof len, MSG_NOSIGNAL)
               == (gssize)sizeof len &&
           send(nd_media_broker_fd, url, len, MSG_NOSIGNAL) == (gssize)len;
}
#else
void
nd_media_broker_start(void)
{
}
#endif

#else
void
nd_media_broker_start(void)
{
}
#endif

gboolean
nd_media_launch_external(GtkWindow *parent, const char *url,
                         gboolean is_video, gboolean stream)
{
    if (!url || !*url) return FALSE;

#if defined(G_OS_WIN32)
    if (!nd_media_url_is_safe(url, FALSE)) return FALSE;
    gunichar2 *wurl = g_utf8_to_utf16(url, -1, NULL, NULL, NULL);
    if (wurl) {
        HINSTANCE rc = ShellExecuteW(NULL, L"open", (LPCWSTR)wurl,
                                     NULL, NULL, SW_SHOWNORMAL);
        g_free(wurl);
        if ((INT_PTR)rc > 32) return TRUE;
    }
    char *m = nd_media_suggest_message(is_video, stream, "VLC");
    nd_media_suggest(parent, m, "VLC", "https://videolan.org");
    g_free(m);
    return FALSE;
#elif defined(__APPLE__)
    if (!nd_media_url_is_safe(url, FALSE)) return FALSE;
    static const char *const apps[] = {
        "IINA", "VLC", "mpv", "QuickTime Player", NULL,
    };
    for (int i = 0; apps[i]; i++) {
        char *bundle = g_strdup_printf("/Applications/%s.app", apps[i]);
        gboolean present = g_file_test(bundle, G_FILE_TEST_EXISTS);
        g_free(bundle);
        if (!present) continue;
        char *argv[] = {
            (char *)"open", (char *)"-a", (char *)apps[i], (char *)url, NULL,
        };
        if (nd_media_spawnv(argv)) return TRUE;
    }
    char *argv[] = { (char *)"open", (char *)url, NULL };
    if (nd_media_spawnv(argv)) return TRUE;
    char *m = nd_media_suggest_message(is_video, stream, "IINA");
    nd_media_suggest(parent, m, "IINA", "https://iina.io");
    g_free(m);
    return FALSE;
#else
    if (!nd_media_url_is_safe(url, stream ? FALSE : TRUE)) return FALSE;
    if (!nd_media_player_available(url, stream)) {
        char *m = nd_media_suggest_message(is_video, stream, "mpv");
        nd_media_suggest(parent, m, "mpv", "https://mpv.io");
        g_free(m);
        return FALSE;
    }
    if (stream && !nd_media_ytdlp_available()) {
        nd_media_suggest(parent,
            "Playing a streaming page like this needs yt-dlp so the media "
            "player can resolve the video. Install yt-dlp and try again.",
            "yt-dlp", "https://github.com/yt-dlp/yt-dlp");
        return FALSE;
    }
#if defined(__linux__)
    if (nd_media_broker_send(url)) return TRUE;
#endif
    return nd_media_run_player(url);
#endif
}
