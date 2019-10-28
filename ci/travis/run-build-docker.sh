#!/bin/bash
set -e

. ./ci/travis/lib.sh

# Env-vars to pass to the docker image, should they be defined
ENV_VARS="BUILD_TYPE DEFCONFIG ARCH CROSS_COMPILE DTS_FILES IMAGE"

if [ "$DO_NOT_DOCKERIZE" = "1" ] ; then
	. ./ci/travis/run-build.sh
else
	TRAVIS_BUILD_DIR="${TRAVIS_BUILD_DIR:-.}"

	# cd to docker build dir if it exists
	if [ -d /docker_build_dir ] ; then
		cd /docker_build_dir
		TRAVIS_BUILD_DIR="/docker_build_dir"
	fi

	cat /dev/null > "${TRAVIS_BUILD_DIR}/env"
	BUILD_TYPE=${BUILD_TYPE:-default}
	for env in $ENV_VARS ; do
		val="$(eval echo "\$${env}")"
		if [ -n "$val" ] ; then
			echo "export ${env}=${val}" >> "${TRAVIS_BUILD_DIR}/env"
		fi
	done
	prepare_docker_image "ubuntu:rolling"
	run_docker_script "./ci/travis/run-build.sh" "ubuntu:rolling"
fi
