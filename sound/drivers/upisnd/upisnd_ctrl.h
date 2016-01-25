/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Pisound Micro Linux kernel module.
 * Copyright (C) 2017-2025  Vilniaus Blokas UAB, https://blokas.io/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef UPISOUND_CTRL_H
#define UPISOUND_CTRL_H

struct upisnd_ctrl {
	struct i2c_client       *client;
	struct upisnd_gpio      *gpio;

	struct upisnd_version_t version;
	char                    serial[12];

	struct gpio_desc        *data_available;
	struct gpio_desc        *reset;
};

int upisnd_ctrl_probe(struct i2c_client *client);
void upisnd_ctrl_remove(struct i2c_client *client);

#endif // UPISOUND_CTRL_H

/* vim: set ts=8 sw=8 noexpandtab: */
