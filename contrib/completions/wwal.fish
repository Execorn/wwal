# SPDX-License-Identifier: GPL-3.0-or-later
complete -c wwal -f

set -l trans_types "none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl custom random"
set -l trans_pos "center top bottom left right top-left top-right bottom-left bottom-right cursor mouse"
set -l sync_modes "simultaneous staggered"
set -l scaling_modes "fill fit stretch center tile span crop cover contain no"
set -l filters "Nearest Bilinear CatmullRom Mitchell Lanczos3"
set -l layers "background bottom"
set -l battery_policies "ignore reduce-fps pause-video"
set -l backdrops "none auto-blur"

complete -c wwal -s n -l namespace -d "Socket and layer-shell namespace" -r
complete -c wwal -l layer -a "$layers" -d "Wayland layer-shell layer" -r
complete -c wwal -l battery-policy -a "$battery_policies" -d "Laptop battery power governor policy" -r
complete -c wwal -s v -l verbose -d "Enable verbose output"
complete -c wwal -s h -l help -d "Display help message"

complete -c wwal -n "__fish_use_subcommand" -a img -d "Decode and set wallpaper with GPU/SIMD transitions"
complete -c wwal -n "__fish_use_subcommand" -a video -d "Play hardware-accelerated video wallpaper (VA-API)"
complete -c wwal -n "__fish_use_subcommand" -a shader -d "Run live Shadertoy .frag or GLES 3.1 .comp shader"
complete -c wwal -n "__fish_use_subcommand" -a slideshow -d "Manage daemon-native timerfd wallpaper slideshow"
complete -c wwal -n "__fish_use_subcommand" -a color -d "Set wallpaper to solid hex color (wp_single_pixel_buffer_v1)"
complete -c wwal -n "__fish_use_subcommand" -a clear -d "Clear wallpaper to solid hex color (swww/awww alias)"
complete -c wwal -n "__fish_use_subcommand" -a theme -d "Extract OKLab/CAM16/HCT Material Design 3 palette"
complete -c wwal -n "__fish_use_subcommand" -a pause -d "Pause video, animation, or shader playback"
complete -c wwal -n "__fish_use_subcommand" -a resume -d "Resume paused video, animation, or shader playback"
complete -c wwal -n "__fish_use_subcommand" -a unpause -d "Resume paused video, animation, or shader playback"
complete -c wwal -n "__fish_use_subcommand" -a toggle -d "Toggle video, animation, or shader playback pause/resume"
complete -c wwal -n "__fish_use_subcommand" -a query -d "List connected Wayland outputs and active geometry"
complete -c wwal -n "__fish_use_subcommand" -a stats -d "Report real-time RSS/PSS, DMA-BUF, VBlank PLL, and pause_mask telemetry"
complete -c wwal -n "__fish_use_subcommand" -a restore -d "Restore last persisted per-output state"
complete -c wwal -n "__fish_use_subcommand" -a clear-cache -d "Remove cached hardware transcodes and palette files"
complete -c wwal -n "__fish_use_subcommand" -a ping -d "Check if wwald daemon is running and responsive"
complete -c wwal -n "__fish_use_subcommand" -a kill -d "Gracefully terminate the running wwald daemon"
complete -c wwal -n "__fish_use_subcommand" -a help -d "Display help message"

for subcmd in img slideshow
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-type -a "$trans_types" -d "Transition type" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-duration -d "Transition duration in seconds" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-fps -d "Target frame rate" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-bezier -d "Cubic Bezier control points X1,Y1,X2,Y2" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-angle -d "Transition angle in degrees" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-wave -d "Wave frequency and amplitude (e.g. 20,0.05)" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l transition-pos -a "$trans_pos" -d "Center coordinate or semantic position" -r
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
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l fill-color -d "Padding hex color for fit/center modes" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l filter -a "$filters" -d "Scaling filter hint" -r
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l backdrop -a "$backdrops" -d "Overview backdrop mode or file" -r -F
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l depth-map -d "Depth-map image for 3D Parallax Occlusion Mapping" -r -F
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l theme -d "Extract Material You palette and sync caelestia/OSC"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l no-theme -d "Disable automatic Material You palette extraction"
    complete -c wwal -n "__fish_seen_subcommand_from $subcmd" -l animate -d "Play animated GIF/WebP/APNG via video engine"
end

complete -c wwal -n "__fish_seen_subcommand_from img" -F
complete -c wwal -n "__fish_seen_subcommand_from theme" -F

complete -c wwal -n "__fish_seen_subcommand_from color clear pause resume unpause toggle restore" -s o -l output -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from color clear pause resume unpause toggle restore" -l outputs -d "Target monitor name(s)" -r

complete -c wwal -n "__fish_seen_subcommand_from video" -F
complete -c wwal -n "__fish_seen_subcommand_from video" -s o -l output -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l outputs -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l loop -d "Loop count (0 = infinite)" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l speed -d "Playback speed multiplier" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l audio -d "Enable PipeWire audio capture"
complete -c wwal -n "__fish_seen_subcommand_from video" -l backdrop -a "$backdrops" -d "Overview backdrop mode" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l theme -d "Extract Material You palette and sync caelestia/OSC"
complete -c wwal -n "__fish_seen_subcommand_from video" -l no-theme -d "Disable automatic Material You palette extraction"

complete -c wwal -n "__fish_seen_subcommand_from shader" -F
complete -c wwal -n "__fish_seen_subcommand_from shader" -s o -l output -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -l outputs -d "Target monitor name(s)" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -l audio -d "Enable PipeWire 64-bin Bark FFT audio uniforms"
complete -c wwal -n "__fish_seen_subcommand_from shader" -l speed -d "Shader time speed multiplier" -r
complete -c wwal -n "__fish_seen_subcommand_from shader" -l 10bit -d "Request 10-bit wide-gamut scanout"
complete -c wwal -n "__fish_seen_subcommand_from shader" -l backdrop -a "$backdrops" -d "Overview backdrop mode" -r

complete -c wwal -n "__fish_seen_subcommand_from slideshow" -a "start stop pause resume toggle next prev"
complete -c wwal -n "__fish_seen_subcommand_from slideshow" -s d -l interval -d "Slideshow interval in seconds" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow" -s s -l shuffle -d "Shuffle images randomly"
complete -c wwal -n "__fish_seen_subcommand_from slideshow" -l random -d "Shuffle images randomly"
