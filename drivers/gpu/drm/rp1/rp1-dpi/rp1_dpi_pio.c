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
 * Start a PIO SM to generate two interrupts for each horizontal line.
 * The first occurs shortly before the middle of the line. The second
 * is timed such that after receiving the IRQ plus 1 extra delay cycle,
 * another SM's output will align with the next HSYNC within -5ns .. +10ns.
 * To achieve this, we need an accurate measure of (cycles per line) / 2.
 *
 * Measured GPIO -> { wait gpio ; irq set | irq wait ; sideset } -> GPIO
 * round-trip delay is about 8 cycles when pins are not heavily loaded.
 *
 * PIO code           ; Notional time % 1000-cycle period
 * --------           ; ---------------------------------
 * 0: wait 1 gpio 3   ;   0..  8
 * 1: mov x, y        ;   8..  9
 * 2: jmp x--, 2      ;   9..499    (Y should be T/2 - 11)
 * 3: irq set 1       ; 499..500
 * 4: mov x, y    [8] ; 500..509
 * 5: jmp x--, 5      ; 509..999
 * 6: irq set 1       ; 999..1000
 */

static int rp1dpi_pio_start_timer_both(struct rp1_dpi *dpi, u32 flags, u32 tc)
{
	static const u16 instructions[2][7] = {
		{ 0x2083, 0xa022, 0x0042, 0xc001, 0xa822, 0x0045, 0xc001 }, /* +H */
		{ 0x2003, 0xa022, 0x0042, 0xc001, 0xa822, 0x0045, 0xc001 }, /* -H */
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
	if (offset == PIO_ORIGIN_ANY) {
		pio_sm_unclaim(dpi->pio, sm);
		return -EBUSY;
	}

	pio_sm_config cfg = pio_get_default_sm_config();

	pio_sm_set_enabled(dpi->pio, sm, false);
	sm_config_set_wrap(&cfg, offset, offset + 6);
	pio_sm_init(dpi->pio, sm, offset, &cfg);

	pio_sm_put(dpi->pio, sm, tc - 11);
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
 * This version assumes VFP length is within 2..256 half-lines for any field
 * (one half-line delay is needed to sample DE; we always wait for the next
 * half-line boundary to improve VSync start accuracy) and VBP in 1..255.
 */

static int rp1dpi_pio_vsync_ilace(struct rp1_dpi *dpi,
				  struct drm_display_mode const *mode)
{
	u16 instructions[] = {  /* This is mutable */
		//      .wrap_target
		0xa0e6, //  0: mov    osr, isr    side 0     ; top: rewind parameters
		0x2081, //  1: wait   1 gpio, 1   side 0     ; main: while (!DE) wait;
		0x2783, //  2: wait   1 gpio, 3   side 0 [7] ;  do { @HSync
		0xc041, //  3: irq    clear 1     side 0     ;   flush stale IRQs
		0x20c1, //  4: wait   1 irq, 1    side 0     ;   @midline
		0x00c2, //  5: jmp    pin, 2      side 0     ;  } while (DE)
		0x0007, //  6: jmp    7           side 0     ;  <modify for -DE fixup>
		0x6028, //  7: out    x, 8        side 0     ;  x = VFPlen - 2
		0x20c1, //  8: wait   1 irq, 1    side 0     ;  do { @halfline
		0x0048, //  9: jmp    x--, 8      side 0     ;  } while (x--)
		0xb022, // 10: mov    x, y        side 1     ;  VSYNC=1; x = VSyncLen
		0x30c1, // 11: wait   1 irq, 1    side 1     ;  VSYNC=1; do { @halfline
		0x104b, // 12: jmp    x--, 11     side 1     ;  VSYNC=1; } while (x--)
		0x6028, // 13: out    x, 8        side 0     ;  VSYNC=0; x = VBPLen - 1
		0x20c1, // 14: wait   1 irq, 1    side 0     ;  do { @halfline
		0x004e, // 15: jmp    x--, 14     side 0     ;  } while (x--)
		0x00c0, // 16: jmp    pin, 0      side 0     ;  if (DE) reset phase
		0x0012, // 17: jmp    18          side 0     ;  <modify for -DE fixup>
		0x00e1, // 18: jmp    !osre, 1    side 0     ;  if (!phase) goto main
		//     .wrap                                 ;  goto top
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

	/*
	 * Compute half-line time constant (round uppish so that VSync should
	 * switch never > 5ns before DPICLK, while defeating roundoff errors)
	 * and start the timer SM.
	 */
	tc = (u32)clk_get_rate(dpi->clocks[RP1DPI_CLK_DPI]);
	if (!tc)
		tc = 1000u * mode->clock;
	tc = ((u64)mode->htotal * (u64)sysclk + ((7ul * tc) >> 2)) /
		(u64)(2ul * tc);
	if (rp1dpi_pio_start_timer_both(dpi, mode->flags, tc) < 0) {
		pio_sm_unclaim(dpi->pio, sm);
		return -EBUSY;
	}

	/* Adapt program code according to DE and Sync polarity; configure program */
	pio_sm_set_enabled(dpi->pio, sm, false);
	if (dpi->de_inv) {
		instructions[1] ^= 0x0080;
		instructions[5]  = 0x00c7;
		instructions[6]  = 0x0002;
		instructions[16] = 0x00d2;
		instructions[17] = 0x0000;
	}
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		instructions[2] ^= 0x0080;
	if (mode->flags & DRM_MODE_FLAG_NVSYNC) {
		for (i = 0; i < ARRAY_SIZE(instructions); i++)
			instructions[i] ^= 0x1000;
	}
	offset = pio_add_program(dpi->pio, &prog);
	if (offset == PIO_ORIGIN_ANY)
		return -EBUSY;

	/* Configure pins and SM */
	dpi->pio_stole_gpio2 = true;
	sm_config_set_wrap(&cfg, offset, offset + ARRAY_SIZE(instructions) - 1);
	sm_config_set_sideset(&cfg, 1, false, false);
	sm_config_set_sideset_pins(&cfg, 2);
	pio_gpio_init(dpi->pio, 2);
	sm_config_set_jmp_pin(&cfg, 1); /* "DE" is always GPIO1 */
	pio_sm_init(dpi->pio, sm, offset, &cfg);
	pio_sm_set_consecutive_pindirs(dpi->pio, sm, 2, 1, true);

	/* Compute vertical times, remembering how we rounded vdisplay, vtotal */
	vfp = mode->vsync_start - (mode->vdisplay & ~1);
	vbp = (mode->vtotal | 1) - mode->vsync_end;
	if (vfp > 256) {
		vbp += vfp - 256;
		vfp = 256;
	} else if (vfp < 3) {
		vbp = (vbp > 3 - vfp) ? (vbp - 3 + vfp) : 1;
		vfp = 3;
	}

	pio_sm_put(dpi->pio, sm,
		   (vfp - 2) + ((vbp - 1) << 8) +
		   ((vfp - 3) << 16) + (vbp << 24));
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
