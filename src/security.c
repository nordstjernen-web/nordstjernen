/* Nordstjernen — refuse privileged startup + Linux Landlock + seccomp sandbox. */

#define _GNU_SOURCE
#include "security.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/prctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#ifdef ND_HAVE_SECCOMP
#include <seccomp.h>
#endif
#endif

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#ifdef G_OS_WIN32
static gboolean
nd_win_is_elevated(void)
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
#endif

gboolean
nd_security_refuse_root(void)
{
#if defined(__linux__) || defined(__APPLE__)
    if (geteuid() != 0 && getuid() != 0) return TRUE;
    if (g_getenv("ND_ALLOW_ROOT")) {
        g_warning("nordstjernen: running as root because ND_ALLOW_ROOT is set");
        return TRUE;
    }
    fprintf(stderr,
        "nordstjernen: refusing to run as root.\n"
        "  Web browsers process untrusted content; running as root exposes\n"
        "  the whole system if the renderer is compromised.\n"
        "  Re-run as an unprivileged user, or set ND_ALLOW_ROOT=1 to override.\n");
    return FALSE;
#elif defined(G_OS_WIN32)
    if (!nd_win_is_elevated()) return TRUE;
    if (g_getenv("ND_ALLOW_ROOT")) {
        g_warning("nordstjernen: running elevated because ND_ALLOW_ROOT is set");
        return TRUE;
    }
    const char *msg =
        "Nordstjernen refuses to run as Administrator.\n\n"
        "Web browsers process untrusted content; running with elevated\n"
        "privileges exposes the whole system if the renderer is compromised.\n\n"
        "Re-launch as a normal user, or set the ND_ALLOW_ROOT environment\n"
        "variable to 1 to override.";
    fprintf(stderr, "nordstjernen: %s\n", msg);
    MessageBoxA(NULL, msg, "Nordstjernen", MB_OK | MB_ICONERROR);
    return FALSE;
#else
    return TRUE;
#endif
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
    struct stat st;
    if (stat(path, &st) != 0) return;
    int pfd = open(path, O_PATH | O_CLOEXEC);
    if (pfd < 0) return;
    struct landlock_path_beneath_attr pb = {
        .allowed_access = allowed,
        .parent_fd      = pfd,
    };
    (void)landlock_add_rule_(rfd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(pfd);
}

void
nd_security_sandbox_init(const char *self_exe)
{
    if (g_getenv("ND_NO_SANDBOX")) return;

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
    guint64 fs_all = fs_read | fs_write | fs_exec;

    struct landlock_ruleset_attr attr = { .handled_access_fs = fs_all };
    int rfd = landlock_create_ruleset_(&attr, sizeof(attr), 0);
    if (rfd < 0) {
        if (errno != ENOSYS && errno != EOPNOTSUPP)
            g_info("landlock: create_ruleset failed: %s", g_strerror(errno));
        return;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        g_info("landlock: PR_SET_NO_NEW_PRIVS failed: %s", g_strerror(errno));
    }

    add_path_rw(rfd, fs_read | fs_exec, "/usr");
    add_path_rw(rfd, fs_read | fs_exec, "/usr/local");
    add_path_rw(rfd, fs_read | fs_exec, "/lib");
    add_path_rw(rfd, fs_read | fs_exec, "/lib64");
    add_path_rw(rfd, fs_read, "/etc");
    add_path_rw(rfd, fs_read, "/var/lib/ca-certificates");
    add_path_rw(rfd, fs_read, "/proc/self");
    add_path_rw(rfd, fs_read, "/sys/class/drm");
    add_path_rw(rfd, fs_read, "/sys/devices");
    add_path_rw(rfd, fs_read, "/dev/urandom");
    add_path_rw(rfd, fs_read, "/dev/null");
    add_path_rw(rfd, fs_read, "/dev/shm");
    add_path_rw(rfd, fs_read, "/dev/dri");

    add_path_rw(rfd, fs_read, "/tmp/.X11-unix");
    add_path_rw(rfd, fs_read, "/tmp/.ICE-unix");
    const char *xauth = g_getenv("XAUTHORITY");
    if (xauth && *xauth) {
        char *xauth_dir = g_path_get_dirname(xauth);
        add_path_rw(rfd, fs_read, xauth_dir);
        g_free(xauth_dir);
    }

    const char *home = g_get_home_dir();
    add_path_rw(rfd, fs_read, g_get_user_config_dir());
    add_path_rw(rfd, fs_read, g_get_user_data_dir());

    char *nd_config_dir = g_build_filename(g_get_user_config_dir(),
                                       ND_APP_DIR_NAME, NULL);
    char *nd_data   = g_build_filename(g_get_user_data_dir(),
                                       ND_APP_DIR_NAME, NULL);
    char *nd_cache  = g_build_filename(g_get_user_cache_dir(),
                                       ND_APP_DIR_NAME, NULL);
    g_mkdir_with_parents(nd_config_dir, 0700);
    g_mkdir_with_parents(nd_data,   0700);
    g_mkdir_with_parents(nd_cache,  0700);
    add_path_rw(rfd, fs_all, nd_config_dir);
    add_path_rw(rfd, fs_all, nd_data);
    add_path_rw(rfd, fs_all, nd_cache);
    add_path_rw(rfd, fs_read, g_get_user_runtime_dir());
    g_free(nd_config_dir);
    g_free(nd_data);
    g_free(nd_cache);

    char *font_legacy = g_build_filename(home, ".fonts", NULL);
    char *fontconfig  = g_build_filename(home, ".fontconfig", NULL);
    char *icons_dir   = g_build_filename(home, ".icons", NULL);
    char *themes_dir  = g_build_filename(home, ".themes", NULL);
    add_path_rw(rfd, fs_read, font_legacy);
    add_path_rw(rfd, fs_read, fontconfig);
    add_path_rw(rfd, fs_read, icons_dir);
    add_path_rw(rfd, fs_read, themes_dir);
    g_free(font_legacy);
    g_free(fontconfig);
    g_free(icons_dir);
    g_free(themes_dir);

    if (self_exe) {
        char *exe_dir = g_path_get_dirname(self_exe);
        add_path_rw(rfd, fs_read | fs_exec, exe_dir);
        g_free(exe_dir);
    }

    if (landlock_restrict_self_(rfd, 0) != 0) {
        g_info("landlock: restrict_self failed: %s", g_strerror(errno));
    }
    close(rfd);
}

#ifdef ND_HAVE_SECCOMP
static const char *const nd_seccomp_blocked_names[] = {
    "acct",
    "add_key",
    "bpf",
    "clock_adjtime",
    "clock_adjtime64",
    "clock_settime",
    "clock_settime64",
    "create_module",
    "delete_module",
    "fanotify_init",
    "finit_module",
    "get_kernel_syms",
    "init_module",
    "io_uring_enter",
    "io_uring_register",
    "io_uring_setup",
    "ioperm",
    "iopl",
    "kcmp",
    "kexec_file_load",
    "kexec_load",
    "keyctl",
    "lookup_dcookie",
    "mbind",
    "migrate_pages",
    "mount",
    "move_mount",
    "move_pages",
    "name_to_handle_at",
    "nfsservctl",
    "open_by_handle_at",
    "open_tree",
    "perf_event_open",
    "personality",
    "pivot_root",
    "process_vm_readv",
    "process_vm_writev",
    "ptrace",
    "query_module",
    "quotactl",
    "reboot",
    "request_key",
    "setdomainname",
    "sethostname",
    "setns",
    "settimeofday",
    "stime",
    "swapoff",
    "swapon",
    "sysfs",
    "_sysctl",
    "umount",
    "umount2",
    "unshare",
    "uselib",
    "userfaultfd",
    "ustat",
    "vhangup",
    "vmsplice",
};
#endif

void
nd_security_seccomp_init(void)
{
    if (g_getenv("ND_NO_SANDBOX")) return;
    if (g_getenv("ND_NO_SECCOMP")) return;

#ifdef ND_HAVE_SECCOMP
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        g_info("seccomp: PR_SET_NO_NEW_PRIVS failed: %s", g_strerror(errno));
        return;
    }

    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (!ctx) {
        g_info("seccomp: seccomp_init failed");
        return;
    }
    (void)seccomp_attr_set(ctx, SCMP_FLTATR_CTL_TSYNC, 1);

    for (size_t i = 0; i < G_N_ELEMENTS(nd_seccomp_blocked_names); i++) {
        int nr = seccomp_syscall_resolve_name(nd_seccomp_blocked_names[i]);
        if (nr == __NR_SCMP_ERROR) continue;
        (void)seccomp_rule_add(ctx, SCMP_ACT_ERRNO(EPERM), nr, 0);
    }

    int rc = seccomp_load(ctx);
    if (rc != 0) {
        g_info("seccomp: load failed: %s", g_strerror(-rc));
    }
    seccomp_release(ctx);
#endif
}

#else

void
nd_security_sandbox_init(const char *self_exe)
{
    (void)self_exe;
}

void
nd_security_seccomp_init(void)
{
}

#endif
