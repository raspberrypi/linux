// SPDX-License-Identifier: GPL-2.0
/* Copyright 2011 Broadcom Corporation.  All rights reserved. */

#include <linux/platform_device.h>

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of.h>

#include "bcm2835.h"
#include <soc/bcm2835/raspberrypi-firmware.h>

static bool enable_hdmi, enable_hdmi0, enable_hdmi1;
static bool enable_headphones;
static bool enable_compat_alsa = true;

module_param(enable_hdmi, bool, 0444);
MODULE_PARM_DESC(enable_hdmi, "Enables HDMI virtual audio device");
module_param(enable_headphones, bool, 0444);
MODULE_PARM_DESC(enable_headphones, "Enables Headphones virtual audio device");
module_param(enable_compat_alsa, bool, 0444);
MODULE_PARM_DESC(enable_compat_alsa,
		 "Enables ALSA compatibility virtual audio device");

static void bcm2835_devm_free_vchi_ctx(struct device *dev, void *res)
{
	struct bcm2835_vchi_ctx *vchi_ctx = res;

	bcm2835_free_vchi_ctx(vchi_ctx);
}

static int bcm2835_devm_add_vchi_ctx(struct device *dev)
{
	struct bcm2835_vchi_ctx *vchi_ctx;
	int ret;

	vchi_ctx = devres_alloc(bcm2835_devm_free_vchi_ctx, sizeof(*vchi_ctx),
				GFP_KERNEL);
	if (!vchi_ctx)
		return -ENOMEM;

	ret = bcm2835_new_vchi_ctx(dev, vchi_ctx);
	if (ret) {
		devres_free(vchi_ctx);
		return ret;
	}

	devres_add(dev, vchi_ctx);

	return 0;
}

typedef int (*bcm2835_audio_newpcm_func)(struct bcm2835_chip *chip,
					 const char *name,
					 enum snd_bcm2835_route route,
					 u32 numchannels);

typedef int (*bcm2835_audio_newctl_func)(struct bcm2835_chip *chip);

struct bcm2835_audio_driver {
	struct device_driver driver;
	const char *shortname;
	const char *longname;
	int minchannels;
	bcm2835_audio_newpcm_func newpcm;
	bcm2835_audio_newctl_func newctl;
	enum snd_bcm2835_route route;
};

static int bcm2835_audio_alsa_newpcm(struct bcm2835_chip *chip,
				     const char *name,
				     enum snd_bcm2835_route route,
				     u32 numchannels)
{
	int err;

	err = snd_bcm2835_new_pcm(chip, "bcm2835 ALSA", 0, AUDIO_DEST_AUTO,
				  numchannels - 1, false);
	if (err)
		return err;

	err = snd_bcm2835_new_pcm(chip, "bcm2835 IEC958/HDMI", 1, AUDIO_DEST_HDMI0, 1, true);
	if (err)
		return err;

	err = snd_bcm2835_new_pcm(chip, "bcm2835 IEC958/HDMI1", 2, AUDIO_DEST_HDMI1, 1, true);
	if (err)
		return err;

	return 0;
}

static int bcm2835_audio_simple_newpcm(struct bcm2835_chip *chip,
				       const char *name,
				       enum snd_bcm2835_route route,
				       u32 numchannels)
{
	return snd_bcm2835_new_pcm(chip, name, 0, route, numchannels, false);
}

static struct bcm2835_audio_driver bcm2835_audio_alsa = {
	.driver = {
		.name = "bcm2835_alsa",
		.owner = THIS_MODULE,
	},
	.shortname = "bcm2835 ALSA",
	.longname  = "bcm2835 ALSA",
	.minchannels = 2,
	.newpcm = bcm2835_audio_alsa_newpcm,
	.newctl = snd_bcm2835_new_ctl,
};

static struct bcm2835_audio_driver bcm2835_audio_hdmi0 = {
	.driver = {
		.name = "bcm2835_hdmi",
		.owner = THIS_MODULE,
	},
	.shortname = "bcm2835 HDMI 1",
	.longname  = "bcm2835 HDMI 1",
	.minchannels = 1,
	.newpcm = bcm2835_audio_simple_newpcm,
	.newctl = snd_bcm2835_new_hdmi_ctl,
	.route = AUDIO_DEST_HDMI0
};

static struct bcm2835_audio_driver bcm2835_audio_hdmi1 = {
	.driver = {
		.name = "bcm2835_hdmi",
		.owner = THIS_MODULE,
	},
	.shortname = "bcm2835 HDMI 2",
	.longname  = "bcm2835 HDMI 2",
	.minchannels = 1,
	.newpcm = bcm2835_audio_simple_newpcm,
	.newctl = snd_bcm2835_new_hdmi_ctl,
	.route = AUDIO_DEST_HDMI1
};

static struct bcm2835_audio_driver bcm2835_audio_headphones = {
	.driver = {
		.name = "bcm2835_headphones",
		.owner = THIS_MODULE,
	},
	.shortname = "bcm2835 Headphones",
	.longname  = "bcm2835 Headphones",
	.minchannels = 1,
	.newpcm = bcm2835_audio_simple_newpcm,
	.newctl = snd_bcm2835_new_headphones_ctl,
	.route = AUDIO_DEST_HEADPHONES
};

struct bcm2835_audio_drivers {
	struct bcm2835_audio_driver *audio_driver;
	const bool *is_enabled;
};

static struct bcm2835_audio_drivers children_devices[] = {
	{
		.audio_driver = &bcm2835_audio_alsa,
		.is_enabled = &enable_compat_alsa,
	},
	{
		.audio_driver = &bcm2835_audio_hdmi0,
		.is_enabled = &enable_hdmi0,
	},
	{
		.audio_driver = &bcm2835_audio_hdmi1,
		.is_enabled = &enable_hdmi1,
	},
	{
		.audio_driver = &bcm2835_audio_headphones,
		.is_enabled = &enable_headphones,
	},
};

static void bcm2835_card_free(void *data)
{
	snd_card_free(data);
}

static int snd_add_child_device(struct device *dev,
				struct bcm2835_audio_driver *audio_driver,
				u32 numchans)
{
	struct bcm2835_chip *chip;
	struct snd_card *card;
	int err;

	err = snd_card_new(dev, -1, NULL, THIS_MODULE, sizeof(*chip), &card);
	if (err < 0) {
		dev_err(dev, "Failed to create card");
		return err;
	}

	chip = card->private_data;
	chip->card = card;
	chip->dev = dev;
	mutex_init(&chip->audio_mutex);

	chip->vchi_ctx = devres_find(dev,
				     bcm2835_devm_free_vchi_ctx, NULL, NULL);
	if (!chip->vchi_ctx) {
		err = -ENODEV;
		goto error;
	}

	strscpy(card->driver, audio_driver->driver.name, sizeof(card->driver));
	strscpy(card->shortname, audio_driver->shortname, sizeof(card->shortname));
	strscpy(card->longname, audio_driver->longname, sizeof(card->longname));

	err = audio_driver->newpcm(chip, audio_driver->shortname,
		audio_driver->route,
		numchans);
	if (err) {
		dev_err(dev, "Failed to create pcm, error %d\n", err);
		goto error;
	}

	err = audio_driver->newctl(chip);
	if (err) {
		dev_err(dev, "Failed to create controls, error %d\n", err);
		goto error;
	}

	err = snd_card_register(card);
	if (err) {
		dev_err(dev, "Failed to register card, error %d\n", err);
		goto error;
	}

	dev_set_drvdata(dev, chip);

	err = devm_add_action(dev, bcm2835_card_free, card);
	if (err < 0) {
		dev_err(dev, "Failed to add devm action, err %d\n", err);
		goto error;
	}

	dev_info(dev, "card created with %d channels\n", numchans);
	return 0;

 error:
	snd_card_free(card);
	return err;
}

static int snd_add_child_devices(struct device *device, u32 numchans)
{
	int extrachannels_per_driver = 0;
	int extrachannels_remainder = 0;
	int count_devices = 0;
	int extrachannels = 0;
	int minchannels = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(children_devices); i++)
		if (*children_devices[i].is_enabled)
			count_devices++;

	if (!count_devices)
		return 0;

	for (i = 0; i < ARRAY_SIZE(children_devices); i++)
		if (*children_devices[i].is_enabled)
			minchannels +=
				children_devices[i].audio_driver->minchannels;

	if (minchannels < numchans) {
		extrachannels = numchans - minchannels;
		extrachannels_per_driver = extrachannels / count_devices;
		extrachannels_remainder = extrachannels % count_devices;
	}

	dev_dbg(device, "minchannels %d\n", minchannels);
	dev_dbg(device, "extrachannels %d\n", extrachannels);
	dev_dbg(device, "extrachannels_per_driver %d\n",
		extrachannels_per_driver);
	dev_dbg(device, "extrachannels_remainder %d\n",
		extrachannels_remainder);

	for (i = 0; i < ARRAY_SIZE(children_devices); i++) {
		struct bcm2835_audio_driver *audio_driver;
		int numchannels_this_device;
		int err;

		if (!*children_devices[i].is_enabled)
			continue;

		audio_driver = children_devices[i].audio_driver;

		if (audio_driver->minchannels > numchans) {
			dev_err(device,
				"Out of channels, needed %d but only %d left\n",
				audio_driver->minchannels,
				numchans);
			continue;
		}

		numchannels_this_device =
			audio_driver->minchannels + extrachannels_per_driver +
			extrachannels_remainder;
		extrachannels_remainder = 0;

		numchans -= numchannels_this_device;

		err = snd_add_child_device(device, audio_driver,
					   numchannels_this_device);
		if (err)
			return err;
	}

	return 0;
}

static void set_hdmi_enables(struct device *dev)
{
	struct device_node *firmware_node;
	struct rpi_firmware *firmware;
	u32 num_displays, i, display_id;
	int ret;

	firmware_node = of_parse_phandle(dev->of_node, "brcm,firmware", 0);
	firmware = rpi_firmware_get(firmware_node);

	if (!firmware)
		return;

	of_node_put(firmware_node);

	ret = rpi_firmware_property(firmware,
				    RPI_FIRMWARE_FRAMEBUFFER_GET_NUM_DISPLAYS,
				    &num_displays, sizeof(u32));

	if (ret)
		return;

	for (i = 0; i < num_displays; i++) {
		display_id = i;
		ret = rpi_firmware_property(firmware,
				RPI_FIRMWARE_FRAMEBUFFER_GET_DISPLAY_ID,
				&display_id, sizeof(display_id));
		if (!ret) {
			if (display_id == 2)
				enable_hdmi0 = true;
			if (display_id == 7)
				enable_hdmi1 = true;
		}
	}

	if (!enable_hdmi0 && enable_hdmi1) {
		/* Swap them over and reassign route. This means
		 * that if we only have one connected, it is always named
		 *  HDMI1, irrespective of if its on port HDMI0 or HDMI1.
		 *  This should match with the naming of HDMI ports in DRM
		 */
		enable_hdmi0 = true;
		enable_hdmi1 = false;
		bcm2835_audio_hdmi0.route = AUDIO_DEST_HDMI1;
	}
}

static int snd_bcm2835_alsa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	u32 numchans;
	int err;

	err = of_property_read_u32(dev->of_node, "brcm,pwm-channels",
				   &numchans);
	if (err) {
		dev_err(dev, "Failed to get DT property 'brcm,pwm-channels'");
		return err;
	}

	if (numchans == 0 || numchans > MAX_SUBSTREAMS) {
		numchans = MAX_SUBSTREAMS;
		dev_warn(dev,
			 "Illegal 'brcm,pwm-channels' value, will use %u\n",
			 numchans);
	}

	if (!enable_compat_alsa) {
		if (!of_property_read_bool(dev->of_node, "brcm,disable-hdmi"))
			set_hdmi_enables(dev);

		// In this mode, always enable analog output
		enable_headphones = true;
	} else {
		enable_hdmi0 = enable_hdmi;
	}

	err = bcm2835_devm_add_vchi_ctx(dev);
	if (err)
		return err;

	err = snd_add_child_devices(dev, numchans);
	if (err)
		return err;

	return 0;
}

#ifdef CONFIG_PM

static int snd_bcm2835_alsa_suspend(struct platform_device *pdev,
				    pm_message_t state)
{
	return 0;
}

static int snd_bcm2835_alsa_resume(struct platform_device *pdev)
{
	return 0;
}

#endif

static const struct of_device_id snd_bcm2835_of_match_table[] = {
	{ .compatible = "brcm,bcm2835-audio",},
	{},
};
MODULE_DEVICE_TABLE(of, snd_bcm2835_of_match_table);

static struct platform_driver bcm2835_alsa_driver = {
	.probe = snd_bcm2835_alsa_probe,
#ifdef CONFIG_PM
	.suspend = snd_bcm2835_alsa_suspend,
	.resume = snd_bcm2835_alsa_resume,
#endif
	.driver = {
		.name = "bcm2835_audio",
		.of_match_table = snd_bcm2835_of_match_table,
	},
};
module_platform_driver(bcm2835_alsa_driver);

MODULE_AUTHOR("Dom Cobley");
MODULE_DESCRIPTION("Alsa driver for BCM2835 chip");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:bcm2835_audio");
