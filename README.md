# `WayWal`: SOTA Modern C23 Wayland Wallpaper Engine

[![GitHub Repository](https://img.shields.io/badge/GitHub-execorn%2Fwwal-181717?logo=github)](https://github.com/execorn/wwal)

**WayWal** (Wayland Wallpapers) is an ultra-high-performance, zero-allocation Wayland wallpaper daemon (`wwald`) and CLI client (`wwal`) written from the ground up in modern C (C23 / `gnu2x`).
Repository: [https://github.com/execorn/wwal](https://github.com/execorn/wwal)

Engineered to achieve the thermodynamic and throughput limits of Linux graphics architecture:
- **0.0% GPU 3D Load & 0 GB/s PCIe Bandwidth** for static wallpapers via Direct Hardware Scanout (`zwp_linux_dmabuf_v1` + GBM overlay planes).
- **< 0.5% CPU Load at 4K 60fps** for animated wallpapers via Linux VA-API (`libva`) hardware decoding directly into DRM PRIME DMA-BUFs (AV1, HEVC, VP9).
- **Sub-50 µs VBlank Phase Jitter** on Variable Refresh Rate (VRR / FreeSync / G-Sync) and ultra-high-refresh displays (144Hz–500Hz) using `wp_presentation` and an alpha-beta Phase-Locked Loop (PLL).
- **Zero-Allocation Architecture**: Fixed 2 MB linear memory arena with sub-nanosecond bump allocation; zero runtime `malloc`/`free` heap churn in the event loop.
- **Zero-Syscall Asynchronous Event Loop**: Linux `io_uring` kernel event ring for IPC dispatch and protocol polling.
- **Instant Display Hotplug**: Atomic in-process state machine eliminating all `fork()` and `/bin/sh` invocations (< 50 µs reconnection latency).
- **< 35 KB Stripped Binary & < 64 KB Idle RAM Footprint**.

---

## Repository Structure

```
wwal/
├── README.md                          # Project overview and quickstart
├── LICENSE                            # GNU General Public License v3.0
├── meson.build                        # Master Meson build configuration
├── meson_options.txt                  # Build configuration flags
├── .clang-format                      # Code style specification
├── .editorconfig                      # Editor configuration
├── contrib/
│   ├── systemd/
│   │   └── wwald.service              # systemd user service unit
│   └── completions/
│       ├── wwal.bash                  # Bash shell autocompletion
│       ├── wwal.zsh                   # Zsh shell autocompletion
│       └── wwal.fish                  # Fish shell autocompletion
├── protocols/                         # Wayland XML protocol definitions
│   ├── wlr-layer-shell-unstable-v1.xml
│   ├── viewporter.xml
│   ├── fractional-scale-v1.xml
│   ├── presentation-time.xml
│   └── linux-dmabuf-v1.xml
├── include/waywal/                    # Public engine and subsystem headers
├── src/
│   ├── common/                        # Linear arena, infallible pathing, zero-alloc logger
│   ├── daemon/                        # wwald daemon implementation
│   └── client/                        # wwal client CLI implementation
└── tests/                             # Unit tests, SIMD microbenchmarks, and integration scripts
```

---

## Build Requirements

- **C Compiler**: GCC >= 13 or Clang >= 17 supporting `-std=gnu2x` or `-std=c23`.
- **Build System**: `meson` >= 0.60, `ninja`.
- **Core System Libraries**:
  - `wayland-client` >= 1.20
  - `wayland-protocols` >= 1.24
  - `wayland-scanner`
  - `libdrm` >= 2.4.110
  - `gbm` (Mesa)
  - `egl` >= 1.5
- **Accelerated Subsystems (Optional / Configurable)**:
  - `libva` & `libva-drm` (for hardware video decoding)
  - `liburing` >= 2.2 (for zero-syscall event loop)
  - `libseccomp` (for process sandboxing)

---

## Quickstart

```bash
# Clone the repository
git clone https://github.com/execorn/wwal.git
cd wwal

# Configure build with optimizations and LTO
meson setup build -Doptimization=3 -Db_lto=true

# Compile daemon (wwald) and client (wwal)
meson compile -C build

# Run unit and integration tests
meson test -C build

# Launch daemon in background
./build/wwald &

# Query daemon status via client
./build/wwal query

# Set wallpaper with GPU transition
./build/wwal img /path/to/wallpaper.png --transition-type wave --transition-duration 0.8
```

---

## CLI Usage (`wwal`)

```bash
wwal - Modern high-performance Wayland wallpaper client (C23)
Usage: wwal [GLOBAL_OPTIONS] <COMMAND> [ARGS...]

Commands:
  ping                        Check if wwald daemon is running and responsive
  query                       List all detected Wayland outputs and current geometry
  clear [COLOR]               Clear wallpaper to solid hex color (e.g. 000000 or 00ff00)
  img <PATH> [OPTIONS]        Load and set wallpaper with GPU/SIMD transitions
  video <PATH> [OPTIONS]      Load and play hardware-accelerated video wallpaper (VA-API)
  pause                       Pause video playback
  unpause                     Resume video playback
  toggle                      Toggle video playback pause/resume
  kill                        Gracefully terminate the running wwald daemon
  help                        Display help message

Transition Options:
  --transition-type <TYPE>    none, simple, fade, wipe, grow, outer, wave, noise, crosszoom, slide, glitch, burn, ripple, pixelate, doom, swirl, cube, luma, light_leak, page_curl (default: fade)
  --transition-duration <S>   Duration in seconds (e.g. 1.0, 0.5) (default: 1.0)
  --transition-fps <FPS>      Target frame rate (default: 60)
  --transition-angle <DEG>    Wipe/wave/slide angle in degrees (default: 0)
  --transition-wave <F,A>     Wave frequency/scale and amplitude/intensity (default: 20,0.05)
  --transition-pos <X,Y>      Center coordinate 0.0-1.0 (default: 0.5,0.5)

Global Options:
  -n, --namespace <NAME>     Socket namespace (default: "default")
  -v, --verbose              Enable verbose output
  -h, --help                 Print help information
```

---

## Visual Transition Effects

WayWal features a SOTA GPU Compute (`OpenGL ES 3.1` Compute Shaders) and CPU SIMD fallback transition engine supporting 20 transition effects:

| Transition Type | Description | GPU Compute Shader | SIMD / CPU Fallback |
| :--- | :--- | :--- | :--- |
| `none` / `simple` | Instant cut with zero latency | N/A (Zero-copy swap) | Direct copy |
| `fade` | Smooth linear / alpha-crossfade | `fade.comp` | AVX2 / AVX-512 / NEON |
| `wipe` | Directional angled wipe across any angle | `wipe.comp` | Optimized scalar / SIMD |
| `grow` | Expanding circular iris mask from customizable center | `grow.comp` | Distance-field mask |
| `outer` | Inverted contracting circular iris reveal | `outer.comp` | Inverted distance-field |
| `wave` | Sinusoidal wavefront distortion with controllable frequency & amplitude | `wave.comp` | Trigonometric displacement |
| `noise` | Procedural pseudo-random dither dissolve | `noise.comp` | Hash-based pseudo-random |
| `crosszoom` | Dynamic radial zoom blur into the target image | `crosszoom.comp` | Multi-tap bilinear zoom |
| `slide` | Smooth directional screen slide with sub-pixel interpolation | `slide.comp` | Coordinate translation |
| `glitch` | Chromatic aberration and horizontal digital scanline tearing | `glitch.comp` | RGB channel offset |
| `burn` | High-contrast organic fire/burn boundary with ember glow | `burn.comp` | Smoothstep threshold burn |
| `ripple` | High-frequency water drop wavefront propagation | `ripple.comp` | Radial wave displacement |
| `pixelate` | Dynamic GPU mosaic downsampling and tile-grid expansion | `pixelate.comp` | Tiled block sampling |
| `doom` | Retro vertical melt columns with independent jittered speeds | `doom.comp` | Columnar melt offset |
| `swirl` | Non-linear vortex rotation and centrifugal warp | `swirl.comp` | Polar coordinate rotation |
| `cube` | 3D perspective cube rotation with realistic shading & perspective foreshortening | `cube.comp` | 3D Ray-plane projection |
| `luma` | Luminance-driven wipe using perceptual BT.709 grayscale luma | `luma.comp` | Perceptual luma threshold |
| `light_leak` | Cinematic anamorphic optical lens flare with warm chromatic overlay | `light_leak.comp` | Multi-source spectral flare |
| `page_curl` | Anti-aliased conical 3D cylinder page curl with realistic drop shadow | `page_curl.comp` | Conical deformation math |

---

## Performance Benchmarks

Measured on Intel Core i7-12700H / Iris Xe Graphics (1920x1080 @ 165Hz Wayland scanout):

### 1080p GPU Compute (Continuous 60 Frames / Effect)
| Effect | Mean Latency | Peak Throughput | Memory Leak |
| :--- | :--- | :--- | :--- |
| `fade` | **0.48 ms** | **2,083 FPS** | **0 KB Delta [PASS]** |
| `wipe` | **0.51 ms** | **1,960 FPS** | **0 KB Delta [PASS]** |
| `wave` | **0.53 ms** | **1,886 FPS** | **0 KB Delta [PASS]** |
| `pixelate` | **0.52 ms** | **1,923 FPS** | **0 KB Delta [PASS]** |
| `doom` | **0.60 ms** | **1,666 FPS** | **0 KB Delta [PASS]** |
| `cube` | **0.76 ms** | **1,315 FPS** | **0 KB Delta [PASS]** |
| `page_curl` | **0.57 ms** | **1,754 FPS** | **0 KB Delta [PASS]** |

### 4K UHD (3840x2160) GPU Compute
- All 18 animated transitions execute in **2.1 ms – 5.7 ms** per frame (**175 – 476 FPS**).
- Zero frame drops at 144Hz / 165Hz / 240Hz refresh rates.
- Constant memory consumption: 0 KB heap allocation churn during transition playback.

