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
#include <linux/printk.h>
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

/*
 * Default TV standard parameter; it may be overridden by the OF
 * property "tv_norm" (which should be one of the strings below).
 *
 * The default (empty string) supports various 60Hz and 50Hz modes,
 * and will automatically select NTSC[-M] or PAL[-BDGHIKL]; the two
 * "fake" 60Hz standards NTSC-443 and PAL60 also support 50Hz PAL.
 * Other values will restrict the set of video modes offered.
 *
 * Finally, the DRM connector property "mode" (which is an integer)
 * can be used to override this value, but it does not prevent the
 * selection of an inapplicable video mode.
 */

static char *rp1vec_tv_norm_str;
module_param_named(tv_norm, rp1vec_tv_norm_str, charp, 0600);
MODULE_PARM_DESC(tv_norm, "Default TV norm.\n"
		 "\t\tSupported: NTSC, NTSC-J, NTSC-443, PAL, PAL-M, PAL-N,\n"
		 "\t\t\tPAL60.\n"
		 "\t\tDefault: empty string: infer PAL for a 50 Hz mode,\n"
		 "\t\t\tNTSC otherwise");

const char * const rp1vec_tvstd_names[] = {
	[RP1VEC_TVSTD_NTSC]     = "NTSC",
	[RP1VEC_TVSTD_NTSC_J]   = "NTSC-J",
	[RP1VEC_TVSTD_NTSC_443] = "NTSC-443",
	[RP1VEC_TVSTD_PAL]      = "PAL",
	[RP1VEC_TVSTD_PAL_M]    = "PAL-M",
	[RP1VEC_TVSTD_PAL_N]    = "PAL-N",
	[RP1VEC_TVSTD_PAL60]    = "PAL60",
	[RP1VEC_TVSTD_DEFAULT]  = "",
};

static int rp1vec_parse_tv_norm(const char *str)
{
	int i;

	if (str && *str) {
		for (i = 0; i < ARRAY_SIZE(rp1vec_tvstd_names); ++i) {
			if (strcasecmp(str, rp1vec_tvstd_names[i]) == 0)
				return i;
		}
	}
	return RP1VEC_TVSTD_DEFAULT;
}

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
	.prepare_fb = drm_gem_simple_display_pipe_prepare_fb,
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
			 720, 720 + 14, 720 + 14 + 64, 858, 0,
			 480, 480 + 7, 480 + 7 + 6, 525, 0,
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* Cropped and horizontally squashed to be TV-safe */
		DRM_MODE("704x432i", DRM_MODE_TYPE_DRIVER, 15429,
			 704, 704 + 72, 704 + 72 + 72, 980, 0,
			 432, 432 + 31, 432 + 31 + 6, 525, 0,
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* Full size 625/50i with Rec.601 pixel rate */
		DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 13500,
			 720, 720 + 20, 720 + 20 + 64, 864, 0,
			 576, 576 + 4, 576 + 4 + 6, 625, 0,
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* Cropped and squashed, for square(ish) pixels */
		DRM_MODE("704x512i", DRM_MODE_TYPE_DRIVER, 15429,
			 704, 704 + 80, 704 + 80 + 72, 987, 0,
			 512, 512 + 36, 512 + 36 + 6, 625, 0,
			 DRM_MODE_FLAG_INTERLACE)
	}
};

static int rp1vec_connector_get_modes(struct drm_connector *connector)
{
	struct rp1_vec *vec = container_of(connector, struct rp1_vec, connector);
	bool ok525 = RP1VEC_TVSTD_SUPPORT_525(vec->tv_norm);
	bool ok625 = RP1VEC_TVSTD_SUPPORT_625(vec->tv_norm);
	int i, prog, n = 0;

	for (i = 0; i < ARRAY_SIZE(rp1vec_modes); i++) {
		if ((rp1vec_modes[i].vtotal == 625) ? ok625 : ok525) {
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
				    mode->vtotal == ((ok525) ? 525 : 625))
					mode->type |= DRM_MODE_TYPE_PREFERRED;

				drm_mode_set_name(mode);
				drm_mode_probed_add(connector, mode);
				n++;
			}
		}
	}

	return n;
}

static void rp1vec_connector_reset(struct drm_connector *connector)
{
	struct rp1_vec *vec = container_of(connector, struct rp1_vec, connector);

	drm_atomic_helper_connector_reset(connector);
	if (connector->state)
		connector->state->tv.mode = vec->tv_norm;
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
	 * Check the mode roughly matches one of our standard modes
	 * (optionally half-height and progressive). Ignore H/V sync
	 * timings which for interlaced TV are approximate at best.
	 */
	int i, prog;

	prog = !(mode->flags & DRM_MODE_FLAG_INTERLACE);

	for (i = 0; i < ARRAY_SIZE(rp1vec_modes); i++) {
		const struct drm_display_mode *ref = rp1vec_modes + i;

		if (mode->hdisplay == ref->hdisplay           &&
		    mode->vdisplay == (ref->vdisplay >> prog) &&
		    mode->clock + 2 >= ref->clock             &&
		    mode->clock <= ref->clock + 2             &&
		    mode->htotal + 2 >= ref->htotal           &&
		    mode->htotal <= ref->htotal + 2           &&
		    mode->vtotal + 2 >= (ref->vtotal >> prog) &&
		    mode->vtotal <= (ref->vtotal >> prog) + 2)
			return MODE_OK;
	}
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
	struct drm_device *drm;
	struct rp1_vec *vec;
	const char *str;
	int i, ret;

	dev_info(dev, __func__);
	drm = drm_dev_alloc(&rp1vec_driver, dev);
	if (IS_ERR(drm)) {
		ret = PTR_ERR(drm);
		dev_err(dev, "%s drm_dev_alloc %d", __func__, ret);
		return ret;
	}

	vec = drmm_kzalloc(drm, sizeof(*vec), GFP_KERNEL);
	if (!vec) {
		dev_err(dev, "%s drmm_kzalloc failed", __func__);
		ret = -ENOMEM;
		goto err_free_drm;
	}
	init_completion(&vec->finished);
	vec->drm = drm;
	vec->pdev = pdev;
	drm->dev_private = vec;
	platform_set_drvdata(pdev, drm);

	str = rp1vec_tv_norm_str;
	of_property_read_string(dev->of_node, "tv_norm", &str);
	vec->tv_norm = rp1vec_parse_tv_norm(str);

	for (i = 0; i < RP1VEC_NUM_HW_BLOCKS; i++) {
		vec->hw_base[i] =
			devm_ioremap_resource(dev,
					      platform_get_resource(vec->pdev, IORESOURCE_MEM, i));
		if (IS_ERR(vec->hw_base[i])) {
			ret = PTR_ERR(vec->hw_base[i]);
			dev_err(dev, "Error memory mapping regs[%d]\n", i);
			goto err_free_drm;
		}
	}
	ret = platform_get_irq(vec->pdev, 0);
	if (ret > 0)
		ret = devm_request_irq(dev, ret, rp1vec_hw_isr,
				       IRQF_SHARED, "rp1-vec", vec);
	if (ret) {
		dev_err(dev, "Unable to request interrupt\n");
		ret = -EINVAL;
		goto err_free_drm;
	}
	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	vec->vec_clock = devm_clk_get(dev, NULL);
	if (IS_ERR(vec->vec_clock)) {
		ret = PTR_ERR(vec->vec_clock);
		goto err_free_drm;
	}
	ret = clk_prepare_enable(vec->vec_clock);

	ret = drmm_mode_config_init(drm);
	if (ret)
		goto err_free_drm;
	drm->mode_config.max_width  = 768;
	drm->mode_config.max_height = 576;
	drm->mode_config.fb_base    = 0;
	drm->mode_config.preferred_depth = 32;
	drm->mode_config.prefer_shadow	 = 0;
	drm->mode_config.prefer_shadow_fbdev = 1;
	//drm->mode_config.fbdev_use_iomem = false;
	drm->mode_config.quirk_addfb_prefer_host_byte_order = true;
	drm->mode_config.funcs = &rp1vec_mode_funcs;
	drm_vblank_init(drm, 1);

	ret = drm_mode_create_tv_properties(drm, ARRAY_SIZE(rp1vec_tvstd_names),
					    rp1vec_tvstd_names);
	if (ret)
		goto err_free_drm;

	drm_connector_init(drm, &vec->connector, &rp1vec_connector_funcs,
			   DRM_MODE_CONNECTOR_Composite);
	if (ret)
		goto err_free_drm;

	vec->connector.interlace_allowed = true;
	drm_connector_helper_add(&vec->connector, &rp1vec_connector_helper_funcs);

	drm_object_attach_property(&vec->connector.base,
				   drm->mode_config.tv_mode_property,
				   vec->tv_norm);

	ret = drm_simple_display_pipe_init(drm,
					   &vec->pipe,
					   &rp1vec_pipe_funcs,
					   rp1vec_formats,
					   ARRAY_SIZE(rp1vec_formats),
					   NULL,
					   &vec->connector);
	if (ret)
		goto err_free_drm;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_free_drm;

	drm_fbdev_generic_setup(drm, 32); /* the "32" is preferred BPP */
	return ret;

err_free_drm:
	dev_info(dev, "%s fail %d", __func__, ret);
	drm_dev_put(drm);
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
