// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 VIEWE
 *
 * DRM panel driver for VIEWE 7" 720x1280 Jadard JD9365TX (module 7KF82).
 * 2-lane MIPI-DSI video mode. Optional reset-gpios; many FPCs use POR only.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#define JD9365TX_DISPOFF_DELAY_MS	80
#define JD9365TX_SLPIN_DELAY_MS		200
#define JD9365TX_DISPON_DELAY_MS	20

struct jd9365tx_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset;
	struct regulator *vdd;
	struct regulator *iovcc;
	enum drm_panel_orientation orientation;
};

static inline struct jd9365tx_panel *to_jd9365tx(struct drm_panel *panel)
{
	return container_of(panel, struct jd9365tx_panel, panel);
}

static void jd9365tx_panel_reset(struct jd9365tx_panel *ctx)
{
	if (!ctx->reset)
		return;

	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset, 0);
	msleep(20);
}

static void jd9365tx_init_sequence(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xdf, 0x90, 0x84, 0x14);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbb, 0x46, 0x55, 0xb5, 0x22, 0x22,
				     0x55);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb7, 0x16, 0x00, 0x6e, 0x44, 0x77);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb3, 0x00, 0x01, 0x50, 0x50, 0x3c,
				     0x3c, 0xa0, 0x00, 0x20, 0xb6);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbc, 0x0c, 0x28, 0x88);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x00, 0x5c, 0x56);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbf, 0x10, 0x35, 0xc3);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc0, 0xbd, 0xbd);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc3, 0x03, 0x09, 0x00, 0x04, 0x2b,
				     0xe2);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc4, 0x05, 0xe3, 0x0d, 0xd1, 0x05,
				     0x2c, 0x06, 0x51, 0x06, 0x51, 0x06, 0x51,
				     0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0xe0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc5, 0x01, 0x00, 0x8c, 0x00, 0x5e,
				     0x01, 0x7d, 0x09, 0x60, 0x02, 0xd0, 0x05,
				     0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
				     0x00, 0x00, 0x00, 0x01, 0x0e, 0x01, 0x92,
				     0x02, 0x00, 0x02, 0xb0, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc6, 0x00, 0xe4, 0x00, 0xc8, 0x00,
				     0x1d, 0x28, 0x82, 0x00, 0x00, 0x00, 0x01,
				     0x00, 0x01, 0x00, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc8, 0x20, 0x80, 0xb4);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcb, 0x7c, 0x66, 0x57, 0x42, 0x32,
				     0x2a, 0x1b, 0x1f, 0x0a, 0x26, 0x27, 0x25,
				     0x44, 0x32, 0x41, 0x35, 0x35, 0x2b, 0x1e,
				     0x14, 0x06, 0x7c, 0x66, 0x57, 0x42, 0x32,
				     0x2a, 0x1b, 0x1f, 0x0a, 0x26, 0x27, 0x2d,
				     0x4c, 0x3a, 0x41, 0x35, 0x35, 0x2b, 0x1e,
				     0x14, 0x06);
	/* 2-lane: 0x31, 4-lane: 0x33 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcc, 0x31);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcd, 0x23, 0x0e, 0x23, 0x0e, 0x23,
				     0x0e, 0x23, 0x0e, 0x23, 0x23, 0x22, 0x22);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xce, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0xaa,
				     0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
				     0xaa, 0xaa);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcf, 0x40, 0x00, 0x00, 0x3f, 0xff,
				     0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x3f, 0xff, 0xff, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd0, 0x00, 0x3f, 0x3f, 0x64, 0x64,
				     0x3f, 0x3f, 0x3e, 0x3e, 0x2a, 0x2b, 0x3f,
				     0x3f, 0x1f, 0x1d, 0x1b, 0x19, 0x17, 0x15,
				     0x13, 0x11, 0x01, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd1, 0x00, 0x3f, 0x3f, 0x64, 0x64,
				     0x3f, 0x3f, 0x3e, 0x3e, 0x2a, 0x2b, 0x3f,
				     0x3f, 0x1e, 0x1c, 0x1a, 0x18, 0x16, 0x14,
				     0x12, 0x10, 0x00, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd2, 0x00, 0x3f, 0x3f, 0x24, 0x24,
				     0x3f, 0x3f, 0x3e, 0x3e, 0x2a, 0x2b, 0x3f,
				     0x3f, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1a,
				     0x1c, 0x1e, 0x02, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd3, 0x00, 0xbf, 0xbf, 0xa4, 0xa4,
				     0xbf, 0x3f, 0x3e, 0x3e, 0x2a, 0x2b, 0x3f,
				     0x3f, 0x11, 0x13, 0x15, 0x17, 0x19, 0x1b,
				     0x1d, 0x1f, 0x03, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd4, 0x00, 0x60, 0x0c, 0x01, 0x03,
				     0x20, 0x04, 0x00, 0x00, 0x00, 0x07, 0x00,
				     0x05, 0x1d, 0x01, 0x23, 0x45, 0x60, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x20,
				     0x05, 0x22, 0x01, 0x03, 0x80, 0x0a, 0x00,
				     0x0a, 0x05, 0x24, 0x05, 0x24, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
				     0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
				     0x20, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd5, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x00,
				     0x00, 0x0a, 0x78, 0x00, 0x00, 0x05, 0xe6,
				     0xf7, 0xa0, 0x0f, 0x08, 0x08, 0x10, 0x00,
				     0x2f, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd7, 0x00, 0x09, 0x7d, 0x09, 0x7d,
				     0x09, 0x7d, 0x09, 0x7d, 0x09, 0x7d, 0x09,
				     0x7d, 0x09, 0x7d);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x01);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc7, 0x14, 0x14, 0x04, 0x04, 0x47);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xcd, 0x30, 0x53);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xce, 0x09, 0x00, 0xe0, 0x00, 0xe0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x02);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb3, 0x4a, 0xa3, 0xe2, 0x2f, 0x43);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb4, 0x7f);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbb, 0x00, 0x7d, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x41, 0x40, 0x43, 0x04);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbd, 0x1b);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbf, 0x0f, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc1, 0x50, 0x40, 0x00, 0x00, 0x00,
				     0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00,
				     0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc2, 0x02, 0x42, 0x50, 0x00, 0x02,
				     0xe0, 0x31);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc3, 0x20, 0xff);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc4, 0x00, 0x11, 0x07, 0x00, 0x0e,
				     0x01, 0x08);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc6, 0x4a, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe5, 0x00, 0x70, 0x70, 0x19, 0xc8,
				     0x09, 0x00, 0x42, 0x01, 0x30, 0x05, 0x00,
				     0x00, 0x05, 0x07);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe6, 0x10, 0x0f, 0x88, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xe9, 0x10, 0xa0, 0xa0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xec, 0x10, 0x77, 0x0d);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x03);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd1, 0x00, 0x00, 0x39, 0xff, 0x08);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xed, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x00);
	/* TEAR_ON; Sleep Out / Display On follow in prepare() */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, MIPI_DCS_SET_TEAR_ON);
	mipi_dsi_msleep(dsi_ctx, 30);
}

static int jd9365tx_prepare(struct drm_panel *panel)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	int ret;

	ret = regulator_enable(ctx->iovcc);
	if (ret)
		return ret;

	ret = regulator_enable(ctx->vdd);
	if (ret) {
		regulator_disable(ctx->iovcc);
		return ret;
	}

	msleep(150);

	jd9365tx_panel_reset(ctx);

	jd9365tx_init_sequence(&dsi_ctx);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, JD9365TX_DISPON_DELAY_MS);
	if (dsi_ctx.accum_err)
		goto err_power;

	return 0;

err_power:
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->vdd);
	regulator_disable(ctx->iovcc);
	return dsi_ctx.accum_err;
}

static int jd9365tx_disable(struct drm_panel *panel)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, JD9365TX_DISPOFF_DELAY_MS);
	return 0;
}

static int jd9365tx_unprepare(struct drm_panel *panel)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, JD9365TX_SLPIN_DELAY_MS);

	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		msleep(20);
	}

	regulator_disable(ctx->vdd);
	regulator_disable(ctx->iovcc);
	msleep(20);

	return 0;
}

/* 800 * 1510 * 50 / 1000 = 60400 */
static const struct drm_display_mode jd9365tx_7kf82_mode = {
	.clock		= 60400,
	.hdisplay	= 720,
	.hsync_start	= 720 + 20,
	.hsync_end	= 720 + 20 + 20,
	.htotal		= 720 + 20 + 20 + 40,
	.vdisplay	= 1280,
	.vsync_start	= 1280 + 200,
	.vsync_end	= 1280 + 200 + 2,
	.vtotal		= 1280 + 200 + 2 + 28,
	.width_mm	= 87,
	.height_mm	= 155,
	.type		= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int jd9365tx_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector,
						    &jd9365tx_7kf82_mode);
}

static enum drm_panel_orientation
jd9365tx_get_orientation(struct drm_panel *panel)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs jd9365tx_funcs = {
	.prepare = jd9365tx_prepare,
	.disable = jd9365tx_disable,
	.unprepare = jd9365tx_unprepare,
	.get_modes = jd9365tx_get_modes,
	.get_orientation = jd9365tx_get_orientation,
};

static int jd9365tx_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct jd9365tx_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct jd9365tx_panel, panel,
				   &jd9365tx_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;

	ctx->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return PTR_ERR(ctx->reset);

	ctx->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ctx->vdd))
		return PTR_ERR(ctx->vdd);

	ctx->iovcc = devm_regulator_get(dev, "iovcc");
	if (IS_ERR(ctx->iovcc))
		return PTR_ERR(ctx->iovcc);

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get orientation\n");

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void jd9365tx_remove(struct mipi_dsi_device *dsi)
{
	struct jd9365tx_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id jd9365tx_of_match[] = {
	{ .compatible = "viewe,7kf82" },
	{ }
};
MODULE_DEVICE_TABLE(of, jd9365tx_of_match);

static struct mipi_dsi_driver jd9365tx_driver = {
	.probe = jd9365tx_probe,
	.remove = jd9365tx_remove,
	.driver = {
		.name = "panel-viewe-jd9365tx",
		.of_match_table = jd9365tx_of_match,
	},
};
module_mipi_dsi_driver(jd9365tx_driver);

MODULE_AUTHOR("Hefei <3066883572@qq.com>");
MODULE_DESCRIPTION("VIEWE 7\" JD9365TX MIPI-DSI panel");
MODULE_LICENSE("GPL");
