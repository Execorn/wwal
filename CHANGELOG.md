# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Native Wallpaper Engine Workshop runtime (`we_pkg.c`, `we_tex.c`, `we_mdl.c`, `we_shader.c`, `we_scene.c`, `we_particle.c`, `we_script.c`, `we_workshop.c`): zero-extraction `mmap` `PKGV0001..22` archives, hardware `BC1`/`BC2`/`BC3`/`BC7` and `LZ4` `TEXV0005` textures, 2D Puppet Warp skeletal `.mdl` meshes, AVX2 Structure-of-Arrays particle simulation, multi-pass GLSL effect chains, sandboxed `SceneScript` VM, and `wwal workshop list / info / apply`.
- Multi-pass Shadertoy rendering (`Buffer A`..`Buffer D` `GL_RGBA16F` ping-pong passes, `iChannel0`..`iChannel3` bindings, `#pragma waywal`, hardware derivatives, `iMouse`, `iDate`, and live `u_palette_*` uniforms).
- 2.5D depth-map parallax wallpaper mode (`--depth-map <PATH>` and `--depth-strength <FLOAT>`) and 4-tier Wayland cursor and workspace-slide spring parallax.
- Universal live-to-live cross-mode GPU transitions across static images, hardware videos, live shaders, and Workshop scenes, plus panoramic `--scaling-mode span`.
- Event-driven D-Bus `MPRIS2` integration (`mpris_bridge.c`) exposing `u_album_art`, `u_media_playing`, `u_media_position`, `u_media_Color0..4`, and `--theme-from-media` dynamic album art theming.
- PipeWire background audio volume control and automatic external playback ducking (`--volume`, `--mute-on-audio`, `wwal audio`).
- Rich JSON IPC inspection (`wwal status [--json]`, `wwal query --json`, `wwal stats --json`), live uniform and Workshop property mutation (`wwal set-uniform`), and newline-delimited JSON event streaming (`wwal subscribe`).
- `zwlr_foreign_toplevel_manager_v1` occlusion tracking (`protocols/wlr-foreign-toplevel-management-unstable-v1.xml`) and `--battery-policy` CLI flag.
- WebM `EBML_ID_DURATION` parsing, Xiph/Fixed/EBML block lacing expansion, and MP4 `ctts` v0/v1 composition time offset support in `demuxer.c`.

### Fixed

- Backward-compatible IPC header parsing in `WAYWAL_REQ_SET_IMAGE` for legacy `waywal_img_metadata_t` payloads.
- Deduplicated client/daemon HCT palette extraction, theme file exports, `/dev/pts/*` broadcasts, and hook execution on `wwal img`, while wiring daemon-side theme/hook updates and `SIGHUP` directory rescanning into `slideshow.c`.
- Preserved per-effect wave/blur defaults in `crosszoom`, `slide`, `glitch`, `burn`, `ripple`, and `wave`, fixed `--transition-type simple` and `random`, and eliminated double Cubic-Bezier easing.
- Added `in_flight` poll SQE tracking in `uring_loop.c` and forced `epoll`/`io_uring` re-registration on compositor IPC reconnect.

### Performance

- Vectorized AVX2 and AVX-512 BW scanline alpha blending without per-scanline `_mm_sfence()` stalls, cached GLSL uniform locations across all 19 transition compute shaders, and eliminated redundant 4K RGBA heap buffers in `video_engine.c` and `main.c`.

## [1.0.0] - 2026-10-26

### Added

- **Wayland Core & Direct Scanout (`wwald`)**:
  - `zwp_linux_dmabuf_v1` v4 surface feedback negotiation supporting linear and tiled GPU modifiers (NVIDIA Block-Linear, AMD DCC, Intel CCS, ARM AFBC) and 10-bit wide-gamut formats (`XRGB2101010` / `ARGB2101010`).
  - `wp_single_pixel_buffer_v1` 1x1 solid color scanout (`wwal clear` / `wwal color`).
  - `wp_fractional_scale_v1` and `wp_viewporter` physical HiDPI rendering (`scale_120`) without compositor upscaling blur.
  - `wp_presentation` VBlank Phase-Locked Loop (PLL) for tear-free frame pacing on fixed-refresh and VRR displays.
  - Secondary `zwlr_layer_surface_v1` overview backdrop surface (`--backdrop auto-blur`) using a 2-pass Dual-Kawase compute blur for Niri and scrollable tiling compositors.
  - Multi-GPU render-node discovery with linear PRIME export fallback and hotplug-safe `wl_output` lifecycle management.
- **GPU Compute & CPU SIMD Transition Engine**:
  - 18 OpenGL ES 3.1 compute shader transitions (`fade`, `wipe`, `grow`, `outer`, `wave`, `noise`, `crosszoom`, `slide`, `glitch`, `burn`, `ripple`, `pixelate`, `doom`, `swirl`, `cube`, `luma`, `light_leak`, `page_curl`) plus custom `.comp` shader loading (`--transition-shader`).
  - Multi-threaded OpenMP + runtime-dispatched AVX-512 (`vbmi2`), AVX2, NEON, and scalar CPU transition kernels.
  - Aspect-ratio scaling modes (`fill`, `fit`, `stretch`, `center`, `tile`) with Lanczos3, Mitchell, Catmull-Rom, Bilinear, and Nearest resampling filters.
- **Zero-FFmpeg Hardware Video & Native Animation Engine**:
  - In-process MP4/ISOBMFF and WebM/Matroska EBML zero-allocation demuxers (`demuxer.c`).
  - Hardware VA-API (`libva`) decoding for H.264, HEVC (Main & Main10), VP9 (Profile 0 & 2), and AV1 (Main Profile) with direct `NV12`/`P010` DMA-BUF export on AMD/Intel and a 3-surface `BGRX` VPP ring on NVIDIA.
  - Native in-process delta-frame animation engine (`anim_engine.c`) for animated GIF, APNG, and animated WebP without external transcoding dependencies.
- **Live Shadertoy & PipeWire Audio-Reactive Engine**:
  - Live GLSL ES 3.1 `.comp` and Shadertoy `.frag` wallpaper engine (`wwal shader`) with `OES_get_program_binary` disk caching and `inotify` hot-reload on file save.
  - Lock-free PipeWire audio capture and 2048-point Hann-windowed AVX2 radix-2 FFT producing 64-bin Bark-scale spectrum uniforms and an `iChannel0` audio texture (`--audio`).
- **Material Design 3 (`OKLab` / `CAM16` / `HCT`) Theme Engine**:
  - Sub-millisecond AVX2 Wu 3D variance quantization and weighted K-Means++ palette extraction (`wwal theme` / `--theme`).
  - Automatic generation of 29 Material Design 3 dark and light role tokens synced to `~/.local/state/caelestia/scheme.json` (Quickshell), `~/.cache/wwal/colors.{json,css,hyprland.conf}`, and active `/dev/pts/*` terminal emulators via non-blocking OSC 4/10/11 sequences.
- **Zero-Watt Power & Occlusion Governor**:
  - Automatic 0.00% CPU/GPU timer disarmament when a monitor is occluded by a fullscreen or maximized window (`ext-foreign-toplevel-list-v1` + Hyprland/Sway IPC sockets), when the user session is idle (`ext-idle-notify-v1`), or on laptop battery power (`/sys/class/power_supply`).
- **Client CLI, Persistence & Sandbox Hardening**:
  - Drop-in CLI compatibility when invoked as `swww`, `swww-daemon`, `awww`, or `awww-daemon`.
  - Support for PNG, JPEG, BMP, WebP, AVIF (including multi-frame macOS Solar Dynamic schedules), JPEG-XL, GIF, APNG, and SVG.
  - CRC32-verified binary state persistence (`state.bin`), declarative `~/.config/wwal/config.toml` parser with per-output and per-workspace rules, and real-time telemetry (`wwal stats`).
  - Linux `io_uring` event loop (`uring_loop.c`) and Seccomp-BPF + Landlock v3 process sandboxing (`security.c`).

[Unreleased]: https://github.com/Execorn/wwal/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Execorn/wwal/releases/tag/v1.0.0
