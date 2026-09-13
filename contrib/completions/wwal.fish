# Fish completion for wwal (WayWal)

# Disable file completions by default
complete -c wwal -f

# Global options
complete -c wwal -s n -l namespace -d "Socket namespace" -r
complete -c wwal -s v -l verbose -d "Enable verbose output"
complete -c wwal -s h -l help -d "Display help message"

# Subcommands
complete -c wwal -n "__fish_use_subcommand" -a ping -d "Check if wwald daemon is running and responsive"
complete -c wwal -n "__fish_use_subcommand" -a query -d "List all detected Wayland outputs and current geometry"
complete -c wwal -n "__fish_use_subcommand" -a clear -d "Clear wallpaper to solid hex color"
complete -c wwal -n "__fish_use_subcommand" -a img -d "Load and set wallpaper with GPU/SIMD transitions"
complete -c wwal -n "__fish_use_subcommand" -a video -d "Load and play hardware-accelerated video wallpaper"
complete -c wwal -n "__fish_use_subcommand" -a slideshow -d "Manage daemon-native wallpaper slideshow"
complete -c wwal -n "__fish_use_subcommand" -a pause -d "Pause video playback"
complete -c wwal -n "__fish_use_subcommand" -a unpause -d "Resume video playback"
complete -c wwal -n "__fish_use_subcommand" -a toggle -d "Toggle video playback pause/resume"
complete -c wwal -n "__fish_use_subcommand" -a kill -d "Gracefully terminate the running wwald daemon"
complete -c wwal -n "__fish_use_subcommand" -a help -d "Display help message"

# Common transition options helper
set -l trans_types "none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl custom"
set -l trans_pos "center top bottom left right top-left top-right bottom-left bottom-right cursor mouse"
set -l sync_modes "simultaneous staggered"

# img options
complete -c wwal -n "__fish_seen_subcommand_from img" -F
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-type -a "$trans_types" -d "Transition type"
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-duration -d "Transition duration in seconds" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-fps -d "Target frame rate" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-angle -d "Transition angle in degrees" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-wave -d "Wave frequency and amplitude (e.g. 20,0.05)" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-pos -a "$trans_pos" -d "Center coordinate or semantic position" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-shader -d "Path to custom GLSL compute shader" -r -F
complete -c wwal -n "__fish_seen_subcommand_from img" -s o -l output -d "Target monitor name" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l sync-mode -a "$sync_modes" -d "Multi-monitor transition sync mode"
complete -c wwal -n "__fish_seen_subcommand_from img" -l stagger-delay -d "Stagger delay between monitors in ms" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l 10bit -d "Force 10-bit wide gamut scanout"

# clear options
complete -c wwal -n "__fish_seen_subcommand_from clear" -s o -l output -d "Target monitor name" -r

# video options
complete -c wwal -n "__fish_seen_subcommand_from video" -F
complete -c wwal -n "__fish_seen_subcommand_from video" -s o -l output -d "Target monitor name" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l loop -d "Loop count (0 = infinite)" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l speed -d "Playback speed multiplier" -r

# slideshow subcommands and options
complete -c wwal -n "__fish_seen_subcommand_from slideshow" -a "start stop pause resume toggle next prev"
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -s d -l interval -d "Slideshow interval in seconds" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -s s -l shuffle -d "Shuffle images randomly"
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -s o -l output -d "Target monitor name" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l transition-type -a "$trans_types" -d "Transition type"
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l transition-duration -d "Transition duration in seconds" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l transition-fps -d "Target frame rate" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l transition-angle -d "Transition angle in degrees" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l transition-wave -d "Wave frequency and amplitude (e.g. 20,0.05)" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l transition-pos -a "$trans_pos" -d "Center coordinate or semantic position" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l transition-shader -d "Path to custom GLSL compute shader" -r -F
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l sync-mode -a "$sync_modes" -d "Multi-monitor transition sync mode"
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l stagger-delay -d "Stagger delay between monitors in ms" -r
complete -c wwal -n "__fish_seen_subcommand_from slideshow; and __fish_seen_subcommand_from start" -l 10bit -d "Force 10-bit wide gamut scanout"
