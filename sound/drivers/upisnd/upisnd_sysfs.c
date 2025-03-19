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

enum {
	// Max lengths are with '\0' included.
	MAX_ELEMENT_NAME_LENGTH  = 64,
	MAX_SETUP_REQUEST_LENGTH = 63 + MAX_ELEMENT_NAME_LENGTH,
};

static void upisnd_config_release(struct kobject *kobj);
static void upisnd_element_release(struct kobject *kobj);

static ssize_t upisnd_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	printd("hi %s %s", kobject_name(kobj), attr->name);
	struct kobj_attribute *attribute = container_of(attr, struct kobj_attribute, attr);

	if (!attribute->show)
		return -EIO;

	int err = attribute->show(kobj, attribute, buf);

	return err;
}

static ssize_t upisnd_attr_store(struct kobject *kobj,
				 struct attribute *attr,
				 const char *buf,
				 size_t len)
{
	struct kobj_attribute *attribute = container_of(attr, struct kobj_attribute, attr);

	if (!attribute->store)
		return -EIO;

	int err = attribute->store(kobj, attribute, buf, len);

	return err;
}

static const struct sysfs_ops upisnd_attr_ops = {
	.show = &upisnd_attr_show,
	.store = &upisnd_attr_store
};

#define to_config(kobj_) container_of(kobj_, struct upisnd_config, kset.kobj)
struct upisnd_config {
	struct kset            kset;
	struct upisnd_instance *instance;
	struct kset            *elements;
};

static int upisnd_element_cleanup(struct kobject *kobj, struct upisnd_instance *instance);

static void upisnd_config_release(struct kobject *kobj)
{
	printd("hi");
	struct upisnd_config *cfg = to_config(kobj);

	kfree(cfg);
}

static const struct kobj_type upisnd_config_type = {
	.release = &upisnd_config_release,
	.sysfs_ops = &upisnd_attr_ops
};

static const struct kobj_type upisnd_element_type = {
	.release = &upisnd_element_release,
	.sysfs_ops = &upisnd_attr_ops
};

// Used for handling encoder value overflow.
enum upisnd_value_mode {
	UPISND_VALUE_MODE_CLAMP = 0,
	UPISND_VALUE_MODE_WRAP,
};

#define to_element(kobj_) container_of(kobj_, struct upisnd_element, kobj)
struct upisnd_element {
	struct kobject         kobj;
	// Mapped from upisnd pins to gpio pin numbering.
	// gpio_pins[1] is used only for encoder B pin.
	// Negative number is used to indicate invalid/unused pin.
	int                    gpio_pins[2];
	int                    value;
	int                    raw_value;
	int                    input_min;
	int                    input_max;
	int                    value_low;
	int                    value_high;
	enum upisnd_value_mode value_mode;
};

// Returns true if the value changed.
static bool upisnd_element_update_value(struct upisnd_element *el)
{
	int old_value = el->value;

	int value = el->raw_value;

	switch (el->value_mode) {
	case UPISND_VALUE_MODE_CLAMP:
		value = min(max(value, el->input_min), el->input_max);
		break;
	case UPISND_VALUE_MODE_WRAP:
		if (el->input_max != el->input_min) {
			while (value < el->input_min)
				value += el->input_max - el->input_min;
			value = (value - el->input_min) % (el->input_max - el->input_min)
				+ el->input_min;
		} else {
			value = el->input_min;
		}
		break;
	default:
		return false;
	}

	el->raw_value = value;
	el->value = upisnd_map_value_range(value, el->input_min, el->input_max, el->value_low,
					   el->value_high);

	return el->value != old_value;
}

static int upisnd_parse_gpio_dir(enum upisnd_pin_direction_e *dir, char **s, const char *sep)
{
	if (!dir || !s || !sep)
		return -EINVAL;

	char *t = strsep(s, sep);

	if (!t || *t == '\0')
		return -EINVAL;

	if (strncasecmp(t, "input", 6u) == 0) {
		*dir = UPISND_PIN_DIR_INPUT;
		return 0;
	} else if (strncasecmp(t, "output", 7u) == 0) {
		*dir = UPISND_PIN_DIR_OUTPUT;
		return 0;
	}

	return -EINVAL;
}

static int upisnd_parse_gpio_input_pull(enum upisnd_pin_pull_e *pull, char **s, const char *sep)
{
	if (!pull || !s || !sep)
		return -EINVAL;

	char *t = strsep(s, sep);

	if (!t || *t == '\0')
		return -EINVAL;

	if (strncasecmp(t, "pull_up", 8u) == 0) {
		*pull = UPISND_PIN_PULL_UP;
		return 0;
	} else if (strncasecmp(t, "pull_down", 10u) == 0) {
		*pull = UPISND_PIN_PULL_DOWN;
		return 0;
	} else if (strncasecmp(t, "pull_none", 10u) == 0) {
		*pull = UPISND_PIN_PULL_NONE;
		return 0;
	}

	return -EINVAL;
}

static int upisnd_parse_gpio_output_level(bool *level, char **s, const char *sep)
{
	if (!level || !s || !sep)
		return -EINVAL;

	char *t = strsep(s, sep);

	if (!t || *t == '\0')
		return -EINVAL;

	return kstrtobool(t, level);
}

static int upisnd_parse_pin(upisnd_pin_t *pin, char **s, const char *sep)
{
	if (!pin || !s || !sep)
		return -EINVAL;

	char *t = strsep(s, sep);

	*pin = upisnd_name_to_pin(t);

	return upisnd_is_pin_valid(*pin) ? 0 : -EINVAL;
}

static int upisnd_validate_setup(upisnd_setup_t setup)
{
	switch (upisnd_setup_get_element_type(setup)) {
	case UPISND_ELEMENT_TYPE_ENCODER:
	{
		int a = upisnd_check_caps(upisnd_setup_get_pin_id(setup), UPISND_PIN_CAP_ENCODER);
		int b = upisnd_check_caps(upisnd_setup_get_encoder_pin_b_id(setup),
					  UPISND_PIN_CAP_ENCODER);

		if (a < 0 || b < 0) {
			printe("Invalid or incapable pins for Encoder (%d, %d)", a, b);
			return -EINVAL;
		}
		return 0;
	}
	case UPISND_ELEMENT_TYPE_ANALOG_IN:
	{
		int err = upisnd_check_caps(upisnd_setup_get_pin_id(setup),
					    UPISND_PIN_CAP_ANALOG_IN);

		if (err < 0) {
			printe("Invalid or incapable pin for Analog In (%d)", err);
			return err;
		}
		return 0;
	}
	case UPISND_ELEMENT_TYPE_GPIO:
	{
		int err = upisnd_check_caps(upisnd_setup_get_pin_id(setup), UPISND_PIN_CAP_GPIO);

		if (err < 0) {
			printe("Invalid or incapable pin for GPIO (%d)", err);
			return err;
		}
		return 0;
	}
	case UPISND_ELEMENT_TYPE_ACTIVITY:
	{
		int err;

		switch (upisnd_setup_get_activity_type(setup)) {
		case UPISND_ACTIVITY_TYPE_MIDI_IN:
		case UPISND_ACTIVITY_TYPE_MIDI_OUT:
			err = upisnd_check_caps(upisnd_setup_get_pin_id(setup),
						UPISND_PIN_CAP_MIDI_ACTIVITY);
			if (err < 0) {
				printe("Invalid or incapable pin for MIDI Activity (%d)", err);
				return err;
			}
			break;
		default:
			printe("Invalid activity type!");
			err = -EINVAL;
			break;
		}

		return err;
	}
	break;
	case UPISND_ELEMENT_TYPE_NONE:
	default:
		return -EINVAL;
	}
}

static int upisnd_parse_setup(upisnd_setup_t *setup, const char *buf, size_t len)
{
	if (!setup || !buf || len + 1 >= MAX_SETUP_REQUEST_LENGTH)
		return -EINVAL;

	*setup = 0;
	upisnd_setup_set_element_type(setup, UPISND_ELEMENT_TYPE_NONE);

	char b[MAX_SETUP_REQUEST_LENGTH];

	strscpy(b, buf, len + 1);

	static const char *const SEP = "\n\t ";

	char *s = b;
	char *token = strsep(&s, SEP);

	if (!token || *token == '\0')
		return -EINVAL;

	if (strncasecmp(token, "encoder", 8u) == 0) {
		upisnd_setup_set_element_type(setup, UPISND_ELEMENT_TYPE_ENCODER);
	} else if (strncasecmp(token, "analog_in", 10u) == 0) {
		upisnd_setup_set_element_type(setup, UPISND_ELEMENT_TYPE_ANALOG_IN);
	} else if (strncasecmp(token, "activity_", 9u) == 0) {
		if (strncasecmp(&token[9], "midi_in", 8u) == 0)
			upisnd_setup_set_activity_type(setup, UPISND_ACTIVITY_TYPE_MIDI_IN);
		else if (strncasecmp(&token[9], "midi_out", 9u) == 0)
			upisnd_setup_set_activity_type(setup, UPISND_ACTIVITY_TYPE_MIDI_OUT);
		else
			return -EINVAL;
		upisnd_setup_set_element_type(setup, UPISND_ELEMENT_TYPE_ACTIVITY);
	} else if (strncasecmp(token, "gpio", 5u) == 0) {
		upisnd_setup_set_element_type(setup, UPISND_ELEMENT_TYPE_GPIO);
	} else {
		return -EINVAL;
	}

	int err = 0;

	upisnd_pin_t pin;
	enum upisnd_pin_direction_e dir;
	enum upisnd_pin_pull_e pull;
	bool output;

	switch (upisnd_setup_get_element_type(*setup)) {
	case UPISND_ELEMENT_TYPE_ANALOG_IN:
	case UPISND_ELEMENT_TYPE_ACTIVITY:
	case UPISND_ELEMENT_TYPE_GPIO:
	case UPISND_ELEMENT_TYPE_ENCODER:
		err = upisnd_parse_pin(&pin, &s, SEP);

		if (err != 0)
			break;

		upisnd_setup_set_pin_id(setup, pin);

		switch (upisnd_setup_get_element_type(*setup)) {
		case UPISND_ELEMENT_TYPE_ENCODER:
			err = upisnd_parse_gpio_input_pull(&pull, &s, SEP);
			if (err != 0)
				break;
			upisnd_setup_set_gpio_pull(setup, pull);

			err = upisnd_parse_pin(&pin, &s, SEP);
			if (err != 0)
				break;
			upisnd_setup_set_encoder_pin_b_id(setup, pin);

			err = upisnd_parse_gpio_input_pull(&pull, &s, SEP);
			if (err == 0)
				upisnd_setup_set_encoder_pin_b_pull(setup, pull);
			break;
		case UPISND_ELEMENT_TYPE_GPIO:
			err = upisnd_parse_gpio_dir(&dir, &s, SEP);
			if (err != 0)
				break;

			upisnd_setup_set_gpio_dir(setup, dir);

			switch (dir) {
			case UPISND_PIN_DIR_INPUT:
				err = upisnd_parse_gpio_input_pull(&pull, &s, SEP);
				if (err == 0)
					upisnd_setup_set_gpio_pull(setup, pull);
				break;
			case UPISND_PIN_DIR_OUTPUT:
				err = upisnd_parse_gpio_output_level(&output, &s, SEP);
				if (err == 0)
					upisnd_setup_set_gpio_output(setup, output);
				break;
			default:
				return -EINVAL;
			}
			break;
		default:
			break;
		}
		break;
	default:
		return -EINVAL;
	}

	if (err >= 0)
		err = upisnd_validate_setup(*setup);

	return err;
}

static ssize_t upisnd_element_type_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buf);
static ssize_t upisnd_element_value_show(struct kobject *kobj, struct kobj_attribute *attr,
					 char *buf);
static ssize_t upisnd_element_value_store(struct kobject *kobj, struct kobj_attribute *attr,
					  const char *buf, size_t len);
static ssize_t upisnd_element_input_min_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf);
static ssize_t upisnd_element_input_min_store(struct kobject *kobj, struct kobj_attribute *attr,
					      const char *buf, size_t len);
static ssize_t upisnd_element_input_max_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf);
static ssize_t upisnd_element_input_max_store(struct kobject *kobj, struct kobj_attribute *attr,
					      const char *buf, size_t len);
static ssize_t upisnd_element_value_low_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf);
static ssize_t upisnd_element_value_low_store(struct kobject *kobj, struct kobj_attribute *attr,
					      const char *buf, size_t len);
static ssize_t upisnd_element_value_high_show(struct kobject *kobj, struct kobj_attribute *attr,
					      char *buf);
static ssize_t upisnd_element_value_high_store(struct kobject *kobj, struct kobj_attribute *attr,
					       const char *buf, size_t len);
static ssize_t upisnd_element_value_mode_show(struct kobject *kobj, struct kobj_attribute *attr,
					      char *buf);
static ssize_t upisnd_element_value_mode_store(struct kobject *kobj, struct kobj_attribute *attr,
					       const char *buf, size_t len);
static ssize_t upisnd_element_direction_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf);
static ssize_t upisnd_element_pin_show(struct kobject *kobj, struct kobj_attribute *attr,
				       char *buf);
static ssize_t upisnd_element_pin_name_show(struct kobject *kobj, struct kobj_attribute *attr,
					    char *buf);
static ssize_t upisnd_element_pin_b_show(struct kobject *kobj, struct kobj_attribute *attr,
					 char *buf);
static ssize_t upisnd_element_pin_b_name_show(struct kobject *kobj, struct kobj_attribute *attr,
					      char *buf);
static ssize_t upisnd_element_activity_type_show(struct kobject *kobj, struct kobj_attribute *attr,
						 char *buf);
static ssize_t upisnd_element_gpio_export(struct kobject *kobj, struct kobj_attribute *attr,
					  const char *buf, size_t len);
static ssize_t upisnd_element_gpio_unexport(struct kobject *kobj, struct kobj_attribute *attr,
					    const char *buf, size_t len);

static struct kobj_attribute upisnd_element_type_attr = __ATTR(type, 0444, upisnd_element_type_show,
	NULL);
static struct kobj_attribute upisnd_element_value_attr = __ATTR(value, 0664,
	upisnd_element_value_show, upisnd_element_value_store);
static struct kobj_attribute upisnd_element_input_min_attr = __ATTR(input_min, 0664,
	upisnd_element_input_min_show, upisnd_element_input_min_store);
static struct kobj_attribute upisnd_element_input_max_attr = __ATTR(input_max, 0664,
	upisnd_element_input_max_show, upisnd_element_input_max_store);
static struct kobj_attribute upisnd_element_value_low_attr = __ATTR(value_low, 0664,
	upisnd_element_value_low_show, upisnd_element_value_low_store);
static struct kobj_attribute upisnd_element_value_high_attr = __ATTR(value_high, 0664,
	upisnd_element_value_high_show, upisnd_element_value_high_store);
static struct kobj_attribute upisnd_element_value_mode_attr = __ATTR(value_mode, 0664,
	upisnd_element_value_mode_show, upisnd_element_value_mode_store);
static struct kobj_attribute upisnd_element_direction_attr = __ATTR(direction, 0444,
	upisnd_element_direction_show, NULL);
static struct kobj_attribute upisnd_element_pin_attr = __ATTR(pin, 0444, upisnd_element_pin_show,
	NULL);
static struct kobj_attribute upisnd_element_pin_name_attr = __ATTR(pin_name, 0444,
	upisnd_element_pin_name_show, NULL);
static struct kobj_attribute upisnd_element_pin_b_attr = __ATTR(pin_b, 0444,
	upisnd_element_pin_b_show, NULL);
static struct kobj_attribute upisnd_element_pin_b_name_attr = __ATTR(pin_b_name, 0444,
	upisnd_element_pin_b_name_show, NULL);
static struct kobj_attribute upisnd_element_activity_type_attr = __ATTR(activity_type, 0444,
	upisnd_element_activity_type_show, NULL);
static struct kobj_attribute upisnd_element_gpio_export_attr = __ATTR(gpio_export, 0220, NULL,
	upisnd_element_gpio_export);
static struct kobj_attribute upisnd_element_gpio_unexport_attr = __ATTR(gpio_unexport, 0220, NULL,
	upisnd_element_gpio_unexport);

static struct attribute *upisnd_element_attrs[] = {
	&upisnd_element_type_attr.attr,
	&upisnd_element_value_attr.attr,
	&upisnd_element_input_min_attr.attr,
	&upisnd_element_input_max_attr.attr,
	&upisnd_element_value_low_attr.attr,
	&upisnd_element_value_high_attr.attr,
	&upisnd_element_value_mode_attr.attr,
	&upisnd_element_direction_attr.attr,
	&upisnd_element_pin_attr.attr,
	&upisnd_element_pin_name_attr.attr,
	&upisnd_element_pin_b_attr.attr,
	&upisnd_element_pin_b_name_attr.attr,
	&upisnd_element_activity_type_attr.attr,
	&upisnd_element_gpio_export_attr.attr,
	&upisnd_element_gpio_unexport_attr.attr,
	NULL
};

static umode_t upisnd_element_attr_is_visible(struct kobject *kobj, struct attribute *attr, int idx)
{
	printd("Hi!");

	struct upisnd_element *element = to_element(kobj);
	struct upisnd_config *cfg = to_config(kobj->parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	enum upisnd_element_type_e t = upisnd_setup_get_element_type(instance->gpio.pin_configs
		[element->gpio_pins[0]].setup);

	switch (t) {
	case UPISND_ELEMENT_TYPE_ENCODER:
		if (attr == &upisnd_element_value_mode_attr.attr ||
		    attr == &upisnd_element_pin_b_attr.attr ||
			attr == &upisnd_element_pin_b_name_attr.attr)
			return attr->mode;
		fallthrough;
	case UPISND_ELEMENT_TYPE_ANALOG_IN:
		if (attr == &upisnd_element_input_min_attr.attr ||
		    attr == &upisnd_element_input_max_attr.attr ||
			attr == &upisnd_element_value_low_attr.attr ||
			attr == &upisnd_element_value_high_attr.attr)
			return attr->mode;
		break;
	case UPISND_ELEMENT_TYPE_GPIO:
		if (attr == &upisnd_element_gpio_export_attr.attr ||
		    attr == &upisnd_element_gpio_unexport_attr.attr ||
			attr == &upisnd_element_direction_attr.attr)
			return attr->mode;
		break;
	default:
		break;
	}

	switch (t) {
	case UPISND_ELEMENT_TYPE_ANALOG_IN:
	case UPISND_ELEMENT_TYPE_ENCODER:
	case UPISND_ELEMENT_TYPE_GPIO:
		if (attr == &upisnd_element_value_attr.attr) {
			if (t == UPISND_ELEMENT_TYPE_GPIO) {
				if (upisnd_setup_get_gpio_dir(instance->gpio.pin_configs
				    [element->gpio_pins[0]].setup) == UPISND_PIN_DIR_INPUT)
					return 0444;
			}
			return attr->mode;
		}
		break;
	case UPISND_ELEMENT_TYPE_ACTIVITY:
		if (attr == &upisnd_element_activity_type_attr.attr)
			return attr->mode;
		break;
	case UPISND_ELEMENT_TYPE_NONE:
	default:
		break;
	}

	if (attr == &upisnd_element_type_attr.attr ||
	    attr == &upisnd_element_pin_attr.attr ||
		attr == &upisnd_element_pin_name_attr.attr)
		return attr->mode;

	return 0;
}

static struct attribute_group upisnd_element_group = {
	.attrs = upisnd_element_attrs,
	.is_visible = &upisnd_element_attr_is_visible
};

static struct upisnd_element *upisnd_create_element(struct upisnd_config *config, const char *name)
{
	struct upisnd_element *el = kzalloc(sizeof(*el), GFP_KERNEL);

	if (!el) {
		printe("Failed allocating upisnd_element!");
		return NULL;
	}

	el->kobj.kset = config->elements;
	el->gpio_pins[0] = UPISND_PIN_INVALID;
	el->gpio_pins[1] = UPISND_PIN_INVALID;

	int err = kobject_init_and_add(&el->kobj, &upisnd_element_type, NULL, "%s", name);

	if (err < 0) {
		printe("Failed initializing upisnd_element kobject! (err=%d, name='%s')",
		       err, name);
		goto cleanup;
	}

	return el;

cleanup:
	kobject_put(&el->kobj);
	kfree(el);
	return NULL;
}

static void upisnd_element_release(struct kobject *kobj)
{
	printd("hi");
	struct upisnd_element *element = to_element(kobj);

	kfree(element);
}

static int upisnd_setup_get_pins(int pins[2], upisnd_setup_t setup)
{
	pins[0] = UPISND_PIN_INVALID;
	pins[1] = UPISND_PIN_INVALID;

	switch (upisnd_setup_get_element_type(setup)) {
	case UPISND_ELEMENT_TYPE_ENCODER:
		pins[1] = upisnd_setup_get_encoder_pin_b_id(setup);
		if (!upisnd_is_pin_valid(pins[1]))
			pins[1] = UPISND_PIN_INVALID;
		fallthrough;
	case UPISND_ELEMENT_TYPE_ANALOG_IN:
	case UPISND_ELEMENT_TYPE_GPIO:
	case UPISND_ELEMENT_TYPE_NONE:
	case UPISND_ELEMENT_TYPE_ACTIVITY:
		pins[0] = upisnd_setup_get_pin_id(setup);
		if (!upisnd_is_pin_valid(pins[0]))
			pins[0] = UPISND_PIN_INVALID;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int upisnd_setup_do(struct upisnd_instance *instance, struct upisnd_element *element,
			   upisnd_setup_t setup, int pins[2])
{
	int err;

	element->raw_value = 0;
	element->value = 0;
	element->input_min = 0;
	element->value_low = 0;
	element->input_max = upisnd_setup_get_element_type(setup) !=
			     UPISND_ELEMENT_TYPE_ENCODER ? 1023 : 23;
	element->value_high = element->input_max;
	element->value_mode = UPISND_VALUE_MODE_CLAMP;

	int i;

	for (i = 0; i < 2; ++i) {
		if (pins[i] == UPISND_PIN_INVALID)
			break;

		int pin = pins[i];

		printd("Check %d %08x %d %p %p", pin, instance->gpio.pin_configs[pin].setup,
		       upisnd_setup_get_element_type(instance->gpio.pin_configs[pin].setup),
		       instance->gpio.pin_configs[pin].element, element);

		if (upisnd_setup_get_element_type(instance->gpio.pin_configs[pin].setup) !=
			UPISND_ELEMENT_TYPE_NONE && instance->gpio.pin_configs[pin].element !=
			element) {
			printe("Pin %s already used via %s%s!",
			       upisnd_pin_name(pin),
			       instance->gpio.pin_configs[pin].element ? "sysfs " : "gpio",
			       instance->gpio.pin_configs[pin].element ?
			       instance->gpio.pin_configs[pin].element->kobj.name : "");
			return -EBUSY;
		}
	}

	// Unsetup previous config if necessary.
	for (i = 0; i < 2; ++i) {
		if (element->gpio_pins[i] != UPISND_PIN_INVALID &&
		    element->gpio_pins[i] != pins[i]) {
			upisnd_setup_t s = instance->gpio.pin_configs[element->gpio_pins[i]].setup;

			upisnd_setup_set_element_type(&s, UPISND_ELEMENT_TYPE_NONE);
			upisnd_gpio_setup(instance, s);
			instance->gpio.pin_configs[element->gpio_pins[i]].setup = s;
			instance->gpio.pin_subscription_refcounts[element->gpio_pins[i]] = 0;
		}
	}

	err = upisnd_gpio_setup(instance, setup);

	if (err < 0)
		return err;

	instance->gpio.pin_configs[pins[0]].element = element;
	instance->gpio.pin_configs[pins[0]].flags = 0;
	instance->gpio.pin_configs[pins[0]].setup = setup;
	instance->gpio.pin_configs[pins[0]].gpio_desc = NULL;

	element->gpio_pins[0] = pins[0];
	element->gpio_pins[1] = pins[1];

	switch (upisnd_setup_get_element_type(setup)) {
	case UPISND_ELEMENT_TYPE_ENCODER:
		upisnd_gpio_set_subscription(instance, pins[0], true);
		upisnd_gpio_set_subscription(instance, pins[1], true);
		instance->gpio.pin_configs[pins[1]].element = element;
		instance->gpio.pin_configs[pins[1]].flags = UPISND_PIN_FLAG_IS_ENCODER_B;
		instance->gpio.pin_configs[pins[1]].setup = setup;
		instance->gpio.pin_configs[pins[1]].gpio_desc = NULL;
		break;
	case UPISND_ELEMENT_TYPE_GPIO:
		if (upisnd_setup_get_gpio_dir(setup) != UPISND_PIN_DIR_INPUT)
			break;
		fallthrough;
	default:
		upisnd_gpio_set_subscription(instance, pins[0], true);
		break;
	}

	return 0;
}

static ssize_t upisnd_setup(struct kobject *kobj, struct kobj_attribute *attr, const char *buf,
			    size_t length)
{
	if (length + 1 >= MAX_SETUP_REQUEST_LENGTH) {
		printe("Request exceeds maximum length!");
		return -EINVAL;
	}

	int err;

	char b[MAX_SETUP_REQUEST_LENGTH];

	strscpy(b, buf, length + 1);

	char *s = b;
	char *name = strsep(&s, " /\n\t");

	if (!name || *name == '\0' || !s || *s == '\0')
		return -EINVAL;

	upisnd_setup_t setup;

	err = upisnd_parse_setup(&setup, s, length - (s - name));
	if (err < 0) {
		printe("Failed parsing setup request! (%d)", err);
		return err;
	}

	int pins[2];

	upisnd_setup_get_pins(pins, setup);

	struct upisnd_config *cfg = to_config(kobj);

	struct upisnd_instance *instance = cfg->instance;

	down_write(&instance->rw_gpio_config_sem);

	struct kobject *existing = kset_find_obj(cfg->elements, name);

	if (existing) {
		bool setup_matches = false;
		struct upisnd_element *el = to_element(existing);

		if (upisnd_is_pin_valid(el->gpio_pins[0]))
			setup_matches = instance->gpio.pin_configs[el->gpio_pins[0]].setup == setup;

		kobject_put(existing);
		up_write(&instance->rw_gpio_config_sem);

		if (setup_matches) {
			printd("%s already existed and requested setup matched, returning success.",
			       name);
			return length;
		}

		printe("%s is already setup! (%d)", name, -EEXIST);
		return -EEXIST;
	}

	struct upisnd_element *element = upisnd_create_element(cfg, name);

	if (!element) {
		up_write(&instance->rw_gpio_config_sem);
		return -ENOMEM;
	}

	err = upisnd_setup_do(instance, element, setup, pins);

	if (err >= 0) {
		err = sysfs_create_group(&element->kobj, &upisnd_element_group);
		if (err < 0)
			printe
			  ("Failed creating pisound-micro element attributes! (err=%d, name ='%s')",
			   err, name);
	}

	up_write(&instance->rw_gpio_config_sem);

	if (err < 0) {
		kobject_put(&element->kobj);
		return err;
	}

	kobject_uevent(&element->kobj, KOBJ_ADD);
	return length;
}

static ssize_t upisnd_unsetup(struct kobject *kobj, struct kobj_attribute *attr, const char *buf,
			      size_t length)
{
	if (length + 1 >= MAX_ELEMENT_NAME_LENGTH) {
		printe("Element name exceeds maximum length!");
		return -EINVAL;
	}

	char b[MAX_ELEMENT_NAME_LENGTH];

	strscpy(b, buf, length + 1);

	char *s = b;
	char *name = strsep(&s, " /\n\t");

	if (!name || *name == '\0')
		return -EINVAL;

	struct upisnd_config *cfg = to_config(kobj);

	down_write(&cfg->instance->rw_gpio_config_sem);

	struct kobject *existing = kset_find_obj(cfg->elements, name);

	if (!existing) {
		printe("%s not found!", name);
		up_write(&cfg->instance->rw_gpio_config_sem);
		return -ENOENT;
	}

	upisnd_element_cleanup(existing, cfg->instance);

	printd("Found %p", existing);
	kobject_put(existing); // kset_find_obj increments the
	kobject_put(existing); // refcount, so we have to put twice.

	up_write(&cfg->instance->rw_gpio_config_sem);

	return length;
}

static ssize_t upisnd_adc_gain_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct upisnd_config *cfg = to_config(kobj);

	s32 adc_gain;
	int err = upisnd_comm_get_value(cfg->instance, UPISND_VALUE_ADC_GAIN, &adc_gain);

	if (err < 0) {
		printe("Failed getting ADC gain value! (%d)", err);
		return err;
	}

	return sprintf(buf, "%d\n", adc_gain);
}

static ssize_t upisnd_adc_gain_store(struct kobject *kobj, struct kobj_attribute *attr,
				     const char *buf, size_t length)
{
	struct upisnd_config *cfg = to_config(kobj);

	int adc_gain;
	int err = kstrtoint(buf, 10, &adc_gain);

	if (err < 0) {
		printe("Failed parsing ADC gain! (%d)", err);
		return err;
	}

	err = upisnd_comm_set_value(cfg->instance, UPISND_VALUE_ADC_GAIN, adc_gain);
	if (err < 0) {
		printe("Failed setting ADC gain value! (%d)", err);
		return err;
	}

	return length;
}

static ssize_t upisnd_adc_offset_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct upisnd_config *cfg = to_config(kobj);

	s32 adc_offset;
	int err = upisnd_comm_get_value(cfg->instance, UPISND_VALUE_ADC_OFFSET, &adc_offset);

	if (err < 0) {
		printe("Failed getting ADC offset value! (%d)", err);
		return err;
	}

	return sprintf(buf, "%d\n", adc_offset);
}

static ssize_t upisnd_adc_offset_store(struct kobject *kobj, struct kobj_attribute *attr,
				       const char *buf, size_t length)
{
	struct upisnd_config *cfg = to_config(kobj);

	int adc_offset;
	int err = kstrtoint(buf, 10, &adc_offset);

	if (err < 0) {
		printe("Failed parsing ADC offset! (%d)", err);
		return err;
	}

	err = upisnd_comm_set_value(cfg->instance, UPISND_VALUE_ADC_OFFSET, adc_offset);
	if (err < 0) {
		printe("Failed setting ADC offset value! (%d)", err);
		return err;
	}

	return length;
}

static struct kobj_attribute upisnd_setup_attr = __ATTR(setup, 0220, NULL, upisnd_setup);
static struct kobj_attribute upisnd_unsetup_attr = __ATTR(unsetup, 0220, NULL, upisnd_unsetup);
static struct kobj_attribute upisnd_adc_gain_attr = __ATTR(adc_gain, 0664, upisnd_adc_gain_show,
							   upisnd_adc_gain_store);
static struct kobj_attribute upisnd_adc_offset_attr = __ATTR(adc_offset, 0664,
							     upisnd_adc_offset_show,
							     upisnd_adc_offset_store);

static struct attribute *upisnd_root_attrs[] = {
	&upisnd_setup_attr.attr,
	&upisnd_unsetup_attr.attr,
	&upisnd_adc_gain_attr.attr,
	&upisnd_adc_offset_attr.attr,
	NULL
};

static umode_t upisnd_root_is_visible(struct kobject *kobj, struct attribute *attr, int idx)
{
	struct upisnd_config *cfg = to_config(kobj);
	struct upisnd_instance *instance = cfg->instance;

	if (!(instance->flags & UPISND_FLAG_ADC_CALIBRATION)) {
		if (attr == &upisnd_adc_gain_attr.attr || attr == &upisnd_adc_offset_attr.attr)
			return 0;
	}

	return attr->mode;
}

static const struct attribute_group upisnd_root_group = {
	.attrs = upisnd_root_attrs,
	.is_visible = &upisnd_root_is_visible
};

static struct upisnd_config *upisnd_create_config(struct upisnd_instance *instance,
						  const char *name)
{
	struct upisnd_config *cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);

	if (!cfg) {
		printe("Failed allocating upisnd_config!");
		return NULL;
	}

	printd("cfg=%p", cfg);

	cfg->instance = instance;

	int err = kobject_set_name(&cfg->kset.kobj, name);

	if (err < 0) {
		printe("Failed setting config kset name! (%d)", err);
		goto cleanup;
	}

	cfg->kset.kobj.ktype = &upisnd_config_type;
	err = kset_register(&cfg->kset);

	if (err < 0) {
		printe("Failed registering config kset! (%d)", err);
		goto cleanup;
	}

	cfg->kset.kobj.kset = &cfg->kset;

	cfg->elements = kset_create_and_add("elements", NULL, &cfg->kset.kobj);
	if (!cfg->elements) {
		printe("Failed creating elements kset!");
		goto cleanup;
	}

	err = sysfs_create_group(&cfg->kset.kobj, &upisnd_root_group);
	if (err < 0) {
		printe("Failed creating pisound-micro attributes! (%d)", err);
		goto cleanup;
	}

	err = kobject_uevent(&cfg->kset.kobj, KOBJ_ADD);
	printd("kobject_uevent(&cfg->kobj, KOBJ_ADD) = %d", err);

	return cfg;

cleanup:
	if (cfg->elements) {
		kset_unregister(cfg->elements);
		cfg->elements = NULL;
	}

	kset_unregister(&cfg->kset);
	return NULL;
}

static ssize_t upisnd_element_gpio_export(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf,
					  size_t len)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);
	struct upisnd_config *cfg = to_config(kobj->parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	struct gpio_desc *desc = NULL;

	int pin = element->gpio_pins[0];

	if (pin == UPISND_PIN_INVALID) {
		printe("Element %s is not set up yet!", kobj->name);
		return -EINVAL;
	}

	down_write(&instance->rw_gpio_config_sem);
	if (upisnd_setup_get_element_type(instance->gpio.pin_configs[pin].setup) !=
	    UPISND_ELEMENT_TYPE_GPIO) {
		printe("Element %s is not set up as a GPIO!", kobj->name);
		up_write(&instance->rw_gpio_config_sem);
		return -EINVAL;
	}

	instance->gpio.pin_configs[pin].flags |= UPISND_PIN_FLAG_CONFIGURING_THROUGH_SYSFS;
	up_write(&instance->rw_gpio_config_sem);

	int err = 0;

	printd("Requesting own %d", pin);
	desc = gpiochip_request_own_desc(&instance->gpio.gpio_chip, pin, kobj->name, 0, 0);
	if (IS_ERR(desc)) {
		printe("Failed requesting own GPIO desc for pin %d! (%ld)",
		       upisnd_setup_get_pin_id(instance->gpio.pin_configs[pin].setup),
		       PTR_ERR(desc));
		err = PTR_ERR(desc);
		desc = NULL;
	}

	if (err >= 0) {
		err = gpiod_export(desc, true);

		if (err < 0) {
			printe("Failed exporting GPIO via sysfs for pin %d! (%d)",
			       upisnd_setup_get_pin_id(instance->gpio.pin_configs[pin].setup), err);
			gpiochip_free_own_desc(desc);
			desc = NULL;
		}
	}

	printd("Result: %p %d", desc, err);

	down_write(&instance->rw_gpio_config_sem);
	instance->gpio.pin_configs[pin].flags = 0;
	instance->gpio.pin_configs[pin].gpio_desc = desc;
	up_write(&instance->rw_gpio_config_sem);

	return err >= 0 ? len : err;
}

static ssize_t upisnd_element_gpio_unexport(struct kobject *kobj, struct kobj_attribute *attr,
					    const char *buf, size_t len)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);
	struct upisnd_config *cfg = to_config(kobj->parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	int pin = element->gpio_pins[0];

	if (pin == UPISND_PIN_INVALID) {
		printe("Element %s is not set up yet!", kobj->name);
		return -EINVAL;
	}

	down_write(&instance->rw_gpio_config_sem);
	struct gpio_desc *desc = instance->gpio.pin_configs[pin].gpio_desc;

	if (!desc) {
		up_write(&instance->rw_gpio_config_sem);
		printe("Element %s is not exported via gpio sysfs!", kobj->name);
		return -EINVAL;
	}

	instance->gpio.pin_configs[pin].flags |= UPISND_PIN_FLAG_CONFIGURING_THROUGH_SYSFS;
	up_write(&instance->rw_gpio_config_sem);

	printd("Freeing own %d", pin);
	gpiochip_free_own_desc(desc);

	down_write(&instance->rw_gpio_config_sem);
	instance->gpio.pin_configs[pin].flags &= ~UPISND_PIN_FLAG_CONFIGURING_THROUGH_SYSFS;
	instance->gpio.pin_configs[pin].gpio_desc = NULL;
	up_write(&instance->rw_gpio_config_sem);

	return len;
}

static int upisnd_element_cleanup(struct kobject *kobj, struct upisnd_instance *instance)
{
	printd("(%p)", kobj);

	struct upisnd_element *element = to_element(kobj);

	if (element->gpio_pins[0] == UPISND_PIN_INVALID)
		return 0;

	upisnd_setup_t setup = instance->gpio.pin_configs[element->gpio_pins[0]].setup;

	if (upisnd_setup_get_element_type(setup) == UPISND_ELEMENT_TYPE_NONE) {
		printe("Pin %d is assigned, but the '%s' element is not set up",
		       element->gpio_pins[0], kobj->name);
		return 0;
	}

	if (instance->gpio.pin_configs[element->gpio_pins[0]].gpio_desc) {
		printd("Freeing own desc");
		struct gpio_desc *desc = instance->gpio.pin_configs[element->gpio_pins[0]]
					 .gpio_desc;

		instance->gpio.pin_configs[element->gpio_pins[0]].flags |=
			UPISND_PIN_FLAG_CONFIGURING_THROUGH_SYSFS;
		up_write(&instance->rw_gpio_config_sem);
		gpiochip_free_own_desc(desc);
		down_write(&instance->rw_gpio_config_sem);
		instance->gpio.pin_configs[element->gpio_pins[0]].flags &=
			~UPISND_PIN_FLAG_CONFIGURING_THROUGH_SYSFS;
		printd("Freed");
	}

	upisnd_setup_set_element_type(&setup, UPISND_ELEMENT_TYPE_NONE);
	int err = upisnd_gpio_setup(instance, setup);

	int i;

	for (i = 0; i < ARRAY_SIZE(element->gpio_pins); ++i) {
		if (element->gpio_pins[i] == UPISND_PIN_INVALID)
			break;

		memset(&instance->gpio.pin_configs[element->gpio_pins[i]], 0,
		       sizeof(struct upisnd_pin_config_t));
		instance->gpio.pin_subscription_refcounts[element->gpio_pins[i]] = 0;

		element->gpio_pins[i] = UPISND_PIN_INVALID;
	}

	return err;
}

static ssize_t upisnd_element_type_show(struct kobject *kobj, struct kobj_attribute *attr,
					char *buf)
{
	const char *type = NULL;
	struct upisnd_element *element = to_element(kobj);
	struct upisnd_config *cfg = to_config(kobj->parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	enum upisnd_element_type_e t = UPISND_ELEMENT_TYPE_NONE;

	if (element->gpio_pins[0] != UPISND_PIN_INVALID) {
		t = upisnd_setup_get_element_type(instance->gpio.pin_configs[element->gpio_pins[0]]
						  .setup);
	}

	switch (t) {
	case UPISND_ELEMENT_TYPE_ENCODER:
		type = "encoder";
		break;
	case UPISND_ELEMENT_TYPE_ANALOG_IN:
		type = "analog_in";
		break;
	case UPISND_ELEMENT_TYPE_GPIO:
		type = "gpio";
		break;
	case UPISND_ELEMENT_TYPE_ACTIVITY:
		type = "activity";
		break;
	case UPISND_ELEMENT_TYPE_NONE:
		type = "none";
		break;
	default:
		type = "unknown";
		break;
	}

	return sprintf(buf, "%s\n", type);
}

static ssize_t upisnd_element_value_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	struct upisnd_element *element = to_element(kobj);
	struct upisnd_config *cfg = to_config(kobj->parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	enum upisnd_element_type_e t = UPISND_ELEMENT_TYPE_NONE;
	int pin = UPISND_PIN_INVALID;

	if (element->gpio_pins[0] != UPISND_PIN_INVALID) {
		t = upisnd_setup_get_element_type(instance->gpio.pin_configs[element->gpio_pins[0]]
						  .setup);
		pin = element->gpio_pins[0];
	}

	switch (t) {
	case UPISND_ELEMENT_TYPE_ANALOG_IN:
	case UPISND_ELEMENT_TYPE_ENCODER:
		return sprintf(buf, "%d\n", element->value);
	case UPISND_ELEMENT_TYPE_GPIO:
		return sprintf(buf, "%d\n", upisnd_gpio_get(instance, pin));
	case UPISND_ELEMENT_TYPE_ACTIVITY:
	case UPISND_ELEMENT_TYPE_NONE:
	default:
		return 0;
	}
}

static ssize_t upisnd_element_value_store(struct kobject *kobj, struct kobj_attribute *attr,
					  const char *buf, size_t len)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);
	struct upisnd_config *cfg = to_config(kobj->parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	printd("%p %p", kobj, cfg);

	enum upisnd_element_type_e t = UPISND_ELEMENT_TYPE_NONE;
	int pin = UPISND_PIN_INVALID;

	if (element->gpio_pins[0] != UPISND_PIN_INVALID) {
		t = upisnd_setup_get_element_type(instance->gpio.pin_configs[element->gpio_pins[0]]
						  .setup);
		pin = element->gpio_pins[0];
	}

	int value, err;
	bool b;

	bool changed = false;

	switch (t) {
	case UPISND_ELEMENT_TYPE_NONE:
		return -EINVAL;
	case UPISND_ELEMENT_TYPE_GPIO:
		err = kstrtobool(buf, &b);
		if (err < 0) {
			printe("Failed parsing the provided boolean! (%d)", err);
			return err;
		}
		changed = b != upisnd_gpio_get(instance, pin);
		upisnd_gpio_set(instance, pin, b);
		break;
	case UPISND_ELEMENT_TYPE_ENCODER:
	case UPISND_ELEMENT_TYPE_ANALOG_IN:
		err = kstrtoint(buf, 0, &value);
		if (err < 0) {
			printe("Failed parsing the provided integer! (%d)", err);
			return err;
		}
		down_write(&instance->rw_gpio_config_sem);
		element->raw_value = upisnd_unmap_value_range(value, element->input_min,
							      element->input_max,
							      element->value_low,
							      element->value_high);
		changed = upisnd_element_update_value(element);
		up_write(&instance->rw_gpio_config_sem);
		break;
	case UPISND_ELEMENT_TYPE_ACTIVITY:
		return -EINVAL;
	}

	if (changed)
		sysfs_notify(kobj, NULL, "value");

	return len;
}

static ssize_t upisnd_element_input_min_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf)
{
	struct upisnd_element *element = to_element(kobj);

	return sprintf(buf, "%d\n", element->input_min);
}

static ssize_t upisnd_element_store_value_int(struct upisnd_element *element, int *value,
					      const char *buf, size_t len)
{
	int v;
	int err = kstrtoint(buf, 0, &v);

	if (err < 0) {
		printe("Failed parsing the provided integer! (%d)", err);
		return err;
	}

	struct upisnd_config *cfg = to_config(element->kobj.parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	down_write(&instance->rw_gpio_config_sem);
	*value = v;
	bool changed = upisnd_element_update_value(element);

	up_write(&instance->rw_gpio_config_sem);

	if (changed)
		sysfs_notify(&element->kobj, NULL, "value");

	return len;
}

static ssize_t upisnd_element_input_min_store(struct kobject *kobj, struct kobj_attribute *attr,
					      const char *buf, size_t len)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);
	int result = upisnd_element_store_value_int(element, &element->input_min, buf, len);

	if (result >= 0) {
		if (element->input_min > element->input_max) {
			int t = element->input_min;

			element->input_min = element->input_max;
			element->input_max = t;
		}
	}
	return result;
}

static ssize_t upisnd_element_input_max_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);

	return sprintf(buf, "%d\n", element->input_max);
}

static ssize_t upisnd_element_input_max_store(struct kobject *kobj, struct kobj_attribute *attr,
					      const char *buf, size_t len)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);
	int result = upisnd_element_store_value_int(element, &element->input_max, buf, len);

	if (result >= 0) {
		if (element->input_min > element->input_max) {
			int t = element->input_min;

			element->input_min = element->input_max;
			element->input_max = t;
		}
	}
	return result;
}

static ssize_t upisnd_element_value_low_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);

	return sprintf(buf, "%d\n", element->value_low);
}

static ssize_t upisnd_element_value_low_store(struct kobject *kobj, struct kobj_attribute *attr,
					      const char *buf, size_t len)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);

	return upisnd_element_store_value_int(element, &element->value_low, buf, len);
}

static ssize_t upisnd_element_value_high_show(struct kobject *kobj, struct kobj_attribute *attr,
					      char *buf)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);

	return sprintf(buf, "%d\n", element->value_high);
}

static ssize_t upisnd_element_value_high_store(struct kobject *kobj, struct kobj_attribute *attr,
					       const char *buf, size_t len)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);

	return upisnd_element_store_value_int(element, &element->value_high, buf, len);
}

static ssize_t upisnd_element_value_mode_show(struct kobject *kobj, struct kobj_attribute *attr,
					      char *buf)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);

	const char *s;

	switch (element->value_mode) {
	case UPISND_VALUE_MODE_CLAMP:
		s = "clamp";
		break;
	case UPISND_VALUE_MODE_WRAP:
		s = "wrap";
		break;
	default:
		return -EINVAL;
	}

	return sprintf(buf, "%s\n", s);
}

static ssize_t upisnd_element_value_mode_store(struct kobject *kobj, struct kobj_attribute *attr,
					       const char *buf, size_t len)
{
	if (!kobj || !kobj->parent || !kobj->parent->parent)
		return -EINVAL;

	static const char *const SEP = "\n\t ";

	char b[8];

	strscpy(b, buf, min(len + 1, sizeof(b)));

	char *s = b;
	char *token = strsep(&s, SEP);

	if (!token || *token == '\0')
		return -EINVAL;

	enum upisnd_value_mode mode;

	if (strncasecmp(token, "clamp", 6u) == 0)
		mode = UPISND_VALUE_MODE_CLAMP;
	else if (strncasecmp(token, "wrap", 5u) == 0)
		mode = UPISND_VALUE_MODE_WRAP;
	else
		return -EINVAL;

	struct upisnd_element *element = to_element(kobj);
	struct upisnd_config *cfg = to_config(element->kobj.parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	down_write(&instance->rw_gpio_config_sem);
	element->value_mode = mode;
	bool changed = upisnd_element_update_value(element);

	up_write(&instance->rw_gpio_config_sem);

	if (changed)
		sysfs_notify(&element->kobj, NULL, "value");

	return len;
}

static ssize_t upisnd_element_direction_show(struct kobject *kobj, struct kobj_attribute *attr,
					     char *buf)
{
	struct upisnd_element *element = to_element(kobj);
	struct upisnd_config *cfg = to_config(kobj->parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	enum upisnd_element_type_e t = UPISND_ELEMENT_TYPE_NONE;
	int pin = UPISND_PIN_INVALID;

	if (element->gpio_pins[0] != UPISND_PIN_INVALID) {
		t = upisnd_setup_get_element_type(instance->gpio.pin_configs[element->gpio_pins[0]]
						  .setup);
		pin = element->gpio_pins[0];
	}

	switch (t) {
	case UPISND_ELEMENT_TYPE_ENCODER:
		return sprintf(buf, "in\n");
	case UPISND_ELEMENT_TYPE_GPIO:
		return sprintf(buf, upisnd_gpio_get_direction(instance, pin) == 0 ?
			       "out\n" : "in\n");
	case UPISND_ELEMENT_TYPE_ACTIVITY:
		return sprintf(buf, "out\n");
	case UPISND_ELEMENT_TYPE_NONE:
	default:
		return 0;
	}
}

static ssize_t upisnd_element_pin_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct upisnd_element *element = to_element(kobj);

	if (element->gpio_pins[0] != UPISND_PIN_INVALID)
		return sprintf(buf, "%d\n", element->gpio_pins[0]);

	return -EINVAL;
}

static ssize_t upisnd_element_pin_name_show(struct kobject *kobj, struct kobj_attribute *attr,
					    char *buf)
{
	struct upisnd_element *element = to_element(kobj);

	if (element->gpio_pins[0] != UPISND_PIN_INVALID)
		return sprintf(buf, "%s\n", upisnd_pin_name(element->gpio_pins[0]));

	return -EINVAL;
}

static ssize_t upisnd_element_pin_b_show(struct kobject *kobj, struct kobj_attribute *attr,
					 char *buf)
{
	struct upisnd_element *element = to_element(kobj);

	if (element->gpio_pins[1] != UPISND_PIN_INVALID)
		return sprintf(buf, "%d\n", element->gpio_pins[1]);

	return -EINVAL;
}

static ssize_t upisnd_element_pin_b_name_show(struct kobject *kobj, struct kobj_attribute *attr,
					      char *buf)
{
	struct upisnd_element *element = to_element(kobj);

	if (element->gpio_pins[1] != UPISND_PIN_INVALID)
		return sprintf(buf, "%s\n", upisnd_pin_name(element->gpio_pins[1]));

	return -EINVAL;
}

static ssize_t upisnd_element_activity_type_show(struct kobject *kobj, struct kobj_attribute *attr,
						 char *buf)
{
	struct upisnd_element *element = to_element(kobj);

	struct upisnd_config *cfg = to_config(kobj->parent->parent);
	struct upisnd_instance *instance = cfg->instance;

	enum upisnd_element_type_e t = UPISND_ELEMENT_TYPE_NONE;

	if (element->gpio_pins[0] != UPISND_PIN_INVALID)
		t = upisnd_setup_get_element_type(instance->gpio.pin_configs[element->gpio_pins[0]]
						  .setup);

	if (t != UPISND_ELEMENT_TYPE_ACTIVITY)
		return -EINVAL;

	switch (upisnd_setup_get_activity_type(instance->gpio.pin_configs[element->gpio_pins[0]]
		.setup)) {
	case UPISND_ACTIVITY_TYPE_MIDI_IN:
		return sprintf(buf, "midi_in\n");
	case UPISND_ACTIVITY_TYPE_MIDI_OUT:
		return sprintf(buf, "midi_out\n");
	default:
		return 0;
	}
}

static void upisnd_sysfs_ctrl_event_handler(struct work_struct *work);

int upisnd_sysfs_init(struct upisnd_instance *instance, const char *name)
{
	instance->config = upisnd_create_config(instance, name ? name : "pisound-micro");
	if (!instance->config)
		return -ENOMEM;

	INIT_KFIFO(instance->ctrl_event_fifo);
	INIT_WORK(&instance->ctrl_event_handler, upisnd_sysfs_ctrl_event_handler);

	return 0;
}

void upisnd_sysfs_uninit(struct upisnd_instance *instance)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(instance->gpio.pin_configs); ++i) {
		if (instance->gpio.pin_configs[i].gpio_desc) {
			gpiochip_free_own_desc(instance->gpio.pin_configs[i].gpio_desc);
			instance->gpio.pin_configs[i].gpio_desc = NULL;
		}
	}

	kfifo_free(&instance->ctrl_event_fifo);

	if (instance->config) {
		struct kobject *k, *t;

		if (instance->config->elements) {
			list_for_each_entry_safe(k, t, &instance->config->elements->list, entry) {
				printd("Putting element %p", k);
				kobject_put(k);
			}

			printd("Unregistering elements set");
			kset_unregister(instance->config->elements);
			instance->config->elements = NULL;
		}

		instance->config->kset.kobj.kset = NULL;
		kset_unregister(&instance->config->kset);
		instance->config = NULL;
	}
}

static void upisnd_sysfs_ctrl_event_handler(struct work_struct *work)
{
	printd("ctrl event handler");
	struct upisnd_instance *instance = container_of(work, struct upisnd_instance,
							ctrl_event_handler);
	struct kobject *objs[UPISND_NUM_GPIOS];

	memset(objs, 0, sizeof(objs));

	unsigned int i, j = 0;
	int n;
	struct control_event_t events[32];

	down_write(&instance->rw_gpio_config_sem);
	while ((n = kfifo_out(&instance->ctrl_event_fifo, events, 32)) > 0) {
		printd("Got %d events", n);
		for (i = 0; i < n; ++i) {
			upisnd_pin_t pin = events[i].pin;

			if (!upisnd_is_pin_valid(pin)) {
				printe("Received invalid pin (%d), ignoring", pin);
				continue;
			}

			struct upisnd_element *element = instance->gpio.pin_configs[pin].element;

			if (element) {
				switch (upisnd_setup_get_element_type(instance->gpio.pin_configs
					[pin].setup)) {
				case UPISND_ELEMENT_TYPE_ENCODER:
					element->raw_value += (int16_t)events[i].raw_value;
					printd("Encoder %s, old raw: %d, new raw: %d",
					       element->kobj.name,
					       element->raw_value - (int16_t)events[i].raw_value,
					       element->raw_value);
					break;
				case UPISND_ELEMENT_TYPE_ANALOG_IN:
					element->raw_value = (uint16_t)(events[i].raw_value
							     & 0x03ff);
					printd("Analog in %s, raw: %d", element->kobj.name,
					       element->raw_value);
					break;
				case UPISND_ELEMENT_TYPE_GPIO:
					if (upisnd_setup_get_gpio_dir(instance->gpio
					    .pin_configs[pin].setup) == UPISND_PIN_DIR_INPUT) {
						element->raw_value = (uint16_t)events[i].raw_value;
						printd("Button %s, raw: %d", element->kobj.name,
						       element->raw_value);
						break;
					}
					break;
				default:
					printe
					("Got control event for non gpio, enc or analog element %s!"
					, element->kobj.name);
					continue;
				}

				objs[pin] = &element->kobj;
			}
		}
	}
	for (i = 0; i < ARRAY_SIZE(objs); ++i) {
		if (objs[i] && upisnd_element_update_value(to_element(objs[i])))
			objs[j++] = kobject_get(objs[i]);
	}
	up_write(&instance->rw_gpio_config_sem);

	for (i = 0; i < j; ++i) {
		printd("Notify %d %p", i, objs[i]);
		sysfs_notify(objs[i], NULL, "value");
		kobject_put(objs[i]);
	}

	if (!kfifo_is_empty(&instance->ctrl_event_fifo) &&
	    !work_pending(&instance->ctrl_event_handler))
		queue_work(instance->work_queue, &instance->ctrl_event_handler);
}

void upisnd_sysfs_handle_irq_event(struct upisnd_instance *instance,
				   const struct irq_event_t *events, unsigned int n)
{
	printd("Converting and pushing %u events", n);
	struct control_event_t cev;
	unsigned int i;

	for (i = 0; i < n; ++i) {
		cev.pin = events[i].num;
		cev.raw_value = events[i].high ? 1 : 0;
		kfifo_put(&instance->ctrl_event_fifo, cev);
	}
	if (!work_pending(&instance->ctrl_event_handler))
		queue_work(instance->work_queue, &instance->ctrl_event_handler);
}

void upisnd_sysfs_handle_control_event(struct upisnd_instance *instance,
				       const struct control_event_t *events, unsigned int n)
{
	printd("Pushing %u events", n);
	kfifo_in(&instance->ctrl_event_fifo, events, n);
	if (!work_pending(&instance->ctrl_event_handler))
		queue_work(instance->work_queue, &instance->ctrl_event_handler);
}

/* vim: set ts=8 sw=8 noexpandtab: */
