// SPDX-License-Identifier: GPL-2.0-only
/*
 * PIO code for Raspberry Pi RP1 DPI driver
 *
 * Copyright (c) 2024 Raspberry Pi Limited.
 */

/*
 * RP1 DPI can't generate any composite sync, and in interlaced modes
 * its native vertical sync output will be an incorrect/modified signal.
 *
 * So we need to use PIO *either* to generate CSYNC in both progressive
 * and interlaced modes, *or* to fix up VSYNC for interlaced modes only.
 * It can't do both: interlaced modes can have only one of CSYNC, VSYNC.
 * All these cases require GPIOs 1, 2 and 3 to be declared as outputs.
 *
 * Note that PIO's VSYNC or CSYNC output is not synchronous to DPICLK
 * and may suffer up to +/-5ns of jitter.
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
	tc = ((u64)mode->htotal * (u64)clock_get_hz(clk_sys) + ((7ul * tc) >> 2)) /
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
	sm_config_set_wrap(&cfg, offset, offset + ARRAY_SIZE(instructions) - 1);
	sm_config_set_sideset(&cfg, 1, false, false);
	sm_config_set_sideset_pins(&cfg, 2); /* PIO produces VSync on GPIO2 */
	pio_gpio_init(dpi->pio, 2);
	sm_config_set_jmp_pin(&cfg, 1); /* DE on GPIO1 */
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

/*
 * COMPOSITE SYNC FOR PROGRESSIVE
 *
 * Copy HSYNC pulses to CSYNC (adding 1 cycle); then when VSYNC
 * is asserted, extend each pulse by an additional Y + 1 cycles.
 *
 * The following time constant should be written to the FIFO:
 *    (htotal - 2 * hsync_width) * sys_clock / dpi_clock - 2.
 *
 * The default configuration is +HSync, +VSync, -CSync; other
 * polarities can be made by modifying the PIO program code.
 */

static int rp1dpi_pio_csync_prog(struct rp1_dpi *dpi,
				 struct drm_display_mode const *mode)
{
	unsigned int i, tc, offset;
	unsigned short instructions[] = {  /* This is mutable */
		0x90a0, //  0: pull   block      side 1
		0x7040, //  1: out    y, 32      side 1
		//     .wrap_target
		0xb322, //  2: mov    x, y       side 1 [3]
		0x3083, //  3: wait   1 gpio, 3  side 1
		0xa422, //  4: mov    x, y       side 0 [4]
		0x2003, //  5: wait   0 gpio, 3  side 0
		0x00c7, //  6: jmp    pin, 7     side 0    ; modify to flip VSync polarity
		//     .wrap                               ; modify to flip VSync polarity
		0x0047, //  7: jmp    x--, 7     side 0
		0x1002, //  8: jmp    2          side 1
	};
	struct pio_program prog = {
		.instructions = instructions,
		.length = ARRAY_SIZE(instructions),
		.origin = -1
	};
	pio_sm_config cfg = pio_get_default_sm_config();
	int sm = pio_claim_unused_sm(dpi->pio, true);

	if (sm < 0)
		return -EBUSY;

	/* Adapt program code for sync polarity; configure program */
	pio_sm_set_enabled(dpi->pio, sm, false);
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		instructions[6] = 0x00c2; /* jmp pin, 2 side 0 */
	if (mode->flags & DRM_MODE_FLAG_NHSYNC) {
		instructions[3] ^= 0x80;
		instructions[5] ^= 0x80;
	}
	if (mode->flags & DRM_MODE_FLAG_PCSYNC) {
		for (i = 0; i < ARRAY_SIZE(instructions); i++)
			instructions[i] ^= 0x1000;
	}
	offset = pio_add_program(dpi->pio, &prog);
	if (offset == PIO_ORIGIN_ANY)
		return -EBUSY;

	/* Configure pins and SM */
	sm_config_set_wrap(&cfg, offset + 2,
			   offset + (mode->flags & DRM_MODE_FLAG_NVSYNC) ? 7 : 6);
	sm_config_set_sideset(&cfg, 1, false, false);
	sm_config_set_sideset_pins(&cfg, 1); /* PIO produces CSync on GPIO 1 */
	pio_gpio_init(dpi->pio, 1);
	sm_config_set_jmp_pin(&cfg, 2); /* VSync on GPIO 2 */
	pio_sm_init(dpi->pio, sm, offset, &cfg);
	pio_sm_set_consecutive_pindirs(dpi->pio, sm, 1, 1, true);

	/* Place time constant into the FIFO; start the SM */
	tc = (u32)clk_get_rate(dpi->clocks[RP1DPI_CLK_DPI]);
	if (!tc)
		tc = 1000u * mode->clock;
	tc = ((u64)(mode->htotal - 2 * (mode->hsync_end - mode->hsync_start)) *
	      (u64)clock_get_hz(clk_sys)) / (u64)tc;
	pio_sm_put(dpi->pio, sm, tc - 2);
	pio_sm_set_enabled(dpi->pio, sm, true);

	return 0;
}

/*
 * Claim all four SMs. Use SMs 1,2,3 to generate an interrupt:
 * 1: At the end of the left "broad pulse"
 * 2: In the middle of the scanline
 * 3: At the end of the right "broad pulse"
 */
static int rp1dpi_pio_claim_all_start_timers(struct rp1_dpi *dpi,
					     struct drm_display_mode const *mode)
{
	static const u16 instructions[2][4] = {
		{ 0xa022, 0x2083, 0x0042, 0xc010 }, /* posedge */
		{ 0xa022, 0x2003, 0x0042, 0xc010 }, /* negedge */
	};
	const struct pio_program prog = {
		.instructions = instructions[(mode->flags & DRM_MODE_FLAG_NHSYNC) ? 1 : 0],
		.length = ARRAY_SIZE(instructions[0]),
		.origin = -1
	};
	u32 tc[3], sysclk, dpiclk;
	int offset, i;

	dpiclk = clk_get_rate(dpi->clocks[RP1DPI_CLK_DPI]);
	if (!dpiclk)
		dpiclk = 1000u * mode->clock;
	sysclk = clock_get_hz(clk_sys);
	tc[1] = ((u64)mode->htotal * (u64)sysclk) / (u64)(2ul * dpiclk);
	tc[2] = ((u64)(mode->htotal + mode->hsync_start - mode->hsync_end) * (u64)sysclk) /
		(u64)dpiclk;
	tc[0] = tc[2] - tc[1];

	i = pio_claim_sm_mask(dpi->pio, 0xF);
	if (i != 0)
		return -EBUSY;

	offset = pio_add_program(dpi->pio, &prog);
	if (offset == PIO_ORIGIN_ANY)
		return -EBUSY;

	for (i = 0; i < 3; i++) {
		pio_sm_config cfg = pio_get_default_sm_config();

		pio_sm_set_enabled(dpi->pio, i + 1, false);
		sm_config_set_wrap(&cfg, offset, offset + 3);
		pio_sm_init(dpi->pio, i + 1, offset, &cfg);

		pio_sm_put(dpi->pio, i + 1, tc[i] - 4);
		pio_sm_exec(dpi->pio, i + 1, pio_encode_pull(false, false));
		pio_sm_exec(dpi->pio, i + 1, pio_encode_out(pio_y, 32));
		pio_sm_set_enabled(dpi->pio, i + 1, true);
	}

	return 0;
}

/*
 * COMPOSITE SYNC FOR INTERLACED
 *
 * DPI VSYNC (GPIO2) must be a modified signal which is always active-low.
 * It should go low for 1 or 2 scanlines, 2 or 2.5 lines before Vsync-start.
 * Desired VSync width minus 1 (in half-lines) should be written to the FIFO.
 *
 * Three PIO SMs will be configured as timers, to fire at the end of a left
 * broad pulse, the middle of a scanline, and the end of a right broad pulse.
 *
 * HSYNC->CSYNC latency is about 5 cycles, with a jitter of up to 1 cycle.
 *
 * Default program is compiled for +HSync, -CSync. The program may be
 * modified for other polarities. GPIO2 polarity is always active low.
 */

static int rp1dpi_pio_csync_ilace(struct rp1_dpi *dpi,
				  struct drm_display_mode const *mode)
{
	static const int wrap_target = 2;
	static const int wrap = 23;
	unsigned short instructions[] = {  /* This is mutable */
		0x90a0, //  0: pull   block        side 1
		0x7040, //  1: out    y, 32        side 1
		//     .wrap_target                           ; while (true) {
		0x3083, //  2: wait   1 gpio, 3    side 1     ;   do { @HSync
		0xa422, //  3: mov    x, y         side 0 [4] ;   CSYNC: x = VSW - 1
		0x2003, //  4: wait   0 gpio, 3    side 0     ;   CSYNC: HSync->CSync
		0x12c2, //  5: jmp    pin, 2       side 1 [2] ;   } while (VSync)
		0x3083, //  6: wait   1 gpio, 3    side 1     ;   @HSync
		0xc442, //  7: irq    clear 2      side 0 [4] ;   CSYNC: flush IRQ
		0x2003, //  8: wait   0 gpio, 3    side 0     ;   CSYNC: Hsync->CSync
		0x30c2, //  9: wait   1 irq, 2     side 1     ;   @midline
		0x10d4, // 10: jmp    pin, 20      side 1     ;   if (!VSync) goto sync_left;
		0x3083, // 11: wait   1 gpio, 3    side 1     ;   @HSync
		0xa442, // 12: nop                 side 0 [4] ;   CSYNC: delay
		0x2003, // 13: wait   0 gpio, 3    side 0     ;   CSYNC: Hsync->CSync
		0xd042, // 14: irq    clear 2      side 1     ;   do { flush IRQ
		0xd043, // 15: irq    clear 3      side 1     ;     flush IRQ
		0x30c2, // 16: wait   1 irq, 2     side 1     ;     @midline
		0x20c3, // 17: wait   1 irq, 3     side 0     ;     CSYNC: @BroadRight
		0x1054, // 18: jmp    x--, 20      side 1     ;     if (x-- == 0)
		0x1002, // 19: jmp    2            side 1     ;       break;
		0xd041, // 20: irq    clear 1      side 1     ; sync_left: flush IRQ
		0x3083, // 21: wait   1 gpio, 3    side 1     ;     @HSync
		0x20c1, // 22: wait   1 irq, 1     side 0     ;     CSYNC: @BroadLeft
		0x104e, // 23: jmp    x--, 14      side 1     ;   } while (x--);
		//     .wrap                                  ; }
	};
	struct pio_program prog = {
		.instructions = instructions,
		.length = ARRAY_SIZE(instructions),
		.origin = -1
	};
	pio_sm_config cfg = pio_get_default_sm_config();
	unsigned int i, offset;

	/* Claim SM 0 and start timers on the other three SMs. */
	i = rp1dpi_pio_claim_all_start_timers(dpi, mode);
	if (i < 0)
		return -EBUSY;

	/* Adapt program code according to CSync polarity; configure program */
	pio_sm_set_enabled(dpi->pio, 0, false);
	for (i = 0; i < prog.length; i++) {
		if (mode->flags & DRM_MODE_FLAG_PCSYNC)
			instructions[i] ^= 0x1000;
		if ((mode->flags & DRM_MODE_FLAG_NHSYNC) && (instructions[i] & 0xe07f) == 0x2003)
			instructions[i] ^= 0x0080;
	}
	offset = pio_add_program(dpi->pio, &prog);
	if (offset == PIO_ORIGIN_ANY)
		return -1;

	/* Configure pins and SM; set VSync width; start the SM */
	sm_config_set_wrap(&cfg, offset + wrap_target, offset + wrap);
	sm_config_set_sideset(&cfg, 1, false, false);
	sm_config_set_sideset_pins(&cfg, 1); /* PIO produces CSync on GPIO 1 */
	pio_gpio_init(dpi->pio, 1);
	sm_config_set_jmp_pin(&cfg, 2); /* DPI "helper signal" is GPIO 2 */
	pio_sm_init(dpi->pio, 0, offset, &cfg);
	pio_sm_set_consecutive_pindirs(dpi->pio, 0, 1, 1, true);
	pio_sm_put(dpi->pio, 0, mode->vsync_end - mode->vsync_start - 1);
	pio_sm_set_enabled(dpi->pio, 0, true);

	return 0;
}

/*
 * COMPOSITE SYNC (TV-STYLE) for 625/25i and 525/30i only.
 *
 * DPI VSYNC (GPIO2) must be a modified signal which is always active-low.
 * It should go low for 1 or 2 scanlines, VSyncWidth/2.0 or (VSyncWidth+1)/2.0
 * lines before Vsync-start, i.e. just after the last full active TV line
 * (noting that RP1 DPI does not generate half-lines).
 *
 * This will push the image up by 1 line compared to customary DRM timings in
 * "PAL" mode, or 2 lines in "NTSC" mode (which is arguably too low anyway),
 * but avoids a collision between an active line and an equalizing pulse.
 *
 * Another wrinkle is that when the first equalizing pulse aligns with HSync,
 * it becomes a normal-width sync pulse. This was a deliberate simplification.
 * It is unlikely that any TV will notice or care.
 */

static int rp1dpi_pio_csync_tv(struct rp1_dpi *dpi,
			       struct drm_display_mode const *mode)
{
	static const int wrap_target = 6;
	static const int wrap = 27;
	unsigned short instructions[] = {  /* This is mutable */
		0x3703, //  0: wait  0 gpio, 3  side 1 [7] ; while (HSync) delay;
		0x3083, //  1: wait  1 gpio, 3  side 1     ; do { @HSync
		0xa7e6, //  2: mov   osr, isr   side 0 [7] ;   CSYNC: rewind sequence
		0x2003, //  3: wait  0 gpio, 3  side 0     ;   CSYNC: HSync->CSync
		0xb7e6, //  4: mov   osr, isr   side 1 [7] ;   delay
		0x10c1, //  5: jmp   pin, 1     side 1     ; } while (VSync)
		//     .wrap_target                        ; while (true) {
		0xd042, //  6: irq   clear 2    side 1     ;   flush stale IRQ
		0xd043, //  7: irq   clear 3    side 1     ;   flush stale IRQ
		0xb022, //  8: mov   x, y       side 1     ;   X = EQWidth - 3
		0x30c2, //  9: wait  1 irq, 2   side 1     ;   @midline
		0x004a, // 10: jmp   x--, 10    side 0     ;   CSYNC: while (x--) ;
		0x6021, // 11: out   x, 1       side 0     ;   CSYNC: next pulse broad?
		0x002e, // 12: jmp   !x, 14     side 0     ;   CSYNC: if (broad)
		0x20c3, // 13: wait  1 irq, 3   side 0     ;   CSYNC:   @BroadRight
		0x7021, // 14: out   x, 1       side 1     ;   sequence not finished?
		0x1020, // 15: jmp   !x, 0      side 1     ;   if (finished) break
		0xd041, // 16: irq   clear 1    side 1     ;   flush stale IRQ
		0xb022, // 17: mov   x, y       side 1     ;   X = EQWidth - 3
		0x3083, // 18: wait  1 gpio, 3  side 1     ;   @HSync
		0x0053, // 19: jmp   x--, 19    side 0     ;   CSYNC: while (x--) ;
		0x6021, // 20: out   x, 1       side 0     ;   CSYNC: next pulse broad?
		0x0037, // 21: jmp   !x, 23     side 0     ;   CSYNC: if (broad)
		0x20c1, // 22: wait  1 irq, 1   side 0     ;   CSYNC:  @BroadLeft
		0x7021, // 23: out   x, 1       side 1     ;   sequence not finished?
		0x1020, // 24: jmp   !x, 0      side 1     ;   if (finished) break
		0x10c6, // 25: jmp   pin, 6     side 1     ;   if (VSync) continue
		0xb0e6, // 26: mov   osr, isr   side 1     ;   rewind sequence
		0x7022, // 27: out   x, 2       side 1     ;   skip 2 bits
		//     .wrap                               ; }
	};
	struct pio_program prog = {
		.instructions = instructions,
		.length = ARRAY_SIZE(instructions),
		.origin = -1
	};
	pio_sm_config cfg = pio_get_default_sm_config();
	unsigned int i, offset;

	/* Claim SM 0 and start timers on the other three SMs. */
	i = rp1dpi_pio_claim_all_start_timers(dpi, mode);
	if (i < 0)
		return -EBUSY;

	/* Adapt program code according to CSync polarity; configure program */
	pio_sm_set_enabled(dpi->pio, 0, false);
	for (i = 0; i < ARRAY_SIZE(instructions); i++) {
		if (mode->flags & DRM_MODE_FLAG_PCSYNC)
			instructions[i] ^= 0x1000;
		if ((mode->flags & DRM_MODE_FLAG_NHSYNC) && (instructions[i] & 0xe07f) == 0x2003)
			instructions[i] ^= 0x0080;
	}
	offset = pio_add_program(dpi->pio, &prog);
	if (offset == PIO_ORIGIN_ANY)
		return -1;

	/* Configure pins and SM */
	sm_config_set_wrap(&cfg, offset + wrap_target, offset + wrap);
	sm_config_set_sideset(&cfg, 1, false, false);
	sm_config_set_sideset_pins(&cfg, 1); /* PIO produces CSync on GPIO 1 */
	pio_gpio_init(dpi->pio, 1);
	sm_config_set_jmp_pin(&cfg, 2); /* DPI VSync "helper" signal is GPIO 2 */
	pio_sm_init(dpi->pio, 0, offset, &cfg);
	pio_sm_set_consecutive_pindirs(dpi->pio, 0, 1, 1, true);

	/* Load parameters (Vsync pattern; EQ pulse width) into ISR and Y */
	i = (mode->vsync_end - mode->vsync_start <= 5);
	pio_sm_put(dpi->pio, 0, i ? 0x02ABFFAA : 0xAABFFEAA);
	pio_sm_put(dpi->pio, 0, clock_get_hz(clk_sys) / (i ? 425531 : 434782) - 3);
	pio_sm_exec(dpi->pio, 0, pio_encode_pull(false, false));
	pio_sm_exec(dpi->pio, 0, pio_encode_out(pio_y, 32));
	pio_sm_exec(dpi->pio, 0, pio_encode_in(pio_y, 32));
	pio_sm_exec(dpi->pio, 0, pio_encode_pull(false, false));
	pio_sm_exec(dpi->pio, 0, pio_encode_out(pio_y, 32));

	/* Start the SM */
	pio_sm_set_enabled(dpi->pio, 0, true);

	return 0;
}

int rp1dpi_pio_start(struct rp1_dpi *dpi, const struct drm_display_mode *mode,
		     bool force_csync)
{
	int r;

	/*
	 * Check if PIO is needed *and* we have an appropriate pin mapping
	 * that allows all three Sync GPIOs to be snooped on or overridden.
	 */
	if (!(mode->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_CSYNC)) &&
	    !force_csync)
		return 0;
	if (!dpi->sync_gpios_mapped) {
		drm_warn(&dpi->drm, "DPI needs GPIOs 1-3 for Interlace or CSync\n");
		return -EINVAL;
	}

	if (dpi->pio)
		pio_close(dpi->pio);

	dpi->pio = pio_open();
	if (IS_ERR(dpi->pio)) {
		drm_err(&dpi->drm, "Could not open PIO\n");
		dpi->pio = NULL;
		return -ENODEV;
	}

	if ((mode->flags & DRM_MODE_FLAG_CSYNC) || force_csync) {
		drm_info(&dpi->drm, "Using PIO to generate CSync on GPIO1\n");
		if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
			if (mode->clock > 15 * mode->htotal &&
			    mode->clock < 16 * mode->htotal &&
			    (mode->vtotal == 525 || mode->vtotal == 625))
				r = rp1dpi_pio_csync_tv(dpi, mode);
			else
				r = rp1dpi_pio_csync_ilace(dpi, mode);
		} else {
			r = rp1dpi_pio_csync_prog(dpi, mode);
		}
	} else {
		drm_info(&dpi->drm, "Using PIO to generate VSync on GPIO2\n");
		r = rp1dpi_pio_vsync_ilace(dpi, mode);
	}
	if (r) {
		drm_err(&dpi->drm, "Failed to initialize PIO\n");
		rp1dpi_pio_stop(dpi);
	}

	return r;
}

void rp1dpi_pio_stop(struct rp1_dpi *dpi)
{
	if (dpi->pio) {
		/* Return any "stolen" pins to DPI function */
		pio_gpio_set_function(dpi->pio, 1, GPIO_FUNC_FSEL1);
		pio_gpio_set_function(dpi->pio, 2, GPIO_FUNC_FSEL1);
		pio_close(dpi->pio);
		dpi->pio = NULL;
	}
}

#else /* !IS_REACHABLE(CONFIG_RP1_PIO) */

int rp1dpi_pio_start(struct rp1_dpi *dpi, const struct drm_display_mode *mode,
		     bool force_csync)
{
	if (mode->flags & (DRM_MODE_FLAG_CSYNC | DRM_MODE_FLAG_INTERLACE) || force_csync) {
		drm_warn(&dpi->drm, "DPI needs PIO support for Interlace or CSync\n");
		return -ENODEV;
	} else {
		return 0;
	}
}

void rp1dpi_pio_stop(struct rp1_dpi *dpi)
{
}

#endif
