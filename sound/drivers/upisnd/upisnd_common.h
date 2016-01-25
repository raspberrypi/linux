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

#ifndef UPISOUND_COMMON_H
#define UPISOUND_COMMON_H

#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/kfifo.h>
#include <linux/kobject.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/crc8.h>
#include <linux/rbtree.h>
#include <linux/kref.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/of_irq.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/consumer.h>

#include <linux/pinctrl/pinconf-generic.h>

#include <sound/asound.h>
#include <sound/asequencer.h>
#include <sound/soc.h>
#include <sound/rawmidi.h>
#include <sound/pcm_params.h>

#include "upisnd_protocol.h"
#include "upisnd_comm.h"
#include "upisnd_debug.h"
#include "upisnd_midi.h"
#include "upisnd_sound.h"
#include "upisnd_sysfs.h"
#include "upisnd_ctrl.h"
#include "upisnd_pins.h"
#include "upisnd_gpio.h"
#include "upisnd_utils.h"

enum upisnd_flags_e {
	UPISND_FLAG_DUMMY           = 1 << 0,
	UPISND_FLAG_ADC_CALIBRATION = 1 << 1,
};

struct upisnd_instance {
	struct kref             refcount;
	struct platform_device  *pdev;
	struct device           *ctrl_dev;
	struct device           *codec_dev;
	struct workqueue_struct *work_queue;

	struct upisnd_ctrl      ctrl;
	struct upisnd_comm      comm;
	struct upisnd_midi      midi;
	struct upisnd_gpio      gpio;

	struct rw_semaphore     rw_gpio_config_sem;
	struct upisnd_config    *config;
	DECLARE_KFIFO(ctrl_event_fifo, struct control_event_t, 128);
	struct work_struct      ctrl_event_handler;

	struct snd_soc_card     sound_card;
	struct snd_soc_dai_link dai_link;

	u32                     flags;
};

void upisnd_instance_release(struct kref *kref);

#endif // UPISOUND_COMMON_H

/* vim: set ts=8 sw=8 noexpandtab: */
