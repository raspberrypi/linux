#########################################################################
# File Name: sync_to_localconfig.sh
# Author: 
# Email: 
# Created Time: 2019年03月03日 星期日 00时19分58秒
#########################################################################
#!/bin/bash
cp arch/arm/configs/bcm2709_defconfig .config 
make menuconfig ARCH=arm
