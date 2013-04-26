/*****************************************************************************
* Copyright 2011 Broadcom Corporation.  All rights reserved.
*
* Unless you and Broadcom execute a separate written software license
* agreement governing use of this software, this software is licensed to you
* under the terms of the GNU General Public License version 2, available at
* http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
*	
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a
* license other than the GPL, without Broadcom's express prior written
* consent.
*****************************************************************************/

#include <linux/platform_device.h>

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>

#include "bcm2835.h"

/* module parameters (see "Module Parameters") */
/* SNDRV_CARDS: maximum number of cards supported by this module */
static int index[MAX_SUBSTREAMS] = {[0 ... (MAX_SUBSTREAMS - 1)] = -1 };
static char *id[MAX_SUBSTREAMS] = {[0 ... (MAX_SUBSTREAMS - 1)] = NULL };
static int enable[MAX_SUBSTREAMS] = {[0 ... (MAX_SUBSTREAMS - 1)] = 1 };

/* HACKY global pointers needed for successive probes to work : ssp
 * But compared against the changes we will have to do in VC audio_ipc code
 * to export 8 audio_ipc devices as a single IPC device and then monitor all
 * four devices in a thread, this gets things done quickly and should be easier
 * to debug if we run into issues
 */

static struct snd_card *g_card = NULL;
static bcm2835_chip_t *g_chip = NULL;

static int snd_bcm2835_free(bcm2835_chip_t * chip)
{
	kfree(chip);
	return 0;
}

/* component-destructor
 * (see "Management of Cards and Components")
 */
static int snd_bcm2835_dev_free(struct snd_device *device)
{
	return snd_bcm2835_free(device->device_data);
}

/* chip-specific constructor
 * (see "Management of Cards and Components")
 */
static int snd_bcm2835_create(struct snd_card *card,
					struct platform_device *pdev,
					bcm2835_chip_t ** rchip)
{
	bcm2835_chip_t *chip;
	int err;
	static struct snd_device_ops ops = {
		.dev_free = snd_bcm2835_dev_free,
	};

	*rchip = NULL;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;

	chip->card = card;

	err = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops);
	if (err < 0) {
		snd_bcm2835_free(chip);
		return err;
	}

	*rchip = chip;
	return 0;
}

static int snd_bcm2835_alsa_probe(struct platform_device *pdev)
{
	static int dev;
	bcm2835_chip_t *chip;
	struct snd_card *card;
	int err;

	if (dev >= MAX_SUBSTREAMS)
		return -ENODEV;

	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}

	if (dev > 0)
		goto add_register_map;

	err = snd_card_create(index[dev], id[dev], THIS_MODULE, 0, &g_card);
	if (err < 0)
		goto out;

	snd_card_set_dev(g_card, &pdev->dev);
	strcpy(g_card->driver, "BRCM bcm2835 ALSA Driver");
	strcpy(g_card->shortname, "bcm2835 ALSA");
	sprintf(g_card->longname, "%s", g_card->shortname);

	err = snd_bcm2835_create(g_card, pdev, &chip);
	if (err < 0) {
		dev_err(&pdev->dev, "Failed to create bcm2835 chip\n");
		goto out_bcm2835_create;
	}

	g_chip = chip;
	err = snd_bcm2835_new_pcm(chip);
	if (err < 0) {
		dev_err(&pdev->dev, "Failed to create new BCM2835 pcm device\n");
		goto out_bcm2835_new_pcm;
	}

	err = snd_bcm2835_new_ctl(chip);
	if (err < 0) {
		dev_err(&pdev->dev, "Failed to create new BCM2835 ctl\n");
		goto out_bcm2835_new_ctl;
	}

add_register_map:
	card = g_card;
	chip = g_chip;

	BUG_ON(!(card && chip));

	chip->avail_substreams |= (1 << dev);
	chip->pdev[dev] = pdev;

	if (dev == 0) {
		err = snd_card_register(card);
		if (err < 0) {
			dev_err(&pdev->dev,
				"Failed to register bcm2835 ALSA card \n");
			goto out_card_register;
		}
		platform_set_drvdata(pdev, card);
		audio_info("bcm2835 ALSA card created!\n");
	} else {
		audio_info("bcm2835 ALSA chip created!\n");
		platform_set_drvdata(pdev, (void *)dev);
	}

	dev++;

	return 0;

out_card_register:
out_bcm2835_new_ctl:
out_bcm2835_new_pcm:
out_bcm2835_create:
	BUG_ON(!g_card);
	if (snd_card_free(g_card))
		dev_err(&pdev->dev, "Failed to free Registered alsa card\n");
	g_card = NULL;
out:
	dev = SNDRV_CARDS;	/* stop more avail_substreams from being probed */
	dev_err(&pdev->dev, "BCM2835 ALSA Probe failed !!\n");
	return err;
}

static int snd_bcm2835_alsa_remove(struct platform_device *pdev)
{
	uint32_t idx;
	void *drv_data;

	drv_data = platform_get_drvdata(pdev);

	if (drv_data == (void *)g_card) {
		/* This is the card device */
		snd_card_free((struct snd_card *)drv_data);
		g_card = NULL;
		g_chip = NULL;
	} else {
		idx = (uint32_t) drv_data;
		if (g_card != NULL) {
			BUG_ON(!g_chip);
			/* We pass chip device numbers in audio ipc devices
			 * other than the one we registered our card with
			 */
			idx = (uint32_t) drv_data;
			BUG_ON(!idx || idx > MAX_SUBSTREAMS);
			g_chip->avail_substreams &= ~(1 << idx);
			/* There should be atleast one substream registered
			 * after we are done here, as it wil be removed when
			 * the *remove* is called for the card device
			 */
			BUG_ON(!g_chip->avail_substreams);
		}
	}

	platform_set_drvdata(pdev, NULL);

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

static struct platform_driver bcm2835_alsa0_driver = {
	.probe = snd_bcm2835_alsa_probe,
	.remove = snd_bcm2835_alsa_remove,
#ifdef CONFIG_PM
	.suspend = snd_bcm2835_alsa_suspend,
	.resume = snd_bcm2835_alsa_resume,
#endif
	.driver = {
		   .name = "bcm2835_AUD0",
		   .owner = THIS_MODULE,
		   },
};

static struct platform_driver bcm2835_alsa1_driver = {
	.probe = snd_bcm2835_alsa_probe,
	.remove = snd_bcm2835_alsa_remove,
#ifdef CONFIG_PM
	.suspend = snd_bcm2835_alsa_suspend,
	.resume = snd_bcm2835_alsa_resume,
#endif
	.driver = {
		   .name = "bcm2835_AUD1",
		   .owner = THIS_MODULE,
		   },
};

static struct platform_driver bcm2835_alsa2_driver = {
	.probe = snd_bcm2835_alsa_probe,
	.remove = snd_bcm2835_alsa_remove,
#ifdef CONFIG_PM
	.suspend = snd_bcm2835_alsa_suspend,
	.resume = snd_bcm2835_alsa_resume,
#endif
	.driver = {
		   .name = "bcm2835_AUD2",
		   .owner = THIS_MODULE,
		   },
};

static struct platform_driver bcm2835_alsa3_driver = {
	.probe = snd_bcm2835_alsa_probe,
	.remove = snd_bcm2835_alsa_remove,
#ifdef CONFIG_PM
	.suspend = snd_bcm2835_alsa_suspend,
	.resume = snd_bcm2835_alsa_resume,
#endif
	.driver = {
		   .name = "bcm2835_AUD3",
		   .owner = THIS_MODULE,
		   },
};

static struct platform_driver bcm2835_alsa4_driver = {
	.probe = snd_bcm2835_alsa_probe,
	.remove = snd_bcm2835_alsa_remove,
#ifdef CONFIG_PM
	.suspend = snd_bcm2835_alsa_suspend,
	.resume = snd_bcm2835_alsa_resume,
#endif
	.driver = {
		   .name = "bcm2835_AUD4",
		   .owner = THIS_MODULE,
		   },
};

static struct platform_driver bcm2835_alsa5_driver = {
	.probe = snd_bcm2835_alsa_probe,
	.remove = snd_bcm2835_alsa_remove,
#ifdef CONFIG_PM
	.suspend = snd_bcm2835_alsa_suspend,
	.resume = snd_bcm2835_alsa_resume,
#endif
	.driver = {
		   .name = "bcm2835_AUD5",
		   .owner = THIS_MODULE,
		   },
};

static struct platform_driver bcm2835_alsa6_driver = {
	.probe = snd_bcm2835_alsa_probe,
	.remove = snd_bcm2835_alsa_remove,
#ifdef CONFIG_PM
	.suspend = snd_bcm2835_alsa_suspend,
	.resume = snd_bcm2835_alsa_resume,
#endif
	.driver = {
		   .name = "bcm2835_AUD6",
		   .owner = THIS_MODULE,
		   },
};

static struct platform_driver bcm2835_alsa7_driver = {
	.probe = snd_bcm2835_alsa_probe,
	.remove = snd_bcm2835_alsa_remove,
#ifdef CONFIG_PM
	.suspend = snd_bcm2835_alsa_suspend,
	.resume = snd_bcm2835_alsa_resume,
#endif
	.driver = {
		   .name = "bcm2835_AUD7",
		   .owner = THIS_MODULE,
		   },
};

static int bcm2835_alsa_device_init(void)
{
	int err;
	err = platform_driver_register(&bcm2835_alsa0_driver);
	if (err) {
		pr_err("Error registering bcm2835_alsa0_driver %d .\n", err);
		goto out;
	}

	err = platform_driver_register(&bcm2835_alsa1_driver);
	if (err) {
		pr_err("Error registering bcm2835_alsa0_driver %d .\n", err);
		goto unregister_0;
	}

	err = platform_driver_register(&bcm2835_alsa2_driver);
	if (err) {
		pr_err("Error registering bcm2835_alsa0_driver %d .\n", err);
		goto unregister_1;
	}

	err = platform_driver_register(&bcm2835_alsa3_driver);
	if (err) {
		pr_err("Error registering bcm2835_alsa0_driver %d .\n", err);
		goto unregister_2;
	}

	err = platform_driver_register(&bcm2835_alsa4_driver);
	if (err) {
		pr_err("Error registering bcm2835_alsa0_driver %d .\n", err);
		goto unregister_3;
	}

	err = platform_driver_register(&bcm2835_alsa5_driver);
	if (err) {
		pr_err("Error registering bcm2835_alsa0_driver %d .\n", err);
		goto unregister_4;
	}

	err = platform_driver_register(&bcm2835_alsa6_driver);
	if (err) {
		pr_err("Error registering bcm2835_alsa0_driver %d .\n", err);
		goto unregister_5;
	}

	err = platform_driver_register(&bcm2835_alsa7_driver);
	if (err) {
		pr_err("Error registering bcm2835_alsa0_driver %d .\n", err);
		goto unregister_6;
	}

	return 0;

unregister_6:
	platform_driver_unregister(&bcm2835_alsa6_driver);
unregister_5:
	platform_driver_unregister(&bcm2835_alsa5_driver);
unregister_4:
	platform_driver_unregister(&bcm2835_alsa4_driver);
unregister_3:
	platform_driver_unregister(&bcm2835_alsa3_driver);
unregister_2:
	platform_driver_unregister(&bcm2835_alsa2_driver);
unregister_1:
	platform_driver_unregister(&bcm2835_alsa1_driver);
unregister_0:
	platform_driver_unregister(&bcm2835_alsa0_driver);
out:
	return err;
}

static void bcm2835_alsa_device_exit(void)
{
	platform_driver_unregister(&bcm2835_alsa0_driver);
	platform_driver_unregister(&bcm2835_alsa1_driver);
	platform_driver_unregister(&bcm2835_alsa2_driver);
	platform_driver_unregister(&bcm2835_alsa3_driver);
	platform_driver_unregister(&bcm2835_alsa4_driver);
	platform_driver_unregister(&bcm2835_alsa5_driver);
	platform_driver_unregister(&bcm2835_alsa6_driver);
	platform_driver_unregister(&bcm2835_alsa7_driver);
}

late_initcall(bcm2835_alsa_device_init);
module_exit(bcm2835_alsa_device_exit);

MODULE_AUTHOR("Dom Cobley");
MODULE_DESCRIPTION("Alsa driver for BCM2835 chip");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:bcm2835_alsa");
