// SPDX-License-Identifier: GPL-2.0
/*
 * Shared FPC I2C: queue backlight value for the touch poll path.
 */

#include <linux/atomic.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include "viewe_dsi_i2c_bus.h"

#define VIEWE_BL_ADDR	0x45
#define VIEWE_BL_REG	0x86

static atomic_t viewe_bl_pending = ATOMIC_INIT(-1);

void viewe_dsi_bl_request(u8 val)
{
	atomic_set(&viewe_bl_pending, val);
}
EXPORT_SYMBOL_GPL(viewe_dsi_bl_request);

void viewe_dsi_bl_flush(struct i2c_adapter *adap)
{
	int v;
	u8 buf[2];
	struct i2c_msg msg;
	int ret, i;

	if (!adap)
		return;

	v = atomic_xchg(&viewe_bl_pending, -1);
	if (v < 0)
		return;

	buf[0] = VIEWE_BL_REG;
	buf[1] = (u8)v;
	msg.addr = VIEWE_BL_ADDR;
	msg.flags = 0;
	msg.len = sizeof(buf);
	msg.buf = buf;

	for (i = 0; i < 8; i++) {
		ret = i2c_transfer(adap, &msg, 1);
		if (ret == 1)
			return;
	}
	/* retry next poll */
	atomic_set(&viewe_bl_pending, v);
}
EXPORT_SYMBOL_GPL(viewe_dsi_bl_flush);

static int __init viewe_dsi_i2c_bus_init(void)
{
	pr_info("viewe_dsi_i2c_bus: backlight queue ready\n");
	return 0;
}
module_init(viewe_dsi_i2c_bus_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Shared DSI I2C backlight queue");
