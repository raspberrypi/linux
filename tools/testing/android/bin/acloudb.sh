#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

# acloudb .sh is a handy tool dedicated to kernel users to create remote AVDs without having the
# whole AOSP source tree.

# Constants
DEFAULT_ASUITE_HOME="prebuilts/asuite"
DEFAULT_ACLOUD_HOME="$DEFAULT_ASUITE_HOME/acloud/linux-x86"
ACLOUD_BIN="$DEFAULT_ACLOUD_HOME/acloud"
OPT_SKIP_PRERUNCHECK='--skip-pre-run-check'
OPT_DEFAULT_BRANCH=" --branch aosp-main"
# Color constants
BOLD="$(tput bold)"
END="$(tput sgr0)"
GREEN="$(tput setaf 2)"
RED="$(tput setaf 198)"

function adb_checker() {
    [[ "$(uname)" != "Linux" ]] && return
    if ! which adb &> /dev/null; then
        echo -e "\n${RED}Adb not found!${END}"
    fi
}

function main() {
    adb_checker
    EXTRA_OPTIONS=()
    if [[ "$1" == "create" ]]; then
        EXTRA_OPTIONS+=$OPT_SKIP_PRERUNCHECK
        # Add in branch if not specified
        ADD_BRANCH=true
        for i in "$@"; do
            [[ $i == "--branch" ]] && ADD_BRANCH=false
        done
        if $ADD_BRANCH; then
            EXTRA_OPTIONS+=$OPT_DEFAULT_BRANCH
        fi
    fi
    eval "$ACLOUD_BIN" "$@" "${EXTRA_OPTIONS[@]}"
}

main "$@"
