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

#ifndef UPISOUND_PINS_H
#define UPISOUND_PINS_H

enum { UPISND_NUM_GPIOS = 37 };

enum upisnd_pin_e {
	UPISND_PIN_A27, UPISND_PIN_A28, UPISND_PIN_A29, UPISND_PIN_A30,
	UPISND_PIN_A31, UPISND_PIN_A32, UPISND_PIN_B03, UPISND_PIN_B04,
	UPISND_PIN_B05, UPISND_PIN_B06, UPISND_PIN_B07, UPISND_PIN_B08,
	UPISND_PIN_B09, UPISND_PIN_B10, UPISND_PIN_B11, UPISND_PIN_B12,
	UPISND_PIN_B13, UPISND_PIN_B14, UPISND_PIN_B15, UPISND_PIN_B16,
	UPISND_PIN_B17, UPISND_PIN_B18, UPISND_PIN_B23, UPISND_PIN_B24,
	UPISND_PIN_B25, UPISND_PIN_B26, UPISND_PIN_B27, UPISND_PIN_B28,
	UPISND_PIN_B29, UPISND_PIN_B30, UPISND_PIN_B31, UPISND_PIN_B32,
	UPISND_PIN_B33, UPISND_PIN_B34, UPISND_PIN_B37, UPISND_PIN_B38,
	UPISND_PIN_B39,

	UPISND_PIN_COUNT,
	UPISND_PIN_INVALID = UPISND_PIN_COUNT
};

typedef u8 upisnd_pin_t;

static inline bool upisnd_is_pin_valid(upisnd_pin_t pin)
{
	return pin < UPISND_PIN_COUNT;
}

enum upisnd_pin_capability_flags_e {
	UPISND_PIN_CAP_GPIO_DIR_INPUT  = 1 << 0,
	UPISND_PIN_CAP_GPIO_DIR_OUTPUT = 1 << 1,
	UPISND_PIN_CAP_GPIO_PULL_UP    = 1 << 2,
	UPISND_PIN_CAP_GPIO_PULL_DOWN  = 1 << 3,
	UPISND_PIN_CAP_GPIO            = UPISND_PIN_CAP_GPIO_DIR_INPUT |
					 UPISND_PIN_CAP_GPIO_DIR_OUTPUT |
					 UPISND_PIN_CAP_GPIO_PULL_UP |
					 UPISND_PIN_CAP_GPIO_PULL_DOWN,
	UPISND_PIN_CAP_ENCODER         = 1 << 4,
	UPISND_PIN_CAP_ANALOG_IN       = 1 << 5,
	UPISND_PIN_CAP_MIDI_ACTIVITY   = 1 << 6,
};

typedef u8 upisnd_pin_capability_mask_t;

upisnd_pin_t upisnd_name_to_pin(const char *name);
const char *upisnd_pin_name(upisnd_pin_t pin);
int upisnd_check_caps(upisnd_pin_t pin, upisnd_pin_capability_mask_t mask);

#endif // UPISOUND_PINS_H

/* vim: set ts=8 sw=8 noexpandtab: */
