/* SPDX-License-Identifier: GPL-2.0+
 *
 * IEEE1588 (PTP), perout and extts for BCM54210PE PHY
 *
 * Authors: Carlos Fernandez, Kyle Judd, Lasse Johnsen
 * License: GPL
 */

#include <linux/list.h>
#include <linux/ptp_clock_kernel.h>

#define CIRCULAR_BUFFER_COUNT		8
#define CIRCULAR_BUFFER_ITEM_COUNT	32

#define SYNC_IN_PIN			0
#define SYNC_OUT_PIN			1

#define SYNC_OUT_MODE_1			1
#define SYNC_OUT_MODE_2			2

#define DIRECTION_RX			0
#define DIRECTION_TX			1

#define INTC_FSYNC			1
#define INTC_SOP			2

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
