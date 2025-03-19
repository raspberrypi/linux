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
#include "upisnd_codec.h"

static void upisnd_proc_serial_show(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
	struct upisnd_instance *instance = entry->private_data;

	snd_iprintf(buffer, "%s\n", instance->ctrl.serial);
}

static void upisnd_proc_version_show(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
	struct upisnd_instance *instance = entry->private_data;

	snd_iprintf(buffer,
		    "%u.%u.%u\n",
		    instance->ctrl.version.major,
		    instance->ctrl.version.minor,
		    instance->ctrl.version.build);
}

static void upisnd_proc_hwrev_show(struct snd_info_entry *entry, struct snd_info_buffer *buffer)
{
	struct upisnd_instance *instance = entry->private_data;

	snd_iprintf(buffer, "%u\n", instance->ctrl.version.hwrev);
}

static int upisnd_card_probe(struct snd_soc_card *card)
{
	struct upisnd_instance *instance = container_of(card, struct upisnd_instance, sound_card);
	int err;

	err = upisnd_midi_init(instance);

	if (err < 0) {
		printe("Failed to initialize MIDI subsystem! (%d)", err);
		return err;
	}

	err = snd_card_ro_proc_new(card->snd_card, "serial", instance, upisnd_proc_serial_show);

	if (err < 0) {
		printe("Failed to create serial proc entry! (%d)", err);
		return err;
	}

	err = snd_card_ro_proc_new(card->snd_card, "version", instance, upisnd_proc_version_show);

	if (err < 0) {
		printe("Failed to create version proc entry! (%d)", err);
		return err;
	}

	err = snd_card_ro_proc_new(card->snd_card, "hwrev", instance, upisnd_proc_hwrev_show);

	if (err < 0) {
		printe("Failed to create hwrev proc entry! (%d)", err);
		return err;
	}

	return 0;
}

static int upisnd_card_remove(struct snd_soc_card *card)
{
	struct upisnd_instance *instance = container_of(card, struct upisnd_instance, sound_card);

	upisnd_midi_uninit(instance);
	return 0;
}

static int upisnd_startup(struct snd_pcm_substream *substream)
{
	printd("startup");
	return 0;
}

static int upisnd_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	printd("hw_params");

	struct snd_soc_pcm_runtime *rtd = substream->private_data;

	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int ret = 0;

	struct upisnd_instance *instance = container_of(rtd->card,
							struct upisnd_instance,
							sound_card);

	if (!(instance->flags & UPISND_FLAG_DUMMY))
		ret = snd_soc_dai_set_bclk_ratio(cpu_dai, 64);

	if (ret < 0) {
		printe("Failed setting dai bclk ratio!\n");
		return ret;
	}

	return 0;
}

static int upisnd_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);

	return snd_soc_dai_set_tdm_slot(codec_dai, 0, 0, 0, 0); // Enable I2S mode.
}

static const struct snd_soc_ops upisnd_ops = {
	.startup = upisnd_startup,
	.hw_params = upisnd_hw_params,
};

SND_SOC_DAILINK_DEFS(adau1961,
		     DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC("adau1961", "adau-hifi")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static const struct snd_soc_dai_link upisnd_dai_link = {
	.name = "pisound-micro",
	.stream_name = "pisound-micro PCM",
	.dai_fmt =
		SND_SOC_DAIFMT_I2S |
		SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBM_CFM,
	.ops = &upisnd_ops,
	.init = upisnd_dai_init,
	SND_SOC_DAILINK_REG(adau1961),
};

static const struct snd_soc_card upisnd_sound_card = {
	.name = "pisoundmicro",
	.owner = THIS_MODULE,
	.probe = upisnd_card_probe,
	.remove = upisnd_card_remove,
};

int upisnd_sound_init(struct platform_device *pdev, struct upisnd_instance *instance)
{
	struct snd_soc_dai_link_component *comp = NULL;
	struct device_node *i2s_node = NULL;

	memcpy(&instance->sound_card, &upisnd_sound_card, sizeof(upisnd_sound_card));
	memcpy(&instance->dai_link, &upisnd_dai_link, sizeof(upisnd_dai_link));

	if (pdev->dev.of_node) {
		of_property_read_string(pdev->dev.of_node, "card-name", &instance->sound_card.name);
		i2s_node = of_parse_phandle(pdev->dev.of_node, "i2s-controller", 0);
		if (!i2s_node)
			printi("'i2s-controller' node not specified, will use dummy one instead!");
	}

	comp = devm_kzalloc(&pdev->dev, sizeof(*comp) * 3, GFP_KERNEL);
	if (!comp)
		return -ENOMEM;

	char *long_name = devm_kzalloc(&pdev->dev, 26, GFP_KERNEL);

	if (!long_name)
		return -ENOMEM;

	instance->dai_link.cpus = &comp[0];
	instance->dai_link.codecs = &comp[1];
	instance->dai_link.platforms = &comp[2];

	instance->dai_link.num_cpus = 1;
	instance->dai_link.num_codecs = 1;
	instance->dai_link.num_platforms = 1;

	if (i2s_node) {
		instance->dai_link.cpus->of_node = i2s_node;
		instance->dai_link.platforms->of_node = i2s_node;
	} else {
		printi("Setting up dummy interface.");
		instance->dai_link.cpus->name = "snd-soc-dummy";
		instance->dai_link.platforms->name = "snd-soc-dummy";
		instance->dai_link.cpus->dai_name = "snd-soc-dummy-dai";
		instance->dai_link.platforms->dai_name = "snd-soc-dummy-dai";
		instance->dai_link.dai_fmt = (instance->dai_link.dai_fmt & ~SND_SOC_DAIFMT_CBM_CFM)
					     | SND_SOC_DAIFMT_CBS_CFS;
		instance->flags |= UPISND_FLAG_DUMMY;
	}
	instance->dai_link.codecs->of_node = instance->codec_dev->of_node;
	instance->dai_link.codecs->dai_name = "adau-hifi";
	instance->dai_link.stream_name = instance->ctrl.serial;

	instance->sound_card.dev = &pdev->dev;

	instance->sound_card.dai_link = &instance->dai_link;
	instance->sound_card.num_links = 1;

	snprintf(long_name, 26, "Pisound Micro %s", instance->ctrl.serial);
	instance->sound_card.long_name = long_name;
	printd("About to register card %s", instance->sound_card.long_name);

	int err = snd_soc_register_card(&instance->sound_card);

	if (i2s_node)
		of_node_put(i2s_node);

	if (err < 0) {
		instance->sound_card.dev = NULL;
		if (err != -EPROBE_DEFER)
			printe("snd_soc_register_card failed with %d!", err);
		return err;
	}

	struct snd_soc_dai *dai = snd_soc_card_get_codec_dai(&instance->sound_card, "adau-hifi");

	if (!dai) {
		printe("Failed to get codec dai!");
		instance->sound_card.dev = NULL;
		return -ENODEV;
	}

	if (adau1961_is_hp_capless(dai->component)) {
		err = upisnd_comm_set_subscription(instance, UPISND_IRQ_VGND_SHORT_ALERT, true);
		if (err < 0) {
			instance->sound_card.dev = NULL;
			printe("Failed to subscribe to VGND short alert IRQ! (%d)", err);
			return err;
		}
	}

	return 0;
}

static void upisnd_sound_handle_irq_event(struct upisnd_instance *instance,
					  const struct irq_event_t *event)
{
	struct snd_soc_dai *dai = NULL;

	switch (event->num) {
	case UPISND_IRQ_VGND_SHORT_ALERT:
		dai = snd_soc_card_get_codec_dai(&instance->sound_card, "adau-hifi");
		printe("VGND short alert %s Headphone output!",
		       event->high ? "ON, muting" : "OFF, restoring last state");
		if (dai)
			adau1961_set_vgnd_shorted(dai->component, event->high);
		else
			printe("Failed to get codec dai!");
		break;
	default:
		break;
	}
}

void upisnd_sound_handle_irq_events(struct upisnd_instance *instance,
				    const struct irq_event_t *events,
				    unsigned int n)
{
	unsigned int i;

	printd("Handling %u sound IRQ events", n);

	for (i = 0; i < n; ++i)
		upisnd_sound_handle_irq_event(instance, &events[i]);
}

/* vim: set ts=8 sw=8 noexpandtab: */
