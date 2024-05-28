// SPDX-License-Identifier: GPL-2.0

//! Driver for Raspi GPIO LED.
//!
//! Bashd on C driver:../gpio-sample-rpbled.c
//!
//! only build-in:
//! make LLVM=-17 O=build_4b ARCH=arm64 
//!
//! How to use in qemu:
//! / # sudo insmod rust_miscdev.ko
//! / # sudo cat /proc/misc  -> c 10 122
//! / # sudo chmod 777 /dev/rust_misc
//! / # sudo echo "hello" > /dev/rust_misc
//! / # sudo cat /dev/rust_misc  -> Hello
//! 

#![no_std]

// TODO
// 参考 drivers/gpio/exercise-3-gpio/gpio-sample-rpbled.c代码
// C代码里面用到的字符设备，还是用练习二的Msicdev来实现就可以了
// OSLayer已经集成，但是Adapter driver和Pure driver层的代码需要自己实现
// 最好通过树莓派开发板来查看相应的GPIO控制效果
//