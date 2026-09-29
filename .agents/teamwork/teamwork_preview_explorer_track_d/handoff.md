# Handoff Report: Track D Test Coverage & Verification Auditor

**Agent**: `teamwork_preview_explorer_track_d`  
**Role**: Test Coverage & Verification Auditor  
**Working Directory**: `/home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_d`  
**Date**: 2026-09-29T20:27:50Z  
**Type**: Hard Handoff (Investigation & Deliverables Complete)

---

## 1. Observation

Direct observations from source code, test files, and runtime execution:

1. **Test Suite Execution**: Running `meson test -C build --verbose` exited with code 0 (`Ok: 7, Fail: 0`). Targets executed: `phase1`, `phase2`, `phase3`, `phase4`, `phase5`, `advanced`, `integration`.
2. **IPC Protocol Test Coverage (`tests/test_phase1.c:157–213`)**:
   - Only a single test function `test_ipc_roundtrip()` exists.
   - It only transmits a valid `waywal_ipc_hdr_t` with `magic = WAYWAL_IPC_MAGIC` (0x57415957), `version = WAYWAL_IPC_VERSION` (1), and a single valid memfd descriptor.
   - Zero tests exist for invalid magic, invalid versions, partial headers, unexpected attached FDs, or payload overruns.
3. **DRM & VA-API Hardware Skips (`tests/test_phase2.c:24–29`, `tests/test_phase4.c:545–549`)**:
   - `test_phase2.c`:
     ```c
     bool ok = dmabuf_context_init(&ctx);
     if (!ok) {
         printf("[TEST] DRM node or GBM not available in this environment. Skipping hardware scanout test.\n");
         return;
     }
     ```
   - `test_phase4.c`:
     ```c
     bool init_ok = vaapi_decoder_init(&dec, -1, WAYWAL_CODEC_H264, 1920, 1080);
     if (!init_ok) {
         printf("  [SKIPPED] Hardware VA-API not available or DRM node inaccessible\n");
         return;
     }
     ```
   - In environments without active `/dev/dri/renderD128` nodes (e.g. headless CI), both tests skip immediately without testing buffer allocation, DMA-BUF rings, or video decoding.
4. **VPP Bypass in Test Suite (`tests/test_phase4.c:585–591`)**:
   - `test_vaapi_decoder_and_prime_export` decodes into `surf_p` and exports `surf_p` directly, asserting:
     ```c
     assert(prime_frame.drm_format == DRM_FORMAT_NV12);
     ```
   - It never checks `dec.has_vpp`, never checks `dec.vpp_surfaces`, and never verifies VPP failure fallback.
5. **Output Manager Mocking (`tests/test_phase5.c:142–163`)**:
   - The test manually constructs a stack-allocated node:
     ```c
     output_state_t node1;
     memset(&node1, 0, sizeof(node1));
     ...
     om.head = &node1;
     om.count = 1;
     ...
     om.head = NULL;
     om.count = 0;
     output_manager_destroy(&om);
     ```
   - `output_manager_add()` and `output_manager_remove()` in `src/daemon/output_state.c` are NEVER called.
6. **Bitstream Parser & NAL Scanner Vulnerabilities (`src/daemon/video_vaapi.c:915–992`)**:
   - Line 949: `if (scan_off + nal_hdr_len + nal_len <= pkt->size)` — integer overflow occurs if `nal_len > 0xFFFFFFF0`.
   - Line 992: `scan_off += nal_hdr_len + nal_len;` — zero advancement loop if `nal_len == 0`.
   - `tests/test_phase4.c` tests only a single hardcoded 32-byte valid NAL (`dummy_nalu`).
7. **Security Sandbox Verification (`tests/test_phase5.c:169–215`)**:
   - Child calls `security_sandbox_apply(NULL)`. Seccomp execution denial on `execve` is tested via `WTERMSIG(status) == SIGSYS`.
   - Landlock filesystem write restrictions to unauthorized directories (e.g. `/etc`, `/usr`) are never asserted.
   - Fallback when Landlock or Seccomp returns `ENOSYS` on older kernels is never tested.
8. **GIF Frame Parser (`tests/test_advanced.c:343–392`)**:
   - Tests one valid 54-byte GIF89a file and one 7-byte invalid header file.
   - Zero-width or zero-height frames (`img_w = 0`, `img_h = 0`), truncated LZW streams, and malformed extension headers are unexercised.

---

## 2. Logic Chain

1. **Observation 1 & 2** demonstrate that although tests pass cleanly, tests only exercise happy paths with compliant inputs.
2. **Observation 2 & 6** establish that IPC and video bitstream parsing lack negative validation. In `ipc_proto.c` and `video_vaapi.c`, malformed inputs (invalid magic, integer wrap-arounds in NAL lengths, truncated packets) could cause infinite loops, memory corruption, or silent failure in production.
3. **Observation 3** shows that hardware tests skip when DRM/VA-API nodes are unavailable. In containerized CI without GPU passthrough, core rendering and decode logic receives 0% coverage.
4. **Observation 4** reveals that VPP color conversion is never verified: the test suite explicitly verifies NV12 export and ignores VPP. In `video_engine.c`, if VPP fails, dirty VPP surfaces are exported without fallback notification.
5. **Observation 5** establishes that the output hotplug test is purely cosmetic. Because `output_manager_remove()` is never called, regressions in output hot-unplug resource reclamation or DMA-BUF ring destruction cannot be caught by CI.
6. **Observation 7** proves that Landlock sandboxing is unverified: the test only checks Seccomp.
7. **Observation 8** confirms that GIF decoder edge cases (0x0 frames, truncated LZW blocks) are unexercised.
8. Therefore, the nominal 100% test pass rate provides a false sense of security; significant gaps exist across all 6 targeted edge cases.

---

## 3. Caveats

- **VA-API Hardware Dependency**: Live execution of VA-API decoding and VPP depends on host GPU driver support (tested successfully on Mesa Gallium 26.2.3 / AMD Radeon VCN 2.0). On systems without compatible AMD/Intel hardware or drivers, tests skip as designed.
- **Kernel Version Dependency**: The host kernel is Linux 7.2.7-zen with Landlock ABI v10. Native testing of Landlock `ENOSYS` fallback on kernels < 5.13 requires either syscall interposition (e.g. via `ptrace` or `LD_PRELOAD`) or an older kernel VM.

---

## 4. Conclusion

The `wwal` test suite provides solid baseline coverage for algorithmic primitives (Bézier evaluation, SIMD blend bit-exactness, basic MP4 box parsing, and CLI flags), but has critical blind spots in **negative testing, fault injection, and hardware failure modes**:
1. **Critical Gaps**: Malformed IPC messages (GAP-06), video packet length overflow & Exp-Golomb EOF loops (GAP-02, GAP-03), and mocked output hotplug (GAP-04).
2. **High Gaps**: VPP failure fallback (GAP-01), DRM node disconnection (GAP-05), malformed GIF frames (GAP-07), Landlock filesystem write denial (GAP-09), and video engine state machine lifecycle (GAP-11).
3. **Remediation**: Five complete, drop-in C test implementations have been formulated and documented in `audit_report_track_d.md` (Section 5) ready for insertion into the test suite.

---

## 5. Verification Method

To independently verify all findings and test suite behaviors:

1. **Run Full Test Suite**:
   ```bash
   meson test -C /home/execorn/programming/projects/wwal/build --verbose
   ```
   *Expected Result*: All 7 targets pass (`Ok: 7, Fail: 0`).
2. **Verify Skipped Hardware Paths on Headless CI**:
   Run tests with unreadable DRM nodes:
   ```bash
   WAYLAND_DISPLAY=none /home/execorn/programming/projects/wwal/build/tests/test_phase2
   ```
   *Expected Result*: Observe skip message: `[TEST] DRM node or GBM not available in this environment. Skipping hardware scanout test.`
3. **Inspect Output Manager Mock**:
   View lines 132–167 of `/home/execorn/programming/projects/wwal/tests/test_phase5.c` to confirm stack struct assignment and lack of `output_manager_add`/`remove` calls.
4. **Inspect VPP Bypass in Phase 4**:
   View line 591 of `/home/execorn/programming/projects/wwal/tests/test_phase4.c` to confirm that `prime_frame.drm_format == DRM_FORMAT_NV12` is asserted directly, bypassing VPP.
5. **Inspect Audit Report & Drop-In Tests**:
   Inspect `/home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_d/audit_report_track_d.md`.
