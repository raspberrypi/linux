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

#ifndef UPISOUND_DEBUG_H
#define UPISOUND_DEBUG_H

#define UPISOUND_LOG_IMPL(log_func, msg, ...) \
	log_func("pisound-micro(%s): " msg "\n", __func__, ## __VA_ARGS__)

#ifdef UPISOUND_DEBUG
#	define printd(...) UPISOUND_LOG_IMPL(pr_alert, __VA_ARGS__)
#else
#	define printd(...) do {} while (0)
#endif

#define printe(...) UPISOUND_LOG_IMPL(pr_err, __VA_ARGS__)
#define printi(...) UPISOUND_LOG_IMPL(pr_info, __VA_ARGS__)

#endif // UPISOUND_DEBUG_H

/* vim: set ts=8 sw=8 noexpandtab: */
