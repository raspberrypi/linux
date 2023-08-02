// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Driver for Raspberry Pi power button
 *
 * Copyright (C) 2023 Raspberry Pi Ltd.
 *
 * This driver is based on drivers/hwmon/raspberrypi-hwmon.c and
 * input/misc/pm8941-pwrkey.c/ - see original files for copyright information
 */

#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <dt-bindings/input/raspberrypi-button.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

struct rpi_button {
	struct device *dev;
	struct rpi_firmware *fw;
	struct input_dev *input;
	struct delayed_work poll_work;
	unsigned long poll_rate;
	const char *name;
	u32 id;
	u32 code;
};

static void button_poll(struct work_struct *work)
{
	struct rpi_button *button;
	u32 value;
	int err;

	button = container_of(work, struct rpi_button,
			      poll_work.work);

	value = BIT(button->id);
	err = rpi_firmware_property(button->fw, RPI_FIRMWARE_GET_BUTTONS_PRESSED,
				    &value, sizeof(value));
	if (err) {
		dev_err_once(button->dev, "GET_BUTTON_PRESSED not implemented?\n");
		return;
	}

	if (value & BIT(button->id)) {
		input_event(button->input, EV_KEY, button->code, 1);
		input_sync(button->input);
		input_event(button->input, EV_KEY, button->code, 0);
		input_sync(button->input);
	}

	schedule_delayed_work(&button->poll_work, button->poll_rate);
}

static int rpi_button_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rpi_button *button;
	int err;

	button = devm_kzalloc(dev, sizeof(*button), GFP_KERNEL);
	if (!button)
		return -ENOMEM;

	button->dev = dev;

	/* Get the firmware pointer from our parent */
	button->fw = dev_get_drvdata(dev->parent);

	if (device_property_read_u32(dev, "id", &button->id))
		button->id = RASPBERRYPI_BUTTON_POWER;

	if (device_property_read_string(dev, "label", &button->name))
		button->name = "raspberrypi-button";

	if (device_property_read_u32(dev, "linux,code", &button->code)) {
		dev_err(&pdev->dev, "no linux,code property\n");
		return -EINVAL;
	}

	button->input = devm_input_allocate_device(dev);
	if (!button->input) {
		dev_dbg(&pdev->dev, "unable to allocate input device\n");
		return -ENOMEM;
	}

	input_set_capability(button->input, EV_KEY, button->code);

	button->input->name = button->name;
	button->input->phys = "raspberrypi-button/input0";
	button->input->dev.parent = dev;
	button->poll_rate = HZ;

	err = input_register_device(button->input);
	if (err) {
		dev_err(&pdev->dev, "failed to register input device: %d\n",
			err);
		return err;
	}

	err = devm_delayed_work_autocancel(dev, &button->poll_work,
					   button_poll);
	if (err)
		return err;

	platform_set_drvdata(pdev, button);
	schedule_delayed_work(&button->poll_work, button->poll_rate);

	return 0;
}

static const struct of_device_id rpi_button_match[] = {
	{ .compatible = "raspberrypi,firmware-button", },
	{ }
};
MODULE_DEVICE_TABLE(of, rpi_button_match);

static struct platform_driver rpi_button_driver = {
	.probe = rpi_button_probe,
	.driver = {
		.name = "raspberrypi-button",
		.of_match_table = of_match_ptr(rpi_button_match),
	},
};
module_platform_driver(rpi_button_driver);

MODULE_AUTHOR("Phil Elwell <phil@raspberrypi.com>");
MODULE_DESCRIPTION("Raspberry Pi button driver");
MODULE_LICENSE("Dual BSD/GPL");
