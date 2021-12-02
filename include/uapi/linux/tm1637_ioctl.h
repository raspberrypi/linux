/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Definitions for the TM1637 LED driver ioctl interface
 *
 * Author: Sukjin Kong <kongsukjin@beyless.com>
 * Copyright: (C) 2021 Beyless Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#ifndef _UAPI_LINUX_TM1637_IOCTL_H
#define _UAPI_LINUX_TM1637_IOCTL_H

#define MAX_LEDS 6

struct tm1637_ioctl_led_args {
	u8 leds[MAX_LEDS];
};

struct tm1637_ioctl_key_args {
	u8 key;
};

struct tm1637_ioctl_ctl_args {
	u8 brightness;
	u8 led;
};

#define TM1637_IOCTL_BASE 'K'
#define TM1637_IO(nr)				_IO(TM1637_IOCTL_BASE, nr)
#define TM1637_IOR(nr, type)		_IOR(TM1637_IOCTL_BASE, nr, type)
#define TM1637_IOW(nr, type)		_IOW(TM1637_IOCTL_BASE, nr, type)
#define TM1637_IOWR(nr, type)		_IOWR(TM1637_IOCTL_BASE, nr, type)

#define TM1637_IOC_GET_LED0			\
		TM1637_IOR(0x01, struct tm1637_ioctl_led_args)

#define TM1637_IOC_SET_LED0			\
		TM1637_IOW(0x02, struct tm1637_ioctl_led_args)

#define TM1637_IOC_GET_LED1			\
		TM1637_IOR(0x03, struct tm1637_ioctl_led_args)

#define TM1637_IOC_SET_LED1			\
		TM1637_IOW(0x04, struct tm1637_ioctl_led_args)

#define TM1637_IOC_GET_LED2			\
		TM1637_IOR(0x05, struct tm1637_ioctl_led_args)

#define TM1637_IOC_SET_LED2			\
		TM1637_IOW(0x06, struct tm1637_ioctl_led_args)

#define TM1637_IOC_GET_LED3			\
		TM1637_IOR(0x07, struct tm1637_ioctl_led_args)

#define TM1637_IOC_SET_LED3			\
		TM1637_IOW(0x08, struct tm1637_ioctl_led_args)

#define TM1637_IOC_GET_LED4			\
		TM1637_IOR(0x09, struct tm1637_ioctl_led_args)

#define TM1637_IOC_SET_LED4			\
		TM1637_IOW(0x0a, struct tm1637_ioctl_led_args)

#define TM1637_IOC_GET_LED5			\
		TM1637_IOR(0x0b, struct tm1637_ioctl_led_args)

#define TM1637_IOC_SET_LED5			\
		TM1637_IOW(0x0c, struct tm1637_ioctl_led_args)

#define TM1637_IOC_GET_LEDS			\
		TM1637_IOR(0x0d, struct tm1637_ioctl_led_args)

#define TM1637_IOC_SET_LEDS			\
		TM1637_IOW(0x0e, struct tm1637_ioctl_led_args)

#define TM1637_IOC_GET_KEY			\
		TM1637_IOR(0x0f, struct tm1637_ioctl_key_args)

#define TM1637_IOC_GET_BRIGHTNESS	\
		TM1637_IOR(0x11, struct tm1637_ioctl_ctl_args)

#define TM1637_IOC_SET_BRIGHTNESS	\
		TM1637_IOW(0x12, struct tm1637_ioctl_ctl_args)

#define TM1637_IOC_GET_LED			\
		TM1637_IOR(0x13, struct tm1637_ioctl_ctl_args)

#define TM1637_IOC_SET_LED			\
		TM1637_IOW(0x14, struct tm1637_ioctl_ctl_args)

#endif /* _UAPI_LINUX_TM1637_IOCTL_H */
