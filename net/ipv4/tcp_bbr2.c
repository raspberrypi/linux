/* BBR (Bottleneck Bandwidth and RTT) congestion control, v2
 *
 * BBRv2 is a model-based congestion control algorithm that aims for low
 * queues, low loss, and (bounded) Reno/CUBIC coexistence. To maintain a model
 * of the network path, it uses measurements of bandwidth and RTT, as well as
 * (if they occur) packet loss and/or DCTCP/L4S-style ECN signals.  Note that
 * although it can use ECN or loss signals explicitly, it does not require
 * either; it can bound its in-flight data based on its estimate of the BDP.
 *
 * The model has both higher and lower bounds for the operating range:
 *   lo: bw_lo, inflight_lo: conservative short-term lower bound
 *   hi: bw_hi, inflight_hi: robust long-term upper bound
 * The bandwidth-probing time scale is (a) extended dynamically based on
 * estimated BDP to improve coexistence with Reno/CUBIC; (b) bounded by
 * an interactive wall-clock time-scale to be more scalable and responsive
 * than Reno and CUBIC.
 *
 * Here is a state transition diagram for BBR:
 *
 *             |
 *             V
 *    +---> STARTUP  ----+
 *    |        |         |
 *    |        V         |
 *    |      DRAIN   ----+
 *    |        |         |
 *    |        V         |
 *    +---> PROBE_BW ----+
 *    |      ^    |      |
 *    |      |    |      |
 *    |      +----+      |
 *    |                  |
 *    +---- PROBE_RTT <--+
 *
 * A BBR flow starts in STARTUP, and ramps up its sending rate quickly.
 * When it estimates the pipe is full, it enters DRAIN to drain the queue.
 * In steady state a BBR flow only uses PROBE_BW and PROBE_RTT.
 * A long-lived BBR flow spends the vast majority of its time remaining
 * (repeatedly) in PROBE_BW, fully probing and utilizing the pipe's bandwidth
 * in a fair manner, with a small, bounded queue. *If* a flow has been
 * continuously sending for the entire min_rtt window, and hasn't seen an RTT
 * sample that matches or decreases its min_rtt estimate for 10 seconds, then
 * it briefly enters PROBE_RTT to cut inflight to a minimum value to re-probe
 * the path's two-way propagation delay (min_rtt). When exiting PROBE_RTT, if
 * we estimated that we reached the full bw of the pipe then we enter PROBE_BW;
 * otherwise we enter STARTUP to try to fill the pipe.
 *
 * BBR is described in detail in:
 *   "BBR: Congestion-Based Congestion Control",
 *   Neal Cardwell, Yuchung Cheng, C. Stephen Gunn, Soheil Hassas Yeganeh,
 *   Van Jacobson. ACM Queue, Vol. 14 No. 5, September-October 2016.
 *
 * There is a public e-mail list for discussing BBR development and testing:
 *   https://groups.google.com/forum/#!forum/bbr-dev
 *
 * NOTE: BBR might be used with the fq qdisc ("man tc-fq") with pacing enabled,
 * otherwise TCP stack falls back to an internal pacing using one high
 * resolution timer per TCP socket and may use more resources.
 */
#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet_diag.h>
#include <linux/inet.h>
#include <linux/random.h>

#include "tcp_dctcp.h"

/* Scale factor for rate in pkt/uSec unit to avoid truncation in bandwidth
 * estimation. The rate unit ~= (1500 bytes / 1 usec / 2^24) ~= 715 bps.
 * This handles bandwidths from 0.06pps (715bps) to 256Mpps (3Tbps) in a u32.
 * Since the minimum window is >=4 packets, the lower bound isn't
 * an issue. The upper bound isn't an issue with existing technologies.
 */
#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE)

#define BBR_SCALE 8	/* scaling factor for fractions in BBR (e.g. gains) */
#define BBR_UNIT (1 << BBR_SCALE)

#define FLAG_DEBUG_VERBOSE	0x1	/* Verbose debugging messages */
#define FLAG_DEBUG_LOOPBACK	0x2	/* Do NOT skip loopback addr */

#define CYCLE_LEN		8	/* number of phases in a pacing gain cycle */

/* BBR has the following modes for deciding how fast to send: */
enum bbr_mode {
	BBR_STARTUP,	/* ramp up sending rate rapidly to fill pipe */
	BBR_DRAIN,	/* drain any queue created during startup */
	BBR_PROBE_BW,	/* discover, share bw: pace around estimated bw */
	BBR_PROBE_RTT,	/* cut inflight to min to probe min_rtt */
};

/* How does the incoming ACK stream relate to our bandwidth probing? */
enum bbr_ack_phase {
	BBR_ACKS_INIT,		  /* not probing; not getting probe feedback */
	BBR_ACKS_REFILLING,	  /* sending at est. bw to fill pipe */
	BBR_ACKS_PROBE_STARTING,  /* inflight rising to probe bw */
	BBR_ACKS_PROBE_FEEDBACK,  /* getting feedback from bw probing */
	BBR_ACKS_PROBE_STOPPING,  /* stopped probing; still getting feedback */
};

/* BBR congestion control block */
struct bbr {
	u32	min_rtt_us;	        /* min RTT in min_rtt_win_sec window */
	u32	min_rtt_stamp;	        /* timestamp of min_rtt_us */
	u32	probe_rtt_done_stamp;   /* end time for BBR_PROBE_RTT mode */
	u32	probe_rtt_min_us;	/* min RTT in bbr_probe_rtt_win_ms window */
	u32	probe_rtt_min_stamp;	/* timestamp of probe_rtt_min_us*/
	u32     next_rtt_delivered; /* scb->tx.delivered at end of round */
	u32	prior_rcv_nxt;	/* tp->rcv_nxt when CE state last changed */
	u64	cycle_mstamp;	     /* time of this cycle phase start */
	u32     mode:3,		     /* current bbr_mode in state machine */
		prev_ca_state:3,     /* CA state on previous ACK */
		packet_conservation:1,  /* use packet conservation? */
		round_start:1,	     /* start of packet-timed tx->ack round? */
		ce_state:1,          /* If most recent data has CE bit set */
		bw_probe_up_rounds:5,   /* cwnd-limited rounds in PROBE_UP */
		try_fast_path:1, 	/* can we take fast path? */
		unused2:11,
		idle_restart:1,	     /* restarting after idle? */
		probe_rtt_round_done:1,  /* a BBR_PROBE_RTT round at 4 pkts? */
		cycle_idx:3,	/* current index in pacing_gain cycle array */
		has_seen_rtt:1;	     /* have we seen an RTT sample yet? */
	u32	pacing_gain:11,	/* current gain for setting pacing rate */
		cwnd_gain:11,	/* current gain for setting cwnd */
		full_bw_reached:1,   /* reached full bw in Startup? */
		full_bw_cnt:2,	/* number of rounds without large bw gains */
		init_cwnd:7;	/* initial cwnd */
	u32	prior_cwnd;	/* prior cwnd upon entering loss recovery */
	u32	full_bw;	/* recent bw, to estimate if pipe is full */

	/* For tracking ACK aggregation: */
	u64	ack_epoch_mstamp;	/* start of ACK sampling epoch */
	u16	extra_acked[2];		/* max excess data ACKed in epoch */
	u32	ack_epoch_acked:20,	/* packets (S)ACKed in sampling epoch */
		extra_acked_win_rtts:5,	/* age of extra_acked, in round trips */
		extra_acked_win_idx:1,	/* current index in extra_acked array */
	/* BBR v2 state: */
		unused1:2,
		startup_ecn_rounds:2,	/* consecutive hi ECN STARTUP rounds */
		loss_in_cycle:1,	/* packet loss in this cycle? */
		ecn_in_cycle:1;		/* ECN in this cycle? */
	u32	loss_round_delivered; /* scb->tx.delivered ending loss round */
	u32	undo_bw_lo;	     /* bw_lo before latest losses */
	u32	undo_inflight_lo;    /* inflight_lo before latest losses */
	u32	undo_inflight_hi;    /* inflight_hi before latest losses */
	u32	bw_latest;	 /* max delivered bw in last round trip */
	u32	bw_lo;		 /* lower bound on sending bandwidth */
	u32	bw_hi[2];	 /* upper bound of sending bandwidth range*/
	u32	inflight_latest; /* max delivered data in last round trip */
	u32	inflight_lo;	 /* lower bound of inflight data range */
	u32	inflight_hi;	 /* upper bound of inflight data range */
	u32	bw_probe_up_cnt; /* packets delivered per inflight_hi incr */
	u32	bw_probe_up_acks;  /* packets (S)ACKed since inflight_hi incr */
	u32	probe_wait_us;	 /* PROBE_DOWN until next clock-driven probe */
	u32	ecn_eligible:1,	/* sender can use ECN (RTT, handshake)? */
		ecn_alpha:9,	/* EWMA delivered_ce/delivered; 0..256 */
		bw_probe_samples:1,    /* rate samples reflect bw probing? */
		prev_probe_too_high:1, /* did last PROBE_UP go too high? */
		stopped_risky_probe:1, /* last PROBE_UP stopped due to risk? */
		rounds_since_probe:8,  /* packet-timed rounds since probed bw */
		loss_round_start:1,    /* loss_round_delivered round trip? */
		loss_in_round:1,       /* loss marked in this round trip? */
		ecn_in_round:1,	       /* ECN marked in this round trip? */
		ack_phase:3,	       /* bbr_ack_phase: meaning of ACKs */
		loss_events_in_round:4,/* losses in STARTUP round */
		initialized:1;	       /* has bbr_init() been called? */
	u32	alpha_last_delivered;	 /* tp->delivered    at alpha update */
	u32	alpha_last_delivered_ce; /* tp->delivered_ce at alpha update */
	struct	tcp_plb_state plb;

	/* Params configurable using setsockopt. Refer to correspoding
	 * module param for detailed description of params.
	 */
	struct bbr_params {
		u32	high_gain:11,		/* max allowed value: 2047 */
			drain_gain:10,		/* max allowed value: 1023 */
			cwnd_gain:11;		/* max allowed value: 2047 */
		u32	cwnd_min_target:4,	/* max allowed value: 15 */
			min_rtt_win_sec:5,	/* max allowed value: 31 */
			probe_rtt_mode_ms:9,	/* max allowed value: 511 */
			full_bw_cnt:3,		/* max allowed value: 7 */
			cwnd_tso_budget:1,	/* allowed values: {0, 1} */
			unused3:6,
			drain_to_target:1,	/* boolean */
			precise_ece_ack:1,	/* boolean */
			extra_acked_in_startup:1, /* allowed values: {0, 1} */
			fast_path:1;		/* boolean */
		u32	full_bw_thresh:10,	/* max allowed value: 1023 */
			startup_cwnd_gain:11,	/* max allowed value: 2047 */
			bw_probe_pif_gain:9,	/* max allowed value: 511 */
			usage_based_cwnd:1, 	/* boolean */
			unused2:1;
		u16	probe_rtt_win_ms:14,	/* max allowed value: 16383 */
			refill_add_inc:2;	/* max allowed value: 3 */
		u16	extra_acked_gain:11,	/* max allowed value: 2047 */
			extra_acked_win_rtts:5; /* max allowed value: 31*/
		u16	pacing_gain[CYCLE_LEN]; /* max allowed value: 1023 */
		/* Mostly BBR v2 parameters below here: */
		u32	ecn_alpha_gain:8,	/* max allowed value: 255 */
			ecn_factor:8,		/* max allowed value: 255 */
			ecn_thresh:8,		/* max allowed value: 255 */
			beta:8;			/* max allowed value: 255 */
		u32	ecn_max_rtt_us:19,	/* max allowed value: 524287 */
			bw_probe_reno_gain:9,	/* max allowed value: 511 */
			full_loss_cnt:4;	/* max allowed value: 15 */
		u32	probe_rtt_cwnd_gain:8,	/* max allowed value: 255 */
			inflight_headroom:8,	/* max allowed value: 255 */
			loss_thresh:8,		/* max allowed value: 255 */
			bw_probe_max_rounds:8;	/* max allowed value: 255 */
		u32	bw_probe_rand_rounds:4, /* max allowed value: 15 */
			bw_probe_base_us:26,	/* usecs: 0..2^26-1 (67 secs) */
			full_ecn_cnt:2;		/* max allowed value: 3 */
		u32	bw_probe_rand_us:26,	/* usecs: 0..2^26-1 (67 secs) */
			undo:1,			/* boolean */
			tso_rtt_shift:4,	/* max allowed value: 15 */
			unused5:1;
		u32	ecn_reprobe_gain:9,	/* max allowed value: 511 */
			unused1:14,
			ecn_alpha_init:9;	/* max allowed value: 256 */
	} params;

	struct {
		u32	snd_isn; /* Initial sequence number */
		u32	rs_bw; 	 /* last valid rate sample bw */
		u32	target_cwnd; /* target cwnd, based on BDP */
		u8	undo:1,  /* Undo even happened but not yet logged */
			unused:7;
		char	event;	 /* single-letter event debug codes */
		u16	unused2;
	} debug;
};

struct bbr_context {
	u32 sample_bw;
	u32 target_cwnd;
	u32 log:1;
};

/* Window length of min_rtt filter (in sec). Max allowed value is 31 (0x1F) */
static u32 bbr_min_rtt_win_sec = 10;
/* Minimum time (in ms) spent at bbr_cwnd_min_target in BBR_PROBE_RTT mode.
 * Max allowed value is 511 (0x1FF).
 */
static u32 bbr_probe_rtt_mode_ms = 200;
/* Window length of probe_rtt_min_us filter (in ms), and consequently the
 * typical interval between PROBE_RTT mode entries.
 * Note that bbr_probe_rtt_win_ms must be <= bbr_min_rtt_win_sec * MSEC_PER_SEC
 */
static u32 bbr_probe_rtt_win_ms = 5000;
/* Skip TSO below the following bandwidth (bits/sec): */
static int bbr_min_tso_rate = 1200000;

/* Use min_rtt to help adapt TSO burst size, with smaller min_rtt resulting
 * in bigger TSO bursts. By default we cut the RTT-based allowance in half
 * for every 2^9 usec (aka 512 us) of RTT, so that the RTT-based allowance
 * is below 1500 bytes after 6 * ~500 usec = 3ms.
 */
static u32 bbr_tso_rtt_shift = 9;  /* halve allowance per 2^9 usecs, 512us */

/* Select cwnd TSO budget approach:
 *  0: padding
 *  1: flooring
 */
static uint bbr_cwnd_tso_budget = 1;

/* Pace at ~1% below estimated bw, on average, to reduce queue at bottleneck.
 * In order to help drive the network toward lower queues and low latency while
 * maintaining high utilization, the average pacing rate aims to be slightly
 * lower than the estimated bandwidth. This is an important aspect of the
 * design.
 */
static const int bbr_pacing_margin_percent = 1;

/* We use a high_gain value of 2/ln(2) because it's the smallest pacing gain
 * that will allow a smoothly increasing pacing rate that will double each RTT
 * and send the same number of packets per RTT that an un-paced, slow-starting
 * Reno or CUBIC flow would. Max allowed value is 2047 (0x7FF).
 */
static int bbr_high_gain  = BBR_UNIT * 2885 / 1000 + 1;
/* The gain for deriving startup cwnd. Max allowed value is 2047 (0x7FF). */
static int bbr_startup_cwnd_gain  = BBR_UNIT * 2885 / 1000 + 1;
/* The pacing gain of 1/high_gain in BBR_DRAIN is calculated to typically drain
 * the queue created in BBR_STARTUP in a single round. Max allowed value
 * is 1023 (0x3FF).
 */
static int bbr_drain_gain = BBR_UNIT * 1000 / 2885;
/* The gain for deriving steady-state cwnd tolerates delayed/stretched ACKs.
 * Max allowed value is 2047 (0x7FF).
 */
static int bbr_cwnd_gain  = BBR_UNIT * 2;
/* The pacing_gain values for the PROBE_BW gain cycle, to discover/share bw.
 * Max allowed value for each element is 1023 (0x3FF).
 */
enum bbr_pacing_gain_phase {
	BBR_BW_PROBE_UP		= 0,  /* push up inflight to probe for bw/vol */
	BBR_BW_PROBE_DOWN	= 1,  /* drain excess inflight from the queue */
	BBR_BW_PROBE_CRUISE	= 2,  /* use pipe, w/ headroom in queue/pipe */
	BBR_BW_PROBE_REFILL	= 3,  /* v2: refill the pipe again to 100% */
};
static int bbr_pacing_gain[] = {
	BBR_UNIT * 5 / 4,	/* probe for more available bw */
	BBR_UNIT * 3 / 4,	/* drain queue and/or yield bw to other flows */
	BBR_UNIT, BBR_UNIT, BBR_UNIT,	/* cruise at 1.0*bw to utilize pipe, */
	BBR_UNIT, BBR_UNIT, BBR_UNIT	/* without creating excess queue... */
};

/* Try to keep at least this many packets in flight, if things go smoothly. For
 * smooth functioning, a sliding window protocol ACKing every other packet
 * needs at least 4 packets in flight. Max allowed value is 15 (0xF).
 */
static u32 bbr_cwnd_min_target = 4;

/* Cwnd to BDP proportion in PROBE_RTT mode scaled by BBR_UNIT. Default: 50%.
 * Use 0 to disable. Max allowed value is 255.
 */
static u32 bbr_probe_rtt_cwnd_gain = BBR_UNIT * 1 / 2;

/* To estimate if BBR_STARTUP mode (i.e. high_gain) has filled pipe... */
/* If bw has increased significantly (1.25x), there may be more bw available.
 * Max allowed value is 1023 (0x3FF).
 */
static u32 bbr_full_bw_thresh = BBR_UNIT * 5 / 4;
/* But after 3 rounds w/o significant bw growth, estimate pipe is full.
 * Max allowed value is 7 (0x7).
 */
static u32 bbr_full_bw_cnt = 3;

static u32 bbr_flags;		/* Debugging related stuff */

/* Whether to debug using printk.
 */
static bool bbr_debug_with_printk;

/* Whether to debug using ftrace event tcp:tcp_bbr_event.
 * Ignored when bbr_debug_with_printk is set.
 */
static bool bbr_debug_ftrace;

/* Experiment: each cycle, try to hold sub-unity gain until inflight <= BDP. */
static bool bbr_drain_to_target = true;		/* default: enabled */

/* Experiment: Flags to control BBR with ECN behavior.
 */
static bool bbr_precise_ece_ack = true;		/* default: enabled */

/* The max rwin scaling shift factor is 14 (RFC 1323), so the max sane rwin is
 * (2^(16+14) B)/(1024 B/packet) = 1M packets.
 */
static u32 bbr_cwnd_warn_val	= 1U << 20;

static u16 bbr_debug_port_mask;

/* BBR module parameters. These are module parameters only in Google prod.
 * Upstream these are intentionally not module parameters.
 */
static int bbr_pacing_gain_size = CYCLE_LEN;

/* Gain factor for adding extra_acked to target cwnd: */
static int bbr_extra_acked_gain = 256;

/* Window length of extra_acked window. Max allowed val is 31. */
static u32 bbr_extra_acked_win_rtts = 5;

/* Max allowed val for ack_epoch_acked, after which sampling epoch is reset */
static u32 bbr_ack_epoch_acked_reset_thresh = 1U << 20;

/* Time period for clamping cwnd increment due to ack aggregation */
static u32 bbr_extra_acked_max_us = 100 * 1000;

/* Use extra acked in startup ?
 * 0: disabled
 * 1: use latest extra_acked value from 1-2 rtt in startup
 */
static int bbr_extra_acked_in_startup = 1;		/* default: enabled */

/* Experiment: don't grow cwnd beyond twice of what we just probed. */
static bool bbr_usage_based_cwnd;		/* default: disabled */

/* For lab testing, researchers can enable BBRv2 ECN support with this flag,
 * when they know that any ECN marks that the connections experience will be
 * DCTCP/L4S-style ECN marks, rather than RFC3168 ECN marks.
 * TODO(ncardwell): Production use of the BBRv2 ECN functionality depends on
 * negotiation or configuration that is outside the scope of the BBRv2
 * alpha release.
 */
static bool bbr_ecn_enable = false;

module_param_named(min_tso_rate,      bbr_min_tso_rate,      int,    0644);
module_param_named(tso_rtt_shift,     bbr_tso_rtt_shift,     int,    0644);
module_param_named(high_gain,         bbr_high_gain,         int,    0644);
module_param_named(drain_gain,        bbr_drain_gain,        int,    0644);
module_param_named(startup_cwnd_gain, bbr_startup_cwnd_gain, int,    0644);
module_param_named(cwnd_gain,         bbr_cwnd_gain,         int,    0644);
module_param_array_named(pacing_gain, bbr_pacing_gain,       int,
			 &bbr_pacing_gain_size, 0644);
module_param_named(cwnd_min_target,   bbr_cwnd_min_target,   uint,   0644);
module_param_named(probe_rtt_cwnd_gain,
		   bbr_probe_rtt_cwnd_gain,		     uint,   0664);
module_param_named(cwnd_warn_val,     bbr_cwnd_warn_val,     uint,   0664);
module_param_named(debug_port_mask,   bbr_debug_port_mask,   ushort, 0644);
module_param_named(flags,             bbr_flags,             uint,   0644);
module_param_named(debug_ftrace,      bbr_debug_ftrace, bool,   0644);
module_param_named(debug_with_printk, bbr_debug_with_printk, bool,   0644);
module_param_named(min_rtt_win_sec,   bbr_min_rtt_win_sec,   uint,   0644);
module_param_named(probe_rtt_mode_ms, bbr_probe_rtt_mode_ms, uint,   0644);
module_param_named(probe_rtt_win_ms,  bbr_probe_rtt_win_ms,  uint,   0644);
module_param_named(full_bw_thresh,    bbr_full_bw_thresh,    uint,   0644);
module_param_named(full_bw_cnt,       bbr_full_bw_cnt,       uint,   0644);
module_param_named(cwnd_tso_bduget,   bbr_cwnd_tso_budget,   uint,   0664);
module_param_named(extra_acked_gain,  bbr_extra_acked_gain,  int,    0664);
module_param_named(extra_acked_win_rtts,
		   bbr_extra_acked_win_rtts, uint,   0664);
module_param_named(extra_acked_max_us,
		   bbr_extra_acked_max_us, uint,   0664);
module_param_named(ack_epoch_acked_reset_thresh,
		   bbr_ack_epoch_acked_reset_thresh, uint,   0664);
module_param_named(drain_to_target,   bbr_drain_to_target,   bool,   0664);
module_param_named(precise_ece_ack,   bbr_precise_ece_ack,   bool,   0664);
module_param_named(extra_acked_in_startup,
		   bbr_extra_acked_in_startup, int, 0664);
module_param_named(usage_based_cwnd, bbr_usage_based_cwnd, bool,   0664);
module_param_named(ecn_enable,       bbr_ecn_enable,         bool,   0664);

static void bbr2_exit_probe_rtt(struct sock *sk);
static void bbr2_reset_congestion_signals(struct sock *sk);

static void bbr_check_probe_rtt_done(struct sock *sk);

/* Do we estimate that STARTUP filled the pipe? */
static bool bbr_full_bw_reached(const struct sock *sk)
{
	const struct bbr *bbr = inet_csk_ca(sk);

	return bbr->full_bw_reached;
}

/* Return the windowed max recent bandwidth sample, in pkts/uS << BW_SCALE. */
static u32 bbr_max_bw(const struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	return max(bbr->bw_hi[0], bbr->bw_hi[1]);
}

/* Return the estimated bandwidth of the path, in pkts/uS << BW_SCALE. */
static u32 bbr_bw(const struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	return min(bbr_max_bw(sk), bbr->bw_lo);
}

/* Return maximum extra acked in past k-2k round trips,
 * where k = bbr_extra_acked_win_rtts.
 */
static u16 bbr_extra_acked(const struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	return max(bbr->extra_acked[0], bbr->extra_acked[1]);
}

/* Return rate in bytes per second, optionally with a gain.
 * The order here is chosen carefully to avoid overflow of u64. This should
 * work for input rates of up to 2.9Tbit/sec and gain of 2.89x.
 */
static u64 bbr_rate_bytes_per_sec(struct sock *sk, u64 rate, int gain,
				  int margin)
{
	unsigned int mss = tcp_sk(sk)->mss_cache;

	rate *= mss;
	rate *= gain;
	rate >>= BBR_SCALE;
	rate *= USEC_PER_SEC / 100 * (100 - margin);
	rate >>= BW_SCALE;
	rate = max(rate, 1ULL);
	return rate;
}

static u64 bbr_bw_bytes_per_sec(struct sock *sk, u64 rate)
{
	return bbr_rate_bytes_per_sec(sk, rate, BBR_UNIT, 0);
}

static u64 bbr_rate_kbps(struct sock *sk, u64 rate)
{
	rate = bbr_bw_bytes_per_sec(sk, rate);
	rate *= 8;
	do_div(rate, 1000);
	return rate;
}

static u32 bbr_tso_segs_goal(struct sock *sk);
static void bbr_debug(struct sock *sk, u32 acked,
		      const struct rate_sample *rs, struct bbr_context *ctx)
{
	static const char ca_states[] = {
		[TCP_CA_Open]		= 'O',
		[TCP_CA_Disorder]	= 'D',
		[TCP_CA_CWR]		= 'C',
		[TCP_CA_Recovery]	= 'R',
		[TCP_CA_Loss]		= 'L',
	};
	static const char mode[] = {
		'G',  /* Growing   - BBR_STARTUP */
		'D',  /* Drain     - BBR_DRAIN */
		'W',  /* Window    - BBR_PROBE_BW */
		'M',  /* Min RTT   - BBR_PROBE_RTT */
	};
	static const char ack_phase[] = { /* bbr_ack_phase strings */
		'I',	/* BBR_ACKS_INIT	   - 'Init' */
		'R',	/* BBR_ACKS_REFILLING	   - 'Refilling' */
		'B',	/* BBR_ACKS_PROBE_STARTING - 'Before' */
		'F',	/* BBR_ACKS_PROBE_FEEDBACK - 'Feedback' */
		'A',	/* BBR_ACKS_PROBE_STOPPING - 'After' */
	};
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	const u32 una = tp->snd_una - bbr->debug.snd_isn;
	const u32 fack = tcp_highest_sack_seq(tp);
	const u16 dport = ntohs(inet_sk(sk)->inet_dport);
	bool is_port_match = (bbr_debug_port_mask &&
			      ((dport & bbr_debug_port_mask) == 0));
	char debugmsg[320];

	if (sk->sk_state == TCP_SYN_SENT)
		return;  /* no bbr_init() yet if SYN retransmit -> CA_Loss */

	if (!tp->snd_cwnd || tp->snd_cwnd > bbr_cwnd_warn_val) {
		char addr[INET6_ADDRSTRLEN + 10] = { 0 };

		if (sk->sk_family == AF_INET)
			snprintf(addr, sizeof(addr), "%pI4:%u",
				 &inet_sk(sk)->inet_daddr, dport);
		else if (sk->sk_family == AF_INET6)
			snprintf(addr, sizeof(addr), "%pI6:%u",
				 &sk->sk_v6_daddr, dport);

		WARN_ONCE(1,
			"BBR %s cwnd alert: %u "
			"snd_una: %u ca: %d pacing_gain: %u cwnd_gain: %u "
			"bw: %u rtt: %u min_rtt: %u "
			"acked: %u tso_segs: %u "
			"bw: %d %ld %d pif: %u\n",
			addr, tp->snd_cwnd,
			una, inet_csk(sk)->icsk_ca_state,
			bbr->pacing_gain, bbr->cwnd_gain,
			bbr_max_bw(sk), (tp->srtt_us >> 3), bbr->min_rtt_us,
			acked, bbr_tso_segs_goal(sk),
			rs->delivered, rs->interval_us, rs->is_retrans,
			tcp_packets_in_flight(tp));
	}

	if (likely(!bbr_debug_with_printk && !bbr_debug_ftrace))
		return;

	if (!sock_flag(sk, SOCK_DBG) && !is_port_match)
		return;

	if (!ctx->log && !tp->app_limited && !(bbr_flags & FLAG_DEBUG_VERBOSE))
		return;

	if (ipv4_is_loopback(inet_sk(sk)->inet_daddr) &&
	    !(bbr_flags & FLAG_DEBUG_LOOPBACK))
		return;

	snprintf(debugmsg, sizeof(debugmsg) - 1,
		 "BBR %pI4:%-5u %5u,%03u:%-7u %c "
		 "%c %2u br %2u cr %2d rtt %5ld d %2d i %5ld mrtt %d %cbw %llu "
		 "bw %llu lb %llu ib %llu qb %llu "
		 "a %u if %2u %c %c dl %u l %u al %u # %u t %u %c %c "
		 "lr %d er %d ea %d bwl %lld il %d ih %d c %d "
		 "v %d %c %u %c %s\n",
		 &inet_sk(sk)->inet_daddr, dport,
		 una / 1000, una % 1000, fack - tp->snd_una,
		 ca_states[inet_csk(sk)->icsk_ca_state],
		 bbr->debug.undo ? '@' : mode[bbr->mode],
		 tp->snd_cwnd,
		 bbr_extra_acked(sk),	/* br (legacy): extra_acked */
		 rs->tx_in_flight,	/* cr (legacy): tx_inflight */
		 rs->rtt_us,
		 rs->delivered,
		 rs->interval_us,
		 bbr->min_rtt_us,
		 rs->is_app_limited ? '_' : 'l',
		 bbr_rate_kbps(sk, ctx->sample_bw), /* lbw: latest sample bw */
		 bbr_rate_kbps(sk, bbr_max_bw(sk)), /* bw: max bw */
		 0ULL,				    /* lb: [obsolete] */
		 0ULL,				    /* ib: [obsolete] */
		 (u64)sk->sk_pacing_rate * 8 / 1000,
		 acked,
		 tcp_packets_in_flight(tp),
		 rs->is_ack_delayed ? 'd' : '.',
		 bbr->round_start ? '*' : '.',
		 tp->delivered, tp->lost,
		 tp->app_limited,
		 0,			    	    /* #: [obsolete] */
		 ctx->target_cwnd,
		 tp->reord_seen ? 'r' : '.',  /* r: reordering seen? */
		 ca_states[bbr->prev_ca_state],
		 (rs->lost + rs->delivered) > 0 ?
		 (1000 * rs->lost /
		  (rs->lost + rs->delivered)) : 0,    /* lr: loss rate x1000 */
		 (rs->delivered) > 0 ?
		 (1000 * rs->delivered_ce /
		  (rs->delivered)) : 0,		      /* er: ECN rate x1000 */
		 1000 * bbr->ecn_alpha >> BBR_SCALE,  /* ea: ECN alpha x1000 */
		 bbr->bw_lo == ~0U ?
		   -1 : (s64)bbr_rate_kbps(sk, bbr->bw_lo), /* bwl */
		 bbr->inflight_lo,	/* il */
		 bbr->inflight_hi,	/* ih */
		 bbr->bw_probe_up_cnt,	/* c */
		 2,			/* v: version */
		 bbr->debug.event,
		 bbr->cycle_idx,
		 ack_phase[bbr->ack_phase],
		 bbr->bw_probe_samples ? "Y" : "N");
	debugmsg[sizeof(debugmsg) - 1] = 0;

	/* printk takes a higher precedence. */
	if (bbr_debug_with_printk)
		printk(KERN_DEBUG "%s", debugmsg);

	if (unlikely(bbr->debug.undo))
		bbr->debug.undo = 0;
}

/* Convert a BBR bw and gain factor to a pacing rate in bytes per second. */
static unsigned long bbr_bw_to_pacing_rate(struct sock *sk, u32 bw, int gain)
{
	u64 rate = bw;

	rate = bbr_rate_bytes_per_sec(sk, rate, gain,
				      bbr_pacing_margin_percent);
	rate = min_t(u64, rate, sk->sk_max_pacing_rate);
	return rate;
}

/* Initialize pacing rate to: high_gain * init_cwnd / RTT. */
static void bbr_init_pacing_rate_from_rtt(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u64 bw;
	u32 rtt_us;

	if (tp->srtt_us) {		/* any RTT sample yet? */
		rtt_us = max(tp->srtt_us >> 3, 1U);
		bbr->has_seen_rtt = 1;
	} else {			 /* no RTT sample yet */
		rtt_us = USEC_PER_MSEC;	 /* use nominal default RTT */
	}
	bw = (u64)tp->snd_cwnd * BW_UNIT;
	do_div(bw, rtt_us);
	sk->sk_pacing_rate = bbr_bw_to_pacing_rate(sk, bw, bbr->params.high_gain);
}

/* Pace using current bw estimate and a gain factor. */
static void bbr_set_pacing_rate(struct sock *sk, u32 bw, int gain)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	unsigned long rate = bbr_bw_to_pacing_rate(sk, bw, gain);

	if (unlikely(!bbr->has_seen_rtt && tp->srtt_us))
		bbr_init_pacing_rate_from_rtt(sk);
	if (bbr_full_bw_reached(sk) || rate > sk->sk_pacing_rate)
		sk->sk_pacing_rate = rate;
}

static u32 bbr_min_tso_segs(struct sock *sk)
{
	return sk->sk_pacing_rate < (bbr_min_tso_rate >> 3) ? 1 : 2;
}

/* Return the number of segments BBR would like in a TSO/GSO skb, given
 * a particular max gso size as a constraint.
 */
static u32 bbr_tso_segs_generic(struct sock *sk, unsigned int mss_now,
				u32 gso_max_size)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 segs, r;
	u64 bytes;

	/* Budget a TSO/GSO burst size allowance based on bw (pacing_rate). */
	bytes = sk->sk_pacing_rate >> sk->sk_pacing_shift;

	/* Budget a TSO/GSO burst size allowance based on min_rtt. For every
	 * K = 2^tso_rtt_shift microseconds of min_rtt, halve the burst.
	 * The min_rtt-based burst allowance is: 64 KBytes / 2^(min_rtt/K)
	 */
	if (bbr->params.tso_rtt_shift) {
		r = bbr->min_rtt_us >> bbr->params.tso_rtt_shift;
		if (r < BITS_PER_TYPE(u32))   /* prevent undefined behavior */
			bytes += GSO_MAX_SIZE >> r;
	}

	bytes = min_t(u32, bytes, gso_max_size - 1 - MAX_TCP_HEADER);
	segs = max_t(u32, bytes / mss_now, bbr_min_tso_segs(sk));
	return segs;
}

/* Custom tcp_tso_autosize() for BBR, used at transmit time to cap skb size. */
static u32  bbr_tso_segs(struct sock *sk, unsigned int mss_now)
{
	return bbr_tso_segs_generic(sk, mss_now, sk->sk_gso_max_size);
}

/* Like bbr_tso_segs(), using mss_cache, ignoring driver's sk_gso_max_size. */
static u32 bbr_tso_segs_goal(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	return  bbr_tso_segs_generic(sk, tp->mss_cache, GSO_MAX_SIZE);
}

/* Save "last known good" cwnd so we can restore it after losses or PROBE_RTT */
static void bbr_save_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr->prev_ca_state < TCP_CA_Recovery && bbr->mode != BBR_PROBE_RTT)
		bbr->prior_cwnd = tp->snd_cwnd;  /* this cwnd is good enough */
	else  /* loss recovery or BBR_PROBE_RTT have temporarily cut cwnd */
		bbr->prior_cwnd = max(bbr->prior_cwnd, tp->snd_cwnd);
}

static void bbr_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	if (event == CA_EVENT_TX_START) {
		tcp_plb_check_rehash(sk, &bbr->plb);

		if (!tp->app_limited)
			return;
		bbr->idle_restart = 1;
		bbr->ack_epoch_mstamp = tp->tcp_mstamp;
		bbr->ack_epoch_acked = 0;
		/* Avoid pointless buffer overflows: pace at est. bw if we don't
		 * need more speed (we're restarting from idle and app-limited).
		 */
		if (bbr->mode == BBR_PROBE_BW)
			bbr_set_pacing_rate(sk, bbr_bw(sk), BBR_UNIT);
		else if (bbr->mode == BBR_PROBE_RTT)
			bbr_check_probe_rtt_done(sk);
	} else if ((event == CA_EVENT_ECN_IS_CE ||
		    event == CA_EVENT_ECN_NO_CE) &&
		    bbr_ecn_enable &&
		    bbr->params.precise_ece_ack) {
		u32 state = bbr->ce_state;
		dctcp_ece_ack_update(sk, event, &bbr->prior_rcv_nxt, &state);
		bbr->ce_state = state;
		if (tp->fast_ack_mode == 2 && event == CA_EVENT_ECN_IS_CE)
			tcp_enter_quickack_mode(sk, TCP_MAX_QUICKACKS);
	}
}

/* Calculate bdp based on min RTT and the estimated bottleneck bandwidth:
 *
 * bdp = ceil(bw * min_rtt * gain)
 *
 * The key factor, gain, controls the amount of queue. While a small gain
 * builds a smaller queue, it becomes more vulnerable to noise in RTT
 * measurements (e.g., delayed ACKs or other ACK compression effects). This
 * noise may cause BBR to under-estimate the rate.
 */
static u32 bbr_bdp(struct sock *sk, u32 bw, int gain)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 bdp;
	u64 w;

	/* If we've never had a valid RTT sample, cap cwnd at the initial
	 * default. This should only happen when the connection is not using TCP
	 * timestamps and has retransmitted all of the SYN/SYNACK/data packets
	 * ACKed so far. In this case, an RTO can cut cwnd to 1, in which
	 * case we need to slow-start up toward something safe: initial cwnd.
	 */
	if (unlikely(bbr->min_rtt_us == ~0U))	 /* no valid RTT samples yet? */
		return bbr->init_cwnd;  /* be safe: cap at initial cwnd */

	w = (u64)bw * bbr->min_rtt_us;

	/* Apply a gain to the given value, remove the BW_SCALE shift, and
	 * round the value up to avoid a negative feedback loop.
	 */
	bdp = (((w * gain) >> BBR_SCALE) + BW_UNIT - 1) / BW_UNIT;

	return bdp;
}

/* To achieve full performance in high-speed paths, we budget enough cwnd to
 * fit full-sized skbs in-flight on both end hosts to fully utilize the path:
 *   - one skb in sending host Qdisc,
 *   - one skb in sending host TSO/GSO engine
 *   - one skb being received by receiver host LRO/GRO/delayed-ACK engine
 * Don't worry, at low rates (bbr_min_tso_rate) this won't bloat cwnd because
 * in such cases tso_segs_goal is 1. The minimum cwnd is 4 packets,
 * which allows 2 outstanding 2-packet sequences, to try to keep pipe
 * full even with ACK-every-other-packet delayed ACKs.
 */
static u32 bbr_quantization_budget(struct sock *sk, u32 cwnd)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 tso_segs_goal;

	tso_segs_goal = 3 * bbr_tso_segs_goal(sk);

	/* Allow enough full-sized skbs in flight to utilize end systems. */
	if (bbr->params.cwnd_tso_budget == 1) {
		cwnd = max_t(u32, cwnd, tso_segs_goal);
		cwnd = max_t(u32, cwnd, bbr->params.cwnd_min_target);
	} else {
		cwnd += tso_segs_goal;
		cwnd = (cwnd + 1) & ~1U;
	}
	/* Ensure gain cycling gets inflight above BDP even for small BDPs. */
	if (bbr->mode == BBR_PROBE_BW && bbr->cycle_idx == BBR_BW_PROBE_UP)
		cwnd += 2;

	return cwnd;
}

/* Find inflight based on min RTT and the estimated bottleneck bandwidth. */
static u32 bbr_inflight(struct sock *sk, u32 bw, int gain)
{
	u32 inflight;

	inflight = bbr_bdp(sk, bw, gain);
	inflight = bbr_quantization_budget(sk, inflight);

	return inflight;
}

/* With pacing at lower layers, there's often less data "in the network" than
 * "in flight". With TSQ and departure time pacing at lower layers (e.g. fq),
 * we often have several skbs queued in the pacing layer with a pre-scheduled
 * earliest departure time (EDT). BBR adapts its pacing rate based on the
 * inflight level that it estimates has already been "baked in" by previous
 * departure time decisions. We calculate a rough estimate of the number of our
 * packets that might be in the network at the earliest departure time for the
 * next skb scheduled:
 *   in_network_at_edt = inflight_at_edt - (EDT - now) * bw
 * If we're increasing inflight, then we want to know if the transmit of the
 * EDT skb will push inflight above the target, so inflight_at_edt includes
 * bbr_tso_segs_goal() from the skb departing at EDT. If decreasing inflight,
 * then estimate if inflight will sink too low just before the EDT transmit.
 */
static u32 bbr_packets_in_net_at_edt(struct sock *sk, u32 inflight_now)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u64 now_ns, edt_ns, interval_us;
	u32 interval_delivered, inflight_at_edt;

	now_ns = tp->tcp_clock_cache;
	edt_ns = max(tp->tcp_wstamp_ns, now_ns);
	interval_us = div_u64(edt_ns - now_ns, NSEC_PER_USEC);
	interval_delivered = (u64)bbr_bw(sk) * interval_us >> BW_SCALE;
	inflight_at_edt = inflight_now;
	if (bbr->pacing_gain > BBR_UNIT)              /* increasing inflight */
		inflight_at_edt += bbr_tso_segs_goal(sk);  /* include EDT skb */
	if (interval_delivered >= inflight_at_edt)
		return 0;
	return inflight_at_edt - interval_delivered;
}

/* Find the cwnd increment based on estimate of ack aggregation */
static u32 bbr_ack_aggregation_cwnd(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 max_aggr_cwnd, aggr_cwnd = 0;

	if (bbr->params.extra_acked_gain &&
	    (bbr_full_bw_reached(sk) || bbr->params.extra_acked_in_startup)) {
		max_aggr_cwnd = ((u64)bbr_bw(sk) * bbr_extra_acked_max_us)
				/ BW_UNIT;
		aggr_cwnd = (bbr->params.extra_acked_gain * bbr_extra_acked(sk))
			     >> BBR_SCALE;
		aggr_cwnd = min(aggr_cwnd, max_aggr_cwnd);
	}

	return aggr_cwnd;
}

/* Returns the cwnd for PROBE_RTT mode. */
static u32 bbr_probe_rtt_cwnd(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr->params.probe_rtt_cwnd_gain == 0)
		return bbr->params.cwnd_min_target;
	return max_t(u32, bbr->params.cwnd_min_target,
		     bbr_bdp(sk, bbr_bw(sk), bbr->params.probe_rtt_cwnd_gain));
}

/* Slow-start up toward target cwnd (if bw estimate is growing, or packet loss
 * has drawn us down below target), or snap down to target if we're above it.
 */
static void bbr_set_cwnd(struct sock *sk, const struct rate_sample *rs,
			 u32 acked, u32 bw, int gain, u32 cwnd,
			 struct bbr_context *ctx)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 target_cwnd = 0, prev_cwnd = tp->snd_cwnd, max_probe;

	if (!acked)
		goto done;  /* no packet fully ACKed; just apply caps */

	target_cwnd = bbr_bdp(sk, bw, gain);

	/* Increment the cwnd to account for excess ACKed data that seems
	 * due to aggregation (of data and/or ACKs) visible in the ACK stream.
	 */
	target_cwnd += bbr_ack_aggregation_cwnd(sk);
	target_cwnd = bbr_quantization_budget(sk, target_cwnd);

	/* If we're below target cwnd, slow start cwnd toward target cwnd. */
	bbr->debug.target_cwnd = target_cwnd;

	/* Update cwnd and enable fast path if cwnd reaches target_cwnd. */
	bbr->try_fast_path = 0;
	if (bbr_full_bw_reached(sk)) { /* only cut cwnd if we filled the pipe */
		cwnd += acked;
		if (cwnd >= target_cwnd) {
			cwnd = target_cwnd;
			bbr->try_fast_path = 1;
		}
	} else if (cwnd < target_cwnd || cwnd  < 2 * bbr->init_cwnd) {
		cwnd += acked;
	} else {
		bbr->try_fast_path = 1;
	}

	/* When growing cwnd, don't grow beyond twice what we just probed. */
	if (bbr->params.usage_based_cwnd) {
		max_probe = max(2 * tp->max_packets_out, tp->snd_cwnd);
		cwnd = min(cwnd, max_probe);
	}

	cwnd = max_t(u32, cwnd, bbr->params.cwnd_min_target);
done:
	tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);	/* apply global cap */
	if (bbr->mode == BBR_PROBE_RTT)  /* drain queue, refresh min_rtt */
		tp->snd_cwnd = min_t(u32, tp->snd_cwnd, bbr_probe_rtt_cwnd(sk));

	ctx->target_cwnd = target_cwnd;
	ctx->log = (tp->snd_cwnd != prev_cwnd);
}

/* See if we have reached next round trip */
static void bbr_update_round_start(struct sock *sk,
		const struct rate_sample *rs, struct bbr_context *ctx)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->round_start = 0;

	/* See if we've reached the next RTT */
	if (rs->interval_us > 0 &&
	    !before(rs->prior_delivered, bbr->next_rtt_delivered)) {
		bbr->next_rtt_delivered = tp->delivered;
		bbr->round_start = 1;
	}
}

/* Calculate the bandwidth based on how fast packets are delivered */
static void bbr_calculate_bw_sample(struct sock *sk,
			const struct rate_sample *rs, struct bbr_context *ctx)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u64 bw = 0;

	/* Divide delivered by the interval to find a (lower bound) bottleneck
	 * bandwidth sample. Delivered is in packets and interval_us in uS and
	 * ratio will be <<1 for most connections. So delivered is first scaled.
	 * Round up to allow growth at low rates, even with integer division.
	 */
	if (rs->interval_us > 0) {
		if (WARN_ONCE(rs->delivered < 0,
			      "negative delivered: %d interval_us: %ld\n",
			      rs->delivered, rs->interval_us))
			return;

		bw = DIV_ROUND_UP_ULL((u64)rs->delivered * BW_UNIT, rs->interval_us);
	}

	ctx->sample_bw = bw;
	bbr->debug.rs_bw = bw;
}

/* Estimates the windowed max degree of ack aggregation.
 * This is used to provision extra in-flight data to keep sending during
 * inter-ACK silences.
 *
 * Degree of ack aggregation is estimated as extra data acked beyond expected.
 *
 * max_extra_acked = "maximum recent excess data ACKed beyond max_bw * interval"
 * cwnd += max_extra_acked
 *
 * Max extra_acked is clamped by cwnd and bw * bbr_extra_acked_max_us (100 ms).
 * Max filter is an approximate sliding window of 5-10 (packet timed) round
 * trips for non-startup phase, and 1-2 round trips for startup.
 */
static void bbr_update_ack_aggregation(struct sock *sk,
				       const struct rate_sample *rs)
{
	u32 epoch_us, expected_acked, extra_acked;
	struct bbr *bbr = inet_csk_ca(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 extra_acked_win_rtts_thresh = bbr->params.extra_acked_win_rtts;

	if (!bbr->params.extra_acked_gain || rs->acked_sacked <= 0 ||
	    rs->delivered < 0 || rs->interval_us <= 0)
		return;

	if (bbr->round_start) {
		bbr->extra_acked_win_rtts = min(0x1F,
						bbr->extra_acked_win_rtts + 1);
		if (bbr->params.extra_acked_in_startup &&
		    !bbr_full_bw_reached(sk))
			extra_acked_win_rtts_thresh = 1;
		if (bbr->extra_acked_win_rtts >=
		    extra_acked_win_rtts_thresh) {
			bbr->extra_acked_win_rtts = 0;
			bbr->extra_acked_win_idx = bbr->extra_acked_win_idx ?
						   0 : 1;
			bbr->extra_acked[bbr->extra_acked_win_idx] = 0;
		}
	}

	/* Compute how many packets we expected to be delivered over epoch. */
	epoch_us = tcp_stamp_us_delta(tp->delivered_mstamp,
				      bbr->ack_epoch_mstamp);
	expected_acked = ((u64)bbr_bw(sk) * epoch_us) / BW_UNIT;

	/* Reset the aggregation epoch if ACK rate is below expected rate or
	 * significantly large no. of ack received since epoch (potentially
	 * quite old epoch).
	 */
	if (bbr->ack_epoch_acked <= expected_acked ||
	    (bbr->ack_epoch_acked + rs->acked_sacked >=
	     bbr_ack_epoch_acked_reset_thresh)) {
		bbr->ack_epoch_acked = 0;
		bbr->ack_epoch_mstamp = tp->delivered_mstamp;
		expected_acked = 0;
	}

	/* Compute excess data delivered, beyond what was expected. */
	bbr->ack_epoch_acked = min_t(u32, 0xFFFFF,
				   bbr->ack_epoch_acked + rs->acked_sacked);
	extra_acked = bbr->ack_epoch_acked - expected_acked;
	extra_acked = min(extra_acked, tp->snd_cwnd);
	if (extra_acked > bbr->extra_acked[bbr->extra_acked_win_idx])
		bbr->extra_acked[bbr->extra_acked_win_idx] = extra_acked;
}

/* Estimate when the pipe is full, using the change in delivery rate: BBR
 * estimates that STARTUP filled the pipe if the estimated bw hasn't changed by
 * at least bbr_full_bw_thresh (25%) after bbr_full_bw_cnt (3) non-app-limited
 * rounds. Why 3 rounds: 1: rwin autotuning grows the rwin, 2: we fill the
 * higher rwin, 3: we get higher delivery rate samples. Or transient
 * cross-traffic or radio noise can go away. CUBIC Hystart shares a similar
 * design goal, but uses delay and inter-ACK spacing instead of bandwidth.
 */
static void bbr_check_full_bw_reached(struct sock *sk,
				      const struct rate_sample *rs)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 bw_thresh;

	if (bbr_full_bw_reached(sk) || !bbr->round_start || rs->is_app_limited)
		return;

	bw_thresh = (u64)bbr->full_bw * bbr->params.full_bw_thresh >> BBR_SCALE;
	if (bbr_max_bw(sk) >= bw_thresh) {
		bbr->full_bw = bbr_max_bw(sk);
		bbr->full_bw_cnt = 0;
		return;
	}
	++bbr->full_bw_cnt;
	bbr->full_bw_reached = bbr->full_bw_cnt >= bbr->params.full_bw_cnt;
}

/* If pipe is probably full, drain the queue and then enter steady-state. */
static bool bbr_check_drain(struct sock *sk, const struct rate_sample *rs,
			    struct bbr_context *ctx)
{
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr->mode == BBR_STARTUP && bbr_full_bw_reached(sk)) {
		bbr->mode = BBR_DRAIN;	/* drain queue we created */
		tcp_sk(sk)->snd_ssthresh =
				bbr_inflight(sk, bbr_max_bw(sk), BBR_UNIT);
		bbr2_reset_congestion_signals(sk);
	}	/* fall through to check if in-flight is already small: */
	if (bbr->mode == BBR_DRAIN &&
	    bbr_packets_in_net_at_edt(sk, tcp_packets_in_flight(tcp_sk(sk))) <=
	    bbr_inflight(sk, bbr_max_bw(sk), BBR_UNIT))
		return true;  /* exiting DRAIN now */
	return false;
}

static void bbr_check_probe_rtt_done(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	if (!(bbr->probe_rtt_done_stamp &&
	      after(tcp_jiffies32, bbr->probe_rtt_done_stamp)))
		return;

	bbr->probe_rtt_min_stamp = tcp_jiffies32; /* schedule next PROBE_RTT */
	tp->snd_cwnd = max(tp->snd_cwnd, bbr->prior_cwnd);
	bbr2_exit_probe_rtt(sk);
}

/* The goal of PROBE_RTT mode is to have BBR flows cooperatively and
 * periodically drain the bottleneck queue, to converge to measure the true
 * min_rtt (unloaded propagation delay). This allows the flows to keep queues
 * small (reducing queuing delay and packet loss) and achieve fairness among
 * BBR flows.
 *
 * The min_rtt filter window is 10 seconds. When the min_rtt estimate expires,
 * we enter PROBE_RTT mode and cap the cwnd at bbr_cwnd_min_target=4 packets.
 * After at least bbr_probe_rtt_mode_ms=200ms and at least one packet-timed
 * round trip elapsed with that flight size <= 4, we leave PROBE_RTT mode and
 * re-enter the previous mode. BBR uses 200ms to approximately bound the
 * performance penalty of PROBE_RTT's cwnd capping to roughly 2% (200ms/10s).
 *
 * Note that flows need only pay 2% if they are busy sending over the last 10
 * seconds. Interactive applications (e.g., Web, RPCs, video chunks) often have
 * natural silences or low-rate periods within 10 seconds where the rate is low
 * enough for long enough to drain its queue in the bottleneck. We pick up
 * these min RTT measurements opportunistically with our min_rtt filter. :-)
 */
static void bbr_update_min_rtt(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	bool probe_rtt_expired, min_rtt_expired;
	u32 expire;

	/* Track min RTT in probe_rtt_win_ms to time next PROBE_RTT state. */
	expire = bbr->probe_rtt_min_stamp +
		 msecs_to_jiffies(bbr->params.probe_rtt_win_ms);
	probe_rtt_expired = after(tcp_jiffies32, expire);
	if (rs->rtt_us >= 0 &&
	    (rs->rtt_us <= bbr->probe_rtt_min_us ||
	     (probe_rtt_expired && !rs->is_ack_delayed))) {
		bbr->probe_rtt_min_us = rs->rtt_us;
		bbr->probe_rtt_min_stamp = tcp_jiffies32;
	}
	/* Track min RTT seen in the min_rtt_win_sec filter window: */
	expire = bbr->min_rtt_stamp + bbr->params.min_rtt_win_sec * HZ;
	min_rtt_expired = after(tcp_jiffies32, expire);
	if (bbr->probe_rtt_min_us <= bbr->min_rtt_us ||
	    min_rtt_expired) {
		bbr->min_rtt_us = bbr->probe_rtt_min_us;
		bbr->min_rtt_stamp = bbr->probe_rtt_min_stamp;
	}

	if (bbr->params.probe_rtt_mode_ms > 0 && probe_rtt_expired &&
	    !bbr->idle_restart && bbr->mode != BBR_PROBE_RTT) {
		bbr->mode = BBR_PROBE_RTT;  /* dip, drain queue */
		bbr_save_cwnd(sk);  /* note cwnd so we can restore it */
		bbr->probe_rtt_done_stamp = 0;
		bbr->ack_phase = BBR_ACKS_PROBE_STOPPING;
		bbr->next_rtt_delivered = tp->delivered;
	}

	if (bbr->mode == BBR_PROBE_RTT) {
		/* Ignore low rate samples during this mode. */
		tp->app_limited =
			(tp->delivered + tcp_packets_in_flight(tp)) ? : 1;
		/* Maintain min packets in flight for max(200 ms, 1 round). */
		if (!bbr->probe_rtt_done_stamp &&
		    tcp_packets_in_flight(tp) <= bbr_probe_rtt_cwnd(sk)) {
			bbr->probe_rtt_done_stamp = tcp_jiffies32 +
				msecs_to_jiffies(bbr->params.probe_rtt_mode_ms);
			bbr->probe_rtt_round_done = 0;
			bbr->next_rtt_delivered = tp->delivered;
		} else if (bbr->probe_rtt_done_stamp) {
			if (bbr->round_start)
				bbr->probe_rtt_round_done = 1;
			if (bbr->probe_rtt_round_done)
				bbr_check_probe_rtt_done(sk);
		}
	}
	/* Restart after idle ends only once we process a new S/ACK for data */
	if (rs->delivered > 0)
		bbr->idle_restart = 0;
}

static void bbr_update_gains(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	switch (bbr->mode) {
	case BBR_STARTUP:
		bbr->pacing_gain = bbr->params.high_gain;
		bbr->cwnd_gain	 = bbr->params.startup_cwnd_gain;
		break;
	case BBR_DRAIN:
		bbr->pacing_gain = bbr->params.drain_gain;  /* slow, to drain */
		bbr->cwnd_gain = bbr->params.startup_cwnd_gain;  /* keep cwnd */
		break;
	case BBR_PROBE_BW:
		bbr->pacing_gain = bbr->params.pacing_gain[bbr->cycle_idx];
		bbr->cwnd_gain = bbr->params.cwnd_gain;
		break;
	case BBR_PROBE_RTT:
		bbr->pacing_gain = BBR_UNIT;
		bbr->cwnd_gain = BBR_UNIT;
		break;
	default:
		WARN_ONCE(1, "BBR bad mode: %u\n", bbr->mode);
		break;
	}
}

static void bbr_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	int i;

	WARN_ON_ONCE(tp->snd_cwnd >= bbr_cwnd_warn_val);

	bbr->initialized = 1;
	bbr->params.high_gain = min(0x7FF, bbr_high_gain);
	bbr->params.drain_gain = min(0x3FF, bbr_drain_gain);
	bbr->params.startup_cwnd_gain = min(0x7FF, bbr_startup_cwnd_gain);
	bbr->params.cwnd_gain = min(0x7FF, bbr_cwnd_gain);
	bbr->params.cwnd_tso_budget = min(0x1U, bbr_cwnd_tso_budget);
	bbr->params.cwnd_min_target = min(0xFU, bbr_cwnd_min_target);
	bbr->params.min_rtt_win_sec = min(0x1FU, bbr_min_rtt_win_sec);
	bbr->params.probe_rtt_mode_ms = min(0x1FFU, bbr_probe_rtt_mode_ms);
	bbr->params.full_bw_cnt = min(0x7U, bbr_full_bw_cnt);
	bbr->params.full_bw_thresh = min(0x3FFU, bbr_full_bw_thresh);
	bbr->params.extra_acked_gain = min(0x7FF, bbr_extra_acked_gain);
	bbr->params.extra_acked_win_rtts = min(0x1FU, bbr_extra_acked_win_rtts);
	bbr->params.drain_to_target = bbr_drain_to_target ? 1 : 0;
	bbr->params.precise_ece_ack = bbr_precise_ece_ack ? 1 : 0;
	bbr->params.extra_acked_in_startup = bbr_extra_acked_in_startup ? 1 : 0;
	bbr->params.probe_rtt_cwnd_gain = min(0xFFU, bbr_probe_rtt_cwnd_gain);
	bbr->params.probe_rtt_win_ms =
		min(0x3FFFU,
		    min_t(u32, bbr_probe_rtt_win_ms,
			  bbr->params.min_rtt_win_sec * MSEC_PER_SEC));
	for (i = 0; i < CYCLE_LEN; i++)
		bbr->params.pacing_gain[i] = min(0x3FF, bbr_pacing_gain[i]);
	bbr->params.usage_based_cwnd = bbr_usage_based_cwnd ? 1 : 0;
	bbr->params.tso_rtt_shift =  min(0xFU, bbr_tso_rtt_shift);

	bbr->debug.snd_isn = tp->snd_una;
	bbr->debug.target_cwnd = 0;
	bbr->debug.undo = 0;

	bbr->init_cwnd = min(0x7FU, tp->snd_cwnd);
	bbr->prior_cwnd = tp->prior_cwnd;
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	bbr->next_rtt_delivered = 0;
	bbr->prev_ca_state = TCP_CA_Open;
	bbr->packet_conservation = 0;

	bbr->probe_rtt_done_stamp = 0;
	bbr->probe_rtt_round_done = 0;
	bbr->probe_rtt_min_us = tcp_min_rtt(tp);
	bbr->probe_rtt_min_stamp = tcp_jiffies32;
	bbr->min_rtt_us = tcp_min_rtt(tp);
	bbr->min_rtt_stamp = tcp_jiffies32;

	bbr->has_seen_rtt = 0;
	bbr_init_pacing_rate_from_rtt(sk);

	bbr->round_start = 0;
	bbr->idle_restart = 0;
	bbr->full_bw_reached = 0;
	bbr->full_bw = 0;
	bbr->full_bw_cnt = 0;
	bbr->cycle_mstamp = 0;
	bbr->cycle_idx = 0;
	bbr->mode = BBR_STARTUP;
	bbr->debug.rs_bw = 0;

	bbr->ack_epoch_mstamp = tp->tcp_mstamp;
	bbr->ack_epoch_acked = 0;
	bbr->extra_acked_win_rtts = 0;
	bbr->extra_acked_win_idx = 0;
	bbr->extra_acked[0] = 0;
	bbr->extra_acked[1] = 0;

	bbr->ce_state = 0;
	bbr->prior_rcv_nxt = tp->rcv_nxt;
	bbr->try_fast_path = 0;

	cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static u32 bbr_sndbuf_expand(struct sock *sk)
{
	/* Provision 3 * cwnd since BBR may slow-start even during recovery. */
	return 3;
}

/* __________________________________________________________________________
 *
 * Functions new to BBR v2 ("bbr") congestion control are below here.
 * __________________________________________________________________________
 */

/* Incorporate a new bw sample into the current window of our max filter. */
static void bbr2_take_bw_hi_sample(struct sock *sk, u32 bw)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->bw_hi[1] = max(bw, bbr->bw_hi[1]);
}

/* Keep max of last 1-2 cycles. Each PROBE_BW cycle, flip filter window. */
static void bbr2_advance_bw_hi_filter(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	if (!bbr->bw_hi[1])
		return;  /* no samples in this window; remember old window */
	bbr->bw_hi[0] = bbr->bw_hi[1];
	bbr->bw_hi[1] = 0;
}

/* How much do we want in flight? Our BDP, unless congestion cut cwnd. */
static u32 bbr2_target_inflight(struct sock *sk)
{
	u32 bdp = bbr_inflight(sk, bbr_bw(sk), BBR_UNIT);

	return min(bdp, tcp_sk(sk)->snd_cwnd);
}

static bool bbr2_is_probing_bandwidth(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	return (bbr->mode == BBR_STARTUP) ||
		(bbr->mode == BBR_PROBE_BW &&
		 (bbr->cycle_idx == BBR_BW_PROBE_REFILL ||
		  bbr->cycle_idx == BBR_BW_PROBE_UP));
}

/* Has the given amount of time elapsed since we marked the phase start? */
static bool bbr2_has_elapsed_in_phase(const struct sock *sk, u32 interval_us)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	const struct bbr *bbr = inet_csk_ca(sk);

	return tcp_stamp_us_delta(tp->tcp_mstamp,
				  bbr->cycle_mstamp + interval_us) > 0;
}

static void bbr2_handle_queue_too_high_in_startup(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->full_bw_reached = 1;
	bbr->inflight_hi = bbr_inflight(sk, bbr_max_bw(sk), BBR_UNIT);
}

/* Exit STARTUP upon N consecutive rounds with ECN mark rate > ecn_thresh. */
static void bbr2_check_ecn_too_high_in_startup(struct sock *sk, u32 ce_ratio)
{
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr_full_bw_reached(sk) || !bbr->ecn_eligible ||
	    !bbr->params.full_ecn_cnt || !bbr->params.ecn_thresh)
		return;

	if (ce_ratio >= bbr->params.ecn_thresh)
		bbr->startup_ecn_rounds++;
	else
		bbr->startup_ecn_rounds = 0;

	if (bbr->startup_ecn_rounds >= bbr->params.full_ecn_cnt) {
		bbr->debug.event = 'E';  /* ECN caused STARTUP exit */
		bbr2_handle_queue_too_high_in_startup(sk);
		return;
	}
}

static int bbr2_update_ecn_alpha(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	s32 delivered, delivered_ce;
	u64 alpha, ce_ratio;
	u32 gain;

	if (bbr->params.ecn_factor == 0)
		return -1;

	delivered = tp->delivered - bbr->alpha_last_delivered;
	delivered_ce = tp->delivered_ce - bbr->alpha_last_delivered_ce;

	if (delivered == 0 ||		/* avoid divide by zero */
	    WARN_ON_ONCE(delivered < 0 || delivered_ce < 0))  /* backwards? */
		return -1;

	/* See if we should use ECN sender logic for this connection. */
	if (!bbr->ecn_eligible && bbr_ecn_enable &&
	    (bbr->min_rtt_us <= bbr->params.ecn_max_rtt_us ||
	     !bbr->params.ecn_max_rtt_us))
		bbr->ecn_eligible = 1;

	ce_ratio = (u64)delivered_ce << BBR_SCALE;
	do_div(ce_ratio, delivered);
	gain = bbr->params.ecn_alpha_gain;
	alpha = ((BBR_UNIT - gain) * bbr->ecn_alpha) >> BBR_SCALE;
	alpha += (gain * ce_ratio) >> BBR_SCALE;
	bbr->ecn_alpha = min_t(u32, alpha, BBR_UNIT);

	bbr->alpha_last_delivered = tp->delivered;
	bbr->alpha_last_delivered_ce = tp->delivered_ce;

	bbr2_check_ecn_too_high_in_startup(sk, ce_ratio);
	return (int)ce_ratio;
}

/* Each round trip of BBR_BW_PROBE_UP, double volume of probing data. */
static void bbr2_raise_inflight_hi_slope(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 growth_this_round, cnt;

	/* Calculate "slope": packets S/Acked per inflight_hi increment. */
	growth_this_round = 1 << bbr->bw_probe_up_rounds;
	bbr->bw_probe_up_rounds = min(bbr->bw_probe_up_rounds + 1, 30);
	cnt = tp->snd_cwnd / growth_this_round;
	cnt = max(cnt, 1U);
	bbr->bw_probe_up_cnt = cnt;
	bbr->debug.event = 'G';  /* Grow inflight_hi slope */
}

/* In BBR_BW_PROBE_UP, not seeing high loss/ECN/queue, so raise inflight_hi. */
static void bbr2_probe_inflight_hi_upward(struct sock *sk,
					  const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 delta;

	if (!tp->is_cwnd_limited || tp->snd_cwnd < bbr->inflight_hi) {
		bbr->bw_probe_up_acks = 0;  /* don't accmulate unused credits */
		return;  /* not fully using inflight_hi, so don't grow it */
	}

	/* For each bw_probe_up_cnt packets ACKed, increase inflight_hi by 1. */
	bbr->bw_probe_up_acks += rs->acked_sacked;
	if (bbr->bw_probe_up_acks >=  bbr->bw_probe_up_cnt) {
		delta = bbr->bw_probe_up_acks / bbr->bw_probe_up_cnt;
		bbr->bw_probe_up_acks -= delta * bbr->bw_probe_up_cnt;
		bbr->inflight_hi += delta;
		bbr->debug.event = 'I';  /* Increment inflight_hi */
	}

	if (bbr->round_start)
		bbr2_raise_inflight_hi_slope(sk);
}

/* Does loss/ECN rate for this sample say inflight is "too high"?
 * This is used by both the bbr_check_loss_too_high_in_startup() function,
 * which can be used in either v1 or v2, and the PROBE_UP phase of v2, which
 * uses it to notice when loss/ECN rates suggest inflight is too high.
 */
static bool bbr2_is_inflight_too_high(const struct sock *sk,
				     const struct rate_sample *rs)
{
	const struct bbr *bbr = inet_csk_ca(sk);
	u32 loss_thresh, ecn_thresh;

	if (rs->lost > 0 && rs->tx_in_flight) {
		loss_thresh = (u64)rs->tx_in_flight * bbr->params.loss_thresh >>
				BBR_SCALE;
		if (rs->lost > loss_thresh)
			return true;
	}

	if (rs->delivered_ce > 0 && rs->delivered > 0 &&
	    bbr->ecn_eligible && bbr->params.ecn_thresh) {
		ecn_thresh = (u64)rs->delivered * bbr->params.ecn_thresh >>
				BBR_SCALE;
		if (rs->delivered_ce >= ecn_thresh)
			return true;
	}

	return false;
}

/* Calculate the tx_in_flight level that corresponded to excessive loss.
 * We find "lost_prefix" segs of the skb where loss rate went too high,
 * by solving for "lost_prefix" in the following equation:
 *   lost                     /  inflight                     >= loss_thresh
 *  (lost_prev + lost_prefix) / (inflight_prev + lost_prefix) >= loss_thresh
 * Then we take that equation, convert it to fixed point, and
 * round up to the nearest packet.
 */
static u32 bbr2_inflight_hi_from_lost_skb(const struct sock *sk,
					  const struct rate_sample *rs,
					  const struct sk_buff *skb)
{
	const struct bbr *bbr = inet_csk_ca(sk);
	u32 loss_thresh  = bbr->params.loss_thresh;
	u32 pcount, divisor, inflight_hi;
	s32 inflight_prev, lost_prev;
	u64 loss_budget, lost_prefix;

	pcount = tcp_skb_pcount(skb);

	/* How much data was in flight before this skb? */
	inflight_prev = rs->tx_in_flight - pcount;
	if (WARN_ONCE(inflight_prev < 0,
		      "tx_in_flight: %u pcount: %u reneg: %u",
		      rs->tx_in_flight, pcount, tcp_sk(sk)->is_sack_reneg))
		return ~0U;

	/* How much inflight data was marked lost before this skb? */
	lost_prev = rs->lost - pcount;
	if (WARN_ON_ONCE(lost_prev < 0))
		return ~0U;

	/* At what prefix of this lost skb did losss rate exceed loss_thresh? */
	loss_budget = (u64)inflight_prev * loss_thresh + BBR_UNIT - 1;
	loss_budget >>= BBR_SCALE;
	if (lost_prev >= loss_budget) {
		lost_prefix = 0;   /* previous losses crossed loss_thresh */
	} else {
		lost_prefix = loss_budget - lost_prev;
		lost_prefix <<= BBR_SCALE;
		divisor = BBR_UNIT - loss_thresh;
		if (WARN_ON_ONCE(!divisor))  /* loss_thresh is 8 bits */
			return ~0U;
		do_div(lost_prefix, divisor);
	}

	inflight_hi = inflight_prev + lost_prefix;
	return inflight_hi;
}

/* If loss/ECN rates during probing indicated we may have overfilled a
 * buffer, return an operating point that tries to leave unutilized headroom in
 * the path for other flows, for fairness convergence and lower RTTs and loss.
 */
static u32 bbr2_inflight_with_headroom(const struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 headroom, headroom_fraction;

	if (bbr->inflight_hi == ~0U)
		return ~0U;

	headroom_fraction = bbr->params.inflight_headroom;
	headroom = ((u64)bbr->inflight_hi * headroom_fraction) >> BBR_SCALE;
	headroom = max(headroom, 1U);
	return max_t(s32, bbr->inflight_hi - headroom,
		     bbr->params.cwnd_min_target);
}

/* Bound cwnd to a sensible level, based on our current probing state
 * machine phase and model of a good inflight level (inflight_lo, inflight_hi).
 */
static void bbr2_bound_cwnd_for_inflight_model(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 cap;

	/* tcp_rcv_synsent_state_process() currently calls tcp_ack()
	 * and thus cong_control() without first initializing us(!).
	 */
	if (!bbr->initialized)
		return;

	cap = ~0U;
	if (bbr->mode == BBR_PROBE_BW &&
	    bbr->cycle_idx != BBR_BW_PROBE_CRUISE) {
		/* Probe to see if more packets fit in the path. */
		cap = bbr->inflight_hi;
	} else {
		if (bbr->mode == BBR_PROBE_RTT ||
		    (bbr->mode == BBR_PROBE_BW &&
		     bbr->cycle_idx == BBR_BW_PROBE_CRUISE))
			cap = bbr2_inflight_with_headroom(sk);
	}
	/* Adapt to any loss/ECN since our last bw probe. */
	cap = min(cap, bbr->inflight_lo);

	cap = max_t(u32, cap, bbr->params.cwnd_min_target);
	tp->snd_cwnd = min(cap, tp->snd_cwnd);
}

/* Estimate a short-term lower bound on the capacity available now, based
 * on measurements of the current delivery process and recent history. When we
 * are seeing loss/ECN at times when we are not probing bw, then conservatively
 * move toward flow balance by multiplicatively cutting our short-term
 * estimated safe rate and volume of data (bw_lo and inflight_lo). We use a
 * multiplicative decrease in order to converge to a lower capacity in time
 * logarithmic in the magnitude of the decrease.
 *
 * However, we do not cut our short-term estimates lower than the current rate
 * and volume of delivered data from this round trip, since from the current
 * delivery process we can estimate the measured capacity available now.
 *
 * Anything faster than that approach would knowingly risk high loss, which can
 * cause low bw for Reno/CUBIC and high loss recovery latency for
 * request/response flows using any congestion control.
 */
static void bbr2_adapt_lower_bounds(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u32 ecn_cut, ecn_inflight_lo, beta;

	/* We only use lower-bound estimates when not probing bw.
	 * When probing we need to push inflight higher to probe bw.
	 */
	if (bbr2_is_probing_bandwidth(sk))
		return;

	/* ECN response. */
	if (bbr->ecn_in_round && bbr->ecn_eligible && bbr->params.ecn_factor) {
		/* Reduce inflight to (1 - alpha*ecn_factor). */
		ecn_cut = (BBR_UNIT -
			   ((bbr->ecn_alpha * bbr->params.ecn_factor) >>
			    BBR_SCALE));
		if (bbr->inflight_lo == ~0U)
			bbr->inflight_lo = tp->snd_cwnd;
		ecn_inflight_lo = (u64)bbr->inflight_lo * ecn_cut >> BBR_SCALE;
	} else {
		ecn_inflight_lo = ~0U;
	}

	/* Loss response. */
	if (bbr->loss_in_round) {
		/* Reduce bw and inflight to (1 - beta). */
		if (bbr->bw_lo == ~0U)
			bbr->bw_lo = bbr_max_bw(sk);
		if (bbr->inflight_lo == ~0U)
			bbr->inflight_lo = tp->snd_cwnd;
		beta = bbr->params.beta;
		bbr->bw_lo =
			max_t(u32, bbr->bw_latest,
			      (u64)bbr->bw_lo *
			      (BBR_UNIT - beta) >> BBR_SCALE);
		bbr->inflight_lo =
			max_t(u32, bbr->inflight_latest,
			      (u64)bbr->inflight_lo *
			      (BBR_UNIT - beta) >> BBR_SCALE);
	}

	/* Adjust to the lower of the levels implied by loss or ECN. */
	bbr->inflight_lo = min(bbr->inflight_lo, ecn_inflight_lo);
}

/* Reset any short-term lower-bound adaptation to congestion, so that we can
 * push our inflight up.
 */
static void bbr2_reset_lower_bounds(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->bw_lo = ~0U;
	bbr->inflight_lo = ~0U;
}

/* After bw probing (STARTUP/PROBE_UP), reset signals before entering a state
 * machine phase where we adapt our lower bound based on congestion signals.
 */
static void bbr2_reset_congestion_signals(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->loss_in_round = 0;
	bbr->ecn_in_round = 0;
	bbr->loss_in_cycle = 0;
	bbr->ecn_in_cycle = 0;
	bbr->bw_latest = 0;
	bbr->inflight_latest = 0;
}

/* Update (most of) our congestion signals: track the recent rate and volume of
 * delivered data, presence of loss, and EWMA degree of ECN marking.
 */
static void bbr2_update_congestion_signals(
	struct sock *sk, const struct rate_sample *rs, struct bbr_context *ctx)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	u64 bw;

	bbr->loss_round_start = 0;
	if (rs->interval_us <= 0 || !rs->acked_sacked)
		return; /* Not a valid observation */
	bw = ctx->sample_bw;

	if (!rs->is_app_limited || bw >= bbr_max_bw(sk))
		bbr2_take_bw_hi_sample(sk, bw);

	bbr->loss_in_round |= (rs->losses > 0);

	/* Update rate and volume of delivered data from latest round trip: */
	bbr->bw_latest       = max_t(u32, bbr->bw_latest,       ctx->sample_bw);
	bbr->inflight_latest = max_t(u32, bbr->inflight_latest, rs->delivered);

	if (before(rs->prior_delivered, bbr->loss_round_delivered))
		return;		/* skip the per-round-trip updates */
	/* Now do per-round-trip updates. */
	bbr->loss_round_delivered = tp->delivered;  /* mark round trip */
	bbr->loss_round_start = 1;
	bbr2_adapt_lower_bounds(sk);

	/* Update windowed "latest" (single-round-trip) filters. */
	bbr->loss_in_round = 0;
	bbr->ecn_in_round  = 0;
	bbr->bw_latest = ctx->sample_bw;
	bbr->inflight_latest = rs->delivered;
}

/* Bandwidth probing can cause loss. To help coexistence with loss-based
 * congestion control we spread out our probing in a Reno-conscious way. Due to
 * the shape of the Reno sawtooth, the time required between loss epochs for an
 * idealized Reno flow is a number of round trips that is the BDP of that
 * flow. We count packet-timed round trips directly, since measured RTT can
 * vary widely, and Reno is driven by packet-timed round trips.
 */
static bool bbr2_is_reno_coexistence_probe_time(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 inflight, rounds, reno_gain, reno_rounds;

	/* Random loss can shave some small percentage off of our inflight
	 * in each round. To survive this, flows need robust periodic probes.
	 */
	rounds = bbr->params.bw_probe_max_rounds;

	reno_gain = bbr->params.bw_probe_reno_gain;
	if (reno_gain) {
		inflight = bbr2_target_inflight(sk);
		reno_rounds = ((u64)inflight * reno_gain) >> BBR_SCALE;
		rounds = min(rounds, reno_rounds);
	}
	return bbr->rounds_since_probe >= rounds;
}

/* How long do we want to wait before probing for bandwidth (and risking
 * loss)? We randomize the wait, for better mixing and fairness convergence.
 *
 * We bound the Reno-coexistence inter-bw-probe time to be 62-63 round trips.
 * This is calculated to allow fairness with a 25Mbps, 30ms Reno flow,
 * (eg 4K video to a broadband user):
 *   BDP = 25Mbps * .030sec /(1514bytes) = 61.9 packets
 *
 * We bound the BBR-native inter-bw-probe wall clock time to be:
 *  (a) higher than 2 sec: to try to avoid causing loss for a long enough time
 *      to allow Reno at 30ms to get 4K video bw, the inter-bw-probe time must
 *      be at least: 25Mbps * .030sec / (1514bytes) * 0.030sec = 1.9secs
 *  (b) lower than 3 sec: to ensure flows can start probing in a reasonable
 *      amount of time to discover unutilized bw on human-scale interactive
 *      time-scales (e.g. perhaps traffic from a web page download that we
 *      were competing with is now complete).
 */
static void bbr2_pick_probe_wait(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	/* Decide the random round-trip bound for wait until probe: */
	bbr->rounds_since_probe =
		prandom_u32_max(bbr->params.bw_probe_rand_rounds);
	/* Decide the random wall clock bound for wait until probe: */
	bbr->probe_wait_us = bbr->params.bw_probe_base_us +
			     prandom_u32_max(bbr->params.bw_probe_rand_us);
}

static void bbr2_set_cycle_idx(struct sock *sk, int cycle_idx)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->cycle_idx = cycle_idx;
	/* New phase, so need to update cwnd and pacing rate. */
	bbr->try_fast_path = 0;
}

/* Send at estimated bw to fill the pipe, but not queue. We need this phase
 * before PROBE_UP, because as soon as we send faster than the available bw
 * we will start building a queue, and if the buffer is shallow we can cause
 * loss. If we do not fill the pipe before we cause this loss, our bw_hi and
 * inflight_hi estimates will underestimate.
 */
static void bbr2_start_bw_probe_refill(struct sock *sk, u32 bw_probe_up_rounds)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	bbr2_reset_lower_bounds(sk);
	if (bbr->inflight_hi != ~0U)
		bbr->inflight_hi += bbr->params.refill_add_inc;
	bbr->bw_probe_up_rounds = bw_probe_up_rounds;
	bbr->bw_probe_up_acks = 0;
	bbr->stopped_risky_probe = 0;
	bbr->ack_phase = BBR_ACKS_REFILLING;
	bbr->next_rtt_delivered = tp->delivered;
	bbr2_set_cycle_idx(sk, BBR_BW_PROBE_REFILL);
}

/* Now probe max deliverable data rate and volume. */
static void bbr2_start_bw_probe_up(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->ack_phase = BBR_ACKS_PROBE_STARTING;
	bbr->next_rtt_delivered = tp->delivered;
	bbr->cycle_mstamp = tp->tcp_mstamp;
	bbr2_set_cycle_idx(sk, BBR_BW_PROBE_UP);
	bbr2_raise_inflight_hi_slope(sk);
}

/* Start a new PROBE_BW probing cycle of some wall clock length. Pick a wall
 * clock time at which to probe beyond an inflight that we think to be
 * safe. This will knowingly risk packet loss, so we want to do this rarely, to
 * keep packet loss rates low. Also start a round-trip counter, to probe faster
 * if we estimate a Reno flow at our BDP would probe faster.
 */
static void bbr2_start_bw_probe_down(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	bbr2_reset_congestion_signals(sk);
	bbr->bw_probe_up_cnt = ~0U;     /* not growing inflight_hi any more */
	bbr2_pick_probe_wait(sk);
	bbr->cycle_mstamp = tp->tcp_mstamp;		/* start wall clock */
	bbr->ack_phase = BBR_ACKS_PROBE_STOPPING;
	bbr->next_rtt_delivered = tp->delivered;
	bbr2_set_cycle_idx(sk, BBR_BW_PROBE_DOWN);
}

/* Cruise: maintain what we estimate to be a neutral, conservative
 * operating point, without attempting to probe up for bandwidth or down for
 * RTT, and only reducing inflight in response to loss/ECN signals.
 */
static void bbr2_start_bw_probe_cruise(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr->inflight_lo != ~0U)
		bbr->inflight_lo = min(bbr->inflight_lo, bbr->inflight_hi);

	bbr2_set_cycle_idx(sk, BBR_BW_PROBE_CRUISE);
}

/* Loss and/or ECN rate is too high while probing.
 * Adapt (once per bw probe) by cutting inflight_hi and then restarting cycle.
 */
static void bbr2_handle_inflight_too_high(struct sock *sk,
					  const struct rate_sample *rs)
{
	struct bbr *bbr = inet_csk_ca(sk);
	const u32 beta = bbr->params.beta;

	bbr->prev_probe_too_high = 1;
	bbr->bw_probe_samples = 0;  /* only react once per probe */
	bbr->debug.event = 'L';     /* Loss/ECN too high */
	/* If we are app-limited then we are not robustly
	 * probing the max volume of inflight data we think
	 * might be safe (analogous to how app-limited bw
	 * samples are not known to be robustly probing bw).
	 */
	if (!rs->is_app_limited)
		bbr->inflight_hi = max_t(u32, rs->tx_in_flight,
					 (u64)bbr2_target_inflight(sk) *
					 (BBR_UNIT - beta) >> BBR_SCALE);
	if (bbr->mode == BBR_PROBE_BW && bbr->cycle_idx == BBR_BW_PROBE_UP)
		bbr2_start_bw_probe_down(sk);
}

/* If we're seeing bw and loss samples reflecting our bw probing, adapt
 * using the signals we see. If loss or ECN mark rate gets too high, then adapt
 * inflight_hi downward. If we're able to push inflight higher without such
 * signals, push higher: adapt inflight_hi upward.
 */
static bool bbr2_adapt_upper_bounds(struct sock *sk,
				   const struct rate_sample *rs)
{
	struct bbr *bbr = inet_csk_ca(sk);

	/* Track when we'll see bw/loss samples resulting from our bw probes. */
	if (bbr->ack_phase == BBR_ACKS_PROBE_STARTING && bbr->round_start)
		bbr->ack_phase = BBR_ACKS_PROBE_FEEDBACK;
	if (bbr->ack_phase == BBR_ACKS_PROBE_STOPPING && bbr->round_start) {
		/* End of samples from bw probing phase. */
		bbr->bw_probe_samples = 0;
		bbr->ack_phase = BBR_ACKS_INIT;
		/* At this point in the cycle, our current bw sample is also
		 * our best recent chance at finding the highest available bw
		 * for this flow. So now is the best time to forget the bw
		 * samples from the previous cycle, by advancing the window.
		 */
		if (bbr->mode == BBR_PROBE_BW && !rs->is_app_limited)
			bbr2_advance_bw_hi_filter(sk);
		/* If we had an inflight_hi, then probed and pushed inflight all
		 * the way up to hit that inflight_hi without seeing any
		 * high loss/ECN in all the resulting ACKs from that probing,
		 * then probe up again, this time letting inflight persist at
		 * inflight_hi for a round trip, then accelerating beyond.
		 */
		if (bbr->mode == BBR_PROBE_BW &&
		    bbr->stopped_risky_probe && !bbr->prev_probe_too_high) {
			bbr->debug.event = 'R';  /* reprobe */
			bbr2_start_bw_probe_refill(sk, 0);
			return true;  /* yes, decided state transition */
		}
	}

	if (bbr2_is_inflight_too_high(sk, rs)) {
		if (bbr->bw_probe_samples)  /*  sample is from bw probing? */
			bbr2_handle_inflight_too_high(sk, rs);
	} else {
		/* Loss/ECN rate is declared safe. Adjust upper bound upward. */
		if (bbr->inflight_hi == ~0U)  /* no excess queue signals yet? */
			return false;

		/* To be resilient to random loss, we must raise inflight_hi
		 * if we observe in any phase that a higher level is safe.
		 */
		if (rs->tx_in_flight > bbr->inflight_hi) {
			bbr->inflight_hi = rs->tx_in_flight;
			bbr->debug.event = 'U';  /* raise up inflight_hi */
		}

		if (bbr->mode == BBR_PROBE_BW &&
		    bbr->cycle_idx == BBR_BW_PROBE_UP)
			bbr2_probe_inflight_hi_upward(sk, rs);
	}

	return false;
}

/* Check if it's time to probe for bandwidth now, and if so, kick it off. */
static bool bbr2_check_time_to_probe_bw(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 n;

	/* If we seem to be at an operating point where we are not seeing loss
	 * but we are seeing ECN marks, then when the ECN marks cease we reprobe
	 * quickly (in case a burst of cross-traffic has ceased and freed up bw,
	 * or in case we are sharing with multiplicatively probing traffic).
	 */
	if (bbr->params.ecn_reprobe_gain && bbr->ecn_eligible &&
	    bbr->ecn_in_cycle && !bbr->loss_in_cycle &&
	    inet_csk(sk)->icsk_ca_state == TCP_CA_Open) {
		bbr->debug.event = 'A';  /* *A*ll clear to probe *A*gain */
		/* Calculate n so that when bbr2_raise_inflight_hi_slope()
		 * computes growth_this_round as 2^n it will be roughly the
		 * desired volume of data (inflight_hi*ecn_reprobe_gain).
		 */
		n = ilog2((((u64)bbr->inflight_hi *
			    bbr->params.ecn_reprobe_gain) >> BBR_SCALE));
		bbr2_start_bw_probe_refill(sk, n);
		return true;
	}

	if (bbr2_has_elapsed_in_phase(sk, bbr->probe_wait_us) ||
	    bbr2_is_reno_coexistence_probe_time(sk)) {
		bbr2_start_bw_probe_refill(sk, 0);
		return true;
	}
	return false;
}

/* Is it time to transition from PROBE_DOWN to PROBE_CRUISE? */
static bool bbr2_check_time_to_cruise(struct sock *sk, u32 inflight, u32 bw)
{
	struct bbr *bbr = inet_csk_ca(sk);
	bool is_under_bdp, is_long_enough;

	/* Always need to pull inflight down to leave headroom in queue. */
	if (inflight > bbr2_inflight_with_headroom(sk))
		return false;

	is_under_bdp = inflight <= bbr_inflight(sk, bw, BBR_UNIT);
	if (bbr->params.drain_to_target)
		return is_under_bdp;

	is_long_enough = bbr2_has_elapsed_in_phase(sk, bbr->min_rtt_us);
	return is_under_bdp || is_long_enough;
}

/* PROBE_BW state machine: cruise, refill, probe for bw, or drain? */
static void bbr2_update_cycle_phase(struct sock *sk,
				    const struct rate_sample *rs)
{
	struct bbr *bbr = inet_csk_ca(sk);
	bool is_risky = false, is_queuing = false;
	u32 inflight, bw;

	if (!bbr_full_bw_reached(sk))
		return;

	/* In DRAIN, PROBE_BW, or PROBE_RTT, adjust upper bounds. */
	if (bbr2_adapt_upper_bounds(sk, rs))
		return;		/* already decided state transition */

	if (bbr->mode != BBR_PROBE_BW)
		return;

	inflight = bbr_packets_in_net_at_edt(sk, rs->prior_in_flight);
	bw = bbr_max_bw(sk);

	switch (bbr->cycle_idx) {
	/* First we spend most of our time cruising with a pacing_gain of 1.0,
	 * which paces at the estimated bw, to try to fully use the pipe
	 * without building queue. If we encounter loss/ECN marks, we adapt
	 * by slowing down.
	 */
	case BBR_BW_PROBE_CRUISE:
		if (bbr2_check_time_to_probe_bw(sk))
			return;		/* already decided state transition */
		break;

	/* After cruising, when it's time to probe, we first "refill": we send
	 * at the estimated bw to fill the pipe, before probing higher and
	 * knowingly risking overflowing the bottleneck buffer (causing loss).
	 */
	case BBR_BW_PROBE_REFILL:
		if (bbr->round_start) {
			/* After one full round trip of sending in REFILL, we
			 * start to see bw samples reflecting our REFILL, which
			 * may be putting too much data in flight.
			 */
			bbr->bw_probe_samples = 1;
			bbr2_start_bw_probe_up(sk);
		}
		break;

	/* After we refill the pipe, we probe by using a pacing_gain > 1.0, to
	 * probe for bw. If we have not seen loss/ECN, we try to raise inflight
	 * to at least pacing_gain*BDP; note that this may take more than
	 * min_rtt if min_rtt is small (e.g. on a LAN).
	 *
	 * We terminate PROBE_UP bandwidth probing upon any of the following:
	 *
	 * (1) We've pushed inflight up to hit the inflight_hi target set in the
	 *     most recent previous bw probe phase. Thus we want to start
	 *     draining the queue immediately because it's very likely the most
	 *     recently sent packets will fill the queue and cause drops.
	 *     (checked here)
	 * (2) We have probed for at least 1*min_rtt_us, and the
	 *     estimated queue is high enough (inflight > 1.25 * estimated_bdp).
	 *     (checked here)
	 * (3) Loss filter says loss rate is "too high".
	 *     (checked in bbr_is_inflight_too_high())
	 * (4) ECN filter says ECN mark rate is "too high".
	 *     (checked in bbr_is_inflight_too_high())
	 */
	case BBR_BW_PROBE_UP:
		if (bbr->prev_probe_too_high &&
		    inflight >= bbr->inflight_hi) {
			bbr->stopped_risky_probe = 1;
			is_risky = true;
			bbr->debug.event = 'D';   /* D for danger */
		} else if (bbr2_has_elapsed_in_phase(sk, bbr->min_rtt_us) &&
			   inflight >=
			   bbr_inflight(sk, bw,
					bbr->params.bw_probe_pif_gain)) {
			is_queuing = true;
			bbr->debug.event = 'Q'; /* building Queue */
		}
		if (is_risky || is_queuing) {
			bbr->prev_probe_too_high = 0;  /* no loss/ECN (yet) */
			bbr2_start_bw_probe_down(sk);  /* restart w/ down */
		}
		break;

	/* After probing in PROBE_UP, we have usually accumulated some data in
	 * the bottleneck buffer (if bw probing didn't find more bw). We next
	 * enter PROBE_DOWN to try to drain any excess data from the queue. To
	 * do this, we use a pacing_gain < 1.0. We hold this pacing gain until
	 * our inflight is less then that target cruising point, which is the
	 * minimum of (a) the amount needed to leave headroom, and (b) the
	 * estimated BDP. Once inflight falls to match the target, we estimate
	 * the queue is drained; persisting would underutilize the pipe.
	 */
	case BBR_BW_PROBE_DOWN:
		if (bbr2_check_time_to_probe_bw(sk))
			return;		/* already decided state transition */
		if (bbr2_check_time_to_cruise(sk, inflight, bw))
			bbr2_start_bw_probe_cruise(sk);
		break;

	default:
		WARN_ONCE(1, "BBR invalid cycle index %u\n", bbr->cycle_idx);
	}
}

/* Exiting PROBE_RTT, so return to bandwidth probing in STARTUP or PROBE_BW. */
static void bbr2_exit_probe_rtt(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr2_reset_lower_bounds(sk);
	if (bbr_full_bw_reached(sk)) {
		bbr->mode = BBR_PROBE_BW;
		/* Raising inflight after PROBE_RTT may cause loss, so reset
		 * the PROBE_BW clock and schedule the next bandwidth probe for
		 * a friendly and randomized future point in time.
		 */
		bbr2_start_bw_probe_down(sk);
		/* Since we are exiting PROBE_RTT, we know inflight is
		 * below our estimated BDP, so it is reasonable to cruise.
		 */
		bbr2_start_bw_probe_cruise(sk);
	} else {
		bbr->mode = BBR_STARTUP;
	}
}

/* Exit STARTUP based on loss rate > 1% and loss gaps in round >= N. Wait until
 * the end of the round in recovery to get a good estimate of how many packets
 * have been lost, and how many we need to drain with a low pacing rate.
 */
static void bbr2_check_loss_too_high_in_startup(struct sock *sk,
					       const struct rate_sample *rs)
{
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr_full_bw_reached(sk))
		return;

	/* For STARTUP exit, check the loss rate at the end of each round trip
	 * of Recovery episodes in STARTUP. We check the loss rate at the end
	 * of the round trip to filter out noisy/low loss and have a better
	 * sense of inflight (extent of loss), so we can drain more accurately.
	 */
	if (rs->losses && bbr->loss_events_in_round < 0xf)
		bbr->loss_events_in_round++;  /* update saturating counter */
	if (bbr->params.full_loss_cnt && bbr->loss_round_start &&
	    inet_csk(sk)->icsk_ca_state == TCP_CA_Recovery &&
	    bbr->loss_events_in_round >= bbr->params.full_loss_cnt &&
	    bbr2_is_inflight_too_high(sk, rs)) {
		bbr->debug.event = 'P';  /* Packet loss caused STARTUP exit */
		bbr2_handle_queue_too_high_in_startup(sk);
		return;
	}
	if (bbr->loss_round_start)
		bbr->loss_events_in_round = 0;
}

/* If we are done draining, advance into steady state operation in PROBE_BW. */
static void bbr2_check_drain(struct sock *sk, const struct rate_sample *rs,
			     struct bbr_context *ctx)
{
	struct bbr *bbr = inet_csk_ca(sk);

	if (bbr_check_drain(sk, rs, ctx)) {
		bbr->mode = BBR_PROBE_BW;
		bbr2_start_bw_probe_down(sk);
	}
}

static void bbr2_update_model(struct sock *sk, const struct rate_sample *rs,
			      struct bbr_context *ctx)
{
	bbr2_update_congestion_signals(sk, rs, ctx);
	bbr_update_ack_aggregation(sk, rs);
	bbr2_check_loss_too_high_in_startup(sk, rs);
	bbr_check_full_bw_reached(sk, rs);
	bbr2_check_drain(sk, rs, ctx);
	bbr2_update_cycle_phase(sk, rs);
	bbr_update_min_rtt(sk, rs);
}

/* Fast path for app-limited case.
 *
 * On each ack, we execute bbr state machine, which primarily consists of:
 * 1) update model based on new rate sample, and
 * 2) update control based on updated model or state change.
 *
 * There are certain workload/scenarios, e.g. app-limited case, where
 * either we can skip updating model or we can skip update of both model
 * as well as control. This provides signifcant softirq cpu savings for
 * processing incoming acks.
 *
 * In case of app-limited, if there is no congestion (loss/ecn) and
 * if observed bw sample is less than current estimated bw, then we can
 * skip some of the computation in bbr state processing:
 *
 * - if there is no rtt/mode/phase change: In this case, since all the
 *   parameters of the network model are constant, we can skip model
 *   as well control update.
 *
 * - else we can skip rest of the model update. But we still need to
 *   update the control to account for the new rtt/mode/phase.
 *
 * Returns whether we can take fast path or not.
 */
static bool bbr2_fast_path(struct sock *sk, bool *update_model,
		const struct rate_sample *rs, struct bbr_context *ctx)
{
	struct bbr *bbr = inet_csk_ca(sk);
	u32 prev_min_rtt_us, prev_mode;

	if (bbr->params.fast_path && bbr->try_fast_path &&
	    rs->is_app_limited && ctx->sample_bw < bbr_max_bw(sk) &&
	    !bbr->loss_in_round && !bbr->ecn_in_round) {
		prev_mode = bbr->mode;
		prev_min_rtt_us = bbr->min_rtt_us;
		bbr2_check_drain(sk, rs, ctx);
		bbr2_update_cycle_phase(sk, rs);
		bbr_update_min_rtt(sk, rs);

		if (bbr->mode == prev_mode &&
		    bbr->min_rtt_us == prev_min_rtt_us &&
		    bbr->try_fast_path)
			return true;

		/* Skip model update, but control still needs to be updated */
		*update_model = false;
	}
	return false;
}

static void bbr2_main(struct sock *sk, const struct rate_sample *rs)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	struct bbr_context ctx = { 0 };
	bool update_model = true;
	u32 bw;
	int ce_ratio = -1;

	bbr->debug.event = '.';  /* init to default NOP (no event yet) */

	bbr_update_round_start(sk, rs, &ctx);
	if (bbr->round_start) {
		bbr->rounds_since_probe =
			min_t(s32, bbr->rounds_since_probe + 1, 0xFF);
		ce_ratio = bbr2_update_ecn_alpha(sk);
		tcp_plb_update_state(sk, &bbr->plb, ce_ratio);
		tcp_plb_check_rehash(sk, &bbr->plb);
	}

	bbr->ecn_in_round  |= rs->is_ece;
	bbr_calculate_bw_sample(sk, rs, &ctx);

	if (bbr2_fast_path(sk, &update_model, rs, &ctx))
		goto out;

	if (update_model)
		bbr2_update_model(sk, rs, &ctx);

	bbr_update_gains(sk);
	bw = bbr_bw(sk);
	bbr_set_pacing_rate(sk, bw, bbr->pacing_gain);
	bbr_set_cwnd(sk, rs, rs->acked_sacked, bw, bbr->cwnd_gain,
		     tp->snd_cwnd, &ctx);
	bbr2_bound_cwnd_for_inflight_model(sk);

out:
	bbr->prev_ca_state = inet_csk(sk)->icsk_ca_state;
	bbr->loss_in_cycle |= rs->lost > 0;
	bbr->ecn_in_cycle  |= rs->delivered_ce > 0;

	bbr_debug(sk, rs->acked_sacked, rs, &ctx);
}

/* Module parameters that are settable by TCP_CONGESTION_PARAMS are declared
 * down here, so that the algorithm functions that use the parameters must use
 * the per-socket parameters; if they accidentally use the global version
 * then there will be a compile error.
 * TODO(ncardwell): move all per-socket parameters down to this section.
 */

/* On losses, scale down inflight and pacing rate by beta scaled by BBR_SCALE.
 * No loss response when 0. Max allwed value is 255.
 */
static u32 bbr_beta = BBR_UNIT * 30 / 100;

/* Gain factor for ECN mark ratio samples, scaled by BBR_SCALE.
 * Max allowed value is 255.
 */
static u32 bbr_ecn_alpha_gain = BBR_UNIT * 1 / 16;  /* 1/16 = 6.25% */

/* The initial value for the ecn_alpha state variable. Default and max
 * BBR_UNIT (256), representing 1.0. This allows a flow to respond quickly
 * to congestion if the bottleneck is congested when the flow starts up.
 */
static u32 bbr_ecn_alpha_init = BBR_UNIT;	/* 1.0, to respond quickly */

/* On ECN, cut inflight_lo to (1 - ecn_factor * ecn_alpha) scaled by BBR_SCALE.
 * No ECN based bounding when 0. Max allwed value is 255.
 */
static u32 bbr_ecn_factor = BBR_UNIT * 1 / 3;	    /* 1/3 = 33% */

/* Estimate bw probing has gone too far if CE ratio exceeds this threshold.
 * Scaled by BBR_SCALE. Disabled when 0. Max allowed is 255.
 */
static u32 bbr_ecn_thresh = BBR_UNIT * 1 / 2;  /* 1/2 = 50% */

/* Max RTT (in usec) at which to use sender-side ECN logic.
 * Disabled when 0 (ECN allowed at any RTT).
 * Max allowed for the parameter is 524287 (0x7ffff) us, ~524 ms.
 */
static u32 bbr_ecn_max_rtt_us = 5000;

/* If non-zero, if in a cycle with no losses but some ECN marks, after ECN
 * clears then use a multiplicative increase to quickly reprobe bw by
 * starting inflight probing at the given multiple of inflight_hi.
 * Default for this experimental knob is 0 (disabled).
 * Planned value for experiments: BBR_UNIT * 1 / 2 = 128, representing 0.5.
 */
static u32 bbr_ecn_reprobe_gain;

/* Estimate bw probing has gone too far if loss rate exceeds this level. */
static u32 bbr_loss_thresh = BBR_UNIT * 2 / 100;  /* 2% loss */

/* Exit STARTUP if number of loss marking events in a Recovery round is >= N,
 * and loss rate is higher than bbr_loss_thresh.
 * Disabled if 0. Max allowed value is 15 (0xF).
 */
static u32 bbr_full_loss_cnt = 8;

/* Exit STARTUP if number of round trips with ECN mark rate above ecn_thresh
 * meets this count. Max allowed value is 3.
 */
static u32 bbr_full_ecn_cnt = 2;

/* Fraction of unutilized headroom to try to leave in path upon high loss. */
static u32 bbr_inflight_headroom = BBR_UNIT * 15 / 100;

/* Multiplier to get target inflight (as multiple of BDP) for PROBE_UP phase.
 * Default is 1.25x, as in BBR v1. Max allowed is 511.
 */
static u32 bbr_bw_probe_pif_gain = BBR_UNIT * 5 / 4;

/* Multiplier to get Reno-style probe epoch duration as: k * BDP round trips.
 * If zero, disables this BBR v2 Reno-style BDP-scaled coexistence mechanism.
 * Max allowed is 511.
 */
static u32 bbr_bw_probe_reno_gain = BBR_UNIT;

/* Max number of packet-timed rounds to wait before probing for bandwidth.  If
 * we want to tolerate 1% random loss per round, and not have this cut our
 * inflight too much, we must probe for bw periodically on roughly this scale.
 * If low, limits Reno/CUBIC coexistence; if high, limits loss tolerance.
 * We aim to be fair with Reno/CUBIC up to a BDP of at least:
 *  BDP = 25Mbps * .030sec /(1514bytes) = 61.9 packets
 */
static u32 bbr_bw_probe_max_rounds = 63;

/* Max amount of randomness to inject in round counting for Reno-coexistence.
 * Max value is 15.
 */
static u32 bbr_bw_probe_rand_rounds = 2;

/* Use BBR-native probe time scale starting at this many usec.
 * We aim to be fair with Reno/CUBIC up to an inter-loss time epoch of at least:
 *  BDP*RTT = 25Mbps * .030sec /(1514bytes) * 0.030sec = 1.9 secs
 */
static u32 bbr_bw_probe_base_us = 2 * USEC_PER_SEC;  /* 2 secs */

/* Use BBR-native probes spread over this many usec: */
static u32 bbr_bw_probe_rand_us = 1 * USEC_PER_SEC;  /* 1 secs */

/* Undo the model changes made in loss recovery if recovery was spurious? */
static bool bbr_undo = true;

/* Use fast path if app-limited, no loss/ECN, and target cwnd was reached? */
static bool bbr_fast_path = true;	/* default: enabled */

/* Use fast ack mode ? */
static int bbr_fast_ack_mode = 1;	/* default: rwnd check off */

/* How much to additively increase inflight_hi when entering REFILL? */
static u32 bbr_refill_add_inc;		/* default: disabled */

module_param_named(beta,                 bbr_beta,                 uint, 0644);
module_param_named(ecn_alpha_gain,       bbr_ecn_alpha_gain,       uint, 0644);
module_param_named(ecn_alpha_init,       bbr_ecn_alpha_init,       uint, 0644);
module_param_named(ecn_factor,           bbr_ecn_factor,           uint, 0644);
module_param_named(ecn_thresh,           bbr_ecn_thresh,           uint, 0644);
module_param_named(ecn_max_rtt_us,       bbr_ecn_max_rtt_us,       uint, 0644);
module_param_named(ecn_reprobe_gain,     bbr_ecn_reprobe_gain,     uint, 0644);
module_param_named(loss_thresh,          bbr_loss_thresh,          uint, 0664);
module_param_named(full_loss_cnt,        bbr_full_loss_cnt,        uint, 0664);
module_param_named(full_ecn_cnt,         bbr_full_ecn_cnt,         uint, 0664);
module_param_named(inflight_headroom,    bbr_inflight_headroom,    uint, 0664);
module_param_named(bw_probe_pif_gain,    bbr_bw_probe_pif_gain,    uint, 0664);
module_param_named(bw_probe_reno_gain,   bbr_bw_probe_reno_gain,   uint, 0664);
module_param_named(bw_probe_max_rounds,  bbr_bw_probe_max_rounds,  uint, 0664);
module_param_named(bw_probe_rand_rounds, bbr_bw_probe_rand_rounds, uint, 0664);
module_param_named(bw_probe_base_us,     bbr_bw_probe_base_us,     uint, 0664);
module_param_named(bw_probe_rand_us,     bbr_bw_probe_rand_us,     uint, 0664);
module_param_named(undo,                 bbr_undo,                 bool, 0664);
module_param_named(fast_path,		 bbr_fast_path,		   bool, 0664);
module_param_named(fast_ack_mode,	 bbr_fast_ack_mode,	   uint, 0664);
module_param_named(refill_add_inc,       bbr_refill_add_inc,       uint, 0664);

static void bbr2_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	const struct net *net = sock_net(sk);

	bbr_init(sk);	/* run shared init code for v1 and v2 */

	/* BBR v2 parameters: */
	bbr->params.beta = min_t(u32, 0xFFU, bbr_beta);
	bbr->params.ecn_alpha_gain = min_t(u32, 0xFFU, bbr_ecn_alpha_gain);
	bbr->params.ecn_alpha_init = min_t(u32, BBR_UNIT, bbr_ecn_alpha_init);
	bbr->params.ecn_factor = min_t(u32, 0xFFU, bbr_ecn_factor);
	bbr->params.ecn_thresh = min_t(u32, 0xFFU, bbr_ecn_thresh);
	bbr->params.ecn_max_rtt_us = min_t(u32, 0x7ffffU, bbr_ecn_max_rtt_us);
	bbr->params.ecn_reprobe_gain = min_t(u32, 0x1FF, bbr_ecn_reprobe_gain);
	bbr->params.loss_thresh = min_t(u32, 0xFFU, bbr_loss_thresh);
	bbr->params.full_loss_cnt = min_t(u32, 0xFU, bbr_full_loss_cnt);
	bbr->params.full_ecn_cnt = min_t(u32, 0x3U, bbr_full_ecn_cnt);
	bbr->params.inflight_headroom =
		min_t(u32, 0xFFU, bbr_inflight_headroom);
	bbr->params.bw_probe_pif_gain =
		min_t(u32, 0x1FFU, bbr_bw_probe_pif_gain);
	bbr->params.bw_probe_reno_gain =
		min_t(u32, 0x1FFU, bbr_bw_probe_reno_gain);
	bbr->params.bw_probe_max_rounds =
		min_t(u32, 0xFFU, bbr_bw_probe_max_rounds);
	bbr->params.bw_probe_rand_rounds =
		min_t(u32, 0xFU, bbr_bw_probe_rand_rounds);
	bbr->params.bw_probe_base_us =
		min_t(u32, (1 << 26) - 1, bbr_bw_probe_base_us);
	bbr->params.bw_probe_rand_us =
		min_t(u32, (1 << 26) - 1, bbr_bw_probe_rand_us);
	bbr->params.undo = bbr_undo;
	bbr->params.fast_path = bbr_fast_path ? 1 : 0;
	bbr->params.refill_add_inc = min_t(u32, 0x3U, bbr_refill_add_inc);

	/* BBR v2 state: */
	bbr->initialized = 1;
	/* Start sampling ECN mark rate after first full flight is ACKed: */
	bbr->loss_round_delivered = tp->delivered + 1;
	bbr->loss_round_start = 0;
	bbr->undo_bw_lo = 0;
	bbr->undo_inflight_lo = 0;
	bbr->undo_inflight_hi = 0;
	bbr->loss_events_in_round = 0;
	bbr->startup_ecn_rounds = 0;
	bbr2_reset_congestion_signals(sk);
	bbr->bw_lo = ~0U;
	bbr->bw_hi[0] = 0;
	bbr->bw_hi[1] = 0;
	bbr->inflight_lo = ~0U;
	bbr->inflight_hi = ~0U;
	bbr->bw_probe_up_cnt = ~0U;
	bbr->bw_probe_up_acks = 0;
	bbr->bw_probe_up_rounds = 0;
	bbr->probe_wait_us = 0;
	bbr->stopped_risky_probe = 0;
	bbr->ack_phase = BBR_ACKS_INIT;
	bbr->rounds_since_probe = 0;
	bbr->bw_probe_samples = 0;
	bbr->prev_probe_too_high = 0;
	bbr->ecn_eligible = 0;
	bbr->ecn_alpha = bbr->params.ecn_alpha_init;
	bbr->alpha_last_delivered = 0;
	bbr->alpha_last_delivered_ce = 0;

	bbr->plb.enabled = 0;
	bbr->plb.consec_cong_rounds = 0;
	bbr->plb.pause_until = 0;
	if ((tp->ecn_flags & TCP_ECN_OK) &&
	    net->ipv4.sysctl_tcp_plb_enabled)
		bbr->plb.enabled = 1;

	tp->fast_ack_mode = min_t(u32, 0x2U, bbr_fast_ack_mode);

	if ((tp->ecn_flags & TCP_ECN_OK) && bbr_ecn_enable)
		tp->ecn_flags |= TCP_ECN_ECT_PERMANENT;
}

/* Core TCP stack informs us that the given skb was just marked lost. */
static void bbr2_skb_marked_lost(struct sock *sk, const struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);
	struct tcp_skb_cb *scb = TCP_SKB_CB(skb);
	struct rate_sample rs;

	/* Capture "current" data over the full round trip of loss,
	 * to have a better chance to see the full capacity of the path.
	*/
	if (!bbr->loss_in_round)  /* first loss in this round trip? */
		bbr->loss_round_delivered = tp->delivered;  /* set round trip */
	bbr->loss_in_round = 1;
	bbr->loss_in_cycle = 1;

	if (!bbr->bw_probe_samples)
		return;  /* not an skb sent while probing for bandwidth */
	if (unlikely(!scb->tx.delivered_mstamp))
		return;  /* skb was SACKed, reneged, marked lost; ignore it */
	/* We are probing for bandwidth. Construct a rate sample that
	 * estimates what happened in the flight leading up to this lost skb,
	 * then see if the loss rate went too high, and if so at which packet.
	 */
	memset(&rs, 0, sizeof(rs));
	rs.tx_in_flight = scb->tx.in_flight;
	rs.lost = tp->lost - scb->tx.lost;
	rs.is_app_limited = scb->tx.is_app_limited;
	if (bbr2_is_inflight_too_high(sk, &rs)) {
		rs.tx_in_flight = bbr2_inflight_hi_from_lost_skb(sk, &rs, skb);
		bbr2_handle_inflight_too_high(sk, &rs);
	}
}

/* Revert short-term model if current loss recovery event was spurious. */
static u32 bbr2_undo_cwnd(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	bbr->debug.undo = 1;
	bbr->full_bw = 0;   /* spurious slow-down; reset full pipe detection */
	bbr->full_bw_cnt = 0;
	bbr->loss_in_round = 0;

	if (!bbr->params.undo)
		return tp->snd_cwnd;

	/* Revert to cwnd and other state saved before loss episode. */
	bbr->bw_lo = max(bbr->bw_lo, bbr->undo_bw_lo);
	bbr->inflight_lo = max(bbr->inflight_lo, bbr->undo_inflight_lo);
	bbr->inflight_hi = max(bbr->inflight_hi, bbr->undo_inflight_hi);
	return bbr->prior_cwnd;
}

/* Entering loss recovery, so save state for when we undo recovery. */
static u32 bbr2_ssthresh(struct sock *sk)
{
	struct bbr *bbr = inet_csk_ca(sk);

	bbr_save_cwnd(sk);
	/* For undo, save state that adapts based on loss signal. */
	bbr->undo_bw_lo		= bbr->bw_lo;
	bbr->undo_inflight_lo	= bbr->inflight_lo;
	bbr->undo_inflight_hi	= bbr->inflight_hi;
	return tcp_sk(sk)->snd_ssthresh;
}

static enum tcp_bbr2_phase bbr2_get_phase(struct bbr *bbr)
{
	switch (bbr->mode) {
	case BBR_STARTUP:
		return BBR2_PHASE_STARTUP;
	case BBR_DRAIN:
		return BBR2_PHASE_DRAIN;
	case BBR_PROBE_BW:
		break;
	case BBR_PROBE_RTT:
		return BBR2_PHASE_PROBE_RTT;
	default:
		return BBR2_PHASE_INVALID;
	}
	switch (bbr->cycle_idx) {
	case BBR_BW_PROBE_UP:
		return BBR2_PHASE_PROBE_BW_UP;
	case BBR_BW_PROBE_DOWN:
		return BBR2_PHASE_PROBE_BW_DOWN;
	case BBR_BW_PROBE_CRUISE:
		return BBR2_PHASE_PROBE_BW_CRUISE;
	case BBR_BW_PROBE_REFILL:
		return BBR2_PHASE_PROBE_BW_REFILL;
	default:
		return BBR2_PHASE_INVALID;
	}
}

static size_t bbr2_get_info(struct sock *sk, u32 ext, int *attr,
			    union tcp_cc_info *info)
{
	if (ext & (1 << (INET_DIAG_BBRINFO - 1)) ||
	    ext & (1 << (INET_DIAG_VEGASINFO - 1))) {
		struct bbr *bbr = inet_csk_ca(sk);
		u64 bw = bbr_bw_bytes_per_sec(sk, bbr_bw(sk));
		u64 bw_hi = bbr_bw_bytes_per_sec(sk, bbr_max_bw(sk));
		u64 bw_lo = bbr->bw_lo == ~0U ?
			~0ULL : bbr_bw_bytes_per_sec(sk, bbr->bw_lo);

		memset(&info->bbr2, 0, sizeof(info->bbr2));
		info->bbr2.bbr_bw_lsb		= (u32)bw;
		info->bbr2.bbr_bw_msb		= (u32)(bw >> 32);
		info->bbr2.bbr_min_rtt		= bbr->min_rtt_us;
		info->bbr2.bbr_pacing_gain	= bbr->pacing_gain;
		info->bbr2.bbr_cwnd_gain	= bbr->cwnd_gain;
		info->bbr2.bbr_bw_hi_lsb	= (u32)bw_hi;
		info->bbr2.bbr_bw_hi_msb	= (u32)(bw_hi >> 32);
		info->bbr2.bbr_bw_lo_lsb	= (u32)bw_lo;
		info->bbr2.bbr_bw_lo_msb	= (u32)(bw_lo >> 32);
		info->bbr2.bbr_mode		= bbr->mode;
		info->bbr2.bbr_phase		= (__u8)bbr2_get_phase(bbr);
		info->bbr2.bbr_version		= (__u8)2;
		info->bbr2.bbr_inflight_lo	= bbr->inflight_lo;
		info->bbr2.bbr_inflight_hi	= bbr->inflight_hi;
		info->bbr2.bbr_extra_acked	= bbr_extra_acked(sk);
		*attr = INET_DIAG_BBRINFO;
		return sizeof(info->bbr2);
	}
	return 0;
}

static void bbr2_set_state(struct sock *sk, u8 new_state)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bbr *bbr = inet_csk_ca(sk);

	if (new_state == TCP_CA_Loss) {
		struct rate_sample rs = { .losses = 1 };
		struct bbr_context ctx = { 0 };

		tcp_plb_update_state_upon_rto(sk, &bbr->plb);
		bbr->prev_ca_state = TCP_CA_Loss;
		bbr->full_bw = 0;
		if (!bbr2_is_probing_bandwidth(sk) && bbr->inflight_lo == ~0U) {
			/* bbr_adapt_lower_bounds() needs cwnd before
			 * we suffered an RTO, to update inflight_lo:
			 */
			bbr->inflight_lo =
				max(tp->snd_cwnd, bbr->prior_cwnd);
		}
		bbr_debug(sk, 0, &rs, &ctx);
	} else if (bbr->prev_ca_state == TCP_CA_Loss &&
		   new_state != TCP_CA_Loss) {
		tp->snd_cwnd = max(tp->snd_cwnd, bbr->prior_cwnd);
		bbr->try_fast_path = 0; /* bound cwnd using latest model */
	}
}

static struct tcp_congestion_ops tcp_bbr2_cong_ops __read_mostly = {
	.flags		= TCP_CONG_NON_RESTRICTED | TCP_CONG_WANTS_CE_EVENTS,
	.name		= "bbr2",
	.owner		= THIS_MODULE,
	.init		= bbr2_init,
	.cong_control	= bbr2_main,
	.sndbuf_expand	= bbr_sndbuf_expand,
	.skb_marked_lost = bbr2_skb_marked_lost,
	.undo_cwnd	= bbr2_undo_cwnd,
	.cwnd_event	= bbr_cwnd_event,
	.ssthresh	= bbr2_ssthresh,
	.tso_segs	= bbr_tso_segs,
	.get_info	= bbr2_get_info,
	.set_state	= bbr2_set_state,
};

static int __init bbr_register(void)
{
	BUILD_BUG_ON(sizeof(struct bbr) > ICSK_CA_PRIV_SIZE);
	return tcp_register_congestion_control(&tcp_bbr2_cong_ops);
}

static void __exit bbr_unregister(void)
{
	tcp_unregister_congestion_control(&tcp_bbr2_cong_ops);
}

module_init(bbr_register);
module_exit(bbr_unregister);

MODULE_AUTHOR("Van Jacobson <vanj@google.com>");
MODULE_AUTHOR("Neal Cardwell <ncardwell@google.com>");
MODULE_AUTHOR("Yuchung Cheng <ycheng@google.com>");
MODULE_AUTHOR("Soheil Hassas Yeganeh <soheil@google.com>");
MODULE_AUTHOR("Priyaranjan Jha <priyarjha@google.com>");
MODULE_AUTHOR("Yousuk Seung <ysseung@google.com>");
MODULE_AUTHOR("Kevin Yang <yyd@google.com>");
MODULE_AUTHOR("Arjun Roy <arjunroy@google.com>");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TCP BBR (Bottleneck Bandwidth and RTT)");
