#########################################################################
# File Name: auto.sh
# Author: Oxa71a5
# Email: oxa71a5@163.com
# Created Time: 2018年08月04日 星期六 11时02分26秒
#########################################################################
#!/bin/bash
remote_ip="192.168.2.107"
if [ ! -d "/tmp/normalimage" ]
then
    echo "mkdir /tmp/normalimage"
    mkdir /tmp/normalimage
fi


if [ ! -d "/tmp/normalimage/boot" ]
then
    mkdir /tmp/normalimage/boot/
    mkdir /tmp/normalimage/boot/overlays
fi


echo "copy kernel image"
scripts/mkknlimg arch/arm/boot/zImage /tmp/normalimage/kernel7.img
cp arch/arm/boot/dts/*.dtb /tmp/normalimage/boot
cp arch/arm/boot/dts/overlays/*.dtb* /tmp/normalimage/boot/overlays
cp -r modules/lib/ /tmp/normalimage/

cp replace.sh /tmp/normalimage/


echo "zip these files to tar.gz"

cd /tmp/
tar -zcvf normalimage.tar.gz normalimage
echo ""
echo "Send to remote target"
sshpass -p "'" scp /tmp/normalimage.tar.gz pi@$remote_ip:~/

echo "extract remote target"
sshpass -p "'" ssh pi@$remote_ip tar -zxf /home/pi/normalimage.tar.gz
echo "exec replace shell"
sshpass -p "'" ssh pi@$remote_ip chmod 777 /home/pi/normalimage/replace.sh
sshpass -p "'" ssh pi@$remote_ip  /home/pi/normalimage/replace.sh
echo "Reboot remote slave"
sshpass -p "'" ssh pi@$remote_ip sudo reboot 
echo "done!"
