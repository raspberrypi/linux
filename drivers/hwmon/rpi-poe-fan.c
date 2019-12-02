// SPDX-License-Identifier: GPL-2.0
/*
 * rpi-poe-fan.c - Hwmon driver for Raspberry Pi PoE HAT fan.
 *
 * Copyright (C) 2018 Raspberry Pi (Trading) Ltd.
 * Based on pwm-fan.c by Kamil Debski <k.debski@samsung.com>
 *
 * Author: Serge Schneider <serge@raspberrypi.org>
 */

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define MAX_PWM 255

#define POE_CUR_PWM 0x0
#define POE_DEF_PWM 0x1

struct rpi_poe_fan_ctx {
	struct mutex lock;
	struct rpi_firmware *fw;
	u32 set_tag;
	unsigned int pwm_value;
	unsigned int def_pwm_value;
	unsigned int rpi_poe_fan_state;
	unsigned int rpi_poe_fan_max_state;
	unsigned int *rpi_poe_fan_cooling_levels;
	struct thermal_cooling_device *cdev;
	struct notifier_block nb;
};

struct fw_tag_data_s{
	u32 reg;
	u32 val;
	u32 ret;
};

static int write_reg(struct rpi_firmware *fw, u32 reg, u32 *val, u32 set_tag)
{
	struct fw_tag_data_s fw_tag_data = {
		.reg = reg,
		.val = *val
	};
	int ret;

	ret = rpi_firmware_property(fw, set_tag,
				    &fw_tag_data, sizeof(fw_tag_data));
	if (ret) {
		return ret;
	} else if (fw_tag_data.ret) {
		return -EIO;
	}
	return 0;
}

static int read_reg(struct rpi_firmware *fw, u32 reg, u32 *val){
	struct fw_tag_data_s fw_tag_data = {
		.reg = reg,
	};
	int ret;
	ret = rpi_firmware_property(fw, RPI_FIRMWARE_GET_POE_HAT_VAL,
				    &fw_tag_data, sizeof(fw_tag_data));
	if (ret) {
		return ret;
	} else if (fw_tag_data.ret) {
		return -EIO;
	}
	*val = fw_tag_data.val;
	return 0;
}

static int rpi_poe_reboot(struct notifier_block *nb, unsigned long code,
			  void *unused)
{
	struct rpi_poe_fan_ctx *ctx = container_of(nb, struct rpi_poe_fan_ctx,
						   nb);

	if (ctx->pwm_value != ctx->def_pwm_value)
		write_reg(ctx->fw, POE_CUR_PWM, &ctx->def_pwm_value, ctx->set_tag);

	return NOTIFY_DONE;
}

static int  __set_pwm(struct rpi_poe_fan_ctx *ctx, u32 pwm)
{
	int ret = 0;

	mutex_lock(&ctx->lock);
	if (ctx->pwm_value == pwm)
		goto exit_set_pwm_err;

	ret = write_reg(ctx->fw, POE_CUR_PWM, &pwm, ctx->set_tag);
	if (!ret)
		ctx->pwm_value = pwm;
exit_set_pwm_err:
	mutex_unlock(&ctx->lock);
	return ret;
}

static int  __set_def_pwm(struct rpi_poe_fan_ctx *ctx, u32 def_pwm)
{
	int ret = 0;
	mutex_lock(&ctx->lock);
	if (ctx->def_pwm_value == def_pwm)
		goto exit_set_def_pwm_err;

	ret = write_reg(ctx->fw, POE_DEF_PWM, &def_pwm, ctx->set_tag);
	if (!ret)
		ctx->def_pwm_value = def_pwm;
exit_set_def_pwm_err:
	mutex_unlock(&ctx->lock);
	return ret;
}

static void rpi_poe_fan_update_state(struct rpi_poe_fan_ctx *ctx,
				     unsigned long pwm)
{
	int i;

	for (i = 0; i < ctx->rpi_poe_fan_max_state; ++i)
		if (pwm < ctx->rpi_poe_fan_cooling_levels[i + 1])
			break;

	ctx->rpi_poe_fan_state = i;
}

static ssize_t set_pwm(struct device *dev, struct device_attribute *attr,
		       const char *buf, size_t count)
{
	struct rpi_poe_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned long pwm;
	int ret;

	if (kstrtoul(buf, 10, &pwm) || pwm > MAX_PWM)
		return -EINVAL;

	ret = __set_pwm(ctx, pwm);
	if (ret)
		return ret;

	rpi_poe_fan_update_state(ctx, pwm);
	return count;
}

static ssize_t set_def_pwm(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct rpi_poe_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned long def_pwm;
	int ret;

	if (kstrtoul(buf, 10, &def_pwm) || def_pwm > MAX_PWM)
		return -EINVAL;

	ret = __set_def_pwm(ctx, def_pwm);
	if (ret)
		return ret;
	return count;
}

static ssize_t show_pwm(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct rpi_poe_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", ctx->pwm_value);
}

static ssize_t show_def_pwm(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct rpi_poe_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", ctx->def_pwm_value);
}


static SENSOR_DEVICE_ATTR(pwm1, 0644, show_pwm, set_pwm, 0);
static SENSOR_DEVICE_ATTR(def_pwm1, 0644, show_def_pwm, set_def_pwm, 1);

static struct attribute *rpi_poe_fan_attrs[] = {
	&sensor_dev_attr_pwm1.dev_attr.attr,
	&sensor_dev_attr_def_pwm1.dev_attr.attr,
	NULL,
};

ATTRIBUTE_GROUPS(rpi_poe_fan);

/* thermal cooling device callbacks */
static int rpi_poe_fan_get_max_state(struct thermal_cooling_device *cdev,
				     unsigned long *state)
{
	struct rpi_poe_fan_ctx *ctx = cdev->devdata;

	if (!ctx)
		return -EINVAL;

	*state = ctx->rpi_poe_fan_max_state;

	return 0;
}

static int rpi_poe_fan_get_cur_state(struct thermal_cooling_device *cdev,
				     unsigned long *state)
{
	struct rpi_poe_fan_ctx *ctx = cdev->devdata;

	if (!ctx)
		return -EINVAL;

	*state = ctx->rpi_poe_fan_state;

	return 0;
}

static int rpi_poe_fan_set_cur_state(struct thermal_cooling_device *cdev,
				     unsigned long state)
{
	struct rpi_poe_fan_ctx *ctx = cdev->devdata;
	int ret;

	if (!ctx || (state > ctx->rpi_poe_fan_max_state))
		return -EINVAL;

	if (state == ctx->rpi_poe_fan_state)
		return 0;

	ret = __set_pwm(ctx, ctx->rpi_poe_fan_cooling_levels[state]);
	if (ret) {
		dev_err(&cdev->device, "Cannot set pwm!\n");
		return ret;
	}

	ctx->rpi_poe_fan_state = state;

	return ret;
}

static const struct thermal_cooling_device_ops rpi_poe_fan_cooling_ops = {
	.get_max_state = rpi_poe_fan_get_max_state,
	.get_cur_state = rpi_poe_fan_get_cur_state,
	.set_cur_state = rpi_poe_fan_set_cur_state,
};

static int rpi_poe_fan_of_get_cooling_data(struct device *dev,
				       struct rpi_poe_fan_ctx *ctx)
{
	struct device_node *np = dev->of_node;
	int num, i, ret;

	if (!of_find_property(np, "cooling-levels", NULL))
		return 0;

	ret = of_property_count_u32_elems(np, "cooling-levels");
	if (ret <= 0) {
		dev_err(dev, "cooling-levels property missing or invalid: %d\n",
			ret);
		return ret ? : -EINVAL;
	}

	num = ret;
	ctx->rpi_poe_fan_cooling_levels = devm_kzalloc(dev, num * sizeof(u32),
						   GFP_KERNEL);
	if (!ctx->rpi_poe_fan_cooling_levels)
		return -ENOMEM;

	ret = of_property_read_u32_array(np, "cooling-levels",
					 ctx->rpi_poe_fan_cooling_levels, num);
	if (ret) {
		dev_err(dev, "Property 'cooling-levels' cannot be read!\n");
		return ret;
	}

	for (i = 0; i < num; i++) {
		if (ctx->rpi_poe_fan_cooling_levels[i] > MAX_PWM) {
			dev_err(dev, "PWM fan state[%d]:%d > %d\n", i,
				ctx->rpi_poe_fan_cooling_levels[i], MAX_PWM);
			return -EINVAL;
		}
	}

	ctx->rpi_poe_fan_max_state = num - 1;

	return 0;
}

static int rpi_poe_fan_probe(struct platform_device *pdev)
{
	struct thermal_cooling_device *cdev;
	struct rpi_poe_fan_ctx *ctx;
	struct device *hwmon;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *fw_node;
	u32 revision;
	int ret;

	fw_node = of_parse_phandle(np, "firmware", 0);
	if (!fw_node) {
		dev_err(&pdev->dev, "Missing firmware node\n");
		return -ENOENT;
	}

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);

	ctx->fw = rpi_firmware_get(fw_node);
	if (!ctx->fw)
		return -EPROBE_DEFER;
	ret = rpi_firmware_property(ctx->fw,
		RPI_FIRMWARE_GET_FIRMWARE_REVISION,
		&revision, sizeof(revision));
	if (ret) {
		dev_err(&pdev->dev, "Failed to get firmware revision: %i\n", ret);
		return ret;
	}
	if (revision < 0x60af72e8)
		ctx->set_tag = RPI_FIRMWARE_SET_POE_HAT_VAL_OLD;
	else
		ctx->set_tag = RPI_FIRMWARE_SET_POE_HAT_VAL;

	platform_set_drvdata(pdev, ctx);

	ctx->nb.notifier_call = rpi_poe_reboot;
	ret = register_reboot_notifier(&ctx->nb);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register reboot notifier: %i\n",
			ret);
		return ret;
	}
	ret = read_reg(ctx->fw, POE_DEF_PWM, &ctx->def_pwm_value);
	if (ret) {
		dev_err(&pdev->dev, "Failed to get default PWM value: %i\n",
			ret);
		goto err;
	}
	ret = read_reg(ctx->fw, POE_CUR_PWM, &ctx->pwm_value);
	if (ret) {
		dev_err(&pdev->dev, "Failed to get current PWM value: %i\n",
			ret);
		goto err;
	}

	hwmon = devm_hwmon_device_register_with_groups(&pdev->dev, "rpipoefan",
						       ctx, rpi_poe_fan_groups);
	if (IS_ERR(hwmon)) {
		dev_err(&pdev->dev, "Failed to register hwmon device\n");
		ret = PTR_ERR(hwmon);
		goto err;
	}

	ret = rpi_poe_fan_of_get_cooling_data(&pdev->dev, ctx);
	if (ret)
		return ret;

	rpi_poe_fan_update_state(ctx, ctx->pwm_value);
	if (!IS_ENABLED(CONFIG_THERMAL))
		return 0;

	cdev = thermal_of_cooling_device_register(np,
						  "rpi-poe-fan", ctx,
						  &rpi_poe_fan_cooling_ops);
	if (IS_ERR(cdev)) {
		dev_err(&pdev->dev,
			"Failed to register rpi-poe-fan as cooling device");
		ret = PTR_ERR(cdev);
		goto err;
	}
	ctx->cdev = cdev;
	//thermal_cdev_update(cdev);

	return 0;
err:
	unregister_reboot_notifier(&ctx->nb);
	return ret;
}

static int rpi_poe_fan_remove(struct platform_device *pdev)
{
	struct rpi_poe_fan_ctx *ctx = platform_get_drvdata(pdev);
	u32 value = ctx->def_pwm_value;

	unregister_reboot_notifier(&ctx->nb);
	thermal_cooling_device_unregister(ctx->cdev);
	if (ctx->pwm_value != value)
		write_reg(ctx->fw, POE_CUR_PWM, &value, ctx->set_tag);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int rpi_poe_fan_suspend(struct device *dev)
{
	struct rpi_poe_fan_ctx *ctx = dev_get_drvdata(dev);
	u32 value = 0;
	int ret = 0;

	if (ctx->pwm_value != value)
		ret = write_reg(ctx->fw, POE_CUR_PWM, &value, ctx->set_tag);
	return ret;
}

static int rpi_poe_fan_resume(struct device *dev)
{
	struct rpi_poe_fan_ctx *ctx = dev_get_drvdata(dev);
	u32 value = ctx->pwm_value;
	int ret = 0;

	if (value != 0)
		ret = write_reg(ctx->fw, POE_CUR_PWM, &value, ctx->set_tag);

	return ret;
}
#endif

static SIMPLE_DEV_PM_OPS(rpi_poe_fan_pm, rpi_poe_fan_suspend,
			 rpi_poe_fan_resume);

static const struct of_device_id of_rpi_poe_fan_match[] = {
	{ .compatible = "raspberrypi,rpi-poe-fan", },
	{},
};
MODULE_DEVICE_TABLE(of, of_rpi_poe_fan_match);

static struct platform_driver rpi_poe_fan_driver = {
	.probe		= rpi_poe_fan_probe,
	.remove		= rpi_poe_fan_remove,
	.driver	= {
		.name		= "rpi-poe-fan",
		.pm		= &rpi_poe_fan_pm,
		.of_match_table	= of_rpi_poe_fan_match,
	},
};

module_platform_driver(rpi_poe_fan_driver);

MODULE_AUTHOR("Serge Schneider <serge@raspberrypi.org>");
MODULE_ALIAS("platform:rpi-poe-fan");
MODULE_DESCRIPTION("Raspberry Pi PoE HAT fan driver");
MODULE_LICENSE("GPL");
