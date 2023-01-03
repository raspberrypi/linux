// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2018 Intel Corporation

/*
 * DW9807 is a 10-bit DAC driver, capable of sinking up to 100mA.
 *
 * DW9817 is a bidirectional 10-bit driver, driving up to +/- 100mA.
 * Operationally it is identical to DW9807, except that the idle position is
 * the mid-point, not 0.
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define DW9807_MAX_FOCUS_POS	1023
/*
 * This sets the minimum granularity for the focus positions.
 * A value of 1 gives maximum accuracy for a desired focus position.
 */
#define DW9807_FOCUS_STEPS	1
/*
 * This acts as the minimum granularity of lens movement.
 * Keep this value power of 2, so the control steps can be
 * uniformly adjusted for gradual lens movement, with desired
 * number of control steps.
 */
#define DW9807_CTRL_STEPS	16
#define DW9807_CTRL_DELAY_US	1000

#define DW9807_CTL_ADDR		0x02
/*
 * DW9807 separates two registers to control the VCM position.
 * One for MSB value, another is LSB value.
 */
#define DW9807_MSB_ADDR		0x03
#define DW9807_LSB_ADDR		0x04
#define DW9807_STATUS_ADDR	0x05
#define DW9807_MODE_ADDR	0x06
#define DW9807_RESONANCE_ADDR	0x07

#define MAX_RETRY		10

#define DW9807_PW_MIN_DELAY_US		100
#define DW9807_PW_DELAY_RANGE_US	10

struct dw9807_cfg {
	unsigned int idle_pos;
	unsigned int default_pos;
};

struct dw9807_device {
	struct v4l2_ctrl_handler ctrls_vcm;
	struct v4l2_subdev sd;
	u16 current_val;
	u16 idle_pos;
	struct regulator *vdd;
	struct notifier_block notifier;
	bool first;
};

static inline struct dw9807_device *sd_to_dw9807_vcm(
					struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct dw9807_device, sd);
}

static int dw9807_i2c_check(struct i2c_client *client)
{
	const char status_addr = DW9807_STATUS_ADDR;
	char status_result;
	int ret;

	ret = i2c_master_send(client, &status_addr, sizeof(status_addr));
	if (ret < 0) {
		dev_err(&client->dev, "I2C write STATUS address fail ret = %d\n",
			ret);
		return ret;
	}

	ret = i2c_master_recv(client, &status_result, sizeof(status_result));
	if (ret < 0) {
		dev_err(&client->dev, "I2C read STATUS value fail ret = %d\n",
			ret);
		return ret;
	}

	return status_result;
}

static int dw9807_set_dac(struct i2c_client *client, u16 data)
{
	const char tx_data[3] = {
		DW9807_MSB_ADDR, ((data >> 8) & 0x03), (data & 0xff)
	};
	int val, ret;

	/*
	 * According to the datasheet, need to check the bus status before we
	 * write VCM position. This ensure that we really write the value
	 * into the register
	 */
	ret = readx_poll_timeout(dw9807_i2c_check, client, val, val <= 0,
			DW9807_CTRL_DELAY_US, MAX_RETRY * DW9807_CTRL_DELAY_US);

	if (ret || val < 0) {
		if (ret) {
			dev_warn(&client->dev,
				"Cannot do the write operation because VCM is busy\n");
		}

		return ret ? -EBUSY : val;
	}

	/* Write VCM position to registers */
	ret = i2c_master_send(client, tx_data, sizeof(tx_data));
	if (ret < 0) {
		dev_err(&client->dev,
			"I2C write MSB fail ret=%d\n", ret);

		return ret;
	}

	return 0;
}

/*
 * The lens position is gradually moved in units of DW9807_CTRL_STEPS,
 * to make the movements smoothly. In all cases, even when "start" and
 * "end" are the same, the lens will be set to the "end" position.
 *
 * (We don't use hardware slew rate control, because it differs widely
 * between otherwise-compatible ICs, and may need lens-specific tuning.)
 */
static int dw9807_ramp(struct i2c_client *client, int start, int end)
{
	int step, val, ret;

	if (start < end)
		step = DW9807_CTRL_STEPS;
	else
		step = -DW9807_CTRL_STEPS;

	val = start;
	while (true) {
		val += step;
		if (step * (val - end) >= 0)
			val = end;
		ret = dw9807_set_dac(client, val);
		if (ret)
			dev_err_ratelimited(&client->dev, "%s I2C failure: %d",
					    __func__, ret);
		if (val == end)
			break;
		usleep_range(DW9807_CTRL_DELAY_US, DW9807_CTRL_DELAY_US + 10);
	}

	return ret;
}

static int dw9807_active(struct dw9807_device *dw9807_dev)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dw9807_dev->sd);
	const char tx_data[2] = { DW9807_CTL_ADDR, 0x00 };
	int ret;

	/* Power on */
	ret = i2c_master_send(client, tx_data, sizeof(tx_data));
	if (ret < 0) {
		dev_err(&client->dev, "I2C write CTL fail ret = %d\n", ret);
		return ret;
	}

	dw9807_dev->first = true;

	return dw9807_ramp(client, dw9807_dev->idle_pos, dw9807_dev->current_val);
}

static int dw9807_standby(struct dw9807_device *dw9807_dev)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dw9807_dev->sd);
	const char tx_data[2] = { DW9807_CTL_ADDR, 0x01 };
	int ret;

	if (abs(dw9807_dev->current_val - dw9807_dev->idle_pos) > DW9807_CTRL_STEPS)
		dw9807_ramp(client, dw9807_dev->current_val, dw9807_dev->idle_pos);

	/* Power down */
	ret = i2c_master_send(client, tx_data, sizeof(tx_data));
	if (ret < 0) {
		dev_err(&client->dev, "I2C write CTL fail ret = %d\n", ret);
		return ret;
	}

	return 0;
}

static int dw9807_regulator_event(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	struct dw9807_device *dw9807_dev =
		container_of(nb, struct dw9807_device, notifier);

	if (action & REGULATOR_EVENT_ENABLE) {
		/*
		 * Initialisation delay between VDD low->high and the moment
		 * when the i2c command is available.
		 * From the datasheet, it should be 10ms + 2ms (max power
		 * up sequence duration)
		 */
		usleep_range(DW9807_PW_MIN_DELAY_US,
			     DW9807_PW_MIN_DELAY_US +
			     DW9807_PW_DELAY_RANGE_US);

		dw9807_active(dw9807_dev);
	} else if (action & REGULATOR_EVENT_PRE_DISABLE) {
		dw9807_standby(dw9807_dev);
	}

	return 0;
}

static int dw9807_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct dw9807_device *dev_vcm = container_of(ctrl->handler,
		struct dw9807_device, ctrls_vcm);

	if (ctrl->id == V4L2_CID_FOCUS_ABSOLUTE) {
		struct i2c_client *client = v4l2_get_subdevdata(&dev_vcm->sd);
		int start = (dev_vcm->first) ? dev_vcm->current_val : ctrl->val;

		dev_vcm->first = false;
		dev_vcm->current_val = ctrl->val;
		return dw9807_ramp(client, start, ctrl->val);
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops dw9807_vcm_ctrl_ops = {
	.s_ctrl = dw9807_set_ctrl,
};

static int dw9807_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int dw9807_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops dw9807_int_ops = {
	.open = dw9807_open,
	.close = dw9807_close,
};

static const struct v4l2_subdev_ops dw9807_ops = { };

static void dw9807_subdev_cleanup(struct dw9807_device *dw9807_dev)
{
	v4l2_async_unregister_subdev(&dw9807_dev->sd);
	v4l2_ctrl_handler_free(&dw9807_dev->ctrls_vcm);
	media_entity_cleanup(&dw9807_dev->sd.entity);
}

static int dw9807_init_controls(struct dw9807_device *dev_vcm)
{
	struct v4l2_ctrl_handler *hdl = &dev_vcm->ctrls_vcm;
	const struct v4l2_ctrl_ops *ops = &dw9807_vcm_ctrl_ops;
	struct i2c_client *client = v4l2_get_subdevdata(&dev_vcm->sd);

	v4l2_ctrl_handler_init(hdl, 1);

	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FOCUS_ABSOLUTE,
			  0, DW9807_MAX_FOCUS_POS, DW9807_FOCUS_STEPS,
			  dev_vcm->current_val);

	dev_vcm->sd.ctrl_handler = hdl;
	if (hdl->error) {
		dev_err(&client->dev, "%s fail error: 0x%x\n",
			__func__, hdl->error);
		return hdl->error;
	}

	return 0;
}

/* Compatible devices; in fact there are many similar chips.
 * "data" holds the powered-off (zero current) lens position and a
 * default/initial control value (which need not be the same as the powered-off
 * value).
 */
static const struct dw9807_cfg dw9807_cfg = {
	.idle_pos = 0,
	.default_pos = 0
};

static const struct dw9807_cfg dw9817_cfg = {
	.idle_pos = 512,
	.default_pos = 480,
};

static const struct of_device_id dw9807_of_table[] = {
	{ .compatible = "dongwoon,dw9807-vcm", .data = &dw9807_cfg },
	{ .compatible = "dongwoon,dw9817-vcm", .data = &dw9817_cfg },
	{ /* sentinel */ }
};

static int dw9807_probe(struct i2c_client *client)
{
	struct dw9807_device *dw9807_dev;
	const struct of_device_id *match;
	const struct dw9807_cfg *cfg;
	int rval;

	dw9807_dev = devm_kzalloc(&client->dev, sizeof(*dw9807_dev),
				  GFP_KERNEL);
	if (dw9807_dev == NULL)
		return -ENOMEM;

	dw9807_dev->vdd = devm_regulator_get_optional(&client->dev, "VDD");
	if (IS_ERR(dw9807_dev->vdd)) {
		if (PTR_ERR(dw9807_dev->vdd) != -ENODEV)
			return PTR_ERR(dw9807_dev->vdd);

		dw9807_dev->vdd = NULL;
	} else {
		dw9807_dev->notifier.notifier_call = dw9807_regulator_event;

		rval = regulator_register_notifier(dw9807_dev->vdd,
						   &dw9807_dev->notifier);
		if (rval) {
			dev_err(&client->dev,
				"could not register regulator notifier\n");
			return rval;
		}
	}

	match = i2c_of_match_device(dw9807_of_table, client);
	if (match) {
		cfg = (const struct dw9807_cfg *)match->data;
		dw9807_dev->idle_pos = cfg->idle_pos;
		dw9807_dev->current_val = cfg->default_pos;
	}

	v4l2_i2c_subdev_init(&dw9807_dev->sd, client, &dw9807_ops);
	dw9807_dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dw9807_dev->sd.internal_ops = &dw9807_int_ops;

	rval = dw9807_init_controls(dw9807_dev);
	if (rval)
		goto err_cleanup;

	rval = media_entity_pads_init(&dw9807_dev->sd.entity, 0, NULL);
	if (rval < 0)
		goto err_cleanup;

	dw9807_dev->sd.entity.function = MEDIA_ENT_F_LENS;

	rval = v4l2_async_register_subdev(&dw9807_dev->sd);
	if (rval < 0)
		goto err_cleanup;

	if (!dw9807_dev->vdd)
		pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	return 0;

err_cleanup:
	v4l2_ctrl_handler_free(&dw9807_dev->ctrls_vcm);
	media_entity_cleanup(&dw9807_dev->sd.entity);

	return rval;
}

static void dw9807_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9807_device *dw9807_dev = sd_to_dw9807_vcm(sd);

	if (dw9807_dev->vdd)
		regulator_unregister_notifier(dw9807_dev->vdd,
					      &dw9807_dev->notifier);

	pm_runtime_disable(&client->dev);

	dw9807_subdev_cleanup(dw9807_dev);
}

/*
 * This function sets the vcm position, so it consumes least current
 * The lens position is gradually moved in units of DW9807_CTRL_STEPS,
 * to make the movements smoothly.
 */
static int __maybe_unused dw9807_vcm_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9807_device *dw9807_dev = sd_to_dw9807_vcm(sd);

	if (dw9807_dev->vdd)
		return regulator_disable(dw9807_dev->vdd);

	return dw9807_standby(dw9807_dev);
}

/*
 * This function sets the vcm position to the value set by the user
 * through v4l2_ctrl_ops s_ctrl handler
 * The lens position is gradually moved in units of DW9807_CTRL_STEPS,
 * to make the movements smoothly.
 */
static int  __maybe_unused dw9807_vcm_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9807_device *dw9807_dev = sd_to_dw9807_vcm(sd);

	if (dw9807_dev->vdd)
		return regulator_enable(dw9807_dev->vdd);

	return dw9807_active(dw9807_dev);
}

MODULE_DEVICE_TABLE(of, dw9807_of_table);

static const struct dev_pm_ops dw9807_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dw9807_vcm_suspend, dw9807_vcm_resume)
	SET_RUNTIME_PM_OPS(dw9807_vcm_suspend, dw9807_vcm_resume, NULL)
};

static struct i2c_driver dw9807_i2c_driver = {
	.driver = {
		.name = "dw9807",
		.pm = &dw9807_pm_ops,
		.of_match_table = dw9807_of_table,
	},
	.probe = dw9807_probe,
	.remove = dw9807_remove,
};

module_i2c_driver(dw9807_i2c_driver);

MODULE_AUTHOR("Chiang, Alan");
MODULE_DESCRIPTION("DW9807 VCM driver");
MODULE_LICENSE("GPL v2");
