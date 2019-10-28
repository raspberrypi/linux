#!/bin/sh -e

echo_red()   { printf "\033[1;31m$*\033[m\n"; }
echo_green() { printf "\033[1;32m$*\033[m\n"; }
echo_blue()  { printf "\033[1;34m$*\033[m\n"; }

command_exists() {
	local cmd=$1
	[ -n "$cmd" ] || return 1
	type "$cmd" >/dev/null 2>&1
}

prepare_docker_image() {
	local DOCKER_IMAGE="$1"
	sudo apt-get -qq update
	echo 'DOCKER_OPTS="-H tcp://127.0.0.1:2375 -H unix:///var/run/docker.sock -s devicemapper"' | sudo tee /etc/default/docker > /dev/null
	sudo service docker restart
	sudo docker pull "$DOCKER_IMAGE"
}

run_docker_script() {
	local DOCKER_SCRIPT="$1"
	local DOCKER_IMAGE="$2"
	local OS_TYPE="$3"
	local MOUNTPOINT="${4:-docker_build_dir}"
	sudo docker run --rm=true \
		-v "$(pwd):/${MOUNTPOINT}:rw" \
		$DOCKER_IMAGE \
		/bin/bash -e "/${MOUNTPOINT}/${DOCKER_SCRIPT}" "${MOUNTPOINT}" "${OS_TYPE}"
}

ensure_command_exists() {
	local cmd="$1"
	local package="$2"
	[ -n "$cmd" ] || return 1
	[ -n "$package" ] || package="$cmd"
	! command_exists "$cmd" || return 0
	# go through known package managers
	for pacman in apt-get brew yum ; do
		command_exists $pacman || continue
		$pacman install -y $package || {
			# Try an update if install doesn't work the first time
			$pacman -y update && \
				$pacman install -y $package
		}
		return $?
	done
	return 1
}

ensure_command_exists sudo
