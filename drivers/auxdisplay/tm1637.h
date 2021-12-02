// SPDX-License-Identifier: GPL-2.0
/*
 * TM1637 LED driver
 *
 * Author: Sukjin Kong <kongsukjin@beyless.com>
 * Copyright: (C) 2021 Beyless Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * Derived from tm1637.c
 */

#ifndef __TM1637_H__
#define __TM1637_H__

#define TM1637_DEV_ATTR_RW(name, show, store) \
	DEVICE_ATTR(name, S_IRUGO | S_IWUSR, show, store)
#define TM1637_DEV_ATTR_RO(name, show) \
	DEVICE_ATTR(name, S_IRUGO, show, NULL)
#define TM1637_DEV_ATTR_WO(name, store) \
	DEVICE_ATTR(name, S_IWUSR, NULL, store)

#define show_led(nr) \
static ssize_t tm1637_show_led##nr(struct device *dev, \
				struct device_attribute *attr, \
				char *buf) \
{ \
	return tm1637_show_led(dev, attr, buf, nr); \
}

#define store_led(nr) \
static ssize_t tm1637_store_led##nr(struct device *dev, \
                 struct device_attribute *attr, \
				 const char *buf, size_t len)\
{ \
	return tm1637_store_led(dev, attr, buf, len, nr); \
}

#endif /* __TM1637__H__ */
