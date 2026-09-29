# Handoff Report: Track A Hardware Video & Bitstream Systems Auditor

**Agent**: `teamwork_preview_explorer_track_a` (Hardware Video & Bitstream Systems Auditor)  
**Parent Agent**: `37374eb1-5475-4223-859d-3b2561a3e82f` (parent)  
**Handoff Type**: Hard (Task Complete)  
**Target Artifact**: `/home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_a/audit_report_track_a.md`

---

## 1. Observation

Direct observations from 100% line-by-line reading of the assigned scope files:

1. **Stack write overflow in `unescape_rbsp()`**:
   In `src/daemon/video_vaapi.c:348-357`:
   ```c
   while (si < src_len && di < dst_len) {
       if (si + 2 < src_len && src[si] == 0 && src[si + 1] == 0 && src[si + 2] == 3) {
           dst[di++] = 0;
           dst[di++] = 0;
           si += 3;
       }
   ```
   When `di == dst_len - 1`, the first byte writes at `dst[dst_len - 1]`, and the second unconditionally writes at `dst[dst_len]`, which is outside the bounds of `rbsp[512]` or `rbsp[4096]` on the stack.

2. **Stack array overflow in `parse_h264_slice_header()`**:
   In `src/daemon/video_vaapi.c:560, 598-602`:
   ```c
   sp->num_ref_idx_l0_active_minus1 = (uint8_t)read_ue(&br);
   ...
   for (uint32_t i = 0; i <= sp->num_ref_idx_l0_active_minus1; ++i) {
       sp->luma_weight_l0_flag = (uint8_t)read_bit(&br);
       if (sp->luma_weight_l0_flag) {
           sp->luma_weight_l0[i] = (int16_t)read_se(&br);
   ```
   In `<va/va.h>:3707`, `int16_t luma_weight_l0[32];` is an array of size 32. If `num_ref_idx_l0_active_minus1 >= 32` (read from bitstream), the loop writes out-of-bounds directly into stack memory.

3. **Heap over-read in MP4 sample table parsing**:
   In `src/daemon/demuxer.c:854, 882-888`:
   ```c
   uint32_t stsc_entry_count = read_u32_be(pt.stsc_data + 4);
   const uint8_t *stsc_entries = pt.stsc_data + 8;
   ...
   const uint8_t *ent = stsc_entries + stsc_idx * 12;
   uint32_t next_first = read_u32_be(ent + 12);
   ```
   `stsc_entry_count`, `chunk_count` (`stco`/`co64`), `stts_count`, and `stss_count` are never checked against the respective box payload sizes (`pt.stsc_size`, `pt.stco_size`, `pt.co64_size`, etc.).

4. **PRIME FD leak and uninitialized FD 0 close hazard**:
   In `src/daemon/video_vaapi.c:1251, 1284, 1293`:
   ```c
   memset(out_frame, 0, sizeof(*out_frame));
   ...
   out_frame->num_planes = plane_idx > 0 ? plane_idx : prime_desc.num_objects;
   ```
   Unpopulated `out_frame->fds` remain 0 (`STDIN_FILENO`). If `plane_idx == 0` but `num_objects > 0`, `vaapi_prime_frame_close()` closes standard input (`close(0)`). Furthermore, any exported object FDs in `prime_desc.objects[]` not referenced by layers are never closed and leaked.

5. **Compositor read vs hardware VPP write race**:
   In `src/daemon/video_engine.c:288`:
   ```c
   out->video_buffer_in_use[surf_idx] = true;
   ```
   `out->video_buffer_in_use[surf_idx]` is updated by listener callbacks, but is NEVER checked anywhere in `vaapi_decoder_decode_packet()` or `video_engine_dispatch_frame()`. Candidate target surfaces are selected without verifying whether the Wayland compositor is still actively reading/compositing them.

6. **DPB desynchronization across seamless video loops**:
   In `src/daemon/video_engine.c:241` and `src/daemon/video_vaapi.c:845, 874, 1187`:
   On loop rewind, `seek_us(ve->demuxer, 0)` is called, but `ve->decoder.frame_num` is never reset to 0, and `dec->dpb.count` is only cleared AFTER rendering Frame 0. Frame 0 is rendered with stale reference frames from the end of the previous loop.

7. **VA-API specification violation for `slice_data_bit_offset`**:
   In `src/daemon/video_vaapi.c:687-710`:
   The loop re-adds emulation prevention bytes into `sp->slice_data_bit_offset`, whereas `<va/va.h>:3673` specifies that `slice_data_bit_offset` represents the bit count *after removal of any emulation prevention bytes*.

---

## 2. Logic Chain

1. **From Observation 1 to TRK-A-01**:
   Because `unescape_rbsp()` writes two bytes whenever `si + 2 < src_len && src[si..si+2] == 0 0 3`, a check of only `di < dst_len` permits the second byte to write at `dst[dst_len]`. Since the destination is a fixed-size stack array (`512` or `4096` bytes), this triggers a deterministic stack buffer overflow.
2. **From Observation 2 to TRK-A-02**:
   Because Exp-Golomb integers decoded from untrusted bitstreams can yield values up to 255 for `num_ref_idx_l0_active_minus1`, and because `VASliceParameterBufferH264` declares 32-element arrays, iterating up to `num_ref_idx_l0_active_minus1` without clamping to 31 causes out-of-bounds writes on the stack.
3. **From Observation 3 to TRK-A-03**:
   Because MP4 index box headers declare entry counts independently of the box payload length, reading `count * entry_size` bytes without verifying that `count * entry_size <= box_size - 8` causes the pointer to advance beyond the mapped buffer, resulting in heap over-reads and segmentation faults.
4. **From Observation 4 to TRK-A-04**:
   In C, `memset(..., 0, ...)` sets integer fields to 0, which corresponds to `STDIN_FILENO`. In error or partial-mapping branches of `vaapi_decoder_export_prime()`, `out_frame->fds[i]` remains 0. When passed to `vaapi_prime_frame_close()`, `close(0)` is executed, closing stdin. Unmapped FDs in `prime_desc.objects[]` have no reference in `out_frame` and are leaked.
5. **From Observation 5 to TRK-A-07 & TRK-A-08**:
   Wayland compositors hold DMA-BUF `wl_buffer`s until composite or scanout completion, signaling release asynchronously via `wl_buffer.release`. Because the decoder's surface allocator only checks `dec->dpb.entries[]` and ignores `out->video_buffer_in_use[]`, and because pool size $N=8$ provides fewer surfaces than the worst-case requirement of 16 DPB refs + 1 decode target + 2 compositor in-flight buffers, hardware VPP overwrites surfaces currently being read by the compositor.
6. **From Observation 6 to TRK-A-09**:
   H.264 slice decoding requires `pic_param.frame_num` to match the slice header modulo $2^{\text{log2\_max\_frame\_num\_minus4}+4}$. When a video loops, the bitstream frame number resets to 0, but the decoder's internal counter continues incrementing, and the DPB retains reference pictures from the previous loop, breaking decoder state machine consistency.
7. **From Observation 7 to TRK-A-11**:
   Hardware decoders (e.g. AMD VCN, Intel iHD) rely on `slice_data_bit_offset` to find the start of macroblock data in the RBSP stream. Re-adding emulation prevention bytes causes the hardware to skip valid bitstream data, producing corrupted decodes.

---

## 3. Caveats

- **VA-API Driver Quirks**: Certain proprietary or legacy VA-API drivers historically tolerated non-compliant `slice_data_bit_offset` values or required explicit workarounds. The fix strictly aligns with the official libva specification (`<va/va.h>`).
- **Hardware Codec Availability**: Testing in CI environments lacking a physical DRM render node (`/dev/dri/renderD128`) exercises software fallbacks; hardware ASIC execution requires AMD, Intel, or NVIDIA hardware with VA-API DRM support.

---

## 4. Conclusion

The `wwal` hardware video and bitstream pipeline contains high-impact security vulnerabilities and architectural hazards:
- **Remote / Crash-DoS Vulnerabilities**: Stack buffer overflows in RBSP unescaping (TRK-A-01) and slice weight tables (TRK-A-02), and heap over-reads in MP4 sample table parsing (TRK-A-03).
- **Resource & Lifecycle Hazards**: Stdin closing hazard and DMA-BUF FD leaks (TRK-A-04), `memfd` leaks (TRK-A-06), and unchecked VA buffer creation (TRK-A-05).
- **Pipeline Correctness**: Compositor read / VPP write race conditions (TRK-A-07), surface pool starvation ($N=8$ vs 16 refs, TRK-A-08), loop state desync (TRK-A-09), and VA-API offset specification violations (TRK-A-11).

All 14 findings have been completely analyzed and documented with compilable, drop-in C code fix snippets in `/home/execorn/programming/projects/wwal/.agents/teamwork/teamwork_preview_explorer_track_a/audit_report_track_a.md`.

---

## 5. Verification Method

1. **Codebase Inspection**:
   Examine `audit_report_track_a.md` and review the drop-in C code replacements for each finding against `src/daemon/video_vaapi.c`, `src/daemon/demuxer.c`, and `src/daemon/video_engine.c`.
2. **Build and Test Suite Execution**:
   ```bash
   ninja -C /home/execorn/programming/projects/wwal/build test
   ```
3. **Fuzzing and Malformed Bitstream Verification**:
   Verify that malformed H.264 packets (with emulation prevention byte at offset 511, or `num_ref_idx_l0_active_minus1 > 31`) do not crash the daemon or trigger AddressSanitizer stack-buffer-overflow alerts.
4. **DMA-BUF FD Tracking**:
   Verify with `ls -l /proc/$(pidof wwald)/fd` during video playback that FD count remains strictly constant across video loop transitions and does not leak DMA-BUF prime or `memfd` descriptors.
