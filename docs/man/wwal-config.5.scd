wwal-config(5) "wwal 1.1.0" "WayWal Manual"

# NAME

wwal-config - declarative TOML configuration file for wwald(1) and wwal(1)

# SYNOPSIS

*~/.config/wwal/config.toml*

# DESCRIPTION

*wwal*(1) and *wwald*(1) read their declarative configuration from *$WWAL_CONFIG_PATH* or *$XDG_CONFIG_HOME/wwal/config.toml* (defaulting to *~/.config/wwal/config.toml*). The file uses standard TOML syntax and is parsed by *wwald*'s built-in zero-allocation parser at daemon startup and upon receiving *SIGHUP*. Leading *~/* and environment variables (*$HOME*, *$XDG_CONFIG_HOME*, *$XDG_CACHE_HOME*, *$XDG_DATA_HOME*, *$XDG_STATE_HOME*) are expanded automatically in all file path fields.

Command-line flags passed to *wwal*(1) override values configured in *config.toml* for that invocation.

# SECTIONS

## [daemon] (aliases: [general], [audio], [mpris])

Controls core *wwald*(1) lifecycle, layer-shell behavior, and Zero-Watt power governance.

*namespace* = "<string>"
	Default IPC and Wayland layer-shell namespace (default: *"default"*).

*layer* = "<background|bottom>"
	Wayland *zwlr_layer_shell_v1* layer used for wallpaper surfaces (default: *"background"*).

*backdrop* = "<none|auto-blur|path>"
	Secondary layer surface (*waywal-backdrop*) mode for compositors with overview backdrops such as Niri (default: *"none"*). Set to *"auto-blur"* to run the GPU 2-pass Dual-Kawase blur shader automatically.

*crop_gravity* = "<gravity>"
	Default UV crop anchor for *fill* (*crop*) scaling: *center*, *top*, *bottom*, *left*, *right*, *top-left* (*tl*), *top-right* (*tr*), *bottom-left* (*bl*), *bottom-right* (*br*), cardinal aliases (*north*, *south*, *east*, *west*, *nw*, *ne*, *sw*, *se*), or *"X,Y"* / *"X%,Y%"* (default: *"center"*).

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

*theme_from_media* = <bool>
	Dynamically extract Material Design 3 palettes from active D-Bus *MPRIS2* media player album art (default: *false*).

*recursive* = <bool>
	Scan slideshow directories recursively by default (default: *false*).

## [theme]

Controls the in-pipeline OKLab Wu quantizer + weighted K-Means++ + CAM16/HCT dual-mode Material Design 3 palette and template engine.

*enabled*, *enable* = <bool>
	Automatically extract and export color palettes whenever a wallpaper changes (default: *true*).

*mode* = "<dark|light|auto>"
	Target Material Design 3 tonal scheme polarity (default: *"dark"*).

*auto_day_night* = <bool>
	Automatically switch between *dark* and *light* theme polarity based on astronomical solar elevation (*[solar]*) or time of day (default: *false*).

*scheme_variant* = "<tonal_spot|vibrant|expressive|neutral|monochrome|fidelity|content|rainbow|fruit_salad>"
	CAM16/HCT chroma and hue rotation variant (default: *"tonal_spot"*).

*contrast* = <float>
	Contrast level between *-1.0* and *1.0* (default: *0.0*).

*source_color_index* = <0..3>
	Select which of the 4 ranked K-Means++ seed clusters to use as the primary Material Design 3 seed (default: *0*).

*seed_color* = "<#RRGGBB>"
	Optional static hex seed color override that bypasses image quantization when non-empty.

*caelestia_sync* = <bool>
	Atomically write *~/.local/state/caelestia/scheme.json* for Quickshell (*caelestia-shell*) live theme synchronization (default: *true*).

*tty_sequences*, *terminal_osc* = <bool>
	Broadcast non-blocking ANSI *OSC 4*, *OSC 10*, and *OSC 11* escape sequences to active */dev/pts/\** terminals (default: *true*).

*export_css* = <bool>
	Write *~/.cache/wwal/colors.css* and *~/.cache/wwal/colors.json* (default: *true*).

*export_hyprland* = <bool>
	Write *~/.cache/wwal/colors-hyprland.conf* (default: *true*).

*pywal_compat*, *export_pywal* = <bool>
	Atomically write *~/.cache/wal/colors.json*, *colors*, *colors.sh*, *colors.css*, and *sequences* for 100% *pywal* and *Pywalfox* compatibility (default: *true*).

*theme_from_media* = <bool>
	Dynamically extract Material Design 3 palettes from active D-Bus *MPRIS2* media player album art (default: *false*).

## [template."<NAME>"] (alias: [templates."<NAME>"])

Defines a declarative configuration template rendered atomically whenever a theme is extracted. Both *dark* and *light* palettes are synthesized simultaneously so templates can reference *default*, *dark*, and *light* tokens in a single pass:

*input_path*, *input*, *template*, *src* = "<path>"
	Source template file path.

*output_path*, *output*, *target*, *dst* = "<path>"
	Destination path where the rendered file is written atomically (protected against overwriting system directories or sensitive dotfiles such as *~/.ssh*).

*post_hook*, *hook* = "<command>"
	Optional shell command executed asynchronously after this template is rendered (for example, *"killall -SIGUSR1 kitty || true"*).

*enabled*, *enable* = <bool>
	Enable or disable this template rule (default: *true*).

Supported template token syntax inside *input_path*:
- *Matugen dual-mode role & tonal tokens*: *{{colors.<role>.<default|dark|light>.<fmt>}}* and *{{palettes.<primary|secondary|tertiary|neutral|neutral_variant|error>.<0..100>.<fmt>}}*, where *<fmt>* is one of *hex*, *strip* (*hex_stripped*), *rgb*, *rgb_css*, *rgba*, *xrgba*, *octal*, *hsl*, *oklch*, *red* (*r*), *green* (*g*), *blue* (*b*), *alpha* (*a*), *hue* (*h*), *saturation* (*s*), *lightness* (*l*).
- *Wallust / Jinja2 pipe filters*: *lighten(x)*, *darken(x)*, *set_lightness(x)* (*lightness(x)*), *saturate(x)*, *desaturate(x)*, *alpha(x)* (*set_alpha(x)*), *blend(target, factor)* (in perceptual OKLab space), *invert*, and *complementary* (180-degree OKLCh hue rotation). Supports chained pipes and parenthesized properties such as *{{ (colors.primary.default | lighten(0.15) | alpha(0.85)).rgba }}*.
- *Pywal single-brace tokens*: *{background}*, *{foreground}*, *{cursor}*, *{wallpaper}*, and *{color0}*..*{color15}* with optional *.strip*, *.rgb*, *.xrgba*, and *.octal* suffixes.

## [transition]

Sets default parameters for GPU compute and CPU SIMD wallpaper transitions.

*type* = "<string>"
	Default transition effect: *none*, *simple*, *fade*, *wipe*, *grow*, *outer*, *wave*, *noise*, *crosszoom*, *slide*, *glitch*, *burn*, *ripple*, *pixelate*, *doom*, *swirl*, *cube*, *luma*, *light_leak*, *page_curl*, *custom*, *left*, *right*, *top*, *bottom*, *center*, *any*, or *random* (default: *"fade"*).

*duration* = <float>
	Transition duration in seconds as an unquoted floating-point number such as *0.75* or *1.2* (default: *0.7*).

*fps* = <int>
	Target transition frame rate (default: *60*).

*angle* = <float>
	Angle in degrees for directional transitions (default: *0.0*).

*pos* = "<string>"
	Default anchor position, such as *"center"*, *"cursor"*, *"tl"*, or *"0.5,0.5"* (default: *"center"*).

*invert_y* = <bool>
	Invert the Y coordinate of *pos* (default: *false*).

*crop_gravity* = "<gravity>"
	Default crop anchor for transitions (default: *"center"*).

*bezier* = [ <x1>, <y1>, <x2>, <y2> ]
	Four-element float array defining the cubic-bezier easing curve (default: *[ 0.25, 0.1, 0.25, 1.0 ]*).

*sync_mode* = "<simultaneous|staggered>"
	Multi-monitor transition synchronization mode (default: *"simultaneous"*).

*stagger_delay_ms* = <int>
	Delay in milliseconds between monitors in *staggered* mode (default: *150*).

## [slideshow]

Sets global default parameters for mixed-media directory slideshows.

*interval*, *duration* = <int|string>
	Slideshow interval in seconds or human-readable units such as *"30m"* or *"1h"* (default: *300*).

*shuffle* = <bool>
	Randomize slideshow order (default: *true*).

*sort*, *sorting* = "<ascending|descending|name|mtime|random>"
	Default traversal order for slideshow directories (*name* is an alias for *ascending*).

*recursive* = <bool>
	Scan slideshow directories recursively (default: *false*).

## [solar] & [[schedule]]

Configures astronomical solar elevation calculations (NOAA solar position equations) and time-of-day dynamic wallpaper and theme polarity switching.

*enabled*, *enable* = <bool>
	Enable solar/schedule evaluation (default: *true* when solar paths or schedules are configured).

*latitude*, *lat* = <float>
	Geographic latitude in decimal degrees (*-90.0..90.0*).

*longitude*, *lon*, *lng* = <float>
	Geographic longitude in decimal degrees (*-180.0..180.0*).

*auto_theme_mode*, *auto_day_night* = <bool>
	Automatically switch Material Design 3 theme polarity between *light* (daytime) and *dark* (dusk/night) at solar transitions.

*sunrise* = "<HH:MM>", *sunset* = "<HH:MM>"
	Explicit 24-hour sunrise and sunset time overrides when *latitude* and *longitude* are omitted.

*dawn_path* (*dawn*), *day_path* (*day*), *dusk_path* (*dusk*), *night_path* (*night*) = "<path>"
	Shorthand wallpaper paths for the four solar periods.

*manifest_path*, *manifest*, *path* = "<path>"
	Optional GNOME *.xml* dynamic wallpaper manifest path.

Each *[[schedule]]* table supports:
- *phase* = *"dawn|morning|day|noon|afternoon|dusk|evening|night"* or *time* (*start*) = *"HH:MM"*
- *path* (*image*, *file*, *media_path*) = *"<wallpaper-path>"*
- *theme_mode* (*mode*) = *"dark|light|auto"*
- *output* (*output_name*) = *"<output-name>"*

## [hooks]

Configures optional shell commands executed asynchronously by *wwald*(1) on state changes.

*on_wallpaper_change* = "<command>"
	Command executed after a wallpaper transition completes.

*on_theme_change* = "<command>"
	Command executed after a Material Design 3 palette is exported.

## [output."<NAME>"], [default], [any]

Configures initial wallpaper, video, shader, Workshop scene, or slideshow settings for a specific Wayland output (such as *[output."DP-1"]* or *[output."eDP-1"]*). Use *[output."\*"]*, *[default]*, or *[any]* (*wpaperd* compatibility) as a wildcard fallback for unconfigured monitors.

*path*, *image*, *media_path* = "<path>"
	Path to an image, video, shader, Workshop scene, GNOME *.xml* schedule, or slideshow directory.

*mode* = "<img|video|shader|workshop|slideshow|color>"
	Content mode for this output (default: inferred from _path_).

*scaling* = "<fill|fit|fit-border-color|stretch|center|tile|span>"
	Aspect-ratio scaling mode (default: *"fill"*).

*crop_gravity* = "<gravity>"
	Per-output UV crop anchor for *fill* (*crop*) scaling.

*filter* = "<Nearest|Bilinear|CatmullRom|Mitchell|Lanczos3>"
	Per-output resampling filter hint.

*fill_color*, *color* = "<hex>"
	Padding color for *fit* and *center* scaling modes, or solid color when *mode = "color"* (default: *"000000"*).

*interval*, *duration* = <int|string>
	Slideshow interval in seconds or human-readable duration string (*"30m"*, *"1h"*) when *mode = "slideshow"* (default: *300*).

*shuffle* = <bool>
	Randomize slideshow order on this output (default: *true*).

*sort*, *sorting*, *order* = "<ascending|descending|name|mtime|random>"
	Per-output slideshow sort order.

*recursive* = <bool>
	Scan the output's slideshow directory recursively (default: *false*).

*audio* = <bool>
	Enable PipeWire Bark FFT audio capture when *mode = "shader"* or *mode = "workshop"* (default: *false*).

*volume* = <float>
	Per-output wallpaper audio playback volume in *0.0..1.0* (default: *0.0*).

*mute_on_audio* = <bool>
	Per-output automatic volume ducking when external audio plays (default: *false*).

## [workspace."<NAME>"]

Binds a specific wallpaper, video, shader, Workshop scene, or slideshow to a named or numbered compositor workspace (such as *[workspace."1"]* or *[workspace."gaming"]*) on Hyprland, Sway, or Niri.

*path*, *image*, *media_path* = "<path>"
	Wallpaper, video, shader, Workshop scene, or directory path to activate when switching to this workspace.

*output*, *output_name* = "<NAME>"
	Optional output constraint if the workspace is pinned to a monitor.

*mode* = "<img|video|shader|workshop|slideshow|color>"
	Content mode override for this workspace (default: inferred from _path_).

*color*, *fill_color* = "<hex>"
	Padding color or solid background color when *mode = "color"*.

*scaling* = "<fill|fit|fit-border-color|stretch|center|tile|span>"
	Aspect-ratio scaling mode override for this workspace.

*crop_gravity* = "<gravity>"
	Per-workspace UV crop anchor for *fill* (*crop*) scaling.

*filter* = "<Nearest|Bilinear|CatmullRom|Mitchell|Lanczos3>"
	Per-workspace resampling filter hint.

*interval*, *duration* = <int|string>
	Slideshow interval when *mode = "slideshow"*.

*shuffle* = <bool>
	Randomize slideshow order on this workspace.

*sorting*, *sort*, *order* = "<ascending|descending|name|mtime|random>"
	Slideshow sort order on this workspace.

*recursive* = <bool>
	Scan the workspace's slideshow directory recursively.

*fps* = <int>
	Target frame rate when running a shader or Workshop scene on this workspace.

*audio* = <bool>
	Enable PipeWire Bark FFT audio capture for this workspace's shader or Workshop scene.

*prop_overrides*, *props* = "<key=val,...>"
	Wallpaper Engine Workshop user property overrides for this workspace.

*transition_type*, *transition* = "<string>"
	Optional transition effect override when entering this workspace.

*transition_duration* = <float>
	Optional transition duration in seconds when entering this workspace.

*transition_angle*, *angle* = <float>
	Optional transition angle in degrees when entering this workspace.

*directional* = <bool>
	Automatically align slide/wipe transition direction with workspace switch direction (default: *true*).

# EXAMPLE

```
[daemon]
namespace = "default"
layer = "background"
backdrop = "auto-blur"
crop_gravity = "center"
10bit = true
restore_on_start = true
pause_on_fullscreen = true
pause_on_occluded_workspace = true
pause_on_idle = true
battery_policy = "pause-video"

[theme]
enable = true
mode = "dark"
auto_day_night = true
scheme_variant = "tonal_spot"
source_color_index = 0
caelestia_sync = true
terminal_osc = true
export_css = true
export_hyprland = true
pywal_compat = true

[template."kitty"]
input_path = "~/.config/wwal/templates/kitty.conf"
output_path = "~/.config/kitty/colors-wwal.conf"
post_hook = "killall -SIGUSR1 kitty || true"

[solar]
latitude = 37.7749
longitude = -122.4194
auto_theme_mode = true

[transition]
type = "grow"
duration = 0.75
fps = 144
pos = "cursor"
bezier = [ 0.22, 1.0, 0.36, 1.0 ]
sync_mode = "staggered"
stagger_delay_ms = 120

[default]
path = "~/Pictures/Wallpapers/Rotating"
duration = "30m"
sort = "mtime"
```

# SEE ALSO

*wwal*(1), *wwald*(1)
