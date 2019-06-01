#########################################################################
# File Name: auto.sh
# Author: Oxa71a5
# Email: oxa71a5@163.com
# Created Time: 2018年08月04日 星期六 11时02分26秒
#########################################################################
#!/bin/bash
remote_ip="192.168.2.107"
if [ ! -d "/tmp/normal2image" ]
then
    echo "mkdir /tmp/normal2image"
    mkdir /tmp/normal2image
fi


if [ ! -d "/tmp/normal2image/boot" ]
then
    mkdir /tmp/normal2image/boot/
    mkdir /tmp/normal2image/boot/overlays
fi


echo "copy kernel image"
scripts/mkknlimg arch/arm/boot/zImage /tmp/normal2image/kernel7.img
cp arch/arm/boot/dts/*.dtb /tmp/normal2image/boot
cp arch/arm/boot/dts/overlays/*.dtb* /tmp/normal2image/boot/overlays

cp replace.normal.sh /tmp/normal2image/


echo "zip these files to tar.gz"

cd /tmp/
tar -zcvf normal2image.tar.gz normal2image
echo ""
echo "Send vmlinux to remote target"
sshpass -p "'" scp vmlinux pi@$remote_ip:~/kernel/
echo "Send normal image to remote target"
sshpass -p "'" scp /tmp/normal2image.tar.gz pi@$remote_ip:~/

echo "extract remote target"
sshpass -p "'" ssh pi@$remote_ip tar -zxf /home/pi/normal2image.tar.gz
echo "exec replace shell"
sshpass -p "'" ssh pi@$remote_ip chmod 777 /home/pi/normal2image/replace.normal.sh
sshpass -p "'" ssh pi@$remote_ip  /home/pi/normal2image/replace.normal.sh
echo "Reboot remote slave"
sshpass -p "'" ssh pi@$remote_ip sudo reboot 
echo "done!"
