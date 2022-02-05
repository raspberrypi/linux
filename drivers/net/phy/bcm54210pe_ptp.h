// SPDX-License-Identifier: GPL-2.0+
/*
 *  drivers/net/phy/bcm54210pe_ptp.h
 *
* IEEE1588 (PTP), perout and extts for BCM54210PE PHY
 *
 * Authors: Carlos Fernandez, Kyle Judd, Lasse Johnsen
 * License: GPL
 */

#include <linux/ptp_clock_kernel.h>
#include <linux/list.h>

#define CIRCULAR_BUFFER_COUNT 8
#define CIRCULAR_BUFFER_ITEM_COUNT 32

#define SYNC_IN_PIN 0
#define SYNC_OUT_PIN 1

#define SYNC_OUT_MODE_1 1
#define SYNC_OUT_MODE_2 2

#define DIRECTION_RX 0
#define DIRECTION_TX 1

struct bcm54210pe_ptp {
	struct ptp_clock_info caps;
	struct ptp_clock *ptp_clock;
	struct bcm54210pe_private *chosen;
};

struct bcm54210pe_circular_buffer_item {
	struct list_head list;

	u8 msg_type;
	u16 sequence_id;
	u64 time_stamp;
	bool is_valid;
};

struct bcm54210pe_private {
	struct phy_device *phydev;
	struct bcm54210pe_ptp *ptp;
	struct mii_timestamper mii_ts;
	struct ptp_pin_desc sdp_config[2];

	int ts_tx_config;
	int tx_rx_filter;

	bool one_step;
	bool perout_en;
	bool extts_en;

	int second_on_set;

	int perout_mode;
	int perout_period;
	int perout_pulsewidth;

	u64 last_extts_ts;
	u64 last_immediate_ts[2];

	struct sk_buff_head tx_skb_queue;
	struct sk_buff_head rx_skb_queue;

	struct bcm54210pe_circular_buffer_item
		circular_buffer_items[CIRCULAR_BUFFER_COUNT]
				     [CIRCULAR_BUFFER_ITEM_COUNT];
	struct list_head circular_buffers[CIRCULAR_BUFFER_COUNT];

	struct work_struct txts_work, rxts_work;
	struct delayed_work perout_ws, extts_ws;
	struct mutex clock_lock, timestamp_buffer_lock;

	int fib_sequence[10];

	int fib_factor_rx;
	int fib_factor_tx;

	int hwts_tx_en;
	int hwts_rx_en;
	int layer;
	int version;
};

static bool bcm54210pe_rxtstamp(struct mii_timestamper *mii_ts, struct sk_buff *skb, int type);
static void bcm54210pe_txtstamp(struct mii_timestamper *mii_ts, struct sk_buff *skb, int type);
static void bcm54210pe_run_rx_timestamp_match_thread(struct work_struct *w);
static void bcm54210pe_run_tx_timestamp_match_thread(struct work_struct *w);
static void bcm54210pe_read_sop_time_register(struct bcm54210pe_private *private);
static bool bcm54210pe_fetch_timestamp(u8 txrx, u8 message_type, u16 seq_id, struct bcm54210pe_private *private, u64 *timestamp);

static u16  bcm54210pe_get_base_nco6_reg(struct bcm54210pe_private *private, u16 val, bool do_nse_init);
static int  bcm54210pe_interrupts_enable(struct phy_device *phydev, bool fsync_en, bool sop_en);
static int  bcm54210pe_gettimex(struct ptp_clock_info *info, struct timespec64 *ts, struct ptp_system_timestamp *sts);
static int  bcm54210pe_get80bittime(struct bcm54210pe_private *private, struct timespec64 *ts, struct ptp_system_timestamp *sts);
static int  bcm54210pe_get48bittime(struct bcm54210pe_private *private, u64 *time_stamp);
static void bcm54210pe_read80bittime_register(struct phy_device *phydev, u64 *time_stamp_80, u64 *time_stamp_48);
static void bcm54210pe_read48bittime_register(struct phy_device *phydev, u64 *time_stamp);

static int  bcm54210pe_perout_enable(struct bcm54210pe_private *private, s64 period, s64 pulsewidth, int on);
static void bcm54210pe_run_perout_mode_one_thread(struct work_struct *perout_ws);

static int  bcm54210pe_extts_enable(struct bcm54210pe_private *private, int enable);
static void bcm54210pe_run_extts_thread(struct work_struct *extts_ws);
static void bcm54210pe_trigger_extts_event(struct bcm54210pe_private *private, u64 timestamp);

static u64  convert_48bit_to_80bit(u64 second_on_set, u64 ts);
static u64  four_u16_to_ns(u16 *four_u16);
static u64  ts_to_ns(struct timespec64 *ts);
static void ns_to_ts(u64 time_stamp, struct timespec64 *ts);
