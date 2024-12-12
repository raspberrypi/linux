// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM Driver for DPI output on Raspberry Pi RP1
 *
 * Copyright (c) 2023 Raspberry Pi Limited.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/printk.h>
#include <linux/console.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/cred.h>
#include <linux/media-bus-format.h>
#include <linux/pinctrl/consumer.h>
#include <drm/drm_drv.h>
#include <drm/drm_mm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_ttm.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_vblank.h>
#include <drm/drm_of.h>

#include "rp1_dpi.h"

/*
 * Default bus format, where not specified by a connector/bridge
 * and not overridden by the OF property "default_bus_fmt".
 * This value is for compatibility with vc4 and VGA666-style boards,
 * even though RP1 hardware cannot achieve the full 18-bit depth
 * with that pinout (MEDIA_BUS_FMT_RGB666_1X24_CPADHI is preferred).
 */
static unsigned int default_bus_fmt = MEDIA_BUS_FMT_RGB666_1X18;
module_param(default_bus_fmt, uint, 0644);

/* -------------------------------------------------------------- */

static void rp1dpi_pipe_update(struct drm_simple_display_pipe *pipe,
			       struct drm_plane_state *old_state)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;
	struct drm_framebuffer *fb = pipe->plane.state->fb;
	struct rp1_dpi *dpi = pipe->crtc.dev->dev_private;
	struct drm_gem_object *gem = fb ? drm_gem_fb_get_obj(fb, 0) : NULL;
	struct drm_gem_dma_object *dma_obj = gem ? to_drm_gem_dma_obj(gem) : NULL;
	bool can_update = fb && dma_obj && dpi && dpi->pipe_enabled;

	/* (Re-)start DPI-DMA where required; and update FB address */
	if (can_update) {
		if (!dpi->dpi_running || fb->format->format != dpi->cur_fmt) {
			if (dpi->dpi_running &&
			    fb->format->format != dpi->cur_fmt) {
				rp1dpi_hw_stop(dpi);
				rp1dpi_pio_stop(dpi);
				dpi->dpi_running = false;
			}
			if (!dpi->dpi_running) {
				rp1dpi_hw_setup(dpi,
						fb->format->format,
						dpi->bus_fmt,
						dpi->de_inv,
						&pipe->crtc.state->mode);
				rp1dpi_pio_start(dpi, &pipe->crtc.state->mode);
				dpi->dpi_running = true;
			}
			dpi->cur_fmt = fb->format->format;
			drm_crtc_vblank_on(&pipe->crtc);
		}
		rp1dpi_hw_update(dpi, dma_obj->dma_addr, fb->offsets[0], fb->pitches[0]);
	}

	/* Arm VBLANK event (or call it immediately in some error cases) */
	spin_lock_irqsave(&pipe->crtc.dev->event_lock, flags);
	event = pipe->crtc.state->event;
	if (event) {
		pipe->crtc.state->event = NULL;
		if (can_update && drm_crtc_vblank_get(&pipe->crtc) == 0)
			drm_crtc_arm_vblank_event(&pipe->crtc, event);
		else
			drm_crtc_send_vblank_event(&pipe->crtc, event);
	}
	spin_unlock_irqrestore(&pipe->crtc.dev->event_lock, flags);
}

static void rp1dpi_pipe_enable(struct drm_simple_display_pipe *pipe,
			       struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	static const unsigned int M = 1000000;
	struct rp1_dpi *dpi = pipe->crtc.dev->dev_private;
	struct drm_connector *conn;
	struct drm_connector_list_iter conn_iter;
	unsigned int fpix, fdiv, fvco;
	int ret;

	/* Look up the connector attached to DPI so we can get the
	 * bus_format.  Ideally the bridge would tell us the
	 * bus_format we want, but it doesn't yet, so assume that it's
	 * uniform throughout the bridge chain.
	 */
	dev_info(&dpi->pdev->dev, __func__);
	drm_connector_list_iter_begin(pipe->encoder.dev, &conn_iter);
	drm_for_each_connector_iter(conn, &conn_iter) {
		if (conn->encoder == &pipe->encoder) {
			dpi->de_inv = !!(conn->display_info.bus_flags &
							DRM_BUS_FLAG_DE_LOW);
			dpi->clk_inv = !!(conn->display_info.bus_flags &
						DRM_BUS_FLAG_PIXDATA_DRIVE_NEGEDGE);
			if (conn->display_info.num_bus_formats)
				dpi->bus_fmt = conn->display_info.bus_formats[0];
			break;
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	/* Set DPI clock to desired frequency. Currently (experimentally)
	 * we take control of the VideoPLL, to ensure we can generate it
	 * accurately. NB: this prevents concurrent use of DPI and VEC!
	 * Magic numbers ensure the parent clock is within [100MHz, 200MHz]
	 * with VCO in [1GHz, 1.33GHz]. The initial divide is by 6, 8 or 10.
	 */
	fpix = 1000 * pipe->crtc.state->mode.clock;
	fpix = clamp(fpix, 1 * M, 200 * M);
	fdiv = fpix;
	while (fdiv < 100 * M)
		fdiv *= 2;
	fvco = fdiv * 2 * DIV_ROUND_UP(500 * M, fdiv);
	ret = clk_set_rate(dpi->clocks[RP1DPI_CLK_PLLCORE], fvco);
	if (ret)
		dev_err(&dpi->pdev->dev, "Failed to set PLL VCO to %u (%d)", fvco, ret);
	ret = clk_set_rate(dpi->clocks[RP1DPI_CLK_PLLDIV], fdiv);
	if (ret)
		dev_err(&dpi->pdev->dev, "Failed to set PLL output to %u (%d)", fdiv, ret);
	ret = clk_set_rate(dpi->clocks[RP1DPI_CLK_DPI], fpix);
	if (ret)
		dev_err(&dpi->pdev->dev, "Failed to set DPI clock to %u (%d)", fpix, ret);

	rp1dpi_vidout_setup(dpi, dpi->clk_inv);
	clk_prepare_enable(dpi->clocks[RP1DPI_CLK_PLLCORE]);
	clk_prepare_enable(dpi->clocks[RP1DPI_CLK_PLLDIV]);
	pinctrl_pm_select_default_state(&dpi->pdev->dev);
	clk_prepare_enable(dpi->clocks[RP1DPI_CLK_DPI]);
	dev_info(&dpi->pdev->dev, "Want %u /%u %u /%u %u; got VCO=%lu DIV=%lu DPI=%lu",
		 fvco, fvco / fdiv, fdiv, fdiv / fpix, fpix,
		 clk_get_rate(dpi->clocks[RP1DPI_CLK_PLLCORE]),
		 clk_get_rate(dpi->clocks[RP1DPI_CLK_PLLDIV]),
		 clk_get_rate(dpi->clocks[RP1DPI_CLK_DPI]));

	/* Start DPI-DMA. pipe already has the new crtc and plane state. */
	dpi->pipe_enabled = true;
	dpi->cur_fmt = 0xdeadbeef;
	rp1dpi_pipe_update(pipe, 0);
}

static void rp1dpi_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct rp1_dpi *dpi = pipe->crtc.dev->dev_private;

	dev_info(&dpi->pdev->dev, __func__);
	drm_crtc_vblank_off(&pipe->crtc);
	if (dpi->dpi_running) {
		rp1dpi_hw_stop(dpi);
		rp1dpi_pio_stop(dpi);
		dpi->dpi_running = false;
	}
	clk_disable_unprepare(dpi->clocks[RP1DPI_CLK_DPI]);
	pinctrl_pm_select_sleep_state(&dpi->pdev->dev);
	clk_disable_unprepare(dpi->clocks[RP1DPI_CLK_PLLDIV]);
	clk_disable_unprepare(dpi->clocks[RP1DPI_CLK_PLLCORE]);
	dpi->pipe_enabled = false;
}

static int rp1dpi_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct rp1_dpi *dpi = pipe->crtc.dev->dev_private;

	if (dpi)
		rp1dpi_hw_vblank_ctrl(dpi, 1);

	return 0;
}

static void rp1dpi_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct rp1_dpi *dpi = pipe->crtc.dev->dev_private;

	if (dpi)
		rp1dpi_hw_vblank_ctrl(dpi, 0);
}

static enum drm_mode_status rp1dpi_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
						   const struct drm_display_mode *mode)
{
#if !IS_REACHABLE(CONFIG_RP1_PIO)
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;
#endif
	if (mode->clock < 1000) /* 1 MHz */
		return MODE_CLOCK_LOW;
	if (mode->clock > 200000) /* 200 MHz */
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static const struct drm_simple_display_pipe_funcs rp1dpi_pipe_funcs = {
	.enable	    = rp1dpi_pipe_enable,
	.update	    = rp1dpi_pipe_update,
	.disable    = rp1dpi_pipe_disable,
	.enable_vblank	= rp1dpi_pipe_enable_vblank,
	.disable_vblank = rp1dpi_pipe_disable_vblank,
	.mode_valid = rp1dpi_pipe_mode_valid,
};

static const struct drm_mode_config_funcs rp1dpi_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void rp1dpi_stopall(struct drm_device *drm)
{
	if (drm->dev_private) {
		struct rp1_dpi *dpi = drm->dev_private;

		if (dpi->dpi_running || rp1dpi_hw_busy(dpi)) {
			rp1dpi_hw_stop(dpi);
			clk_disable_unprepare(dpi->clocks[RP1DPI_CLK_DPI]);
			rp1dpi_pio_stop(dpi);
			dpi->dpi_running = false;
		}
		rp1dpi_vidout_poweroff(dpi);
		pinctrl_pm_select_sleep_state(&dpi->pdev->dev);
	}
}

DEFINE_DRM_GEM_DMA_FOPS(rp1dpi_fops);

static struct drm_driver rp1dpi_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &rp1dpi_fops,
	.name			= "drm-rp1-dpi",
	.desc			= "drm-rp1-dpi",
	.date			= "0",
	.major			= 1,
	.minor			= 0,
	DRM_GEM_DMA_DRIVER_OPS,
	.release		= rp1dpi_stopall,
};

static const u32 rp1dpi_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565
};

static int rp1dpi_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rp1_dpi *dpi;
	struct drm_bridge *bridge = NULL;
	struct drm_panel *panel;
	int i, j, ret;

	dev_info(dev, __func__);
	ret = drm_of_find_panel_or_bridge(pdev->dev.of_node, 0, 0,
					  &panel, &bridge);
	if (ret) {
		dev_info(dev, "%s: bridge not found\n", __func__);
		return -EPROBE_DEFER;
	}
	if (panel) {
		bridge = devm_drm_panel_bridge_add(dev, panel);
		if (IS_ERR(bridge))
			return PTR_ERR(bridge);
	}

	dpi = devm_drm_dev_alloc(dev, &rp1dpi_driver, struct rp1_dpi, drm);
	if (IS_ERR(dpi)) {
		ret = PTR_ERR(dpi);
		dev_err(dev, "%s devm_drm_dev_alloc %d", __func__, ret);
		return ret;
	}
	dpi->pdev = pdev;
	spin_lock_init(&dpi->hw_lock);

	dpi->bus_fmt = default_bus_fmt;
	ret = of_property_read_u32(dev->of_node, "default_bus_fmt", &dpi->bus_fmt);

	for (i = 0; i < RP1DPI_NUM_HW_BLOCKS; i++) {
		dpi->hw_base[i] =
			devm_ioremap_resource(dev,
					      platform_get_resource(dpi->pdev, IORESOURCE_MEM, i));
		if (IS_ERR(dpi->hw_base[i])) {
			dev_err(dev, "Error memory mapping regs[%d]\n", i);
			return PTR_ERR(dpi->hw_base[i]);
		}
	}
	ret = platform_get_irq(dpi->pdev, 0);
	if (ret > 0)
		ret = devm_request_irq(dev, ret, rp1dpi_hw_isr,
				       IRQF_SHARED, "rp1-dpi", dpi);
	if (ret) {
		dev_err(dev, "Unable to request interrupt\n");
		return -EINVAL;
	}

	for (i = 0; i < RP1DPI_NUM_CLOCKS; i++) {
		static const char * const myclocknames[RP1DPI_NUM_CLOCKS] = {
			"dpiclk", "plldiv", "pllcore"
		};
		dpi->clocks[i] = devm_clk_get(dev, myclocknames[i]);
		if (IS_ERR(dpi->clocks[i])) {
			dev_err(dev, "Unable to request clock %s\n", myclocknames[i]);
			return PTR_ERR(dpi->clocks[i]);
		}
	}

	ret = drmm_mode_config_init(&dpi->drm);
	if (ret)
		goto done_err;

	/* Check if PIO can snoop on or override DPI's GPIO1 */
	dpi->gpio1_used = false;
	for (i = 0; !dpi->gpio1_used; i++) {
		u32 p = 0;
		const char *str = NULL;
		struct device_node *np1 = of_parse_phandle(dev->of_node, "pinctrl-0", i);

		if (!np1)
			break;

		if (!of_property_read_string(np1, "function", &str) && !strcmp(str, "dpi")) {
			for (j = 0; !dpi->gpio1_used; j++) {
				if (of_property_read_string_index(np1, "pins", j, &str))
					break;
				if (!strcmp(str, "gpio1"))
					dpi->gpio1_used = true;
			}
			for (j = 0; !dpi->gpio1_used; j++) {
				if (of_property_read_u32_index(np1, "brcm,pins", j, &p))
					break;
				if (p == 1)
					dpi->gpio1_used = true;
			}
		}
		of_node_put(np1);
	}

	/* Now we have all our resources, finish driver initialization */
	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	init_completion(&dpi->finished);
	dpi->drm.dev_private = dpi;
	platform_set_drvdata(pdev, &dpi->drm);

	dpi->drm.mode_config.max_width  = 4096;
	dpi->drm.mode_config.max_height = 4096;
	dpi->drm.mode_config.preferred_depth = 32;
	dpi->drm.mode_config.prefer_shadow   = 0;
	dpi->drm.mode_config.quirk_addfb_prefer_host_byte_order = true;
	dpi->drm.mode_config.funcs = &rp1dpi_mode_funcs;
	drm_vblank_init(&dpi->drm, 1);

	ret = drm_simple_display_pipe_init(&dpi->drm,
					   &dpi->pipe,
					   &rp1dpi_pipe_funcs,
					   rp1dpi_formats,
					   ARRAY_SIZE(rp1dpi_formats),
					   NULL, NULL);
	if (!ret)
		ret = drm_simple_display_pipe_attach_bridge(&dpi->pipe, bridge);
	if (ret)
		goto done_err;

	drm_mode_config_reset(&dpi->drm);

	ret = drm_dev_register(&dpi->drm, 0);
	if (ret)
		return ret;

	drm_fbdev_ttm_setup(&dpi->drm, 32);
	return ret;

done_err:
	dev_err(dev, "%s fail %d\n", __func__, ret);
	return ret;
}

static void rp1dpi_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	rp1dpi_stopall(drm);
	drm_dev_unregister(drm);
	drm_atomic_helper_shutdown(drm);
	drm_dev_put(drm);
}

static void rp1dpi_platform_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	rp1dpi_stopall(drm);
}

static const struct of_device_id rp1dpi_of_match[] = {
	{
		.compatible = "raspberrypi,rp1dpi",
	},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, rp1dpi_of_match);

static struct platform_driver rp1dpi_platform_driver = {
	.probe		= rp1dpi_platform_probe,
	.remove		= rp1dpi_platform_remove,
	.shutdown	= rp1dpi_platform_shutdown,
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = rp1dpi_of_match,
	},
};

module_platform_driver(rp1dpi_platform_driver);

MODULE_AUTHOR("Nick Hollinghurst");
MODULE_DESCRIPTION("DRM driver for DPI output on Raspberry Pi RP1");
MODULE_LICENSE("GPL");
