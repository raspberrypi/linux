/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 Raspberry Pi Ltd.
 *
 */

#ifndef _RP1_DPHY_
#define _RP1_DPHY_

#include <linux/io.h>
#include <linux/types.h>
#include <media/v4l2-mediabus.h>

struct dphy_data {
	struct device *dev;

	void __iomem *base;

	u32 dphy_rate;
	u32 max_lanes;
	u32 active_lanes;
	bool lane_polarities[1 + V4L2_MBUS_CSI2_MAX_DATA_LANES];
};

void dphy_probe(struct dphy_data *dphy);
void dphy_start(struct dphy_data *dphy);
void dphy_stop(struct dphy_data *dphy);

#endif
