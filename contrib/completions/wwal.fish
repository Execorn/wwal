# SPDX-License-Identifier: GPL-3.0-or-later
complete -c wwal -f
complete -c swww -w wwal
complete -c awww -w wwal

set -l trans_types "none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl custom random left right top bottom center any"
set -l trans_pos "center top bottom left right top-left top-right bottom-left bottom-right tl tr bl br cursor mouse"
set -l sync_modes "simultaneous staggered"
set -l scaling_modes "fill fit fit-border-color stretch center tile span crop cover contain no"
set -l filters "Nearest Bilinear CatmullRom Mitchell Lanczos3"
set -l formats "xrgb xbgr rgb bgr"
set -l layers "background bottom"
set -l battery_policies "ignore reduce-fps pause-video"
set -l backdrops "none auto-blur"
set -l pick_menus "auto rofi fuzzel walker dmenu stdout json"
set -l crop_gravities "center top bottom left right top-left top-right bottom-left bottom-right tl tr bl br north south east west nw ne sw se"
set -l sort_modes "ascending descending name mtime random"
set -l shader_presets "aurora cyber_grid fluid_ink synthwave_sun starfield rain_glass audio_bars audio_ring crt_retro plasma_orb lava_lamp northern_lights outrun crt"

complete -c wwal -s j -l json -d "Format query/status/stats output as JSON"
complete -c wwal -s n -l namespace -d "Socket and layer-shell namespace" -r
complete -c wwal -s a -l all -d "Broadcast command to all active daemon namespaces"
complete -c wwal -s q -l quiet -d "Suppress non-error output"
complete -c wwal -s d -l daemon -d "Fork daemon into background"
complete -c wwal -l no-daemon -d "Stay in foreground"
complete -c wwal -s f -l format -a "$formats" -d "Pixel format hint (swww/awww compatibility)" -r
complete -c wwal -l layer -a "$layers" -d "Wayland layer-shell layer" -r
complete -c wwal -l battery-policy -a "$battery_policies" -d "Laptop battery power governor policy" -r
complete -c wwal -s v -l verbose -d "Enable verbose output"
complete -c wwal -s V -l version -d "Print version information"
complete -c wwal -s h -l help -d "Display help message"

complete -c wwal -n "__fish_use_subcommand" -a img -d "Universal wallpaper command (images, videos, shaders, Workshop, XML, dirs)"
complete -c wwal -n "__fish_use_subcommand" -a pick -d "Launch unified icon-grid wallpaper, Workshop, video, and shader picker"
complete -c wwal -n "__fish_use_subcommand" -a video -d "Play hardware-accelerated video wallpaper (VA-API)"
complete -c wwal -n "__fish_use_subcommand" -a shader -d "Run live Shadertoy .frag, GLES 3.1 .comp, or built-in preset shader"
complete -c wwal -n "__fish_use_subcommand" -a workshop -d "Discover, inspect, apply, or sync previews for Steam Workshop (431960)"
complete -c wwal -n "__fish_use_subcommand" -a slideshow -d "Manage daemon-native timerfd or GNOME XML wallpaper slideshow"
complete -c wwal -n "__fish_use_subcommand" -a next -d "Advance active slideshow to next wallpaper"
complete -c wwal -n "__fish_use_subcommand" -a prev -d "Return active slideshow to previous wallpaper"
complete -c wwal -n "__fish_use_subcommand" -a previous -d "Return active slideshow to previous wallpaper"
complete -c wwal -n "__fish_use_subcommand" -a color -d "Set wallpaper to solid hex color (wp_single_pixel_buffer_v1)"
complete -c wwal -n "__fish_use_subcommand" -a clear -d "Clear wallpaper to solid hex color (swww/awww alias)"
complete -c wwal -n "__fish_use_subcommand" -a theme -d "Extract dual-mode OKLab/CAM16/HCT Material Design 3 palette"
complete -c wwal -n "__fish_use_subcommand" -a audio -d "Configure live wallpaper audio volume and auto-ducking"
complete -c wwal -n "__fish_use_subcommand" -a set-uniform -d "Mutate live shader uniform or Workshop property"
complete -c wwal -n "__fish_use_subcommand" -a uniform -d "Mutate live shader uniform or Workshop property"
complete -c wwal -n "__fish_use_subcommand" -a status -d "Inspect outputs, active media, MPRIS, and telemetry"
complete -c wwal -n "__fish_use_subcommand" -a subscribe -d "Stream newline-delimited JSON state change events"
complete -c wwal -n "__fish_use_subcommand" -a pause -d "Pause video, animation, shader, or Workshop playback"
complete -c wwal -n "__fish_use_subcommand" -a resume -d "Resume paused video, animation, shader, or Workshop playback"
complete -c wwal -n "__fish_use_subcommand" -a unpause -d "Resume paused video, animation, shader, or Workshop playback"
complete -c wwal -n "__fish_use_subcommand" -a toggle -d "Toggle video, animation, shader, or Workshop playback pause/resume"
complete -c wwal -n "__fish_use_subcommand" -a query -d "List connected Wayland outputs and active geometry"
complete -c wwal -n "__fish_use_subcommand" -a stats -d "Report real-time RSS/PSS, DMA-BUF, VBlank PLL, and pause_mask telemetry"
complete -c wwal -n "__fish_use_subcommand" -a restore -d "Restore last persisted per-output state"
complete -c wwal -n "__fish_use_subcommand" -a clear-cache -d "Remove cached hardware transcodes, thumbnails, and palette files"
complete -c wwal -n "__fish_use_subcommand" -a init -d "Launch or initialize wallpaper daemon (swww/awww alias)"
complete -c wwal -n "__fish_use_subcommand" -a daemon -d "Launch or initialize wallpaper daemon"
complete -c wwal -n "__fish_use_subcommand" -a ping -d "Check if wwald daemon is running and responsive"
complete -c wwal -n "__fish_use_subcommand" -a kill -d "Gracefully terminate the running wwald daemon"
complete -c wwal -n "__fish_use_subcommand" -a version -d "Print version information"
complete -c wwal -n "__fish_use_subcommand" -a help -d "Display help message"

for subcmd in img slideshow pick
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -s t -l transition-type -a "$trans_types" -d "Transition type" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-duration -d "Transition duration in seconds (float, e.g. 0.5, 1.5)" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-fps -d "Target frame rate" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-bezier -d "Cubic Bezier control points X1,Y1,X2,Y2" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-angle -d "Transition angle in degrees" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-wave -d "Wave frequency and amplitude (e.g. 20,0.05)" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-pos -a "$trans_pos" -d "Center coordinate or semantic position" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l invert-y -d "Invert Y coordinate of transition-pos"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-step -d "Per-frame transition step (1..255)" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-shader -d "Path to custom GLSL compute shader" -r -F
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -s o -l output -d "Target monitor name(s)" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l outputs -d "Target monitor name(s)" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l sync-mode -a "$sync_modes" -d "Multi-monitor transition sync mode" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l stagger-delay -d "Stagger delay between monitors in ms" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l 10bit -d "Request 10-bit wide-gamut scanout"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l scaling-mode -a "$scaling_modes" -d "Aspect-ratio scaling mode" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l mode -a "$scaling_modes" -d "Aspect-ratio scaling mode" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l resize -a "$scaling_modes" -d "Aspect-ratio scaling mode (swww/awww)" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l no-resize -d "Center image at 1:1 pixel resolution"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l crop-gravity -a "$crop_gravities" -d "Crop anchor for fill/crop mode" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l fill-color -d "Padding hex color for fit/center modes" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -s f -l filter -a "$filters" -d "Scaling filter hint" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l no-cache -d "Bypass transition/palette disk caches"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l clear-cache -d "Purge cached transcodes and thumbnails before applying"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l backdrop -a "$backdrops" -d "Overview backdrop mode or file" -r -F
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l depth-map -d "Depth-map image for 3D Parallax Occlusion Mapping" -r -F
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l depth-strength -d "Depth parallax displacement multiplier" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l theme -d "Extract Material You palette and sync caelestia/OSC/templates"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l no-theme -d "Disable automatic Material You palette extraction"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l theme-mode -a "dark light auto" -d "Target Material Design 3 polarity" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l dark -d "Force dark Material Design 3 theme mode"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l light -d "Force light Material Design 3 theme mode"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l auto-day-night -d "Auto-select dark/light mode from solar elevation or time"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l no-auto-day-night -d "Disable automatic solar day/night theme polarity"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l latitude -d "Observer latitude (-90..90) for astronomical solar elevation" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l longitude -d "Observer longitude (-180..180) for astronomical solar elevation" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l source-color-index -a "0 1 2 3" -d "Ranked K-Means++ seed cluster index" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l seed-color -d "Override palette extraction with explicit hex seed color" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l theme-from-media -d "Extract Material You palette dynamically from MPRIS2 album art"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l no-theme-from-media -d "Disable MPRIS2 album art theme extraction"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l animate -d "Play animated GIF/WebP/APNG via video engine"
end

complete -c wwal -n "__fish_seen_subcommand_from img" -F
complete -c wwal -n "__fish_seen_subcommand_from pick" -F
complete -c wwal -n "__fish_seen_subcommand_from pick" -l menu -a "$pick_menus" -d "Launcher menu protocol" -r
complete -c wwal -n "__fish_seen_subcommand_from pick" -l dir -d "Add wallpaper search directory" -r -F

complete -c wwal -n "__fish_seen_subcommand_from theme" -F
complete -c wwal -n "__fish_seen_subcommand_from theme" -s i -l img -l image -d "Input image path for palette extraction" -r -F
complete -c wwal -n "__fish_seen_subcommand_from theme" -l mode -l theme-mode -a "dark light auto" -d "Target Material Design 3 polarity" -r
complete -c wwal -n "__fish_seen_subcommand_from theme" -l dark -d "Force dark Material Design 3 theme mode"
complete -c wwal -n "__fish_seen_subcommand_from theme" -l light -d "Force light Material Design 3 theme mode"
complete -c wwal -n "__fish_seen_subcommand_from theme" -l auto-day-night -d "Auto-select dark/light mode from solar elevation or time"
complete -c wwal -n "__fish_seen_subcommand_from theme" -l no-auto-day-night -d "Disable automatic solar day/night theme polarity"
complete -c wwal -n "__fish_seen_subcommand_from theme" -l latitude -d "Observer latitude (-90..90) for astronomical solar elevation" -r
complete -c wwal -n "__fish_seen_subcommand_from theme" -l longitude -d "Observer longitude (-180..180) for astronomical solar elevation" -r
complete -c wwal -n "__fish_seen_subcommand_from theme" -l source-color-index -a "0 1 2 3" -d "Ranked K-Means++ seed cluster index" -r
complete -c wwal -n "__fish_seen_subcommand_from theme" -l seed-color -d "Override palette extraction with explicit hex seed color" -r
complete -c wwal -n "__fish_seen_subcommand_from theme" -l no-cache -d "Bypass palette disk cache"
complete -c wwal -n "__fish_seen_subcommand_from theme" -l theme-from-media -d "Extract Material You palette dynamically from MPRIS2 album art"
complete -c wwal -n "__fish_seen_subcommand_from theme" -l no-theme-from-media -d "Disable MPRIS2 album art theme extraction"

complete -c wwal -n "__fish_seen_subcommand_from color clear pause resume unpause toggle restore set-uniform uniform next prev previous" -s o -l output -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from color clear pause resume unpause toggle restore set-uniform uniform next prev previous" -l outputs -d "Target monitor name(s)" -r

complete -c wwal -n "__fish_seen_subcommand_from video" -F
complete -c wwal -n "__fish_seen_subcommand_from video" -s o -l output -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l outputs -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l loop -d "Loop count (0 = infinite)" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l speed -d "Playback speed multiplier" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l audio -d "Enable PipeWire audio capture"
complete -c wwal -n "__fish_seen_subcommand_from video" -l volume -d "Wallpaper audio volume (0.0..1.0)" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l mute-on-audio -d "Auto-duck volume when another app plays audio"
complete -c wwal -n "__fish_seen_subcommand_from video" -l no-mute-on-audio -d "Disable automatic audio ducking"
complete -c wwal -n "__fish_seen_subcommand_from video" -l crop-gravity -a "$crop_gravities" -d "Crop anchor for fill/crop mode" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l backdrop -a "$backdrops" -d "Overview backdrop mode" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l theme -d "Extract Material You palette and sync caelestia/OSC"
complete -c wwal -n "__fish_seen_subcommand_from video" -l no-theme -d "Disable automatic Material You palette extraction"
complete -c wwal -n "__fish_seen_subcommand_from video" -l theme-mode -a "dark light auto" -d "Target Material Design 3 polarity" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l dark -d "Force dark Material Design 3 theme mode"
complete -c wwal -n "__fish_seen_subcommand_from video" -l light -d "Force light Material Design 3 theme mode"
complete -c wwal -n "__fish_seen_subcommand_from video" -l auto-day-night -d "Auto-select dark/light mode from solar elevation or time"
complete -c wwal -n "__fish_seen_subcommand_from video" -l no-auto-day-night -d "Disable automatic solar day/night theme polarity"
complete -c wwal -n "__fish_seen_subcommand_from video" -l latitude -d "Observer latitude (-90..90) for astronomical solar elevation" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l longitude -d "Observer longitude (-180..180) for astronomical solar elevation" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l source-color-index -a "0 1 2 3" -d "Ranked K-Means++ seed cluster index" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l seed-color -d "Override palette extraction with explicit hex seed color" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l theme-from-media -d "Extract Material You palette dynamically from MPRIS2 album art"
complete -c wwal -n "__fish_seen_subcommand_from video" -l no-theme-from-media -d "Disable MPRIS2 album art theme extraction"

complete -c wwal -n "__fish_seen_subcommand_from shader" -a "list preset $shader_presets"
complete -c wwal -n "__fish_seen_subcommand_from shader" -l list -d "List built-in procedural shader presets"
complete -c wwal -n "__fish_seen_subcommand_from shader" -l preset -a "$shader_presets" -d "Run a built-in procedural shader preset" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -F
complete -c wwal -n "__fish_seen_subcommand_from shader" -s o -l output -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -l outputs -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -l audio -d "Enable PipeWire 64-bin Bark FFT audio uniforms"
complete -c wwal -n "__fish_seen_subcommand_from shader" -l volume -d "Wallpaper audio volume (0.0..1.0)" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -l mute-on-audio -d "Auto-duck volume when another app plays audio"
complete -c wwal -n "__fish_seen_subcommand_from shader" -l no-mute-on-audio -d "Disable automatic audio ducking"
complete -c wwal -n "__fish_seen_subcommand_from shader" -l fps -d "Target shader frame rate" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -l speed -d "Shader time speed multiplier" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -l 10bit -d "Request 10-bit wide-gamut scanout"
complete -c wwal -n "__fish_seen_subcommand_from shader" -l backdrop -a "$backdrops" -d "Overview backdrop mode" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -l theme-from-media -d "Extract Material You palette dynamically from MPRIS2 album art"
complete -c wwal -n "__fish_seen_subcommand_from shader" -l no-theme-from-media -d "Disable MPRIS2 album art theme extraction"

complete -c wwal -n "__fish_seen_subcommand_from workshop" -a "list info apply sync-previews"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -F
complete -c wwal -n "__fish_seen_subcommand_from workshop" -s o -l output -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l outputs -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -s p -l prop -d "Override Workshop user property (key=value)" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l audio -d "Enable PipeWire 64-bin Bark FFT audio reactivity"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l volume -d "Wallpaper audio volume (0.0..1.0)" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l mute-on-audio -d "Auto-duck volume when another app plays audio"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l no-mute-on-audio -d "Disable automatic audio ducking"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l fps -d "Target frame rate" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l backdrop -a "$backdrops" -d "Overview backdrop mode" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l theme -d "Extract Material You palette and sync caelestia/OSC"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l no-theme -d "Disable automatic Material You palette extraction"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l theme-mode -a "dark light auto" -d "Target Material Design 3 polarity" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l dark -d "Force dark Material Design 3 theme mode"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l light -d "Force light Material Design 3 theme mode"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l auto-day-night -d "Auto-select dark/light mode from solar elevation or time"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l no-auto-day-night -d "Disable automatic solar day/night theme polarity"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l latitude -d "Observer latitude (-90..90) for astronomical solar elevation" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l longitude -d "Observer longitude (-180..180) for astronomical solar elevation" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l source-color-index -a "0 1 2 3" -d "Ranked K-Means++ seed cluster index" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l seed-color -d "Override palette extraction with explicit hex seed color" -r
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l theme-from-media -d "Extract Material You palette dynamically from MPRIS2 album art"
complete -c wwal -n "__fish_seen_subcommand_from workshop" -l no-theme-from-media -d "Disable MPRIS2 album art theme extraction"

complete -c wwal -n "__fish_seen_subcommand_from audio" -l volume -d "Wallpaper audio volume (0.0..1.0)" -r
complete -c wwal -n "__fish_seen_subcommand_from audio" -l mute-on-audio -d "Auto-duck volume when another app plays audio"
complete -c wwal -n "__fish_seen_subcommand_from audio" -l no-mute-on-audio -d "Disable automatic audio ducking"
complete -c wwal -n "__fish_seen_subcommand_from audio" -l theme-from-media -d "Extract Material You palette dynamically from MPRIS2 album art"
complete -c wwal -n "__fish_seen_subcommand_from audio" -l no-theme-from-media -d "Disable MPRIS2 album art theme extraction"

complete -c wwal -n "__fish_seen_subcommand_from slideshow" -a "start stop pause resume toggle next prev previous"
complete -c wwal -n "__fish_seen_subcommand_from slideshow" -s d -l interval -l duration -d "Slideshow interval (seconds or 30m, 1h)" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow" -s s -l shuffle -d "Shuffle wallpapers randomly"
complete -c wwal -n "__fish_seen_subcommand_from slideshow" -l random -d "Shuffle wallpapers randomly"
complete -c wwal -n "__fish_seen_subcommand_from slideshow" -l sort -l sorting -a "$sort_modes" -d "Slideshow traversal sort order" -r
