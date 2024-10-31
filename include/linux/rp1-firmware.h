/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2023 2023-2024 Raspberry Pi Ltd.
 */

#ifndef __SOC_RP1_FIRMWARE_H__
#define __SOC_RP1_FIRMWARE_H__

#include <linux/types.h>
#include <linux/of_device.h>

#define RP1_FOURCC(s) ((uint32_t)((s[0] << 24) | (s[1] << 16) | (s[2] << 8) | (s[3] << 0)))

struct rp1_firmware;

#if IS_ENABLED(CONFIG_FIRMWARE_RP1)
int rp1_firmware_message(struct rp1_firmware *fw, uint16_t op,
			 const void *data, unsigned int data_len,
			 void *resp, unsigned int resp_space);
void rp1_firmware_put(struct rp1_firmware *fw);
struct rp1_firmware *rp1_firmware_get(struct device_node *fwnode);
struct rp1_firmware *devm_rp1_firmware_get(struct device *dev, struct device_node *fwnode);
int rp1_firmware_get_feature(struct rp1_firmware *fw, uint32_t fourcc,
			     uint32_t *op_base, uint32_t *op_count);
#else
static inline int rp1_firmware_message(struct rp1_firmware *fw, uint16_t op,
				       const void *data, unsigned int data_len,
				       void *resp, unsigned int resp_space)
{
	return -EOPNOTSUPP;
}

static inline void rp1_firmware_put(struct rp1_firmware *fw) { }

static inline struct rp1_firmware *rp1_firmware_get(struct device_node *fwnode)
{
	return NULL;
}

static inline struct rp1_firmware *devm_rp1_firmware_get(struct device *dev,
							 struct device_node *fwnode)
{
	return NULL;
}

static inline int rp1_firmware_get_feature(struct rp1_firmware *fw, uint32_t fourcc,
					   uint32_t *op_base, uint32_t *op_count)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* __SOC_RP1_FIRMWARE_H__ */
