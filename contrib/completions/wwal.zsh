# SPDX-License-Identifier: GPL-3.0-or-later
#compdef wwal swww awww

_wwal() {
    local -a commands
    commands=(
        'img:Decode and set wallpaper with GPU compute or SIMD transitions'
        'video:Play hardware-accelerated video wallpaper via VA-API'
        'shader:Run live Shadertoy fragment or GLES 3.1 compute shader wallpaper'
        'slideshow:Manage daemon-native timerfd wallpaper slideshow'
        'color:Set wallpaper to solid hex color via wp_single_pixel_buffer_v1'
        'clear:Clear wallpaper to solid hex color (swww/awww alias)'
        'theme:Extract OKLab/CAM16/HCT Material Design 3 palette and sync caelestia/OSC'
        'pause:Pause video, animation, or live shader playback'
        'resume:Resume paused video, animation, or live shader playback'
        'unpause:Resume paused video, animation, or live shader playback'
        'toggle:Toggle video, animation, or live shader playback pause/resume'
        'query:List connected Wayland outputs, scale factors, and geometry'
        'stats:Report real-time RSS/PSS, DMA-BUF modifiers, VBlank PLL, and pause_mask telemetry'
        'restore:Restore last persisted per-output state from state.bin'
        'clear-cache:Remove cached hardware transcodes and palette files'
        'ping:Check if wwald daemon is running and responsive'
        'kill:Gracefully terminate the running wwald daemon'
        'help:Display help message'
    )

    local -a global_opts
    global_opts=(
        '(-n --namespace)'{-n,--namespace}'[Socket and layer-shell namespace]:namespace:'
        '--layer[Wayland layer-shell layer]:layer:(background bottom)'
        '--battery-policy[Laptop battery power governor policy]:policy:(ignore reduce-fps pause-video)'
        '(-v --verbose)'{-v,--verbose}'[Enable verbose output]'
        '(-h --help)'{-h,--help}'[Print help information]'
    )

    local -a trans_opts
    trans_opts=(
        '--transition-type[Transition effect]:type:(none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl custom random)'
        '--transition-duration[Transition duration in seconds]:duration:'
        '--transition-fps[Target frame rate]:fps:'
        '--transition-bezier[Cubic Bezier control points X1,Y1,X2,Y2]:bezier:'
        '--transition-angle[Transition angle in degrees]:angle:'
        '--transition-wave[Wave frequency and amplitude]:freq_amp:'
        '--transition-pos[Transition anchor position X,Y or semantic alias]:pos:(center top bottom left right top-left top-right bottom-left bottom-right cursor mouse)'
        '--transition-step[Per-frame transition step (1..255)]:step:'
        '--transition-shader[Custom GLSL compute shader path]:shader:_files -g "*.([cC][oO][mM][pP]|[gG][lL][sS][lL]|[fF][rR][aA][gG])"'
        '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:'
        '--sync-mode[Multi-monitor transition sync mode]:mode:(simultaneous staggered)'
        '--stagger-delay[Delay between monitors in ms]:delay:'
        '--10bit[Request 10-bit wide-gamut scanout (XRGB2101010)]'
        '(--scaling-mode --mode --resize)'{--scaling-mode,--mode,--resize}'[Aspect-ratio scaling mode]:mode:(fill fit stretch center tile crop cover contain no)'
        '--fill-color[Padding hex color for fit/center modes]:hex_color:'
        '--filter[Scaling filter hint]:filter:(Nearest Bilinear CatmullRom Mitchell Lanczos3)'
        '--backdrop[Secondary overview backdrop surface mode]:backdrop:(none auto-blur)'
        '--theme[Extract Material You palette and sync caelestia/OSC]'
        '--no-theme[Disable automatic Material You palette extraction]'
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
                        '1:image file:_files -g "*.([pP][nN][gG]|[jJ][pP][gG]|[jJ][pP][eE][gG]|[wW][eE][bB][pP]|[aA][vV][iI][fF]|[hH][eE][iI][cC]|[jJ][xX][lL]|[sS][vV][gG]|[bB][mM][pP]|[gG][iI][fF])"'
                    ;;
                video)
                    _arguments \
                        $global_opts \
                        '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:' \
                        '--loop[Loop count (0 = infinite)]:count:' \
                        '--speed[Playback speed multiplier]:speed:' \
                        '--audio[Enable PipeWire audio capture]' \
                        '--backdrop[Secondary overview backdrop surface mode]:backdrop:(none auto-blur)' \
                        '--theme[Extract Material You palette and sync caelestia/OSC]' \
                        '--no-theme[Disable automatic Material You palette extraction]' \
                        '1:video file:_files -g "*.([mM][pP]4|[wW][eE][bB][mM]|[mM][kK][vV]|[aA][vV][iI]|[mM][oO][vV]|[gG][iI][fF])"'
                    ;;
                shader)
                    _arguments \
                        $global_opts \
                        '*'{-o,--output,--outputs}'[Target monitor name(s)]:output:' \
                        '--audio[Enable PipeWire 64-bin Bark FFT audio uniforms]' \
                        '--speed[Time speed multiplier]:speed:' \
                        '--10bit[Request 10-bit wide-gamut scanout]' \
                        '--backdrop[Secondary overview backdrop surface mode]:backdrop:(none auto-blur)' \
                        '1:shader file:_files -g "*.([fF][rR][aA][gG]|[cC][oO][mM][pP]|[gG][lL][sS][lL])"'
                    ;;
                theme)
                    _arguments \
                        $global_opts \
                        '1:image file:_files -g "*.([pP][nN][gG]|[jJ][pP][gG]|[jJ][pP][eE][gG]|[wW][eE][bB][pP]|[aA][vV][iI][fF]|[hH][eE][iI][cC]|[jJ][xX][lL]|[sS][vV][gG]|[bB][mM][pP])"'
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
                    )
                    if (( CURRENT == 2 )); then
                        _describe -t slideshow_cmds 'slideshow subcommands' slideshow_cmds
                        _files -/
                    else
                        case $words[2] in
                            start|*)
                                _arguments \
                                    $global_opts \
                                    $trans_opts \
                                    '(-d --interval)'{-d,--interval}'[Slideshow interval in seconds]:interval:' \
                                    '(-s --shuffle --random)'{-s,--shuffle,--random}'[Shuffle images randomly]' \
                                    '1:directory:_files -/'
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
                pause|resume|unpause|toggle|restore)
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
