// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_modes.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <linux/module.h>

struct cwu50 {
	struct device *dev;
	struct drm_panel panel;
	struct regulator *supply;
	struct gpio_desc *reset_gpio;
	struct backlight_device *backlight;
	bool prepared;
	bool enabled;
	enum drm_panel_orientation orientation;
};

static const struct drm_display_mode default_mode = {
	.clock = 62500,
	.hdisplay = 720,
	.hsync_start = 720 + 43,
	.hsync_end = 720+ 43 + 20,
	.htotal = 720 + 43 + 20 + 20,
	.vdisplay = 1280,
	.vsync_start = 1280 + 8,
	.vsync_end = 1280 + 8+ 2,
	.vtotal = 1280 + 8 + 2 + 16,
};

static inline struct cwu50 *panel_to_cwu50(struct drm_panel *panel)
{
	return container_of(panel, struct cwu50, panel);
}

#define dcs_write_seq(seq...)                              \
({                                                              \
	static const u8 d[] = { seq };                          \
	mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	 \
})

static void cwu50_init_sequence(struct cwu50 *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	dcs_write_seq(0xE1,0x93);
	dcs_write_seq(0xE2,0x65);
	dcs_write_seq(0xE3,0xF8);
	dcs_write_seq(0x70,0x20);
	dcs_write_seq(0x71,0x13);
	dcs_write_seq(0x72,0x06);
	dcs_write_seq(0x75,0x03);
	dcs_write_seq(0xE0,0x01);
	dcs_write_seq(0x00,0x00);
	dcs_write_seq(0x01,0x47);//VCOM0x47
	dcs_write_seq(0x03,0x00);
	dcs_write_seq(0x04,0x4D);
	dcs_write_seq(0x0C,0x64);
	dcs_write_seq(0x17,0x00);
	dcs_write_seq(0x18,0xBF);
	dcs_write_seq(0x19,0x00);
	dcs_write_seq(0x1A,0x00);
	dcs_write_seq(0x1B,0xBF);
	dcs_write_seq(0x1C,0x00);
	dcs_write_seq(0x1F,0x7E);
	dcs_write_seq(0x20,0x24);
	dcs_write_seq(0x21,0x24);
	dcs_write_seq(0x22,0x4E);
	dcs_write_seq(0x24,0xFE);
	dcs_write_seq(0x37,0x09);
	dcs_write_seq(0x38,0x04);
	dcs_write_seq(0x3C,0x76);
	dcs_write_seq(0x3D,0xFF);
	dcs_write_seq(0x3E,0xFF);
	dcs_write_seq(0x3F,0x7F);
	dcs_write_seq(0x40,0x04);//Dot inversion type
	dcs_write_seq(0x41,0xA0);
	dcs_write_seq(0x44,0x11);
	dcs_write_seq(0x55,0x02);
	dcs_write_seq(0x56,0x01);
	dcs_write_seq(0x57,0x49);
	dcs_write_seq(0x58,0x09);
	dcs_write_seq(0x59,0x2A);
	dcs_write_seq(0x5A,0x1A);
	dcs_write_seq(0x5B,0x1A);
	dcs_write_seq(0x5D,0x78);
	dcs_write_seq(0x5E,0x6E);
	dcs_write_seq(0x5F,0x66);
	dcs_write_seq(0x60,0x5E);
	dcs_write_seq(0x61,0x60);
	dcs_write_seq(0x62,0x54);
	dcs_write_seq(0x63,0x5C);
	dcs_write_seq(0x64,0x47);
	dcs_write_seq(0x65,0x5F);
	dcs_write_seq(0x66,0x5D);
	dcs_write_seq(0x67,0x5B);
	dcs_write_seq(0x68,0x76);
	dcs_write_seq(0x69,0x61);
	dcs_write_seq(0x6A,0x63);
	dcs_write_seq(0x6B,0x50);
	dcs_write_seq(0x6C,0x45);
	dcs_write_seq(0x6D,0x34);
	dcs_write_seq(0x6E,0x1C);
	dcs_write_seq(0x6F,0x07);
	dcs_write_seq(0x70,0x78);
	dcs_write_seq(0x71,0x6E);
	dcs_write_seq(0x72,0x66);
	dcs_write_seq(0x73,0x5E);
	dcs_write_seq(0x74,0x60);
	dcs_write_seq(0x75,0x54);
	dcs_write_seq(0x76,0x5C);
	dcs_write_seq(0x77,0x47);
	dcs_write_seq(0x78,0x5F);
	dcs_write_seq(0x79,0x5D);
	dcs_write_seq(0x7A,0x5B);
	dcs_write_seq(0x7B,0x76);
	dcs_write_seq(0x7C,0x61);
	dcs_write_seq(0x7D,0x63);
	dcs_write_seq(0x7E,0x50);
	dcs_write_seq(0x7F,0x45);
	dcs_write_seq(0x80,0x34);
	dcs_write_seq(0x81,0x1C);
	dcs_write_seq(0x82,0x07);
	dcs_write_seq(0xE0,0x02);
	dcs_write_seq(0x00,0x44);
	dcs_write_seq(0x01,0x46);
	dcs_write_seq(0x02,0x48);
	dcs_write_seq(0x03,0x4A);
	dcs_write_seq(0x04,0x40);
	dcs_write_seq(0x05,0x42);
	dcs_write_seq(0x06,0x1F);
	dcs_write_seq(0x07,0x1F);
	dcs_write_seq(0x08,0x1F);
	dcs_write_seq(0x09,0x1F);
	dcs_write_seq(0x0A,0x1F);
	dcs_write_seq(0x0B,0x1F);
	dcs_write_seq(0x0C,0x1F);
	dcs_write_seq(0x0D,0x1F);
	dcs_write_seq(0x0E,0x1F);
	dcs_write_seq(0x0F,0x1F);
	dcs_write_seq(0x10,0x1F);
	dcs_write_seq(0x11,0x1F);
	dcs_write_seq(0x12,0x1F);
	dcs_write_seq(0x13,0x1F);
	dcs_write_seq(0x14,0x1E);
	dcs_write_seq(0x15,0x1F);
	dcs_write_seq(0x16,0x45);
	dcs_write_seq(0x17,0x47);
	dcs_write_seq(0x18,0x49);
	dcs_write_seq(0x19,0x4B);
	dcs_write_seq(0x1A,0x41);
	dcs_write_seq(0x1B,0x43);
	dcs_write_seq(0x1C,0x1F);
	dcs_write_seq(0x1D,0x1F);
	dcs_write_seq(0x1E,0x1F);
	dcs_write_seq(0x1F,0x1F);
	dcs_write_seq(0x20,0x1F);
	dcs_write_seq(0x21,0x1F);
	dcs_write_seq(0x22,0x1F);
	dcs_write_seq(0x23,0x1F);
	dcs_write_seq(0x24,0x1F);
	dcs_write_seq(0x25,0x1F);
	dcs_write_seq(0x26,0x1F);
	dcs_write_seq(0x27,0x1F);
	dcs_write_seq(0x28,0x1F);
	dcs_write_seq(0x29,0x1F);
	dcs_write_seq(0x2A,0x1E);
	dcs_write_seq(0x2B,0x1F);
	dcs_write_seq(0x2C,0x0B);
	dcs_write_seq(0x2D,0x09);
	dcs_write_seq(0x2E,0x07);
	dcs_write_seq(0x2F,0x05);
	dcs_write_seq(0x30,0x03);
	dcs_write_seq(0x31,0x01);
	dcs_write_seq(0x32,0x1F);
	dcs_write_seq(0x33,0x1F);
	dcs_write_seq(0x34,0x1F);
	dcs_write_seq(0x35,0x1F);
	dcs_write_seq(0x36,0x1F);
	dcs_write_seq(0x37,0x1F);
	dcs_write_seq(0x38,0x1F);
	dcs_write_seq(0x39,0x1F);
	dcs_write_seq(0x3A,0x1F);
	dcs_write_seq(0x3B,0x1F);
	dcs_write_seq(0x3C,0x1F);
	dcs_write_seq(0x3D,0x1F);
	dcs_write_seq(0x3E,0x1F);
	dcs_write_seq(0x3F,0x1F);
	dcs_write_seq(0x40,0x1F);
	dcs_write_seq(0x41,0x1E);
	dcs_write_seq(0x42,0x0A);
	dcs_write_seq(0x43,0x08);
	dcs_write_seq(0x44,0x06);
	dcs_write_seq(0x45,0x04);
	dcs_write_seq(0x46,0x02);
	dcs_write_seq(0x47,0x00);
	dcs_write_seq(0x48,0x1F);
	dcs_write_seq(0x49,0x1F);
	dcs_write_seq(0x4A,0x1F);
	dcs_write_seq(0x4B,0x1F);
	dcs_write_seq(0x4C,0x1F);
	dcs_write_seq(0x4D,0x1F);
	dcs_write_seq(0x4E,0x1F);
	dcs_write_seq(0x4F,0x1F);
	dcs_write_seq(0x50,0x1F);
	dcs_write_seq(0x51,0x1F);
	dcs_write_seq(0x52,0x1F);
	dcs_write_seq(0x53,0x1F);
	dcs_write_seq(0x54,0x1F);
	dcs_write_seq(0x55,0x1F);
	dcs_write_seq(0x56,0x1F);
	dcs_write_seq(0x57,0x1E);
	dcs_write_seq(0x58,0x40);
	dcs_write_seq(0x59,0x00);
	dcs_write_seq(0x5A,0x00);
	dcs_write_seq(0x5B,0x30);
	dcs_write_seq(0x5C,0x02);
	dcs_write_seq(0x5D,0x40);
	dcs_write_seq(0x5E,0x01);
	dcs_write_seq(0x5F,0x02);
	dcs_write_seq(0x60,0x00);
	dcs_write_seq(0x61,0x01);
	dcs_write_seq(0x62,0x02);
	dcs_write_seq(0x63,0x65);
	dcs_write_seq(0x64,0x66);
	dcs_write_seq(0x65,0x00);
	dcs_write_seq(0x66,0x00);
	dcs_write_seq(0x67,0x74);
	dcs_write_seq(0x68,0x06);
	dcs_write_seq(0x69,0x65);
	dcs_write_seq(0x6A,0x66);
	dcs_write_seq(0x6B,0x10);
	dcs_write_seq(0x6C,0x00);
	dcs_write_seq(0x6D,0x04);
	dcs_write_seq(0x6E,0x04);
	dcs_write_seq(0x6F,0x88);
	dcs_write_seq(0x70,0x00);
	dcs_write_seq(0x71,0x00);
	dcs_write_seq(0x72,0x06);
	dcs_write_seq(0x73,0x7B);
	dcs_write_seq(0x74,0x00);
	dcs_write_seq(0x75,0x87);
	dcs_write_seq(0x76,0x00);
	dcs_write_seq(0x77,0x5D);
	dcs_write_seq(0x78,0x17);
	dcs_write_seq(0x79,0x1F);
	dcs_write_seq(0x7A,0x00);
	dcs_write_seq(0x7B,0x00);
	dcs_write_seq(0x7C,0x00);
	dcs_write_seq(0x7D,0x03);
	dcs_write_seq(0x7E,0x7B);
	dcs_write_seq(0xE0,0x04);
	dcs_write_seq(0x09,0x10);
	dcs_write_seq(0xE0,0x00);
	dcs_write_seq(0xE6,0x02);
	dcs_write_seq(0xE7,0x02);
	dcs_write_seq(0x11);// SLPOUT
	msleep (120);
	dcs_write_seq(0x29);// DSPON
	msleep (20);
	dcs_write_seq(0x35,0x00);
}

static int cwu50_disable(struct drm_panel *panel)
{
	struct cwu50 *ctx = panel_to_cwu50(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (!ctx->enabled)
		return 0;

	backlight_disable(ctx->backlight);

	ctx->enabled = false;

	return 0;
}

static int cwu50_unprepare(struct drm_panel *panel)
{
	struct cwu50 *ctx = panel_to_cwu50(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

#if 0
	if (!ctx->prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret) {
		dev_err(ctx->dev, "failed to turn display off (%d)\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret) {
		dev_err(ctx->dev, "failed to enter sleep mode (%d)\n", ret);
		return ret;
	}
	msleep(120);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(5);

	ctx->prepared = false;
#endif

	return 0;
}

static int cwu50_prepare(struct drm_panel *panel)
{
	struct cwu50 *ctx = panel_to_cwu50(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (ctx->prepared)
		return 0;

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(120);

	/* Enabe tearing mode: send TE (tearing effect) at VBLANK */
	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret) {
		dev_err(ctx->dev, "failed to enable vblank TE (%d)\n", ret);
		return ret;
	}
	/* Exit sleep mode and power on */

	cwu50_init_sequence(ctx);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret) {
		dev_err(ctx->dev, "failed to exit sleep mode (%d)\n", ret);
		return ret;
	}
	msleep(120);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret) {
		dev_err(ctx->dev, "failed to turn display on (%d)\n", ret);
		return ret;
	}
	msleep(20);

	ctx->prepared = true;

	return 0;
}

static int cwu50_enable(struct drm_panel *panel)
{
	struct cwu50 *ctx = panel_to_cwu50(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (ctx->enabled)
		return 0;

	backlight_enable(ctx->backlight);

	ctx->enabled = true;

	return 0;
}

static int cwu50_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct cwu50 *ctx = panel_to_cwu50(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "bad mode or failed to add mode\n");
		return -EINVAL;
	}
	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	/* set up connector's "panel orientation" property */
	drm_connector_set_panel_orientation(connector, ctx->orientation);

	drm_mode_probed_add(connector, mode);

	return 1; /* Number of modes */
}

static const struct drm_panel_funcs cwu50_drm_funcs = {
	.disable = cwu50_disable,
	.unprepare = cwu50_unprepare,
	.prepare = cwu50_prepare,
	.enable = cwu50_enable,
	.get_modes = cwu50_get_modes,
};

static int cwu50_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct cwu50 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dev = dev;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_VIDEO_SYNC_PULSE;

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		ret = PTR_ERR(ctx->reset_gpio);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to request GPIO (%d)\n", ret);
		return ret;
	}

	ctx->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(ctx->backlight)) {
		dev_err(ctx->dev, "devm_of_find_backlight");
		return PTR_ERR(ctx->backlight);
	}

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret) {
		dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, ret);
		return ret;
	}

	ctx->panel.prepare_prev_first = true;

	drm_panel_init(&ctx->panel, dev, &cwu50_drm_funcs, DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void cwu50_remove(struct mipi_dsi_device *dsi)
{
	struct cwu50 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id cwu50_of_match[] = {
	{ .compatible = "cw,cwu50" },
	{ }
};
MODULE_DEVICE_TABLE(of, cwu50_of_match);

static struct mipi_dsi_driver cwu50_driver = {
	.probe = cwu50_probe,
	.remove = cwu50_remove,
	.driver = {
		.name = "panel-cwu50",
		.of_match_table = cwu50_of_match,
	},
};
module_mipi_dsi_driver(cwu50_driver);

MODULE_DESCRIPTION("DRM Driver for cwu50 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
