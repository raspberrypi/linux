// SPDX-License-Identifier: GPL-2.0+
/*
 * Raspberry Pi Customer OTP driver
 *
 * Copyright (C) 2018 Stefan Wahren <stefan.wahren@i2se.com>
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define CUSTOMER_CELLS 8

struct rpi_otp {
	struct nvmem_device *nvmem;
	struct rpi_firmware *fw;
};

/*
 * Packet definition used by RPI_FIRMWARE_GET_CUSTOMER_OTP
 */
struct rpi_customer_otp_packet {
	u32 index;
	u32 length;
	u32 cells[CUSTOMER_CELLS];
};

static int rpi_otp_read(void *context, unsigned int offset, void *val,
			size_t bytes)
{
	struct rpi_customer_otp_packet packet;
	struct rpi_otp *otp = context;
	u32 *buf = val;
	int ret;

	packet.index = 0;
	packet.length = CUSTOMER_CELLS;
	memset(packet.cells, 0xff, sizeof(packet.cells));

	ret = rpi_firmware_property(otp->fw, RPI_FIRMWARE_GET_CUSTOMER_OTP,
				    &packet, sizeof(packet));

	if (ret)
		return ret;

	/* Request rejected by firmware */
	if (packet.index)
		return -EIO;

	while (bytes) {
		if ((offset / 4) < sizeof(packet.cells))
			*buf = packet.cells[offset / 4];
		else
			*buf = 0;

		buf++;
		bytes -= 4;
		offset += 4;
	}

	return 0;
}

static struct nvmem_config ocotp_config = {
	.name = "rpi-customer-otp",
	.size = CUSTOMER_CELLS * 4,
	.stride = 4,
	.word_size = 4,
	.reg_read = rpi_otp_read,
};

static int rpi_otp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	struct device_node *fw_np;
	struct rpi_otp *otp;

	match = of_match_device(dev->driver->of_match_table, dev);
	if (!match)
		return -EINVAL;

	otp = devm_kzalloc(dev, sizeof(*otp), GFP_KERNEL);
	if (!otp)
		return -ENOMEM;

	fw_np = of_parse_phandle(pdev->dev.of_node, "firmware", 0);
	if (!fw_np) {
		dev_err(&pdev->dev, "no firmware node\n");
		return -ENODEV;
	}

	otp->fw = rpi_firmware_get(fw_np);
	of_node_put(fw_np);
	if (!otp->fw)
		return -EPROBE_DEFER;

	ocotp_config.priv = otp;
	ocotp_config.dev = dev;
	otp->nvmem = nvmem_register(&ocotp_config);
	if (IS_ERR(otp->nvmem))
		return PTR_ERR(otp->nvmem);

	platform_set_drvdata(pdev, otp);

	return 0;
}

static int rpi_otp_remove(struct platform_device *pdev)
{
	struct rpi_otp *otp = platform_get_drvdata(pdev);

	return nvmem_unregister(otp->nvmem);
}

static const struct of_device_id rpi_otp_of_match[] = {
	{ .compatible = "raspberrypi,bcm2835-customer-otp", },
	{ /* sentinel */},
};
MODULE_DEVICE_TABLE(of, rpi_otp_of_match);

static struct platform_driver rpi_otp_driver = {
	.probe = rpi_otp_probe,
	.remove = rpi_otp_remove,
	.driver = {
		.name = "rpi-customer-otp",
		.of_match_table = rpi_otp_of_match,
	},
};

module_platform_driver(rpi_otp_driver);
MODULE_AUTHOR("Stefan Wahren <stefan.wahren@i2se.com>");
MODULE_DESCRIPTION("Raspberry Pi Customer OTP driver");
MODULE_LICENSE("GPL v2");
