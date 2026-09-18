/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VIEWE_DSI_I2C_BUS_H
#define VIEWE_DSI_I2C_BUS_H

#include <linux/types.h>

struct i2c_adapter;

/* Queue brightness; touch poll flushes after coord read (TP first). */
void viewe_dsi_bl_request(u8 val);
void viewe_dsi_bl_flush(struct i2c_adapter *adap);

#endif
