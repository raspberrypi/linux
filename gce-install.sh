#!/bin/bash
# Build a Linux kernel, install it on a remote machine, and reboot the machine.
# This script is only known to work for Debian/Ubuntu-based GCE VMs.

set -e

usage() {
	echo "gce-install.sh -m <MACHINE_IP>"
}

MACHINE=""
VERBOSE=""

while getopts "h?vm:p:z:" opt; do
	case "${opt}" in
		h|\?)
			usage
			exit 0
			;;
		v)
			VERBOSE="set -x"
			;;
		m)
			MACHINE=${OPTARG}
			;;
	esac
done

if [ -z ${MACHINE} ]; then
	usage
	exit -1
fi

umask 022

${VERBOSE}

BRANCH=`git rev-parse --abbrev-ref HEAD | sed s/-/+/g`
SHA1=`git rev-parse --short HEAD`
LOCALVERSION=+${BRANCH}+${SHA1}+GCE
GCE_PKG_DIR=${PWD}/gce/${LOCALVERSION}/pkg
GCE_INSTALL_DIR=${PWD}/gce/${LOCALVERSION}/install
GCE_BUILD_DIR=${PWD}/gce/${LOCALVERSION}/build
KERNEL_PKG=kernel-${LOCALVERSION}.tar.gz2
MAKE_OPTS="-j`nproc` \
           LOCALVERSION=${LOCALVERSION} \
           EXTRAVERSION="" \
           INSTALL_PATH=${GCE_INSTALL_DIR}/boot \
           INSTALL_MOD_PATH=${GCE_INSTALL_DIR}"

echo "cleaning..."
mkdir -p ${GCE_BUILD_DIR}
mkdir -p ${GCE_INSTALL_DIR}/boot
mkdir -p ${GCE_PKG_DIR}

set +e
echo "copying config.gce to .config ..."
cp config.gce .config
echo "running make olddefconfig ..."
make olddefconfig               > /tmp/make.olddefconfig
make ${MAKE_OPTS} prepare         > /tmp/make.prepare
echo "making..."
make ${MAKE_OPTS}                 > /tmp/make.default
echo "making modules ..."
make ${MAKE_OPTS} modules         > /tmp/make.modules
echo "making install ..."
make ${MAKE_OPTS} install         > /tmp/make.install
echo "making modules_install ..."
make ${MAKE_OPTS} modules_install > /tmp/make.modules_install
set -e

echo "making tarball ..."
(cd ${GCE_INSTALL_DIR}; tar -cvzf ${GCE_PKG_DIR}/${KERNEL_PKG}  boot/* lib/modules/* --owner=0 --group=0  > /tmp/make.tarball)

echo "running: scp $GCE_PKG_DIR/$KERNEL_PKG ${MACHINE}:~/"
scp ${GCE_PKG_DIR}/${KERNEL_PKG} ${MACHINE}:~/

ssh ${MACHINE} "
$VERBOSE
sudo rm -rf /boot/*GCE /lib/modules/*GCE
sudo tar --no-same-owner -xzvf ${KERNEL_PKG} -C / > /tmp/tar.out.txt
cd /boot
for v in \$(ls vmlinuz-* | sed s/vmlinuz-//g); do
	sudo mkinitramfs -k -o initrd.img-\${v} \${v}
done
sudo update-grub
sudo reboot
"
umask 027
