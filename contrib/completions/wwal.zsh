# SPDX-License-Identifier: GPL-3.0-or-later
#compdef wwal swww awww

_wwal() {
    local -a commands
    commands=(
        'img:Universal wallpaper command (images, videos, shaders, Workshop, XML, directories, hex colors)'
        'pick:Launch unified icon-grid wallpaper, Workshop, video, and shader picker'
        'video:Play hardware-accelerated video wallpaper via VA-API'
        'shader:Run live Shadertoy fragment, GLES 3.1 compute, or built-in preset shader wallpaper'
        'workshop:List, inspect, apply, or sync previews for native Steam Workshop (431960) wallpapers'
        'slideshow:Manage daemon-native timerfd or GNOME XML wallpaper slideshow'
        'next:Advance active slideshow to next wallpaper'
        'prev:Return active slideshow to previous wallpaper'
        'previous:Return active slideshow to previous wallpaper'
        'color:Set wallpaper to solid hex color via wp_single_pixel_buffer_v1'
        'clear:Clear wallpaper to solid hex color (swww/awww alias)'
        'theme:Extract dual-mode OKLab/CAM16/HCT Material Design 3 palette and render templates'
        'audio:Configure live wallpaper audio volume and external playback auto-ducking'
        'set-uniform:Mutate live shader uniform or Workshop property'
        'uniform:Alias for set-uniform'
        'status:Inspect outputs, active media, MPRIS metadata, and telemetry'
        'subscribe:Stream newline-delimited JSON state change events'
        'pause:Pause video, animation, live shader, or Workshop scene playback'
        'resume:Resume paused video, animation, live shader, or Workshop scene playback'
        'unpause:Resume paused video, animation, live shader, or Workshop scene playback'
        'toggle:Toggle video, animation, live shader, or Workshop scene playback pause/resume'
        'query:List connected Wayland outputs, scale factors, and geometry'
        'stats:Report real-time RSS/PSS, DMA-BUF modifiers, VBlank PLL, and pause_mask telemetry'
        'restore:Restore last persisted per-output state from state.bin'
        'clear-cache:Remove cached hardware transcodes, thumbnails, and palette files'
        'init:Launch or initialize wallpaper daemon (swww/awww alias)'
        'daemon:Launch or initialize wallpaper daemon'
        'ping:Check if wwald daemon is running and responsive'
        'kill:Gracefully terminate the running wwald daemon'
        'version:Print version information'
        'help:Display help message'
    )

    local -a global_opts
    global_opts=(
        '(-j --json)'{-j,--json}'[Format query/status/stats output as JSON]'
        '(-n --namespace)'{-n,--namespace}'[Socket and layer-shell namespace]:namespace:'
        '(-a --all)'{-a,--all}'[Broadcast command to all active daemon namespaces]'
        '(-q --quiet)'{-q,--quiet}'[Suppress non-error output]'
        '(-d --daemon)'{-d,--daemon}'[Fork daemon into background]'
        '--no-daemon[Stay in foreground]'
        '(-f --format)'{-f,--format}'[Pixel format hint for swww/awww daemon compatibility]:format:(xrgb xbgr rgb bgr)'
        '--layer[Wayland layer-shell layer]:layer:(background bottom)'
        '--battery-policy[Laptop battery power governor policy]:policy:(ignore reduce-fps pause-video)'
        '(-v --verbose)'{-v,--verbose}'[Enable verbose output]'
        '(-V --version)'{-V,--version}'[Print version information]'
        '(-h --help)'{-h,--help}'[Print help information]'
    )

    local -a trans_opts
    trans_opts=(
        '(-t --transition-type)'{-t,--transition-type}'[Transition effect]:type:(none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl custom random left right top bottom center any)'
        '--transition-duration[Transition duration in seconds (float, e.g. 0.5, 1.5)]:duration:'
        '--transition-fps[Target frame rate]:fps:'
        '--transition-bezier[Cubic Bezier control points X1,Y1,X2,Y2]:bezier:'
        '--transition-angle[Transition angle in degrees]:angle:'
        '--transition-wave[Wave frequency and amplitude]:freq_amp:'
        '--transition-pos[Transition anchor position X,Y or semantic alias]:pos:(center top bottom left right top-left top-right bottom-left bottom-right tl tr bl br cursor mouse)'
        '--invert-y[Invert Y coordinate of transition-pos]'
        '--transition-step[Per-frame transition step (1..255)]:step:'
        '--transition-shader[Custom GLSL compute shader path]:shader:_files -g "*.([cC][oO][mM][pP]|[gG][lL][sS][lL]|[fF][rR][aA][gG]|[sS][hH][aA][dD][eE][rR])"'
        '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:'
        '--sync-mode[Multi-monitor transition sync mode]:mode:(simultaneous staggered)'
        '--stagger-delay[Delay between monitors in ms]:delay:'
        '--10bit[Request 10-bit wide-gamut scanout (XRGB2101010)]'
        '(--scaling-mode --mode --resize)'{--scaling-mode,--mode,--resize}'[Aspect-ratio scaling mode]:mode:(fill fit fit-border-color stretch center tile span crop cover contain no)'
        '--no-resize[Center image at 1:1 pixel resolution]'
        '--crop-gravity[Crop anchor for fill/crop mode]:gravity:(center top bottom left right top-left top-right bottom-left bottom-right tl tr bl br north south east west nw ne sw se)'
        '--fill-color[Padding hex color for fit/center modes]:hex_color:'
        '(-f --filter)'{-f,--filter}'[Scaling filter hint]:filter:(Nearest Bilinear CatmullRom Mitchell Lanczos3)'
        '--no-cache[Bypass transition/palette disk caches]'
        '--clear-cache[Purge cached transcodes and thumbnails before applying]'
        '--backdrop[Secondary overview backdrop surface mode]:backdrop:(none auto-blur)'
        '--depth-map[2D depth-map image for 3D Parallax Occlusion Mapping]:depth_map:_files -g "*.([pP][nN][gG]|[jP][pP][gG]|[jJ][pP][eE][gG]|[wW][eE][bB][pP]|[aA][vV][iI][fF]|[jJ][xX][lL]|[bB][mM][pP]|[qQ][oO][iI])"'
        '--depth-strength[Depth parallax displacement multiplier]:depth_strength:'
        '--theme[Extract Material You palette and sync caelestia/OSC/templates]'
        '--no-theme[Disable automatic Material You palette extraction]'
        '--theme-mode[Target Material Design 3 polarity]:mode:(dark light auto)'
        '--dark[Force dark Material Design 3 theme mode]'
        '--light[Force light Material Design 3 theme mode]'
        '--auto-day-night[Automatically select dark/light mode from solar elevation or time]'
        '--no-auto-day-night[Disable automatic solar day/night theme polarity]'
        '--latitude[Observer latitude (-90..90) for astronomical solar elevation]:latitude:'
        '--longitude[Observer longitude (-180..180) for astronomical solar elevation]:longitude:'
        '--source-color-index[Select ranked K-Means++ seed cluster index (0..3)]:index:(0 1 2 3)'
        '--seed-color[Override palette extraction with explicit hex seed color]:hex_color:'
        '--theme-from-media[Extract Material You palette dynamically from MPRIS2 album art]'
        '--no-theme-from-media[Disable MPRIS2 album art theme extraction]'
        '--animate[Play animated GIF/WebP/APNG via video/animation engine]'
    )

    _arguments -C \
        $global_opts \
        '1: :->command' \
        '*:: :->args'

    case $state in
        command)
            _describe -t commands 'wwal commands' commands
            ;;
        args)
            case $line[1] in
                img)
                    _arguments \
                        $global_opts \
                        $trans_opts \
                        '1:wallpaper file or directory:_files -g "*.([pP][nN][gG]|[jJ][pP][gG]|[jJ][pP][eE][gG]|[wW][eE][bB][pP]|[aA][vV][iI][fF]|[hH][eE][iI][cC]|[hH][eE][iI][fF]|[jJ][xX][lL]|[sS][vV][gG]|[bB][mM][pP]|[gG][iI][fF]|[qQ][oO][iI]|[pP][pP][mM]|[pP][gG][mM]|[pP][nN][mM]|[tT][gG][aA]|[mM][pP]4|[wW][eE][bB][mM]|[mM][kK][vV]|[aA][vV][iI]|[mM][oO][vV]|[mM]4[vV]|[fF][rR][aA][gG]|[cC][oO][mM][pP]|[gG][lL][sS][lL]|[sS][hH][aA][dD][eE][rR]|[xX][mM][lL])"'
                    ;;
                pick)
                    _arguments \
                        $global_opts \
                        $trans_opts \
                        '--menu[Launcher menu protocol]:menu:(auto rofi fuzzel walker dmenu stdout json)' \
                        '--dir[Add wallpaper search directory]:dir:_files -/' \
                        '1:directory:_files -/'
                    ;;
                video)
                    _arguments \
                        $global_opts \
                        '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:' \
                        '--loop[Loop count (0 = infinite)]:count:' \
                        '--speed[Playback speed multiplier]:speed:' \
                        '--audio[Enable PipeWire audio capture]' \
                        '--volume[Wallpaper audio volume (0.0..1.0)]:volume:' \
                        '--mute-on-audio[Auto-duck volume when another app plays audio]' \
                        '--no-mute-on-audio[Disable automatic audio ducking]' \
                        '--crop-gravity[Crop anchor for fill/crop mode]:gravity:(center top bottom left right top-left top-right bottom-left bottom-right tl tr bl br)' \
                        '--backdrop[Secondary overview backdrop surface mode]:backdrop:(none auto-blur)' \
                        '--theme[Extract Material You palette and sync caelestia/OSC]' \
                        '--no-theme[Disable automatic Material You palette extraction]' \
                        '--theme-mode[Target Material Design 3 polarity]:mode:(dark light auto)' \
                        '--dark[Force dark Material Design 3 theme mode]' \
                        '--light[Force light Material Design 3 theme mode]' \
                        '--auto-day-night[Automatically select dark/light mode from solar elevation or time]' \
                        '--no-auto-day-night[Disable automatic solar day/night theme polarity]' \
                        '--latitude[Observer latitude (-90..90) for astronomical solar elevation]:latitude:' \
                        '--longitude[Observer longitude (-180..180) for astronomical solar elevation]:longitude:' \
                        '--source-color-index[Select ranked K-Means++ seed cluster index (0..3)]:index:(0 1 2 3)' \
                        '--seed-color[Override palette extraction with explicit hex seed color]:hex_color:' \
                        '--theme-from-media[Extract Material You palette dynamically from MPRIS2 album art]' \
                        '--no-theme-from-media[Disable MPRIS2 album art theme extraction]' \
                        '1:video file:_files -g "*.([mM][pP]4|[wW][eE][bB][mM]|[mM][kK][vV]|[aA][vV][iI]|[mM][oO][vV]|[mM]4[vV]|[gG][iI][fF])"'
                    ;;
                shader)
                    local -a shader_subcmds shader_presets
                    shader_subcmds=(
                        'list:List built-in procedural shader presets'
                        'preset:Run a built-in procedural shader preset'
                    )
                    shader_presets=(
                        'aurora:Northern lights atmospheric ribbon curtains'
                        'cyber_grid:Retro-futuristic neon horizon grid'
                        'fluid_ink:Organic procedural fluid ink swirls'
                        'synthwave_sun:Outrun retro-futuristic neon sun and horizon grid'
                        'starfield:Parallax 3D warp-speed starfield'
                        'rain_glass:Bokeh raindrops refracting on window glass'
                        'audio_bars:64-bin Bark FFT audio spectrum visualizer'
                        'audio_ring:Polar circular audio spectrum with reactive bass core'
                        'crt_retro:Barrel-curved CRT phosphor terminal with scanlines'
                        'plasma_orb:Raymarched volumetric energy orb with plasma filaments'
                        'lava_lamp:Alias for plasma_orb'
                        'northern_lights:Alias for aurora'
                        'outrun:Alias for synthwave_sun'
                        'crt:Alias for crt_retro'
                    )
                    if (( CURRENT == 2 )); then
                        _describe -t shader_subcmds 'shader subcommands' shader_subcmds
                        _describe -t shader_presets 'built-in shader presets' shader_presets
                        _files -g "*.([fF][rR][aA][gG]|[cC][oO][mM][pP]|[gG][lL][sS][lL]|[sS][hH][aA][dD][eE][rR])"
                    elif [[ $words[2] == "preset" || $words[2] == "--preset" ]] && (( CURRENT == 3 )); then
                        _describe -t shader_presets 'built-in shader presets' shader_presets
                    else
                        _arguments \
                            $global_opts \
                            '--list[List built-in procedural shader presets]' \
                            '--preset[Run a built-in procedural shader preset]:preset:(aurora cyber_grid fluid_ink synthwave_sun starfield rain_glass audio_bars audio_ring crt_retro plasma_orb lava_lamp northern_lights outrun crt)' \
                            '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:' \
                            '--audio[Enable PipeWire 64-bin Bark FFT audio uniforms]' \
                            '--volume[Wallpaper audio volume (0.0..1.0)]:volume:' \
                            '--mute-on-audio[Auto-duck volume when another app plays audio]' \
                            '--no-mute-on-audio[Disable automatic audio ducking]' \
                            '--fps[Target shader frame rate]:fps:' \
                            '--speed[Time speed multiplier]:speed:' \
                            '--10bit[Request 10-bit wide-gamut scanout]' \
                            '--backdrop[Secondary overview backdrop surface mode]:backdrop:(none auto-blur)' \
                            '--theme-from-media[Extract Material You palette dynamically from MPRIS2 album art]' \
                            '--no-theme-from-media[Disable MPRIS2 album art theme extraction]' \
                            '*:shader file:_files -g "*.([fF][rR][aA][gG]|[cC][oO][mM][pP]|[gG][lL][sS][lL]|[sS][hH][aA][dD][eE][rR])"'
                    fi
                    ;;
                workshop)
                    local -a workshop_cmds
                    workshop_cmds=(
                        'list:List installed Steam Workshop (431960) wallpapers'
                        'info:Inspect Workshop project.json, scene.pkg, and user properties'
                        'apply:Render native Wallpaper Engine scene, video, or web wallpaper'
                        'sync-previews:Sync human-readable Workshop preview symlinks for external pickers'
                    )
                    if (( CURRENT == 2 )); then
                        _describe -t workshop_cmds 'workshop subcommands' workshop_cmds
                        _files -/
                    else
                        _arguments \
                            $global_opts \
                            '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:' \
                            '*'{-p,--prop}'[Override Workshop user property (key=value)]:prop:' \
                            '--audio[Enable PipeWire 64-bin Bark FFT audio reactivity]' \
                            '--volume[Wallpaper audio volume (0.0..1.0)]:volume:' \
                            '--mute-on-audio[Auto-duck volume when another app plays audio]' \
                            '--no-mute-on-audio[Disable automatic audio ducking]' \
                            '--fps[Target frame rate]:fps:' \
                            '--backdrop[Secondary overview backdrop surface mode]:backdrop:(none auto-blur)' \
                            '--theme[Extract Material You palette and sync caelestia/OSC]' \
                            '--no-theme[Disable automatic Material You palette extraction]' \
                            '--theme-mode[Target Material Design 3 polarity]:mode:(dark light auto)' \
                            '--dark[Force dark Material Design 3 theme mode]' \
                            '--light[Force light Material Design 3 theme mode]' \
                            '--auto-day-night[Automatically select dark/light mode from solar elevation or time]' \
                            '--no-auto-day-night[Disable automatic solar day/night theme polarity]' \
                            '--latitude[Observer latitude (-90..90) for astronomical solar elevation]:latitude:' \
                            '--longitude[Observer longitude (-180..180) for astronomical solar elevation]:longitude:' \
                            '--source-color-index[Select ranked K-Means++ seed cluster index (0..3)]:index:(0 1 2 3)' \
                            '--seed-color[Override palette extraction with explicit hex seed color]:hex_color:' \
                            '--theme-from-media[Extract Material You palette dynamically from MPRIS2 album art]' \
                            '--no-theme-from-media[Disable MPRIS2 album art theme extraction]' \
                            '*:workshop ID or directory:_files'
                    fi
                    ;;
                audio)
                    _arguments \
                        $global_opts \
                        '--volume[Wallpaper audio volume (0.0..1.0)]:volume:' \
                        '--mute-on-audio[Auto-duck volume when another app plays audio]' \
                        '--no-mute-on-audio[Disable automatic audio ducking]' \
                        '--theme-from-media[Extract Material You palette dynamically from MPRIS2 album art]' \
                        '--no-theme-from-media[Disable MPRIS2 album art theme extraction]'
                    ;;
                set-uniform|uniform)
                    _arguments \
                        $global_opts \
                        '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:' \
                        '1:uniform or property name:' \
                        '*:float values:'
                    ;;
                theme)
                    _arguments \
                        $global_opts \
                        '(-i --img --image)'{-i,--img,--image}'[Input image path for palette extraction]:image:_files -g "*.([pP][nN][gG]|[jJ][pP][gG]|[jJ][pP][eE][gG]|[wW][eE][bB][pP]|[aA][vV][iI][fF]|[hH][eE][iI][cC]|[hH][eE][iI][fF]|[jJ][xX][lL]|[sS][vV][gG]|[bB][mM][pP]|[qQ][oO][iI]|[pP][pP][mM]|[pP][gG][mM]|[pP][nN][mM])"' \
                        '(--mode --theme-mode)'{--mode,--theme-mode}'[Target Material Design 3 polarity]:mode:(dark light auto)' \
                        '--dark[Force dark Material Design 3 theme mode]' \
                        '--light[Force light Material Design 3 theme mode]' \
                        '--auto-day-night[Automatically select dark/light mode from solar elevation or time]' \
                        '--no-auto-day-night[Disable automatic solar day/night theme polarity]' \
                        '--latitude[Observer latitude (-90..90) for astronomical solar elevation]:latitude:' \
                        '--longitude[Observer longitude (-180..180) for astronomical solar elevation]:longitude:' \
                        '--source-color-index[Select ranked K-Means++ seed cluster index (0..3)]:index:(0 1 2 3)' \
                        '--seed-color[Override palette extraction with explicit hex seed color]:hex_color:' \
                        '--no-cache[Bypass palette disk cache]' \
                        '--theme-from-media[Extract Material You palette dynamically from MPRIS2 album art]' \
                        '--no-theme-from-media[Disable MPRIS2 album art theme extraction]' \
                        '1:image file:_files -g "*.([pP][nN][gG]|[jJ][pP][gG]|[jJ][pP][eE][gG]|[wW][eE][bB][pP]|[aA][vV][iI][fF]|[hH][eE][iI][cC]|[hH][eE][iI][fF]|[jJ][xX][lL]|[sS][vV][gG]|[bB][mM][pP]|[qQ][oO][iI]|[pP][pP][mM]|[pP][gG][mM]|[pP][nN][mM])"'
                    ;;
                slideshow)
                    local -a slideshow_cmds
                    slideshow_cmds=(
                        'start:Start wallpaper slideshow'
                        'stop:Stop active slideshow'
                        'pause:Pause slideshow timer'
                        'resume:Resume slideshow timer'
                        'toggle:Toggle slideshow pause/resume'
                        'next:Advance to next wallpaper'
                        'prev:Return to previous wallpaper'
                        'previous:Return to previous wallpaper'
                    )
                    if (( CURRENT == 2 )); then
                        _describe -t slideshow_cmds 'slideshow subcommands' slideshow_cmds
                        _files -/ -g "*.([xX][mM][lL])"
                    else
                        case $words[2] in
                            start|*)
                                _arguments \
                                    $global_opts \
                                    $trans_opts \
                                    '(-d --interval --duration)'{-d,--interval,--duration}'[Slideshow interval (seconds or 30m, 1h)]:interval:' \
                                    '(-s --shuffle --random)'{-s,--shuffle,--random}'[Shuffle wallpapers randomly]' \
                                    '(--sort --sorting)'{--sort,--sorting}'[Slideshow traversal sort order]:sort:(ascending descending name mtime random)' \
                                    '1:directory or XML schedule:_files -/ -g "*.([xX][mM][lL])"'
                                ;;
                        esac
                    fi
                    ;;
                color|clear)
                    _arguments \
                        $global_opts \
                        '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:' \
                        '1:color (HEX):'
                    ;;
                pause|resume|unpause|toggle|restore|next|prev|previous)
                    _arguments \
                        $global_opts \
                        '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:'
                    ;;
                *)
                    _arguments $global_opts
                    ;;
            esac
            ;;
    esac
}

_wwal "$@"
