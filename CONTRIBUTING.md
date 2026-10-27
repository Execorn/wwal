# Contributing to WayWal (`wwal`)

Bug reports, hardware compatibility fixes, new transition shaders, and performance patches are welcome.

## Build Requirements

You need a Linux system running a Wayland compositor that supports `wlr-layer-shell-unstable-v1`, plus a C compiler with C23 (`gnu2x`) support (`GCC >= 13` or `Clang >= 17`).

Required build packages (`pkg-config` names):
- `meson` (`>= 0.60`), `ninja`, `pkg-config`, `python3`
- `wayland-client` (`>= 1.20`), `wayland-protocols` (`>= 1.34`), `wayland-scanner`
- `libdrm` (`>= 2.4.110`), `gbm`, `egl`, `glesv2`
- `libva`, `libva-drm`
- `liburing`
- `libseccomp`
- `libpng`, `libturbojpeg`

Optional packages:
- `scdoc` (builds manual pages from `docs/man/*.scd`)
- `libwebp`, `libavif`, `libjxl`, `librsvg-2.0`, `libpipewire-0.3`, `libdbus-1`, `libsystemd` / `libelogind`, `liblz4`, `libquickjs` (loaded at runtime via `dlopen()`)

## Development Workflow

Configure a debug build with AddressSanitizer and UndefinedBehaviorSanitizer enabled:

```bash
meson setup build-san \
  --buildtype=debug \
  -Doptimization=1 \
  -Db_lto=false \
  -Db_lundef=false \
  -Db_sanitize=address,undefined

meson compile -C build-san
```

Run the test suite and standalone fuzzers before opening a pull request:

```bash
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
meson test -C build-san --print-errorlogs

./build-san/fuzz_demuxer
./build-san/fuzz_bitstream
./build-san/fuzz_config_cli
```

If your changes touch rendering, SIMD kernels, or palette extraction, run the transition benchmark on a release build:

```bash
meson setup build --buildtype=release -Doptimization=3 -Db_lto=true
meson compile -C build
OMP_NUM_THREADS=4 ./build/bench_transitions 60
```

## Compute Shaders & Manual Pages

- **Compute shaders (`src/daemon/shaders/*.comp`)**: Edit the standalone `.comp` files directly. Meson runs `scripts/embed_shaders.py` during compilation to generate `shaders_embedded.h` inside the build directory.
- **Manual pages (`docs/man/*.scd`)**: Edit the `.scd` source files. If `scdoc` is installed, regenerate the pre-rendered man pages in `docs/man/` (`scdoc < docs/man/wwal.1.scd > docs/man/wwal.1`) so systems without `scdoc` still get up-to-date pages.

## Code Style & Licensing

- All source and header files must start with `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Format C and header files with `clang-format` before committing:
  ```bash
  clang-format -i src/*/*.c src/daemon/*.h src/client/*.h src/daemon/simd/*.c src/daemon/simd/*.h include/waywal/*.h tests/*.c tests/*/*.c
  ```
- Keep hot paths (`uring_loop.c`, `render_engine.c`, `video_engine.c`, `audio_fft.c`) free of per-frame heap allocations. Use the bump arena (`waywal/arena.h`) or pre-allocated fixed rings.
- When adding a new syscall to the daemon runtime path, check `src/daemon/security.c` to make sure the Seccomp-BPF allowlist permits it.
- Use Conventional Commit prefixes (`feat:`, `fix:`, `perf:`, `docs:`, `build:`, `test:`).
