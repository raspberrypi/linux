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

#define MODULE_NAME "drm-rp1-vec"
#define DRIVER_NAME "drm-rp1-vec"

/* ---------------------------------------------------------------------- */

#define RP1VEC_HW_BLOCK_VEC   0
#define RP1VEC_HW_BLOCK_CFG   1
#define RP1VEC_NUM_HW_BLOCKS  2

enum {
	RP1VEC_TVSTD_NTSC = 0,	/* +525 => NTSC       625 => PAL   */
	RP1VEC_TVSTD_NTSC_J,	/* +525 => NTSC-J     625 => PAL   */
	RP1VEC_TVSTD_NTSC_443,	/* +525 => NTSC-443  +625 => PAL   */
	RP1VEC_TVSTD_PAL,	/*  525 => NTSC      +625 => PAL   */
	RP1VEC_TVSTD_PAL_M,	/* +525 => PAL-M      625 => PAL   */
	RP1VEC_TVSTD_PAL_N,	/*  525 => NTSC      +625 => PAL-N */
	RP1VEC_TVSTD_PAL60,	/* +525 => PAL60     +625 => PAL   */
	RP1VEC_TVSTD_DEFAULT,	/* +525 => NTSC      +625 => PAL   */
};

/* Which standards support which modes? Those marked with + above */
#define RP1VEC_TVSTD_SUPPORT_525(n) ((0xD7 >> (n)) & 1)
#define RP1VEC_TVSTD_SUPPORT_625(n) ((0xEC >> (n)) & 1)

/* ---------------------------------------------------------------------- */

struct rp1_vec {
	/* DRM and platform device pointers */
	struct drm_device *drm;
	struct platform_device *pdev;

	/* Framework and helper objects */
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	/* Clock. We assume this is always at 108 MHz. */
	struct clk *vec_clock;

	/* Block (VCC, CFG) base addresses, and current state */
	void __iomem *hw_base[RP1VEC_NUM_HW_BLOCKS];
	u32 cur_fmt;
	int tv_norm;
	bool vec_running, pipe_enabled;
	struct completion finished;
};

extern const char * const rp1vec_tvstd_names[];

/* ---------------------------------------------------------------------- */
/* Functions to control the VEC/DMA block				  */

void rp1vec_hw_setup(struct rp1_vec *vec,
		     u32 in_format,
		struct drm_display_mode const *mode,
		int tvstd);
void rp1vec_hw_update(struct rp1_vec *vec, dma_addr_t addr, u32 offset, u32 stride);
void rp1vec_hw_stop(struct rp1_vec *vec);
int rp1vec_hw_busy(struct rp1_vec *vec);
irqreturn_t rp1vec_hw_isr(int irq, void *dev);
void rp1vec_hw_vblank_ctrl(struct rp1_vec *vec, int enable);

/* ---------------------------------------------------------------------- */
/* Functions to control the VIDEO OUT CFG block and check RP1 platform	  */

void rp1vec_vidout_setup(struct rp1_vec *vec);
void rp1vec_vidout_poweroff(struct rp1_vec *vec);
