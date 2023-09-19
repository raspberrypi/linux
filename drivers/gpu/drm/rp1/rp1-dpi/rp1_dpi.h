/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DRM Driver for DSI output on Raspberry Pi RP1
 *
 * Copyright (c) 2023 Raspberry Pi Limited.
 */

#include <linux/types.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <drm/drm_device.h>
#include <drm/drm_simple_kms_helper.h>

#define MODULE_NAME "drm-rp1-dpi"
#define DRIVER_NAME "drm-rp1-dpi"

/* ---------------------------------------------------------------------- */

#define RP1DPI_HW_BLOCK_DPI   0
#define RP1DPI_HW_BLOCK_CFG   1
#define RP1DPI_NUM_HW_BLOCKS  2

#define RP1DPI_CLK_DPI      0
#define RP1DPI_CLK_PLLDIV   1
#define RP1DPI_CLK_PLLCORE  2
#define RP1DPI_NUM_CLOCKS   3

/* ---------------------------------------------------------------------- */

struct rp1_dpi {
	/* DRM and platform device pointers */
	struct drm_device *drm;
	struct platform_device *pdev;

	/* Framework and helper objects */
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	/* Clocks: Video PLL, its primary divider, and DPI clock. */
	struct clk *clocks[RP1DPI_NUM_CLOCKS];

	/* Block (DPI, VOCFG) base addresses, and current state */
	void __iomem *hw_base[RP1DPI_NUM_HW_BLOCKS];
	u32 cur_fmt;
	u32 bus_fmt;
	bool de_inv, clk_inv;
	bool dpi_running, pipe_enabled;
	struct completion finished;
};

/* ---------------------------------------------------------------------- */
/* Functions to control the DPI/DMA block				  */

void rp1dpi_hw_setup(struct rp1_dpi *dpi,
		     u32 in_format,
		     u32 bus_format,
		     bool de_inv,
		     struct drm_display_mode const *mode);
void rp1dpi_hw_update(struct rp1_dpi *dpi, dma_addr_t addr, u32 offset, u32 stride);
void rp1dpi_hw_stop(struct rp1_dpi *dpi);
int rp1dpi_hw_busy(struct rp1_dpi *dpi);
irqreturn_t rp1dpi_hw_isr(int irq, void *dev);
void rp1dpi_hw_vblank_ctrl(struct rp1_dpi *dpi, int enable);

/* ---------------------------------------------------------------------- */
/* Functions to control the VIDEO OUT CFG block and check RP1 platform	  */

void rp1dpi_vidout_setup(struct rp1_dpi *dpi, bool drive_negedge);
void rp1dpi_vidout_poweroff(struct rp1_dpi *dpi);
