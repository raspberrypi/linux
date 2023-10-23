#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# A depmod wrapper

if test $# -ne 1 -a $# -ne 2; then
	echo "Usage: $0 <kernelrelease> [System.map folder]" >&2
	exit 1
fi

KERNELRELEASE=$1
KBUILD_MIXED_TREE=$2

: ${DEPMOD:=depmod}

if ! test -r ${KBUILD_MIXED_TREE}System.map ; then
	echo "Warning: modules_install: missing 'System.map' file. Skipping depmod." >&2
	exit 0
fi

# legacy behavior: "depmod" in /sbin, no /sbin in PATH
PATH="$PATH:/sbin"
if [ -z $(command -v $DEPMOD) ]; then
	echo "Warning: 'make modules_install' requires $DEPMOD. Please install it." >&2
	echo "This is probably in the kmod package." >&2
	exit 0
fi

set -- -ae -F ${KBUILD_MIXED_TREE}System.map
if test -n "$INSTALL_MOD_PATH"; then
	set -- "$@" -b "$INSTALL_MOD_PATH"
fi
exec "$DEPMOD" "$@" "$KERNELRELEASE"
