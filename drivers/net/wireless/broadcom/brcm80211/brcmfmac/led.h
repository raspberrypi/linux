/*
 * Copyright 2006, Johannes Berg <johannes@sipsolutions.net>
 * Copyright 2017, Russell Joyce <russell.joyce@york.ac.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/atomic.h>
#include <linux/leds.h>
#include "cfg80211.h"

#define BRCMFMAC_BLINK_DELAY 50 /* ms */

static inline void brcmfmac_led_rx(struct brcmf_cfg80211_info *info)
{
#ifdef CONFIG_BRCMFMAC_LEDS
	unsigned long led_delay = BRCMFMAC_BLINK_DELAY;

	if (atomic_read(&info->rx_led_active))
		led_trigger_blink_oneshot(&info->rx_led, &led_delay,
					  &led_delay, 0);

	if (atomic_read(&info->rxtx_led_active))
		led_trigger_blink_oneshot(&info->rxtx_led, &led_delay,
					  &led_delay, 0);
#endif
}

static inline void brcmfmac_led_tx(struct brcmf_cfg80211_info *info)
{
#ifdef CONFIG_BRCMFMAC_LEDS
	unsigned long led_delay = BRCMFMAC_BLINK_DELAY;

	if (atomic_read(&info->tx_led_active))
		led_trigger_blink_oneshot(&info->tx_led, &led_delay,
					  &led_delay, 0);

	if (atomic_read(&info->rxtx_led_active))
		led_trigger_blink_oneshot(&info->rxtx_led, &led_delay,
					  &led_delay, 0);
#endif
}

#ifdef CONFIG_BRCMFMAC_LEDS
void brcmfmac_led_init(struct brcmf_cfg80211_info *info);
void brcmfmac_led_exit(struct brcmf_cfg80211_info *info);
#else
static inline void brcmfmac_led_init(struct brcmf_cfg80211_info *info)
{
}

static inline void brcmfmac_led_exit(struct brcmf_cfg80211_info *info)
{
}
#endif
