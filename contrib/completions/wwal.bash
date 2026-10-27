# SPDX-License-Identifier: GPL-3.0-or-later
_wwal_completion() {
    local cur prev words cword
    _init_completion || return

    local commands="img video shader workshop slideshow color clear theme audio set-uniform status subscribe pause resume unpause toggle query stats restore clear-cache ping kill help"
    local global_opts="-j --json -n --namespace -v --verbose -h --help --layer --battery-policy"
    local trans_types="none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl custom random"
    local trans_positions="center top bottom left right top-left top-right bottom-left bottom-right cursor mouse"
    local sync_modes="simultaneous staggered"
    local scaling_modes="fill fit stretch center tile span crop cover contain no"
    local filters="Nearest Bilinear CatmullRom Mitchell Lanczos3"
    local layers="background bottom"
    local battery_policies="ignore reduce-fps pause-video"
    local backdrops="none auto-blur"
    local slideshow_actions="start stop pause resume toggle next prev"
    local workshop_actions="list info apply"

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
        --scaling-mode|--mode|--resize)
            COMPREPLY=( $(compgen -W "${scaling_modes}" -- "$cur") )
            return 0
            ;;
        --filter)
            COMPREPLY=( $(compgen -W "${filters}" -- "$cur") )
            return 0
            ;;
        --layer)
            COMPREPLY=( $(compgen -W "${layers}" -- "$cur") )
            return 0
            ;;
        --battery-policy)
            COMPREPLY=( $(compgen -W "${battery_policies}" -- "$cur") )
            return 0
            ;;
        --backdrop)
            COMPREPLY=( $(compgen -W "${backdrops}" -- "$cur") )
            _filedir '@(png|jpg|jpeg|webp|avif|heic|jxl|svg|bmp)'
            return 0
            ;;
        --transition-shader)
            _filedir '@(comp|glsl|frag)'
            return 0
            ;;
        --depth-map)
            _filedir '@(png|jpg|jpeg|webp|avif|heic|jxl|bmp|pnm|tga)'
            return 0
            ;;
        --transition-duration|--transition-fps|--transition-angle|--transition-wave|--transition-bezier|--transition-step|--stagger-delay|--fill-color|--depth-strength|-p|--prop|--volume|--fps|--interval|-d|--loop|--speed|-n|--namespace|-o|--output|--outputs)
            return 0
            ;;
    esac

    local trans_opts="--transition-type --transition-duration --transition-fps --transition-angle --transition-wave --transition-pos --transition-bezier --transition-step --transition-shader -o --output --outputs --sync-mode --stagger-delay --10bit --scaling-mode --mode --resize --fill-color --filter --backdrop --depth-map --depth-strength --layer --battery-policy --theme --no-theme --theme-from-media --no-theme-from-media --animate"

    case "$cmd" in
        img)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "${trans_opts} ${global_opts}" -- "$cur") )
            else
                _filedir '@(png|jpg|jpeg|webp|avif|heic|jxl|svg|bmp|gif|pnm|tga)'
            fi
            ;;
        video)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--loop --speed --audio --volume --mute-on-audio --no-mute-on-audio --battery-policy --layer --backdrop --theme --no-theme --theme-from-media --no-theme-from-media -o --output --outputs ${global_opts}" -- "$cur") )
            else
                _filedir '@(mp4|mkv|webm|avi|mov|gif)'
            fi
            ;;
        shader)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--audio --volume --mute-on-audio --no-mute-on-audio --speed --fps --battery-policy --layer --backdrop --10bit --theme-from-media --no-theme-from-media -o --output --outputs ${global_opts}" -- "$cur") )
            else
                _filedir '@(frag|comp|glsl)'
            fi
            ;;
        workshop)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "-p --prop --audio --volume --mute-on-audio --no-mute-on-audio --fps --battery-policy --layer --backdrop --theme --no-theme --theme-from-media --no-theme-from-media -o --output --outputs ${global_opts}" -- "$cur") )
            else
                COMPREPLY=( $(compgen -W "${workshop_actions}" -- "$cur") )
                _filedir
            fi
            ;;
        audio)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--volume --mute-on-audio --no-mute-on-audio --theme-from-media --no-theme-from-media ${global_opts}" -- "$cur") )
            fi
            ;;
        theme)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--theme-from-media --no-theme-from-media ${global_opts}" -- "$cur") )
            else
                _filedir '@(png|jpg|jpeg|webp|avif|heic|jxl|svg|bmp)'
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
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "-d --interval -s --shuffle --random ${trans_opts} ${global_opts}" -- "$cur") )
                else
                    COMPREPLY=( $(compgen -W "${slideshow_actions}" -- "$cur") )
                    _filedir -d
                fi
            elif [[ "$subcmd" == "start" ]]; then
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "-d --interval -s --shuffle --random ${trans_opts} ${global_opts}" -- "$cur") )
                else
                    _filedir -d
                fi
            else
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "${global_opts}" -- "$cur") )
                fi
            fi
            ;;
        color|clear)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "-o --output --outputs ${global_opts}" -- "$cur") )
            fi
            ;;
        pause|resume|unpause|toggle|restore|set-uniform)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "-o --output --outputs ${global_opts}" -- "$cur") )
            fi
            ;;
        *)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "${global_opts}" -- "$cur") )
            fi
            ;;
    esac
}

complete -F _wwal_completion wwal swww awww
