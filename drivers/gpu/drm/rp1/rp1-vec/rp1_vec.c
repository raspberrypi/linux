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
#include <drm/drm_fbdev_ttm.h>
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
 * Linux doesn't make it easy to create custom video modes for the console
 * with non-CVT timings; so add a module parameter for it. The format is:
 * "<pclk>,<hact>,<hfp>,<hsync>,<hbp>,<vact>,<vfp>,<vsync>,<vbp>[,i]"
 * (where each comma may be replaced by any sequence of punctuation).
 * pclk should be 108000/n for 5 <= n <= 16 (twice this for "fake" modes).
 */

static char *rp1vec_cmode_str;
module_param_named(cmode, rp1vec_cmode_str, charp, 0600);
MODULE_PARM_DESC(cmode, "Custom video mode:\n"
		 "\t\t<pclk>,<hact>,<hfp>,<hsync>,<hbp>,<vact>,<vfp>,<vsync>,<vbp>[,i]\n");

static struct drm_display_mode *rp1vec_parse_custom_mode(struct drm_device *dev)
{
	char const *p = rp1vec_cmode_str;
	struct drm_display_mode *mode;
	unsigned int n, vals[9];

	if (!p)
		return NULL;

	for (n = 0; n < 9; n++) {
		unsigned int v = 0;

		if (!isdigit(*p))
			return NULL;
		do {
			v = 10u * v + (*p - '0');
		} while (isdigit(*++p));

		vals[n] = v;
		while (ispunct(*p))
			p++;
	}

	mode = drm_mode_create(dev);
	if (!mode)
		return NULL;

	mode->clock	  = vals[0];
	mode->hdisplay	  = vals[1];
	mode->hsync_start = mode->hdisplay + vals[2];
	mode->hsync_end	  = mode->hsync_start + vals[3];
	mode->htotal	  = mode->hsync_end + vals[4];
	mode->vdisplay	  = vals[5];
	mode->vsync_start = mode->vdisplay + vals[6];
	mode->vsync_end	  = mode->vsync_start + vals[7];
	mode->vtotal	  = mode->vsync_end + vals[8];
	mode->type  = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
	if (strchr(p, 'i'))
		mode->flags |= DRM_MODE_FLAG_INTERLACE;

	return mode;
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
	.enable_vblank	= rp1vec_pipe_enable_vblank,
	.disable_vblank = rp1vec_pipe_disable_vblank,
};

static void rp1vec_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

/*
 * Check the mode roughly matches something we can generate.
 * The choice of hardware TV mode depends on total lines and frame rate.
 * Within each hardware mode, allow pixel clock, image size and offsets
 * to vary, up to a maximum horizontal active period and line count.
 * Don't check sync timings here: the HW driver will sanitize them.
 */

static enum drm_mode_status rp1vec_mode_valid(struct drm_device *dev,
					      const struct drm_display_mode *mode)
{
	int prog	  = !(mode->flags & DRM_MODE_FLAG_INTERLACE);
	int fake_31khz	  = prog && mode->vtotal >= 500;
	int vtotal_2fld	  = mode->vtotal << (prog && !fake_31khz);
	int vdisplay_2fld = mode->vdisplay << (prog && !fake_31khz);
	int real_clock	  = mode->clock >> fake_31khz;

	/* Check pixel clock is in the permitted range */
	if (real_clock < 6750)
		return MODE_CLOCK_LOW;
	else if (real_clock > 21600)
		return MODE_CLOCK_HIGH;

	/* Try to match against the 525-line 60Hz mode (System M) */
	if (vtotal_2fld >= 524 && vtotal_2fld <= 526 && vdisplay_2fld <= 486 &&
	    mode->htotal * vtotal_2fld > 32 * real_clock &&
	    mode->htotal * vtotal_2fld < 34 * real_clock &&
	    37 * mode->hdisplay <= 2 * real_clock) /* 54us */
		return MODE_OK;

	/* All other supported TV Systems (625-, 405-, 819-line) are 50Hz */
	if (mode->htotal * vtotal_2fld > 39 * real_clock &&
	    mode->htotal * vtotal_2fld < 41 * real_clock) {
		if (vtotal_2fld >= 624 && vtotal_2fld <= 626 && vdisplay_2fld <= 576 &&
		    37 * mode->hdisplay <= 2 * real_clock) /* 54us */
			return MODE_OK;

		if (vtotal_2fld == 405 && vdisplay_2fld <= 380 &&
		    49 * mode->hdisplay <= 4 * real_clock) /* 81.6us */
			return MODE_OK;

		if (vtotal_2fld == 819 && vdisplay_2fld <= 738 &&
		    25 * mode->hdisplay <= real_clock) /* 40us */
			return MODE_OK;
	}

	return MODE_BAD;
}

static const struct drm_display_mode rp1vec_modes[6] = {
	{ /* Full size 525/60i with Rec.601 pixel rate */
		DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 13500,
			 720, 720 + 16, 720 + 16 + 64, 858, 0,
			 480, 480 + 6, 480 + 6 + 6, 525, 0,
			 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* Cropped and horizontally squashed to be TV-safe */
		DRM_MODE("704x432i", DRM_MODE_TYPE_DRIVER, 15429,
			 704, 704 + 76, 704 + 76 + 72, 980, 0,
			 432, 432 + 30, 432 + 30 + 6, 525, 0,
			 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* Full size 625/50i with Rec.601 pixel rate */
		DRM_MODE("720x576i", DRM_MODE_TYPE_DRIVER, 13500,
			 720, 720 + 12, 720 + 12 + 64, 864, 0,
			 576, 576 + 5, 576 + 5 + 5, 625, 0,
			 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* Cropped and squashed, for square(ish) pixels */
		DRM_MODE("704x512i", DRM_MODE_TYPE_DRIVER, 15429,
			 704, 704 + 72, 704 + 72 + 72, 987, 0,
			 512, 512 + 37, 512 + 37 + 5, 625, 0,
			 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* System A (405 lines) */
		DRM_MODE("544x380i", DRM_MODE_TYPE_DRIVER, 6750,
			 544, 544 + 12, 544 + 12 + 60, 667, 0,
			 380, 380 + 0,	380 + 0 + 8, 405, 0,
			 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
			 DRM_MODE_FLAG_INTERLACE)
	},
	{ /* System E (819 lines) */
		DRM_MODE("848x738i", DRM_MODE_TYPE_DRIVER, 21600,
			 848, 848 + 12, 848 + 12 + 54, 1055, 0,
			 738, 738 + 6, 738 + 6 + 1, 819, 0,
			 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC |
			 DRM_MODE_FLAG_INTERLACE)
	}
};

/*
 * Advertise a custom mode, if specified; then those from the table above.
 * From each interlaced mode above, derive a half-height progressive one.
 *
 * This driver always supports all 525-line and 625-line standard modes
 * regardless of connector's tv_mode; non-standard combinations generally
 * default to PAL[-BDGHIK] or NTSC[-M] (with a special case for "PAL60").
 *
 * The "vintage" standards (System A, System E) are advertised only when
 * the default tv_mode was DRM_MODE_TV_MODE_MONOCHROME, and only interlaced.
 */

static int rp1vec_connector_get_modes(struct drm_connector *connector)
{
	u64 tvstd;
	int i, prog, limit, n = 0, preferred_lines = 525;
	struct drm_display_mode *mode;

	if (!drm_object_property_get_default_value(&connector->base,
						   connector->dev->mode_config.tv_mode_property,
						   &tvstd))
		preferred_lines = (tvstd == DRM_MODE_TV_MODE_PAL   ||
				   tvstd == DRM_MODE_TV_MODE_PAL_N ||
				   tvstd >= DRM_MODE_TV_MODE_SECAM) ? 625 : 525;

	mode = rp1vec_parse_custom_mode(connector->dev);
	if (mode) {
		if (rp1vec_mode_valid(connector->dev, mode) == 0) {
			drm_mode_set_name(mode);
			drm_mode_probed_add(connector, mode);
			n++;
			preferred_lines = 0;
		} else {
			drm_mode_destroy(connector->dev, mode);
		}
	}

	limit = (tvstd < DRM_MODE_TV_MODE_MONOCHROME) ? 4 : ARRAY_SIZE(rp1vec_modes);
	for (i = 0; i < limit; i++) {
		for (prog = 0; prog < 2; prog++) {
			mode = drm_mode_duplicate(connector->dev, &rp1vec_modes[i]);
			if (!mode)
				return n;

			if (prog) {
				mode->flags &= ~DRM_MODE_FLAG_INTERLACE;
				mode->vdisplay	  >>= 1;
				mode->vsync_start >>= 1;
				mode->vsync_end	  >>= 1;
				mode->vtotal      >>= 1;
				if (mode->vtotal == 262 && tvstd < DRM_MODE_TV_MODE_PAL)
					mode->vtotal++;
			} else if (mode->hdisplay == 704 && mode->vtotal == preferred_lines) {
				mode->type |= DRM_MODE_TYPE_PREFERRED;
			}
			drm_mode_set_name(mode);
			drm_mode_probed_add(connector, mode);
			n++;

			if (mode->vtotal == 405 || mode->vtotal == 819)
				break; /* Don't offer progressive for Systems A, E */
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
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_ABGR8888,
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
	spin_lock_init(&vec->hw_lock);

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

	vec->drm.mode_config.min_width	= 256;
	vec->drm.mode_config.min_height = 128;
	vec->drm.mode_config.max_width	= 960; /* for "widescreen" @ 18MHz */
	vec->drm.mode_config.max_height = 738; /* for System E only */
	vec->drm.mode_config.preferred_depth = 32;
	vec->drm.mode_config.prefer_shadow = 0;
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

	drm_fbdev_ttm_setup(&vec->drm, 32);
	return ret;

done_err:
	dev_err(dev, "%s fail %d", __func__, ret);
	return ret;
}

static void rp1vec_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	rp1vec_stopall(drm);
	drm_dev_unregister(drm);
	drm_atomic_helper_shutdown(drm);
	drm_dev_put(drm);
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
