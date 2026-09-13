#compdef wwal

_wwal() {
    local -a commands
    commands=(
        'ping:Check if wwald daemon is running and responsive'
        'query:List all detected Wayland outputs and current geometry'
        'clear:Clear wallpaper to solid hex color (e.g. 000000 or 00ff00)'
        'img:Load and set wallpaper with GPU/SIMD transitions'
        'video:Load and play hardware-accelerated video wallpaper (VA-API)'
        'slideshow:Manage daemon-native wallpaper slideshow'
        'pause:Pause video playback'
        'unpause:Resume video playback'
        'toggle:Toggle video playback pause/resume'
        'kill:Gracefully terminate the running wwald daemon'
        'help:Display help message'
    )

    local -a global_opts
    global_opts=(
        '(-n --namespace)'{-n,--namespace}'[Socket namespace]:namespace:'
        '(-v --verbose)'{-v,--verbose}'[Enable verbose output]'
        '(-h --help)'{-h,--help}'[Print help information]'
    )

    local -a trans_opts
    trans_opts=(
        '--transition-type[Transition type]:type:(none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl custom)'
        '--transition-duration[Transition duration in seconds]:duration:'
        '--transition-fps[Target frame rate]:fps:'
        '--transition-angle[Transition angle in degrees]:angle:'
        '--transition-wave[Wave frequency and amplitude]:freq_amp:'
        '--transition-pos[Transition center position X,Y or semantic alias]:pos:(center top bottom left right top-left top-right bottom-left bottom-right cursor mouse)'
        '--transition-shader[Custom GLSL compute shader path]:shader:_files -g "*.([cC][oO][mM][pP]|[gG][lL][sS][lL])"'
        '(-o --output)'{-o,--output}'[Target monitor name]:output:'
        '--sync-mode[Multi-monitor transition sync mode]:mode:(simultaneous staggered)'
        '--stagger-delay[Delay between monitors in ms]:delay:'
        '--10bit[Force 10-bit wide gamut scanout]'
        '(--scaling-mode --mode)'{--scaling-mode,--mode}'[Aspect ratio scaling mode]:mode:(fill fit stretch center tile crop cover contain)'
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
                        '1:image file:_files -g "*.([pP][nN][gG]|[jJ][pP][gG]|[jJ][pP][eE][gG]|[wW][eE][bB][pP]|[bB][mM][pP])"'
                    ;;
                video)
                    _arguments \
                        $global_opts \
                        '(-o --output)'{-o,--output}'[Target monitor name]:output:' \
                        '--loop[Loop count (0 = infinite)]:count:' \
                        '--speed[Playback speed multiplier]:speed:' \
                        '1:video file:_files -g "*.([mM][pP]4|[wW][eE][bB][mM]|[mM][kK][vV])"'
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
                    else
                        case $words[2] in
                            start)
                                _arguments \
                                    $global_opts \
                                    $trans_opts \
                                    '(-d --interval)'{-d,--interval}'[Slideshow interval in seconds]:interval:' \
                                    '(-s --shuffle)'{-s,--shuffle}'[Shuffle images randomly]' \
                                    '1:directory:_files -/'
                                ;;
                            *)
                                _arguments $global_opts
                                ;;
                        esac
                    fi
                    ;;
                clear)
                    _arguments \
                        $global_opts \
                        '(-o --output)'{-o,--output}'[Target monitor name]:output:' \
                        '1:color (HEX):'
                    ;;
                *)
                    _arguments $global_opts
                    ;;
            esac
            ;;
    esac
}

_wwal "$@"
