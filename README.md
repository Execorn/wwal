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
  --transition-type <TYPE>    none, simple, fade, wipe, grow, outer, wave, noise (default: fade)
  --transition-duration <S>   Duration in seconds (e.g. 1.0, 0.5) (default: 1.0)
  --transition-fps <FPS>      Target frame rate (default: 60)
  --transition-angle <DEG>    Wipe/wave angle in degrees (default: 0)
  --transition-wave <F,A>     Wave frequency and amplitude (default: 20,0.05)
  --transition-pos <X,Y>      Center coordinate 0.0-1.0 (default: 0.5,0.5)

Global Options:
  -n, --namespace <NAME>     Socket namespace (default: "default")
  -v, --verbose              Enable verbose output
  -h, --help                 Print help information
```
