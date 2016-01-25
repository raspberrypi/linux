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

#ifndef UPISOUND_UTILS_H
#define UPISOUND_UTILS_H

int upisnd_map_value_range(int v, int input_min, int input_max, int output_min, int output_max);
int upisnd_unmap_value_range(int v, int input_min, int input_max, int output_min, int output_max);

#endif // UPISOUND_UTILS_H

/* vim: set ts=8 sw=8 noexpandtab: */
