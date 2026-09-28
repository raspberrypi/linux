// SPDX-License-Identifier: GPL-2.0
/* RP1 PIO -> local DMA -> Ethernet TSU latch, exposed on the Ethernet PHC.
 * The direct path supports one EXTS channel and 28 dynamically routable GPIOs.
 * While PEROUT is active, up to two independent EXTS lanes use synchronized
 * PIO counters mapped to the output pad monitor's TSU markers. The PIO waits
 * for an arm token before detecting each direct-path transition. Host rearm
 * creates dead time; unarmed transitions cannot be counted.
 */
#include <linux/debugfs.h>
#include <linux/build_bug.h>
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pio_rp1.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/rp1.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include "macb.h"
#include "macb_rp1_ptp.h"
#include "macb_rp1_ptp_counter.h"
#include "macb_rp1_ptp_math.h"

#define EXT_GPIO_COUNT 28
#define EXT_MAX_AGE_NS 100000000ULL
#define EXT_WATCHDOG_MS 100
#define EXT_INPUT_BIAS_NS 0
#define EXT_OUTPUT_BIAS_NS 0

#define OUT_RECORDS 128
#define RP1_MAX_EXTTS 2
#define RP1_OUTPUT_PROGRAM_WORDS 22
#define RP1_OUTPUT_STARTUP_PROGRAM_WORDS 26
#define RP1_COUNTER_PROGRAM_WORDS 10
#define RP1_COUNTER_RING_WORDS 256

/* The compact output program generates both the pad waveform and TSU markers.
 * Do not add an input-direction monitor SM on this same GPIO: its direction
 * request removes the pad output enable on RP1.
 */
static const u16 rp1_compact_output_program[RP1_OUTPUT_PROGRAM_WORDS] = {
	0x80a0, 0xa027, 0x0042, 0x80a0, 0xa0c3, 0x8020, 0xa0c7, 0xa04b,
	0xa023, 0x8080, 0xa027, 0x00b4, 0x0034, 0xa046, 0xa0c3, 0x9000,
	0xb022, 0x1051, 0xa042, 0x0000, 0x0089, 0x0009,
};

/* Rising-edge counter shared by the output monitor and external-input lanes.
 * PIO_ADD_PROGRAM relocates its JMP addresses.
 */
static const u16 rp1_counter_program[RP1_COUNTER_PROGRAM_WORDS] = {
	0x00c7, 0x0080, 0x0000, 0x00c5, 0x0080,
	0x0083, 0x0003, 0xa0c2, 0x8000, 0x0003,
};

struct out_record { u64 sequence, target, stamp; s64 error; };

struct rp1_counter_anchor {
	u64 sequence;
	u64 stamp;
	u64 counter_sequence;
	u64 wraps;
	u32 y;
	u32 rate;
	bool marker_valid;
	bool counter_valid;
};

struct rp1_input_sample {
	u64 sequence;
	u64 wraps;
	u32 y;
};

struct rp1_duplex_input {
	struct rp1_extts *parent;
	struct rp1_pinctrl_pio_pin *pin;
	struct dma_chan *chan;
	struct device *dma_dev;
	u32 *ring;
	dma_addr_t ring_dma;
	struct rp1_input_sample *pending;
	int gpio;
	int sm;
	int error;
	int requested_gpio;
	unsigned int requested_edges;
	u32 saved_dma;
	bool saved_dma_valid;
	bool requested;
	bool prepared;
	bool started;
	atomic64_t produced;
	u64 consumed;
	u64 samples;
	u64 overruns;
	u64 sequence;
	u64 wraps;
	u32 last_y;
	bool have_last_y;
	u64 events;
	u64 last_event_ns;
	u64 pending_head;
	u64 pending_tail;
	u64 max_uncertainty_ns;
};

struct rate_record {
	u64 sequence, before, after, target, pulse;
	u32 old_rate, new_rate, deadline_word;
	u64 sent_at, drained_at;
	int action;
};

struct rp1_extts {
	struct macb *bp;
	struct mutex lock; /* Serializes PTP, PIO, DMA and GPIO state. */
	struct delayed_work work;
	wait_queue_head_t monitor_wait;
	struct workqueue_struct *wq;
	struct dentry *debug;
	struct rp1_pio_client *pio;
	struct rp1_pinctrl_pio_pin *pin;
	void __iomem *control;
	resource_size_t control_phys;
	struct dma_chan *chan;
	struct dma_chan *monitor_chan;
	struct device *monitor_dma_dev;
	u32 *monitor_ring;
	dma_addr_t monitor_ring_dma;
	dma_addr_t target;
	dma_cookie_t cookie;
	atomic_t done;
	struct ptp_pin_desc pins[EXT_GPIO_COUNT];
	int sm, offset, words, gpio, edge, last_error;
	int monitor_sm, monitor_offset, monitor_error;
	bool mapped, saved_dma_valid, enabled, armed, dying;
	bool monitor_program_loaded, monitor_started, monitor_saved_dma_valid;
	u32 saved_dma;
	u32 monitor_saved_dma;
	u64 armed_at, last_stamp, events, faults, timeouts, clock_steps;
	atomic64_t monitor_produced;
	u64 monitor_consumed, monitor_samples, monitor_overruns, monitor_last;
	u64 monitor_wraps;
	bool monitor_have_last_y;
	u64 last_age, max_age, last_deadtime, max_deadtime;
	bool output, compact, calibrating, preparing;
	u32 out_rate, out_high, out_width, out_phase;
	u64 map_at, map_cycles_fp, out_retargets, out_late_skips;
	u64 out_max_update_ns;
	u32 map_rate, out_deadline;
	bool map_valid, rate_irqs_held;
	unsigned long rate_irq_flags;
	u64 out_first, out_target, out_count, out_anchors, rate_changes;
	u64 out_prepares, out_prepare_stamp, out_prepare_age, out_max_prepare_age;
	u64 out_min_lead, out_last_latch;
	s64 out_error, out_min_error, out_max_error;
	struct rp1_duplex_input input[RP1_MAX_EXTTS];
	struct rp1_counter_anchor anchors[OUT_RECORDS];
	u64 anchor_sequence;
	u64 anchor_out_base;
	u64 anchor_mismatches;
	int out_startup_origin;
	int out_compact_origin;
	struct out_record out_records[OUT_RECORDS];
	u64 rate_before, rate_count, out_rate_epoch, rate_history_floor;
	struct rate_record rate_records[OUT_RECORDS];
};

static int out_start(struct rp1_extts *s);
static int out_event(struct rp1_extts *s, u64 stamp, u64 now);
static int out_enable(struct rp1_extts *s, struct ptp_perout_request *rq, int on);
static int out_monitor_prepare(struct rp1_extts *s);
static int out_monitor_start(struct rp1_extts *s);
static int out_monitor_release(struct rp1_extts *s);
static int duplex_input_enable(struct rp1_extts *s, unsigned int index,
			       unsigned int edges, int on);
static int duplex_inputs_resync(struct rp1_extts *s, bool *disrupted);
static int duplex_input_prepare(struct rp1_extts *s, unsigned int index,
				int gpio);
static int duplex_input_release(struct rp1_extts *s, unsigned int index);
static void ext_fail(struct rp1_extts *s, int err);
static int duplex_input_submit(struct rp1_duplex_input *in);
static int duplex_monitor_submit(struct rp1_extts *s);
static void duplex_anchor_counter(struct rp1_extts *s, u64 sequence,
				  u32 y, u64 wraps);

static int out_inhibit(struct rp1_extts *s)
{
	if (!s->output || !s->pin)
		return 0;
	/* The provider asserts the low override before disabling the pad. */
	return rp1_pinctrl_pio_set_output(s->pin, false);
}

static void ext_config(struct rp1_extts *s, struct rp1_pio_sm_init_args *init)
{
	unsigned int wrap_bottom = s->offset +
		(s->output && !s->compact ? 5 : 0);

	init->sm = s->sm;
	init->initial_pc = s->offset;
	init->config.clkdiv = 1 << 16;
	init->config.execctrl = wrap_bottom << 7 |
		(s->offset + s->words - 1) << 12;
	init->config.shiftctrl = BIT(18) | BIT(19);
	if (s->output)
		init->config.pinctrl = BIT(29) | (s->gpio << 10);
}

static int ext_sm_enable(struct rp1_extts *s, bool enable)
{
	struct rp1_pio_sm_set_enabled_args args = {
		.mask = BIT(s->sm), .enable = enable,
	};
	int ret = rp1_pio_sm_set_enabled(s->pio, &args);

	return ret < 0 ? ret : 0;
}

static int ext_dmactrl(struct rp1_extts *s, u32 ctrl)
{
	struct rp1_pio_sm_set_dmactrl_args args = { .sm = s->sm, .ctrl = ctrl };
	int ret = rp1_pio_sm_set_dmactrl(s->pio, &args);

	return ret < 0 ? ret : 0;
}

static int ext_dmactrl_sm(struct rp1_extts *s, int sm, u32 ctrl)
{
	struct rp1_pio_sm_set_dmactrl_args args = { .sm = sm, .ctrl = ctrl };
	int ret = rp1_pio_sm_set_dmactrl(s->pio, &args);

	return ret < 0 ? ret : 0;
}

static int ext_sm_set_enabled_mask(struct rp1_extts *s, u16 mask, bool enable)
{
	struct rp1_pio_sm_set_enabled_args args = {
		.mask = mask, .enable = enable,
	};
	int ret = rp1_pio_sm_set_enabled(s->pio, &args);

	return ret < 0 ? ret : 0;
}

static int ext_now(struct rp1_extts *s, u64 *ns)
{
	struct timespec64 ts;
	u64 sec;
	u32 nsec;
	int ret = s->bp->ptp_clock_info.gettimex64(&s->bp->ptp_clock_info, &ts, NULL);

	if (ret)
		return ret;
	if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= NSEC_PER_SEC)
		return -ERANGE;
	sec = ts.tv_sec;
	nsec = ts.tv_nsec;
	if (sec > div_u64(U64_MAX - nsec, NSEC_PER_SEC))
		return -ERANGE;
	*ns = sec * NSEC_PER_SEC + nsec;
	return 0;
}

static int ext_stamp(struct rp1_extts *s, u64 *ns)
{
	u32 hi = readl(s->bp->regs + 0x1c4);
	u32 lo = readl(s->bp->regs + 0x1c8);
	u32 nsec = readl(s->bp->regs + 0x1cc);
	u64 sec = (u64)hi << 32 | lo;

	if (hi & 0xffff0000 || nsec >= NSEC_PER_SEC ||
	    sec > div_u64(U64_MAX - NSEC_PER_SEC, NSEC_PER_SEC))
		return -ERANGE;
	*ns = sec * NSEC_PER_SEC + nsec;
	return 0;
}

/* Caller holds s->lock. The callback never takes this mutex. */
static int ext_stop(struct rp1_extts *s)
{
	struct rp1_pio_sm_clear_fifos_args args = { .sm = s->sm };
	int first_err = 0;
	unsigned int i;
	int ret;

	s->map_valid = false;
	ret = out_inhibit(s);
	if (ret)
		first_err = ret;
	if (s->pio && s->sm >= 0) {
		ret = ext_sm_enable(s, false);
		if (ret && !first_err)
			first_err = ret;
		if (s->monitor_sm >= 0) {
			ret = ext_sm_set_enabled_mask(s, BIT(s->monitor_sm), false);
			if (ret && !first_err)
				first_err = ret;
		}
		if (s->chan) {
			ret = dmaengine_terminate_sync(s->chan);
			if (ret && !first_err)
				first_err = ret;
		}
		if (s->monitor_chan) {
			ret = dmaengine_terminate_sync(s->monitor_chan);
			if (ret && !first_err)
				first_err = ret;
		}
		{
			unsigned int i;

			for (i = 0; i < RP1_MAX_EXTTS; i++) {
				struct rp1_duplex_input *in = &s->input[i];

				if (in->sm >= 0) {
					ret = ext_sm_set_enabled_mask(s, BIT(in->sm), false);
					if (ret && !first_err)
						first_err = ret;
				}
				if (in->chan) {
					ret = dmaengine_terminate_sync(in->chan);
					if (ret && !first_err)
						first_err = ret;
				}
			}
		}
		if (!first_err) {
			ret = rp1_pio_sm_clear_fifos(s->pio, &args);
			if (ret < 0)
				first_err = ret;
		}
	} else if (s->chan) {
		ret = dmaengine_terminate_sync(s->chan);
		if (ret && !first_err)
			first_err = ret;
	}
	s->monitor_started = false;
	for (i = 0; i < RP1_MAX_EXTTS; i++)
		s->input[i].started = false;
	if (s->armed) {
		/* 6 -> 0 creates a housekeeping capture. It is never published. */
		writel(0, s->control);
		if (readl(s->control))
			first_err = first_err ?: -EIO;
		else
			s->armed = false;
	}
	atomic_set(&s->done, 0);
	cancel_delayed_work(&s->work);
	if (!s->enabled)
		wake_up_all(&s->monitor_wait);
	return first_err;
}

static int ext_release(struct rp1_extts *s)
{
	int ret;

	ret = ext_stop(s);
	if (ret)
		return ret;
	{
		unsigned int i;

		for (i = 0; i < RP1_MAX_EXTTS; i++) {
			if (s->input[i].prepared || s->input[i].sm >= 0 ||
			    s->input[i].pin || s->input[i].chan ||
			    s->input[i].ring) {
				ret = duplex_input_release(s, i);
				if (ret)
					return ret;
			}
		}
	}
	ret = out_monitor_release(s);
	if (ret)
		return ret;
	if (s->chan) {
		if (s->mapped) {
			dma_unmap_resource(s->chan->device->dev, s->target, 4, DMA_FROM_DEVICE, 0);
			s->mapped = false;
		}
		ret = rp1_pio_dma_release(s->pio, s->sm, PIO_DIR_FROM_SM, s->chan);
		if (ret)
			return ret;
		s->chan = NULL;
	}
	if (s->pio) {
		if (s->saved_dma_valid) {
			ret = ext_dmactrl(s, s->saved_dma);
			if (ret)
				return ret;
		}
		ret = rp1_pio_close_checked(s->pio);
		if (ret)
			return ret;
		s->pio = NULL;
	}
	if (s->pin) {
		ret = rp1_pinctrl_pio_release(s->pin);
		if (ret)
			return ret;
		s->pin = NULL;
	}
	s->sm = -1;
	s->offset = -1;
	s->compact = false;
	s->saved_dma_valid = false;
	return 0;
}

static void out_monitor_dma_done(void *arg)
{
	struct rp1_extts *s = arg;

	atomic64_inc(&s->monitor_produced);
	if (!READ_ONCE(s->dying))
		mod_delayed_work(s->wq, &s->work, 0);
}

static int out_monitor_release(struct rp1_extts *s)
{
	struct rp1_pio_sm_claim_args unclaim;
	struct rp1_pio_remove_program_args remove;
	int ret;

	if (s->monitor_chan) {
		ret = rp1_pio_dma_release(s->pio, s->monitor_sm,
					  PIO_DIR_FROM_SM, s->monitor_chan);
		if (ret)
			return ret;
		s->monitor_chan = NULL;
	}
	if (s->monitor_ring) {
		if (!s->monitor_dma_dev)
			return -EINVAL;
		dma_free_coherent(s->monitor_dma_dev,
				  RP1_COUNTER_RING_WORDS * sizeof(*s->monitor_ring),
				  s->monitor_ring, s->monitor_ring_dma);
		s->monitor_ring = NULL;
		s->monitor_ring_dma = 0;
	}
	if (s->monitor_dma_dev) {
		put_device(s->monitor_dma_dev);
		s->monitor_dma_dev = NULL;
	}
	if (s->monitor_program_loaded) {
		remove.num_instrs = RP1_COUNTER_PROGRAM_WORDS;
		remove.origin = s->monitor_offset;
		ret = rp1_pio_remove_program(s->pio, &remove);
		if (ret < 0)
			return ret;
		s->monitor_program_loaded = false;
		s->monitor_offset = -1;
	}
	if (s->monitor_sm >= 0) {
		if (s->monitor_saved_dma_valid) {
			ret = ext_dmactrl_sm(s, s->monitor_sm, s->monitor_saved_dma);
			if (ret)
				return ret;
			s->monitor_saved_dma_valid = false;
		}
		unclaim.mask = BIT(s->monitor_sm);
		ret = rp1_pio_sm_unclaim(s->pio, &unclaim);
		if (ret < 0)
			return ret;
		s->monitor_sm = -1;
	}
	s->monitor_started = false;
	s->monitor_offset = -1;
	s->monitor_consumed = 0;
	s->monitor_samples = 0;
	s->monitor_overruns = 0;
	s->monitor_last = 0;
	s->monitor_wraps = 0;
	s->monitor_have_last_y = false;
	atomic64_set(&s->monitor_produced, 0);
	return 0;
}

static int out_monitor_prepare(struct rp1_extts *s)
{
	struct rp1_pio_sm_claim_args claim = {};
	struct rp1_pio_add_program_args prog = {
		.num_instrs = RP1_COUNTER_PROGRAM_WORDS,
	};
	struct rp1_pio_sm_init_args init = {};
	struct rp1_pio_sm_clear_fifos_args clear = {};
	struct rp1_pio_sm_exec_args exec = {};
	struct rp1_pio_sm_set_dmactrl_args dma = {};
	struct rp1_pio_sm_get_dmactrl_args old_dma = {};
	struct device *dma_dev;
	int ret;

	if (s->monitor_sm >= 0)
		return 0;
	if (!s->compact || s->offset < 0 ||
	    s->offset + RP1_OUTPUT_PROGRAM_WORDS + RP1_COUNTER_PROGRAM_WORDS >
	    RP1_PIO_INSTRUCTION_COUNT)
		return -ENOSPC;

	ret = rp1_pio_sm_claim(s->pio, &claim);
	if (ret < 0)
		return ret;
	s->monitor_sm = ret;
	s->monitor_offset = s->offset + RP1_OUTPUT_PROGRAM_WORDS;
	prog.origin = s->monitor_offset;
	memcpy(prog.instrs, rp1_counter_program, sizeof(rp1_counter_program));
	ret = rp1_pio_can_add_program(s->pio, &prog);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ENOSPC;
	ret = rp1_pio_add_program(s->pio, &prog);
	if (ret < 0)
		return ret;
	if (ret != s->monitor_offset) {
		struct rp1_pio_remove_program_args remove = {
			.num_instrs = RP1_COUNTER_PROGRAM_WORDS,
			.origin = ret,
		};
		int remove_ret = rp1_pio_remove_program(s->pio, &remove);

		return remove_ret < 0 ? remove_ret : -EPROTO;
	}
	s->monitor_program_loaded = true;

	init.sm = s->monitor_sm;
	init.initial_pc = s->monitor_offset;
	init.config.clkdiv = 1 << 16;
	init.config.execctrl = (s->monitor_offset << 7) |
		((s->monitor_offset + RP1_COUNTER_PROGRAM_WORDS - 1) << 12) |
		(s->gpio << 24);
	init.config.shiftctrl = BIT(18) | BIT(19);
	/* Deliberately omit PINCTRL and SET_PINDIRS. This SM only samples JMP PIN. */
	ret = rp1_pio_sm_init(s->pio, &init);
	if (ret < 0)
		return ret;
	clear.sm = s->monitor_sm;
	ret = rp1_pio_sm_clear_fifos(s->pio, &clear);
	if (ret < 0)
		return ret;
	old_dma.sm = s->monitor_sm;
	ret = rp1_pio_sm_get_dmactrl(s->pio, &old_dma);
	if (ret < 0)
		return ret;
	s->monitor_saved_dma = old_dma.ctrl;
	s->monitor_saved_dma_valid = true;
	exec.sm = s->monitor_sm;
	exec.instr = 0xa04b; /* MOV Y, ~NULL */
	ret = rp1_pio_sm_exec(s->pio, &exec);
	if (ret < 0)
		return ret;
	dma.sm = s->monitor_sm;
	dma.ctrl = 0x80000101;
	ret = rp1_pio_sm_set_dmactrl(s->pio, &dma);
	if (ret < 0)
		return ret;
	s->monitor_chan = rp1_pio_dma_request(s->pio, s->monitor_sm,
					      PIO_DIR_FROM_SM);
	if (IS_ERR(s->monitor_chan)) {
		ret = PTR_ERR(s->monitor_chan);
		s->monitor_chan = NULL;
		return ret;
	}
	dma_dev = s->monitor_chan->device->dev;
	get_device(dma_dev);
	s->monitor_dma_dev = dma_dev;
	s->monitor_ring = dma_alloc_coherent(dma_dev,
					     RP1_COUNTER_RING_WORDS * sizeof(*s->monitor_ring),
			&s->monitor_ring_dma, GFP_KERNEL);
	if (!s->monitor_ring)
		return -ENOMEM;
	memset(s->monitor_ring, 0,
	       RP1_COUNTER_RING_WORDS * sizeof(*s->monitor_ring));
	atomic64_set(&s->monitor_produced, 0);
	s->monitor_consumed = 0;
	s->monitor_samples = 0;
	s->monitor_overruns = 0;
	s->monitor_last = 0;
	/* Keep the channel prepared but unissued until both counter lanes can
	 * establish their epoch together. The descriptor is submitted at start so
	 * PHC-step recovery can reuse the same resources safely.
	 */
	s->monitor_started = false;
	return 0;
}

static int out_monitor_start(struct rp1_extts *s)
{
	struct rp1_pio_sm_enable_sync_args enable = {};
	unsigned int i;
	int ret;

	if (s->monitor_sm < 0 || !s->monitor_chan || !s->monitor_ring)
		return -ENODEV;
	if (s->monitor_started)
		return -EALREADY;
	for (i = 0; i < RP1_MAX_EXTTS; i++) {
		if (!s->input[i].requested || s->input[i].prepared)
			continue;
		ret = duplex_input_prepare(s, i, s->input[i].requested_gpio);
		if (ret)
			return ret;
	}
	ret = duplex_monitor_submit(s);
	if (ret)
		return ret;
	enable.mask = BIT(s->sm) | BIT(s->monitor_sm);
	memset(s->monitor_ring, 0,
	       RP1_COUNTER_RING_WORDS * sizeof(*s->monitor_ring));
	atomic64_set(&s->monitor_produced, 0);
	s->monitor_consumed = 0;
	s->monitor_samples = 0;
	s->monitor_wraps = 0;
	s->monitor_have_last_y = false;
	for (i = 0; i < RP1_MAX_EXTTS; i++) {
		if (!s->input[i].prepared)
			continue;
		memset(s->input[i].ring, 0,
		       RP1_COUNTER_RING_WORDS * sizeof(*s->input[i].ring));
		atomic64_set(&s->input[i].produced, 0);
		s->input[i].consumed = 0;
		s->input[i].samples = 0;
		s->input[i].overruns = 0;
		s->input[i].sequence = 0;
		s->input[i].wraps = 0;
		s->input[i].have_last_y = false;
		s->input[i].pending_head = 0;
		s->input[i].pending_tail = 0;
		s->input[i].last_event_ns = 0;
		ret = duplex_input_submit(&s->input[i]);
		if (ret)
			return ret;
		dma_async_issue_pending(s->input[i].chan);
		enable.mask |= BIT(s->input[i].sm);
	}
	dma_async_issue_pending(s->monitor_chan);
	s->anchor_out_base = s->out_count;
	s->anchor_sequence = 0;
	memset(s->anchors, 0, sizeof(s->anchors));
	ret = rp1_pio_sm_enable_sync(s->pio, &enable);
	if (ret < 0) {
		dmaengine_terminate_sync(s->monitor_chan);
		for (i = 0; i < RP1_MAX_EXTTS; i++)
			if (s->input[i].prepared)
				dmaengine_terminate_sync(s->input[i].chan);
		return ret;
	}
	s->monitor_started = true;
	s->monitor_error = 0;
	for (i = 0; i < RP1_MAX_EXTTS; i++)
		s->input[i].started = s->input[i].prepared;
	wake_up_all(&s->monitor_wait);
	return 0;
}

static int out_monitor_drain(struct rp1_extts *s)
{
	u64 produced = atomic64_read(&s->monitor_produced), first;
	u32 available;
	int ret;

	if (!s->monitor_started || !s->monitor_ring)
		return 0;
	ret = rp1_counter_ring_window(produced, s->monitor_consumed,
				      RP1_COUNTER_RING_WORDS, &first, &available);
	if (ret == -EOVERFLOW) {
		s->monitor_overruns += first - s->monitor_consumed;
		s->monitor_consumed = first;
	} else if (ret) {
		return ret;
	}
	while (available--) {
		u32 value;
		u32 index = s->monitor_consumed % RP1_COUNTER_RING_WORDS;
		u64 sequence;

		dma_rmb();
		value = READ_ONCE(s->monitor_ring[index]);
		sequence = s->monitor_consumed + 1;
		if (s->monitor_have_last_y && value > s->monitor_last)
			s->monitor_wraps++;
		s->monitor_last = value;
		s->monitor_consumed++;
		s->monitor_samples = sequence;
		s->monitor_have_last_y = true;
		duplex_anchor_counter(s, sequence, value, s->monitor_wraps);
	}
	if (ret == -EOVERFLOW)
		s->monitor_error = ret;
	return ret == -EOVERFLOW ? 0 : ret;
}

static void duplex_input_dma_done(void *arg)
{
	struct rp1_duplex_input *in = arg;
	struct rp1_extts *s = in->parent;

	atomic64_inc(&in->produced);
	if (!READ_ONCE(s->dying))
		mod_delayed_work(s->wq, &s->work, 0);
}

static int duplex_input_submit(struct rp1_duplex_input *in)
{
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;

	desc = dmaengine_prep_dma_cyclic(in->chan, in->ring_dma,
					 RP1_COUNTER_RING_WORDS * sizeof(*in->ring),
					 sizeof(*in->ring), DMA_DEV_TO_MEM,
					 DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -EIO;
	desc->callback = duplex_input_dma_done;
	desc->callback_param = in;
	cookie = dmaengine_submit(desc);
	return dma_submit_error(cookie);
}

static int duplex_input_release(struct rp1_extts *s, unsigned int index)
{
	struct rp1_duplex_input *in = &s->input[index];
	struct rp1_pio_sm_claim_args unclaim;
	int ret;

	if (in->sm >= 0 && s->pio) {
		ret = ext_sm_set_enabled_mask(s, BIT(in->sm), false);
		if (ret)
			return ret;
	}
	if (in->chan) {
		ret = dmaengine_terminate_sync(in->chan);
		if (ret)
			return ret;
		ret = rp1_pio_dma_release(s->pio, in->sm, PIO_DIR_FROM_SM, in->chan);
		if (ret)
			return ret;
		in->chan = NULL;
	}
	if (in->ring) {
		if (!in->dma_dev)
			return -EINVAL;
		dma_free_coherent(in->dma_dev,
				  RP1_COUNTER_RING_WORDS * sizeof(*in->ring),
				  in->ring, in->ring_dma);
		in->ring = NULL;
		in->ring_dma = 0;
	}
	kfree(in->pending);
	in->pending = NULL;
	if (in->dma_dev) {
		put_device(in->dma_dev);
		in->dma_dev = NULL;
	}
	if (in->sm >= 0 && s->pio) {
		if (in->saved_dma_valid) {
			ret = ext_dmactrl_sm(s, in->sm, in->saved_dma);
			if (ret)
				return ret;
			in->saved_dma_valid = false;
		}
		unclaim.mask = BIT(in->sm);
		ret = rp1_pio_sm_unclaim(s->pio, &unclaim);
		if (ret < 0)
			return ret;
		in->sm = -1;
	}
	if (in->pin) {
		ret = rp1_pinctrl_pio_release(in->pin);
		if (ret)
			return ret;
		in->pin = NULL;
	}
	in->prepared = false;
	in->started = false;
	in->gpio = -1;
	in->consumed = 0;
	in->samples = 0;
	in->overruns = 0;
	in->sequence = 0;
	in->wraps = 0;
	in->have_last_y = false;
	in->pending_head = 0;
	in->pending_tail = 0;
	atomic64_set(&in->produced, 0);
	return 0;
}

static int duplex_input_prepare(struct rp1_extts *s, unsigned int index,
				int gpio)
{
	struct rp1_duplex_input *in = &s->input[index];
	struct rp1_pio_sm_claim_args claim = {};
	struct rp1_pio_sm_init_args init = {};
	struct rp1_pio_sm_set_pindirs_args dirs = {};
	struct rp1_pio_sm_clear_fifos_args clear = {};
	struct rp1_pio_sm_get_dmactrl_args old_dma = {};
	struct rp1_pio_sm_exec_args exec = {};
	struct device_node *provider_node;
	bool synchronized;
	int ret;

	if (in->prepared || !s->monitor_program_loaded || s->monitor_offset < 0)
		return -EBUSY;
	{
		unsigned int i;

		if (gpio == s->gpio)
			return -EBUSY;
		for (i = 0; i < RP1_MAX_EXTTS; i++)
			if (i != index && s->input[i].prepared &&
			    gpio == s->input[i].gpio)
				return -EBUSY;
	}
	provider_node = of_parse_phandle(s->bp->pdev->dev.of_node,
					 "raspberrypi,gpio-controller", 0);
	if (!provider_node)
		return -EINVAL;
	in->pin = rp1_pinctrl_pio_request(&s->bp->pdev->dev, provider_node, gpio);
	of_node_put(provider_node);
	if (IS_ERR(in->pin)) {
		ret = PTR_ERR(in->pin);
		in->pin = NULL;
		return ret;
	}
	in->parent = s;
	in->gpio = gpio;
	ret = rp1_pio_gpio_is_synchronized(s->pio, gpio, &synchronized);
	if (ret || !synchronized)
		return ret ? ret : -EOPNOTSUPP;
	ret = rp1_pio_sm_claim(s->pio, &claim);
	if (ret < 0)
		return ret;
	in->sm = ret;
	init.sm = in->sm;
	init.initial_pc = s->monitor_offset;
	init.config.clkdiv = 1 << 16;
	init.config.execctrl = (s->monitor_offset << 7) |
		((s->monitor_offset + RP1_COUNTER_PROGRAM_WORDS - 1) << 12) |
		(gpio << 24);
	init.config.shiftctrl = BIT(18) | BIT(19);
	ret = rp1_pio_sm_init(s->pio, &init);
	if (ret < 0)
		return ret;
	dirs.sm = in->sm;
	dirs.mask = BIT(gpio);
	ret = rp1_pio_sm_set_pindirs(s->pio, &dirs);
	if (ret < 0)
		return ret;
	clear.sm = in->sm;
	ret = rp1_pio_sm_clear_fifos(s->pio, &clear);
	if (ret < 0)
		return ret;
	old_dma.sm = in->sm;
	ret = rp1_pio_sm_get_dmactrl(s->pio, &old_dma);
	if (ret < 0)
		return ret;
	in->saved_dma = old_dma.ctrl;
	in->saved_dma_valid = true;
	ret = ext_dmactrl_sm(s, in->sm, 0x80000101);
	if (ret)
		return ret;
	exec.sm = in->sm;
	exec.instr = 0xa04b; /* MOV Y, ~NULL */
	ret = rp1_pio_sm_exec(s->pio, &exec);
	if (ret < 0)
		return ret;
	in->chan = rp1_pio_dma_request(s->pio, in->sm, PIO_DIR_FROM_SM);
	if (IS_ERR(in->chan)) {
		ret = PTR_ERR(in->chan);
		in->chan = NULL;
		return ret;
	}
	in->dma_dev = in->chan->device->dev;
	get_device(in->dma_dev);
	in->ring = dma_alloc_coherent(in->dma_dev,
				      RP1_COUNTER_RING_WORDS * sizeof(*in->ring),
			&in->ring_dma, GFP_KERNEL);
	if (!in->ring)
		return -ENOMEM;
	memset(in->ring, 0, RP1_COUNTER_RING_WORDS * sizeof(*in->ring));
	in->pending = kcalloc(RP1_COUNTER_RING_WORDS,
			      sizeof(*in->pending), GFP_KERNEL);
	if (!in->pending)
		return -ENOMEM;
	atomic64_set(&in->produced, 0);
	in->prepared = true;
	in->error = 0;
	return 0;
}

static int duplex_monitor_submit(struct rp1_extts *s)
{
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	int ret;

	desc = dmaengine_prep_dma_cyclic(s->monitor_chan, s->monitor_ring_dma,
					 RP1_COUNTER_RING_WORDS * sizeof(*s->monitor_ring),
					 sizeof(*s->monitor_ring), DMA_DEV_TO_MEM,
					 DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -EIO;
	desc->callback = out_monitor_dma_done;
	desc->callback_param = s;
	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	return ret;
}

static int duplex_inputs_resync(struct rp1_extts *s, bool *disrupted)
{
	struct rp1_pio_sm_enable_sync_args enable = {};
	struct rp1_pio_sm_clear_fifos_args clear = {};
	struct rp1_pio_sm_exec_args exec = {};
	u16 mask = 0;
	u64 now;
	unsigned int i;
	int ret;

	*disrupted = false;
	if (s->monitor_sm < 0 || !s->monitor_chan || !s->monitor_ring)
		return -ENODEV;
	ret = ext_now(s, &now);
	if (ret)
		return ret;
	if (s->out_target <= now + 50000000ULL)
		return -ETIME;
	mask |= BIT(s->monitor_sm);
	for (i = 0; i < RP1_MAX_EXTTS; i++)
		if (s->input[i].prepared)
			mask |= BIT(s->input[i].sm);
	/* A failed firmware call may still have changed part of the mask. */
	*disrupted = true;
	ret = ext_sm_set_enabled_mask(s, mask, false);
	if (ret)
		return ret;
	s->monitor_started = false;
	for (i = 0; i < RP1_MAX_EXTTS; i++)
		s->input[i].started = false;
	ret = dmaengine_terminate_sync(s->monitor_chan);
	if (ret)
		return ret;
	for (i = 0; i < RP1_MAX_EXTTS; i++) {
		struct rp1_duplex_input *in = &s->input[i];

		if (!in->prepared)
			continue;
		ret = dmaengine_terminate_sync(in->chan);
		if (ret)
			return ret;
	}
	for (i = 0; i <= RP1_MAX_EXTTS; i++) {
		int sm = i ? s->input[i - 1].sm : s->monitor_sm;

		if (i && !s->input[i - 1].prepared)
			continue;
		clear.sm = sm;
		ret = rp1_pio_sm_clear_fifos(s->pio, &clear);
		if (ret < 0)
			return ret;
		exec.sm = sm;
		exec.instr = 0xa04b;
		ret = rp1_pio_sm_exec(s->pio, &exec);
		if (ret < 0)
			return ret;
	}
	memset(s->monitor_ring, 0,
	       RP1_COUNTER_RING_WORDS * sizeof(*s->monitor_ring));
	atomic64_set(&s->monitor_produced, 0);
	s->monitor_consumed = 0;
	s->monitor_samples = 0;
	s->monitor_overruns = 0;
	s->monitor_last = 0;
	s->monitor_wraps = 0;
	s->monitor_have_last_y = false;
	s->anchor_sequence = 0;
	s->anchor_out_base = s->out_count;
	memset(s->anchors, 0, sizeof(s->anchors));
	ret = duplex_monitor_submit(s);
	if (ret)
		return ret;
	for (i = 0; i < RP1_MAX_EXTTS; i++) {
		struct rp1_duplex_input *in = &s->input[i];

		if (!in->prepared)
			continue;
		memset(in->ring, 0,
		       RP1_COUNTER_RING_WORDS * sizeof(*in->ring));
		atomic64_set(&in->produced, 0);
		in->consumed = 0;
		in->samples = 0;
		in->overruns = 0;
		in->sequence = 0;
		in->wraps = 0;
		in->have_last_y = false;
		in->pending_head = 0;
		in->pending_tail = 0;
		in->last_event_ns = 0;
		ret = duplex_input_submit(in);
		if (ret)
			return ret;
	}
	dma_async_issue_pending(s->monitor_chan);
	for (i = 0; i < RP1_MAX_EXTTS; i++)
		if (s->input[i].prepared)
			dma_async_issue_pending(s->input[i].chan);
	enable.mask = mask;
	ret = rp1_pio_sm_enable_sync(s->pio, &enable);
	if (ret < 0) {
		dmaengine_terminate_sync(s->monitor_chan);
		for (i = 0; i < RP1_MAX_EXTTS; i++)
			if (s->input[i].prepared)
				dmaengine_terminate_sync(s->input[i].chan);
		return ret;
	}
	s->monitor_started = true;
	s->monitor_error = 0;
	for (i = 0; i < RP1_MAX_EXTTS; i++)
		s->input[i].started = s->input[i].prepared;
	wake_up_all(&s->monitor_wait);
	return 0;
}

static struct rp1_counter_anchor *duplex_anchor_get(struct rp1_extts *s,
						    u64 sequence)
{
	struct rp1_counter_anchor *anchor =
		&s->anchors[sequence % OUT_RECORDS];

	if (anchor->sequence != sequence) {
		memset(anchor, 0, sizeof(*anchor));
		anchor->sequence = sequence;
	}
	return anchor;
}

static void duplex_anchor_counter(struct rp1_extts *s, u64 sequence,
				  u32 y, u64 wraps)
{
	struct rp1_counter_anchor *anchor;

	if (!sequence)
		return;
	anchor = duplex_anchor_get(s, sequence);
	anchor->counter_sequence = sequence;
	anchor->wraps = wraps;
	anchor->y = y;
	anchor->counter_valid = true;
	s->anchor_sequence = max(s->anchor_sequence, sequence);
}

static void duplex_anchor_marker(struct rp1_extts *s, u64 output_sequence,
				 u64 stamp)
{
	struct rp1_counter_anchor *anchor;
	u64 sequence;

	if (!s->monitor_started || output_sequence <= s->anchor_out_base)
		return;
	sequence = output_sequence - s->anchor_out_base;
	anchor = duplex_anchor_get(s, sequence);
	anchor->stamp = stamp;
	anchor->rate = s->out_rate;
	anchor->marker_valid = true;
}

static struct rp1_counter_anchor *duplex_latest_anchor(struct rp1_extts *s)
{
	struct rp1_counter_anchor *latest = NULL;
	unsigned int i;

	for (i = 0; i < OUT_RECORDS; i++) {
		struct rp1_counter_anchor *anchor = &s->anchors[i];

		if (!anchor->counter_valid || !anchor->marker_valid ||
		    anchor->sequence != anchor->counter_sequence)
			continue;
		if (!latest || anchor->stamp > latest->stamp)
			latest = anchor;
	}
	return latest;
}

static int duplex_map_input_sample(struct rp1_extts *s,
				   struct rp1_duplex_input *in,
				   const struct rp1_input_sample *sample,
				   u64 now, u64 *stamp, u64 *uncertainty)
{
	struct rp1_counter_rate_change *changes;
	struct rp1_counter_anchor *anchor = duplex_latest_anchor(s);
	u64 start, i;
	unsigned int count = 0;
	s64 cycle_delta;
	u64 oldest;
	int ret;

	if (!anchor)
		return -EAGAIN;
	if (anchor->stamp > now || !anchor->rate)
		return -ESTALE;
	if (sample->sequence > RP1_S64_MAX ||
	    anchor->counter_sequence > RP1_S64_MAX)
		return -ERANGE;
	if (rp1_counter_signed_cycle_delta(anchor->y,
					   anchor->counter_sequence,
					  anchor->wraps, sample->y,
					  sample->sequence, sample->wraps,
					  &cycle_delta))
		return -ESTALE;
	changes = kcalloc(OUT_RECORDS, sizeof(*changes), GFP_KERNEL);
	if (!changes)
		return -ENOMEM;
	start = s->rate_count > OUT_RECORDS ? s->rate_count - OUT_RECORDS : 0;
	start = max(start, s->out_rate_epoch);
	for (i = start; i < s->rate_count; i++) {
		struct rate_record *record = &s->rate_records[i % OUT_RECORDS];
		struct rp1_counter_rate_change *change;

		if (record->sequence != i + 1) {
			ret = -ESTALE;
			goto out_free;
		}
		if (cycle_delta >= 0) {
			if (record->before < anchor->stamp) {
				if (record->after > anchor->stamp) {
					ret = -ESTALE;
					goto out_free;
				}
				continue;
			}
			if (record->after > now)
				continue;
		} else {
			if (record->after > anchor->stamp) {
				if (record->before < anchor->stamp) {
					ret = -ESTALE;
					goto out_free;
				}
				continue;
			}
		}
		if (count == OUT_RECORDS) {
			ret = -E2BIG;
			goto out_free;
		}
		change = &changes[count++];
		change->before_ns = record->before;
		change->after_ns = record->after;
		change->old_rate = record->old_rate;
		change->new_rate = record->new_rate;
	}
	oldest = anchor->stamp > 5000000000ULL ? anchor->stamp - 5000000000ULL : 0;
	oldest = max(oldest, s->rate_history_floor);
	ret = rp1_counter_sample_to_phc(anchor->stamp, anchor->rate,
					anchor->y, anchor->counter_sequence,
					 anchor->wraps, sample->y,
					 sample->sequence, sample->wraps,
					 oldest, now, changes, count, stamp,
					 uncertainty);
out_free:
	kfree(changes);
	return ret;
}

static int duplex_input_publish_pending(struct rp1_extts *s,
					unsigned int index)
{
	struct rp1_duplex_input *in = &s->input[index];
	u64 now;
	int ret;

	if (!in->prepared || !in->started)
		return 0;
	while (in->pending_head < in->pending_tail) {
		struct rp1_input_sample *sample =
			&in->pending[in->pending_head % RP1_COUNTER_RING_WORDS];
		struct ptp_clock_event ev = {
			.type = PTP_CLOCK_EXTTS,
			.index = index,
		};
		u64 uncertainty;

		ret = ext_now(s, &now);
		if (ret)
			return ret;
		ret = duplex_map_input_sample(s, in, sample, now,
					      &ev.timestamp, &uncertainty);
		if (ret == -EAGAIN)
			return 0;
		if (ret)
			return ret;
		if (ev.timestamp <= in->last_event_ns)
			return -ESTALE;
		ptp_clock_event(s->bp->ptp_clock, &ev);
		in->last_event_ns = ev.timestamp;
		in->events++;
		in->max_uncertainty_ns = max(in->max_uncertainty_ns, uncertainty);
		in->pending_head++;
	}
	return 0;
}

static int duplex_input_drain(struct rp1_extts *s, unsigned int index)
{
	struct rp1_duplex_input *in = &s->input[index];
	u64 produced = atomic64_read(&in->produced), first;
	u32 available;
	int ret;

	if (!in->started || !in->ring)
		return 0;
	ret = rp1_counter_ring_window(produced, in->consumed,
				      RP1_COUNTER_RING_WORDS, &first, &available);
	if (ret == -EOVERFLOW) {
		in->overruns += first - in->consumed;
		in->consumed = first;
		return ret;
	}
	if (ret)
		return ret;
	while (available--) {
		u32 y;
		struct rp1_input_sample *sample;

		dma_rmb();
		y = READ_ONCE(in->ring[in->consumed % RP1_COUNTER_RING_WORDS]);
		if (in->pending_tail - in->pending_head >= RP1_COUNTER_RING_WORDS)
			return -EOVERFLOW;
		if (in->have_last_y && y > in->last_y)
			in->wraps++;
		in->sequence = in->consumed + 1;
		sample = &in->pending[in->pending_tail % RP1_COUNTER_RING_WORDS];
		sample->sequence = in->sequence;
		sample->wraps = in->wraps;
		sample->y = y;
		in->pending_tail++;
		in->last_y = y;
		in->have_last_y = true;
		in->consumed++;
		in->samples++;
	}
	return duplex_input_publish_pending(s, index);
}

static int duplex_input_enable(struct rp1_extts *s, unsigned int index,
			       unsigned int edges, int on)
{
	struct rp1_duplex_input *in;
	bool disrupted;
	int gpio, ret;

	if (index >= RP1_MAX_EXTTS)
		return -EOPNOTSUPP;
	in = &s->input[index];
	if (!on) {
		ret = duplex_input_release(s, index);
		if (!ret) {
			in->requested = false;
			in->requested_gpio = -1;
			in->requested_edges = 0;
		}
		return ret;
	}
	if (edges != PTP_RISING_EDGE)
		return -EOPNOTSUPP;
	if (!s->output || !s->enabled || !s->monitor_started)
		return -EOPNOTSUPP;
	if (in->prepared)
		return in->gpio == ptp_find_pin(s->bp->ptp_clock,
						 PTP_PF_EXTTS, index) ? 0 : -EBUSY;
	gpio = ptp_find_pin(s->bp->ptp_clock, PTP_PF_EXTTS, index);
	if (gpio < 0 || gpio >= EXT_GPIO_COUNT)
		return -EINVAL;
	ret = duplex_input_prepare(s, index, gpio);
	if (ret)
		goto fail;
	ret = duplex_inputs_resync(s, &disrupted);
	if (ret) {
		if (disrupted) {
			/* Resync has stopped the shared counter epoch. Releasing only the
			 * new lane would leave existing lanes registered but silent.
			 */
			ext_fail(s, ret);
			return ret;
		}
		goto fail;
	}
	in->requested = true;
	in->requested_gpio = gpio;
	in->requested_edges = edges;
	return 0;
fail:
	if (duplex_input_release(s, index))
		dev_err(&s->bp->pdev->dev,
			"RP1 EXTS%u cleanup retained resources after error %d\n",
			index, ret);
	return ret;
}

static void ext_fail(struct rp1_extts *s, int err)
{
	s->last_error = err;
	s->faults++;
	s->enabled = false;
	wake_up_all(&s->monitor_wait);
	if (ext_release(s))
		netdev_err(s->bp->dev, "RP1 cleanup retained resources after error %d\n",
			   s->last_error);
	netdev_warn(s->bp->dev, "RP1 %s stopped (%d), enable the channel to retry\n",
		    s->output ? "PEROUT" : "EXTTS", err);
}

static void ext_done(void *arg)
{
	struct rp1_extts *s = arg;

	atomic_set(&s->done, 1);
	if (!READ_ONCE(s->dying))
		mod_delayed_work(s->wq, &s->work, 0);
}

static int ext_arm(struct rp1_extts *s)
{
	struct dma_async_tx_descriptor *desc;
	struct rp1_pio_sm_put_args put = { .sm = s->sm, .data = 0 };
	struct rp1_pio_sm_fifo_state_args fifo = { .sm = s->sm };
	u64 now;
	int ret;

	if (readl(s->control))
		return -EBUSY;
	ret = rp1_pio_sm_fifo_state(s->pio, &fifo);
	if (ret < 0 || fifo.level)
		return ret < 0 ? ret : -ESTALE;
	ret = ext_now(s, &now);
	if (ret)
		return ret;
	desc = dmaengine_prep_slave_single(s->chan, s->target, 4,
					   DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -EIO;
	desc->callback = ext_done;
	desc->callback_param = s;
	s->cookie = dmaengine_submit(desc);
	ret = dma_submit_error(s->cookie);
	if (ret)
		return ret;
	atomic_set(&s->done, 0);
	s->armed_at = now;
	s->armed = true;
	writel(6, s->control);
	if (readl(s->control) != 6)
		return -EIO;
	mod_delayed_work(s->wq, &s->work, msecs_to_jiffies(EXT_WATCHDOG_MS));
	dma_wmb();
	dma_async_issue_pending(s->chan);
	if (s->output)
		return 0;
	/* Allow a new edge only after its DMA is ready. No queued old tokens. */
	ret = rp1_pio_sm_put(s->pio, &put);
	if (ret < 0)
		return ret;
	return 0;
}

static void ext_work(struct work_struct *work)
{
	struct rp1_extts *s = container_of(to_delayed_work(work), struct rp1_extts, work);
	struct ptp_clock_event ev = { .type = PTP_CLOCK_EXTTS, .index = 0 };
	u64 stamp, now, rearmed;
	int ret;

	mutex_lock(&s->lock);
	if (s->monitor_started) {
		ret = out_monitor_drain(s);
		if (ret)
			s->monitor_error = ret;
		if (s->monitor_error) {
			unsigned int i;

			for (i = 0; i < RP1_MAX_EXTTS; i++)
				if (s->input[i].prepared) {
					ext_fail(s, s->monitor_error);
					goto out;
				}
		}
	}
	{
		unsigned int i;

		for (i = 0; i < RP1_MAX_EXTTS; i++) {
			ret = duplex_input_drain(s, i);
			if (ret) {
				s->input[i].error = ret;
				ext_fail(s, ret);
				goto out;
			}
		}
	}
	if (s->dying || !s->enabled || !s->armed)
		goto out;
	if (!atomic_read(&s->done)) {
		if (dma_async_is_tx_complete(s->chan, s->cookie, NULL, NULL) == DMA_ERROR) {
			ext_fail(s, -EIO);
			goto out;
		}
		if (s->output) {
			ret = ext_now(s, &now);
			if (ret || now > s->out_target + 200000000ULL) {
				s->timeouts++;
				ext_fail(s, ret ? ret : -ETIMEDOUT);
				goto out;
			}
		}
		/* An absent source is idle, not a fabricated timestamp or error. */
		mod_delayed_work(s->wq, &s->work, msecs_to_jiffies(EXT_WATCHDOG_MS));
		goto out;
	}
	if (dma_async_is_tx_complete(s->chan, s->cookie, NULL, NULL) != DMA_COMPLETE ||
	    readl(s->control)) {
		ext_fail(s, -EIO);
		goto out;
	}
	s->armed = false;
	ret = ext_stamp(s, &stamp);
	if (!ret)
		ret = ext_now(s, &now);
	if (ret || stamp < s->armed_at || stamp > now ||
	    (s->last_stamp && stamp <= s->last_stamp) ||
	    (!s->output && stamp < EXT_INPUT_BIAS_NS)) {
		ext_fail(s, ret ? ret : -ESTALE);
		goto out;
	}
	s->last_age = now - stamp;
	s->max_age = max(s->max_age, s->last_age);
	if (s->last_age > EXT_MAX_AGE_NS) {
		s->timeouts++;
		ext_fail(s, -ETIMEDOUT);
		goto out;
	}
	s->last_stamp = stamp;
	if (s->output) {
		ret = out_event(s, stamp, now);
		if (!ret) {
			unsigned int i;

			for (i = 0; i < RP1_MAX_EXTTS; i++) {
				ret = duplex_input_publish_pending(s, i);
				if (ret)
					break;
			}
		}
		if (ret)
			ext_fail(s, ret);
		goto out;
	}
	s->events++;
	ev.timestamp = stamp - EXT_INPUT_BIAS_NS;
	ptp_clock_event(s->bp->ptp_clock, &ev);
	ret = ext_arm(s);
	if (!ret && !ext_now(s, &rearmed) && rearmed >= stamp) {
		s->last_deadtime = rearmed - stamp;
		s->max_deadtime = max(s->max_deadtime, s->last_deadtime);
	}
	if (ret)
		ext_fail(s, ret);
out:
	mutex_unlock(&s->lock);
}

static int ext_pin_get(struct rp1_extts *s)
{
	struct device_node *provider_node;

	provider_node = of_parse_phandle(s->bp->pdev->dev.of_node,
					 "raspberrypi,gpio-controller", 0);
	if (!provider_node)
		return -EINVAL;
	s->pin = rp1_pinctrl_pio_request(&s->bp->pdev->dev, provider_node,
					 s->gpio);
	of_node_put(provider_node);
	if (IS_ERR(s->pin)) {
		int ret = PTR_ERR(s->pin);

		s->pin = NULL;
		return ret;
	}

	return 0;
}

static int out_add_program_at_origin(struct rp1_extts *s,
				     const u16 *instructions, unsigned int words)
{
	struct rp1_pio_add_program_args prog = { .num_instrs = words };
	unsigned int origin;

	if (!words || words > RP1_PIO_INSTRUCTION_COUNT)
		return -EINVAL;

	for (origin = 0; origin <= RP1_PIO_INSTRUCTION_COUNT - words; origin++) {
		int ret;

		prog.origin = origin;
		/* PIO_ADD_PROGRAM relocates JMP instructions in firmware. */
		memcpy(prog.instrs, instructions, words * sizeof(prog.instrs[0]));

		ret = rp1_pio_can_add_program(s->pio, &prog);
		if (ret < 0)
			return ret;
		if (!ret)
			continue;
		ret = rp1_pio_add_program(s->pio, &prog);
		if (ret < 0)
			return ret;
		if (ret != origin) {
			struct rp1_pio_remove_program_args remove = {
				.num_instrs = words,
				.origin = ret,
			};
			int remove_ret = rp1_pio_remove_program(s->pio, &remove);

			return remove_ret < 0 ? remove_ret : -EPROTO;
		}
		return ret;
	}

	return -ENOSPC;
}

static int ext_acquire(struct rp1_extts *s)
{
	struct rp1_pio_sm_claim_args claim = {};
	struct rp1_pio_add_program_args prog = { .origin = RP1_PIO_ORIGIN_ANY };
	struct rp1_pio_sm_init_args init = {};
	struct rp1_pio_sm_set_pindirs_args dirs = {};
	struct rp1_pio_sm_get_dmactrl_args dma = {};
	struct device_node *provider_node;
	bool synchronized;
	int ret;

	if (s->pin || s->pio || s->chan || s->mapped)
		return -EBUSY;
	if (!s->control || readl(s->control))
		return -EBUSY;
	ret = ext_pin_get(s);
	if (ret)
		return ret;
	provider_node = of_parse_phandle(s->bp->pdev->dev.of_node,
					 "raspberrypi,pio", 0);
	if (!provider_node)
		return -EINVAL;
	s->pio = rp1_pio_open_for_device(&s->bp->pdev->dev, provider_node);
	of_node_put(provider_node);
	if (IS_ERR(s->pio)) {
		ret = PTR_ERR(s->pio);
		s->pio = NULL;
		return ret;
	}
	ret = rp1_pio_gpio_is_synchronized(s->pio, s->gpio, &synchronized);
	if (ret || (!s->output && !synchronized))
		return ret ? ret : -EOPNOTSUPP;
	s->sm = rp1_pio_sm_claim(s->pio, &claim);
	if (s->sm < 0)
		return s->sm;
	ret = ext_sm_enable(s, false);
	if (ret)
		return ret;
	dma.sm = s->sm;
	ret = rp1_pio_sm_get_dmactrl(s->pio, &dma);
	if (ret < 0)
		return ret;
	s->saved_dma = dma.ctrl;
	s->saved_dma_valid = true;
	ret = ext_dmactrl(s, s->saved_dma & ~BIT(31));
	if (ret)
		return ret;
	/* WAIT GPIO uses the absolute GPIO, independent of IN_BASE. */
	if (s->edge == PTP_EXTTS_EDGES) {
		/* Alternate WAIT LOW and WAIT HIGH to capture every transition. */
		const u16 program[] = {
			0x80a0, 0x2000 | s->gpio, 0xa0c3, 0x8020,
			0x80a0, 0x2080 | s->gpio, 0xa0c3, 0x8020,
		};

		prog.num_instrs = ARRAY_SIZE(program);
		memcpy(prog.instrs, program, sizeof(program));
	} else {
		/* PULL; WAIT opposite level; WAIT selected level; MOV; PUSH. */
		prog.num_instrs = 5;
		prog.instrs[0] = 0x80a0;
		prog.instrs[1] = 0x2000 |
			(s->edge == PTP_FALLING_EDGE ? 0x80 : 0) | s->gpio;
		prog.instrs[2] = 0x2000 |
			(s->edge == PTP_RISING_EDGE ? 0x80 : 0) | s->gpio;
		prog.instrs[3] = 0xa0c3;
		prog.instrs[4] = 0x8020;
	}
	if (s->output) {
		/* Startup -> checkpoint: G0+C+7. HIGH: H+3.
		 * Edge -> next checkpoint: H+C+10 (unless a LOW PULL stalls).
		 * Checkpoint -> edge: 9+4*(UINT_MAX-deadline) cycles.
		 * ISR holds HIGH; Y counts down; X is a live-replaceable deadline.
		 * Only C,H are queued outside the fine loop. While counting, every
		 * TX word is a replacement deadline; empty PULL preserves X.
		 */
		const u16 program[] = {
			0x80a0, 0xa027, 0xa0c3, 0x8020, 0x0044,
			0x80a0, 0xa027, 0x0047, 0x80a0, 0xa0c3, 0x8020,
			0xa0c7, 0xa04b, 0xa023, 0x8080, 0xa027, 0x00b8,
			0xa046, 0xa0c3, 0x9000, 0xb022, 0x1055, 0xa042,
			0x0005, 0x008e, 0x000e,
		};
		BUILD_BUG_ON(ARRAY_SIZE(program) != RP1_OUTPUT_STARTUP_PROGRAM_WORDS);
		prog.num_instrs = ARRAY_SIZE(program);
		memcpy(prog.instrs, program, sizeof(program));
	}
	s->words = prog.num_instrs;
	s->offset = s->output ?
		out_add_program_at_origin(s, prog.instrs, prog.num_instrs) :
		rp1_pio_add_program(s->pio, &prog);
	if (s->offset < 0)
		return s->offset;
	if (s->output)
		s->out_startup_origin = s->offset;
	ext_config(s, &init);
	ret = rp1_pio_sm_init(s->pio, &init);
	if (ret < 0)
		return ret;
	dirs.sm = s->sm;
	dirs.mask = BIT(s->gpio);
	ret = rp1_pio_sm_set_pindirs(s->pio, &dirs);
	if (ret < 0)
		return ret;
	ret = ext_dmactrl(s, 0x80000101);
	if (ret)
		return ret;
	s->chan = rp1_pio_dma_request(s->pio, s->sm, PIO_DIR_FROM_SM);
	if (IS_ERR(s->chan)) {
		ret = PTR_ERR(s->chan);
		s->chan = NULL;
		return ret;
	}
	s->target = dma_map_resource(s->chan->device->dev, s->control_phys, 4,
				     DMA_FROM_DEVICE, 0);
	if (dma_mapping_error(s->chan->device->dev, s->target))
		return -EIO;
	s->mapped = true;
	if (s->target + 3 > dma_get_mask(s->chan->device->dev) ||
	    (s->chan->device->dev->bus_dma_limit &&
	     s->target + 3 > s->chan->device->dev->bus_dma_limit))
		return -ERANGE;
	return 0;
}

static int ext_start(struct rp1_extts *s)
{
	struct rp1_pio_sm_init_args init = { .sm = s->sm, .initial_pc = s->offset };
	struct rp1_pio_sm_set_pindirs_args dirs = { .sm = s->sm, .mask = BIT(s->gpio) };
	struct rp1_pio_sm_clear_fifos_args clear = { .sm = s->sm };
	int ret;

	if (s->output)
		return out_start(s);
	init.config.clkdiv = 1 << 16;
	init.config.execctrl = s->offset << 7 | (s->offset + s->words - 1) << 12;
	init.config.shiftctrl = BIT(18) | BIT(19);
	ret = rp1_pio_sm_init(s->pio, &init);
	if (ret < 0)
		return ret;
	ret = rp1_pio_sm_clear_fifos(s->pio, &clear);
	if (ret < 0)
		return ret;
	ret = rp1_pio_sm_set_pindirs(s->pio, &dirs);
	if (ret < 0)
		return ret;
	s->last_stamp = 0;
	ret = ext_dmactrl(s, 0x80000101);
	if (!ret)
		ret = ext_arm(s);
	if (!ret)
		ret = s->monitor_sm >= 0 ? out_monitor_start(s) :
			ext_sm_enable(s, true);
	return ret;
}

static u32 out_rate(struct rp1_extts *s)
{
	unsigned long flags;
	u32 ti, sub;

	spin_lock_irqsave(&s->bp->tsu_clk_lock, flags);
	ti = readl(s->bp->regs + GEM_TI);
	sub = readl(s->bp->regs + GEM_TISUBN);
	spin_unlock_irqrestore(&s->bp->tsu_clk_lock, flags);
	/* Alternate increments and another TSU source rate are not supported. */
	if ((ti & ~255U) || s->bp->tsu_rate != 50000000 || ti < 18 || ti > 22)
		return 0;
	return (ti << 24) | ((sub & 0xffff) << 8) | (sub >> 24);
}

static int out_put(struct rp1_extts *s, u32 value)
{
	struct rp1_pio_sm_put_args put = { .sm = s->sm, .data = value };
	struct rp1_pio_sm_fifo_state_args fifo = { .sm = s->sm, .tx = 1 };
	int ret = rp1_pio_sm_fifo_state(s->pio, &fifo);

	if (ret < 0 || fifo.full)
		return ret < 0 ? ret : -ENOSPC;
	ret = rp1_pio_sm_put(s->pio, &put);
	return ret < 0 ? ret : 0;
}

static int out_load_compact_program(struct rp1_extts *s)
{
	struct rp1_pio_remove_program_args remove = {
		.num_instrs = s->words,
		.origin = s->offset,
	};
	struct rp1_pio_sm_clear_fifos_args clear = { .sm = s->sm };
	struct rp1_pio_sm_set_pindirs_args dirs = {
		.sm = s->sm, .mask = BIT(s->gpio), .dirs = BIT(s->gpio),
	};
	struct rp1_pio_sm_init_args init = {};
	int ret;

	if (!s->pio || s->sm < 0 || s->offset < 0 || s->compact ||
	    s->words != RP1_OUTPUT_STARTUP_PROGRAM_WORDS)
		return -EINVAL;

	ret = ext_sm_enable(s, false);
	if (ret)
		return ret;
	ret = rp1_pio_sm_clear_fifos(s->pio, &clear);
	if (ret < 0)
		return ret;
	ret = rp1_pio_remove_program(s->pio, &remove);
	if (ret < 0)
		return ret;
	s->offset = -1;
	s->words = 0;

	ret = out_add_program_at_origin(s, rp1_compact_output_program,
					RP1_OUTPUT_PROGRAM_WORDS);
	if (ret < 0)
		return ret;
	s->offset = ret;
	s->out_compact_origin = ret;
	s->words = RP1_OUTPUT_PROGRAM_WORDS;
	s->compact = true;
	ext_config(s, &init);
	ret = rp1_pio_sm_init(s->pio, &init);
	if (ret < 0)
		return ret;
	ret = rp1_pio_sm_clear_fifos(s->pio, &clear);
	if (ret < 0)
		return ret;
	ret = rp1_pio_sm_set_pindirs(s->pio, &dirs);
	if (ret < 0)
		return ret;
	ret = ext_dmactrl(s, 0x80000101);
	if (ret)
		return ret;
	/* The output SM itself emits the TSU marker on its pad transition. */
	ret = out_monitor_prepare(s);
	if (ret) {
		int cleanup_ret = out_monitor_release(s);

		if (cleanup_ret)
			return cleanup_ret;
		s->monitor_error = ret;
		wake_up_all(&s->monitor_wait);
		dev_dbg(&s->bp->pdev->dev,
			"RP1 output pad monitor unavailable (%d); output remains active\n",
			ret);
	} else {
		s->monitor_error = 0;
	}
	return 0;
}

static int out_start(struct rp1_extts *s)
{
	struct rp1_pio_sm_init_args init = {};
	struct rp1_pio_sm_clear_fifos_args clear = { .sm = s->sm };
	struct rp1_pio_sm_set_pindirs_args dirs = {
		.sm = s->sm, .mask = BIT(s->gpio), .dirs = BIT(s->gpio),
	};
	unsigned int i;
	bool input_requested = false;
	u64 now;
	int ret;

	s->out_rate = out_rate(s);
	if (!s->out_rate)
		return -EOPNOTSUPP;
	s->out_high = out_ns_cycles(s->out_width, s->out_rate) - 3;
	ret = out_inhibit(s);
	if (ret)
		return ret;
	ext_config(s, &init);
	ret = rp1_pio_sm_init(s->pio, &init);
	if (ret < 0)
		return ret;
	ret = rp1_pio_sm_clear_fifos(s->pio, &clear);
	if (ret < 0)
		return ret;
	ret = rp1_pio_sm_set_pindirs(s->pio, &dirs);
	if (ret < 0)
		return ret;
	ret = ext_now(s, &now);
	if (ret || s->out_first < now + OUT_MIN_LEAD_NS)
		return ret ? ret : -ETIME;
	s->calibrating = true;
	s->last_stamp = 0;
	s->map_valid = false;
	s->out_rate_epoch = s->rate_count;
	s->rate_history_floor = 0;
	s->out_target = now; /* Watchdog for the immediate, invisible anchor. */
	ret = ext_dmactrl(s, 0x80000101);
	if (!ret)
		ret = out_put(s, OUT_GUARD_CYCLES);
	if (!ret)
		ret = ext_arm(s);
	if (!ret && s->monitor_sm >= 0) {
		ret = out_monitor_start(s);
	} else if (!ret && s->compact) {
		for (i = 0; i < RP1_MAX_EXTTS; i++)
			input_requested |= s->input[i].requested;
		if (input_requested)
			ret = s->monitor_error ?: -ENODEV;
		else
			ret = ext_sm_enable(s, true);
	} else if (!ret) {
		ret = ext_sm_enable(s, true);
	}
	return ret;
}

/* These helpers only perform arithmetic or ordered MMIO. No firmware RPC.
 * s->lock serializes updates against timestamp collection and time steps.
 */
static int out_map_advance(struct rp1_extts *s, u64 at, u32 rate)
{
	if (at < s->map_at || at - s->map_at > 100000000ULL || !s->map_rate)
		return -ERANGE;
	s->map_cycles_fp += out_ns_cycles_fp(at - s->map_at, s->map_rate);
	s->map_at = at;
	s->map_rate = rate;
	return 0;
}

static int out_map_seed(struct rp1_extts *s, u64 stamp)
{
	u64 i, start = s->rate_count > OUT_RECORDS ? s->rate_count - OUT_RECORDS : 0;
	bool found = false;
	int ret;

	start = max(start, s->out_rate_epoch);
	if (stamp < s->rate_history_floor)
		return -ESTALE;
	s->map_at = stamp;
	s->map_cycles_fp = 0;
	s->map_rate = s->out_rate;
	/* Include updates between this frozen checkpoint and its collection. */
	for (i = start; i < s->rate_count; i++) {
		struct rate_record *r = &s->rate_records[i % OUT_RECORDS];
		u64 mid = r->before + (r->after - r->before) / 2;

		if (mid < stamp)
			continue;
		if (!found) {
			s->map_rate = r->old_rate;
			found = true;
		}
		if (r->old_rate != s->map_rate)
			return -ESTALE;
		ret = out_map_advance(s, mid, r->new_rate);
		if (ret)
			return ret;
	}
	return s->map_rate == s->out_rate ? 0 : -ESTALE;
}

static int out_send_deadline(struct rp1_extts *s, u64 *sent, u64 *drained)
{
	u64 cycles_fp, deadline = s->out_target + EXT_OUTPUT_BIAS_NS;
	int ret = ext_now(s, sent);

	if (ret)
		return ret;
	/* No word may escape the fine loop and be mistaken for a coarse count.
	 * Include the previously programmed match when a new rate delays it.
	 */
	if (deadline < *sent + OUT_UPDATE_GUARD_NS)
		return -ETIME;
	if (deadline <= s->map_at || deadline - s->map_at > 100000000ULL)
		return -ERANGE;
	cycles_fp = s->map_cycles_fp + out_ns_cycles_fp(deadline - s->map_at, s->map_rate);
	if (cycles_fp < ((u64)OUT_FINE_OFFSET_CYCLES << 16) ||
	    cycles_fp > ((u64)10000000 << 16))
		return -ERANGE;
	if (s->map_valid) {
		u64 old_fp = ((u64)OUT_FINE_OFFSET_CYCLES +
			      (u64)(OUT_COUNTER_SEED - s->out_deadline) *
			      OUT_FINE_TICK_CYCLES) << 16;
		u64 current_fp = s->map_cycles_fp +
			out_ns_cycles_fp(*sent - s->map_at, s->map_rate);

		if (old_fp < current_fp + out_ns_cycles_fp(OUT_UPDATE_GUARD_NS, s->map_rate))
			return -ETIME;
	}
	s->out_deadline = out_deadline_word(cycles_fp);
	ret = rp1_pio_sm_put_mmio(s->pio, s->sm, s->out_deadline);
	if (ret)
		return ret;
	/* A following ordered PHC read drains the posted write to this endpoint. */
	ret = ext_now(s, drained);
	if (ret || *drained < *sent || *drained >= deadline - OUT_UPDATE_GUARD_NS / 2)
		return ret ? ret : -ETIME;
	s->map_valid = true;
	return 0;
}

static int out_event(struct rp1_extts *s, u64 stamp, u64 now)
{
	u64 cycles, measured_now, sent, drained;
	unsigned int i;
	bool transitioned = false;
	bool input_requested = false;
	int ret;

	if (out_rate(s) != s->out_rate)
		return -ESTALE;
	if (s->calibrating) {
		if (s->out_first + EXT_OUTPUT_BIAS_NS < stamp + OUT_MIN_LEAD_NS)
			return -ETIME;
		cycles = out_ns_cycles(s->out_first + EXT_OUTPUT_BIAS_NS -
				       OUT_PREP_LEAD_NS - stamp, s->out_rate);
		if (cycles <= OUT_COMPACT_CHECKPOINT_CYCLES || cycles > U32_MAX)
			return -ERANGE;
		if (!s->compact) {
			ret = out_load_compact_program(s);
			if (ret)
				return ret;
			transitioned = true;
		}
		s->out_high = out_ns_cycles(s->out_width, s->out_rate) - 3;
		/* The compact program starts from word zero after the startup program
		 * is removed. Its first checkpoint is five cycles after its count.
		 * OUT_GUARD_CYCLES has already elapsed before this PHC anchor.
		 */
		ret = out_put(s, cycles - OUT_COMPACT_CHECKPOINT_CYCLES);
		if (!ret)
			ret = out_put(s, s->out_high);
		s->out_target = s->out_first;
		s->preparing = true;
		if (!ret)
			ret = ext_arm(s);
		if (!ret && transitioned) {
			if (s->monitor_sm >= 0) {
				ret = out_monitor_start(s);
			} else {
				for (i = 0; i < RP1_MAX_EXTTS; i++)
					input_requested |= s->input[i].requested;
				ret = input_requested ?
					(s->monitor_error ?: -ENODEV) :
					ext_sm_enable(s, true);
			}
		}
		if (!ret)
			ret = ext_now(s, &measured_now);
		if (ret || measured_now - stamp + 50000000ULL >=
		    out_cycles_ns(OUT_GUARD_CYCLES, s->out_rate))
			return ret ? ret : -ETIME;
		ret = rp1_pinctrl_pio_set_output(s->pin, true);
		if (ret)
			return ret;
		s->calibrating = false;
		s->out_anchors++;
		return 0;
	}
	if (s->preparing) {
		unsigned long flags;

		if (s->out_target + EXT_OUTPUT_BIAS_NS <= now + OUT_UPDATE_GUARD_NS ||
		    s->out_target + EXT_OUTPUT_BIAS_NS > stamp + 30000000ULL)
			return -ETIME;
		s->map_valid = false;
		ret = out_map_seed(s, stamp);
		if (!ret)
			ret = ext_arm(s);
		if (ret)
			return ret;
		local_irq_save(flags);
		ret = out_send_deadline(s, &sent, &drained);
		local_irq_restore(flags);
		if (ret)
			return ret;
		s->out_prepares++;
		s->out_prepare_stamp = stamp;
		s->out_prepare_age = drained - stamp;
		s->out_max_prepare_age = max(s->out_max_prepare_age, s->out_prepare_age);
		s->out_min_lead = min(s->out_min_lead,
				      s->out_target + EXT_OUTPUT_BIAS_NS - drained);
		s->preparing = false;
		return 0;
	}

	s->map_valid = false;
	s->out_last_latch = stamp;
	s->out_error = (s64)stamp - (s64)(s->out_target + EXT_OUTPUT_BIAS_NS);
	s->out_count++;
	if (s->out_count == 1) {
		s->out_min_error = s->out_error;
		s->out_max_error = s->out_error;
	}
	s->out_min_error = min(s->out_min_error, s->out_error);
	s->out_max_error = max(s->out_max_error, s->out_error);
	s->out_records[(s->out_count - 1) % OUT_RECORDS] = (struct out_record) {
		.sequence = s->out_count, .target = s->out_target,
		.stamp = stamp, .error = s->out_error,
	};
	duplex_anchor_marker(s, s->out_count, stamp);
	if (abs(s->out_error) > 100000)
		return -ERANGE;
	cycles = out_ns_cycles(s->out_target + NSEC_PER_SEC +
			       EXT_OUTPUT_BIAS_NS - OUT_PREP_LEAD_NS - stamp,
			       s->out_rate);
	if (cycles <= s->out_high + 10 || cycles > U32_MAX)
		return -ERANGE;
	cycles -= s->out_high + 10;
	s->out_high = out_ns_cycles(s->out_width, s->out_rate) - 3;
	s->out_target += NSEC_PER_SEC;
	s->preparing = true;
	ret = ext_arm(s);
	if (!ret)
		ret = out_put(s, cycles);
	if (!ret)
		ret = out_put(s, s->out_high);
	return ret;
}

static int out_enable(struct rp1_extts *s, struct ptp_perout_request *rq, int on)
{
	u64 now, start;
	u32 width = NSEC_PER_SEC / 2;
	int pin, ret = 0;

	if (rq->index)
		return -EOPNOTSUPP;
	if (on && (rq->flags & ~(PTP_PEROUT_PHASE | PTP_PEROUT_DUTY_CYCLE)))
		return -EOPNOTSUPP;
	if (on && (rq->period.sec != 1 || rq->period.nsec))
		return -EOPNOTSUPP;
	if (on && (rq->flags & PTP_PEROUT_DUTY_CYCLE)) {
		if (rq->on.sec || rq->on.nsec < 1000000 ||
		    rq->on.nsec > 500000000 || rq->on.nsec % 5)
			return -ERANGE;
		width = rq->on.nsec;
	}
	mutex_lock(&s->lock);
	if (s->dying) {
		ret = -ENODEV;
		goto out;
	}
	if (!on) {
		if (s->output) {
			if (s->input[1].requested || s->input[1].prepared) {
				ret = -EBUSY;
				goto out;
			}
			s->enabled = false;
			wake_up_all(&s->monitor_wait);
			ret = ext_release(s);
			if (ret)
				goto out;
			s->output = false;
			if (s->input[0].requested) {
				s->gpio = s->input[0].requested_gpio;
				s->edge = s->input[0].requested_edges;
				s->last_stamp = 0;
				ret = ext_acquire(s);
				if (!ret) {
					s->enabled = true;
					ret = ext_start(s);
				}
				if (ret) {
					s->enabled = false;
					if (ext_release(s))
						dev_err(&s->bp->pdev->dev,
							"RP1 EXTS0 restart cleanup failed\n");
				}
			}
		}
		goto out;
	}
	pin = ptp_find_pin(s->bp->ptp_clock, PTP_PF_PEROUT, 0);
	if (pin < 0 || pin >= EXT_GPIO_COUNT) {
		ret = -EINVAL;
		goto out;
	}
	if (s->enabled) {
		if (s->output || !s->input[0].requested ||
		    s->input[1].requested || s->input[0].requested_gpio == pin) {
			ret = -EBUSY;
			goto out;
		}
		/* Promote the direct one-channel input path into the duplex counter
		 * path. The requested GPIO and edge mode survive resource teardown.
		 */
		s->enabled = false;
		ret = ext_release(s);
		if (ret)
			goto out;
	}
	ret = ext_now(s, &now);
	if (ret)
		goto out;
	if (rq->flags & PTP_PEROUT_PHASE) {
		if (rq->phase.sec || rq->phase.nsec >= NSEC_PER_SEC || rq->phase.nsec % 5) {
			ret = -EINVAL;
			goto out;
		}
		start = out_next_grid(now + NSEC_PER_SEC, rq->phase.nsec);
	} else {
		if (rq->start.sec < 0 || rq->start.nsec >= NSEC_PER_SEC || rq->start.nsec % 5 ||
		    rq->start.sec > div_u64(U64_MAX - NSEC_PER_SEC, NSEC_PER_SEC)) {
			ret = -EINVAL;
			goto out;
		}
		start = (u64)rq->start.sec * NSEC_PER_SEC + rq->start.nsec;
		if (start < now + OUT_MIN_LEAD_NS || start > now + 10 * NSEC_PER_SEC) {
			ret = -ETIME;
			goto out;
		}
	}
	s->gpio = pin;
	s->output = true;
	s->out_first = start;
	s->out_phase = do_div(start, NSEC_PER_SEC);
	s->out_width = width;
	s->out_count = 0;
	s->out_anchors = 0;
	s->out_prepares = 0;
	s->out_prepare_stamp = 0;
	s->out_prepare_age = 0;
	s->out_max_prepare_age = 0;
	s->out_retargets = 0;
	s->out_late_skips = 0;
	s->out_max_update_ns = 0;
	s->out_error = 0;
	s->out_min_error = 0;
	s->out_max_error = 0;
	s->out_min_lead = U64_MAX;
	s->out_startup_origin = -1;
	s->out_compact_origin = -1;
	s->monitor_sm = -1;
	s->monitor_offset = -1;
	atomic64_set(&s->monitor_produced, 0);
	ret = ext_acquire(s);
	if (!ret) {
		s->enabled = true;
		ret = out_start(s);
	}
	if (ret) {
		s->enabled = false;
		if (ext_release(s))
			dev_err(&s->bp->pdev->dev,
				"RP1 PEROUT cleanup retained resources after error %d\n", ret);
	}
	s->last_error = ret;
out:
	mutex_unlock(&s->lock);
	return ret;
}

int rp1_extts_enable(struct macb *bp, struct ptp_clock_request *rq, int on)
{
	struct rp1_extts *s = bp->rp1_extts;
	unsigned int edges;
	int pin, ret = 0;

	if (s && rq->type == PTP_CLK_REQ_PEROUT)
		return out_enable(s, &rq->perout, on);
	if (!s || rq->type != PTP_CLK_REQ_EXTTS ||
	    rq->extts.index >= RP1_MAX_EXTTS)
		return -EOPNOTSUPP;
	edges = rq->extts.flags & PTP_EXTTS_EDGES;
	if (on && !edges && (rq->extts.flags & PTP_STRICT_FLAGS))
		return -EINVAL;
	if (!edges)
		edges = PTP_RISING_EDGE;
	mutex_lock(&s->lock);
	if (s->dying) {
		ret = -ENODEV;
		goto out;
	}
	if (on && s->output && s->enabled && !s->monitor_started) {
		long waited;

		mutex_unlock(&s->lock);
		waited = wait_event_interruptible_timeout(s->monitor_wait,
							  READ_ONCE(s->monitor_started) ||
			(READ_ONCE(s->monitor_sm) < 0 &&
			 READ_ONCE(s->monitor_error)) ||
			!READ_ONCE(s->enabled) || READ_ONCE(s->dying),
			msecs_to_jiffies(2000));
		if (waited < 0)
			return waited;
		if (!waited)
			return -ETIMEDOUT;
		mutex_lock(&s->lock);
		if (s->dying) {
			ret = -ENODEV;
			goto out;
		}
		if (!s->monitor_started) {
			ret = s->monitor_error ?: s->last_error ?: -EIO;
			goto out;
		}
	}
	if (s->output && s->enabled) {
		ret = duplex_input_enable(s, rq->extts.index, edges, on);
		if (ret)
			s->last_error = ret;
		goto out;
	}
	if (!on && s->input[rq->extts.index].requested) {
		if (!s->output && s->enabled && !rq->extts.index) {
			s->enabled = false;
			ret = ext_release(s);
		} else {
			ret = duplex_input_release(s, rq->extts.index);
		}
		if (!ret) {
			s->input[rq->extts.index].requested = false;
			s->input[rq->extts.index].requested_gpio = -1;
			s->input[rq->extts.index].requested_edges = 0;
		}
		goto out;
	}
	if (rq->extts.index) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (!on) {
		if (!s->output) {
			s->enabled = false;
			ret = ext_release(s);
		}
		goto out;
	}
	pin = ptp_find_pin(bp->ptp_clock, PTP_PF_EXTTS, 0);
	if (pin < 0 || pin >= EXT_GPIO_COUNT) {
		ret = -EINVAL;
		goto out;
	}
	if (s->enabled) {
		ret = (!s->output && s->gpio == pin && s->edge == edges) ? 0 : -EBUSY;
		goto out;
	}
	s->gpio = pin;
	s->output = false;
	s->edge = edges;
	s->input[0].requested = true;
	s->input[0].requested_gpio = pin;
	s->input[0].requested_edges = edges;
	ret = ext_acquire(s);
	if (!ret) {
		s->enabled = true;
		ret = ext_start(s);
	}
	if (ret) {
		s->enabled = false;
		s->last_error = ret;
		if (ext_release(s))
			dev_err(&bp->pdev->dev,
				"RP1 EXTS cleanup retained resources after error %d\n", ret);
		s->input[0].requested = false;
		s->input[0].requested_gpio = -1;
		s->input[0].requested_edges = 0;
	} else {
		s->last_error = 0;
	}
out:
	mutex_unlock(&s->lock);
	return ret;
}

int rp1_extts_verify(struct macb *bp, unsigned int pin,
		     enum ptp_pin_function func, unsigned int chan)
{
	if (!bp->rp1_extts || pin >= EXT_GPIO_COUNT)
		return -EINVAL;
	if (func == PTP_PF_NONE)
		return 0;
	if (func == PTP_PF_EXTTS)
		return chan < RP1_MAX_EXTTS ? 0 : -EINVAL;
	if (func == PTP_PF_PEROUT)
		return chan ? -EINVAL : 0;
	return -EOPNOTSUPP;
}

int rp1_extts_clock_lock(struct macb *bp)
{
	struct rp1_extts *s = bp->rp1_extts;
	int ret;

	if (!s)
		return 0;
	mutex_lock(&s->lock);
	if (!s->enabled)
		return 0;
	ret = ext_stop(s);
	if (ret)
		mutex_unlock(&s->lock);
	return ret;
}

static int ext_resume(struct rp1_extts *s)
{
	u64 now;
	int ret;

	if (s->output) {
		ret = ext_now(s, &now);
		if (ret)
			return ret;
		/* Preserve the requested grid across a time step. Output
		 * stays LOW during reacquisition; continuity is not promised.
		 */
		s->out_first = out_next_grid(now + NSEC_PER_SEC, s->out_phase);
	}
	return ext_start(s);
}

int rp1_extts_clock_unlock(struct macb *bp)
{
	struct rp1_extts *s = bp->rp1_extts;
	int ret = 0;

	if (!s)
		return 0;
	s->clock_steps++;
	if (s->enabled) {
		ret = ext_resume(s);
		if (ret)
			ext_fail(s, ret);
	}
	mutex_unlock(&s->lock);
	return ret;
}

void rp1_extts_rate_lock(struct macb *bp)
{
	struct rp1_extts *s = bp->rp1_extts;

	if (!s)
		return;
	mutex_lock(&s->lock);
	s->rate_before = 0;
	s->rate_irqs_held = s->enabled && s->output;
	if (s->rate_irqs_held) {
		/* The wrapped MACB adjfine and our completion only use arithmetic
		 * and MMIO. Keep host preemption out of the rate-to-FIFO interval.
		 */
		local_irq_save(s->rate_irq_flags);
		ext_now(s, &s->rate_before);
	}
}

void rp1_extts_rate_unlock(struct macb *bp)
{
	struct rp1_extts *s = bp->rp1_extts;
	u32 rate;
	u64 after = 0;
	int ret = 0;

	if (!s)
		return;
	s->rate_changes++;
	if (s->enabled && s->output) {
		struct rate_record *r;

		rate = out_rate(s);
		ext_now(s, &after);
		s->rate_count++;
		r = &s->rate_records[(s->rate_count - 1) % OUT_RECORDS];
		if (r->sequence > s->out_rate_epoch)
			s->rate_history_floor = r->after;
		*r = (struct rate_record) {
			.sequence = s->rate_count, .before = s->rate_before, .after = after,
			.target = s->out_target, .pulse = s->out_count,
			.old_rate = s->out_rate, .new_rate = rate,
		};
		if (!rate || !s->rate_before || after < s->rate_before) {
			ret = -EOPNOTSUPP;
		} else if (rate != s->out_rate) {
			if (s->map_valid) {
				ret = out_map_advance(s, s->rate_before +
					      (after - s->rate_before) / 2, rate);
				if (!ret) {
					ret = out_send_deadline(s, &r->sent_at, &r->drained_at);
					if (ret == -ETIME && !r->drained_at) {
						s->out_late_skips++;
						r->action = 2;
						/* Keep the current edge inside the final guard. */
						ret = 0;
					} else if (!ret) {
						s->out_retargets++;
						r->action = 1;
						r->deadline_word = s->out_deadline;
					}
				}
			}
			s->out_rate = rate;
		}
		if (r->drained_at)
			s->out_max_update_ns = max(s->out_max_update_ns, r->drained_at - r->before);
	}
	if (s->rate_irqs_held) {
		local_irq_restore(s->rate_irq_flags);
		s->rate_irqs_held = false;
	}
	/* Resource release and firmware calls are always outside IRQ masking. */
	if (ret)
		ext_fail(s, ret);
	mutex_unlock(&s->lock);
}

static int ext_status_show(struct seq_file *m, void *unused)
{
	struct rp1_extts *s = m->private;

	mutex_lock(&s->lock);
	seq_printf(m, "backend=pio-dma-tsu source=%s phc=%d pins=28 extts=%d perout=1\n",
		   s->output ? "perout" : "physical",
		   s->bp->ptp_clock ? ptp_clock_index(s->bp->ptp_clock) : -1,
		   RP1_MAX_EXTTS);
	seq_printf(m, "enabled=%d armed=%d gpio=%d edge=%s sm=%d error=%d\n",
		   s->enabled, s->armed, s->gpio,
		   s->edge == PTP_EXTTS_EDGES ? "both" :
		   s->edge == PTP_FALLING_EDGE ? "falling" : "rising",
		   s->sm, s->last_error);
	seq_printf(m, "events=%llu faults=%llu timeouts=%llu clock_steps=%llu bias_ns=%d\n",
		   s->events, s->faults, s->timeouts, s->clock_steps, EXT_INPUT_BIAS_NS);
	seq_printf(m, "last_latch_ns=%llu collection_age_ns=%llu max_collection_age_ns=%llu\n",
		   s->last_stamp, s->last_age, s->max_age);
	seq_printf(m, "rearm_deadtime_ns=%llu max_rearm_deadtime_ns=%llu\n",
		   s->last_deadtime, s->max_deadtime);
	seq_puts(m, "unarmed_edges=unobservable calibration=uncalibrated nominal_pio_cycle_ns=5\n");
	seq_printf(m, "out_active=%d out_calibrating=%d out_gpio=%d out_count=%llu out_anchors=%llu rate_changes=%llu startup_origin=%d compact_origin=%d\n",
		   s->output && s->enabled, s->output && s->enabled && s->calibrating,
		   s->output ? s->gpio : -1, s->out_count, s->out_anchors,
		   s->rate_changes, s->out_startup_origin, s->out_compact_origin);
	seq_printf(m, "out_first_ns=%llu out_next_target_ns=%llu out_width_ns=%u out_phase_ns=%u out_rate_q24=%u\n",
		   s->out_first, s->out_target, s->out_width, s->out_phase, s->out_rate);
	seq_printf(m, "out_preparing=%d out_prepares=%llu out_prepare_stamp_ns=%llu out_prepare_age_ns=%llu out_max_prepare_age_ns=%llu\n",
		   s->preparing, s->out_prepares, s->out_prepare_stamp,
		   s->out_prepare_age, s->out_max_prepare_age);
	seq_printf(m, "retarget_enabled=%d out_retargets=%llu out_late_skips=%llu out_max_update_ns=%llu\n",
		   true, s->out_retargets, s->out_late_skips, s->out_max_update_ns);
	seq_printf(m, "out_last_latch_ns=%llu out_error_ns=%lld out_min_error_ns=%lld out_max_error_ns=%lld out_min_lead_ns=%llu EXT_OUTPUT_BIAS_NS=%d\n",
		   s->out_last_latch, s->out_error, s->out_min_error, s->out_max_error,
		   s->out_min_lead == U64_MAX ? 0 : s->out_min_lead, EXT_OUTPUT_BIAS_NS);
	seq_printf(m, "out_pad_monitor=%d monitor_sm=%d monitor_origin=%d monitor_samples=%llu monitor_overruns=%llu monitor_last_counter=%llu monitor_error=%d marker_source=output-pio-transition\n",
		   s->monitor_started, s->monitor_sm, s->monitor_offset,
		   s->monitor_samples, s->monitor_overruns, s->monitor_last,
		   s->monitor_error);
	{
		unsigned int i;

		for (i = 0; i < RP1_MAX_EXTTS; i++) {
			struct rp1_duplex_input *in = &s->input[i];

			seq_printf(m, "extts%u requested=%d requested_gpio=%d requested_edges=%u prepared=%d started=%d gpio=%d sm=%d samples=%llu events=%llu overruns=%llu pending=%llu last_event_ns=%llu max_uncertainty_ns=%llu error=%d\n",
				   i, in->requested, in->requested_gpio,
				   in->requested_edges, in->prepared, in->started,
				   in->gpio, in->sm,
				   in->samples, in->events, in->overruns,
				   in->pending_tail - in->pending_head,
				   in->last_event_ns, in->max_uncertainty_ns, in->error);
		}
	}
	seq_puts(m, "perout_period_ns=1000000000 input_gpio_must_differ_from_output=1 output_accuracy=uncalibrated-marker-latch\n");
	mutex_unlock(&s->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ext_status);

static int out_records_show(struct seq_file *m, void *unused)
{
	struct rp1_extts *s = m->private;
	u64 i, start;

	mutex_lock(&s->lock);
	seq_puts(m, "sequence target_ns latch_ns marker_error_ns\n");
	start = s->out_count > OUT_RECORDS ? s->out_count - OUT_RECORDS : 0;
	for (i = start; i < s->out_count; i++) {
		struct out_record *r = &s->out_records[i % OUT_RECORDS];

		seq_printf(m, "%llu %llu %llu %lld\n", r->sequence, r->target, r->stamp, r->error);
	}
	mutex_unlock(&s->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(out_records);

static int rate_records_show(struct seq_file *m, void *unused)
{
	struct rp1_extts *s = m->private;
	u64 i, start;

	mutex_lock(&s->lock);
	seq_puts(m, "sequence before_ns after_ns target_ns pulse old_rate_q24 new_rate_q24 action sent_ns drained_ns deadline_word\n");
	start = s->rate_count > OUT_RECORDS ? s->rate_count - OUT_RECORDS : 0;
	for (i = start; i < s->rate_count; i++) {
		struct rate_record *r = &s->rate_records[i % OUT_RECORDS];

		seq_printf(m, "%llu %llu %llu %llu %llu %u %u %d %llu %llu %u\n", r->sequence,
			   r->before, r->after, r->target, r->pulse, r->old_rate, r->new_rate,
			r->action, r->sent_at, r->drained_at, r->deadline_word);
	}
	mutex_unlock(&s->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rate_records);

int rp1_extts_quiesce(struct macb *bp)
{
	struct rp1_extts *s = bp->rp1_extts;
	int ret;

	if (!s)
		return 0;
	debugfs_remove_recursive(s->debug);
	s->debug = NULL;
	mutex_lock(&s->lock);
	WRITE_ONCE(s->dying, true);
	s->enabled = false;
	ret = ext_release(s);
	mutex_unlock(&s->lock);
	cancel_delayed_work_sync(&s->work);
	return ret;
}

int rp1_extts_destroy(struct macb *bp)
{
	struct rp1_extts *s = bp->rp1_extts;
	int ret;

	if (!s)
		return 0;
	ret = rp1_extts_quiesce(bp);
	if (ret) {
		dev_err(&bp->pdev->dev,
			"RP1 PTP cleanup failed (%d); retaining active resources\n", ret);
		bp->rp1_extts = NULL;
		s->bp = NULL;
		return ret;
	}
	destroy_workqueue(s->wq);
	bp->rp1_extts = NULL;
	kfree(s);
	return 0;
}

int rp1_extts_create(struct macb *bp)
{
	struct rp1_extts *s;
	struct device_node *np;
	int i;

	if (!of_device_is_compatible(bp->pdev->dev.of_node, "raspberrypi,rp1-gem") ||
	    !bp->rp1_eth_cfg || !bp->rp1_eth_cfg_phys)
		return -ENODEV;
	np = of_parse_phandle(bp->pdev->dev.of_node, "raspberrypi,pio", 0);
	if (!np)
		return -EINVAL;
	of_node_put(np);
	np = of_parse_phandle(bp->pdev->dev.of_node,
			      "raspberrypi,gpio-controller", 0);
	if (!np)
		return -EINVAL;
	of_node_put(np);

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->bp = bp;
	s->sm = -1;
	s->offset = -1;
	s->monitor_sm = -1;
	s->monitor_offset = -1;
	s->out_startup_origin = -1;
	s->out_compact_origin = -1;
	s->control = bp->rp1_eth_cfg;
	s->control_phys = bp->rp1_eth_cfg_phys;
	mutex_init(&s->lock);
	init_waitqueue_head(&s->monitor_wait);
	INIT_DELAYED_WORK(&s->work, ext_work);
	s->wq = alloc_ordered_workqueue("rp1-extts", WQ_MEM_RECLAIM);
	if (!s->wq) {
		kfree(s);
		return -ENOMEM;
	}
	bp->rp1_extts = s;
	for (i = 0; i < RP1_MAX_EXTTS; i++) {
		s->input[i].sm = -1;
		s->input[i].gpio = -1;
		s->input[i].requested_gpio = -1;
		atomic64_set(&s->input[i].produced, 0);
	}
	for (i = 0; i < EXT_GPIO_COUNT; i++) {
		snprintf(s->pins[i].name, sizeof(s->pins[i].name), "GPIO%d", i);
		s->pins[i].index = i;
		s->pins[i].func = PTP_PF_NONE;
		s->pins[i].chan = 0;
	}
	bp->ptp_clock_info.n_pins = EXT_GPIO_COUNT;
	bp->ptp_clock_info.n_ext_ts = RP1_MAX_EXTTS;
	bp->ptp_clock_info.n_per_out = 1;
	bp->ptp_clock_info.supported_extts_flags =
		PTP_RISING_EDGE | PTP_FALLING_EDGE | PTP_STRICT_FLAGS;
	bp->ptp_clock_info.supported_perout_flags =
		PTP_PEROUT_PHASE | PTP_PEROUT_DUTY_CYCLE;
	bp->ptp_clock_info.pin_config = s->pins;
	s->debug = debugfs_create_dir("rp1-extts", NULL);
	debugfs_create_file("status", 0444, s->debug, s, &ext_status_fops);
	debugfs_create_file("output_records", 0444, s->debug, s, &out_records_fops);
	debugfs_create_file("rate_records", 0444, s->debug, s, &rate_records_fops);
	return 0;
}
