/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tas5805m.h - ALSA SoC Texas Instruments TAS5805M Mono Audio Amplifier
 *
 * Copyright (C)2015-2016 Texas Instruments Incorporated -  https://www.ti.com
 *
 * Author: Andriy Malyshenko <andriy@sonocotta.com>
 */

#ifndef __TAS5805M_H__
#define __TAS5805M_H__

/* register control codes */
#define TAS5805M_REG_PAGE_SET 0x00
#define TAS5805M_REG_BOOK_SET 0x7f
#define REG_PAGE TAS5805M_REG_PAGE_SET
#define REG_BOOK TAS5805M_REG_BOOK_SET

#define TAS5805M_BOOK_CONTROL_PORT 0x00
#define TAS5805M_REG_PAGE_0        0x00

/* Datasheet-defined registers on page 0, book 0 */
#define TAS5805M_REG_RESET_CTRL      0x01
#define TAS5805M_REG_DEVICE_CTRL_1   0x02
#define TAS5805M_REG_DEVICE_CTRL_2   0x03
#define TAS5805M_REG_SDOUT_SEL       0x30
#define TAS5805M_REG_CLKDET_STATUS   0x39
#define TAS5805M_REG_UNDOCUMENTED_0  0x46
#define TAS5805M_REG_VOL_CTRL        0x4c
#define TAS5805M_REG_AUTO_MUTE_TIME  0x51
#define TAS5805M_REG_ANA_CTRL        0x53
#define TAS5805M_REG_ANALOG_GAIN     0x54
#define TAS5805M_REG_ADR_PIN_CTRL    0x60
#define TAS5805M_REG_ADR_PIN_CONFIG  0x61
#define TAS5805M_REG_DSP_MISC        0x66
#define TAS5805M_REG_CHAN_FAULT      0x70
#define TAS5805M_REG_GLOBAL_FAULT1   0x71
#define TAS5805M_REG_GLOBAL_FAULT2   0x72
#define TAS5805M_REG_OT_WARNING      0x73
#define TAS5805M_REG_FAULT           0x78
#define TAS5805M_REG_UNDOCUMENTED_1  0x7d
#define TAS5805M_REG_UNDOCUMENTED_2  0x7e
#define TAS5805M_REG_PAGE1_UNDOC_0   0x51

/* Volume control using hardware register TAS5805M_REG_VOL_CTRL (0x4c)
 * Register value to dB mapping:
 *   0x00 = +24.0 dB
 *   0x30 =   0.0 dB  
 *   0xFE = -103.0 dB
 *   0xFF = Mute
 * Hardware step is 0.5 dB, we expose 1.0 dB steps to ALSA
 */
#define TAS5805M_VOLUME_MAX	0x00  /* +24 dB */
#define TAS5805M_VOLUME_MIN	0xFE  /* -103 dB */
#define TAS5805M_VOLUME_ZERO_DB	0x30  /* 0 dB */

#define TAS5805M_AGAIN_MAX 0x00
#define TAS5805M_AGAIN_MIN 0x1F

/* TAS5805M_REG_RESET_CTRL register values */
#define TAS5805M_RESET_CONTROL_PORT  BIT(0)
#define TAS5805M_RESET_DSP           BIT(4)

/* TAS5805M_REG_DEVICE_CTRL_1 register values */
#define TAS5805M_DCTRL1_BD_MODE      0x00
#define TAS5805M_DCTRL1_1SPW_MODE    0x01
#define TAS5805M_DCTRL1_HYBRID_MODE  0x02
#define TAS5805M_DCTRL1_DAMP_PBTL    BIT(2)
#define TAS5805M_DCTRL1_FSW768KHZ    0x00
#define TAS5805M_DCTRL1_FSW384KHZ    BIT(4)
#define TAS5805M_DCTRL1_FSW480KHZ    GENMASK(5,4)
#define TAS5805M_DCTRL1_FSW576KHZ    BIT(6)

/* TAS5805M_REG_DEVICE_CTRL_2 register values */
#define TAS5805M_DCTRL2_MODE_DEEP_SLEEP 0x00
#define TAS5805M_DCTRL2_MODE_SLEEP      0x01
#define TAS5805M_DCTRL2_MODE_HIZ        0x02
#define TAS5805M_DCTRL2_MODE_PLAY       0x03
#define TAS5805M_DCTRL2_MUTE		    BIT(3)
#define TAS5805M_DCTRL2_DIS_DSP		    BIT(4)

/* TAS5805M_REG_FAULT register values */
#define TAS5805M_ANALOG_FAULT_CLEAR 0x80

/* Level meter register definitions */
#define TAS5805M_BOOK_4 0x78
#define TAS5805M_REG_BOOK_4_LEVEL_METER_PAGE 0x02
#define TAS5805M_REG_LEVEL_METER_LEFT 0x60
#define TAS5805M_REG_LEVEL_METER_RIGHT 0x64

/* Mixer register definitions */
#define TAS5805M_BOOK_5 0x8c
#define TAS5805M_BOOK_5_MIXER_PAGE 0x29
#define TAS5805M_REG_LEFT_TO_LEFT_GAIN 0x18
#define TAS5805M_REG_RIGHT_TO_LEFT_GAIN 0x1c
#define TAS5805M_REG_LEFT_TO_RIGHT_GAIN 0x20
#define TAS5805M_REG_RIGHT_TO_RIGHT_GAIN 0x24

#define TAS5805M_BOOK_5_VOLUME_PAGE 0x2a
#define TAS5805M_REG_LEFT_VOLUME 0x24
#define TAS5805M_REG_RIGHT_VOLUME 0x28

/* Mixer gain values */
#define TAS5805M_MIXER_VALUE_MUTE 0x00000000
#define TAS5805M_MIXER_VALUE_0DB 0x00008000
#define TAS5805M_MIXER_VALUE_MINUS6DB 0x00004000

#endif /* __TAS5805M_H__ */