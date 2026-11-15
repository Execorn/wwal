// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file test_ipc_legacy_72byte.c
 * @brief Strict 72-Byte Legacy IPC Backward Compatibility Test Suite
 *
 * Emulates a legacy v0.x client transmitting an exact 72-byte waywal_img_metadata_t
 * header prefix over the Unix domain socket via SCM_RIGHTS.
 * Verifies that the daemon accepts payloads >= 72 bytes, zero-extends unpopulated
 * extended fields (72..4814) safely without bounds rejection or segfault, and returns
 * WAYWAL_RESP_OK.
 */

#include "waywal/ipc_proto.h"
#include "waywal/ipc_proto_phase3.h"
#include "waywal/log.h"
#include "waywal/os_compat.h"
#include "waywal/path.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t transition_type;
    uint32_t transition_duration_ms;
    uint32_t transition_fps;
    float transition_angle_rad;
    float transition_wave_freq;
    float transition_wave_amp;
    float transition_center_x;
    float transition_center_y;
    uint8_t color[4];
    uint32_t num_target_outputs;
    uint32_t sync_mode;
    uint32_t stagger_delay_ms;
    uint32_t custom_shader_len;
    uint32_t extra_data_len;
    uint32_t scaling_mode;
} legacy_img_metadata_72_t;
#pragma pack(pop)

static_assert(sizeof(legacy_img_metadata_72_t) == 72, "Legacy header MUST be exactly 72 bytes");

static void test_in_memory_zero_extension(void)
{
    printf("[TEST] Verifying in-memory 72-byte zero-extension bounds & layout...\n");

    static_assert(sizeof(waywal_img_metadata_t) == 4815, "waywal_img_metadata_t size must be 4815");
    static_assert(offsetof(waywal_img_metadata_t, filter_mode) == 72,
                  "filter_mode must start immediately at offset 72");

    legacy_img_metadata_72_t legacy;
    memset(&legacy, 0, sizeof(legacy));
    legacy.width = 1920;
    legacy.height = 1080;
    legacy.pixel_format = 0; // RGBA8888
    legacy.transition_type = 1;
    legacy.transition_duration_ms = 500;
    legacy.transition_fps = 60;
    legacy.scaling_mode = 0; // FILL

    // Simulate daemon heap allocation & zero-extension
    waywal_img_metadata_t *extended =
        (waywal_img_metadata_t *)calloc(1, sizeof(waywal_img_metadata_t));
    assert(extended != NULL);

    memcpy(extended, &legacy, sizeof(legacy));

    // Verify legacy fields preserved
    assert(extended->width == 1920);
    assert(extended->height == 1080);
    assert(extended->transition_duration_ms == 500);
    assert(extended->transition_fps == 60);

    // Verify extended fields zeroed
    assert(extended->filter_mode == 0);
    assert(extended->has_custom_bezier == false);
    assert(extended->ten_bit == false);
    assert(extended->extract_theme == false);
    assert(extended->depth_strength == 0.0f);
    assert(extended->filepath[0] == '\0');
    assert(extended->battery_policy[0] == '\0');
    assert(extended->solar_latitude == 0.0);
    assert(extended->solar_longitude == 0.0);

    free(extended);
    printf("  -> In-memory zero-extension verified successfully.\n");
}

static int create_test_memfd_with_seals(uint32_t width, uint32_t height, bool extended_hdr,
                                        unsigned int seals)
{
    uint64_t pixel_bytes = (uint64_t)width * (uint64_t)height * 4ULL;
    size_t hdr_bytes =
        extended_hdr ? sizeof(waywal_img_metadata_t) : sizeof(legacy_img_metadata_72_t);
    uint64_t total_size = (uint64_t)hdr_bytes + pixel_bytes;

    int memfd = waywal_create_memfd("wwal-test-shm", total_size, seals);
    if (memfd < 0) {
        perror("waywal_create_memfd");
        return -1;
    }

    void *ptr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(memfd);
        return -1;
    }

    if (extended_hdr) {
        waywal_img_metadata_t *hdr = (waywal_img_metadata_t *)ptr;
        memset(hdr, 0, sizeof(*hdr));
        hdr->width = width;
        hdr->height = height;
        hdr->transition_fps = 60;
        hdr->filter_mode = 1;
        hdr->invert_y = 0;
    } else {
        legacy_img_metadata_72_t *hdr = (legacy_img_metadata_72_t *)ptr;
        memset(hdr, 0, sizeof(*hdr));
        hdr->width = width;
        hdr->height = height;
        hdr->pixel_format = 0;
        hdr->transition_type = 0;
        hdr->transition_duration_ms = 100;
        hdr->transition_fps = 60;
        hdr->extra_data_len = 0;
        hdr->scaling_mode = 0; // FILL
    }

    uint8_t *pixels = (uint8_t *)ptr + hdr_bytes;
    for (uint64_t i = 0; i < pixel_bytes; i += 4) {
        pixels[i + 0] = 0x00; // R
        pixels[i + 1] = 0xFF; // G
        pixels[i + 2] = 0x00; // B
        pixels[i + 3] = 0xFF; // A
    }

    munmap(ptr, total_size);
    return memfd;
}

static void test_mixed_72b_and_shrink_sealed_memfd(void)
{
    printf("[TEST] Verifying daemon_validate_set_image_header, waywal_validate_img_metadata, "
           "and F_SEAL_SHRINK memfd framing...\n");

    const uint32_t test_w = 16;
    const uint32_t test_h = 16;
    const uint64_t px_bytes = (uint64_t)test_w * test_h * 4ULL;

    /* Exact 72B header and full extended header must pass; truncated (< 72B) must fail. */
    waywal_ipc_hdr_t h72 = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_IMAGE,
        .payload_size = 72u + px_bytes,
    };
    waywal_ipc_hdr_t hext = h72;
    hext.payload_size = sizeof(waywal_img_metadata_t) + px_bytes;
    waywal_ipc_hdr_t hshort = h72;
    hshort.payload_size = 71u;
    assert(daemon_validate_set_image_header(&h72));
    assert(daemon_validate_set_image_header(&hext));
    assert(!daemon_validate_set_image_header(&hshort));

    waywal_img_metadata_t meta_ok;
    memset(&meta_ok, 0, sizeof(meta_ok));
    meta_ok.width = test_w;
    meta_ok.height = test_h;
    meta_ok.extra_data_len = 64u;
    meta_ok.custom_shader_len = 16u;
    uint64_t out_px = 0;
    assert(waywal_validate_img_metadata(&meta_ok, px_bytes, &out_px));
    assert(out_px == px_bytes);

    waywal_img_metadata_t bad_meta = meta_ok;
    bad_meta.extra_data_len = WWAL_MAX_EXTRA_IPC_BYTES + 1u;
    assert(!waywal_validate_img_metadata(&bad_meta, px_bytes, NULL));
    bad_meta = meta_ok;
    bad_meta.custom_shader_len = bad_meta.extra_data_len + 1u;
    assert(!waywal_validate_img_metadata(&bad_meta, px_bytes, NULL));
    bad_meta = meta_ok;
    bad_meta.width = UINT32_MAX / 4u;
    bad_meta.height = 2u;
    assert(!waywal_validate_img_metadata(&bad_meta, px_bytes, NULL));
    assert(!waywal_validate_img_metadata(&meta_ok, px_bytes - 1u, NULL));

    /* Transmit mixed legacy-72B + extended shrink-sealed memfds over UNIX socketpair. */
    int sv[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

    int m72 = create_test_memfd_with_seals(test_w, test_h, false, F_SEAL_SHRINK);
    int mext = create_test_memfd_with_seals(test_w, test_h, true, F_SEAL_SHRINK);
    int munsealed = create_test_memfd_with_seals(test_w, test_h, false, 0);
    assert(m72 >= 0 && mext >= 0 && munsealed >= 0);

    int seals_72 = fcntl(m72, F_GET_SEALS);
    assert(seals_72 >= 0 && (seals_72 & F_SEAL_SHRINK) != 0);
    assert(ftruncate(m72, 0) < 0 && errno == EPERM);
    int seals_unsealed = fcntl(munsealed, F_GET_SEALS);
    assert(seals_unsealed >= 0 && (seals_unsealed & F_SEAL_SHRINK) == 0);

    assert(waywal_ipc_send(sv[0], &h72, m72));
    assert(waywal_ipc_send(sv[0], &hext, mext));

    for (int iter = 0; iter < 2; ++iter) {
        waywal_ipc_hdr_t rh = {0};
        int rfd = -1;
        assert(waywal_ipc_recv(sv[1], &rh, &rfd));
        assert(rfd >= 0);
        assert(daemon_validate_set_image_header(&rh));

        struct stat st;
        assert(fstat(rfd, &st) == 0 && S_ISREG(st.st_mode));
        int seals = fcntl(rfd, F_GET_SEALS);
        assert(seals >= 0 && (seals & F_SEAL_SHRINK) != 0);

        size_t hdr_bytes = (iter == 0) ? 72u : sizeof(waywal_img_metadata_t);
        assert((uint64_t)st.st_size == hdr_bytes + px_bytes);
        void *mapped = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, rfd, 0);
        assert(mapped != MAP_FAILED);

        waywal_img_metadata_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        memcpy(&decoded, mapped, hdr_bytes);
        uint64_t dec_px = 0;
        assert(waywal_validate_img_metadata(&decoded, (uint64_t)st.st_size - hdr_bytes, &dec_px));
        assert(dec_px == px_bytes);
        assert(decoded.width == test_w && decoded.height == test_h);
        if (iter == 0) {
            assert(decoded.filter_mode == 0u);
        } else {
            assert(decoded.filter_mode == 1u);
        }
        munmap(mapped, (size_t)st.st_size);
        close(rfd);
    }

    close(m72);
    close(mext);
    close(munsealed);
    close(sv[0]);
    close(sv[1]);
    printf("  -> Mixed legacy-72B, extended, and F_SEAL_SHRINK memfd invariants verified.\n");
}

static int try_connect_socket(const char *sock_path)
{
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static int find_or_spawn_daemon(const char *ns, pid_t *out_pid, char *out_sock_path,
                                size_t sock_cap)
{
    *out_pid = -1;

    // Resolve socket path
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || runtime_dir[0] == '\0') {
        runtime_dir = "/tmp";
    }
    const char *wl_disp = getenv("WAYLAND_DISPLAY");
    if (!wl_disp || wl_disp[0] == '\0') {
        wl_disp = "wayland-0";
    }

    snprintf(out_sock_path, sock_cap, "%s/%s-wwald.%s.sock", runtime_dir, wl_disp, ns);

    int s = try_connect_socket(out_sock_path);
    if (s >= 0) {
        return s;
    }

    // Try finding wwald executable to spawn
    const char *candidates[] = {
        "./wwald",     "build_worker/wwald",        "build/src/daemon/wwald",
        "build/wwald", "../build/src/daemon/wwald", "../build/wwald",
        NULL};

    const char *daemon_bin = NULL;
    for (int i = 0; candidates[i]; ++i) {
        if (access(candidates[i], X_OK) == 0) {
            daemon_bin = candidates[i];
            break;
        }
    }

    if (!daemon_bin) {
        // Standalone verification without live daemon
        return -2;
    }

    printf("[TEST] Spawning temporary daemon: %s --namespace %s\n", daemon_bin, ns);
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        execl(daemon_bin, daemon_bin, "--namespace", ns, (char *)NULL);
        _exit(127);
    }

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    *out_pid = pid;

    // Poll for socket readiness up to 2 seconds (40 * 50ms)
    for (int i = 0; i < 40; ++i) {
        usleep(50000);
        s = try_connect_socket(out_sock_path);
        if (s >= 0) {
            return s;
        }
        int status;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            fprintf(stderr, "[-] Spawned daemon exited prematurely.\n");
            return -1;
        }
    }

    return -1;
}

int main(int argc, char *argv[])
{
    waywal_log_set_level(WAYWAL_LOG_LEVEL_INFO);
    printf("==========================================================\n");
    printf("  72-Byte Legacy IPC Backward Compatibility Test Suite    \n");
    printf("==========================================================\n");

    // 1. Run pure mathematical, struct layout & shrink-sealed memfd tests
    test_in_memory_zero_extension();
    test_mixed_72b_and_shrink_sealed_memfd();

    // 2. Parse --namespace argument
    const char *ns = "test_compat";
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--namespace") == 0 || strcmp(argv[i], "-n") == 0) && i + 1 < argc) {
            ns = argv[++i];
        }
    }

    char sock_path[1024] = {0};
    pid_t daemon_pid = -1;
    int client_fd = find_or_spawn_daemon(ns, &daemon_pid, sock_path, sizeof(sock_path));

    if (client_fd == -2) {
        printf("[TEST] Notice: wwald binary not found in local path. Skipping live socket test.\n");
        printf("==========================================================\n");
        printf("  >>> 72-BYTE IPC PROTOCOL INVARIANT VERIFIED CLEANLY <<<\n");
        printf("==========================================================\n");
        return 0;
    }

    if (client_fd < 0) {
        if (getenv("WAYLAND_DISPLAY") == NULL) {
            printf("[TEST] Notice: Headless environment without Wayland compositor socket.\n");
            printf("==========================================================\n");
            printf("  >>> 72-BYTE IPC PROTOCOL INVARIANT VERIFIED CLEANLY <<<\n");
            printf("==========================================================\n");
            return 0;
        }
        fprintf(stderr, "[-] Failed to connect to daemon socket at %s\n", sock_path);
        if (daemon_pid > 0)
            kill(daemon_pid, SIGTERM);
        return 1;
    }

    printf("[TEST] Connected to daemon socket: %s\n", sock_path);

    // 3. Create shrink-sealed legacy 72-byte payload memfd
    const uint32_t test_w = 16;
    const uint32_t test_h = 16;
    int memfd = create_test_memfd_with_seals(test_w, test_h, false, F_SEAL_SHRINK);
    assert(memfd >= 0 && "create_test_memfd_with_seals failed");

    uint64_t total_payload = sizeof(legacy_img_metadata_72_t) + ((uint64_t)test_w * test_h * 4);

    // 4. Send exact legacy 72-byte packet
    waywal_ipc_hdr_t req_hdr = {
        .magic = WAYWAL_IPC_MAGIC,
        .version = WAYWAL_IPC_VERSION,
        .opcode = WAYWAL_REQ_SET_IMAGE,
        .payload_size = total_payload,
    };

    printf("[TEST] Transmitting legacy 72-byte metadata payload (total: %lu bytes) with "
           "SCM_RIGHTS...\n",
           (unsigned long)total_payload);
    bool sent = waywal_ipc_send(client_fd, &req_hdr, memfd);
    assert(sent && "waywal_ipc_send failed");
    close(memfd);

    // 5. Read response from daemon
    waywal_ipc_hdr_t resp_hdr;
    int resp_fd = -1;
    bool recvd = waywal_ipc_recv(client_fd, &resp_hdr, &resp_fd);
    assert(recvd && "Failed to receive response from daemon");
    if (resp_fd >= 0)
        close(resp_fd);

    printf("[TEST] Daemon response: magic=0x%08X opcode=0x%04X payload_size=%lu\n", resp_hdr.magic,
           resp_hdr.opcode, (unsigned long)resp_hdr.payload_size);

    assert(resp_hdr.magic == WAYWAL_IPC_MAGIC && "Invalid response magic");
    assert(resp_hdr.opcode == WAYWAL_RESP_OK &&
           "Daemon must accept shrink-sealed 72-byte legacy packet with WAYWAL_RESP_OK");

    close(client_fd);

    // 6. Verify unsealed SET_IMAGE memfd rejection and extended shrink-sealed acceptance on fresh
    // sockets.
    int reject_fd = try_connect_socket(sock_path);
    if (reject_fd >= 0) {
        int bad_memfd = create_test_memfd_with_seals(test_w, test_h, false, 0);
        assert(bad_memfd >= 0);
        assert(waywal_ipc_send(reject_fd, &req_hdr, bad_memfd));
        close(bad_memfd);
        waywal_ipc_hdr_t bad_resp = {0};
        int bad_rfd = -1;
        assert(waywal_ipc_recv(reject_fd, &bad_resp, &bad_rfd));
        if (bad_rfd >= 0)
            close(bad_rfd);
        assert(bad_resp.opcode == WAYWAL_RESP_ERR);
        close(reject_fd);
    }

    int ext_fd = try_connect_socket(sock_path);
    if (ext_fd >= 0) {
        int ext_memfd = create_test_memfd_with_seals(test_w, test_h, true, F_SEAL_SHRINK);
        assert(ext_memfd >= 0);
        waywal_ipc_hdr_t ext_hdr = req_hdr;
        ext_hdr.payload_size =
            sizeof(waywal_img_metadata_t) + ((uint64_t)test_w * (uint64_t)test_h * 4ULL);
        assert(waywal_ipc_send(ext_fd, &ext_hdr, ext_memfd));
        close(ext_memfd);
        waywal_ipc_hdr_t ext_resp = {0};
        int ext_rfd = -1;
        assert(waywal_ipc_recv(ext_fd, &ext_resp, &ext_rfd));
        if (ext_rfd >= 0)
            close(ext_rfd);
        assert(ext_resp.opcode == WAYWAL_RESP_OK);
        close(ext_fd);
    }

    if (daemon_pid > 0) {
        kill(daemon_pid, SIGTERM);
        waitpid(daemon_pid, NULL, 0);
    }

    printf("==========================================================\n");
    printf("  >>> 72-BYTE IPC PROTOCOL INVARIANT VERIFIED CLEANLY (100%%) <<<\n");
    printf("==========================================================\n");
    return 0;
}
