/*
 * File:         sound/soc/bcm/bcm2708-ssm2602.c
 * Author:       Lino von Burg
 *
 * Created:      05-2017
 * Description:  board driver for SSM2602 sound chip
 *
 * Modified:
 *               Copyright 2008 Analog Devices Inc.
 *
 * Bugs:         Enter bugs at http://blackfin.uclinux.org/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see the file COPYING, or write
 * to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>

#include <asm/dma.h>
/* #include <asm/portmux.h> */
#include <linux/gpio.h>
#include "../codecs/ssm2602.h"

static struct snd_soc_card bcm2708_ssm2602;

static int bcm2708_ssm2602_dai_init(struct snd_soc_pcm_runtime *rtd)
{
    /*
     * If you are using a crystal source which frequency is not 12MHz
     * then modify the below case statement with frequency of the crystal.
     *
     * If you are using the SPORT to generate clocking then this is
     * where to do it.
     */
    return snd_soc_dai_set_sysclk(rtd->codec_dai, SSM2602_SYSCLK, 12000000,
        SND_SOC_CLOCK_IN);
}

/* CODEC is master for BCLK and LRC in this configuration. */
#define BCM2708_SSM2602_DAIFMT (SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | \
                SND_SOC_DAIFMT_CBM_CFM)

static struct snd_soc_dai_link bcm2708_ssm2602_dai[] = {
    {
        .name = "ssm2602",
        .stream_name = "SSM2602",
        .cpu_dai_name = "bcm2708-i2s.0",
        .codec_dai_name = "ssm2602-hifi",
        .platform_name = "bcm2708-i2s.0",
        .codec_name = "ssm2602.0-001b",
        .init = bcm2708_ssm2602_dai_init,
        .dai_fmt = BCM2708_SSM2602_DAIFMT,
    },
};

static struct snd_soc_card bcm2708_ssm2602 = {
    .name = "bcm2708-ssm2602",
    .owner = THIS_MODULE,
    .dai_link = &bcm2708_ssm2602_dai,
    .num_links = ARRAY_SIZE(bcm2708_ssm2602_dai),
};

static struct platform_device *bcm2708_ssm2602_snd_device;

static int __init bcm2708_ssm2602_init(void)
{
    int ret;

    pr_debug("%s enter\n", __func__);
    bcm2708_ssm2602_snd_device = platform_device_alloc("soc-audio", -1);
    if (!bcm2708_ssm2602_snd_device)
        return -ENOMEM;

    platform_set_drvdata(bcm2708_ssm2602_snd_device, &bcm2708_ssm2602);
    ret = platform_device_add(bcm2708_ssm2602_snd_device);

    if (ret)
        platform_device_put(bcm2708_ssm2602_snd_device);

    return ret;
}

static void __exit bcm2708_ssm2602_exit(void)
{
    pr_debug("%s enter\n", __func__);
    platform_device_unregister(bcm2708_ssm2602_snd_device);
}

module_init(bcm2708_ssm2602_init);
module_exit(bcm2708_ssm2602_exit);

/* Module information */
MODULE_AUTHOR("Lino von Burg");
MODULE_DESCRIPTION("ALSA SoC SSM2602 bcm2708");
MODULE_LICENSE("GPL");
