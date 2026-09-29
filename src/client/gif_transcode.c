#include "waywal/gif_transcode.h"

#include "waywal/log.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

bool is_gif_file(const char *filepath)
{
    if (!filepath)
        return false;
    FILE *fp = fopen(filepath, "rb");
    if (!fp)
        return false;
    uint8_t magic[6];
    size_t n = fread(magic, 1, 6, fp);
    fclose(fp);
    if (n < 6)
        return false;
    return (memcmp(magic, "GIF87a", 6) == 0 || memcmp(magic, "GIF89a", 6) == 0);
}

static bool mkdir_p(const char *path)
{
    char tmp[4096];
    size_t len = strlen(path);
    if (len >= sizeof(tmp))
        return false;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0) {
                if (errno != EEXIST)
                    return false;
                struct stat st;
                if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))
                    return false;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0) {
        if (errno != EEXIST)
            return false;
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))
            return false;
    }
    return true;
}

#define WAYWAL_GIF_TRANSCODE_VERSION 0x56325F4950505030ULL /* "V2_IPPP0" */

static bool compute_file_hash(const char *filepath, char *out_hex, size_t out_size)
{
    if (out_size < 17)
        return false;

    struct stat st;
    if (stat(filepath, &st) < 0)
        return false;

    uint64_t hash = 14695981039346656037ULL ^ WAYWAL_GIF_TRANSCODE_VERSION;
    uint64_t sz = (uint64_t)st.st_size;
    uint64_t mt = (uint64_t)st.st_mtime;

    for (int i = 0; i < 8; ++i) {
        hash ^= (sz >> (i * 8)) & 0xFF;
        hash *= 1099511628211ULL;
        hash ^= (mt >> (i * 8)) & 0xFF;
        hash *= 1099511628211ULL;
    }

    FILE *fp = fopen(filepath, "rb");
    if (!fp)
        return false;

    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        for (size_t i = 0; i < n; ++i) {
            hash ^= buf[i];
            hash *= 1099511628211ULL;
        }
    }
    fclose(fp);

    snprintf(out_hex, out_size, "%016" PRIx64, hash);
    return true;
}

static bool get_cache_dir(char *out_dir, size_t out_size)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0]) {
        snprintf(out_dir, out_size, "%s/wwal/transcoded", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0])
            return false;
        snprintf(out_dir, out_size, "%s/.cache/wwal/transcoded", home);
    }
    return mkdir_p(out_dir);
}

bool gif_transcode_resolve(const char *filepath, char *out_path, size_t out_path_size)
{
    if (!filepath || !out_path || out_path_size == 0)
        return false;

    if (!is_gif_file(filepath)) {
        strncpy(out_path, filepath, out_path_size - 1);
        out_path[out_path_size - 1] = '\0';
        return true;
    }

    char cache_dir[4096];
    if (!get_cache_dir(cache_dir, sizeof(cache_dir))) {
        WAYWAL_LOG_ERR("Failed to create JIT transcode cache directory");
        return false;
    }

    char hash_str[32];
    if (!compute_file_hash(filepath, hash_str, sizeof(hash_str))) {
        WAYWAL_LOG_ERR("Failed to compute hash for GIF file: %s", filepath);
        return false;
    }

    char target_mp4[4096];
    snprintf(target_mp4, sizeof(target_mp4), "%s/%s.mp4", cache_dir, hash_str);

    if (strlen(target_mp4) >= out_path_size) {
        WAYWAL_LOG_ERR("Output buffer too small for cached video path");
        return false;
    }

    struct stat st;
    if (stat(target_mp4, &st) == 0 && st.st_size > 1024) {
        WAYWAL_LOG_INFO("Using cached hardware-accelerated video: %s", target_mp4);
        strncpy(out_path, target_mp4, out_path_size - 1);
        out_path[out_path_size - 1] = '\0';
        return true;
    }

    char lock_path[4096];
    snprintf(lock_path, sizeof(lock_path), "%s/%s.lock", cache_dir, hash_str);
    int lock_fd = open(lock_path, O_RDWR | O_CREAT, 0644);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_EX);
        if (stat(target_mp4, &st) == 0 && st.st_size > 1024) {
            WAYWAL_LOG_INFO("Using cached hardware-accelerated video: %s", target_mp4);
            strncpy(out_path, target_mp4, out_path_size - 1);
            out_path[out_path_size - 1] = '\0';
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
            return true;
        }
    }

    bool success = false;

    /* Cache miss: transcode via ffmpeg */
    WAYWAL_LOG_INFO(
        "Animated GIF detected: JIT transcoding to hardware-accelerated MP4 via ffmpeg...");

    char tmp_mp4[4096];
    snprintf(tmp_mp4, sizeof(tmp_mp4), "%s/%s.tmp.%d.mp4", cache_dir, hash_str, (int)getpid());

    pid_t pid = fork();
    if (pid < 0) {
        WAYWAL_LOG_ERR("fork failed: %s", strerror(errno));
        goto cleanup;
    }

    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }

        char *const args[] = {"ffmpeg",
                              "-y",
                              "-v",
                              "error",
                              "-i",
                              (char *)filepath,
                              "-vf",
                              "pad=ceil(iw/2)*2:ceil(ih/2)*2",
                              "-c:v",
                              "libx264",
                              "-pix_fmt",
                              "yuv420p",
                              "-preset",
                              "fast",
                              "-crf",
                              "18",
                              "-bf",
                              "0",
                              "-slices",
                              "1",
                              "-x264-params",
                              "repeat-headers=1:bframes=0",
                              "-movflags",
                              "+faststart",
                              "-an",
                              tmp_mp4,
                              NULL};
        execvp("ffmpeg", args);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            unlink(tmp_mp4);
            goto cleanup;
        }
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 127) {
            WAYWAL_LOG_ERR(
                "ffmpeg not found on system PATH. Please install ffmpeg for JIT GIF transcoding.");
            unlink(tmp_mp4);
            goto cleanup;
        }
        if (code != 0) {
            WAYWAL_LOG_ERR("ffmpeg transcoding failed with exit code %d", code);
            unlink(tmp_mp4);
            goto cleanup;
        }
    } else {
        WAYWAL_LOG_ERR("ffmpeg transcoding terminated abnormally");
        unlink(tmp_mp4);
        goto cleanup;
    }

    if (stat(tmp_mp4, &st) != 0 || st.st_size <= 1024) {
        WAYWAL_LOG_ERR("Transcoded video file is invalid or too small");
        unlink(tmp_mp4);
        goto cleanup;
    }

    if (rename(tmp_mp4, target_mp4) != 0) {
        WAYWAL_LOG_ERR("Failed to rename temporary transcoded file to cache: %s", strerror(errno));
        unlink(tmp_mp4);
        goto cleanup;
    }

    WAYWAL_LOG_INFO("JIT transcoding complete: cached hardware-accelerated video to %s",
                    target_mp4);
    strncpy(out_path, target_mp4, out_path_size - 1);
    out_path[out_path_size - 1] = '\0';
    success = true;

cleanup:
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
    return success;
}
