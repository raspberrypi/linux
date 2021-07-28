# usage $ ./Build.sh model(zero,3,4) environment(cross,compile)
function build_Script(){
	sudo apt-get update
	sudo apt-get install git bc bison flex libssl-dev make -y
	if [ $2 = "cross" ]; then
		bit=$(getconf LONG_BIT)

		if [ "${bit}" = "32" ]; then
			echo PATH=\$PATH:~/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian/bin >> ~/.bashrc # 32
			echo "32 Bit Setting"
		elif [ "${bit}" = "64" ]; then
			echo PATH=\$PATH:~/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64/bin >> ~/.bashrc # 64
			echo "64 Bit Setting"
		fi
		source ~/.bashrc
	fi

	if [ $1 = "zero" ]; then
		KERNEL=kernel # pi zero
		if [ $2 = "local" ]; then
			make bcmrpi_defconfig # Local
		elif [ $2 = "cross" ]; then
			make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcmrpi_defconfig # pi zero
		fi
		echo "Pi Zero Board Config"
	elif [ $1 = "3" ]; then
		KERNEL=kernel7 # pi 3
		if [ $2 = "local" ]; then
			make bcm2709_defconfig # Local
		elif [ $2 = "cross" ]; then
			make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcm2709_defconfig # pi 3
		fi
		echo "Pi 3 Board Config"
	elif [ $1 = "4" ]; then
		KERNEL=kernel7l # pi 4
		if [ $2 = "local" ]; then
			make bcm2711_defconfig # Local
		elif [ $2 = "cross" ]; then
			make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcm2711_defconfig # pi 4
		fi
		echo "Pi 4 Board Config"
	fi

	num=$(expr $(grep ^processor /proc/cpuinfo | wc -l) \* 2)
	echo "calculate process number .."
	if [ $2 = "cross" ]; then
		make -j$num ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage modules dtbs # cross
	elif [ $2 = "local" ]; then
		make -j$num zImage modules dtbs # Local
	fi
	echo "make end - $(date)"
	TargetPath="/media/$USER"
	if [ $2 = "cross" ]; then
		sudo env PATH=$PATH make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- INSTALL_MOD_PATH=/media/$USER/rootfs modules_install
		sudo cp /media/$USER/boot/$KERNEL.img /media/$USER/boot/$KERNEL-backup.img
		TargetPath="/media/$USER"
	elif [ $2 = "local" ]; then
		sudo make modules_install
		TargetPath=" "
	fi


	sudo cp arch/arm/boot/dts/*.dtb $TargetPath/boot
	sudo cp arch/arm/boot/dts/overlays/*.dtb* $TargetPath/boot/overlays/
	sudo cp arch/arm/boot/dts/overlays/README $TargetPath/boot/overlays/
	sudo cp arch/arm/boot/zImage $TargetPath/boot/$KERNEL.img

	echo "Finish - $(date)"
}

build_Script $1 $2
