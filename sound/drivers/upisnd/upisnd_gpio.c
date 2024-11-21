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

enum upisnd_pinconf_dir_e {
	UPISND_PINCONF_DIR_INPUT_PULL_AS_IS_OR_NONE = 0,
	UPISND_PINCONF_DIR_INPUT_PULL_UP            = 1,
	UPISND_PINCONF_DIR_INPUT_PULL_DOWN          = 2,
	UPISND_PINCONF_DIR_OUTPUT_LOW               = 4,
	UPISND_PINCONF_DIR_OUTPUT_HIGH              = 5,

	// If 3rd bit is set, direction is output, otherwise it's input.
	UPISND_PINCONF_DIR_MASK                     = 0x04,
};

enum { UPISND_PINCONF_DIRECTION = (PIN_CONFIG_END + 1) }; // enum upisnd_pinconf_dir_e

static int upisnd_gpio_chip_get_direction(struct gpio_chip *gc, unsigned int offset);
static int upisnd_gpio_chip_get(struct gpio_chip *gc, unsigned int offset);
static void upisnd_gpio_chip_set(struct gpio_chip *gc, unsigned int offset, int val);
static int upisnd_gpio_chip_get_multiple(struct gpio_chip *gc,
					 unsigned long *mask,
					 unsigned long *bits);
static void upisnd_gpio_chip_set_multiple(struct gpio_chip *gc,
					  unsigned long *mask,
					  unsigned long *bits);
static int upisnd_gpio_chip_set_config(struct gpio_chip *gc,
				       unsigned int offset,
				       unsigned long config);
static int upisnd_gpio_chip_dir_in(struct gpio_chip *gc, unsigned int offset);
static int upisnd_gpio_chip_dir_out(struct gpio_chip *gc, unsigned int offset, int val);
static int upisnd_gpio_chip_request(struct gpio_chip *gc, unsigned int offset);
static void upisnd_gpio_chip_free(struct gpio_chip *gc, unsigned int offset);

static const char * const upisnd_gpio_names[UPISND_NUM_GPIOS] = {
	"A27", "A28", "A29", "A30", "A31", "A32", "B03", "B04",
	"B05", "B06", "B07", "B08", "B09", "B10", "B11", "B12",
	"B13", "B14", "B15", "B16", "B17", "B18", "B23", "B24",
	"B25", "B26", "B27", "B28", "B29", "B30", "B31", "B32",
	"B33", "B34", "B37", "B38", "B39"
};

static const struct gpio_chip upisnd_gpio_chip = {
	.label = "pisound-micro-gpio",
	.owner = THIS_MODULE,
	.request = &upisnd_gpio_chip_request,
	.free = &upisnd_gpio_chip_free,
	.direction_input = &upisnd_gpio_chip_dir_in,
	.direction_output = &upisnd_gpio_chip_dir_out,
	.get_direction = &upisnd_gpio_chip_get_direction,
	.get = &upisnd_gpio_chip_get,
	.set = &upisnd_gpio_chip_set,
	.get_multiple = &upisnd_gpio_chip_get_multiple,
	.set_multiple = &upisnd_gpio_chip_set_multiple,
	.set_config = &upisnd_gpio_chip_set_config,
	.base = -1,
	.ngpio = UPISND_NUM_GPIOS,
	.can_sleep = true,
	.names = upisnd_gpio_names,
};

enum {
	IRQ_FLAG_TYPES_CHANGED_BIT = 0,
	IRQ_FLAG_MASKS_CHANGED_BIT = 1,
	IRQ_FLAG_HANDLING_IRQ_BIT  = 2,
};

static int upisnd_gpio_chip_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct upisnd_instance *instance = gpiochip_get_data(gc);
	struct upisnd_gpio *gpio = &instance->gpio;

	down_read(&instance->rw_gpio_config_sem);
	upisnd_setup_t setup = gpio->pin_configs[offset].setup;

	up_read(&instance->rw_gpio_config_sem);

	int dir = -EINVAL;

	if (upisnd_setup_get_element_type(setup) == UPISND_ELEMENT_TYPE_GPIO)
		dir = upisnd_setup_get_gpio_dir(setup) == UPISND_PIN_DIR_INPUT ? 1 : 0;

	printd("%p %u %d %08x", gc, offset, dir, setup);
	return dir;
}

static const char *upisnd_gpio_pull_to_str(enum upisnd_pin_pull_e pull)
{
	switch (pull) {
	default:
	case UPISND_PIN_PULL_NONE: return "pull_none";
	case UPISND_PIN_PULL_UP:   return "pull_up";
	case UPISND_PIN_PULL_DOWN: return "pull_down";
	}
}

static void upisnd_print_setup(const upisnd_setup_t setup)
{
	switch (upisnd_setup_get_element_type(setup)) {
	case UPISND_ELEMENT_TYPE_NONE:
		printi("Setup None %d", upisnd_setup_get_pin_id(setup));
		break;
	case UPISND_ELEMENT_TYPE_ENCODER:
		printi("Setup Encoder %d %s %d %s",
		       upisnd_setup_get_pin_id(setup),
		       upisnd_gpio_pull_to_str(upisnd_setup_get_gpio_pull(setup)),
		       upisnd_setup_get_encoder_pin_b_id(setup),
		       upisnd_gpio_pull_to_str(upisnd_setup_get_encoder_pin_b_pull(setup)));
		break;
	case UPISND_ELEMENT_TYPE_ANALOG_IN:
		printi("Setup Analog In %d (%d)",
		       upisnd_setup_get_pin_id(setup),
		       upisnd_setup_get_pin_id(setup));
		break;
	case UPISND_ELEMENT_TYPE_GPIO:
		if (upisnd_setup_get_gpio_dir(setup) == UPISND_PIN_DIR_INPUT)
			printi("Setup GPIO Input %d %s",
			       upisnd_setup_get_pin_id(setup),
			       upisnd_gpio_pull_to_str(upisnd_setup_get_gpio_pull(setup)));
		else
			printi("Setup GPIO Output %d %s",
			       upisnd_setup_get_pin_id(setup),
			       upisnd_setup_get_gpio_output(setup) ? "high" : "low");
		break;
	case UPISND_ELEMENT_TYPE_ACTIVITY:
		printi("Setup Activity %d %d",
		       upisnd_setup_get_pin_id(setup),
		       upisnd_setup_get_activity_type(setup));
		break;
	default:
		printe("Unknown Setup type %d", upisnd_setup_get_element_type(setup));
		break;
	}
}

int upisnd_gpio_setup(struct upisnd_instance *instance, upisnd_setup_t setup)
{
	int err;

#ifdef UPISND_DEBUG
	printd("Committing setup:");
	upisnd_print_setup(setup);
	err = upisnd_comm_commit_setup(instance, setup);
	printd("Result: %d", err)
#else
	err = upisnd_comm_commit_setup(instance, setup);

	if (err < 0) {
		printe("Failed to commit setup (%d), failed request:", err);
		upisnd_print_setup(setup);
	}
#endif

	return err;
}

static int upisnd_gpio_chip_get(struct gpio_chip *gc, unsigned int offset)
{
	printd("%p %u", gc, offset);

	int value = -EINVAL;

	struct upisnd_instance *instance = gpiochip_get_data(gc);

	unsigned long state[2];

	down_read(&instance->rw_gpio_config_sem);
	upisnd_setup_t setup = instance->gpio.pin_configs[offset].setup;

	state[0] = instance->gpio.gpio_state[0];
	state[1] = instance->gpio.gpio_state[1];
	up_read(&instance->rw_gpio_config_sem);

	if (upisnd_setup_get_element_type(setup) == UPISND_ELEMENT_TYPE_GPIO) {
		switch (upisnd_setup_get_gpio_dir(setup)) {
		case UPISND_PIN_DIR_OUTPUT:
			value = test_bit(offset, state) ? 1 : 0;
			break;
		case UPISND_PIN_DIR_INPUT:
			if (test_bit(IRQ_FLAG_HANDLING_IRQ_BIT, &instance->gpio.irq_flags)) {
				value = test_bit(offset, state) ? 1 : 0;
			} else {
				down_write(&instance->rw_gpio_config_sem);
				value = upisnd_comm_gpio_get(instance, offset);
				assign_bit(offset, instance->gpio.gpio_state, value);
				up_write(&instance->rw_gpio_config_sem);
			}
			break;
		default:
			break;
		}
	}

	return value;
}

static void upisnd_gpio_chip_set(struct gpio_chip *gc, unsigned int offset, int val)
{
	printd("%p %u %d", gc, offset, val);

	struct upisnd_instance *instance = gpiochip_get_data(gc);

	down_write(&instance->rw_gpio_config_sem);
	upisnd_setup_t setup = instance->gpio.pin_configs[offset].setup;

	if (upisnd_setup_get_element_type(setup) == UPISND_ELEMENT_TYPE_GPIO) {
		if (upisnd_setup_get_gpio_dir(setup) == UPISND_PIN_DIR_OUTPUT) {
			bool on = val != 0;

			assign_bit(offset, instance->gpio.gpio_state, on);
			upisnd_comm_gpio_set(instance, offset, on);
		}
	}
	up_write(&instance->rw_gpio_config_sem);
}

static void encode_gpio_state(struct upisnd_all_gpio_state_t *s, const unsigned long *bits)
{
	u8 *p = (uint8_t *)s->state;

	p[0] = (bits[0]) & 0xff;
	p[1] = (bits[0] >>  8) & 0xff;
	p[2] = (bits[0] >> 16) & 0xff;
	p[3] = (bits[0] >> 24) & 0xff;
	p[4] = (bits[1]) & 0xff;
}

static void decode_gpio_state(unsigned long *bits, const struct upisnd_all_gpio_state_t *s)
{
	const u8 *p = (const uint8_t *)s->state;

	bits[0] =
		(p[0]) |
		(p[1] <<  8) |
		(p[2] << 16) |
		(p[3] << 24);
	bits[1] =
		(p[4]);
}

static int upisnd_gpio_chip_get_multiple(struct gpio_chip *gc,
					 unsigned long *mask,
					 unsigned long *bits)
{
	unsigned int i;
	int err = 0;

	memset(bits, 0, sizeof(bits[0]) * 2);

	printd("%p %02lx%08lx", gc, mask[1], mask[0]);

	struct upisnd_instance *instance = gpiochip_get_data(gc);

	unsigned long state[2];

	down_read(&instance->rw_gpio_config_sem);
	for_each_set_bit(i, mask, UPISND_NUM_GPIOS) {
		upisnd_setup_t setup = instance->gpio.pin_configs[i].setup;

		if (upisnd_setup_get_element_type(setup) != UPISND_ELEMENT_TYPE_GPIO) {
			printe("Pin %u is not configured as GPIO!", i);
			err = -EINVAL;
		}
	}
	up_read(&instance->rw_gpio_config_sem);

	if (err < 0)
		return err;

	if (test_bit(IRQ_FLAG_HANDLING_IRQ_BIT, &instance->gpio.irq_flags)) {
		state[0] = instance->gpio.gpio_state[0];
		state[1] = instance->gpio.gpio_state[1];
	} else {
		struct upisnd_all_gpio_state_t s;

		down_write(&instance->rw_gpio_config_sem);

		err = upisnd_comm_gpio_get_all(instance, &s);

		if (err < 0) {
			up_write(&instance->rw_gpio_config_sem);
			return err;
		}

		decode_gpio_state(state, &s);

		instance->gpio.gpio_state[0] = state[0];
		instance->gpio.gpio_state[1] = state[1];
		up_write(&instance->rw_gpio_config_sem);
	}

	bits[0] = state[0] & mask[0];
	bits[1] = state[1] & mask[1];

	return err;
}

static void upisnd_gpio_chip_set_multiple(struct gpio_chip *gc,
					  unsigned long *mask,
					  unsigned long *bits)
{
	unsigned long state[2];

	printd("%p %02lx%08lx %02lx%08lx", gc, mask[1], mask[0], bits[1], bits[0]);

	struct upisnd_instance *instance = gpiochip_get_data(gc);

	down_write(&instance->rw_gpio_config_sem);

	state[0] = (instance->gpio.gpio_state[0] & ~mask[0]) | (bits[0] & mask[0]);
	state[1] = (instance->gpio.gpio_state[1] & ~mask[1]) | (bits[1] & mask[1]);

	struct upisnd_all_gpio_state_t s;

	encode_gpio_state(&s, state);

	upisnd_comm_gpio_set_all(instance, &s);

	instance->gpio.gpio_state[0] = state[0];
	instance->gpio.gpio_state[1] = state[1];

	up_write(&instance->rw_gpio_config_sem);
}

static int upisnd_gpio_chip_dir_in(struct gpio_chip *gc, unsigned int offset)
{
	printd("%p %u", gc, offset);
	return upisnd_gpio_chip_set_config(gc, offset, pinconf_to_config_packed
		((enum pin_config_param)UPISND_PINCONF_DIRECTION,
		UPISND_PINCONF_DIR_INPUT_PULL_AS_IS_OR_NONE));
}

static int upisnd_gpio_chip_dir_out(struct gpio_chip *gc, unsigned int offset, int val)
{
	printd("%p %u %d", gc, offset, val);
	return upisnd_gpio_chip_set_config(gc, offset, pinconf_to_config_packed
		((enum pin_config_param)UPISND_PINCONF_DIRECTION,
		val ? UPISND_PINCONF_DIR_OUTPUT_HIGH : UPISND_PINCONF_DIR_OUTPUT_LOW));
}

static int upisnd_gpio_chip_request(struct gpio_chip *gc, unsigned int offset)
{
	printd("(%p, %u)", gc, offset);

	struct upisnd_instance *instance = gpiochip_get_data(gc);

	int err;
	upisnd_setup_t setup = 0;

	down_write(&instance->rw_gpio_config_sem);

	if (upisnd_setup_get_element_type(instance->gpio.pin_configs[offset].setup) ==
					  UPISND_ELEMENT_TYPE_GPIO) {
		setup = instance->gpio.pin_configs[offset].setup;
	} else {
		upisnd_setup_set_element_type(&setup, UPISND_ELEMENT_TYPE_GPIO);
		upisnd_setup_set_gpio_dir(&setup, UPISND_PIN_DIR_INPUT);
		upisnd_setup_set_gpio_pull(&setup, UPISND_PIN_PULL_NONE);
		upisnd_setup_set_pin_id(&setup, offset);
	}

	// If the pin is already in use via setup interface.
	if (instance->gpio.pin_configs[offset].element) {
		// If the pin is not being requested via setup interface, deny the request.
		if (!(instance->gpio.pin_configs[offset].flags &
		      UPISND_PIN_FLAG_CONFIGURING_THROUGH_SYSFS)) {
			printe("Pin %u already exported through pisound-micro sysfs!", offset);
			err = -EBUSY;
		} else {
			err = 0;
		}

		up_write(&instance->rw_gpio_config_sem);
		return err;
	}

	err = upisnd_gpio_setup(instance, setup);

	if (err >= 0) {
		instance->gpio.pin_configs[offset].element = NULL;
		instance->gpio.pin_configs[offset].flags = 0;
		instance->gpio.pin_configs[offset].setup = setup;
		instance->gpio.pin_configs[offset].gpio_desc = NULL;
	}

	up_write(&instance->rw_gpio_config_sem);

	return err;
}

static void upisnd_gpio_chip_free(struct gpio_chip *gc, unsigned int offset)
{
	printd("(%p, %u)", gc, offset);

	struct upisnd_instance *instance = gpiochip_get_data(gc);

	down_write(&instance->rw_gpio_config_sem);
	if (instance->gpio.pin_configs[offset].element ||
	    (instance->gpio.pin_configs[offset].flags & UPISND_PIN_FLAG_CONFIGURING_THROUGH_SYSFS)
	    ) {
		// Unsetup will be taken care of in upisnd_element_cleanup,
		// ignore requests that come through gpiochip_free_own_desc.
		up_write(&instance->rw_gpio_config_sem);
		return;
	}

	upisnd_setup_t setup = instance->gpio.pin_configs[offset].setup;

	upisnd_setup_set_element_type(&setup, UPISND_ELEMENT_TYPE_NONE);
	upisnd_gpio_setup(instance, setup);

	memset(&instance->gpio.pin_configs[offset], 0, sizeof(struct upisnd_pin_config_t));

	up_write(&instance->rw_gpio_config_sem);

	printd("Done");
}

static int upisnd_gpio_chip_set_config(struct gpio_chip *gc,
				       unsigned int offset,
				       unsigned long config)
{
	int p = pinconf_to_config_param(config);
	u32 arg = pinconf_to_config_argument(config);

	printd("%p %u %08lx p=%d arg=%u", gc, offset, config, p, arg);

	struct upisnd_instance *instance = gpiochip_get_data(gc);

	down_write(&instance->rw_gpio_config_sem);
	if (instance->gpio.pin_configs[offset].flags & UPISND_PIN_FLAG_CONFIGURING_THROUGH_SYSFS) {
		// Setup has taken care of configuring the pin already,
		// ignore requests that come through gpiochip_request_own_desc.
		up_write(&instance->rw_gpio_config_sem);
		return 0;
	}

	int err = 0;

	upisnd_setup_t setup = instance->gpio.pin_configs[offset].setup;

	if (upisnd_setup_get_element_type(setup) != UPISND_ELEMENT_TYPE_GPIO) {
		err = -EINVAL;
		goto cleanup;
	}

	switch (p) {
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_PULL_UP:
		if (upisnd_setup_get_gpio_dir(setup) != UPISND_PIN_DIR_INPUT) {
			err = -EINVAL;
			goto cleanup;
		}
		if (arg != 0)
			upisnd_setup_set_gpio_pull(&setup, p == PIN_CONFIG_BIAS_PULL_UP ?
						   UPISND_PIN_PULL_UP : UPISND_PIN_PULL_DOWN);
		else
			upisnd_setup_set_gpio_pull(&setup, UPISND_PIN_PULL_NONE);
		break;
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
		if (upisnd_setup_get_gpio_dir(setup) != UPISND_PIN_DIR_INPUT) {
			err = -EINVAL;
			goto cleanup;
		}
		upisnd_setup_set_gpio_pull(&setup, UPISND_PIN_PULL_NONE);
		break;
	case UPISND_PINCONF_DIRECTION:
		if (arg & UPISND_PINCONF_DIR_MASK) {
			// Output.
			upisnd_setup_set_gpio_dir(&setup, UPISND_PIN_DIR_OUTPUT);
			upisnd_setup_set_gpio_output(&setup, arg == UPISND_PINCONF_DIR_OUTPUT_HIGH);
		} else {
			// Input.
			if (arg == UPISND_PINCONF_DIR_INPUT_PULL_AS_IS_OR_NONE) {
				if (upisnd_setup_get_gpio_dir(setup) == UPISND_PIN_DIR_OUTPUT)
					upisnd_setup_set_gpio_pull(&setup, UPISND_PIN_PULL_NONE);
			} else {
				upisnd_setup_set_gpio_pull(&setup, (enum upisnd_pin_pull_e)arg);
			}
			upisnd_setup_set_gpio_dir(&setup, UPISND_PIN_DIR_INPUT);
		}
		break;
	default:
		printe("Not supported param %d", p);
		err = -ENOTSUPP;
		goto cleanup;
	}

	err = upisnd_gpio_setup(instance, setup);

	if (err >= 0)
		instance->gpio.pin_configs[offset].setup = setup;

	printd("Done");

cleanup:
	up_write(&instance->rw_gpio_config_sem);

	return err;
}

static int upisnd_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	printd("%p %u", data, type);

	if (!(type & IRQ_TYPE_EDGE_BOTH)) {
		printe("IRQ %d: unsupported type %u\n", data->irq, type);
		return -ENOTSUPP;
	}

	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct upisnd_instance *instance = gpiochip_get_data(gc);

	unsigned long pin = data->hwirq;

	down_read(&instance->rw_gpio_config_sem);
	if (upisnd_setup_get_element_type(instance->gpio.pin_configs[pin].setup) !=
		UPISND_ELEMENT_TYPE_GPIO ||
		upisnd_setup_get_gpio_dir(instance->gpio.pin_configs[pin].setup) !=
		UPISND_PIN_DIR_INPUT) {
		printe("Pin %s is not set up as an input!", upisnd_pin_name(pin));
		up_read(&instance->rw_gpio_config_sem);
		return -EACCES;
	}
	up_read(&instance->rw_gpio_config_sem);

	upisnd_irq_config_set_irq_type(&instance->gpio.irq_type_config, pin, type);
	set_bit(IRQ_FLAG_TYPES_CHANGED_BIT, &instance->gpio.irq_flags);

	return 0;
}

static void upisnd_gpio_irq_ack(struct irq_data *data)
{
	printd("%p mask=%08x irq=%u hwirq=%lu", data, data->mask, data->irq, data->hwirq);
}

static void upisnd_gpio_irq_mask(struct irq_data *data)
{
	printd("%p mask=%08x irq=%u hwirq=%lu", data, data->mask, data->irq, data->hwirq);

	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct upisnd_instance *instance = gpiochip_get_data(gc);

	unsigned long pin = data->hwirq;

	down_read(&instance->rw_gpio_config_sem);
	if (upisnd_setup_get_element_type(instance->gpio.pin_configs[pin].setup) !=
		UPISND_ELEMENT_TYPE_GPIO ||
		upisnd_setup_get_gpio_dir(instance->gpio.pin_configs[pin].setup) !=
		UPISND_PIN_DIR_INPUT) {
		printe("Pin %s is not set up as an input!", upisnd_pin_name(pin));
		up_read(&instance->rw_gpio_config_sem);
		return;
	}
	up_read(&instance->rw_gpio_config_sem);

	upisnd_irq_mask_config_set(&instance->gpio.irq_mask_config, pin, true);
	set_bit(IRQ_FLAG_MASKS_CHANGED_BIT, &instance->gpio.irq_flags);
}

static void upisnd_gpio_irq_unmask(struct irq_data *data)
{
	printd("%p mask=%08x irq=%u hwirq=%lu", data, data->mask, data->irq, data->hwirq);

	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct upisnd_instance *instance = gpiochip_get_data(gc);

	unsigned long pin = data->hwirq;

	down_read(&instance->rw_gpio_config_sem);
	if (upisnd_setup_get_element_type(instance->gpio.pin_configs[pin].setup) !=
		UPISND_ELEMENT_TYPE_GPIO ||
		upisnd_setup_get_gpio_dir(instance->gpio.pin_configs[pin].setup) !=
		UPISND_PIN_DIR_INPUT) {
		printe("Pin %s is not set up as an input!", upisnd_pin_name(pin));
		up_read(&instance->rw_gpio_config_sem);
		return;
	}
	up_read(&instance->rw_gpio_config_sem);

	upisnd_irq_mask_config_set(&instance->gpio.irq_mask_config, pin, false);
	set_bit(IRQ_FLAG_MASKS_CHANGED_BIT, &instance->gpio.irq_flags);
}

static void upisnd_gpio_irq_bus_lock(struct irq_data *data)
{
	printd("%p mask=%08x irq=%u hwirq=%lu", data, data->mask, data->irq, data->hwirq);

	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct upisnd_instance *instance = gpiochip_get_data(gc);

	mutex_lock(&instance->gpio.gpio_irq_lock);
}

static void upisnd_gpio_irq_bus_sync_unlock(struct irq_data *data)
{
	printd("%p mask=%08x irq=%u hwirq=%lu", data, data->mask, data->irq, data->hwirq);

	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct upisnd_instance *instance = gpiochip_get_data(gc);

	int result = 0;

	if (test_bit(IRQ_FLAG_TYPES_CHANGED_BIT, &instance->gpio.irq_flags)) {
		printd("Types changed");
		result = upisnd_comm_set_irq_types(instance, &instance->gpio.irq_type_config);
		printd("result: %d", result);

		clear_bit(IRQ_FLAG_TYPES_CHANGED_BIT, &instance->gpio.irq_flags);
	}

	if (test_bit(IRQ_FLAG_MASKS_CHANGED_BIT, &instance->gpio.irq_flags)) {
		printd("Masks changed");
		result = upisnd_comm_set_irq_masks(instance, &instance->gpio.irq_mask_config);
		printd("result: %d", result);

		clear_bit(IRQ_FLAG_MASKS_CHANGED_BIT, &instance->gpio.irq_flags);
	}

	printd("done");
	mutex_unlock(&instance->gpio.gpio_irq_lock);
}

static const struct irq_chip upisnd_gpio_irq_chip = {
	.name = "pisound-micro-gpio",
	.irq_set_type = upisnd_gpio_irq_set_type,
	.irq_ack = upisnd_gpio_irq_ack,
	.irq_mask = upisnd_gpio_irq_mask,
	.irq_unmask = upisnd_gpio_irq_unmask,
	.irq_bus_lock = upisnd_gpio_irq_bus_lock,
	.irq_bus_sync_unlock = upisnd_gpio_irq_bus_sync_unlock,
	.flags = IRQCHIP_MASK_ON_SUSPEND | IRQCHIP_IMMUTABLE,
};

static void upisnd_gpio_irq_event_handler(struct work_struct *work)
{
	struct upisnd_instance *instance = container_of(work,
							struct upisnd_instance,
							gpio.irq_event_handler);

	int n;
	struct irq_event_t events[128];

	set_bit(IRQ_FLAG_HANDLING_IRQ_BIT, &instance->gpio.irq_flags);

	unsigned int offset;
	unsigned long masked[2];

	memset(masked, 0, sizeof(masked));
	u8 i;

	down_write(&instance->rw_gpio_config_sem);
	n = kfifo_out(&instance->gpio.irq_event_fifo, events, ARRAY_SIZE(events));
	if (n > 0) {
		printd("Handling %u IRQ events:", n);
		for (i = 0; i < n; ++i) {
			offset = events[i].num;

			if (offset == 0x7f) {
				printi("\tAlert event %s", events[i].high ? "on" : "off");
				continue;
			}

			printd("\t%d %s %s", offset, upisnd_pin_name(offset),
			       events[i].high ? "up" : "down");

			assign_bit(offset, instance->gpio.gpio_state, events[i].high);

			if (upisnd_irq_mask_config_get(&instance->gpio.irq_mask_config, offset))
				set_bit(offset, masked);
		}
	}
	up_write(&instance->rw_gpio_config_sem);

	for (i = 0; i < n; ++i) {
		if (!test_bit(i, masked)) {
			offset = events[i].num;
			int nested_irq = irq_find_mapping(instance->gpio.gpio_chip.irq.domain,
							  offset);

			if (unlikely(nested_irq <= 0)) {
				dev_warn_ratelimited(instance->gpio.gpio_chip.parent,
						     "unmapped interrupt %d\n",
						     offset);
				continue;
			} else {
				printd("Before handle_nested_irq");
				handle_nested_irq(nested_irq);
				printd("After handle_nested_irq");
			}
		}
	}

	clear_bit(IRQ_FLAG_HANDLING_IRQ_BIT, &instance->gpio.irq_flags);

	if (!kfifo_is_empty(&instance->gpio.irq_event_fifo))
		queue_work(instance->work_queue, &instance->gpio.irq_event_handler);
}

void upisnd_gpio_handle_irq_event(struct upisnd_instance *instance,
				  const struct irq_event_t *events,
				  unsigned int n)
{
	printd("Pushing %u events", n);
	kfifo_in(&instance->gpio.irq_event_fifo, events, n);
	if (!work_pending(&instance->gpio.irq_event_handler))
		queue_work(instance->work_queue, &instance->gpio.irq_event_handler);
}

void upisnd_gpio_reset(struct upisnd_instance *instance)
{
	down_write(&instance->rw_gpio_config_sem);

	memset(&instance->gpio.gpio_state, 0, sizeof(instance->gpio.gpio_state));
	memset(instance->gpio.pin_configs, 0, sizeof(instance->gpio.pin_configs));
	memset(&instance->gpio.irq_mask_config, 0xff, sizeof(instance->gpio.irq_mask_config));

	up_write(&instance->rw_gpio_config_sem);
}

int upisnd_gpio_init(struct upisnd_instance *instance)
{
	struct upisnd_gpio *gpio = &instance->gpio;

	mutex_init(&gpio->gpio_irq_lock);

	// Set all IRQs as masked initially.
	memset(&gpio->irq_mask_config, 0xff, sizeof(gpio->irq_mask_config));

	INIT_KFIFO(gpio->irq_event_fifo);
	INIT_WORK(&gpio->irq_event_handler, upisnd_gpio_irq_event_handler);

	gpio->gpio_chip = upisnd_gpio_chip;
	gpio->gpio_chip.parent = instance->ctrl_dev;

	gpio->gpio_irq = &upisnd_gpio_irq_chip;

	struct gpio_irq_chip *girq = &gpio->gpio_chip.irq;

	gpio_irq_chip_set_chip(girq, gpio->gpio_irq);
	girq->threaded = true;
	girq->num_parents = 1;
	girq->parents = devm_kzalloc(instance->ctrl_dev, sizeof(*girq->parents), GFP_KERNEL);

	if (!girq->parents)
		return -ENOMEM;

	*girq->parents = irq_of_parse_and_map(instance->ctrl_dev->of_node, 0);

	printd("Doing devm_gpiochip_add_data");
	int err = devm_gpiochip_add_data(instance->ctrl_dev, &gpio->gpio_chip, instance);

	printd("result: %d, base = %d", err, gpio->gpio_chip.base);
	return err;
}

void upisnd_gpio_uninit(struct upisnd_instance *instance)
{
	kfifo_free(&instance->gpio.irq_event_fifo);
}

void upisnd_gpio_set(struct upisnd_instance *instance, unsigned int offset, bool value)
{
	upisnd_gpio_chip_set(&instance->gpio.gpio_chip, offset, value);
}

int upisnd_gpio_get(struct upisnd_instance *instance, unsigned int offset)
{
	return upisnd_gpio_chip_get(&instance->gpio.gpio_chip, offset);
}

int upisnd_gpio_get_direction(struct upisnd_instance *instance, unsigned int offset)
{
	return upisnd_gpio_chip_get_direction(&instance->gpio.gpio_chip, offset);
}

int upisnd_gpio_set_irq_type(struct upisnd_instance *instance,
			     unsigned int offset,
			     unsigned int irq_type)
{
	printd("(%d, %d)", offset, irq_type);
	if (offset >= UPISND_NUM_GPIOS)
		return -EINVAL;

	if (!(irq_type & IRQ_TYPE_EDGE_BOTH)) {
		printe("IRQ %d: unsupported type %u\n", offset, irq_type);
		return -ENOTSUPP;
	}

	mutex_lock(&instance->gpio.gpio_irq_lock);

	if (upisnd_setup_get_element_type(instance->gpio.pin_configs[offset].setup) !=
		UPISND_ELEMENT_TYPE_GPIO ||
		upisnd_setup_get_gpio_dir(instance->gpio.pin_configs[offset].setup) !=
		UPISND_PIN_DIR_INPUT) {
		printe("Pin %s is not set up as an input!", upisnd_pin_name(offset));
		return -EACCES;
	}
	upisnd_irq_config_set_irq_type(&instance->gpio.irq_type_config, offset, irq_type);
	int result = upisnd_comm_set_irq_types(instance, &instance->gpio.irq_type_config);
	(void)result;

	mutex_unlock(&instance->gpio.gpio_irq_lock);

	printd("result: %d", result);
	return 0;
}

int upisnd_gpio_set_subscription(struct upisnd_instance *instance, unsigned int offset, bool on)
{
	printd("(%d, %d)", offset, on);

	if (offset >= UPISND_NUM_GPIOS)
		return -EINVAL;

	int result = 0;
	int *rc = &instance->gpio.pin_subscription_refcounts[offset];
	int c;

	mutex_lock(&instance->gpio.gpio_irq_lock);

	if (on)
		c = (*rc)++;
	else
		c = --(*rc);

	if (c == 0) {
		printd("Count was 0, setting %d sub to %d", offset, on);
		result = upisnd_comm_set_subscription(instance, offset, on);
	}

	mutex_unlock(&instance->gpio.gpio_irq_lock);

	return result;
}

enum upisnd_element_type_e upisnd_gpio_get_type(struct upisnd_instance *instance,
						unsigned int offset)
{
	if (offset >= UPISND_NUM_GPIOS)
		return UPISND_ELEMENT_TYPE_NONE;

	down_read(&instance->rw_gpio_config_sem);
	upisnd_setup_t setup = instance->gpio.pin_configs[offset].setup;

	up_read(&instance->rw_gpio_config_sem);

	return upisnd_setup_get_element_type(setup);
}

/* vim: set ts=8 sw=8 noexpandtab: */
