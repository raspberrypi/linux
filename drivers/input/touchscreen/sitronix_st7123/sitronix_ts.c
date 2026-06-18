// SPDX-License-Identifier: GPL-2.0
/*
 * Sitronix Touchscreen Controller Driver
 *
 * Copyright (C) 2018 Sitronix Technology Co., Ltd.
 *	CT Chen <ct_chen@sitronix.com.tw>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "sitronix_ts.h"
#include "sitronix_st7123.h"

struct sitronix_ts_data *gts;
//unsigned char wbuf[SITRONIX_RW_BUF_LEN+8] = {0};
//unsigned char rbuf[SITRONIX_RW_BUF_LEN+8] = {0};
unsigned char *wbuf;
unsigned char *rbuf;

#define POLL_INTERVAL_MS 17 /* 17ms = 60fps */
/* #define SITRONIX_TS_INPUT_PHYS_NAME "sitronix_ts/input0" */

#if defined(CONFIG_FB)
static void sitronix_ts_resume_work(struct work_struct *work);
static int sitronix_ts_suspend(struct device *dev);
static int sitronix_ts_resume(struct device *dev);
#ifdef _MSM_DRM_NOTIFY_H_
static int sitronix_drm_notifier_callback(struct notifier_block *self,
					  unsigned long event, void *data);
#else
static int sitronix_fb_notifier_callback(struct notifier_block *self,
					 unsigned long event, void *data);
#endif
#elif defined(CONFIG_DRM)
static void sitronix_ts_resume_work(struct work_struct *work);
static int sitronix_ts_suspend(struct device *dev);
static int sitronix_ts_resume(struct device *dev);
#if defined(CONFIG_DRM_PANEL)
static int sitronix_drm_notifier_callback(struct notifier_block *self,
					  unsigned long event, void *data);
#endif /* CONFIG_DRM_PANEL */
#elif defined(CONFIG_HAS_EARLYSUSPEND)
static void sitronix_ts_early_suspend(struct early_suspend *h);
static void sitronix_ts_late_resume(struct early_suspend *h);
#endif

static inline void sitronix_ts_pen_down(struct sitronix_ts_data *ts_data,
					int id, uint16_t x, uint16_t y)
{
#ifdef SITRONIX_TS_MT_SLOT
	input_mt_slot(ts_data->input_dev, id);
	input_mt_report_slot_state(ts_data->input_dev, MT_TOOL_FINGER, true);
	// input_report_abs(ts_data->input_dev, ABS_MT_POSITION_X, x);
	// input_report_abs(ts_data->input_dev, ABS_MT_POSITION_Y, y);
	touchscreen_report_pos(ts_data->input_dev, &ts_data->prop, x, y, true);
	input_report_abs(ts_data->input_dev, ABS_MT_TOUCH_MAJOR, 1);
	input_report_abs(ts_data->input_dev, ABS_MT_PRESSURE, 255);
	//input_report_abs(ts_data->input_dev, ABS_MT_TRACKING_ID, id);
#else /* SITRONIX_TS_MT_SLOT */
	input_report_abs(ts_data->input_dev, ABS_MT_TRACKING_ID, id);
	// input_report_abs(ts_data->input_dev, ABS_MT_POSITION_X, x);
	// input_report_abs(ts_data->input_dev, ABS_MT_POSITION_Y, y);
	touchscreen_report_pos(ts_data->input_dev, &ts_data->prop, x, y, true);
	input_report_abs(ts_data->input_dev, ABS_MT_TOUCH_MAJOR, 10);
	input_report_abs(ts_data->input_dev, ABS_MT_TOUCH_MINOR, 10);
	input_report_abs(ts_data->input_dev, ABS_MT_PRESSURE, 10);
	input_mt_sync(ts_data->input_dev);
#endif /* SITRONIX_TS_MT_SLOT */
}

static inline void sitronix_ts_pen_up(struct input_dev *input_dev, int id)
{
#ifdef SITRONIX_TS_MT_SLOT
	input_mt_slot(input_dev, id);
	input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, false);
	input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR, 0);
	input_report_abs(input_dev, ABS_MT_PRESSURE, 0);
#endif /* SITRONIX_TS_MT_SLOT */
}

static inline void sitronix_ts_pen_allup(struct sitronix_ts_data *ts_data)
{
	int i;

	for (i = 0; i < ts_data->ts_dev_info.max_touches; i++)
		sitronix_ts_pen_up(ts_data->input_dev, i);

	input_report_key(ts_data->input_dev, BTN_TOUCH, 0);
	input_sync(ts_data->input_dev);
}

static void sitronix_process_events(struct sitronix_ts_data *ts_data)
{
	int i;
	int ret;
	uint16_t x, y;
	uint8_t z;
	uint8_t read_len = (ts_data->ts_dev_info.max_touches * 7) + 5;
	uint8_t touch_count = 0;
	uint8_t *p;
	uint16_t coordCkhsum = 0x5A; //since v01.12.01
	uint16_t recvCkhsum = 0x00;

	if (ts_data->irq_wait_mode == STDEV_IRQ_WAITMODE_RAW) {
		ts_data->irq_wait_flag = 1;
		wake_up_interruptible(&ts_data->irq_wait_queue);
		// return IRQ_HANDLED;
		return;
	}

	sitronix_mt_pause_one();

	mutex_lock(&ts_data->mutex);

	ret = sitronix_ts_reg_read(ts_data, TOUCH_INFO, ts_data->coord_buf,
				   read_len);
	if (ret < 0) {
		sterr("%s: Read finger touch error!(%d)\n", __func__, ret);
		goto exit_invalid_data;
	}

	if (ts_data->coord_buf[0] & 0x80) {
		/* ESD check fail */
		sterr("Firmware RstChip = 1 , reset device\n");
#ifdef SITRONIX_HDL_IN_IRQ
		sitronix_ts_pen_allup(ts_data);
		sitronix_ts_reset_device(ts_data);
		ts_data->is_reset_chip = true;
		goto exit_invalid_data;
#else
		sitronix_ts_pen_allup(ts_data);
		sitronix_ts_reset_device(ts_data);
#endif
	}

	if (ts_data->exdiff_flag) { //enable ex-diff data
		sitronix_ts_get_exdiff(ts_data, ts_data->exdiff_buf);
	}

	//if((ts_data->coord_buf[0] & 0x0A) == 0x0A){
	if (ts_data->is_support_coord_chksum) {
		/* support coord checksum*/
		STChecksumCalculation(&coordCkhsum, ts_data->coord_buf,
				      4); //part 1
		if (ts_data->coord_buf[0] & 0x08) {
			//with Coord
			STChecksumCalculation(
				&coordCkhsum, ts_data->coord_buf + 4,
				(ts_data->ts_dev_info.max_touches *
				 7)); //part 2
		}
		//coordCkhsum = coordCkhsum & 0xFF;
		recvCkhsum = ts_data->coord_buf[read_len - 1];

		coordCkhsum = coordCkhsum & 0xFF;

		if (coordCkhsum != recvCkhsum) {
			sterr("coord checksum error: expect = 0x%02X, received = 0x%02X\n",
			      coordCkhsum, recvCkhsum);
			goto exit_invalid_data;
		}
	}

	if (!ts_data->in_suspend) {
		touch_count = 0;
		p = &ts_data->coord_buf[4];
		for (i = 0; i < ts_data->ts_dev_info.max_touches; i++) {
			if (*p & 0x80) {
				touch_count++;
				x = (uint16_t)(((uint16_t)(*p & 0x3F) << 8) |
					       ((uint16_t)*(p + 1) & 0xFF));
				y = (uint16_t)(((uint16_t)(*(p + 2) & 0x3F)
						<< 8) |
					       ((uint16_t)*(p + 3) & 0xFF));
				z = *(p + 4);

				sitronix_ts_pen_down(ts_data, i, x, y);
			} else {
#ifdef SITRONIX_TS_MT_SLOT
				sitronix_ts_pen_up(ts_data->input_dev, i);
#endif /* SITRONIX_TS_MT_SLOT */
			}
			p += 7;
		}
#ifndef SITRONIX_TS_MT_SLOT
		if (touch_count == 0)
			sitronix_ts_pen_up(ts_data->input_dev, 0);
#endif /* SITRONIX_TS_MT_SLOT */
		input_report_key(ts_data->input_dev, BTN_TOUCH,
				 (touch_count > 0));
		input_sync(ts_data->input_dev);
	}

exit_invalid_data:

	if (ts_data->irq_wait_mode == STDEV_IRQ_WAITMODE_COORD) {
		ts_data->irq_wait_flag = 1;
		wake_up_interruptible(&ts_data->irq_wait_queue);
	}
	mutex_unlock(&ts_data->mutex);
}

static irqreturn_t sitronix_ts_irq_handler(int irq, void *data)
{
	struct sitronix_ts_data *ts_data = (struct sitronix_ts_data *)data;

	sitronix_process_events(ts_data);
	return IRQ_HANDLED;
}

/* Timer poll function */
static void sitronix_ts_irq_poll_timer(struct timer_list *t)
{
	struct sitronix_ts_data *ts_data =
		container_of(t, struct sitronix_ts_data, timer);

	schedule_work(&ts_data->work_i2c_poll); /* schedule work queue */
	mod_timer(&ts_data->timer,
		  jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
}

static void sitronix_ts_work_i2c_poll(struct work_struct *work)
{
	struct sitronix_ts_data *ts_data =
		container_of(work, struct sitronix_ts_data, work_i2c_poll);

	sitronix_process_events(ts_data);
}

void sitronix_enable_irq(struct sitronix_ts_data *ts_data)
{
	if (ts_data->irq) {
		dev_dbg(&ts_data->pdev->dev, "%s - 0\n", __func__);
		enable_irq(ts_data->irq);
	} else {
		dev_dbg(&ts_data->pdev->dev, "%s - 1\n", __func__);
		ts_data->timer.expires =
			jiffies + msecs_to_jiffies(POLL_INTERVAL_MS);
		add_timer(&ts_data->timer);
	}
}

void sitronix_disable_irq(struct sitronix_ts_data *ts_data)
{
	if (ts_data->irq) {
		dev_dbg(&ts_data->pdev->dev, "%s - 0\n", __func__);
		disable_irq(ts_data->irq);
	} else {
		dev_dbg(&ts_data->pdev->dev, "%s - 1\n", __func__);
		timer_delete(&ts_data->timer);
		cancel_work_sync(&ts_data->work_i2c_poll);
	}
}

static void sitronix_free_irq(struct sitronix_ts_data *ts_data)
{
	if (ts_data->irq) {
		devm_free_irq(&ts_data->pdev->dev, ts_data->irq, ts_data);
	} else {
		timer_delete(&ts_data->timer);
		cancel_work_sync(&ts_data->work_i2c_poll);
	}
}

static int sitronix_request_irq(struct sitronix_ts_data *ts_data)
{
	if (ts_data->irq) {
		dev_dbg(&ts_data->pdev->dev, "%s - 0\n", __func__);
		dev_dbg(&ts_data->pdev->dev, "%s - irq = %d\n", __func__,
			ts_data->irq);
		return devm_request_threaded_irq(&ts_data->pdev->dev,
						 ts_data->irq, NULL,
						 sitronix_ts_irq_handler,
						 ts_data->irq_flags,
						 ts_data->name, ts_data);
	} else { /* poll when no IRQ */
		dev_dbg(&ts_data->pdev->dev, "%s - 1\n", __func__);
		INIT_WORK(&ts_data->work_i2c_poll,
			  sitronix_ts_work_i2c_poll); /* init work queue */
		timer_setup(&ts_data->timer, sitronix_ts_irq_poll_timer,
			    0); /* init timer */
		sitronix_enable_irq(ts_data);
		return 0;
	}
}

int sitronix_ts_reset_device(struct sitronix_ts_data *ts_data)
{
	return 0;
}

static void sitronix_ts_input_set_params(struct sitronix_ts_data *ts_data)
{
	struct sitronix_ts_device_info *ts_dev_info = &ts_data->ts_dev_info;

#ifdef SITRONIX_TS_MT_SLOT
	input_mt_init_slots(ts_data->input_dev, ts_dev_info->max_touches,
			    INPUT_MT_DIRECT);
	set_bit(INPUT_PROP_DIRECT, ts_data->input_dev->propbit);
	input_set_abs_params(ts_data->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0,
			     0);
	input_set_abs_params(ts_data->input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(ts_data->input_dev, ABS_MT_TRACKING_ID, 0,
			     ts_dev_info->max_touches, 0, 0);
#else /* SITRONIX_TS_MT_SLOT */
	set_bit(ABS_X, ts_data->input_dev->absbit);
	set_bit(ABS_Y, ts_data->input_dev->absbit);
	set_bit(ABS_MT_TOUCH_MAJOR, ts_data->input_dev->absbit);
	set_bit(ABS_MT_TOUCH_MINOR, ts_data->input_dev->absbit);
	set_bit(ABS_MT_POSITION_X, ts_data->input_dev->absbit);
	set_bit(ABS_MT_POSITION_Y, ts_data->input_dev->absbit);
	set_bit(ABS_MT_TOOL_TYPE, ts_data->input_dev->absbit);
	set_bit(ABS_MT_BLOB_ID, ts_data->input_dev->absbit);
	set_bit(ABS_MT_TRACKING_ID, ts_data->input_dev->absbit);
	set_bit(INPUT_PROP_DIRECT, ts_data->input_dev->propbit);
	input_set_abs_params(ts_data->input_dev, ABS_MT_TRACKING_ID, 0,
			     ts_dev_info->max_touches, 0, 0);
	input_set_abs_params(ts_data->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0,
			     0);
	input_set_abs_params(ts_data->input_dev, ABS_MT_TOUCH_MINOR, 0, 255, 0,
			     0);
	input_set_abs_params(ts_data->input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);
#endif /* SITRONIX_TS_MT_SLOT */
	input_set_abs_params(ts_data->input_dev, ABS_MT_POSITION_X, 0,
			     (ts_dev_info->x_res - 1), 0, 0);
	input_set_abs_params(ts_data->input_dev, ABS_MT_POSITION_Y, 0,
			     (ts_dev_info->y_res - 1), 0, 0);
}

static int sitronix_ts_input_dev_init(struct sitronix_ts_data *ts_data)
{
	int ret = 0;

	ts_data->input_dev = input_allocate_device();
	if (ts_data->input_dev == NULL) {
		sterr("%s: Can not allocate input device!\n", __func__);
		return -ENOMEM;
	}

	ts_data->input_dev->name = ts_data->name;
	/* ts_data->input_dev->phys = SITRONIX_TS_INPUT_PHYS_NAME; */
	ts_data->input_dev->id.bustype = ts_data->host_if->bus_type;
	ts_data->input_dev->id.product = 0;
	ts_data->input_dev->id.version = 0;
	ts_data->input_dev->dev.parent = ts_data->pdev->dev.parent;
	input_set_drvdata(ts_data->input_dev, ts_data);

	set_bit(EV_SYN, ts_data->input_dev->evbit);
	set_bit(EV_KEY, ts_data->input_dev->evbit);
	set_bit(EV_ABS, ts_data->input_dev->evbit);
	set_bit(BTN_TOUCH, ts_data->input_dev->keybit);
	/* set_bit(KEY_POWER, ts_data->input_dev->keybit); */
	/* set_bit(KEY_MENU, ts_data->input_dev->keybit); */
	sitronix_ts_input_set_params(ts_data);
	touchscreen_parse_properties(ts_data->input_dev, true, &ts_data->prop);

	ret = input_register_device(ts_data->input_dev);
	if (ret) {
		sterr("%s: Failed to register input device\n", __func__);
		goto err_register_input;
	}

	return 0;

err_register_input:
	input_free_device(ts_data->input_dev);
	ts_data->input_dev = NULL;

	return ret;
}

static bool sitronix_ts_check_ic_sfrver(void)
{
	int ret = 0;

	ret = sitronix_get_chip_id();
	gts->ts_dev_info.chip_id = ret;
	stmsg("IC Chip ID = 0x%X\n", ret);

	ret = sitronix_get_ic_sfrver();
	gts->ts_dev_info.chip_ver = ret;
	stmsg("IC SFR VER = 0x%X\n", ret);

	return true;
}

/**
 * sitronix_ts_probe()
 *
 */
static int sitronix_ts_probe(struct platform_device *pdev)
{
	int ret;
	struct sitronix_ts_data *ts_data = NULL;
	const struct sitronix_ts_host_interface *host_if;
	int irq;
	int status;

	wbuf = NULL;
	rbuf = NULL;

	stmsg("%s:%u\n", __func__, __LINE__);

	ST_START_PRINT;
	host_if = pdev->dev.platform_data;
	if (!host_if) {
		sterr("%s: No hardware interface found!\n", __func__);
		return -EINVAL;
	}

	ts_data = kzalloc(sizeof(*ts_data), GFP_KERNEL);
	if (!ts_data) {
		sterr("%s: Alloc memory for ts_data failed!\n", __func__);
		return -ENOMEM;
	}

	mutex_init(&ts_data->mutex);

	ts_data->skip_first_resume = true;
	ts_data->name = pdev->name;
	ts_data->pdev = pdev;
	ts_data->host_if = host_if;
	ts_data->in_suspend = false;
	ts_data->wr_len = STDEV_WR_DF;
	ts_data->fw_request_status = 0; //not ready
	ts_data->exdiff_flag = false;
#if defined(CONFIG_FB) || defined(CONFIG_DRM)
	ts_data->workqueue = NULL;
#endif //defined(CONFIG_FB) || defined(CONFIG_DRM)
	ts_data->sitronix_kobj = NULL;
	gts = ts_data;

	platform_set_drvdata(pdev, ts_data);

	wbuf = kzalloc((SITRONIX_RW_BUF_LEN + 32), GFP_KERNEL);
	if (!wbuf) {
		sterr("%s: Alloc memory for wbuf failed!\n", __func__);
		ret = -ENOMEM;
		goto err_return;
	}
	rbuf = kzalloc((SITRONIX_RW_BUF_LEN + 32), GFP_KERNEL);
	if (!rbuf) {
		sterr("%s: Alloc memory for rbuf failed!\n", __func__);
		ret = -ENOMEM;
		goto err_return;
	}

	sitronix_ts_reset_device(ts_data);
	if (!sitronix_ts_check_ic_sfrver()) {
		sterr("%s: Failed to sitronix_ts_check_ic_sfrver\n", __func__);
		ret = -EINVAL;
		goto err_return;
	}
	sitronix_replace_dump_buf(NULL);

#ifdef SITRONIX_HDL_IN_PROBE
#ifndef ST_SKIP_HDL_IN_PROBE
	ret = sitronix_do_upgrade();
	if (ret < 0) {
		sterr("%s: Failed to Host Download!\n", __func__);
		ret = -EINVAL;
		goto err_return;
	}
#endif //ST_SKIP_HDL_IN_PROBE
#else //SITRONIX_HDL_IN_PROBE
	sitronix_ts_reset_device(ts_data);
#endif //SITRONIX_HDL_IN_PROBE

#ifndef ST_SKIP_HDL_IN_PROBE
	status = sitronix_ts_get_device_status(ts_data);
	ret = sitronix_ts_get_device_info(ts_data); /* get TP device info */
	if (ret) {
		sterr("%s: Failed to get device info.\n", __func__);
#ifndef SITRONIX_TP_WITH_FLASH
		ret = -EINVAL;
		goto err_return;
#endif //SITRONIX_TP_WITH_FLASH
	}
#else //ST_SKIP_HDL_IN_PROBE
	status = 0;
	ts_data->skip_first_resume = false;
	//set default information
	ts_data->ts_dev_info.x_res = ST_DEFAULT_RES_X;
	ts_data->ts_dev_info.y_res = ST_DEFAULT_RES_Y;
	ts_data->ts_dev_info.max_touches = ST_DEFAULT_MAX_TOUCH;
#endif //ST_SKIP_HDL_IN_PROBE

#ifdef SITRONIX_FLASH_UPGRADE_IN_PROBE
	ret = sitronix_do_upgrade();
	if (ret < 0) {
		sterr("%s: Failed to Upgrade Flash!\n", __func__);
		ret = -EINVAL;
		goto err_return;
	}
#endif //SITRONIX_FLASH_UPGRADE_IN_PROBE

	ret = sitronix_ts_input_dev_init(ts_data);
	if (ret) {
		sterr("%s: Failed to set up input device\n", __func__);
		ret = -EINVAL;
		goto err_return;
	}

	irq = gpio_to_irq(host_if->irq_gpio);
	if (irq > 0)
		ts_data->irq = irq;
	sitronix_request_irq(ts_data);

#if defined(CONFIG_FB)
	ts_data->workqueue =
		create_singlethread_workqueue("sitronix_ts_workqueue");
	if (!ts_data->workqueue) {
		sterr("create sitronix_ts_workqueue fail");
		ret = -ENOMEM;
		goto err_return;
	}
	INIT_WORK(&ts_data->resume_work, sitronix_ts_resume_work);
#ifdef _MSM_DRM_NOTIFY_H_
	ts_data->drm_notif.notifier_call = sitronix_drm_notifier_callback;
	ret = msm_drm_register_client(&ts_data->drm_notif);
	if (ret) {
		sterr("register drm_notifier failed. ret=%d\n", ret);
		ret = -EINVAL;
		goto err_return;
	}
#else
	ts_data->fb_notif.notifier_call = sitronix_fb_notifier_callback;
	ret = fb_register_client(&ts_data->fb_notif);
	if (ret) {
		sterr("register fb_notifier failed. ret=%d\n", ret);
		ret = -EINVAL;
		goto err_return;
	}
#endif
#elif defined(CONFIG_DRM)
	ts_data->workqueue =
		create_singlethread_workqueue("sitronix_ts_workqueue");
	if (!ts_data->workqueue) {
		sterr("create sitronix_ts_workqueue fail");
		ret = -ENOMEM;
		goto err_return;
	}
	INIT_WORK(&ts_data->resume_work, sitronix_ts_resume_work);

	ts_data->drm_notif.notifier_call = sitronix_drm_notifier_callback;
#if defined(CONFIG_DRM_PANEL)
	if (ts_data->active_panel) {
		ret = drm_panel_notifier_register(ts_data->active_panel,
						  &ts_data->drm_notif);
		if (ret)
			sterr("[DRM]drm_panel_notifier_register fail: %d\n",
			      ret);
	}
#else
	ret = msm_drm_register_client(&ts_data->drm_notif);
	if (ret) {
		sterr("register drm_notifier failed. ret=%d\n", ret);
		ret = -EINVAL;
		goto err_return;
	}
#endif /* CONFIG_DRM_PANEL */
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ts->early_suspend.suspend = sitronix_ts_early_suspend;
	ts->early_suspend.resume = sitronix_ts_late_resume;
	ret = register_early_suspend(&ts_data->early_suspend);
	if (ret) {
		sterr("register early suspend failed. ret=%d\n", ret);
		ret = -EINVAL;
		goto err_return;
	}
#endif

	sitronix_mode_backup(); //Backup mode from device.
	sitronix_swk_config_backup(); //Backup SWK config from device.

#ifdef SITRONIX_MONITOR_THREAD
	ts_data->enable_monitor_thread = true;
	ts_data->sitronix_mt_fp = sitronix_ts_monitor_thread_v3;
	sitronix_mt_start(DELAY_MONITOR_THREAD_START_PROBE);
#endif /* SITRONIX_MONITOR_THREAD */

	//return ret;
	return 0;

err_return:

#if defined(CONFIG_FB)
	if (ts_data->workqueue)
		destroy_workqueue(ts_data->workqueue);
#ifdef _MSM_DRM_NOTIFY_H_
	if (msm_drm_unregister_client(&ts->drm_notif))
		sterr("Error occurred while unregistering drm_notifier.\n");
#else
	if (fb_unregister_client(&ts_data->fb_notif))
		sterr("Error occurred while unregistering fb_notifier.\n");
#endif
#elif defined(CONFIG_DRM)
	if (ts_data->workqueue)
		destroy_workqueue(ts_data->workqueue);
#if defined(CONFIG_DRM_PANEL)
	if (ts_data->active_panel)
		drm_panel_notifier_unregister(ts_data->active_panel,
					      &ts_data->drm_notif);
#endif /* CONFIG_DRM_PANEL */
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&ts_data->early_suspend);
#endif

#ifdef SITRONIX_MONITOR_THREAD
	sitronix_mt_stop();
#endif /* SITRONIX_MONITOR_THREAD */

	if (ts_data->irq > 0)
		free_irq(ts_data->irq, ts_data);

	if (ts_data->input_dev) {
		input_unregister_device(ts_data->input_dev);
		// input_free_device(ts_data->input_dev);
		ts_data->input_dev = NULL;
	}

	kfree(rbuf);
	rbuf = NULL;

	kfree(wbuf);
	wbuf = NULL;

	mutex_destroy(&ts_data->mutex);

	if (ts_data) {
		platform_set_drvdata(pdev, NULL);
		kfree(ts_data);
		gts = NULL;
	}

	return ret;
}

static void sitronix_ts_remove(struct platform_device *pdev)
{
	//struct sitronix_ts_data *ts_data = platform_get_drvdata(pdev);
	struct sitronix_ts_data *ts_data = NULL;

	stmsg("%s:%u\n", __func__, __LINE__);

	ts_data = platform_get_drvdata(pdev);

#ifdef SITRONIX_MONITOR_THREAD
	if (ts_data)
		sitronix_mt_stop();
#endif /* SITRONIX_MONITOR_THREAD */

#if defined(CONFIG_FB)
	if (ts_data && ts_data->workqueue)
		destroy_workqueue(ts_data->workqueue);
#ifdef _MSM_DRM_NOTIFY_H_
	if (ts_data && msm_drm_unregister_client(&ts_data->drm_notif))
		sterr("Error occurred while unregistering drm_notifier.\n");
#else
	if (ts_data && fb_unregister_client(&ts_data->fb_notif))
		sterr("Error occurred while unregistering fb_notifier.\n");
#endif
#elif defined(CONFIG_DRM)
	if (ts_data && ts_data->workqueue)
		destroy_workqueue(ts_data->workqueue);
#if defined(CONFIG_DRM_PANEL)
	if (ts_data && ts_data->active_panel)
		drm_panel_notifier_unregister(ts_data->active_panel,
					      &ts_data->drm_notif);
#endif /* CONFIG_DRM_PANEL */
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	if (ts_data)
		unregister_early_suspend(&ts_data->early_suspend);
#endif

	if (ts_data)
		sitronix_free_irq(ts_data);

	if (ts_data && ts_data->input_dev) {
		input_unregister_device(ts_data->input_dev);
		// input_free_device(ts_data->input_dev);
	}

	if (ts_data)
		mutex_destroy(&ts_data->mutex);

	kfree(rbuf);
	rbuf = NULL;

	kfree(wbuf);
	wbuf = NULL;

	kfree(ts_data);
}

#if (defined(CONFIG_FB) || defined(CONFIG_DRM))
static int sitronix_ts_suspend(struct device *dev)
{
	/* struct sitronix_ts_data *ts_data = dev_get_drvdata(dev); */

	/* stdbg("%s:%u, ts_data=0x%x\n", __FUNCTION__, __LINE__, (uint32_t)ts_data); */
	stmsg("run %s\n", __func__);

	gts->skip_first_resume = false;
	mutex_lock(&gts->mutex);
	/*
	 * if (gts->in_suspend) {
	 *     stdbg("%s:%u, Already in suspend!\n", __func__, __LINE__);
	 *     mutex_unlock(&gts->mutex);
	 *     return 0;
	 * }
	 */

#ifdef SITRONIX_MONITOR_THREAD
#ifdef MONITOR_THREAD_STOP_IN_SUSPEND
	sitronix_mt_stop();
#endif /* MONITOR_THREAD_STOP_IN_SUSPEND */
#endif /* SITRONIX_MONITOR_THREAD */

	// sitronix_ts_irq_enable(gts, false);
	sitronix_disable_irq(gts);

	msleep(20);
	sitronix_ts_powerdown(gts, true);
	usleep_range(10000, 11000);

	sitronix_ts_pen_allup(gts);

	gts->in_suspend = true;
	mutex_unlock(&gts->mutex);
	return 0;
}

static int sitronix_ts_resume(struct device *dev)
{
	/* struct sitronix_ts_data *ts_data = dev_get_drvdata(dev); */
	/* stdbg("%s:%u, ts_data=0x%x\n", __FUNCTION__, __LINE__, (uint32_t)ts_data); */
	stmsg("run %s\n", __func__);

	if (gts->skip_first_resume) {
		gts->skip_first_resume = false;
		return 0;
	}
#ifdef SITRONIX_TP_RESUME_BEFORE_DISPON
	msleep(SITRONIX_RESUME_DELAY);
#endif /* SITRONIX_TP_RESUME_BEFORE_DISPON */
	mutex_lock(&gts->mutex);
#ifdef SITRONIX_TP_WITH_FLASH
	sitronix_ts_reset_device(gts);
#endif

#ifdef SITRONIX_HDL_IN_RESUME
	sitronix_do_upgrade();
#endif /* SITRONIX_HDL_IN_RESUME */

	sitronix_ts_powerdown(gts, false);
	/* kthread_run(sitronix_ts_powerdown_thread, gts, "Sitronix powerdown Thread"); */
	/* msleep(200); */
	/* sitronix_ts_reset_device(gts); */

	// sitronix_ts_irq_enable(gts, true);
	sitronix_enable_irq(gts);

	gts->in_suspend = false;

	sitronix_mode_restore();
	sitronix_swk_config_restore();
	mutex_unlock(&gts->mutex);

#ifdef SITRONIX_MONITOR_THREAD
#ifdef MONITOR_THREAD_STOP_IN_SUSPEND
	sitronix_mt_start(DELAY_MONITOR_THREAD_START_RESUME);
#else

#endif /* MONITOR_THREAD_STOP_IN_SUSPEND */
#endif /* SITRONIX_MONITOR_THREAD */
	return 0;
}

static void sitronix_ts_resume_work(struct work_struct *work)
{
	sitronix_ts_resume(&gts->pdev->dev);
}
#endif

#if defined(CONFIG_FB)
#ifdef _MSM_DRM_NOTIFY_H_
static int sitronix_drm_notifier_callback(struct notifier_block *self,
					  unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank;
	struct ts_sitronix *ts =
		container_of(self, struct ts_sitronix, drm_notif);

	if (!evdata || (evdata->id != 0))
		return 0;

	if (evdata->data && ts) {
		blank = evdata->data;
#ifdef SITRONIX_TP_RESUME_BEFORE_DISPON
		if (event == MSM_DRM_EARLY_EVENT_BLANK) {
			if (*blank == MSM_DRM_BLANK_UNBLANK) {
				//stdbg("event=%lu, *blank=%d\n", event, *blank);
				queue_work(ts->workqueue, &ts->resume_work);
				//sitronix_ts_resume(&ts->pdev->dev);
			}
		}
#endif /* SITRONIX_TP_RESUME_BEFORE_DISPON */
		if (event == MSM_DRM_EVENT_BLANK) {
			if (*blank == MSM_DRM_BLANK_POWERDOWN) {
				//stdbg("event=%lu, *blank=%d\n", event, *blank);
				cancel_work_sync(&ts->resume_work);
				sitronix_ts_suspend(&ts->pdev->dev);
			}
#ifndef SITRONIX_TP_RESUME_BEFORE_DISPON
			if (*blank == MSM_DRM_BLANK_UNBLANK) {
				//stdbg("event=%lu, *blank=%d\n", event, *blank);
				queue_work(ts->workqueue, &ts->resume_work);
			}
#endif /* SITRONIX_TP_RESUME_BEFORE_DISPON */
		}
	}

	return 0;
}
#else
static int sitronix_fb_notifier_callback(struct notifier_block *self,
					 unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct sitronix_ts_data *ts =
		container_of(self, struct sitronix_ts_data, fb_notif);
	//blank = evdata->data;
	//stmsg("event = 0x%02X ,blank = 0x%02X\n",(int)event , *blank);
	//stmsg("event = 0x%02X\n",(int)event );
#ifdef SITRONIX_TP_RESUME_BEFORE_DISPON
	if (evdata && evdata->data && event == FB_EARLY_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK) {
			/* stmsg("event=%lx, *blank=%d\n", event, *blank); */
			queue_work(ts->workqueue, &ts->resume_work);
			//sitronix_ts_resume(&ts->pdev->dev);
		}
	}
#endif /* SITRONIX_TP_RESUME_BEFORE_DISPON */

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_POWERDOWN) {
			/* stmsg("event=%lx, *blank=%d\n", event, *blank); */
			cancel_work_sync(&ts->resume_work);
			sitronix_ts_suspend(&ts->pdev->dev);
		}
#ifndef SITRONIX_TP_RESUME_BEFORE_DISPON
		if (*blank == FB_BLANK_UNBLANK) {
			/* stmsg("event=%lu, *blank=%d\n", event, *blank); */
			/* sitronix_ts_resume(&ts->pdev->dev); */
			queue_work(ts->workqueue, &ts->resume_work);
		}
#endif /* SITRONIX_TP_RESUME_BEFORE_DISPON */
	}
	return 0;
}
#endif

#elif defined(CONFIG_DRM)
static void sitronix_ts_resume_work(struct work_struct *work)
{
	sitronix_ts_resume(&gts->pdev->dev);
}
#if defined(CONFIG_DRM_PANEL)
static int sitronix_drm_notifier_callback(struct notifier_block *self,
					  unsigned long event, void *data)
{
	struct drm_panel_notifier *evdata = data;
	int *blank;
	struct sitronix_ts_data *ts =
		container_of(self, struct sitronix_ts_data, drm_notif);
	/* blank = evdata->data; */
	/* stmsg("event = 0x%02X ,blank = 0x%02X\n",(int)event , *blank); */
	/* stmsg("event = 0x%02X\n",(int)event ); */
#ifdef SITRONIX_TP_RESUME_BEFORE_DISPON
	if (evdata && evdata->data && event == DRM_PANEL_EARLY_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == DRM_PANEL_BLANK_UNBLANK) {
			stmsg("event=%lx, *blank=%d\n", event, *blank);
			queue_work(ts->workqueue, &ts->resume_work);
			//sitronix_ts_resume(&ts->pdev->dev);
		}
	}
#endif /* SITRONIX_TP_RESUME_BEFORE_DISPON */

	if (evdata && evdata->data && event == DRM_PANEL_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == DRM_PANEL_BLANK_POWERDOWN) {
			stmsg("event=%lx, *blank=%d\n", event, *blank);
			cancel_work_sync(&ts->resume_work);
			sitronix_ts_suspend(&ts->pdev->dev);
		}
#ifndef SITRONIX_TP_RESUME_BEFORE_DISPON
		if (*blank == DRM_PANEL_BLANK_UNBLANK) {
			stmsg("event=%lu, *blank=%d\n", event, *blank);
			/* sitronix_ts_resume(&ts->pdev->dev); */
			queue_work(ts->workqueue, &ts->resume_work);
		}
#endif /* SITRONIX_TP_RESUME_BEFORE_DISPON */
	}
	return 0;
}
#endif /* CONFIG_DRM_PANEL */
#elif defined(CONFIG_HAS_EARLYSUSPEND)
static void sitronix_ts_early_suspend(struct early_suspend *h)
{
	struct sitronix_ts_data *ts_data =
		container_of(h, struct sitronix_ts_data, early_suspend);

	sitronix_ts_suspend(&ts_data->pdev->dev);
}

static void sitronix_ts_late_resume(struct early_suspend *h)
{
	struct sitronix_ts_data *ts_data =
		container_of(h, struct sitronix_ts_data, early_suspend);

	sitronix_ts_resume(&ts_data->pdev->dev);
}
#endif /* CONFIG_HAS_EARLYSUSPEND */

//#ifdef CONFIG_PM
#if (!defined(CONFIG_FB) && !defined(CONFIG_HAS_EARLYSUSPEND) && \
	 defined(CONFIG_PM))
static int sitronix_pm_suspend(struct device *dev)
{
	stmsg("pm suspend\n");
	return 0;
}

static int sitronix_pm_resume(struct device *dev)
{
	stmsg("pm resume");
	return 0;
}

static const struct dev_pm_ops sitronix_ts_dev_pm_ops = {
	.suspend = sitronix_pm_suspend,
	.resume = sitronix_pm_resume,
};
#endif /* CONFIG_PM */

#ifdef SITRONIX_INTERFACE_I2C
static struct platform_driver sitronix_ts_i2c_driver = {
	.driver = {
		.name = SITRONIX_TS_I2C_DRIVER_NAME,
		.owner = THIS_MODULE,
//#ifdef CONFIG_PM
#if (!defined(CONFIG_FB) && !defined(CONFIG_HAS_EARLYSUSPEND) && \
	 defined(CONFIG_PM))
		.pm = &sitronix_ts_dev_pm_ops,
#endif
	},
	.probe = sitronix_ts_probe,
	/*.remove = __exit_p(sitronix_ts_remove),*/
	.remove = sitronix_ts_remove,
};
#endif /* SITRONIX_INTERFACE_I2C */

void sitronix_ts_reset_input_dev(void)
{
	stmsg("sitronix_ts_reprobe!\n");
	if (gts->input_dev) {
		sitronix_ts_get_device_info(gts);
		input_unregister_device(gts->input_dev);
		// input_free_device(gts->input_dev);

		sitronix_ts_input_dev_init(gts);
	}
}

static int __init sitronix_ts_init(void)
{
	int ret;

	stmsg("%s\n", SITRONIX_TP_DRIVER_VERSION);

#ifdef SITRONIX_INTERFACE_I2C
	ret = sitronix_ts_i2c_init();
	if (ret == 0)
		platform_driver_register(&sitronix_ts_i2c_driver);
#endif /* #SITRONIX_INTERFACE_I2C */
	return ret;
}

static void __exit sitronix_ts_exit(void)
{
#ifdef SITRONIX_INTERFACE_I2C
	platform_driver_unregister(&sitronix_ts_i2c_driver);
	sitronix_ts_i2c_exit();
#endif /* #ifdef SITRONIX_INTERFACE_I2C */
}

late_initcall(sitronix_ts_init);
// module_init(sitronix_ts_init);
module_exit(sitronix_ts_exit);

MODULE_AUTHOR("Sitronix Technology Co., Ltd.");
MODULE_DESCRIPTION("Sitronix Touchscreen Controller Driver");
MODULE_LICENSE("GPL");
