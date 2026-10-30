wwald(1) "wwal 1.2.0" "WayWal Manual"

# NAME

wwald - zero-copy Wayland wallpaper, VA-API video, Shadertoy, and Material You daemon

# SYNOPSIS

*wwald* [OPTIONS]

# DESCRIPTION

*wwald* is the background rendering daemon for *wwal*(1). Written in ISO C23 with a stripped binary size under 1.4 MB, *wwald* replaces separate static wallpaper daemons, video wallpaper players, GLSL shader backgrounds, Steam Wallpaper Engine runners, and color scheme / template generators within a single sandboxed process.

When invoked via a symlink named *swww-daemon* or *awww-daemon*, *wwald* automatically enables full *swww* / *awww* v0.12.1 daemon flag and socket namespace compatibility.

# ARCHITECTURE

## Event Loop & Systemd Socket Activation

*wwald* drives all Wayland protocol events, IPC client connections, *timerfd* frame clocks, *signalfd*, and compositor occlusion sockets through a Linux *io_uring*(7) event loop using multishot poll (*IORING_POLL_ADD_MULTI*), with an automatic *epoll*(7) fallback on older kernels.

When started via *systemd*(1) socket activation (*wwald.socket*), *wwald* inherits the listening socket from *SD_LISTEN_FDS* (*LISTEN_FDS* / *LISTEN_PID*, file descriptor 3) at *$XDG_RUNTIME_DIR/wwald.sock* and skips *unlink*(2) on shutdown so the socket unit remains reusable across restarts. When started standalone, it binds *$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY-wwald.<namespace>.sock* (or *$XDG_RUNTIME_DIR/wwald.sock*).

## Zero-Copy DMA-BUF & HiDPI Rendering Pipeline

*wwald* negotiates hardware scanout formats and DRM format modifiers directly with the Wayland compositor via *zwp_linux_dmabuf_feedback_v1*:
- Matches the compositor's exact primary DRM device (*dev_t*) and per-surface scanout tranches across single-GPU and hybrid iGPU+dGPU systems.
- Allocates GBM buffer objects using optimal vendor-tiled or compressed modifiers (NVIDIA Block-Linear, AMD DCC, Intel CCS, ARM AFBC) when GPU compute is active, or *DRM_FORMAT_MOD_LINEAR* for CPU SIMD fallback (*AVX-512*, *AVX2*, *ARM NEON*).
- Supports true 10-bit HDR10 wide-gamut surfaces (*DRM_FORMAT_XRGB2101010* / *DRM_FORMAT_ARGB2101010*), zero-RAM solid colors via *wp_single_pixel_buffer_v1*, and explicit GPU fence synchronization via *wp_linux_drm_syncobj_v1*.
- Separates physical pixel dimensions from logical surface dimensions using *wp_fractional_scale_v1* and *wp_viewporter*, guaranteeing crisp 1:1 physical pixel rendering on fractionally scaled 4K/5K displays.

## Zero-FFmpeg Hardware Video Engine

*wwald* contains a native, zero-allocation MP4 (*isobmff*) and WebM/Matroska (*EBML*) demuxer coupled directly to *libva* (*VA-API*):
- Parses H.264 (SPS/PPS/VUI/Slice/DPB), HEVC (VPS/SPS/PPS/RPS), VP9, and AV1 bitstreams in-process without linking *libavformat* or *libavcodec*.
- Evicts already-played file pages from the Linux page cache via *madvise*(2) *MADV_DONTNEED*.
- On AMD (*radeonsi*) and Intel (*iHD*) GPUs, exports decoded *DRM_FORMAT_NV12* and *DRM_FORMAT_P010* surfaces via *VA_EXPORT_SURFACE_COMPOSED_LAYERS* for direct zero-VPP KMS hardware overlay plane scanout.
- On NVIDIA GPUs (*libva-nvidia-driver*), exports separate layers into a compact 3-surface VA-API Video Post-Processing (VPP) *BGRX* ring.
- Synchronizes frame presentation to display VBlank using a Phase-Locked Loop (PLL) fed by *wp_presentation_feedback* timestamps.

## Native Wallpaper Engine, Multi-Pass Shadertoy & Reactive Audio/MPRIS2

*wwald* includes a native Wallpaper Engine (*431960*) Workshop runtime, a multi-pass Shadertoy pipeline, 10 built-in procedural shader presets, and reactive desktop bridges:
- Maps *PKGV0001..PKGV0022* *scene.pkg* archives directly via *mmap*(2) without temporary disk extraction, decoding *TEXV0005* (*BC1*, *BC2*, *BC3*, *BC7*, and *LZ4*) textures, 2D Puppet Warp (*.mdl*) skeletal meshes, 32-byte-aligned AVX2 Structure-of-Arrays particle systems, `"sound"` ambient audio layers, `"text"` clock/label layers, and sandboxed *SceneScript* logic.
- Executes multi-pass Shadertoy fragment shaders (*Buffer A*..*Buffer D* *GL_RGBA16F* ping-pong passes, *iChannel0*..*iChannel3*, *#pragma waywal*, hardware derivatives), 10 built-in procedural shader presets, and 2.5D Depth-Map Parallax Occlusion Mapping driven by cursor and workspace-slide spring physics.
- Streams lock-free PipeWire 2048-point AVX2 64-bin Bark-scale FFT spectrum data, automatically ducks wallpaper audio when external media plays, and listens to D-Bus *MPRIS2* signals with zero idle polling to expose *u_album_art*, *u_media_playing*, and live album art Material Design 3 palettes.

## Zero-Watt Smart Occlusion & Battery Governor

*wwald* maintains a per-output pause bitmask (*waywal_pause_reason_t*):
- *WAYWAL_PAUSE_DPMS_OR_UNMAPPED* (*1 << 0*): Output is unmapped or in DPMS sleep.
- *WAYWAL_PAUSE_TOPLEVEL_FULLSCREEN* (*1 << 1*): Fullscreen window covers the output (tracked via *zwlr_foreign_toplevel_manager_v1*, *ext_foreign_toplevel_list_v1*, or Hyprland/Sway IPC sockets).
- *WAYWAL_PAUSE_TOPLEVEL_MAXIMIZED* (*1 << 2*): Maximized window covers the output.
- *WAYWAL_PAUSE_WORKSPACE_OCCLUDED* (*1 << 3*): Active workspace on the output is fully tiled/opaque.
- *WAYWAL_PAUSE_SESSION_IDLE* (*1 << 4*): Session idle notification received via *ext_idle_notifier_v1*.
- *WAYWAL_PAUSE_BATTERY_SAVER* (*1 << 5*): Laptop battery discharging or power-saver profile active (monitored via *inotify* on */sys/class/power_supply/*).
- *WAYWAL_PAUSE_USER_MANUAL* (*1 << 6*): Paused explicitly via *wwal pause*.

Whenever all active outputs have a non-zero *pause_mask*, *wwald* disarms its frame *timerfd*, dropping CPU, GPU, and NVDEC utilization to strictly *0.00%* (*0 wakeups/sec*).

## Landlock & Seccomp-BPF Sandboxing

At startup, after initializing DRM/GBM and Wayland connections, *wwald* locks itself down using:
- *PR_SET_NO_NEW_PRIVS* and *PR_SET_DUMPABLE = 0*.
- Linux *Landlock* (*ABI v1..v4*): Restricts filesystem visibility to read-only system libraries, device nodes, *$XDG_RUNTIME_DIR*, and configured wallpaper/cache/template directories, blocking access to sensitive user data (*~/.ssh*, *~/.gnupg*, etc.).
- *Seccomp-BPF* syscall allowlist: Blocks network socket creation (*AF_INET*, *AF_INET6*), *ptrace*, and privilege escalation syscalls.

# OPTIONS

*-n, --namespace* <NAME>
	Set the IPC socket and Wayland layer-shell namespace (default: *default*, or *$SWWW_NAMESPACE* / *$AWWW_NAMESPACE*).

*-d, --daemon*
	Fork into the background and signal readiness once the IPC socket and Wayland surfaces are initialized.

*--no-daemon*
	Stay in the foreground (*swww-daemon* / *awww-daemon* compatibility).

*-f, --format* <xrgb|xbgr|rgb|bgr>
	Pixel channel order hint (*swww-daemon* / *awww-daemon* compatibility).

*-q, --quiet*
	Suppress informational log messages.

*--layer* <background|bottom>
	Set default Wayland layer-shell placement (default: *background*).

*--backdrop* <none|auto-blur|PATH>
	Configure secondary overview backdrop layer surface mode (default: *none*).

*--battery-policy* <ignore|reduce-fps|pause-video>
	Set laptop battery power governance policy (default: *reduce-fps*).

*--volume* <0.0..1.0>
	Set default wallpaper audio playback volume (default: *0.0*).

*--mute-on-audio*, *--no-mute-on-audio*
	Enable or disable automatic PipeWire volume ducking when external audio plays.

*--theme-from-media*, *--no-theme-from-media*
	Enable or disable dynamic Material Design 3 palette extraction from D-Bus MPRIS2 album art.

*-v, --verbose*
	Enable verbose logging.

*-V, --version*
	Print version information and exit.

*-h, --help*
	Display help message and exit.

# ENVIRONMENT

*WWAL_CONFIG_PATH*
	Explicit path to *config.toml* (defaults to *$XDG_CONFIG_HOME/wwal/config.toml*).

*WWAL_WORKSHOP_DIR*
	Override Steam Workshop (*431960*) directory path.

*WWAL_WE_ASSETS_DIR*
	Override Wallpaper Engine default assets directory path.

*WWAL_SHADER_PRESET_DIR*, *WWAL_SHADER_DIR*
	Override directory searched for shader presets (*.glsl*, *.frag*) before falling back to embedded procedural shaders.

*WWAL_CACHE_DIR*
	Override cache directory path (defaults to *$XDG_CACHE_HOME/wwal* and *$XDG_CACHE_HOME/wal*).

*WWAL_STATE_DIR*
	Override persistent state directory path (defaults to *$XDG_STATE_HOME/wwal*).

*SWWW_NAMESPACE*, *AWWW_NAMESPACE*
	Fallback socket namespace when *-n* is not specified.

# SIGNALS

*SIGTERM*, *SIGINT*
	Gracefully tear down Wayland surfaces, release GBM/VA-API buffers, and exit.

*SIGHUP*
	Reload *~/.config/wwal/config.toml* and re-evaluate active solar/schedule rules and templates.

# SEE ALSO

*wwal*(1), *wwal-config*(5)
