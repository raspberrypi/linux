// SPDX-License-Identifier: GPL-2.0+
/*
 *  drivers/net/phy/bcm54210pe_ptp.c
 *
 * PTP module for BCM54210PE
 *
 * Authors: Carlos Fernandez
 * License: GPL
 * Copyright (C) 2021 Technica-Electronics GmbH
 */

#include <linux/gpio/consumer.h>
#include <linux/ip.h>                                                                                
#include <linux/net_tstamp.h>
#include <linux/mii.h>
#include <linux/phy.h>                                                                               
#include <linux/ptp_classify.h>
#include <linux/ptp_clock_kernel.h>                                                                  
#include <linux/udp.h>
#include <asm/unaligned.h> 
#include <linux/brcmphy.h>
#include <linux/irq.h>
#include <linux/workqueue.h>
#include "bcm54210pe_ptp.h"

/* PTP header data offsets	*/
#define PTP_CONTROL_OFFSET	32
#define PTP_TSMT_OFFSET 	0
#define PTP_SEQUENCE_ID_OFFSET	30
#define PTP_CLOCK_ID_OFFSET	20
#define PTP_CLOCK_ID_SIZE	8
#define PTP_SEQUENCE_PORT_NUMER_OFFSET  (PTP_CLOCK_ID_OFFSET + PTP_CLOCK_ID_SIZE)


#define EXT_SELECT_REG		0x17
#define EXT_DATA_REG		0x15

#define EXT_ENABLE_REG1		0x17
#define EXT_ENABLE_DATA1	0x0F7E
#define EXT_ENABLE_REG2		0x15
#define EXT_ENABLE_DATA2	0x0000

#define EXT_1588_SLICE_REG	0x0810
#define EXT_1588_SLICE_DATA	0x0101

#define ORIGINAL_TIME_CODE_0 0x0854
#define ORIGINAL_TIME_CODE_1 0x0855
#define ORIGINAL_TIME_CODE_2 0x0856
#define ORIGINAL_TIME_CODE_3 0x0857
#define ORIGINAL_TIME_CODE_4 0x0858

#define TIME_STAMP_REG_0	0x0889
#define TIME_STAMP_REG_1	0x088A
#define TIME_STAMP_REG_2	0x088B
#define TIME_STAMP_REG_3	0x08C4
#define TIME_STAMP_INFO_1	0x088C
#define TIME_STAMP_INFO_2	0x088D
#define TIME_STAMP_INFO_3	0x08F9
#define TIME_STAMP_INFO_4	0x08FA
#define TIME_STAMP_INFO_5	0x08FB
#define TIME_STAMP_INFO_6	0x08FC
#define TIME_STAMP_INFO_7	0x08FD
#define INTERRUPT_STATUS_REG	0x085F
#define INTERRUPT_MASK_REG	0x085E
#define EXT_SOFTWARE_RESET	0x0F70
#define EXT_RESET1		0x0001 //RESET
#define EXT_RESET2		0x0000 //NORMAL OPERATION
#define GLOBAL_TIMESYNC_REG	0x0FF5

#define TX_EVENT_MODE_REG	0x0811
#define RX_EVENT_MODE_REG	0x0819
#define TX_TSCAPTURE_ENABLE_REG	0x0821
#define RX_TSCAPTURE_ENABLE_REG	0x0822
#define TXRX_1588_OPTION_REG	0x0823

#define TX_TS_OFFSET_LSB	0x0834
#define TX_TS_OFFSET_MSB	0x0835
#define RX_TS_OFFSET_LSB	0x0844
#define RX_TS_OFFSET_MSB	0x0845
#define NSE_DPPL_NCO_6_REG	0x087F
#define TIMECODE_SEL_REG	0x08C3
#define SHADOW_REG_CONTROL	0x085C
#define SHADOW_REG_LOAD		0x085D

static long ts_to_ns(u16 *ts)
{
	long ns;
	ns = ts[3]; 
	ns = (ns << 48) | ts[2];  
	ns = (ns << 32) | ts[1];
	ns = (ns << 16) | ts[0];
	return ns; 
}

/* get the start of the ptp header in this skb. Adapted from mv88e6xx/hwtstamp.c*/
static bool get_ptp_header(struct sk_buff *skb, unsigned int type, u8 *hdr )
{
	u8 *data = skb_mac_header(skb);
	unsigned int offset = 0;

	if (type & PTP_CLASS_VLAN)
		offset += VLAN_HLEN;

	switch (type & PTP_CLASS_PMASK) {
	case PTP_CLASS_IPV4:
		offset += ETH_HLEN + IPV4_HLEN(data + offset) + UDP_HLEN;
		break;
	case PTP_CLASS_IPV6:
		offset += ETH_HLEN + IP6_HLEN + UDP_HLEN;
		break;
	case PTP_CLASS_L2:
		offset += ETH_HLEN;
		break;
	default:
		return false;
	}

	/* Ensure that the entire header is present in this packet. */
	if (skb->len + ETH_HLEN < offset + 34)
		return false;

	hdr = data + offset;
	return true;
}


static bool compare_ptp_header(struct sk_buff *skb, struct bcm54210pe_fifo_item *item)
{
	unsigned int type;
	u8 *hdr; 
	u8 msgtype;
        u16 seqid;
	u16 port_number; 

	type = ptp_classify_raw(skb);

	if(!get_ptp_header(skb, type, hdr))
		return false;

	if (unlikely(type & PTP_CLASS_V1)) {
                /* msg type is located at the control field for ptp v1 */
                memcpy(&msgtype, hdr + PTP_CONTROL_OFFSET, sizeof(u8));
        } else {
                memcpy(&msgtype, hdr + PTP_TSMT_OFFSET, sizeof(u8));
                msgtype &= 0x0f;
        }

	seqid = ntohs((__be16 *)(hdr + PTP_SEQUENCE_ID_OFFSET));
	
	memcpy(&port_number, hdr + PTP_SEQUENCE_PORT_NUMER_OFFSET, sizeof(u16));

	if (msgtype != item->msgtype)
		return false;
	if (seqid != item->sequence_id)
		return false;
	return true;
}

static void tx_timestamp_work(struct work_struct *w)
{
	struct bcm54210pe_private *priv = container_of(w, struct bcm54210pe_private,
                                                 txts_work);
	struct skb_shared_hwtstamps *shhwtstamps = NULL;
	struct sk_buff *skb;
	struct bcm54210pe_fifo_item *item; 
	struct list_head *this, *next;
	
	skb = skb_dequeue(&priv->tx_queue);
	if (!skb) {
		return;
	}	printk("DEBUG: %s %d\n",__FUNCTION__, __LINE__);
	
	list_for_each_safe(this, next, &priv->tx_fifo) {
		item = list_entry(this, struct bcm54210pe_fifo_item, list);
		if (compare_ptp_header(skb, item)) {
			shhwtstamps = skb_hwtstamps(skb);
			memset(shhwtstamps, 0, sizeof(*shhwtstamps));
			shhwtstamps->hwtstamp = ns_to_ktime((ts_to_ns(item->ts)));
			list_del_init(&item->list);
			list_add(&item->list, &priv->ts_pool);
		}
	}
	if (shhwtstamps)
		skb_complete_tx_timestamp(skb, shhwtstamps);
	return; 
}
 
static int bcm54210pe_read_ext(struct phy_device *phydev, int reg, u16 *val)
{
	int err;
	err = phy_write(phydev, EXT_SELECT_REG, reg);
	if (err < 0)
		return err;
	*val = phy_read(phydev, EXT_DATA_REG);
	if (err < 0)
		return err;
	
	printk("DEBUG Reading %X on register %X\n",*val, reg);
       	return 0;
}

static int bcm54210pe_write_ext(struct phy_device *phydev, int reg, u16 val)
{
	int err;
	printk("DEBUG Writing %X on register %X\n",val, reg);
	err = phy_write(phydev, EXT_SELECT_REG, reg);
	if (err < 0)
		return err;
	err = phy_write(phydev, EXT_DATA_REG, val);
	return err; 
}

static int bcm54210pe_sw_reset(struct phy_device *phydev)
{
	u16 err;
	u16 aux;
        
	err =  bcm54210pe_write_ext(phydev, EXT_SOFTWARE_RESET, EXT_RESET1);
	bcm54210pe_read_ext(phydev, EXT_ENABLE_REG1, &err);
        if (err < 0)
                return err;
        err = bcm54210pe_write_ext(phydev, EXT_SOFTWARE_RESET, EXT_RESET2);
	err = bcm54210pe_read_ext(phydev, EXT_SOFTWARE_RESET, &aux);
        return err;
}

static int bcm54210pe_get_fifo(struct phy_device *phydev)
{	
	struct bcm54210pe_private *priv = phydev->priv;
	struct bcm54210pe_fifo_item *item;
	u16 fifo_info[2];
	u16 pending_interrupt = 0;


	do {
		if (!list_empty(&priv->ts_pool)) {
			item = list_first_entry(&priv->ts_pool, 
					struct bcm54210pe_fifo_item, list);
			list_del_init(&item->list);
		}
		else
			return -ENOMEM;

		// Set the read start bit, which stops the fifo from changing whilst it's being read
		bcm54210pe_write_ext(phydev, 0x885, 1);

		bcm54210pe_read_ext(phydev, 0x8c4, item->ts[3]); 
		bcm54210pe_read_ext(phydev, 0x88b, item->ts[2]); 
		bcm54210pe_read_ext(phydev, 0x88a, item->ts[1]); 
		bcm54210pe_read_ext(phydev, 0x889, item->ts[0]); 
		bcm54210pe_read_ext(phydev, 0x88c, &fifo_info[0]); 
		bcm54210pe_read_ext(phydev, 0x88d, &fifo_info[1]);
		//  Set the read end bit
		bcm54210pe_write_ext(phydev, 0x885, 2);
		// Then clear it (why this can't be self clearing makes no sense!
		bcm54210pe_write_ext(phydev, 0x885, 0);

		item->msgtype = (u8) (fifo_info[0] & 0x000F); 
		item->txrx = (fifo_info[0] & 0x0010);
		item->sequence_id = fifo_info[1];

		if (item->txrx) {
			list_add_tail(&item->list, &priv->tx_fifo);
			schedule_work(&priv->txts_work);
		}
		else
			list_add_tail(&item->list, &priv->rx_fifo); 
	
		bcm54210pe_read_ext(phydev, 0x85f, &pending_interrupt); 
		pending_interrupt &= 2;

	} while (pending_interrupt); 

	return 0; 
}

irqreturn_t bcm54210pe_handle_interrupt(struct phy_device *phydev)
{
	u16 interrupt_status = 0; 
	
	printk("DEBUG: %s %d\n",__FUNCTION__, __LINE__);
	
	bcm54210pe_read_ext(phydev, INTERRUPT_STATUS_REG, &interrupt_status);
	if (!interrupt_status)
		return IRQ_NONE;

	if (interrupt_status & 0x0001)
		bcm54210pe_get_fifo(phydev); 
	return IRQ_HANDLED;
}


static int bcm54210pe_config_1588(struct phy_device *phydev)
{
	int err;

	err =  bcm54210pe_write_ext(phydev, GLOBAL_TIMESYNC_REG, 0x0001); //Enable global timesync register.
	err =  bcm54210pe_write_ext(phydev, EXT_1588_SLICE_REG, 0x0101); //ENABLE TX and RX slice 1588
	err =  bcm54210pe_write_ext(phydev, TX_EVENT_MODE_REG, 0xFF00); //Add 80bit timestamp + NO CPU MODE in TX
	err =  bcm54210pe_write_ext(phydev, RX_EVENT_MODE_REG, 0xFF00); //Add 32+32 bits timestamp + NO CPU mode in RX
	err =  bcm54210pe_write_ext(phydev, TIMECODE_SEL_REG, 0x0101); //Select 80 bit counter
	

	err =  bcm54210pe_write_ext(phydev, TX_TSCAPTURE_ENABLE_REG, 0x0001); //Enable timestamp capture in TX 
	err =  bcm54210pe_write_ext(phydev, RX_TSCAPTURE_ENABLE_REG, 0x0001); //Enable timestamp capture in RX

	//Load Original Time Code Register
	err =  bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_0, 0x0064);
	err =  bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_1, 0x0064);
	err =  bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_2, 0x0064);
	err =  bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_3, 0x0064);
	err =  bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_4, 0x0064);

	//Enable shadow register
	err = bcm54210pe_write_ext(phydev, SHADOW_REG_CONTROL, 0x0000);
	err = bcm54210pe_write_ext(phydev, SHADOW_REG_LOAD, 0x07c0);

	//1n ts resolution
	err = bcm54210pe_write_ext(phydev,0x85b, 0x0160);

	err =  bcm54210pe_write_ext(phydev, NSE_DPPL_NCO_6_REG, 0xF020); //NCO Register 6 => Enable SYNC_OUT pulse train and Internal Syncout ad framesync

	return err; 
}

static int bcm54210pe_gettime(struct ptp_clock_info *info, struct timespec64 *ts)
{
	u16 time4, time3, time2, time1, time0;
	struct bcm54210pe_ptp *ptp =
		container_of(info, struct bcm54210pe_ptp, caps);
	struct phy_device *phydev = ptp->chosen->phydev;

	printk("DEBUG: %s %d\n",__FUNCTION__, __LINE__);
	mutex_lock(&ptp->clock_lock);

	// Trigger sync which will capture the heartbeat counter
	bcm54210pe_write_ext(phydev, NSE_DPPL_NCO_6_REG, 0xF000);
	bcm54210pe_write_ext(phydev, NSE_DPPL_NCO_6_REG, 0xF020);

	// Set Heart beat time read start
	bcm54210pe_write_ext(phydev, 0x88e, 0x400);
	bcm54210pe_read_ext(phydev, 0x8ed, &time4);
	bcm54210pe_read_ext(phydev, 0x8ec, &time3);
	bcm54210pe_read_ext(phydev, 0x888, &time2);
	bcm54210pe_read_ext(phydev, 0x887, &time1);
	bcm54210pe_read_ext(phydev, 0x886, &time0);
	
	// Set read end bit
	bcm54210pe_write_ext(phydev, 0x88e, 0x800);
	bcm54210pe_write_ext(phydev, 0x88e, 0x000);

	mutex_unlock(&ptp->clock_lock);
	
	printk("DEBUG: Get time %d %d %d %d %d\n",time4, time3, time2, time1, time0);
	
	ts->tv_sec = time4; 
	ts->tv_sec = (ts->tv_sec << 32) | time3;  
	ts->tv_sec = (ts->tv_sec << 16) | time2;
	ts->tv_nsec = time1;
	ts->tv_nsec = (ts->tv_nsec << 16) | time0;

	printk("DEBUG: Current PCH time %d.%d\n",ts->tv_sec, ts->tv_nsec);
	
	return 0;
}


static int bcm54210pe_settime(struct ptp_clock_info *info,
                           const struct timespec64 *ts)
{
	int var[4];

	struct bcm54210pe_ptp *ptp =
		container_of(info, struct bcm54210pe_ptp, caps);
	struct phy_device *phydev = ptp->chosen->phydev;
	
	var[4] = (int) (ts->tv_sec & 0xFFFF00000000) >> 32;
	var[3] = (int) (ts->tv_sec & 0x0000FFFF0000) >> 16; 
	var[2] = (int) (ts->tv_sec & 0x00000000FFFF);
	var[1] = (int) (ts->tv_nsec & 0x0000FFFF00000) >> 16;
	var[0] = (int) (ts->tv_nsec & 0x000000000FFFF); 

	printk("DEBUG: Original time %d %d\n",ts->tv_sec, ts->tv_nsec);
	printk("DEBUG: Set time %d %d %d %d %d\n",var[4], var[3], var[2], var[1], var[0]);
	
	bcm54210pe_write_ext(phydev, NSE_DPPL_NCO_6_REG, 0xF000);
	
	//Load Original Time Code Register
	bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_0, var[0]);
	bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_1, var[1]);
	bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_2, var[2]);
	bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_3, var[3]);
	bcm54210pe_write_ext(phydev, ORIGINAL_TIME_CODE_4, var[4]);

	//Enable shadow register
	bcm54210pe_write_ext(phydev, SHADOW_REG_CONTROL, 0x0000);
	bcm54210pe_write_ext(phydev, SHADOW_REG_LOAD, 0x07c0);
	
	bcm54210pe_write_ext(phydev, NSE_DPPL_NCO_6_REG, 0xF020); //NCO Register 6 => Enable SYNC_OUT pulse train and Internal Syncout ad framesync
	
	return 0; 
}

static int bcm54210pe_adjfine(struct ptp_clock_info *info, long scaled_ppm)
{
	int err = 0; 
	u64 adj;
	u16 lo, hi;

	struct bcm54210pe_ptp *ptp =
		container_of(info, struct bcm54210pe_ptp, caps);
	struct phy_device *phydev = ptp->chosen->phydev;

	if (scaled_ppm < 0) {
		err = -EINVAL; 
		goto finish;
	}

	adj = scaled_ppm;
	adj <<= 13;
	adj = div_u64(adj, 15625);
	
	hi = (adj >> 16);
	lo = adj & 0xffff;

	mutex_lock(&ptp->timeset_lock);
	
	bcm54210pe_write_ext(phydev, NSE_DPPL_NCO_6_REG, 0xF000);
	bcm54210pe_write_ext(phydev, 0x0873, lo);
	bcm54210pe_write_ext(phydev, 0x0874, hi);	
	
	//Enable shadow register
	bcm54210pe_write_ext(phydev, SHADOW_REG_CONTROL, 0x0000);
	bcm54210pe_write_ext(phydev, SHADOW_REG_LOAD, 0x07c0);
	//Force sync
	bcm54210pe_write_ext(phydev, NSE_DPPL_NCO_6_REG, 0xF020); 
finish:
	mutex_unlock(&ptp->timeset_lock);
	return err;

}

static int bcm54210pe_adjtime(struct ptp_clock_info *info, s64 delta)
{
	int err; 
	struct timespec64 ts;
	u64 now;

	struct bcm54210pe_ptp *ptp =
		container_of(info, struct bcm54210pe_ptp, caps);
	struct phy_device *phydev = ptp->chosen->phydev;

	mutex_lock(&ptp->timeset_lock);

	err = bcm54210pe_gettime(info, &ts);
	if (err < 0)
		goto finish;	
	
	now = ktime_to_ns(timespec64_to_ktime(ts));
	ts = ns_to_timespec64(now + delta);
	
	err = bcm54210pe_settime(info, &ts);

finish:
	mutex_unlock(&ptp->timeset_lock);
	return err;
}

bool bcm54210pe_rxtstamp(struct mii_timestamper *mii_ts,
                             struct sk_buff *skb, int type)
{
	struct bcm54210pe_fifo_item *item; 
	struct list_head *this, *next;
	struct skb_shared_hwtstamps *shhwtstamps = NULL;
	struct bcm54210pe_private *priv = 
		container_of(mii_ts, struct bcm54210pe_private, mii_ts);

	printk("DEBUG: %s %d\n",__FUNCTION__, __LINE__);
	
	list_for_each_safe(this, next, &priv->rx_fifo) {
		item = list_entry(this, struct bcm54210pe_fifo_item, list);
		if (compare_ptp_header(skb, item)) {
			shhwtstamps = skb_hwtstamps(skb);
			memset(shhwtstamps, 0, sizeof(*shhwtstamps));
			shhwtstamps->hwtstamp = ns_to_ktime((ts_to_ns(item->ts)));
			list_del_init(&item->list);
			list_add(&item->list, &priv->ts_pool);
		}
	}
	if (shhwtstamps) {
		netif_rx_ni(skb); 
		return true;
	}
	return false;
}


void bcm54210pe_txtstamp(struct mii_timestamper *mii_ts,
                             struct sk_buff *skb, int type)
{
	struct bcm54210pe_private *device = container_of(mii_ts,
			struct bcm54210pe_private, mii_ts);

	printk("DEBUG: %s %d\n",__FUNCTION__, __LINE__);

	switch (device->ts_tx_config) {
	case HWTSTAMP_TX_ON:
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
		skb_queue_tail(&device->tx_queue, skb);
		break;
	case HWTSTAMP_TX_OFF:
	default:
		kfree_skb(skb);
		break;
	}
}

int bcm54210pe_ts_info(struct mii_timestamper *mii_ts,
                           struct ethtool_ts_info *info)
{
	struct bcm54210pe_private *bcm54210pe = container_of(mii_ts, 
			struct bcm54210pe_private, mii_ts);

	printk("DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

	info->so_timestamping =
		SOF_TIMESTAMPING_TX_HARDWARE |
		SOF_TIMESTAMPING_RX_HARDWARE |
		SOF_TIMESTAMPING_RAW_HARDWARE;

	info->phc_index = ptp_clock_index(bcm54210pe->ptp->ptp_clock);
	info->tx_types =
		(1 << HWTSTAMP_TX_OFF) |
		(1 << HWTSTAMP_TX_ON) ;
		/* TODO: (1 << HWTSTAMP_TX_ONESTEP_SYNC);*/
      	info->rx_filters =
                (1 << HWTSTAMP_FILTER_NONE) |
                (1 << HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
                (1 << HWTSTAMP_FILTER_PTP_V2_L4_EVENT);
	return 0;
}

int bcm54210pe_hwtstamp(struct mii_timestamper *mii_ts, struct ifreq *ifr)
{
	struct hwtstamp_config ts_config;
	struct bcm54210pe_private *device = container_of(mii_ts, 
			struct bcm54210pe_private, mii_ts);

	printk("DEBUG: %s %d\n",__FUNCTION__, __LINE__);
	if (copy_from_user(&ts_config, ifr->ifr_data, sizeof(ts_config)))
		return -EFAULT;
	if (ts_config.flags) /* reserved for future extensions */
		return -EINVAL;
	if (ts_config.tx_type < 0)
		return -ERANGE;
	
	switch (ts_config.tx_type) {
        /* TODO: case HWTSTAMP_TX_ONESTEP_SYNC:
                device->one_step = true;
               	break;*/
        case HWTSTAMP_TX_ON:
               	break;
        case HWTSTAMP_TX_OFF:
               	break;
        default:
		return -ERANGE;		
	}
	
	device->ts_tx_config = ts_config.tx_type;

   	switch (ts_config.rx_filter) {
        case HWTSTAMP_FILTER_NONE:
                break;
       	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
                break;
       	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
                break;
       	default:
               	return -ERANGE;
	}

	device->tx_rx_filter = ts_config.rx_filter;
	skb_queue_purge(&device->tx_queue);
	return copy_to_user(ifr->ifr_data, &ts_config, sizeof(ts_config)) ? -EFAULT : 0;
}


static const struct ptp_clock_info bcm54210pe_clk_caps = {
        .owner          = THIS_MODULE,
        .name           = "BCM54210PE_PHC",
        .max_adj        = S32_MAX,
        .n_alarm        = 0,
        .n_pins         = 0,
        .n_ext_ts       = 0,
        .n_per_out      = 0,
        .pps            = 0,
        .adjtime        = &bcm54210pe_adjtime,
        .adjfine        = &bcm54210pe_adjfine,
        .gettime64      = &bcm54210pe_gettime,
        .settime64      = &bcm54210pe_settime,
};


int bcm54210pe_probe(struct phy_device *phydev)
{
	int err = 0, i;
	struct bcm54210pe_ptp *ptp;
        struct bcm54210pe_private *bcm54210pe;

	printk("DEBUG: HI! Passed %s %d \n",__FUNCTION__,__LINE__);

	bcm54210pe_sw_reset(phydev);
	bcm54210pe_config_1588(phydev);
	bcm54210pe = kzalloc(sizeof(struct bcm54210pe_private), GFP_KERNEL);
        if (!bcm54210pe) {
		err = -ENOMEM;
                goto error;
	}

	ptp = kzalloc(sizeof(struct bcm54210pe_ptp), GFP_KERNEL);
        if (!ptp) {
		err = -ENOMEM;
                goto error;
	}

        bcm54210pe->phydev = phydev;

	bcm54210pe->ptp = ptp;

	skb_queue_head_init(&bcm54210pe->tx_queue);
	bcm54210pe->mii_ts.rxtstamp =  bcm54210pe_rxtstamp;
	bcm54210pe->mii_ts.txtstamp = bcm54210pe_txtstamp;
	bcm54210pe->mii_ts.hwtstamp = bcm54210pe_hwtstamp;
	bcm54210pe->mii_ts.ts_info  = bcm54210pe_ts_info;


	phydev->mii_ts = &bcm54210pe->mii_ts;
	
	INIT_WORK(&bcm54210pe->txts_work, tx_timestamp_work);
	INIT_LIST_HEAD(&bcm54210pe->tx_fifo);
	INIT_LIST_HEAD(&bcm54210pe->rx_fifo);
	INIT_LIST_HEAD(&bcm54210pe->ts_pool);
	
	for (i = 0; i < MAX_POOL_SIZE; i++)
		list_add(&bcm54210pe->ts_pool_data[i].list, &bcm54210pe->ts_pool);
	
	memcpy(&bcm54210pe->ptp->caps, &bcm54210pe_clk_caps, sizeof(bcm54210pe_clk_caps));
	mutex_init(&bcm54210pe->ptp->clock_lock);
	mutex_init(&bcm54210pe->ptp->timeset_lock);
	ptp->chosen = bcm54210pe;
        phydev->priv = bcm54210pe;
	ptp->caps.owner = THIS_MODULE;

	bcm54210pe->ptp->ptp_clock = ptp_clock_register(&bcm54210pe->ptp->caps,
			&phydev->mdio.dev);
	if (IS_ERR(bcm54210pe->ptp->ptp_clock)) {
                        err = PTR_ERR(bcm54210pe->ptp->ptp_clock);
                        goto error;
	}
	printk("DEBUG: %s %d\n",__FUNCTION__, __LINE__);
error:
	return err;
}
