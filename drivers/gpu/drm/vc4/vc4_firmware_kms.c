/*
 * Copyright (C) 2016 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/**
 * DOC: VC4 firmware KMS module.
 *
 * As a hack to get us from the current closed source driver world
 * toward a totally open stack, implement KMS on top of the Raspberry
 * Pi's firmware display stack.
 */

#include "drm/drm_atomic_helper.h"
#include "drm/drm_plane_helper.h"
#include "drm/drm_crtc_helper.h"
#include "drm/drm_fourcc.h"
#include "linux/clk.h"
#include "linux/debugfs.h"
#include "drm/drm_fb_cma_helper.h"
#include "linux/component.h"
#include "linux/of_device.h"
#include "vc4_drv.h"
#include "vc4_regs.h"
#include <soc/bcm2835/raspberrypi-firmware.h>

/* The firmware delivers a vblank interrupt to us through the SMI
 * hardware, which has only this one register.
 */
#define SMICS 0x0
#define SMICS_INTERRUPTS (BIT(9) | BIT(10) | BIT(11))

struct vc4_crtc {
	struct drm_crtc base;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	struct drm_plane *primary;
	struct drm_plane *cursor;
	void __iomem *regs;

	struct drm_pending_vblank_event *event;
};

static inline struct vc4_crtc *to_vc4_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct vc4_crtc, base);
}

struct vc4_fkms_encoder {
	struct drm_encoder base;
};

static inline struct vc4_fkms_encoder *
to_vc4_fkms_encoder(struct drm_encoder *encoder)
{
	return container_of(encoder, struct vc4_fkms_encoder, base);
}

/* VC4 FKMS connector KMS struct */
struct vc4_fkms_connector {
	struct drm_connector base;

	/* Since the connector is attached to just the one encoder,
	 * this is the reference to it so we can do the best_encoder()
	 * hook.
	 */
	struct drm_encoder *encoder;
};

static inline struct vc4_fkms_connector *
to_vc4_fkms_connector(struct drm_connector *connector)
{
	return container_of(connector, struct vc4_fkms_connector, base);
}

/* Firmware's structure for making an FB mbox call. */
struct fbinfo_s {
	u32 xres, yres, xres_virtual, yres_virtual;
	u32 pitch, bpp;
	u32 xoffset, yoffset;
	u32 base;
	u32 screen_size;
	u16 cmap[256];
};

struct vc4_fkms_plane {
	struct drm_plane base;
	struct fbinfo_s *fbinfo;
	dma_addr_t fbinfo_bus_addr;
	u32 pitch;
};

static inline struct vc4_fkms_plane *to_vc4_fkms_plane(struct drm_plane *plane)
{
	return (struct vc4_fkms_plane *)plane;
}

/* Turns the display on/off. */
static int vc4_plane_set_primary_blank(struct drm_plane *plane, bool blank)
{
	struct vc4_dev *vc4 = to_vc4_dev(plane->dev);

	u32 packet = blank;

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] primary plane %s",
			 plane->base.id, plane->name,
			 blank ? "blank" : "unblank");

	return rpi_firmware_property(vc4->firmware,
				     RPI_FIRMWARE_FRAMEBUFFER_BLANK,
				     &packet, sizeof(packet));
}

static void vc4_primary_plane_atomic_update(struct drm_plane *plane,
					    struct drm_plane_state *old_state)
{
	struct vc4_dev *vc4 = to_vc4_dev(plane->dev);
	struct vc4_fkms_plane *vc4_plane = to_vc4_fkms_plane(plane);
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_cma_object *bo = drm_fb_cma_get_gem_obj(fb, 0);
	volatile struct fbinfo_s *fbinfo = vc4_plane->fbinfo;
	u32 bpp = 32;
	int ret;

	fbinfo->xres = state->crtc_w;
	fbinfo->yres = state->crtc_h;
	fbinfo->xres_virtual = state->crtc_w;
	fbinfo->yres_virtual = state->crtc_h;
	fbinfo->bpp = bpp;
	fbinfo->xoffset = state->crtc_x;
	fbinfo->yoffset = state->crtc_y;
	fbinfo->base = bo->paddr + fb->offsets[0];
	fbinfo->pitch = fb->pitches[0];

	if (fb->modifier == DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED)
		fbinfo->bpp |= BIT(31);

	/* A bug in the firmware makes it so that if the fb->base is
	 * set to nonzero, the configured pitch gets overwritten with
	 * the previous pitch.  So, to get the configured pitch
	 * recomputed, we have to make it allocate itself a new buffer
	 * in VC memory, first.
	 */
	if (vc4_plane->pitch != fb->pitches[0]) {
		u32 saved_base = fbinfo->base;
		fbinfo->base = 0;

		ret = rpi_firmware_transaction(vc4->firmware,
					       RPI_FIRMWARE_CHAN_FB,
					       vc4_plane->fbinfo_bus_addr);
		fbinfo->base = saved_base;

		vc4_plane->pitch = fbinfo->pitch;
		WARN_ON_ONCE(vc4_plane->pitch != fb->pitches[0]);
	}

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] primary update %dx%d@%d +%d,%d 0x%08x/%d\n",
			 plane->base.id, plane->name,
			 state->crtc_w,
			 state->crtc_h,
			 bpp,
			 state->crtc_x,
			 state->crtc_y,
			 bo->paddr + fb->offsets[0],
			 fb->pitches[0]);

	ret = rpi_firmware_transaction(vc4->firmware,
				       RPI_FIRMWARE_CHAN_FB,
				       vc4_plane->fbinfo_bus_addr);
	WARN_ON_ONCE(fbinfo->pitch != fb->pitches[0]);
	WARN_ON_ONCE(fbinfo->base != bo->paddr + fb->offsets[0]);

	/* If the CRTC is on (or going to be on) and we're enabled,
	 * then unblank.  Otherwise, stay blank until CRTC enable.
	*/
	if (state->crtc->state->active)
		vc4_plane_set_primary_blank(plane, false);
}

static void vc4_primary_plane_atomic_disable(struct drm_plane *plane,
					     struct drm_plane_state *old_state)
{
	vc4_plane_set_primary_blank(plane, true);
}

static void vc4_cursor_plane_atomic_update(struct drm_plane *plane,
					   struct drm_plane_state *old_state)
{
	struct vc4_dev *vc4 = to_vc4_dev(plane->dev);
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_cma_object *bo = drm_fb_cma_get_gem_obj(fb, 0);
	int ret;
	u32 packet_state[] = {
		state->crtc->state->active,
		state->crtc_x,
		state->crtc_y,
		0
	};
	u32 packet_info[] = { state->crtc_w, state->crtc_h,
			      0, /* unused */
			      bo->paddr + fb->offsets[0],
			      0, 0, /* hotx, hoty */};
	WARN_ON_ONCE(fb->pitches[0] != state->crtc_w * 4);

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] update %dx%d cursor at %d,%d (0x%08x/%d)",
			 plane->base.id, plane->name,
			 state->crtc_w,
			 state->crtc_h,
			 state->crtc_x,
			 state->crtc_y,
			 bo->paddr + fb->offsets[0],
			 fb->pitches[0]);

	ret = rpi_firmware_property(vc4->firmware,
				    RPI_FIRMWARE_SET_CURSOR_STATE,
				    &packet_state,
				    sizeof(packet_state));
	if (ret || packet_state[0] != 0)
		DRM_ERROR("Failed to set cursor state: 0x%08x\n", packet_state[0]);

	ret = rpi_firmware_property(vc4->firmware,
				    RPI_FIRMWARE_SET_CURSOR_INFO,
				    &packet_info,
				    sizeof(packet_info));
	if (ret || packet_info[0] != 0)
		DRM_ERROR("Failed to set cursor info: 0x%08x\n", packet_info[0]);
}

static void vc4_cursor_plane_atomic_disable(struct drm_plane *plane,
					    struct drm_plane_state *old_state)
{
	struct vc4_dev *vc4 = to_vc4_dev(plane->dev);
	u32 packet_state[] = { false, 0, 0, 0 };
	int ret;

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] disabling cursor", plane->base.id, plane->name);

	ret = rpi_firmware_property(vc4->firmware,
				    RPI_FIRMWARE_SET_CURSOR_STATE,
				    &packet_state,
				    sizeof(packet_state));
	if (ret || packet_state[0] != 0)
		DRM_ERROR("Failed to set cursor state: 0x%08x\n", packet_state[0]);
}

static int vc4_plane_atomic_check(struct drm_plane *plane,
				  struct drm_plane_state *state)
{
	return 0;
}

static void vc4_plane_destroy(struct drm_plane *plane)
{
	drm_plane_helper_disable(plane);
	drm_plane_cleanup(plane);
}

static const struct drm_plane_funcs vc4_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = vc4_plane_destroy,
	.set_property = NULL,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const struct drm_plane_helper_funcs vc4_primary_plane_helper_funcs = {
	.prepare_fb = NULL,
	.cleanup_fb = NULL,
	.atomic_check = vc4_plane_atomic_check,
	.atomic_update = vc4_primary_plane_atomic_update,
	.atomic_disable = vc4_primary_plane_atomic_disable,
};

static const struct drm_plane_helper_funcs vc4_cursor_plane_helper_funcs = {
	.prepare_fb = NULL,
	.cleanup_fb = NULL,
	.atomic_check = vc4_plane_atomic_check,
	.atomic_update = vc4_cursor_plane_atomic_update,
	.atomic_disable = vc4_cursor_plane_atomic_disable,
};

static struct drm_plane *vc4_fkms_plane_init(struct drm_device *dev,
					     enum drm_plane_type type)
{
	struct drm_plane *plane = NULL;
	struct vc4_fkms_plane *vc4_plane;
	u32 xrgb8888 = DRM_FORMAT_XRGB8888;
	u32 argb8888 = DRM_FORMAT_ARGB8888;
	int ret = 0;
	bool primary = (type == DRM_PLANE_TYPE_PRIMARY);

	vc4_plane = devm_kzalloc(dev->dev, sizeof(*vc4_plane),
				 GFP_KERNEL);
	if (!vc4_plane) {
		ret = -ENOMEM;
		goto fail;
	}

	plane = &vc4_plane->base;
	ret = drm_universal_plane_init(dev, plane, 0xff,
				       &vc4_plane_funcs,
				       primary ? &xrgb8888 : &argb8888, 1, NULL,
				       type, primary ? "primary" : "cursor");

	if (type == DRM_PLANE_TYPE_PRIMARY) {
		vc4_plane->fbinfo =
			dma_alloc_coherent(dev->dev,
					   sizeof(*vc4_plane->fbinfo),
					   &vc4_plane->fbinfo_bus_addr,
					   GFP_KERNEL);
		memset(vc4_plane->fbinfo, 0, sizeof(*vc4_plane->fbinfo));

		drm_plane_helper_add(plane, &vc4_primary_plane_helper_funcs);
	} else {
		drm_plane_helper_add(plane, &vc4_cursor_plane_helper_funcs);
	}

	return plane;
fail:
	if (plane)
		vc4_plane_destroy(plane);

	return ERR_PTR(ret);
}

static void vc4_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	/* Everyting is handled in the planes. */
}

static void vc4_crtc_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);

	/* Always turn the planes off on CRTC disable. In DRM, planes
	 * are enabled/disabled through the update/disable hooks
	 * above, and the CRTC enable/disable independently controls
	 * whether anything scans out at all, but the firmware doesn't
	 * give us a CRTC-level control for that.
	 */
	vc4_cursor_plane_atomic_disable(vc4_crtc->cursor,
					vc4_crtc->cursor->state);
	vc4_plane_set_primary_blank(vc4_crtc->primary, true);
}

static void vc4_crtc_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);

	/* Unblank the planes (if they're supposed to be displayed). */
	if (vc4_crtc->primary->state->fb)
		vc4_plane_set_primary_blank(vc4_crtc->primary, false);
	if (vc4_crtc->cursor->state->fb) {
		vc4_cursor_plane_atomic_update(vc4_crtc->cursor,
					       vc4_crtc->cursor->state);
	}
}

static int vc4_crtc_atomic_check(struct drm_crtc *crtc,
				 struct drm_crtc_state *state)
{
	return 0;
}

static void vc4_crtc_atomic_flush(struct drm_crtc *crtc,
				  struct drm_crtc_state *old_state)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_device *dev = crtc->dev;

	if (crtc->state->event) {
		unsigned long flags;

		crtc->state->event->pipe = drm_crtc_index(crtc);

		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		spin_lock_irqsave(&dev->event_lock, flags);
		vc4_crtc->event = crtc->state->event;
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
}

static void vc4_crtc_handle_page_flip(struct vc4_crtc *vc4_crtc)
{
	struct drm_crtc *crtc = &vc4_crtc->base;
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	if (vc4_crtc->event) {
		drm_crtc_send_vblank_event(crtc, vc4_crtc->event);
		vc4_crtc->event = NULL;
		drm_crtc_vblank_put(crtc);
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static irqreturn_t vc4_crtc_irq_handler(int irq, void *data)
{
	struct vc4_crtc *vc4_crtc = data;
	u32 stat = readl(vc4_crtc->regs + SMICS);
	irqreturn_t ret = IRQ_NONE;

	if (stat & SMICS_INTERRUPTS) {
		writel(0, vc4_crtc->regs + SMICS);
		drm_crtc_handle_vblank(&vc4_crtc->base);
		vc4_crtc_handle_page_flip(vc4_crtc);
		ret = IRQ_HANDLED;
	}

	return ret;
}

static int vc4_page_flip(struct drm_crtc *crtc,
			 struct drm_framebuffer *fb,
			 struct drm_pending_vblank_event *event,
			 uint32_t flags, struct drm_modeset_acquire_ctx *ctx)
{
	if (flags & DRM_MODE_PAGE_FLIP_ASYNC) {
		DRM_ERROR("Async flips aren't allowed\n");
		return -EINVAL;
	}

	return drm_atomic_helper_page_flip(crtc, fb, event, flags, ctx);
}

static const struct drm_crtc_funcs vc4_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.destroy = drm_crtc_cleanup,
	.page_flip = vc4_page_flip,
	.set_property = NULL,
	.cursor_set = NULL, /* handled by drm_mode_cursor_universal */
	.cursor_move = NULL, /* handled by drm_mode_cursor_universal */
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs vc4_crtc_helper_funcs = {
	.mode_set_nofb = vc4_crtc_mode_set_nofb,
	.atomic_disable = vc4_crtc_disable,
	.atomic_enable = vc4_crtc_enable,
	.atomic_check = vc4_crtc_atomic_check,
	.atomic_flush = vc4_crtc_atomic_flush,
};

/* Frees the page flip event when the DRM device is closed with the
 * event still outstanding.
 */
void vc4_fkms_cancel_page_flip(struct drm_crtc *crtc, struct drm_file *file)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);

	if (vc4_crtc->event && vc4_crtc->event->base.file_priv == file) {
		kfree(&vc4_crtc->event->base);
		drm_crtc_vblank_put(crtc);
		vc4_crtc->event = NULL;
	}

	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static const struct of_device_id vc4_firmware_kms_dt_match[] = {
	{ .compatible = "raspberrypi,rpi-firmware-kms" },
	{}
};

static enum drm_connector_status
vc4_fkms_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static int vc4_fkms_connector_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	u32 wh[2] = {0, 0};
	int ret;
	struct drm_display_mode *mode;

	ret = rpi_firmware_property(vc4->firmware,
				    RPI_FIRMWARE_FRAMEBUFFER_GET_PHYSICAL_WIDTH_HEIGHT,
				    &wh, sizeof(wh));
	if (ret) {
		DRM_ERROR("Failed to get screen size: %d (0x%08x 0x%08x)\n",
			  ret, wh[0], wh[1]);
		return 0;
	}

	mode = drm_cvt_mode(dev, wh[0], wh[1], 60 /* vrefresh */,
			    0, 0, false);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static struct drm_encoder *
vc4_fkms_connector_best_encoder(struct drm_connector *connector)
{
	struct vc4_fkms_connector *fkms_connector =
		to_vc4_fkms_connector(connector);
	return fkms_connector->encoder;
}

static void vc4_fkms_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs vc4_fkms_connector_funcs = {
	.detect = vc4_fkms_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = vc4_fkms_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs vc4_fkms_connector_helper_funcs = {
	.get_modes = vc4_fkms_connector_get_modes,
	.best_encoder = vc4_fkms_connector_best_encoder,
};

static struct drm_connector *vc4_fkms_connector_init(struct drm_device *dev,
						     struct drm_encoder *encoder)
{
	struct drm_connector *connector = NULL;
	struct vc4_fkms_connector *fkms_connector;
	int ret = 0;

	fkms_connector = devm_kzalloc(dev->dev, sizeof(*fkms_connector),
				      GFP_KERNEL);
	if (!fkms_connector) {
		ret = -ENOMEM;
		goto fail;
	}
	connector = &fkms_connector->base;

	fkms_connector->encoder = encoder;

	drm_connector_init(dev, connector, &vc4_fkms_connector_funcs,
			   DRM_MODE_CONNECTOR_HDMIA);
	drm_connector_helper_add(connector, &vc4_fkms_connector_helper_funcs);

	connector->polled = (DRM_CONNECTOR_POLL_CONNECT |
			     DRM_CONNECTOR_POLL_DISCONNECT);

	connector->interlace_allowed = 0;
	connector->doublescan_allowed = 0;

	drm_mode_connector_attach_encoder(connector, encoder);

	return connector;

 fail:
	if (connector)
		vc4_fkms_connector_destroy(connector);

	return ERR_PTR(ret);
}

static void vc4_fkms_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
}

static const struct drm_encoder_funcs vc4_fkms_encoder_funcs = {
	.destroy = vc4_fkms_encoder_destroy,
};

static void vc4_fkms_encoder_enable(struct drm_encoder *encoder)
{
}

static void vc4_fkms_encoder_disable(struct drm_encoder *encoder)
{
}

static const struct drm_encoder_helper_funcs vc4_fkms_encoder_helper_funcs = {
	.enable = vc4_fkms_encoder_enable,
	.disable = vc4_fkms_encoder_disable,
};

static int vc4_fkms_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_crtc *vc4_crtc;
	struct vc4_fkms_encoder *vc4_encoder;
	struct drm_crtc *crtc;
	struct drm_plane *primary_plane, *cursor_plane, *destroy_plane, *temp;
	struct device_node *firmware_node;
	int ret;

	vc4->firmware_kms = true;

	vc4_crtc = devm_kzalloc(dev, sizeof(*vc4_crtc), GFP_KERNEL);
	if (!vc4_crtc)
		return -ENOMEM;
	crtc = &vc4_crtc->base;

	firmware_node = of_parse_phandle(dev->of_node, "brcm,firmware", 0);
	vc4->firmware = rpi_firmware_get(firmware_node);
	if (!vc4->firmware) {
		DRM_DEBUG("Failed to get Raspberry Pi firmware reference.\n");
		return -EPROBE_DEFER;
	}
	of_node_put(firmware_node);

	/* Map the SMI interrupt reg */
	vc4_crtc->regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(vc4_crtc->regs))
		return PTR_ERR(vc4_crtc->regs);

	/* For now, we create just the primary and the legacy cursor
	 * planes.  We should be able to stack more planes on easily,
	 * but to do that we would need to compute the bandwidth
	 * requirement of the plane configuration, and reject ones
	 * that will take too much.
	 */
	primary_plane = vc4_fkms_plane_init(drm, DRM_PLANE_TYPE_PRIMARY);
	if (IS_ERR(primary_plane)) {
		dev_err(dev, "failed to construct primary plane\n");
		ret = PTR_ERR(primary_plane);
		goto err;
	}

	cursor_plane = vc4_fkms_plane_init(drm, DRM_PLANE_TYPE_CURSOR);
	if (IS_ERR(cursor_plane)) {
		dev_err(dev, "failed to construct cursor plane\n");
		ret = PTR_ERR(cursor_plane);
		goto err;
	}

	drm_crtc_init_with_planes(drm, crtc, primary_plane, cursor_plane,
				  &vc4_crtc_funcs, NULL);
	drm_crtc_helper_add(crtc, &vc4_crtc_helper_funcs);
	primary_plane->crtc = crtc;
	cursor_plane->crtc = crtc;

	vc4_crtc->primary = primary_plane;
	vc4_crtc->cursor = cursor_plane;

	vc4_encoder = devm_kzalloc(dev, sizeof(*vc4_encoder), GFP_KERNEL);
	if (!vc4_encoder)
		return -ENOMEM;
	vc4_crtc->encoder = &vc4_encoder->base;
	vc4_encoder->base.possible_crtcs |= drm_crtc_mask(crtc) ;
	drm_encoder_init(drm, &vc4_encoder->base, &vc4_fkms_encoder_funcs,
			 DRM_MODE_ENCODER_TMDS, NULL);
	drm_encoder_helper_add(&vc4_encoder->base,
			       &vc4_fkms_encoder_helper_funcs);

	vc4_crtc->connector = vc4_fkms_connector_init(drm, &vc4_encoder->base);
	if (IS_ERR(vc4_crtc->connector)) {
		ret = PTR_ERR(vc4_crtc->connector);
		goto err_destroy_encoder;
	}

	writel(0, vc4_crtc->regs + SMICS);
	ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
			       vc4_crtc_irq_handler, 0, "vc4 firmware kms",
			       vc4_crtc);
	if (ret)
		goto err_destroy_connector;

	platform_set_drvdata(pdev, vc4_crtc);

	return 0;

err_destroy_connector:
	vc4_fkms_connector_destroy(vc4_crtc->connector);
err_destroy_encoder:
	vc4_fkms_encoder_destroy(vc4_crtc->encoder);
	list_for_each_entry_safe(destroy_plane, temp,
				 &drm->mode_config.plane_list, head) {
		if (destroy_plane->possible_crtcs == 1 << drm_crtc_index(crtc))
		    destroy_plane->funcs->destroy(destroy_plane);
	}
err:
	return ret;
}

static void vc4_fkms_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct vc4_crtc *vc4_crtc = dev_get_drvdata(dev);

	vc4_fkms_connector_destroy(vc4_crtc->connector);
	vc4_fkms_encoder_destroy(vc4_crtc->encoder);
	drm_crtc_cleanup(&vc4_crtc->base);

	platform_set_drvdata(pdev, NULL);
}

static const struct component_ops vc4_fkms_ops = {
	.bind   = vc4_fkms_bind,
	.unbind = vc4_fkms_unbind,
};

static int vc4_fkms_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_fkms_ops);
}

static int vc4_fkms_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_fkms_ops);
	return 0;
}

struct platform_driver vc4_firmware_kms_driver = {
	.probe = vc4_fkms_probe,
	.remove = vc4_fkms_remove,
	.driver = {
		.name = "vc4_firmware_kms",
		.of_match_table = vc4_firmware_kms_dt_match,
	},
};
