# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **50-Shader Built-In Workshop Effect Library & HLSL AST Transpiler (`we_builtin_effects.c`, `we_shader.c`)**: Clean-room stock post-processing, distortion, lighting, color-grading, and particle shader library stored as LZ4-compressed `.rodata` blocks (`< 25 µs` full-table decompression) with Robin-Hood FNV-1a lookup and a Pratt AST HLSL-to-GLSL ES 3.10 transpiler (`tex2D*`, `rcp`, `mad`, `atan2`, `[COMBO]` integer preprocessor evaluation, `flat`/`centroid` varyings, and `mul(A, B)` side-effect hoisting).
- **240 Hz Substepped XPBD Puppet Spring Physics & 3D Curl-Noise Wind (`we_physics.c`, `we_puppet.c`)**: Frame-rate-invariant (`30`–`240 Hz`) Extended Position-Based Dynamics skeletal spring-bone solver with 6 material presets (`Silk`, `LongHair`, `HeavyCape`, `FoliageBranch`, `SoftRibbon`, `MetallicChain`), analytic divergence-free 3D Simplex curl-noise wind fields, Wayland cursor SDF repulsion/vortex coupling, and PipeWire Bark-band audio impulses.
- **3D Perspective Camera, Catmull-Rom + SQUAD Spline & 8-Light Engine (`we_camera3d.c`, `we_scene.c`)**: Right-handed GLES 3.1 perspective frustum camera with 5-point Gauss-Legendre arc-length centripetal Catmull-Rom position splines, Shoemake SQUAD quaternion orientation slerp, Circle-of-Confusion depth of field, `GL_DEPTH_COMPONENT24` two-subpass depth rendering, and `std140` 8-light (`Point`, `Spot`, `Directional`, `Tube`) uniform packing.
- **Visual Shader Graph DAG & SSA GLSL ES 3.10 Compiler (`shader_graph.c`)**: Multi-pass node-graph compiler featuring Tarjan $O(V + E)$ SCC cycle rejection with temporal `BUFFER_A..D` feedback latch exemption, spatial convolution barrier pass scheduling, Kahn topological sort with Sethi-Ullman register pressure minimization, forced opaque `IMAGE_PASS` alpha, and bounded `.wwgraph` JSON serialization.
- **Shader Graph, 3D Camera, Physics & Lighting IPC (`0x001D`..`0x0020`, `0x800C`..`0x800F`) & Studio Docks**: Added `WAYWAL_REQ_COMPILE_SHADER_GRAPH`, `WAYWAL_REQ_SET_CAMERA_3D`, `WAYWAL_REQ_SET_PHYSICS_PARAMS`, and `WAYWAL_REQ_SET_SCENE_LIGHTS` wire messages, 500 ms micro-drag `CommandBus` coalescing, and 4 dockable `wwal-studio` panels (`NodeGraphEditorWidget`, `EffectLibraryBrowserWidget`, `CameraLightingInspectorWidget`, `PhysicsSpringInspectorWidget`) under `DockLayoutManager` layout revision `5`.

### Fixed

- **`.wwpkg` Cryptographic, Tar & JCS Hardening (`wwpkg.c`, `creator_packaging_wizard.cpp`)**: Enforced RFC 8032 canonical scalar `S < L`, fixed 32-byte seed signing (`wwpkg_ed25519_seed_keypair`), bound normalized paths and byte lengths into v2 RFC 6962 Merkle leaves, capped streaming Zstd decompression (`<= 512 MiB`, `<= 4,096` files), hardened ustar header bounds and RFC 8785 JCS UTF-16 surrogate/locale-independent float canonicalization, parsed `trusted_keys.json` structurally with real notary Ed25519 verification, and enabled shader/video packaging in `CreatorPackagingWizard`.
- **Confined Compatibility Repair & Workshop Scanner Safety (`we_compat_scanner.c`, `we_vdf.c`)**: Confined all `we_compat_apply_repairs()` reads/writes beneath `target_dir` via `openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)`, implemented `REPAIR_SYNTHESIZE_USER_PROP` and swizzle-safe `REPAIR_PROMOTE_VARYING`, preserved Period-8/16 rolling XOR `.pkg` entry counts, added chained 256 KiB VDF arena slabs, and switched `getpwuid()` to `getpwuid_r()`.
- **Asynchronous Daemon IPC Workers, `IpcClient` Queuing & Fullscreen Media Preservation (`ipc_server.c`, `ipc_client.cpp`, `layer_shell.c`, `vaapi_h264.c`)**: Offloaded `WORKSHOP_SYNC`, `WORKSHOP_DOWNLOAD`, `WWPKG_INSTALL` (`SCM_RIGHTS`), and `SCAN_COMPAT` onto bounded daemon worker threads with isolated subscriber reply FDs, marshaled `IpcClient` callbacks onto the Qt GUI thread, preserved active media across fullscreen `zwlr_layer_surface_v1` recreation, rejected zero-plane VA-API PRIME exports, and expanded H.264 High-Profile SPS RBSP parsing.
- **Studio Download Queue, Catalog Watcher & Dock Security (`download_manager.cpp`, `recursive_fs_watcher.cpp`, `catalog_index_manager.cpp`, `openhub_browser_widget.cpp`)**: Fixed SteamUGC init and `steamcmd` `waitpid` status handling, enforced HTTPS-only SSRF bounds and HTTP `200`/`206`/`416`/`429` semantics, escaped untrusted inspector HTML, gated package apply on verified Ed25519 signatures, and enabled LeakSanitizer (`detect_leaks=1`) in CI alongside Studio RPM/Nix/Deb packaging.
- **Shader Graph Runtime Bridge, XPBD Substep & Studio Lifecycle Hardening (`shader_engine.c`, `we_physics.c`, `we_camera3d.c`, `wwpkg.c`, `gpu_preview_viewport.cpp`)**: Bridged multi-pass `#pragma Pass` `.wwgraph` shaders in `wwald` and `wwal-studio` with double-buffered ping-pong FBOs, switched puppet XPBD to topological outer-substep solving with 2D capsule cursor SDFs and signed asymmetric bone limits, hardened HLSL comment-masked `mul()` hoisting and `.wwgraph` 7x7 type coercion, enforced single-FD `.wwpkg` TOCTOU verification and float-to-integer clamping, and fixed Studio worker teardown, HiDPI PBO sizing, and `CommandBus` inspector undo coalescing.

### Performance

- **Dual-Frame `.wwpkg` Container & SHA-NI Merkle Hashing (`wwpkg.c`)**: Added a 176-byte outer Ed25519 skippable Zstd envelope (`< 60 µs` pre-decompression `mmap` metadata verification via `wwpkg_open_metadata_only()`) and runtime x86-64 CPUID SHA-NI acceleration.
- **Zero-`stat` Workshop Delta Reconcile, `io_uring` `STATX`, FTS5 Trigram & ETC2 PBO Grid (`we_appworkshop.c`, `catalog_index_manager.cpp`, `virtualized_grid_widget.cpp`)**: Added open-addressed Fibonacci hashing + SIMD structural ACF parsing, zero-`stat` SQLite WAL manifest delta reconciliation with batched `io_uring` `STATX`, FTS5 `trigram` + 64-bit Myers bit-parallel fuzzy ranking, and double-buffered PBO `GL_COMPRESSED_SRGB8_ETC2` thumbnail uploads.
- **Event-Driven HTTP/2 Download Reactor & Wayland `wp_commit_timing_v1` / `wp_fifo_v1` Pacing (`download_manager.cpp`, `video_engine.c`)**: Replaced blocking curl workers with a `curl_multi_socket_action` + `epoll` HTTP/2 `4+1` lane reactor and added hardware Wayland `wp_commit_timing_v1` + `wp_fifo_v1` video presentation pacing with `<= 1` bounded queue depth.
- **SIMD Curl-Noise, Spline LUT, RCU Catalog & Perceptual ETC2 Array Grid (`we_physics.c`, `we_camera3d.c`, `catalog_soa_rcu_pool.hpp`, `etc2_simd_compressor.hpp`, `node_graph_editor_widget.cpp`)**: Vectorized 8-wide AVX2+FMA 3D Simplex curl-noise (`>6.5×` speedup), cached 1,024-sample camera Catmull-Rom/SQUAD spline LUTs with Gribb-Hartmann 6-plane frustum culling, added wide LZ4 match copying, 23-bit GLSL dead-helper pruning, `dirty_pass_mask` uniform coalescing, and AVX2 VDF lexing, and upgraded `wwal-studio` with a 4-slot read-only SQLite pool + lock-free `CatalogSoaSnapshot` RCU reader (`0.48 ms` 50k BM25), `UniformPinSpatialGrid` $O(1)$ hit-testing, and AVX2 Rec.709 `GL_COMPRESSED_SRGB8_ETC2` `GL_TEXTURE_2D_ARRAY` thumbnails (`59 MB` VRAM for 1,500 layers).

## [3.0.0] - 2026-11-09

### Added

- **WayWal Studio (`wwal-studio`) Desktop IDE**: Native C++23 / Qt 6 workstation featuring dockable Workspace Presets (`Browse`, `Inspect & Tune`, `Theme Lab`), a 144 FPS Virtualized Media Grid with zero-copy DMA-BUF hover previews, a live GPU Preview Viewport, a Multi-Monitor Topology Canvas (`Fill`, `Fit`, `Center`, `Stretch`, `Tile`, `Span`), an interactive Cubic-Bezier Transition Curve Editor, a real-time Shader & Workshop Property Inspector, and a live Material Design 3 (`OKLab` / `CAM16` / `HCT`) Theme Studio with WCAG 2.1 contrast auditing and dotfile template export.
- **Multi-Drive Steam Workshop (`431960`) Discovery & Hybrid Filesystem Watcher**: Automatic parsing of Valve KeyValues VDF (`libraryfolders.vdf`, `appworkshop_431960.acf`) across all mounted Steam libraries, coupled with a hybrid `fanotify` (`FAN_MARK_FILESYSTEM`) and recursive `inotify` watcher with `epoll` + `timerfd` 300 ms debounce and 1,000 ms flush ceiling.
- **Embedded SQLite3 WAL + FTS5 Catalog Index**: Sub-millisecond (`< 0.1 ms` at 50,000 items) full-text prefix search and multi-faceted filtering backed by 6 covering indices and zero-churn FTS5 synchronization triggers.
- **Multi-Backend Download & Subscription Queue Manager**: 3-tier download arbitration (`libsteam_api.so` ISteamUGC runtime `dlopen`, Steam Web API + HTTP/2 byte-range resume worker with full-jitter HTTP 429 backoff, and asynchronous `steamcmd` pipe runner) governed by a Token Bucket bandwidth limiter and EWMA speed/ETA estimator.
- **WayWal Open Hub & Cryptographic `.wwpkg` Ecosystem**: Open `.wwpkg` (`tar.zst`) wallpaper package container with RFC 8785 JSON Canonicalization Scheme (JCS), RFC 6962 domain-separated SHA-256 Merkle tree proofs (`0x00` leaf / `0x01` internal node), RFC 8032 Ed25519 digital signatures (`216`-byte `wwpkg_signature_t`), and a 6-step `CreatorPackagingWizard` in `wwal-studio`.
- **Deterministic 4-Tier Wallpaper Engine Compatibility Scanner (`we_compat_scanner`)**: Pre-apply static & AST analyzer for `PKGV0001`..`PKGV0024` archives with rolling XOR de-masking, PE/ELF/Zip-Slip quarantine, 6-pass `scene.json` / GLSL ES 3.10 AST auditing, automatic shader repair (`highp float`, HLSL-to-GLSL intrinsics, `fragColor.a = 1.0`), and a QuickJS instruction/memory watchdog (`<= 2.0 ms`, `16 MiB` heap, `100,000` opcodes).
- **Extended Daemon IPC Protocol (`0x0019`..`0x001C`, `0x8008`..`0x800B`)**: Added `WAYWAL_REQ_WORKSHOP_SYNC`, `WAYWAL_REQ_WORKSHOP_DOWNLOAD`, `WAYWAL_REQ_WWPKG_INSTALL`, and `WAYWAL_REQ_SCAN_COMPAT` with zero-copy DMA-BUF output export (`WAYWAL_REQ_EXPORT_OUTPUT_DMABUF`) prioritizing active video engines.

### Fixed

- **VA-API H.264 B-Frame Reorder Buffer & VUI Parsing (`vaapi_h264.c`, `video_engine.c`)**: Fixed `constraint_set0_flag` vs `constraint_set1_flag` bit extraction, parsed H.264 SPS VUI `bitstream_restriction_flag` / `max_num_reorder_frames`, expanded `WAYWAL_VIDEO_REORDER_CAP` to `17` frames, prevented premature B-frame reorder buffer draining acrossPOC gaps (`poc <= ve->next_present_poc`), eliminated loop-boundary decode stalls (`looped_this_tick`), and fixed VPP round-robin surface selection on NVIDIA.
- **VBlank PLL Phase-Locking & Frame Pacing (`video_engine.c`, `presentation.c`)**: Sampled `CLOCK_MONOTONIC` after synchronous GPU/VPP blits, aligned PLL vblank predictions past the current frame boundary to prevent `8 ms` / `23 ms` frame-repeat oscillations, replaced superseded `wp_presentation_feedback` requests on back-to-back commits, and cached `/proc/self/smaps_rollup` reads in `ipc_server.c` (`500 ms` TTL) so `wwal-studio` telemetry polling never stalls the render loop.
- **Hyprland Fullscreen Layer-Surface Recovery (`layer_shell.c`, `wayland_core.c`, `main.c`)**: Automatically recreates and remaps `zwlr_layer_surface_v1` background surfaces that were initially mapped behind a fullscreen window (`out->pres_sync.pll.frame_seq == 0`) as soon as fullscreen exits or the active workspace changes, working around Hyprland's `CLayerSurface::onMap` `m_alpha = 0` skip.

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

[Unreleased]: https://github.com/Execorn/wwal/compare/v3.0.0...HEAD
[3.0.0]: https://github.com/Execorn/wwal/compare/v1.2.0...v3.0.0
[1.2.0]: https://github.com/Execorn/wwal/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/Execorn/wwal/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/Execorn/wwal/releases/tag/v1.0.0
