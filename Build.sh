echo PATH=\$PATH:~/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian/bin >> ~/.bashrc # 32
echo PATH=\$PATH:~/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64/bin >> ~/.bashrc # 64
source ~/.bashrc

KERNEL=kernel # pi zero
KERNEL=kernel7 # pi 3
KERNEL=kernel7l # pi 4

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcmrpi_defconfig # pi zero
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcm2709_defconfig # pi 3
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- bcm2711_defconfig # pi 4

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage modules dtbs

sudo env PATH=$PATH make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- INSTALL_MOD_PATH=/media/$USER/rootfs modules_install
sudo cp /media/$USER/boot/$KERNEL.img /media/$USER/boot/$KERNEL-backup.img
sudo cp arch/arm/boot/zImage /media/$USER/boot/$KERNEL.img
sudo cp arch/arm/boot/dts/*.dtb /media/$USER/boot
sudo cp arch/arm/boot/dts/overlays/*.dtb* /media/$USER/boot/overlays/
sudo cp arch/arm/boot/dts/overlays/README /media/$USER/boot/overlays/
