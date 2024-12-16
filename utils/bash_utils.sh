#!/bin/bash

# Color strings {{{
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
ENDC='\033[0m'
# }}}

printerr() {
    printf "$@" >&2
}

dump_stack() {
    if [ "$#" -eq 1 ]; then
        local i="$1"
    else
        local i=0
    fi
    local line_no function_name file_name
    printf "Traceback (most recent call last):\n"
    while caller "$i"; do 
        ((i++))
    done | while read -r line_no function_name file_name; do
        printf "  File \"%s\", line %s, in %s\n" "$file_name" "$line_no" "$function_name"
        printf "    %s\n" "$(sed "${line_no}q;d" "$file_name" | sed "s/^\s*//g")"
    done
}

assert() {
    __assert 1 0 "$@"
}

assert_zero() {
    __assert 1 1 "$@"
}

__assert() {
    # $1 dump stack level
    # $2 equal to zero?
    # $3 value to be judge on [optional]
    # $4 message [optional]
    # return: 0 on success, 1 otherwise 
    local err_str="Assertion Failed"
    if [ "$#" -eq 0 ]; then
        printf "__assert() internal error\n"
        exit 255
    elif [ "$#" -eq 1 ] || [ "$#" -eq 2 ]; then
        # assert with no args
        :
    elif [ "$2" -ne 0 ] && [ "$3" -ne 0 ] || [ "$2" -eq 0 ] && [ "$3" -eq 0 ]; then
        if [ "$#" -ge 4 ]; then
            err_str="$err_str: $4"
        fi
    else
        return 0
    fi
    printf "$err_str" "${@:5}"
    dump_stack $(( 1 + "$1" ))
    return 1
}

exit_on_retval() {
    local retval=$?
    if [ "$retval" -ne 0 ]; then exit "$retval"; fi
    return $retval
}

assert_exit() {
    __assert 1 0 "$@"
    exit_on_retval
}

assert_zero_exit() {
    __assert 1 1 "$@"
    exit_on_retval
}

pretty_countdown() {
    [ "$#" -ge 2 ]
    assert_zero "$?"

    local from=$2
    local interval=1
    for wait_time in $(seq 1 "$from"); do
        printf "\r\033[2K$1" "$(( "$from" + 1 - "$wait_time" ))"
        sleep 1
    done
    printf "\n"
}

display_time() {
    printf "### Current time BEGIN ###########################################\n"
    timedatectl | sed -e "s/^/# /"
    printf "### Current time END #############################################\n"
}
