/* Nordstjernen — application entry point: process bootstrap, headless render
 * driver dispatch, and the out-of-process (IPC) GTK browser shell. */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <glib-unix.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#if defined(G_OS_WIN32) && defined(NS_HAVE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

#include "bytecode_cache.h"
#include "cache.h"
#include "config.h"
#include "debuglog.h"
#include "font.h"
#include "engine.h"
#include "headless.h"
#include "spellcheck.h"
#include "history.h"
#include "i18n.h"
#include "net.h"
#include "proc_limits.h"
#include "procview.h"
#include "procwindow.h"
#include "rproc_inproc.h"
#include "security.h"
#include "threaddump.h"
#include "watchdog.h"

static char *g_self_exe;

const char *
ns_app_self_exe(void)
{
    return g_self_exe;
}

static void
init_self_exe(const char *argv0)
{
#ifdef __linux__
    char *resolved = g_file_read_link("/proc/self/exe", NULL);
    if (resolved) { g_self_exe = resolved; return; }
#endif
#ifdef __APPLE__
    {
        uint32_t size = 0;
        _NSGetExecutablePath(NULL, &size);
        if (size > 0 && size <= 32768) {
            char *raw = g_malloc(size);
            if (_NSGetExecutablePath(raw, &size) == 0) {
                char *real = realpath(raw, NULL);
                if (real) {
                    g_self_exe = g_strdup(real);
                    free(real);
                } else {
                    g_self_exe = g_strdup(raw);
                }
            }
            g_free(raw);
            if (g_self_exe) return;
        }
    }
#endif
#ifdef G_OS_WIN32
    {
        DWORD cap = MAX_PATH;
        wchar_t *buf = g_new(wchar_t, cap);
        DWORD n = GetModuleFileNameW(NULL, buf, cap);
        while (n >= cap && cap < 32768) {
            cap *= 2;
            buf = g_renew(wchar_t, buf, cap);
            n = GetModuleFileNameW(NULL, buf, cap);
        }
        if (n > 0 && n < cap)
            g_self_exe = g_utf16_to_utf8((gunichar2 *)buf, -1, NULL, NULL, NULL);
        g_free(buf);
        if (g_self_exe) return;
    }
#endif
    if (argv0) {
        if (g_path_is_absolute(argv0))
            g_self_exe = g_strdup(argv0);
        else
            g_self_exe = g_find_program_in_path(argv0);
        if (!g_self_exe) g_self_exe = g_strdup(argv0);
    }
}

static GLogWriterOutput
ns_log_writer(GLogLevelFlags log_level,
              const GLogField *fields, gsize n_fields,
              gpointer user_data)
{
    (void)user_data;
    const char *captured = NULL;
    const char *captured_domain = NULL;
    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].value) {
            const char *m = fields[i].value;
            captured = m;
            if (strstr(m, "win32 session dbus binary not found") ||
                strstr(m, "but sizes must be >= 0") ||
                strstr(m, "Baselines must be inside the widget size") ||
                strstr(m, "without a current allocation") ||
                strstr(m, "No IM module matching GTK_IM_MODULE="))
                return G_LOG_WRITER_HANDLED;
        }
        if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0 && fields[i].value)
            captured_domain = fields[i].value;
    }
    if (captured) {
        ns_dlog_level lvl = NS_DLOG_INFO;
        if (log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
            lvl = NS_DLOG_ERROR;
        else if (log_level & G_LOG_LEVEL_WARNING)
            lvl = NS_DLOG_WARN;
        ns_debug_log_emit(lvl, captured_domain ? captured_domain : "glib",
                          "%s", captured);
    }
    return g_log_writer_default(log_level, fields, n_fields, user_data);
}

#ifdef G_OS_WIN32

__declspec(dllimport) HRESULT __stdcall
SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID);

static void
ns_win32_set_app_id(void)
{
    (void)SetCurrentProcessExplicitAppUserModelID(L"Nordstjernen.Browser");
}

static void
ns_win32_use_fontconfig_backend(const char *dir)
{
#ifdef NS_HAVE_FONTCONFIG
    if (dir && !g_getenv("FONTCONFIG_FILE")) {
        char *conf = g_build_filename(dir, "etc", "fonts", "fonts.conf", NULL);
        if (g_file_test(conf, G_FILE_TEST_EXISTS))
            g_setenv("FONTCONFIG_FILE", conf, TRUE);
        g_free(conf);
    }
    if (dir && !g_getenv("FONTCONFIG_PATH")) {
        char *fonts = g_build_filename(dir, "etc", "fonts", NULL);
        if (g_file_test(fonts, G_FILE_TEST_IS_DIR))
            g_setenv("FONTCONFIG_PATH", fonts, TRUE);
        g_free(fonts);
    }
    if (!g_getenv("PANGOCAIRO_BACKEND"))
        g_setenv("PANGOCAIRO_BACKEND", "fc", TRUE);
    FcInit();
#endif
}

static void
ns_win32_anchor_gtk_data(void)
{
    if (!g_self_exe) {
        ns_win32_use_fontconfig_backend(NULL);
        return;
    }
    char *dir = g_path_get_dirname(g_self_exe);
    if (!dir) {
        ns_win32_use_fontconfig_backend(NULL);
        return;
    }
    ns_win32_use_fontconfig_backend(dir);
    char *share_dir = g_build_filename(dir, "share", NULL);
    if (g_file_test(share_dir, G_FILE_TEST_IS_DIR)) {
        if (!g_getenv("GTK_DATA_PREFIX")) g_setenv("GTK_DATA_PREFIX", dir, TRUE);
        if (!g_getenv("GTK_EXE_PREFIX"))  g_setenv("GTK_EXE_PREFIX",  dir, TRUE);
        if (!g_getenv("XDG_DATA_DIRS"))   g_setenv("XDG_DATA_DIRS", share_dir, TRUE);
    }
    g_free(share_dir);
    if (!g_getenv("GDK_PIXBUF_MODULE_FILE")) {
        char *loaders = g_build_filename(dir,
            "lib", "gdk-pixbuf-2.0", "2.10.0", "loaders.cache", NULL);
        if (g_file_test(loaders, G_FILE_TEST_EXISTS))
            g_setenv("GDK_PIXBUF_MODULE_FILE", loaders, TRUE);
        g_free(loaders);
    }
    {
        char *ca = g_build_filename(dir,
            "etc", "ssl", "certs", "ca-bundle.crt", NULL);
        if (g_file_test(ca, G_FILE_TEST_EXISTS)) {
            if (!g_getenv("CURL_CA_BUNDLE")) g_setenv("CURL_CA_BUNDLE", ca, TRUE);
            if (!g_getenv("SSL_CERT_FILE"))  g_setenv("SSL_CERT_FILE",  ca, TRUE);
        }
        g_free(ca);
    }
    g_free(dir);
}

static gboolean
ns_win32_args_need_console(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (g_strcmp0(argv[i], "--headless")     == 0 ||
            g_strcmp0(argv[i], "--print-config") == 0 ||
            g_strcmp0(argv[i], "--wpt")          == 0 ||
            g_str_has_prefix(argv[i], "--dump=") ||
            g_str_has_prefix(argv[i], "--url=")  ||
            g_str_has_prefix(argv[i], "--viewport=") ||
            g_str_has_prefix(argv[i], "--eval=") ||
            g_str_has_prefix(argv[i], "--inspect=") ||
            g_str_has_prefix(argv[i], "--inspect-at=") ||
            g_str_has_prefix(argv[i], "--wpt-timeout-ms=") ||
            g_str_has_prefix(argv[i], "--settle-ms="))
            return TRUE;
    }
    return FALSE;
}

static gboolean
ns_win32_fd_is_bound(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

static void
ns_win32_attach_parent_console(void)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE *fp;
    if (!ns_win32_fd_is_bound(_fileno(stdout)))
        (void)freopen_s(&fp, "CONOUT$", "w", stdout);
    if (!ns_win32_fd_is_bound(_fileno(stderr)))
        (void)freopen_s(&fp, "CONOUT$", "w", stderr);
    if (!ns_win32_fd_is_bound(_fileno(stdin)))
        (void)freopen_s(&fp, "CONIN$",  "r", stdin);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}
#endif

#ifdef __APPLE__
static void
ns_macos_anchor_gtk_data(void)
{
    if (!g_self_exe) return;
    char *macos_dir = g_path_get_dirname(g_self_exe);
    if (!macos_dir) return;
    char *contents = g_path_get_dirname(macos_dir);
    g_free(macos_dir);
    if (!contents) return;
    char *res = g_build_filename(contents, "Resources", NULL);
    g_free(contents);

    char *schemas = g_build_filename(res, "share", "glib-2.0", "schemas", NULL);
    if (g_file_test(schemas, G_FILE_TEST_IS_DIR) &&
        !g_getenv("GSETTINGS_SCHEMA_DIR"))
        g_setenv("GSETTINGS_SCHEMA_DIR", schemas, TRUE);
    g_free(schemas);

    char *loaders = g_build_filename(res, "lib", "gdk-pixbuf-2.0",
                                     "2.10.0", "loaders", NULL);
    char *cache = g_build_filename(res, "lib", "gdk-pixbuf-2.0",
                                   "2.10.0", "loaders.cache", NULL);
    if (g_file_test(cache, G_FILE_TEST_EXISTS)) {
        if (!g_getenv("GDK_PIXBUF_MODULE_FILE"))
            g_setenv("GDK_PIXBUF_MODULE_FILE", cache, TRUE);
        if (!g_getenv("GDK_PIXBUF_MODULEDIR"))
            g_setenv("GDK_PIXBUF_MODULEDIR", loaders, TRUE);
    }
    g_free(cache);
    g_free(loaders);

    char *ca = g_build_filename(res, "etc", "ssl", "certs",
                                "ca-bundle.crt", NULL);
    if (g_file_test(ca, G_FILE_TEST_EXISTS)) {
        if (!g_getenv("CURL_CA_BUNDLE")) g_setenv("CURL_CA_BUNDLE", ca, TRUE);
        if (!g_getenv("SSL_CERT_FILE"))  g_setenv("SSL_CERT_FILE",  ca, TRUE);
    }
    g_free(ca);

    g_free(res);
}
#endif

#ifdef __linux__
static gboolean
ns_linux_has_gpu_render_node(void)
{
    GDir *dir = g_dir_open("/dev/dri", 0, NULL);
    if (!dir) return FALSE;
    const char *name;
    gboolean found = FALSE;
    while ((name = g_dir_read_name(dir))) {
        if (g_str_has_prefix(name, "renderD") || g_str_has_prefix(name, "card")) {
            found = TRUE;
            break;
        }
    }
    g_dir_close(dir);
    return found;
}
#endif

static void
ns_apply_gsk_renderer(const char *pref)
{
    if (g_getenv("GSK_RENDERER")) return;
    if (!pref || !*pref ||
        g_ascii_strcasecmp(pref, "auto")    == 0 ||
        g_ascii_strcasecmp(pref, "default") == 0 ||
        g_ascii_strcasecmp(pref, "system")  == 0) {
#ifdef __linux__
        if (!ns_linux_has_gpu_render_node()) {
            g_setenv("GSK_RENDERER", "cairo", TRUE);
            g_message("no GPU detected; using the software (cairo) renderer");
        }
#endif
        return;
    }
    static const char *const known[] = {
        "gl", "ngl", "opengl", "vulkan", "cairo", "help",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(known); i++) {
        if (g_ascii_strcasecmp(pref, known[i]) == 0) {
            g_setenv("GSK_RENDERER", known[i], TRUE);
            return;
        }
    }
    g_warning("ignoring unknown gsk_renderer '%s' "
              "(expected one of: auto, gl, ngl, vulkan, cairo)", pref);
}

/* The GTK GUI runs the process-per-tab IPC renderer (a thin shell spawning
 * sandboxed nordstjernen-renderer processes). Headless / dump / eval / inspect
 * modes run the in-process engine without a display. */
static gboolean
ns_proc_mode_wanted(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (g_strcmp0(a, "--headless") == 0 ||
            g_strcmp0(a, "--print-config") == 0 ||
            g_strcmp0(a, "--wpt") == 0 ||
            g_str_has_prefix(a, "--dump=") ||
            g_str_has_prefix(a, "--eval=") ||
            g_str_has_prefix(a, "--inspect=") ||
            g_str_has_prefix(a, "--inspect-at=") ||
            g_str_has_prefix(a, "--act="))
            return FALSE;
    }
    return TRUE;
}

static gboolean
ns_single_process_wanted(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (g_strcmp0(argv[i], "--single-process") == 0)
            return TRUE;
    const char *env = g_getenv(NS_PROC_SINGLE_PROCESS_ENV);
    return env && *env && g_strcmp0(env, "0") != 0;
}

static void
ns_add_screenshot_writable_dirs(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *p = NULL;
        if (g_str_has_prefix(argv[i], "--dump=")) {
            const char *v = argv[i] + 7;
            if      (g_str_has_prefix(v, "png:")) p = v + 4;
            else if (g_str_has_prefix(v, "pdf:")) p = v + 4;
        } else if (g_str_has_prefix(argv[i], "--screenshot=")) {
            p = argv[i] + 13;
        }
        if (!p || !*p || *p == '-') continue;
        char *dir = g_path_get_dirname(p);
        if (dir && *dir) {
            g_mkdir_with_parents(dir, 0700);
            ns_security_add_writable_dir(dir);
        }
        g_free(dir);
    }
}

static int
ns_run_headless(ns_headless_opts *hopts)
{
    if (!g_getenv("NS_WEBGL_ALLOW"))
        g_setenv("NS_WEBGL_ALLOW", "1", TRUE);
    ns_net_init();
    ns_net_set_allow_file_urls(TRUE);
    if (hopts->debug_levels & (1u << NS_DLOG_NET))
        ns_net_set_log_fetches(TRUE);
    ns_cache_init();
    ns_bytecode_cache_init();
    ns_history_init();
    ns_font_init();
    ns_spell_init();
    char *file_url = NULL;
    if (hopts->url && !strstr(hopts->url, "://") &&
        !g_str_has_prefix(hopts->url, "about:") &&
        !g_str_has_prefix(hopts->url, "data:") &&
        g_file_test(hopts->url, G_FILE_TEST_EXISTS)) {
        char *abs_path = g_canonicalize_filename(hopts->url, NULL);
        file_url = g_filename_to_uri(abs_path, NULL, NULL);
        g_free(abs_path);
        if (file_url) hopts->url = file_url;
    }
    int rc = ns_headless_run(hopts);
    if (hopts->debug_levels & (1u << NS_DLOG_NET)) {
        guint64 fetches = 0, bytes = 0, relayouts = 0;
        double sum_ms = 0, span_ms = 0, layout_ms = 0;
        ns_net_perf_snapshot(&fetches, &bytes, &sum_ms, &span_ms);
        ns_engine_layout_perf(&relayouts, &layout_ms);
        g_printerr("[net perf] fetches=%" G_GUINT64_FORMAT " bytes=%"
                   G_GUINT64_FORMAT " net_span=%.1fms net_sum=%.1fms | "
                   "relayouts=%" G_GUINT64_FORMAT " layout=%.1fms\n",
                   fetches, bytes, span_ms, sum_ms, relayouts, layout_ms);
    }
    g_free(file_url);
    ns_bytecode_cache_shutdown();
    ns_history_shutdown();
    ns_cache_shutdown();
    ns_net_shutdown();
    ns_config_shutdown();
    return rc;
}

static int
ns_run_proc_gui(int argc, char **argv, const char *url,
                const char *gsk_renderer_override, const char *session_path,
                gboolean recover, gboolean private_mode)
{
    (void)argc; (void)argv;
    const ns_config *cfg = ns_config_get();
    ns_apply_gsk_renderer(gsk_renderer_override ? gsk_renderer_override
                          : (cfg ? cfg->gsk_renderer : NULL));
    const char *start = url;
    if (!start || !*start)
        start = (cfg && cfg->home_url && *cfg->home_url) ? cfg->home_url
                                                         : "about:start";
    char *file_url = NULL;
    if (!strstr(start, "://") && !g_str_has_prefix(start, "about:") &&
        !g_str_has_prefix(start, "data:") &&
        g_file_test(start, G_FILE_TEST_EXISTS)) {
        char *abs_path = g_canonicalize_filename(start, NULL);
        file_url = g_filename_to_uri(abs_path, NULL, NULL);
        g_free(abs_path);
        if (file_url) start = file_url;
    }
    int status = ns_procapp_run(start, session_path, recover, private_mode);
    g_free(file_url);
    g_free(g_self_exe);
    g_self_exe = NULL;
    ns_config_shutdown();
    return status;
}

int
main(int argc, char **argv)
{
#ifdef G_OS_WIN32
    if (ns_win32_args_need_console(argc, argv))
        ns_win32_attach_parent_console();
#endif
    if (!ns_security_refuse_root()) return 77;
    init_self_exe(argc > 0 ? argv[0] : NULL);
    ns_i18n_init(g_self_exe);
    ns_config_init();
    ns_thread_dump_install_signal("nordstjernen");

    gboolean proc_mode = ns_proc_mode_wanted(argc, argv);

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--enable-webgpu") == 0) {
#ifdef ND_HAVE_WEBGPU
            g_setenv("NS_WEBGPU_ALLOW", "1", TRUE);
#else
            g_printerr("nordstjernen: --enable-webgpu ignored "
                       "(built without WebGPU support)\n");
#endif
        }
    }

    /* The GUI shell is supervised: a normal launch becomes a tiny watchdog
     * that spawns the real (thin, engine-free) shell as a child and restarts
     * it on crash or hang. The supervisor inits no network, sandbox, or UI;
     * those belong to the child. Headless/tooling modes are never supervised. */
    if (proc_mode) {
        const ns_config *cfg = ns_config_get();
        if (ns_watchdog_should_supervise(argc, argv,
                                         cfg && cfg->watchdog_enabled)) {
            int rc = ns_watchdog_run_supervisor(g_self_exe, argc, argv);
            g_free(g_self_exe);
            g_self_exe = NULL;
            ns_config_shutdown();
            return rc;
        }
    }

    ns_security_win32_mitigations_init(proc_mode);
    ns_add_screenshot_writable_dirs(argc, argv);

    gboolean single_process = proc_mode &&
                              ns_single_process_wanted(argc, argv);
    if (single_process)
        ns_rproc_single_process_enable();

    if (proc_mode) {
        /* The proc-mode shell processes no untrusted bytes (it only blits
         * renderer framebuffers and forwards input), but it must fork/execv
         * renderer processes and create POSIX shm — which the no-execve seccomp
         * filter and the read-only /dev/shm Landlock rule would block. Apply
         * Landlock only (with /dev/shm writable and the renderer directory
         * executable) and skip seccomp; the real syscall confinement lives in
         * the sandboxed renderer processes. In single-process mode there is
         * no renderer to exec, so the exec-dir widening is skipped and the
         * engine runs in this process under the same Landlock. */
        ns_security_add_writable_dir("/dev/shm");
        if (!single_process) {
            char *rpath = ns_proc_renderer_path();
            if (rpath) {
                char *rdir = g_path_get_dirname(rpath);
                if (rdir) {
                    ns_security_add_exec_dir(rdir);
                    g_free(rdir);
                }
                g_free(rpath);
            }
        }
        ns_security_sandbox_init(g_self_exe);
    } else {
        ns_security_sandbox_init(g_self_exe);
        ns_security_seccomp_init();
    }
    ns_debug_log_init();
    g_log_set_writer_func(ns_log_writer, NULL, NULL);
#ifdef G_OS_WIN32
    ns_win32_set_app_id();
    ns_win32_anchor_gtk_data();
#endif
#ifdef __APPLE__
    ns_macos_anchor_gtk_data();
#endif

    const char *gsk_renderer_override = NULL;
    gboolean private_window = FALSE;
    ns_headless_opts hopts = {
        .url = NULL,
        .dump = NS_DUMP_TEXT,
        .out_path = NULL,
        .viewport_width = 1000,
        .settle_ms = 200,
        .time_ms = 1000,
    };
    gboolean dump_set = FALSE;
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--proxy=")) {
            const char *pxy = argv[i] + 8;
            ns_net_set_proxy_override(pxy);
            if (pxy && *pxy) {
                g_setenv("NS_HTTP_PROXY", pxy, TRUE);
                g_setenv("NS_HTTPS_PROXY", pxy, TRUE);
            }
        } else if (g_str_has_prefix(argv[i], "--gsk-renderer=")) {
            gsk_renderer_override = argv[i] + 15;
        } else if (g_strcmp0(argv[i], "--private") == 0) {
            private_window = TRUE;
        } else if (g_str_has_prefix(argv[i], "--window-size=")) {
            char *end = NULL;
            gint64 w = g_ascii_strtoll(argv[i] + 14, &end, 10);
            if (end != argv[i] + 14 && (*end == 'x' || *end == 'X') &&
                w > 0 && w < 100000) {
                const char *hs = end + 1;
                gint64 h = g_ascii_strtoll(hs, &end, 10);
                if (end != hs && *end == '\0' && h > 0 && h < 100000)
                    ns_procapp_set_window_size((int)w, (int)h);
            }
        } else if (g_strcmp0(argv[i], "--print-config") == 0) {
            char *dump = ns_config_dump();
            fputs(dump, stdout);
            g_free(dump);
            ns_config_shutdown();
            return 0;
        } else if (g_strcmp0(argv[i], "--headless") == 0) {
            /* headless dispatch is driven by proc_mode below */
        } else if (g_str_has_prefix(argv[i], "--dump=")) {
            const char *v = argv[i] + 7;
            dump_set = TRUE;
            if      (g_strcmp0(v, "text")   == 0) hopts.dump = NS_DUMP_TEXT;
            else if (g_strcmp0(v, "dom")    == 0) hopts.dump = NS_DUMP_DOM;
            else if (g_strcmp0(v, "layout") == 0) hopts.dump = NS_DUMP_LAYOUT;
            else if (g_strcmp0(v, "none")   == 0) hopts.dump = NS_DUMP_NONE;
            else if (g_str_has_prefix(v, "png:")) { hopts.dump = NS_DUMP_PNG; hopts.out_path = v + 4; }
            else if (g_str_has_prefix(v, "pdf:")) { hopts.dump = NS_DUMP_PDF; hopts.out_path = v + 4; }
        } else if (g_str_has_prefix(argv[i], "--viewport=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 11, &end, 10);
            if (end != argv[i] + 11 && *end == '\0' && n > 0 && n < 100000)
                hopts.viewport_width = (int)n;
        } else if (g_str_has_prefix(argv[i], "--viewport-height=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 18, &end, 10);
            if (end != argv[i] + 18 && *end == '\0' && n > 0 && n < 100000)
                hopts.viewport_height = (int)n;
        } else if (g_str_has_prefix(argv[i], "--settle-ms=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 12, &end, 10);
            if (end != argv[i] + 12 && *end == '\0' && n >= 0 && n < 600000)
                hopts.settle_ms = (int)n;
        } else if (g_str_has_prefix(argv[i], "--time-ms=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 10, &end, 10);
            if (end != argv[i] + 10 && *end == '\0' && n >= 0 && n < 600000)
                hopts.time_ms = (int)n;
        } else if (g_strcmp0(argv[i], "--wpt") == 0) {
            hopts.wpt = TRUE;
        } else if (g_str_has_prefix(argv[i], "--wpt-timeout-ms=")) {
            char *end = NULL;
            gint64 n = g_ascii_strtoll(argv[i] + 17, &end, 10);
            if (end != argv[i] + 17 && *end == '\0' && n > 0 && n < 600000)
                hopts.wpt_timeout_ms = (int)n;
        } else if (g_str_has_prefix(argv[i], "--act=")) {
            hopts.actions = argv[i] + 6;
        } else if (g_str_has_prefix(argv[i], "--eval=")) {
            hopts.eval = argv[i] + 7;
        } else if (g_str_has_prefix(argv[i], "--inspect=")) {
            hopts.inspect = argv[i] + 10;
        } else if (g_str_has_prefix(argv[i], "--inspect-at=")) {
            hopts.inspect_at = argv[i] + 13;
        } else if (g_strcmp0(argv[i], "--debug") == 0) {
            hopts.debug_levels = ns_headless_debug_mask("all");
        } else if (g_str_has_prefix(argv[i], "--debug=")) {
            hopts.debug_levels = ns_headless_debug_mask(argv[i] + 8);
        } else if (g_str_has_prefix(argv[i], "--url=")) {
            hopts.url = argv[i] + 6;
        } else if (argv[i][0] != '-' && !hopts.url) {
            hopts.url = argv[i];
        }
    }
    if (!dump_set && (hopts.inspect || hopts.inspect_at || hopts.wpt))
        hopts.dump = NS_DUMP_NONE;

    if (proc_mode) {
        if (ns_watchdog_is_child(argc, argv)) {
            ns_watchdog_child_guard_parent_death();
            /* The multiprocess shell runs no page JS, so any prolonged
             * unresponsiveness means a wedged UI loop (blit/IPC deadlock). In
             * single-process mode the shell does run page JS on its main
             * loop, so extend the hang budget by the configured JS eval
             * budget, exactly as an in-process engine requires. */
            const ns_config *wcfg = ns_config_get();
            ns_watchdog_child_arm_hang_monitor(
                single_process && wcfg ? wcfg->js_eval_budget_ms : 0);
        }
        const char *session = ns_watchdog_child_session_arg(argc, argv);
        gboolean recover = ns_watchdog_child_is_recovery();
        return ns_run_proc_gui(argc, argv, hopts.url, gsk_renderer_override,
                               session, recover, private_window);
    }

    ns_apply_gsk_renderer(gsk_renderer_override ? gsk_renderer_override
                          : (ns_config_get() ? ns_config_get()->gsk_renderer
                                             : NULL));
    int headless_rc = ns_run_headless(&hopts);
    if (!ns_net_idle()) {
        fflush(NULL);
        _exit(headless_rc);
    }
    return headless_rc;
}
