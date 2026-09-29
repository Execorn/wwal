# Dispatch for Track C Explorer

**Role**: Linux Security, Sandboxing & IPC Auditor
**Working directory**: /home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_c
**Original request path**: /home/execorn/programming/projects/wwal/.agents/teamwork/ORIGINAL_REQUEST.md
**Prior walkthrough path**: /home/execorn/.gemini/antigravity/brain/828314ee-18c7-4600-b607-54e4f6c254e1/walkthrough.md
**Scope Files (MUST read 100% of these files without sampling)**:
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

**Requirements to Audit**:
- R1: Transcode pipes, IPC client sockets, unclosed FDs on client error paths, memory leaks in image loading or IPC serialization.
- R3: FNV-1a hash key derivation in `gif_transcode.c` (collision safety, stale cache cleanup, TOCTOU races in temp file creation and rename).
- R5: Landlock LSM path permissions (exact necessary R/W/Exec, ABI version changes, ruleset flags).
- R5: Seccomp-BPF allowed syscalls (DRM ioctls, `io_uring_enter`, `futex`, IPC socket calls, PR_SET_NO_NEW_PRIVS, preventing privilege escalation while allowing necessary hardware ops).
- R5: IPC message length checks, framing, payload parsing, null-termination, string handling in `ipc_proto.c` and daemon IPC dispatch for RCE and crash-DoS vectors.

**Deliverables**:
- Write comprehensive report to `audit_report_track_c.md` in your working directory.
- Provide compilable, drop-in C code fix snippets for ALL Critical and High severity findings.
- Write `handoff.md` and send completion message to orchestrator.

## 2026-09-29T20:22:50Z
You are the Linux Security, Sandboxing & IPC Auditor for the wwal codebase.
Your working directory is /home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_c.
Read and follow /home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_c/DISPATCH.md.
Also read /home/execorn/programming/projects/wwal/.agents/teamwork/ORIGINAL_REQUEST.md and /home/execorn/.gemini/antigravity/brain/828314ee-18c7-4600-b607-54e4f6c254e1/walkthrough.md.

You MUST read and analyze 100% of the lines in every file below (NO SAMPLING):
- src/daemon/security.c (293 lines)
- src/client/image_loader.c (601 lines)
- src/client/cli_parser.c (543 lines)
- src/client/main.c (531 lines)
- src/client/gif_transcode.c (258 lines)
- src/common/ipc_proto.c (273 lines)
- src/common/log.c, arena.c, bezier.c, path.c, os_compat.c
- include/waywal/ipc_proto.h

Investigate:
1. R1: Transcode pipes, IPC client sockets, unclosed FDs on client error paths, memory leaks in image loading or IPC serialization.
2. R3: FNV-1a hash key derivation in gif_transcode.c (collision safety, stale cache cleanup, TOCTOU races in temp file creation and rename).
3. R5: Landlock LSM path permissions (exact necessary R/W/Exec, ABI version changes, ruleset flags). Seccomp-BPF allowed syscalls (DRM ioctls, io_uring_enter, futex, IPC socket calls, PR_SET_NO_NEW_PRIVS, preventing privilege escalation while allowing necessary hardware ops). IPC message length checks, framing, payload parsing, null-termination, string handling in ipc_proto.c and daemon IPC dispatch for RCE and crash-DoS vectors.

Format of output:
Write your full findings to /home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_c/audit_report_track_c.md.
For each finding, provide:
- Unique ID (TRK-C-01, etc.)
- Title & Severity (Critical / High / Medium / Low / Informational)
- File and exact line number
- Root cause analysis
- Impact assessment
- Compilable, drop-in C code fix snippet for ALL Critical and High severity findings!
Also write a summary handoff.md and send_message to orchestrator when finished.

