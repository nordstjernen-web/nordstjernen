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
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/prctl.h>
#include <linux/seccomp.h>
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

#if defined(__x86_64__)
#define ND_SECCOMP_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define ND_SECCOMP_ARCH AUDIT_ARCH_AARCH64
#elif defined(__i386__)
#define ND_SECCOMP_ARCH AUDIT_ARCH_I386
#elif defined(__arm__)
#define ND_SECCOMP_ARCH AUDIT_ARCH_ARM
#elif defined(__riscv) && __riscv_xlen == 64
#define ND_SECCOMP_ARCH AUDIT_ARCH_RISCV64
#elif defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2
#define ND_SECCOMP_ARCH AUDIT_ARCH_PPC64LE
#endif

#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif
#ifndef SECCOMP_FILTER_FLAG_TSYNC
#define SECCOMP_FILTER_FLAG_TSYNC 1U
#endif

static const int nd_seccomp_blocked[] = {
#ifdef __NR_acct
    __NR_acct,
#endif
#ifdef __NR_add_key
    __NR_add_key,
#endif
#ifdef __NR_bpf
    __NR_bpf,
#endif
#ifdef __NR_clock_adjtime
    __NR_clock_adjtime,
#endif
#ifdef __NR_clock_adjtime64
    __NR_clock_adjtime64,
#endif
#ifdef __NR_clock_settime
    __NR_clock_settime,
#endif
#ifdef __NR_clock_settime64
    __NR_clock_settime64,
#endif
#ifdef __NR_create_module
    __NR_create_module,
#endif
#ifdef __NR_delete_module
    __NR_delete_module,
#endif
#ifdef __NR_fanotify_init
    __NR_fanotify_init,
#endif
#ifdef __NR_finit_module
    __NR_finit_module,
#endif
#ifdef __NR_get_kernel_syms
    __NR_get_kernel_syms,
#endif
#ifdef __NR_init_module
    __NR_init_module,
#endif
#ifdef __NR_io_uring_setup
    __NR_io_uring_setup,
#endif
#ifdef __NR_io_uring_enter
    __NR_io_uring_enter,
#endif
#ifdef __NR_io_uring_register
    __NR_io_uring_register,
#endif
#ifdef __NR_ioperm
    __NR_ioperm,
#endif
#ifdef __NR_iopl
    __NR_iopl,
#endif
#ifdef __NR_kcmp
    __NR_kcmp,
#endif
#ifdef __NR_kexec_file_load
    __NR_kexec_file_load,
#endif
#ifdef __NR_kexec_load
    __NR_kexec_load,
#endif
#ifdef __NR_keyctl
    __NR_keyctl,
#endif
#ifdef __NR_lookup_dcookie
    __NR_lookup_dcookie,
#endif
#ifdef __NR_mbind
    __NR_mbind,
#endif
#ifdef __NR_migrate_pages
    __NR_migrate_pages,
#endif
#ifdef __NR_mount
    __NR_mount,
#endif
#ifdef __NR_move_mount
    __NR_move_mount,
#endif
#ifdef __NR_move_pages
    __NR_move_pages,
#endif
#ifdef __NR_name_to_handle_at
    __NR_name_to_handle_at,
#endif
#ifdef __NR_nfsservctl
    __NR_nfsservctl,
#endif
#ifdef __NR_open_by_handle_at
    __NR_open_by_handle_at,
#endif
#ifdef __NR_open_tree
    __NR_open_tree,
#endif
#ifdef __NR_perf_event_open
    __NR_perf_event_open,
#endif
#ifdef __NR_personality
    __NR_personality,
#endif
#ifdef __NR_pivot_root
    __NR_pivot_root,
#endif
#ifdef __NR_process_vm_readv
    __NR_process_vm_readv,
#endif
#ifdef __NR_process_vm_writev
    __NR_process_vm_writev,
#endif
#ifdef __NR_ptrace
    __NR_ptrace,
#endif
#ifdef __NR_query_module
    __NR_query_module,
#endif
#ifdef __NR_quotactl
    __NR_quotactl,
#endif
#ifdef __NR_reboot
    __NR_reboot,
#endif
#ifdef __NR_request_key
    __NR_request_key,
#endif
#ifdef __NR_setdomainname
    __NR_setdomainname,
#endif
#ifdef __NR_sethostname
    __NR_sethostname,
#endif
#ifdef __NR_setns
    __NR_setns,
#endif
#ifdef __NR_settimeofday
    __NR_settimeofday,
#endif
#ifdef __NR_stime
    __NR_stime,
#endif
#ifdef __NR_swapoff
    __NR_swapoff,
#endif
#ifdef __NR_swapon
    __NR_swapon,
#endif
#ifdef __NR_sysfs
    __NR_sysfs,
#endif
#ifdef __NR__sysctl
    __NR__sysctl,
#endif
#ifdef __NR_umount
    __NR_umount,
#endif
#ifdef __NR_umount2
    __NR_umount2,
#endif
#ifdef __NR_unshare
    __NR_unshare,
#endif
#ifdef __NR_uselib
    __NR_uselib,
#endif
#ifdef __NR_userfaultfd
    __NR_userfaultfd,
#endif
#ifdef __NR_ustat
    __NR_ustat,
#endif
#ifdef __NR_vhangup
    __NR_vhangup,
#endif
#ifdef __NR_vmsplice
    __NR_vmsplice,
#endif
};

void
nd_security_seccomp_init(void)
{
    if (g_getenv("ND_NO_SANDBOX")) return;
    if (g_getenv("ND_NO_SECCOMP")) return;

#ifndef ND_SECCOMP_ARCH
    return;
#else
    const size_t n = G_N_ELEMENTS(nd_seccomp_blocked);
    if (n == 0 || n > 200) return;

    const size_t total = 4 + n + 2;
    struct sock_filter *filter = g_new0(struct sock_filter, total);

    size_t k = 0;
    filter[k++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
        (guint32)offsetof(struct seccomp_data, arch));
    filter[k++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
        (guint32)ND_SECCOMP_ARCH, 1, 0);
    filter[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
        SECCOMP_RET_KILL_PROCESS);
    filter[k++] = (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
        (guint32)offsetof(struct seccomp_data, nr));

    for (size_t i = 0; i < n; i++) {
        guint8 jt = (guint8)(n - i);
        filter[k++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
            (guint32)nd_seccomp_blocked[i], jt, 0);
    }
    filter[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
        SECCOMP_RET_ALLOW);
    filter[k++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K,
        SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));

    struct sock_fprog prog = {
        .len    = (unsigned short)total,
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        g_info("seccomp: PR_SET_NO_NEW_PRIVS failed: %s", g_strerror(errno));
        g_free(filter);
        return;
    }

    long r = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                     SECCOMP_FILTER_FLAG_TSYNC, &prog);
    if (r != 0 && errno == ENOSYS) {
        r = prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
    }
    if (r != 0) {
        g_info("seccomp: install failed: %s", g_strerror(errno));
    }
    g_free(filter);
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
