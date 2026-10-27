# `WayWal` (`wwal`): C23 Wayland Wallpaper, Video, Shader & Theme Engine

[![CI](https://github.com/Execorn/wwal/actions/workflows/ci.yml/badge.svg)](https://github.com/Execorn/wwal/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Execorn/wwal?color=007ec6)](https://github.com/Execorn/wwal/releases)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Language: C23](https://img.shields.io/badge/Language-C23-00599C?logo=c)](meson.build)
[![Platform: Wayland](https://img.shields.io/badge/Platform-Wayland%20%7C%20Linux-FFBC00?logo=wayland)](https://wayland.freedesktop.org/)

<p align="center">
  <a href="assets/demo.mp4">
    <img src="assets/demo.webp" alt="WayWal Demo" width="100%">
  </a>
</p>

**WayWal** (`wwald` daemon and `wwal` CLI) is a fast, low-memory Wayland desktop engine written in C23 (`gnu2x`). It replaces static wallpaper tools (`swww`/`awww`, `hyprpaper`, `wpaperd`), video players (`mpvpaper`), Shadertoy and audio tools (`shaderbg`, `glava`), Linux Wallpaper Engine ports, and Material Design 3 theme tools (`matugen`, `wallust`). Everything runs inside a single `< 2 MB` binary pair.

---

## Why Use WayWal?

- **Zero-Copy Direct Scanout**: Draws static wallpapers directly into GPU DMA-BUFs (`zwp_linux_dmabuf_v1` with linear, NVIDIA block-linear, AMD DCC, and Intel CCS formats). Solid colors use 1×1 buffers (`wp_single_pixel_buffer_v1`) with **0.0% GPU 3D load**.
- **True Physical HiDPI & Panoramic Span**: Uses `wp_fractional_scale_v1` and `wp_viewporter`. Your 4K and 5K screens at fractional scales (`1.25x`, `1.5x`, `1.75x`) draw at exact pixel size. Use `--scaling-mode span` to stretch one wide image across all screens.
- **Native Wallpaper Engine Workshop Runtime**: Loads Steam Workshop (`431960`) wallpapers directly from disk via `mmap` (`wwal workshop list / info / apply <id|path> [--prop key=val]`). It reads `scene.pkg` (`PKGV0001..22`) archives and `.tex` (`TEXV0005` `BC1`/`BC2`/`BC3`/`BC7` and `LZ4`) textures without unpacking files. It also runs `.mdl` Puppet Warp skeletal mesh models, an AVX2 SoA particle simulator, multi-pass effects, and a safe `SceneScript` VM.
- **Hardware Video & Native Animations**: Plays H.264, HEVC, VP9, and AV1 videos without FFmpeg at runtime. It uses VA-API (`libva`) with direct `NV12`/`P010` KMS scanout on Intel and AMD. On NVIDIA, it uses a 3-surface `BGRX` VPP ring. It also includes a built-in GIF, APNG, and WebP engine.
- **Multi-Pass Shadertoy & 2.5D Depth Parallax**: Runs GLSL ES 3.1 `.comp` and multi-pass Shadertoy `.frag` shaders (`Buffer A`..`Buffer D` `GL_RGBA16F` passes, `iChannel0`..`iChannel3`, `#pragma waywal`, and hardware derivatives `dFdx`/`dFdy`/`fwidth`). It also supports 2.5D Depth-Map Parallax Occlusion Mapping (`--depth-map`, `--depth-strength`). Both modes react to 4-tier Wayland cursor tracking and workspace-slide spring motion.
- **PipeWire Audio Ducking & D-Bus MPRIS2**: Streams lock-free PipeWire 2048-point AVX2 64-bin Bark-scale audio data (`--audio`). You can set wallpaper audio volume and auto-duck sound when other apps play media (`--volume`, `--mute-on-audio`, `wwal audio`). It also listens to D-Bus `MPRIS2` events with zero idle polling. This feeds `u_album_art`, `u_media_playing`, and live album art themes (`--theme-from-media`).
- **Live-to-Live GPU Transitions & Fast Themes**: Runs 18 built-in GLES 3.1 compute transitions smoothly between static images, videos, live shaders, and Workshop scenes. When the wallpaper changes, it builds `CAM16`/`HCT` and `OKLab` color themes from 4K images in **0.32 ms**. It syncs themes to Quickshell (`caelestia`), Waybar, Hyprland, and `/dev/pts/*` terminals.
- **Zero-Watt Smart Pause & Battery Saver**: Pauses video, shader, and Workshop scenes down to **0.00% CPU and GPU** when a screen is covered by a full-screen or full-size window (`ext-foreign-toplevel-list-v1` + Hyprland/Sway/Niri IPC). It also pauses when the session is idle (`ext-idle-notify-v1`) or when your laptop runs on battery power.
- **Drop-In `swww` / `awww` Support & Live JSON IPC**: Works as a drop-in replacement for `swww` and `awww` scripts (`waypaper`, Rofi menus). You can query live JSON state (`wwal status --json`, `wwal query --json`, `wwal stats --json`). You can also change live shader uniforms (`wwal set-uniform`) and watch state events (`wwal subscribe`).

> [!TIP]
> WayWal runs 18 built-in transition shaders on the GPU via OpenGL ES 3.1 compute shaders (`0.09–0.17 ms` at 1080p, `0.20–0.41 ms` at 4K). If no GPU is present, its OpenMP + AVX-512 / AVX2 / NEON CPU fallback draws 720p transitions at **1,548 – 26,087 FPS**.

---

## Dependencies & Requirements

To build from source, you need a C23 compiler (`GCC >= 13` or `Clang >= 17`), `meson >= 0.60`, `ninja`, `pkg-config`, and `python3`.

| Group | `pkg-config` / Package Names | Role |
| :--- | :--- | :--- |
| **Core Wayland & DRM** | `wayland-client >= 1.20`, `wayland-protocols >= 1.34`, `wayland-scanner`, `libdrm >= 2.4.110`, `gbm`, `egl`, `glesv2` | Layer surfaces, DMA-BUF buffers, and GLES 3.1 pipelines |
| **Video & Kernel** | `libva`, `libva-drm`, `liburing`, `libseccomp` | VA-API video decode, `io_uring` loop, and Seccomp sandbox |
| **Base Image Formats** | `libpng`, `libturbojpeg` | Fast native PNG, APNG, and JPEG decode |
| **Optional Plugins (`dlopen`)** | `libwebp`, `libavif`, `libjxl`, `librsvg-2.0`, `libpipewire-0.3`, `libdbus-1`, `libquickjs` | Loaded on demand for WebP, AVIF, JXL, SVG, PipeWire audio, D-Bus `MPRIS2`, or QuickJS |
| **Optional Build Tools** | `scdoc`, `systemd` | Builds `docs/man/*.scd` man pages and sets systemd unit paths |

---

## Installation & Quickstart

### Build from Source

```bash
git clone https://github.com/Execorn/wwal.git
cd wwal

# Configure release build with Link-Time Optimization (LTO)
meson setup build --prefix="$HOME/.local" --buildtype=release -Doptimization=3 -Db_lto=true

# Compile wwald (daemon) and wwal (client)
meson compile -C build

# Run unit, subsystem, fuzz, and E2E test suites
meson test -C build --print-errorlogs

# Install binaries, manual pages, shell completions, and systemd user units
meson install -C build
```

### Packages

- **Arch Linux (`PKGBUILD`)**:
  ```bash
  cd contrib && makepkg -si
  ```
- **NixOS / Home Manager (`flake.nix`)**:
  ```bash
  nix run github:Execorn/wwal -- query
  ```
- **Fedora / RHEL**: Build with `contrib/fedora/wwal.spec`.
- **Debian / Ubuntu**: Build with `contrib/debian/`.
- **Alpine Linux**: Build with `contrib/alpine/APKBUILD`.
- **Void Linux**: Build with `contrib/void/template`.

### Common Commands

```bash
# Start the daemon (or enable systemd socket activation: systemctl --user enable --now wwald.socket)
wwald &

# Set a wallpaper with a radial ripple transition from the mouse cursor and generate a Material 3 theme
wwal img ~/Pictures/Wallpapers/scenery.jpg --transition-type ripple --transition-pos cursor --theme

# Set a 2.5D depth-parallax wallpaper or span a panoramic canvas across monitors
wwal img ~/Pictures/Wallpapers/canyon.jpg --depth-map ~/Pictures/Wallpapers/canyon_depth.png --depth-strength 1.2
wwal img ~/Pictures/Wallpapers/panoramic.jxl --scaling-mode span

# List, inspect, and apply native Steam Workshop (Wallpaper Engine 431960) scenes
wwal workshop list
wwal workshop info 2841902341
wwal workshop apply 2841902341 --prop schemecolor="0.1 0.6 0.9" --audio --mute-on-audio

# Play a hardware-accelerated 4K video wallpaper with smart audio ducking
wwal video ~/Videos/loop.mp4 --volume 0.4 --mute-on-audio

# Run a multi-pass audio- & MPRIS-reactive Shadertoy wallpaper
wwal shader ./my_shader.frag --audio --theme-from-media --fps 60

# Mutate live shader uniforms or Workshop user properties and stream JSON state events
wwal set-uniform u_speed 1.5
wwal status --json
wwal subscribe

# Restore saved per-monitor wallpapers after login or hotplug
wwal restore
```

---

## Configuration (`~/.config/wwal/config.toml`)

WayWal works out of the box with no setup. Create `~/.config/wwal/config.toml` if you want to save default settings:

```toml
[daemon]
layer = "background"              # "background" or "bottom"
namespace = "waywal"              # Layer surface namespace
battery_policy = "reduce-fps"     # "ignore", "reduce-fps" (24 fps), or "pause-video"
auto_pause_fullscreen = true      # Pause video/shaders/scenes when a window is fullscreen
auto_pause_maximized = false
idle_timeout_sec = 300            # Pause animations when session is idle
ten_bit = false                   # Enable 10-bit XRGB2101010 scanout
backdrop = "none"                 # "none", "auto-blur" (for Niri overview), or file path
volume = 0.0                      # Default wallpaper audio volume (0.0 .. 1.0)
mute_on_audio = true              # Auto-duck volume when another app plays audio

[theme]
enabled = true                    # Extract Material 3 palette on every wallpaper change
mode = "dark"                     # "dark" or "light"
caelestia_sync = true             # Write ~/.local/state/caelestia/scheme.json
tty_sequences = true              # Broadcast OSC 4/10/11 palette to active /dev/pts/* terminals
theme_from_media = false          # Extract palette from active D-Bus MPRIS2 album art

[transition]
type = "grow"
duration = 1.0
fps = 120
bezier = [0.25, 0.1, 0.25, 1.0]

[output."DP-1"]
path = "~/Pictures/Wallpapers/ultrawide.jxl"
scaling = "fill"
```

---

## CLI Reference (`wwal`)

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
  ping / kill                    Check responsiveness or gracefully terminate the running wwald daemon

Key Options:
  --transition-type <TYPE>       none, simple, fade, wipe, grow, outer, wave, noise, crosszoom,
                                 slide, glitch, burn, ripple, pixelate, doom, swirl, cube, luma,
                                 light_leak, page_curl, custom, left, right, top, bottom, center, any, random
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
  --theme-from-media             Dynamically extract Material 3 theme from active D-Bus MPRIS2 album art
  --layer <LAYER>                background or bottom
  --battery-policy <POLICY>      ignore, reduce-fps, or pause-video
  --10bit                        Enable 10-bit wide-gamut scanout (XRGB2101010 / ARGB2101010)
  -j, --json                     Format query, status, or stats output as structured JSON
```

---

## Repository Structure

```text
wwal/
├── README.md                          # Project overview, quickstart, and benchmarks
├── LICENSE                            # GNU General Public License v3.0
├── CHANGELOG.md                       # Release history
├── CONTRIBUTING.md                    # Developer workflow, sanitizers, and style guide
├── SECURITY.md                        # Sandbox architecture and vulnerability reporting
├── SUPPORT.md                         # Troubleshooting and diagnostic guide
├── CODE_OF_CONDUCT.md                 # Contributor Covenant v2.1
├── flake.nix                          # NixOS Flake package, devShell, and Home Manager module
├── meson.build                        # Meson build configuration and test targets
├── meson_options.txt                  # Feature and codec build options
├── docs/
│   └── man/                           # scdoc manual pages (wwal.1, wwald.1, wwal-config.5)
├── contrib/
│   ├── PKGBUILD                       # Arch Linux package build script
│   ├── fedora/wwal.spec               # Fedora / RPM spec file
│   ├── debian/                        # Debian / Ubuntu packaging
│   ├── alpine/APKBUILD                # Alpine Linux package build script
│   ├── void/template                  # Void Linux xbps-src template
│   ├── systemd/                       # wwald.service and wwald.socket user units
│   └── completions/                   # Bash, Zsh, and Fish completions
├── protocols/                         # Wayland XML protocols (wlr-layer-shell, wlr-foreign-toplevel)
├── scripts/
│   ├── embed_shaders.py               # Build-time GLSL compute shader embedding script
│   ├── build_optimized.sh             # PGO + LTO + LLVM BOLT build pipeline
│   └── benchmark.py                   # Live daemon transition and RSS benchmark
├── include/waywal/                    # Subsystem headers (IPC, Workshop PKG/TEX/MDL/Scene/Script, MPRIS)
├── src/
│   ├── common/                        # Arena, IPC, Bezier, OKLab/HCT palette, TOML, state store
│   ├── daemon/                        # Wayland core, GBM/DMA-BUF, VA-API, Shaders, Workshop runtime, MPRIS, Audio
│   └── client/                        # CLI parser (wwal + swww/awww), multi-format image loader
└── tests/                             # Unit, subsystem, Workshop scene/script/particle, LibFuzzer, and E2E suites
```

---

## Performance Benchmarks

Measured with `./build/bench_transitions 60` (`OMP_NUM_THREADS=4`):

| Subsystem / Kernel | Resolution | Latency / Frame Time | Throughput / Budget |
| :--- | :--- | ---: | :--- |
| **GPU Compute Transitions (18 Shaders)** | `1920×1080` | `0.094 – 0.175 ms` | **5,714 – 10,609 FPS** (`0 KB` leak) |
| **GPU Compute Transitions (18 Shaders)** | `3840×2160` (4K) | `0.203 – 0.411 ms` | **2,431 – 4,937 FPS** (`0 KB` leak) |
| **CPU OpenMP + AVX2 Transitions** | `1280×720` | `0.038 – 0.646 ms` | **1,548 – 26,087 FPS** |
| **OKLab Wu + K-Means++ + HCT Palette** | `3840×2160` (4K) | `0.325 ms` | **< 0.40 ms target** (`~500×` faster than external CLI) |
| **2048-pt Hann AVX2 64-Bin Bark FFT** | `48 kHz` PCM | `0.012 ms` | **< 0.015 ms target** (lock-free SPSC ring) |
| **Hardware VA-API Video Playback** | `1080p` / `4K` | `0.3%` CPU | **0.00% CPU/GPU when occluded** |

---

## Contributing & Security

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for build steps, test tools, and code style rules. To report a bug or security issue, read [`SECURITY.md`](SECURITY.md).
