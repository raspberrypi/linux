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

struct upisnd_pin_def_t {
	const char                   *name;
	upisnd_pin_capability_mask_t capabilities;
};

static const struct upisnd_pin_def_t upisnd_pins[UPISND_PIN_COUNT] = {
	{ "A27", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY },
	{ "A28", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY },
	{ "A29", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY },
	{ "A30", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY },
	{ "A31", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY },
	{ "A32", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY },
	{ "B03", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B04", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B05", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B06", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B07", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B08", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B09", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B10", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	// Pull down is not available in MCU revisions A, B, C, D, E. F and G are not affected.
	{ "B11", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	// Pull down is not available in MCU revisions A, B, C, D, E. F and G are not affected.
	{ "B12", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B13", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B14", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B15", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B16", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B17", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B18", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ENCODER },
	{ "B23", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B24", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B25", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B26", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B27", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B28", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B29", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B30", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B31", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B32", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B33", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B34", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY | UPISND_PIN_CAP_ANALOG_IN },
	{ "B37", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY },
	{ "B38", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY },
	{ "B39", UPISND_PIN_CAP_GPIO | UPISND_PIN_CAP_MIDI_ACTIVITY },
};

upisnd_pin_t upisnd_name_to_pin(const char *name)
{
	if (!name || strlen(name) != 3 || !isdigit(name[1]) || !isdigit(name[2]))
		return UPISND_PIN_INVALID;

	char sanitized[4];

	switch (*name) {
	case 'a': case 'A':
		sanitized[0] = 'A';
		break;
	case 'b': case 'B':
		sanitized[0] = 'B';
		break;
	default:
		return UPISND_PIN_INVALID;
	}

	memcpy(&sanitized[1], &name[1], 2);
	sanitized[3] = '\0';

	int i;

	for (i = 0; i < ARRAY_SIZE(upisnd_pins); ++i) {
		if (strcmp(sanitized, upisnd_pins[i].name) == 0)
			return i;
	}

	return UPISND_PIN_INVALID;
}

const char *upisnd_pin_name(upisnd_pin_t pin)
{
	if (!upisnd_is_pin_valid(pin))
		return "";

	return upisnd_pins[pin].name;
}

int upisnd_check_caps(upisnd_pin_t pin, upisnd_pin_capability_mask_t mask)
{
	if (!upisnd_is_pin_valid(pin))
		return -ENXIO;

	return (upisnd_pins[pin].capabilities & mask) == mask ? 0 : -EINVAL;
}

/* vim: set ts=8 sw=8 noexpandtab: */
