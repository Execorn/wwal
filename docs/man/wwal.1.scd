wwal(1) "wwal 1.0.0" "WayWal Manual"

# NAME

wwal - zero-copy Wayland wallpaper, VA-API video, Shadertoy, and Material You client

# SYNOPSIS

*wwal* [GLOBAL OPTIONS] <COMMAND> [ARGUMENTS...]

# DESCRIPTION

*wwal* is the command-line controller for *wwald*(1), a C23 Wayland wallpaper engine that unifies static DMA-BUF wallpapers, 18 GLES 3.1 compute shader transitions, zero-FFmpeg VA-API hardware video playback, live Shadertoy fragment/compute shaders with PipeWire 64-bin Bark FFT audio reactivity, and sub-millisecond OKLab/CAM16/HCT Material Design 3 color extraction.

When invoked via a symlink named *swww* or *awww*, *wwal* automatically enables full command-line compatibility with *swww*/*awww* scripts, pickers, and flags.

# GLOBAL OPTIONS

*-j, --json*
	Format *query*, *status*, and *stats* output as structured JSON.

*-n, --namespace* <NAME>
	Connect to the daemon socket namespace _NAME_ (default: *default*). Also sets the Wayland layer-shell namespace unless overridden.

*-v, --verbose*
	Enable verbose diagnostic output.

*-h, --help*
	Display usage information and exit.

# COMMANDS

*img* <PATH|-> [OPTIONS]
	Decode and display a static or animated image (PNG, JPEG, WebP, AVIF, HEIC, JPEG-XL, SVG, BMP, GIF). Pass *-* to read image bytes from standard input. File descriptors are passed to *wwald*(1) via *SCM_RIGHTS* for zero-copy decoding inside the sandbox.

*video* <PATH> [OPTIONS]
	Play a hardware-accelerated video wallpaper (MP4, WebM, MKV, GIF) using the zero-FFmpeg MP4/WebM demuxer and VA-API hardware decode pipeline (H.264, HEVC, VP9, AV1).

*shader* <PATH> [OPTIONS]
	Run a live procedural GLSL compute shader (*.comp*) or multi-pass Shadertoy fragment shader (*.frag*) on the background layer with automatic *inotify*(7) hot-reloading.

*workshop* <list|info|apply> [ID|PATH] [OPTIONS]
	Discover, inspect, or render native Steam Workshop (*431960* Wallpaper Engine) scenes, videos, web wallpapers, `.pkg` archives, `.tex` textures, `.mdl` Puppet Warp skeletal meshes, AVX2 particle systems, and `SceneScript` logic. Use *--prop key=val* to override user properties.

*slideshow* <DIR|ACTION> [OPTIONS]
	Start a zero-idle-CPU *timerfd* wallpaper slideshow from _DIR_, or control an active slideshow using one of the actions: *start*, *stop*, *pause*, *resume*, *toggle*, *next*, *prev*.

*color* <HEX> [OPTIONS]
	Set the target output(s) to a solid RGB/RGBA hex color (for example, *#1e1e2e* or *1e1e2eff*) using *wp_single_pixel_buffer_v1* when supported by the compositor.

*clear* [HEX] [OPTIONS]
	Alias for *color* (defaults to *000000* if no color is given) for *swww*/*awww* compatibility.

*theme* <PATH> [OPTIONS]
	Extract an OKLab/CAM16/HCT Material Design 3 palette from _PATH_ in under 0.4 ms and write *~/.local/state/caelestia/scheme.json*, *~/.cache/wwal/colors.json*, *~/.cache/wwal/colors.css*, *~/.cache/wwal/colors-hyprland.conf*, and terminal OSC 4/10/11 sequences.

*audio* [OPTIONS]
	Configure live wallpaper audio playback volume (*--volume 0.0..1.0*) and external application auto-ducking (*--mute-on-audio* / *--no-mute-on-audio*).

*set-uniform* <NAME> <VALUES...>
	Mutate a live shader uniform or Workshop user property (1 to 4 float values) without reloading the active scene.

*status* [--json]
	Inspect connected outputs, active wallpaper/video/shader/Workshop paths, D-Bus MPRIS2 media metadata, and daemon telemetry.

*subscribe*
	Stream newline-delimited JSON events whenever wallpapers, themes, or media states change.

*pause*
	Pause active video, animation, live shader, or Workshop scene playback.

*resume*, *unpause*
	Resume paused video, animation, live shader, or Workshop scene playback.

*toggle*
	Toggle video, animation, live shader, or Workshop scene playback between paused and playing states.

*query* [--json]
	List all connected Wayland outputs, physical and logical dimensions, scale factors, refresh rates, and active wallpaper state.

*stats* [--json]
	Query real-time daemon telemetry (opcode *0x000C*), including daemon RSS/PSS memory footprint, active VRAM surface pool count, negotiated DRM FourCC format and modifier (Linear, NVIDIA Block-Linear, ARM AFBC, AMD DCC, Intel CCS), VBlank PLL period/phase jitter estimates, and per-output Zero-Watt *pause_mask* reasons.

*restore*
	Restore the last persisted per-output wallpaper, video, shader, Workshop scene, or color state from *$XDG_STATE_HOME/wwal/state.bin*.

*clear-cache*
	Remove cached hardware-transcoded animations and palette artifacts from *$XDG_CACHE_HOME/wwal*.

*ping*
	Check whether *wwald*(1) is running and responsive over the IPC socket.

*kill*
	Gracefully terminate the running *wwald*(1) daemon.

*help*
	Show command-line usage summary.

# IMAGE & TRANSITION OPTIONS

*--transition-type* <TYPE>
	Select transition effect (default: *fade*). Supported GPU GLES 3.1 compute shader and AVX-512/AVX2/NEON SIMD transition types:
	- *none*: Instant buffer swap with zero transition frames.
	- *simple*: Linear alpha cross-dissolve.
	- *fade*: Cubic-bezier eased alpha crossfade.
	- *wipe*: Directional linear edge wipe controlled by *--transition-angle*.
	- *grow*: Expanding circular iris anchored at *--transition-pos*.
	- *outer*: Contracting circular iris toward *--transition-pos*.
	- *wave*: Sinusoidal wave front controlled by *--transition-wave* and *--transition-angle*.
	- *noise*: Perlin/simplex procedural noise dissolve.
	- *crosszoom*: Radial motion-blur zoom crossfade.
	- *slide*: Directional viewport push transition.
	- *glitch*: RGB chromatic aberration and block-shift digital glitch.
	- *burn*: Procedural ember/fire edge burn.
	- *ripple*: Concentric water ripple refraction anchored at *--transition-pos*.
	- *pixelate*: Dynamic mosaic block quantization transition.
	- *doom*: Classic vertical column melt transition.
	- *swirl*: Spiral vortex distortion around *--transition-pos*.
	- *cube*: 3D perspective rotating cube face transition.
	- *luma*: Luminance threshold ramp transition.
	- *light_leak*: Warm filmic anamorphic flare transition.
	- *page_curl*: 3D cylindrical page curl with specular shading.
	- *custom*: User-supplied GLSL compute shader via *--transition-shader*.
	- *random*: Randomly choose one of the 18 built-in shader transitions.

*--transition-duration* <SECONDS>
	Duration of the transition in seconds (default: *1.0*).

*--transition-fps* <FPS>
	Target transition frame rate (default: *60*; automatically locks to display VBlank via *wp_presentation*).

*--transition-bezier* <X1,Y1,X2,Y2>
	Cubic Bezier control points for transition progress easing (default: *0.25,0.1,0.25,1.0*; *swww* default: *0.54,0,0.34,0.99*).

*--transition-angle* <DEGREES>
	Direction angle in degrees for *wipe*, *wave*, and *slide* transitions (default: *0*).

*--transition-wave* <FREQ,AMP>
	Frequency and amplitude for the *wave* transition (default: *20,0.05*).

*--transition-pos* <POS>
	Anchor position for *grow*, *outer*, *ripple*, and *swirl* transitions. Accepts normalized coordinates *X,Y* in *0.0..1.0*, or semantic aliases: *center*, *top*, *bottom*, *left*, *right*, *top-left*, *top-right*, *bottom-left*, *bottom-right*, *cursor*, *mouse*.

*--transition-step* <STEP>
	Per-frame progress step (*1..255*) for *swww*/*awww* compatibility.

*--transition-shader* <FILE>
	Path to a custom GLSL ES 3.10 compute shader (*.comp*) when *--transition-type custom* is selected.

*-o, --output, --outputs* <NAMES>
	Target specific Wayland output(s) by name (for example, *DP-1* or *DP-1,HDMI-A-1*). May be specified multiple times.

*--sync-mode* <simultaneous|staggered>
	Synchronize transitions across multiple monitors simultaneously or with a cascading delay (default: *simultaneous*).

*--stagger-delay* <MS>
	Cascading delay in milliseconds between outputs when *--sync-mode staggered* is active (default: *150*).

*--scaling-mode, --mode, --resize* <MODE>
	Aspect-ratio scaling mode:
	- *fill*, *crop*, *cover*: Scale to fill the entire output, cropping excess edges (default).
	- *fit*, *contain*: Scale to fit inside the output, padding letterbox/pillarbox bars with *--fill-color*.
	- *stretch*: Stretch non-uniformly to output dimensions.
	- *center*, *no*: Center at 1:1 pixel resolution, padding surrounding space with *--fill-color*.
	- *tile*: Repeat image in a tiled grid across the output.
	- *span*: Span a single panoramic canvas across the combined logical layout of all active monitors.

*--depth-map* <FILE>
	Enable interactive 3D Parallax Occlusion Mapping (POM) on a 2D image using the grayscale depth map at _FILE_, driven by smoothed cursor tracking and workspace-slide inertia.

*--depth-strength* <FLOAT>
	Depth parallax displacement multiplier when *--depth-map* is active (default: *1.0*).

*--fill-color* <HEX>
	Background padding color in 6- or 8-digit hex for *fit* and *center* modes (default: *000000*).

*--filter* <Nearest|Bilinear|CatmullRom|Mitchell|Lanczos3>
	Sampling filter hint for image scaling (*swww*/*awww* compatibility).

*--10bit*
	Request 10-bit wide-gamut scanout (*DRM_FORMAT_XRGB2101010* / *DRM_FORMAT_ARGB2101010*) and preserve 16-bit PNG/AVIF/JXL channel precision when supported by the compositor.

*--backdrop* <none|auto-blur|FILE>
	Configure a secondary layer-shell surface with namespace *waywal-backdrop* for compositors with overview backdrops such as Niri (*place-within-backdrop true*). *auto-blur* renders a 2-pass Dual-Kawase Gaussian blur of the active wallpaper on the GPU at zero extra image-decode cost.

*--layer* <background|bottom>
	Wayland layer-shell layer placement (default: *background*).

*--theme, --no-theme*
	Enable or disable automatic Material Design 3 OKLab/CAM16/HCT color extraction and *caelestia*/*OSC* synchronization on wallpaper change.

*--theme-from-media, --no-theme-from-media*
	Enable or disable dynamic Material Design 3 palette extraction from active D-Bus *MPRIS2* album art.

*--animate*
	Route animated GIF/WebP/APNG files through the hardware video/animation engine.

# VIDEO, SHADER & POWER OPTIONS

*-p, --prop* <KEY=VALUE>
	Override a Wallpaper Engine Workshop user property when applying a scene (repeatable).

*--loop* <COUNT>
	Number of playback loops for *video* (*0* for infinite loop, default: *0*).

*--speed* <MULTIPLIER>
	Playback speed multiplier for *video* and *shader* (default: *1.0*).

*--fps* <FPS>
	Target rendering frame rate for *shader* and *workshop* scenes (default: *60*).

*--audio*
	Enable lock-free PipeWire default sink monitor capture and 64-bin logarithmic Bark-scale FFT uniforms (*u_audio_bands[64]*, *u_audio_envelope*, and Shadertoy *iChannel0* 512x2 audio texture) for *shader* and *workshop*.

*--volume* <0.0..1.0>
	Set wallpaper audio playback volume for *video*, *shader*, *workshop*, and *audio* (default: *0.0*).

*--mute-on-audio, --no-mute-on-audio*
	Enable or disable automatic exponential volume ducking when another application plays audio via PipeWire.

*--battery-policy* <ignore|reduce-fps|pause-video>
	Configure the laptop battery governor policy when discharging or entering power-saver mode.

# SLIDESHOW OPTIONS

*-d, --interval* <SECONDS>
	Interval in seconds between slideshow transitions (default: *300*).

*-s, --shuffle, --random*
	Randomize slideshow traversal order using Fisher-Yates shuffling.

# ENVIRONMENT

*WAYLAND_DISPLAY*
	Wayland compositor socket name.

*XDG_RUNTIME_DIR*
	Directory containing the *wwald*(1) IPC socket (*$XDG_RUNTIME_DIR/wwald.sock* or *$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY-wwald.<namespace>.sock*).

*XDG_CONFIG_HOME*
	Location of *wwal/config.toml* (defaults to *~/.config*).

*XDG_CACHE_HOME*
	Location of *wwal/* generated color schemes and cached transcodes (defaults to *~/.cache*).

*XDG_STATE_HOME*
	Location of *wwal/state.bin* persistent output state (defaults to *~/.local/state*).

# FILES

*~/.config/wwal/config.toml*
	Declarative daemon, theme, transition, output, and workspace configuration. See *wwal-config*(5).

*~/.local/state/caelestia/scheme.json*
	Material Design 3 tonal palette output for Quickshell (*caelestia-shell*).

*~/.cache/wwal/colors.json*, *~/.cache/wwal/colors.css*, *~/.cache/wwal/colors-hyprland.conf*
	Exported OKLab/HCT palette files for Waybar, GTK, and Hyprland.

# EXAMPLES

Set a wallpaper with a 144 FPS circular iris transition anchored at the cursor and extract a Material You palette:

```
wwal img ~/Pictures/cyberpunk.avif --transition-type grow --transition-pos cursor --transition-fps 144 --theme
```

Set a wallpaper with a Niri Dual-Kawase blurred overview backdrop and custom cubic-bezier curve:

```
wwal img ~/Pictures/mountain.jxl --backdrop auto-blur --transition-bezier 0.22,1.0,0.36,1.0
```

Play a 4K 60 FPS video wallpaper with zero-copy VA-API decoding:

```
wwal video ~/Videos/loop.mp4 --output DP-1
```

Run an audio-reactive Shadertoy wallpaper fed by PipeWire FFT:

```
wwal shader ~/Shaders/nebula.frag --audio
```

Inspect live DMA-BUF modifiers, VRAM pool usage, and Zero-Watt occlusion state:

```
wwal stats
```

# SEE ALSO

*wwald*(1), *wwal-config*(5)
