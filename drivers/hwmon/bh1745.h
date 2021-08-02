/*
 * This file is part of the APDS990x sensor driver.
 * Chip is combined proximity and ambient light sensor.
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Samu Onkalo <samu.p.onkalo@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifndef  __RGB_BH1745_H__
#define __RGB_BH1745_H__

/************ define register for IC ************/
/* BH1745 REGSTER */
#define BH1745_SYSTEMCONTROL      (0x40)
#define BH1745_MODECONTROL1       (0x41)
#define BH1745_MODECONTROL2       (0x42)
#define BH1745_MODECONTROL3       (0x44)
#define BH1745_READ_DATA                (0x50)
#define BH1745_INTERRUPT                (0x60)
#define BH1745_PERSISTENCE             (0x61)
#define BH1745_RED_DATA_LSB         (0x50)
#define BH1745_RED_DATA_MSB         (0x51)
#define BH1745_GREEN_DATA_LSB         (0x52)
#define BH1745_GREEN_DATA_MSB         (0x53)
#define BH1745_BLUE_DATA_LSB         (0x54)
#define BH1745_BLUE_DATA_MSB         (0x55)
#define BH1745_CLEAR_DATA_LSB         (0x56)
#define BH1745_CLEAR_DATA_MSB         (0x57)
#define MODECONTROL2_VALID              (0x80)

/************ define parameter for register ************/

/* REG_SYSTEMCONTROL(0x40) */
#define SW_RESET                           (1 << 7)
#define INT_RESET                          (1 << 6)

/* REG_MODECONTROL1(0x41) */
#define MEASURE_160MS               (0x00)
#define MEASURE_320MS               (0x01)
#define MEASURE_640MS               (0x02)
#define MEASURE_1280MS               (0x03)
#define MEASUREMENT_MAX         (0x05)
#define MEASURE_160MS_TIME    (160)

#define MEASURE_DELAY_320MS     320
#define MEASURE_DELAY_640MS     640
#define MEASURE_DELAY_1280MS    1280
/* REG_MODECONTROL2(0x42) */
#define ADC_GAIN_X1                     (0x00)
#define ADC_GAIN_X2                     (1 << 0)
#define ADC_GAIN_X16                    (1 << 1)
#define ADC_GAIN                            (16)
#define RGBC_EN_ON                      (1 << 4)
#define RGBC_EN_OFF                     (0 << 4)
#define RGBC_VALID_HIGH            (1 << 7)

/* REG_MODECONTROL3(0x44) */
#define MODE_CTL_FIX_VAL          (0x02)

/* REG_INTERRUPT(0x60) */
#define BH1745_IRQ_EN                 (1 << 0)
#define BH1745_IRQ_DISABLE      (0 << 0)
#define BH1745_IRQ_SRC_R         (0 << 2)
#define BH1745_IRQ_SRC_G         (1 << 2)
#define BH1745_IRQ_SRC_B         (2 << 2)
#define BH1745_IRQ_LATCH         (0 << 4)
#define BH1745_IRQ_SRC_B         (2 << 2)

/* REG_PERSISTENCE(0x61) */
#define BH1745_PPERS_0	0x00  /* Interrupt status is toggled at each measurement end. */
#define BH1745_PPERS_1	0x01  /* Interrupt status is updated at each measurement end. */
#define BH1745_PPERS_2	0x10  /* Interrupt status is updated if 4 consecutive threshold judgments are the same */
#define BH1745_PPERS_3	0x11  /*Interrupt status is updated if 8 consecutive threshold judgments are the same */

#define BH1745_RGB_DATA_MAX          65536
#define BH1745_LUX_MAX                       30000

/* POWER SUPPLY VOLTAGE RANGE */
#define BH1745_VDD_MIN_UV  2000000
#define BH1745_VDD_MAX_UV  3300000
#define BH1745_VIO_MIN_UV  1750000
#define BH1745_VIO_MAX_UV  1950000

/*the rgb_bh1745_platform_data structure needs to cite the definition of rgb_bh1745_data*/
struct rgb_bh1745_data;

struct rgb_bh1745_platform_data {
	u8	   pdrive;
	int    (*setup_resources)(void);
	int    (*release_resources)(void);

	int irq_num;
	int (*power)(unsigned char onoff);
	/*add the parameters to the init and exit function*/
	int (*init)(struct rgb_bh1745_data *data);
	void (*exit)(struct rgb_bh1745_data *data);
	int (*power_on)(bool,struct rgb_bh1745_data *data);

	bool i2c_pull_up;
	bool digital_pwr_regulator;

	unsigned int irq_gpio;
	u32 irq_gpio_flags;
	int panel_id;
	int tp_color;
};

/* the parameter for lux calculation formula for the correponding GAGAIN */
#define BH1745_AGAIN_1X_LUXCALCULATION	1  /* 1X ALS GAIN */
#define BH1745_AGAIN_8X_LUXCALCULATION	8  /* 8X ALS GAIN */
#define BH1745_AGAIN_16X_LUXCALCULATION	16  /* 16X ALS GAIN */
#define BH1745_AGAIN_120X_LUXCALCULATION	120  /* 120X ALS GAIN */

/* Register Value define : CONTROL */
#define BH1745_AGAIN_1X	0x00  /* 1X ALS GAIN */
#define BH1745_AGAIN_8X	0x01  /* 8X ALS GAIN */
#define BH1745_AGAIN_16X	0x02  /* 16X ALS GAIN */
#define BH1745_AGAIN_120X	0x03  /* 120X ALS GAIN */

#endif
