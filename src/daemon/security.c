#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/security.h"
#include "waywal/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/landlock.h>
#include <sched.h>

#ifdef HAVE_LIBSECCOMP
#include <seccomp.h>
#endif

#ifndef landlock_create_ruleset
static inline int landlock_create_ruleset(
    const struct landlock_ruleset_attr *const attr,
    const size_t size, const __u32 flags) {
    return (int)syscall(SYS_landlock_create_ruleset, attr, size, flags);
}
#endif

#ifndef landlock_add_rule
static inline int landlock_add_rule(
    const int ruleset_fd, const enum landlock_rule_type rule_type,
    const void *const rule_attr, const __u32 flags) {
    return (int)syscall(SYS_landlock_add_rule, ruleset_fd, rule_type, rule_attr, flags);
}
#endif

#ifndef landlock_restrict_self
static inline int landlock_restrict_self(const int ruleset_fd, const __u32 flags) {
    return (int)syscall(SYS_landlock_restrict_self, ruleset_fd, flags);
}
#endif

bool security_sandbox_apply(const char *runtime_dir) {
    /* 1. Prevent privilege escalation */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        WAYWAL_LOG_WARN("PR_SET_NO_NEW_PRIVS failed: %s", strerror(errno));
    } else {
        WAYWAL_LOG_INFO("Security: PR_SET_NO_NEW_PRIVS enforced");
    }

    /* 2. Landlock LSM filesystem sandboxing */
    int abi = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi > 0) {
        struct landlock_ruleset_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.handled_access_fs =
            LANDLOCK_ACCESS_FS_EXECUTE |
            LANDLOCK_ACCESS_FS_READ_FILE |
            LANDLOCK_ACCESS_FS_READ_DIR |
            LANDLOCK_ACCESS_FS_WRITE_FILE |
            LANDLOCK_ACCESS_FS_MAKE_REG |
            LANDLOCK_ACCESS_FS_REMOVE_FILE;

        int ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
        if (ruleset_fd >= 0) {
            /* Allow read-only across standard system mounts required for DRM/Mesa/VA-API drivers */
            const char *ro_paths[] = { "/", "/usr", "/etc", "/sys", "/proc", "/dev", "/home", NULL };
            for (int i = 0; ro_paths[i] != NULL; ++i) {
                int dir_fd = open(ro_paths[i], O_PATH | O_DIRECTORY | O_CLOEXEC);
                if (dir_fd >= 0) {
                    struct landlock_path_beneath_attr path_attr = {
                        .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR,
                        .parent_fd      = dir_fd,
                    };
                    landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
                    close(dir_fd);
                }
            }

            /* Allow read-write strictly for DRI device nodes (/dev/dri) and /tmp (ARCH-07) */
            const char *rw_paths[] = { "/dev/dri", "/tmp", NULL };
            for (int i = 0; rw_paths[i] != NULL; ++i) {
                int dir_fd = open(rw_paths[i], O_PATH | O_DIRECTORY | O_CLOEXEC);
                if (dir_fd < 0 && strcmp(rw_paths[i], "/dev/dri") == 0) {
                    /* Fallback to /dev if /dev/dri not present (e.g. non-DRI container) */
                    dir_fd = open("/dev", O_PATH | O_DIRECTORY | O_CLOEXEC);
                }
                if (dir_fd >= 0) {
                    struct landlock_path_beneath_attr path_attr = {
                        .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE |
                                          LANDLOCK_ACCESS_FS_READ_DIR |
                                          LANDLOCK_ACCESS_FS_WRITE_FILE |
                                          LANDLOCK_ACCESS_FS_MAKE_REG |
                                          LANDLOCK_ACCESS_FS_REMOVE_FILE,
                        .parent_fd      = dir_fd,
                    };
                    landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
                    close(dir_fd);
                }
            }

            /* Allow read-write in runtime directory for IPC socket management */
            const char *rt = runtime_dir ? runtime_dir : getenv("XDG_RUNTIME_DIR");
            if (rt) {
                int rt_fd = open(rt, O_PATH | O_DIRECTORY | O_CLOEXEC);
                if (rt_fd >= 0) {
                    struct landlock_path_beneath_attr path_attr = {
                        .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE |
                                          LANDLOCK_ACCESS_FS_READ_DIR |
                                          LANDLOCK_ACCESS_FS_WRITE_FILE |
                                          LANDLOCK_ACCESS_FS_MAKE_REG |
                                          LANDLOCK_ACCESS_FS_REMOVE_FILE,
                        .parent_fd      = rt_fd,
                    };
                    landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
                    close(rt_fd);
                }
            }

            if (landlock_restrict_self(ruleset_fd, 0) == 0) {
                WAYWAL_LOG_INFO("Security: Landlock LSM filesystem sandbox engaged (ABI v%d)", abi);
            }
            close(ruleset_fd);
        }
    } else {
        WAYWAL_LOG_DEBUG("Landlock LSM not available on current kernel");
    }

#ifdef HAVE_LIBSECCOMP
    /* 3. Seccomp-BPF syscall sandbox (SOTA-09) */
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ERRNO(EPERM));
    if (ctx) {
        /* Denied Dangerous Syscalls -> SCMP_ACT_KILL (SOTA-09) */
        const int denied_syscalls[] = {
            SCMP_SYS(execve),
            SCMP_SYS(execveat),
            SCMP_SYS(fork),
            SCMP_SYS(vfork),
            SCMP_SYS(ptrace),
            SCMP_SYS(kill),
            SCMP_SYS(tkill),
            SCMP_SYS(tgkill),
            SCMP_SYS(mount),
            SCMP_SYS(umount2),
            SCMP_SYS(chroot),
            SCMP_SYS(pivot_root),
            SCMP_SYS(reboot),
            SCMP_SYS(kexec_load),
            SCMP_SYS(init_module),
            SCMP_SYS(delete_module),
            SCMP_SYS(bpf),
        };

        for (size_t i = 0; i < sizeof(denied_syscalls) / sizeof(denied_syscalls[0]); ++i) {
            if (denied_syscalls[i] != __NR_SCMP_ERROR) {
                seccomp_rule_add(ctx, SCMP_ACT_KILL, denied_syscalls[i], 0);
            }
        }

        /* Clone handling: allow threads (CLONE_THREAD for driver worker threads), kill process forking */
        seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(clone), 1,
                         SCMP_A0(SCMP_CMP_MASKED_EQ, (scmp_datum_t)CLONE_THREAD, (scmp_datum_t)CLONE_THREAD));
        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(clone), 1,
                         SCMP_A0(SCMP_CMP_MASKED_EQ, (scmp_datum_t)CLONE_THREAD, 0));

        /* Route clone3 to ENOSYS so libc falls back to clone(CLONE_THREAD) */
        if (SCMP_SYS(clone3) != __NR_SCMP_ERROR) {
            seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(clone3), 0);
        }

        /* Explicitly Allowed Syscalls -> SCMP_ACT_ALLOW (SOTA-09) */
        const int allowed_syscalls[] = {
            /* Core SOTA-09 allowlist */
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
            SCMP_SYS(open),
            SCMP_SYS(openat),
            SCMP_SYS(access),
            SCMP_SYS(faccessat),
            SCMP_SYS(faccessat2),
            SCMP_SYS(getdents),
            SCMP_SYS(getdents64),
            SCMP_SYS(readlink),
            SCMP_SYS(readlinkat),

            /* Sockets & IPC */
            SCMP_SYS(socket),
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
            SCMP_SYS(brk),
            SCMP_SYS(madvise),
            SCMP_SYS(memfd_create),

            /* Clocks */
            SCMP_SYS(clock_getres),
            SCMP_SYS(clock_nanosleep),
            SCMP_SYS(nanosleep),
            SCMP_SYS(gettimeofday),

            /* Process & Signals */
            SCMP_SYS(prctl),
            SCMP_SYS(rt_sigaction),
            SCMP_SYS(rt_sigprocmask),
            SCMP_SYS(futex),
            SCMP_SYS(getpid),
            SCMP_SYS(gettid),
            SCMP_SYS(getuid),
            SCMP_SYS(geteuid),
            SCMP_SYS(getgid),
            SCMP_SYS(getegid),
            SCMP_SYS(sched_yield),
            SCMP_SYS(sched_getaffinity),
            SCMP_SYS(sched_setaffinity),
            SCMP_SYS(rseq),
            SCMP_SYS(getrandom),
            SCMP_SYS(sysinfo),
            SCMP_SYS(uname),

            /* Stat & Filesystem */
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
