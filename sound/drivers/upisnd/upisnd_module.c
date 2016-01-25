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

void upisnd_instance_release(struct kref *kref)
{
	struct upisnd_instance *instance = container_of(kref, struct upisnd_instance, refcount);

	if (instance->codec_dev) {
		put_device(instance->codec_dev);
		instance->codec_dev = NULL;
	}

	if (instance->ctrl_dev) {
		instance->ctrl_dev->platform_data = NULL;
		put_device(instance->ctrl_dev);
		instance->ctrl_dev = NULL;
	}

	if (instance->sound_card.dev) {
		snd_soc_unregister_card(&instance->sound_card);
		memset(&instance->sound_card, 0, sizeof(instance->sound_card));
	}

	if (instance->work_queue) {
		flush_workqueue(instance->work_queue);
		destroy_workqueue(instance->work_queue);
		instance->work_queue = NULL;
	}

	printd("Releasing instance %p", instance);
	kfree(instance);
}

static int of_dev_node_match(struct device *dev, const void *data)
{
	return dev->of_node == data;
}

static int upisnd_probe(struct platform_device *pdev)
{
	printd("Load %p", pdev);

	int err = 0;
	struct device_node *node;
	struct device_node *i2c_node;
	struct device *dev;

	struct upisnd_instance *instance = pdev->dev.platform_data;

	if (!instance) {
		instance = kzalloc(sizeof(*instance), GFP_KERNEL);

		if (!instance) {
			printe("Failed to allocate instance!");
			err = -ENOMEM;
			goto cleanup;
		}
		kref_init(&instance->refcount);

		pdev->dev.platform_data = instance;
		instance->pdev = pdev;

		init_rwsem(&instance->rw_gpio_config_sem);

		instance->work_queue = create_singlethread_workqueue("upisnd_workqueue");
		if (!instance->work_queue) {
			printe("Failed creating single thread work queue!");
			err = -ENOMEM;
			goto cleanup;
		}
	}

	node = pdev->dev.of_node;
	if (!node) {
		printe("Device node not found!");
		err = -ENODEV;
		goto cleanup;
	}

	i2c_node = of_parse_phandle(node, "ctrl", 0);
	if (!i2c_node) {
		printe("Failed to read 'ctrl' node!");
		err = -ENODEV;
		goto cleanup;
	}

	dev = bus_find_device(&i2c_bus_type, NULL, i2c_node, of_dev_node_match);
	of_node_put(i2c_node);

	if (!dev) {
		printe("Failed to find 'ctrl' device (%pOF)!", i2c_node);
		err = -ENODEV;
		goto cleanup;
	}

	bool got_ref = false;

	if (!dev->platform_data) {
		kref_get(&instance->refcount);
		dev->platform_data = instance;
		got_ref = true;
	}
	instance->ctrl_dev = dev;

	if (instance->ctrl.serial[0] == '\0') {
		printd("Deferring probe until serial is retrieved!");
		return -EPROBE_DEFER;
	}

	i2c_node = of_parse_phandle(node, "codec", 0);

	if (!i2c_node) {
		printe("Failed to read 'codec' node!");
		err = -ENODEV;
		goto cleanup;
	}

	dev = bus_find_device(&i2c_bus_type, NULL, i2c_node, of_dev_node_match);
	of_node_put(i2c_node);

	if (!dev) {
		printe("Failed to find 'codec' device!");
		err = -ENODEV;
		goto cleanup;
	}

	instance->codec_dev = dev;

	err = upisnd_sound_init(pdev, instance);
	if (err != 0) {
		if (err != -EPROBE_DEFER)
			printe("Failed initializing sound card! (%d)", err);
		goto cleanup;
	}

cleanup:
	if (err != 0) {
		if (err != -EPROBE_DEFER)
			printe("Error %d!", err);

		if (instance) {
			if (got_ref) {
				instance->ctrl_dev->platform_data = NULL;
				kref_put(&instance->refcount, &upisnd_instance_release);
			}

			if (instance->codec_dev) {
				put_device(instance->codec_dev);
				instance->codec_dev = NULL;
			}
		}
	}

	return err;
}

static void upisnd_remove(struct platform_device *pdev)
{
	printd("Unload %p", pdev);
	struct upisnd_instance *instance = dev_get_platdata(&pdev->dev);

	kref_put(&instance->refcount, &upisnd_instance_release);
	pdev->dev.platform_data = NULL;
}

static const struct of_device_id upisnd_of_match[] = {
	{.compatible = "blokas,pisound-micro" },
	{}
};

static const struct of_device_id upisnd_ctrl_of_match[] = {
	{.compatible = "blokas,upisnd-ctrl" },
	{}
};

static struct platform_driver upisnd_driver = {
	.driver = {
		.name = "snd-pisound-micro",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(upisnd_of_match),
	},
	.probe = upisnd_probe,
	.remove = upisnd_remove,
};

static const struct i2c_device_id upisnd_ctrl_idtable[] = {
	{ "blokas,upisnd-ctrl", 0 },
	{}
};

static struct i2c_driver upisnd_ctrl_driver = {
	.driver = {
		.name = "snd-pisound-micro-ctrl",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(upisnd_ctrl_of_match),
	},
	.id_table = upisnd_ctrl_idtable,
	.probe = upisnd_ctrl_probe,
	.remove = upisnd_ctrl_remove,
};

static int upisnd_module_init(void)
{
	int err, progress = 0;

	err = upisnd_comm_module_init();
	if (err != 0)
		goto cleanup;

	++progress;
	err = platform_driver_register(&upisnd_driver);
	if (err != 0)
		goto cleanup;

	++progress;
	err = i2c_add_driver(&upisnd_ctrl_driver);
	if (err != 0)
		goto cleanup;

cleanup:
	if (err) {
		printe("Error %d occurred, progress: %d", err, progress);
		switch (progress) {
		case 2:
			i2c_del_driver(&upisnd_ctrl_driver);
			fallthrough;
		case 1:
			platform_driver_unregister(&upisnd_driver);
			fallthrough;
		case 0:
			// No comm uninit.
			fallthrough;
		default:
			break;
		}
	}

	return err;
}

static void upisnd_module_exit(void)
{
	i2c_del_driver(&upisnd_ctrl_driver);
	platform_driver_unregister(&upisnd_driver);
}

module_init(upisnd_module_init);
module_exit(upisnd_module_exit);

MODULE_DEVICE_TABLE(of, upisnd_of_match);
MODULE_DEVICE_TABLE(of, upisnd_ctrl_of_match);
MODULE_DEVICE_TABLE(i2c, upisnd_ctrl_idtable);

MODULE_AUTHOR("Giedrius Trainavi\xc4\x8dius <giedrius@blokas.io>");
MODULE_DESCRIPTION("Audio, MIDI & I/O Driver for Pisound Micro, https://blokas.io/");
MODULE_LICENSE("GPL v2");

/* vim: set ts=8 sw=8 noexpandtab: */
