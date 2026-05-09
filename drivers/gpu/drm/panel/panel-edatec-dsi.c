// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright © 2023 Raspberry Pi Ltd
 *
 * Based on panel-raspberrypi-touchscreen by Broadcom
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/i2c.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pm.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#define ED_DSI_DRIVER_NAME "ed-ts-dsi"

#define REG_PWM		0x01
#define REG_IODIR	0x02
#define REG_PWR		0x03
#define REG_OUTPUT	0x0A

#define CMD_BRIDGE_INIT		0x11
#define CMD_BACKLIGHT_EN 	0x12
#define CMD_FW_VERSION		0xE1

#define PIN_LCD_BL_EN	BIT(0)
#define PIN_LCD_BL_PWM	BIT(1)
#define PIN_LCD_RST		BIT(2)
#define PIN_TP_RST		BIT(3)

static int bl_power = 0;

enum gpio_signals {
	LCD_BL_EN_N,
	LCD_BL_PWM_N,
	LCD_RST_N,
	TP_RST_N,
	NUM_GPIO
};

struct gpio_signal_mappings {
	unsigned int reg;
	unsigned int mask;
};

static const struct gpio_signal_mappings mappings[NUM_GPIO] = {
	[LCD_BL_EN_N] = { REG_OUTPUT, PIN_LCD_BL_EN },
	[LCD_BL_PWM_N] = { REG_OUTPUT, PIN_LCD_BL_PWM },
	[LCD_RST_N] = { REG_OUTPUT, PIN_LCD_RST },
	[TP_RST_N] = { REG_OUTPUT, PIN_TP_RST },
};

struct ed_panel {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;
	struct i2c_client *i2c;
	const struct drm_display_mode *mode;
	
	struct mutex	lock;
	struct regmap	*regmap;
	bool gpio_states[NUM_GPIO];
	u8 port_states;
	struct gpio_chip gc;
	
	enum drm_panel_orientation orientation;
};

static const struct regmap_config ed_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.disable_locking = 1,
	.max_register = REG_OUTPUT,
	.cache_type = REGCACHE_RBTREE,
};

static int ed_set_port_state(struct ed_panel *ts, int reg, u8 val)
{
	ts->port_states = val;
	return regmap_write(ts->regmap, reg, val);
};

static u8 ed_get_port_state(struct ed_panel *ts, int reg)
{
	return ts->port_states;
};

/* 7.0inch 1024x600 */
static const struct drm_display_mode ed_panel_7_0_mode = {
	.clock = 50000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 20,
	.htotal = 1024 + 160 + 20 + 140,
	.vdisplay = 600,
	.vsync_start = 600 + 12,
	.vsync_end = 600 + 12 + 3,
	.vtotal = 600 + 12 + 3 + 20,
};

static const struct drm_display_mode ed_panel_7_0_cm0_mode = {
	.clock = 41000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 40,
	.hsync_end = 1024 + 40 + 10,
	.htotal = 1024 + 40 + 10 + 40,
	.vdisplay = 600,
	.vsync_start = 600 + 4,
	.vsync_end = 600 + 4 + 2,
	.vtotal = 600 + 4 + 2 + 4,
};

static struct ed_panel *panel_to_ts(struct drm_panel *panel)
{
	return container_of(panel, struct ed_panel, base);
}

static void ed_panel_i2c_write(struct ed_panel *ts, u8 reg, u8 val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(ts->i2c, reg, val);
	if (ret)
		dev_err(&ts->i2c->dev, "I2C write failed: %d\n", ret);
}

static int ed_panel_disable(struct drm_panel *panel)
{
	struct ed_panel *ts = panel_to_ts(panel);
	ed_panel_i2c_write(ts, CMD_BACKLIGHT_EN, 0x00);

	return 0;
}

static int ed_panel_unprepare(struct drm_panel *panel)
{
	return 0;
}

static int ed_panel_prepare(struct drm_panel *panel)
{
	return 0;
}

static int ed_panel_enable(struct drm_panel *panel)
{
	struct ed_panel *ts = panel_to_ts(panel);
	ed_panel_i2c_write(ts, CMD_BACKLIGHT_EN, 0x01);
	
	return 0;
}

static int ed_panel_get_modes(struct drm_panel *panel,
				  struct drm_connector *connector)
{
	static const u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
	struct ed_panel *ts = panel_to_ts(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, ts->mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			ts->mode->hdisplay,
			ts->mode->vdisplay,
			drm_mode_vrefresh(ts->mode));
	}

	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	drm_mode_set_name(mode);

	drm_mode_probed_add(connector, mode);

	connector->display_info.bpc = 8;
	connector->display_info.width_mm = 154;
	connector->display_info.height_mm = 86;
	drm_display_info_set_bus_formats(&connector->display_info,
					 &bus_format, 1);

	/*
	 * TODO: Remove once all drm drivers call
	 * drm_connector_set_orientation_from_panel()
	 */
	drm_connector_set_panel_orientation(connector, ts->orientation);

	return 1;
}

static enum drm_panel_orientation ed_panel_get_orientation(struct drm_panel *panel)
{
	struct ed_panel *ts = panel_to_ts(panel);

	return ts->orientation;
}

static const struct drm_panel_funcs ed_panel_funcs = {
	.disable = ed_panel_disable,
	.unprepare = ed_panel_unprepare,
	.prepare = ed_panel_prepare,
	.enable = ed_panel_enable,
	.get_modes = ed_panel_get_modes,
	.get_orientation = ed_panel_get_orientation,
};

static int ed_panel_bl_update_status(struct backlight_device *bl)
{
    struct backlight_properties *props = &(bl->props);
	struct ed_panel *ts = bl_get_data(bl);
    int bl_power_new = props->power;

	mutex_lock(&ts->lock);
	if (bl_power_new != bl_power) {
		ed_panel_i2c_write(ts, REG_PWR, bl_power_new);
		bl_power = bl_power_new;
	} else {
		ed_panel_i2c_write(ts, REG_PWM, backlight_get_brightness(bl));
	}
	mutex_unlock(&ts->lock);

	return 0;
}

static const struct backlight_ops ed_panel_bl_ops = {
	.update_status = ed_panel_bl_update_status,
};

static struct backlight_device *
ed_panel_create_backlight(struct ed_panel *ts)
{
	struct device *dev = ts->base.dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 255,
		.max_brightness = 255,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, ts,
						  &ed_panel_bl_ops, &props);
}

static int ed_gpio_get_direction(struct gpio_chip *gc, unsigned int off)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static int ed_set_bit(struct ed_panel *ts, unsigned int reg, unsigned int pin, bool enabled)
{
	unsigned int mask = BIT(pin);
	unsigned int val  = enabled ? 0xffff : 0x0000;
	
	return regmap_update_bits(ts->regmap, reg, mask, val);
}

static int ed_direction_input(struct gpio_chip *gc, unsigned int off)
{
	struct ed_panel *ts = gpiochip_get_data(gc);
	int status;
	
	mutex_lock(&ts->lock);
	status = ed_set_bit(ts, REG_IODIR, off, true);	   
	mutex_unlock(&ts->lock);
	
	return status;	
}

static int ed_direction_output(struct gpio_chip *gc, unsigned int off, int value)
{
	struct ed_panel *ts = gpiochip_get_data(gc);
	int status;
	u8 last_val;

	mutex_lock(&ts->lock);
	status = ed_set_bit(ts, REG_IODIR, off, false);
	
	last_val = ed_get_port_state(ts, mappings[off].reg);
	if (value)
		last_val |= mappings[off].mask;
	else
		last_val &= ~mappings[off].mask;

	ed_set_port_state(ts, mappings[off].reg, last_val);
	
	mutex_unlock(&ts->lock);
	
	return status;
}

static int ed_gpio_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct ed_panel *ts = gpiochip_get_data(gc);
	u8 last_val;

	if (off >= NUM_GPIO)
		return -1;

	mutex_lock(&ts->lock);

	last_val = ed_get_port_state(ts, mappings[off].reg);
	if (val)
		last_val |= mappings[off].mask;
	else
		last_val &= ~mappings[off].mask;

	ed_set_port_state(ts, mappings[off].reg, last_val);

	mutex_unlock(&ts->lock);
	return 0;
}

static int ed_gpio_get(struct gpio_chip *gc, unsigned int off)
{
	struct ed_panel *ts = gpiochip_get_data(gc);
	u8 last_val;
	int status;
	
	if (off >= NUM_GPIO)
	return -1;

	mutex_lock(&ts->lock);
	last_val = ed_get_port_state(ts, mappings[off].reg);
	status = !!(last_val & BIT(off));
	mutex_unlock(&ts->lock);
	
	return status;
}


static int ed_firmware_version(struct i2c_client *i2c)
{
	struct i2c_msg msgs[1];
	u8 addr_buf[1] = { CMD_FW_VERSION };
	u8 data_buf[16] = { 0 };
	int ret;

	/* Write register address */
	msgs[0].addr = i2c->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = addr_buf;

	ret = i2c_transfer(i2c->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	usleep_range(200, 400);

	/* Read data from register */
	msgs[0].addr = i2c->addr;
	msgs[0].flags = I2C_M_RD;
	msgs[0].len = 16;
	msgs[0].buf = data_buf;

	ret = i2c_transfer(i2c->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;
	
	dev_info(&i2c->dev, "Firmware version: %s\n", data_buf);

	return 0;
}

static int ed_panel_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct ed_panel *ts;
	struct regmap *regmap;
	struct device_node *endpoint, *dsi_host_node;
	struct mipi_dsi_host *host;
	struct mipi_dsi_device_info info = {
		.type = ED_DSI_DRIVER_NAME,
		.channel = 0,
		.node = NULL,
	};
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->mode = of_device_get_match_data(dev);
	if (!ts->mode)
		return -EINVAL;

	ed_firmware_version(i2c);

	mutex_init(&ts->lock);
	i2c_set_clientdata(i2c, ts);

	ts->i2c = i2c;
	
	regmap = devm_regmap_init_i2c(i2c, &ed_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		goto error;
	}
	
	ts->regmap = regmap;
	
	ret = of_drm_get_panel_orientation(dev->of_node, &ts->orientation);
	if (ret) {
		dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, ret);
		goto error;
	}

	/* Look up the DSI host.  It needs to probe before we do. */
	endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
	if (!endpoint)
	{
		ret = -ENODEV;
		goto error;
	}

	dsi_host_node = of_graph_get_remote_port_parent(endpoint);
	if (!dsi_host_node)
	{
		of_node_put(endpoint);
		ret = -ENODEV;
		goto error;
	}

	host = of_find_mipi_dsi_host_by_node(dsi_host_node);
	of_node_put(dsi_host_node);
	if (!host) {
		of_node_put(endpoint);
		ret = -EPROBE_DEFER;
		goto error;
	}

	info.node = of_graph_get_remote_port(endpoint);
	if (!info.node)
	{
		of_node_put(endpoint);
		ret = -ENODEV;
		goto error;
	}

	of_node_put(endpoint);

	ts->dsi = devm_mipi_dsi_device_register_full(dev, host, &info);
	if (IS_ERR(ts->dsi)) {
		dev_err(dev, "DSI device registration failed: %ld\n",
			PTR_ERR(ts->dsi));
		ret = PTR_ERR(ts->dsi);
		goto error;
	}

	if(ts->mode->clock > 42000)
	{
		ed_panel_i2c_write(ts, CMD_BRIDGE_INIT, 0x01);
	}
	else
	{
		ed_panel_i2c_write(ts, CMD_BRIDGE_INIT, 0x02);
	}
	msleep(20);

	drm_panel_init(&ts->base, dev, &ed_panel_funcs,
				DRM_MODE_CONNECTOR_DSI);

	ts->base.backlight = ed_panel_create_backlight(ts);
	if (IS_ERR(ts->base.backlight)) {
		ret = PTR_ERR(ts->base.backlight);
		dev_err(dev, "Failed to create backlight: %d\n", ret);
		goto error;
	}

	/* This appears last, as it's what will unblock the DSI host
	 * driver's component bind function.
	 */
	drm_panel_add(&ts->base);

	ts->dsi->mode_flags = (MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE | MIPI_DSI_MODE_LPM);
	
	ts->dsi->format = MIPI_DSI_FMT_RGB888;
	ts->dsi->lanes = 2;

	ret = devm_mipi_dsi_attach(dev, ts->dsi);
	if (ret) {
		dev_err(dev, "failed to attach dsi to host: %d\n", ret);
		goto error;
	}
	ts->gc.parent = &i2c->dev;
	ts->gc.label = i2c->name;
	ts->gc.owner = THIS_MODULE;
	ts->gc.base = -1;
	ts->gc.ngpio = NUM_GPIO;

	ts->gc.set = ed_gpio_set;
	ts->gc.get = ed_gpio_get;
	ts->gc.get_direction = ed_gpio_get_direction;
	ts->gc.direction_input = ed_direction_input;
	ts->gc.direction_output = ed_direction_output;
	ts->gc.can_sleep = true;

	ret = devm_gpiochip_add_data(&i2c->dev, &ts->gc, ts);
	if (ret) {
		dev_err(&i2c->dev, "Failed to create gpiochip: %d\n", ret);
		goto error;
	}
	
	return 0;

error:
	
	mutex_destroy(&ts->lock);
	return ret;
}

static void ed_panel_remove(struct i2c_client *i2c)
{
	struct ed_panel *ts = i2c_get_clientdata(i2c);

	drm_panel_remove(&ts->base);
	mutex_destroy(&ts->lock);
}

static void ed_panel_shutdown(struct i2c_client *i2c)
{
	struct ed_panel *ts = i2c_get_clientdata(i2c);
	
	ed_panel_i2c_write(ts, CMD_BACKLIGHT_EN, 0x00);
}

static const struct of_device_id ed_panel_of_ids[] = {
	{
		.compatible = "rzw,t70p383rk-v2",
		.data = &ed_panel_7_0_mode,
	}, 
	{
		.compatible = "rzw,t70p383rk-lite",
		.data = &ed_panel_7_0_cm0_mode,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, ed_panel_of_ids);

static struct i2c_driver ed_panel_driver = {
	.driver = {
		.name = "edatec_disp_070c",
		.of_match_table = ed_panel_of_ids,
	},
	.probe = ed_panel_probe,
	.remove = ed_panel_remove,
	.shutdown = ed_panel_shutdown,
};
module_i2c_driver(ed_panel_driver);

MODULE_AUTHOR("EDATEC<support@edatec.cn>");
MODULE_DESCRIPTION("EDATEC TFT LCD panel driver");
MODULE_LICENSE("GPL v2");
