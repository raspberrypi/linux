// SPDX-License-Identifier: GPL-2.0-only
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

#include "upisnd_common.h"

int upisnd_map_value_range(int v, int input_min, int input_max, int output_min, int output_max)
{
	int x = (v - input_min) * (output_max - output_min);

	return x / (input_max - input_min) + output_min;
}

int upisnd_unmap_value_range(int v, int input_min, int input_max, int output_min, int output_max)
{
	int x = (v - output_min) * (input_max - input_min);

	return x / (output_max - output_min) + input_min;
}

/* vim: set ts=8 sw=8 noexpandtab: */
