wwal(1) "wwal 1.2.0" "WayWal Manual"

# NAME

wwal - zero-copy Wayland wallpaper, VA-API video, Shadertoy, and Material You client

# SYNOPSIS

*wwal* [GLOBAL OPTIONS] <COMMAND> [ARGUMENTS...]

# DESCRIPTION

*wwal* is the command-line controller for *wwald*(1), a C23 Wayland wallpaper engine that unifies static DMA-BUF wallpapers, 18 GLES 3.1 compute shader transitions, zero-FFmpeg VA-API hardware video playback, live Shadertoy fragment/compute shaders with PipeWire 64-bin Bark FFT audio reactivity, Steam Wallpaper Engine (*431960*) Workshop scenes, and sub-millisecond dual-mode OKLab/CAM16/HCT Material Design 3 color extraction and template rendering.

When invoked via a symlink named *swww*, *awww*, *swww-daemon*, or *awww-daemon*, *wwal* automatically enables full command-line, environment variable, and cache-file compatibility with *swww*/*awww* v0.12.1 scripts, GUI pickers, and launchers.

# GLOBAL OPTIONS

*-j, --json*
	Format *query*, *status*, and *stats* output as structured JSON.

*-n, --namespace* <NAME>
	Connect to the daemon socket namespace _NAME_ (default: *default*). Also sets the Wayland layer-shell namespace unless overridden.

*-a, --all*
	Broadcast the command to all active *wwald* daemon namespaces discovered in *$XDG_RUNTIME_DIR* (optionally filtered by *WWAL_NAMESPACE_PREFIX*).

*-q, --quiet*
	Suppress non-error informational output.

*-d, --daemon*
	Fork *wwald* into the background when invoked with *init*, *daemon*, *swww-daemon*, or *awww-daemon*.

*--no-daemon*
	Keep *wwald* in the foreground (*swww-daemon* / *awww-daemon* compatibility).

*-f, --format* <xrgb|xbgr|rgb|bgr>
	Pixel channel order hint when launching *wwald* (*swww-daemon* / *awww-daemon* compatibility).

*-v, --verbose*
	Enable verbose diagnostic output.

*-V, --version*
	Print version information and exit.

*-h, --help*
	Display usage information and exit.

# COMMANDS

*img* <PATH|-> [OPTIONS]
	Universal wallpaper command. Automatically detects and dispatches static or animated images (PNG, JPEG, WebP, AVIF, HEIC/HEIF, JPEG-XL, SVG, BMP, GIF, APNG, QOI, PPM/PGM/PNM, TGA), hardware videos (MP4, WebM, MKV, MOV, AVI, M4V), live shaders (*.frag*, *.comp*, *.glsl*, *.shader*), Steam Workshop numeric IDs, Workshop folders, or synced preview symlinks, GNOME dynamic timed wallpaper XML schedules (*.xml*), slideshow directories, and solid hex colors. Pass *-* to read image bytes from standard input. File descriptors are passed to *wwald*(1) via *SCM_RIGHTS* for zero-copy decoding inside the sandbox.

*pick* [DIR] [OPTIONS]
	Launch a unified icon-grid wallpaper picker across static images, videos, shaders, and Steam Workshop scenes. Automatically generates cached *128x128* / *160x90* thumbnails in *~/.cache/wwal/thumbs/* and pipes icon-annotated entries into *rofi*, *fuzzel*, *walker*, or *dmenu*. Use *--menu <auto|rofi|fuzzel|walker|dmenu|stdout|json>* to select the output protocol and *--dir <DIR>* to add search directories.

*video* <PATH> [OPTIONS]
	Play a hardware-accelerated video wallpaper (MP4, WebM, MKV, MOV, AVI, M4V, GIF) using the zero-FFmpeg MP4/WebM demuxer and VA-API hardware decode pipeline (H.264, HEVC, VP9, AV1).

*shader* <PATH|list|preset> [NAME] [OPTIONS]
	Run a live procedural GLSL compute shader (*.comp*), multi-pass Shadertoy fragment shader (*.frag*) with automatic *inotify*(7) hot-reloading, or one of 10 built-in procedural shader presets installed in *shaders/presets/\*.frag*. Use *wwal shader list* (or *--list*) to list built-in presets (*aurora*, *cyber_grid*, *fluid_ink*, *synthwave_sun*, *starfield*, *rain_glass*, *audio_bars*, *audio_ring*, *crt_retro*, *plasma_orb*, plus aliases *lava_lamp*, *northern_lights*, *outrun*, *crt*) and *wwal shader preset <NAME>* (or *--preset <NAME>*) to launch one directly.

*workshop* <list|info|apply|sync-previews> [ID|PATH] [OPTIONS]
	Discover, inspect, render, or export preview symlinks for native Steam Workshop (*431960* Wallpaper Engine) scenes, videos, web wallpapers, `.pkg` archives, `.tex` textures, `.mdl` Puppet Warp skeletal meshes, AVX2 particle systems, `"sound"` ambient audio layers, `"text"` clock/label layers, and `SceneScript` logic. Use *--prop key=val* to override user properties, or *wwal workshop sync-previews [DIR]* to populate human-readable preview symlinks in *~/.cache/wwal/workshop-previews/* for external GUI pickers (*waypaper*, *azote*).

*slideshow* <DIR|FILE.xml|ACTION> [OPTIONS]
	Start a zero-idle-CPU *timerfd* mixed-media wallpaper slideshow from _DIR_ or a GNOME timed dynamic wallpaper schedule (_FILE.xml_), or control an active slideshow using one of the actions: *start*, *stop*, *pause*, *resume*, *toggle*, *next*, *prev*, *previous*.

*next* [-o <OUTPUT>]
	Advance the active slideshow on the target output(s) to the next wallpaper.

*prev*, *previous* [-o <OUTPUT>]
	Return the active slideshow on the target output(s) to the previous wallpaper.

*color* <HEX> [OPTIONS]
	Set the target output(s) to a solid RGB/RGBA hex color (for example, *#1e1e2e* or *1e1e2eff*) using *wp_single_pixel_buffer_v1* when supported by the compositor.

*clear* [HEX] [OPTIONS]
	Alias for *color* (defaults to *000000* if no color is given) for *swww*/*awww* compatibility.

*theme* [PATH] [-i|--img|--image <PATH>] [OPTIONS]
	Extract simultaneous dark and light OKLab/CAM16/HCT Material Design 3 palettes from _PATH_ (or via *-i, --img, --image <PATH>*, or the currently active wallpaper if omitted) in under 0.5 ms, render all configured *[template."<name>"]* files (supporting Matugen, Wallust, and Pywal syntax and 7 pipe filters), and write *~/.local/state/caelestia/scheme.json*, *~/.cache/wwal/colors.{json,css,hyprland.conf}*, *~/.cache/wal/{colors.json,colors,colors.sh,colors.css,sequences}*, and terminal OSC 4/10/11 sequences.

*audio* [OPTIONS]
	Configure live wallpaper audio playback volume (*--volume 0.0..1.0*) and external application auto-ducking (*--mute-on-audio* / *--no-mute-on-audio*).

*set-uniform*, *uniform* <NAME> <VALUES...>
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
	Remove cached hardware-transcoded animations, thumbnails, and palette artifacts from *$XDG_CACHE_HOME/wwal*.

*init*, *daemon*
	Launch or initialize the *wwald*(1) daemon (*swww*/*awww* compatibility alias).

*ping*
	Check whether *wwald*(1) is running and responsive over the IPC socket.

*kill*
	Gracefully terminate the running *wwald*(1) daemon.

*version*
	Print version information and exit.

*help*
	Show command-line usage summary.

# IMAGE & TRANSITION OPTIONS

*-t, --transition-type* <TYPE>
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
	- *left*, *right*, *top*, *bottom*: Directional slide transition aliases (*swww*/*awww* compatibility).
	- *center*, *any*: Centered or arbitrary radial iris grow aliases (*swww*/*awww* compatibility).
	- *random*: Randomly choose one of the 18 built-in shader transitions.

*--transition-duration* <SECONDS>
	Duration of the transition in seconds as a floating-point number such as *0.5* or *1.5* (default: *1.2*).

*--transition-fps* <FPS>
	Target transition frame rate (default: *60*; automatically locks to display VBlank via *wp_presentation*).

*--transition-bezier* <X1,Y1,X2,Y2>
	Cubic Bezier control points for transition progress easing (default: *0.25,0.1,0.25,1.0*; *swww* default: *0.54,0,0.34,0.99*).

*--transition-angle* <DEGREES>
	Direction angle in degrees for *wipe*, *wave*, and *slide* transitions (default: *0*).

*--transition-wave* <FREQ,AMP>
	Frequency and amplitude for the *wave* transition (default: *20,0.05*).

*--transition-pos* <POS>
	Anchor position for *grow*, *outer*, *ripple*, and *swirl* transitions. Accepts normalized coordinates *X,Y* in *0.0..1.0*, percentage coordinates (*50%,50%*), pixel offsets, or semantic aliases: *center*, *top*, *bottom*, *left*, *right*, *top-left* (*tl*, *topleft*), *top-right* (*tr*, *topright*), *bottom-left* (*bl*, *bottomleft*), *bottom-right* (*br*, *bottomright*), *cursor*, *mouse*.

*--invert-y*
	Invert the Y coordinate of *--transition-pos* (*swww*/*awww* compatibility).

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
	- *fill*, *crop*, *cover*: Scale to fill the entire output, cropping excess edges anchored at *--crop-gravity* (default).
	- *fit*, *fit-border-color*, *contain*: Scale to fit inside the output, padding letterbox/pillarbox bars with *--fill-color*.
	- *stretch*: Stretch non-uniformly to output dimensions.
	- *center*, *no*: Center at 1:1 pixel resolution, padding surrounding space with *--fill-color*.
	- *tile*: Repeat image in a tiled grid across the output.
	- *span*: Span a single panoramic canvas across the combined logical layout of all active monitors.

*--no-resize*
	Center image at 1:1 pixel resolution without scaling (alias for *--resize no*).

*--crop-gravity* <GRAVITY>
	Crop anchor when *--scaling-mode fill* (*crop*) is active. Accepts *center* (*c*), *top* (*north*, *n*, *t*), *bottom* (*south*, *s*, *b*), *left* (*west*, *w*, *l*), *right* (*east*, *e*, *r*), *top-left* (*tl*, *nw*, *northwest*), *top-right* (*tr*, *ne*, *northeast*), *bottom-left* (*bl*, *sw*, *southwest*), *bottom-right* (*br*, *se*, *southeast*), or normalized/percentage coordinates (*0.5,0.2* or *50%,20%*).

*--depth-map* <FILE>
	Enable interactive 3D Parallax Occlusion Mapping (POM) on a 2D image using the grayscale depth map at _FILE_, driven by smoothed cursor tracking and workspace-slide inertia.

*--depth-strength* <FLOAT>
	Depth parallax displacement multiplier when *--depth-map* is active (default: *1.0*).

*--fill-color* <HEX>
	Background padding color in 6- or 8-digit hex for *fit* and *center* modes (default: *000000*).

*-f, --filter* <Nearest|Bilinear|CatmullRom|Mitchell|Lanczos3>
	Sampling filter hint for image scaling (*swww*/*awww* compatibility).

*--no-cache*
	Bypass cached transition/palette artifacts for this invocation.

*--clear-cache*
	Purge cached transcodes and thumbnails before applying the new wallpaper.

*--10bit*
	Request 10-bit wide-gamut scanout (*DRM_FORMAT_XRGB2101010* / *DRM_FORMAT_ARGB2101010*) and preserve 16-bit PNG/AVIF/JXL channel precision when supported by the compositor.

*--backdrop* <none|auto-blur|FILE>
	Configure a secondary layer-shell surface with namespace *waywal-backdrop* for compositors with overview backdrops such as Niri (*place-within-backdrop true*). *auto-blur* renders a 2-pass Dual-Kawase Gaussian blur of the active wallpaper on the GPU at zero extra image-decode cost.

*--layer* <background|bottom>
	Wayland layer-shell layer placement (default: *background*).

*--theme, --no-theme*
	Enable or disable automatic Material Design 3 OKLab/CAM16/HCT color extraction and *caelestia*/*OSC*/template synchronization on wallpaper change.

*--theme-mode* <dark|light|auto>, *--dark*, *--light*
	Select dark, light, or auto Material Design 3 tonal polarity (default: *dark*).

*--auto-day-night, --no-auto-day-night*
	Enable or disable automatic dark/light theme polarity switching based on astronomical solar elevation (*[solar]* latitude/longitude) or local time of day.

*--latitude* <DEG>, *--longitude* <DEG>
	Observer latitude (*-90.0..90.0*) and longitude (*-180.0..180.0*) in decimal degrees for astronomical NOAA solar elevation calculations.

*--source-color-index* <0..3>
	Select which of the 4 ranked K-Means++ seed clusters to use as the primary Material Design 3 seed (*0* = dominant, *1..3* = secondary/tertiary/quaternary accents).

*--seed-color* <HEX>
	Override image color quantization with an explicit hex seed color (for example, *#89b4fa*).

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

*-d, --interval, --duration* <DURATION>
	Interval between slideshow transitions in seconds or human-readable units such as *30m*, *1h*, or *10m 30s* (default: *300*).

*-s, --shuffle, --random*
	Randomize slideshow traversal order using Fisher-Yates shuffling.

*--sort, --sorting* <ascending|descending|name|mtime|random>
	Sort order for slideshow directory traversal (default: *ascending*; *name* is an alias for *ascending*).

*--recursive, --no-recursive*
	Enable or disable recursive subdirectory traversal when scanning a slideshow directory (default: *--recursive*).

# ENVIRONMENT

*WAYLAND_DISPLAY*
	Wayland compositor socket name.

*XDG_RUNTIME_DIR*
	Directory containing the *wwald*(1) IPC socket (*$XDG_RUNTIME_DIR/wwald.sock* or *$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY-wwald.<namespace>.sock*).

*XDG_CONFIG_HOME*, *WWAL_CONFIG_PATH*
	Location of *wwal/config.toml* (defaults to *~/.config/wwal/config.toml*), or explicit config file path override via *WWAL_CONFIG_PATH*.

*XDG_CACHE_HOME*, *WWAL_CACHE_DIR*
	Location of *wwal/* generated color schemes, thumbnails, and cached transcodes (defaults to *~/.cache*).

*XDG_STATE_HOME*, *WWAL_STATE_DIR*
	Location of *wwal/state.bin* persistent output state (defaults to *~/.local/state*).

*WWAL_WORKSHOP_DIR*, *WWAL_WE_ASSETS_DIR*
	Override path for Steam Workshop (*431960*) items and Wallpaper Engine default assets.

*WWAL_SHADER_PRESET_DIR*, *WWAL_SHADER_DIR*
	Override directory path for built-in procedural shader preset files (*.frag*).

*WWAL_NAMESPACE_PREFIX*
	Optional namespace prefix filter when broadcasting commands with *-a, --all*.

*SWWW_NAMESPACE*, *AWWW_NAMESPACE*, *SWWW_TRANSITION*, *AWWW_TRANSITION*, *SWWW_TRANSITION_FPS*, *AWWW_TRANSITION_FPS*, *SWWW_TRANSITION_STEP*, *AWWW_TRANSITION_STEP*, *SWWW_TRANSITION_DURATION*, *AWWW_TRANSITION_DURATION*, *SWWW_TRANSITION_ANGLE*, *AWWW_TRANSITION_ANGLE*, *SWWW_TRANSITION_POS*, *AWWW_TRANSITION_POS*, *SWWW_TRANSITION_BEZIER*, *AWWW_TRANSITION_BEZIER*, *SWWW_TRANSITION_WAVE*, *AWWW_TRANSITION_WAVE*, *SWWW_INVERT_Y*, *AWWW_INVERT_Y*, *SWWW_TRANSITION_INVERT_Y*, *AWWW_TRANSITION_INVERT_Y*, *SWWW_CROP_GRAVITY*, *AWWW_CROP_GRAVITY*, *SWWW_FILTER*, *AWWW_FILTER*, *SWWW_RESIZE*, *AWWW_RESIZE*, *SWWW_FILL_COLOR*, *AWWW_FILL_COLOR*, *SWWW_OUTPUTS*, *AWWW_OUTPUTS*
	Drop-in *swww* / *awww* v0.12.1 environment variable overrides.

# FILES

*~/.config/wwal/config.toml*
	Declarative daemon, theme, template, solar/schedule, transition, output, and workspace configuration. See *wwal-config*(5).

*~/.local/state/caelestia/scheme.json*
	Material Design 3 tonal palette output for Quickshell (*caelestia-shell*).

*~/.cache/wwal/colors.json*, *~/.cache/wwal/colors.css*, *~/.cache/wwal/colors-hyprland.conf*
	Exported OKLab/HCT palette files for Waybar, GTK, and Hyprland.

*~/.cache/wal/colors.json*, *~/.cache/wal/colors*, *~/.cache/wal/colors.sh*, *~/.cache/wal/colors.css*, *~/.cache/wal/sequences*
	Pywal and Pywalfox compatibility palette files.

*~/.cache/wwal/current_wallpaper*, *~/.cache/wwal/current_wallpaper.png*
	Symlink to the active wallpaper and pre-rendered lockscreen PNG frame (*<= 1280x720*) for *hyprlock* and *swaylock*.

*~/.cache/wwal/thumbs/*, *~/.cache/wwal/workshop-previews/*
	Cached picker thumbnails and human-readable Steam Workshop preview symlinks.

*~/.cache/swww/<output>*, *~/.cache/awww/<output>*
	Drop-in *swww*/*awww* per-output state cache files.

# EXAMPLES

Set a wallpaper with a 144 FPS circular iris transition anchored at the cursor and extract a Material You palette:

```
wwal img ~/Pictures/cyberpunk.avif --transition-type grow --transition-pos cursor --transition-fps 144 --theme
```

Launch the unified icon-grid wallpaper picker (Rofi / Fuzzel / Walker):

```
wwal pick
```

Run a built-in procedural shader preset with PipeWire audio reactivity:

```
wwal shader preset audio_bars --audio
```

Sync Steam Workshop preview symlinks for *waypaper* and apply a Workshop scene:

```
wwal workshop sync-previews
wwal workshop apply 2841902341
```

# SEE ALSO

*wwald*(1), *wwal-config*(5)
