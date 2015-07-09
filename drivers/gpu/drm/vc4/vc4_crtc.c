/*
 * Copyright (C) 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Controls the timings of the hardware's pixel valve.
 */

#include "drm_atomic.h"
#include "drm_atomic_helper.h"
#include "drm_crtc_helper.h"
#include "drm_fb_cma_helper.h"
#include "linux/component.h"
#include "vc4_drv.h"
#include "vc4_regs.h"

#define CRTC_WRITE(offset, val) writel(val, vc4_crtc->regs + (offset))
#define CRTC_READ(offset) readl(vc4_crtc->regs + (offset))

#define CRTC_REG(reg) { reg, #reg }
static const struct {
	u32 reg;
	const char *name;
} crtc_regs[] = {
	CRTC_REG(PV_CONTROL),
	CRTC_REG(PV_V_CONTROL),
	CRTC_REG(PV_VSYNCD),
	CRTC_REG(PV_HORZA),
	CRTC_REG(PV_HORZB),
	CRTC_REG(PV_VERTA),
	CRTC_REG(PV_VERTB),
	CRTC_REG(PV_VERTA_EVEN),
	CRTC_REG(PV_VERTB_EVEN),
	CRTC_REG(PV_INTEN),
	CRTC_REG(PV_INTSTAT),
	CRTC_REG(PV_STAT),
	CRTC_REG(PV_HACT_ACT),
};

static void
vc4_crtc_dump_regs(struct vc4_crtc *vc4_crtc)
{
	int i;

	rmb();
	for (i = 0; i < ARRAY_SIZE(crtc_regs); i++) {
		DRM_INFO("0x%04x (%s): 0x%08x\n",
			 crtc_regs[i].reg, crtc_regs[i].name,
			 CRTC_READ(crtc_regs[i].reg));
	}
}

static void vc4_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
}

static bool vc4_crtc_mode_fixup(struct drm_crtc *crtc,
				const struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	return true;
}

static u32 vc4_get_fifo_full_level(u32 format)
{
	static const u32 fifo_len_bytes = 64;
	static const u32 hvs_latency_pix = 6;

	switch(format) {
	case PV_CONTROL_FORMAT_DSIV_16:
	case PV_CONTROL_FORMAT_DSIC_16:
		return fifo_len_bytes - 2 * hvs_latency_pix;
	case PV_CONTROL_FORMAT_DSIV_18:
		return fifo_len_bytes - 14;
	case PV_CONTROL_FORMAT_24:
	case PV_CONTROL_FORMAT_DSIV_24:
	default:
		return fifo_len_bytes - 3 * hvs_latency_pix;
	}
}

static void vc4_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_crtc_state *state = crtc->state;
	struct drm_display_mode *mode = &state->adjusted_mode;
	u32 vactive = (mode->vdisplay >>
		       ((mode->flags & DRM_MODE_FLAG_INTERLACE) ? 1 : 0));
	u32 format = PV_CONTROL_FORMAT_24;
	bool debug_dump_regs = false;

	if (debug_dump_regs) {
		DRM_INFO("CRTC %d regs before:\n", drm_crtc_index(crtc));
		vc4_crtc_dump_regs(vc4_crtc);
	}

	/* Reset the PV fifo. */
	CRTC_WRITE(PV_CONTROL, 0);
	CRTC_WRITE(PV_CONTROL, PV_CONTROL_FIFO_CLR | PV_CONTROL_EN);
	CRTC_WRITE(PV_CONTROL, 0);

	CRTC_WRITE(PV_HORZA,
		   VC4_SET_FIELD(mode->htotal - mode->hsync_end,
				 PV_HORZA_HBP) |
		   VC4_SET_FIELD(mode->hsync_end - mode->hsync_start,
				 PV_HORZA_HSYNC));
	CRTC_WRITE(PV_HORZB,
		   VC4_SET_FIELD(mode->hsync_start - mode->hdisplay,
				 PV_HORZB_HFP) |
		   VC4_SET_FIELD(mode->hdisplay, PV_HORZB_HACTIVE));

	CRTC_WRITE(PV_VERTA,
		   VC4_SET_FIELD(mode->vtotal - mode->vsync_end,
				 PV_VERTA_VBP) |
		   VC4_SET_FIELD(mode->vsync_end - mode->vsync_start,
				 PV_VERTA_VSYNC));
	CRTC_WRITE(PV_VERTB,
		   VC4_SET_FIELD(mode->vsync_start - mode->vdisplay,
				 PV_VERTB_VFP) |
		   VC4_SET_FIELD(vactive, PV_VERTB_VACTIVE));
	if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
		/* Write PV_VERTA_EVEN/VERTB_EVEN */
	}

	CRTC_WRITE(PV_HACT_ACT, mode->hdisplay);

	CRTC_WRITE(PV_CONTROL,
		   VC4_SET_FIELD(format, PV_CONTROL_FORMAT) |
		   VC4_SET_FIELD(vc4_get_fifo_full_level(format),
				 PV_CONTROL_FIFO_LEVEL) |
		   PV_CONTROL_CLR_AT_START |
		   PV_CONTROL_TRIGGER_UNDERFLOW |
		   PV_CONTROL_WAIT_HSTART |
		   PV_CONTROL_CLK_MUX_EN |
		   VC4_SET_FIELD(PV_CONTROL_CLK_SELECT_DPI_SMI_HDMI,
				 PV_CONTROL_CLK_SELECT) |
		   PV_CONTROL_FIFO_CLR |
		   PV_CONTROL_EN);

	CRTC_WRITE(PV_V_CONTROL,
		   PV_VCONTROL_CONTINUOUS);

	if (debug_dump_regs) {
		DRM_INFO("CRTC %d regs after:\n", drm_crtc_index(crtc));
		vc4_crtc_dump_regs(vc4_crtc);
	}
}

static void
require_hvs_enabled(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	WARN_ON_ONCE((HVS_READ(SCALER_DISPCTRL) & SCALER_DISPCTRL_ENABLE) !=
		     SCALER_DISPCTRL_ENABLE);
}

static void vc4_crtc_disable(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);

	require_hvs_enabled(dev);

	CRTC_WRITE(PV_V_CONTROL,
		   CRTC_READ(PV_V_CONTROL) & ~PV_VCONTROL_VIDEN);

	/* Without a wait here, we end up with a black screen and the
	 * scaler fifo empty warning triggering during
	 * vc4_crtc_enable().
	 */
	msleep(30);

	if (HVS_READ(SCALER_DISPCTRLX(vc4_crtc->channel)) &
	    SCALER_DISPCTRLX_ENABLE) {
		HVS_WRITE(SCALER_DISPCTRLX(vc4_crtc->channel),
			  SCALER_DISPCTRLX_RESET);

		/*
		 * While the docs say that reset is self-clearing, it
		 * seems it doesn't actually.
		 */
		HVS_WRITE(SCALER_DISPCTRLX(vc4_crtc->channel), 0);
	}

	/* Once we leave, the scaler should be disabled and its fifo empty. */

	WARN_ON_ONCE(HVS_READ(SCALER_DISPCTRLX(vc4_crtc->channel)) &
		     SCALER_DISPCTRLX_RESET);

	WARN_ON_ONCE(VC4_GET_FIELD(HVS_READ(SCALER_DISPSTATX(vc4_crtc->channel)),
				   SCALER_DISPSTATX_MODE) !=
		     SCALER_DISPSTATX_MODE_DISABLED);

	WARN_ON_ONCE((HVS_READ(SCALER_DISPSTATX(vc4_crtc->channel)) &
		      (SCALER_DISPSTATX_FULL | SCALER_DISPSTATX_EMPTY)) !=
		     SCALER_DISPSTATX_EMPTY);
}

static void vc4_crtc_enable(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_crtc_state *state = crtc->state;
	struct drm_display_mode *mode = &state->adjusted_mode;

	require_hvs_enabled(dev);

	/* Turn on the scaler, which will wait for vstart to start
	 * compositing.
	 */
	HVS_WRITE(SCALER_DISPCTRLX(vc4_crtc->channel),
		  VC4_SET_FIELD(mode->hdisplay, SCALER_DISPCTRLX_WIDTH) |
		  VC4_SET_FIELD(mode->vdisplay, SCALER_DISPCTRLX_HEIGHT) |
		  SCALER_DISPCTRLX_ENABLE);

	/* The FIFO should still be empty at this point, since the PV
	 * is disabled, and thus we haven't seen the start.
	 */
	WARN_ON_ONCE((HVS_READ(SCALER_DISPSTATX(vc4_crtc->channel)) &
		      (SCALER_DISPSTATX_FULL | SCALER_DISPSTATX_EMPTY)) !=
		     SCALER_DISPSTATX_EMPTY);

	/* Turn on the pixel valve, which will emit the vstart signal. */
	CRTC_WRITE(PV_V_CONTROL,
		   CRTC_READ(PV_V_CONTROL) | PV_VCONTROL_VIDEN);
}

static int vc4_crtc_atomic_check(struct drm_crtc *crtc,
				 struct drm_crtc_state *state)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_plane *plane;
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	u32 dlist_count = 0;

	drm_atomic_crtc_state_for_each_plane(plane, state) {
		struct drm_plane_state *plane_state =
			state->state->plane_states[drm_plane_index(plane)];

		/* plane might not have changed, in which case take
		 * current state:
		 */
		if (!plane_state)
			plane_state = plane->state;

		dlist_count += vc4_plane_dlist_size(plane_state);
	}

	dlist_count++; /* Account for SCALER_CTL0_END. */

	if (!vc4_crtc->dlist || dlist_count > vc4_crtc->dlist_size) {
		vc4_crtc->dlist = ((u32 __iomem *)vc4->hvs->dlist +
				   HVS_BOOTLOADER_DLIST_END);
		vc4_crtc->dlist_size = ((SCALER_DLIST_SIZE >> 2) -
					HVS_BOOTLOADER_DLIST_END);

		if (dlist_count > vc4_crtc->dlist_size) {
			DRM_DEBUG_KMS("dlist too large for CRTC (%d > %d).\n",
				      dlist_count, vc4_crtc->dlist_size);
			return -EINVAL;
		}
	}

	return 0;
}

static void vc4_crtc_atomic_begin(struct drm_crtc *crtc)
{
}

static void vc4_crtc_atomic_flush(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_plane *plane;
	bool debug_dump_regs = false;
	u32 __iomem *dlist_next = vc4_crtc->dlist;

	if (debug_dump_regs) {
		DRM_INFO("CRTC %d HVS before:\n", drm_crtc_index(crtc));
		vc4_hvs_dump_state(dev);
	}

	/*
	 * Copy all the active planes' dlist contents to the hardware dlist.
	 *
	 * XXX: If the new display list was large enough that it
	 * overlapped a currently-read display list, we need to do
	 * something like disable scanout before putting in the new
	 * list.
	 */
	drm_atomic_crtc_for_each_plane(plane, crtc) {
		dlist_next += vc4_plane_write_dlist(plane, dlist_next);
	}

	if (dlist_next == vc4_crtc->dlist) {
		/* If no planes were enabled, use the SCALER_CTL0_END
		 * at the start of the display list memory (in the
		 * bootloader section).  We'll rewrite that
		 * SCALER_CTL0_END, just in case, though.
		 */
		writel(SCALER_CTL0_END, vc4->hvs->dlist);
		HVS_WRITE(SCALER_DISPLISTX(vc4_crtc->channel), 0);
	} else {
		writel(SCALER_CTL0_END, dlist_next);
		dlist_next++;

		HVS_WRITE(SCALER_DISPLISTX(vc4_crtc->channel),
			  (u32 *)vc4_crtc->dlist - (u32 *)vc4->hvs->dlist);

		/* Make the next display list start after ours. */
		vc4_crtc->dlist_size -= (dlist_next - vc4_crtc->dlist);
		vc4_crtc->dlist = dlist_next;
	}

	if (debug_dump_regs) {
		DRM_INFO("CRTC %d HVS after:\n", drm_crtc_index(crtc));
		vc4_hvs_dump_state(dev);
	}

	if (crtc->state->event) {
		unsigned long flags;

		crtc->state->event->pipe = drm_crtc_index(crtc);

		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		spin_lock_irqsave(&dev->event_lock, flags);
		vc4_crtc->event = crtc->state->event;
		spin_unlock_irqrestore(&dev->event_lock, flags);
		crtc->state->event = NULL;
	}
}

int vc4_enable_vblank(struct drm_device *dev, int crtc_id)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_crtc *vc4_crtc = vc4->crtc[crtc_id];

	CRTC_WRITE(PV_INTEN, PV_INT_VFP_START);

	return 0;
}

void vc4_disable_vblank(struct drm_device *dev, int crtc_id)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_crtc *vc4_crtc = vc4->crtc[crtc_id];

	CRTC_WRITE(PV_INTEN, 0);
}

static void
vc4_crtc_handle_page_flip(struct vc4_crtc *vc4_crtc)
{
	struct drm_crtc *crtc = &vc4_crtc->base;
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	if (vc4_crtc->event) {
		drm_crtc_send_vblank_event(crtc, vc4_crtc->event);
		vc4_crtc->event = NULL;
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static irqreturn_t vc4_crtc_irq_handler(int irq, void *data)
{
	struct vc4_crtc *vc4_crtc = data;
	u32 stat = CRTC_READ(PV_INTSTAT);
	irqreturn_t ret = IRQ_NONE;

	if (stat & PV_INT_VFP_START) {
		CRTC_WRITE(PV_INTSTAT, PV_INT_VFP_START);
		drm_crtc_handle_vblank(&vc4_crtc->base);
		vc4_crtc_handle_page_flip(vc4_crtc);
		ret = IRQ_HANDLED;
	}

	return ret;
}

struct vc4_async_flip_state {
	struct drm_crtc *crtc;
	struct drm_framebuffer *fb;
	struct drm_pending_vblank_event *event;

	struct vc4_seqno_cb cb;
};

/* Called when the V3D execution for the BO being flipped to is done, so that
 * we can actually update the plane's address to point to it.
 */
static void
vc4_async_page_flip_complete(struct vc4_seqno_cb *cb)
{
	struct vc4_async_flip_state *flip_state =
		container_of(cb, struct vc4_async_flip_state, cb);
	struct drm_crtc *crtc = flip_state->crtc;
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_plane *plane = crtc->primary;

	vc4_plane_async_set_fb(plane, flip_state->fb);
	if (flip_state->event) {
		unsigned long flags;
		spin_lock_irqsave(&dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, flip_state->event);
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}

	drm_framebuffer_unreference(flip_state->fb);
	kfree(flip_state);

	up(&vc4->async_modeset);
}

/* Implements async (non-vblank-synced) page flips.
 *
 * The page flip ioctl needs to return immediately, so we grab the
 * modeset semaphore on the pipe, and queue the address update for
 * when V3D is done with the BO being flipped to.
 */
static int vc4_async_page_flip(struct drm_crtc *crtc,
			       struct drm_framebuffer *fb,
			       struct drm_pending_vblank_event *event,
			       uint32_t flags)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_plane *plane = crtc->primary;
	int ret = 0;
	struct vc4_async_flip_state *flip_state;
	struct drm_gem_cma_object *cma_bo = drm_fb_cma_get_gem_obj(fb, 0);
	struct vc4_bo *bo = to_vc4_bo(&cma_bo->base);

	flip_state = kzalloc(sizeof(*flip_state), GFP_KERNEL);
	if (!flip_state)
		return -ENOMEM;

	drm_framebuffer_reference(fb);
	flip_state->fb = fb;
	flip_state->crtc = crtc;
	flip_state->event = event;

	/* Make sure all other async modesetes have landed. */
	ret = down_interruptible(&vc4->async_modeset);
	if (ret) {
		kfree(flip_state);
		return ret;
	}

	/* Immediately update the plane's legacy fb pointer, so that later
	 * modeset prep sees the state that will be present when the semaphore
	 * is released.
	 */
	drm_atomic_set_fb_for_plane(plane->state, fb);
	plane->fb = fb;

	vc4_queue_seqno_cb(dev, &flip_state->cb, bo->seqno,
			   vc4_async_page_flip_complete);

	/* Driver takes ownership of state on successful async commit. */
	return 0;
}

static int vc4_page_flip(struct drm_crtc *crtc,
		  struct drm_framebuffer *fb,
		  struct drm_pending_vblank_event *event,
		  uint32_t flags)
{
	if (flags & DRM_MODE_PAGE_FLIP_ASYNC)
		return vc4_async_page_flip(crtc, fb, event, flags);
	else
		return drm_atomic_helper_page_flip(crtc, fb, event, flags);
}

static const struct drm_crtc_funcs vc4_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.destroy = vc4_crtc_destroy,
	.page_flip = vc4_page_flip,
	.set_property = NULL,
	.cursor_set = NULL, /* handled by drm_mode_cursor_universal */
	.cursor_move = NULL, /* handled by drm_mode_cursor_universal */
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs vc4_crtc_helper_funcs = {
	.mode_fixup = vc4_crtc_mode_fixup,
	.mode_set_nofb = vc4_crtc_mode_set_nofb,
	.disable = vc4_crtc_disable,
	.enable = vc4_crtc_enable,
	.atomic_check = vc4_crtc_atomic_check,
	.atomic_begin = vc4_crtc_atomic_begin,
	.atomic_flush = vc4_crtc_atomic_flush,
};

/* Frees the page flip event when the DRM device is closed with the
 * event still outstanding.
 */
void vc4_cancel_page_flip(struct drm_crtc *crtc, struct drm_file *file)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);

	if (vc4_crtc->event && vc4_crtc->event->base.file_priv == file) {
		vc4_crtc->event->base.destroy(&vc4_crtc->event->base);
		drm_crtc_vblank_put(crtc);
		vc4_crtc->event = NULL;
	}

	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static int vc4_crtc_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_crtc *vc4_crtc;
	struct drm_crtc *crtc;
	struct drm_plane *primary_plane, *cursor_plane;
	int ret;

	primary_plane = vc4_plane_init(drm, DRM_PLANE_TYPE_PRIMARY);
	if (!primary_plane) {
		dev_err(dev, "failed to construct primary plane\n");
		ret = PTR_ERR(primary_plane);
		goto fail;
	}

	cursor_plane = vc4_plane_init(drm, DRM_PLANE_TYPE_CURSOR);
	if (!cursor_plane) {
		dev_err(dev, "failed to construct cursor_plane\n");
		ret = PTR_ERR(cursor_plane);
		goto fail;
	}

	vc4_crtc = devm_kzalloc(dev, sizeof(*vc4_crtc), GFP_KERNEL);
	if (!vc4_crtc)
		return -ENOMEM;
	crtc = &vc4_crtc->base;

	vc4_crtc->regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(vc4_crtc->regs))
		return PTR_ERR(vc4_crtc->regs);

	drm_crtc_init_with_planes(drm, crtc, primary_plane, cursor_plane,
				  &vc4_crtc_funcs);
	drm_crtc_helper_add(crtc, &vc4_crtc_helper_funcs);
	primary_plane->crtc = crtc;
	cursor_plane->crtc = crtc;
	vc4->crtc[drm_crtc_index(crtc)] = vc4_crtc;

	/* Until we have full scanout setup to route things through to
	 * encoders, line things up like the firmware did.
	 */
	switch (drm_crtc_index(crtc)) {
	case 0:
		vc4_crtc->channel = 0;
		break;
	case 1:
		vc4_crtc->channel = 2;
		break;
	default:
	case 2:
		vc4_crtc->channel = 1;
		break;
	}

	CRTC_WRITE(PV_INTEN, 0);
	CRTC_WRITE(PV_INTSTAT, PV_INT_VFP_START);
	ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
			       vc4_crtc_irq_handler, 0, "vc4 crtc", vc4_crtc);

	platform_set_drvdata(pdev, vc4_crtc);

	return 0;

fail:
	return ret;
}

static void vc4_crtc_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct vc4_crtc *vc4_crtc = dev_get_drvdata(dev);

	vc4_crtc_destroy(&vc4_crtc->base);

	CRTC_WRITE(PV_INTEN, 0);

	platform_set_drvdata(pdev, NULL);
}

static const struct component_ops vc4_crtc_ops = {
	.bind   = vc4_crtc_bind,
	.unbind = vc4_crtc_unbind,
};

static int vc4_crtc_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_crtc_ops);
}

static int vc4_crtc_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_crtc_ops);
	return 0;
}

static const struct of_device_id vc4_crtc_dt_match[] = {
	{ .compatible = "brcm,vc4-pixelvalve" },
	{}
};

static struct platform_driver vc4_crtc_driver = {
	.probe = vc4_crtc_dev_probe,
	.remove = vc4_crtc_dev_remove,
	.driver = {
		.name = "vc4_crtc",
		.of_match_table = vc4_crtc_dt_match,
	},
};

void __init vc4_crtc_register(void)
{
	platform_driver_register(&vc4_crtc_driver);
}

void __exit vc4_crtc_unregister(void)
{
	platform_driver_unregister(&vc4_crtc_driver);
}
