/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Driver for the TAS5754M DAC+amplifier combo devices
 *
 * Author:	(copied from pcm512x.h)
 *		Mark Brown <broonie@kernel.org>
 *		Copyright 2014 Linaro Ltd
 */

#ifndef _SND_SOC_TAS5754M
#define _SND_SOC_TAS5754M

#include <linux/pm.h>
#include <linux/regmap.h>

#define TAS5754M_VIRT_BASE 0x000
#define TAS5754M_PAGE_LEN  0x80
#define TAS5754M_PAGE_BASE(n)  (TAS5754M_VIRT_BASE + (TAS5754M_PAGE_LEN * n))

#define TAS5754M_PAGE              0

#define TAS5754M_RESET             (TAS5754M_PAGE_BASE(0) +   1)
#define TAS5754M_POWER             (TAS5754M_PAGE_BASE(0) +   2)
#define TAS5754M_MUTE              (TAS5754M_PAGE_BASE(0) +   3)
#define TAS5754M_PLL_EN            (TAS5754M_PAGE_BASE(0) +   4)
#define TAS5754M_SPI_MISO_FUNCTION (TAS5754M_PAGE_BASE(0) +   6)
#define TAS5754M_DSP               (TAS5754M_PAGE_BASE(0) +   7)
#define TAS5754M_GPIO_EN           (TAS5754M_PAGE_BASE(0) +   8)
#define TAS5754M_BCLK_LRCLK_CFG    (TAS5754M_PAGE_BASE(0) +   9)
#define TAS5754M_DSP_GPIO_INPUT    (TAS5754M_PAGE_BASE(0) +  10)
#define TAS5754M_MASTER_MODE       (TAS5754M_PAGE_BASE(0) +  12)
#define TAS5754M_PLL_REF           (TAS5754M_PAGE_BASE(0) +  13)
#define TAS5754M_DAC_REF           (TAS5754M_PAGE_BASE(0) +  14)
#define TAS5754M_GPIO_DACIN        (TAS5754M_PAGE_BASE(0) +  16)
#define TAS5754M_GPIO_PLLIN        (TAS5754M_PAGE_BASE(0) +  18)
#define TAS5754M_SYNCHRONIZE       (TAS5754M_PAGE_BASE(0) +  19)
#define TAS5754M_PLL_COEFF_0       (TAS5754M_PAGE_BASE(0) +  20)
#define TAS5754M_PLL_COEFF_1       (TAS5754M_PAGE_BASE(0) +  21)
#define TAS5754M_PLL_COEFF_2       (TAS5754M_PAGE_BASE(0) +  22)
#define TAS5754M_PLL_COEFF_3       (TAS5754M_PAGE_BASE(0) +  23)
#define TAS5754M_PLL_COEFF_4       (TAS5754M_PAGE_BASE(0) +  24)
#define TAS5754M_DSP_CLKDIV        (TAS5754M_PAGE_BASE(0) +  27)
#define TAS5754M_DAC_CLKDIV        (TAS5754M_PAGE_BASE(0) +  28)
#define TAS5754M_NCP_CLKDIV        (TAS5754M_PAGE_BASE(0) +  29)
#define TAS5754M_OSR_CLKDIV        (TAS5754M_PAGE_BASE(0) +  30)
#define TAS5754M_MASTER_CLKDIV_1   (TAS5754M_PAGE_BASE(0) +  32)
#define TAS5754M_MASTER_CLKDIV_2   (TAS5754M_PAGE_BASE(0) +  33)
#define TAS5754M_FS_SPEED_MODE     (TAS5754M_PAGE_BASE(0) +  34)
#define TAS5754M_IDAC_1            (TAS5754M_PAGE_BASE(0) +  35)
#define TAS5754M_IDAC_2            (TAS5754M_PAGE_BASE(0) +  36)
#define TAS5754M_ERROR_DETECT      (TAS5754M_PAGE_BASE(0) +  37)
#define TAS5754M_I2S_1             (TAS5754M_PAGE_BASE(0) +  40)
#define TAS5754M_I2S_2             (TAS5754M_PAGE_BASE(0) +  41)
#define TAS5754M_DAC_ROUTING       (TAS5754M_PAGE_BASE(0) +  42)
#define TAS5754M_DSP_PROGRAM       (TAS5754M_PAGE_BASE(0) +  43)
#define TAS5754M_CLKDET            (TAS5754M_PAGE_BASE(0) +  44)
#define TAS5754M_AUTO_MUTE         (TAS5754M_PAGE_BASE(0) +  59)
#define TAS5754M_DIGITAL_VOLUME_1  (TAS5754M_PAGE_BASE(0) +  60)
#define TAS5754M_DIGITAL_VOLUME_2  (TAS5754M_PAGE_BASE(0) +  61)
#define TAS5754M_DIGITAL_VOLUME_3  (TAS5754M_PAGE_BASE(0) +  62)
#define TAS5754M_DIGITAL_MUTE_1    (TAS5754M_PAGE_BASE(0) +  63)
#define TAS5754M_DIGITAL_MUTE_2    (TAS5754M_PAGE_BASE(0) +  64)
#define TAS5754M_DIGITAL_MUTE_3    (TAS5754M_PAGE_BASE(0) +  65)
#define TAS5754M_GPIO_OUTPUT_1     (TAS5754M_PAGE_BASE(0) +  80)
#define TAS5754M_GPIO_OUTPUT_2     (TAS5754M_PAGE_BASE(0) +  81)
#define TAS5754M_GPIO_OUTPUT_3     (TAS5754M_PAGE_BASE(0) +  82)
#define TAS5754M_GPIO_OUTPUT_4     (TAS5754M_PAGE_BASE(0) +  83)
#define TAS5754M_GPIO_OUTPUT_5     (TAS5754M_PAGE_BASE(0) +  84)
#define TAS5754M_GPIO_OUTPUT_6     (TAS5754M_PAGE_BASE(0) +  85)
#define TAS5754M_GPIO_CONTROL_1    (TAS5754M_PAGE_BASE(0) +  86)
#define TAS5754M_GPIO_CONTROL_2    (TAS5754M_PAGE_BASE(0) +  87)
#define TAS5754M_OVERFLOW          (TAS5754M_PAGE_BASE(0) +  90)
#define TAS5754M_RATE_DET_1        (TAS5754M_PAGE_BASE(0) +  91)
#define TAS5754M_RATE_DET_2        (TAS5754M_PAGE_BASE(0) +  92)
#define TAS5754M_RATE_DET_3        (TAS5754M_PAGE_BASE(0) +  93)
#define TAS5754M_RATE_DET_4        (TAS5754M_PAGE_BASE(0) +  94)
#define TAS5754M_CLOCK_STATUS      (TAS5754M_PAGE_BASE(0) +  95)
#define TAS5754M_ANALOG_MUTE_DET   (TAS5754M_PAGE_BASE(0) + 108)
#define TAS5754M_GPIN              (TAS5754M_PAGE_BASE(0) + 119)
#define TAS5754M_DIGITAL_MUTE_DET  (TAS5754M_PAGE_BASE(0) + 120)

#define TAS5754M_OUTPUT_AMPLITUDE  (TAS5754M_PAGE_BASE(1) +   1)
#define TAS5754M_ANALOG_GAIN_CTRL  (TAS5754M_PAGE_BASE(1) +   2)
#define TAS5754M_UNDERVOLTAGE_PROT (TAS5754M_PAGE_BASE(1) +   5)
#define TAS5754M_ANALOG_MUTE_CTRL  (TAS5754M_PAGE_BASE(1) +   6)
#define TAS5754M_ANALOG_GAIN_BOOST (TAS5754M_PAGE_BASE(1) +   7)
#define TAS5754M_VCOM_CTRL_1       (TAS5754M_PAGE_BASE(1) +   8)
#define TAS5754M_VCOM_CTRL_2       (TAS5754M_PAGE_BASE(1) +   9)

#define TAS5754M_CRAM_CTRL         (TAS5754M_PAGE_BASE(44) +  1)

#define TAS5754M_FLEX_A            (TAS5754M_PAGE_BASE(253) + 63)
#define TAS5754M_FLEX_B            (TAS5754M_PAGE_BASE(253) + 64)

#define TAS5754M_MAX_REGISTER      (TAS5754M_PAGE_BASE(253) + 64)

/* Page 0, Register 1 - reset */
#define TAS5754M_RSTR (1 << 0)
#define TAS5754M_RSTM (1 << 4)

/* Page 0, Register 2 - power */
#define TAS5754M_RQPD       (1 << 0)
#define TAS5754M_RQPD_SHIFT 0
#define TAS5754M_RQST       (1 << 4)
#define TAS5754M_RQST_SHIFT 4

/* Page 0, Register 3 - mute */
#define TAS5754M_RQMR (1 << 0)
#define TAS5754M_RQMR_SHIFT 0
#define TAS5754M_RQML (1 << 4)
#define TAS5754M_RQML_SHIFT 4

/* Page 0, Register 4 - PLL */
#define TAS5754M_PLLE       (1 << 0)
#define TAS5754M_PLLE_SHIFT 0
#define TAS5754M_PLCK       (1 << 4)
#define TAS5754M_PLCK_SHIFT 4

/* Page 0, Register 7 - DSP */
#define TAS5754M_SDSL       (1 << 0)
#define TAS5754M_SDSL_SHIFT 0
#define TAS5754M_DEMP       (1 << 4)
#define TAS5754M_DEMP_SHIFT 4

/* Page 0, Register 8 - GPIO output enable */
#define TAS5754M_G1OE       (1 << 0)
#define TAS5754M_G2OE       (1 << 1)
#define TAS5754M_G3OE       (1 << 2)
#define TAS5754M_G4OE       (1 << 3)
#define TAS5754M_G5OE       (1 << 4)
#define TAS5754M_G6OE       (1 << 5)

/* Page 0, Register 9 - BCK, LRCLK configuration */
#define TAS5754M_LRKO       (1 << 0)
#define TAS5754M_LRKO_SHIFT 0
#define TAS5754M_BCKO       (1 << 4)
#define TAS5754M_BCKO_SHIFT 4
#define TAS5754M_BCKP       (1 << 5)
#define TAS5754M_BCKP_SHIFT 5

/* Page 0, Register 12 - Master mode BCK, LRCLK reset */
#define TAS5754M_RLRK       (1 << 0)
#define TAS5754M_RLRK_SHIFT 0
#define TAS5754M_RBCK       (1 << 1)
#define TAS5754M_RBCK_SHIFT 1

/* Page 0, Register 13 - PLL reference */
#define TAS5754M_SREF        (7 << 4)
#define TAS5754M_SREF_SHIFT  4
#define TAS5754M_SREF_SCK    (0 << 4)
#define TAS5754M_SREF_BCK    (1 << 4)
#define TAS5754M_SREF_GPIO   (3 << 4)

/* Page 0, Register 14 - DAC reference */
#define TAS5754M_SDAC        (7 << 4)
#define TAS5754M_SDAC_SHIFT  4
#define TAS5754M_SDAC_MCK    (0 << 4)
#define TAS5754M_SDAC_PLL    (1 << 4)
#define TAS5754M_SDAC_SCK    (3 << 4)
#define TAS5754M_SDAC_BCK    (4 << 4)
#define TAS5754M_SDAC_GPIO   (5 << 4)

/* Page 0, Register 16, 18 - GPIO source for DAC, PLL */
#define TAS5754M_GREF        (7 << 0)
#define TAS5754M_GREF_SHIFT  0
#define TAS5754M_GREF_GPIO1  (0 << 0)
#define TAS5754M_GREF_GPIO2  (1 << 0)
#define TAS5754M_GREF_GPIO3  (2 << 0)
#define TAS5754M_GREF_GPIO4  (3 << 0)
#define TAS5754M_GREF_GPIO5  (4 << 0)
#define TAS5754M_GREF_GPIO6  (5 << 0)

/* Page 0, Register 19 - synchronize */
#define TAS5754M_RQSY        (1 << 0)
#define TAS5754M_RQSY_RESUME (0 << 0)
#define TAS5754M_RQSY_HALT   (1 << 0)

/* Page 0, Register 34 - fs speed mode */
#define TAS5754M_FSSP        (3 << 0)
#define TAS5754M_FSSP_SHIFT  0
#define TAS5754M_FSSP_48KHZ  (0 << 0)
#define TAS5754M_FSSP_96KHZ  (1 << 0)
#define TAS5754M_FSSP_192KHZ (2 << 0)
#define TAS5754M_FSSP_384KHZ (3 << 0)

/* Page 0, Register 37 - Error detection */
#define TAS5754M_IPLK (1 << 0)
#define TAS5754M_DCAS (1 << 1)
#define TAS5754M_IDCM (1 << 2)
#define TAS5754M_IDCH (1 << 3)
#define TAS5754M_IDSK (1 << 4)
#define TAS5754M_IDBK (1 << 5)
#define TAS5754M_IDFS (1 << 6)

/* Page 0, Register 40 - I2S configuration */
#define TAS5754M_ALEN       (3 << 0)
#define TAS5754M_ALEN_SHIFT 0
#define TAS5754M_ALEN_16    (0 << 0)
#define TAS5754M_ALEN_20    (1 << 0)
#define TAS5754M_ALEN_24    (2 << 0)
#define TAS5754M_ALEN_32    (3 << 0)
#define TAS5754M_AFMT       (3 << 4)
#define TAS5754M_AFMT_SHIFT 4
#define TAS5754M_AFMT_I2S   (0 << 4)
#define TAS5754M_AFMT_DSP   (1 << 4)
#define TAS5754M_AFMT_RTJ   (2 << 4)
#define TAS5754M_AFMT_LTJ   (3 << 4)

/* Page 0, Register 42 - DAC routing */
#define TAS5754M_AUPR_SHIFT 0
#define TAS5754M_AUPL_SHIFT 4

/* Page 0, Register 59 - auto mute */
#define TAS5754M_ATMR_SHIFT 0
#define TAS5754M_ATML_SHIFT 4

/* Page 0, Register 63 - ramp rates */
#define TAS5754M_VNDF_SHIFT 6
#define TAS5754M_VNDS_SHIFT 4
#define TAS5754M_VNUF_SHIFT 2
#define TAS5754M_VNUS_SHIFT 0

/* Page 0, Register 64 - emergency ramp rates */
#define TAS5754M_VEDF_SHIFT 6
#define TAS5754M_VEDS_SHIFT 4

/* Page 0, Register 65 - Digital mute enables */
#define TAS5754M_ACTL_SHIFT 2
#define TAS5754M_AMLE_SHIFT 1
#define TAS5754M_AMRE_SHIFT 0

/* Page 0, Register 80-85, GPIO output selection */
#define TAS5754M_GxSL       (31 << 0)
#define TAS5754M_GxSL_SHIFT 0
#define TAS5754M_GxSL_OFF   (0 << 0)
#define TAS5754M_GxSL_DSP   (1 << 0)
#define TAS5754M_GxSL_REG   (2 << 0)
#define TAS5754M_GxSL_AMUTB (3 << 0)
#define TAS5754M_GxSL_AMUTL (4 << 0)
#define TAS5754M_GxSL_AMUTR (5 << 0)
#define TAS5754M_GxSL_CLKI  (6 << 0)
#define TAS5754M_GxSL_SDOUT (7 << 0)
#define TAS5754M_GxSL_ANMUL (8 << 0)
#define TAS5754M_GxSL_ANMUR (9 << 0)
#define TAS5754M_GxSL_PLLLK (10 << 0)
#define TAS5754M_GxSL_CPCLK (11 << 0)
#define TAS5754M_GxSL_UV0_7 (14 << 0)
#define TAS5754M_GxSL_UV0_3 (15 << 0)
#define TAS5754M_GxSL_PLLCK (16 << 0)

/* Page 1, Register 2 - analog volume control */
#define TAS5754M_RAGN_SHIFT 0
#define TAS5754M_LAGN_SHIFT 4

/* Page 1, Register 7 - analog boost control */
#define TAS5754M_AGBR_SHIFT 0
#define TAS5754M_AGBL_SHIFT 4

#endif
