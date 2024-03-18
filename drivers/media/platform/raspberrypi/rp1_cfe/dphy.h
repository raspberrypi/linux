/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 Raspberry Pi Ltd.
 *
 */

#ifndef _RP1_DPHY_
#define _RP1_DPHY_

#include <linux/io.h>
#include <linux/types.h>

struct dphy_data {
	struct device *dev;

	void __iomem *base;

	u32 dphy_freq;
	u32 num_lanes;
};

void dphy_probe(struct dphy_data *dphy);
void dphy_start(struct dphy_data *dphy);
void dphy_stop(struct dphy_data *dphy);

#endif
