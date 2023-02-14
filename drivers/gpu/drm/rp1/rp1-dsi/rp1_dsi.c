// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM Driver for DSI output on Raspberry Pi RP1
 *
 * Copyright (c) 2023 Raspberry Pi Limited.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/phy/phy-mipi-dphy.h>
#include <linux/string.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "rp1_dsi.h"

static inline struct rp1_dsi *
bridge_to_rp1_dsi(struct drm_bridge *bridge)
{
	return container_of(bridge, struct rp1_dsi, bridge);
}

static void rp1_dsi_bridge_pre_enable(struct drm_bridge *bridge,
				      struct drm_bridge_state *old_state)
{
	struct rp1_dsi *dsi = bridge_to_rp1_dsi(bridge);

	rp1dsi_dsi_setup(dsi, &dsi->pipe.crtc.state->adjusted_mode);
}

static void rp1_dsi_bridge_enable(struct drm_bridge *bridge,
				  struct drm_bridge_state *old_state)
{
}

static void rp1_dsi_bridge_disable(struct drm_bridge *bridge,
				   struct drm_bridge_state *state)
{
}

static void rp1_dsi_bridge_post_disable(struct drm_bridge *bridge,
					struct drm_bridge_state *state)
{
	struct rp1_dsi *dsi = bridge_to_rp1_dsi(bridge);

	if (dsi->dsi_running) {
		rp1dsi_dsi_stop(dsi);
		dsi->dsi_running = false;
	}
}

static int rp1_dsi_bridge_attach(struct drm_bridge *bridge,
				 enum drm_bridge_attach_flags flags)
{
	struct rp1_dsi *dsi = bridge_to_rp1_dsi(bridge);

	/* Attach the panel or bridge to the dsi bridge */
	return drm_bridge_attach(bridge->encoder, dsi->out_bridge,
				 &dsi->bridge, flags);
	return 0;
}

static const struct drm_bridge_funcs rp1_dsi_bridge_funcs = {
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_pre_enable = rp1_dsi_bridge_pre_enable,
	.atomic_enable = rp1_dsi_bridge_enable,
	.atomic_disable = rp1_dsi_bridge_disable,
	.atomic_post_disable = rp1_dsi_bridge_post_disable,
	.attach = rp1_dsi_bridge_attach,
};

static void rp1dsi_pipe_update(struct drm_simple_display_pipe *pipe,
			       struct drm_plane_state *old_state)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;
	struct drm_framebuffer *fb = pipe->plane.state->fb;
	struct rp1_dsi *dsi = pipe->crtc.dev->dev_private;
	struct drm_gem_object *gem = fb ? drm_gem_fb_get_obj(fb, 0) : NULL;
	struct drm_gem_dma_object *dma_obj = gem ? to_drm_gem_dma_obj(gem) : NULL;
	bool can_update = fb && dma_obj && dsi && dsi->pipe_enabled;

	/* (Re-)start DSI,DMA where required; and update FB address */
	if (can_update) {
		if (!dsi->dma_running || fb->format->format != dsi->cur_fmt) {
			if (dsi->dma_running && fb->format->format != dsi->cur_fmt) {
				rp1dsi_dma_stop(dsi);
				dsi->dma_running = false;
			}
			if (!dsi->dma_running) {
				rp1dsi_dma_setup(dsi,
						 fb->format->format, dsi->display_format,
						&pipe->crtc.state->adjusted_mode);
				dsi->dma_running = true;
			}
			dsi->cur_fmt  = fb->format->format;
			drm_crtc_vblank_on(&pipe->crtc);
		}
		rp1dsi_dma_update(dsi, dma_obj->dma_addr, fb->offsets[0], fb->pitches[0]);
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

static inline struct rp1_dsi *
encoder_to_rp1_dsi(struct drm_encoder *encoder)
{
	struct drm_simple_display_pipe *pipe =
		container_of(encoder, struct drm_simple_display_pipe, encoder);
	return container_of(pipe, struct rp1_dsi, pipe);
}

static void rp1dsi_encoder_enable(struct drm_encoder *encoder)
{
	struct rp1_dsi *dsi = encoder_to_rp1_dsi(encoder);

	/* Put DSI into video mode before starting video */
	rp1dsi_dsi_set_cmdmode(dsi, 0);

	/* Start DMA -> DPI */
	dsi->pipe_enabled = true;
	dsi->cur_fmt = 0xdeadbeef;
	rp1dsi_pipe_update(&dsi->pipe, 0);
}

static void rp1dsi_encoder_disable(struct drm_encoder *encoder)
{
	struct rp1_dsi *dsi = encoder_to_rp1_dsi(encoder);

	drm_crtc_vblank_off(&dsi->pipe.crtc);
	if (dsi->dma_running) {
		rp1dsi_dma_stop(dsi);
		dsi->dma_running = false;
	}
	dsi->pipe_enabled = false;

	/* Return to command mode after stopping video */
	rp1dsi_dsi_set_cmdmode(dsi, 1);
}

static const struct drm_encoder_helper_funcs rp1_dsi_encoder_funcs = {
	.enable = rp1dsi_encoder_enable,
	.disable = rp1dsi_encoder_disable,
};

static void rp1dsi_pipe_enable(struct drm_simple_display_pipe *pipe,
			       struct drm_crtc_state *crtc_state,
			       struct drm_plane_state *plane_state)
{
}

static void rp1dsi_pipe_disable(struct drm_simple_display_pipe *pipe)
{
}

static int rp1dsi_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct rp1_dsi *dsi = pipe->crtc.dev->dev_private;

	if (dsi)
		rp1dsi_dma_vblank_ctrl(dsi, 1);

	return 0;
}

static void rp1dsi_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct rp1_dsi *dsi = pipe->crtc.dev->dev_private;

	if (dsi)
		rp1dsi_dma_vblank_ctrl(dsi, 0);
}

static const struct drm_simple_display_pipe_funcs rp1dsi_pipe_funcs = {
	.enable	    = rp1dsi_pipe_enable,
	.update	    = rp1dsi_pipe_update,
	.disable    = rp1dsi_pipe_disable,
	.prepare_fb = drm_gem_simple_display_pipe_prepare_fb,
	.enable_vblank  = rp1dsi_pipe_enable_vblank,
	.disable_vblank = rp1dsi_pipe_disable_vblank,
};

static const struct drm_mode_config_funcs rp1dsi_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const u32 rp1dsi_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565
};

static void rp1dsi_stopall(struct drm_device *drm)
{
	if (drm->dev_private) {
		struct rp1_dsi *dsi = drm->dev_private;

		if (dsi->dma_running || rp1dsi_dma_busy(dsi)) {
			rp1dsi_dma_stop(dsi);
			dsi->dma_running = false;
		}
		if (dsi->dsi_running) {
			rp1dsi_dsi_stop(dsi);
			dsi->dsi_running = false;
		}
		if (dsi->clocks[RP1DSI_CLOCK_CFG])
			clk_disable_unprepare(dsi->clocks[RP1DSI_CLOCK_CFG]);
	}
}

DEFINE_DRM_GEM_DMA_FOPS(rp1dsi_fops);

static struct drm_driver rp1dsi_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &rp1dsi_fops,
	.name			= "drm-rp1-dsi",
	.desc			= "drm-rp1-dsi",
	.date			= "0",
	.major			= 1,
	.minor			= 0,
	DRM_GEM_DMA_DRIVER_OPS,
	.release		= rp1dsi_stopall,
};

static int rp1dsi_bind(struct rp1_dsi *dsi)
{
	struct platform_device *pdev = dsi->pdev;
	struct drm_device *drm = dsi->drm;
	int ret;

	dsi->out_bridge = drmm_of_get_bridge(drm, pdev->dev.of_node, 0, 0);
	if (IS_ERR(dsi->out_bridge))
		return PTR_ERR(dsi->out_bridge);

	ret = drmm_mode_config_init(drm);
	if (ret)
		goto rtn;

	drm->mode_config.max_width  = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.fb_base    = 0;
	drm->mode_config.preferred_depth = 32;
	drm->mode_config.prefer_shadow	 = 0;
	drm->mode_config.prefer_shadow_fbdev = 1;
	drm->mode_config.quirk_addfb_prefer_host_byte_order = true;
	drm->mode_config.funcs = &rp1dsi_mode_funcs;
	drm_vblank_init(drm, 1);

	ret = drm_simple_display_pipe_init(drm,
					   &dsi->pipe,
					   &rp1dsi_pipe_funcs,
					   rp1dsi_formats,
					   ARRAY_SIZE(rp1dsi_formats),
					   NULL, NULL);
	if (ret)
		goto rtn;

	/* We need slightly more complex encoder handling (enabling/disabling
	 * video mode), so add encoder helper functions.
	 */
	drm_encoder_helper_add(&dsi->pipe.encoder, &rp1_dsi_encoder_funcs);

	ret = drm_simple_display_pipe_attach_bridge(&dsi->pipe, &dsi->bridge);
	if (ret)
		goto rtn;

	drm_bridge_add(&dsi->bridge);

	drm_mode_config_reset(drm);

	if (dsi->clocks[RP1DSI_CLOCK_CFG])
		clk_prepare_enable(dsi->clocks[RP1DSI_CLOCK_CFG]);

	ret = drm_dev_register(drm, 0);

	if (ret == 0)
		drm_fbdev_generic_setup(drm, 32);

rtn:
	if (ret)
		dev_err(&pdev->dev, "%s returned %d\n", __func__, ret);
	else
		dev_info(&pdev->dev, "%s succeeded", __func__);

	return ret;
}

static void rp1dsi_unbind(struct rp1_dsi *dsi)
{
	struct drm_device *drm = dsi->drm;

	rp1dsi_stopall(drm);
	drm_dev_unregister(drm);
	drm_atomic_helper_shutdown(drm);
}

int rp1dsi_host_attach(struct mipi_dsi_host *host, struct mipi_dsi_device *dsi_dev)
{
	struct rp1_dsi *dsi = container_of(host, struct rp1_dsi, dsi_host);

	dev_info(&dsi->pdev->dev, "%s: Attach DSI device name=%s channel=%d lanes=%d format=%d flags=0x%lx hs_rate=%lu lp_rate=%lu",
		 __func__, dsi_dev->name, dsi_dev->channel, dsi_dev->lanes,
		 dsi_dev->format, dsi_dev->mode_flags, dsi_dev->hs_rate,
		 dsi_dev->lp_rate);
	dsi->vc              = dsi_dev->channel & 3;
	dsi->lanes           = dsi_dev->lanes;

	switch (dsi_dev->format) {
	case MIPI_DSI_FMT_RGB666:
	case MIPI_DSI_FMT_RGB666_PACKED:
	case MIPI_DSI_FMT_RGB565:
	case MIPI_DSI_FMT_RGB888:
		break;
	default:
		return -EINVAL;
	}
	dsi->display_format  = dsi_dev->format;
	dsi->display_flags   = dsi_dev->mode_flags;
	dsi->display_hs_rate = dsi_dev->hs_rate;
	dsi->display_lp_rate = dsi_dev->lp_rate;

	/*
	 * Previously, we added a separate component to handle panel/bridge
	 * discovery and DRM registration, but now it's just a function call.
	 * The downstream/attaching device should deal with -EPROBE_DEFER
	 */
	return rp1dsi_bind(dsi);
}

int rp1dsi_host_detach(struct mipi_dsi_host *host, struct mipi_dsi_device *dsi_dev)
{
	struct rp1_dsi *dsi = container_of(host, struct rp1_dsi, dsi_host);

	/*
	 * Unregister the DRM driver.
	 * TODO: Check we are cleaning up correctly and not doing things multiple times!
	 */
	rp1dsi_unbind(dsi);
	return 0;
}

ssize_t rp1dsi_host_transfer(struct mipi_dsi_host *host, const struct mipi_dsi_msg *msg)
{
	struct rp1_dsi *dsi = container_of(host, struct rp1_dsi, dsi_host);
	struct mipi_dsi_packet packet;
	int ret = 0;

	/* Write */
	ret = mipi_dsi_create_packet(&packet, msg);
	if (ret) {
		dev_err(dsi->drm->dev, "RP1DSI: failed to create packet: %d\n", ret);
		return ret;
	}

	rp1dsi_dsi_send(dsi, *(u32 *)(&packet.header), packet.payload_length, packet.payload);

	/* Optional read back */
	if (msg->rx_len && msg->rx_buf)
		ret = rp1dsi_dsi_recv(dsi, msg->rx_len, msg->rx_buf);

	return (ssize_t)ret;
}

static const struct mipi_dsi_host_ops rp1dsi_mipi_dsi_host_ops = {
	.attach = rp1dsi_host_attach,
	.detach = rp1dsi_host_detach,
	.transfer = rp1dsi_host_transfer
};

static int rp1dsi_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct drm_device *drm;
	struct rp1_dsi *dsi;
	int i, ret;

	drm = drm_dev_alloc(&rp1dsi_driver, dev);
	if (IS_ERR(drm)) {
		ret = PTR_ERR(drm);
		return ret;
	}
	dsi = drmm_kzalloc(drm, sizeof(*dsi), GFP_KERNEL);
	if (!dsi) {
		ret = -ENOMEM;
		goto err_free_drm;
	}
	init_completion(&dsi->finished);
	dsi->drm = drm;
	dsi->pdev = pdev;
	drm->dev_private = dsi;
	platform_set_drvdata(pdev, drm);

	dsi->bridge.funcs = &rp1_dsi_bridge_funcs;
	dsi->bridge.of_node = dev->of_node;
	dsi->bridge.type = DRM_MODE_CONNECTOR_DSI;

	/* Safe default values for DSI mode */
	dsi->lanes = 1;
	dsi->display_format = MIPI_DSI_FMT_RGB888;
	dsi->display_flags  = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;

	/* Hardware resources */
	for (i = 0; i < RP1DSI_NUM_CLOCKS; i++) {
		static const char * const myclocknames[RP1DSI_NUM_CLOCKS] = {
			"cfgclk", "dpiclk", "byteclk", "refclk"
		};
		dsi->clocks[i] = devm_clk_get(dev, myclocknames[i]);
		if (IS_ERR(dsi->clocks[i])) {
			ret = PTR_ERR(dsi->clocks[i]);
			dev_err(dev, "Error getting clocks[%d]\n", i);
			goto err_free_drm;
		}
	}

	for (i = 0; i < RP1DSI_NUM_HW_BLOCKS; i++) {
		dsi->hw_base[i] =
			devm_ioremap_resource(dev,
					      platform_get_resource(dsi->pdev,
								    IORESOURCE_MEM,
								    i));
		if (IS_ERR(dsi->hw_base[i])) {
			ret = PTR_ERR(dsi->hw_base[i]);
			dev_err(dev, "Error memory mapping regs[%d]\n", i);
			goto err_free_drm;
		}
	}
	ret = platform_get_irq(dsi->pdev, 0);
	if (ret > 0)
		ret = devm_request_irq(dev, ret, rp1dsi_dma_isr,
				       IRQF_SHARED, "rp1-dsi", dsi);
	if (ret) {
		dev_err(dev, "Unable to request interrupt\n");
		ret = -EINVAL;
		goto err_free_drm;
	}
	rp1dsi_mipicfg_setup(dsi);
	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	/* Create the MIPI DSI Host and wait for the panel/bridge to attach to it */
	dsi->dsi_host.ops = &rp1dsi_mipi_dsi_host_ops;
	dsi->dsi_host.dev = dev;
	ret = mipi_dsi_host_register(&dsi->dsi_host);
	if (ret)
		goto err_free_drm;

	return ret;

err_free_drm:
	dev_err(dev, "%s fail %d\n", __func__, ret);
	drm_dev_put(drm);
	return ret;
}

static int rp1dsi_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct rp1_dsi *dsi = drm->dev_private;

	mipi_dsi_host_unregister(&dsi->dsi_host);
	return 0;
}

static void rp1dsi_platform_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	rp1dsi_stopall(drm);
}

static const struct of_device_id rp1dsi_of_match[] = {
	{
		.compatible = "raspberrypi,rp1dsi",
	},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, rp1dsi_of_match);

static struct platform_driver rp1dsi_platform_driver = {
	.probe		= rp1dsi_platform_probe,
	.remove		= rp1dsi_platform_remove,
	.shutdown       = rp1dsi_platform_shutdown,
	.driver		= {
		.name	= DRIVER_NAME,
		.owner  = THIS_MODULE,
		.of_match_table = rp1dsi_of_match,
	},
};

module_platform_driver(rp1dsi_platform_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MIPI DSI driver for Raspberry Pi RP1");
MODULE_AUTHOR("Nick Hollinghurst");
