/*
 * Raspberry Pi Sense HAT joystick driver
 * http://raspberrypi.org
 *
 * Copyright (C) 2015 Raspberry Pi
 *
 * Author: Serge Schneider
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/module.h>

#include <linux/mfd/rpisense/joystick.h>
#include <linux/mfd/rpisense/core.h>

static struct rpisense *rpisense;
static unsigned char keymap[5] = {KEY_DOWN, KEY_RIGHT, KEY_UP, KEY_ENTER, KEY_LEFT,};

static void keys_work_fn(struct work_struct *work)
{
	int i;
	static s32 prev_keys;
	struct rpisense_js *rpisense_js = &rpisense->joystick;
	s32 keys = rpisense_reg_read(rpisense, RPISENSE_KEYS);
	s32 changes = keys ^ prev_keys;

	prev_keys = keys;
	for (i = 0; i < 5; i++) {
		if (changes & 1) {
			input_report_key(rpisense_js->keys_dev,
					 keymap[i], keys & 1);
		}
		changes >>= 1;
		keys >>= 1;
	}
	input_sync(rpisense_js->keys_dev);
}

static irqreturn_t keys_irq_handler(int irq, void *pdev)
{
	struct rpisense_js *rpisense_js = &rpisense->joystick;

	schedule_work(&rpisense_js->keys_work_s);
	return IRQ_HANDLED;
}

static int rpisense_js_probe(struct platform_device *pdev)
{
	int ret;
	int i;
	struct rpisense_js *rpisense_js;

	rpisense = rpisense_get_dev();
	rpisense_js = &rpisense->joystick;

	INIT_WORK(&rpisense_js->keys_work_s, keys_work_fn);

	rpisense_js->keys_dev = input_allocate_device();
	if (!rpisense_js->keys_dev) {
		dev_err(&pdev->dev, "Could not allocate input device.\n");
		return -ENOMEM;
	}

	rpisense_js->keys_dev->evbit[0] = BIT_MASK(EV_KEY);
	for (i = 0; i < ARRAY_SIZE(keymap); i++) {
		set_bit(keymap[i],
			rpisense_js->keys_dev->keybit);
	}

	rpisense_js->keys_dev->name = "Raspberry Pi Sense HAT Joystick";
	rpisense_js->keys_dev->phys = "rpi-sense-joy/input0";
	rpisense_js->keys_dev->id.bustype = BUS_I2C;
	rpisense_js->keys_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REP);
	rpisense_js->keys_dev->keycode = keymap;
	rpisense_js->keys_dev->keycodesize = sizeof(unsigned char);
	rpisense_js->keys_dev->keycodemax = ARRAY_SIZE(keymap);

	ret = input_register_device(rpisense_js->keys_dev);
	if (ret) {
		dev_err(&pdev->dev, "Could not register input device.\n");
		goto err_keys_alloc;
	}

	ret = gpiod_direction_input(rpisense_js->keys_desc);
	if (ret) {
		dev_err(&pdev->dev, "Could not set keys-int direction.\n");
		goto err_keys_reg;
	}

	rpisense_js->keys_irq = gpiod_to_irq(rpisense_js->keys_desc);
	if (rpisense_js->keys_irq < 0) {
		dev_err(&pdev->dev, "Could not determine keys-int IRQ.\n");
		ret = rpisense_js->keys_irq;
		goto err_keys_reg;
	}

	ret = devm_request_irq(&pdev->dev, rpisense_js->keys_irq,
			       keys_irq_handler, IRQF_TRIGGER_RISING,
			       "keys", &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "IRQ request failed.\n");
		goto err_keys_reg;
	}
	return 0;
err_keys_reg:
	input_unregister_device(rpisense_js->keys_dev);
err_keys_alloc:
	input_free_device(rpisense_js->keys_dev);
	return ret;
}

static int rpisense_js_remove(struct platform_device *pdev)
{
	struct rpisense_js *rpisense_js = &rpisense->joystick;

	input_unregister_device(rpisense_js->keys_dev);
	input_free_device(rpisense_js->keys_dev);
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id rpisense_js_id[] = {
	{ .compatible = "rpi,rpi-sense-js" },
	{ },
};
MODULE_DEVICE_TABLE(of, rpisense_js_id);
#endif

static struct platform_device_id rpisense_js_device_id[] = {
	{ .name = "rpi-sense-js" },
	{ },
};
MODULE_DEVICE_TABLE(platform, rpisense_js_device_id);

static struct platform_driver rpisense_js_driver = {
	.probe = rpisense_js_probe,
	.remove = rpisense_js_remove,
	.driver = {
		.name = "rpi-sense-js",
		.owner = THIS_MODULE,
	},
};

module_platform_driver(rpisense_js_driver);

MODULE_DESCRIPTION("Raspberry Pi Sense HAT joystick driver");
MODULE_AUTHOR("Serge Schneider <serge@raspberrypi.org>");
MODULE_LICENSE("GPL");
