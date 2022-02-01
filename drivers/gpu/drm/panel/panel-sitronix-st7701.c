// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019, Amarula Solutions.
 * Author: Jagan Teki <jagan@amarulasolutions.com>
 */

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <video/mipi_display.h>

#define SPI_DATA_FLAG			0x100

/* Command2 BKx selection command */
#define DSI_CMD2BKX_SEL			0xFF

/* Command2, BK0 commands */
#define DSI_CMD2_BK0_PVGAMCTRL		0xB0 /* Positive Voltage Gamma Control */
#define DSI_CMD2_BK0_NVGAMCTRL		0xB1 /* Negative Voltage Gamma Control */
#define DSI_CMD2_BK0_LNESET		0xC0 /* Display Line setting */
#define DSI_CMD2_BK0_PORCTRL		0xC1 /* Porch control */
#define DSI_CMD2_BK0_INVSEL		0xC2 /* Inversion selection, Frame Rate Control */

/* Command2, BK1 commands */
#define DSI_CMD2_BK1_VRHS		0xB0 /* Vop amplitude setting */
#define DSI_CMD2_BK1_VCOM		0xB1 /* VCOM amplitude setting */
#define DSI_CMD2_BK1_VGHSS		0xB2 /* VGH Voltage setting */
#define DSI_CMD2_BK1_TESTCMD		0xB3 /* TEST Command Setting */
#define DSI_CMD2_BK1_VGLS		0xB5 /* VGL Voltage setting */
#define DSI_CMD2_BK1_PWCTLR1		0xB7 /* Power Control 1 */
#define DSI_CMD2_BK1_PWCTLR2		0xB8 /* Power Control 2 */
#define DSI_CMD2_BK1_SPD1		0xC1 /* Source pre_drive timing set1 */
#define DSI_CMD2_BK1_SPD2		0xC2 /* Source EQ2 Setting */
#define DSI_CMD2_BK1_MIPISET1		0xD0 /* MIPI Setting 1 */

/*
 * Command2 with BK function selection.
 *
 * BIT[4, 0]: [CN2, BKXSEL]
 * 10 = CMD2BK0, Command2 BK0
 * 11 = CMD2BK1, Command2 BK1
 * 00 = Command2 disable
 */
#define DSI_CMD2BK3_SEL			0x13
#define DSI_CMD2BK1_SEL			0x11
#define DSI_CMD2BK0_SEL			0x10
#define DSI_CMD2BKX_SEL_NONE		0x00
#define SPI_CMD2BK3_SEL			(SPI_DATA_FLAG | DSI_CMD2BK3_SEL)
#define SPI_CMD2BK1_SEL			(SPI_DATA_FLAG | DSI_CMD2BK1_SEL)
#define SPI_CMD2BK0_SEL			(SPI_DATA_FLAG | DSI_CMD2BK0_SEL)
#define SPI_CMD2BKX_SEL_NONE		(SPI_DATA_FLAG | DSI_CMD2BKX_SEL_NONE)

/* Command2, BK0 bytes */
#define DSI_LINESET_LINE		0x69
#define DSI_LINESET_LDE_EN		BIT(7)
#define DSI_LINESET_LINEDELTA		GENMASK(1, 0)
#define DSI_CMD2_BK0_LNESET_B1		DSI_LINESET_LINEDELTA
#define DSI_CMD2_BK0_LNESET_B0		(DSI_LINESET_LDE_EN | DSI_LINESET_LINE)
#define DSI_INVSEL_DEFAULT		GENMASK(5, 4)
#define DSI_INVSEL_NLINV		GENMASK(2, 0)
#define DSI_INVSEL_RTNI			GENMASK(2, 1)
#define DSI_CMD2_BK0_INVSEL_B1		DSI_INVSEL_RTNI
#define DSI_CMD2_BK0_INVSEL_B0		(DSI_INVSEL_DEFAULT | DSI_INVSEL_NLINV)
#define DSI_CMD2_BK0_PORCTRL_B0(m)	((m)->vtotal - (m)->vsync_end)
#define DSI_CMD2_BK0_PORCTRL_B1(m)	((m)->vsync_start - (m)->vdisplay)

/* Command2, BK1 bytes */
#define DSI_CMD2_BK1_VRHA_SET		0x45
#define DSI_CMD2_BK1_VCOM_SET		0x13
#define DSI_CMD2_BK1_VGHSS_SET		GENMASK(2, 0)
#define DSI_CMD2_BK1_TESTCMD_VAL	BIT(7)
#define DSI_VGLS_DEFAULT		BIT(6)
#define DSI_VGLS_SEL			GENMASK(2, 0)
#define DSI_CMD2_BK1_VGLS_SET		(DSI_VGLS_DEFAULT | DSI_VGLS_SEL)
#define DSI_PWCTLR1_AP			BIT(7) /* Gamma OP bias, max */
#define DSI_PWCTLR1_APIS		BIT(2) /* Source OP input bias, min */
#define DSI_PWCTLR1_APOS		BIT(0) /* Source OP output bias, min */
#define DSI_CMD2_BK1_PWCTLR1_SET	(DSI_PWCTLR1_AP | DSI_PWCTLR1_APIS | \
					DSI_PWCTLR1_APOS)
#define DSI_PWCTLR2_AVDD		BIT(5) /* AVDD 6.6v */
#define DSI_PWCTLR2_AVCL		0x0    /* AVCL -4.4v */
#define DSI_CMD2_BK1_PWCTLR2_SET	(DSI_PWCTLR2_AVDD | DSI_PWCTLR2_AVCL)
#define DSI_SPD1_T2D			BIT(3)
#define DSI_CMD2_BK1_SPD1_SET		(GENMASK(6, 4) | DSI_SPD1_T2D)
#define DSI_CMD2_BK1_SPD2_SET		DSI_CMD2_BK1_SPD1_SET
#define DSI_MIPISET1_EOT_EN		BIT(3)
#define DSI_CMD2_BK1_MIPISET1_SET	(BIT(7) | DSI_MIPISET1_EOT_EN)

struct st7701;

enum st7701_ctrl_if {
	ST7701_CTRL_DSI,
	ST7701_CTRL_SPI,
};

struct st7701_panel_desc {
	const struct drm_display_mode *mode;
	unsigned int lanes;
	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	u32 mediabus_format;
	const char *const *supply_names;
	unsigned int num_supplies;
	unsigned int panel_sleep_delay;
	void (*init_sequence)(struct st7701 *st7701);
	unsigned int conn_type;
	enum st7701_ctrl_if interface;
	u32 bus_flags;
};

struct st7701 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct spi_device *spi;
	const struct device *dev;

	const struct st7701_panel_desc *desc;

	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset;
	unsigned int sleep_delay;
};

static inline struct st7701 *panel_to_st7701(struct drm_panel *panel)
{
	return container_of(panel, struct st7701, panel);
}

static inline int st7701_dsi_write(struct st7701 *st7701, const void *seq,
				   size_t len)
{
	return mipi_dsi_dcs_write_buffer(st7701->dsi, seq, len);
}

#define ST7701_DSI(st7701, seq...)				\
	{							\
		const u8 d[] = { seq };				\
		st7701_dsi_write(st7701, d, ARRAY_SIZE(d));	\
	}

#define ST7701_SPI(st7701, seq...)				\
	{							\
		const u16 d[] = { seq };			\
		struct spi_transfer xfer = { };			\
		struct spi_message spi;				\
								\
		spi_message_init(&spi);				\
								\
		xfer.tx_buf = d;				\
		xfer.bits_per_word = 9;				\
		xfer.len = sizeof(u16) * ARRAY_SIZE(d);		\
								\
		spi_message_add_tail(&xfer, &spi);		\
		spi_sync((st7701)->spi, &spi);			\
	}

static void ts8550b_init_sequence(struct st7701 *st7701)
{
	const struct drm_display_mode *mode = st7701->desc->mode;

	ST7701_DSI(st7701, MIPI_DCS_SOFT_RESET, 0x00);

	/* We need to wait 5ms before sending new commands */
	msleep(5);

	ST7701_DSI(st7701, MIPI_DCS_EXIT_SLEEP_MODE, 0x00);

	msleep(st7701->sleep_delay);

	/* Command2, BK0 */
	ST7701_DSI(st7701, DSI_CMD2BKX_SEL,
		   0x77, 0x01, 0x00, 0x00, DSI_CMD2BK0_SEL);
	ST7701_DSI(st7701, DSI_CMD2_BK0_PVGAMCTRL, 0x00, 0x0E, 0x15, 0x0F,
		   0x11, 0x08, 0x08, 0x08, 0x08, 0x23, 0x04, 0x13, 0x12,
		   0x2B, 0x34, 0x1F);
	ST7701_DSI(st7701, DSI_CMD2_BK0_NVGAMCTRL, 0x00, 0x0E, 0x95, 0x0F,
		   0x13, 0x07, 0x09, 0x08, 0x08, 0x22, 0x04, 0x10, 0x0E,
		   0x2C, 0x34, 0x1F);
	ST7701_DSI(st7701, DSI_CMD2_BK0_LNESET,
		   DSI_CMD2_BK0_LNESET_B0, DSI_CMD2_BK0_LNESET_B1);
	ST7701_DSI(st7701, DSI_CMD2_BK0_PORCTRL,
		   DSI_CMD2_BK0_PORCTRL_B0(mode),
		   DSI_CMD2_BK0_PORCTRL_B1(mode));
	ST7701_DSI(st7701, DSI_CMD2_BK0_INVSEL,
		   DSI_CMD2_BK0_INVSEL_B0, DSI_CMD2_BK0_INVSEL_B1);

	/* Command2, BK1 */
	ST7701_DSI(st7701, DSI_CMD2BKX_SEL,
			0x77, 0x01, 0x00, 0x00, DSI_CMD2BK1_SEL);
	ST7701_DSI(st7701, DSI_CMD2_BK1_VRHS, DSI_CMD2_BK1_VRHA_SET);
	ST7701_DSI(st7701, DSI_CMD2_BK1_VCOM, DSI_CMD2_BK1_VCOM_SET);
	ST7701_DSI(st7701, DSI_CMD2_BK1_VGHSS, DSI_CMD2_BK1_VGHSS_SET);
	ST7701_DSI(st7701, DSI_CMD2_BK1_TESTCMD, DSI_CMD2_BK1_TESTCMD_VAL);
	ST7701_DSI(st7701, DSI_CMD2_BK1_VGLS, DSI_CMD2_BK1_VGLS_SET);
	ST7701_DSI(st7701, DSI_CMD2_BK1_PWCTLR1, DSI_CMD2_BK1_PWCTLR1_SET);
	ST7701_DSI(st7701, DSI_CMD2_BK1_PWCTLR2, DSI_CMD2_BK1_PWCTLR2_SET);
	ST7701_DSI(st7701, DSI_CMD2_BK1_SPD1, DSI_CMD2_BK1_SPD1_SET);
	ST7701_DSI(st7701, DSI_CMD2_BK1_SPD2, DSI_CMD2_BK1_SPD2_SET);
	ST7701_DSI(st7701, DSI_CMD2_BK1_MIPISET1, DSI_CMD2_BK1_MIPISET1_SET);

	/**
	 * ST7701_SPEC_V1.2 is unable to provide enough information above this
	 * specific command sequence, so grab the same from vendor BSP driver.
	 */
	ST7701_DSI(st7701, 0xE0, 0x00, 0x00, 0x02);
	ST7701_DSI(st7701, 0xE1, 0x0B, 0x00, 0x0D, 0x00, 0x0C, 0x00, 0x0E,
		   0x00, 0x00, 0x44, 0x44);
	ST7701_DSI(st7701, 0xE2, 0x33, 0x33, 0x44, 0x44, 0x64, 0x00, 0x66,
		   0x00, 0x65, 0x00, 0x67, 0x00, 0x00);
	ST7701_DSI(st7701, 0xE3, 0x00, 0x00, 0x33, 0x33);
	ST7701_DSI(st7701, 0xE4, 0x44, 0x44);
	ST7701_DSI(st7701, 0xE5, 0x0C, 0x78, 0x3C, 0xA0, 0x0E, 0x78, 0x3C,
		   0xA0, 0x10, 0x78, 0x3C, 0xA0, 0x12, 0x78, 0x3C, 0xA0);
	ST7701_DSI(st7701, 0xE6, 0x00, 0x00, 0x33, 0x33);
	ST7701_DSI(st7701, 0xE7, 0x44, 0x44);
	ST7701_DSI(st7701, 0xE8, 0x0D, 0x78, 0x3C, 0xA0, 0x0F, 0x78, 0x3C,
		   0xA0, 0x11, 0x78, 0x3C, 0xA0, 0x13, 0x78, 0x3C, 0xA0);
	ST7701_DSI(st7701, 0xEB, 0x02, 0x02, 0x39, 0x39, 0xEE, 0x44, 0x00);
	ST7701_DSI(st7701, 0xEC, 0x00, 0x00);
	ST7701_DSI(st7701, 0xED, 0xFF, 0xF1, 0x04, 0x56, 0x72, 0x3F, 0xFF,
		   0xFF, 0xFF, 0xFF, 0xF3, 0x27, 0x65, 0x40, 0x1F, 0xFF);

	/* disable Command2 */
	ST7701_DSI(st7701, DSI_CMD2BKX_SEL,
		   0x77, 0x01, 0x00, 0x00, DSI_CMD2BKX_SEL_NONE);
}

static void txw210001b0_init_sequence(struct st7701 *st7701)
{
	ST7701_SPI(st7701, MIPI_DCS_SOFT_RESET);

	usleep_range(5000, 7000);

	ST7701_SPI(st7701, DSI_CMD2BKX_SEL,
		   0x177, 0x101, 0x100, 0x100, SPI_CMD2BK0_SEL);

	ST7701_SPI(st7701, DSI_CMD2_BK0_LNESET, 0x13B, 0x100);

	ST7701_SPI(st7701, DSI_CMD2_BK0_PORCTRL, 0x10B, 0x102);

	ST7701_SPI(st7701, DSI_CMD2_BK0_INVSEL, 0x100, 0x102);

	ST7701_SPI(st7701, 0xCC, 0x110);

	/*
	 * Gamma option B:
	 * Positive Voltage Gamma Control
	 */
	ST7701_SPI(st7701, DSI_CMD2_BK0_PVGAMCTRL,
		   0x102, 0x113, 0x11B, 0x10D, 0x110, 0x105, 0x108, 0x107,
		   0x107, 0x124, 0x104, 0x111, 0x10E, 0x12C, 0x133, 0x11D);

	/* Negative Voltage Gamma Control */
	ST7701_SPI(st7701, DSI_CMD2_BK0_NVGAMCTRL,
		   0x105, 0x113, 0x11B, 0x10D, 0x111, 0x105, 0x108, 0x107,
		   0x107, 0x124, 0x104, 0x111, 0x10E, 0x12C, 0x133, 0x11D);

	ST7701_SPI(st7701, DSI_CMD2BKX_SEL,
		   0x177, 0x101, 0x100, 0x100, SPI_CMD2BK1_SEL);

	ST7701_SPI(st7701, DSI_CMD2_BK1_VRHS, 0x15D);

	ST7701_SPI(st7701, DSI_CMD2_BK1_VCOM, 0x143);

	ST7701_SPI(st7701, DSI_CMD2_BK1_VGHSS, 0x181);

	ST7701_SPI(st7701, DSI_CMD2_BK1_TESTCMD, 0x180);

	ST7701_SPI(st7701, DSI_CMD2_BK1_VGLS, 0x143);

	ST7701_SPI(st7701, DSI_CMD2_BK1_PWCTLR1, 0x185);

	ST7701_SPI(st7701, DSI_CMD2_BK1_PWCTLR2, 0x120);

	ST7701_SPI(st7701, DSI_CMD2_BK1_SPD1, 0x178);

	ST7701_SPI(st7701, DSI_CMD2_BK1_SPD2, 0x178);

	ST7701_SPI(st7701, DSI_CMD2_BK1_MIPISET1, 0x188);

	ST7701_SPI(st7701, 0xE0, 0x100, 0x100, 0x102);

	ST7701_SPI(st7701, 0xE1,
		   0x103, 0x1A0, 0x100, 0x100, 0x104, 0x1A0, 0x100, 0x100,
		   0x100, 0x120, 0x120);

	ST7701_SPI(st7701, 0xE2,
		   0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100,
		   0x100, 0x100, 0x100, 0x100, 0x100);

	ST7701_SPI(st7701, 0xE3, 0x100, 0x100, 0x111, 0x100);

	ST7701_SPI(st7701, 0xE4, 0x122, 0x100);

	ST7701_SPI(st7701, 0xE5,
		   0x105, 0x1EC, 0x1A0, 0x1A0, 0x107, 0x1EE, 0x1A0, 0x1A0,
		   0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100);

	ST7701_SPI(st7701, 0xE6, 0x100, 0x100, 0x111, 0x100);

	ST7701_SPI(st7701, 0xE7, 0x122, 0x100);

	ST7701_SPI(st7701, 0xE8,
		   0x106, 0x1ED, 0x1A0, 0x1A0, 0x108, 0x1EF, 0x1A0, 0x1A0,
		   0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100, 0x100);

	ST7701_SPI(st7701, 0xEB,
		   0x100, 0x100, 0x140, 0x140, 0x100, 0x100, 0x100);

	ST7701_SPI(st7701, 0xED,
		   0x1FF, 0x1FF, 0x1FF, 0x1BA, 0x10A, 0x1BF, 0x145, 0x1FF,
		   0x1FF, 0x154, 0x1FB, 0x1A0, 0x1AB, 0x1FF, 0x1FF, 0x1FF);

	ST7701_SPI(st7701, 0xEF, 0x110, 0x10D, 0x104, 0x108, 0x13F, 0x11F);

	ST7701_SPI(st7701, DSI_CMD2BKX_SEL,
		   0x177, 0x101, 0x100, 0x100, SPI_CMD2BK3_SEL);

	ST7701_SPI(st7701, 0xEF, 0x108);

	ST7701_SPI(st7701, DSI_CMD2BKX_SEL,
		   0x177, 0x101, 0x100, 0x100, SPI_CMD2BKX_SEL_NONE);

	ST7701_SPI(st7701, 0xCD, 0x108);  /* RGB format COLCTRL */

	ST7701_SPI(st7701, 0x36, 0x108); /* MadCtl */

	ST7701_SPI(st7701, 0x3A, 0x166);  /* Colmod */

	ST7701_SPI(st7701, MIPI_DCS_EXIT_SLEEP_MODE);
}

static int st7701_prepare(struct drm_panel *panel)
{
	struct st7701 *st7701 = panel_to_st7701(panel);
	int ret;

	gpiod_set_value(st7701->reset, 0);

	ret = regulator_bulk_enable(st7701->desc->num_supplies,
				    st7701->supplies);
	if (ret < 0)
		return ret;
	msleep(20);

	gpiod_set_value(st7701->reset, 1);
	msleep(150);

	st7701->desc->init_sequence(st7701);

	return 0;
}

static int st7701_enable(struct drm_panel *panel)
{
	struct st7701 *st7701 = panel_to_st7701(panel);

	switch (st7701->desc->interface) {
	case ST7701_CTRL_DSI:
		ST7701_DSI(st7701, MIPI_DCS_SET_DISPLAY_ON, 0x00);
		break;
	case ST7701_CTRL_SPI:
		ST7701_SPI(st7701, MIPI_DCS_SET_DISPLAY_ON);
		msleep(30);
		break;
	}

	return 0;
}

static int st7701_disable(struct drm_panel *panel)
{
	struct st7701 *st7701 = panel_to_st7701(panel);

	switch (st7701->desc->interface) {
	case ST7701_CTRL_DSI:
		ST7701_DSI(st7701, MIPI_DCS_SET_DISPLAY_OFF, 0x00);
		break;
	case ST7701_CTRL_SPI:
		ST7701_SPI(st7701, MIPI_DCS_SET_DISPLAY_OFF);
		break;
	}

	return 0;
}

static int st7701_unprepare(struct drm_panel *panel)
{
	struct st7701 *st7701 = panel_to_st7701(panel);

	switch (st7701->desc->interface) {
	case ST7701_CTRL_DSI:
		ST7701_DSI(st7701, MIPI_DCS_ENTER_SLEEP_MODE, 0x00);
		break;
	case ST7701_CTRL_SPI:
		ST7701_SPI(st7701, MIPI_DCS_ENTER_SLEEP_MODE);
		break;
	}

	msleep(st7701->sleep_delay);

	gpiod_set_value(st7701->reset, 0);

	/**
	 * During the Resetting period, the display will be blanked
	 * (The display is entering blanking sequence, which maximum
	 * time is 120 ms, when Reset Starts in Sleep Out –mode. The
	 * display remains the blank state in Sleep In –mode.) and
	 * then return to Default condition for Hardware Reset.
	 *
	 * So we need wait sleep_delay time to make sure reset completed.
	 */
	msleep(st7701->sleep_delay);

	regulator_bulk_disable(st7701->desc->num_supplies, st7701->supplies);

	return 0;
}

static int st7701_get_modes(struct drm_panel *panel,
			    struct drm_connector *connector)
{
	struct st7701 *st7701 = panel_to_st7701(panel);
	const struct drm_display_mode *desc_mode = st7701->desc->mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, desc_mode);
	if (!mode) {
		dev_err(st7701->dev, "failed to add mode %ux%u@%u\n",
			desc_mode->hdisplay, desc_mode->vdisplay,
			drm_mode_vrefresh(desc_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	if (st7701->desc->mediabus_format)
		drm_display_info_set_bus_formats(&connector->display_info,
						 &st7701->desc->mediabus_format,
						 1);
	connector->display_info.bus_flags = 0;

	connector->display_info.width_mm = desc_mode->width_mm;
	connector->display_info.height_mm = desc_mode->height_mm;

	if (st7701->desc->bus_flags)
		connector->display_info.bus_flags = st7701->desc->bus_flags;

	return 1;
}

static const struct drm_panel_funcs st7701_funcs = {
	.disable	= st7701_disable,
	.unprepare	= st7701_unprepare,
	.prepare	= st7701_prepare,
	.enable		= st7701_enable,
	.get_modes	= st7701_get_modes,
};

static const struct drm_display_mode ts8550b_mode = {
	.clock		= 27500,

	.hdisplay	= 480,
	.hsync_start	= 480 + 38,
	.hsync_end	= 480 + 38 + 12,
	.htotal		= 480 + 38 + 12 + 12,

	.vdisplay	= 854,
	.vsync_start	= 854 + 18,
	.vsync_end	= 854 + 18 + 8,
	.vtotal		= 854 + 18 + 8 + 4,

	.width_mm	= 69,
	.height_mm	= 139,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const char * const ts8550b_supply_names[] = {
	"VCC",
	"IOVCC",
};

static const struct st7701_panel_desc ts8550b_desc = {
	.mode = &ts8550b_mode,
	.lanes = 2,
	.flags = MIPI_DSI_MODE_VIDEO,
	.format = MIPI_DSI_FMT_RGB888,
	.supply_names = ts8550b_supply_names,
	.num_supplies = ARRAY_SIZE(ts8550b_supply_names),
	.panel_sleep_delay = 80, /* panel need extra 80ms for sleep out cmd */
	.init_sequence = ts8550b_init_sequence,
	.conn_type = DRM_MODE_CONNECTOR_DSI,
	.interface = ST7701_CTRL_DSI,
};

static const struct drm_display_mode txw210001b0_mode = {
	.clock		= 19200,

	.hdisplay	= 480,
	.hsync_start	= 480 + 10,
	.hsync_end	= 480 + 10 + 16,
	.htotal		= 480 + 10 + 16 + 56,

	.vdisplay	= 480,
	.vsync_start	= 480 + 15,
	.vsync_end	= 480 + 15 + 60,
	.vtotal		= 480 + 15 + 60 + 15,

	.width_mm	= 53,
	.height_mm	= 53,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,

	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct st7701_panel_desc txw210001b0_desc = {
	.mode = &txw210001b0_mode,
	.mediabus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.supply_names = ts8550b_supply_names,
	.num_supplies = ARRAY_SIZE(ts8550b_supply_names),
	.init_sequence = txw210001b0_init_sequence,
	.conn_type = DRM_MODE_CONNECTOR_DPI,
	.interface = ST7701_CTRL_SPI,
	.bus_flags = DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE,
};

static const struct st7701_panel_desc hyperpixel2r_desc = {
	.mode = &txw210001b0_mode,
	.mediabus_format = MEDIA_BUS_FMT_RGB666_1X24_CPADHI,
	.supply_names = ts8550b_supply_names,
	.num_supplies = ARRAY_SIZE(ts8550b_supply_names),
	.init_sequence = txw210001b0_init_sequence,
	.conn_type = DRM_MODE_CONNECTOR_DPI,
	.interface = ST7701_CTRL_SPI,
	.bus_flags = DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE,
};

static int st7701_probe(struct device *dev, struct st7701 **ret_st7701)
{
	const struct st7701_panel_desc *desc;
	struct st7701 *st7701;
	int ret, i;

	st7701 = devm_kzalloc(dev, sizeof(*st7701), GFP_KERNEL);
	if (!st7701)
		return -ENOMEM;

	desc = of_device_get_match_data(dev);
	if (!desc)
		return -EINVAL;

	st7701->supplies = devm_kcalloc(dev, desc->num_supplies,
					sizeof(*st7701->supplies),
					GFP_KERNEL);
	if (!st7701->supplies)
		return -ENOMEM;

	for (i = 0; i < desc->num_supplies; i++)
		st7701->supplies[i].supply = desc->supply_names[i];

	ret = devm_regulator_bulk_get(dev, desc->num_supplies,
				      st7701->supplies);
	if (ret < 0)
		return ret;

	st7701->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(st7701->reset)) {
		dev_err(dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(st7701->reset);
	}

	drm_panel_init(&st7701->panel, dev, &st7701_funcs,
		       desc->conn_type);

	/**
	 * Once sleep out has been issued, ST7701 IC required to wait 120ms
	 * before initiating new commands.
	 *
	 * On top of that some panels might need an extra delay to wait, so
	 * add panel specific delay for those cases. As now this panel specific
	 * delay information is referenced from those panel BSP driver, example
	 * ts8550b and there is no valid documentation for that.
	 */
	st7701->sleep_delay = 120 + desc->panel_sleep_delay;

	ret = drm_panel_of_backlight(&st7701->panel);
	if (ret)
		return ret;

	drm_panel_add(&st7701->panel);

	st7701->desc = desc;
	st7701->dev = dev;

	*ret_st7701 = st7701;

	return 0;
}

static int st7701_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct st7701 *st7701;
	int ret;

	ret = st7701_probe(&dsi->dev, &st7701);

	if (ret)
		return ret;

	dsi->mode_flags = st7701->desc->flags;
	dsi->format = st7701->desc->format;
	dsi->lanes = st7701->desc->lanes;

	mipi_dsi_set_drvdata(dsi, st7701);
	st7701->dsi = dsi;

	return mipi_dsi_attach(dsi);
}

static int st7701_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct st7701 *st7701 = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&st7701->panel);

	return 0;
}

static const struct of_device_id st7701_dsi_of_match[] = {
	{ .compatible = "techstar,ts8550b", .data = &ts8550b_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, st7701_dsi_of_match);

static struct mipi_dsi_driver st7701_dsi_driver = {
	.probe		= st7701_dsi_probe,
	.remove		= st7701_dsi_remove,
	.driver = {
		.name		= "st7701",
		.of_match_table	= st7701_dsi_of_match,
	},
};

/* SPI display  probe */
static const struct of_device_id st7701_spi_of_match[] = {
	{	.compatible = "txw,txw210001b0",
		.data = &txw210001b0_desc,
	}, {
		.compatible = "pimoroni,hyperpixel2round",
		.data = &hyperpixel2r_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, st7701_spi_of_match);

static int st7701_spi_probe(struct spi_device *spi)
{
	struct st7701 *st7701;
	int ret;

	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 9;
	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(&spi->dev, "failed to setup SPI: %d\n", ret);
		return ret;
	}

	ret = st7701_probe(&spi->dev, &st7701);

	if (ret)
		return ret;

	spi_set_drvdata(spi, st7701);
	st7701->spi = spi;

	return 0;
}

static int st7701_spi_remove(struct spi_device *spi)
{
	struct st7701 *ctx = spi_get_drvdata(spi);

	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct spi_device_id st7701_spi_ids[] = {
	{ "txw210001b0", 0 },
	{ "hyperpixel2round", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, st7701_spi_ids);

static struct spi_driver st7701_spi_driver = {
	.probe = st7701_spi_probe,
	.remove = st7701_spi_remove,
	.driver = {
		.name = "st7701",
		.of_match_table = st7701_spi_of_match,
	},
	.id_table = st7701_spi_ids,
};

static int __init panel_st7701_init(void)
{
	int err;

	err = spi_register_driver(&st7701_spi_driver);
	if (err < 0)
		return err;

	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI)) {
		err = mipi_dsi_driver_register(&st7701_dsi_driver);
		if (err < 0)
			goto err_did_spi_register;
	}

	return 0;

err_did_spi_register:
	spi_unregister_driver(&st7701_spi_driver);

	return err;
}
module_init(panel_st7701_init);

static void __exit panel_st7701_exit(void)
{
	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI))
		mipi_dsi_driver_unregister(&st7701_dsi_driver);

	spi_unregister_driver(&st7701_spi_driver);
}
module_exit(panel_st7701_exit);

MODULE_AUTHOR("Jagan Teki <jagan@amarulasolutions.com>");
MODULE_DESCRIPTION("Sitronix ST7701 LCD Panel Driver");
MODULE_LICENSE("GPL");
