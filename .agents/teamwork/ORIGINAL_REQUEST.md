# Original User Request

## 2026-09-29T20:20:02Z

Exhaustive zero-compromise systems code and architecture audit of the `wwal` wallpaper daemon codebase (~15,000 lines of C23). The codebase implements a Wayland wallpaper engine (`wwald` daemon + `wwal` CLI client) with VA-API hardware video decode/VPP, DMA-BUF zero-copy compositor integration, io_uring event loops, Landlock LSM + Seccomp-BPF sandboxing, and JIT GIF-to-H.264 transcode caching. The audit must produce a structured report covering 6 vectors with concrete C code remediation snippets for all critical/high findings. Integrate arXiv literature search for VA-API/DRM zero-copy optimization, io_uring best practices, and Landlock/Seccomp hardening patterns — cite relevant papers in the report.

Working directory: /home/execorn/programming/projects/wwal
Integrity mode: development

## Reference Material

### Prior Audit Walkthrough (must read before auditing)
Located at: `/home/execorn/.gemini/antigravity/brain/828314ee-18c7-4600-b607-54e4f6c254e1/walkthrough.md`

Key architectural context from the walkthrough:
1. Animated GIFs are JIT-transcoded to H.264 MP4 in the unprivileged client (`wwal`), cached via FNV-1a content-addressed hashing at `$XDG_CACHE_HOME/wwal/transcoded/<hash>.mp4`
2. Video FDs are passed from client to daemon via `SCM_RIGHTS` over Unix domain sockets, bypassing the daemon's filesystem sandbox
3. The daemon (`wwald`) runs under strict Landlock LSM + Seccomp-BPF + `PR_SET_NO_NEW_PRIVS` — deliberately forbids `execve`
4. VA-API hardware decode into DMA-BUF surfaces with zero CPU copies to Wayland compositor
5. Previously fixed bugs: MP4 VisualSampleEntry sub-box offset (86-byte header vs 36), hardcoded NAL offsets, missing chroma_format_idc/num_ref_frames in pic_param
6. Transcode uses `-bf 0 -slices 1` to eliminate B-frames and `-x264-params repeat-headers=1` for in-band SPS/PPS
7. Build system: Meson (`gnu2x` C standard, `-O3`, LTO, PIE)

### Codebase Structure
```
src/daemon/   — wwald daemon (~8K LoC)
  video_vaapi.c (1314 lines) — VA-API H.264 decode, DPB management, VPP color conversion, PRIME export
  demuxer.c (1031 lines) — MP4/ISOBMFF container parser, avcC extraction, sample table parsing
  video_engine.c (423 lines) — Frame pacing, decode orchestration, buffer management
  layer_shell.c (1133 lines) — wlr-layer-shell surface management, output binding
  render_engine.c (951 lines) — DMA-BUF wl_buffer creation, frame submission
  gpu_compute.c (790 lines) — GPU compute shaders
  main.c (661 lines) — Daemon entry, Wayland registry, global state
  wayland_core.c (494 lines) — Wayland display connection management
  dmabuf_alloc.c (395 lines) — GBM buffer allocation, DRM device management
  slideshow.c (372 lines) — Slideshow timer logic
  security.c (293 lines) — Landlock LSM + Seccomp-BPF sandbox
  output_state.c (213 lines) — Output hotplug state machine
  dmabuf_feedback.c (184 lines) — DMA-BUF feedback handling
  presentation.c (175 lines) — wp_presentation PLL frame pacing
  uring_loop.c (176 lines) — io_uring event loop

src/client/   — wwal CLI client (~2K LoC)
  image_loader.c (601 lines) — Image loading and processing
  cli_parser.c (543 lines) — CLI argument parsing
  main.c (531 lines) — Client entry, IPC dispatch
  gif_transcode.c (258 lines) — GIF→H.264 JIT transcode with content-addressed cache

src/common/   — Shared utilities (~700 LoC)
  ipc_proto.c (273 lines) — IPC protocol serialization
  log.c, arena.c, bezier.c, path.c, os_compat.c

include/waywal/   — Public headers
  vaapi_dec.h, demuxer.h, video_engine.h, dmabuf.h, ipc_proto.h, etc.

tests/   — Test suites (~2.3K LoC)
  test_phase1.c through test_phase5.c, test_advanced.c, bench_transitions.c
```

## Requirements

### R1. Memory Safety, Leak Analysis & FD Hygiene Audit
Trace all file descriptors (DMA-BUF prime FDs, DRM render node FDs, timerfd, IPC client sockets, transcode pipes) through every code path including error returns. Verify no FD leaks on error, no double-close in `vaapi_prime_frame_close()` duplicate elimination. Verify every `VABufferID` allocated via `vaCreateBuffer()` is destroyed via `vaDestroyBuffer()` on all paths. Check VA surface destruction completeness in `vaapi_decoder_destroy()` for both `surfaces[]` and `vpp_surfaces[]`. Identify any unfreed heap allocations on loop restarts, output disconnection, or video seek/loop wrap.

### R2. Hardware Video Pipeline & DPB State Machine Correctness
Verify DPB target surface selection (`current_surface_idx`) never collides with active reference surfaces in `dec->dpb.entries[]`. Audit FIFO sliding-window eviction bounds against `max_refs` and `WAYWAL_VA_SURFACE_POOL_SIZE`. Audit the bitstream reader (`bit_reader_t`), Exp-Golomb decoders (`read_ue`, `read_se`), and `unescape_rbsp()` for buffer over-reads, integer overflows, and malformed bitstream handling. Verify NAL start-code detection under both 3-byte and 4-byte prefixes. Check VPP synchronization via `vaSyncSurface()` — verify compositor reads don't conflict with VPP rendering and that pool size (N=8) provides sufficient headroom.

### R3. Performance, Zero-Copy & Bottleneck Analysis
Identify any heap allocations, heavy syscalls, or redundant state queries inside `video_engine_render_frame()`. Verify frame pacing accuracy to discrete VBlank multiples via `presentation_pll` without clock drift. Check that `ensure_output_video_buffer()` correctly caches and reuses wl_buffers across loop iterations. Audit hash key derivation in `gif_transcode.c` for collision safety, stale cache cleanup, and TOCTOU races.

### R4. Concurrency, io_uring Event Loop & Signal Safety
Audit `uring_loop.c` for SQE submission overflows, CQE processing correctness, multishot poll re-arming, and clean cancellation on shutdown. Verify `SIGINT`/`SIGTERM`/`SIGHUP` handlers are async-signal-safe. Check that graceful cleanup doesn't hang on blocked ring waits.

### R5. Sandboxing & Security Enforcement
Audit Landlock LSM path permissions — verify exact necessary Read/Write/Exec without breaking under ABI version changes. Audit Seccomp-BPF allowed syscalls — verify DRM ioctls, `io_uring_enter`, `futex`, and IPC socket calls are permitted while blocking privilege escalation. Audit IPC message length checks, payload parsing, null-termination, and string handling in `ipc_proto.c` and the daemon's IPC dispatch for RCE and crash-DoS vectors.

### R6. Test Coverage Gap Analysis
Examine all test files (`test_phase1.c` through `test_phase5.c`, `test_advanced.c`). Determine if edge cases are adequately covered: VPP failure fallback, DRM node disconnection, corrupted video packets, zero-length GIF frames, malformed IPC messages, Landlock/Seccomp fallback on older kernels.

### R7. ArXiv Literature Research
Search arXiv for papers on: VA-API/DRM zero-copy pipeline optimization, io_uring event loop design patterns, Landlock LSM and Seccomp-BPF hardening best practices, H.264 DPB management correctness. Cite relevant findings in the audit report where they inform recommendations.

## Acceptance Criteria

### Audit Report Completeness
- [ ] Report contains an Executive Summary with overall code health assessment
- [ ] Every issue found has: unique ID, file:line location, root cause analysis, impact assessment, severity rating (Critical/High/Medium/Low/Informational)
- [ ] All Critical and High severity issues include concrete C code fix snippets (compilable, drop-in replacements)
- [ ] Report covers all 6 audit vectors (R1–R6) with explicit findings or explicit "no issues found" statements per sub-item
- [ ] Architecture & Scalability Recommendations section is present with forward-looking suggestions

### Audit Depth Verification
- [ ] Every file listed in the codebase structure above has been read and analyzed (not just sampled)
- [ ] FD lifecycle tracing covers at minimum: DMA-BUF prime FDs, DRM render node FD, timerfd, IPC client sockets, and transcode cache FDs
- [ ] VA-API buffer lifecycle tracing covers all `vaCreateBuffer`/`vaDestroyBuffer` pairs including error paths
- [ ] Bitstream parser (`bit_reader_t`, `read_ue`, `read_se`, `unescape_rbsp`) audited for all boundary conditions
- [ ] Security sandbox audit covers both Landlock path permissions AND Seccomp-BPF syscall allowlist
- [ ] IPC input validation audit covers message framing, length bounds, null-termination, and type confusion

### Literature Integration
- [ ] At least 3 relevant arXiv papers cited in the report with URLs
- [ ] Paper findings connected to specific code recommendations (not just listed)

### Report Format
- [ ] Report uses the exact markdown structure specified: Executive Summary → Critical Vulnerabilities → Efficiency & Bottlenecks → Hardening & Code Quality → Architecture Recommendations
- [ ] Report saved as a markdown artifact file
