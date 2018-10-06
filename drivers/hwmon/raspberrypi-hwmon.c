// SPDX-License-Identifier: GPL-2.0+
/*
 * Raspberry Pi voltage sensor driver
 *
 * Based on firmware/raspberrypi.c by Noralf Trønnes
 *
 * Copyright (C) 2018 Stefan Wahren <stefan.wahren@i2se.com>
 */
#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

/*
 * This section defines some rate limited logging that prevent
 * repeated messages at much lower Hz than the default kernel settings.
 * It's usually 5s, this is 5 minutes.
 * Burst 3 means you may get three messages 'quickly', before
 * the ratelimiting kicks in.
 */
#define LOCAL_RATELIMIT_INTERVAL (5 * 60 * HZ)
#define LOCAL_RATELIMIT_BURST 3

#ifdef CONFIG_PRINTK
#define printk_ratelimited_local(fmt, ...)	\
({						\
	static DEFINE_RATELIMIT_STATE(_rs,	\
		LOCAL_RATELIMIT_INTERVAL,	\
		LOCAL_RATELIMIT_BURST);		\
						\
	if (__ratelimit(&_rs))			\
		printk(fmt, ##__VA_ARGS__);	\
})
#else
#define printk_ratelimited_local(fmt, ...)	\
	no_printk(fmt, ##__VA_ARGS__)
#endif

#define pr_crit_ratelimited_local(fmt, ...)              \
	printk_ratelimited_local(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info_ratelimited_local(fmt, ...)              \
printk_ratelimited_local(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)

#define UNDERVOLTAGE_STICKY_BIT	BIT(16)

struct rpi_hwmon_data {
	struct device *hwmon_dev;
	struct rpi_firmware *fw;
	u32 last_throttled;
	struct delayed_work get_values_poll_work;
};

static void rpi_firmware_get_throttled(struct rpi_hwmon_data *data)
{
	u32 new_uv, old_uv, value;
	int ret;

	/* Request firmware to clear sticky bits */
	value = 0xffff;

	ret = rpi_firmware_property(data->fw, RPI_FIRMWARE_GET_THROTTLED,
				    &value, sizeof(value));
	if (ret) {
		dev_err_once(data->hwmon_dev, "Failed to get throttled (%d)\n",
			     ret);
		return;
	}

	new_uv = value & UNDERVOLTAGE_STICKY_BIT;
	old_uv = data->last_throttled & UNDERVOLTAGE_STICKY_BIT;
	data->last_throttled = value;

	if (new_uv == old_uv)
		return;

	if (new_uv) {
		pr_crit_ratelimited_local("Under-voltage detected! (0x%08x)\n",
					  value);
	} else {
		pr_info_ratelimited_local("Voltage normalised (0x%08x)\n",
					  value);
	}

	sysfs_notify(&data->hwmon_dev->kobj, NULL, "in0_lcrit_alarm");
}

static void get_values_poll(struct work_struct *work)
{
	struct rpi_hwmon_data *data;

	data = container_of(work, struct rpi_hwmon_data,
			    get_values_poll_work.work);

	rpi_firmware_get_throttled(data);

	/*
	 * We can't run faster than the sticky shift (100ms) since we get
	 * flipping in the sticky bits that are cleared.
	 */
	schedule_delayed_work(&data->get_values_poll_work, 2 * HZ);
}

static int rpi_read(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int channel, long *val)
{
	struct rpi_hwmon_data *data = dev_get_drvdata(dev);

	*val = !!(data->last_throttled & UNDERVOLTAGE_STICKY_BIT);
	return 0;
}

static umode_t rpi_is_visible(const void *_data, enum hwmon_sensor_types type,
			      u32 attr, int channel)
{
	return 0444;
}

static const struct hwmon_channel_info *rpi_info[] = {
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_LCRIT_ALARM),
	NULL
};

static const struct hwmon_ops rpi_hwmon_ops = {
	.is_visible = rpi_is_visible,
	.read = rpi_read,
};

static const struct hwmon_chip_info rpi_chip_info = {
	.ops = &rpi_hwmon_ops,
	.info = rpi_info,
};

static int rpi_hwmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rpi_hwmon_data *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* Parent driver assure that firmware is correct */
	data->fw = dev_get_drvdata(dev->parent);

	data->hwmon_dev = devm_hwmon_device_register_with_info(dev, "rpi_volt",
							       data,
							       &rpi_chip_info,
							       NULL);

	ret = devm_delayed_work_autocancel(dev, &data->get_values_poll_work,
					   get_values_poll);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, data);

	if (!PTR_ERR_OR_ZERO(data->hwmon_dev))
		schedule_delayed_work(&data->get_values_poll_work, 2 * HZ);

	return PTR_ERR_OR_ZERO(data->hwmon_dev);
}

static struct platform_driver rpi_hwmon_driver = {
	.probe = rpi_hwmon_probe,
	.driver = {
		.name = "raspberrypi-hwmon",
	},
};
module_platform_driver(rpi_hwmon_driver);

MODULE_AUTHOR("Stefan Wahren <wahrenst@gmx.net>");
MODULE_DESCRIPTION("Raspberry Pi voltage sensor driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:raspberrypi-hwmon");
