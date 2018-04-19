/*
 * Driver for the SABRE ESS CODEC
 *
 * Author: Jaikumar <jaikumar@cem-solutions.net>
 *		Copyright 2018
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _SND_SOC_SABRE_ESS_
#define _SND_SOC_SABRE_ESS_

#include <linux/pm.h>
#include <linux/regmap.h>

#define SABRE_ESS_CHIP_ID		0x30
#define SABRE_ESS_VIRT_BASE		0x100
#define SABRE_ESS_PAGE			0

#define SABRE_ESS_CHIP_ID_REG	(SABRE_ESS_VIRT_BASE + 0)
#define SABRE_ESS_RESET			(SABRE_ESS_VIRT_BASE + 1)
#define SABRE_ESS_VOLUME_1		(SABRE_ESS_VIRT_BASE + 2)
#define SABRE_ESS_VOLUME_2		(SABRE_ESS_VIRT_BASE + 3)
#define SABRE_ESS_MUTE			(SABRE_ESS_VIRT_BASE + 4)
#define SABRE_ESS_DSP_PROGRAM	(SABRE_ESS_VIRT_BASE + 5)
#define SABRE_ESS_DEEMPHASIS	(SABRE_ESS_VIRT_BASE + 6)
#define SABRE_ESS_DOP			(SABRE_ESS_VIRT_BASE + 7)
#define SABRE_ESS_FORMAT		(SABRE_ESS_VIRT_BASE + 8)
#define SABRE_ESS_COMMAND		(SABRE_ESS_VIRT_BASE + 9)
#define SABRE_ESS_MAX_REGISTER	(SABRE_ESS_VIRT_BASE + 9)

#define SABRE_ESS_FMT			0xff
#define SABRE_ESS_CHAN_MONO		0x00
#define SABRE_ESS_CHAN_STEREO	0x80
#define SABRE_ESS_ALEN_16		0x10
#define SABRE_ESS_ALEN_24		0x20
#define SABRE_ESS_ALEN_32		0x30
#define SABRE_ESS_RATE_11025	0x01
#define SABRE_ESS_RATE_22050	0x02
#define SABRE_ESS_RATE_32000	0x03
#define SABRE_ESS_RATE_44100	0x04
#define SABRE_ESS_RATE_48000	0x05
#define SABRE_ESS_RATE_88200	0x06
#define SABRE_ESS_RATE_96000	0x07
#define SABRE_ESS_RATE_176400	0x08
#define SABRE_ESS_RATE_192000	0x09
#define SABRE_ESS_RATE_352800	0x0a
#define SABRE_ESS_RATE_384000	0x0b

extern const struct regmap_config sabre_ess_regmap;

int sabre_ess_probe(struct device *dev, struct regmap *regmap);
void sabre_ess_remove(struct device *dev);

#endif /* _SND_SOC_SABRE_ESS_ */
