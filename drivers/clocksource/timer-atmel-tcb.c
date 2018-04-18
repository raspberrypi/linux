// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/sched_clock.h>
#include <soc/at91/atmel_tcb.h>

static struct atmel_tcb_clksrc {
	struct clocksource clksrc;
	struct clock_event_device clkevt;
	struct regmap *regmap;
	void __iomem *base;
	struct clk *clk[2];
	char name[20];
	int channels[2];
	int bits;
	int irq;
	struct {
		u32 cmr;
		u32 imr;
		u32 rc;
		bool clken;
	} cache[2];
	u32 bmr_cache;
	bool registered;
} tc = {
	.clksrc = {
		.rating		= 200,
		.mask		= CLOCKSOURCE_MASK(32),
		.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
	},
	.clkevt	= {
		.features	= CLOCK_EVT_FEAT_ONESHOT,
		/* Should be lower than at91rm9200's system timer */
		.rating		= 125,
	},
};

static struct tc_clkevt_device {
	struct clock_event_device clkevt;
	struct regmap *regmap;
	void __iomem *base;
	struct clk *slow_clk;
	struct clk *clk;
	char name[20];
	int channel;
	int irq;
	struct {
		u32 cmr;
		u32 imr;
		u32 rc;
		bool clken;
	} cache;
	bool registered;
} tce = {
	.clkevt	= {
		.features		= CLOCK_EVT_FEAT_PERIODIC |
					  CLOCK_EVT_FEAT_ONESHOT,
		/*
		 * Should be lower than at91rm9200's system timer
		 * but higher than tc.clkevt.rating
		 */
		.rating			= 140,
	},
};

/*
 * Clockevent device using its own channel
 */
static int tc_clkevt2_shutdown(struct clock_event_device *d)
{
	writel(0xff, tce.base + ATMEL_TC_IDR(tce.channel));
	writel(ATMEL_TC_CCR_CLKDIS, tce.base + ATMEL_TC_CCR(tce.channel));
	if (!clockevent_state_detached(d))
		clk_disable(tce.clk);

	return 0;
}

/* For now, we always use the 32K clock ... this optimizes for NO_HZ,
 * because using one of the divided clocks would usually mean the
 * tick rate can never be less than several dozen Hz (vs 0.5 Hz).
 *
 * A divided clock could be good for high resolution timers, since
 * 30.5 usec resolution can seem "low".
 */
static int tc_clkevt2_set_oneshot(struct clock_event_device *d)
{
	if (clockevent_state_oneshot(d) || clockevent_state_periodic(d))
		tc_clkevt2_shutdown(d);

	clk_enable(tce.clk);

	/* slow clock, count up to RC, then irq and stop */
	writel(ATMEL_TC_CMR_TCLK(4) | ATMEL_TC_CMR_CPCSTOP |
	       ATMEL_TC_CMR_WAVE | ATMEL_TC_CMR_WAVESEL_UPRC,
	       tce.base + ATMEL_TC_CMR(tce.channel));
	writel(ATMEL_TC_CPCS, tce.base + ATMEL_TC_IER(tce.channel));

	return 0;
}

static int tc_clkevt2_set_periodic(struct clock_event_device *d)
{
	if (clockevent_state_oneshot(d) || clockevent_state_periodic(d))
		tc_clkevt2_shutdown(d);

	/* By not making the gentime core emulate periodic mode on top
	 * of oneshot, we get lower overhead and improved accuracy.
	 */
	clk_enable(tce.clk);

	/* slow clock, count up to RC, then irq and restart */
	writel(ATMEL_TC_CMR_TCLK(4) | ATMEL_TC_CMR_WAVE |
	       ATMEL_TC_CMR_WAVESEL_UPRC,
	       tce.base + ATMEL_TC_CMR(tce.channel));
	writel((32768 + HZ / 2) / HZ, tce.base + ATMEL_TC_RC(tce.channel));

	/* Enable clock and interrupts on RC compare */
	writel(ATMEL_TC_CPCS, tce.base + ATMEL_TC_IER(tce.channel));
	writel(ATMEL_TC_CCR_CLKEN | ATMEL_TC_CCR_SWTRG,
	       tce.base + ATMEL_TC_CCR(tce.channel));

	return 0;
}

static int tc_clkevt2_next_event(unsigned long delta,
				 struct clock_event_device *d)
{
	writel(delta, tce.base + ATMEL_TC_RC(tce.channel));
	writel(ATMEL_TC_CCR_CLKEN | ATMEL_TC_CCR_SWTRG,
	       tce.base + ATMEL_TC_CCR(tce.channel));

	return 0;
}

static irqreturn_t tc_clkevt2_irq(int irq, void *handle)
{
	unsigned int sr;

	sr = readl(tce.base + ATMEL_TC_SR(tce.channel));
	if (sr & ATMEL_TC_CPCS) {
		tce.clkevt.event_handler(&tce.clkevt);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static void tc_clkevt2_suspend(struct clock_event_device *d)
{
	tce.cache.cmr = readl(tce.base + ATMEL_TC_CMR(tce.channel));
	tce.cache.imr = readl(tce.base + ATMEL_TC_IMR(tce.channel));
	tce.cache.rc = readl(tce.base + ATMEL_TC_RC(tce.channel));
	tce.cache.clken = !!(readl(tce.base + ATMEL_TC_SR(tce.channel)) &
				ATMEL_TC_CLKSTA);
}

static void tc_clkevt2_resume(struct clock_event_device *d)
{
	/* Restore registers for the channel, RA and RB are not used  */
	writel(tce.cache.cmr, tc.base + ATMEL_TC_CMR(tce.channel));
	writel(tce.cache.rc, tc.base + ATMEL_TC_RC(tce.channel));
	writel(0, tc.base + ATMEL_TC_RA(tce.channel));
	writel(0, tc.base + ATMEL_TC_RB(tce.channel));
	/* Disable all the interrupts */
	writel(0xff, tc.base + ATMEL_TC_IDR(tce.channel));
	/* Reenable interrupts that were enabled before suspending */
	writel(tce.cache.imr, tc.base + ATMEL_TC_IER(tce.channel));

	/* Start the clock if it was used */
	if (tce.cache.clken)
		writel(ATMEL_TC_CCR_CLKEN | ATMEL_TC_CCR_SWTRG,
		       tc.base + ATMEL_TC_CCR(tce.channel));
}

static int __init tc_clkevt_register(struct device_node *node,
				     struct regmap *regmap, void __iomem *base,
				     int channel, int irq, int bits)
{
	int ret;

	tce.regmap = regmap;
	tce.base = base;
	tce.channel = channel;
	tce.irq = irq;

	tce.slow_clk = of_clk_get_by_name(node->parent, "slow_clk");
	if (IS_ERR(tce.slow_clk))
		return PTR_ERR(tce.slow_clk);

	ret = clk_prepare_enable(tce.slow_clk);
	if (ret)
		return ret;

	tce.clk = tcb_clk_get(node, tce.channel);
	if (IS_ERR(tce.clk)) {
		ret = PTR_ERR(tce.clk);
		goto err_slow;
	}

	snprintf(tce.name, sizeof(tce.name), "%s:%d",
		 kbasename(node->parent->full_name), channel);
	tce.clkevt.cpumask = cpumask_of(0);
	tce.clkevt.name = tce.name;
	tce.clkevt.set_next_event = tc_clkevt2_next_event,
	tce.clkevt.set_state_shutdown = tc_clkevt2_shutdown,
	tce.clkevt.set_state_periodic = tc_clkevt2_set_periodic,
	tce.clkevt.set_state_oneshot = tc_clkevt2_set_oneshot,
	tce.clkevt.suspend = tc_clkevt2_suspend,
	tce.clkevt.resume = tc_clkevt2_resume,

	/* try to enable clk to avoid future errors in mode change */
	ret = clk_prepare_enable(tce.clk);
	if (ret)
		goto err_slow;
	clk_disable(tce.clk);

	clockevents_config_and_register(&tce.clkevt, 32768, 1, BIT(bits) - 1);

	ret = request_irq(tce.irq, tc_clkevt2_irq, IRQF_TIMER | IRQF_SHARED,
			  tce.clkevt.name, &tce);
	if (ret)
		goto err_clk;

	tce.registered = true;

	return 0;

err_clk:
	clk_unprepare(tce.clk);
err_slow:
	clk_disable_unprepare(tce.slow_clk);

	return ret;
}

/*
 * Clocksource and clockevent using the same channel(s)
 */
static u64 tc_get_cycles(struct clocksource *cs)
{
	u32 lower, upper;

	do {
		upper = readl_relaxed(tc.base + ATMEL_TC_CV(tc.channels[1]));
		lower = readl_relaxed(tc.base + ATMEL_TC_CV(tc.channels[0]));
	} while (upper != readl_relaxed(tc.base + ATMEL_TC_CV(tc.channels[1])));

	return (upper << 16) | lower;
}

static u64 tc_get_cycles32(struct clocksource *cs)
{
	return readl_relaxed(tc.base + ATMEL_TC_CV(tc.channels[0]));
}

static u64 notrace tc_sched_clock_read(void)
{
	return tc_get_cycles(&tc.clksrc);
}

static u64 notrace tc_sched_clock_read32(void)
{
	return tc_get_cycles32(&tc.clksrc);
}

static int tcb_clkevt_next_event(unsigned long delta,
				 struct clock_event_device *d)
{
	u32 old, next, cur;


	old = readl(tc.base + ATMEL_TC_CV(tc.channels[0]));
	next = old + delta;
	writel(next, tc.base + ATMEL_TC_RC(tc.channels[0]));
	cur = readl(tc.base + ATMEL_TC_CV(tc.channels[0]));

	/* check whether the delta elapsed while setting the register */
	if ((next < old && cur < old && cur > next) ||
	    (next > old && (cur < old || cur > next))) {
		/*
		 * Clear the CPCS bit in the status register to avoid
		 * generating a spurious interrupt next time a valid
		 * timer event is configured.
		 */
		old = readl(tc.base + ATMEL_TC_SR(tc.channels[0]));
		return -ETIME;
	}

	writel(ATMEL_TC_CPCS, tc.base + ATMEL_TC_IER(tc.channels[0]));

	return 0;
}

static irqreturn_t tc_clkevt_irq(int irq, void *handle)
{
	unsigned int sr;

	sr = readl(tc.base + ATMEL_TC_SR(tc.channels[0]));
	if (sr & ATMEL_TC_CPCS) {
		tc.clkevt.event_handler(&tc.clkevt);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int tcb_clkevt_oneshot(struct clock_event_device *dev)
{
	if (clockevent_state_oneshot(dev))
		return 0;

	/*
	 * Because both clockevent devices may share the same IRQ, we don't want
	 * the less likely one to stay requested
	 */
	return request_irq(tc.irq, tc_clkevt_irq, IRQF_TIMER | IRQF_SHARED,
			   tc.name, &tc);
}

static int tcb_clkevt_shutdown(struct clock_event_device *dev)
{
	writel(0xff, tc.base + ATMEL_TC_IDR(tc.channels[0]));
	if (tc.bits == 16)
		writel(0xff, tc.base + ATMEL_TC_IDR(tc.channels[1]));

	if (!clockevent_state_detached(dev))
		free_irq(tc.irq, &tc);

	return 0;
}

static void __init tcb_setup_dual_chan(struct atmel_tcb_clksrc *tc,
				       int mck_divisor_idx)
{
	/* first channel: waveform mode, input mclk/8, clock TIOA on overflow */
	writel(mck_divisor_idx			/* likely divide-by-8 */
	       | ATMEL_TC_CMR_WAVE
	       | ATMEL_TC_CMR_WAVESEL_UP	/* free-run */
	       | ATMEL_TC_CMR_ACPA(SET)		/* TIOA rises at 0 */
	       | ATMEL_TC_CMR_ACPC(CLEAR),	/* (duty cycle 50%) */
	       tc->base + ATMEL_TC_CMR(tc->channels[0]));
	writel(0x0000, tc->base + ATMEL_TC_RA(tc->channels[0]));
	writel(0x8000, tc->base + ATMEL_TC_RC(tc->channels[0]));
	writel(0xff, tc->base + ATMEL_TC_IDR(tc->channels[0]));	/* no irqs */
	writel(ATMEL_TC_CCR_CLKEN, tc->base + ATMEL_TC_CCR(tc->channels[0]));

	/* second channel: waveform mode, input TIOA */
	writel(ATMEL_TC_CMR_XC(tc->channels[1])		/* input: TIOA */
	       | ATMEL_TC_CMR_WAVE
	       | ATMEL_TC_CMR_WAVESEL_UP,		/* free-run */
	       tc->base + ATMEL_TC_CMR(tc->channels[1]));
	writel(0xff, tc->base + ATMEL_TC_IDR(tc->channels[1]));	/* no irqs */
	writel(ATMEL_TC_CCR_CLKEN, tc->base + ATMEL_TC_CCR(tc->channels[1]));

	/* chain both channel, we assume the previous channel */
	regmap_write(tc->regmap, ATMEL_TC_BMR,
		     ATMEL_TC_BMR_TCXC(1 + tc->channels[1], tc->channels[1]));
	/* then reset all the timers */
	regmap_write(tc->regmap, ATMEL_TC_BCR, ATMEL_TC_BCR_SYNC);
}

static void __init tcb_setup_single_chan(struct atmel_tcb_clksrc *tc,
					 int mck_divisor_idx)
{
	/* channel 0:  waveform mode, input mclk/8 */
	writel(mck_divisor_idx			/* likely divide-by-8 */
	       | ATMEL_TC_CMR_WAVE
	       | ATMEL_TC_CMR_WAVESEL_UP,	/* free-run */
	       tc->base + ATMEL_TC_CMR(tc->channels[0]));
	writel(0xff, tc->base + ATMEL_TC_IDR(tc->channels[0]));	/* no irqs */
	writel(ATMEL_TC_CCR_CLKEN, tc->base + ATMEL_TC_CCR(tc->channels[0]));

	/* then reset all the timers */
	regmap_write(tc->regmap, ATMEL_TC_BCR, ATMEL_TC_BCR_SYNC);
}

static void tc_clksrc_suspend(struct clocksource *cs)
{
	int i;

	for (i = 0; i < 1 + (tc.bits == 16); i++) {
		tc.cache[i].cmr = readl(tc.base + ATMEL_TC_CMR(tc.channels[i]));
		tc.cache[i].imr = readl(tc.base + ATMEL_TC_IMR(tc.channels[i]));
		tc.cache[i].rc = readl(tc.base + ATMEL_TC_RC(tc.channels[i]));
		tc.cache[i].clken = !!(readl(tc.base +
					     ATMEL_TC_SR(tc.channels[i])) &
				       ATMEL_TC_CLKSTA);
	}

	if (tc.bits == 16)
		regmap_read(tc.regmap, ATMEL_TC_BMR, &tc.bmr_cache);
}

static void tc_clksrc_resume(struct clocksource *cs)
{
	int i;

	for (i = 0; i < 1 + (tc.bits == 16); i++) {
		/* Restore registers for the channel, RA and RB are not used  */
		writel(tc.cache[i].cmr, tc.base + ATMEL_TC_CMR(tc.channels[i]));
		writel(tc.cache[i].rc, tc.base + ATMEL_TC_RC(tc.channels[i]));
		writel(0, tc.base + ATMEL_TC_RA(tc.channels[i]));
		writel(0, tc.base + ATMEL_TC_RB(tc.channels[i]));
		/* Disable all the interrupts */
		writel(0xff, tc.base + ATMEL_TC_IDR(tc.channels[i]));
		/* Reenable interrupts that were enabled before suspending */
		writel(tc.cache[i].imr, tc.base + ATMEL_TC_IER(tc.channels[i]));

		/* Start the clock if it was used */
		if (tc.cache[i].clken)
			writel(ATMEL_TC_CCR_CLKEN, tc.base +
			       ATMEL_TC_CCR(tc.channels[i]));
	}

	/* in case of dual channel, chain channels */
	if (tc.bits == 16)
		regmap_write(tc.regmap, ATMEL_TC_BMR, tc.bmr_cache);
	/* Finally, trigger all the channels*/
	regmap_write(tc.regmap, ATMEL_TC_BCR, ATMEL_TC_BCR_SYNC);
}

static int __init tcb_clksrc_register(struct device_node *node,
				      struct regmap *regmap, void __iomem *base,
				      int channel, int channel1, int irq,
				      int bits)
{
	u32 rate, divided_rate = 0;
	int best_divisor_idx = -1;
	int i, err = -1;
	u64 (*tc_sched_clock)(void);

	tc.regmap = regmap;
	tc.base = base;
	tc.channels[0] = channel;
	tc.channels[1] = channel1;
	tc.irq = irq;
	tc.bits = bits;

	tc.clk[0] = tcb_clk_get(node, tc.channels[0]);
	if (IS_ERR(tc.clk[0]))
		return PTR_ERR(tc.clk[0]);
	err = clk_prepare_enable(tc.clk[0]);
	if (err) {
		pr_debug("can't enable T0 clk\n");
		goto err_clk;
	}

	/* How fast will we be counting?  Pick something over 5 MHz.  */
	rate = (u32)clk_get_rate(tc.clk[0]);
	for (i = 0; i < 5; i++) {
		unsigned int divisor = atmel_tc_divisors[i];
		unsigned int tmp;

		if (!divisor)
			continue;

		tmp = rate / divisor;
		pr_debug("TC: %u / %-3u [%d] --> %u\n", rate, divisor, i, tmp);
		if (best_divisor_idx > 0) {
			if (tmp < 5 * 1000 * 1000)
				continue;
		}
		divided_rate = tmp;
		best_divisor_idx = i;
	}

	if (tc.bits == 32) {
		tc.clksrc.read = tc_get_cycles32;
		tcb_setup_single_chan(&tc, best_divisor_idx);
		tc_sched_clock = tc_sched_clock_read32;
		snprintf(tc.name, sizeof(tc.name), "%s:%d",
			 kbasename(node->parent->full_name), tc.channels[0]);
	} else {
		tc.clk[1] = tcb_clk_get(node, tc.channels[1]);
		if (IS_ERR(tc.clk[1]))
			goto err_disable_t0;

		err = clk_prepare_enable(tc.clk[1]);
		if (err) {
			pr_debug("can't enable T1 clk\n");
			goto err_clk1;
		}
		tc.clksrc.read = tc_get_cycles,
		tcb_setup_dual_chan(&tc, best_divisor_idx);
		tc_sched_clock = tc_sched_clock_read;
		snprintf(tc.name, sizeof(tc.name), "%s:%d,%d",
			 kbasename(node->parent->full_name), tc.channels[0],
			 tc.channels[1]);
	}

	pr_debug("%s at %d.%03d MHz\n", tc.name,
		 divided_rate / 1000000,
		 ((divided_rate + 500000) % 1000000) / 1000);

	tc.clksrc.name = tc.name;
	tc.clksrc.suspend = tc_clksrc_suspend;
	tc.clksrc.resume = tc_clksrc_resume;

	err = clocksource_register_hz(&tc.clksrc, divided_rate);
	if (err)
		goto err_disable_t1;

	sched_clock_register(tc_sched_clock, 32, divided_rate);

	tc.registered = true;

	/* Set up and register clockevents */
	tc.clkevt.name = tc.name;
	tc.clkevt.cpumask = cpumask_of(0);
	tc.clkevt.set_next_event = tcb_clkevt_next_event;
	tc.clkevt.set_state_oneshot = tcb_clkevt_oneshot;
	tc.clkevt.set_state_shutdown = tcb_clkevt_shutdown;
	clockevents_config_and_register(&tc.clkevt, divided_rate, 1,
					BIT(tc.bits) - 1);

	return 0;

err_disable_t1:
	if (tc.bits == 16)
		clk_disable_unprepare(tc.clk[1]);

err_clk1:
	if (tc.bits == 16)
		clk_put(tc.clk[1]);

err_disable_t0:
	clk_disable_unprepare(tc.clk[0]);

err_clk:
	clk_put(tc.clk[0]);

	pr_err("%s: unable to register clocksource/clockevent\n",
	       tc.clksrc.name);

	return err;
}

static int __init tcb_clksrc_init(struct device_node *node)
{
	const struct of_device_id *match;
	const struct atmel_tcb_info *tcb_info;
	struct regmap *regmap;
	void __iomem *tcb_base;
	u32 channel;
	int bits, irq, err, chan1 = -1;

	if (tc.registered && tce.registered)
		return -ENODEV;

	/*
	 * The regmap has to be used to access registers that are shared
	 * between channels on the same TCB but we keep direct IO access for
	 * the counters to avoid the impact on performance
	 */
	regmap = syscon_node_to_regmap(node->parent);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	tcb_base = of_iomap(node->parent, 0);
	if (!tcb_base) {
		pr_err("%s +%d %s\n", __FILE__, __LINE__, __func__);
		return -ENXIO;
	}

	match = of_match_node(atmel_tcb_dt_ids, node->parent);
	tcb_info = match->data;
	bits = tcb_info->bits;

	err = of_property_read_u32_index(node, "reg", 0, &channel);
	if (err)
		return err;

	irq = tcb_irq_get(node, channel);
	if (irq < 0)
		return irq;

	if (tc.registered)
		return tc_clkevt_register(node, regmap, tcb_base, channel, irq,
					  bits);

	if (bits == 16) {
		of_property_read_u32_index(node, "reg", 1, &chan1);
		if (chan1 == -1) {
			if (tce.registered) {
				pr_err("%s: clocksource needs two channels\n",
				       node->parent->full_name);
				return -EINVAL;
			} else {
				return tc_clkevt_register(node, regmap,
							  tcb_base, channel,
							  irq, bits);
			}
		}
	}

	return tcb_clksrc_register(node, regmap, tcb_base, channel, chan1, irq,
				   bits);
}
CLOCKSOURCE_OF_DECLARE(atmel_tcb_clksrc, "atmel,tcb-timer",
		       tcb_clksrc_init);
