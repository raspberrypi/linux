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

struct cwd686 {
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
	.clock = 54465,
	.hdisplay = 480,
	.hsync_start = 480 + 150,
	.hsync_end = 480 + 150 + 24,
	.htotal = 480 + 150 + 24 + 40,
	.vdisplay = 1280,
	.vsync_start = 1280 + 12,
	.vsync_end = 1280 + 12+ 6,
	.vtotal = 1280 + 12 + 6 + 10,
};

static inline struct cwd686 *panel_to_cwd686(struct drm_panel *panel)
{
	return container_of(panel, struct cwd686, panel);
}

#define dcs_write_seq(seq...)                              \
({                                                              \
	static const u8 d[] = { seq };                          \
	mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	 \
})

static void cwd686_init_sequence(struct cwd686 *ctx) 
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	dcs_write_seq(0xF0,0x5A,0x59);
	dcs_write_seq(0xF1,0xA5,0xA6);
	dcs_write_seq(0xB0,0x54,0x32,0x23,0x45,0x44,0x44,0x44,0x44,0x9F,0x00,0x01,0x9F,0x00,0x01);
	dcs_write_seq(0xB1,0x32,0x84,0x02,0x83,0x29,0x06,0x06,0x72,0x06,0x06);
	dcs_write_seq(0xB2,0x73);
	dcs_write_seq(0xB3,0x0B,0x09,0x13,0x11,0x0F,0x0D,0x00,0x00,0x00,0x03,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x05,0x07);
	dcs_write_seq(0xB4,0x0A,0x08,0x12,0x10,0x0E,0x0C,0x00,0x00,0x00,0x03,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x04,0x06);
	dcs_write_seq(0xB6,0x13,0x13);
	dcs_write_seq(0xB8,0xB4,0x43,0x02,0xCC);
	dcs_write_seq(0xB9,0xA5,0x20,0xFF,0xC8);
	dcs_write_seq(0xBA,0x88,0x23);
	dcs_write_seq(0xBD,0x43,0x0E,0x0E,0x50,0x50,0x29,0x10,0x03,0x44,0x03);
	dcs_write_seq(0xC1,0x00,0x0C,0x16,0x04,0x00,0x30,0x10,0x04);
	dcs_write_seq(0xC2,0x21,0x81);
	dcs_write_seq(0xC3,0x02,0x30);
	dcs_write_seq(0xC7,0x25,0x6A);
	dcs_write_seq(0xC8,0x7C,0x68,0x59,0x4E,0x4B,0x3C,0x41,0x2B,0x44,0x43,0x43,0x60,0x4E,0x55,0x47,0x44,0x38,0x27,0x06,0x7C,0x68,0x59,0x4E,0x4B,0x3C,0x41,0x2B,0x44,0x43,0x43,0x60,0x4E,0x55,0x47,0x44,0x38,0x27,0x06);//GAMMA2.2
	//dcs_write_seq(0xC8,0x7C,0x66,0x56,0x4A,0x46,0x37,0x3B,0x24,0x3D,0x3C,0x3A,0x56,0x42,0x48,0x39,0x38,0x2C,0x17,0x06,0x7C,0x66,0x56,0x4A,0x46,0x37,0x3B,0x24,0x3D,0x3C,0x3A,0x56,0x42,0x48,0x39,0x38,0x2C,0x17,0x06);//GAMMA2.5
	//dcs_write_seq(0xC8,0x7C,0x69,0x5B,0x50,0x4E,0x40,0x46,0x31,0x4A,0x49,0x49,0x67,0x56,0x5E,0x51,0x4E,0x41,0x2F,0x06,0x7C,0x69,0x5B,0x50,0x4E,0x40,0x46,0x31,0x4A,0x49,0x49,0x67,0x56,0x5E,0x51,0x4E,0x41,0x2F,0x06);//GAMMA2.0
	//dcs_write_seq(0xC8,0x7C,0x6D,0x60,0x56,0x54,0x47,0x4c,0x37,0x50,0x4e,0x4e,0x6d,0x5c,0x66,0x59,0x56,0x4A,0x36,0x06,0x7C,0x6D,0x60,0x56,0x54,0x47,0x4c,0x37,0x50,0x4e,0x4e,0x6d,0x5c,0x66,0x59,0x56,0x4A,0x36,0x06);//GAMMA1.8
	//dcs_write_seq(0xC8,0x7C,0x6e,0x62,0x59,0x58,0x4b,0x52,0x3d,0x57,0x56,0x56,0x75,0x66,0x71,0x66,0x64,0x55,0x44,0x06,0x7C,0x6e,0x62,0x59,0x58,0x4b,0x52,0x3d,0x57,0x56,0x56,0x75,0x66,0x71,0x66,0x64,0x55,0x44,0x06);//GAMMA1.6
	dcs_write_seq(0xD4,0x00,0x00,0x00,0x32,0x04,0x51);
	dcs_write_seq(0xF1,0x5A,0x59);
	dcs_write_seq(0xF0,0xA5,0xA6);
	dcs_write_seq(0x36,0x14);
	dcs_write_seq(0x35,0x00);
	dcs_write_seq(0x11);
	msleep(120);
	dcs_write_seq(0x29);
	msleep(20);
}

static int cwd686_disable(struct drm_panel *panel)
{
	struct cwd686 *ctx = panel_to_cwd686(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (!ctx->enabled)
		return 0;

	backlight_disable(ctx->backlight);

	ctx->enabled = false;

	return 0;
}

static int cwd686_unprepare(struct drm_panel *panel)
{
	struct cwd686 *ctx = panel_to_cwd686(panel);
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

static int cwd686_prepare(struct drm_panel *panel)
{
	struct cwd686 *ctx = panel_to_cwd686(panel);
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

	cwd686_init_sequence(ctx);

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

static int cwd686_enable(struct drm_panel *panel)
{
	struct cwd686 *ctx = panel_to_cwd686(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (ctx->enabled)
		return 0;

	backlight_enable(ctx->backlight);

	ctx->enabled = true;

	return 0;
}

static int cwd686_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct cwd686 *ctx = panel_to_cwd686(panel);
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

static const struct drm_panel_funcs cwd686_drm_funcs = {
	.disable = cwd686_disable,
	.unprepare = cwd686_unprepare,
	.prepare = cwd686_prepare,
	.enable = cwd686_enable,
	.get_modes = cwd686_get_modes,
};

static int cwd686_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct cwd686 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dev = dev;

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE | MIPI_DSI_MODE_LPM;

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

	drm_panel_init(&ctx->panel, dev, &cwd686_drm_funcs, DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach() failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void cwd686_remove(struct mipi_dsi_device *dsi)
{
	struct cwd686 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id cwd686_of_match[] = {
	{ .compatible = "cw,cwd686" },
	{ }
};
MODULE_DEVICE_TABLE(of, cwd686_of_match);

static struct mipi_dsi_driver cwd686_driver = {
	.probe = cwd686_probe,
	.remove = cwd686_remove,
	.driver = {
		.name = "panel-cwd686",
		.of_match_table = cwd686_of_match,
	},
};
module_mipi_dsi_driver(cwd686_driver);

MODULE_DESCRIPTION("DRM Driver for cwd686 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
