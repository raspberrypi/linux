/*
 * DRM driver for Sitronix ST7796S panels
 *
 * Copyright 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <drm/drm_fb_helper.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm-helpers.h>
#include <video/mipi_display.h>

#define ST7796_NOP         0x00
#define ST7796_SWRESET     0x01
#define ST7796_RDDID       0x04
#define ST7796_RDDST       0x09

#define ST7796_SLPIN       0x10
#define ST7796_SLPOUT      0x11
#define ST7796_PTLON       0x12
#define ST7796_NORON       0x13

#define ST7796_RDMODE      0x0A
#define ST7796_RDMADCTL    0x0B
#define ST7796_RDPIXFMT    0x0C
#define ST7796_RDIMGFMT    0x0A
#define ST7796_RDSELFDIAG  0x0F

#define ST7796_INVOFF      0x20
#define ST7796_INVON       0x21

#define ST7796_DISPOFF     0x28
#define ST7796_DISPON      0x29

#define ST7796_CASET       0x2A
#define ST7796_PASET       0x2B
#define ST7796_RAMWR       0x2C
#define ST7796_RAMRD       0x2E

#define ST7796_PTLAR       0x30
#define ST7796_VSCRDEF     0x33
#define ST7796_MADCTL      0x36
#define ST7796_VSCRSADD    0x37
#define ST7796_PIXFMT      0x3A

#define ST7796_WRDISBV     0x51
#define ST7796_RDDISBV     0x52
#define ST7796_WRCTRLD     0x53

#define ST7796_IFMODE      0xB0
#define ST7796_FRMCTR1     0xB1
#define ST7796_FRMCTR2     0xB2
#define ST7796_FRMCTR3     0xB3
#define ST7796_INVCTR      0xB4
#define ST7796_BPCCTR      0xB5
#define ST7796_DFUNCTR     0xB6
#define ST7796_EMSET       0xB7

#define ST7796_PWCTR1      0xC0
#define ST7796_PWCTR2      0xC1
#define ST7796_PWCTR3      0xC2

#define ST7796_VMCTR1      0xC5
#define ST7796_VMCOFF      0xC6

#define ST7796_RDID4       0xD3

#define ST7796_GMCTRP1     0xE0
#define ST7796_GMCTRN1     0xE1
#define ST7796_DOCACTL     0xE8

#define ST7796_CSCONCTL    0xF0

#define ST7796_MADCTL_MH	BIT(2)
#define ST7796_MADCTL_BGR	BIT(3)
#define ST7796_MADCTL_ML	BIT(4)
#define ST7796_MADCTL_MV	BIT(5)
#define ST7796_MADCTL_MX	BIT(6)
#define ST7796_MADCTL_MY	BIT(7)

static void st7796_enable(struct drm_simple_display_pipe *pipe,
			    struct drm_crtc_state *crtc_state,
			    struct drm_plane_state *plane_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	u8 addr_mode;
	int ret;

	DRM_DEBUG_KMS("\n");

	ret = mipi_dbi_poweron_conditional_reset(mipi);
	if (ret < 0)
		return;
	if (ret == 1)
		goto out_enable;

	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_OFF);
	msleep(120);

	/* Software reset */
	mipi_dbi_command(mipi, ST7796_SWRESET);
	msleep(120);

	/* Sleep exit */
	mipi_dbi_command(mipi, ST7796_SLPOUT);
	msleep(120);

	/* Command Set control */
	mipi_dbi_command(mipi, ST7796_CSCONCTL, 0xC3);    //Enable extension command 2 partI

	/* Command Set control */
	mipi_dbi_command(mipi, ST7796_CSCONCTL, 0x96);    //Enable extension command 2 partII

	/* Memory Data Access Control MX, MY, RGB mode */
	mipi_dbi_command(mipi, ST7796_MADCTL, 0x48);    //X-Mirror, Top-Left to right-Buttom, RGB  

	/* Interface Pixel Format */
	mipi_dbi_command(mipi, ST7796_PIXFMT, 0x05);    //Control interface color format set to 16

	mipi_dbi_command(mipi, ST7796_IFMODE, 0x80);
	mipi_dbi_command(mipi, ST7796_DFUNCTR, 0x00, 0x02);
	mipi_dbi_command(mipi, ST7796_BPCCTR, 0x02, 0x03, 0x00, 0x04);
	mipi_dbi_command(mipi, ST7796_FRMCTR1, 0x80, 0x10);
	mipi_dbi_command(mipi, ST7796_INVCTR, 0x00);
	mipi_dbi_command(mipi, ST7796_EMSET, 0xC6);
	mipi_dbi_command(mipi, ST7796_VMCTR1, 0x24);
	mipi_dbi_command(mipi, 0xE4, 0x31);

	/* Display Output Ctrl Adjust */
	mipi_dbi_command(mipi, ST7796_DOCACTL, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33);

	/* Power control2 */
	mipi_dbi_command(mipi, ST7796_PWCTR2, 0x06);    //VAP(GVDD)=3.85+( vcom+vcom offset), VAN(GVCL)=-3.85+( vcom+vcom offset)

	/* Power control 3 */
	mipi_dbi_command(mipi, ST7796_PWCTR3, 0xA7);    //Source driving current level=low, Gamma driving current level=High

	/* VCOM Control */
	mipi_dbi_command(mipi, ST7796_VMCTR1, 0x18);    //VCOM=0.9
	msleep(120);
	
	/* ST7796 Gamma Sequence */
	/* Gamma"+" */
	mipi_dbi_command(mipi, ST7796_GMCTRP1, 0xF0, 0x09, 0x0b, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B); 
	/* Gamma"-" */
	mipi_dbi_command(mipi, ST7796_GMCTRN1, 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B);
	msleep(120);

	/* Command Set control */
	mipi_dbi_command(mipi, ST7796_CSCONCTL, 0x3C);    //Disable extension command 2 partI
	mipi_dbi_command(mipi, ST7796_CSCONCTL, 0x69);    //Disable extension command 2 partII
	msleep(100);

	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_ON);
	msleep(100);

out_enable:
	/* The PiTFT (ili9340) has a hardware reset circuit that
	 * resets only on power-on and not on each reboot through
	 * a gpio like the rpi-display does.
	 * As a result, we need to always apply the rotation value
	 * regardless of the display "on/off" state.
	 */
	switch (mipi->rotation) {
	default:
		addr_mode = ST7796_MADCTL_MX;
		break;
	case 90:
		addr_mode = ST7796_MADCTL_MV;
		break;
	case 180:
		addr_mode = ST7796_MADCTL_MY;
		break;
	case 270:
		addr_mode = ST7796_MADCTL_MV | ST7796_MADCTL_MY |
			    ST7796_MADCTL_MX;
		break;
	}
	addr_mode |= ST7796_MADCTL_BGR;
	mipi_dbi_command(mipi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);
	mipi_dbi_enable_flush(mipi, crtc_state, plane_state);
}

static const struct drm_simple_display_pipe_funcs st7796_pipe_funcs = {
	.enable = st7796_enable,
	.disable = mipi_dbi_pipe_disable,
	.update = tinydrm_display_pipe_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static const struct drm_display_mode st7796_mode = {
	TINYDRM_MODE(320, 480, 56, 84),
};

DEFINE_DRM_GEM_CMA_FOPS(st7796_fops);

static struct drm_driver st7796_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	.fops			= &st7796_fops,
	TINYDRM_GEM_DRIVER_OPS,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "st7796",
	.desc			= "Sitronix ST7796",
	.date			= "20200418",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id st7796_of_match[] = {
	{ .compatible = "sitronix,st7796" },
	{},
};
MODULE_DEVICE_TABLE(of, st7796_of_match);

static const struct spi_device_id st7796_id[] = {
	{ "st7796", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, st7796_id);

static int st7796_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dbi *mipi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	mipi = devm_kzalloc(dev, sizeof(*mipi), GFP_KERNEL);
	if (!mipi)
		return -ENOMEM;

	mipi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(mipi->reset)) {
		DRM_DEV_ERROR(dev, "Failed to get gpio 'reset'\n");
		return PTR_ERR(mipi->reset);
	}

	dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc)) {
		DRM_DEV_ERROR(dev, "Failed to get gpio 'dc'\n");
		return PTR_ERR(dc);
	}

	mipi->regulator = devm_regulator_get(dev, "power");
	if (IS_ERR(mipi->regulator))
		return PTR_ERR(mipi->regulator);

	mipi->backlight = devm_of_find_backlight(dev);
	if (IS_ERR(mipi->backlight))
		return PTR_ERR(mipi->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, mipi, dc);
	if (ret)
		return ret;

	ret = mipi_dbi_init(&spi->dev, mipi, &st7796_pipe_funcs,
			    &st7796_driver, &st7796_mode, rotation);
	if (ret)
		return ret;

	spi_set_drvdata(spi, mipi);

	return devm_tinydrm_register(&mipi->tinydrm);
}

static void st7796_shutdown(struct spi_device *spi)
{
	struct mipi_dbi *mipi = spi_get_drvdata(spi);

	tinydrm_shutdown(&mipi->tinydrm);
}

static int __maybe_unused st7796_pm_suspend(struct device *dev)
{
	struct mipi_dbi *mipi = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(mipi->tinydrm.drm);
}

static int __maybe_unused st7796_pm_resume(struct device *dev)
{
	struct mipi_dbi *mipi = dev_get_drvdata(dev);

	drm_mode_config_helper_resume(mipi->tinydrm.drm);

	return 0;
}

static const struct dev_pm_ops st7796_pm_ops = {
	//SET_SYSTEM_SLEEP_PM_OPS(st7796_pm_suspend, st7796_pm_resume)
	SET_SYSTEM_SLEEP_PM_OPS(st7796_pm_resume, st7796_pm_suspend)
};

static struct spi_driver st7796_spi_driver = {
	.driver = {
		.name = "st7796",
		.owner = THIS_MODULE,
		.of_match_table = st7796_of_match,
		.pm = &st7796_pm_ops,
	},
	.id_table = st7796_id,
	.probe = st7796_probe,
	.shutdown = st7796_shutdown,
};
module_spi_driver(st7796_spi_driver);

MODULE_DESCRIPTION("Sitronix ST7796 DRM driver");
MODULE_AUTHOR("BIRD TECHSTEP <t.artsamart@gmail.com>");
MODULE_LICENSE("GPL");
