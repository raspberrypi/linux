// SPDX-License-Identifier: GPL-2.0-only
/*
 * PIO code for Raspberry Pi RP1 DPI driver
 *
 * Copyright (c) 2024 Raspberry Pi Limited.
 */

/*
 * Use PIO to fix up VSYNC for interlaced modes.
 *
 * For this to work we *require* DPI's pinctrl to enable DE on GPIO1.
 * PIO can then snoop on HSYNC and DE pins to generate corrected VSYNC.
 *
 * Note that corrected VSYNC outputs will not be synchronous to DPICLK,
 * will lag HSYNC by about 30ns and may suffer up to 5ns of jitter.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <drm/drm_print.h>

#include "rp1_dpi.h"

#if IS_REACHABLE(CONFIG_RP1_PIO)

#include <linux/pio_rp1.h>

/*
 * Start a PIO SM to generate an interrupt just after HSYNC onset, then another
 * after a fixed delay (during which we assume HSYNC will have been deasserted).
 */

static int rp1dpi_pio_start_timer_both(struct rp1_dpi *dpi, u32 flags, u32 tc)
{
	static const u16 instructions[2][5] = {
		{ 0xa022, 0x2083, 0xc001, 0x0043, 0xc001 }, /* posedge */
		{ 0xa022, 0x2003, 0xc001, 0x0043, 0xc001 }, /* negedge */
	};
	const struct pio_program prog = {
		.instructions = instructions[(flags & DRM_MODE_FLAG_NHSYNC) ? 1 : 0],
		.length = ARRAY_SIZE(instructions[0]),
		.origin = -1
	};
	int offset, sm;

	sm = pio_claim_unused_sm(dpi->pio, true);
	if (sm < 0)
		return -EBUSY;

	offset = pio_add_program(dpi->pio, &prog);
	if (offset == PIO_ORIGIN_ANY)
		return -EBUSY;

	pio_sm_config cfg = pio_get_default_sm_config();

	pio_sm_set_enabled(dpi->pio, sm, false);
	sm_config_set_wrap(&cfg, offset, offset + 4);
	pio_sm_init(dpi->pio, sm, offset, &cfg);

	pio_sm_put(dpi->pio, sm, tc - 4);
	pio_sm_exec(dpi->pio, sm, pio_encode_pull(false, false));
	pio_sm_exec(dpi->pio, sm, pio_encode_out(pio_y, 32));
	pio_sm_set_enabled(dpi->pio, sm, true);

	return 0;
}

/*
 * Snoop on DE, HSYNC to count half-lines in the vertical blanking interval
 * to determine when the VSYNC pulse should start and finish. Then, at a
 * suitable moment (which should be an odd number of half-lines since the
 * last active line), sample DE again to detect field phase.
 *
 * This version assumes VFP length is within 2..129 half-lines for any field
 * (one half-line delay is needed to sample DE; we always wait for the next
 * half-line boundary to improve VSync start accuracy).
 */

static int rp1dpi_pio_vsync_ilace(struct rp1_dpi *dpi,
				  struct drm_display_mode const *mode)
{
	static const int wrap_target = 14;
	static const int wrap = 26;
	u16 instructions[] = {  /* This is mutable */
		0xa0e6, //  0: mov    osr, isr    side 0     ; top: rewind parameters
		0x2081, //  1: wait   1 gpio, 1   side 0     ; main: while (!DE) wait;
		0x2783, //  2: wait   1 gpio, 3   side 0 [7] ;  do { @HSync
		0xc041, //  3: irq    clear 1     side 0     ;   flush stale IRQs
		0x20c1, //  4: wait   1 irq, 1    side 0     ;   @midline
		0x00c1, //  5: jmp    pin, 1      side 0     ;  } while (DE)
		0x0007, //  6: jmp    7           side 0     ;  <modify for -DE fixup>
		0x6027, //  7: out    x, 7        side 0     ;  x = VFPlen - 2
		0x000a, //  8: jmp    10          side 0     ;  while (x--) {
		0x20c1, //  9: wait   1 irq, 1    side 0     ;    @halfline
		0x0049, // 10: jmp    x--, 9      side 0     ;  }
		0x6021, // 11: out    x, 1        side 0     ;  test for aligned case
		0x003a, // 12: jmp    !x, 26      side 0     ;  if (!x) goto precise;
		0x20c1, // 13: wait   1 irq, 1    side 0     ;  @halfline
		//     .wrap_target                          ; vsjoin:
		0xb722, // 14: mov    x, y        side 1 [7] ;  VSYNC=1; x = VSyncLen
		0xd041, // 15: irq    clear 1     side 1     ;  VSYNC=1; flush stale IRQs
		0x30c1, // 16: wait   1 irq, 1    side 1     ;  VSYNC=1; do { @halfline
		0x1050, // 17: jmp    x--, 16     side 1     ;  VSYNC=1; } while (x--)
		0x6028, // 18: out    x, 8        side 0     ;  VSYNC=0; x = VBPLen
		0x0015, // 19: jmp    21          side 0     ;  while (x--) {
		0x20c1, // 20: wait   1 irq, 1    side 0     ;    @halfline
		0x0054, // 21: jmp    x--, 20     side 0     ;  }
		0x00c0, // 22: jmp    pin, 0      side 0     ;  if (DE) reset phase
		0x0018, // 23: jmp    24          side 0     ;  <modify for -DE fixup>
		0x00e1, // 24: jmp    !osre, 1    side 0     ;  if (!phase) goto main
		0x0000, // 25: jmp    0           side 0     ;  goto top
		0x2083, // 26: wait   1 gpio, 3   side 0     ; precise: @HSync
		//     .wrap                                 ;  goto vsjoin
	};
	struct pio_program prog = {
		.instructions = instructions,
		.length = ARRAY_SIZE(instructions),
		.origin = -1
	};
	pio_sm_config cfg = pio_get_default_sm_config();
	unsigned int i, offset;
	u32 tc, vfp, vbp;
	u32 sysclk = clock_get_hz(clk_sys);
	int sm = pio_claim_unused_sm(dpi->pio, true);

	if (sm < 0)
		return -EBUSY;

	/* Compute mid-line time constant and start the timer SM */
	tc = (mode->htotal * (u64)sysclk) / (u64)(2000u * mode->clock);
	if (rp1dpi_pio_start_timer_both(dpi, mode->flags, tc) < 0) {
		pio_sm_unclaim(dpi->pio, sm);
		return -EBUSY;
	}

	/* Adapt program code according to DE and Sync polarity; configure program */
	pio_sm_set_enabled(dpi->pio, sm, false);
	if (dpi->de_inv) {
		instructions[1] ^= 0x0080;
		instructions[5]  = 0x00c7;
		instructions[6]  = 0x0001;
		instructions[22] = 0x00d8;
		instructions[23] = 0x0000;
	}
	for (i = 0; i < ARRAY_SIZE(instructions); i++) {
		if (mode->flags & DRM_MODE_FLAG_NVSYNC)
			instructions[i] ^= 0x1000;
		if ((mode->flags & DRM_MODE_FLAG_NHSYNC) && (instructions[i] & 0xe07f) == 0x2003)
			instructions[i] ^= 0x0080;
	}
	offset = pio_add_program(dpi->pio, &prog);
	if (offset == PIO_ORIGIN_ANY)
		return -EBUSY;

	/* Configure pins and SM */
	dpi->pio_stole_gpio2 = true;
	sm_config_set_wrap(&cfg, offset + wrap_target, offset + wrap);
	sm_config_set_sideset(&cfg, 1, false, false);
	sm_config_set_sideset_pins(&cfg, 2);
	pio_gpio_init(dpi->pio, 2);
	sm_config_set_jmp_pin(&cfg, 1); /* "DE" is always GPIO1 */
	pio_sm_init(dpi->pio, sm, offset, &cfg);
	pio_sm_set_consecutive_pindirs(dpi->pio, sm, 2, 1, true);

	/* Compute vertical times, remembering how we rounded vdisplay, vtotal */
	vfp = mode->vsync_start - (mode->vdisplay & ~1);
	vbp = (mode->vtotal | 1) - mode->vsync_end;
	if (vfp > 128) {
		vbp += vfp - 128;
		vfp = 128;
	} else if (vfp < 3) {
		vbp = (vbp > 3 - vfp) ? (vbp - 3 + vfp) : 0;
		vfp = 3;
	}

	pio_sm_put(dpi->pio, sm,
		   (vfp - 2) + ((vfp & 1) << 7) + (vbp << 8) +
		   ((vfp - 3) << 16) + (((~vfp) & 1) << 23) + ((vbp + 1) << 24));
	pio_sm_put(dpi->pio, sm, mode->vsync_end - mode->vsync_start - 1);
	pio_sm_exec(dpi->pio, sm, pio_encode_pull(false, false));
	pio_sm_exec(dpi->pio, sm, pio_encode_out(pio_y, 32));
	pio_sm_exec(dpi->pio, sm, pio_encode_in(pio_y, 32));
	pio_sm_exec(dpi->pio, sm, pio_encode_pull(false, false));
	pio_sm_exec(dpi->pio, sm, pio_encode_out(pio_y, 32));
	pio_sm_set_enabled(dpi->pio, sm, true);

	return 0;
}

int rp1dpi_pio_start(struct rp1_dpi *dpi, const struct drm_display_mode *mode)
{
	int r;

	if (!(mode->flags & DRM_MODE_FLAG_INTERLACE) || !dpi->gpio1_used)
		return 0;

	if (dpi->pio)
		pio_close(dpi->pio);

	dpi->pio = pio_open();
	if (IS_ERR(dpi->pio)) {
		drm_err(&dpi->drm, "Could not open PIO\n");
		dpi->pio = NULL;
		return -ENODEV;
	}

	r = rp1dpi_pio_vsync_ilace(dpi, mode);
	if (r) {
		drm_err(&dpi->drm, "Failed to initialize PIO\n");
		rp1dpi_pio_stop(dpi);
	}

	return r;
}

void rp1dpi_pio_stop(struct rp1_dpi *dpi)
{
	if (dpi->pio) {
		if (dpi->pio_stole_gpio2)
			pio_gpio_set_function(dpi->pio, 2, GPIO_FUNC_FSEL1);
		pio_close(dpi->pio);
		dpi->pio_stole_gpio2 = false;
		dpi->pio = NULL;
	}
}

#else /* !IS_REACHABLE(CONFIG_RP1_PIO) */

int rp1dpi_pio_start(struct rp1_dpi *dpi, const struct drm_display_mode *mode)
{
	return -ENODEV;
}

void rp1dpi_pio_stop(struct rp1_dpi *dpi)
{
}

#endif
