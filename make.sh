date >> make.log
KERNEL=kernel7
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KERNEL=kernel7 bcm2709_defconfig && make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KERNEL=kernel7 -j4 zImage modules dtbs && make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- INSTALL_MOD_PATH=modules/ modules_install
date >> make.log
echo >> make.log

