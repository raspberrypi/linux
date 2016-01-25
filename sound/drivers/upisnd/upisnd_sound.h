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

#ifndef UPISOUND_SOUND_H
#define UPISOUND_SOUND_H

int upisnd_sound_init(struct platform_device *pdev, struct upisnd_instance *instance);

void upisnd_sound_handle_irq_events(struct upisnd_instance *instance,
				    const struct irq_event_t *events,
				    unsigned int n);

#endif // UPISOUND_SOUND_H

/* vim: set ts=8 sw=8 noexpandtab: */
