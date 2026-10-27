# WayWal Technical Architecture & Reference

This document covers the internal architecture, Wayland protocols, hardware acceleration pipelines, full CLI reference, build dependencies, and benchmark results for **WayWal** (`wwald` and `wwal`).

For a quick overview, installation instructions, and everyday examples, see the main [`README.md`](../README.md).

---

## 1. Subsystem Architecture

WayWal is written in C23 (`gnu2x`) and compiled into two binaries (`wwald` daemon and `wwal` CLI client) sharing `libwaywal_common.a`:

- **Zero-Copy Wayland Direct Scanout (`wayland_core.c`, `layer_shell.c`, `dmabuf_alloc.c`, `dmabuf_feedback.c`)**:
  - Uses `zwp_linux_dmabuf_v1` (v4 surface feedback) to negotiate hardware scanout modifiers directly with the Wayland compositor, including linear, NVIDIA Block-Linear, AMD DCC, Intel CCS, and ARM AFBC formats, plus 10-bit wide-gamut (`XRGB2101010` / `ARGB2101010`).
  - Solid-color wallpapers (`wwal clear`) use `wp_single_pixel_buffer_v1` 1×1 buffers for **0.0% GPU 3D load**.
  - Fractional HiDPI rendering (`1.25x`, `1.5x`, `1.75x`) uses `wp_fractional_scale_v1` and `wp_viewporter` so buffers render at exact physical pixel dimensions without compositor upscaling blur.
  - Frame pacing is locked to display refresh via a `wp_presentation` VBlank Phase-Locked Loop (PLL) supporting both fixed-refresh and VRR monitors.
  - Multi-GPU laptops automatically discover prime render nodes and fall back to linear PRIME DMA-BUF export when scanning out across secondary display controllers.

- **GPU Compute & CPU SIMD Transition Engine (`gpu_compute.c`, `shader_engine.c`, `simd/`)**:
  - Ships 18 OpenGL ES 3.1 compute shader transitions (`fade`, `wipe`, `grow`, `outer`, `wave`, `noise`, `crosszoom`, `slide`, `glitch`, `burn`, `ripple`, `pixelate`, `doom`, `swirl`, `cube`, `luma`, `light_leak`, `page_curl`) plus custom `.comp` shaders (`--transition-shader`).
  - On tiled or 10-bit scanout buffers where `glBindImageTexture` is unsupported, compute shaders dispatch into a linear scratch texture and blit via a fullscreen triangle pass (`prog_blit`) synchronized with `eglCreateSyncKHR` fences before `wl_surface_commit`.
  - Supports live-to-live cross-mode transitions across static images, hardware videos, live shaders, and Steam Workshop scenes.
  - When no GPU is available, falls back to multi-threaded OpenMP + runtime-dispatched AVX-512 (`vbmi2`), AVX2, NEON, and scalar CPU transition kernels.

- **Native Wallpaper Engine Workshop Runtime (`we_pkg.c`, `we_tex.c`, `we_puppet.c`, `we_shader.c`, `we_scene*.c`, `we_particle.c`, `we_script*.c`)**:
  - Memory-maps (`mmap`) Steam Workshop (`431960`) `scene.pkg` (`PKGV0001..22`) archives directly from disk with zero temporary file extraction.
  - Decodes `.tex` (`TEXV0001..0005`, `TEXI`, `TEXB`, `TEXS`) containers supporting hardware compressed `BC1` (`DXT1`), `BC2` (`DXT3`), `BC3` (`DXT5`), `BC7` (`GL_EXT_texture_compression_bptc`), `RG88`, `R8`, `LZ4` mip streams, spritesheets, and embedded MP4 video textures.
  - Evaluates `.mdl` (`MDLV`/`MDLS`/`MDLA`) 2D Puppet Warp skeletal meshes with AVX2 FMA3 bone skinning and blendshapes.
  - Simulates particle systems using an AVX2 Structure-of-Arrays (SoA) integrator with SIMD 3D turbulent curl noise, vortex/attractor control points, and batched quad emission.
  - Transpiles Wallpaper Engine `.vert`/`.frag` shaders (`#include` trees, `COMBO` directives, HLSL-to-GLSL types/intrinsics, `g_*` uniforms) into GLSL ES 3.00 (`#version 300 es`).
  - Executes `SceneScript` property logic inside a sandboxed AST bytecode VM with a 5 ms per-frame execution budget.

- **Zero-FFmpeg Hardware Video & Native Animation Engine (`demuxer.c`, `video_vaapi.c`, `vaapi_*.c`, `video_engine.c`, `anim_engine.c`)**:
  - Includes a zero-allocation in-process MP4/ISOBMFF and WebM/Matroska EBML demuxer (`demuxer.c`).
  - Drives `libva` (VA-API) directly for H.264, HEVC (Main & Main10), VP9 (Profile 0 & 2), and AV1 (Main Profile) hardware decoding with AVX2 Annex-B start-code and RBSP unescaping.
  - Exports decoded `NV12`/`P010` surfaces directly as Wayland DMA-BUFs on AMD and Intel GPUs, or blits through a 3-surface `BGRX` VA-API VPP ring on NVIDIA GPUs.
  - Renders animated GIF, APNG, and WebP files natively via an in-process delta-frame engine (`anim_engine.c`).

- **Multi-Pass Shadertoy, 2.5D Depth Parallax, PipeWire Audio & D-Bus MPRIS2 (`shader_engine.c`, `audio_fft.c`, `mpris.c`)**:
  - Supports multi-pass Shadertoy `.frag` shaders (`Buffer A`..`Buffer D` `GL_RGBA16F` ping-pong passes, `iChannel0..3`, `iMouse`, `iDate`, `#pragma waywal`, hardware derivatives `dFdx`/`dFdy`/`fwidth`, and `OES_get_program_binary` disk caching with `inotify` hot-reload).
  - Supports 2.5D Depth-Map Parallax Occlusion Mapping (`--depth-map`, `--depth-strength`) with 4-tier Wayland cursor tracking and workspace-slide spring physics.
  - Captures desktop audio lock-free via PipeWire into a 2048-point Hann-windowed AVX2 radix-2 FFT producing 64-bin Bark-scale spectrum uniforms and `iChannel0` audio textures (`--audio`), with automatic volume ducking (`--mute-on-audio`) when external apps play audio.
  - Tracks media players via event-driven D-Bus `MPRIS2` (`mpris.c`) with zero idle polling, exposing `u_album_art`, `u_media_playing`, `u_media_position`, `u_media_Color0..4`, and `--theme-from-media`.

- **Material Design 3 (`OKLab` / `CAM16` / `HCT`) Theme Engine (`palette_hct.c`)**:
  - Extracts 29 Material Design 3 dark and light role tokens from 4K wallpapers in **0.32 ms** using AVX2 Wu 3D variance quantization, golden-ratio deterministic K-Means++, and algebraic `CAM16`/`HCT` tone solving.
  - Writes `~/.local/state/caelestia/scheme.json` (Quickshell / Caelestia), `~/.cache/wwal/colors.{json,css,hyprland.conf}`, custom user templates (`~/.config/wwal/templates/`), and non-blocking OSC 4/10/11 sequences to `/dev/pts/*` terminals.

- **Zero-Watt Smart Pause, `io_uring` Event Loop & Sandbox (`power_governor.c`, `uring_loop.c`, `security.c`)**:
  - Disarms all render timers down to **0.00% CPU and GPU** when an output is occluded by a fullscreen or maximized window (`zwlr_foreign_toplevel_manager_v1` + Hyprland/Sway/Niri IPC), when the session is idle (`ext-idle-notify-v1`), or on laptop battery power (`/sys/class/power_supply`).
  - Runs on a Linux `io_uring` multishot poll event loop (`uring_loop.c`) with `epoll` fallback, hardened after initialization by Landlock v3 filesystem restrictions and a Seccomp-BPF syscall filter (`security.c`).

---

## 2. Build Dependencies

To build from source, you need a C23 compiler (`GCC >= 13` or `Clang >= 17`), `meson >= 0.60`, `ninja`, `pkg-config`, and `python3`.

| Group | `pkg-config` / Package Names | Role |
| :--- | :--- | :--- |
| **Core Wayland & DRM** | `wayland-client >= 1.20`, `wayland-protocols >= 1.34`, `wayland-scanner`, `libdrm >= 2.4.110`, `gbm`, `egl`, `glesv2` | Layer surfaces, DMA-BUF buffers, and GLES 3.1 pipelines |
| **Video & Kernel** | `libva`, `libva-drm`, `liburing`, `libseccomp` | VA-API video decode, `io_uring` loop, and Seccomp sandbox |
| **Base Image Formats** | `libpng`, `libturbojpeg` | Native PNG, APNG, and JPEG decode |
| **Optional Plugins (`dlopen`)** | `libwebp`, `libavif`, `libjxl`, `librsvg-2.0`, `libpipewire-0.3`, `libdbus-1`, `libsystemd` / `libelogind`, `liblz4`, `libquickjs` | Loaded lazily at runtime (`dlopen`) with zero startup RSS overhead when unused |
| **Optional Build Tools** | `scdoc`, `systemd` | Builds `docs/man/*.scd` man pages and sets systemd user unit paths |

### Meson Build Options (`meson_options.txt`)

```bash
meson setup build \
  --prefix="$HOME/.local" \
  --buildtype=release \
  -Doptimization=3 \
  -Db_lto=true \
  -Dwebp=enabled \
  -Davif=enabled \
  -Djxl=enabled \
  -Dsvg=enabled \
  -Dpipewire=enabled \
  -Dsimd=true \
  -Dman-pages=enabled
```

---

## 3. Full CLI Reference (`wwal`)

`wwal` also acts as a drop-in replacement when symlinked or invoked as `swww`, `swww-daemon`, `awww`, or `awww-daemon`.

```text
Usage: wwal [GLOBAL_OPTIONS] <COMMAND> [ARGS...]

Commands:
  img <PATH|-> [OPTIONS]         Load and set wallpaper (PNG, JPEG, WebP, AVIF, JXL, SVG, GIF, APNG)
  video <PATH> [OPTIONS]         Play hardware-accelerated video wallpaper (H.264, HEVC, VP9, AV1)
  shader <PATH> [OPTIONS]        Run live GLSL ES 3.1 (.comp) or multi-pass Shadertoy (.frag) wallpaper
  workshop list                  Auto-discover installed Steam Workshop (431960) wallpapers
  workshop info <ID|PATH>        Inspect Workshop project.json, scene.pkg, and user properties
  workshop [apply] <ID|PATH>     Render native Wallpaper Engine scene, particles, Puppet Warp & SceneScript
  slideshow <DIR|ACTION>         Start or control timerfd slideshow (stop, pause, resume, toggle, next, prev)
  theme [PATH] [OPTIONS]         Extract < 0.4ms OKLab/HCT Material 3 palette & sync caelestia/terminals
  audio [OPTIONS]                Configure live wallpaper audio volume and external playback auto-ducking
  set-uniform <NAME> <V...>      Mutate live shader uniform or Workshop property without reloading
  status [--json]                Inspect outputs, active media, MPRIS metadata, and daemon telemetry
  stats [--json]                 Show live daemon RSS, DRM modifier, VBlank PLL, and pause telemetry
  query [--json]                 List connected Wayland outputs, physical/logical geometry, and scale
  subscribe                      Stream newline-delimited JSON state change events
  clear [COLOR]                  Set solid hex color via 1x1 wp_single_pixel_buffer_v1
  restore                        Restore saved per-output wallpapers from state.bin
  pause / unpause / toggle       Control active video, shader, or Workshop scene playback
  clear-cache                    Purge cached video transcodes and compiled shader binaries
  ping / kill / version          Check responsiveness, stop wwald, or print version info

Key Options:
  -o, --outputs <NAMES>          Comma-separated target outputs (e.g. DP-1,HDMI-A-1)
  --transition-type <TYPE>       none, simple, fade, wipe, grow, outer, wave, noise, crosszoom,
                                 slide, glitch, burn, ripple, pixelate, doom, swirl, cube, luma,
                                 light_leak, page_curl, custom, left, right, top, bottom, center, any, random
  --transition-duration <SEC>    Transition duration in seconds (default: 1.0)
  --transition-fps <FPS>         Transition frame rate (default: 60)
  --transition-bezier <B>        Cubic bezier control points X1,Y1,X2,Y2 (e.g. 0.25,0.1,0.25,1.0)
  --transition-pos <POS>         cursor, center, tl, tr, bl, br, top, bottom, left, right, or X,Y
  --scaling-mode, --resize <M>   fill (crop), fit, stretch, center (no), tile, span
  --fill-color <RRGGBB>          Padding color for fit/center modes
  --filter <FILTER>              Nearest, Bilinear, CatmullRom, Mitchell, Lanczos3
  --backdrop <MODE>              none, auto-blur (Dual-Kawase blur for Niri overview), or image path
  --depth-map <PATH>             2.5D depth map image for cursor/workspace Parallax Occlusion Mapping
  --depth-strength <FLOAT>       Depth parallax displacement multiplier (default: 1.0)
  --prop <KEY=VALUE>             Override Wallpaper Engine Workshop user property (repeatable)
  --audio                        Enable PipeWire 64-bin Bark FFT uniforms & iChannel0 for shaders/scenes
  --volume <0.0..1.0>            Set wallpaper audio playback volume (default: 0.0)
  --mute-on-audio                Auto-duck wallpaper volume when another application plays audio
  --theme / --no-theme           Enable or disable automatic Material 3 / caelestia theme extraction
  --dark / --light               Force dark or light Material 3 theme mode (default: dark)
  --scheme-variant <VARIANT>     tonalspot, vibrant, expressive, content, fidelity, rainbow, fruit_salad, monochrome, neutral
  --theme-from-media             Dynamically extract Material 3 theme from active D-Bus MPRIS2 album art
  --layer <LAYER>                background or bottom
  --battery-policy <POLICY>      ignore, reduce-fps, or pause-video
  --10bit                        Enable 10-bit wide-gamut scanout (XRGB2101010 / ARGB2101010)
  -j, --json                     Format query, status, or stats output as structured JSON
  -V, --version                  Print version information
```

---

## 4. Repository Layout

```text
wwal/
├── README.md                          # User-friendly overview, installation, and examples
├── docs/
│   ├── ARCHITECTURE.md                # Technical architecture, CLI reference, and benchmarks
│   └── man/                           # scdoc manual pages (wwal.1, wwald.1, wwal-config.5)
├── contrib/
│   ├── config.toml.example            # Annotated example configuration file
│   ├── shaders/                       # Sample Shadertoy (.frag) wallpapers
│   ├── systemd/                       # wwald.service and wwald.socket user units
│   └── completions/                   # Bash, Zsh, and Fish shell completions
├── protocols/                         # Wayland XML protocols (wlr-layer-shell, wlr-foreign-toplevel)
├── scripts/                           # Shader embedding, PGO/LTO/BOLT build, and benchmark scripts
├── include/waywal/                    # Subsystem headers (IPC, JSON, image loader, Workshop, MPRIS)
├── src/
│   ├── common/                        # Arena, IPC, Bezier, OKLab/HCT palette, TOML, JSON, image loader, Workshop PKG/project
│   ├── daemon/                        # Wayland core, IPC server, GBM/DMA-BUF, VA-API codecs, Shaders, Workshop runtime, MPRIS, Audio
│   └── client/                        # CLI parser (wwal + swww/awww) and client dispatch
└── tests/                             # Unit, subsystem, Workshop scene/script/particle, LibFuzzer, and E2E suites
```

---

## 5. Microbenchmarks

Measured with `./build/bench_transitions 60` (`OMP_NUM_THREADS=4`):

| Subsystem / Kernel | Resolution | Latency / Frame Time | Throughput / Budget |
| :--- | :--- | ---: | :--- |
| **GPU Compute Transitions (18 Shaders)** | `1920×1080` | `0.094 – 0.175 ms` | **5,714 – 10,609 FPS** (`0 KB` leak) |
| **GPU Compute Transitions (18 Shaders)** | `3840×2160` (4K) | `0.203 – 0.411 ms` | **2,431 – 4,937 FPS** (`0 KB` leak) |
| **CPU OpenMP + AVX2 Transitions** | `1280×720` | `0.038 – 0.646 ms` | **1,548 – 26,087 FPS** |
| **OKLab Wu + K-Means++ + HCT Palette** | `3840×2160` (4K) | `0.325 ms` | **< 0.40 ms target** (`~500×` faster than external CLI) |
| **2048-pt Hann AVX2 64-Bin Bark FFT** | `48 kHz` PCM | `0.012 ms` | **< 0.015 ms target** (lock-free SPSC ring) |
| **Hardware VA-API Video Playback** | `1080p` / `4K` | `0.3%` CPU | **0.00% CPU/GPU when occluded** |
