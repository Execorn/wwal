// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "waywal/path.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void path_buf_init(path_buf_t *pb)
{
    if (!pb)
        return;
    pb->len = 0;
    pb->data[0] = '\0';
}

void path_buf_from_str(path_buf_t *pb, const char *str)
{
    if (!pb)
        return;
    pb->len = 0;
    pb->data[0] = '\0';

    if (!str)
        return;

    size_t slen = strlen(str);
    if (slen >= WAYWAL_PATH_MAX) {
        slen = WAYWAL_PATH_MAX - 1;
    }

    memcpy(pb->data, str, slen);
    pb->len = slen;
    pb->data[pb->len] = '\0';
}

bool path_buf_push(path_buf_t *pb, const char *component)
{
    if (!pb)
        return false;
    if (!component || component[0] == '\0')
        return true;

    size_t comp_len = strlen(component);
    if (comp_len == 0)
        return true;

    bool need_sep = (pb->len > 0 && pb->data[pb->len - 1] != '/' && component[0] != '/');
    size_t needed = pb->len + comp_len + (need_sep ? 1 : 0) + 1;
    if (needed > WAYWAL_PATH_MAX) {
        return false;
    }

    if (need_sep) {
        pb->data[pb->len++] = '/';
    }

    memcpy(&pb->data[pb->len], component, comp_len);
    pb->len += comp_len;
    pb->data[pb->len] = '\0';
    return true;
}

bool path_buf_append(path_buf_t *pb, const char *suffix)
{
    if (!pb)
        return false;
    if (!suffix || suffix[0] == '\0')
        return true;

    size_t suf_len = strlen(suffix);
    if (pb->len + suf_len + 1 > WAYWAL_PATH_MAX) {
        return false;
    }

    memcpy(&pb->data[pb->len], suffix, suf_len);
    pb->len += suf_len;
    pb->data[pb->len] = '\0';
    return true;
}

bool path_buf_parent(path_buf_t *pb)
{
    if (!pb || pb->len == 0)
        return false;

    /* Trim trailing slashes if len > 1 */
    while (pb->len > 1 && pb->data[pb->len - 1] == '/') {
        pb->data[--pb->len] = '\0';
    }

    if (pb->len == 1 && pb->data[0] == '/') {
        return false; /* Root "/" has no parent */
    }

    /* Search backwards for previous slash */
    for (size_t i = pb->len; i > 0; --i) {
        if (pb->data[i - 1] == '/') {
            if (i - 1 == 0) {
                /* Parent is root "/" */
                pb->len = 1;
                pb->data[1] = '\0';
            } else {
                pb->len = i - 1;
                pb->data[pb->len] = '\0';
            }
            return true;
        }
    }

    /* No slash found: relative single-component path */
    pb->len = 0;
    pb->data[0] = '\0';
    return true;
}

const char *path_buf_cstr(const path_buf_t *pb)
{
    if (!pb)
        return "";
    return pb->data;
}

void waywal_normalize_path(char *path)
{
    if (!path || path[0] != '/')
        return;

    char *dst = path;
    size_t out_len = 1;
    const char *p = path + 1;

    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        const char *seg_start = p;
        while (*p != '\0' && *p != '/')
            p++;
        size_t seg_len = (size_t)(p - seg_start);

        if (seg_len == 1 && seg_start[0] == '.')
            continue;

        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            if (out_len > 1) {
                while (out_len > 1 && dst[out_len - 1] != '/')
                    out_len--;
                if (out_len > 1 && dst[out_len - 1] == '/')
                    out_len--;
            }
            continue;
        }

        if (out_len > 1)
            dst[out_len++] = '/';
        memmove(&dst[out_len], seg_start, seg_len);
        out_len += seg_len;
    }

    dst[out_len] = '\0';
}

static bool resolve_home_dir(char *buf, size_t cap)
{
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        int n = snprintf(buf, cap, "%s", home);
        return (n >= 0 && (size_t)n < cap);
    }

    struct passwd pw;
    struct passwd *pw_res = NULL;
    char pw_buf[4096];
    if (getpwuid_r(getuid(), &pw, pw_buf, sizeof(pw_buf), &pw_res) == 0 && pw_res &&
        pw_res->pw_dir && pw_res->pw_dir[0] != '\0') {
        int n = snprintf(buf, cap, "%s", pw_res->pw_dir);
        return (n >= 0 && (size_t)n < cap);
    }
    return false;
}

static bool resolve_env_or_xdg(const char *var_name, char *val_buf, size_t val_cap)
{
    const char *env = getenv(var_name);
    if (env && env[0] != '\0') {
        int n = snprintf(val_buf, val_cap, "%s", env);
        return (n >= 0 && (size_t)n < val_cap);
    }

    if (strcmp(var_name, "XDG_RUNTIME_DIR") == 0) {
        int n = snprintf(val_buf, val_cap, "/run/user/%u", (unsigned int)getuid());
        return (n >= 0 && (size_t)n < val_cap);
    }

    char home[WAYWAL_PATH_MAX];
    if (!resolve_home_dir(home, sizeof(home)))
        return false;

    if (strcmp(var_name, "HOME") == 0) {
        int n = snprintf(val_buf, val_cap, "%s", home);
        return (n >= 0 && (size_t)n < val_cap);
    }
    if (strcmp(var_name, "XDG_CONFIG_HOME") == 0) {
        int n = snprintf(val_buf, val_cap, "%s/.config", home);
        return (n >= 0 && (size_t)n < val_cap);
    }
    if (strcmp(var_name, "XDG_CACHE_HOME") == 0) {
        int n = snprintf(val_buf, val_cap, "%s/.cache", home);
        return (n >= 0 && (size_t)n < val_cap);
    }
    if (strcmp(var_name, "XDG_STATE_HOME") == 0) {
        int n = snprintf(val_buf, val_cap, "%s/.local/state", home);
        return (n >= 0 && (size_t)n < val_cap);
    }
    if (strcmp(var_name, "XDG_DATA_HOME") == 0) {
        int n = snprintf(val_buf, val_cap, "%s/.local/share", home);
        return (n >= 0 && (size_t)n < val_cap);
    }
    return false;
}

bool waywal_expand_path(const char *raw_path, char *out_buf, size_t out_cap)
{
    if (!raw_path || !out_buf || out_cap == 0) {
        if (out_buf && out_cap > 0)
            out_buf[0] = '\0';
        return false;
    }

    char tmp[WAYWAL_PATH_MAX];
    size_t pos = 0;
    const char *p = raw_path;

    if (p[0] == '~') {
        if (p[1] != '/' && p[1] != '\0') {
            out_buf[0] = '\0';
            return false;
        }
        char home[WAYWAL_PATH_MAX];
        if (!resolve_home_dir(home, sizeof(home))) {
            out_buf[0] = '\0';
            return false;
        }
        size_t hlen = strlen(home);
        if (hlen >= sizeof(tmp)) {
            out_buf[0] = '\0';
            return false;
        }
        memcpy(tmp, home, hlen);
        pos = hlen;
        p += 1;
    }

    while (*p != '\0') {
        if (*p == '$') {
            const char *var_start = p + 1;
            const char *var_end = var_start;
            const char *next_p = NULL;

            if (*var_start == '{') {
                var_start++;
                if (!(isalpha((unsigned char)*var_start) || *var_start == '_')) {
                    out_buf[0] = '\0';
                    return false;
                }
                var_end = var_start + 1;
                while (isalnum((unsigned char)*var_end) || *var_end == '_')
                    var_end++;
                if (*var_end != '}') {
                    out_buf[0] = '\0';
                    return false;
                }
                next_p = var_end + 1;
            } else if (isalpha((unsigned char)*var_start) || *var_start == '_') {
                var_end = var_start + 1;
                while (isalnum((unsigned char)*var_end) || *var_end == '_')
                    var_end++;
                next_p = var_end;
            } else {
                out_buf[0] = '\0';
                return false;
            }

            if (!next_p || var_end <= var_start || (size_t)(var_end - var_start) >= 128) {
                out_buf[0] = '\0';
                return false;
            }

            char var_name[128];
            size_t vlen = (size_t)(var_end - var_start);
            memcpy(var_name, var_start, vlen);
            var_name[vlen] = '\0';

            char resolved[4096];
            if (!resolve_env_or_xdg(var_name, resolved, sizeof(resolved))) {
                out_buf[0] = '\0';
                return false;
            }

            size_t rlen = strlen(resolved);
            if (pos + rlen >= sizeof(tmp)) {
                out_buf[0] = '\0';
                return false;
            }
            memcpy(&tmp[pos], resolved, rlen);
            pos += rlen;
            p = next_p;
            continue;
        }

        if (pos + 1 >= sizeof(tmp)) {
            out_buf[0] = '\0';
            return false;
        }
        tmp[pos++] = *p++;
    }
    tmp[pos] = '\0';

    if (tmp[0] == '/') {
        waywal_normalize_path(tmp);
        pos = strlen(tmp);
    }

    if (pos + 1 > out_cap) {
        out_buf[0] = '\0';
        return false;
    }

    memcpy(out_buf, tmp, pos + 1);
    return true;
}

typedef struct {
    const char *name;
    const char *filename;
    const char *title;
    const char *description;
    bool audio_reactive;
} builtin_shader_preset_def_t;

static const builtin_shader_preset_def_t k_builtin_shader_presets[] = {
    {
        "aurora",
        "aurora.frag",
        "Aurora Borealis",
        "Northern Lights curtains over an arctic night sky with stars and audio reactivity",
        false,
    },
    {
        "cyber_grid",
        "cyber_grid.frag",
        "Cyber Grid",
        "Outrun perspective neon grid with wireframe terrain, sun, and audio reactivity",
        false,
    },
    {
        "fluid_ink",
        "fluid_ink.frag",
        "Fluid Ink",
        "Domain-warped FBM liquid marble ink currents with mouse ripple and audio reactivity",
        false,
    },
    {
        "synthwave_sun",
        "synthwave_sun.frag",
        "Synthwave Sun",
        "Segmented retrowave sunset with ocean reflections and palm silhouettes",
        false,
    },
    {
        "starfield",
        "starfield.frag",
        "Starfield",
        "Multi-layer 3D parallax starfield with cosmic nebula clouds and warp speed",
        false,
    },
    {
        "rain_glass",
        "rain_glass.frag",
        "Rain on Glass",
        "Procedural refractive raindrops and condensation trails sliding down bokeh glass",
        false,
    },
    {
        "audio_bars",
        "audio_bars.frag",
        "Audio Bars",
        "64-band log-frequency Bark FFT spectrum analyzer with peak hold and floor reflection",
        true,
    },
    {
        "audio_ring",
        "audio_ring.frag",
        "Audio Ring",
        "Polar circular audio spectrum with reactive bass core and expanding shockwave rings",
        true,
    },
    {
        "crt_retro",
        "crt_retro.frag",
        "CRT Retro",
        "Barrel-curved CRT phosphor terminal with radar scope, scanlines, and RGB aberration",
        false,
    },
    {
        "plasma_orb",
        "plasma_orb.frag",
        "Plasma Orb",
        "Raymarched volumetric energy orb with internal plasma filaments and mouse tracking",
        false,
    },
};

static bool try_stat_regular_shader(const char *candidate, char *out_buf, size_t out_cap)
{
    if (!candidate || candidate[0] == '\0')
        return false;
    struct stat st;
    if (stat(candidate, &st) != 0 || !S_ISREG(st.st_mode) || access(candidate, R_OK) != 0)
        return false;

    char real_buf[PATH_MAX];
    const char *src = realpath(candidate, real_buf) ? real_buf : candidate;
    size_t slen = strlen(src);
    if (slen + 1 > out_cap)
        return false;
    memcpy(out_buf, src, slen + 1);
    return true;
}

static bool probe_preset_stem(const char *stem, char *out_buf, size_t out_cap)
{
    char cand[WAYWAL_PATH_MAX];

    const char *env_preset_dir = getenv("WWAL_SHADER_PRESET_DIR");
    if (env_preset_dir && env_preset_dir[0] != '\0') {
        char exp_dir[WAYWAL_PATH_MAX];
        const char *dir =
            waywal_expand_path(env_preset_dir, exp_dir, sizeof(exp_dir)) ? exp_dir : env_preset_dir;
        snprintf(cand, sizeof(cand), "%s/%s.frag", dir, stem);
        if (try_stat_regular_shader(cand, out_buf, out_cap))
            return true;
    }

    const char *env_shader_dir = getenv("WWAL_SHADER_DIR");
    if (env_shader_dir && env_shader_dir[0] != '\0') {
        char exp_dir[WAYWAL_PATH_MAX];
        const char *dir =
            waywal_expand_path(env_shader_dir, exp_dir, sizeof(exp_dir)) ? exp_dir : env_shader_dir;
        snprintf(cand, sizeof(cand), "%s/presets/%s.frag", dir, stem);
        if (try_stat_regular_shader(cand, out_buf, out_cap))
            return true;
        snprintf(cand, sizeof(cand), "%s/%s.frag", dir, stem);
        if (try_stat_regular_shader(cand, out_buf, out_cap))
            return true;
    }

    char xdg_cfg[WAYWAL_PATH_MAX];
    if (resolve_env_or_xdg("XDG_CONFIG_HOME", xdg_cfg, sizeof(xdg_cfg))) {
        snprintf(cand, sizeof(cand), "%s/wwal/shaders/presets/%s.frag", xdg_cfg, stem);
        if (try_stat_regular_shader(cand, out_buf, out_cap))
            return true;
        snprintf(cand, sizeof(cand), "%s/wwal/shaders/%s.frag", xdg_cfg, stem);
        if (try_stat_regular_shader(cand, out_buf, out_cap))
            return true;
    }

    char home[WAYWAL_PATH_MAX];
    if (resolve_home_dir(home, sizeof(home))) {
        snprintf(cand, sizeof(cand), "%s/.config/wwal/shaders/presets/%s.frag", home, stem);
        if (try_stat_regular_shader(cand, out_buf, out_cap))
            return true;
        snprintf(cand, sizeof(cand), "%s/.config/wwal/shaders/%s.frag", home, stem);
        if (try_stat_regular_shader(cand, out_buf, out_cap))
            return true;
    }

    char xdg_data[WAYWAL_PATH_MAX];
    if (resolve_env_or_xdg("XDG_DATA_HOME", xdg_data, sizeof(xdg_data))) {
        snprintf(cand, sizeof(cand), "%s/wwal/shaders/presets/%s.frag", xdg_data, stem);
        if (try_stat_regular_shader(cand, out_buf, out_cap))
            return true;
        snprintf(cand, sizeof(cand), "%s/wwal/shaders/%s.frag", xdg_data, stem);
        if (try_stat_regular_shader(cand, out_buf, out_cap))
            return true;
    }

    snprintf(cand, sizeof(cand), "shaders/presets/%s.frag", stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;

    snprintf(cand, sizeof(cand), "../shaders/presets/%s.frag", stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;

    char exe_path[PATH_MAX];
    ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (exe_len > 0) {
        exe_path[exe_len] = '\0';
        char *slash = strrchr(exe_path, '/');
        if (slash) {
            *slash = '\0';
            snprintf(cand, sizeof(cand), "%s/../share/wwal/shaders/presets/%s.frag", exe_path,
                     stem);
            if (try_stat_regular_shader(cand, out_buf, out_cap))
                return true;
            snprintf(cand, sizeof(cand), "%s/../shaders/presets/%s.frag", exe_path, stem);
            if (try_stat_regular_shader(cand, out_buf, out_cap))
                return true;
            snprintf(cand, sizeof(cand), "%s/../../shaders/presets/%s.frag", exe_path, stem);
            if (try_stat_regular_shader(cand, out_buf, out_cap))
                return true;
            snprintf(cand, sizeof(cand), "%s/shaders/presets/%s.frag", exe_path, stem);
            if (try_stat_regular_shader(cand, out_buf, out_cap))
                return true;
        }
    }

#ifdef WWAL_DATADIR
    snprintf(cand, sizeof(cand), "%s/wwal/shaders/presets/%s.frag", WWAL_DATADIR, stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;
    snprintf(cand, sizeof(cand), "%s/waywal/shaders/presets/%s.frag", WWAL_DATADIR, stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;
    snprintf(cand, sizeof(cand), "%s/shaders/presets/%s.frag", WWAL_DATADIR, stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;
#endif

#ifdef WWAL_SOURCE_ROOT
    snprintf(cand, sizeof(cand), "%s/shaders/presets/%s.frag", WWAL_SOURCE_ROOT, stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;
#endif

    snprintf(cand, sizeof(cand), "/usr/local/share/wwal/shaders/presets/%s.frag", stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;

    snprintf(cand, sizeof(cand), "/usr/share/wwal/shaders/presets/%s.frag", stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;

    snprintf(cand, sizeof(cand), "/usr/share/wwal/shaders/%s.frag", stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;

    snprintf(cand, sizeof(cand), "contrib/shaders/%s.frag", stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;

    snprintf(cand, sizeof(cand), "../contrib/shaders/%s.frag", stem);
    if (try_stat_regular_shader(cand, out_buf, out_cap))
        return true;

    return false;
}

static bool has_shader_file_extension(const char *path)
{
    if (!path)
        return false;
    const char *dot = strrchr(path, '.');
    if (!dot)
        return false;
    return strcasecmp(dot, ".frag") == 0 || strcasecmp(dot, ".glsl") == 0 ||
           strcasecmp(dot, ".comp") == 0 || strcasecmp(dot, ".shader") == 0;
}

bool waywal_shader_resolve_path(const char *name_or_path, char *out_buf, size_t out_cap)
{
    if (!name_or_path || name_or_path[0] == '\0' || !out_buf || out_cap == 0) {
        if (out_buf && out_cap > 0)
            out_buf[0] = '\0';
        return false;
    }

    bool explicit_preset = (strncasecmp(name_or_path, "preset:", 7) == 0);
    if (!explicit_preset) {
        bool looks_like_file_path =
            (strchr(name_or_path, '/') != NULL || strchr(name_or_path, '\\') != NULL ||
             name_or_path[0] == '~' || name_or_path[0] == '$' ||
             has_shader_file_extension(name_or_path));
        if (looks_like_file_path) {
            char expanded[WAYWAL_PATH_MAX];
            const char *direct = waywal_expand_path(name_or_path, expanded, sizeof(expanded))
                                     ? expanded
                                     : name_or_path;
            if (try_stat_regular_shader(direct, out_buf, out_cap))
                return true;
        }
    }

    const char *p = explicit_preset ? (name_or_path + 7) : name_or_path;

    if (p[0] == '\0' || strchr(p, '/') != NULL || strchr(p, '\\') != NULL ||
        strstr(p, "..") != NULL) {
        out_buf[0] = '\0';
        return false;
    }

    char raw_stem[128];
    size_t plen = strlen(p);
    if (plen >= sizeof(raw_stem)) {
        out_buf[0] = '\0';
        return false;
    }
    memcpy(raw_stem, p, plen + 1);

    char *dot = strrchr(raw_stem, '.');
    if (dot && (strcasecmp(dot, ".frag") == 0 || strcasecmp(dot, ".glsl") == 0 ||
                strcasecmp(dot, ".shader") == 0)) {
        *dot = '\0';
    }

    if (raw_stem[0] == '\0') {
        out_buf[0] = '\0';
        return false;
    }

    char norm_stem[128];
    for (size_t i = 0;; ++i) {
        unsigned char c = (unsigned char)raw_stem[i];
        if (c == '\0') {
            norm_stem[i] = '\0';
            break;
        }
        if (c == '-') {
            norm_stem[i] = '_';
        } else if (isalnum(c) || c == '_') {
            norm_stem[i] = (char)tolower(c);
        } else {
            out_buf[0] = '\0';
            return false;
        }
    }

    /* Check exact stem first (in case a user created a custom override), then canonical aliases */
    if (probe_preset_stem(norm_stem, out_buf, out_cap))
        return true;

    if (strcmp(raw_stem, norm_stem) != 0 && probe_preset_stem(raw_stem, out_buf, out_cap))
        return true;

    const char *alias_target = NULL;
    if (strcmp(norm_stem, "lava_lamp") == 0) {
        alias_target = "plasma_orb";
    } else if (strcmp(norm_stem, "northern_lights") == 0) {
        alias_target = "aurora";
    } else if (strcmp(norm_stem, "outrun") == 0) {
        alias_target = "synthwave_sun";
    } else if (strcmp(norm_stem, "crt") == 0) {
        alias_target = "crt_retro";
    }

    if (alias_target && probe_preset_stem(alias_target, out_buf, out_cap))
        return true;

    if (!explicit_preset) {
        char expanded[WAYWAL_PATH_MAX];
        const char *direct =
            waywal_expand_path(name_or_path, expanded, sizeof(expanded)) ? expanded : name_or_path;
        if (try_stat_regular_shader(direct, out_buf, out_cap))
            return true;
    }

    out_buf[0] = '\0';
    return false;
}

static void trim_metadata_val(const char *src, size_t len, char *dst, size_t dst_cap)
{
    if (!dst || dst_cap == 0)
        return;
    while (len > 0 && (*src == ' ' || *src == '\t' || *src == ':')) {
        src++;
        len--;
    }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' || src[len - 1] == '\r' ||
                       src[len - 1] == '\n')) {
        len--;
    }
    if (len >= dst_cap)
        len = dst_cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void parse_preset_file_metadata(waywal_shader_preset_info_t *dst)
{
    if (!dst || dst->path[0] == '\0')
        return;

    FILE *f = fopen(dst->path, "rb");
    if (!f)
        return;

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *line = buf;
    bool in_block_comment = false;
    while (*line != '\0') {
        const char *nl = strchr(line, '\n');
        if (!nl && n == sizeof(buf) - 1) {
            break;
        }
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        const char *s = line;
        size_t slen = len;
        while (slen > 0 && (*s == ' ' || *s == '\t')) {
            s++;
            slen--;
        }
        while (slen > 0 && (s[slen - 1] == ' ' || s[slen - 1] == '\t' || s[slen - 1] == '\r')) {
            slen--;
        }

        while (slen > 0) {
            if (in_block_comment) {
                const char *end_bc = (const char *)memmem(s, slen, "*/", 2);
                if (!end_bc) {
                    slen = 0;
                    break;
                }
                in_block_comment = false;
                size_t adv = (size_t)((end_bc + 2) - s);
                s += adv;
                slen -= adv;
                while (slen > 0 && (*s == ' ' || *s == '\t')) {
                    s++;
                    slen--;
                }
            } else if (slen >= 2 && s[0] == '/' && s[1] == '*') {
                in_block_comment = true;
                s += 2;
                slen -= 2;
            } else {
                break;
            }
        }

        if (slen == 0) {
            if (!nl)
                break;
            line = nl + 1;
            continue;
        }

        bool is_comment = (slen >= 2 && s[0] == '/' && s[1] == '/');
        bool is_version = (slen >= 8 && strncasecmp(s, "#version", 8) == 0);
        bool is_pragma = (slen >= 7 && strncasecmp(s, "#pragma", 7) == 0);

        if (!is_comment && !is_version && !is_pragma) {
            break;
        }

        if (is_comment) {
            s += 2;
            slen -= 2;
            while (slen > 0 && (*s == ' ' || *s == '\t')) {
                s++;
                slen--;
            }
            if (slen >= 7 && strncasecmp(s, "#pragma", 7) == 0) {
                is_pragma = true;
            }
        }

        if (is_pragma) {
            if (slen >= 14 && strncasecmp(s, "#pragma waywal", 14) == 0) {
                const char *ps = s + 14;
                size_t pslen = slen - 14;
                while (pslen > 0 && (*ps == ' ' || *ps == '\t')) {
                    ps++;
                    pslen--;
                }
                if (pslen >= 5 && strncasecmp(ps, "audio", 5) == 0 &&
                    (pslen == 5 || ps[5] == ' ' || ps[5] == '\t' || ps[5] == ':' ||
                     ps[5] == '=')) {
                    if (pslen == 5) {
                        dst->audio_reactive = true;
                    } else {
                        char val[32];
                        const char *vs = ps + 5;
                        size_t vslen = pslen - 5;
                        while (vslen > 0 &&
                               (*vs == ' ' || *vs == '\t' || *vs == ':' || *vs == '=')) {
                            vs++;
                            vslen--;
                        }
                        trim_metadata_val(vs, vslen, val, sizeof(val));
                        if (val[0] == '\0' || strcasecmp(val, "true") == 0 ||
                            strcmp(val, "1") == 0 || strcasecmp(val, "yes") == 0) {
                            dst->audio_reactive = true;
                        } else if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0 ||
                                   strcasecmp(val, "no") == 0) {
                            dst->audio_reactive = false;
                        }
                    }
                }
            }
        } else if (is_comment) {
            if (slen > 5 && strncasecmp(s, "@name", 5) == 0 &&
                (s[5] == ':' || s[5] == ' ' || s[5] == '\t')) {
                char val[sizeof(dst->title)];
                trim_metadata_val(s + 5, slen - 5, val, sizeof(val));
                if (val[0] != '\0') {
                    snprintf(dst->title, sizeof(dst->title), "%s", val);
                }
            } else if (slen > 12 && strncasecmp(s, "@description", 12) == 0 &&
                       (s[12] == ':' || s[12] == ' ' || s[12] == '\t')) {
                char val[sizeof(dst->description)];
                trim_metadata_val(s + 12, slen - 12, val, sizeof(val));
                if (val[0] != '\0') {
                    snprintf(dst->description, sizeof(dst->description), "%s", val);
                }
            } else if (slen > 7 && strncasecmp(s, "@author", 7) == 0 &&
                       (s[7] == ':' || s[7] == ' ' || s[7] == '\t')) {
                char val[sizeof(dst->author)];
                trim_metadata_val(s + 7, slen - 7, val, sizeof(val));
                if (val[0] != '\0') {
                    snprintf(dst->author, sizeof(dst->author), "%s", val);
                }
            } else if (slen > 15 && strncasecmp(s, "@audio_reactive", 15) == 0 &&
                       (s[15] == ':' || s[15] == ' ' || s[15] == '\t')) {
                char val[32];
                trim_metadata_val(s + 15, slen - 15, val, sizeof(val));
                if (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
                    strcasecmp(val, "yes") == 0) {
                    dst->audio_reactive = true;
                } else if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0 ||
                           strcasecmp(val, "no") == 0) {
                    dst->audio_reactive = false;
                }
            } else if (slen >= 6 && strncasecmp(s, "@audio", 6) == 0 &&
                       (slen == 6 || s[6] == ':' || s[6] == ' ' || s[6] == '\t')) {
                char val[32];
                trim_metadata_val(s + 6, slen - 6, val, sizeof(val));
                if (val[0] == '\0' || strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
                    strcasecmp(val, "yes") == 0) {
                    dst->audio_reactive = true;
                } else if (strcasecmp(val, "false") == 0 || strcmp(val, "0") == 0 ||
                           strcasecmp(val, "no") == 0) {
                    dst->audio_reactive = false;
                }
            }
        }
        if (!nl)
            break;
        line = nl + 1;
    }
}

bool waywal_shader_is_audio_reactive(const char *name_or_path)
{
    if (!name_or_path || name_or_path[0] == '\0')
        return false;

    const char *p = (strncasecmp(name_or_path, "preset:", 7) == 0) ? (name_or_path + 7)
                                                                   : name_or_path;
    const char *slash = strrchr(p, '/');
    const char *bslash = strrchr(p, '\\');
    const char *base = slash ? (slash + 1) : p;
    if (bslash && bslash + 1 > base)
        base = bslash + 1;

    bool default_audio = (strncasecmp(base, "audio_", 6) == 0 ||
                          strncasecmp(base, "audio-", 6) == 0);

    char stem[128];
    size_t blen = strlen(base);
    if (blen > 0 && blen < sizeof(stem)) {
        memcpy(stem, base, blen + 1);
        char *dot = strrchr(stem, '.');
        if (dot && has_shader_file_extension(stem)) {
            *dot = '\0';
        }
        for (size_t i = 0; stem[i] != '\0'; ++i) {
            if (stem[i] == '-')
                stem[i] = '_';
            else
                stem[i] = (char)tolower((unsigned char)stem[i]);
        }
        const size_t num_builtins =
            sizeof(k_builtin_shader_presets) / sizeof(k_builtin_shader_presets[0]);
        for (size_t i = 0; i < num_builtins; ++i) {
            if (strcmp(k_builtin_shader_presets[i].name, stem) == 0) {
                default_audio = k_builtin_shader_presets[i].audio_reactive;
                break;
            }
        }
    }

    char resolved[WAYWAL_PATH_MAX];
    resolved[0] = '\0';
    if (!waywal_shader_resolve_path(name_or_path, resolved, sizeof(resolved)) &&
        strncasecmp(name_or_path, "preset:", 7) != 0) {
        char expanded[WAYWAL_PATH_MAX];
        const char *direct =
            waywal_expand_path(name_or_path, expanded, sizeof(expanded)) ? expanded : name_or_path;
        (void)try_stat_regular_shader(direct, resolved, sizeof(resolved));
    }

    if (resolved[0] != '\0') {
        const char *rbase = strrchr(resolved, '/');
        rbase = rbase ? (rbase + 1) : resolved;
        if (strncasecmp(rbase, "audio_", 6) == 0 || strncasecmp(rbase, "audio-", 6) == 0) {
            default_audio = true;
        }
        waywal_shader_preset_info_t info;
        memset(&info, 0, sizeof(info));
        snprintf(info.path, sizeof(info.path), "%s", resolved);
        info.audio_reactive = default_audio;
        parse_preset_file_metadata(&info);
        return info.audio_reactive;
    }

    return default_audio;
}

typedef struct {
    char (*names)[64];
    size_t count;
    size_t cap;
} preset_overflow_seen_t;

static void scan_custom_preset_dir(const char *dir_path, waywal_shader_preset_info_t *out_list,
                                   size_t max_entries, size_t *io_count,
                                   preset_overflow_seen_t *overflow)
{
    if (!dir_path || dir_path[0] == '\0')
        return;

    DIR *d = opendir(dir_path);
    if (!d)
        return;

    const size_t num_builtins =
        sizeof(k_builtin_shader_presets) / sizeof(k_builtin_shader_presets[0]);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcasecmp(dot, ".frag") != 0)
            continue;

        size_t stem_len = (size_t)(dot - de->d_name);
        if (stem_len == 0 || stem_len >= 64)
            continue;

        char norm_name[64];
        bool valid = true;
        for (size_t i = 0; i < stem_len; ++i) {
            unsigned char c = (unsigned char)de->d_name[i];
            if (c == '-') {
                norm_name[i] = '_';
            } else if (isalnum(c) || c == '_') {
                norm_name[i] = (char)tolower(c);
            } else {
                valid = false;
                break;
            }
        }
        if (!valid)
            continue;
        norm_name[stem_len] = '\0';

        bool already_seen = false;
        for (size_t b = 0; b < num_builtins; ++b) {
            if (strcmp(k_builtin_shader_presets[b].name, norm_name) == 0) {
                already_seen = true;
                break;
            }
        }
        if (!already_seen && out_list) {
            size_t stored = (*io_count < max_entries) ? *io_count : max_entries;
            for (size_t s = 0; s < stored; ++s) {
                if (strcmp(out_list[s].name, norm_name) == 0) {
                    already_seen = true;
                    break;
                }
            }
        }
        if (!already_seen && overflow) {
            for (size_t s = 0; s < overflow->count; ++s) {
                if (strcmp(overflow->names[s], norm_name) == 0) {
                    already_seen = true;
                    break;
                }
            }
        }
        if (already_seen)
            continue;

        char full_cand[WAYWAL_PATH_MAX];
        snprintf(full_cand, sizeof(full_cand), "%s/%s", dir_path, de->d_name);
        char resolved_path[WAYWAL_PATH_MAX];
        if (!try_stat_regular_shader(full_cand, resolved_path, sizeof(resolved_path)))
            continue;

        if (out_list && *io_count < max_entries) {
            waywal_shader_preset_info_t *dst = &out_list[*io_count];
            memset(dst, 0, sizeof(*dst));
            snprintf(dst->name, sizeof(dst->name), "%s", norm_name);
            snprintf(dst->path, sizeof(dst->path), "%s", resolved_path);
            snprintf(dst->filename, sizeof(dst->filename), "%s.frag", norm_name);
            snprintf(dst->title, sizeof(dst->title), "%s", norm_name);
            snprintf(dst->description, sizeof(dst->description),
                     "Custom procedural shader preset");
            snprintf(dst->author, sizeof(dst->author), "Custom");
            dst->audio_reactive = (strncmp(norm_name, "audio_", 6) == 0);
            dst->is_builtin = false;
            parse_preset_file_metadata(dst);
        } else if (overflow) {
            if (overflow->count == overflow->cap) {
                size_t new_cap = overflow->cap ? (overflow->cap * 2) : 16;
                char (*new_names)[64] =
                    (char (*)[64])realloc(overflow->names, new_cap * sizeof(*new_names));
                if (new_names) {
                    overflow->names = new_names;
                    overflow->cap = new_cap;
                }
            }
            if (overflow->count < overflow->cap) {
                snprintf(overflow->names[overflow->count],
                         sizeof(overflow->names[overflow->count]), "%s", norm_name);
                overflow->count++;
            }
        }
        (*io_count)++;
    }
    closedir(d);
}

size_t waywal_shader_preset_list(waywal_shader_preset_info_t *out_list, size_t max_entries)
{
    const size_t num_builtins =
        sizeof(k_builtin_shader_presets) / sizeof(k_builtin_shader_presets[0]);

    size_t count = 0;

    for (size_t i = 0; i < num_builtins; ++i) {
        const builtin_shader_preset_def_t *b = &k_builtin_shader_presets[i];
        if (out_list && count < max_entries) {
            waywal_shader_preset_info_t *dst = &out_list[count];
            memset(dst, 0, sizeof(*dst));
            snprintf(dst->name, sizeof(dst->name), "%s", b->name);
            snprintf(dst->filename, sizeof(dst->filename), "%s", b->filename);
            snprintf(dst->title, sizeof(dst->title), "%s", b->title);
            snprintf(dst->description, sizeof(dst->description), "%s", b->description);
            snprintf(dst->author, sizeof(dst->author), "WayWal");
            dst->audio_reactive = b->audio_reactive;
            dst->is_builtin = true;
            if (!probe_preset_stem(b->name, dst->path, sizeof(dst->path))) {
                snprintf(dst->path, sizeof(dst->path), "shaders/presets/%s", b->filename);
            }
            parse_preset_file_metadata(dst);
        }
        count++;
    }

    preset_overflow_seen_t overflow = {NULL, 0, 0};

    const char *env_preset_dir = getenv("WWAL_SHADER_PRESET_DIR");
    if (env_preset_dir && env_preset_dir[0] != '\0') {
        char exp_dir[WAYWAL_PATH_MAX];
        const char *dir =
            waywal_expand_path(env_preset_dir, exp_dir, sizeof(exp_dir)) ? exp_dir : env_preset_dir;
        scan_custom_preset_dir(dir, out_list, max_entries, &count, &overflow);
    }

    const char *env_shader_dir = getenv("WWAL_SHADER_DIR");
    if (env_shader_dir && env_shader_dir[0] != '\0') {
        char exp_dir[WAYWAL_PATH_MAX];
        const char *dir =
            waywal_expand_path(env_shader_dir, exp_dir, sizeof(exp_dir)) ? exp_dir : env_shader_dir;
        char sub[WAYWAL_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/presets", dir);
        scan_custom_preset_dir(sub, out_list, max_entries, &count, &overflow);
    }

    char xdg_sub[WAYWAL_PATH_MAX] = {0};
    char xdg_cfg[WAYWAL_PATH_MAX];
    if (resolve_env_or_xdg("XDG_CONFIG_HOME", xdg_cfg, sizeof(xdg_cfg))) {
        snprintf(xdg_sub, sizeof(xdg_sub), "%s/wwal/shaders/presets", xdg_cfg);
        scan_custom_preset_dir(xdg_sub, out_list, max_entries, &count, &overflow);
    }

    char home[WAYWAL_PATH_MAX];
    if (resolve_home_dir(home, sizeof(home))) {
        char sub[WAYWAL_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/.config/wwal/shaders/presets", home);
        if (strcmp(sub, xdg_sub) != 0) {
            scan_custom_preset_dir(sub, out_list, max_entries, &count, &overflow);
        }
    }

#ifdef WWAL_DATADIR
    {
        char sub[WAYWAL_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/wwal/shaders/presets", WWAL_DATADIR);
        scan_custom_preset_dir(sub, out_list, max_entries, &count, &overflow);
        snprintf(sub, sizeof(sub), "%s/waywal/shaders/presets", WWAL_DATADIR);
        scan_custom_preset_dir(sub, out_list, max_entries, &count, &overflow);
    }
#endif

    free(overflow.names);
    return (out_list && max_entries > 0 && count > max_entries) ? max_entries : count;
}
