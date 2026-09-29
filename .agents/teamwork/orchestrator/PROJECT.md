# Project: wwal Zero-Compromise Systems Code and Architecture Audit

## Architecture
The `wwal` system consists of:
1. `wwald` (Wayland daemon):
   - VA-API hardware video decoding (H.264) + VPP color conversion + DMA-BUF PRIME export
   - Wayland wlr-layer-shell surface management and direct scanout via DMA-BUF
   - Custom MP4/ISOBMFF demuxer with in-band/out-of-band SPS/PPS parsing
   - io_uring event loop for async event multiplexing
   - Dual-layer sandboxing: Landlock LSM + Seccomp-BPF with PR_SET_NO_NEW_PRIVS
   - wp_presentation PLL frame pacing and output hotplug handling
2. `wwal` (CLI client):
   - JIT GIF-to-H.264 transcode via ffmpeg with FNV-1a content-addressed caching
   - Unix domain socket IPC with `SCM_RIGHTS` file descriptor passing
   - CLI parsing and image loading
3. `common`:
   - IPC serialization, arena allocator, logging, bezier curves, path utilities, os compatibility
4. `tests`:
   - Unit and integration tests phases 1-5, advanced tests, transition benchmarks

## Feature Inventory
| # | Feature / Vector | Description | Assigned Track / Milestone | Source |
|---|------------------|-------------|----------------------------|--------|
| 1 | R1: Memory Safety & FD Hygiene | DMA-BUF prime FDs, DRM render node, timerfd, IPC sockets, transcode pipes, vaCreateBuffer/vaDestroyBuffer, surface pools | Track A, B, C | ORIGINAL_REQUEST §R1 |
| 2 | R2: Hardware Video & DPB | DPB surface allocation, reference collision, FIFO eviction, bitstream reader, Exp-Golomb, NAL prefix handling, vaSyncSurface | Track A | ORIGINAL_REQUEST §R2 |
| 3 | R3: Performance & Zero-Copy | Allocations/syscalls in video_engine_render_frame, presentation PLL drift, wl_buffer caching, FNV-1a collision & TOCTOU | Track A, B, C | ORIGINAL_REQUEST §R3 |
| 4 | R4: io_uring & Signal Safety | uring_loop.c SQE overflow, CQE processing, multishot re-arming, cancel, SIGINT/SIGTERM/SIGHUP async-safety | Track B | ORIGINAL_REQUEST §R4 |
| 5 | R5: Sandboxing & IPC Security | Landlock LSM path permissions, Seccomp-BPF allowlist, IPC length/framing/null-termination/RCE/DoS | Track C | ORIGINAL_REQUEST §R5 |
| 6 | R6: Test Coverage Gaps | Test suites 1-5, advanced, benchmarks; edge cases (VPP failure, DRM disconnect, corruption, zero-len GIF, malformed IPC) | Track D | ORIGINAL_REQUEST §R6 |
| 7 | R7: ArXiv Literature Research | Academic literature search & citation on zero-copy DRM, io_uring, Landlock/seccomp, DPB management | Track E / Specialist | ORIGINAL_REQUEST §R7 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Setup & Survey | Establish orchestrator state, BRIEFING.md, PROJECT.md, progress.md | none | DONE |
| M2 | Specialist Investigations | Dispatch Tracks A, B, C, D to read and analyze 100% of files; investigate R1-R6 | M1 | IN_PROGRESS |
| M3 | ArXiv Research & Theoretical Mapping | Query arXiv on zero-copy DRM, io_uring, Landlock/seccomp, and DPB correctness | M1 | IN_PROGRESS |
| M4 | Synthesis & Drop-in C Fix Snippets | Reconcile findings, verify severity, generate compilable C code snippets for Critical/High issues | M2, M3 | PLANNED |
| M5 | Report Artifact Generation & Final Verification | Assemble final comprehensive audit report artifact per exact acceptance criteria | M4 | PLANNED |

## Code Layout & Audit Track Allocation
### Track A (Video, VA-API, DPB, Demuxer)
- `src/daemon/video_vaapi.c`
- `src/daemon/demuxer.c`
- `src/daemon/video_engine.c`
- `include/waywal/vaapi_dec.h`
- `include/waywal/demuxer.h`
- `include/waywal/video_engine.h`

### Track B (Compositor, Wayland, Render, DMA-BUF, DRM, io_uring)
- `src/daemon/layer_shell.c`
- `src/daemon/render_engine.c`
- `src/daemon/gpu_compute.c`
- `src/daemon/main.c`
- `src/daemon/wayland_core.c`
- `src/daemon/dmabuf_alloc.c`
- `src/daemon/slideshow.c`
- `src/daemon/output_state.c`
- `src/daemon/dmabuf_feedback.c`
- `src/daemon/presentation.c`
- `src/daemon/uring_loop.c`
- `include/waywal/dmabuf.h`

### Track C (Sandboxing, Security, IPC, Client & JIT Transcode)
- `src/daemon/security.c`
- `src/client/image_loader.c`
- `src/client/cli_parser.c`
- `src/client/main.c`
- `src/client/gif_transcode.c`
- `src/common/ipc_proto.c`
- `src/common/log.c`
- `src/common/arena.c`
- `src/common/bezier.c`
- `src/common/path.c`
- `src/common/os_compat.c`
- `include/waywal/ipc_proto.h`

### Track D (Test Coverage Gap Analysis & Adversarial Verification)
- `tests/test_phase1.c`
- `tests/test_phase2.c`
- `tests/test_phase3.c`
- `tests/test_phase4.c`
- `tests/test_phase5.c`
- `tests/test_advanced.c`
- `tests/bench_transitions.c`

### Track E (arXiv Literature Research & Academic Grounding)
- Literature search on DRM zero-copy pipelines, io_uring event loops, Landlock/seccomp Linux sandboxing, and video DPB verification.
