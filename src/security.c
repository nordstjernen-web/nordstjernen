/* Nordstjernen — refuse privileged startup + Linux Landlock + seccomp sandbox.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#define _GNU_SOURCE
#include "security.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#ifndef G_OS_WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/random.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#ifdef NS_HAVE_SECCOMP
#include <seccomp.h>
#endif
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if (defined(__APPLE__) && !TARGET_OS_IPHONE) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/random.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif

#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef G_OS_WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

gboolean
ns_security_csprng_fill(void *buf, gsize len)
{
    if (!buf || len == 0) return TRUE;
#if defined(G_OS_WIN32)
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return TRUE;
#elif defined(__linux__)
    gsize off = 0;
    while (off < len) {
        ssize_t n = getrandom((char *)buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (gsize)n;
    }
    if (off == len) return TRUE;
#elif (defined(__APPLE__) && !TARGET_OS_IPHONE) || defined(__FreeBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    gsize off = 0;
    while (off < len) {
        gsize take = len - off;
        if (take > 256) take = 256;
        if (getentropy((char *)buf + off, take) != 0) break;
        off += take;
    }
    if (off == len) return TRUE;
#endif
#ifndef G_OS_WIN32
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return FALSE;
    gsize got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        got += (gsize)n;
    }
    close(fd);
    return got == len;
#else
    return FALSE;
#endif
}

static gboolean
ns_sri_digest_equal_b64(GChecksumType type,
                        const char *want_b64, gsize want_len,
                        const void *body, gsize body_len)
{
    GChecksum *cs = g_checksum_new(type);
    g_checksum_update(cs, (const guchar *)body, (gssize)body_len);
    guint8 raw[64];
    gsize  raw_len = sizeof raw;
    g_checksum_get_digest(cs, raw, &raw_len);
    g_checksum_free(cs);
    g_autofree char *got = g_base64_encode(raw, raw_len);
    if (!got) return FALSE;
    gsize got_len = strlen(got);
    if (got_len != want_len) return FALSE;
    return memcmp(got, want_b64, want_len) == 0;
}

gboolean
ns_security_sri_check(const char *integrity_attr,
                      const void *body, gsize body_len)
{
    if (!integrity_attr || !*integrity_attr) return TRUE;
    if (!body || body_len == 0) return FALSE;

    int strongest = 0;
    g_auto(GStrv) tokens = g_strsplit_set(integrity_attr, " \t\r\n", -1);
    for (int i = 0; tokens[i]; i++) {
        const char *t = tokens[i];
        if (g_str_has_prefix(t, "sha256-") && strongest < 256) strongest = 256;
        else if (g_str_has_prefix(t, "sha384-") && strongest < 384) strongest = 384;
        else if (g_str_has_prefix(t, "sha512-") && strongest < 512) strongest = 512;
    }
    if (strongest == 0) return TRUE;

    GChecksumType ctype = G_CHECKSUM_SHA256;
    const char *prefix = "sha256-";
    if (strongest == 384) { ctype = G_CHECKSUM_SHA384; prefix = "sha384-"; }
    else if (strongest == 512) { ctype = G_CHECKSUM_SHA512; prefix = "sha512-"; }
    gsize prefix_len = strlen(prefix);

    for (int i = 0; tokens[i]; i++) {
        const char *t = tokens[i];
        if (!g_str_has_prefix(t, prefix)) continue;
        const char *b64 = t + prefix_len;
        const char *qmark = strchr(b64, '?');
        gsize b64_len = qmark ? (gsize)(qmark - b64) : strlen(b64);
        if (b64_len == 0) continue;
        if (ns_sri_digest_equal_b64(ctype, b64, b64_len, body, body_len))
            return TRUE;
    }
    return FALSE;
}

#ifdef G_OS_WIN32
static gboolean
ns_win_is_elevated(void)
{
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    PSID admins_sid = NULL;
    BOOL is_member = FALSE;
    if (!AllocateAndInitializeSid(&nt_auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admins_sid))
        return FALSE;
    if (!CheckTokenMembership(NULL, admins_sid, &is_member))
        is_member = FALSE;
    FreeSid(admins_sid);
    return is_member ? TRUE : FALSE;
}

static gboolean
ns_win_relaunch_deelevated(void)
{
    HWND shell = GetShellWindow();
    if (!shell) return FALSE;
    DWORD shell_pid = 0;
    GetWindowThreadProcessId(shell, &shell_pid);
    if (!shell_pid) return FALSE;

    HANDLE shell_proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, shell_pid);
    if (!shell_proc) return FALSE;

    HANDLE shell_tok = NULL;
    gboolean relaunched = FALSE;
    if (OpenProcessToken(shell_proc, TOKEN_DUPLICATE, &shell_tok)) {
        HANDLE user_tok = NULL;
        DWORD access = TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE |
                       TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID;
        if (DuplicateTokenEx(shell_tok, access, NULL, SecurityImpersonation,
                             TokenPrimary, &user_tok)) {
            TOKEN_ELEVATION elev;
            DWORD got = 0;
            gboolean shell_elevated =
                GetTokenInformation(user_tok, TokenElevation, &elev,
                                    sizeof elev, &got) && elev.TokenIsElevated;
            if (!shell_elevated) {
                wchar_t exe[4096];
                DWORD n = GetModuleFileNameW(NULL, exe, G_N_ELEMENTS(exe));
                LPWSTR app = (n > 0 && n < G_N_ELEMENTS(exe)) ? exe : NULL;
                LPWSTR src = GetCommandLineW();
                gsize bytes = ((gsize)lstrlenW(src) + 1) * sizeof(wchar_t);
                LPWSTR cmd = g_malloc(bytes);
                memcpy(cmd, src, bytes);
                SetEnvironmentVariableW(L"NS_DEELEVATED", L"1");
                STARTUPINFOW si;
                memset(&si, 0, sizeof si);
                si.cb = sizeof si;
                PROCESS_INFORMATION pi;
                memset(&pi, 0, sizeof pi);
                if (CreateProcessWithTokenW(user_tok, 0, app, cmd, 0,
                                            NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    relaunched = TRUE;
                }
                g_free(cmd);
            }
            CloseHandle(user_tok);
        }
        CloseHandle(shell_tok);
    }
    CloseHandle(shell_proc);
    return relaunched;
}
#endif

gboolean
ns_security_refuse_root(void)
{
#if defined(__linux__) || defined(__APPLE__)
    if (geteuid() != 0 && getuid() != 0) return TRUE;
    if (g_getenv("NS_ALLOW_ROOT")) {
        g_warning("nordstjernen: running as root because NS_ALLOW_ROOT is set");
        return TRUE;
    }
    fprintf(stderr,
        "nordstjernen: refusing to run as root.\n"
        "  Web browsers process untrusted content; running as root exposes\n"
        "  the whole system if the renderer is compromised.\n"
        "  Re-run as an unprivileged user, or set NS_ALLOW_ROOT=1 to override.\n");
    return FALSE;
#elif defined(G_OS_WIN32)
    if (!ns_win_is_elevated()) return TRUE;
    if (g_getenv("NS_ALLOW_ROOT")) {
        g_warning("nordstjernen: running as Administrator because "
                  "NS_ALLOW_ROOT is set");
        return TRUE;
    }
    if (!g_getenv("NS_DEELEVATED") && ns_win_relaunch_deelevated()) {
        g_warning("nordstjernen: dropped Administrator rights by relaunching "
                  "as the current desktop user");
        return FALSE;
    }

    const char *msg =
        "Nordstjernen is running as administrator.\n"
        "\n"
        "Nordstjernen tried to restart itself without administrator rights but "
        "could not. You do not need administrator rights to browse the web, and "
        "it is much safer without them: if a web page exploited a flaw in the "
        "browser while it has administrator rights, it could take over your "
        "whole PC instead of being limited to your own user account.\n"
        "\n"
        "How to start Nordstjernen normally:\n"
        "  -  Close this window, then open Nordstjernen with a normal click on "
        "its Start-menu or desktop icon. Do not choose \"Run as "
        "administrator\".\n"
        "  -  If you started it from a Command Prompt or PowerShell, use an "
        "ordinary window that is not marked \"Administrator\".\n"
        "  -  If a shortcut keeps starting it elevated, right-click the "
        "shortcut, choose Properties, then Advanced, and turn off \"Run as "
        "administrator\".\n"
        "\n"
        "Do you want to run as administrator anyway?\n"
        "  -  No (recommended): quit, then reopen Nordstjernen normally.\n"
        "  -  Yes: run as administrator this once, at your own risk.";
    fprintf(stderr, "nordstjernen: %s\n", msg);

    int choice = MessageBoxA(NULL, msg,
        "Nordstjernen - running as administrator",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
        MB_SETFOREGROUND | MB_TOPMOST);
    if (choice == IDYES) {
        g_warning("nordstjernen: running as Administrator at the user's "
                  "request");
        return TRUE;
    }
    return FALSE;
#else
    return TRUE;
#endif
}

static GPtrArray *ns_extra_writable_dirs = NULL;
static GPtrArray *ns_extra_exec_dirs = NULL;

void
ns_security_add_writable_dir(const char *dir)
{
    if (!dir || !*dir) return;
    if (!ns_extra_writable_dirs)
        ns_extra_writable_dirs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ns_extra_writable_dirs, g_strdup(dir));
}

void
ns_security_add_exec_dir(const char *dir)
{
    if (!dir || !*dir) return;
    if (!ns_extra_exec_dirs)
        ns_extra_exec_dirs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(ns_extra_exec_dirs, g_strdup(dir));
}

#ifdef __linux__

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif

static int
landlock_create_ruleset_(const struct landlock_ruleset_attr *attr,
                         size_t size, guint32 flags)
{
    return (int)syscall(SYS_landlock_create_ruleset, attr, size, flags);
}

static int
landlock_add_rule_(int ruleset_fd, enum landlock_rule_type type,
                   const void *rule_attr, guint32 flags)
{
    return (int)syscall(SYS_landlock_add_rule, ruleset_fd, type,
                        rule_attr, flags);
}

static int
landlock_restrict_self_(int ruleset_fd, guint32 flags)
{
    return (int)syscall(SYS_landlock_restrict_self, ruleset_fd, flags);
}

static void
add_path_rw(int rfd, guint64 allowed, const char *path)
{
    if (!path) return;
    /* Open directly (no prior stat) to avoid a TOCTOU race; O_PATH fails for a
     * nonexistent path, which is all the previous stat() checked for. */
    int pfd = open(path, O_PATH | O_CLOEXEC);
    if (pfd < 0) return;
    struct landlock_path_beneath_attr pb = {
        .allowed_access = allowed,
        .parent_fd      = pfd,
    };
    (void)landlock_add_rule_(rfd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(pfd);
}

static void
ns_sandbox_require_or_die(const char *what)
{
    const char *req = g_getenv("NS_REQUIRE_SANDBOX");
    if (!req || !*req || g_strcmp0(req, "0") == 0)
        return;
    g_printerr("nordstjernen: %s, but NS_REQUIRE_SANDBOX is set — refusing to "
               "run untrusted content unconfined.\n", what);
    _exit(70);
}

void
ns_security_sandbox_init(const char *self_exe)
{
    if (g_getenv("NS_NO_SANDBOX")) return;

    guint64 fs_read =
        LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR;
    guint64 fs_write =
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REFER;
    guint64 fs_exec = LANDLOCK_ACCESS_FS_EXECUTE;
    guint64 fs_rw   = fs_read | fs_write;
    guint64 fs_all  = fs_read | fs_write | fs_exec;

    struct landlock_ruleset_attr attr = { .handled_access_fs = fs_all };
    int rfd = landlock_create_ruleset_(&attr, sizeof(attr), 0);
    if (rfd < 0) {
        if (errno != ENOSYS && errno != EOPNOTSUPP)
            g_info("landlock: create_ruleset failed: %s", g_strerror(errno));
        ns_sandbox_require_or_die("Landlock filesystem sandbox is unavailable");
        return;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        g_info("landlock: PR_SET_NO_NEW_PRIVS failed: %s", g_strerror(errno));
    }

    static const char *const system_exec_dirs[] = {
        "/usr", "/usr/local", "/lib", "/lib64", NULL,
    };
    static const char *const system_read_dirs[] = {
        "/etc", "/var/lib/ca-certificates", "/var/cache/fontconfig",
        "/proc", "/sys", "/run",
        "/dev/shm", "/dev/dri",
        "/tmp/.X11-unix", "/tmp/.ICE-unix",
        NULL,
    };
    add_path_rw(rfd, LANDLOCK_ACCESS_FS_READ_FILE, "/dev/urandom");
    add_path_rw(rfd, LANDLOCK_ACCESS_FS_READ_FILE |
                     LANDLOCK_ACCESS_FS_WRITE_FILE, "/dev/null");
    for (gsize i = 0; system_exec_dirs[i]; i++)
        add_path_rw(rfd, fs_read | fs_exec, system_exec_dirs[i]);
    for (gsize i = 0; system_read_dirs[i]; i++)
        add_path_rw(rfd, fs_read, system_read_dirs[i]);

    guint64 fs_dev = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;
    for (int vi = 0; vi < 64; vi++) {
        char vdev[32];
        g_snprintf(vdev, sizeof vdev, "/dev/video%d", vi);
        if (g_file_test(vdev, G_FILE_TEST_EXISTS))
            add_path_rw(rfd, fs_dev, vdev);
    }
    if (g_file_test("/dev/snd", G_FILE_TEST_IS_DIR))
        add_path_rw(rfd, fs_read | LANDLOCK_ACCESS_FS_WRITE_FILE, "/dev/snd");

    const char *xauth = g_getenv("XAUTHORITY");
    if (xauth && *xauth) {
        char *xauth_dir = g_path_get_dirname(xauth);
        add_path_rw(rfd, fs_read, xauth_dir);
        g_free(xauth_dir);
    }

    const char *home = g_get_home_dir();

    add_path_rw(rfd, fs_read, g_get_user_config_dir());
    add_path_rw(rfd, fs_read, g_get_user_data_dir());
    add_path_rw(rfd, fs_read, g_get_user_cache_dir());
    add_path_rw(rfd, fs_rw,   g_get_user_runtime_dir());

    char *ns_cfg_root =
        g_build_filename(g_get_user_config_dir(), "nordstjernen", NULL);
    g_mkdir_with_parents(ns_cfg_root, 0700);
    add_path_rw(rfd, fs_rw, ns_cfg_root);
    g_free(ns_cfg_root);

    char *ns_data_root =
        g_build_filename(g_get_user_data_dir(), "nordstjernen", NULL);
    g_mkdir_with_parents(ns_data_root, 0700);
    add_path_rw(rfd, fs_rw, ns_data_root);
    g_free(ns_data_root);

    char *ns_cache_top =
        g_build_filename(g_get_user_cache_dir(), "nordstjernen", NULL);
    g_mkdir_with_parents(ns_cache_top, 0700);
    add_path_rw(rfd, fs_rw, ns_cache_top);
    g_free(ns_cache_top);

    char *ns_cache_root =
        g_build_filename(g_get_user_cache_dir(), "nordstjernen", "cache", NULL);
    g_mkdir_with_parents(ns_cache_root, 0700);
    add_path_rw(rfd, fs_rw, ns_cache_root);
    g_free(ns_cache_root);

    {
        const char *dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        char *dl_dir = NULL;
        if (dl && *dl)
            dl_dir = g_strdup(dl);
        else if (home)
            dl_dir = g_build_filename(home, "Downloads", NULL);
        if (dl_dir && home && g_str_has_prefix(dl_dir, home)) {
            g_mkdir_with_parents(dl_dir, 0700);
            if (g_file_test(dl_dir, G_FILE_TEST_IS_DIR))
                add_path_rw(rfd, fs_rw, dl_dir);
        }
        g_free(dl_dir);
    }

    static const char *const home_ro_subdirs[] = {
        ".fonts", ".fontconfig", ".icons", ".themes", NULL,
    };
    for (gsize i = 0; home_ro_subdirs[i]; i++) {
        g_autofree char *p = g_build_filename(home, home_ro_subdirs[i], NULL);
        add_path_rw(rfd, fs_read, p);
    }

    if (self_exe) {
        g_autofree char *exe_dir = g_path_get_dirname(self_exe);
        add_path_rw(rfd, fs_read | fs_exec, exe_dir);
        const char *const dev_data_rel[] = {
            "../data",
            "../../data",
            "../../../data",
            "../share/nordstjernen",
            NULL,
        };
        for (gsize i = 0; dev_data_rel[i]; i++) {
            g_autofree char *p = g_build_filename(exe_dir, dev_data_rel[i], NULL);
            if (g_file_test(p, G_FILE_TEST_IS_DIR))
                add_path_rw(rfd, fs_read, p);
        }
        const char *const prefix_lib_rel[] = {
            "../lib",
            "../lib64",
            NULL,
        };
        for (gsize i = 0; prefix_lib_rel[i]; i++) {
            g_autofree char *p = g_build_filename(exe_dir, prefix_lib_rel[i], NULL);
            if (g_file_test(p, G_FILE_TEST_IS_DIR))
                add_path_rw(rfd, fs_read | fs_exec, p);
        }
        const char *const dev_root_markers[] = {
            "../meson.build",
            "../../meson.build",
            "../../../meson.build",
            NULL,
        };
        for (gsize i = 0; dev_root_markers[i]; i++) {
            g_autofree char *marker =
                g_build_filename(exe_dir, dev_root_markers[i], NULL);
            if (g_file_test(marker, G_FILE_TEST_IS_REGULAR)) {
                g_autofree char *root = g_path_get_dirname(marker);
                add_path_rw(rfd, fs_read, root);
                break;
            }
        }
    }

    if (ns_extra_writable_dirs) {
        for (guint i = 0; i < ns_extra_writable_dirs->len; i++) {
            const char *p = g_ptr_array_index(ns_extra_writable_dirs, i);
            add_path_rw(rfd, fs_rw, p);
        }
    }

    if (ns_extra_exec_dirs) {
        for (guint i = 0; i < ns_extra_exec_dirs->len; i++) {
            const char *p = g_ptr_array_index(ns_extra_exec_dirs, i);
            add_path_rw(rfd, fs_read | fs_exec, p);
        }
    }

    if (landlock_restrict_self_(rfd, 0) != 0) {
        g_info("landlock: restrict_self failed: %s", g_strerror(errno));
        ns_sandbox_require_or_die("Landlock enforcement (restrict_self) failed");
    }
    close(rfd);
}

#ifdef NS_HAVE_SECCOMP
static const char *const ns_seccomp_allowed_names[] = {
    "accept",
    "accept4",
    "access",
    "arch_prctl",
    "bind",
    "brk",
    "capget",
    "chdir",
    "chmod",
    "clock_getres",
    "clock_getres_time64",
    "clock_gettime",
    "clock_gettime64",
    "clock_nanosleep",
    "clock_nanosleep_time64",
    "clone",
    "clone3",
    "close",
    "close_range",
    "connect",
    "copy_file_range",
    "creat",
    "dup",
    "dup2",
    "dup3",
    "epoll_create",
    "epoll_create1",
    "epoll_ctl",
    "epoll_pwait",
    "epoll_pwait2",
    "epoll_wait",
    "eventfd",
    "eventfd2",
    "exit",
    "exit_group",
    "faccessat",
    "faccessat2",
    "fadvise64",
    "fadvise64_64",
    "fallocate",
    "fchdir",
    "fchmod",
    "fchmodat",
    "fcntl",
    "fcntl64",
    "fdatasync",
    "flock",
    "fstat",
    "fstat64",
    "fstatat64",
    "fstatfs",
    "fstatfs64",
    "fsync",
    "ftruncate",
    "ftruncate64",
    "futex",
    "futex_time64",
    "futex_waitv",
    "futimesat",
    "getcpu",
    "getcwd",
    "getdents",
    "getdents64",
    "getegid",
    "getegid32",
    "geteuid",
    "geteuid32",
    "getgid",
    "getgid32",
    "getgroups",
    "getgroups32",
    "getitimer",
    "getpeername",
    "getpgid",
    "getpgrp",
    "getpid",
    "getppid",
    "getpriority",
    "getrandom",
    "getresgid",
    "getresgid32",
    "getresuid",
    "getresuid32",
    "getrlimit",
    "get_robust_list",
    "getrusage",
    "getsid",
    "getsockname",
    "getsockopt",
    "gettid",
    "gettimeofday",
    "getuid",
    "getuid32",
    "getxattr",
    "inotify_add_watch",
    "inotify_init",
    "inotify_init1",
    "inotify_rm_watch",
    "ioctl",
    "kill",
    "lgetxattr",
    "link",
    "linkat",
    "listen",
    "listxattr",
    "_llseek",
    "llistxattr",
    "lseek",
    "lstat",
    "lstat64",
    "madvise",
    "mbind",
    "membarrier",
    "memfd_create",
    "memfd_secret",
    "mincore",
    "mkdir",
    "mkdirat",
    "mlock",
    "mlock2",
    "mlockall",
    "mmap",
    "mmap2",
    "mprotect",
    "mremap",
    "msync",
    "munlock",
    "munlockall",
    "munmap",
    "nanosleep",
    "newfstatat",
    "open",
    "openat",
    "openat2",
    "pause",
    "pidfd_open",
    "pidfd_send_signal",
    "pipe",
    "pipe2",
    "pkey_alloc",
    "pkey_free",
    "pkey_mprotect",
    "poll",
    "ppoll",
    "ppoll_time64",
    "prctl",
    "pread64",
    "preadv",
    "preadv2",
    "prlimit64",
    "pselect6",
    "pselect6_time64",
    "pwrite64",
    "pwritev",
    "pwritev2",
    "read",
    "readahead",
    "readlink",
    "readlinkat",
    "readv",
    "recv",
    "recvfrom",
    "recvmmsg",
    "recvmmsg_time64",
    "recvmsg",
    "remap_file_pages",
    "rename",
    "renameat",
    "renameat2",
    "restart_syscall",
    "rmdir",
    "rseq",
    "rt_sigaction",
    "rt_sigpending",
    "rt_sigprocmask",
    "rt_sigqueueinfo",
    "rt_sigreturn",
    "rt_sigsuspend",
    "rt_sigtimedwait",
    "rt_sigtimedwait_time64",
    "rt_tgsigqueueinfo",
    "sched_getaffinity",
    "sched_getattr",
    "sched_getparam",
    "sched_get_priority_max",
    "sched_get_priority_min",
    "sched_getscheduler",
    "sched_rr_get_interval",
    "sched_setaffinity",
    "sched_setattr",
    "sched_setparam",
    "sched_setscheduler",
    "sched_yield",
    "select",
    "semctl",
    "semget",
    "semop",
    "semtimedop",
    "send",
    "sendfile",
    "sendfile64",
    "sendmmsg",
    "sendmsg",
    "sendto",
    "setitimer",
    "setpgid",
    "setpriority",
    "setrlimit",
    "set_robust_list",
    "setsid",
    "setsockopt",
    "set_tid_address",
    "shmat",
    "shmctl",
    "shmdt",
    "shmget",
    "shutdown",
    "sigaltstack",
    "signalfd",
    "signalfd4",
    "socket",
    "socketpair",
    "splice",
    "stat",
    "stat64",
    "statfs",
    "statfs64",
    "statx",
    "symlink",
    "symlinkat",
    "sync",
    "sync_file_range",
    "syncfs",
    "sysinfo",
    "tee",
    "tgkill",
    "time",
    "timer_create",
    "timer_delete",
    "timer_getoverrun",
    "timer_gettime",
    "timer_gettime64",
    "timer_settime",
    "timer_settime64",
    "timerfd_create",
    "timerfd_gettime",
    "timerfd_gettime64",
    "timerfd_settime",
    "timerfd_settime64",
    "times",
    "tkill",
    "truncate",
    "truncate64",
    "umask",
    "uname",
    "unlink",
    "unlinkat",
    "utime",
    "utimensat",
    "utimensat_time64",
    "utimes",
    "wait4",
    "waitid",
    "waitpid",
    "write",
    "writev",
};
#endif

void
ns_security_seccomp_init(void)
{
    if (g_getenv("NS_NO_SANDBOX")) return;
    if (g_getenv("NS_NO_SECCOMP")) return;

#ifdef NS_HAVE_SECCOMP
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        g_info("seccomp: PR_SET_NO_NEW_PRIVS failed: %s", g_strerror(errno));
        ns_sandbox_require_or_die("seccomp prerequisite (no_new_privs) failed");
        return;
    }

    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ERRNO(EPERM));
    if (!ctx) {
        g_info("seccomp: seccomp_init failed");
        ns_sandbox_require_or_die("seccomp syscall filter could not be created");
        return;
    }
    (void)seccomp_attr_set(ctx, SCMP_FLTATR_CTL_TSYNC, 1);

    for (size_t i = 0; i < G_N_ELEMENTS(ns_seccomp_allowed_names); i++) {
        int nr = seccomp_syscall_resolve_name(ns_seccomp_allowed_names[i]);
        if (nr == __NR_SCMP_ERROR) continue;
        (void)seccomp_rule_add(ctx, SCMP_ACT_ALLOW, nr, 0);
    }

    int rc = seccomp_load(ctx);
    if (rc != 0) {
        g_warning("seccomp: load failed, process is NOT syscall-sandboxed: %s",
                  g_strerror(-rc));
        ns_sandbox_require_or_die("seccomp syscall filter failed to load");
    }
    seccomp_release(ctx);
#endif
}

#elif defined(__APPLE__) && !TARGET_OS_IPHONE

#include <limits.h>
#include <sandbox.h>
#include <stdlib.h>

static void
sb_append_subpath(GString *prof, const char *dir)
{
    if (!dir || !*dir)
        return;
    char resolved[PATH_MAX];
    const char *use = realpath(dir, resolved) ? resolved : dir;
    g_string_append(prof, "  (subpath \"");
    for (const char *c = use; *c; c++) {
        if (*c == '"' || *c == '\\')
            g_string_append_c(prof, '\\');
        g_string_append_c(prof, *c);
    }
    g_string_append(prof, "\")\n");
}

void
ns_security_sandbox_init(const char *self_exe)
{
    (void)self_exe;
    if (g_getenv("NS_NO_SANDBOX"))
        return;

    GString *prof = g_string_new(
        "(version 1)\n"
        "(allow default)\n"
        "(deny file-write*)\n"
        "(allow file-write*\n"
        "  (subpath \"/private/var/folders\")\n"
        "  (subpath \"/private/tmp\")\n"
        "  (subpath \"/tmp\")\n"
        "  (subpath \"/dev\")\n");

    sb_append_subpath(prof, g_get_user_runtime_dir());

    char *cfg = g_build_filename(g_get_user_config_dir(), "nordstjernen", NULL);
    sb_append_subpath(prof, cfg);
    g_free(cfg);
    char *data = g_build_filename(g_get_user_data_dir(), "nordstjernen", NULL);
    sb_append_subpath(prof, data);
    g_free(data);
    char *cache = g_build_filename(g_get_user_cache_dir(), "nordstjernen", NULL);
    sb_append_subpath(prof, cache);
    g_free(cache);

    const char *dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    sb_append_subpath(prof, dl);

    if (ns_extra_writable_dirs) {
        for (guint i = 0; i < ns_extra_writable_dirs->len; i++)
            sb_append_subpath(prof,
                              g_ptr_array_index(ns_extra_writable_dirs, i));
    }

    g_string_append(prof, ")\n");

    char *err = NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    int rc = sandbox_init(prof->str, 0, &err);
#pragma clang diagnostic pop
    if (rc != 0) {
        g_warning("sandbox: macOS Seatbelt init failed, process is NOT "
                  "filesystem-confined: %s", err ? err : "unknown error");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (err)
            sandbox_free_error(err);
#pragma clang diagnostic pop
    } else {
        g_info("sandbox: macOS Seatbelt filesystem write-confinement active");
    }
    g_string_free(prof, TRUE);
}

void
ns_security_seccomp_init(void)
{
}

#else

void
ns_security_sandbox_init(const char *self_exe)
{
    (void)self_exe;
}

void
ns_security_seccomp_init(void)
{
}

#endif

#ifdef G_OS_WIN32

typedef BOOL (WINAPI *ns_smp_fn)(int policy, PVOID buffer, SIZE_T length);

static void
ns_win32_apply_mitigation(ns_smp_fn fn, int policy, DWORD flags)
{
    if (!fn) return;
    DWORD m = flags;
    (void)fn(policy, &m, sizeof m);
}

void
ns_security_win32_mitigations_init(gboolean allow_child_processes)
{
    if (g_getenv("NS_NO_WIN32_MITIGATIONS")) return;

    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (!k) return;
    ns_smp_fn fn = (ns_smp_fn)(void *)GetProcAddress(k, "SetProcessMitigationPolicy");
    if (!fn) return;

    ns_win32_apply_mitigation(fn, 0,  0x0Fu);
    ns_win32_apply_mitigation(fn, 3,  0x03u);
    ns_win32_apply_mitigation(fn, 6,  0x01u);
    ns_win32_apply_mitigation(fn, 7,  0x01u);
    ns_win32_apply_mitigation(fn, 10, 0x07u);
    if (!allow_child_processes)
        ns_win32_apply_mitigation(fn, 13, 0x01u);
}

#else

void
ns_security_win32_mitigations_init(gboolean allow_child_processes)
{
    (void)allow_child_processes;
}

#endif

void
ns_security_mark_download_origin(const char *path, const char *url)
{
    if (!path || !*path)
        return;
#if defined(__APPLE__)
    (void)url;
    char value[96];
    g_snprintf(value, sizeof value, "0001;%08x;Nordstjernen;",
               (unsigned)(g_get_real_time() / G_USEC_PER_SEC));
    setxattr(path, "com.apple.quarantine", value, strlen(value), 0, 0);
#elif defined(__linux__)
    if (url && *url)
        setxattr(path, "user.xdg.origin.url", url, strlen(url), 0);
#elif defined(G_OS_WIN32)
    char *stream = g_strconcat(path, ":Zone.Identifier", NULL);
    wchar_t *wstream = (wchar_t *)g_utf8_to_utf16(stream, -1, NULL, NULL, NULL);
    if (wstream) {
        HANDLE h = CreateFileW(wstream, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            GString *zone = g_string_new("[ZoneTransfer]\r\nZoneId=3\r\n");
            if (url && *url) {
                g_string_append_printf(zone, "HostUrl=%s\r\n", url);
                g_string_append_printf(zone, "ReferrerUrl=%s\r\n", url);
            }
            DWORD wrote = 0;
            WriteFile(h, zone->str, (DWORD)zone->len, &wrote, NULL);
            CloseHandle(h);
            g_string_free(zone, TRUE);
        }
        g_free(wstream);
    }
    g_free(stream);
#else
    (void)url;
#endif
}

void
ns_security_harden_allocator(void)
{
#ifdef __GLIBC__
    mallopt(M_PERTURB, 0xAA);
#endif
}
