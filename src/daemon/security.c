// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/security.h"

#include "waywal/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef HAVE_LIBSECCOMP
#include <seccomp.h>
#endif

#ifndef landlock_create_ruleset
static inline int landlock_create_ruleset(const struct landlock_ruleset_attr *const attr,
                                          const size_t size, const __u32 flags)
{
    return (int)syscall(SYS_landlock_create_ruleset, attr, size, flags);
}
#endif

#ifndef landlock_add_rule
static inline int landlock_add_rule(const int ruleset_fd, const enum landlock_rule_type rule_type,
                                    const void *const rule_attr, const __u32 flags)
{
    return (int)syscall(SYS_landlock_add_rule, ruleset_fd, rule_type, rule_attr, flags);
}
#endif

#ifndef landlock_restrict_self
static inline int landlock_restrict_self(const int ruleset_fd, const __u32 flags)
{
    return (int)syscall(SYS_landlock_restrict_self, ruleset_fd, flags);
}
#endif

static void mkdir_p_user_dir(const char *path)
{
    if (!path || !path[0])
        return;
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, 0700);
            *p = '/';
        }
    }
    (void)mkdir(tmp, 0700);
}

static void add_landlock_dir_rule(int ruleset_fd, const char *path, __u64 allowed_access)
{
    if (ruleset_fd < 0 || !path || !path[0])
        return;
    int dir_fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        struct landlock_path_beneath_attr path_attr = {
            .allowed_access = allowed_access,
            .parent_fd = dir_fd,
        };
        landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
        close(dir_fd);
    } else {
        int file_fd = open(path, O_PATH | O_CLOEXEC);
        if (file_fd >= 0) {
            struct landlock_path_beneath_attr path_attr = {
                .allowed_access =
                    allowed_access & (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                                      LANDLOCK_ACCESS_FS_EXECUTE),
                .parent_fd = file_fd,
            };
            if (path_attr.allowed_access != 0) {
                landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
            }
            close(file_fd);
        }
    }
}

static bool is_sensitive_home_entry(const char *name)
{
    if (!name || name[0] == '\0')
        return true;
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, ".ssh") == 0 ||
            strcmp(name, ".gnupg") == 0 || strcmp(name, ".aws") == 0 || strcmp(name, ".pki") == 0 ||
            strcmp(name, ".password-store") == 0);
}

static void apply_landlock_sandbox(const char *runtime_dir)
{
    int abi = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi <= 0) {
        WAYWAL_LOG_DEBUG("Landlock LSM not available on current kernel");
        return;
    }

    const char *home = getenv("HOME");
    if (home && home[0]) {
        char pre_path[4096];
        const char *pre_dirs[] = {
            ".cache",       ".cache/wwal",       ".local",
            ".local/state", ".local/state/wwal", ".local/state/caelestia",
            ".config",      ".config/wwal",
        };
        for (size_t i = 0; i < sizeof(pre_dirs) / sizeof(pre_dirs[0]); ++i) {
            snprintf(pre_path, sizeof(pre_path), "%s/%s", home, pre_dirs[i]);
            mkdir_p_user_dir(pre_path);
        }
    }

    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE |
                             LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_WRITE_FILE |
                             LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REMOVE_FILE;

    int ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (ruleset_fd < 0) {
        return;
    }

    const __u64 ro_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    const __u64 rx_access = ro_access | LANDLOCK_ACCESS_FS_EXECUTE;
    const __u64 rw_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                            LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_REG |
                            LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_EXECUTE;

    /* Allow read+execute on standard system binary/library mounts for DRM/Mesa/VA-API and user
     * hooks */
    const char *rx_paths[] = {"/usr", "/bin", "/sbin", "/lib", "/lib64", NULL};
    for (int i = 0; rx_paths[i] != NULL; ++i) {
        add_landlock_dir_rule(ruleset_fd, rx_paths[i], rx_access);
    }

    /* Allow read-only across standard system mounts required for DRM/Mesa/VA-API drivers */
    const char *ro_paths[] = {"/etc", "/sys", "/proc", "/dev", "/dev/dri", "/tmp", NULL};
    for (int i = 0; ro_paths[i] != NULL; ++i) {
        add_landlock_dir_rule(ruleset_fd, ro_paths[i], ro_access);
    }

    /* Allow read-write strictly for DRI/GPU device nodes (/dev, /dev/dri) and /tmp */
    const char *rw_paths[] = {"/dev", "/dev/dri", "/tmp", NULL};
    for (int i = 0; rw_paths[i] != NULL; ++i) {
        int dir_fd = open(rw_paths[i], O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd < 0 && strcmp(rw_paths[i], "/dev/dri") == 0) {
            dir_fd = open("/dev", O_PATH | O_DIRECTORY | O_CLOEXEC);
        }
        if (dir_fd >= 0) {
            struct landlock_path_beneath_attr path_attr = {
                .allowed_access = rw_access,
                .parent_fd = dir_fd,
            };
            landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
            close(dir_fd);
        }
    }

    /* Allow read-write in runtime directory for IPC socket management */
    const char *rt = runtime_dir ? runtime_dir : getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) {
        add_landlock_dir_rule(ruleset_fd, rt, rw_access);
    }

    const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
    if (xdg_cfg && xdg_cfg[0]) {
        mkdir_p_user_dir(xdg_cfg);
        add_landlock_dir_rule(ruleset_fd, xdg_cfg, rw_access);
    }

    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0]) {
        add_landlock_dir_rule(ruleset_fd, xdg_data, ro_access);
    }

    const char *xdg_pics = getenv("XDG_PICTURES_DIR");
    if (xdg_pics && xdg_pics[0]) {
        add_landlock_dir_rule(ruleset_fd, xdg_pics, ro_access);
    }
    const char *xdg_vids = getenv("XDG_VIDEOS_DIR");
    if (xdg_vids && xdg_vids[0]) {
        add_landlock_dir_rule(ruleset_fd, xdg_vids, ro_access);
    }

    const char *wwal_cache = getenv("WWAL_CACHE_DIR");
    if (wwal_cache && wwal_cache[0]) {
        mkdir_p_user_dir(wwal_cache);
        add_landlock_dir_rule(ruleset_fd, wwal_cache, rw_access);
    }

    const char *wwal_state = getenv("WWAL_STATE_DIR");
    if (wwal_state && wwal_state[0]) {
        mkdir_p_user_dir(wwal_state);
        add_landlock_dir_rule(ruleset_fd, wwal_state, rw_access);
    }

    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    if (xdg_cache && xdg_cache[0]) {
        mkdir_p_user_dir(xdg_cache);
        add_landlock_dir_rule(ruleset_fd, xdg_cache, rw_access);
        char cache_wwal[4096];
        snprintf(cache_wwal, sizeof(cache_wwal), "%s/wwal", xdg_cache);
        mkdir_p_user_dir(cache_wwal);
        add_landlock_dir_rule(ruleset_fd, cache_wwal, rw_access);
    }

    const char *xdg_state = getenv("XDG_STATE_HOME");
    if (xdg_state && xdg_state[0]) {
        mkdir_p_user_dir(xdg_state);
        add_landlock_dir_rule(ruleset_fd, xdg_state, rw_access);
        char state_wwal[4096];
        snprintf(state_wwal, sizeof(state_wwal), "%s/wwal", xdg_state);
        mkdir_p_user_dir(state_wwal);
        add_landlock_dir_rule(ruleset_fd, state_wwal, rw_access);

        char state_caelestia[4096];
        snprintf(state_caelestia, sizeof(state_caelestia), "%s/caelestia", xdg_state);
        mkdir_p_user_dir(state_caelestia);
        add_landlock_dir_rule(ruleset_fd, state_caelestia, rw_access);
    }

    char cwd_buf[4096];
    if (getcwd(cwd_buf, sizeof(cwd_buf)) != NULL) {
        if (!home || strcmp(cwd_buf, home) != 0) {
            add_landlock_dir_rule(ruleset_fd, cwd_buf, ro_access);
        }
    }

    if (home && home[0]) {
        char path_buf[4096];

        /* Grant read-only access across $HOME entries (excluding sensitive keystores like ~/.ssh)
         */
        DIR *hdir = opendir(home);
        if (hdir) {
            struct dirent *ent;
            while ((ent = readdir(hdir)) != NULL) {
                if (is_sensitive_home_entry(ent->d_name))
                    continue;
                int n = snprintf(path_buf, sizeof(path_buf), "%s/%s", home, ent->d_name);
                if (n > 0 && (size_t)n < sizeof(path_buf)) {
                    add_landlock_dir_rule(ruleset_fd, path_buf, ro_access);
                }
            }
            closedir(hdir);
        }

        const char *home_ro_subdirs[] = {
            "Pictures",
            "Videos",
            "Wallpapers",
            "Downloads",
            ".config",
            ".local",
            ".local/share/wallpapers",
            ".local/share/Steam",
        };
        for (size_t i = 0; i < sizeof(home_ro_subdirs) / sizeof(home_ro_subdirs[0]); ++i) {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", home, home_ro_subdirs[i]);
            add_landlock_dir_rule(ruleset_fd, path_buf, ro_access);
        }

        const char *home_rw_subdirs[] = {
            ".cache",       ".cache/wwal",       ".local",
            ".local/state", ".local/state/wwal", ".local/state/caelestia",
            ".config",      ".config/wwal",
        };
        for (size_t i = 0; i < sizeof(home_rw_subdirs) / sizeof(home_rw_subdirs[0]); ++i) {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", home, home_rw_subdirs[i]);
            add_landlock_dir_rule(ruleset_fd, path_buf, rw_access);
        }
    }

    if (landlock_restrict_self(ruleset_fd, 0) == 0) {
        WAYWAL_LOG_INFO("Security: Landlock LSM filesystem sandbox engaged (ABI v%d)", abi);
    }
    close(ruleset_fd);
}

bool security_sandbox_apply(const char *runtime_dir)
{
    /* 1. Prevent privilege escalation */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        WAYWAL_LOG_WARN("PR_SET_NO_NEW_PRIVS failed: %s", strerror(errno));
    } else {
        WAYWAL_LOG_INFO("Security: PR_SET_NO_NEW_PRIVS enforced");
    }

    /* 2. Landlock LSM filesystem sandboxing */
    apply_landlock_sandbox(runtime_dir);

#ifdef HAVE_LIBSECCOMP
    /* 3. Seccomp-BPF syscall sandbox */
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ERRNO(EPERM));
    if (ctx) {
        /* Immediately terminate process on high-risk kernel/namespace syscalls */
        const int denied_syscalls[] = {
            SCMP_SYS(execveat),    SCMP_SYS(ptrace),        SCMP_SYS(mount),  SCMP_SYS(umount2),
            SCMP_SYS(chroot),      SCMP_SYS(pivot_root),    SCMP_SYS(reboot), SCMP_SYS(kexec_load),
            SCMP_SYS(init_module), SCMP_SYS(delete_module), SCMP_SYS(bpf),
        };

        for (size_t i = 0; i < sizeof(denied_syscalls) / sizeof(denied_syscalls[0]); ++i) {
            if (denied_syscalls[i] != __NR_SCMP_ERROR) {
                seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, denied_syscalls[i], 0);
            }
        }

        /* Explicitly allowed syscalls for Wayland, DRM/GBM, PipeWire, and user hooks */
        const int allowed_syscalls[] = {
            /* Process spawning for user-configured wallpaper/theme hooks */
            SCMP_SYS(clone),
            SCMP_SYS(clone3),
            SCMP_SYS(fork),
            SCMP_SYS(vfork),
            SCMP_SYS(execve),
            SCMP_SYS(kill),
            SCMP_SYS(arch_prctl),
            SCMP_SYS(set_tid_address),
            SCMP_SYS(getppid),
            SCMP_SYS(getpgrp),
            SCMP_SYS(getpgid),
            SCMP_SYS(setpgid),
            SCMP_SYS(setsid),
            /* Core I/O, memory, and event loop allowlist */
            SCMP_SYS(read),
            SCMP_SYS(write),
            SCMP_SYS(recvmsg),
            SCMP_SYS(sendmsg),
            SCMP_SYS(mmap),
            SCMP_SYS(munmap),
            SCMP_SYS(ioctl),
            SCMP_SYS(epoll_wait),
            SCMP_SYS(io_uring_enter),
            SCMP_SYS(timerfd_settime),
            SCMP_SYS(clock_gettime),
            SCMP_SYS(close),
            SCMP_SYS(accept4),
            SCMP_SYS(poll),
            SCMP_SYS(sigreturn),
            SCMP_SYS(rt_sigreturn),
            SCMP_SYS(exit_group),
            SCMP_SYS(exit),

            /* Extended async/polling support */
            SCMP_SYS(ppoll),
            SCMP_SYS(select),
            SCMP_SYS(pselect6),
            SCMP_SYS(epoll_create1),
            SCMP_SYS(epoll_create),
            SCMP_SYS(epoll_ctl),
            SCMP_SYS(epoll_pwait),
            SCMP_SYS(io_uring_setup),
            SCMP_SYS(io_uring_register),
            SCMP_SYS(timerfd_create),
            SCMP_SYS(timerfd_gettime),
            SCMP_SYS(eventfd),
            SCMP_SYS(eventfd2),
            SCMP_SYS(signalfd),
            SCMP_SYS(signalfd4),
#ifdef __NR_inotify_init
            SCMP_SYS(inotify_init),
#endif
            SCMP_SYS(inotify_init1),
            SCMP_SYS(inotify_add_watch),
            SCMP_SYS(inotify_rm_watch),

            /* File I/O & Dynamic Driver Loading */
            SCMP_SYS(readv),
            SCMP_SYS(writev),
            SCMP_SYS(pread64),
            SCMP_SYS(pwrite64),
            SCMP_SYS(lseek),
            SCMP_SYS(fcntl),
            SCMP_SYS(dup),
            SCMP_SYS(dup2),
            SCMP_SYS(dup3),
            SCMP_SYS(pipe),
            SCMP_SYS(pipe2),
            SCMP_SYS(ftruncate),
            SCMP_SYS(fdatasync),
            SCMP_SYS(fsync),
            SCMP_SYS(rename),
            SCMP_SYS(renameat),
            SCMP_SYS(renameat2),
            SCMP_SYS(mkdir),
            SCMP_SYS(mkdirat),
            SCMP_SYS(unlink),
            SCMP_SYS(unlinkat),
            SCMP_SYS(open),
            SCMP_SYS(openat),
            SCMP_SYS(access),
            SCMP_SYS(faccessat),
            SCMP_SYS(faccessat2),
            SCMP_SYS(getdents),
            SCMP_SYS(getdents64),
            SCMP_SYS(readlink),
            SCMP_SYS(readlinkat),
            SCMP_SYS(getcwd),
            SCMP_SYS(chdir),
            SCMP_SYS(fchdir),
            SCMP_SYS(umask),

            /* Sockets & IPC */
            SCMP_SYS(socket),
            SCMP_SYS(socketpair),
            SCMP_SYS(connect),
            SCMP_SYS(bind),
            SCMP_SYS(listen),
            SCMP_SYS(accept),
            SCMP_SYS(getsockopt),
            SCMP_SYS(setsockopt),
            SCMP_SYS(getsockname),
            SCMP_SYS(getpeername),
            SCMP_SYS(sendto),
            SCMP_SYS(recvfrom),
            SCMP_SYS(shutdown),

            /* Memory */
            SCMP_SYS(mprotect),
            SCMP_SYS(mremap),
            SCMP_SYS(brk),
            SCMP_SYS(madvise),
            SCMP_SYS(memfd_create),
            SCMP_SYS(fallocate),
            SCMP_SYS(flock),

            /* Clocks */
            SCMP_SYS(clock_getres),
            SCMP_SYS(clock_nanosleep),
            SCMP_SYS(nanosleep),
            SCMP_SYS(gettimeofday),

            /* Process & Signals */
            SCMP_SYS(prctl),
            SCMP_SYS(rt_sigaction),
            SCMP_SYS(rt_sigprocmask),
            SCMP_SYS(sigaltstack),
            SCMP_SYS(tgkill),
            SCMP_SYS(tkill),
            SCMP_SYS(futex),
            SCMP_SYS(set_robust_list),
            SCMP_SYS(get_robust_list),
            SCMP_SYS(getpid),
            SCMP_SYS(gettid),
            SCMP_SYS(getuid),
            SCMP_SYS(geteuid),
            SCMP_SYS(getgid),
            SCMP_SYS(getegid),
            SCMP_SYS(getpriority),
            SCMP_SYS(setpriority),
            SCMP_SYS(sched_yield),
            SCMP_SYS(sched_getaffinity),
            SCMP_SYS(sched_setaffinity),
            SCMP_SYS(sched_getparam),
            SCMP_SYS(sched_setparam),
            SCMP_SYS(sched_getscheduler),
            SCMP_SYS(sched_setscheduler),
            SCMP_SYS(sched_get_priority_max),
            SCMP_SYS(sched_get_priority_min),
            SCMP_SYS(rseq),
            SCMP_SYS(prlimit64),
            SCMP_SYS(getrlimit),
            SCMP_SYS(setrlimit),
            SCMP_SYS(wait4),
            SCMP_SYS(waitid),
            SCMP_SYS(getrandom),
            SCMP_SYS(sysinfo),
            SCMP_SYS(uname),

            /* Stat & Filesystem */
            SCMP_SYS(stat),
            SCMP_SYS(lstat),
            SCMP_SYS(fstat),
            SCMP_SYS(newfstatat),
            SCMP_SYS(statx),
            SCMP_SYS(statfs),
            SCMP_SYS(fstatfs),
        };

        for (size_t i = 0; i < sizeof(allowed_syscalls) / sizeof(allowed_syscalls[0]); ++i) {
            if (allowed_syscalls[i] != __NR_SCMP_ERROR) {
                seccomp_rule_add(ctx, SCMP_ACT_ALLOW, allowed_syscalls[i], 0);
            }
        }

        int rc = seccomp_load(ctx);
        seccomp_release(ctx);

        if (rc < 0) {
            WAYWAL_LOG_WARN("Failed to load seccomp BPF filter: %s", strerror(-rc));
        } else {
            WAYWAL_LOG_INFO("Security: Seccomp-BPF sandbox filter loaded successfully");
        }
    }
#endif

    return true;
}
