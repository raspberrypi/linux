#########################################################################
# File Name: sync_to_defconfig.sh
# Author: 
# Email: 
# Created Time: 2019年03月03日 星期日 00时19分38秒
#########################################################################
#!/bin/bash
cp .config arch/arm/configs/bcm2709_defconfig
git diff
