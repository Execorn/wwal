# Security Policy

## Supported Versions

Security updates apply to the latest stable release branch and `master`:

| Version | Supported |
| :--- | :--- |
| `1.0.x` | Yes |
| `< 1.0` | No |

## Reporting a Vulnerability

Please do not report security vulnerabilities in public GitHub issues.

Use [GitHub Private Vulnerability Reporting](https://github.com/Execorn/wwal/security/advisories/new) to submit a report privately. Include a minimal reproduction command, input file (if applicable), and backtrace from an AddressSanitizer build (`-Db_sanitize=address,undefined`).

You can expect an initial response within 72 hours.

## Security Architecture & Scope

`wwald` parses untrusted image, video, shader, and configuration files while running inside the user's graphical session. The daemon enforces several OS-level mitigations after startup:

1. **Seccomp-BPF Syscall Allowlist (`src/daemon/security.c`)**: Restricts `wwald` to the specific set of Linux syscalls required for Wayland sockets, `io_uring`, DRM/GBM/VA-API `ioctl` calls, `memfd`/`shm`, and filesystem reads.
2. **Landlock LSM (`ABI v1..v3`)**: Limits filesystem write access to `$XDG_RUNTIME_DIR`, `$XDG_CACHE_HOME/wwal`, `$XDG_STATE_HOME/wwal`, and `$XDG_STATE_HOME/caelestia`.
3. **Process Hardening**: Sets `PR_SET_NO_NEW_PRIVS` and `PR_SET_DUMPABLE(0)`, and enforces `RLIMIT_CORE = 0`.
4. **Zero-Allocation Demuxers & Bounds-Checked Parsers**: `demuxer.c` (MP4/ISOBMFF and WebM/EBML), `video_vaapi.c` (H.264/HEVC/VP9/AV1 bitstream parsers), `anim_engine.c` (GIF/APNG/WebP), `toml_parser.c`, and `ipc_proto.c` are continuously tested with standalone fuzz targets (`tests/fuzz/`).

Reports covering memory corruption in media parsers, integer overflows in image allocation paths, UNIX socket IPC validation bypasses, or Seccomp/Landlock escapes are highest priority.
