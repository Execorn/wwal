# SPDX-License-Identifier: GPL-3.0-or-later
_wwal_completion() {
    local cur prev words cword
    _init_completion || return

    local commands="img pick video shader workshop slideshow next prev previous color clear theme audio set-uniform uniform status subscribe pause resume unpause toggle query stats restore clear-cache init daemon ping kill version help"
    local global_opts="-j --json -n --namespace -a --all -q --quiet -d --daemon --no-daemon -f --format -v --verbose -V --version -h --help --layer --battery-policy"
    local trans_types="none simple fade wipe grow outer wave noise crosszoom slide glitch burn ripple pixelate doom swirl cube luma light_leak page_curl custom random left right top bottom center any"
    local trans_positions="center top bottom left right top-left top-right bottom-left bottom-right tl tr bl br topleft topright bottomleft bottomright cursor mouse"
    local sync_modes="simultaneous staggered"
    local scaling_modes="fill fit fit-border-color stretch center tile span crop cover contain no"
    local filters="Nearest Bilinear CatmullRom Mitchell Lanczos3"
    local formats="xrgb xbgr rgb bgr"
    local layers="background bottom"
    local battery_policies="ignore reduce-fps pause-video"
    local backdrops="none auto-blur"
    local slideshow_actions="start stop pause resume toggle next prev previous"
    local workshop_actions="list info apply sync-previews"
    local pick_menus="auto rofi fuzzel walker dmenu stdout json"
    local crop_gravities="center top bottom left right top-left top-right bottom-left bottom-right tl tr bl br north south east west nw ne sw se"
    local sort_modes="ascending descending name mtime random"
    local shader_presets="aurora cyber_grid fluid_ink synthwave_sun starfield rain_glass audio_bars audio_ring crt_retro plasma_orb lava_lamp northern_lights outrun crt"
    local theme_modes="dark light auto"

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
        -t|--transition-type)
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
        -f|--filter)
            COMPREPLY=( $(compgen -W "${filters} ${formats}" -- "$cur") )
            return 0
            ;;
        --format)
            COMPREPLY=( $(compgen -W "${formats}" -- "$cur") )
            return 0
            ;;
        --crop-gravity)
            COMPREPLY=( $(compgen -W "${crop_gravities}" -- "$cur") )
            return 0
            ;;
        --menu)
            COMPREPLY=( $(compgen -W "${pick_menus}" -- "$cur") )
            return 0
            ;;
        --dir)
            _filedir -d
            return 0
            ;;
        --sort|--sorting)
            COMPREPLY=( $(compgen -W "${sort_modes}" -- "$cur") )
            return 0
            ;;
        --theme-mode)
            COMPREPLY=( $(compgen -W "${theme_modes}" -- "$cur") )
            return 0
            ;;
        --source-color-index)
            COMPREPLY=( $(compgen -W "0 1 2 3" -- "$cur") )
            return 0
            ;;
        preset|--preset)
            COMPREPLY=( $(compgen -W "${shader_presets}" -- "$cur") )
            return 0
            ;;
        -i|--img|--image)
            _filedir '@(png|jpg|jpeg|webp|avif|heic|heif|jxl|svg|bmp|qoi|ppm|pgm|pnm)'
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
            _filedir '@(png|jpg|jpeg|webp|avif|heic|jxl|svg|bmp|qoi|ppm|pgm|pnm)'
            return 0
            ;;
        --transition-shader)
            _filedir '@(comp|glsl|frag|shader)'
            return 0
            ;;
        --depth-map)
            _filedir '@(png|jpg|jpeg|webp|avif|heic|jxl|bmp|pnm|tga|qoi)'
            return 0
            ;;
        --transition-duration|--transition-fps|--transition-angle|--transition-wave|--transition-bezier|--transition-step|--stagger-delay|--fill-color|--depth-strength|--seed-color|--latitude|--longitude|-p|--prop|--volume|--fps|--interval|--duration|-d|--loop|--speed|-n|--namespace|-o|--output|--outputs)
            return 0
            ;;
    esac

    local trans_opts="-t --transition-type --transition-duration --transition-fps --transition-angle --transition-wave --transition-pos --invert-y --transition-bezier --transition-step --transition-shader -o --output --outputs --sync-mode --stagger-delay --10bit --scaling-mode --mode --resize --no-resize --crop-gravity --fill-color -f --filter --format --no-cache --clear-cache --backdrop --depth-map --depth-strength --layer --battery-policy --theme --no-theme --theme-mode --dark --light --auto-day-night --no-auto-day-night --latitude --longitude --source-color-index --seed-color --theme-from-media --no-theme-from-media --animate"

    case "$cmd" in
        img)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "${trans_opts} ${global_opts}" -- "$cur") )
            else
                _filedir '@(png|jpg|jpeg|webp|avif|heic|heif|jxl|svg|bmp|gif|pnm|ppm|pgm|tga|qoi|mp4|mkv|webm|avi|mov|m4v|frag|comp|glsl|shader|xml)'
            fi
            ;;
        pick)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--menu --dir ${trans_opts} ${global_opts}" -- "$cur") )
            else
                _filedir -d
            fi
            ;;
        video)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--loop --speed --audio --volume --mute-on-audio --no-mute-on-audio --crop-gravity --battery-policy --layer --backdrop --theme --no-theme --theme-mode --dark --light --auto-day-night --no-auto-day-night --latitude --longitude --source-color-index --seed-color --theme-from-media --no-theme-from-media -o --output --outputs ${global_opts}" -- "$cur") )
            else
                _filedir '@(mp4|mkv|webm|avi|mov|m4v|gif)'
            fi
            ;;
        shader)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--list --preset --audio --volume --mute-on-audio --no-mute-on-audio --speed --fps --battery-policy --layer --backdrop --10bit --theme-from-media --no-theme-from-media -o --output --outputs ${global_opts}" -- "$cur") )
            else
                COMPREPLY=( $(compgen -W "list preset ${shader_presets}" -- "$cur") )
                _filedir '@(frag|comp|glsl|shader)'
            fi
            ;;
        workshop)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "-p --prop --audio --volume --mute-on-audio --no-mute-on-audio --fps --battery-policy --layer --backdrop --theme --no-theme --theme-mode --dark --light --auto-day-night --no-auto-day-night --latitude --longitude --source-color-index --seed-color --theme-from-media --no-theme-from-media -o --output --outputs ${global_opts}" -- "$cur") )
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
                COMPREPLY=( $(compgen -W "-i --img --image --theme-mode --dark --light --mode --auto-day-night --no-auto-day-night --latitude --longitude --source-color-index --seed-color --no-cache --theme-from-media --no-theme-from-media ${global_opts}" -- "$cur") )
            else
                _filedir '@(png|jpg|jpeg|webp|avif|heic|heif|jxl|svg|bmp|qoi|ppm|pgm|pnm)'
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
                    COMPREPLY=( $(compgen -W "-d --interval --duration -s --shuffle --random --sort --sorting --recursive --no-recursive ${trans_opts} ${global_opts}" -- "$cur") )
                else
                    COMPREPLY=( $(compgen -W "${slideshow_actions}" -- "$cur") )
                    _filedir '@(xml)'
                    _filedir -d
                fi
            elif [[ "$subcmd" == "start" ]]; then
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "-d --interval --duration -s --shuffle --random --sort --sorting --recursive --no-recursive ${trans_opts} ${global_opts}" -- "$cur") )
                else
                    _filedir '@(xml)'
                    _filedir -d
                fi
            else
                if [[ "$cur" == -* ]]; then
                    COMPREPLY=( $(compgen -W "-o --output --outputs ${global_opts}" -- "$cur") )
                fi
            fi
            ;;
        color|clear)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "-o --output --outputs ${global_opts}" -- "$cur") )
            fi
            ;;
        pause|resume|unpause|toggle|restore|set-uniform|uniform|next|prev|previous)
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
