// SPDX-License-Identifier: GPL-2.0
#include "sitronix_ts.h"

static int i2cErrorCount;
static atomic_t iMonitorThreadPostpone = ATOMIC_INIT(0);
static uint8_t PreCheckData[4];
static int StatusCheckCount;
static int DisCheckCount;
//#define SITRONIX_MT_GLOBAL_RESET
#ifdef SITRONIX_MT_GLOBAL_RESET
#define MT_GLOBAL_RESET_COUNT 3
#endif //SITRONIX_MT_GLOBAL_RESET

static int sitroinx_ts_check_display_off(void)
{
	uint8_t cmd[8] = { 0 };
	int ret = 0;
	int off = 0;

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0xA5;
	cmd[5] = 0x3C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x14;
	cmd[5] = 0x55;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFC;
	cmd[4] = 0x5A;
	cmd[5] = 0x9E;
	cmd[6] = 0x0A;
	cmd[7] = 0x00;

	ret = sitronix_ts_addrmode_write(gts, cmd, 7);
	if (ret < 0) {
		sterr("read display CMD1 fail - 1\n");
		return -1;
	}

	wbuf[1] = 0x83;
	wbuf[2] = 0x07;
	wbuf[3] = 0x00;

	off = sitronix_ts_addrmode_split_read(gts, wbuf, 3, rbuf, 2);

	if (off < 0) {
		sterr("read display CMD1 fail - 2\n");
		ret = -1;
	}

	cmd[1] = 0x03;
	cmd[2] = 0x06;
	cmd[3] = 0xFE;
	cmd[4] = 0x00;
	cmd[5] = 0xA5;

	sitronix_ts_addrmode_write(gts, cmd, 5);

	if (ret < 0) {
		sterr("read display CMD1 fail - 3\n");
		return -1;
	}

	stmsg("Display status = 0x%X\n", rbuf[off]);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x41;
	cmd[5] = 0x4C;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	cmd[1] = 0x53;
	cmd[2] = 0x71;
	cmd[3] = 0x23;
	cmd[4] = 0x5A;
	cmd[5] = 0xC3;
	ret = sitronix_ts_addrmode_write(gts, cmd, 5);

	if (rbuf[off] & 0x04)
		return 0;
	else
		return 1;
}

void sitronix_ts_mt_reset_process(void)
{
#ifdef SITRONIX_HDL_IN_MT
	mutex_lock(&gts->mutex);
	sitronix_do_upgrade();
	mutex_unlock(&gts->mutex);
#else
	sitronix_ts_reset_device(gts);
#endif /* SITRONIX_HDL_IN_MT */

	/* restore status*/
	if (gts->in_suspend) {
		mutex_lock(&gts->mutex);
		msleep(20);
		sitronix_ts_powerdown(gts, true);
		mutex_unlock(&gts->mutex);
	}

	mutex_lock(&gts->mutex);
	sitronix_mode_restore();
	mutex_unlock(&gts->mutex);
}
int sitronix_ts_monitor_thread_v3(void *data)
{
	int ret = 0;
	uint8_t buf[12] = { 0 };
	//	uint8_t drvBuf[4] = {0};
	bool disStaus;
	int result = 0;
#ifdef SITRONIX_MT_CHECK_DIS
	int i;
	signed short disv;
	bool disOK;
	uint8_t disbuf[40 * 2 + 4] = { 0 };
#endif //SITRONIX_MT_CHECK_DIS
	int mt_peroid = DELAY_MONITOR_THREAD_PEROID_NORMAL;
#ifdef SITRONIX_MT_GLOBAL_RESET
	uint8_t mt_reset_count = 0;
#endif //SITRONIX_MT_GLOBAL_RESET

	disStaus = true;
	stmsg("%s start and delay %d ms\n", __func__,
	      gts->sitronix_ts_delay_monitor_thread_start);
	msleep(gts->sitronix_ts_delay_monitor_thread_start);
	while (!kthread_should_stop()) {
		if (gts->is_reset_chip) {
			sitronix_ts_mt_reset_process();
			gts->is_reset_chip = false;
		} else if (gts->is_suspend_mt) {
			stdbg("MT suspended\n");
		} else if (gts->is_pause_mt || gts->upgrade_doing) {
			stdbg("MT paused\n");
		} else if (atomic_read(&iMonitorThreadPostpone)) {
			atomic_set(&iMonitorThreadPostpone, 0);
		} else {
			mutex_lock(&gts->mutex);
			ret = sitronix_ts_reg_read(gts, FIRMWARE_VERSION, buf,
						   12);
#ifdef SITRONIX_MT_CHECK_DIS
			ret = sitronix_ts_reg_read(
				gts, DATA_OUTPUT_BUFFER, disbuf,
				4 + (gts->ts_dev_info.y_chs * 2));
#endif //SITRONIX_MT_CHECK_DIS
			mutex_unlock(&gts->mutex);

			stmsg("monitor sensing counter: %02x %02x\n", buf[0xA],
			      buf[0xB]);

			if (ret < 0) {
				sterr("read I2C fail (%d)\n", ret);
				result = 0;
				goto exit_i2c_invalid;
			}

			if ((buf[1] & 0x0F) == 0x6) {
				result = 0;
				sterr("read Status: bootcode\n");
				goto exit_i2c_invalid;
			} else {
				result = 1;
				if (PreCheckData[0] == buf[0xA] &&
				    PreCheckData[1] == buf[0xB]) {
					mutex_lock(&gts->mutex);
					if (sitroinx_ts_check_display_off() ==
					    1) {
						StatusCheckCount++;
						mt_peroid =
							DELAY_MONITOR_THREAD_PEROID_NORMAL;
					} else {
						StatusCheckCount++;
						mt_peroid =
							DELAY_MONITOR_THREAD_PEROID_ERROR;
					}
					mutex_unlock(&gts->mutex);
				} else {
					StatusCheckCount = 0;
#ifdef SITRONIX_MT_GLOBAL_RESET
					mt_reset_count = 0;
#endif //SITRONIX_MT_GLOBAL_RESET
					mt_peroid =
						DELAY_MONITOR_THREAD_PEROID_NORMAL;
				}
				PreCheckData[0] = buf[0xA];
				PreCheckData[1] = buf[0xB];

				if (StatusCheckCount >= 3) {
					sterr("IC Status doesn't update!\n");
					result = -1;
					StatusCheckCount = 0;
				}
			}
#ifdef SITRONIX_MT_CHECK_DIS
			if (disbuf[0] == 0x93) {
				disOK = true;
				for (i = 0; i < gts->ts_dev_info.y_chs; i++) {
					disv = (signed short)((disbuf[4 +
								      2 * i]) *
								      0x100 +
							      disbuf[5 + 2 * i]);
					if (disv > SITRONIX_MT_DIS_LIMIT ||
					    disv < -SITRONIX_MT_DIS_LIMIT) {
						sterr("MT dis err (%d,%d)=%d\n",
						      disbuf[2], i, disv);
						disOK = false;
						break;
					}
				}

				if (!disOK) {
					DisCheckCount++;
					mt_peroid =
						DELAY_MONITOR_THREAD_PEROID_ERROR;
				} else {
					DisCheckCount = 0;
					mt_peroid =
						DELAY_MONITOR_THREAD_PEROID_NORMAL;
				}

				if (DisCheckCount >= 3) {
					sterr("Distance error for 3 times!\n");
					result = -1;
					DisCheckCount = 0;
				}
			}
#endif //SITRONIX_MT_CHECK_DIS

			if (-1 == result) {
				stmsg("ESD detected chip abnormal, reset device!\n");
				sitronix_ts_mt_reset_process();
				mt_peroid = DELAY_MONITOR_THREAD_PEROID_NORMAL;
				i2cErrorCount = 0;
				StatusCheckCount = 0;

#ifdef SITRONIX_MT_GLOBAL_RESET
				//MT reset count > 3. Call Display global reset.
				mt_reset_count++;
				if (mt_reset_count >= MT_GLOBAL_RESET_COUNT) {
					stmsg("ESD detected chip abnormal, global reset device!\n");
					//display_global_reset();	//Call Display global reset.
					mt_reset_count = 0;
				}
#endif //SITRONIX_MT_GLOBAL_RESET
			}
exit_i2c_invalid:
			if (result == 0) {
				i2cErrorCount++;
				if ((i2cErrorCount >= 2)) {
					stmsg("I2C abnormal or status bootcode, reset it!\n");
					sitronix_ts_mt_reset_process();
					mt_peroid =
						DELAY_MONITOR_THREAD_PEROID_NORMAL;
					i2cErrorCount = 0;
					StatusCheckCount = 0;

#ifdef SITRONIX_MT_GLOBAL_RESET
					//I2C Error > 3, Call Display global reset.
					//display_global_reset();
#endif //SITRONIX_MT_GLOBAL_RESET
				}
			} else {
				i2cErrorCount = 0;
			}
		}
		msleep(mt_peroid);
	}
	return 0;
}

void sitronix_mt_pause_one(void)
{
	if (gts->enable_monitor_thread == 1)
		atomic_set(&iMonitorThreadPostpone, 1);
}

void sitronix_mt_pause(void)
{
#ifdef SITRONIX_MONITOR_THREAD
	gts->is_pause_mt = 1;
#endif
}

void sitronix_mt_restore(void)
{
#ifdef SITRONIX_MONITOR_THREAD
	gts->is_pause_mt = 0;
#endif
}

void sitronix_mt_suspend(void)
{
	gts->is_suspend_mt = 1;
}

void sitronix_mt_resume(void)
{
	sitronix_mt_pause_one();
	gts->is_suspend_mt = 0;
}

void sitronix_mt_stop(void)
{
	if (gts && gts->enable_monitor_thread == 1) {
		if (gts->SitronixMonitorThread) {
			kthread_stop(gts->SitronixMonitorThread);
			gts->SitronixMonitorThread = NULL;
		}
	}
}

void sitronix_mt_start(int startDelayMS)
{
	if (gts && gts->enable_monitor_thread == 1) {
		/* atomic_set(&ts_data->iMonitorThreadPostpone, 1); */
		sitronix_mt_pause_one();
		StatusCheckCount = 0;
		i2cErrorCount = 0;
		DisCheckCount = 0;
		gts->sitronix_ts_delay_monitor_thread_start = startDelayMS;
		if (!gts->SitronixMonitorThread)
			gts->SitronixMonitorThread =
				kthread_run(gts->sitronix_mt_fp, gts,
					    "Sitronix Monitor Thread");
		if (IS_ERR(gts->SitronixMonitorThread))
			gts->SitronixMonitorThread = NULL;
	}
}
