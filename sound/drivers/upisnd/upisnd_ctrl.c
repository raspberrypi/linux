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

static void upisnd_ctrl_handle_irq_events(struct upisnd_instance *instance,
					  const struct irq_event_t *events,
					  unsigned int n);
static void upisnd_ctrl_handle_control_events(struct upisnd_instance *instance,
					      const struct control_event_t *events,
					      unsigned int n);

static const struct upisnd_comm_handler_ops upisnd_comm_handlers = {
	.handle_midi_data        = &upisnd_handle_midi_data,
	.handle_gpio_irq_events  = &upisnd_ctrl_handle_irq_events,
	.handle_sound_irq_events = &upisnd_sound_handle_irq_events,
	.handle_control_events   = &upisnd_ctrl_handle_control_events,
};

static void upisnd_ctrl_cleanup(struct upisnd_instance *instance);

static inline bool upisnd_has_data(struct upisnd_instance *instance)
{
	bool x = gpiod_get_value(instance->ctrl.data_available);

	printd("data_available = %d", x);
	return x;
}

static irqreturn_t upisnd_data_available_interrupt_handler(int irq, void *dev_id)
{
	printd("irq_handler called");

	struct upisnd_instance *instance = dev_id;

	if (irq == instance->ctrl.client->irq) {
		printd("handling comm interrupt");
		upisnd_handle_comm_interrupt(instance);
		printd("handling done");
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static void upisnd_reset(struct upisnd_instance *instance)
{
	printd("Resetting...");

	gpiod_direction_output(instance->ctrl.reset, 1);
	usleep_range(1000, 5000);
#ifdef UPISOUND_DEV
	gpiod_direction_input(ctrl->reset);
#else
	gpiod_set_value(instance->ctrl.reset, 0);
#endif
	msleep(30);

	upisnd_gpio_reset(instance);

	printd("Done!");
}

int upisnd_ctrl_probe(struct i2c_client *client)
{
	int err = 0;

	printd("Ctrl %p irq %d", client, client->irq);
	printd("Dev %p", &client->dev);
	printd("Data %p", dev_get_platdata(&client->dev));

	struct upisnd_instance *instance = dev_get_platdata(&client->dev);

	if (!instance)
		return -EPROBE_DEFER;

	struct upisnd_ctrl *ctrl = &instance->ctrl;

	ctrl->client = client;

	err = upisnd_gpio_init(instance);
	if (err < 0) {
		printe("GPIO init failed! %d", err);
		goto cleanup;
	}

	ctrl->reset = devm_gpiod_get_index(&client->dev, "reset", 0, GPIOD_ASIS);
	if (IS_ERR(ctrl->reset)) {
		printe("Failed getting reset gpio!");
		err = PTR_ERR(ctrl->reset);
		ctrl->reset = NULL;
		goto cleanup;
	}

	upisnd_reset(instance);

	err = upisnd_comm_init(instance, client, &upisnd_comm_handlers);
	if (err < 0) {
		printe("Communication init failed! (%d)", err);
		goto cleanup;
	}

	ctrl->data_available = devm_gpiod_get_index(&client->dev, "data_available", 0, GPIOD_IN);
	if (IS_ERR(ctrl->data_available)) {
		printe("Failed getting data_available gpio!");
		err = PTR_ERR(ctrl->data_available);
		ctrl->data_available = NULL;
		goto cleanup;
	}

	err = devm_request_threaded_irq(&client->dev,
					ctrl->client->irq,
					NULL,
					upisnd_data_available_interrupt_handler,
					IRQF_SHARED | IRQF_ONESHOT | IRQF_TRIGGER_LOW,
					"data_available_int",
					instance);
	if (err != 0) {
		printe("data_available IRQ request failed! %d", err);
		goto cleanup;
	}

	err = upisnd_comm_get_version(instance, &instance->ctrl.version);
	if (err < 0) {
		printe("Failed getting version! %d", err);
		goto cleanup;
	}
	err = upisnd_comm_get_serial_number(instance, instance->ctrl.serial);
	if (err < 0) {
		printe("Failed getting serial number! %d", err);
		goto cleanup;
	}

	if (instance->ctrl.serial[0] != 'P' ||
	    instance->ctrl.serial[1] != 'S' ||
	    instance->ctrl.serial[2] != 'M' ||
	    strlen(instance->ctrl.serial) != 11) {
		printe("Unexpected serial number %s!", instance->ctrl.serial);
		err = -EINVAL;
		goto cleanup;
	}

	printi("Pisound Micro %s version %d.%d.%d, hw rev: %d",
	       instance->ctrl.serial,
	       instance->ctrl.version.major,
	       instance->ctrl.version.minor,
	       instance->ctrl.version.build,
	       instance->ctrl.version.hwrev);

	if (instance->ctrl.version.bootloader_mode) {
		printe("Pisound Micro is in bootloader mode! Please reflash the firmware, refer to documentation on https://blokas.io/");
		err = -EINVAL;
		goto cleanup;
	}

	const char *name = NULL;

	if (client->dev.of_node) {
		err = of_property_read_string(client->dev.of_node, "kobj-name", &name);

		if (of_property_read_bool(client->dev.of_node, "adc-calibration"))
			instance->flags |= UPISND_FLAG_ADC_CALIBRATION;
	}
	err = upisnd_sysfs_init(instance, name);
	if (err != 0) {
		printe("sysfs init failed! %d", err);
		goto cleanup;
	}

cleanup:
	if (err != 0) {
		printe("Error %d!", err);
		upisnd_ctrl_cleanup(instance);
		return err;
	}

	return 0;
}

void upisnd_ctrl_remove(struct i2c_client *client)
{
	printd("Ctrl %p", client);

	struct upisnd_instance *instance = dev_get_platdata(&client->dev);

	if (!instance)
		return;

	upisnd_ctrl_cleanup(instance);

	kref_put(&instance->refcount, &upisnd_instance_release);
	client->dev.platform_data = NULL;
}

static void upisnd_off(struct upisnd_instance *instance)
{
	printd("Turning off...");

#ifndef UPISOUND_DEV
	gpiod_direction_output(instance->ctrl.reset, 1);
#else
	upisnd_reset(&instance->ctrl);
#endif
	printd("Done!");
}

static void upisnd_ctrl_cleanup(struct upisnd_instance *instance)
{
	printd("cleanup");
	upisnd_sysfs_uninit(instance);

	if (instance->ctrl.reset) {
		upisnd_off(instance);
		instance->ctrl.reset = NULL;
	}
}

static void upisnd_ctrl_handle_irq_events(struct upisnd_instance *instance,
					  const struct irq_event_t *events,
					  unsigned int n)
{
	upisnd_gpio_handle_irq_event(instance, events, n);
	upisnd_sysfs_handle_irq_event(instance, events, n);
}

static void upisnd_ctrl_handle_control_events(struct upisnd_instance *instance,
					      const struct control_event_t *events,
					      unsigned int n)
{
	upisnd_sysfs_handle_control_event(instance, events, n);
}

/* vim: set ts=8 sw=8 noexpandtab: */
