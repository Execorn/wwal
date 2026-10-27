wwal-config(5) "wwal 1.1.0" "WayWal Manual"

# NAME

wwal-config - declarative TOML configuration file for wwald(1) and wwal(1)

# SYNOPSIS

*~/.config/wwal/config.toml*

# DESCRIPTION

*wwal* and *wwald*(1) read their declarative configuration from *$XDG_CONFIG_HOME/wwal/config.toml* (defaulting to *~/.config/wwal/config.toml*). The file uses standard TOML syntax and is parsed by *wwald*'s built-in zero-allocation recursive-descent parser at daemon startup and upon receiving *SIGHUP*.

Command-line flags passed to *wwal*(1) override values configured in *config.toml* for that invocation.

# SECTIONS

## [daemon]

Controls core *wwald*(1) lifecycle, layer-shell behavior, and Zero-Watt power governance.

*namespace* = "<string>"
	Default IPC and Wayland layer-shell namespace (default: *"default"*).

*layer* = "<background|bottom>"
	Wayland *zwlr_layer_shell_v1* layer used for wallpaper surfaces (default: *"background"*).

*backdrop* = "<none|auto-blur|path>"
	Secondary layer surface (*waywal-backdrop*) mode for compositors with overview backdrops such as Niri (default: *"none"*). Set to *"auto-blur"* to run the GPU 2-pass Dual-Kawase blur shader automatically.

*ten_bit*, *10bit* = <bool>
	Enable 10-bit wide-gamut scanout (*XRGB2101010* / *ARGB2101010*) when supported by the compositor (default: *false*).

*restore_on_start* = <bool>
	Automatically restore per-output state from *$XDG_STATE_HOME/wwal/state.bin* when *wwald* starts or when a monitor is hotplugged (default: *true*).

*auto_pause_fullscreen*, *pause_on_fullscreen* = <bool>
	Pause video and live shader rendering on an output when a fullscreen window covers it (default: *true*).

*auto_pause_maximized*, *pause_on_maximized* = <bool>
	Pause video and live shader rendering on an output when a maximized window covers it (default: *false*).

*pause_on_occluded_workspace* = <bool>
	Pause video and live shader rendering when Hyprland/Sway IPC reports the active workspace is fully tiled and opaque (default: *true*).

*pause_on_idle* = <bool>
	Pause animations when *ext_idle_notifier_v1* reports the user session is idle or locked (default: *true*).

*idle_timeout_sec* = <int>
	Idle timeout in seconds before *ext_idle_notifier_v1* pauses animations (default: *300*).

*battery_policy* = "<ignore|reduce-fps|pause-video>"
	Power governor behavior when running on laptop battery power (default: *"reduce-fps"*).

*workshop_dir* = "<path>"
	Custom Steam Workshop (*431960*) directory path override for *wwal workshop*.

*volume* = <float>
	Default wallpaper audio playback volume in *0.0..1.0* (default: *0.0*).

*mute_on_audio* = <bool>
	Automatically duck wallpaper audio volume when another application plays audio via PipeWire (default: *false*).

## [theme]

Controls the in-pipeline OKLab Wu quantizer + weighted K-Means++ + CAM16/HCT Material Design 3 palette generator.

*enabled*, *enable* = <bool>
	Automatically extract and export color palettes whenever a wallpaper changes (default: *true*).

*mode* = "<dark|light>"
	Target Material Design 3 tonal scheme polarity (default: *"dark"*).

*scheme_variant* = "<tonal_spot|vibrant|expressive|neutral|monochrome|fidelity|content|rainbow|fruit_salad>"
	CAM16/HCT chroma and hue rotation variant (default: *"tonal_spot"*).

*contrast* = <float>
	Contrast level between *-1.0* and *1.0* (default: *0.0*).

*caelestia_sync* = <bool>
	Atomically write *~/.local/state/caelestia/scheme.json* for Quickshell (*caelestia-shell*) live theme synchronization (default: *true*).

*tty_sequences*, *terminal_osc* = <bool>
	Broadcast non-blocking ANSI *OSC 4*, *OSC 10*, and *OSC 11* escape sequences to active */dev/pts/\** terminals (default: *true*).

*export_css* = <bool>
	Write *~/.cache/wwal/colors.css* and *~/.cache/wwal/colors.json* (default: *true*).

*export_hyprland* = <bool>
	Write *~/.cache/wwal/colors-hyprland.conf* (default: *true*).

*theme_from_media* = <bool>
	Dynamically extract Material Design 3 palettes from active D-Bus *MPRIS2* media player album art (default: *false*).

## [transition]

Sets default parameters for GPU compute and CPU SIMD wallpaper transitions.

*type* = "<string>"
	Default transition effect: *none*, *simple*, *fade*, *wipe*, *grow*, *outer*, *wave*, *noise*, *crosszoom*, *slide*, *glitch*, *burn*, *ripple*, *pixelate*, *doom*, *swirl*, *cube*, *luma*, *light_leak*, *page_curl*, *custom*, *left*, *right*, *top*, *bottom*, *center*, *any*, or *random* (default: *"fade"*).

*duration* = <float>
	Transition duration in seconds (default: *1.0*).

*fps* = <int>
	Target transition frame rate (default: *60*).

*angle* = <float>
	Angle in degrees for directional transitions (default: *0.0*).

*pos* = "<string>"
	Default anchor position, such as *"center"*, *"cursor"*, or *"0.5,0.5"* (default: *"center"*).

*bezier* = [ <x1>, <y1>, <x2>, <y2> ]
	Four-element float array defining the cubic-bezier easing curve (default: *[ 0.25, 0.1, 0.25, 1.0 ]*).

*sync_mode* = "<simultaneous|staggered>"
	Multi-monitor transition synchronization mode (default: *"simultaneous"*).

*stagger_delay_ms* = <int>
	Delay in milliseconds between monitors in *staggered* mode (default: *150*).

## [hooks]

Configures optional shell commands executed asynchronously by *wwald*(1) on state changes.

*on_wallpaper_change* = "<command>"
	Command executed after a wallpaper transition completes.

*on_theme_change* = "<command>"
	Command executed after a Material Design 3 palette is exported.

## [output."<NAME>"]

Configures initial wallpaper, video, shader, Workshop scene, or slideshow settings for a specific Wayland output (such as *[output."DP-1"]* or *[output."eDP-1"]*). Use *[output."\*"]* as a wildcard fallback for unconfigured monitors.

*path* = "<path>"
	Path to an image, video, shader, Workshop scene, or slideshow directory.

*mode* = "<img|video|shader|workshop|slideshow|color>"
	Content mode for this output (default: inferred from _path_).

*scaling* = "<fill|fit|stretch|center|tile|span>"
	Aspect-ratio scaling mode (default: *"fill"*).

*fill_color* = "<hex>"
	Padding color for *fit* and *center* scaling modes (default: *"000000"*).

*interval* = <int>
	Slideshow interval in seconds when *mode = "slideshow"* (default: *300*).

*shuffle* = <bool>
	Randomize slideshow order on this output (default: *true*).

*audio* = <bool>
	Enable PipeWire Bark FFT audio capture when *mode = "shader"* or *mode = "workshop"* (default: *false*).

*volume* = <float>
	Per-output wallpaper audio playback volume in *0.0..1.0* (default: *0.0*).

*mute_on_audio* = <bool>
	Per-output automatic volume ducking when external audio plays (default: *false*).

## [workspace."<NAME>"]

Binds a specific wallpaper, video, or shader to a named or numbered compositor workspace (such as *[workspace."1"]* or *[workspace."gaming"]*) on Hyprland, Sway, or Niri.

*path* = "<path>"
	Wallpaper path to activate when switching to this workspace.

*output* = "<NAME>"
	Optional output constraint if the workspace is pinned to a monitor.

*transition_type* = "<string>"
	Optional transition override when entering this workspace.

# EXAMPLE

```
[daemon]
namespace = "default"
layer = "background"
backdrop = "auto-blur"
10bit = true
restore_on_start = true
pause_on_fullscreen = true
pause_on_maximized = true
pause_on_occluded_workspace = true
pause_on_idle = true
battery_policy = "pause-video"

[theme]
enable = true
mode = "dark"
scheme_variant = "tonal_spot"
caelestia_sync = true
terminal_osc = true
export_css = true
export_hyprland = true

[transition]
type = "grow"
duration = 0.75
fps = 144
pos = "cursor"
bezier = [ 0.22, 1.0, 0.36, 1.0 ]
sync_mode = "staggered"
stagger_delay_ms = 120

[output."DP-1"]
path = "~/Pictures/Wallpapers/ultrawide-neon.jxl"
scaling = "fill"

[output."eDP-1"]
path = "~/Pictures/Wallpapers/Rotating"
mode = "slideshow"
interval = 600
shuffle = true

[workspace."9"]
path = "~/Shaders/audio_nebula.frag"
output = "DP-1"
transition_type = "fade"
```

# SEE ALSO

*wwal*(1), *wwald*(1)
