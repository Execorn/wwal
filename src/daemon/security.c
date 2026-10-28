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

void security_sandbox_register_config(const struct waywal_config *cfg)
{
    g_sandbox_cfg = cfg;
}

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
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(tmp, mode);
            *p = '/';
        }
    }
    (void)mkdir(tmp, mode);
}

static void mkdir_p_user_dir(const char *path)
{
    mkdir_p_mode(path, 0700);
}

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
    snprintf(norm_path, sizeof(norm_path), "%s", path);
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
            ".ssh",  ".gnupg",  ".aws",   ".pki", ".password-store",
            ".kube", ".docker", ".netrc", ".local/share/keyrings",
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
        const char *data_roots[2] = {
            xdg_data, realpath(xdg_data, real_xdg_data) ? real_xdg_data : NULL};
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
        "/",      "/etc",  "/usr",  "/bin",  "/sbin", "/lib", "/lib64",
        "/boot",  "/proc", "/sys",  "/dev",  "/var",  "/run", "/root",
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
    snprintf(norm_path, sizeof(norm_path), "%s", path);
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
        snprintf(probe, sizeof(probe), "%s", norm_path);
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

static void add_landlock_dir_rule(int ruleset_fd, const char *path, __u64 allowed_access)
{
    if (ruleset_fd < 0 || !path || !path[0])
        return;
    if (is_sensitive_target_path(path))
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

static void add_landlock_share_root_excluding_keyrings(int ruleset_fd, const char *share_root,
                                                       __u64 ro_access)
{
    if (ruleset_fd < 0 || !share_root || share_root[0] == '\0')
        return;
    DIR *sdir = opendir(share_root);
    if (!sdir)
        return;
    struct dirent *sent;
    char child_path[4096];
    while ((sent = readdir(sdir)) != NULL) {
        if (strcmp(sent->d_name, ".") == 0 || strcmp(sent->d_name, "..") == 0 ||
            strcmp(sent->d_name, "keyrings") == 0)
            continue;
        int n = snprintf(child_path, sizeof(child_path), "%s/%s", share_root, sent->d_name);
        if (n > 0 && (size_t)n < sizeof(child_path)) {
            add_landlock_dir_rule(ruleset_fd, child_path, ro_access);
        }
    }
    closedir(sdir);
}

static void apply_landlock_sandbox(const char *runtime_dir)
{
    int abi = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi <= 0) {
        WAYWAL_LOG_DEBUG("Landlock LSM not available on current kernel");
        return;
    }

    char home_buf[4096] = {0};
    const char *home = resolve_sandbox_home(home_buf, sizeof(home_buf)) ? home_buf : NULL;
    if (home && home[0]) {
        char pre_path[4096];
        const char *pre_dirs[] = {
            ".cache",
            ".cache/wwal",
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
            ".local/share/wallpapers",
            ".config",
            ".config/wwal",
            "Pictures",
            "Videos",
            "Wallpapers",
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
    const char *rx_paths[] = {"/usr", "/bin", "/sbin", "/lib", "/lib64", "/nix", "/opt", NULL};
    for (int i = 0; rx_paths[i] != NULL; ++i) {
        add_landlock_dir_rule(ruleset_fd, rx_paths[i], rx_access);
    }

    /* Allow read-only across standard system and external media mounts required for DRM/Mesa/VA-API
     * drivers and wallpaper repositories */
    const char *ro_paths[] = {"/etc",       "/sys",      "/proc", "/mnt", "/media",
                              "/run/media", "/var/home", "/nix",  "/opt", NULL};
    for (int i = 0; ro_paths[i] != NULL; ++i) {
        add_landlock_dir_rule(ruleset_fd, ro_paths[i], ro_access);
    }

    /* Allow read-write strictly for DRI/GPU device nodes (/dev, /dev/dri) and /tmp */
    const char *rw_paths[] = {"/dev", "/dev/dri", "/tmp", NULL};
    for (int i = 0; rw_paths[i] != NULL; ++i) {
        if (strcmp(rw_paths[i], "/tmp") == 0 && home &&
            strncmp(home, "/tmp/wwal_sbox_test_", 20) == 0) {
            continue;
        }
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
        add_landlock_share_root_excluding_keyrings(ruleset_fd, xdg_data, rx_access);
    }

    const char *xdg_pics = getenv("XDG_PICTURES_DIR");
    if (xdg_pics && xdg_pics[0]) {
        add_landlock_dir_rule(ruleset_fd, xdg_pics, ro_access);
    }
    const char *xdg_vids = getenv("XDG_VIDEOS_DIR");
    if (xdg_vids && xdg_vids[0]) {
        add_landlock_dir_rule(ruleset_fd, xdg_vids, ro_access);
    }

    const char *wwal_ws = getenv("WWAL_WORKSHOP_DIR");
    if (wwal_ws && wwal_ws[0]) {
        add_landlock_dir_rule(ruleset_fd, wwal_ws, ro_access);
    }

    const char *wwal_preset_dir = getenv("WWAL_SHADER_PRESET_DIR");
    if (wwal_preset_dir && wwal_preset_dir[0]) {
        add_landlock_dir_rule(ruleset_fd, wwal_preset_dir, ro_access);
    }

    const char *wwal_shader_dir = getenv("WWAL_SHADER_DIR");
    if (wwal_shader_dir && wwal_shader_dir[0]) {
        add_landlock_dir_rule(ruleset_fd, wwal_shader_dir, ro_access);
    }

#ifdef WWAL_DATADIR
    add_landlock_dir_rule(ruleset_fd, WWAL_DATADIR, ro_access);
#endif
#ifdef WWAL_SOURCE_ROOT
    add_landlock_dir_rule(ruleset_fd, WWAL_SOURCE_ROOT "/shaders", ro_access);
    add_landlock_dir_rule(ruleset_fd, WWAL_SOURCE_ROOT "/contrib", ro_access);
    add_landlock_dir_rule(ruleset_fd, WWAL_SOURCE_ROOT "/tests", ro_access);
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
                if (snprintf(rel_dir, sizeof(rel_dir), "%s/../shaders", exe_buf) <
                    (int)sizeof(rel_dir)) {
                    add_landlock_dir_rule(ruleset_fd, rel_dir, ro_access);
                }
                if (snprintf(rel_dir, sizeof(rel_dir), "%s/../share/wwal/shaders", exe_buf) <
                    (int)sizeof(rel_dir)) {
                    add_landlock_dir_rule(ruleset_fd, rel_dir, ro_access);
                }
            }
        }
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
        const char *cache_subdirs[] = {"wwal", "wal", "swww", "awww"};
        for (size_t i = 0; i < sizeof(cache_subdirs) / sizeof(cache_subdirs[0]); ++i) {
            char cache_sub[4096];
            snprintf(cache_sub, sizeof(cache_sub), "%s/%s", xdg_cache, cache_subdirs[i]);
            mkdir_p_user_dir(cache_sub);
            add_landlock_dir_rule(ruleset_fd, cache_sub, rw_access);
        }
    }

    const char *xdg_state = getenv("XDG_STATE_HOME");
    if (xdg_state && xdg_state[0]) {
        mkdir_p_user_dir(xdg_state);
        const char *state_subdirs[] = {"wwal", "caelestia", "caelestia/wallpaper"};
        for (size_t i = 0; i < sizeof(state_subdirs) / sizeof(state_subdirs[0]); ++i) {
            char state_sub[4096];
            snprintf(state_sub, sizeof(state_sub), "%s/%s", xdg_state, state_subdirs[i]);
            mkdir_p_user_dir(state_sub);
            add_landlock_dir_rule(ruleset_fd, state_sub, rw_access);
        }
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
                if (is_sensitive_home_entry(ent->d_name) || strcmp(ent->d_name, ".local") == 0)
                    continue;
                int n = snprintf(path_buf, sizeof(path_buf), "%s/%s", home, ent->d_name);
                if (n > 0 && (size_t)n < sizeof(path_buf)) {
                    __u64 ent_access = (strcmp(ent->d_name, "bin") == 0 ||
                                        strcmp(ent->d_name, ".cargo") == 0)
                                           ? rx_access
                                           : ro_access;
                    add_landlock_dir_rule(ruleset_fd, path_buf, ent_access);
                }
            }
            closedir(hdir);
        }

        /* Enumerate ~/.local/share entries read+execute while excluding ~/.local/share/keyrings */
        char local_share[4096];
        if (snprintf(local_share, sizeof(local_share), "%s/.local/share", home) <
            (int)sizeof(local_share)) {
            add_landlock_share_root_excluding_keyrings(ruleset_fd, local_share, rx_access);
        }

        const char *home_rx_subdirs[] = {
            ".local/bin",
            "bin",
            ".cargo/bin",
        };
        for (size_t i = 0; i < sizeof(home_rx_subdirs) / sizeof(home_rx_subdirs[0]); ++i) {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", home, home_rx_subdirs[i]);
            add_landlock_dir_rule(ruleset_fd, path_buf, rx_access);
        }

        const char *home_ro_subdirs[] = {
            "Pictures",
            "Videos",
            "Wallpapers",
            "Downloads",
            ".config",
            ".local/share/wallpapers",
            ".local/share/Steam",
        };
        for (size_t i = 0; i < sizeof(home_ro_subdirs) / sizeof(home_ro_subdirs[0]); ++i) {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", home, home_ro_subdirs[i]);
            add_landlock_dir_rule(ruleset_fd, path_buf, ro_access);
        }

        const char *home_rw_subdirs[] = {
            ".cache",
            ".cache/wwal",
            ".cache/wal",
            ".cache/swww",
            ".cache/awww",
            ".local/state/wwal",
            ".local/state/caelestia",
            ".local/state/caelestia/wallpaper",
            ".config",
            ".config/wwal",
        };
        for (size_t i = 0; i < sizeof(home_rw_subdirs) / sizeof(home_rw_subdirs[0]); ++i) {
            snprintf(path_buf, sizeof(path_buf), "%s/%s", home, home_rw_subdirs[i]);
            add_landlock_dir_rule(ruleset_fd, path_buf, rw_access);
        }
    }

    if (g_sandbox_cfg) {
        char real_home[4096] = {0};
        if (home && home[0]) {
            (void)realpath(home, real_home);
        }
        for (size_t i = 0; i < g_sandbox_cfg->template_count; ++i) {
            const waywal_template_rule_t *tr = &g_sandbox_cfg->templates[i];
            if (!tr->enabled)
                continue;
            if (tr->output_path[0] != '\0') {
                char out_norm[4096];
                snprintf(out_norm, sizeof(out_norm), "%s", tr->output_path);
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
                            add_landlock_dir_rule(ruleset_fd, pb.data, rw_access);
                        }
                    }
                }
            }
            if (tr->input_path[0] != '\0') {
                char in_norm[4096];
                snprintf(in_norm, sizeof(in_norm), "%s", tr->input_path);
                waywal_normalize_path(in_norm);
                if (!is_sensitive_target_path(in_norm)) {
                    path_buf_t pb;
                    path_buf_from_str(&pb, in_norm);
                    if (path_buf_parent(&pb) && pb.len > 0) {
                        waywal_normalize_path(pb.data);
                        if (!is_sensitive_ancestor(pb.data)) {
                            add_landlock_dir_rule(ruleset_fd, pb.data, ro_access);
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
        for (size_t i = 0;
             i < g_sandbox_cfg->workspace_count && i < WAYWAL_CONFIG_MAX_WORKSPACES; ++i) {
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

        for (size_t i = 0; i < ro_cfg_count; ++i) {
            char p_norm[4096];
            snprintf(p_norm, sizeof(p_norm), "%s", ro_cfg_paths[i]);
            waywal_normalize_path(p_norm);
            if (!is_sensitive_target_path(p_norm)) {
                path_buf_t pb;
                path_buf_from_str(&pb, p_norm);
                if (path_buf_parent(&pb) && pb.len > 0) {
                    waywal_normalize_path(pb.data);
                    if (!((home && strcmp(pb.data, home) == 0) ||
                          (real_home[0] && strcmp(pb.data, real_home) == 0)) &&
                        !is_sensitive_ancestor(pb.data)) {
                        add_landlock_dir_rule(ruleset_fd, pb.data, ro_access);
                    }
                }
            }
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
            SCMP_SYS(ptrace),        SCMP_SYS(mount),      SCMP_SYS(umount2),     SCMP_SYS(chroot),
            SCMP_SYS(pivot_root),    SCMP_SYS(reboot),     SCMP_SYS(kexec_load),  SCMP_SYS(init_module),
            SCMP_SYS(delete_module), SCMP_SYS(bpf),
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
