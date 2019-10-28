#!/bin/bash
set -e

TRAVIS_BUILD_DIR="${TRAVIS_BUILD_DIR:-.}"

# cd to docker build dir if it exists
if [ -d /docker_build_dir ] ; then
	cd /docker_build_dir
	TRAVIS_BUILD_DIR="/docker_build_dir"
fi

. ./ci/travis/lib.sh

if [ -f "${TRAVIS_BUILD_DIR}/env" ] ; then
	echo_blue "Loading environment variables"
	cat "${TRAVIS_BUILD_DIR}/env"
	. "${TRAVIS_BUILD_DIR}/env"
fi

KCFLAGS="-Werror"
export KCFLAGS

APT_LIST="build-essential bc u-boot-tools flex bison libssl-dev"

if [ "$ARCH" == "arm64" ] ; then
	APT_LIST="$APT_LIST gcc-aarch64-linux-gnu"
else
	APT_LIST="$APT_LIST gcc-arm-linux-gnueabihf"
fi

adjust_kcflags_against_gcc() {
	# FIXME: remove this function once kernel gets upgrade and
	#        these have been fixed upstream. Currently, these are
	#	 for kernel 4.19
	GCC="${CROSS_COMPILE}gcc"
	if [ "$($GCC -dumpversion | cut -d. -f1)" -ge "8" ]; then
		KCFLAGS="$KCFLAGS -Wno-error=stringop-truncation"
		KCFLAGS="$KCFLAGS -Wno-error=packed-not-aligned"
		KCFLAGS="$KCFLAGS -Wno-error=stringop-overflow= -Wno-error=sizeof-pointer-memaccess"
		KCFLAGS="$KCFLAGS -Wno-error=missing-attributes"
	fi

	if [ "$($GCC -dumpversion | cut -d. -f1)" -ge "9" ]; then
		KCFLAGS="$KCFLAGS -Wno-error=address-of-packed-member -Wno-error=stringop-truncation"
	fi
	export KCFLAGS
}

apt_update_install() {
	sudo -s <<-EOF
		apt-get -qq update
		apt-get -y install $@
	EOF
	adjust_kcflags_against_gcc
}

build_default() {
	apt_update_install $APT_LIST
	make ${DEFCONFIG}
	make -j`getconf _NPROCESSORS_ONLN`
}

ORIGIN=${ORIGIN:-origin}

BUILD_TYPE=${BUILD_TYPE:-${1}}
BUILD_TYPE=${BUILD_TYPE:-default}

build_${BUILD_TYPE}
