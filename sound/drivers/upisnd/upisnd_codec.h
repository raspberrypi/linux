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

 #ifndef UPISOUND_CODEC_H
 #define UPISOUND_CODEC_H

extern void adau1961_set_vgnd_shorted(struct snd_soc_component *component, bool shorted);
extern bool adau1961_is_hp_capless(struct snd_soc_component *component);

#endif // UPISOUND_CODEC_H
