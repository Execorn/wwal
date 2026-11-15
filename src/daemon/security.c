// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/security.h"

#include "waywal/log.h"
#include "waywal/path.h"
#include "waywal/toml_parser.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <pwd.h>
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

static const waywal_config_t *g_sandbox_cfg = NULL;

void security_sandbox_register_config(const struct waywal_config *cfg) { g_sandbox_cfg = cfg; }

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

static void mkdir_p_mode(const char *path, mode_t mode)
{
    if (!path || !path[0])
        return;
    char tmp[4096];
    int n = snprintf(tmp, sizeof(tmp), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) {
        WAYWAL_LOG_WARN("Security: ignoring oversized directory path during sandbox setup");
        return;
    }
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, mode);
            *p = '/';
        }
    }
    (void)mkdir(tmp, mode);
}

static void mkdir_p_user_dir(const char *path) { mkdir_p_mode(path, 0700); }

static bool resolve_sandbox_home(char *buf, size_t cap)
{
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        int n = snprintf(buf, cap, "%s", home);
        if (n > 0 && (size_t)n < cap) {
            waywal_normalize_path(buf);
            return true;
        }
    }
    struct passwd pw;
    struct passwd *pw_res = NULL;
    char pw_buf[4096];
    if (getpwuid_r(getuid(), &pw, pw_buf, sizeof(pw_buf), &pw_res) == 0 && pw_res &&
        pw_res->pw_dir && pw_res->pw_dir[0] != '\0') {
        int n = snprintf(buf, cap, "%s", pw_res->pw_dir);
        if (n > 0 && (size_t)n < cap) {
            waywal_normalize_path(buf);
            return true;
        }
    }
    return false;
}

static bool is_sensitive_home_entry(const char *name)
{
    if (!name || name[0] == '\0')
        return true;
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, ".ssh") == 0 ||
            strcmp(name, ".gnupg") == 0 || strcmp(name, ".aws") == 0 || strcmp(name, ".pki") == 0 ||
            strcmp(name, ".password-store") == 0 || strcmp(name, ".kube") == 0 ||
            strcmp(name, ".docker") == 0 || strcmp(name, ".netrc") == 0);
}

static bool path_equals_or_inside(const char *candidate, const char *prefix)
{
    if (!candidate || !prefix || prefix[0] == '\0')
        return false;
    size_t plen = strlen(prefix);
    if (strncmp(candidate, prefix, plen) != 0)
        return false;
    return candidate[plen] == '\0' || candidate[plen] == '/';
}

static bool is_sensitive_target_path(const char *path)
{
    if (!path || path[0] == '\0')
        return true;

    char norm_path[4096];
    int pn = snprintf(norm_path, sizeof(norm_path), "%s", path);
    if (pn <= 0 || (size_t)pn >= sizeof(norm_path))
        return true;
    waywal_normalize_path(norm_path);

    char resolved_buf[4096];
    const char *check = realpath(norm_path, resolved_buf) ? resolved_buf : norm_path;

    if (strcmp(check, "/etc/shadow") == 0 || strcmp(check, "/etc/gshadow") == 0 ||
        strcmp(norm_path, "/etc/shadow") == 0 || strcmp(norm_path, "/etc/gshadow") == 0) {
        return true;
    }

    char home[4096];
    if (resolve_sandbox_home(home, sizeof(home))) {
        char real_home[4096];
        const char *homes[2] = {home, realpath(home, real_home) ? real_home : NULL};
        static const char *const sensitive_rel[] = {
            ".ssh",    ".gnupg",          ".aws",
            ".pki",    ".password-store", ".kube",
            ".docker", ".netrc",          ".local/share/keyrings",
        };
        for (size_t h = 0; h < 2; ++h) {
            if (!homes[h] || homes[h][0] == '\0')
                continue;
            for (size_t i = 0; i < sizeof(sensitive_rel) / sizeof(sensitive_rel[0]); ++i) {
                char sens_path[4096];
                if (snprintf(sens_path, sizeof(sens_path), "%s/%s", homes[h], sensitive_rel[i]) <
                    (int)sizeof(sens_path)) {
                    if (path_equals_or_inside(check, sens_path) ||
                        path_equals_or_inside(norm_path, sens_path)) {
                        return true;
                    }
                }
            }
        }
    }

    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] != '\0') {
        char real_xdg_data[4096];
        const char *data_roots[2] = {xdg_data,
                                     realpath(xdg_data, real_xdg_data) ? real_xdg_data : NULL};
        for (size_t d = 0; d < 2; ++d) {
            if (!data_roots[d] || data_roots[d][0] == '\0')
                continue;
            char sens_keyrings[4096];
            if (snprintf(sens_keyrings, sizeof(sens_keyrings), "%s/keyrings", data_roots[d]) <
                (int)sizeof(sens_keyrings)) {
                if (path_equals_or_inside(check, sens_keyrings) ||
                    path_equals_or_inside(norm_path, sens_keyrings)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool is_single_sensitive_ancestor_str(const char *cand)
{
    if (!cand || cand[0] != '/')
        return true;

    static const char *const sys_ancestors[] = {
        "/",     "/etc",  "/usr", "/bin", "/sbin", "/lib", "/lib64",
        "/boot", "/proc", "/sys", "/dev", "/var",  "/run", "/root",
    };
    for (size_t i = 0; i < sizeof(sys_ancestors) / sizeof(sys_ancestors[0]); ++i) {
        if (strcmp(cand, sys_ancestors[i]) == 0)
            return true;
    }

    if (path_equals_or_inside(cand, "/etc") || path_equals_or_inside(cand, "/boot") ||
        path_equals_or_inside(cand, "/proc") || path_equals_or_inside(cand, "/sys") ||
        path_equals_or_inside(cand, "/dev") || path_equals_or_inside(cand, "/usr") ||
        path_equals_or_inside(cand, "/bin") || path_equals_or_inside(cand, "/sbin") ||
        path_equals_or_inside(cand, "/lib") || path_equals_or_inside(cand, "/lib64") ||
        path_equals_or_inside(cand, "/root")) {
        return true;
    }

    char home[4096];
    if (resolve_sandbox_home(home, sizeof(home))) {
        waywal_normalize_path(home);
        if (path_equals_or_inside(home, cand))
            return true;
        char real_home[4096];
        if (realpath(home, real_home) && path_equals_or_inside(real_home, cand))
            return true;
    }
    return false;
}

static bool is_sensitive_ancestor(const char *path)
{
    if (!path || path[0] != '/')
        return true;

    char norm_path[4096];
    int pn = snprintf(norm_path, sizeof(norm_path), "%s", path);
    if (pn <= 0 || (size_t)pn >= sizeof(norm_path))
        return true;
    waywal_normalize_path(norm_path);

    if (is_sensitive_target_path(norm_path) || is_single_sensitive_ancestor_str(norm_path))
        return true;

    char resolved_buf[4096];
    if (realpath(norm_path, resolved_buf)) {
        if (is_sensitive_target_path(resolved_buf) ||
            is_single_sensitive_ancestor_str(resolved_buf)) {
            return true;
        }
    } else {
        /* Walk up to nearest existing ancestor to catch symlinked parent directories */
        char probe[4096];
        int prn = snprintf(probe, sizeof(probe), "%s", norm_path);
        if (prn <= 0 || (size_t)prn >= sizeof(probe))
            return true;
        while (probe[0] == '/' && strcmp(probe, "/") != 0) {
            char *slash = strrchr(probe, '/');
            if (!slash || slash == probe)
                break;
            size_t prefix_len = (size_t)(slash - probe);
            probe[prefix_len] = '\0';
            char anc_real[4096];
            if (realpath(probe, anc_real)) {
                char reconstructed[4096];
                int n = snprintf(reconstructed, sizeof(reconstructed), "%s%s", anc_real,
                                 norm_path + prefix_len);
                if (n < 0 || (size_t)n >= sizeof(reconstructed))
                    return true;
                waywal_normalize_path(reconstructed);
                if (is_sensitive_target_path(reconstructed) ||
                    is_single_sensitive_ancestor_str(reconstructed)) {
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

int security_landlock_probe_abi(void)
{
    return landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
}

uint64_t security_landlock_handled_fs_mask(int abi)
{
    if (abi <= 0) {
        return 0u;
    }
    uint64_t mask = (uint64_t)(LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                               LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                               LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                               LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                               LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
                               LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                               LANDLOCK_ACCESS_FS_MAKE_SYM);
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) {
        mask |= (uint64_t)LANDLOCK_ACCESS_FS_REFER;
    }
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) {
        mask |= (uint64_t)LANDLOCK_ACCESS_FS_TRUNCATE;
    }
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) {
        mask |= (uint64_t)LANDLOCK_ACCESS_FS_IOCTL_DEV;
    }
#endif
    return mask;
}

uint64_t security_landlock_writable_fs_mask(int abi)
{
    if (abi <= 0) {
        return 0u;
    }
    /* Writable cache/config/state/tmp directories must never retain execute permission so staged
     * payloads cannot be executed directly under confinement. */
    uint64_t mask = (uint64_t)(LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                               LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
                               LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_DIR |
                               LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK);
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) {
        mask |= (uint64_t)LANDLOCK_ACCESS_FS_REFER;
    }
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) {
        mask |= (uint64_t)LANDLOCK_ACCESS_FS_TRUNCATE;
    }
#endif
    mask &= ~(uint64_t)LANDLOCK_ACCESS_FS_EXECUTE;
    return mask;
}

typedef struct {
    dev_t dev;
    ino_t ino;
    __u64 allowed_access;
    bool is_dir;
    char path[4096];
} landlock_rule_entry_t;

typedef struct {
    landlock_rule_entry_t *items;
    size_t count;
    size_t cap;
    bool failed;
} landlock_rule_vec_t;

static void landlock_rule_vec_free(landlock_rule_vec_t *vec)
{
    if (!vec) {
        return;
    }
    free(vec->items);
    vec->items = NULL;
    vec->count = 0u;
    vec->cap = 0u;
}

static bool landlock_rule_vec_upsert(landlock_rule_vec_t *vec, dev_t dev, ino_t ino, __u64 access,
                                     bool is_dir, const char *norm_path)
{
    for (size_t i = 0; i < vec->count; ++i) {
        if (vec->items[i].dev == dev && vec->items[i].ino == ino) {
            vec->items[i].allowed_access |= access;
            return true;
        }
    }
    if (vec->count == vec->cap) {
        size_t new_cap = vec->cap == 0u ? 64u : vec->cap * 2u;
        if (new_cap < vec->cap || new_cap > SIZE_MAX / sizeof(landlock_rule_entry_t)) {
            vec->failed = true;
            return false;
        }
        landlock_rule_entry_t *grown =
            (landlock_rule_entry_t *)realloc(vec->items, new_cap * sizeof(landlock_rule_entry_t));
        if (!grown) {
            vec->failed = true;
            return false;
        }
        vec->items = grown;
        vec->cap = new_cap;
    }
    landlock_rule_entry_t *dst = &vec->items[vec->count++];
    dst->dev = dev;
    dst->ino = ino;
    dst->allowed_access = access;
    dst->is_dir = is_dir;
    int pn = snprintf(dst->path, sizeof(dst->path), "%s", norm_path);
    if (pn <= 0 || (size_t)pn >= sizeof(dst->path)) {
        vec->count--;
        vec->failed = true;
        return false;
    }
    return true;
}

static bool is_system_usrmerge_link(const char *path)
{
    return strcmp(path, "/bin") == 0 || strcmp(path, "/sbin") == 0 || strcmp(path, "/lib") == 0 ||
           strcmp(path, "/lib64") == 0;
}

static void collect_landlock_rule(landlock_rule_vec_t *vec, const char *path, __u64 allowed_access,
                                  bool dir_only)
{
    if (!vec || vec->failed || !path || !path[0] || allowed_access == 0u)
        return;

    char norm_path[4096];
    int pn = snprintf(norm_path, sizeof(norm_path), "%s", path);
    if (pn <= 0 || (size_t)pn >= sizeof(norm_path)) {
        WAYWAL_LOG_ERR("Security: Landlock rule path exceeds buffer length");
        vec->failed = true;
        return;
    }
    waywal_normalize_path(norm_path);
    if (is_sensitive_target_path(norm_path))
        return;

    bool is_dir = true;
    __u64 effective_access = allowed_access;
    int fd = open(norm_path, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int dir_err = errno;
        if (dir_err == ELOOP) {
            if (!is_system_usrmerge_link(norm_path)) {
                WAYWAL_LOG_WARN("Security: refusing symlink Landlock rule target '%s'", norm_path);
            }
            return;
        }
        if (!dir_only && dir_err == ENOTDIR) {
            fd = open(norm_path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
                if (errno == ELOOP) {
                    WAYWAL_LOG_WARN("Security: refusing symlink Landlock file target '%s'",
                                    norm_path);
                }
                return;
            }
            struct stat fst;
            /* O_PATH | O_NOFOLLOW without O_DIRECTORY opens a symlink itself, so enforce
             * S_ISREG before recording any file rule. */
            if (fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode)) {
                close(fd);
                return;
            }
            is_dir = false;
            effective_access =
                allowed_access & (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                                  LANDLOCK_ACCESS_FS_EXECUTE);
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
            effective_access |= (allowed_access & LANDLOCK_ACCESS_FS_TRUNCATE);
#endif
            if (effective_access == 0u ||
                !landlock_rule_vec_upsert(vec, fst.st_dev, fst.st_ino, effective_access, false,
                                          norm_path)) {
                close(fd);
                return;
            }
            close(fd);
            return;
        }
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return;
    }
    (void)landlock_rule_vec_upsert(vec, st.st_dev, st.st_ino, effective_access, is_dir, norm_path);
    close(fd);
}

static bool commit_landlock_rules(int ruleset_fd, const landlock_rule_vec_t *vec)
{
    if (ruleset_fd < 0 || !vec || vec->failed) {
        return false;
    }
    for (size_t i = 0; i < vec->count; ++i) {
        const landlock_rule_entry_t *entry = &vec->items[i];
        int open_flags = entry->is_dir ? (O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                                       : (O_PATH | O_NOFOLLOW | O_CLOEXEC);
        int fd = open(entry->path, open_flags);
        if (fd < 0) {
            WAYWAL_LOG_ERR("Security: failed to open Landlock target '%s': %s", entry->path,
                           strerror(errno));
            return false;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_dev != entry->dev || st.st_ino != entry->ino ||
            (entry->is_dir ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) {
            WAYWAL_LOG_ERR("Security: Landlock target '%s' changed identity before rule commit",
                           entry->path);
            close(fd);
            return false;
        }
        struct landlock_path_beneath_attr path_attr = {
            .allowed_access = entry->allowed_access,
            .parent_fd = fd,
        };
        int rc = landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
        int add_err = errno;
        close(fd);
        if (rc != 0) {
            WAYWAL_LOG_ERR("Security: landlock_add_rule failed for '%s': %s", entry->path,
                           strerror(add_err));
            return false;
        }
    }
    return true;
}

static void add_landlock_share_root_excluding_keyrings(landlock_rule_vec_t *vec,
                                                       const char *share_root, __u64 ro_access,
                                                       __u64 rw_access)
{
    if (!vec || vec->failed || !share_root || share_root[0] == '\0')
        return;
    int rfd = open(share_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rfd < 0)
        return;
    DIR *sdir = fdopendir(rfd);
    if (!sdir) {
        close(rfd);
        return;
    }
    struct dirent *sent;
    char child_path[4096];
    while ((sent = readdir(sdir)) != NULL) {
        if (strcmp(sent->d_name, ".") == 0 || strcmp(sent->d_name, "..") == 0 ||
            strcmp(sent->d_name, "keyrings") == 0)
            continue;
        int n = snprintf(child_path, sizeof(child_path), "%s/%s", share_root, sent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child_path)) {
            WAYWAL_LOG_ERR("Security: oversized XDG_DATA_HOME child path under '%s'", share_root);
            vec->failed = true;
            break;
        }
        __u64 access = (strcmp(sent->d_name, "wwal") == 0) ? rw_access : ro_access;
        collect_landlock_rule(vec, child_path, access, false);
    }
    closedir(sdir);
}

static bool format_joined_path(char *dst, size_t cap, const char *base, const char *suffix,
                               landlock_rule_vec_t *vec)
{
    int n = snprintf(dst, cap, "%s/%s", base, suffix);
    if (n <= 0 || (size_t)n >= cap) {
        WAYWAL_LOG_ERR("Security: truncated sandbox path '%s/%s'", base ? base : "",
                       suffix ? suffix : "");
        if (vec) {
            vec->failed = true;
        }
        return false;
    }
    return true;
}

static bool apply_landlock_sandbox(const char *runtime_dir)
{
    int abi = security_landlock_probe_abi();
    if (abi < 0) {
        int probe_err = errno;
        if (probe_err == ENOSYS || probe_err == EOPNOTSUPP) {
            WAYWAL_LOG_WARN("Security: Landlock LSM unsupported (%s); continuing without Landlock",
                            strerror(probe_err));
            return true;
        }
        WAYWAL_LOG_ERR("Security: Landlock ABI probe failed: %s", strerror(probe_err));
        return false;
    }
    if (abi == 0) {
        WAYWAL_LOG_ERR("Security: invalid Landlock ABI version 0");
        return false;
    }

    char home_buf[4096] = {0};
    const char *home = resolve_sandbox_home(home_buf, sizeof(home_buf)) ? home_buf : NULL;
    if (home && home[0]) {
        char pre_path[4096];
        const char *pre_dirs[] = {
            ".cache",
            ".cache/wwal",
            ".cache/wwal/exports",
            ".cache/wwal/repairs",
            ".cache/wal",
            ".cache/swww",
            ".cache/awww",
            ".local",
            ".local/bin",
            ".local/state",
            ".local/state/wwal",
            ".local/state/caelestia",
            ".local/state/caelestia/wallpaper",
            ".local/share",
            ".local/share/wwal",
            ".local/share/wallpapers",
            ".config",
            ".config/wwal",
            ".config/waywal",
            "Pictures",
            "Videos",
            "Wallpapers",
        };
        for (size_t i = 0; i < sizeof(pre_dirs) / sizeof(pre_dirs[0]); ++i) {
            int pn = snprintf(pre_path, sizeof(pre_path), "%s/%s", home, pre_dirs[i]);
            if (pn > 0 && (size_t)pn < sizeof(pre_path)) {
                mkdir_p_user_dir(pre_path);
            }
        }
    }

    struct landlock_ruleset_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = (__u64)security_landlock_handled_fs_mask(abi);
#ifdef LANDLOCK_ACCESS_NET_BIND_TCP
    if (abi >= 4) {
        attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
    }
#endif

    int ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
    if (ruleset_fd < 0) {
        WAYWAL_LOG_ERR("Security: landlock_create_ruleset(ABI v%d) failed: %s", abi,
                       strerror(errno));
        return false;
    }

    const __u64 ro_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    const __u64 rx_access = ro_access | LANDLOCK_ACCESS_FS_EXECUTE;
    const __u64 rw_access = (__u64)security_landlock_writable_fs_mask(abi);
    const __u64 cache_rw_access = rw_access | LANDLOCK_ACCESS_FS_MAKE_SYM;
    __u64 dev_access = rw_access;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) {
        dev_access |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    }
#endif

    landlock_rule_vec_t rules = {0};

    /* Standard system binary and library mounts for DRM/Mesa/VA-API and user hooks */
    const char *rx_paths[] = {"/usr", "/bin", "/sbin", "/lib", "/lib64", "/nix", "/opt", NULL};
    for (int i = 0; rx_paths[i] != NULL; ++i) {
        collect_landlock_rule(&rules, rx_paths[i], rx_access, true);
    }

    /* Read-only system and external media mounts */
    const char *ro_paths[] = {"/etc",   "/sys",       "/proc",     "/mnt",
                              "/media", "/run/media", "/var/home", NULL};
    for (int i = 0; ro_paths[i] != NULL; ++i) {
        collect_landlock_rule(&rules, ro_paths[i], ro_access, true);
    }

    const char *cur_user = getenv("USER");
    if (cur_user && cur_user[0] != '\0') {
        char user_media[4096];
        if (format_joined_path(user_media, sizeof(user_media), "/run/media", cur_user, &rules)) {
            struct stat st_u;
            if (stat(user_media, &st_u) == 0 && S_ISDIR(st_u.st_mode)) {
                collect_landlock_rule(&rules, user_media, ro_access, true);
            }
        }
    }

    /* Allow device ioctls strictly under /dev and /dev/dri, and writable staging under /tmp */
    collect_landlock_rule(&rules, "/dev", dev_access, true);
    collect_landlock_rule(&rules, "/dev/dri", dev_access, true);
    if (!(home && strncmp(home, "/tmp/wwal_sbox_test_", 20) == 0)) {
        collect_landlock_rule(&rules, "/tmp", rw_access, true);
    }

    const char *rt = runtime_dir ? runtime_dir : getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) {
        collect_landlock_rule(&rules, rt, rw_access, true);
    }

    const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
    if (xdg_cfg && xdg_cfg[0]) {
        mkdir_p_user_dir(xdg_cfg);
        collect_landlock_rule(&rules, xdg_cfg, ro_access, true);
        const char *cfg_rw_subdirs[] = {"wwal", "waywal"};
        for (size_t i = 0; i < sizeof(cfg_rw_subdirs) / sizeof(cfg_rw_subdirs[0]); ++i) {
            char cfg_sub[4096];
            if (format_joined_path(cfg_sub, sizeof(cfg_sub), xdg_cfg, cfg_rw_subdirs[i], &rules)) {
                mkdir_p_user_dir(cfg_sub);
                collect_landlock_rule(&rules, cfg_sub, rw_access, true);
            }
        }
    }

    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0]) {
        add_landlock_share_root_excluding_keyrings(&rules, xdg_data, ro_access, rw_access);
    }

    const char *ro_env_dirs[] = {
        getenv("XDG_PICTURES_DIR"),       getenv("XDG_VIDEOS_DIR"),  getenv("WWAL_WORKSHOP_DIR"),
        getenv("WWAL_SHADER_PRESET_DIR"), getenv("WWAL_SHADER_DIR"),
    };
    for (size_t i = 0; i < sizeof(ro_env_dirs) / sizeof(ro_env_dirs[0]); ++i) {
        if (ro_env_dirs[i] && ro_env_dirs[i][0]) {
            collect_landlock_rule(&rules, ro_env_dirs[i], ro_access, false);
        }
    }

#ifdef WWAL_DATADIR
    collect_landlock_rule(&rules, WWAL_DATADIR, ro_access, true);
#endif
#ifdef WWAL_SOURCE_ROOT
    collect_landlock_rule(&rules, WWAL_SOURCE_ROOT "/shaders", ro_access, true);
    collect_landlock_rule(&rules, WWAL_SOURCE_ROOT "/contrib", ro_access, true);
    collect_landlock_rule(&rules, WWAL_SOURCE_ROOT "/tests", ro_access, true);
#endif
    {
        char exe_buf[4096];
        ssize_t elen = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
        if (elen > 0) {
            exe_buf[elen] = '\0';
            char *slash = strrchr(exe_buf, '/');
            if (slash) {
                *slash = '\0';
                char rel_dir[4096];
                if (format_joined_path(rel_dir, sizeof(rel_dir), exe_buf, "../shaders", NULL)) {
                    collect_landlock_rule(&rules, rel_dir, ro_access, true);
                }
                if (format_joined_path(rel_dir, sizeof(rel_dir), exe_buf, "../share/wwal/shaders",
                                       NULL)) {
                    collect_landlock_rule(&rules, rel_dir, ro_access, true);
                }
            }
        }
    }

    const char *wwal_cache = getenv("WWAL_CACHE_DIR");
    if (wwal_cache && wwal_cache[0]) {
        mkdir_p_user_dir(wwal_cache);
        collect_landlock_rule(&rules, wwal_cache, cache_rw_access, true);
    }

    const char *wwal_state = getenv("WWAL_STATE_DIR");
    if (wwal_state && wwal_state[0]) {
        mkdir_p_user_dir(wwal_state);
        collect_landlock_rule(&rules, wwal_state, rw_access, true);
    }

    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    if (xdg_cache && xdg_cache[0]) {
        mkdir_p_user_dir(xdg_cache);
        collect_landlock_rule(&rules, xdg_cache, rw_access, true);
        const char *cache_subdirs[] = {"wwal", "wwal/exports", "wwal/repairs",
                                       "wal",  "swww",         "awww"};
        for (size_t i = 0; i < sizeof(cache_subdirs) / sizeof(cache_subdirs[0]); ++i) {
            char cache_sub[4096];
            if (format_joined_path(cache_sub, sizeof(cache_sub), xdg_cache, cache_subdirs[i],
                                   &rules)) {
                mkdir_p_user_dir(cache_sub);
                if (strcmp(cache_subdirs[i], "wwal") == 0) {
                    collect_landlock_rule(&rules, cache_sub, cache_rw_access, true);
                }
            }
        }
    }

    const char *xdg_state = getenv("XDG_STATE_HOME");
    if (xdg_state && xdg_state[0]) {
        mkdir_p_user_dir(xdg_state);
        const char *state_subdirs[] = {"wwal", "caelestia", "caelestia/wallpaper"};
        for (size_t i = 0; i < sizeof(state_subdirs) / sizeof(state_subdirs[0]); ++i) {
            char state_sub[4096];
            if (format_joined_path(state_sub, sizeof(state_sub), xdg_state, state_subdirs[i],
                                   &rules)) {
                mkdir_p_user_dir(state_sub);
                collect_landlock_rule(&rules, state_sub, rw_access, true);
            }
        }
    }

    char cwd_buf[4096];
    if (getcwd(cwd_buf, sizeof(cwd_buf)) != NULL) {
        if (!home || strcmp(cwd_buf, home) != 0) {
            collect_landlock_rule(&rules, cwd_buf, ro_access, true);
        }
    }

    if (home && home[0]) {
        char path_buf[4096];

        /* Enumerate top-level $HOME entries read-only, skipping sensitive keystores and explicit
         * XDG subtrees so least-privilege rules below stay authoritative. */
        int hfd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (hfd >= 0) {
            DIR *hdir = fdopendir(hfd);
            if (!hdir) {
                close(hfd);
            } else {
                struct dirent *ent;
                while ((ent = readdir(hdir)) != NULL) {
                    if (is_sensitive_home_entry(ent->d_name) ||
                        strcmp(ent->d_name, ".local") == 0 || strcmp(ent->d_name, ".cache") == 0 ||
                        strcmp(ent->d_name, ".config") == 0) {
                        continue;
                    }
                    if (format_joined_path(path_buf, sizeof(path_buf), home, ent->d_name, &rules)) {
                        __u64 ent_access =
                            (strcmp(ent->d_name, "bin") == 0 || strcmp(ent->d_name, ".cargo") == 0)
                                ? rx_access
                                : ro_access;
                        collect_landlock_rule(&rules, path_buf, ent_access, false);
                    }
                }
                closedir(hdir);
            }
        }

        char local_share[4096];
        if (format_joined_path(local_share, sizeof(local_share), home, ".local/share", &rules)) {
            add_landlock_share_root_excluding_keyrings(&rules, local_share, ro_access, rw_access);
        }

        const char *home_rx_subdirs[] = {".local/bin", "bin", ".cargo/bin"};
        for (size_t i = 0; i < sizeof(home_rx_subdirs) / sizeof(home_rx_subdirs[0]); ++i) {
            if (format_joined_path(path_buf, sizeof(path_buf), home, home_rx_subdirs[i], &rules)) {
                collect_landlock_rule(&rules, path_buf, rx_access, true);
            }
        }

        const char *home_ro_subdirs[] = {
            "Pictures",           "Videos",  "Wallpapers",
            "Downloads",          ".config", ".local/share/wallpapers",
            ".local/share/Steam",
        };
        for (size_t i = 0; i < sizeof(home_ro_subdirs) / sizeof(home_ro_subdirs[0]); ++i) {
            if (format_joined_path(path_buf, sizeof(path_buf), home, home_ro_subdirs[i], &rules)) {
                collect_landlock_rule(&rules, path_buf, ro_access, true);
            }
        }

        if (format_joined_path(path_buf, sizeof(path_buf), home, ".cache", &rules)) {
            collect_landlock_rule(&rules, path_buf, rw_access, true);
        }
        if (format_joined_path(path_buf, sizeof(path_buf), home, ".cache/wwal", &rules)) {
            collect_landlock_rule(&rules, path_buf, cache_rw_access, true);
        }

        const char *home_rw_subdirs[] = {
            ".local/state/wwal", ".local/state/caelestia", ".local/state/caelestia/wallpaper",
            ".config/wwal",      ".config/waywal",
        };
        for (size_t i = 0; i < sizeof(home_rw_subdirs) / sizeof(home_rw_subdirs[0]); ++i) {
            if (format_joined_path(path_buf, sizeof(path_buf), home, home_rw_subdirs[i], &rules)) {
                collect_landlock_rule(&rules, path_buf, rw_access, true);
            }
        }
    }

    if (g_sandbox_cfg) {
        char real_home[4096] = {0};
        if (home && home[0]) {
            if (!realpath(home, real_home)) {
                real_home[0] = '\0';
            }
        }
        for (size_t i = 0; i < g_sandbox_cfg->template_count; ++i) {
            const waywal_template_rule_t *tr = &g_sandbox_cfg->templates[i];
            if (!tr->enabled)
                continue;
            if (tr->output_path[0] != '\0') {
                char out_norm[4096];
                int on = snprintf(out_norm, sizeof(out_norm), "%s", tr->output_path);
                if (on <= 0 || (size_t)on >= sizeof(out_norm)) {
                    WAYWAL_LOG_ERR("Security: oversized template output path '%s'", tr->name);
                    rules.failed = true;
                    break;
                }
                waywal_normalize_path(out_norm);
                if (!is_sensitive_target_path(out_norm)) {
                    path_buf_t pb;
                    path_buf_from_str(&pb, out_norm);
                    if (path_buf_parent(&pb) && pb.len > 0) {
                        waywal_normalize_path(pb.data);
                        if ((home && strcmp(pb.data, home) == 0) ||
                            (real_home[0] && strcmp(pb.data, real_home) == 0)) {
                            WAYWAL_LOG_WARN("Security: template '%s' output '%s' resides directly "
                                            "in $HOME; template outputs must reside in a "
                                            "subdirectory (e.g. ~/.config/... or ~/.cache/...)",
                                            tr->name, tr->output_path);
                        } else if (!is_sensitive_ancestor(pb.data)) {
                            mkdir_p_mode(pb.data, 0755);
                            collect_landlock_rule(&rules, pb.data, rw_access, true);
                        }
                    }
                }
            }
            if (tr->input_path[0] != '\0') {
                char in_norm[4096];
                int inn = snprintf(in_norm, sizeof(in_norm), "%s", tr->input_path);
                if (inn <= 0 || (size_t)inn >= sizeof(in_norm)) {
                    WAYWAL_LOG_ERR("Security: oversized template input path '%s'", tr->name);
                    rules.failed = true;
                    break;
                }
                waywal_normalize_path(in_norm);
                if (!is_sensitive_target_path(in_norm)) {
                    path_buf_t pb;
                    path_buf_from_str(&pb, in_norm);
                    if (path_buf_parent(&pb) && pb.len > 0) {
                        waywal_normalize_path(pb.data);
                        if (!is_sensitive_ancestor(pb.data)) {
                            collect_landlock_rule(&rules, pb.data, ro_access, true);
                        }
                    }
                }
            }
        }

        const char *ro_cfg_paths[WAYWAL_CONFIG_MAX_SCHEDULE + WAYWAL_CONFIG_MAX_OUTPUTS +
                                 WAYWAL_CONFIG_MAX_WORKSPACES + 8];
        size_t ro_cfg_count = 0;
        for (size_t i = 0; i < g_sandbox_cfg->schedule_count && i < WAYWAL_CONFIG_MAX_SCHEDULE;
             ++i) {
            if (g_sandbox_cfg->schedule[i].path[0] != '\0') {
                ro_cfg_paths[ro_cfg_count++] = g_sandbox_cfg->schedule[i].path;
            }
        }
        for (size_t i = 0; i < g_sandbox_cfg->output_count && i < WAYWAL_CONFIG_MAX_OUTPUTS; ++i) {
            if (g_sandbox_cfg->outputs[i].path[0] != '\0') {
                ro_cfg_paths[ro_cfg_count++] = g_sandbox_cfg->outputs[i].path;
            }
        }
        for (size_t i = 0; i < g_sandbox_cfg->workspace_count && i < WAYWAL_CONFIG_MAX_WORKSPACES;
             ++i) {
            if (g_sandbox_cfg->workspaces[i].path[0] != '\0') {
                ro_cfg_paths[ro_cfg_count++] = g_sandbox_cfg->workspaces[i].path;
            }
        }
        if (g_sandbox_cfg->solar.dawn_path[0] != '\0')
            ro_cfg_paths[ro_cfg_count++] = g_sandbox_cfg->solar.dawn_path;
        if (g_sandbox_cfg->solar.day_path[0] != '\0')
            ro_cfg_paths[ro_cfg_count++] = g_sandbox_cfg->solar.day_path;
        if (g_sandbox_cfg->solar.dusk_path[0] != '\0')
            ro_cfg_paths[ro_cfg_count++] = g_sandbox_cfg->solar.dusk_path;
        if (g_sandbox_cfg->solar.night_path[0] != '\0')
            ro_cfg_paths[ro_cfg_count++] = g_sandbox_cfg->solar.night_path;
        if (g_sandbox_cfg->solar.manifest_path[0] != '\0')
            ro_cfg_paths[ro_cfg_count++] = g_sandbox_cfg->solar.manifest_path;

        for (size_t i = 0; i < ro_cfg_count && !rules.failed; ++i) {
            char p_norm[4096];
            int cn = snprintf(p_norm, sizeof(p_norm), "%s", ro_cfg_paths[i]);
            if (cn <= 0 || (size_t)cn >= sizeof(p_norm)) {
                WAYWAL_LOG_ERR("Security: oversized configured media path");
                rules.failed = true;
                break;
            }
            waywal_normalize_path(p_norm);
            if (!is_sensitive_target_path(p_norm)) {
                path_buf_t pb;
                path_buf_from_str(&pb, p_norm);
                if (path_buf_parent(&pb) && pb.len > 0) {
                    waywal_normalize_path(pb.data);
                    if (!((home && strcmp(pb.data, home) == 0) ||
                          (real_home[0] && strcmp(pb.data, real_home) == 0)) &&
                        !is_sensitive_ancestor(pb.data)) {
                        collect_landlock_rule(&rules, pb.data, ro_access, true);
                    }
                }
            }
        }
    }

    bool ok = commit_landlock_rules(ruleset_fd, &rules);
    landlock_rule_vec_free(&rules);
    if (!ok) {
        close(ruleset_fd);
        return false;
    }

    if (landlock_restrict_self(ruleset_fd, 0) != 0) {
        WAYWAL_LOG_ERR("Security: landlock_restrict_self(ABI v%d) failed: %s", abi,
                       strerror(errno));
        close(ruleset_fd);
        return false;
    }

    WAYWAL_LOG_INFO("Security: Landlock LSM filesystem sandbox engaged (ABI v%d)", abi);
    close(ruleset_fd);
    return true;
}

bool security_sandbox_apply(const char *runtime_dir)
{
    /* 1. Prevent privilege escalation */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        WAYWAL_LOG_ERR("Security: PR_SET_NO_NEW_PRIVS failed: %s", strerror(errno));
        return false;
    }
    WAYWAL_LOG_INFO("Security: PR_SET_NO_NEW_PRIVS enforced");

    /* 2. Landlock LSM filesystem sandboxing */
    if (!apply_landlock_sandbox(runtime_dir)) {
        WAYWAL_LOG_ERR("Security: Landlock LSM sandbox setup failed");
        return false;
    }

#ifdef HAVE_LIBSECCOMP
    /* 3. Seccomp-BPF syscall sandbox (default deny with EPERM; note shm* and timer_create
     * remain intentionally excluded from allowed_syscalls) */
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ERRNO(EPERM));
    if (!ctx) {
        WAYWAL_LOG_ERR("Security: seccomp_init failed");
        return false;
    }

    /* Immediately terminate process on high-risk kernel, namespace, and VM-inspection syscalls */
    const int denied_syscalls[] = {
        SCMP_SYS(ptrace),
#ifdef __NR_process_vm_readv
        SCMP_SYS(process_vm_readv),
#endif
#ifdef __NR_process_vm_writev
        SCMP_SYS(process_vm_writev),
#endif
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
        if (denied_syscalls[i] == __NR_SCMP_ERROR)
            continue;
        int add_rc = seccomp_rule_add(ctx, SCMP_ACT_KILL_PROCESS, denied_syscalls[i], 0);
        if (add_rc < 0 && add_rc != -EEXIST) {
            WAYWAL_LOG_ERR("Security: seccomp_rule_add(KILL, nr=%d) failed: %s", denied_syscalls[i],
                           strerror(-add_rc));
            seccomp_release(ctx);
            return false;
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
        SCMP_SYS(execveat),
        SCMP_SYS(kill),
        SCMP_SYS(arch_prctl),
        SCMP_SYS(set_tid_address),
        SCMP_SYS(getppid),
        SCMP_SYS(getpgrp),
        SCMP_SYS(getpgid),
        SCMP_SYS(setpgid),
        SCMP_SYS(setsid),
        SCMP_SYS(getsid),
#ifdef __NR_pidfd_open
        SCMP_SYS(pidfd_open),
#endif
#ifdef __NR_pidfd_send_signal
        SCMP_SYS(pidfd_send_signal),
#endif
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
#ifdef __NR_epoll_pwait2
        SCMP_SYS(epoll_pwait2),
#endif
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
        SCMP_SYS(preadv),
        SCMP_SYS(pwritev),
        SCMP_SYS(pread64),
        SCMP_SYS(pwrite64),
#ifdef __NR_preadv2
        SCMP_SYS(preadv2),
#endif
#ifdef __NR_pwritev2
        SCMP_SYS(pwritev2),
#endif
#ifdef __NR_copy_file_range
        SCMP_SYS(copy_file_range),
#endif
        SCMP_SYS(sendfile),
        SCMP_SYS(splice),
#ifdef __NR_fadvise64
        SCMP_SYS(fadvise64),
#endif
        SCMP_SYS(lseek),
        SCMP_SYS(fcntl),
        SCMP_SYS(dup),
        SCMP_SYS(dup2),
        SCMP_SYS(dup3),
        SCMP_SYS(pipe),
        SCMP_SYS(pipe2),
        SCMP_SYS(truncate),
        SCMP_SYS(ftruncate),
        SCMP_SYS(fdatasync),
        SCMP_SYS(fsync),
#ifdef __NR_chmod
        SCMP_SYS(chmod),
#endif
#ifdef __NR_fchmod
        SCMP_SYS(fchmod),
#endif
        SCMP_SYS(fchmodat),
#ifdef __NR_fchmodat2
        SCMP_SYS(fchmodat2),
#endif
#ifdef __NR_chown
        SCMP_SYS(chown),
#endif
#ifdef __NR_fchown
        SCMP_SYS(fchown),
#endif
#ifdef __NR_fchownat
        SCMP_SYS(fchownat),
#endif
#ifdef __NR_lchown
        SCMP_SYS(lchown),
#endif
        SCMP_SYS(utimensat),
#ifdef __NR_futimesat
        SCMP_SYS(futimesat),
#endif
#ifdef __NR_close_range
        SCMP_SYS(close_range),
#endif
        SCMP_SYS(rename),
        SCMP_SYS(renameat),
        SCMP_SYS(renameat2),
#ifdef __NR_link
        SCMP_SYS(link),
#endif
#ifdef __NR_linkat
        SCMP_SYS(linkat),
#endif
        SCMP_SYS(mkdir),
        SCMP_SYS(mkdirat),
        SCMP_SYS(rmdir),
        SCMP_SYS(unlink),
        SCMP_SYS(unlinkat),
#ifdef __NR_mknod
        SCMP_SYS(mknod),
#endif
#ifdef __NR_mknodat
        SCMP_SYS(mknodat),
#endif
#ifdef __NR_symlink
        SCMP_SYS(symlink),
#endif
        SCMP_SYS(symlinkat),
        SCMP_SYS(open),
        SCMP_SYS(openat),
#ifdef __NR_openat2
        SCMP_SYS(openat2),
#endif
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
#ifdef __NR_sendmmsg
        SCMP_SYS(sendmmsg),
#endif
#ifdef __NR_recvmmsg
        SCMP_SYS(recvmmsg),
#endif
        SCMP_SYS(shutdown),

        /* Memory */
        SCMP_SYS(mprotect),
        SCMP_SYS(mremap),
        SCMP_SYS(brk),
        SCMP_SYS(madvise),
        SCMP_SYS(mincore),
        SCMP_SYS(memfd_create),
        SCMP_SYS(fallocate),
        SCMP_SYS(flock),
#ifdef __NR_kcmp
        SCMP_SYS(kcmp),
#endif
#ifdef __NR_membarrier
        SCMP_SYS(membarrier),
#endif

        /* Clocks & Timers */
        SCMP_SYS(clock_getres),
        SCMP_SYS(clock_nanosleep),
        SCMP_SYS(nanosleep),
        SCMP_SYS(gettimeofday),
        SCMP_SYS(setitimer),
        SCMP_SYS(getitimer),
#ifdef __NR_alarm
        SCMP_SYS(alarm),
#endif

        /* Process & Signals */
        SCMP_SYS(prctl),
        SCMP_SYS(rt_sigaction),
        SCMP_SYS(rt_sigprocmask),
        SCMP_SYS(rt_sigpending),
        SCMP_SYS(rt_sigsuspend),
        SCMP_SYS(rt_sigtimedwait),
        SCMP_SYS(sigaltstack),
        SCMP_SYS(tgkill),
        SCMP_SYS(tkill),
        SCMP_SYS(futex),
#ifdef __NR_futex_waitv
        SCMP_SYS(futex_waitv),
#endif
        SCMP_SYS(set_robust_list),
        SCMP_SYS(get_robust_list),
        SCMP_SYS(getpid),
        SCMP_SYS(gettid),
        SCMP_SYS(getuid),
        SCMP_SYS(geteuid),
        SCMP_SYS(getgid),
        SCMP_SYS(getegid),
#ifdef __NR_setuid
        SCMP_SYS(setuid),
#endif
#ifdef __NR_setgid
        SCMP_SYS(setgid),
#endif
#ifdef __NR_setreuid
        SCMP_SYS(setreuid),
#endif
#ifdef __NR_setregid
        SCMP_SYS(setregid),
#endif
#ifdef __NR_setresuid
        SCMP_SYS(setresuid),
#endif
#ifdef __NR_setresgid
        SCMP_SYS(setresgid),
#endif
#ifdef __NR_getresuid
        SCMP_SYS(getresuid),
#endif
#ifdef __NR_getresgid
        SCMP_SYS(getresgid),
#endif
#ifdef __NR_getgroups
        SCMP_SYS(getgroups),
#endif
#ifdef __NR_setgroups
        SCMP_SYS(setgroups),
#endif
#ifdef __NR_capget
        SCMP_SYS(capget),
#endif
#ifdef __NR_capset
        SCMP_SYS(capset),
#endif
        SCMP_SYS(getpriority),
        SCMP_SYS(setpriority),
        SCMP_SYS(sched_yield),
        SCMP_SYS(sched_getaffinity),
        SCMP_SYS(sched_setaffinity),
        SCMP_SYS(sched_getparam),
        SCMP_SYS(sched_setparam),
        SCMP_SYS(sched_getscheduler),
        SCMP_SYS(sched_setscheduler),
#ifdef __NR_sched_setattr
        SCMP_SYS(sched_setattr),
#endif
#ifdef __NR_sched_getattr
        SCMP_SYS(sched_getattr),
#endif
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
        int sc = allowed_syscalls[i];
        if (sc == __NR_SCMP_ERROR)
            continue;
        bool dup = false;
        for (size_t j = 0; j < i; ++j) {
            if (allowed_syscalls[j] == sc) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        int add_rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, sc, 0);
        if (add_rc < 0) {
            WAYWAL_LOG_ERR("Security: seccomp_rule_add(ALLOW, nr=%d) failed: %s", sc,
                           strerror(-add_rc));
            seccomp_release(ctx);
            return false;
        }
    }

    int rc = seccomp_load(ctx);
    seccomp_release(ctx);

    if (rc < 0) {
        WAYWAL_LOG_ERR("Security: failed to load seccomp BPF filter: %s", strerror(-rc));
        return false;
    }
    WAYWAL_LOG_INFO("Security: Seccomp-BPF sandbox filter loaded successfully");
#endif

    return true;
}
