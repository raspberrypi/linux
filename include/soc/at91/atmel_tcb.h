//SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018 Microchip */

#ifndef __SOC_ATMEL_TCB_H
#define __SOC_ATMEL_TCB_H

/* Channel registers */
#define ATMEL_TC_COFFS(c)		((c) * 0x40)
#define ATMEL_TC_CCR(c)			ATMEL_TC_COFFS(c)
#define ATMEL_TC_CMR(c)			(ATMEL_TC_COFFS(c) + 0x4)
#define ATMEL_TC_SMMR(c)		(ATMEL_TC_COFFS(c) + 0x8)
#define ATMEL_TC_RAB(c)			(ATMEL_TC_COFFS(c) + 0xc)
#define ATMEL_TC_CV(c)			(ATMEL_TC_COFFS(c) + 0x10)
#define ATMEL_TC_RA(c)			(ATMEL_TC_COFFS(c) + 0x14)
#define ATMEL_TC_RB(c)			(ATMEL_TC_COFFS(c) + 0x18)
#define ATMEL_TC_RC(c)			(ATMEL_TC_COFFS(c) + 0x1c)
#define ATMEL_TC_SR(c)			(ATMEL_TC_COFFS(c) + 0x20)
#define ATMEL_TC_IER(c)			(ATMEL_TC_COFFS(c) + 0x24)
#define ATMEL_TC_IDR(c)			(ATMEL_TC_COFFS(c) + 0x28)
#define ATMEL_TC_IMR(c)			(ATMEL_TC_COFFS(c) + 0x2c)
#define ATMEL_TC_EMR(c)			(ATMEL_TC_COFFS(c) + 0x30)

/* Block registers */
#define ATMEL_TC_BCR			0xc0
#define ATMEL_TC_BMR			0xc4
#define ATMEL_TC_QIER			0xc8
#define ATMEL_TC_QIDR			0xcc
#define ATMEL_TC_QIMR			0xd0
#define ATMEL_TC_QISR			0xd4
#define ATMEL_TC_FMR			0xd8
#define ATMEL_TC_WPMR			0xe4

/* CCR fields */
#define ATMEL_TC_CCR_CLKEN		BIT(0)
#define ATMEL_TC_CCR_CLKDIS		BIT(1)
#define ATMEL_TC_CCR_SWTRG		BIT(2)

/* Common CMR fields */
#define ATMEL_TC_CMR_TCLKS_MSK		GENMASK(2, 0)
#define ATMEL_TC_CMR_TCLK(x)		(x)
#define ATMEL_TC_CMR_XC(x)		((x) + 5)
#define ATMEL_TC_CMR_CLKI		BIT(3)
#define ATMEL_TC_CMR_BURST_MSK		GENMASK(5, 4)
#define ATMEL_TC_CMR_BURST_XC(x)	(((x) + 1) << 4)
#define ATMEL_TC_CMR_WAVE		BIT(15)

/* Capture mode CMR fields */
#define ATMEL_TC_CMR_LDBSTOP		BIT(6)
#define ATMEL_TC_CMR_LDBDIS		BIT(7)
#define ATMEL_TC_CMR_ETRGEDG_MSK	GENMASK(9, 8)
#define ATMEL_TC_CMR_ETRGEDG_NONE	(0 << 8)
#define ATMEL_TC_CMR_ETRGEDG_RISING	(1 << 8)
#define ATMEL_TC_CMR_ETRGEDG_FALLING	(2 << 8)
#define ATMEL_TC_CMR_ETRGEDG_BOTH	(3 << 8)
#define ATMEL_TC_CMR_ABETRG		BIT(10)
#define ATMEL_TC_CMR_CPCTRG		BIT(14)
#define ATMEL_TC_CMR_LDRA_MSK		GENMASK(17, 16)
#define ATMEL_TC_CMR_LDRA_NONE		(0 << 16)
#define ATMEL_TC_CMR_LDRA_RISING	(1 << 16)
#define ATMEL_TC_CMR_LDRA_FALLING	(2 << 16)
#define ATMEL_TC_CMR_LDRA_BOTH		(3 << 16)
#define ATMEL_TC_CMR_LDRB_MSK		GENMASK(19, 18)
#define ATMEL_TC_CMR_LDRB_NONE		(0 << 18)
#define ATMEL_TC_CMR_LDRB_RISING	(1 << 18)
#define ATMEL_TC_CMR_LDRB_FALLING	(2 << 18)
#define ATMEL_TC_CMR_LDRB_BOTH		(3 << 18)
#define ATMEL_TC_CMR_SBSMPLR_MSK	GENMASK(22, 20)
#define ATMEL_TC_CMR_SBSMPLR(x)		((x) << 20)

/* Waveform mode CMR fields */
#define ATMEL_TC_CMR_CPCSTOP		BIT(6)
#define ATMEL_TC_CMR_CPCDIS		BIT(7)
#define ATMEL_TC_CMR_EEVTEDG_MSK	GENMASK(9, 8)
#define ATMEL_TC_CMR_EEVTEDG_NONE	(0 << 8)
#define ATMEL_TC_CMR_EEVTEDG_RISING	(1 << 8)
#define ATMEL_TC_CMR_EEVTEDG_FALLING	(2 << 8)
#define ATMEL_TC_CMR_EEVTEDG_BOTH	(3 << 8)
#define ATMEL_TC_CMR_EEVT_MSK		GENMASK(11, 10)
#define ATMEL_TC_CMR_EEVT_XC(x)		(((x) + 1) << 10)
#define ATMEL_TC_CMR_ENETRG		BIT(12)
#define ATMEL_TC_CMR_WAVESEL_MSK	GENMASK(14, 13)
#define ATMEL_TC_CMR_WAVESEL_UP		(0 << 13)
#define ATMEL_TC_CMR_WAVESEL_UPDOWN	(1 << 13)
#define ATMEL_TC_CMR_WAVESEL_UPRC	(2 << 13)
#define ATMEL_TC_CMR_WAVESEL_UPDOWNRC	(3 << 13)
#define ATMEL_TC_CMR_ACPA_MSK		GENMASK(17, 16)
#define ATMEL_TC_CMR_ACPA(a)		(ATMEL_TC_CMR_ACTION_##a << 16)
#define ATMEL_TC_CMR_ACPC_MSK		GENMASK(19, 18)
#define ATMEL_TC_CMR_ACPC(a)		(ATMEL_TC_CMR_ACTION_##a << 18)
#define ATMEL_TC_CMR_AEEVT_MSK		GENMASK(21, 20)
#define ATMEL_TC_CMR_AEEVT(a)		(ATMEL_TC_CMR_ACTION_##a << 20)
#define ATMEL_TC_CMR_ASWTRG_MSK		GENMASK(23, 22)
#define ATMEL_TC_CMR_ASWTRG(a)		(ATMEL_TC_CMR_ACTION_##a << 22)
#define ATMEL_TC_CMR_BCPB_MSK		GENMASK(25, 24)
#define ATMEL_TC_CMR_BCPB(a)		(ATMEL_TC_CMR_ACTION_##a << 24)
#define ATMEL_TC_CMR_BCPC_MSK		GENMASK(27, 26)
#define ATMEL_TC_CMR_BCPC(a)		(ATMEL_TC_CMR_ACTION_##a << 26)
#define ATMEL_TC_CMR_BEEVT_MSK		GENMASK(29, 28)
#define ATMEL_TC_CMR_BEEVT(a)		(ATMEL_TC_CMR_ACTION_##a << 28)
#define ATMEL_TC_CMR_BSWTRG_MSK		GENMASK(31, 30)
#define ATMEL_TC_CMR_BSWTRG(a)		(ATMEL_TC_CMR_ACTION_##a << 30)
#define ATMEL_TC_CMR_ACTION_NONE	0
#define ATMEL_TC_CMR_ACTION_SET		1
#define ATMEL_TC_CMR_ACTION_CLEAR	2
#define ATMEL_TC_CMR_ACTION_TOGGLE	3

/* SMMR fields */
#define ATMEL_TC_SMMR_GCEN		BIT(0)
#define ATMEL_TC_SMMR_DOWN		BIT(1)

/* SR/IER/IDR/IMR fields */
#define ATMEL_TC_COVFS			BIT(0)
#define ATMEL_TC_LOVRS			BIT(1)
#define ATMEL_TC_CPAS			BIT(2)
#define ATMEL_TC_CPBS			BIT(3)
#define ATMEL_TC_CPCS			BIT(4)
#define ATMEL_TC_LDRAS			BIT(5)
#define ATMEL_TC_LDRBS			BIT(6)
#define ATMEL_TC_ETRGS			BIT(7)
#define ATMEL_TC_CLKSTA			BIT(16)
#define ATMEL_TC_MTIOA			BIT(17)
#define ATMEL_TC_MTIOB			BIT(18)

/* EMR fields */
#define ATMEL_TC_EMR_TRIGSRCA_MSK	GENMASK(1, 0)
#define ATMEL_TC_EMR_TRIGSRCA_TIOA	0
#define ATMEL_TC_EMR_TRIGSRCA_PWMX	1
#define ATMEL_TC_EMR_TRIGSRCB_MSK	GENMASK(5, 4)
#define ATMEL_TC_EMR_TRIGSRCB_TIOB	(0 << 4)
#define ATMEL_TC_EMR_TRIGSRCB_PWM	(1 << 4)
#define ATMEL_TC_EMR_NOCLKDIV		BIT(8)

/* BCR fields */
#define ATMEL_TC_BCR_SYNC		BIT(0)

/* BMR fields */
#define ATMEL_TC_BMR_TCXC_MSK(c)	GENMASK(((c) * 2) + 1, (c) * 2)
#define ATMEL_TC_BMR_TCXC(x, c)		((x) << (2 * (c)))
#define ATMEL_TC_BMR_QDEN		BIT(8)
#define ATMEL_TC_BMR_POSEN		BIT(9)
#define ATMEL_TC_BMR_SPEEDEN		BIT(10)
#define ATMEL_TC_BMR_QDTRANS		BIT(11)
#define ATMEL_TC_BMR_EDGPHA		BIT(12)
#define ATMEL_TC_BMR_INVA		BIT(13)
#define ATMEL_TC_BMR_INVB		BIT(14)
#define ATMEL_TC_BMR_INVIDX		BIT(15)
#define ATMEL_TC_BMR_SWAP		BIT(16)
#define ATMEL_TC_BMR_IDXPHB		BIT(17)
#define ATMEL_TC_BMR_AUTOC		BIT(18)
#define ATMEL_TC_MAXFILT_MSK		GENMASK(25, 20)
#define ATMEL_TC_MAXFILT(x)		(((x) - 1) << 20)
#define ATMEL_TC_MAXCMP_MSK		GENMASK(29, 26)
#define ATMEL_TC_MAXCMP(x)		((x) << 26)

/* QEDC fields */
#define ATMEL_TC_QEDC_IDX		BIT(0)
#define ATMEL_TC_QEDC_DIRCHG		BIT(1)
#define ATMEL_TC_QEDC_QERR		BIT(2)
#define ATMEL_TC_QEDC_MPE		BIT(3)
#define ATMEL_TC_QEDC_DIR		BIT(8)

/* FMR fields */
#define ATMEL_TC_FMR_ENCF(x)		BIT(x)

/* WPMR fields */
#define ATMEL_TC_WPMR_WPKEY		(0x54494d << 8)
#define ATMEL_TC_WPMR_WPEN		BIT(0)

static const u8 atmel_tc_divisors[5] = { 2, 8, 32, 128, 0, };

static const struct of_device_id atmel_tcb_dt_ids[] = {
	{
		.compatible = "atmel,at91rm9200-tcb",
		.data = (void *)16,
	}, {
		.compatible = "atmel,at91sam9x5-tcb",
		.data = (void *)32,
	}, {
		/* sentinel */
	}
};

#endif /* __SOC_ATMEL_TCB_H */
