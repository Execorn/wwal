# Bash completion for wwal (WayWal)

_wwal_completion() {
    local cur prev words cword
    _init_completion || return

    local commands="ping query clear img video slideshow pause unpause toggle kill help"
    local global_opts="-n --namespace -v --verbose -h --help"
    local trans_types="none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl custom"
    local trans_positions="center top bottom left right top-left top-right bottom-left bottom-right cursor mouse"
    local sync_modes="simultaneous staggered"
    local slideshow_actions="start stop pause resume toggle next prev"

    # Find the subcommand if one was already given
    local cmd=""
    for ((i = 1; i < cword; i++)); do
        if [[ "${commands}" =~ (^|[[:space:]])"${words[i]}"($|[[:space:]]) ]]; then
            cmd="${words[i]}"
            break
        fi
    done

    if [[ -z "$cmd" ]]; then
        if [[ "$cur" == -* ]]; then
            COMPREPLY=( $(compgen -W "${global_opts}" -- "$cur") )
        else
            COMPREPLY=( $(compgen -W "${commands}" -- "$cur") )
        fi
        return 0
    fi

    case "$prev" in
        --transition-type)
            COMPREPLY=( $(compgen -W "${trans_types}" -- "$cur") )
            return 0
            ;;
        --transition-pos)
            COMPREPLY=( $(compgen -W "${trans_positions}" -- "$cur") )
            return 0
            ;;
        --sync-mode)
            COMPREPLY=( $(compgen -W "${sync_modes}" -- "$cur") )
            return 0
            ;;
        --transition-shader)
            _filedir '@(comp|glsl)'
            return 0
            ;;
        --transition-duration|--transition-fps|--transition-angle|--transition-wave|--stagger-delay|--interval|--loop|--speed|-n|--namespace|-o|--output)
            return 0
            ;;
    esac

    local trans_opts="--transition-type --transition-duration --transition-fps --transition-angle --transition-wave --transition-pos --transition-shader -o --output --sync-mode --stagger-delay --10bit"

    case "$cmd" in
        img)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "${trans_opts} ${global_opts}" -- "$cur") )
            else
                _filedir '@(png|jpg|jpeg|webp|bmp|gif|pnm|tga)'
            fi
            ;;
        video)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--loop --speed -o --output ${global_opts}" -- "$cur") )
            else
                _filedir '@(mp4|mkv|webm|avi|mov)'
            fi
            ;;
        slideshow)
            local subcmd=""
            for ((i = 2; i < cword; i++)); do
                if [[ "${slideshow_actions}" =~ (^|[[:space:]])"${words[i]}"($|[[:space:]]) ]]; then
                    subcmd="${words[i]}"
                    break
                fi
            done
            if [[ -z "$subcmd" ]]; then
                COMPREPLY=( $(compgen -W "${slideshow_actions}" -- "$cur") )
            elif [[ "$subcmd" == "start" ]]; then
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "--interval --shuffle ${trans_opts} ${global_opts}" -- "$cur") )
                else
                    _filedir -d
                fi
            else
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "${global_opts}" -- "$cur") )
                fi
            fi
            ;;
        clear)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "-o --output ${global_opts}" -- "$cur") )
            fi
            ;;
        *)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "${global_opts}" -- "$cur") )
            fi
            ;;
    esac
}

complete -F _wwal_completion wwal
