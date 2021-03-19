// SPDX-License-Identifier: GPL-2.0+
/*
 *  drivers/net/phy/bcm54210pe_ptp.h
 *
 * PTP for BCM54210PE header file
 *
 * Authors: Carlos Fernandez
 * License: GPL
 * Copyright (C) 2021 Technica-Electronics GmbH
 */


#include <linux/ptp_clock_kernel.h>                                                                  
#include <linux/list.h>

#define MAX_POOL_SIZE	32

struct bcm54210pe_ptp {
        struct ptp_clock_info caps;
        struct ptp_clock *ptp_clock;
	struct mutex clock_lock;
	struct bcm54210pe_private *chosen;
	struct mutex timeset_lock;
};

struct bcm54210pe_fifo_item
{
	struct list_head list;
	u16 ts[4];
	u8 domain_number;
	u8 msgtype;
	u8 txrx;
	u16 sequence_id;
        u16 source_clock_id[4];
	u16 port_number;
};

struct bcm54210pe_private {
	struct phy_device *phydev;
	struct bcm54210pe_ptp *ptp;
	struct mii_timestamper mii_ts;
	int ts_tx_config;
	int tx_rx_filter;
	bool one_step;
	struct sk_buff_head tx_queue;
	struct list_head tx_fifo;
	struct list_head rx_fifo;
	struct list_head ts_pool; 
	struct bcm54210pe_fifo_item ts_pool_data[MAX_POOL_SIZE];
	struct work_struct txts_work;
};
