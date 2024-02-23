// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/**
 * raspberrypi-otp.c
 *
 * nvmem driver using firmware mailbox to access otp
 *
 * Copyright (c) 2024, Raspberry Pi Ltd.
 */

#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

struct rpi_otp_priv {
	struct rpi_firmware *fw;
	u32 block;
};

#define MAX_ROWS 192

#define RPI_FIRMWARE_GET_USER_OTP 0x00030024
#define RPI_FIRMWARE_SET_USER_OTP 0x00038024

static int rpi_otp_read(void *context, unsigned int offset, void *val,
			     size_t bytes)
{
	struct rpi_otp_priv *priv = context;
	int words = bytes / sizeof(u32);
	int index = offset / sizeof(u32);
	u32 data[3 + MAX_ROWS] = {priv->block, index, words};
	int err = 0;

	if (words > MAX_ROWS)
		return -EINVAL;

	err = rpi_firmware_property(priv->fw, RPI_FIRMWARE_GET_USER_OTP,
				    &data, sizeof(data));
	if (err == 0)
		memcpy(val, data + 3, bytes);
	else
		memset(val, 0xee, bytes);
	return err;
}

static int rpi_otp_write(void *context, unsigned int offset, void *val,
			     size_t bytes)
{
	struct rpi_otp_priv *priv = context;
	int words = bytes / sizeof(u32);
	int index = offset / sizeof(u32);
	u32 data[3 + MAX_ROWS] = {priv->block, index, words};

	if (bytes > MAX_ROWS * sizeof(u32))
		return -EINVAL;

	memcpy(data + 3, val, bytes);
	return rpi_firmware_property(priv->fw, RPI_FIRMWARE_SET_USER_OTP,
				    &data, sizeof(data));
}

static int rpi_otp_probe(struct platform_device *pdev)
{
	struct rpi_otp_priv *priv;
	struct nvmem_config config = {
		.dev = &pdev->dev,
		.reg_read = rpi_otp_read,
		.reg_write = rpi_otp_write,
		.stride = sizeof(u32),
		.word_size = sizeof(u32),
		.type = NVMEM_TYPE_OTP,
		.root_only = true,
	};
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *fw_node;
	struct rpi_firmware *fw;
	u32 reg[2];
	const char *pname;

	if (of_property_read_u32_array(np, "reg", reg, ARRAY_SIZE(reg))) {
		dev_err(dev, "Failed to parse \"reg\" property\n");
		return -EINVAL;
	}

	pname = of_get_property(np, "name", NULL);
	if (!pname) {
		dev_err(dev, "Failed to parse \"name\" property\n");
		return -ENOENT;
	}

	config.name = pname;
	config.size = reg[1] * sizeof(u32);
	config.read_only = !of_property_read_bool(np, "rw");

	fw_node = of_parse_phandle(np, "firmware", 0);
	if (!fw_node) {
		dev_err(dev, "Missing firmware node\n");
		return -ENOENT;
	}

	fw = rpi_firmware_get(fw_node);
	if (!fw)
		return -EPROBE_DEFER;

	priv = devm_kzalloc(config.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->fw = fw;
	priv->block = reg[0];
	config.priv = priv;

	return PTR_ERR_OR_ZERO(devm_nvmem_register(config.dev, &config));
}

static const struct of_device_id rpi_otp_of_match[] = {
	{ .compatible = "raspberrypi,rpi-otp", },
	{}
};

MODULE_DEVICE_TABLE(of, rpi_otp_of_match);

static struct platform_driver rpi_otp_driver = {
	.driver = {
		.name = "rpi_otp",
		.of_match_table = rpi_otp_of_match,
	},
	.probe = rpi_otp_probe,
};

module_platform_driver(rpi_otp_driver);

MODULE_AUTHOR("Dom Cobley <popcornmix@gmail.com>");
MODULE_LICENSE("GPL");
