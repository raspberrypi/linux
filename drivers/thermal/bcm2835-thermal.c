/*****************************************************************************
* Copyright 2011 Broadcom Corporation.  All rights reserved.
*
* Unless you and Broadcom execute a separate written software license
* agreement governing use of this software, this software is licensed to you
* under the terms of the GNU General Public License version 2, available at
* http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
*
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a
* license other than the GPL, without Broadcom's express prior written
* consent.
*****************************************************************************/

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

static int bcm2835_thermal_get_property(struct thermal_zone_device *tz,
					unsigned long *temp, u32 tag)
{
	struct rpi_firmware *fw = tz->devdata;
	struct {
		u32 id;
		u32 val;
	} packet;
	int ret;

	*temp = 0;
	packet.id = 0;
	ret = rpi_firmware_property(fw, tag, &packet, sizeof(packet));
	if (ret) {
		dev_err(&tz->device, "Failed to get temperature\n");
		return ret;
	}

	*temp = packet.val;
	dev_dbg(&tz->device, "%stemp=%lu\n",
		tag == RPI_FIRMWARE_GET_MAX_TEMPERATURE ? "max" : "", *temp);

	return 0;
}

static int bcm2835_thermal_get_temp(struct thermal_zone_device *tz,
				    unsigned long *temp)
{
	return bcm2835_thermal_get_property(tz, temp,
					    RPI_FIRMWARE_GET_TEMPERATURE);
}

static int bcm2835_thermal_get_max_temp(struct thermal_zone_device *tz,
					int trip, unsigned long *temp)
{
	/*
	 * The maximum safe temperature of the SoC.
	 * Overclock may be disabled above this temperature.
	 */
	return bcm2835_thermal_get_property(tz, temp,
					    RPI_FIRMWARE_GET_MAX_TEMPERATURE);
}

static int bcm2835_thermal_get_trip_type(struct thermal_zone_device *tz,
					 int trip, enum thermal_trip_type *type)
{
	*type = THERMAL_TRIP_HOT;

	return 0;
}

static int bcm2835_thermal_get_mode(struct thermal_zone_device *tz,
				    enum thermal_device_mode *mode)
{
	*mode = THERMAL_DEVICE_ENABLED;

	return 0;
}

static struct thermal_zone_device_ops ops  = {
	.get_temp = bcm2835_thermal_get_temp,
	.get_trip_temp = bcm2835_thermal_get_max_temp,
	.get_trip_type = bcm2835_thermal_get_trip_type,
	.get_mode = bcm2835_thermal_get_mode,
};

static int bcm2835_thermal_probe(struct platform_device *pdev)
{
	struct device_node *fw_np;
	struct rpi_firmware *fw;
	struct thermal_zone_device *tz;

	fw_np = of_parse_phandle(pdev->dev.of_node, "firmware", 0);
/* Remove comment when booting without Device Tree is no longer supported
	if (!fw_np) {
		dev_err(&pdev->dev, "Missing firmware node\n");
		return -ENOENT;
	}
*/
	fw = rpi_firmware_get(fw_np);
	if (!fw)
		return -EPROBE_DEFER;

	tz = thermal_zone_device_register("bcm2835_thermal", 1, 0, fw, &ops,
					  NULL, 0, 0);
	if (IS_ERR(tz)) {
		dev_err(&pdev->dev, "Failed to register the thermal device\n");
		return PTR_ERR(tz);
	}

	platform_set_drvdata(pdev, tz);

	return 0;
}

static int bcm2835_thermal_remove(struct platform_device *pdev)
{
	thermal_zone_device_unregister(platform_get_drvdata(pdev));

	return 0;
}

static const struct of_device_id bcm2835_thermal_of_match_table[] = {
	{ .compatible = "brcm,bcm2835-thermal", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_thermal_of_match_table);

static struct platform_driver bcm2835_thermal_driver = {
	.probe = bcm2835_thermal_probe,
	.remove = bcm2835_thermal_remove,
	.driver = {
		.name = "bcm2835_thermal",
		.of_match_table = bcm2835_thermal_of_match_table,
	},
};
module_platform_driver(bcm2835_thermal_driver);

MODULE_AUTHOR("Dorian Peake");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_DESCRIPTION("Thermal driver for bcm2835 chip");
MODULE_LICENSE("GPL");
