/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DRM Driver for DSI output on Raspberry Pi RP1
 *
 * Copyright (c) 2023 Raspberry Pi Limited.
 */
#ifndef _RP1_DSI_H_
#define _RP1_DSI_H_

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/types.h>

#include <drm/drm_bridge.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_simple_kms_helper.h>

#define MODULE_NAME "drm-rp1-dsi"
#define DRIVER_NAME "drm-rp1-dsi"

/* ---------------------------------------------------------------------- */

#define RP1DSI_HW_BLOCK_DMA   0
#define RP1DSI_HW_BLOCK_DSI   1
#define RP1DSI_HW_BLOCK_CFG   2
#define RP1DSI_NUM_HW_BLOCKS  3

#define RP1DSI_CLOCK_CFG     0
#define RP1DSI_CLOCK_DPI     1
#define RP1DSI_CLOCK_BYTE    2
#define RP1DSI_CLOCK_REF     3
#define RP1DSI_CLOCK_PLLSYS  4
#define RP1DSI_NUM_CLOCKS    5

/* ---------------------------------------------------------------------- */

struct rp1_dsi;

/*
 * DRM state for one attached DSI device. A new DRM device is created each
 * time a DSI device attaches, and it is released when its last user (for
 * example an open file) goes away, which can be after the DSI device has
 * detached and a new one has attached.
 */
struct rp1dsi_kms {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct rp1_dsi *dsi;
};

/* The DSI bridge of one rp1dsi_kms; refcounted, it lives as long as that DRM device uses it */
struct rp1dsi_bridge {
	struct drm_bridge bridge;
	struct rp1_dsi *dsi;
	struct rp1dsi_kms *kms;
	struct drm_bridge *out_bridge;
};

struct rp1_dsi {
	/* Platform device; the state below lives as long as it is bound */
	struct platform_device *pdev;
	int irq;

	/* DRM state of the attached DSI device, NULL when none is attached */
	struct rp1dsi_kms *kms;
	struct rp1dsi_bridge *bridge;
	struct drm_bridge *panel_bridge;
	bool cfg_clk_on;

	struct mipi_dsi_host dsi_host;

	/* Clocks. We need DPI clock; the others are frequency references */
	struct clk *clocks[RP1DSI_NUM_CLOCKS];

	/* Device tree parsed information */
	u32 lane_polarities[5];

	/* Block (DSI DMA, DSI Host) base addresses, and current state */
	void __iomem *hw_base[RP1DSI_NUM_HW_BLOCKS];
	u32 cur_fmt;
	bool dsi_running, dma_running, pipe_enabled;
	struct completion finished;

	/* Attached display parameters (from mipi_dsi_device) */
	unsigned long display_flags, display_hs_rate, display_lp_rate;
	enum mipi_dsi_pixel_format display_format;
	u8 vc;
	u8 lanes;

	/* DPHY */
	u8 hsfreq_index;
};

/* ---------------------------------------------------------------------- */
/* Functions to control the DSI/DPI/DMA block				  */

void rp1dsi_dma_setup(struct rp1_dsi *dsi,
		      u32 in_format, enum mipi_dsi_pixel_format out_format,
		      struct drm_display_mode const *mode);
void rp1dsi_dma_update(struct rp1_dsi *dsi, dma_addr_t addr, u32 offset, u32 stride);
void rp1dsi_dma_stop(struct rp1_dsi *dsi);
int rp1dsi_dma_busy(struct rp1_dsi *dsi);
irqreturn_t rp1dsi_dma_isr(int irq, void *dev);
void rp1dsi_dma_vblank_ctrl(struct rp1_dsi *dsi, int enable);

/* ---------------------------------------------------------------------- */
/* Functions to control the MIPICFG block and check RP1 platform		  */

void rp1dsi_mipicfg_setup(struct rp1_dsi *dsi);

/* ---------------------------------------------------------------------- */
/* Functions to control the SNPS D-PHY and DSI block setup		  */

void rp1dsi_dsi_setup(struct rp1_dsi *dsi, struct drm_display_mode const *mode);
void rp1dsi_dsi_send(struct rp1_dsi *dsi, u32 header, int len, const u8 *buf,
		bool use_lpm, bool req_ack);
int  rp1dsi_dsi_recv(struct rp1_dsi *dsi, int len, u8 *buf);
void rp1dsi_dsi_set_cmdmode(struct rp1_dsi *dsi, int cmd_mode);
void rp1dsi_dsi_stop(struct rp1_dsi *dsi);

#endif

