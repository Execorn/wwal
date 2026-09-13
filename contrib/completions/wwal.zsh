#compdef wwal

_wwal() {
    local -a commands
    commands=(
        'ping:Check if wwald daemon is running and responsive'
        'query:List all detected Wayland outputs and current geometry'
        'clear:Clear wallpaper to solid hex color (e.g. 000000 or 00ff00)'
        'img:Load and set wallpaper with GPU/SIMD transitions'
        'video:Load and play hardware-accelerated video wallpaper (VA-API)'
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
                        '--transition-type[Transition type]:type:(none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl)' \
                        '--transition-duration[Transition duration in seconds]:duration:' \
                        '--transition-fps[Target frame rate]:fps:' \
                        '--transition-angle[Transition angle in degrees]:angle:' \
                        '--transition-wave[Wave frequency and amplitude]:freq_amp:' \
                        '--transition-pos[Transition center position X,Y]:pos:' \
                        '1:image file:_files -g "*.([pP][nN][gG]|[jJ][pP][gG]|[jJ][pP][eE][gG]|[wW][eE][bB][pP]|[bB][mM][pP])"'
                    ;;
                video)
                    _arguments \
                        $global_opts \
                        '--loop[Loop count (0 = infinite)]:count:' \
                        '--speed[Playback speed multiplier]:speed:' \
                        '1:video file:_files -g "*.([mM][pP]4|[wW][eE][bB][mM]|[mM][kK][vV])"'
                    ;;
                clear)
                    _arguments \
                        $global_opts \
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
