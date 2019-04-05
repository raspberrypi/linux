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
#include "drm/drm_gem_framebuffer_helper.h"
#include "drm/drm_plane_helper.h"
#include "drm/drm_crtc_helper.h"
#include "drm/drm_fourcc.h"
#include "drm/drm_probe_helper.h"
#include "linux/clk.h"
#include "linux/debugfs.h"
#include "drm/drm_fb_cma_helper.h"
#include "linux/component.h"
#include "linux/of_device.h"
#include "vc4_drv.h"
#include "vc4_regs.h"
#include "vc_image_types.h"
#include <soc/bcm2835/raspberrypi-firmware.h>

#define PLANES_PER_CRTC		3

struct set_plane {
	u8 display;
	u8 plane_id;
	u8 vc_image_type;
	s8 layer;

	u16 width;
	u16 height;

	u16 pitch;
	u16 vpitch;

	u32 src_x;	/* 16p16 */
	u32 src_y;	/* 16p16 */

	u32 src_w;	/* 16p16 */
	u32 src_h;	/* 16p16 */

	s16 dst_x;
	s16 dst_y;

	u16 dst_w;
	u16 dst_h;

	u8 alpha;
	u8 num_planes;
	u8 is_vu;
	u8 padding;

	u32 planes[4];  /* DMA address of each plane */
};

struct mailbox_set_plane {
	struct rpi_firmware_property_tag_header tag;
	struct set_plane plane;
};

struct fb_alloc_tags {
	struct rpi_firmware_property_tag_header tag1;
	u32 xres, yres;
	struct rpi_firmware_property_tag_header tag2;
	u32 xres_virtual, yres_virtual;
	struct rpi_firmware_property_tag_header tag3;
	u32 bpp;
	struct rpi_firmware_property_tag_header tag4;
	u32 xoffset, yoffset;
	struct rpi_firmware_property_tag_header tag5;
	u32 base, screen_size;
	struct rpi_firmware_property_tag_header tag6;
	u32 pitch;
	struct rpi_firmware_property_tag_header tag7;
	u32 alpha_mode;
	struct rpi_firmware_property_tag_header tag8;
	u32 layer;
};

static const struct vc_image_format {
	u32 drm;	/* DRM_FORMAT_* */
	u32 vc_image;	/* VC_IMAGE_* */
	u32 is_vu;
} vc_image_formats[] = {
	{
		.drm = DRM_FORMAT_XRGB8888,
		.vc_image = VC_IMAGE_XRGB8888,
	},
	{
		.drm = DRM_FORMAT_ARGB8888,
		.vc_image = VC_IMAGE_ARGB8888,
	},
/*
 *	FIXME: Need to resolve which DRM format goes to which vc_image format
 *	for the remaining RGBA and RGBX formats.
 *	{
 *		.drm = DRM_FORMAT_ABGR8888,
 *		.vc_image = VC_IMAGE_RGBA8888,
 *	},
 *	{
 *		.drm = DRM_FORMAT_XBGR8888,
 *		.vc_image = VC_IMAGE_RGBA8888,
 *	},
 */
	{
		.drm = DRM_FORMAT_RGB565,
		.vc_image = VC_IMAGE_RGB565,
	},
	{
		.drm = DRM_FORMAT_RGB888,
		.vc_image = VC_IMAGE_BGR888,
	},
	{
		.drm = DRM_FORMAT_BGR888,
		.vc_image = VC_IMAGE_RGB888,
	},
	{
		.drm = DRM_FORMAT_YUV422,
		.vc_image = VC_IMAGE_YUV422PLANAR,
	},
	{
		.drm = DRM_FORMAT_YUV420,
		.vc_image = VC_IMAGE_YUV420,
	},
	{
		.drm = DRM_FORMAT_YVU420,
		.vc_image = VC_IMAGE_YUV420,
		.is_vu = 1,
	},
	{
		.drm = DRM_FORMAT_NV12,
		.vc_image = VC_IMAGE_YUV420SP,
	},
	{
		.drm = DRM_FORMAT_NV21,
		.vc_image = VC_IMAGE_YUV420SP,
		.is_vu = 1,
	},
};

static const struct vc_image_format *vc4_get_vc_image_fmt(u32 drm_format)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vc_image_formats); i++) {
		if (vc_image_formats[i].drm == drm_format)
			return &vc_image_formats[i];
	}

	return NULL;
}

/* The firmware delivers a vblank interrupt to us through the SMI
 * hardware, which has only this one register.
 */
#define SMICS 0x0
#define SMICS_INTERRUPTS (BIT(9) | BIT(10) | BIT(11))

#define vc4_crtc vc4_kms_crtc
#define to_vc4_crtc to_vc4_kms_crtc
struct vc4_crtc {
	struct drm_crtc base;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	void __iomem *regs;

	struct drm_pending_vblank_event *event;
	u32 overscan[4];
	bool vblank_enabled;
	u32 display_number;
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
	struct mailbox_set_plane mb;
};

static inline struct vc4_fkms_plane *to_vc4_fkms_plane(struct drm_plane *plane)
{
	return (struct vc4_fkms_plane *)plane;
}

static int vc4_plane_set_blank(struct drm_plane *plane, bool blank)
{
	struct vc4_dev *vc4 = to_vc4_dev(plane->dev);
	struct vc4_fkms_plane *vc4_plane = to_vc4_fkms_plane(plane);
	struct mailbox_set_plane blank_mb = {
		.tag = { RPI_FIRMWARE_SET_PLANE, sizeof(struct set_plane), 0 },
		.plane = {
			.display = vc4_plane->mb.plane.display,
			.plane_id = vc4_plane->mb.plane.plane_id,
		}
	};
	int ret;

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] overlay plane %s",
			 plane->base.id, plane->name,
			 blank ? "blank" : "unblank");

	if (blank)
		ret = rpi_firmware_property_list(vc4->firmware, &blank_mb,
						 sizeof(blank_mb));
	else
		ret = rpi_firmware_property_list(vc4->firmware, &vc4_plane->mb,
						 sizeof(vc4_plane->mb));

	WARN_ONCE(ret, "%s: firmware call failed. Please update your firmware",
		  __func__);
	return ret;
}

static void vc4_plane_atomic_update(struct drm_plane *plane,
				    struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_cma_object *bo = drm_fb_cma_get_gem_obj(fb, 0);
	const struct drm_format_info *drm_fmt = fb->format;
	const struct vc_image_format *vc_fmt =
					vc4_get_vc_image_fmt(drm_fmt->format);
	struct vc4_fkms_plane *vc4_plane = to_vc4_fkms_plane(plane);
	struct mailbox_set_plane *mb = &vc4_plane->mb;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(state->crtc);
	int num_planes = fb->format->num_planes;
	struct drm_display_mode *mode = &state->crtc->mode;

	mb->plane.vc_image_type = vc_fmt->vc_image;
	mb->plane.width = fb->width;
	mb->plane.height = fb->height;
	mb->plane.pitch = fb->pitches[0];
	mb->plane.src_w = state->src_w;
	mb->plane.src_h = state->src_h;
	mb->plane.src_x = state->src_x;
	mb->plane.src_y = state->src_y;
	mb->plane.dst_w = state->crtc_w;
	mb->plane.dst_h = state->crtc_h;
	mb->plane.dst_x = state->crtc_x;
	mb->plane.dst_y = state->crtc_y;
	mb->plane.alpha = state->alpha >> 8;
	mb->plane.layer = state->normalized_zpos ?
					state->normalized_zpos : -127;
	mb->plane.num_planes = num_planes;
	mb->plane.is_vu = vc_fmt->is_vu;
	mb->plane.planes[0] = bo->paddr + fb->offsets[0];

	/* FIXME: If the dest rect goes off screen then clip the src rect so we
	 * don't have off-screen pixels.
	 */
	if (plane->type == DRM_PLANE_TYPE_CURSOR) {
		/* There is no scaling on the cursor plane, therefore the calcs
		 * to alter the source crop as the cursor goes off the screen
		 * are simple.
		 */
		if (mb->plane.dst_x + mb->plane.dst_w > mode->hdisplay) {
			mb->plane.dst_w = mode->hdisplay - mb->plane.dst_x;
			mb->plane.src_w = (mode->hdisplay - mb->plane.dst_x)
									<< 16;
		}
		if (mb->plane.dst_y + mb->plane.dst_h > mode->vdisplay) {
			mb->plane.dst_h = mode->vdisplay - mb->plane.dst_y;
			mb->plane.src_h = (mode->vdisplay - mb->plane.dst_y)
									<< 16;
		}
	}

	if (num_planes > 1) {
		/* Assume this must be YUV */
		/* Makes assumptions on the stride for the chroma planes as we
		 * can't easily plumb in non-standard pitches.
		 */
		mb->plane.planes[1] = bo->paddr + fb->offsets[1];
		if (num_planes > 2)
			mb->plane.planes[2] = bo->paddr + fb->offsets[2];
		else
			mb->plane.planes[2] = 0;

		/* Special case the YUV420 with U and V as line interleaved
		 * planes as we have special handling for that case.
		 */
		if (num_planes == 3 &&
		    (fb->offsets[2] - fb->offsets[1]) == fb->pitches[1])
			mb->plane.vc_image_type = VC_IMAGE_YUV420_S;
	} else {
		mb->plane.planes[1] = 0;
		mb->plane.planes[2] = 0;
	}
	mb->plane.planes[3] = 0;

	switch (fb->modifier) {
	case DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED:
		switch (mb->plane.vc_image_type) {
		case VC_IMAGE_RGBX32:
			mb->plane.vc_image_type = VC_IMAGE_TF_RGBX32;
			break;
		case VC_IMAGE_RGBA32:
			mb->plane.vc_image_type = VC_IMAGE_TF_RGBA32;
			break;
		case VC_IMAGE_RGB565:
			mb->plane.vc_image_type = VC_IMAGE_TF_RGB565;
			break;
		}
		break;
	case DRM_FORMAT_MOD_BROADCOM_SAND128:
		mb->plane.vc_image_type = VC_IMAGE_YUV_UV;
		mb->plane.pitch = fourcc_mod_broadcom_param(fb->modifier);
		break;
	}

	if (vc4_crtc) {
		mb->plane.dst_x += vc4_crtc->overscan[0];
		mb->plane.dst_y += vc4_crtc->overscan[1];
	}

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] plane update %dx%d@%d +dst(%d,%d, %d,%d) +src(%d,%d, %d,%d) 0x%08x/%08x/%08x/%d, alpha %u zpos %u\n",
			 plane->base.id, plane->name,
			 mb->plane.width,
			 mb->plane.height,
			 mb->plane.vc_image_type,
			 state->crtc_x,
			 state->crtc_y,
			 state->crtc_w,
			 state->crtc_h,
			 mb->plane.src_x,
			 mb->plane.src_y,
			 mb->plane.src_w,
			 mb->plane.src_h,
			 mb->plane.planes[0],
			 mb->plane.planes[1],
			 mb->plane.planes[2],
			 fb->pitches[0],
			 state->alpha,
			 state->normalized_zpos);

	/*
	 * Do NOT set now, as we haven't checked if the crtc is active or not.
	 * Set from vc4_plane_set_blank instead.
	 *
	 * If the CRTC is on (or going to be on) and we're enabled,
	 * then unblank.  Otherwise, stay blank until CRTC enable.
	 */
	if (state->crtc->state->active)
		vc4_plane_set_blank(plane, false);
}

static void vc4_plane_atomic_disable(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	//struct vc4_dev *vc4 = to_vc4_dev(plane->dev);
	struct drm_plane_state *state = plane->state;
	struct vc4_fkms_plane *vc4_plane = to_vc4_fkms_plane(plane);

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] plane disable %dx%d@%d +%d,%d\n",
			 plane->base.id, plane->name,
			 state->crtc_w,
			 state->crtc_h,
			 vc4_plane->mb.plane.vc_image_type,
			 state->crtc_x,
			 state->crtc_y);
	vc4_plane_set_blank(plane, true);
}

static int vc4_plane_atomic_check(struct drm_plane *plane,
				  struct drm_plane_state *state)
{
	return 0;
}

static void vc4_plane_destroy(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
}

static bool vc4_fkms_format_mod_supported(struct drm_plane *plane,
					  uint32_t format,
					  uint64_t modifier)
{
	/* Support T_TILING for RGB formats only. */
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_RGB565:
		switch (modifier) {
		case DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED:
		case DRM_FORMAT_MOD_LINEAR:
		case DRM_FORMAT_MOD_BROADCOM_UIF:
			return true;
		default:
			return false;
		}
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		switch (fourcc_mod_broadcom_mod(modifier)) {
		case DRM_FORMAT_MOD_LINEAR:
		case DRM_FORMAT_MOD_BROADCOM_SAND128:
			return true;
		default:
			return false;
		}
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_YUV422:
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
	default:
		return (modifier == DRM_FORMAT_MOD_LINEAR);
	}
}

static const struct drm_plane_funcs vc4_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = vc4_plane_destroy,
	.set_property = NULL,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
	.format_mod_supported = vc4_fkms_format_mod_supported,
};

static const struct drm_plane_helper_funcs vc4_plane_helper_funcs = {
	.prepare_fb = drm_gem_fb_prepare_fb,
	.cleanup_fb = NULL,
	.atomic_check = vc4_plane_atomic_check,
	.atomic_update = vc4_plane_atomic_update,
	.atomic_disable = vc4_plane_atomic_disable,
};

static struct drm_plane *vc4_fkms_plane_init(struct drm_device *dev,
					     enum drm_plane_type type,
					     u8 display_num,
					     u8 plane_id)
{
	struct drm_plane *plane = NULL;
	struct vc4_fkms_plane *vc4_plane;
	u32 formats[ARRAY_SIZE(vc_image_formats)];
	unsigned int default_zpos = 0;
	u32 num_formats = 0;
	int ret = 0;
	static const uint64_t modifiers[] = {
		DRM_FORMAT_MOD_LINEAR,
		/* VC4_T_TILED should come after linear, because we
		 * would prefer to scan out linear (less bus traffic).
		 */
		DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED,
		DRM_FORMAT_MOD_INVALID,
	};
	int i;

	vc4_plane = devm_kzalloc(dev->dev, sizeof(*vc4_plane),
				 GFP_KERNEL);
	if (!vc4_plane) {
		ret = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < ARRAY_SIZE(vc_image_formats); i++)
		formats[num_formats++] = vc_image_formats[i].drm;

	plane = &vc4_plane->base;
	ret = drm_universal_plane_init(dev, plane, 0xff,
				       &vc4_plane_funcs,
				       formats, num_formats, modifiers,
				       type, NULL);

	drm_plane_helper_add(plane, &vc4_plane_helper_funcs);

	drm_plane_create_alpha_property(plane);

	/*
	 * Default frame buffer setup is with FB on -127, and raspistill etc
	 * tend to drop overlays on layer 2. Cursor plane was on layer +127.
	 *
	 * For F-KMS the mailbox call allows for a s8.
	 * Remap zpos 0 to -127 for the background layer, but leave all the
	 * other layers as requested by KMS.
	 */
	switch (type) {
	case DRM_PLANE_TYPE_PRIMARY:
		default_zpos = 0;
		break;
	case DRM_PLANE_TYPE_OVERLAY:
		default_zpos = 1;
		break;
	case DRM_PLANE_TYPE_CURSOR:
		default_zpos = 2;
		break;
	}
	drm_plane_create_zpos_property(plane, default_zpos, 0, 127);

	/* Prepare the static elements of the mailbox structure */
	vc4_plane->mb.tag.tag = RPI_FIRMWARE_SET_PLANE;
	vc4_plane->mb.tag.buf_size = sizeof(struct set_plane);
	vc4_plane->mb.tag.req_resp_size = 0;
	vc4_plane->mb.plane.display = display_num;
	vc4_plane->mb.plane.plane_id = plane_id;
	vc4_plane->mb.plane.layer = default_zpos ? default_zpos : -127;

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
	/* Always turn the planes off on CRTC disable. In DRM, planes
	 * are enabled/disabled through the update/disable hooks
	 * above, and the CRTC enable/disable independently controls
	 * whether anything scans out at all, but the firmware doesn't
	 * give us a CRTC-level control for that.
	 */

	vc4_plane_atomic_disable(crtc->cursor, crtc->cursor->state);
	vc4_plane_atomic_disable(crtc->primary, crtc->primary->state);

	/* FIXME: Disable overlay planes */
}

static void vc4_crtc_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	/* Unblank the planes (if they're supposed to be displayed). */

	if (crtc->primary->state->fb)
		vc4_plane_set_blank(crtc->primary, false);
	if (crtc->cursor->state->fb)
		vc4_plane_set_blank(crtc->cursor, crtc->cursor->state);

	/* FIXME: Enable overlay planes */
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
	struct vc4_crtc **crtc_list = data;
	int i;
	u32 stat = readl(crtc_list[0]->regs + SMICS);
	irqreturn_t ret = IRQ_NONE;

	if (stat & SMICS_INTERRUPTS) {
		writel(0, crtc_list[0]->regs + SMICS);

		for (i = 0; crtc_list[i]; i++) {
			if (crtc_list[i]->vblank_enabled)
				drm_crtc_handle_vblank(&crtc_list[i]->base);
			vc4_crtc_handle_page_flip(crtc_list[i]);
			ret = IRQ_HANDLED;
		}
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

static int vc4_fkms_enable_vblank(struct drm_crtc *crtc)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);

	vc4_crtc->vblank_enabled = true;

	return 0;
}

static void vc4_fkms_disable_vblank(struct drm_crtc *crtc)
{
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
	.enable_vblank = vc4_fkms_enable_vblank,
	.disable_vblank = vc4_fkms_disable_vblank,
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

	drm_connector_attach_encoder(connector, encoder);

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

static int vc4_fkms_create_screen(struct device *dev, struct drm_device *drm,
				  int display_idx, int display_ref,
				  struct vc4_crtc **ret_crtc)
{
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_crtc *vc4_crtc;
	struct vc4_fkms_encoder *vc4_encoder;
	struct drm_crtc *crtc;
	struct drm_plane *primary_plane, *overlay_plane, *cursor_plane;
	struct drm_plane *destroy_plane, *temp;
	u32 blank = 1;
	int ret;

	vc4_crtc = devm_kzalloc(dev, sizeof(*vc4_crtc), GFP_KERNEL);
	if (!vc4_crtc)
		return -ENOMEM;
	crtc = &vc4_crtc->base;

	vc4_crtc->display_number = display_ref;

	/* Blank the firmware provided framebuffer */
	rpi_firmware_property(vc4->firmware,
			      RPI_FIRMWARE_FRAMEBUFFER_BLANK,
			      &blank, sizeof(blank));

	primary_plane = vc4_fkms_plane_init(drm, DRM_PLANE_TYPE_PRIMARY,
					    display_ref,
					    0 + (display_idx * PLANES_PER_CRTC)
					   );
	if (IS_ERR(primary_plane)) {
		dev_err(dev, "failed to construct primary plane\n");
		ret = PTR_ERR(primary_plane);
		goto err;
	}

	overlay_plane = vc4_fkms_plane_init(drm, DRM_PLANE_TYPE_OVERLAY,
					    display_ref,
					    1 + (display_idx * PLANES_PER_CRTC)
					   );
	if (IS_ERR(overlay_plane)) {
		dev_err(dev, "failed to construct overlay plane\n");
		ret = PTR_ERR(overlay_plane);
		goto err;
	}

	cursor_plane = vc4_fkms_plane_init(drm, DRM_PLANE_TYPE_CURSOR,
					   display_ref,
					   2 + (display_idx * PLANES_PER_CRTC)
					  );
	if (IS_ERR(cursor_plane)) {
		dev_err(dev, "failed to construct cursor plane\n");
		ret = PTR_ERR(cursor_plane);
		goto err;
	}

	drm_crtc_init_with_planes(drm, crtc, primary_plane, cursor_plane,
				  &vc4_crtc_funcs, NULL);
	drm_crtc_helper_add(crtc, &vc4_crtc_helper_funcs);

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

	ret = rpi_firmware_property(vc4->firmware,
				    RPI_FIRMWARE_FRAMEBUFFER_GET_OVERSCAN,
				    &vc4_crtc->overscan,
				    sizeof(vc4_crtc->overscan));
	if (ret) {
		DRM_ERROR("Failed to get overscan state: 0x%08x\n", vc4_crtc->overscan[0]);
		memset(&vc4_crtc->overscan, 0, sizeof(vc4_crtc->overscan));
	}

	*ret_crtc = vc4_crtc;

	return 0;

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

static int vc4_fkms_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct device_node *firmware_node;
	struct vc4_crtc **crtc_list;
	u32 num_displays, display_num;
	int ret;
	const u32 display_num_lookup[] = {2, 7, 1};

	vc4->firmware_kms = true;

	/* firmware kms doesn't have precise a scanoutpos implementation, so
	 * we can't do the precise vblank timestamp mode.
	 */
	drm->driver->get_scanout_position = NULL;
	drm->driver->get_vblank_timestamp = NULL;

	firmware_node = of_parse_phandle(dev->of_node, "brcm,firmware", 0);
	vc4->firmware = rpi_firmware_get(firmware_node);
	if (!vc4->firmware) {
		DRM_DEBUG("Failed to get Raspberry Pi firmware reference.\n");
		return -EPROBE_DEFER;
	}
	of_node_put(firmware_node);

	ret = rpi_firmware_property(vc4->firmware,
				    RPI_FIRMWARE_FRAMEBUFFER_GET_NUM_DISPLAYS,
				    &num_displays, sizeof(u32));

	/* If we fail to get the number of displays, or it returns 0, then
	 * assume old firmware that doesn't have the mailbox call, so just
	 * set one display
	 */
	if (ret || num_displays == 0) {
		num_displays = 1;
		DRM_WARN("Unable to determine number of displays's. Assuming 1\n");
		ret = 0;
	}

	/* Allocate a list, with space for a NULL on the end */
	crtc_list = devm_kzalloc(dev, sizeof(crtc_list) * (num_displays + 1),
				 GFP_KERNEL);
	if (!crtc_list)
		return -ENOMEM;

	for (display_num = 0; display_num < num_displays; display_num++) {
		ret = vc4_fkms_create_screen(dev, drm, display_num,
					     display_num_lookup[display_num],
					     &crtc_list[display_num]);
		if (ret)
			DRM_ERROR("Oh dear, failed to create display %u\n",
				  display_num);
	}

	/* Map the SMI interrupt reg */
	crtc_list[0]->regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(crtc_list[0]->regs))
		DRM_ERROR("Oh dear, failed to map registers\n");

	writel(0, crtc_list[0]->regs + SMICS);
	ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
			       vc4_crtc_irq_handler, 0, "vc4 firmware kms",
			       crtc_list);
	if (ret)
		DRM_ERROR("Oh dear, failed to register IRQ\n");

	platform_set_drvdata(pdev, crtc_list);

	return 0;
}

static void vc4_fkms_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct vc4_crtc **crtc_list = dev_get_drvdata(dev);
	int i;

	for (i = 0; crtc_list[i]; i++) {
		vc4_fkms_connector_destroy(crtc_list[i]->connector);
		vc4_fkms_encoder_destroy(crtc_list[i]->encoder);
		drm_crtc_cleanup(&crtc_list[i]->base);
	}

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
