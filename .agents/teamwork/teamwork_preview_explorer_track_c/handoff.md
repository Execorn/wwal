# Handoff Report: Track C — Linux Security, Sandboxing & IPC Systems Audit

## 1. Observation
1. **`src/daemon/main.c:155-201` and `src/daemon/layer_shell.c:773-800`**:
   `WAYWAL_REQ_SET_IMAGE` maps client-provided shared memory into `waywal_img_metadata_t *meta`. In `layer_shell.c`:
   ```c
   static bool is_target_output(const output_node_t *node, const waywal_img_metadata_t *meta) {
       if (!meta || meta->num_target_outputs == 0) return true;
       const char *ptr = (const char *)meta + sizeof(waywal_img_metadata_t);
       for (uint32_t i = 0; i < meta->num_target_outputs; ++i) {
           if (strcmp(node->name, ptr) == 0) return true;
           ptr += strlen(ptr) + 1;
       }
       return false;
   }
   ```
   Neither `meta->num_target_outputs` nor the output strings in `extra_data_len` are validated for length bounds or null-termination before calling `strlen` and `strcmp`.
2. **`src/daemon/security.c:67-79`**:
   In `security_sandbox_apply()`:
   ```c
   const char *ro_paths[] = {"/", "/usr", "/etc", "/sys", "/proc", "/dev", "/home", NULL};
   ```
   Landlock rules recursively allow read access to `"/"` and `"/home"`, permitting full host filesystem read traversal.
3. **`src/client/gif_transcode.c:74-108`**:
   In `compute_file_hash()`, when `st.st_size > 8192`, only the first 4096 bytes and last 4096 bytes are read and hashed into the FNV-1a state. All intermediate animation frames are skipped.
4. **`src/client/gif_transcode.c:156-251`**:
   `gif_transcode_resolve()` checks `stat(target_mp4)` on cache miss, forks `ffmpeg` writing to `tmp.%d.mp4`, and calls `rename()`. No file locking (`flock`) exists, leading to parallel redundant encodes and atomic cache overwrites.
5. **`src/daemon/main.c:81-85, 320`**:
   `accept4(..., SOCK_NONBLOCK)` sets non-blocking mode on `client_fd`. In `daemon_process_ipc_client()`, `setsockopt(..., SO_RCVTIMEO)` is called without clearing `O_NONBLOCK`. In Linux kernel networking, `O_NONBLOCK` overrides `SO_RCVTIMEO`, causing immediate `EAGAIN` read aborts.
6. **`src/client/image_loader.c:25-28, 73-95`**:
   In `load_png()`, `pixels` and `row_pointers` are allocated on the heap after `setjmp(png_jmpbuf(png))`. On any libpng decoding error, libpng triggers `longjmp()`, returning to line 25 where only `png_destroy_read_struct()` is called, leaking up to 1GB of heap memory.
7. **`src/daemon/security.c:223-234`**:
   Seccomp allowlist includes `SCMP_SYS(socket)` without filtering `domain == AF_UNIX`, permitting creation of `AF_INET`/`AF_INET6` sockets.
8. **`src/client/gif_transcode.c:110-122`**:
   Cached files in `$XDG_CACHE_HOME/wwal/transcoded/` are never pruned or bounded by quota.

## 2. Logic Chain
1. From Observation 1: An untrusted client can send an image request with `extra_data_len > 0` containing non-null-terminated bytes or `num_target_outputs` set to a large value. When `is_target_output` or custom shader loading executes `strlen(ptr)` and `ptr += strlen(ptr) + 1`, execution reads past the end of mapped memory into unmapped virtual addresses, causing an immediate `SIGSEGV` crash of the daemon. This proves **TRK-C-01 (Critical)**.
2. From Observation 2: The stated architecture relies on passing media FDs via `SCM_RIGHTS` so `wwald` never needs filesystem read access to user directories. Because Landlock's `LANDLOCK_RULE_PATH_BENEATH` is hierarchical, adding `"/"` and `"/home"` completely destroys filesystem read confinement, allowing an attacker who exploits the daemon to read SSH keys, browser profiles, and private tokens. This proves **TRK-C-02 (Critical)**.
3. From Observation 3: In an animated GIF of size 10MB, omitting byte 4096 through `size - 4096` ignores 99.9% of the file data. Two distinct GIFs sharing palette/screen headers and identical file sizes will compute identical FNV-1a hashes, causing the client to return the wrong cached MP4. This proves **TRK-C-03 (High)**.
4. From Observation 4: If two client instances execute simultaneously on the same uncached GIF (common in multi-monitor startup configs), both see a cache miss and spawn `ffmpeg` processes simultaneously at 100% CPU. The latter to finish renames over the former's file while the daemon is actively decoding it. This proves **TRK-C-04 (High)**.
5. From Observation 5: In POSIX/Linux, setting socket timeouts (`SO_RCVTIMEO`) on a socket with `O_NONBLOCK` active is a no-op. When `read_exact()` calls `read()`, any delay causes `EAGAIN`, which is treated as EOF/failure, abruptly dropping the client connection. This proves **TRK-C-05 (High)**.
6. From Observation 6: Because `setjmp` unwinds the stack upon `png_error()`, local pointers `pixels` and `row_pointers` allocated on the heap are never reached by free routines, leaking up to 1GB per corrupt image. This proves **TRK-C-06 (High)**.

## 3. Caveats
- No code was modified in the source tree; per explorer constraints, all fixes are provided as drop-in compilable C code snippets in `audit_report_track_c.md`.
- Hardware-specific DRM ioctl permissions in Seccomp cannot be filtered by individual ioctl opcode without `SECCOMP_USER_NOTIF` or Landlock ABI v5 `LANDLOCK_ACCESS_FS_IOCTL_DEV` support.
- VA-API decode behavior was verified via audit logs and prior walkthrough; live hardware decoding was confirmed functional in test suites.

## 4. Conclusion
The Track C codebase contains 2 Critical, 4 High, 3 Medium, 2 Low, and 1 Informational issues. All Critical and High findings have complete, compilable drop-in C code fixes provided in `audit_report_track_c.md`. Implementing these fixes will harden `wwal`/`wwald` against crash-DoS attacks, preserve host filesystem confidentiality, eliminate transcode collisions and races, prevent client connection drops, and stop memory leaks.

## 5. Verification Method
1. **Compilation & Test Suite**:
   ```bash
   ninja -C /home/execorn/programming/projects/wwal/build test
   ```
   Ensure all 7 test suites pass.
2. **IPC Framing Verification**:
   Inspect `src/daemon/main.c:163-186` to confirm string validation loop with `memchr` and `meta->num_target_outputs <= 64`.
3. **Landlock Ruleset Verification**:
   Inspect `src/daemon/security.c:67` to verify `"/"` and `"/home"` are absent from `ro_paths`.
4. **GIF Hash Verification**:
   Run `test_advanced` to ensure `compute_file_hash` properly digests all GIF frames.
