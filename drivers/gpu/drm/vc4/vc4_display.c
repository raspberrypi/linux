/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* This is a limited implementation of KMS by talking to the blob
 * running on the VPU to get the video modes set.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/io.h>
#include <mach/vcio.h>

#include "drm_crtc.h"
#include "drm_crtc_helper.h"
#include "drm_gem_cma_helper.h"
#include "drm_fb_cma_helper.h"
#include "vc4_drv.h"
#include "vc4_display.h"
#include "vc4_regs.h"

struct vc4_mode_set_cmd {
	u32 xres, yres, xres_virtual, yres_virtual;
	u32 pitch; /* in bytes */
	u32 bpp;
	u32 xoffset, yoffset;
	u32 base;
	u32 screen_size;
	u16 cmap[256];
};

static enum drm_connector_status
vc4_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static void
vc4_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
	kfree(connector);
}

struct drm_connector_funcs vc4_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.detect = vc4_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = vc4_connector_destroy,
};

static int
vc4_connector_mode_valid(struct drm_connector *connector,
			 struct drm_display_mode *mode)
{
	return 0;
}

static struct drm_encoder *
vc4_connector_best_encoder(struct drm_connector *connector)
{
	return vc4_attached_encoder(connector);
}

static int
vc4_connector_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;

	/* XXX This is the resolution that the firmware is
	 * detecting for the monitor on my desk.
	 */
	mode = drm_gtf_mode(dev, 1680, 1050, 60, false, false);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_connector_helper_funcs vc4_connector_helper_funcs = {
	.get_modes          = vc4_connector_get_modes,
	.mode_valid         = vc4_connector_mode_valid,
	.best_encoder       = vc4_connector_best_encoder,
};

static struct drm_connector *
vc4_connector_init(struct drm_device *dev, struct drm_encoder *encoder)
{
	struct drm_connector *connector;
	struct vc4_connector *vc4_connector;

	vc4_connector = kzalloc(sizeof(struct vc4_connector), GFP_KERNEL);
	if (!vc4_connector)
		return NULL;

	connector = &vc4_connector->base;
	vc4_connector->encoder = encoder;

	drm_connector_init(dev, connector,
			   &vc4_connector_funcs, DRM_MODE_CONNECTOR_VGA);

	drm_connector_helper_add(connector, &vc4_connector_helper_funcs);

	drm_sysfs_connector_add(connector);

	return connector;
}

static void
vc4_crtc_destroy(struct drm_crtc *crtc)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);

	drm_crtc_cleanup(crtc);
	kfree(vc4_crtc);
}

/* These provide the minimum set of functions required to handle a CRTC */
static const struct drm_crtc_funcs vc4_crtc_funcs = {
	.cursor_set = NULL,
	.cursor_move = NULL,
	.gamma_set = NULL,
	.set_config = drm_crtc_helper_set_config,
	.destroy = vc4_crtc_destroy,
};

static void
vc4_crtc_load_lut(struct drm_crtc *crtc)
{
	/* XXX: unimplemented */
}

static void
vc4_crtc_disable(struct drm_crtc *crtc)
{
	/* XXX: unimplemented */
}

#define HVS_REG(reg) { reg, #reg }
static const struct {
	uint32_t reg;
	const char *name;
} hvs_regs[] = {
	HVS_REG(SCALER_DISPCTRL),
	HVS_REG(SCALER_DISPSTAT),
	HVS_REG(SCALER_DISPID),
	HVS_REG(SCALER_DISPECTRL),
	HVS_REG(SCALER_DISPPROF),
	HVS_REG(SCALER_DISPDITHER),
	HVS_REG(SCALER_DISPEOLN),
	HVS_REG(SCALER_DISPLIST0),
	HVS_REG(SCALER_DISPLIST1),
	HVS_REG(SCALER_DISPLIST2),
	HVS_REG(SCALER_DISPLSTAT),
	HVS_REG(SCALER_DISPLACT0),
	HVS_REG(SCALER_DISPLACT1),
	HVS_REG(SCALER_DISPLACT2),
	HVS_REG(SCALER_DISPCTRL0),
	HVS_REG(SCALER_DISPBKGND0),
	HVS_REG(SCALER_DISPSTAT0),
	HVS_REG(SCALER_DISPBASE0),
	HVS_REG(SCALER_DISPCTRL1),
	HVS_REG(SCALER_DISPBKGND1),
	HVS_REG(SCALER_DISPSTAT1),
	HVS_REG(SCALER_DISPBASE1),
	HVS_REG(SCALER_DISPCTRL2),
	HVS_REG(SCALER_DISPBKGND2),
	HVS_REG(SCALER_DISPSTAT2),
	HVS_REG(SCALER_DISPBASE2),
	HVS_REG(SCALER_DISPALPHA2),
};

#if 0
static void
dump_hvs(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int i;

	rmb();
	for (i = 0; i < ARRAY_SIZE(hvs_regs); i++) {
		DRM_INFO("0x%04x (%s): 0x%08x\n",
			 hvs_regs[i].reg, hvs_regs[i].name,
			 HVS_READ(hvs_regs[i].reg));
	}

	DRM_INFO("HVS ctx:\n");
	for (i = 0; i < 64; i += 4) {
		DRM_INFO("0x%08x (%s): 0x%08x 0x%08x 0x%08x 0x%08x\n",
			 i * 4, i < 32 ? "B" : "D",
			 ((uint32_t *)vc4->hvs_ctx)[i + 0],
			 ((uint32_t *)vc4->hvs_ctx)[i + 1],
			 ((uint32_t *)vc4->hvs_ctx)[i + 2],
			 ((uint32_t *)vc4->hvs_ctx)[i + 3]);
	}
}
#endif

static int
vc4_crtc_mode_set(struct drm_crtc *crtc,
		  struct drm_display_mode *mode,
		  struct drm_display_mode *adjusted_mode,
		  int x, int y, struct drm_framebuffer *old_fb)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	volatile struct vc4_mode_set_cmd *set = vc4->mode_set_cmd;
	struct drm_framebuffer *fb = crtc->primary->fb;
	struct drm_gem_cma_object *bo = drm_fb_cma_get_gem_obj(fb, 0);
	uint32_t val;
	uint32_t *dlist = (uint32_t *)vc4->hvs_ctx + HVS_BOOTLOADER_DLIST_END;
	uint32_t dlist_count = 0;

	set->xres = mode->hdisplay;
	set->yres = mode->vdisplay;
	set->xres_virtual = mode->hdisplay;
	set->yres_virtual = mode->vdisplay;
	set->bpp = fb->bits_per_pixel;
	set->xoffset = 0;
	set->yoffset = 0;
	set->base = 0;
	set->pitch = crtc->primary->fb->pitches[0];

#if 0
	DRM_INFO("HVS regs before:\n");
	dump_hvs(dev);
#endif

	wmb();
	bcm_mailbox_write(MBOX_CHAN_FB, vc4->mode_set_cmd_addr);
	bcm_mailbox_read(MBOX_CHAN_FB, &val);
	rmb();

#if 0
	DRM_INFO("HVS regs after modeset:\n");
	dump_hvs(dev);
#endif

	dlist[dlist_count++] =
		(SCALER_CTL0_VALID |
		 (HVS_PIXEL_ORDER_ABGR << SCALER_CTL0_ORDER_SHIFT) |
		 (HVS_PIXEL_FORMAT_RGB8888 << SCALER_CTL0_PIXEL_FORMAT_SHIFT) |
		 SCALER_CTL0_UNITY);

	dlist[dlist_count++] = 0xFF << SCALER_POS0_ALPHA_SHIFT;

	dlist[dlist_count++] =
		((1 << SCALER_POS2_ALPHA_MODE_SHIFT) |
		 (mode->vdisplay << SCALER_POS2_HEIGHT_SHIFT) |
		 (mode->hdisplay << SCALER_POS2_WIDTH_SHIFT));

	dlist[dlist_count++] = 0xc0c0c0c0;
	dlist[dlist_count++] = bo->paddr + fb->offsets[0];
	dlist[dlist_count++] = 0xc0c0c0c0;
	dlist[dlist_count++] = (crtc->primary->fb->pitches[0] &
				SCALER_SRC_PITCH_MASK);
	dlist[0] |= dlist_count << SCALER_CTL0_SIZE_SHIFT;
	dlist[dlist_count++] = SCALER_CTL0_END;
	wmb();

	HVS_WRITE(SCALER_DISPLIST1, HVS_BOOTLOADER_DLIST_END);

#if 0
	DRM_INFO("HVS regs after submit:\n");
	dump_hvs(dev);
#endif

	return 0;
}

static void
vc4_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	/* XXX: unimplemented */
}

static bool
vc4_crtc_mode_fixup(struct drm_crtc *crtc,
		    const struct drm_display_mode *mode,
		    struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void
vc4_crtc_prepare(struct drm_crtc *crtc)
{
	/* XXX: unimplemented */
}

static void
vc4_crtc_commit(struct drm_crtc *crtc)
{
	/* XXX: unimplemented */
}

static const struct drm_crtc_helper_funcs vc4_crtc_helper_funcs = {
	.disable = vc4_crtc_disable,
	.dpms = vc4_crtc_dpms,
	.mode_fixup = vc4_crtc_mode_fixup,
	.mode_set = vc4_crtc_mode_set,
	.mode_set_base = NULL,
	.prepare = vc4_crtc_prepare,
	.commit = vc4_crtc_commit,
	.load_lut = vc4_crtc_load_lut,
};

struct drm_crtc *
vc4_crtc_init(struct drm_device *dev)
{
	struct vc4_crtc *vc4_crtc;
	struct drm_crtc *crtc;
	int ret;

	vc4_crtc = kzalloc(sizeof(*vc4_crtc), GFP_KERNEL);
	if (!vc4_crtc) {
		DRM_ERROR("kzalloc\n");
		return NULL;
	}

	crtc = &vc4_crtc->base;

	ret = drm_crtc_init(dev, crtc, &vc4_crtc_funcs);
	if (ret < 0) {
		kfree(vc4_crtc);
		return NULL;
	}

	drm_crtc_helper_add(crtc, &vc4_crtc_helper_funcs);

	return crtc;
}

static const struct drm_mode_config_funcs vc4_mode_funcs = {
	.fb_create = drm_fb_cma_create,
};

static void
vc4_hvs_init(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int i;

	/* Clear out the non-bootloader dsiplay list contents. */
	for (i = HVS_BOOTLOADER_DLIST_END; i < vc4->hvs_ctx_size / 4; i++)
		writel(SCALER_CTL0_END, (uint32_t *)vc4->hvs_ctx + i);
}

int
vc4_modeset_init(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_encoder *encoder;
	struct drm_connector *connector;

	vc4->mode_set_cmd =
		dma_alloc_coherent(NULL,
				   PAGE_ALIGN(sizeof(struct vc4_mode_set_cmd)),
				   &vc4->mode_set_cmd_addr,
				   GFP_KERNEL);
	if (!vc4->mode_set_cmd)
		return -ENOMEM;

	drm_mode_config_init(dev);

	dev->mode_config.funcs = &vc4_mode_funcs;
	dev->mode_config.preferred_depth = 24;

	/* XXX: fill in limits */
	dev->mode_config.max_width = 2048;
	dev->mode_config.max_height = 2048;

	vc4_crtc_init(dev);

	encoder = drm_stub_encoder_init(dev);
	if (!encoder) {
		DRM_ERROR("drm_stub_encoder_init failed\n");
		return -1;
	}

	connector = vc4_connector_init(dev, encoder);
	if (!connector) {
		DRM_ERROR("vc4_connector_init failed\n");
		return -1;
	}

	vc4_hvs_init(dev);

	drm_mode_connector_attach_encoder(connector, encoder);

	drm_kms_helper_poll_init(dev);

	drm_fbdev_cma_init(dev, 32);

	return 0;
}

