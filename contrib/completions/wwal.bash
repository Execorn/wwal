# Bash completion for wwal (WayWal)

_wwal_completion() {
    local cur prev words cword
    _init_completion || return

    local commands="ping query clear img video pause unpause toggle kill help"
    local global_opts="-n --namespace -v --verbose -h --help"
    local trans_types="none simple fade wipe grow outer wave noise"

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
        --transition-duration|--transition-fps|--transition-angle|--transition-wave|--transition-pos|--loop|--speed|-n|--namespace)
            return 0
            ;;
    esac

    case "$cmd" in
        img)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--transition-type --transition-duration --transition-fps --transition-angle --transition-wave --transition-pos ${global_opts}" -- "$cur") )
            else
                _filedir '@(png|jpg|jpeg|webp|bmp|gif|pnm|tga)'
            fi
            ;;
        video)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "--loop --speed ${global_opts}" -- "$cur") )
            else
                _filedir '@(mp4|mkv|webm|avi|mov)'
            fi
            ;;
        clear)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=( $(compgen -W "${global_opts}" -- "$cur") )
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
