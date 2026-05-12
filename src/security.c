/* Nordstjernen — refuse privileged startup + Linux Landlock filesystem sandbox. */

#define _GNU_SOURCE
#include "security.h"

#include <stdio.h>
#include <string.h>

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
    add_path_rw(rfd, fs_read, "/proc/self");
    add_path_rw(rfd, fs_read, "/sys/class/drm");
    add_path_rw(rfd, fs_read, "/sys/devices");
    add_path_rw(rfd, fs_read, "/dev/urandom");
    add_path_rw(rfd, fs_read, "/dev/null");
    add_path_rw(rfd, fs_read, "/dev/shm");
    add_path_rw(rfd, fs_read, "/dev/dri");

    const char *home = g_get_home_dir();
    add_path_rw(rfd, fs_read, home);

    add_path_rw(rfd, fs_all, g_get_user_config_dir());
    add_path_rw(rfd, fs_all, g_get_user_data_dir());
    add_path_rw(rfd, fs_all, g_get_user_cache_dir());
    add_path_rw(rfd, fs_all, g_get_user_runtime_dir());
    add_path_rw(rfd, fs_all, "/tmp");
    add_path_rw(rfd, fs_all, "/var/tmp");

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

#else

void
nd_security_sandbox_init(const char *self_exe)
{
    (void)self_exe;
}

#endif
