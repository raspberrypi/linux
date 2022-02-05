// SPDX-License-Identifier: GPL-2.0+
/*
 *
 * IEEE1588 (PTP), perout and extts for BCM54210PE PHY
 *
 * Authors: Carlos Fernandez, Kyle Judd, Lasse Johnsen
 * License: GPL
 */

#include <linux/brcmphy.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/irq.h>
#include <linux/mii.h>
#include <linux/net_tstamp.h>
#include <linux/phy.h>
#include <linux/ptp_classify.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/sched.h>
#include <linux/udp.h>
#include <linux/workqueue.h>

#include <asm/unaligned.h>

#include "bcm54210pe_ptp.h"
#include "bcm-phy-lib.h"

MODULE_DESCRIPTION("Broadcom BCM54210PE PHY driver");
MODULE_AUTHOR("Lasse L. Johnsen");
MODULE_LICENSE("GPL");

#define PTP_CONTROL_OFFSET		32
#define PTP_TSMT_OFFSET			0
#define PTP_SEQUENCE_ID_OFFSET	30
#define PTP_CLOCK_ID_OFFSET		20
#define PTP_CLOCK_ID_SIZE		8
#define PTP_SEQUENCE_PORT_NUMER_OFFSET  (PTP_CLOCK_ID_OFFSET + PTP_CLOCK_ID_SIZE)

#define EXT_ENABLE_REG1			0x17
#define EXT_ENABLE_DATA1		0x0F7E
#define EXT_ENABLE_REG2			0x15
#define EXT_ENABLE_DATA2		0x0000

#define EXT_1588_SLICE_REG		0x0810
#define EXT_1588_SLICE_DATA		0x0101

#define ORIGINAL_TIME_CODE_0	0x0854
#define ORIGINAL_TIME_CODE_1	0x0855
#define ORIGINAL_TIME_CODE_2	0x0856
#define ORIGINAL_TIME_CODE_3	0x0857
#define ORIGINAL_TIME_CODE_4	0x0858

#define TIME_STAMP_REG_0		0x0889
#define TIME_STAMP_REG_1		0x088A
#define TIME_STAMP_REG_2		0x088B
#define TIME_STAMP_REG_3		0x08C4
#define TIME_STAMP_INFO_1		0x088C
#define TIME_STAMP_INFO_2		0x088D
#define INTERRUPT_STATUS_REG	0x085F
#define INTERRUPT_MASK_REG		0x085E
#define EXT_SOFTWARE_RESET		0x0F70
#define EXT_RESET1				0x0001 //RESET
#define EXT_RESET2				0x0000 //NORMAL OPERATION
#define GLOBAL_TIMESYNC_REG		0x0FF5

#define TX_EVENT_MODE_REG		0x0811
#define RX_EVENT_MODE_REG		0x0819
#define TX_TSCAPTURE_ENABLE_REG	0x0821
#define RX_TSCAPTURE_ENABLE_REG	0x0822
#define TXRX_1588_OPTION_REG	0x0823

#define TX_TS_OFFSET_LSB		0x0834
#define TX_TS_OFFSET_MSB		0x0835
#define RX_TS_OFFSET_LSB		0x0844
#define RX_TS_OFFSET_MSB		0x0845
#define NSE_DPPL_NCO_1_LSB_REG	0x0873
#define NSE_DPPL_NCO_1_MSB_REG	0x0874

#define NSE_DPPL_NCO_2_0_REG	0x0875
#define NSE_DPPL_NCO_2_1_REG	0x0876
#define NSE_DPPL_NCO_2_2_REG	0x0877

#define NSE_DPPL_NCO_3_0_REG	0x0878
#define NSE_DPPL_NCO_3_1_REG	0x0879
#define NSE_DPPL_NCO_3_2_REG	0x087A

#define NSE_DPPL_NCO_4_REG		0x087B

#define NSE_DPPL_NCO_5_0_REG	0x087C
#define NSE_DPPL_NCO_5_1_REG	0x087D
#define NSE_DPPL_NCO_5_2_REG	0x087E

#define NSE_DPPL_NCO_6_REG		0x087F

#define NSE_DPPL_NCO_7_0_REG	0x0880
#define NSE_DPPL_NCO_7_1_REG	0x0881

#define DPLL_SELECT_REG			0x085b
#define TIMECODE_SEL_REG		0x08C3
#define SHADOW_REG_CONTROL		0x085C
#define SHADOW_REG_LOAD			0x085D

#define PTP_INTERRUPT_REG		0x0D0C

#define CTR_DBG_REG				0x088E
#define HEART_BEAT_REG4			0x08ED
#define HEART_BEAT_REG3			0x08EC
#define HEART_BEAT_REG2			0x0888
#define	HEART_BEAT_REG1			0x0887
#define	HEART_BEAT_REG0			0x0886

#define READ_END_REG			0x0885

static u64 convert_48bit_to_80bit(u64 second_on_set, u64 ts)
{
	return (second_on_set * 1000000000) + ts;
}

static u64 four_u16_to_ns(u16 *four_u16)
{
	u32 seconds;
	u32 nanoseconds;
	struct timespec64 ts;
	u16 *ptr;

	nanoseconds = 0;
	seconds = 0;

	ptr = (u16 *)&nanoseconds;
	*ptr = four_u16[0]; ptr++; *ptr = four_u16[1];

	ptr = (u16 *)&seconds;
	*ptr = four_u16[2]; ptr++; *ptr = four_u16[3];

	ts.tv_sec = seconds;
	ts.tv_nsec = nanoseconds;

	return timespec64_to_ns(&ts);
}

static int bcm54210pe_interrupts_enable(struct phy_device *phydev, bool fsync_en, bool sop_en)
{
	u16 interrupt_mask;

	interrupt_mask = 0;

	if (fsync_en)
		interrupt_mask |= 0x0001;

	if (sop_en)
		interrupt_mask |= 0x0002;

	return bcm_phy_write_exp(phydev, INTERRUPT_MASK_REG, interrupt_mask);
}

static bool bcm54210pe_fetch_timestamp(u8 txrx, u8 message_type, u16 seq_id,
				       struct bcm54210pe_private *private, u64 *timestamp)
{
	struct bcm54210pe_circular_buffer_item *item;
	struct list_head *this, *next;

	u8 index = (txrx * 4) + message_type;

	if (index >= CIRCULAR_BUFFER_COUNT)
		return false;

	list_for_each_safe(this, next, &private->circular_buffers[index]) {
		item = list_entry(this, struct bcm54210pe_circular_buffer_item, list);

		if (item->sequence_id == seq_id && item->is_valid) {
			item->is_valid = false;
			*timestamp = item->time_stamp;
			mutex_unlock(&private->timestamp_buffer_lock);
			return true;
		}
	}

	return false;
}

static u16 bcm54210pe_get_base_nco6_reg(struct bcm54210pe_private *private,
					u16 val, bool do_nse_init)
{
	// Set Global mode to CPU system
	val |= 0xC000;

	// NSE init
	if (do_nse_init)
		val |= 0x1000;

	if (private->extts_en)
		val |= 0x2004;

	if (private->perout_en) {
		if (private->perout_mode == SYNC_OUT_MODE_1)
			val |= 0x0001;
		else if (private->perout_mode == SYNC_OUT_MODE_2)
			val |= 0x0002;
	}

	return val;
}

static void bcm54210pe_read_sop_time_register(struct bcm54210pe_private *private)
{
	struct phy_device *phydev = private->phydev;
	struct bcm54210pe_circular_buffer_item *item;
	u16 fifo_info_1, fifo_info_2;
	u8 tx_or_rx, msg_type, index;
	u16 sequence_id;
	u64 timestamp;
	u16 time[4];
	int deadlock_check;

	deadlock_check = 0;

	mutex_lock(&private->timestamp_buffer_lock);

	while (bcm_phy_read_exp(phydev, INTERRUPT_STATUS_REG) & INTC_SOP) {
		mutex_lock(&private->clock_lock);

		// Flush out the FIFO
		bcm_phy_write_exp(phydev, READ_END_REG, 1);

		time[3] = bcm_phy_read_exp(phydev, TIME_STAMP_REG_3);
		time[2] = bcm_phy_read_exp(phydev, TIME_STAMP_REG_2);
		time[1] = bcm_phy_read_exp(phydev, TIME_STAMP_REG_1);
		time[0] = bcm_phy_read_exp(phydev, TIME_STAMP_REG_0);

		fifo_info_1 = bcm_phy_read_exp(phydev, TIME_STAMP_INFO_1);
		fifo_info_2 = bcm_phy_read_exp(phydev, TIME_STAMP_INFO_2);

		bcm_phy_write_exp(phydev, READ_END_REG, 2);
		bcm_phy_write_exp(phydev, READ_END_REG, 0);

		mutex_unlock(&private->clock_lock);

		msg_type = (u8)((fifo_info_2 & 0xF000) >> 12);
		tx_or_rx = (u8)((fifo_info_2 & 0x0800) >> 11); // 1 = TX, 0 = RX
		sequence_id = fifo_info_1;

		timestamp = four_u16_to_ns(time);

		index = (tx_or_rx * 4) + msg_type;

		if (index < CIRCULAR_BUFFER_COUNT)
			item = list_first_entry_or_null(&private->circular_buffers[index],
							struct bcm54210pe_circular_buffer_item,
							list);

		if (!item)
			continue;

		list_del_init(&item->list);

		item->msg_type = msg_type;
		item->sequence_id = sequence_id;
		item->time_stamp = timestamp;
		item->is_valid = true;

		list_add_tail(&item->list, &private->circular_buffers[index]);

		deadlock_check++;
		if (deadlock_check > 100)
			break;
	}

	mutex_unlock(&private->timestamp_buffer_lock);
}

static void bcm54210pe_run_rx_timestamp_match_thread(struct work_struct *w)
{
	struct bcm54210pe_private *private =
		container_of(w, struct bcm54210pe_private, rxts_work);

	struct skb_shared_hwtstamps *shhwtstamps;
	struct ptp_header *hdr;
	struct sk_buff *skb;

	u8 msg_type;
	u16 sequence_id;
	u64 timestamp;
	int x, type;

	skb = skb_dequeue(&private->rx_skb_queue);

	while (skb) {
		// Yes....  skb_defer_rx_timestamp just did this but <ZZZzzz>....
		skb_push(skb, ETH_HLEN);
		type = ptp_classify_raw(skb);
		skb_pull(skb, ETH_HLEN);

		hdr = ptp_parse_header(skb, type);

		if (!hdr)
			goto dequeue;

		msg_type = ptp_get_msgtype(hdr, type);
		sequence_id = be16_to_cpu(hdr->sequence_id);

		timestamp = 0;

		for (x = 0; x < 10; x++) {
			bcm54210pe_read_sop_time_register(private);
			if (bcm54210pe_fetch_timestamp(0, msg_type, sequence_id,
						       private, &timestamp)) {
				break;
			}

			udelay(private->fib_sequence[x] *
			       private->fib_factor_rx);
		}

		shhwtstamps = skb_hwtstamps(skb);

		if (!shhwtstamps)
			goto dequeue;

		memset(shhwtstamps, 0, sizeof(*shhwtstamps));
		shhwtstamps->hwtstamp = ns_to_ktime(timestamp);

dequeue:
		netif_rx_ni(skb);
		skb = skb_dequeue(&private->rx_skb_queue);
	}
}

static void bcm54210pe_run_tx_timestamp_match_thread(struct work_struct *w)
{
	struct bcm54210pe_private *private =
		container_of(w, struct bcm54210pe_private, txts_work);

	struct skb_shared_hwtstamps *shhwtstamps;
	struct sk_buff *skb;

	struct ptp_header *hdr;
	u8 msg_type;
	u16 sequence_id;
	u64 timestamp;
	int x, type;

	timestamp = 0;
	skb = skb_dequeue(&private->tx_skb_queue);

	while (skb) {
		type = ptp_classify_raw(skb);
		hdr = ptp_parse_header(skb, type);

		if (!hdr)
			goto dequeue;

		msg_type = ptp_get_msgtype(hdr, type);
		sequence_id = be16_to_cpu(hdr->sequence_id);

		for (x = 0; x < 10; x++) {
			bcm54210pe_read_sop_time_register(private);
			if (bcm54210pe_fetch_timestamp(1, msg_type, sequence_id,
						       private, &timestamp)) {
				break;
			}
			udelay(private->fib_sequence[x] * private->fib_factor_tx);
		}
		shhwtstamps = skb_hwtstamps(skb);

		if (!shhwtstamps) {
			kfree_skb(skb);
			goto dequeue;
		}

		memset(shhwtstamps, 0, sizeof(*shhwtstamps));
		shhwtstamps->hwtstamp = ns_to_ktime(timestamp);

		skb_complete_tx_timestamp(skb, shhwtstamps);

dequeue:
		skb = skb_dequeue(&private->tx_skb_queue);
	}
}

static int bcm54210pe_config_1588(struct phy_device *phydev)
{
	int err;

	err = bcm_phy_write_exp(phydev, PTP_INTERRUPT_REG, 0x3c02);

	//Enable global timesync register
	err |=  bcm_phy_write_exp(phydev, GLOBAL_TIMESYNC_REG, 0x0001);

	//ENABLE TX and RX slice 1588
	err |=  bcm_phy_write_exp(phydev, EXT_1588_SLICE_REG, 0x0101);

	//Add 80bit timestamp + NO CPU MODE in TX
	err |=  bcm_phy_write_exp(phydev, TX_EVENT_MODE_REG, 0xFF00);

	//Add 32+32 bits timestamp + NO CPU mode in RX
	err |=  bcm_phy_write_exp(phydev, RX_EVENT_MODE_REG, 0xFF00);

	//Select 80 bit counter
	err |=  bcm_phy_write_exp(phydev, TIMECODE_SEL_REG, 0x0101);

	//Enable timestamp capture in TX
	err |=  bcm_phy_write_exp(phydev, TX_TSCAPTURE_ENABLE_REG, 0x0001);

	//Enable timestamp capture in RX
	err |=  bcm_phy_write_exp(phydev, RX_TSCAPTURE_ENABLE_REG, 0x0001);

	//Enable shadow register
	err |= bcm_phy_write_exp(phydev, SHADOW_REG_CONTROL, 0x0000);
	err |= bcm_phy_write_exp(phydev, SHADOW_REG_LOAD, 0x07c0);

	// Set global mode and trigger immediate framesync to load shaddow registers
	err |=  bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG, 0xC020);

	// Enable Interrupt behaviour (eventhough we get no interrupts)
	err |= bcm54210pe_interrupts_enable(phydev, true, false);

	return err;
}

// Must be called under clock_lock
static void bcm54210pe_trigger_extts_event(struct bcm54210pe_private *private, u64 timestamp)
{
	struct ptp_clock_event event;
	struct timespec64 ts;

	event.type = PTP_CLOCK_EXTTS;
	event.timestamp = convert_48bit_to_80bit(private->second_on_set, timestamp);
	event.index = 0;

	ptp_clock_event(private->ptp->ptp_clock, &event);

	private->last_extts_ts = timestamp;

	ts = ns_to_timespec64(timestamp);
}

// Must be called under clock_lock
static void bcm54210pe_read80bittime_register(struct phy_device *phydev,
					      u64 *time_stamp_80, u64 *time_stamp_48)
{
	u16 time[5];
	u64 ts[3];
	u64 cumulative;

	bcm_phy_write_exp(phydev, CTR_DBG_REG, 0x400);
	time[4] = bcm_phy_read_exp(phydev, HEART_BEAT_REG4);
	time[3] = bcm_phy_read_exp(phydev, HEART_BEAT_REG3);
	time[2] = bcm_phy_read_exp(phydev, HEART_BEAT_REG2);
	time[1] = bcm_phy_read_exp(phydev, HEART_BEAT_REG1);
	time[0] = bcm_phy_read_exp(phydev, HEART_BEAT_REG0);

	// Set read end bit
	bcm_phy_write_exp(phydev, CTR_DBG_REG, 0x800);
	bcm_phy_write_exp(phydev, CTR_DBG_REG, 0x000);

	*time_stamp_80 = four_u16_to_ns(time);

	if (time_stamp_48) {
		ts[2] = (((u64)time[2]) << 32);
		ts[1] = (((u64)time[1]) << 16);
		ts[0] = ((u64)time[0]);

		cumulative = 0;
		cumulative |= ts[0];
		cumulative |= ts[1];
		cumulative |= ts[2];

		*time_stamp_48 = cumulative;
	}
}

// Must be called under clock_lock
static void bcm54210pe_read48bittime_register(struct phy_device *phydev, u64 *time_stamp)
{
	u16 time[3];
	u64 ts[3];
	u64 cumulative;

	bcm_phy_write_exp(phydev, CTR_DBG_REG, 0x400);
	time[2] = bcm_phy_read_exp(phydev, HEART_BEAT_REG2);
	time[1] = bcm_phy_read_exp(phydev, HEART_BEAT_REG1);
	time[0] = bcm_phy_read_exp(phydev, HEART_BEAT_REG0);

	// Set read end bit
	bcm_phy_write_exp(phydev, CTR_DBG_REG, 0x800);
	bcm_phy_write_exp(phydev, CTR_DBG_REG, 0x000);

	ts[2] = (((u64)time[2]) << 32);
	ts[1] = (((u64)time[1]) << 16);
	ts[0] = ((u64)time[0]);

	cumulative = 0;
	cumulative |= ts[0];
	cumulative |= ts[1];
	cumulative |= ts[2];

	*time_stamp = cumulative;
}

static int bcm54210pe_get80bittime(struct bcm54210pe_private *private,
				   struct timespec64 *ts,
				   struct ptp_system_timestamp *sts)
{
	struct phy_device *phydev;
	u16 nco_6_register_value;
	int i;
	u64 time_stamp_48, time_stamp_80, control_ts;

	phydev = private->phydev;

	// Capture timestamp on next framesync
	nco_6_register_value = 0x2000;

	// Lock
	mutex_lock(&private->clock_lock);

	// We share frame sync events with extts, so we need to ensure no event
	// has occurred as we are about to boot the registers, so....

	// If extts is enabled
	if (private->extts_en) {
		// Halt framesyncs generated by the sync in pin
		bcm_phy_modify_exp(phydev, NSE_DPPL_NCO_6_REG, 0x003C, 0x0000);

		// Read what's in the 8- bit register
		bcm54210pe_read48bittime_register(phydev, &control_ts);

		// If it matches neither the last gettime or extts timestamp
		if (control_ts != private->last_extts_ts &&
		    control_ts != private->last_immediate_ts[0]) {
			// Odds are this is a extts not yet logged as an event
			//printk("extts triggered by get80bittime\n");
			bcm54210pe_trigger_extts_event(private, control_ts);
		}
	}

	// Heartbeat register selection. Latch 80 bit Original time counter
	// into Heartbeat register (this is undocumented)
	bcm_phy_write_exp(phydev, DPLL_SELECT_REG, 0x0040);

	// Amend to base register
	nco_6_register_value = bcm54210pe_get_base_nco6_reg(private, nco_6_register_value, false);

	// Set the NCO register
	bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG, nco_6_register_value);

	// Trigger framesync
	if (sts) {
		// If we are doing a gettimex call
		ptp_read_system_prets(sts);
		bcm_phy_modify_exp(phydev, NSE_DPPL_NCO_6_REG, 0x003C, 0x0020);
		ptp_read_system_postts(sts);

	} else {
		// or if we are doing a gettime call
		bcm_phy_modify_exp(phydev, NSE_DPPL_NCO_6_REG, 0x003C, 0x0020);
	}

	for (i = 0; i < 5; i++) {
		bcm54210pe_read80bittime_register(phydev, &time_stamp_80, &time_stamp_48);

		if (time_stamp_80 != 0)
			break;
	}

	// Convert to timespec64
	*ts = ns_to_timespec64(time_stamp_80);

	// If we are using extts
	if (private->extts_en) {
		// Commit last timestamp
		private->last_immediate_ts[0] = time_stamp_48;
		private->last_immediate_ts[1] = time_stamp_80;

		// Heartbeat register selection. Latch 48 bit Original time counter
		// into Heartbeat register (this is undocumented)
		bcm_phy_write_exp(phydev, DPLL_SELECT_REG, 0x0000);

		// Rearm framesync for sync in pin
		nco_6_register_value = bcm54210pe_get_base_nco6_reg(private,
								    nco_6_register_value, false);
		bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG, nco_6_register_value);
	}

	mutex_unlock(&private->clock_lock);

	return 0;
}

static int bcm54210pe_gettimex(struct ptp_clock_info *info,
			       struct timespec64 *ts,
			       struct ptp_system_timestamp *sts)
{
	struct bcm54210pe_ptp *ptp;

	ptp = container_of(info, struct bcm54210pe_ptp, caps);

	return bcm54210pe_get80bittime(ptp->chosen, ts, sts);
}

static int bcm54210pe_gettime(struct ptp_clock_info *info, struct timespec64 *ts)
{
	return bcm54210pe_gettimex(info, ts, NULL);
}

static int bcm54210pe_get48bittime(struct bcm54210pe_private *private, u64 *timestamp)
{
	u16 nco_6_register_value;
	int i, err;

	struct phy_device *phydev = private->phydev;

	// Capture timestamp on next framesync
	nco_6_register_value = 0x2000;

	mutex_lock(&private->clock_lock);

	// Heartbeat register selection. Latch 48 bit Original time counter
	// into Heartbeat register (this is undocumented)
	err = bcm_phy_write_exp(phydev, DPLL_SELECT_REG, 0x0000);

	// Amend to base register
	nco_6_register_value =
		bcm54210pe_get_base_nco6_reg(private, nco_6_register_value, false);

	// Set the NCO register
	err |= bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG, nco_6_register_value);

	// Trigger framesync
	err |= bcm_phy_modify_exp(phydev, NSE_DPPL_NCO_6_REG, 0x003C, 0x0020);

	for (i = 0; i < 5; i++) {
		bcm54210pe_read48bittime_register(phydev, timestamp);

		if (*timestamp != 0)
			break;
	}

	mutex_unlock(&private->clock_lock);

	return err;
}

static int bcm54210pe_settime(struct ptp_clock_info *info, const struct timespec64 *ts)
{
	u16 shadow_load_register, nco_6_register_value;
	u16 original_time_codes[5], local_time_codes[3];
	struct bcm54210pe_ptp *ptp;
	struct phy_device *phydev;

	ptp = container_of(info, struct bcm54210pe_ptp, caps);
	phydev = ptp->chosen->phydev;

	shadow_load_register = 0;
	nco_6_register_value = 0;

	// Assign original time codes (80 bit)
	original_time_codes[4] = (u16)((ts->tv_sec & 0x0000FFFF00000000) >> 32);
	original_time_codes[3] = (u16)((ts->tv_sec  & 0x00000000FFFF0000) >> 16);
	original_time_codes[2] = (u16)(ts->tv_sec  & 0x000000000000FFFF);
	original_time_codes[1] = (u16)((ts->tv_nsec & 0x00000000FFFF0000) >> 16);
	original_time_codes[0] = (u16)(ts->tv_nsec & 0x000000000000FFFF);

	// Assign original time codes (48 bit)
	local_time_codes[2] = 0x4000;
	local_time_codes[1] = (u16)(ts->tv_nsec >> 20);
	local_time_codes[0] = (u16)(ts->tv_nsec >> 4);

	// Set Time Code load bit in the shadow load register
	shadow_load_register |= 0x0400;

	// Set Local Time load bit in the shadow load register
	shadow_load_register |= 0x0080;

	mutex_lock(&ptp->chosen->clock_lock);

	// Write Original Time Code Register
	bcm_phy_write_exp(phydev, ORIGINAL_TIME_CODE_0, original_time_codes[0]);
	bcm_phy_write_exp(phydev, ORIGINAL_TIME_CODE_1, original_time_codes[1]);
	bcm_phy_write_exp(phydev, ORIGINAL_TIME_CODE_2, original_time_codes[2]);
	bcm_phy_write_exp(phydev, ORIGINAL_TIME_CODE_3, original_time_codes[3]);
	bcm_phy_write_exp(phydev, ORIGINAL_TIME_CODE_4, original_time_codes[4]);

	// Write Local Time Code Register
	bcm_phy_write_exp(phydev, NSE_DPPL_NCO_2_0_REG, local_time_codes[0]);
	bcm_phy_write_exp(phydev, NSE_DPPL_NCO_2_1_REG, local_time_codes[1]);
	bcm_phy_write_exp(phydev, NSE_DPPL_NCO_2_2_REG, local_time_codes[2]);

	// Write Shadow register
	bcm_phy_write_exp(phydev, SHADOW_REG_CONTROL, 0x0000);
	bcm_phy_write_exp(phydev, SHADOW_REG_LOAD, shadow_load_register);

	// Set global mode and nse_init
	nco_6_register_value = bcm54210pe_get_base_nco6_reg(ptp->chosen,
							    nco_6_register_value, true);

	// Write to register
	bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG, nco_6_register_value);

	// Trigger framesync
	bcm_phy_modify_exp(phydev, NSE_DPPL_NCO_6_REG, 0x003C, 0x0020);

	// Set the second on set
	ptp->chosen->second_on_set = ts->tv_sec;

	mutex_unlock(&ptp->chosen->clock_lock);

	return 0;
}

static int bcm54210pe_adjfine(struct ptp_clock_info *info, long scaled_ppm)
{
	int err;
	u16 lo, hi;
	u32 corrected_8ns_interval, base_8ns_interval;
	bool negative;

	struct bcm54210pe_ptp *ptp = container_of(info, struct bcm54210pe_ptp, caps);
	struct phy_device *phydev = ptp->chosen->phydev;

	negative = false;
	if (scaled_ppm < 0) {
		negative = true;
		scaled_ppm = -scaled_ppm;
	}

	// This is not completely accurate but very fast
	scaled_ppm >>= 7;

	// Nominal counter increment is 8ns
	base_8ns_interval = 1 << 31;

	// Add or subtract differential
	if (negative)
		corrected_8ns_interval = base_8ns_interval - scaled_ppm;
	else
		corrected_8ns_interval = base_8ns_interval + scaled_ppm;

	// Load up registers
	hi = (corrected_8ns_interval & 0xFFFF0000) >> 16;
	lo = (corrected_8ns_interval & 0x0000FFFF);

	mutex_lock(&ptp->chosen->clock_lock);

	// Set freq_mdio_sel to 1
	bcm_phy_write_exp(phydev, NSE_DPPL_NCO_2_2_REG, 0x4000);

	// Load 125MHz frequency reqcntrl
	bcm_phy_write_exp(phydev, NSE_DPPL_NCO_1_MSB_REG, hi);
	bcm_phy_write_exp(phydev, NSE_DPPL_NCO_1_LSB_REG, lo);

	// On next framesync load freq from freqcntrl
	bcm_phy_write_exp(phydev, SHADOW_REG_LOAD, 0x0040);

	// Trigger framesync
	err = bcm_phy_modify_exp(phydev, NSE_DPPL_NCO_6_REG, 0x003C, 0x0020);

	mutex_unlock(&ptp->chosen->clock_lock);

	return err;
}

static int bcm54210pe_adjtime(struct ptp_clock_info *info, s64 delta)
{
	int err;
	struct timespec64 ts;
	u64 now;

	err = bcm54210pe_gettime(info, &ts);
	if (err < 0)
		return err;

	now = ktime_to_ns(timespec64_to_ktime(ts));
	ts = ns_to_timespec64(now + delta);

	err = bcm54210pe_settime(info, &ts);

	return err;
}

static int bcm54210pe_extts_enable(struct bcm54210pe_private *private, int enable)
{
	int err;
	struct phy_device *phydev;
	u16 nco_6_register_value;

	phydev = private->phydev;

	if (enable) {
		if (!private->extts_en) {
			// Set enable per_out
			private->extts_en = true;
			err = bcm_phy_write_exp(phydev, NSE_DPPL_NCO_4_REG, 0x0001);

			nco_6_register_value = 0;
			nco_6_register_value = bcm54210pe_get_base_nco6_reg(private,
									    nco_6_register_value,
									    false);

			err = bcm_phy_write_exp(phydev, NSE_DPPL_NCO_7_0_REG, 0x0100);
			err = bcm_phy_write_exp(phydev, NSE_DPPL_NCO_7_1_REG, 0x0200);
			err = bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG, nco_6_register_value);

			schedule_delayed_work(&private->extts_ws, msecs_to_jiffies(1));
		}

	} else {
		private->extts_en = false;
		err = bcm_phy_write_exp(phydev, NSE_DPPL_NCO_4_REG, 0x0000);
	}

	return err;
}

static void bcm54210pe_run_extts_thread(struct work_struct *extts_ws)
{
	struct bcm54210pe_private *private;
	struct phy_device *phydev;
	u64 interval, time_stamp_48, time_stamp_80;

	private = container_of((struct delayed_work *)extts_ws,
			       struct bcm54210pe_private, extts_ws);
	phydev = private->phydev;

	interval = 10;	// in ms - long after we are gone from this earth, discussions will be had
			// and songs will be sung about whether this interval is short enough....
			// Before you complain let me say that in Timebeat.app up to ~150ms allows
			// single digit ns servo accuracy. If your client / servo is not as cool:
			// Do better :-)

	mutex_lock(&private->clock_lock);

	bcm54210pe_read80bittime_register(phydev, &time_stamp_80, &time_stamp_48);

	if (private->last_extts_ts != time_stamp_48 &&
	    private->last_immediate_ts[0] != time_stamp_48 &&
	    private->last_immediate_ts[1] != time_stamp_80) {
		bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG, 0xE000);
		bcm54210pe_trigger_extts_event(private, time_stamp_48);
		bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG, 0xE004);
	}

	mutex_unlock(&private->clock_lock);

	// Do we need to reschedule
	if (private->extts_en)
		schedule_delayed_work(&private->extts_ws, msecs_to_jiffies(interval));
}

static int bcm54210pe_perout_enable(struct bcm54210pe_private *private, s64 period,
				    s64 pulsewidth, int enable)
{
	struct phy_device *phydev;
	u16 nco_6_register_value, frequency_hi, frequency_lo,
		pulsewidth_reg, pulse_start_hi, pulse_start_lo;
	int err;

	phydev = private->phydev;

	if (enable) {
		frequency_hi = 0;
		frequency_lo = 0;
		pulsewidth_reg = 0;
		pulse_start_hi = 0;
		pulse_start_lo = 0;

		// Convert interval pulse spacing (period) and pulsewidth to 8 ns units
		period /= 8;
		pulsewidth /= 8;

		// Mode 2 only: If pulsewidth is not explicitly set with PTP_PEROUT_DUTY_CYCLE
		if (pulsewidth == 0) {
			if (period < 2500) {
				// At a frequency at less than 20us (2500 x 8ns) set
				// pulse length to 1/10th of the interval pulse spacing
				pulsewidth = period / 10;

				// Where the interval pulse spacing is short,
				// ensure we set a pulse length of 8ns
				if (pulsewidth == 0)
					pulsewidth = 1;

			} else {
				// Otherwise set pulse with to 4us (8ns x 500 = 4us)
				pulsewidth = 500;
			}
		}

		if (private->perout_mode == SYNC_OUT_MODE_1) {
			// Set period
			private->perout_period = period;

			if (!private->perout_en) {
				// Set enable per_out
				private->perout_en = true;
				schedule_delayed_work(&private->perout_ws, msecs_to_jiffies(1));
			}

			err = 0;

		} else if (private->perout_mode == SYNC_OUT_MODE_2) {
			// Set enable per_out
			private->perout_en = true;

			// Calculate registers

			// Lowest 16 bits of 8ns interval pulse spacing [15:0]
			frequency_lo	= (u16)period;

			// Highest 14 bits of 8ns interval pulse spacing [29:16]
			frequency_hi	= (u16)(0x3FFF & (period >> 16));

			// 2 lowest bits of 8ns pulse length [1:0]
			frequency_hi   |= (u16)pulsewidth << 14;

			// 7 highest bit  of 8 ns pulse length [8:2]
			pulsewidth_reg	= (u16)(0x7F & (pulsewidth >> 2));

			// Get base value
			nco_6_register_value = bcm54210pe_get_base_nco6_reg(private,
									    nco_6_register_value,
									    true);

			mutex_lock(&private->clock_lock);

			// Write to register
			err = bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG,
						nco_6_register_value);

			// Set sync out pulse interval spacing and pulse length
			err |= bcm_phy_write_exp(phydev, NSE_DPPL_NCO_3_0_REG, frequency_lo);
			err |= bcm_phy_write_exp(phydev, NSE_DPPL_NCO_3_1_REG, frequency_hi);
			err |= bcm_phy_write_exp(phydev, NSE_DPPL_NCO_3_2_REG, pulsewidth_reg);

			// On next framesync load sync out frequency
			err |= bcm_phy_write_exp(phydev, SHADOW_REG_LOAD, 0x0200);

			// Trigger immediate framesync
			err |= bcm_phy_modify_exp(phydev, NSE_DPPL_NCO_6_REG, 0x003C, 0x0020);

			mutex_unlock(&private->clock_lock);
		}
	} else {
		// Set disable pps
		private->perout_en = false;

		// Get base value
		nco_6_register_value = bcm54210pe_get_base_nco6_reg(private,
								    nco_6_register_value,
								    false);

		mutex_lock(&private->clock_lock);

		// Write to register
		err = bcm_phy_write_exp(phydev, NSE_DPPL_NCO_6_REG, nco_6_register_value);

		mutex_unlock(&private->clock_lock);
	}

	return err;
}

static void bcm54210pe_run_perout_mode_one_thread(struct work_struct *perout_ws)
{
	struct bcm54210pe_private *private;
	u64 local_time_stamp_48bits; //, local_time_stamp_80bits;
	u64 next_event, time_before_next_pulse, period;
	u16 nco_6_register_value, pulsewidth_nco3_hack;
	u64 wait_one, wait_two;

	private = container_of((struct delayed_work *)perout_ws,
			       struct bcm54210pe_private, perout_ws);
	period = private->perout_period * 8;
	pulsewidth_nco3_hack = 250; // The BCM chip is broken.
				    // It does not respect this in sync out mode 1

	nco_6_register_value = 0;

	// Get base value
	nco_6_register_value = bcm54210pe_get_base_nco6_reg(private, nco_6_register_value, false);

	// Get 48 bit local time
	bcm54210pe_get48bittime(private, &local_time_stamp_48bits);

	// Calculate time before next event and next event time
	time_before_next_pulse =  period - (local_time_stamp_48bits % period);
	next_event = local_time_stamp_48bits + time_before_next_pulse;

	// Lock
	mutex_lock(&private->clock_lock);

	// Set pulsewidth (test reveal this does not work),
	// but registers need content or no pulse will exist
	bcm_phy_write_exp(private->phydev, NSE_DPPL_NCO_3_1_REG, pulsewidth_nco3_hack << 14);
	bcm_phy_write_exp(private->phydev, NSE_DPPL_NCO_3_2_REG, pulsewidth_nco3_hack >> 2);

	// Set sync out pulse interval spacing and pulse length
	bcm_phy_write_exp(private->phydev, NSE_DPPL_NCO_5_0_REG, next_event & 0xFFF0);
	bcm_phy_write_exp(private->phydev, NSE_DPPL_NCO_5_1_REG, next_event >> 16);
	bcm_phy_write_exp(private->phydev, NSE_DPPL_NCO_5_2_REG, next_event >> 32);

	// On next framesync load sync out frequency
	bcm_phy_write_exp(private->phydev, SHADOW_REG_LOAD, 0x0200);

	// Write to register with mode one set for sync out
	bcm_phy_write_exp(private->phydev, NSE_DPPL_NCO_6_REG, nco_6_register_value | 0x0001);

	// Trigger immediate framesync
	bcm_phy_modify_exp(private->phydev, NSE_DPPL_NCO_6_REG, 0x003C, 0x0020);

	// Unlock
	mutex_unlock(&private->clock_lock);

	// Wait until 1/10 period after the next pulse
	wait_one = (time_before_next_pulse / 1000000) + (period / 1000000 / 10);
	mdelay(wait_one);

	// Lock
	mutex_lock(&private->clock_lock);

	// Clear pulse by bumping sync_out_match to max (this pulls sync out down)
	bcm_phy_write_exp(private->phydev, NSE_DPPL_NCO_5_0_REG, 0xFFF0);
	bcm_phy_write_exp(private->phydev, NSE_DPPL_NCO_5_1_REG, 0xFFFF);
	bcm_phy_write_exp(private->phydev, NSE_DPPL_NCO_5_2_REG, 0xFFFF);

	// On next framesync load sync out frequency
	bcm_phy_write_exp(private->phydev, SHADOW_REG_LOAD, 0x0200);

	// Trigger immediate framesync
	bcm_phy_modify_exp(private->phydev, NSE_DPPL_NCO_6_REG, 0x003C, 0x0020);

	// Unlock
	mutex_unlock(&private->clock_lock);

	// Calculate wait before we reschedule the next pulse
	wait_two = (period / 1000000) - (2 * (period / 10000000));

	// Do we need to reschedule
	if (private->perout_en)
		schedule_delayed_work(&private->perout_ws, msecs_to_jiffies(wait_two));
}

bool bcm54210pe_rxtstamp(struct mii_timestamper *mii_ts, struct sk_buff *skb, int type)
{
	struct bcm54210pe_private *private = container_of(mii_ts, struct bcm54210pe_private,
							  mii_ts);

	if (private->hwts_rx_en) {
		skb_queue_tail(&private->rx_skb_queue, skb);
		schedule_work(&private->rxts_work);
		return true;
	}

	return false;
}

void bcm54210pe_txtstamp(struct mii_timestamper *mii_ts, struct sk_buff *skb, int type)
{
	struct bcm54210pe_private *private = container_of(mii_ts, struct bcm54210pe_private,
							  mii_ts);

	switch (private->hwts_tx_en) {
	case HWTSTAMP_TX_ON:
	{
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
		skb_queue_tail(&private->tx_skb_queue, skb);
		schedule_work(&private->txts_work);
		break;
	}

	case HWTSTAMP_TX_OFF:
	{
	}

	default:
	{
		kfree_skb(skb);
		break;
	}
	}
}

int bcm54210pe_ts_info(struct mii_timestamper *mii_ts, struct ethtool_ts_info *info)
{
	struct bcm54210pe_private *bcm54210pe = container_of(mii_ts, struct bcm54210pe_private,
							     mii_ts);

	info->so_timestamping =
		SOF_TIMESTAMPING_TX_HARDWARE |
		SOF_TIMESTAMPING_RX_HARDWARE |
		SOF_TIMESTAMPING_RAW_HARDWARE;

	info->phc_index = ptp_clock_index(bcm54210pe->ptp->ptp_clock);
	info->tx_types =
		(1 << HWTSTAMP_TX_OFF) |
		(1 << HWTSTAMP_TX_ON);
	info->rx_filters =
		(1 << HWTSTAMP_FILTER_NONE) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
		(1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT);
	return 0;
}

int bcm54210pe_hwtstamp(struct mii_timestamper *mii_ts, struct ifreq *ifr)
{
	struct bcm54210pe_private *device = container_of(mii_ts, struct bcm54210pe_private, mii_ts);

	struct hwtstamp_config cfg;

	if (copy_from_user(&cfg, ifr->ifr_data, sizeof(cfg)))
		return -EFAULT;

	if (cfg.flags) /* reserved for future extensions */
		return -EINVAL;

	if (cfg.tx_type < 0 || cfg.tx_type > HWTSTAMP_TX_ONESTEP_SYNC)
		return -ERANGE;

	device->hwts_tx_en = cfg.tx_type;

	switch (cfg.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		device->hwts_rx_en = 0;
		device->layer = 0;
		device->version = 0;
		break;
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		device->hwts_rx_en = 1;
		device->layer = PTP_CLASS_L4;
		device->version = PTP_CLASS_V1;
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_EVENT;
		break;
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		device->hwts_rx_en = 1;
		device->layer = PTP_CLASS_L4;
		device->version = PTP_CLASS_V2;
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
		device->hwts_rx_en = 1;
		device->layer = PTP_CLASS_L2;
		device->version = PTP_CLASS_V2;
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
		break;
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		device->hwts_rx_en = 1;
		device->layer = PTP_CLASS_L4 | PTP_CLASS_L2;
		device->version = PTP_CLASS_V2;
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		break;
	default:
		return -ERANGE;
	}

	return copy_to_user(ifr->ifr_data, &cfg, sizeof(cfg)) ? -EFAULT : 0;
}

static int bcm54210pe_feature_enable(struct ptp_clock_info *info,
				     struct ptp_clock_request *req, int on)
{
	struct bcm54210pe_ptp *ptp = container_of(info, struct bcm54210pe_ptp, caps);
	s64 period, pulsewidth;
	struct timespec64 ts;

	switch (req->type) {
	case PTP_CLK_REQ_PEROUT:

		period = 0;
		pulsewidth = 0;

		// Check if pin func is set correctly
		if (ptp->chosen->sdp_config[SYNC_OUT_PIN].func != PTP_PF_PEROUT)
			return -EOPNOTSUPP;

		// No other flags supported
		if (req->perout.flags & ~PTP_PEROUT_DUTY_CYCLE)
			return -EOPNOTSUPP;

		// Check if a specific pulsewidth is set
		if ((req->perout.flags & PTP_PEROUT_DUTY_CYCLE) > 0) {
			if (ptp->chosen->perout_mode == SYNC_OUT_MODE_1)
				return -EOPNOTSUPP;

			// Extract pulsewidth
			ts.tv_sec = req->perout.on.sec;
			ts.tv_nsec = req->perout.on.nsec;
			pulsewidth = timespec64_to_ns(&ts);

			// 9 bits in 8ns units, so max = 4,088ns
			if (pulsewidth > 511 * 8)
				return -ERANGE;
		}

		// Extract pulse spacing interval (period)
		ts.tv_sec = req->perout.period.sec;
		ts.tv_nsec = req->perout.period.nsec;
		period = timespec64_to_ns(&ts);

		// 16ns is minimum pulse spacing interval (a value of
		// 16 will result in 8ns high followed by 8 ns low)
		if (period != 0 && period < 16)
			return -ERANGE;

		return bcm54210pe_perout_enable(ptp->chosen, period, pulsewidth, on);

	case PTP_CLK_REQ_EXTTS:

		if (ptp->chosen->sdp_config[SYNC_IN_PIN].func != PTP_PF_EXTTS)
			return -EOPNOTSUPP;

		return bcm54210pe_extts_enable(ptp->chosen, on);

	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int bcm54210pe_ptp_verify_pin(struct ptp_clock_info *info, unsigned int pin,
				     enum ptp_pin_function func, unsigned int chan)
{
	switch (func) {
	case PTP_PF_NONE:
		return 0;
	case PTP_PF_EXTTS:
		if (pin == SYNC_IN_PIN)
			return 0;
		break;
	case PTP_PF_PEROUT:
		if (pin == SYNC_OUT_PIN)
			return 0;
		break;
	case PTP_PF_PHYSYNC:
		break;
	}
	return -1;
}

static const struct ptp_clock_info bcm54210pe_clk_caps = {
	.owner		= THIS_MODULE,
	.name		= "BCM54210PE_PHC",
	.max_adj	= 100000000,
	.n_alarm	= 0,
	.n_pins		= 2,
	.n_ext_ts	= 1,
	.n_per_out	= 1,
	.pps		= 0,
	.adjtime	= &bcm54210pe_adjtime,
	.adjfine	= &bcm54210pe_adjfine,
	.gettime64	= &bcm54210pe_gettime,
	.gettimex64	= &bcm54210pe_gettimex,
	.settime64	= &bcm54210pe_settime,
	.enable		= &bcm54210pe_feature_enable,
	.verify		= &bcm54210pe_ptp_verify_pin,
};

static int bcm54210pe_sw_reset(struct phy_device *phydev)
{
	u16 err;
	u16 aux;

	err =  bcm_phy_write_exp(phydev, EXT_SOFTWARE_RESET, EXT_RESET1);
	err |= bcm_phy_read_exp(phydev, EXT_ENABLE_REG1);

	if (err < 0)
		return err;

	err |= bcm_phy_write_exp(phydev, EXT_SOFTWARE_RESET, EXT_RESET2);
	aux = bcm_phy_read_exp(phydev, EXT_SOFTWARE_RESET);
	return err;
}

int bcm54210pe_probe(struct phy_device *phydev)
{
	int x, y;
	struct bcm54210pe_ptp *ptp;
	struct bcm54210pe_private *bcm54210pe;
	struct ptp_pin_desc *sync_in_pin_desc, *sync_out_pin_desc;

	bcm54210pe_sw_reset(phydev);
	bcm54210pe_config_1588(phydev);

	bcm54210pe = kzalloc(sizeof(*bcm54210pe), GFP_KERNEL);
	if (!bcm54210pe)
		return -ENOMEM;

	ptp = kzalloc(sizeof(*ptp), GFP_KERNEL);
	if (!ptp)
		return -ENOMEM;

	bcm54210pe->phydev = phydev;
	bcm54210pe->ptp = ptp;

	bcm54210pe->mii_ts.rxtstamp = bcm54210pe_rxtstamp;
	bcm54210pe->mii_ts.txtstamp = bcm54210pe_txtstamp;
	bcm54210pe->mii_ts.hwtstamp = bcm54210pe_hwtstamp;
	bcm54210pe->mii_ts.ts_info  = bcm54210pe_ts_info;

	phydev->mii_ts = &bcm54210pe->mii_ts;

	// Initialisation of work_structs and similar
	INIT_WORK(&bcm54210pe->txts_work, bcm54210pe_run_tx_timestamp_match_thread);
	INIT_WORK(&bcm54210pe->rxts_work, bcm54210pe_run_rx_timestamp_match_thread);
	INIT_DELAYED_WORK(&bcm54210pe->perout_ws, bcm54210pe_run_perout_mode_one_thread);
	INIT_DELAYED_WORK(&bcm54210pe->extts_ws, bcm54210pe_run_extts_thread);

	// SKB queues
	skb_queue_head_init(&bcm54210pe->tx_skb_queue);
	skb_queue_head_init(&bcm54210pe->rx_skb_queue);

	for (x = 0; x < CIRCULAR_BUFFER_COUNT; x++) {
		INIT_LIST_HEAD(&bcm54210pe->circular_buffers[x]);

		for (y = 0; y < CIRCULAR_BUFFER_ITEM_COUNT; y++)
			list_add(&bcm54210pe->circular_buffer_items[x][y].list,
				 &bcm54210pe->circular_buffers[x]);
	}

	// Caps
	memcpy(&bcm54210pe->ptp->caps, &bcm54210pe_clk_caps, sizeof(bcm54210pe_clk_caps));
	bcm54210pe->ptp->caps.pin_config = bcm54210pe->sdp_config;

	// Mutex
	mutex_init(&bcm54210pe->clock_lock);
	mutex_init(&bcm54210pe->timestamp_buffer_lock);

	// Features
	bcm54210pe->one_step = false;
	bcm54210pe->extts_en = false;
	bcm54210pe->perout_en = false;
	bcm54210pe->perout_mode = SYNC_OUT_MODE_1;

	// Fibonacci RSewoke style progressive backoff scheme
	bcm54210pe->fib_sequence[0] = 1;
	bcm54210pe->fib_sequence[1] = 1;
	bcm54210pe->fib_sequence[2] = 2;
	bcm54210pe->fib_sequence[3] = 3;
	bcm54210pe->fib_sequence[4] = 5;
	bcm54210pe->fib_sequence[5] = 8;
	bcm54210pe->fib_sequence[6] = 13;
	bcm54210pe->fib_sequence[7] = 21;
	bcm54210pe->fib_sequence[8] = 34;
	bcm54210pe->fib_sequence[9] = 55;

	//bcm54210pe->fib_sequence = {1, 1, 2, 3, 5, 8, 13, 21, 34, 55};
	bcm54210pe->fib_factor_rx = 10;
	bcm54210pe->fib_factor_tx = 10;

	// Pin descriptions
	sync_in_pin_desc = &bcm54210pe->sdp_config[SYNC_IN_PIN];
	snprintf(sync_in_pin_desc->name, sizeof(sync_in_pin_desc->name), "SYNC_IN");
	sync_in_pin_desc->index = SYNC_IN_PIN;
	sync_in_pin_desc->func = PTP_PF_NONE;

	sync_out_pin_desc = &bcm54210pe->sdp_config[SYNC_OUT_PIN];
	snprintf(sync_out_pin_desc->name, sizeof(sync_out_pin_desc->name), "SYNC_OUT");
	sync_out_pin_desc->index = SYNC_OUT_PIN;
	sync_out_pin_desc->func = PTP_PF_NONE;

	ptp->chosen = bcm54210pe;
	phydev->priv = bcm54210pe;
	ptp->caps.owner = THIS_MODULE;

	bcm54210pe->ptp->ptp_clock = ptp_clock_register(&bcm54210pe->ptp->caps, &phydev->mdio.dev);

	if (IS_ERR(bcm54210pe->ptp->ptp_clock))
		return PTR_ERR(bcm54210pe->ptp->ptp_clock);

	return 0;
}
