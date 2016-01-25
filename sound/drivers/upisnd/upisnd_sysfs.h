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

#ifndef UPISOUND_SYSFS_H
#define UPISOUND_SYSFS_H

int upisnd_sysfs_init(struct upisnd_instance *instance, const char *name);
void upisnd_sysfs_uninit(struct upisnd_instance *instance);

void upisnd_sysfs_handle_irq_event(struct upisnd_instance *instance,
				   const struct irq_event_t *events,
				   unsigned int n);
void upisnd_sysfs_handle_control_event(struct upisnd_instance *instance,
				       const struct control_event_t *events,
				       unsigned int n);

#endif // UPISOUND_SYSFS_H

/* vim: set ts=8 sw=8 noexpandtab: */
