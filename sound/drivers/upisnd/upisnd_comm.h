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

#ifndef UPISOUND_COMM_H
#define UPISOUND_COMM_H

struct upisnd_instance;

struct upisnd_version_t {
	uint8_t bootloader_mode:1;
	uint8_t hwrev:7;
	u8 major;
	u8 minor;
	u8 build;
};

struct irq_event_t {
	upisnd_irq_num_t num;
	bool             high;
};

struct control_event_t {
	upisnd_pin_t pin:6;
	int16_t      raw_value:10;
};

struct upisnd_comm_handler_ops {
	void (*handle_midi_data)(struct upisnd_instance *instance,
				 const u8 *data,
				 unsigned int n);
	void (*handle_gpio_irq_events)(struct upisnd_instance *instance,
				       const struct irq_event_t *events,
				       unsigned int n);
	void (*handle_sound_irq_events)(struct upisnd_instance *instance,
					const struct irq_event_t *events,
					unsigned int n);
	void (*handle_control_events)(struct upisnd_instance *instance,
				      const struct control_event_t *events,
				      unsigned int n);
};

struct upisnd_comm {
	struct mutex                         comm_lock;
	// comm_lock serializes I2C communication.
	struct i2c_client                    *client;

	const struct upisnd_comm_handler_ops *handler_ops;

	// msg_lock protects the message handlers list and id.
	struct mutex                         msg_lock;
	struct list_head                     msg_handlers;
	upisnd_msg_id_t                      msg_id_counter;
};

int upisnd_comm_module_init(void);
int upisnd_comm_init(struct upisnd_instance *instance,
		     struct i2c_client *client,
		     const struct upisnd_comm_handler_ops *ops);
int upisnd_comm_send_midi(struct upisnd_instance *instance, const void *data, unsigned int n);
int upisnd_comm_commit_setup(struct upisnd_instance *instance, upisnd_setup_t setup);
int upisnd_comm_gpio_set(struct upisnd_instance *instance, u8 pin, bool high);
int upisnd_comm_gpio_get(struct upisnd_instance *instance, u8 pin);
int upisnd_comm_gpio_set_all(struct upisnd_instance *instance,
			     const struct upisnd_all_gpio_state_t *state);
int upisnd_comm_gpio_get_all(struct upisnd_instance *instance,
			     struct upisnd_all_gpio_state_t *result);
int upisnd_comm_set_irq_types(struct upisnd_instance *instance,
			      const struct upisnd_irq_type_config_t *irq_type_config);
int upisnd_comm_set_irq_masks(struct upisnd_instance *instance,
			      const struct upisnd_irq_mask_config_t *irq_mask_config);
int upisnd_comm_set_subscription(struct upisnd_instance *instance,
				 upisnd_irq_num_t irq, bool on);
int upisnd_comm_get_version(struct upisnd_instance *instance,
			    struct upisnd_version_t *result);
int upisnd_comm_get_element_value(struct upisnd_instance *instance,
				  u8 pin,
				  int32_t *result);
int upisnd_comm_get_serial_number(struct upisnd_instance *instance,
				  char result[12]);
int upisnd_comm_get_value(struct upisnd_instance *instance,
			  enum upisnd_value_id_t value_id,
			  int32_t *result);
int upisnd_comm_set_value(struct upisnd_instance *instance,
			  enum upisnd_value_id_t value_id,
			  int32_t value);

void upisnd_handle_comm_interrupt(struct upisnd_instance *instance);

#endif // UPISOUND_COMM_H

/* vim: set ts=8 sw=8 noexpandtab: */
