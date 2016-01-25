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

#ifndef UPISOUND_GPIO_H
#define UPISOUND_GPIO_H

enum {
	UPISND_PIN_FLAG_IS_ENCODER_B              = 1 << 0,
	UPISND_PIN_FLAG_CONFIGURING_THROUGH_SYSFS = 1 << 1,
	UPISND_PIN_FLAG_IRQ_RISING                = 1 << 2,
	UPISND_PIN_FLAG_IRQ_FALLING               = 1 << 3,
};

struct upisnd_pin_config_t {
	upisnd_setup_t                    setup;
	u32                          flags;
	struct gpio_desc                  *gpio_desc;
	struct upisnd_element             *element;
};

struct upisnd_gpio {
	struct gpio_chip                  gpio_chip;
	const struct irq_chip            *gpio_irq;
	// Protects IRQ state and bus.
	struct mutex                      gpio_irq_lock;
	unsigned long                     gpio_state[2];
	unsigned long                     irq_flags;
	struct upisnd_irq_type_config_t   irq_type_config;
	struct upisnd_irq_mask_config_t   irq_mask_config;
	struct upisnd_pin_config_t        pin_configs[UPISND_NUM_GPIOS];
	int                               pin_subscription_refcounts[UPISND_NUM_GPIOS];
	DECLARE_KFIFO(irq_event_fifo, struct irq_event_t, 128);
	struct work_struct                irq_event_handler;
};

int upisnd_gpio_init(struct upisnd_instance *instance);
void upisnd_gpio_uninit(struct upisnd_instance *instance);

void upisnd_gpio_reset(struct upisnd_instance *instance);

int upisnd_gpio_setup(struct upisnd_instance *instance, upisnd_setup_t setup);

void upisnd_gpio_handle_irq_event(struct upisnd_instance *instance,
				  const struct irq_event_t *events,
				  unsigned int n);

void upisnd_gpio_set(struct upisnd_instance *instance, unsigned int offset, bool value);
int upisnd_gpio_get(struct upisnd_instance *instance, unsigned int offset);
int upisnd_gpio_get_direction(struct upisnd_instance *instance, unsigned int offset);

int upisnd_gpio_set_irq_type(struct upisnd_instance *instance,
			     unsigned int offset,
			     unsigned int irq_type);

int upisnd_gpio_set_subscription(struct upisnd_instance *instance, unsigned int offset, bool on);

enum upisnd_element_type_e upisnd_gpio_get_type(struct upisnd_instance *instance,
						unsigned int offset);

#endif // UPISOUND_GPIO_H

/* vim: set ts=8 sw=8 noexpandtab: */
