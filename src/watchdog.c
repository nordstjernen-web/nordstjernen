/* Nordstjernen — supervisor that restarts the browser on crash or hang.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "watchdog.h"
#include "debuglog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <glib-unix.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

#ifdef __linux__
#include <sys/prctl.h>
#endif

#define NS_WATCHDOG_FLAG           "--watchdog"
#define NS_WATCHDOG_NO_FLAG        "--no-watchdog"
#define NS_WATCHDOG_CHILD_FLAG     "--watchdog-child"
#define NS_WATCHDOG_SESSION_PREFIX "--watchdog-session="
#define NS_WATCHDOG_RECOVER_ENV    "NS_WATCHDOG_RECOVER"

#define NS_WATCHDOG_BEAT_SECS      2
#define NS_WATCHDOG_CHECK_SECS     1
#define NS_WATCHDOG_HANG_MIN_SECS  60
#define NS_WATCHDOG_BACKOFF_MS     1000
#define NS_WATCHDOG_BURST_MAX      5
#define NS_WATCHDOG_BURST_SECS     60
#define NS_WATCHDOG_STOP_GRACE_SECS 3
#define NS_WATCHDOG_HANG_EXIT      70

static gint g_beat;
static int  g_hang_secs;

static gboolean
ns_watchdog_beat(gpointer user_data)
{
    (void)user_data;
    g_atomic_int_inc(&g_beat);
    return G_SOURCE_CONTINUE;
}

static gpointer
ns_watchdog_hang_thread(gpointer user_data)
{
    (void)user_data;
    gint   last = g_atomic_int_get(&g_beat);
    gint64 last_change = g_get_monotonic_time();
    for (;;) {
        g_usleep((gulong)NS_WATCHDOG_CHECK_SECS * G_USEC_PER_SEC);
        gint   cur = g_atomic_int_get(&g_beat);
        gint64 now = g_get_monotonic_time();
        if (cur != last) {
            last = cur;
            last_change = now;
            continue;
        }
        if (now - last_change > (gint64)g_hang_secs * G_USEC_PER_SEC) {
            fprintf(stderr,
                    "ns_watchdog: main loop unresponsive for %ds — exiting for restart\n",
                    g_hang_secs);
            fflush(stderr);
            _Exit(NS_WATCHDOG_HANG_EXIT);
        }
    }
    return NULL;
}

static gboolean g_watchdog_child;

void
ns_watchdog_child_guard_parent_death(void)
{
    g_watchdog_child = TRUE;
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1)
        _Exit(0);
#endif
}

int
ns_watchdog_supervisor_pid(void)
{
    if (!g_watchdog_child) return 0;
#ifdef G_OS_WIN32
    DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof pe;
    DWORD parent = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) {
                parent = pe.th32ParentProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return (int)parent;
#else
    pid_t p = getppid();
    return p > 1 ? (int)p : 0;
#endif
}

void
ns_watchdog_child_arm_hang_monitor(int js_budget_ms)
{
    g_hang_secs = js_budget_ms / 1000 + NS_WATCHDOG_HANG_MIN_SECS;
    if (g_hang_secs < NS_WATCHDOG_HANG_MIN_SECS)
        g_hang_secs = NS_WATCHDOG_HANG_MIN_SECS;
    g_message("ns_watchdog: hang monitor armed, exit after %ds unresponsive",
              g_hang_secs);
    g_timeout_add_seconds(NS_WATCHDOG_BEAT_SECS, ns_watchdog_beat, NULL);
    g_thread_unref(g_thread_new("nd-watchdog", ns_watchdog_hang_thread, NULL));
}

static const char *
ns_watchdog_arg_value(int argc, char **argv, const char *prefix)
{
    for (int i = 1; i < argc; i++)
        if (argv[i] && g_str_has_prefix(argv[i], prefix))
            return argv[i] + strlen(prefix);
    return NULL;
}

const char *
ns_watchdog_child_session_arg(int argc, char **argv)
{
    return ns_watchdog_arg_value(argc, argv, NS_WATCHDOG_SESSION_PREFIX);
}

gboolean
ns_watchdog_is_child(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (argv[i] && g_strcmp0(argv[i], NS_WATCHDOG_CHILD_FLAG) == 0)
            return TRUE;
    return FALSE;
}

gboolean
ns_watchdog_child_is_recovery(void)
{
    const char *v = g_getenv(NS_WATCHDOG_RECOVER_ENV);
    return v && g_strcmp0(v, "1") == 0;
}

static gboolean
ns_watchdog_is_oneshot(const char *arg)
{
    return g_strcmp0(arg, "--headless")     == 0 ||
           g_strcmp0(arg, "--print-config") == 0 ||
           g_str_has_prefix(arg, "--dump=")       ||
           g_str_has_prefix(arg, "--eval=")       ||
           g_str_has_prefix(arg, "--inspect=")    ||
           g_str_has_prefix(arg, "--inspect-at=");
}

gboolean
ns_watchdog_should_supervise(int argc, char **argv, gboolean enabled_by_default)
{
    gboolean forced = FALSE;
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (g_strcmp0(argv[i], NS_WATCHDOG_CHILD_FLAG) == 0) return FALSE;
        if (g_strcmp0(argv[i], NS_WATCHDOG_NO_FLAG) == 0)    return FALSE;
        if (ns_watchdog_is_oneshot(argv[i]))                 return FALSE;
        if (g_strcmp0(argv[i], NS_WATCHDOG_FLAG) == 0)       forced = TRUE;
    }
    return forced || enabled_by_default;
}

typedef struct {
    char      **child_argv;
    char       *session_path;
    GMainLoop  *loop;
    GPid        pid;
    gboolean    have_pid;
    gboolean    stopping;
    guint       watch_id;
    int         burst_count;
    gint64      burst_start_us;
    int         exit_status;
} ns_watchdog;

static void
ns_watchdog_report_failure(const char *detail)
{
#ifdef G_OS_WIN32
    const char *log = ns_debug_log_file_path();
    char *body = g_strdup_printf(
        "Nordstjernen could not start.\n\n%s\n\n"
        "A diagnostic log was written to:\n%s\n\n"
        "If this persists, try launching with the cairo renderer by setting "
        "the environment variable NS_GSK_RENDERER=cairo before starting.",
        detail ? detail : "The browser process exited before opening a window.",
        log ? log : "(log file unavailable)");
    wchar_t *wbody = (wchar_t *)g_utf8_to_utf16(body, -1, NULL, NULL, NULL);
    if (wbody) {
        MessageBoxW(NULL, wbody, L"Nordstjernen",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        g_free(wbody);
    }
    g_free(body);
#else
    (void)detail;
#endif
}

static void
ns_watchdog_kill(GPid pid, gboolean force)
{
#ifdef G_OS_WIN32
    (void)force;
    TerminateProcess((HANDLE)pid, 1);
#else
    kill((pid_t)pid, force ? SIGKILL : SIGTERM);
#endif
}

static gboolean ns_watchdog_spawn(ns_watchdog *wd, gboolean recover);

#ifdef G_OS_WIN32
typedef struct {
    wchar_t *data;
    size_t len;
    size_t cap;
} ns_watchdog_wbuf;

static gboolean
ns_watchdog_wbuf_reserve(ns_watchdog_wbuf *b, size_t extra)
{
    if (extra > ((size_t)-1) - b->len - 1) return FALSE;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return TRUE;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2) return FALSE;
        cap *= 2;
    }
    wchar_t *data = g_renew(wchar_t, b->data, cap);
    b->data = data;
    b->cap = cap;
    return TRUE;
}

static gboolean
ns_watchdog_wbuf_append_char(ns_watchdog_wbuf *b, wchar_t ch)
{
    if (!ns_watchdog_wbuf_reserve(b, 1)) return FALSE;
    b->data[b->len++] = ch;
    b->data[b->len] = L'\0';
    return TRUE;
}

static gboolean
ns_watchdog_wbuf_append_many(ns_watchdog_wbuf *b, wchar_t ch, size_t count)
{
    if (!ns_watchdog_wbuf_reserve(b, count)) return FALSE;
    for (size_t i = 0; i < count; i++) b->data[b->len++] = ch;
    b->data[b->len] = L'\0';
    return TRUE;
}

static gboolean
ns_watchdog_wbuf_append_quoted(ns_watchdog_wbuf *b, const wchar_t *arg)
{
    if (!ns_watchdog_wbuf_append_char(b, L'"')) return FALSE;
    size_t slashes = 0;
    for (const wchar_t *p = arg; *p; p++) {
        if (*p == L'\\') {
            slashes++;
            continue;
        }
        if (*p == L'"') {
            if (!ns_watchdog_wbuf_append_many(b, L'\\', slashes * 2 + 1))
                return FALSE;
            if (!ns_watchdog_wbuf_append_char(b, *p)) return FALSE;
            slashes = 0;
            continue;
        }
        if (slashes > 0) {
            if (!ns_watchdog_wbuf_append_many(b, L'\\', slashes)) return FALSE;
            slashes = 0;
        }
        if (!ns_watchdog_wbuf_append_char(b, *p)) return FALSE;
    }
    if (slashes > 0 &&
        !ns_watchdog_wbuf_append_many(b, L'\\', slashes * 2))
        return FALSE;
    return ns_watchdog_wbuf_append_char(b, L'"');
}

static wchar_t *
ns_watchdog_build_command_line(char **argv)
{
    ns_watchdog_wbuf b = {0};
    for (int i = 0; argv[i]; i++) {
        wchar_t *arg = g_utf8_to_utf16(argv[i], -1, NULL, NULL, NULL);
        if (!arg) {
            g_free(b.data);
            return NULL;
        }
        gboolean ok = (i == 0 || ns_watchdog_wbuf_append_char(&b, L' ')) &&
                      ns_watchdog_wbuf_append_quoted(&b, arg);
        g_free(arg);
        if (!ok) {
            g_free(b.data);
            return NULL;
        }
    }
    return b.data;
}

static void
ns_watchdog_restore_recover_env(const wchar_t *old)
{
    SetEnvironmentVariableW(L"NS_WATCHDOG_RECOVER", old && *old ? old : NULL);
}

static gboolean
ns_watchdog_spawn_win32(ns_watchdog *wd, gboolean recover, GPid *pid,
                        GError **err)
{
    wchar_t *app = g_utf8_to_utf16(wd->child_argv[0], -1, NULL, NULL, NULL);
    wchar_t *cmd = ns_watchdog_build_command_line(wd->child_argv);
    const char *old_utf8 = g_getenv(NS_WATCHDOG_RECOVER_ENV);
    wchar_t *old = old_utf8 ? g_utf8_to_utf16(old_utf8, -1, NULL, NULL, NULL)
                            : NULL;
    if (!app || !cmd) {
        g_set_error(err, G_SPAWN_ERROR, G_SPAWN_ERROR_NOMEM,
                    "could not build Windows child command line");
        g_free(app);
        g_free(cmd);
        g_free(old);
        return FALSE;
    }

    SetEnvironmentVariableW(L"NS_WATCHDOG_RECOVER", recover ? L"1" : NULL);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    BOOL ok = CreateProcessW(app, cmd, NULL, NULL, TRUE, 0,
                             NULL, NULL, &si, &pi);
    DWORD error = ok ? 0 : GetLastError();
    ns_watchdog_restore_recover_env(old);
    g_free(app);
    g_free(cmd);
    g_free(old);

    if (!ok) {
        char *msg = g_win32_error_message((gint)error);
        g_set_error(err, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
                    "CreateProcessW failed: %s", msg ? msg : "unknown error");
        g_free(msg);
        return FALSE;
    }

    CloseHandle(pi.hThread);
    *pid = (GPid)pi.hProcess;
    return TRUE;
}
#endif

static gboolean
ns_watchdog_respawn_cb(gpointer user_data)
{
    ns_watchdog *wd = user_data;
    if (!wd->stopping)
        ns_watchdog_spawn(wd, TRUE);
    return G_SOURCE_REMOVE;
}

static void
ns_watchdog_schedule_restart(ns_watchdog *wd)
{
    gint64 now = g_get_monotonic_time();
    if (now - wd->burst_start_us > (gint64)NS_WATCHDOG_BURST_SECS * G_USEC_PER_SEC) {
        wd->burst_start_us = now;
        wd->burst_count = 0;
    }
    wd->burst_count++;
    if (wd->burst_count > NS_WATCHDOG_BURST_MAX) {
        g_warning("ns_watchdog: child failed %d times in under %ds — giving up",
                  wd->burst_count, NS_WATCHDOG_BURST_SECS);
        ns_watchdog_report_failure(
            "The browser repeatedly exited during startup.");
        wd->exit_status = 1;
        g_main_loop_quit(wd->loop);
        return;
    }
    g_message("ns_watchdog: restarting browser (attempt %d)", wd->burst_count);
    g_timeout_add(NS_WATCHDOG_BACKOFF_MS, ns_watchdog_respawn_cb, wd);
}

static void
ns_watchdog_child_exited(GPid pid, gint status, gpointer user_data)
{
    ns_watchdog *wd = user_data;
    g_spawn_close_pid(pid);
    wd->have_pid = FALSE;
    wd->watch_id = 0;

    if (wd->stopping) {
        g_main_loop_quit(wd->loop);
        return;
    }

    gboolean clean;
#ifdef G_OS_WIN32
    clean = (status == 0);
#else
    clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    if (clean) {
        wd->exit_status = 0;
        g_main_loop_quit(wd->loop);
        return;
    }

#ifdef G_OS_WIN32
    g_warning("ns_watchdog: browser exited abnormally (code %d)", status);
#else
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGTERM || sig == SIGINT || sig == SIGHUP ||
            sig == SIGQUIT || sig == SIGKILL) {
            g_message("ns_watchdog: browser terminated by signal %d — stopping",
                      sig);
            wd->exit_status = 0;
            g_main_loop_quit(wd->loop);
            return;
        }
        g_warning("ns_watchdog: browser stopped by signal %d", sig);
    } else {
        g_warning("ns_watchdog: browser exited with code %d", WEXITSTATUS(status));
    }
#endif
    ns_watchdog_schedule_restart(wd);
}

static gboolean
ns_watchdog_spawn(ns_watchdog *wd, gboolean recover)
{
    GError *err = NULL;
    GPid pid = 0;
#ifdef G_OS_WIN32
    gboolean ok = ns_watchdog_spawn_win32(wd, recover, &pid, &err);
#else
    char **envp = g_get_environ();
    if (recover)
        envp = g_environ_setenv(envp, NS_WATCHDOG_RECOVER_ENV, "1", TRUE);
    else
        envp = g_environ_unsetenv(envp, NS_WATCHDOG_RECOVER_ENV);
    gboolean ok = g_spawn_async(NULL, wd->child_argv, envp,
                                G_SPAWN_DO_NOT_REAP_CHILD,
                                NULL, NULL, &pid, &err);
    g_strfreev(envp);
#endif
    if (!ok) {
        g_warning("ns_watchdog: failed to launch browser: %s",
                  err ? err->message : "unknown error");
        char *detail = g_strdup_printf("Failed to launch the browser process: %s",
                                       err ? err->message : "unknown error");
        ns_watchdog_report_failure(detail);
        g_free(detail);
        g_clear_error(&err);
        wd->exit_status = 1;
        g_main_loop_quit(wd->loop);
        return FALSE;
    }
    wd->pid = pid;
    wd->have_pid = TRUE;
    wd->watch_id = g_child_watch_add(pid, ns_watchdog_child_exited, wd);
    return TRUE;
}

#ifndef G_OS_WIN32
static gboolean
ns_watchdog_force_kill_cb(gpointer user_data)
{
    ns_watchdog *wd = user_data;
    if (wd->have_pid)
        ns_watchdog_kill(wd->pid, TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean
ns_watchdog_signal_cb(gpointer user_data)
{
    ns_watchdog *wd = user_data;
    if (!wd->stopping) {
        wd->stopping = TRUE;
        wd->exit_status = 0;
        if (wd->have_pid) {
            ns_watchdog_kill(wd->pid, FALSE);
            g_timeout_add_seconds(NS_WATCHDOG_STOP_GRACE_SECS,
                                  ns_watchdog_force_kill_cb, wd);
        } else {
            g_main_loop_quit(wd->loop);
        }
    }
    return G_SOURCE_CONTINUE;
}
#endif

static char **
ns_watchdog_build_child_argv(const char *self_exe, int argc, char **argv,
                             const char *session_path)
{
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, g_strdup(self_exe));
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (g_strcmp0(argv[i], NS_WATCHDOG_FLAG) == 0) continue;
        if (g_strcmp0(argv[i], NS_WATCHDOG_NO_FLAG) == 0) continue;
        if (g_strcmp0(argv[i], NS_WATCHDOG_CHILD_FLAG) == 0) continue;
        if (g_str_has_prefix(argv[i], NS_WATCHDOG_SESSION_PREFIX)) continue;
        g_ptr_array_add(args, g_strdup(argv[i]));
    }
    g_ptr_array_add(args, g_strdup(NS_WATCHDOG_CHILD_FLAG));
    g_ptr_array_add(args, g_strconcat(NS_WATCHDOG_SESSION_PREFIX, session_path, NULL));
    g_ptr_array_add(args, NULL);
    return (char **)g_ptr_array_free(args, FALSE);
}

int
ns_watchdog_run_supervisor(const char *self_exe, int argc, char **argv)
{
    ns_watchdog wd = { 0 };

    char *uuid = g_uuid_string_random();
    char *name = g_strconcat("nordstjernen-watchdog-", uuid, ".session", NULL);
    wd.session_path = g_build_filename(g_get_user_runtime_dir(), name, NULL);
    g_free(name);
    g_free(uuid);

    wd.child_argv = ns_watchdog_build_child_argv(self_exe, argc, argv,
                                                 wd.session_path);
    wd.loop = g_main_loop_new(NULL, FALSE);
    wd.burst_start_us = g_get_monotonic_time();

#ifndef G_OS_WIN32
    g_unix_signal_add(SIGINT, ns_watchdog_signal_cb, &wd);
    g_unix_signal_add(SIGTERM, ns_watchdog_signal_cb, &wd);
#endif

    g_message("ns_watchdog: supervising browser");

    if (ns_watchdog_spawn(&wd, FALSE))
        g_main_loop_run(wd.loop);

    if (wd.watch_id) g_source_remove(wd.watch_id);
    if (wd.have_pid) {
        ns_watchdog_kill(wd.pid, TRUE);
        g_spawn_close_pid(wd.pid);
    }
    g_main_loop_unref(wd.loop);
    g_unlink(wd.session_path);
    g_strfreev(wd.child_argv);
    g_free(wd.session_path);
    return wd.exit_status;
}
