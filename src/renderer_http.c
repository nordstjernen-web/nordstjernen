/* Nordstjernen — HTTP/JSON renderer server (IPC experiment, pixels in body). */

#define _GNU_SOURCE

#ifdef NS_HAVE_SDL
#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef main
#undef main
#endif
#endif

#include "ipc_http.h"
#include "libnordstjernen.h"
#include "net.h"
#include "renderer_serve.h"
#include "threaddump.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#if defined(NS_HAVE_FONTCONFIG)
#include <glib.h>
#include <fontconfig/fontconfig.h>
#endif
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

#if defined(_WIN32) && defined(NS_HAVE_FONTCONFIG)
/* Force pango's fontconfig backend and point fontconfig at the bundled
 * config, mirroring the GTK shell's startup. The GTK app sets these and the
 * renderer it spawns inherits them, but a renderer spawned by the Java
 * shell (or run standalone) would otherwise use the win32 pango backend and
 * render CJK / many scripts as tofu. Must run before any pango/font use. */
static void
renderer_win32_fontconfig(void)
{
    wchar_t wexe[4096];
    DWORD n = GetModuleFileNameW(NULL, wexe, 4096);
    char *dir = NULL;
    if (n > 0 && n < 4096) {
        char *exe = g_utf16_to_utf8((const gunichar2 *)wexe, -1, NULL, NULL, NULL);
        if (exe) {
            dir = g_path_get_dirname(exe);
            g_free(exe);
        }
    }
    if (dir) {
        if (!g_getenv("FONTCONFIG_FILE")) {
            char *conf = g_build_filename(dir, "etc", "fonts", "fonts.conf", NULL);
            if (g_file_test(conf, G_FILE_TEST_EXISTS))
                g_setenv("FONTCONFIG_FILE", conf, TRUE);
            g_free(conf);
        }
        if (!g_getenv("FONTCONFIG_PATH")) {
            char *fonts = g_build_filename(dir, "etc", "fonts", NULL);
            if (g_file_test(fonts, G_FILE_TEST_IS_DIR))
                g_setenv("FONTCONFIG_PATH", fonts, TRUE);
            g_free(fonts);
        }
        g_free(dir);
    }
    if (!g_getenv("PANGOCAIRO_BACKEND"))
        g_setenv("PANGOCAIRO_BACKEND", "fc", TRUE);
    FcInit();
}
#endif

#ifdef __APPLE__
static gpointer
renderer_parent_death_thread(gpointer unused)
{
    (void)unused;
    while (getppid() != 1)
        g_usleep(G_USEC_PER_SEC);
    _exit(0);
    return NULL;
}

static void
renderer_watch_parent_death(void)
{
    g_thread_unref(g_thread_new("nd-rparent", renderer_parent_death_thread,
                                NULL));
}
#endif

static int
renderer_selftest(const char *url)
{
    if (ns_browser_init() != 0) return 1;
    ns_browser *b = ns_browser_open_viewport(url, 1280, 720.0, 25000);
    if (!b) return 1;
    size_t stride = 1280 * 4;
    unsigned char *fb = malloc(stride * 720);
    if (!fb) { ns_browser_close(b); return 1; }
    for (int frame = 0; frame < 600; frame++) {
        ns_browser_tick(b, 8);
        int rr = ns_browser_render_argb32(b, 0, 0, 1280, 720, 1.0, fb,
                                          (int)stride);
        if (frame == 0)
            fprintf(stderr, "[selftest] first render rc=%d\n", rr);
        g_usleep(16000);
    }
    free(fb);
    ns_browser_close(b);
    return 0;
}

int
main(int argc, char **argv)
{
#ifdef NS_HAVE_SDL
    SDL_SetMainReady();
#endif
    const char *selftest_url = g_getenv("NS_RENDER_SELFTEST");
    if (selftest_url && *selftest_url)
        return renderer_selftest(selftest_url);
    if (argc < 3)
        return 2;

#if defined(_WIN32) && defined(NS_HAVE_FONTCONFIG)
    renderer_win32_fontconfig();
#endif
    int max_w = atoi(argv[1]);
    int max_h = atoi(argv[2]);
    if (max_w <= 0 || max_h <= 0 || max_w > 32768 || max_h > 32768)
        return 2;

#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1)
        return 0;
#elif defined(__APPLE__)
    if (getppid() == 1)
        return 0;
    renderer_watch_parent_death();
#endif

    ns_thread_dump_install_signal("nordstjernen-renderer");

    int shm_mode = argc > 3 && strcmp(argv[3], "shm") == 0;
    int stdio_mode = argc > 3 && strcmp(argv[3], "stdio") == 0;
    size_t fb_size = (size_t)max_w * (size_t)max_h * 4u;
    unsigned char *fb;
    int ctrl_r, ctrl_w;

    if (stdio_mode) {
        /* Control channel over stdin/stdout (fd 0/1), pixels in the body.
         * This is the transport used by JVM / Android clients, which can
         * give a child only its standard streams, not an inherited fd 3
         * or a named pipe. */
#ifdef _WIN32
        _setmode(0, _O_BINARY);
        _setmode(1, _O_BINARY);
#else
        signal(SIGPIPE, SIG_IGN);
#endif
        ctrl_r = 0;
        ctrl_w = 1;
        fb = malloc(fb_size);
        if (!fb)
            return 2;
    } else {
#ifdef _WIN32
    HANDLE proc = GetCurrentProcess();
    HANDLE ipc_in = NULL, ipc_out = NULL;
    if (!DuplicateHandle(proc, GetStdHandle(STD_INPUT_HANDLE), proc, &ipc_in, 0,
                         FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(proc, GetStdHandle(STD_OUTPUT_HANDLE), proc, &ipc_out,
                         0, FALSE, DUPLICATE_SAME_ACCESS))
        return 2;
    HANDLE nul_in = CreateFileA("NUL", GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
    HANDLE nul_out = CreateFileA("NUL", GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, 0, NULL);
    if (nul_in != INVALID_HANDLE_VALUE)
        SetStdHandle(STD_INPUT_HANDLE, nul_in);
    if (nul_out != INVALID_HANDLE_VALUE)
        SetStdHandle(STD_OUTPUT_HANDLE, nul_out);
    FILE *redir_in = freopen("NUL", "r", stdin);
    FILE *redir_out = freopen("NUL", "w", stdout);
    (void)redir_in;
    (void)redir_out;
    ctrl_r = _open_osfhandle((intptr_t)ipc_in, _O_BINARY);
    ctrl_w = _open_osfhandle((intptr_t)ipc_out, _O_BINARY);
    if (ctrl_r < 0 || ctrl_w < 0)
        return 2;
    HANDLE shm_handle = NULL;
    if (shm_mode) {
        if (argc < 5)
            return 2;
        shm_handle = (HANDLE)(uintptr_t)_strtoui64(argv[4], NULL, 10);
        fb = MapViewOfFile(shm_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                           fb_size);
        if (!fb)
            return 2;
    } else {
        fb = malloc(fb_size);
        if (!fb)
            return 2;
    }
#else
    signal(SIGPIPE, SIG_IGN);
    ctrl_r = 3;
    ctrl_w = 3;
    if (shm_mode) {
        int fd = http_recv_fd(ctrl_r);
        if (fd < 0)
            return 2;
        fb = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (fb == MAP_FAILED)
            return 2;
    } else {
        fb = malloc(fb_size);
        if (!fb)
            return 2;
    }
#endif
    }

    for (int i = 3; i < argc; i++)
        if (strcmp(argv[i], "private") == 0) {
            g_setenv("NS_PRIVATE", "1", TRUE);
            break;
        }

    if (ns_browser_init() != 0) {
        if (!shm_mode)
            free(fb);
#ifdef _WIN32
        else
            UnmapViewOfFile(fb);
#else
        else
            munmap(fb, fb_size);
#endif
        return 2;
    }
    ns_browser_sandbox(argv[0]);

    size_t bufsz = fb_size + 65536;
    if (bufsz > 0x7fffffff) bufsz = 0x7fffffff;
    http_set_bufsize(ctrl_r, (int)bufsz);
    http_set_bufsize(ctrl_w, (int)bufsz);

    ns_renderer_session *session =
        ns_renderer_session_new(ctrl_w, fb, max_w, max_h, shm_mode);
    if (!session)
        return 2;

    http_conn c;
    http_conn_init(&c, ctrl_r);

    for (;;) {
        http_head head;
        if (http_read_head(&c, &head) != 0)
            break;

        char *body = NULL;
        if (head.content_length > 0) {
            body = malloc((size_t)head.content_length + 1);
            if (!body || http_read_body(&c, head.content_length, body) != 0) {
                free(body);
                break;
            }
            body[head.content_length] = '\0';
        }

        int quit = ns_renderer_session_handle(session, &head, body);
        free(body);
        if (quit)
            break;
    }

    ns_renderer_session_free(session);
    ns_browser_shutdown();
    if (!ns_net_idle()) {
        fflush(NULL);
        _exit(0);
    }
    if (!shm_mode)
        free(fb);
#ifdef _WIN32
    else
        UnmapViewOfFile(fb);
#else
    else
        munmap(fb, fb_size);
#endif
    return 0;
}
