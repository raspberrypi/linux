/*
 * Copyright (C) 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "drmP.h"
#include "drm_gem_cma_helper.h"

struct vc4_dev {
	struct drm_device *dev;

	struct device_node *firmware_node;

	struct vc4_hdmi *hdmi;
	struct vc4_hvs *hvs;
	struct vc4_crtc *crtc[3];
	struct vc4_v3d *v3d;
};

static inline struct vc4_dev *
to_vc4_dev(struct drm_device *dev)
{
	return (struct vc4_dev *)dev->dev_private;
}

struct vc4_bo {
	struct drm_gem_cma_object base;
};

static inline struct vc4_bo *
to_vc4_bo(struct drm_gem_object *bo)
{
	return (struct vc4_bo *)bo;
}

struct vc4_v3d {
	struct platform_device *pdev;
	void __iomem *regs;
};

struct vc4_hvs {
	struct platform_device *pdev;
	void __iomem *regs;
	void __iomem *dlist;
};

struct vc4_crtc {
	struct drm_crtc base;
	void __iomem *regs;

	u32 displist_reg;

	/*
	 * Pointer to the actual hardware display list memory for the
	 * crtc.
	 */
	u32 __iomem *dlist;

	u32 dlist_size; /* in dwords */
};

static inline struct vc4_crtc *
to_vc4_crtc(struct drm_crtc *crtc)
{
	return (struct vc4_crtc *)crtc;
}

struct vc4_plane {
	struct drm_plane base;
};

static inline struct vc4_plane *
to_vc4_plane(struct drm_plane *plane)
{
	return (struct vc4_plane *)plane;
}

#define V3D_READ(offset) readl(vc4->v3d->regs + offset)
#define V3D_WRITE(offset, val) writel(val, vc4->v3d->regs + offset)
#define HVS_READ(offset) readl(vc4->hvs->regs + offset)
#define HVS_WRITE(offset, val) writel(val, vc4->hvs->regs + offset)

/* vc4_bo.c */
void vc4_free_object(struct drm_gem_object *gem_obj);
struct vc4_bo *vc4_bo_create(struct drm_device *dev, size_t size);
int vc4_dumb_create(struct drm_file *file_priv,
		    struct drm_device *dev,
		    struct drm_mode_create_dumb *args);
struct dma_buf *vc4_prime_export(struct drm_device *dev,
				 struct drm_gem_object *obj, int flags);

/* vc4_crtc.c */
void vc4_crtc_register(void);
void vc4_crtc_unregister(void);
int vc4_enable_vblank(struct drm_device *dev, int crtc_id);
void vc4_disable_vblank(struct drm_device *dev, int crtc_id);

/* vc4_debugfs.c */
int vc4_debugfs_init(struct drm_minor *minor);
void vc4_debugfs_cleanup(struct drm_minor *minor);

/* vc4_drv.c */
void __iomem *vc4_ioremap_regs(struct platform_device *dev, int index);

/* vc4_hdmi.c */
void vc4_hdmi_register(void);
void vc4_hdmi_unregister(void);
struct drm_encoder *vc4_hdmi_encoder_init(struct drm_device *dev);
struct drm_connector *vc4_hdmi_connector_init(struct drm_device *dev,
					      struct drm_encoder *encoder);
int vc4_hdmi_debugfs_regs(struct seq_file *m, void *unused);

/* vc4_hvs.c */
void vc4_hvs_register(void);
void vc4_hvs_unregister(void);
void vc4_hvs_dump_state(struct drm_device *dev);
int vc4_hvs_debugfs_regs(struct seq_file *m, void *unused);

/* vc4_kms.c */
int vc4_kms_load(struct drm_device *dev);

/* vc4_plane.c */
struct drm_plane *vc4_plane_init(struct drm_device *dev,
				 enum drm_plane_type type);
u32 vc4_plane_write_dlist(struct drm_plane *plane, u32 __iomem *dlist);
u32 vc4_plane_dlist_size(struct drm_plane_state *state);

/* vc4_v3d.c */
void vc4_v3d_register(void);
void vc4_v3d_unregister(void);
int vc4_v3d_debugfs_ident(struct seq_file *m, void *unused);
int vc4_v3d_debugfs_regs(struct seq_file *m, void *unused);
int vc4_v3d_set_power(struct vc4_dev *vc4, bool on);
