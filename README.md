# `WayWal`: Modern C23 Wayland Wallpaper Engine

[![GitHub Repository](https://img.shields.io/badge/GitHub-execorn%2Fwwal-181717?logo=github)](https://github.com/execorn/wwal)

**WayWal** is a fast, low-memory Wayland wallpaper daemon (`wwald`) and command-line client (`wwal`) written in modern C (C23 / `gnu2x`).
Repository: [https://github.com/execorn/wwal](https://github.com/execorn/wwal)

WayWal sets static images, animated GIFs, and hardware-decoded videos as your desktop background with smooth GPU and SIMD transitions:
- **0.0% GPU 3D Load & 0 GB/s PCIe Bandwidth** for static wallpapers through Direct Hardware Scanout (`zwp_linux_dmabuf_v1` + GBM overlay planes).
- **< 0.5% CPU Load at 4K 60fps** for video and animated GIF wallpapers using Linux VA-API (`libva`) hardware decoding directly into DRM PRIME DMA-BUFs.
- **Sub-50 µs VBlank Jitter** on Variable Refresh Rate (FreeSync / G-Sync) and high-refresh displays (144Hz–500Hz) via `wp_presentation` and an alpha-beta Phase-Locked Loop (PLL).
- **Fixed-Arena Memory Architecture**: Uses a 2 MB linear memory arena with bump allocation so the event loop runs without runtime `malloc` or `free` churn.
- **Asynchronous Kernel Event Loop**: Uses Linux `io_uring` (with automatic `epoll` fallback) for IPC dispatch, Wayland events, and timers.
- **Defense-in-Depth Sandboxing**: Restricts the daemon with Landlock LSM filesystem rules, Seccomp-BPF syscall filters, and `PR_SET_NO_NEW_PRIVS`.

> [!TIP]
> WayWal runs 18 built-in transition shaders on the GPU via OpenGL ES 3.1 compute shaders on GBM, and includes a full OpenMP + AVX-512 / AVX2 / NEON CPU fallback engine that renders 720p transitions at **1,548 – 26,086 FPS**.

---

## Repository Structure

```
wwal/
├── README.md                          # Project overview, usage, and benchmarks
├── LICENSE                            # GNU General Public License v3.0
├── meson.build                        # Meson build configuration and test targets
├── meson_options.txt                  # Build configuration flags
├── .clang-format                      # Code formatting rules
├── .editorconfig                      # Editor configuration
├── contrib/
│   ├── PKGBUILD                       # Arch Linux package build script
│   ├── systemd/
│   │   └── wwald.service              # systemd user service unit
│   └── completions/
│       ├── wwal.bash                  # Bash shell completion
│       ├── wwal.zsh                   # Zsh shell completion
│       └── wwal.fish                  # Fish shell completion
├── protocols/                         # Wayland XML protocol definitions
│   ├── wlr-layer-shell-unstable-v1.xml
│   ├── viewporter.xml
│   ├── fractional-scale-v1.xml
│   ├── presentation-time.xml
│   ├── linux-dmabuf-v1.xml
│   └── xdg-shell.xml
├── include/waywal/                    # Public engine and subsystem headers
├── scripts/
│   ├── build_optimized.sh             # Full PGO + LTO + BOLT release build script
│   └── benchmark.py                   # Automated benchmark runner
├── src/
│   ├── common/                        # Linear arena, path helpers, IPC framing, logger
│   ├── daemon/                        # wwald daemon, DRM/GBM, VA-API, Wayland core
│   │   ├── shaders/                   # 18 standalone GLES 3.1 .comp shaders & embedded header
│   │   └── simd/                      # AVX-512, AVX2, NEON, and scalar scanline blenders
│   └── client/                        # wwal CLI parser, image loader, GIF transcoder
└── tests/                             # Unit, advanced, benchmark, and 4-tier E2E test suites
    └── e2e/                           # 207 E2E tests (feature, boundary, combo, stress)
```

---

## Build Requirements & Portability Notes

- **Operating System**: Linux with a Wayland compositor supporting `wlr-layer-shell-unstable-v1` (Hyprland, Sway, River, Niri, Wayfire, Labwc).
- **C Compiler**: GCC >= 13 or Clang >= 17 supporting `-std=gnu2x` or `-std=c23`.
- **Build System**: `meson` >= 0.60 and `ninja`.
- **Core Libraries**:
  - `wayland-client` >= 1.20, `wayland-protocols` >= 1.24, `wayland-scanner`
  - `libdrm` >= 2.4.110, `gbm` (Mesa), `egl` >= 1.5, `glesv2` (GLES 3.1)
  - `OpenMP` (for multi-threaded CPU transition rendering)
- **Optional Libraries**:
  - `libva` & `libva-drm` (hardware video decoding)
  - `liburing` >= 2.2 (`io_uring` event loop; falls back to `epoll` when disabled)
  - `libseccomp` (Seccomp-BPF syscall sandboxing)
  - `ffmpeg` (optional runtime helper for JIT animated GIF transcoding)

---

## Installation & Quickstart

### Build from Source

```bash
# Clone the repository
git clone https://github.com/execorn/wwal.git
cd wwal

# Configure release build with Link-Time Optimization (LTO)
meson setup build --buildtype=release -Doptimization=3 -Db_lto=true

# Compile wwald (daemon) and wwal (client)
ninja -C build

# Run all 8 unit, integration, and E2E test suites
meson test -C build --print-errorlogs

# Install binaries to ~/.local/bin
install -m 755 build/wwal build/wwald ~/.local/bin/
```

### Arch Linux (`PKGBUILD`)

```bash
cd contrib
makepkg -si
```

### Basic Usage

```bash
# Start the daemon in the background (or enable contrib/systemd/wwald.service)
wwald &

# Check connected Wayland monitors
wwal query

# Set a wallpaper with a radial ripple transition anchored at the mouse cursor
wwal img ~/Pictures/Wallpapers/scenery.jpg --transition-type ripple --transition-pos cursor

# Set an animated GIF wallpaper using the hardware video engine
wwal img ~/Pictures/Wallpapers/rain.gif --animate

# Start a shuffled slideshow with a 5-minute interval and staggered monitor transitions
wwal slideshow ~/Pictures/Wallpapers -d 300 -s --transition-type burn --sync-mode staggered --stagger-delay 150

# Run a custom OpenGL ES 3.1 compute shader transition
wwal img ~/Pictures/Wallpapers/cyberpunk.png --transition-type custom --transition-shader ./tests/custom_test.comp
```

---

## CLI Reference (`wwal`)

```text
wwal - Modern high-performance Wayland wallpaper client (C23)
Usage: wwal [GLOBAL_OPTIONS] <COMMAND> [ARGS...]

Commands:
  ping                       Check if wwald daemon is running and responsive
  query                      List all detected Wayland outputs and current geometry
  clear [COLOR] [OPTIONS]    Clear wallpaper to solid hex color (e.g. 000000 or 00ff00)
  img <PATH> [OPTIONS]       Load and set wallpaper with GPU/SIMD transitions
  slideshow <DIR> [OPTIONS]  Start zero-idle-CPU timerfd wallpaper slideshow
  slideshow stop             Stop active wallpaper slideshow
  slideshow pause            Pause active slideshow
  slideshow resume           Resume paused slideshow
  slideshow toggle           Toggle pause/resume state
  slideshow next             Advance slideshow to next image immediately
  slideshow prev             Step slideshow to previous image
  video <PATH> [OPTIONS]     Play hardware-accelerated video wallpaper (VA-API)
  pause                      Pause video playback
  unpause                    Resume video playback
  toggle                     Toggle video playback pause/resume
  kill                       Gracefully terminate the running wwald daemon
  help                       Display this help message

Slideshow Options:
  -d, --interval <SECONDS>   Seconds between wallpaper changes (default: 300)
  -s, --shuffle, --random    Shuffle images into random playback order

Image & Transition Options:
  --transition-type <TYPE>   none, simple, fade, wipe, grow, outer, wave, noise,
                             crosszoom, slide, glitch, burn, ripple, pixelate,
                             doom, swirl, cube, luma, light_leak, page_curl, custom,
                             random (default: fade)
  --transition-duration <S>  Duration in seconds (e.g. 1.0, 0.5) (default: 1.0)
  --transition-fps <FPS>     Target frame rate (default: 60)
  --transition-angle <DEG>   Wipe/wave/slide angle in degrees (default: 0)
  --transition-wave <F,A>    Wave frequency and amplitude (default: 20,0.05)
  --transition-pos <POS>     Transition anchor: cursor, center, top, bottom, left,
                             right, top-left, top-right, bottom-left, bottom-right,
                             or <X,Y> coordinates 0.0-1.0 (default: center)
  --transition-shader <FILE> Path to GLSL compute shader (*.comp) for custom transitions
  -o, --output <NAME>        Target specific output(s) (repeatable or comma-separated)
  --sync-mode <MODE>         Multi-monitor sync: simultaneous or staggered (default: simultaneous)
  --stagger-delay <MS>       Delay in milliseconds between cascading monitors (default: 150)
  --10bit                    Enable 10-bit wide-gamut scanout if supported by compositor
  --scaling-mode, --mode <M> Scaling mode: fill, fit, stretch, center, tile (default: fill)
  --animate                  Animate GIF wallpaper using hardware video engine

Video Options:
  --loop <COUNT>             Loop count (0 = infinite) (default: 0)
  --speed <FLOAT>            Playback speed multiplier (default: 1.0)

Global Options:
  -n, --namespace <NAME>     Socket namespace (default: "default")
  -v, --verbose              Enable verbose debug output
  -h, --help                 Print help information
```

---

## Core Subsystems & Configuration

### 1. Zero-Idle-CPU Slideshow Engine
- Uses a kernel `timerfd` registered in the daemon's `io_uring` / `epoll` event loop, using **0.0% CPU** between transitions.
- Scans directories, filters invalid files, and shuffles entries with Fisher-Yates cycle protection.
- Supports live `pause`, `resume`, `toggle`, `next`, and `prev` commands over IPC.

### 2. Multi-Monitor Targeting & Staggered Sync
- Pin wallpapers to specific monitors with `-o eDP-1` or `-o DP-3,DP-4`.
- Use `--sync-mode staggered --stagger-delay 150` to cascade transitions from left to right across multi-monitor layouts.

### 3. Cursor-Anchored Transitions
- Pass `--transition-pos cursor` (or `mouse`) to start radial transitions (`grow`, `outer`, `ripple`, `swirl`, `crosszoom`) from the current pointer position via `wl_pointer` with Hyprland/Sway IPC fallback.

### 4. Scaling Modes & 10-Bit Scanout
- Supports `fill` (default isotropic crop-to-fill), `fit` (letterboxed), `stretch`, `center`, and `tile` scaling with fixed-point bilinear filtering.
- Supports 10-bit wide-gamut direct scanout (`DRM_FORMAT_XRGB2101010`) with `--10bit`.

---

## Visual Transition Effects

WayWal ships with 18 procedural effects implemented in both **OpenGL ES 3.1 Compute Shaders** (`src/daemon/shaders/*.comp`) and **OpenMP + AVX2/AVX-512/NEON SIMD C23 Kernels** (`src/daemon/render_engine.c`):

| Transition Type | Description | GPU Shader (`src/daemon/shaders/`) | CPU SIMD / OpenMP Kernel |
| :--- | :--- | :--- | :--- |
| `none` / `simple` | Instant buffer swap | Zero-copy BO flip | Direct buffer copy |
| `fade` | Smooth alpha crossfade | `fade.comp` | AVX-512 / AVX2 / NEON scanline blend |
| `wipe` | Angled directional plane wipe | `wipe.comp` | Analytical 3-span `memcpy` + AVX2 FMA seam |
| `grow` | Expanding circular iris from center/cursor | `grow.comp` | Analytical 5-span chord `memcpy` + AVX2 annulus |
| `outer` | Contracting circular iris reveal | `outer.comp` | Analytical 5-span chord `memcpy` + AVX2 annulus |
| `wave` | Sinusoidal wavefront distortion | `wave.comp` | Analytical 3-span `memcpy` + AVX2 LUT gather |
| `noise` | Procedural value-noise dissolve | `dissolve_noise.comp` | Separable 1D forward-diff + AVX2 FMA blend |
| `crosszoom` | Radial zoom and flash cross-dissolve | `crosszoom.comp` | 2-tap bilinear GPU / AVX2 gather + saturating add |
| `slide` | Directional push/slide with motion blur | `slide.comp` | Analytical 3-span + contiguous shifted `memcpy` |
| `glitch` | Digital block tear and chromatic aberration | `glitch.comp` | Hoisted block table + 3-op AVX2 RGB muxing |
| `burn` | Dual-octave FBM combustion wavefront | `burn.comp` | Separable 2-octave FBM + AVX2 saturating ember |
| `ripple` | Expanding refractive water shockwave | `ripple.comp` | 5-span chord `memcpy` + AVX2 NR-`rsqrt` annulus |
| `pixelate` | Dynamic mosaic block transition | `pixelate.comp` | First-row block blend + `memcpy` row replication |
| `doom` | Classic vertical column screen melt | `doom.comp` | Row-major scanline OpenMP + 16-px `memcpy` |
| `swirl` | Radial vortex warp around center/cursor | `swirl.comp` | Chord clipping + AVX2 Cody-Waite `sincos` gather |
| `cube` | 3D perspective rotating cube | `cube.comp` | $O(\log W)$ trapezoid clip + AVX2 SoA gather |
| `luma` | Organic gradient-noise luma dissolve | `luma.comp` | Separable SoA LUT + AVX2 uniform-block bypass |
| `light_leak` | Anamorphic diagonal lens flare sweep | `light_leak.comp` | Active-beam interval clip + AVX2 Gaussian LUT |
| `page_curl` | Cylindrical 3D page peel with contact shadow | `page_curl.comp` | Analytical 3-span `memcpy` + 256-entry shade LUT |
| `custom` | User-supplied GLSL ES 3.1 compute shader | User `.comp` file | CPU fallback |

---

## Performance Benchmarks

Measured with `./build/bench_transitions 60` (`OMP_NUM_THREADS=4`):

### GPU Compute Engine (GLES 3.1 on GBM, 1080p & 4K UHD)

| Transition | 1080p Mean | 1080p Max FPS | 4K UHD Mean | 4K UHD Max FPS | 4K RSS Leak |
| :--- | ---: | ---: | ---: | ---: | :--- |
| `page_curl` | `0.094 ms` | **10,609 FPS** | `0.231 ms` | **4,320 FPS** | `0 KB` |
| `grow` | `0.098 ms` | **10,205 FPS** | `0.239 ms` | **4,180 FPS** | `0 KB` |
| `outer` | `0.099 ms` | **10,074 FPS** | `0.241 ms` | **4,142 FPS** | `0 KB` |
| `doom` | `0.100 ms` | **10,047 FPS** | `0.203 ms` | **4,937 FPS** | `0 KB` |
| `pixelate` | `0.103 ms` | **9,755 FPS** | `0.208 ms` | **4,817 FPS** | `0 KB` |
| `wave` | `0.103 ms` | **9,662 FPS** | `0.227 ms` | **4,409 FPS** | `0 KB` |
| `noise` | `0.109 ms` | **9,170 FPS** | `0.272 ms` | **3,671 FPS** | `0 KB` |
| `wipe` | `0.111 ms` | **9,047 FPS** | `0.243 ms` | **4,118 FPS** | `0 KB` |
| `glitch` | `0.114 ms` | **8,765 FPS** | `0.284 ms` | **3,520 FPS** | `0 KB` |
| `ripple` | `0.116 ms` | **8,615 FPS** | `0.254 ms` | **3,930 FPS** | `0 KB` |
| `slide` | `0.122 ms` | **8,187 FPS** | `0.263 ms` | **3,796 FPS** | `0 KB` |
| `swirl` | `0.123 ms` | **8,114 FPS** | `0.299 ms` | **3,340 FPS** | `0 KB` |
| `fade` | `0.127 ms` | **7,903 FPS** | `0.265 ms` | **3,778 FPS** | `0 KB` |
| `cube` | `0.127 ms` | **7,899 FPS** | `0.294 ms` | **3,401 FPS** | `0 KB` |
| `crosszoom` | `0.138 ms` | **7,256 FPS** | `0.322 ms` | **3,107 FPS** | `0 KB` |
| `burn` | `0.141 ms` | **7,109 FPS** | `0.332 ms` | **3,010 FPS** | `0 KB` |
| `light_leak` | `0.147 ms` | **6,792 FPS** | `0.274 ms` | **3,652 FPS** | `0 KB` |
| `luma` | `0.175 ms` | **5,714 FPS** | `0.411 ms` | **2,431 FPS** | `0 KB` |

### CPU OpenMP + AVX2/SIMD Engine (`1280x720`, 4 Threads)

| Transition | Mean Frame Time | Throughput | Transition | Mean Frame Time | Throughput |
| :--- | ---: | ---: | :--- | ---: | ---: |
| `pixelate` | `0.038 ms` | **26,087 FPS** | `page_curl` | `0.153 ms` | **6,532 FPS** |
| `wipe` | `0.044 ms` | **22,611 FPS** | `glitch` | `0.156 ms` | **6,415 FPS** |
| `grow` | `0.046 ms` | **21,778 FPS** | `crosszoom` | `0.198 ms` | **5,044 FPS** |
| `outer` | `0.047 ms` | **21,092 FPS** | `cube` | `0.202 ms` | **4,951 FPS** |
| `doom` | `0.058 ms` | **17,311 FPS** | `luma` | `0.249 ms` | **4,014 FPS** |
| `wave` | `0.088 ms` | **11,315 FPS** | `noise` | `0.267 ms` | **3,752 FPS** |
| `fade` | `0.101 ms` | **9,892 FPS** | `ripple` | `0.314 ms` | **3,186 FPS** |
| `slide` | `0.123 ms` | **8,122 FPS** | `burn` | `0.512 ms` | **1,953 FPS** |
| `light_leak` | `0.142 ms` | **7,043 FPS** | `swirl` | `0.646 ms` | **1,548 FPS** |

---

## Testing & Contributing

> [!IMPORTANT]
> Any modification to a standalone compute shader in `src/daemon/shaders/*.comp` must also update its matching `SHADER_SRC_*` string literal in `src/daemon/shaders/shaders_embedded.h`. The `waywal:e2e` test suite checks character-for-character equality between standalone `.comp` files and `shaders_embedded.h`.

Run the full verification and benchmark suite before submitting pull requests:

```bash
ninja -C build
meson test -C build --print-errorlogs
OMP_NUM_THREADS=4 ./build/bench_transitions 60
```
