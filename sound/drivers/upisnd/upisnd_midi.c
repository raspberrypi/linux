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

static int upisnd_midi_output_open(struct snd_rawmidi_substream *substream)
{
	struct upisnd_instance *instance = substream->rmidi->private_data;

	mutex_lock(&instance->midi.out_lock);
	instance->midi.midi_output = substream;
	mutex_unlock(&instance->midi.out_lock);
	return 0;
}

static void upisnd_midi_output_drain(struct snd_rawmidi_substream *substream);

static int upisnd_midi_output_close(struct snd_rawmidi_substream *substream)
{
	struct upisnd_instance *instance = substream->rmidi->private_data;

	printd("Close: draining output");
	upisnd_midi_output_drain(substream);
	printd("Close: setting to null");
	mutex_lock(&instance->midi.out_lock);
	instance->midi.midi_output = NULL;
	mutex_unlock(&instance->midi.out_lock);
	return 0;
}

static void upisnd_midi_output_trigger(struct snd_rawmidi_substream *substream, int up)
{
	if (up == 0)
		return;

	struct upisnd_instance *instance = substream->rmidi->private_data;

	if (!delayed_work_pending(&instance->midi.midi_out_handler))
		queue_delayed_work(instance->work_queue, &instance->midi.midi_out_handler, 0);
}

static void upisnd_midi_output_drain(struct snd_rawmidi_substream *substream)
{
	struct upisnd_instance *instance = substream->rmidi->private_data;

	printd("Begin draining!");

	do {
		printd("Before flush");
		while (delayed_work_pending(&instance->midi.midi_out_handler))
			flush_delayed_work(&instance->midi.midi_out_handler);
		printd("Flushed");
	} while (!snd_rawmidi_transmit_empty(substream));

	printd("Done!");
}

static int upisnd_midi_input_open(struct snd_rawmidi_substream *substream)
{
	struct upisnd_instance *instance = substream->rmidi->private_data;

	mutex_lock(&instance->midi.in_lock);
	instance->midi.midi_input = substream;
	mutex_unlock(&instance->midi.in_lock);
	return 0;
}

static int upisnd_midi_input_close(struct snd_rawmidi_substream *substream)
{
	struct upisnd_instance *instance = substream->rmidi->private_data;

	mutex_lock(&instance->midi.in_lock);
	instance->midi.midi_input = NULL;
	mutex_unlock(&instance->midi.in_lock);
	return 0;
}

static void upisnd_midi_input_trigger(struct snd_rawmidi_substream *substream, int up)
{
	if (up == 0)
		return;

	struct upisnd_instance *instance = substream->rmidi->private_data;

	if (!work_pending(&instance->midi.midi_in_handler))
		queue_work(instance->work_queue, &instance->midi.midi_in_handler);
}

static const struct snd_rawmidi_ops upisnd_midi_output_ops = {
	.open = upisnd_midi_output_open,
	.close = upisnd_midi_output_close,
	.trigger = upisnd_midi_output_trigger,
	.drain = upisnd_midi_output_drain,
};

static const struct snd_rawmidi_ops upisnd_midi_input_ops = {
	.open = upisnd_midi_input_open,
	.close = upisnd_midi_input_close,
	.trigger = upisnd_midi_input_trigger,
};

static void upisnd_get_port_info(struct snd_rawmidi *rmidi,
				 int number,
				 struct snd_seq_port_info *seq_port_info)
{
	seq_port_info->type =
		SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC |
		SNDRV_SEQ_PORT_TYPE_HARDWARE |
		SNDRV_SEQ_PORT_TYPE_PORT;
	seq_port_info->midi_voices = 0;
}

static const struct snd_rawmidi_global_ops upisnd_midi_ops = {
	.get_port_info = upisnd_get_port_info,
};

static void upisnd_midi_in_handler(struct work_struct *work)
{
	int i, n, err;

	printd("In handler");
	struct upisnd_instance *instance = container_of(work,
							struct upisnd_instance,
							midi.midi_in_handler);

	mutex_lock(&instance->midi.in_lock);
	if (!instance->midi.midi_input)
		goto cleanup;

	u8 data[512];

	n = kfifo_out_peek(&instance->midi.midi_in_fifo, data, sizeof(data));
	err = snd_rawmidi_receive(instance->midi.midi_input, data, n);

	if (err < 0)
		printe("snd_rawmidi_receive failed! (%d)", err);

	if (err > 0) {
		printd("Received %d MIDI bytes", err);
		instance->midi.rx_cnt += err;
	}

	for (i = 0; i < err; ++i)
		kfifo_skip(&instance->midi.midi_in_fifo);

	if (!kfifo_is_empty(&instance->midi.midi_in_fifo) &&
	    !work_pending(&instance->midi.midi_in_handler)) {
		queue_work(instance->work_queue, &instance->midi.midi_in_handler);
	}
cleanup:
	mutex_unlock(&instance->midi.in_lock);
	printd("Done");
}

static void upisnd_midi_out_handler(struct work_struct *work)
{
	printd("Out handler");
	struct upisnd_instance *instance = container_of(work,
							struct upisnd_instance,
							midi.midi_out_handler.work);

	mutex_lock(&instance->midi.out_lock);
	printd("midi_output = %p", instance->midi.midi_output);
	if (!instance->midi.midi_output)
		goto cleanup;

	enum { MIDI_MILLI_BYTES_PER_JIFFY = 3125000 / HZ };
	enum { MIDI_MAX_OUTPUT_BUFFER_SIZE_IN_MILLIBYTES = 4096000 };

	unsigned int now = jiffies;
	unsigned int millibytes_became_available = (MIDI_MILLI_BYTES_PER_JIFFY) *
		(now - instance->midi.last_midi_output_at);

	instance->midi.output_buffer_used_in_millibytes =
		instance->midi.output_buffer_used_in_millibytes <= millibytes_became_available ?
		0 : instance->midi.output_buffer_used_in_millibytes - millibytes_became_available;
	instance->midi.last_midi_output_at = now;

	unsigned int output_buffer_available = (MIDI_MAX_OUTPUT_BUFFER_SIZE_IN_MILLIBYTES -
		instance->midi.output_buffer_used_in_millibytes) / 1000;

	u8 buffer[UPISND_MAX_PACKET_LENGTH - 1];

	printd("Available: %u", output_buffer_available);
	int n = snd_rawmidi_transmit_peek(instance->midi.midi_output,
					  buffer,
					  min(output_buffer_available, sizeof(buffer)));

	if (n > 0) {
		printd("Peeked: %d", n);
		snd_rawmidi_transmit_ack(instance->midi.midi_output, n);
		n = upisnd_comm_send_midi(instance, buffer, (unsigned int)n);
		if (n < 0)
			printe("Error occurred when sending MIDI data over I2C! (%d)", n);
	} else {
		printe("snd_rawmidi_transmit_peek returned error %d!", n);
		goto cleanup;
	}

	instance->midi.tx_cnt += n;
	instance->midi.output_buffer_used_in_millibytes += n * 1000;

	printd("Checking if empty %p", instance->midi.midi_output);
	if (!snd_rawmidi_transmit_empty(instance->midi.midi_output)) {
		unsigned int delay = 0;

		if (instance->midi.output_buffer_used_in_millibytes >
		    MIDI_MAX_OUTPUT_BUFFER_SIZE_IN_MILLIBYTES - 127000)
			delay = 127000 / MIDI_MILLI_BYTES_PER_JIFFY;
		printd("Queue more work after %u jiffies", delay);
		queue_delayed_work(instance->work_queue, &instance->midi.midi_out_handler, delay);
	}

cleanup:
	mutex_unlock(&instance->midi.out_lock);
	printd("Done");
}

static void upisnd_proc_stat_show(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
	const unsigned int *d = entry->private_data;

	snd_iprintf(buffer, "%u\n", *d);
}

int upisnd_midi_init(struct upisnd_instance *instance)
{
	int err;

	mutex_init(&instance->midi.in_lock);
	mutex_init(&instance->midi.out_lock);

	err = snd_card_ro_proc_new(instance->sound_card.snd_card, "tx", &instance->midi.tx_cnt,
				   upisnd_proc_stat_show);
	err = snd_card_ro_proc_new(instance->sound_card.snd_card, "rx", &instance->midi.rx_cnt,
				   upisnd_proc_stat_show);

	err = snd_rawmidi_new(instance->sound_card.snd_card,
			      "pisoundmicro", 0, 1, 1,
			      &instance->midi.rawmidi);

	struct snd_rawmidi *rawmidi = instance->midi.rawmidi;

	if (err < 0) {
		printe("snd_rawmidi_new failed: %d\n", err);
		return err;
	}

	strscpy(rawmidi->name, "pisound-micro ", sizeof(rawmidi->name));
	strcat(rawmidi->name, instance->ctrl.serial);

	rawmidi->info_flags =
		SNDRV_RAWMIDI_INFO_OUTPUT |
		SNDRV_RAWMIDI_INFO_INPUT |
		SNDRV_RAWMIDI_INFO_DUPLEX;

	rawmidi->ops = &upisnd_midi_ops;

	rawmidi->private_data = instance;

	snd_rawmidi_set_ops(rawmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &upisnd_midi_output_ops);

	snd_rawmidi_set_ops(rawmidi, SNDRV_RAWMIDI_STREAM_INPUT, &upisnd_midi_input_ops);

	INIT_KFIFO(instance->midi.midi_in_fifo);
	instance->midi.last_midi_output_at = jiffies;

	INIT_WORK(&instance->midi.midi_in_handler, upisnd_midi_in_handler);
	INIT_DELAYED_WORK(&instance->midi.midi_out_handler, upisnd_midi_out_handler);

	return 0;
}

void upisnd_midi_uninit(struct upisnd_instance *instance)
{
	if (!instance->midi.rawmidi)
		return;

	cancel_work_sync(&instance->midi.midi_in_handler);
	cancel_delayed_work_sync(&instance->midi.midi_out_handler);

	instance->midi.rawmidi->private_data = NULL;

	instance->midi.rawmidi = NULL;
	mutex_lock(&instance->midi.in_lock);
	instance->midi.midi_input = NULL;
	mutex_unlock(&instance->midi.in_lock);
	mutex_lock(&instance->midi.out_lock);
	instance->midi.midi_output = NULL;
	mutex_unlock(&instance->midi.out_lock);

	kfifo_free(&instance->midi.midi_in_fifo);
}

void upisnd_handle_midi_data(struct upisnd_instance *instance, const uint8_t *data, unsigned int n)
{
	printd("%p, %u", instance, n);
	if (n == 0)
		return;

	int i;

	for (i = 0; i < n; ++i) {
		kfifo_put(&instance->midi.midi_in_fifo, data[i]);
		printd("Received MIDI %02x", data[i]);
	}

	if (!work_pending(&instance->midi.midi_in_handler))
		queue_work(instance->work_queue, &instance->midi.midi_in_handler);
	printd("Done");
}

/* vim: set ts=8 sw=8 noexpandtab: */
