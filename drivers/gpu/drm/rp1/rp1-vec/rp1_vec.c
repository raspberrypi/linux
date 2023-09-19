// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM Driver for VEC output on Raspberry Pi RP1
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
#include <linux/console.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/cred.h>
#include <drm/drm_drv.h>
#include <drm/drm_mm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_vblank.h>
#include <drm/drm_of.h>

#include "rp1_vec.h"

static void rp1vec_pipe_update(struct drm_simple_display_pipe *pipe,
			       struct drm_plane_state *old_state)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;
	struct drm_framebuffer *fb = pipe->plane.state->fb;
	struct rp1_vec *vec = pipe->crtc.dev->dev_private;
	struct drm_gem_object *gem = fb ? drm_gem_fb_get_obj(fb, 0) : NULL;
	struct drm_gem_dma_object *dma_obj = gem ? to_drm_gem_dma_obj(gem) : NULL;
	bool can_update = fb && dma_obj && vec && vec->pipe_enabled;

	/* (Re-)start VEC where required; and update FB address */
	if (can_update) {
		if (!vec->vec_running || fb->format->format != vec->cur_fmt) {
			if (vec->vec_running && fb->format->format != vec->cur_fmt) {
				rp1vec_hw_stop(vec);
				vec->vec_running = false;
			}
			if (!vec->vec_running) {
				rp1vec_hw_setup(vec,
						fb->format->format,
						&pipe->crtc.state->mode,
						vec->connector.state->tv.mode);
				vec->vec_running = true;
			}
			vec->cur_fmt  = fb->format->format;
			drm_crtc_vblank_on(&pipe->crtc);
		}
		rp1vec_hw_update(vec, dma_obj->dma_addr, fb->offsets[0], fb->pitches[0]);
	}

	/* Check if VBLANK callback needs to be armed (or sent immediately in some error cases).
	 * Note there is a tiny probability of a race between rp1vec_dma_update() and IRQ;
	 * ordering it this way around is safe, but theoretically might delay an extra frame.
	 */
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

static void rp1vec_pipe_enable(struct drm_simple_display_pipe *pipe,
			       struct drm_crtc_state *crtc_state,
			       struct drm_plane_state *plane_state)
{
	struct rp1_vec *vec = pipe->crtc.dev->dev_private;

	dev_info(&vec->pdev->dev, __func__);
	vec->pipe_enabled = true;
	vec->cur_fmt = 0xdeadbeef;
	rp1vec_vidout_setup(vec);
	rp1vec_pipe_update(pipe, 0);
}

static void rp1vec_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct rp1_vec *vec = pipe->crtc.dev->dev_private;

	dev_info(&vec->pdev->dev, __func__);
	drm_crtc_vblank_off(&pipe->crtc);
	if (vec) {
		if (vec->vec_running) {
			rp1vec_hw_stop(vec);
			vec->vec_running = false;
		}
		vec->pipe_enabled = false;
	}
}

static int rp1vec_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	if (pipe && pipe->crtc.dev) {
		struct rp1_vec *vec = pipe->crtc.dev->dev_private;

		if (vec)
			rp1vec_hw_vblank_ctrl(vec, 1);
	}
	return 0;
}

static void rp1vec_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	if (pipe && pipe->crtc.dev) {
		struct rp1_vec *vec = pipe->crtc.dev->dev_private;

		if (vec)
			rp1vec_hw_vblank_ctrl(vec, 0);
	}
}

static const struct drm_simple_display_pipe_funcs rp1vec_pipe_funcs = {
	.enable	    = rp1vec_pipe_enable,
	.update	    = rp1vec_pipe_update,
	.disable    = rp1vec_pipe_disable,
	.enable_vblank	= rp1vec_pipe_enable_vblank,
	.disable_vblank = rp1vec_pipe_disable_vblank,
};

static void rp1vec_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_display_mode rp1vec_modes[4] = {
	{ /* Full size 525/60i with Rec.601 pixel rate */
		DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 13500,
			 720, 720 + 16, 720 + 16 + 64, 858, 0,
			 480, 480 + 6, 480 + 6 + 6, 525, 0,
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* Cropped and horizontally squashed to be TV-safe */
		DRM_MODE("704x432i", DRM_MODE_TYPE_DRIVER, 15429,
			 704, 704 + 76, 704 + 76 + 72, 980, 0,
			 432, 432 + 30, 432 + 30 + 6, 525, 0,
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* Full size 625/50i with Rec.601 pixel rate */
		DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 13500,
			 720, 720 + 12, 720 + 12 + 64, 864, 0,
			 576, 576 + 5, 576 + 5 + 5, 625, 0,
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* Cropped and squashed, for square(ish) pixels */
		DRM_MODE("704x512i", DRM_MODE_TYPE_DRIVER, 15429,
			 704, 704 + 72, 704 + 72 + 72, 987, 0,
			 512, 512 + 37, 512 + 37 + 5, 625, 0,
			 DRM_MODE_FLAG_INTERLACE)
	}
};

/*
 * Advertise standard and preferred video modes.
 *
 * From each interlaced mode in the table above, derive a progressive one.
 *
 * This driver always supports all 50Hz and 60Hz video modes, regardless
 * of connector's tv_mode; nonstandard combinations generally default
 * to PAL[-BDGHIKL] or NTSC[-M] depending on resolution and field-rate
 * (except that "PAL" with 525/60 will be implemented as "PAL60").
 * However, the preferred mode will depend on the default TV mode.
 */

static int rp1vec_connector_get_modes(struct drm_connector *connector)
{
	u64 val;
	int i, prog, n = 0;
	bool prefer625 = false;

	if (!drm_object_property_get_default_value(&connector->base,
						   connector->dev->mode_config.tv_mode_property,
						   &val))
		prefer625 = (val == DRM_MODE_TV_MODE_PAL   ||
			     val == DRM_MODE_TV_MODE_PAL_N ||
			     val == DRM_MODE_TV_MODE_SECAM);

	for (i = 0; i < ARRAY_SIZE(rp1vec_modes); i++) {
		for (prog = 0; prog < 2; prog++) {
			struct drm_display_mode *mode =
				drm_mode_duplicate(connector->dev,
						   &rp1vec_modes[i]);

			if (prog) {
				mode->flags &= ~DRM_MODE_FLAG_INTERLACE;
				mode->vdisplay	  >>= 1;
				mode->vsync_start >>= 1;
				mode->vsync_end	  >>= 1;
				mode->vtotal	  >>= 1;
			}

			if (mode->hdisplay == 704 &&
			    mode->vtotal == (prefer625 ? 625 : 525))
				mode->type |= DRM_MODE_TYPE_PREFERRED;

			drm_mode_set_name(mode);
			drm_mode_probed_add(connector, mode);
			n++;
		}
	}

	return n;
}

static void rp1vec_connector_reset(struct drm_connector *connector)
{
	drm_atomic_helper_connector_reset(connector);
	drm_atomic_helper_connector_tv_reset(connector);
}

static int rp1vec_connector_atomic_check(struct drm_connector *conn,
					 struct drm_atomic_state *state)
{	struct drm_connector_state *old_state =
		drm_atomic_get_old_connector_state(state, conn);
	struct drm_connector_state *new_state =
		drm_atomic_get_new_connector_state(state, conn);

	if (new_state->crtc && old_state->tv.mode != new_state->tv.mode) {
		struct drm_crtc_state *crtc_state =
			drm_atomic_get_new_crtc_state(state, new_state->crtc);

		crtc_state->mode_changed = true;
	}

	return 0;
}

static enum drm_mode_status rp1vec_mode_valid(struct drm_device *dev,
					      const struct drm_display_mode *mode)
{
	/*
	 * Check the mode roughly matches something we can generate.
	 * The hardware driver is very prescriptive about pixel clocks,
	 * line and frame durations, but we'll tolerate rounding errors.
	 * Within each hardware mode, allow image size and position to vary
	 * (to fine-tune overscan correction or emulate retro devices).
	 * Don't check sync timings here: the HW driver will sanitize them.
	 */

	int prog = !(mode->flags & DRM_MODE_FLAG_INTERLACE);
	int vtotal_full = mode->vtotal << prog;
	int vdisplay_full = mode->vdisplay << prog;

	/* Reject very small frames */
	if (vtotal_full < 256 || mode->hdisplay < 256)
		return MODE_BAD;

	/* Check lines, frame period (ms) and vertical size limit */
	if (vtotal_full >= 524 && vtotal_full <= 526 &&
	    mode->htotal * vtotal_full > 33 * mode->clock &&
	    mode->htotal * vtotal_full < 34 * mode->clock &&
	    vdisplay_full <= 480)
		goto vgood;
	if (vtotal_full >= 624 && vtotal_full <= 626 &&
	    mode->htotal * vtotal_full > 39 * mode->clock &&
	    mode->htotal * vtotal_full < 41 * mode->clock &&
	    vdisplay_full <= 576)
		goto vgood;
	return MODE_BAD;

vgood:
	/* Check pixel rate (kHz) and horizontal size limit */
	if (mode->clock == 13500 && mode->hdisplay <= 720)
		return MODE_OK;
	if (mode->clock >= 15428 && mode->clock <= 15429 &&
	    mode->hdisplay <= 800)
		return MODE_OK;
	return MODE_BAD;
}

static const struct drm_connector_helper_funcs rp1vec_connector_helper_funcs = {
	.get_modes = rp1vec_connector_get_modes,
	.atomic_check = rp1vec_connector_atomic_check,
};

static const struct drm_connector_funcs rp1vec_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = rp1vec_connector_destroy,
	.reset = rp1vec_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs rp1vec_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
	.mode_valid = rp1vec_mode_valid,
};

static const u32 rp1vec_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565
};

static void rp1vec_stopall(struct drm_device *drm)
{
	if (drm->dev_private) {
		struct rp1_vec *vec = drm->dev_private;

		if (vec->vec_running || rp1vec_hw_busy(vec)) {
			rp1vec_hw_stop(vec);
			vec->vec_running = false;
		}
		rp1vec_vidout_poweroff(vec);
	}
}

DEFINE_DRM_GEM_DMA_FOPS(rp1vec_fops);

static struct drm_driver rp1vec_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &rp1vec_fops,
	.name			= "drm-rp1-vec",
	.desc			= "drm-rp1-vec",
	.date			= "0",
	.major			= 1,
	.minor			= 0,
	DRM_GEM_DMA_DRIVER_OPS,
	.release		= rp1vec_stopall,
};

static int rp1vec_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rp1_vec *vec;
	int i, ret;

	dev_info(dev, __func__);
	vec = devm_drm_dev_alloc(dev, &rp1vec_driver, struct rp1_vec, drm);
	if (IS_ERR(vec)) {
		ret = PTR_ERR(vec);
		dev_err(dev, "%s devm_drm_dev_alloc %d", __func__, ret);
		return ret;
	}
	vec->pdev = pdev;

	for (i = 0; i < RP1VEC_NUM_HW_BLOCKS; i++) {
		vec->hw_base[i] =
			devm_ioremap_resource(dev,
					      platform_get_resource(vec->pdev, IORESOURCE_MEM, i));
		if (IS_ERR(vec->hw_base[i])) {
			ret = PTR_ERR(vec->hw_base[i]);
			dev_err(dev, "Error memory mapping regs[%d]\n", i);
			goto done_err;
		}
	}
	ret = platform_get_irq(vec->pdev, 0);
	if (ret > 0)
		ret = devm_request_irq(dev, ret, rp1vec_hw_isr,
				       IRQF_SHARED, "rp1-vec", vec);
	if (ret) {
		dev_err(dev, "Unable to request interrupt\n");
		ret = -EINVAL;
		goto done_err;
	}

	vec->vec_clock = devm_clk_get(dev, NULL);
	if (IS_ERR(vec->vec_clock)) {
		ret = PTR_ERR(vec->vec_clock);
		goto done_err;
	}
	ret = clk_prepare_enable(vec->vec_clock);

	ret = drmm_mode_config_init(&vec->drm);
	if (ret)
		goto done_err;

	/* Now we have all our resources, finish driver initialization */
	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	init_completion(&vec->finished);
	vec->drm.dev_private = vec;
	platform_set_drvdata(pdev, &vec->drm);

	vec->drm.mode_config.max_width  = 800;
	vec->drm.mode_config.max_height = 576;
	vec->drm.mode_config.preferred_depth = 32;
	vec->drm.mode_config.prefer_shadow   = 0;
	vec->drm.mode_config.quirk_addfb_prefer_host_byte_order = true;
	vec->drm.mode_config.funcs = &rp1vec_mode_funcs;
	drm_vblank_init(&vec->drm, 1);

	ret = drm_mode_create_tv_properties(&vec->drm, RP1VEC_SUPPORTED_TV_MODES);
	if (ret)
		goto done_err;

	drm_connector_init(&vec->drm, &vec->connector, &rp1vec_connector_funcs,
			   DRM_MODE_CONNECTOR_Composite);
	if (ret)
		goto done_err;

	vec->connector.interlace_allowed = true;
	drm_connector_helper_add(&vec->connector, &rp1vec_connector_helper_funcs);

	drm_object_attach_property(&vec->connector.base,
				   vec->drm.mode_config.tv_mode_property,
				   (vec->connector.cmdline_mode.tv_mode_specified) ?
					   vec->connector.cmdline_mode.tv_mode :
					   DRM_MODE_TV_MODE_NTSC);

	ret = drm_simple_display_pipe_init(&vec->drm,
					   &vec->pipe,
					   &rp1vec_pipe_funcs,
					   rp1vec_formats,
					   ARRAY_SIZE(rp1vec_formats),
					   NULL,
					   &vec->connector);
	if (ret)
		goto done_err;

	drm_mode_config_reset(&vec->drm);

	ret = drm_dev_register(&vec->drm, 0);
	if (ret)
		goto done_err;

	drm_fbdev_generic_setup(&vec->drm, 32);
	return ret;

done_err:
	dev_err(dev, "%s fail %d", __func__, ret);
	return ret;
}

static int rp1vec_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	rp1vec_stopall(drm);
	drm_dev_unregister(drm);
	drm_atomic_helper_shutdown(drm);
	drm_dev_put(drm);

	return 0;
}

static void rp1vec_platform_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	rp1vec_stopall(drm);
}

static const struct of_device_id rp1vec_of_match[] = {
	{
		.compatible = "raspberrypi,rp1vec",
	},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, rp1vec_of_match);

static struct platform_driver rp1vec_platform_driver = {
	.probe		= rp1vec_platform_probe,
	.remove		= rp1vec_platform_remove,
	.shutdown	= rp1vec_platform_shutdown,
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = rp1vec_of_match,
	},
};

module_platform_driver(rp1vec_platform_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DRM driver for Composite Video on Raspberry Pi RP1");
MODULE_AUTHOR("Nick Hollinghurst");
