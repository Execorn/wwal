# WayWal Technical Architecture & Reference

This document covers the internal architecture, Wayland protocols, hardware acceleration pipelines, full CLI reference, build dependencies, and empirical benchmark results for **WayWal** (`wwald` and `wwal`).

For a quick overview, installation instructions, and everyday examples, see the main [`README.md`](../README.md).

---

## 1. Subsystem Architecture

WayWal is written in C23 (`gnu2x`) and compiled into two binaries (`wwald` daemon `1.34 MB` stripped and `wwal` CLI client `0.32 MB` stripped) sharing `libwaywal_common.a`:

- **Zero-Copy Wayland Direct Scanout (`wayland_core.c`, `layer_shell.c`, `dmabuf_alloc.c`, `dmabuf_feedback.c`)**:
  - Uses `zwp_linux_dmabuf_v1` (v4 surface feedback) to negotiate hardware scanout modifiers directly with the Wayland compositor, including linear, NVIDIA Block-Linear, AMD DCC, Intel CCS, and ARM AFBC formats, plus 10-bit wide-gamut (`XRGB2101010` / `ARGB2101010`).
  - Solid-color wallpapers (`wwal clear` / `wwal color`) use `wp_single_pixel_buffer_v1` 1×1 buffers for **0.0% GPU 3D load**.
  - Fractional HiDPI rendering (`1.25x`, `1.5x`, `1.75x`) uses `wp_fractional_scale_v1` and `wp_viewporter` so buffers render at exact physical pixel dimensions without compositor upscaling blur.
  - Frame pacing is locked to display refresh via a `wp_presentation` VBlank Phase-Locked Loop (PLL) supporting both fixed-refresh and VRR monitors.
  - Multi-GPU laptops automatically discover prime render nodes and fall back to linear PRIME DMA-BUF export when scanning out across secondary display controllers.

- **GPU Compute & CPU SIMD Transition Engine (`gpu_compute.c`, `shader_engine.c`, `simd/`)**:
  - Ships 18 OpenGL ES 3.1 compute shader transitions (`fade`, `wipe`, `grow`, `outer`, `wave`, `noise`, `crosszoom`, `slide`, `glitch`, `burn`, `ripple`, `pixelate`, `doom`, `swirl`, `cube`, `luma`, `light_leak`, `page_curl`) plus custom `.comp` shaders (`--transition-shader`), directional UV crop anchors (`--crop-gravity` / `u_crop_gravity` in `SHADER_SRC_SCALE`), and Y-axis inversion (`--invert-y`).
  - On tiled or 10-bit scanout buffers where `glBindImageTexture` is unsupported, compute shaders dispatch into a linear scratch texture and blit via a fullscreen triangle pass (`prog_blit`) synchronized with `eglCreateSyncKHR` fences before `wl_surface_commit`.
  - Supports live-to-live cross-mode transitions across static images, hardware videos, live shaders, and Steam Workshop scenes.
  - When no GPU is available, falls back to multi-threaded OpenMP + runtime-dispatched AVX-512 (`vbmi2`), AVX2, NEON, and scalar CPU transition kernels.

- **Native Wallpaper Engine Workshop Runtime (`we_pkg.c`, `we_tex.c`, `we_puppet.c`, `we_shader.c`, `we_scene*.c`, `we_particle.c`, `we_script*.c`)**:
  - Memory-maps (`mmap`) Steam Workshop (`431960`) `scene.pkg` (`PKGV0001..22`) archives directly from disk with zero temporary file extraction.
  - Decodes `.tex` (`TEXV0001..0005`, `TEXI`, `TEXB`, `TEXS`) containers supporting hardware compressed `BC1` (`DXT1`), `BC2` (`DXT3`), `BC3` (`DXT5`), `BC7` (`GL_EXT_texture_compression_bptc`), `RG88`, `R8`, `LZ4` mip streams, spritesheets, and embedded MP4 video textures.
  - Evaluates `.mdl` (`MDLV`/`MDLS`/`MDLA`) 2D Puppet Warp skeletal meshes with AVX2 FMA3 bone skinning and blendshapes.
  - Simulates particle systems using an AVX2 Structure-of-Arrays (SoA) integrator with SIMD 3D turbulent curl noise, vortex/attractor control points, and batched quad emission.
  - Renders Workshop `"sound"` multi-track ambient audio layers (looping/random PCM playback with PipeWire auto-ducking) and `"text"` dynamic clock/label layers.
  - Transpiles Wallpaper Engine `.vert`/`.frag` shaders (`#include` trees, `COMBO` directives, HLSL-to-GLSL types/intrinsics, `g_*` uniforms) into GLSL ES 3.00 (`#version 300 es`).
  - Executes `SceneScript` property logic inside a sandboxed AST bytecode VM with a 5 ms per-frame execution budget.
  - Exports UTF-8 safe human-readable preview symlinks (`~/.cache/wwal/workshop-previews/<Title> [<ID>].<ext>`) via `wwal workshop sync-previews` and resolves preview symlinks passed to `wwal img` back to the parent Workshop scene automatically.

- **Zero-FFmpeg Hardware Video & Native Animation Engine (`demuxer.c`, `video_vaapi.c`, `vaapi_*.c`, `video_engine.c`, `anim_engine.c`)**:
  - Includes a zero-allocation in-process MP4/ISOBMFF and WebM/Matroska EBML demuxer (`demuxer.c`).
  - Drives `libva` (VA-API) directly for H.264, HEVC (Main & Main10), VP9 (Profile 0 & 2), and AV1 (Main Profile) hardware decoding with AVX2 Annex-B start-code and RBSP unescaping.
  - Exports decoded `NV12`/`P010` surfaces directly as Wayland DMA-BUFs on AMD and Intel GPUs, or blits through a 3-surface `BGRX` VA-API VPP ring on NVIDIA GPUs.
  - Renders animated GIF, APNG, and WebP files natively via an in-process delta-frame engine (`anim_engine.c`).

- **Multi-Pass Shadertoy, 10 Built-In Shader Presets, 2.5D Depth Parallax, PipeWire Audio & D-Bus MPRIS2 (`shader_engine.c`, `audio_fft.c`, `mpris.c`)**:
  - Supports multi-pass Shadertoy `.frag` shaders (`Buffer A`..`Buffer D` `GL_RGBA16F` ping-pong passes, `iChannel0..3`, `iMouse`, `iDate`, `#pragma waywal`, hardware derivatives `dFdx`/`dFdy`/`fwidth`, and `OES_get_program_binary` disk caching with `inotify` hot-reload).
  - Ships 10 built-in procedural shader presets (`aurora`, `cyber_grid`, `fluid_ink`, `synthwave_sun`, `starfield`, `rain_glass`, `audio_bars`, `audio_ring`, `crt_retro`, `plasma_orb`, with aliases `lava_lamp`, `northern_lights`, `outrun`, `crt`) installed in `shaders/presets/*.frag` and accessible via `wwal shader list` (`--list`) and `wwal shader preset <name>` (`--preset <name>`).
  - Supports 2.5D Depth-Map Parallax Occlusion Mapping (`--depth-map`, `--depth-strength`) with 4-tier Wayland cursor tracking and workspace-slide spring physics.
  - Captures desktop audio lock-free via PipeWire into a 2048-point Hann-windowed AVX2 radix-2 FFT producing 64-bin Bark-scale spectrum uniforms and `iChannel0` audio textures (`--audio`), with automatic volume ducking (`--mute-on-audio`) when external apps play audio.
  - Tracks media players via event-driven D-Bus `MPRIS2` (`mpris.c`) with zero idle polling, exposing `u_album_art`, `u_media_playing`, `u_media_position`, `u_media_Color0..4`, and `--theme-from-media`.

- **Dual-Mode Material Design 3 (`OKLab` / `CAM16` / `HCT`) & Superset Template Engine (`palette_hct.c`)**:
  - Extracts simultaneous `dark` and `light` Material Design 3 palettes (110 color slots + 6 tonal families + 4 ranked K-Means++ seed clusters `--source-color-index 0..3` or `--seed-color <#HEX>`) from 4K wallpapers in **`0.44 ms`** using AVX2 Wu 3D variance quantization, golden-ratio deterministic K-Means++, and algebraic `CAM16`/`HCT` tone solving.
  - Renders declarative `[template."<name>"]` rules (`input_path`, `output_path`, `post_hook`) in **`0.05 ms`** supporting Matugen (`{{colors.<role>.<default|dark|light>.<fmt>}}`, `{{palettes.<family>.<tone>.<fmt>}}`), Wallust/Jinja2 chained pipe filters (`lighten`, `darken`, `saturate`, `desaturate`, `set_lightness`, `alpha`, perceptual OKLab `blend`, `invert`, `complementary`), and Pywal (`{background}`, `{foreground}`, `{color0.strip}`..`{color15.rgb}`) syntax in a single pass, guarded against path traversal or overwriting sensitive dotfiles (`is_unsafe_template_target`).
  - Writes `~/.local/state/caelestia/scheme.json` (Quickshell / Caelestia), `~/.cache/wwal/colors.{json,css,hyprland.conf}`, `~/.cache/wal/{colors.json,colors,colors.sh,colors.css,sequences}` (Pywal & Pywalfox compatibility), `~/.cache/{swww,awww}/<output>` state caches, and `~/.cache/wwal/current_wallpaper.png` (`<= 1280x720` lockscreen frame via `image_save_png_bgra`), plus non-blocking OSC 4/10/11 sequences to `/dev/pts/*` terminals.

- **Astronomical Solar Elevation, Dynamic Schedules & `wpaperd` Slideshow Engine (`slideshow.c`, `toml_parser.c`)**:
  - Computes sunrise, solar noon, dusk, and night via NOAA solar position equations (`[solar]` `latitude`/`longitude`) and evaluates `[[schedule]]` blocks and GNOME `.xml` (`<background><starttime>...`) timed dynamic wallpapers, with automatic daytime/nighttime theme polarity switching (`--auto-day-night`).
  - Supports `wpaperd` configuration syntax (`[default]`/`[any]` wildcard sections, human-readable durations `"30m"`, `"1h"`, and `--sort <ascending|descending|name|mtime|random>`) and mixed-media directory slideshows controlled via `wwal next` and `wwal prev` (`previous`).

- **Zero-Watt Smart Pause, `io_uring` Event Loop & Sandbox (`power_governor.c`, `uring_loop.c`, `security.c`)**:
  - Disarms all render timers down to **`0.00%` CPU and GPU** when an output is occluded by a fullscreen or maximized window (`zwlr_foreign_toplevel_manager_v1` + Hyprland/Sway/Niri IPC), when the session is idle (`ext-idle-notify-v1`), or on laptop battery power (`/sys/class/power_supply`).
  - Runs on a Linux `io_uring` multishot poll event loop (`uring_loop.c`) with `epoll` fallback, hardened after initialization by Landlock v3 filesystem restrictions and a Seccomp-BPF syscall filter (`security.c`).

---

## 2. Build Dependencies

To build from source, you need a C23 compiler (`GCC >= 13` or `Clang >= 17`), `meson >= 0.61`, `ninja`, `pkg-config`, and `python3`.

| Group | `pkg-config` / Package Names | Role |
| :--- | :--- | :--- |
| **Core Wayland & DRM** | `wayland-client >= 1.20`, `wayland-protocols >= 1.34`, `wayland-scanner`, `libdrm >= 2.4.110`, `gbm`, `egl`, `glesv2` | Layer surfaces, DMA-BUF buffers, and GLES 3.1 pipelines |
| **Video & Kernel** | `libva`, `libva-drm`, `liburing`, `libseccomp` | VA-API video decode, `io_uring` loop, and Seccomp sandbox |
| **Base Image Formats** | `libpng`, `libturbojpeg` | Native PNG, APNG, and JPEG decode/encode |
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
  -Denable_simd=true \
  -Dswww_compat=true \
  -Dman-pages=enabled
```

---

## 3. Full CLI Reference (`wwal`)

`wwal` and `wwald` act as drop-in replacements when symlinked or invoked as `swww`, `awww`, `swww-daemon`, or `awww-daemon`.

```text
Usage: wwal [GLOBAL_OPTIONS] <COMMAND> [ARGS...]

Commands:
  img <PATH|-> [OPTIONS]         Universal wallpaper command (images, videos, shaders, Workshop, XML, dirs, hex)
  pick [DIR] [OPTIONS]           Unified icon-grid wallpaper picker (rofi, fuzzel, walker, dmenu, stdout, json)
  video <PATH> [OPTIONS]         Play hardware-accelerated video wallpaper (H.264, HEVC, VP9, AV1)
  shader <PATH|list|preset>      Run live GLSL (.comp/.frag), `list` (`--list`), or `preset <NAME>` (`--preset <NAME>`)
  workshop list                  Auto-discover installed Steam Workshop (431960) wallpapers
  workshop info <ID|PATH>        Inspect Workshop project.json, scene.pkg, and user properties
  workshop [apply] <ID|PATH>     Render native Wallpaper Engine scene, particles, Puppet Warp, sound/text & SceneScript
  workshop sync-previews [DIR]   Export human-readable Workshop preview symlinks for GUI pickers (waypaper)
  slideshow <DIR|FILE.xml|ACT>   Start or control timerfd / GNOME XML slideshow (stop, pause, resume, toggle, next, prev, previous)
  next / prev / previous [-o O]  Advance or rewind active slideshow on target output(s)
  theme [PATH] [-i <PATH>]       Extract < 0.45ms dual-mode OKLab/HCT palette, render templates & sync caelestia/pywal/OSC
  audio [OPTIONS]                Configure live wallpaper audio volume and external playback auto-ducking
  set-uniform <NAME> <V...>      Mutate live shader uniform or Workshop property without reloading
  status [--json]                Inspect outputs, active media, MPRIS metadata, and daemon telemetry
  stats [--json]                 Show live daemon RSS, DRM modifier, VBlank PLL, and pause telemetry
  query [--json]                 List connected Wayland outputs, physical/logical geometry, and scale
  subscribe                      Stream newline-delimited JSON state change events
  clear / color [COLOR]          Set solid hex color via 1x1 wp_single_pixel_buffer_v1
  restore                        Restore saved per-output wallpapers from state.bin
  pause / resume / toggle        Control active video, shader, or Workshop scene playback
  clear-cache                    Purge cached video transcodes, thumbnails, and compiled shader binaries
  init / daemon                  Launch or initialize wwald daemon (swww/awww compatibility)
  ping / kill / version          Check responsiveness, stop wwald, or print version info

Key Options:
  -n, --namespace <NAME>         Socket and layer-shell namespace (default: default)
  -a, --all                      Broadcast command to all active daemon namespaces in $XDG_RUNTIME_DIR
  -q, --quiet                    Suppress non-error output
  -o, --outputs <NAMES>          Comma-separated target outputs (e.g. DP-1,HDMI-A-1)
  -t, --transition-type <TYPE>   none, simple, fade, wipe, grow, outer, wave, noise, crosszoom,
                                 slide, glitch, burn, ripple, pixelate, doom, swirl, cube, luma,
                                 light_leak, page_curl, custom, left, right, top, bottom, center, any, random
  --transition-duration <SEC>    Transition duration in seconds as a float (e.g. 0.5, 1.0, 1.5)
  --transition-fps <FPS>         Transition frame rate (default: 60)
  --transition-bezier <B>        Cubic bezier control points X1,Y1,X2,Y2 (e.g. 0.25,0.1,0.25,1.0)
  --transition-pos <POS>         cursor, center, tl, tr, bl, br, top, bottom, left, right, X,Y, or X%,Y%
  --invert-y                     Invert Y coordinate of --transition-pos
  --scaling-mode, --resize <M>   fill (crop), fit, fit-border-color, stretch, center (no), tile, span
  --no-resize                    Center image at 1:1 pixel scale (alias for --resize no)
  --crop-gravity <GRAVITY>       center, top, bottom, left, right, tl, tr, bl, br, or X,Y / X%,Y%
  --fill-color <RRGGBB>          Padding color for fit/center modes
  -f, --filter <FILTER>          Nearest, Bilinear, CatmullRom, Mitchell, Lanczos3
  -f, --format <FMT>             Pixel format hint for swww/awww daemon compatibility (xrgb, xbgr, rgb, bgr)
  --no-cache / --clear-cache     Bypass or purge cached transition/thumbnail/palette artifacts
  --menu <MODE>                  Picker menu mode: auto, rofi, fuzzel, walker, dmenu, stdout, json
  --dir <DIR>                    Add search directory for `wwal pick`
  --interval, --duration, -d <D> Slideshow interval (seconds or 30s, 15m, 1h, 1h30m)
  --sort, --sorting <ORDER>      Slideshow sort order: ascending, descending, name, mtime, random
  --backdrop <MODE>              none, auto-blur (Dual-Kawase blur for Niri overview), or image path
  --depth-map <PATH>             2.5D depth map image for cursor/workspace Parallax Occlusion Mapping
  --depth-strength <FLOAT>       Depth parallax displacement multiplier (default: 1.0)
  -p, --prop <KEY=VALUE>         Override Wallpaper Engine Workshop user property (repeatable)
  --audio                        Enable PipeWire 64-bin Bark FFT uniforms & iChannel0 for shaders/scenes
  --volume <0.0..1.0>            Set wallpaper audio playback volume (default: 0.0)
  --mute-on-audio                Auto-duck wallpaper volume when another application plays audio
  -i, --img, --image <PATH>      Specify input image path for `wwal theme`
  --theme / --no-theme           Enable or disable automatic Material 3 / caelestia / template extraction
  --theme-mode <dark|light|auto> Select dark, light, or auto Material 3 theme polarity (--dark / --light)
  --auto-day-night               Auto-switch dark/light polarity based on [solar] elevation or time of day
  --latitude <DEG>               Observer latitude (-90..90) for astronomical solar elevation
  --longitude <DEG>              Observer longitude (-180..180) for astronomical solar elevation
  --source-color-index <0..3>    Select ranked K-Means++ seed cluster index (0 = dominant .. 3 = quaternary)
  --seed-color <HEX>             Override image quantization with explicit hex seed color (#RRGGBB)
  --theme-from-media             Dynamically extract Material 3 theme from active D-Bus MPRIS2 album art
  --layer <LAYER>                background or bottom
  --battery-policy <POLICY>      ignore, reduce-fps, or pause-video
  --10bit                        Enable 10-bit wide-gamut scanout (XRGB2101010 / ARGB2101010)
  -j, --json                     Format query, status, or stats output as structured JSON
  -V, --version                  Print version information
```

### Environment Variables

- **WayWal**: `WWAL_CONFIG_PATH`, `WWAL_CACHE_DIR`, `WWAL_STATE_DIR`, `WWAL_WORKSHOP_DIR`, `WWAL_WE_ASSETS_DIR`, `WWAL_SHADER_PRESET_DIR`, `WWAL_SHADER_DIR`, `WWAL_NAMESPACE_PREFIX`.
- **`swww` / `awww` v0.12.1 Compatibility**: `SWWW_NAMESPACE` / `AWWW_NAMESPACE`, `SWWW_TRANSITION` / `AWWW_TRANSITION`, `SWWW_TRANSITION_FPS` / `AWWW_TRANSITION_FPS`, `SWWW_TRANSITION_STEP` / `AWWW_TRANSITION_STEP`, `SWWW_TRANSITION_DURATION` / `AWWW_TRANSITION_DURATION`, `SWWW_TRANSITION_ANGLE` / `AWWW_TRANSITION_ANGLE`, `SWWW_TRANSITION_POS` / `AWWW_TRANSITION_POS`, `SWWW_TRANSITION_BEZIER` / `AWWW_TRANSITION_BEZIER`, `SWWW_TRANSITION_WAVE` / `AWWW_TRANSITION_WAVE`, `SWWW_INVERT_Y` / `AWWW_INVERT_Y`, `SWWW_TRANSITION_INVERT_Y` / `AWWW_TRANSITION_INVERT_Y`, `SWWW_CROP_GRAVITY` / `AWWW_CROP_GRAVITY`, `SWWW_FILTER` / `AWWW_FILTER`, `SWWW_RESIZE` / `AWWW_RESIZE`, `SWWW_FILL_COLOR` / `AWWW_FILL_COLOR`, `SWWW_OUTPUTS` / `AWWW_OUTPUTS`.

---

## 4. Repository Layout

```text
wwal/
├── README.md                          # User-friendly overview, installation, and examples
├── flake.nix                          # NixOS / Home Manager flake & declarative module
├── packaging/fedora/wwal.spec         # Fedora / openSUSE Copr RPM specification
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
│   ├── common/                        # Arena, IPC, Bezier, OKLab/HCT palette & templates, TOML, JSON, image loader, Workshop PKG
│   ├── daemon/                        # Wayland core, IPC server, GBM/DMA-BUF, VA-API codecs, Shaders, Workshop runtime, MPRIS, Audio
│   └── client/                        # CLI parser (wwal + swww/awww), unified picker, and client dispatch
└── tests/                             # Unit, subsystem, Workshop scene/script/particle, LibFuzzer, and E2E suites
```

---

## 5. Empirical Microbenchmarks

Measured with `OMP_NUM_THREADS=4 ./build/bench_transitions 60` and `python3 scripts/benchmark.py`:

| Subsystem / Kernel | Resolution / Workload | Mean / Min / P99 Latency | Throughput / Budget |
| :--- | :--- | ---: | :--- |
| **Stripped Binary Footprint (`wwald` + `wwal`)** | `x86_64` LTO `-O3` | `1,375.4 KB + 326.1 KB` | **`1.66 MB` total** (`0` heavy `DT_NEEDED` libs) |
| **GPU Compute Transitions (18 Shaders)** | `1920×1080` | `0.171 – 0.261 ms` | **3,828 – 5,831 FPS** (`0 KB` steady leak) |
| **GPU Compute Transitions (18 Shaders)** | `3840×2160` (4K) | `0.366 – 0.600 ms` | **1,668 – 2,734 FPS** (`0 KB` leak) |
| **CPU OpenMP + AVX2 Transitions** | `1280×720` | `0.041 – 0.850 ms` | **1,176 – 24,377 FPS** |
| **CPU OpenMP + AVX2 Transitions** | `3840×2160` (4K) | `3.743 – 13.402 ms` | **74.6 – 267.1 FPS** |
| **Dual-Mode OKLab/HCT Extraction (`dark`+`light`)** | `3840×2160` (1,000 iters) | `0.440 ms` (`min 0.421`, `p99 0.508`) | **~500× faster** than external CLI tools |
| **Superset Template Render (7 Filters + 32 Tokens)** | `Matugen+Wallust+Pywal` (1,000 iters) | `0.052 ms` (`min 0.045`, `p99 0.083`) | **`52 µs`** atomic render |
| **End-to-End Theme Pipeline (Extract+Derive+Render)** | `3840×2160` (1,000 iters) | `0.492 ms` (`min 0.468`, `p99 0.592`) | **< 0.50 ms** mean end-to-end |
| **Unified Picker (`wwal pick --menu json`)** | `14 Mixed Assets` | `0.93 ms` warm (`61.22 ms` cold) | **`< 1 ms`** warm icon-grid startup |
| **Lockscreen PNG Frame Export (`image_save_png_bgra`)** | `1280×720` / `4K->720p` | `13.03 ms` / `29.38 ms` | Asynchronous zero-frame-drop export |
| **2048-pt Hann AVX2 64-Bin Bark FFT** | `48 kHz` PCM | `0.012 ms` | **< 0.015 ms target** (lock-free SPSC ring) |
| **Zero-Watt Occluded / Paused Daemon State** | `1080p` / `4K` | `0.00%` CPU / `0.00%` GPU | **`0 wakeups/sec`** (`PSS: 54.3 MB`) |
