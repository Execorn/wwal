# Dispatch for Track A Explorer

**Role**: Hardware Video & Bitstream Systems Auditor
**Working directory**: /home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_a
**Original request path**: /home/execorn/programming/projects/wwal/.agents/teamwork/ORIGINAL_REQUEST.md
**Prior walkthrough path**: /home/execorn/.gemini/antigravity/brain/828314ee-18c7-4600-b607-54e4f6c254e1/walkthrough.md
**Scope Files (MUST read 100% of these files without sampling)**:
- `src/daemon/video_vaapi.c`
- `src/daemon/demuxer.c`
- `src/daemon/video_engine.c`
- `include/waywal/vaapi_dec.h`
- `include/waywal/demuxer.h`
- `include/waywal/video_engine.h`

**Requirements to Audit**:
- R1: Memory Safety, Leak Analysis & FD Hygiene (DMA-BUF prime FDs, `vaapi_prime_frame_close` double-close, `vaCreateBuffer`/`vaDestroyBuffer` completeness, surface destruction in `vaapi_decoder_destroy`, unfreed heap allocations on loop restarts/seek/wrap).
- R2: Hardware Video Pipeline & DPB State Machine Correctness (DPB target surface selection `current_surface_idx`, collisions with active references in `dec->dpb.entries[]`, FIFO sliding-window eviction bounds against `max_refs` and `WAYWAL_VA_SURFACE_POOL_SIZE`. Bitstream reader `bit_reader_t`, `read_ue`, `read_se`, `unescape_rbsp` buffer over-reads, integer overflows, malformed bitstream handling, 3-byte and 4-byte NAL prefixes, VPP synchronization via `vaSyncSurface()`, pool size N=8 headroom).
- R3: Performance & Zero-Copy (heap allocations/heavy syscalls in `video_engine_render_frame`).

**Deliverables**:
- Write comprehensive report to `audit_report_track_a.md` in your working directory.
- Provide compilable, drop-in C code fix snippets for ALL Critical and High severity findings.
- Write `handoff.md` and send completion message to orchestrator.

## 2026-09-29T20:22:50Z
You are the Hardware Video & Bitstream Systems Auditor for the wwal codebase.
Your working directory is /home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_a.
Read and follow /home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_a/DISPATCH.md.
Also read /home/execorn/programming/projects/wwal/.agents/teamwork/ORIGINAL_REQUEST.md and /home/execorn/.gemini/antigravity/brain/828314ee-18c7-4600-b607-54e4f6c254e1/walkthrough.md.

You MUST read and analyze 100% of the lines in every file below (NO SAMPLING):
- src/daemon/video_vaapi.c (1314 lines)
- src/daemon/demuxer.c (1031 lines)
- src/daemon/video_engine.c (423 lines)
- include/waywal/vaapi_dec.h
- include/waywal/demuxer.h
- include/waywal/video_engine.h

Investigate:
1. R1: Memory Safety, Leak Analysis & FD Hygiene. Trace all FDs (DMA-BUF prime FDs, DRM render node FDs). Check error returns for leaks. Verify vaapi_prime_frame_close() duplicate elimination logic for double close. Verify every vaCreateBuffer() has a matching vaDestroyBuffer() on all paths (including failure mid-frame). Check VA surface destruction in vaapi_decoder_destroy() for surfaces[] and vpp_surfaces[]. Check heap leaks on video loop wrap/seek.
2. R2: Hardware Video Pipeline & DPB State Machine. Check target surface selection current_surface_idx — does it ever collide with active reference surfaces in dec->dpb.entries[]? Check FIFO sliding-window eviction bounds against max_refs and WAYWAL_VA_SURFACE_POOL_SIZE. Audit bit_reader_t, read_ue, read_se, and unescape_rbsp() for buffer over-reads, integer overflows, malformed bitstreams. Check 3-byte and 4-byte NAL start codes. Check VPP synchronization vaSyncSurface(), compositor read vs VPP render race, pool size headroom.
3. R3: Allocations / heavy syscalls inside video_engine_render_frame().

Format of output:
Write your full findings to /home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_a/audit_report_track_a.md.
For each finding, provide:
- Unique ID (TRK-A-01, etc.)
- Title & Severity (Critical / High / Medium / Low / Informational)
- File and exact line number
- Root cause analysis
- Impact assessment
- Compilable, drop-in C code fix snippet for ALL Critical and High severity findings!
Also write a summary handoff.md and send_message to orchestrator when finished.
