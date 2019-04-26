/*
 * Copyright 2006, Johannes Berg <johannes@sipsolutions.net>
 * Copyright 2017, Russell Joyce <russell.joyce@york.ac.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/slab.h>
#include "led.h"

static void brcmfmac_rx_led_activate(struct led_classdev *led_cdev)
{
	struct brcmf_cfg80211_info *info = container_of(led_cdev->trigger,
						struct brcmf_cfg80211_info,
						rx_led);

	atomic_inc(&info->rx_led_active);
}

static void brcmfmac_rx_led_deactivate(struct led_classdev *led_cdev)
{
	struct brcmf_cfg80211_info *info = container_of(led_cdev->trigger,
						struct brcmf_cfg80211_info,
						rx_led);

	atomic_dec(&info->rx_led_active);
}

static void brcmfmac_tx_led_activate(struct led_classdev *led_cdev)
{
	struct brcmf_cfg80211_info *info = container_of(led_cdev->trigger,
						struct brcmf_cfg80211_info,
						tx_led);

	atomic_inc(&info->tx_led_active);
}

static void brcmfmac_tx_led_deactivate(struct led_classdev *led_cdev)
{
	struct brcmf_cfg80211_info *info = container_of(led_cdev->trigger,
						struct brcmf_cfg80211_info,
						tx_led);

	atomic_dec(&info->tx_led_active);
}

static void brcmfmac_rxtx_led_activate(struct led_classdev *led_cdev)
{
	struct brcmf_cfg80211_info *info = container_of(led_cdev->trigger,
						struct brcmf_cfg80211_info,
						rxtx_led);

	atomic_inc(&info->rxtx_led_active);
}

static void brcmfmac_rxtx_led_deactivate(struct led_classdev *led_cdev)
{
	struct brcmf_cfg80211_info *info = container_of(led_cdev->trigger,
						struct brcmf_cfg80211_info,
						rxtx_led);

	atomic_dec(&info->rxtx_led_active);
}

void brcmfmac_led_init(struct brcmf_cfg80211_info *info)
{
	info->rx_led.name = kasprintf(GFP_KERNEL, "%srx",
				      wiphy_name(info->wiphy));
	info->tx_led.name = kasprintf(GFP_KERNEL, "%stx",
				      wiphy_name(info->wiphy));
	info->rxtx_led.name = kasprintf(GFP_KERNEL, "%srxtx",
					wiphy_name(info->wiphy));

	atomic_set(&info->rx_led_active, 0);
	info->rx_led.activate = brcmfmac_rx_led_activate;
	info->rx_led.deactivate = brcmfmac_rx_led_deactivate;
	if (info->rx_led.name && led_trigger_register(&info->rx_led)) {
		kfree(info->rx_led.name);
		info->rx_led.name = NULL;
	}

	atomic_set(&info->tx_led_active, 0);
	info->tx_led.activate = brcmfmac_tx_led_activate;
	info->tx_led.deactivate = brcmfmac_tx_led_deactivate;
	if (info->tx_led.name && led_trigger_register(&info->tx_led)) {
		kfree(info->tx_led.name);
		info->tx_led.name = NULL;
	}

	atomic_set(&info->rxtx_led_active, 0);
	info->rxtx_led.activate = brcmfmac_rxtx_led_activate;
	info->rxtx_led.deactivate = brcmfmac_rxtx_led_deactivate;
	if (info->rxtx_led.name && led_trigger_register(&info->rxtx_led)) {
		kfree(info->rxtx_led.name);
		info->rxtx_led.name = NULL;
	}
}

void brcmfmac_led_exit(struct brcmf_cfg80211_info *info)
{
	if (info->rx_led.name)
		led_trigger_unregister(&info->rx_led);
	if (info->tx_led.name)
		led_trigger_unregister(&info->tx_led);
	if (info->rxtx_led.name)
		led_trigger_unregister(&info->rxtx_led);

	kfree(info->rx_led.name);
	kfree(info->tx_led.name);
	kfree(info->rxtx_led.name);
}
