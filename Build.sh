function build_Script(){
bit=$(getconf LONG_BIT)

if [ "${bit}" = "32" ]; then
	echo PATH=\$PATH:~/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian/bin >> ~/.bashrc # 32
	echo "32 Bit Setting"
elif [ "${bit}" = "64" ]; then
	echo PATH=\$PATH:~/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64/bin >> ~/.bashrc # 64
	echo "64 Bit Setting"
fi

source ~/.bashrc

if [ $1 = "zero" ]; then
	KERNEL=kernel # pi zero
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcmrpi_defconfig # pi zero
	echo "Pi Zero Board Config"
elif [ $1 = "3" ]; then
	KERNEL=kernel7 # pi 3
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcm2709_defconfig # pi 3
	echo "Pi 3 Board Config"
elif [ $1 = "4" ]; then
	KERNEL=kernel7l # pi 4
	make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcm2711_defconfig # pi 4
	echo "Pi 4 Board Config"
fi

num=$(expr $(grep ^processor /proc/cpuinfo | wc -l) \* 2)
echo "calculate process number .."
make -j$num ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage modules dtbs

echo "make end - $(date)"

sudo env PATH=$PATH make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- INSTALL_MOD_PATH=/media/$USER/rootfs modules_install
sudo cp /media/$USER/boot/$KERNEL.img /media/$USER/boot/$KERNEL-backup.img
sudo cp arch/arm/boot/zImage /media/$USER/boot/$KERNEL.img
sudo cp arch/arm/boot/dts/*.dtb /media/$USER/boot
sudo cp arch/arm/boot/dts/overlays/*.dtb* /media/$USER/boot/overlays/
sudo cp arch/arm/boot/dts/overlays/README /media/$USER/boot/overlays/

echo "Finish - $(date)"
}

build_Script $1
