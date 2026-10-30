# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.2.0] - 2026-10-30

### Added

- **Hardware TMU Noise Intrinsics (`u_noise_tex`)**: Persistent `256×256` `GL_RGBA8` bilinear wrap-repeat noise LUT bound to `GL_TEXTURE5` with golden-ratio channel offsets (`G = R(x+37, y+17)`, `B = R(x+113, y+53)`) and built-in `waywal_noise2d`, `waywal_noise2d_dual`, and `waywal_noise3d` GLSL helpers in both fragment and compute shader prologues.
- **Multi-Rate & Sub-Resolution `BUFFER_A..D` Passes**: Added `@wwal_buffer_scale` (`0.1`–`1.0`) and `@wwal_buffer_fps` (`0`–`240`, with `0` baking static buffer passes once on frame `0` or resize) comment annotations and `#pragma waywal` directives for multi-pass Shadertoy shaders.
- **Multi-Resolution Transition Scratch Pool**: Added a 4-slot resolution-keyed `GL_RGBA8` scratch texture pool in `gpu_compute.c` for zero-thrash mixed-resolution multi-monitor (`1080p` + `1440p` + `4K`) transitions.

### Fixed

- Fixed OpenGL ES 3.1 `smoothstep(edge0, edge1, x)` inverted-edge undefined behavior (`edge0 >= edge1`) across all 10 built-in `.frag` shader presets and 10 `.comp` transition shaders (`burn`, `ripple`, `slide`, `page_curl`, `pixelate`, `luma`, `wipe`, `wave`, `fade`, `doom`).
- Fixed multi-monitor image transition state cache invalidation when switching scratch resolutions across outputs (`ensure_10bit_scratch_target` and `blit_scratch_to_10bit_bo`).
- Fixed cross-monitor Wayland pointer coordinate derivation (`daemon_query_cursor_pos`) and applied smoothed AGC gain (`fft->current_gain`) in `waywal_audio_get_effective_volume`.

### Performance

- **Up to 5.68× Faster Built-In Shaders & 46% Lower Multi-Monitor GPU Power**: Replaced multi-octave procedural ALU noise and `sin()` hashes with hardware TMU `waywal_noise2d` / `waywal_noise2d_dual` fetches, hoisted loop-invariant trigonometric rotation matrices out of raymarching and layer loops, added transmittance early-exit in `aurora.frag` (`2.55 ms` → `0.45 ms` at `1080p`, `28.5 W` → `15.5 W` across 3 monitors), analytic bounding-sphere ray pre-culling in `plasma_orb.frag`, and $O(1)$ active-bar indexing in `audio_bars.frag` and `audio_ring.frag`.
- **Same-Resolution Multi-Monitor Shader Clone Blit**: Outputs sharing identical resolutions and a mouse-independent shader now blit the rendered texture (`waywal_shader_engine_blit_bo`) instead of re-executing the fragment/compute shader per monitor.
- **Zero-Stall GLES 3.1 Driver Dispatch**: Eliminated per-frame `glGetIntegerv` driver stalls in `shader_engine.c` and `we_tex.c`, gated unused `glGenerateMipmap`, `iDate` `localtime_r`, `audio_tex` uploads, and `_rt_imageLayerComposite` FBO blits in `we_scene.c`, eliminated 60 Hz Hyprland cursor IPC polling for mouse-independent shaders, and batched `wl_display_flush` once per frame tick.

## [1.1.0] - 2026-10-28

### Added

- **100% `swww` & `awww` v0.12.1 Drop-In Compatibility**: Complete multi-call binary parity (`swww`, `swww-daemon`, `awww`, `awww-daemon`), exact `swww query` / `awww query` output format (`[<namespace>: ]<output>: <W>x<H>, scale: <S>, currently displaying: image: <path>|color: <hex>`), `SWWW_TRANSITION*` / `AWWW_TRANSITION*` environment variables, directional `--crop-gravity` (`center`/`north`/`south`/`east`/`west`/`northeast`/`northwest`/`southeast`/`southwest` or `X,Y`), `--invert-y`, `--no-cache`, `--clear-cache`, `-a` / `--all`, solid hex color `img` (`#RRGGBB` / `0xRRGGBB`), universal `img` media auto-dispatch (static images, animated GIF/APNG/WebP, videos, `.frag`/`.comp` shaders, Steam Workshop directories/IDs, dynamic wallpaper XML/JSON manifests, and directories), automatic `wwald` daemon spawning on `img`/`restore`/`init`, and `-Dswww_compat=true` Meson install symlinks.
- **Universal Lockscreen & Dotfile Cache Sync, Launcher Picker & Workshop Bridge**: Zero-config lockscreen and dotfile synchronization (`~/.cache/wwal/current_wallpaper`, fast uncompressed PNG `~/.cache/wwal/current_wallpaper.png` for `hyprlock`/`swaylock`, and NUL-delimited `~/.cache/swww/<output>` + `~/.cache/awww/<output>` caches), unified interactive launcher picker (`wwal pick` with `rofi`/`fuzzel`/`wofi`/`walker`/`tofi`/`dmenu` auto-detection, `\0icon\x1f` thumbnails, `--cmd`, `--print-only`, `--json`, and `--dir`), and Steam Workshop preview symlink bridge (`wwal workshop link-previews` / `sync-previews`) for GUI wallpaper pickers (`waypaper`, `azote`, `variety`).
- **Superset `matugen` + `wallust` + `pywal` Template Engine**: Zero-dependency `[[templates]]` / `[template.<name>]` rendering engine supporting 42 Material Design 3 HCT color tokens (`colors.<role>.<mode>.<prop>` and `palettes.<palette>.<tone>.<prop>`), `wallust` / `pywal` variables (`background`, `foreground`, `cursor`, `color0`..`color15` with `.strip`, `.rgb`, `.rgba`, `.hex`, `.octal`, `.red_dec`), pipe filters (`| strip`, `| rgb`, `| rgba(alpha)`, `| lighten(pct)`, `| darken(pct)`, `| saturate(pct)`, `| alpha(val)`), custom delimiters, `--source-color-index` / `--seed-color` overrides, atomic symlink-preserving file writes, and asynchronous `post_hook` execution.
- **Declarative `wpaperd` Superset, Mixed-Media Slideshows & Per-Workspace Live Media**: Full `wpaperd` TOML configuration superset (`[default]`, `[any]`, and top-level `[DP-1]` / `[HDMI-A-1]` output sections, human-readable durations like `duration = "30m"` or `"1h30m"`, `sorting = "ascending" | "descending" | "mtime" | "random"`, `recursive = true/false`, and `mode = "fit-border-color"`), mixed-media directory slideshows (`image` + `video` + `shader` + `workshop` with non-repeating Fisher-Yates shuffle and `wwal next` / `wwal prev`), and per-workspace live media rules (`video`, `shader`, `workshop`, `slideshow`, `image`, `color`) with per-workspace transition overrides.
- **Native NOAA Solar & Time-of-Day Dynamic Wallpapers**: Astronomical NOAA solar position calculator (`solar_schedule.c` computing solar elevation, azimuth, civil/nautical/astronomical twilight, sunrise, solar noon, and sunset from `[solar]` `latitude`/`longitude`), declarative `[[schedule]]` phase/clock rules (`dawn`, `morning`, `noon`, `afternoon`, `day`, `dusk`, `sunset`, `evening`, `night`, `midnight`, or `HH:MM`), native GNOME Timed Background XML (`<background><starttime>...`) and KDE/JSON (`metadata.json`) dynamic wallpaper loader with GPU cross-fade factor computation, and automatic solar/time-of-day dark/light theme switching (`--auto-day-night` / `mode = "auto"`).
- **Wallpaper Engine Sound Objects, Text Layers & 10 Built-In Procedural Shaders**: Native Steam Wallpaper Engine `"sound"` scene object mixer (`playbackmode` loop/random/single, `mintime`/`maxtime` intervals, WAV/OGG/MP3/FLAC decoding into PipeWire with volume/occlusion/mute-on-audio governance), `"text"` layers rendered via a zero-dependency bitmap font rasterizer driven by `SceneScript` ES6 template literals, `Date`/`Math`/string methods, and `update()` timers, plus 10 curated built-in procedural GLSL ES 3.10 shader presets (`aurora`, `cyber_grid`, `fluid_ink`, `synthwave_sun`, `starfield`, `rain_glass`, `audio_bars`, `audio_ring`, `crt_retro`, `plasma_orb`) accessible via `wwal shader list [--json]` and `wwal shader [preset] <name>`.
- Native Wallpaper Engine Workshop runtime (`we_pkg.c`, `we_tex.c`, `we_puppet.c`, `we_shader.c`, `we_scene.c`, `we_particle.c`, `we_script.c`): zero-extraction `mmap` `PKGV0001..22` archives, hardware `BC1`/`BC2`/`BC3`/`BC7` and `LZ4` `TEXV0005` textures, 2D Puppet Warp skeletal `.mdl` meshes, AVX2 Structure-of-Arrays particle simulation, multi-pass GLSL effect chains, sandboxed `SceneScript` VM, and `wwal workshop list / info / apply`.
- Multi-pass Shadertoy rendering (`Buffer A`..`Buffer D` `GL_RGBA16F` ping-pong passes, `iChannel0`..`iChannel3` bindings, `#pragma waywal`, hardware derivatives, `iMouse`, `iDate`, and live `u_palette_*` uniforms).
- 2.5D depth-map parallax wallpaper mode (`--depth-map <PATH>` and `--depth-strength <FLOAT>`) and Wayland cursor and workspace-slide spring parallax.
- Universal live-to-live cross-mode GPU transitions across static images, hardware videos, live shaders, and Workshop scenes, plus panoramic `--scaling-mode span`.
- Event-driven D-Bus `MPRIS2` integration (`mpris.c`) exposing `u_album_art`, `u_media_playing`, `u_media_position`, `u_media_Color0..4`, and `--theme-from-media` dynamic album art theming.
- PipeWire background audio volume control and automatic external playback ducking (`--volume`, `--mute-on-audio`, `wwal audio`).
- Rich JSON IPC inspection (`wwal status [--json]`, `wwal query --json`, `wwal stats --json`), live uniform and Workshop property mutation (`wwal set-uniform`), and newline-delimited JSON event streaming (`wwal subscribe`).
- `zwlr_foreign_toplevel_manager_v1` occlusion tracking (`protocols/wlr-foreign-toplevel-management-unstable-v1.xml`) and `--battery-policy` CLI flag.
- WebM `EBML_ID_DURATION` parsing, Xiph/Fixed/EBML block lacing expansion, and MP4 `ctts` v0/v1 composition time offset support in `demuxer.c`.
- `--version` / `-V` / `version` CLI support in `wwal` and `wwald` wired to `meson.project_version()`, plus `contrib/config.toml.example` and starter GLSL shaders (`contrib/shaders/plasma.frag`, `contrib/shaders/audio_spectrum.frag`).

### Fixed

- Backward-compatible IPC header parsing in `WAYWAL_REQ_SET_IMAGE` for legacy `waywal_img_metadata_t` payloads.
- Deduplicated client/daemon HCT palette extraction, theme file exports, `/dev/pts/*` broadcasts, and hook execution on `wwal img`, while wiring daemon-side theme/hook updates and `SIGHUP` directory rescanning into `slideshow.c`.
- Preserved per-effect wave/blur defaults in `crosszoom`, `slide`, `glitch`, `burn`, `ripple`, and `wave`, fixed `--transition-type simple` and `random`, and eliminated double Cubic-Bezier easing.
- Added `in_flight` poll SQE tracking in `uring_loop.c` and forced `epoll`/`io_uring` re-registration on compositor IPC reconnect.
- Wired `meson_options.txt` feature toggles (`WWAL_DISABLE_{WEBP,AVIF,JXL,SVG,PIPEWIRE,SIMD}`) across runtime `dlopen` loaders and SIMD dispatch tables, and removed the unused `shaderc` build dependency.
- Hardened VA-API H.264/HEVC/VP9/AV1 bitstream parsers (`pred_weight_table`, multi-command RPLM, dependent slice inheritance, per-slot AV1 `PrevGmParams`/loop-filter/segmentation persistence, and hidden alt-ref frame suppression), multi-plane `NV12`/`P010` GPU video transition snapshots, EGLImage per-scene BO tracking, and Seccomp-BPF / `SceneScript` / JSON / `.pkg` bounds safety.

### Performance

- Vectorized AVX2 and AVX-512 BW scanline alpha blending without per-scanline `_mm_sfence()` stalls, cached GLSL uniform locations across all 19 transition compute shaders and Wallpaper Engine passes, and eliminated redundant 4K RGBA heap buffers in `video_engine.c`, `we_tex.c`, and `main.c`.
- Added AVX2-accelerated Annex-B start-code / RBSP unescaping, AVX2 FMA3 turbulent curl-noise and skeletal puppet skinning, hoisted `OKLab` `cbrtf` out of K-Means inner loops, gated PipeWire FFT and Hyprland cursor IPC on active shader consumers, and added zero-fork ISO-BMFF/WebM stream probing before `ffmpeg` transcoding.
- Reclaimed `source_old_bo` DMA-BUFs immediately upon transition completion and released `GL_RGBA16F` multi-pass ping-pong and scratch textures when stopping shader playback, reducing steady-state VRAM consumption by ~32–96 MB per 4K output.
- Consolidated `image_loader.c`, `we_pkg.c`, `we_json.c`, and `we_project.c` into `libwaywal_common.a` and decomposed `video_vaapi.c`, `we_scene.c`, `main.c`, `we_script.c`, and `render_engine.c` into focused modules.

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

[Unreleased]: https://github.com/Execorn/wwal/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/Execorn/wwal/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/Execorn/wwal/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Execorn/wwal/releases/tag/v1.0.0
