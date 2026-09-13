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
complete -c wwal -n "__fish_use_subcommand" -a pause -d "Pause video playback"
complete -c wwal -n "__fish_use_subcommand" -a unpause -d "Resume video playback"
complete -c wwal -n "__fish_use_subcommand" -a toggle -d "Toggle video playback pause/resume"
complete -c wwal -n "__fish_use_subcommand" -a kill -d "Gracefully terminate the running wwald daemon"
complete -c wwal -n "__fish_use_subcommand" -a help -d "Display help message"

# img options
complete -c wwal -n "__fish_seen_subcommand_from img" -F
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-type -a "none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl" -d "Transition type"
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-duration -d "Transition duration in seconds" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-fps -d "Target frame rate" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-angle -d "Transition angle in degrees" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-wave -d "Wave frequency and amplitude (e.g. 20,0.05)" -r
complete -c wwal -n "__fish_seen_subcommand_from img" -l transition-pos -d "Center coordinate (e.g. 0.5,0.5)" -r

# video options
complete -c wwal -n "__fish_seen_subcommand_from video" -F
complete -c wwal -n "__fish_seen_subcommand_from video" -l loop -d "Loop count (0 = infinite)" -r
complete -c wwal -n "__fish_seen_subcommand_from video" -l speed -d "Playback speed multiplier" -r
