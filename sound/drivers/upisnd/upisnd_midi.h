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

#ifndef UPISOUND_MIDI_H
#define UPISOUND_MIDI_H

struct upisnd_instance;

struct upisnd_midi {
	// in_lock protects the input substream.
	struct mutex                      in_lock;
	// out_lock protects the output substream.
	struct mutex                      out_lock;
	struct work_struct                midi_in_handler;
	struct delayed_work               midi_out_handler;

	struct snd_rawmidi                *rawmidi;
	struct snd_rawmidi_substream      *midi_input;
	struct snd_rawmidi_substream      *midi_output;
	DECLARE_KFIFO(midi_in_fifo, uint8_t, 4096);
	unsigned int                      last_midi_output_at;
	unsigned int                      output_buffer_used_in_millibytes;
	unsigned int                      tx_cnt;
	unsigned int                      rx_cnt;
};

int upisnd_midi_init(struct upisnd_instance *instance);
void upisnd_midi_uninit(struct upisnd_instance *instance);

void upisnd_handle_midi_data(struct upisnd_instance *instance, const uint8_t *data, unsigned int n);

#endif // UPISOUND_MIDI_H

/* vim: set ts=8 sw=8 noexpandtab: */
