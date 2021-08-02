/* nubia kernel driver for rgb_bh1745*/
/*
 * rgb_bh1745.c - Linux kernel modules for ambient light + proximity sensor
 *
 * Copyright (C) 2012 Lee Kai Koon <kai-koon.lee@avagotech.com>
 * Copyright (C) 2012 Avago Technologies
 * Copyright (C) 2013 LGE Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/input.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>
#include <linux/of_gpio.h>
#ifdef SENSORS_CLASS_DEV
#include <linux/sensors.h>
#endif
//#include <linux/wakelock.h>
#include <linux/debugfs.h>
#include <linux/types.h>
#include <linux/kobject.h>
#include <linux/fs.h>
#include <asm/unistd.h>

// #include <asm/div64.h>
#include <linux/math64.h>

#include "bh1745.h"

#define PARSE_DTSI_NUMBER                   (22)
#define JUDEG_COEFF                         (1000)
#define COLOR_CONFIG_PATH	 "/persist/sensors/rgb_color_cfg"
#define RGBC_CAL_PATH		 "/persist/sensors/rgbc_cfg"
#define SCALE_FACTOR(x, y) (x)/(y)

#define MODULE_MANUFACTURE_NUMBER		3
#define VALID_FLAG							0x5555

#define BH1745_DRV_NAME     "bh1745"
#define DRIVER_VERSION		"1.0.0"

#define BH1745_REG_LEN 0x0a

#define BH1745_I2C_RETRY_COUNT		3 	/* Number of times to retry i2c */

/*wait more time to try read or write to avoid potencial risk*/
#define BH1745_I2C_RETRY_TIMEOUT	3	/* Timeout between retry (miliseconds) */

#define BH1745_I2C_BYTE 0
#define BH1745_I2C_WORD 1

#define LOG_TAG "ROHM-BH1745"
//#define DEBUG_ON //DEBUG SWITCH

#define SENSOR_LOG_FILE__ strrchr(__FILE__, '/') ? (strrchr(__FILE__, '/') + 1) : __FILE__

#define SENSOR_LOG_ERROR(fmt, args...) printk(KERN_ERR "[%s] [%s:%d] " fmt,\
					LOG_TAG, __FUNCTION__, __LINE__, ##args)
#define SENSOR_LOG_INFO(fmt, args...)  printk(KERN_INFO "[%s] [%s:%d] "  fmt,\
					LOG_TAG, __FUNCTION__, __LINE__, ##args)
#ifdef  DEBUG_ON
#define SENSOR_LOG_DEBUG(fmt, args...) printk(KERN_DEBUG "[%s] [%s:%d] "  fmt,\
					LOG_TAG, __FUNCTION__, __LINE__, ##args)
#else
#define SENSOR_LOG_DEBUG(fmt, args...)
#endif
/*dynamic debug mask to control log print,you can echo value to rgb_bh1745_debug to control*/
static int rgb_bh1745_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int rgb_bh1745_remove(struct i2c_client *client);

static unsigned short bh1745_atime[6] = {160, 320, 640, 1280, 2560, 5120};
static unsigned char bh1745_again[3] = {1, 2, 16};
static int dim_flag = 0;
static long cofficient_judge = 242;
static long cofficient_red[2]={1565, 932};
static long cofficient_green[2] = {9053, 8607};
static long cofficient_blue[2] = {0,0};
enum tp_color_id{
	GOLD = 0,
	WHITE,
	BLACK,
	BLUE,
	TP_COLOR_NUMBER,
};

static dev_t const bh1745_rgb_dev_t     = MKDEV(MISC_MAJOR, 102);
static struct class         *rgb_class;

struct rgbc_parameter{
	int red;
	int green;
	int blue;
	int clear;
};
struct lux_parameter{
	int lux;
};
struct rgb_fac_cal_cfg{
	struct lux_parameter base;
	struct lux_parameter cur;
	u8 flag;
};

static const char *data_array_name[MODULE_MANUFACTURE_NUMBER] = {
	[0] = "bh1745,cal_data0",
	[1] = "bh1745,cal_data1",
	[2] = "bh1745,cal_data2"
};
typedef struct rgb_bh1745_rgb_data {
	int red;
	int green;
	int blue;
	int clear;
	int lx;
	int color_temp;
} rgb_bh1745_rgb_data;

struct lux_cal_parameter{
	long judge;
	long cw_r_gain;
	long other_r_gain;

	long cw_g_gain;
	long other_g_gain;

	long cw_b_gain;
	long other_b_gain;
}lux_cal_parameter;

struct tp_lx_cal_parameter{
	long tp_module_id;
	struct lux_cal_parameter  gold_lux_cal_parameter;
	struct lux_cal_parameter  white_lux_cal_parameter;
	struct lux_cal_parameter  black_lux_cal_parameter;
	struct lux_cal_parameter  blue_lux_cal_parameter;
}tp_lx_cal_parameter;

struct tp_lx_cal_parameter tp_module_parameter[MODULE_MANUFACTURE_NUMBER] = {
	{.tp_module_id = 0x00},
	{.tp_module_id = 0x01},
	{.tp_module_id = 0x02}
};

struct rgb_bh1745_data {
	struct i2c_client *client;
	/*to protect the i2c read and write operation*/
	struct mutex update_lock;
	/*to protect only one thread to control the device register*/
	struct mutex single_lock;
	struct work_struct	als_dwork;	/* for ALS polling */
	struct device *rgb_dev;
	struct input_dev *input_dev_als;

	/* regulator data */
	bool power_on;
	struct regulator *vdd;

	/* pinctrl data*/
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_default;
#ifdef SENSORS_CLASS_DEV
	/*sensor class for als and ps*/
	struct sensors_classdev als_cdev;
#endif
	struct rgb_bh1745_platform_data *platform_data;

	struct rgb_bh1745_rgb_data rgb_data;

    struct rgb_fac_cal_cfg rgb_cal_data;

	int irq;

	struct hrtimer timer;
	unsigned int enable;
	unsigned int irq_control;
	unsigned int ailt;
	unsigned int aiht;
	unsigned int pers;
	unsigned int config;
	unsigned int control;
	unsigned int measure_time;

	/* control flag from HAL */
	unsigned int enable_als_sensor;
	/*to record the open or close state of als before suspend*/
	unsigned int enable_als_state;

	/* ALS parameters */
	unsigned int als_threshold_l;	/* low threshold */
	unsigned int als_threshold_h;	/* high threshold */
	unsigned int als_data;		/* to store ALS data from CH0 or CH1*/
	int als_prev_lux;		/* to store previous lux value */
	int als_cal_lux;
	unsigned int als_poll_delay;	/* needed for light sensor polling : micro-second (us) */
	bool device_exist;
};
#ifdef SENSORS_CLASS_DEV
static struct sensors_classdev sensors_light_cdev = {
	.name = "bh1745-light",
	.vendor = "rohm",
	.version = 1,
	.handle = SENSORS_LIGHT_HANDLE,
	.type = SENSOR_TYPE_LIGHT,
	.max_range = "10000",
	.resolution = "0.0125",
	.sensor_power = "0.20",
	.min_delay = 1000, /* in microseconds */
	.fifo_reserved_event_count = 0,
	.fifo_max_event_count = 0,
	.enabled = 0,
	.delay_msec = 100,
	.sensors_enable = NULL,
	.sensors_poll_delay = NULL,
};
#endif
/*
 * Global data
 */
static struct workqueue_struct *rgb_bh1745_workqueue = NULL;

/*init the register of device function for probe and every time the chip is powered on*/
static int rgb_bh1745_init_client(struct i2c_client *client);
static int als_polling_count=0;
/*we use the unified the function for i2c write and read operation*/
static int rgb_bh1745_i2c_write(struct i2c_client*client, u8 reg, u16 value,bool flag)
{
	int err,loop;

	struct rgb_bh1745_data *data = i2c_get_clientdata(client);

	loop = BH1745_I2C_RETRY_COUNT;
	/*we give three times to repeat the i2c operation if i2c errors happen*/
	while(loop) {
		mutex_lock(&data->update_lock);
		/*0 is i2c_smbus_write_byte_data,1 is i2c_smbus_write_word_data*/
		if(flag == BH1745_I2C_BYTE)
		{
			err = i2c_smbus_write_byte_data(client, reg, (u8)value);
		}
		else if(flag == BH1745_I2C_WORD)
		{
			err = i2c_smbus_write_word_data(client, reg, value);
		}
		else
		{
			SENSOR_LOG_ERROR("attention: i2c write wrong flag\n");
			mutex_unlock(&data->update_lock);
			return -EINVAL;
		}
		mutex_unlock(&data->update_lock);
		if(err < 0){
			loop--;
			msleep(BH1745_I2C_RETRY_TIMEOUT);
		}
		else
			break;
	}
	/*after three times,we print the register and regulator value*/
	if (loop == 0) {
		SENSOR_LOG_ERROR(" attention:i2c write err = %d\n" ,err);
	}

	return err;
}

static int rgb_bh1745_i2c_read(struct i2c_client *client, u8 reg, bool flag)
{
	int err,loop;

	struct rgb_bh1745_data *data = i2c_get_clientdata(client);

	loop = BH1745_I2C_RETRY_COUNT;
	/*we give three times to repeat the i2c operation if i2c errors happen*/
	while(loop) {
		mutex_lock(&data->update_lock);

		/*0 is i2c_smbus_read_byte_data,1 is i2c_smbus_read_word_data*/
		if (flag == BH1745_I2C_BYTE) {
			err = i2c_smbus_read_byte_data(client, reg);
		} else if (flag == BH1745_I2C_WORD) {
			err = i2c_smbus_read_word_data(client, reg);
		} else {
			SENSOR_LOG_ERROR("attention: i2c read wrong flag\n");
			mutex_unlock(&data->update_lock);
			return -EINVAL;
		}
		mutex_unlock(&data->update_lock);
		if (err < 0) {
			loop--;
			msleep(BH1745_I2C_RETRY_TIMEOUT);
		}
		else
			break;
	}
	/*after three times,we print the register and regulator value*/
	if (loop == 0) {
		SENSOR_LOG_ERROR("attention: i2c read err = %d,reg=0x%x\n",err,reg);
	}

	return err;
}
#ifdef SENSORS_CLASS_DEV

static void rgb_bh1745_dump_register(struct i2c_client *client)
{
	int sys_ctl,mode_ctl1,mode_ctl2,mode_ctl3,irq_ctl,pers;
	sys_ctl = rgb_bh1745_i2c_read(client, BH1745_SYSTEMCONTROL,BH1745_I2C_BYTE);
	mode_ctl1= rgb_bh1745_i2c_read(client, BH1745_MODECONTROL1,BH1745_I2C_BYTE);
	mode_ctl2 = rgb_bh1745_i2c_read(client, BH1745_MODECONTROL2,BH1745_I2C_BYTE);
	mode_ctl3 = rgb_bh1745_i2c_read(client, BH1745_MODECONTROL3,BH1745_I2C_BYTE);
	irq_ctl = rgb_bh1745_i2c_read(client, BH1745_INTERRUPT,BH1745_I2C_BYTE);
	pers = rgb_bh1745_i2c_read(client, BH1745_PERSISTENCE,BH1745_I2C_BYTE);

	SENSOR_LOG_INFO("sys_ctl = 0x%x,mode_ctl1=0x%x,mode_ctl2=0x%x\n",sys_ctl,mode_ctl1,mode_ctl2);
	SENSOR_LOG_INFO("mode_ctl3 = 0x%x,irq_ctl=0x%x,pers=0x%x\n",mode_ctl3,irq_ctl,pers);
}
#endif
/******************************************************************************
 * NAME       : rgb_driver_reset
 * FUNCTION   : reset BH1745 register
 * REMARKS    :
 *****************************************************************************/
static int rgb_bh1745_driver_reset(struct i2c_client *client)
{
    int ret;

    /* set soft ware reset */
    ret = rgb_bh1745_i2c_write(client, BH1745_SYSTEMCONTROL, (SW_RESET | INT_RESET), BH1745_I2C_BYTE);
	if (ret < 0){
		SENSOR_LOG_ERROR("i2c error,rgb_bh1745_driver_reset fail %d\n",ret);
		return ret;
	}
	SENSOR_LOG_DEBUG("rgb_bh1745 reset\n");
	/*wait for device reset sucess*/
	mdelay(1);
    return (ret);
}

static int rgb_bh1745_set_enable(struct i2c_client *client, int enable)
{
	int ret;

	ret = rgb_bh1745_i2c_write(client, BH1745_MODECONTROL2, enable, BH1745_I2C_BYTE);
	if (ret < 0) {
		SENSOR_LOG_ERROR("i2c error,enable = %d\n",enable);
		return ret;
	}
	SENSOR_LOG_DEBUG(" rgb_bh1745 enable = %d\n",enable);
	return ret;
}

static int rgb_bh1745_set_pers(struct i2c_client *client, int pers)
{
	struct rgb_bh1745_data *data = i2c_get_clientdata(client);
	int ret;

	ret = rgb_bh1745_i2c_write(client, BH1745_PERSISTENCE, pers, BH1745_I2C_BYTE);
	if (ret < 0) {
		SENSOR_LOG_ERROR("i2c error,pers = %d\n",pers);
		return ret;
	}

	data->pers = pers;
	SENSOR_LOG_DEBUG("rgb_bh1745 pers = %d\n",pers);
	return ret;
}

static int rgb_bh1745_set_interrupt(struct i2c_client *client, int irq_control)
{
	struct rgb_bh1745_data *data = i2c_get_clientdata(client);
	int ret;

	ret = rgb_bh1745_i2c_write(client,BH1745_INTERRUPT, irq_control, BH1745_I2C_BYTE);
	if (ret < 0) {
		SENSOR_LOG_ERROR(" i2c error,irq_control = %d\n" ,irq_control);
		return ret;
	}

	data->irq_control = irq_control;
	SENSOR_LOG_DEBUG("rgb_bh1745 irq_control = %d\n",irq_control);
	return ret;
}

static int rgb_bh1745_set_control(struct i2c_client *client, int control)
{
	struct rgb_bh1745_data *data = i2c_get_clientdata(client);
	int ret;

	ret = rgb_bh1745_i2c_write(client,BH1745_MODECONTROL3, control, BH1745_I2C_BYTE);
	if (ret < 0) {
		SENSOR_LOG_ERROR("i2c error,control = %d\n", control);
		return ret;
	}

	data->control = control;
	SENSOR_LOG_DEBUG("rgb_bh1745 control = %d\n",control);
	return ret;
}

static int rgb_bh1745_set_measure_time(struct i2c_client *client, int measure_time)
{
	struct rgb_bh1745_data *data = i2c_get_clientdata(client);
	int ret;

	ret = rgb_bh1745_i2c_write(client, BH1745_MODECONTROL1, measure_time, BH1745_I2C_BYTE);
	if (ret < 0) {
		SENSOR_LOG_ERROR("i2c error,measure_time = %d\n",measure_time);
		return ret;
	}

	data->measure_time = measure_time;
	SENSOR_LOG_DEBUG("rgb_bh1745 measure_time = %d\n",measure_time);
	return ret;
}
static void rgb_bh1745_cal_data_init(struct rgb_bh1745_data *data)
{
	data->rgb_cal_data.base.lux= 1;
	data->rgb_cal_data.cur.lux= 1;
	data->rgb_cal_data.flag = 0;
}

/******************************************************************************
 * FUNCTION   : Calculate lux
 * REMARKS    :
 * INPUT      : data  : each data value from IC (Red, Green, Blue, Clear)
 *            : gain  : gain's value of when sensor is gotten the data
 *            : itime : time's value of when sensor is getten data (unit is ms)
 * RETURN     : Lux value
 *****************************************************************************/
static int rgb_bh1745_calc_lx(struct i2c_client *client, struct rgb_bh1745_rgb_data *data, unsigned char gain, unsigned short itime)
{
	long long int lx;
	long long int lx_tmp;
	int ret ;

	unsigned short tmp = 0;

	if ((data->red >= BH1745_RGB_DATA_MAX) 
		|| (data->green >= BH1745_RGB_DATA_MAX)
		|| (data->blue >= BH1745_RGB_DATA_MAX))
	{
		lx = BH1745_LUX_MAX;
		return lx;
	}

	if(data->green < 1)
	{
		lx_tmp = 0;
	}
	else if((data->clear * JUDEG_COEFF) <( cofficient_judge*data->green))
	{
		lx_tmp = data->green*cofficient_green[0] + data->red *cofficient_red[0];
		SENSOR_LOG_DEBUG("lx_temp 1: %lld\n", lx_tmp);
	}
	else
	{
		lx_tmp = data->green*cofficient_green[1]+data->red *cofficient_red[1];
		SENSOR_LOG_DEBUG("lx_temp 1: %lld\n", lx_tmp);
	}

	//SENSOR_LOG_DEBUG("cal lx_tmp is %lld\n", lx_tmp);

	if (lx_tmp < 0)
		lx_tmp = 0;

	//lx= lx_tmp/(gain/16)/(itime/160)/1000;
	tmp = (gain/16)/(itime/160)/1000;

	// do_div(lx_tmp, tmp); // result of lx_tmp/tmp is stored inside lx_tmp. The reminder is returned by the function
	div_s64(lx_tmp, tmp);

	lx = lx_tmp;

	if (lx < 200) {
		if(!dim_flag) {
			ret = rgb_bh1745_i2c_write(client,BH1745_MODECONTROL1, MEASURE_640MS , BH1745_I2C_BYTE);
			if (ret < 0)
				SENSOR_LOG_ERROR("i2c change measurement error = %d\n", ret);
			else
				dim_flag = 1;
		}
	} else {
		if(dim_flag) {
			ret = rgb_bh1745_i2c_write(client,BH1745_MODECONTROL1, MEASURE_320MS , BH1745_I2C_BYTE);
			if (ret < 0)
				SENSOR_LOG_ERROR("i2c change measurement error = %d\n",ret);
			else
				dim_flag = 0;
		}

	}

	SENSOR_LOG_DEBUG("gain = %d, itime=%d, lux = %lld\n",gain, itime, lx);
	//SENSOR_LOG_DEBUG("judge = %ld ,red[0] = %ld, red[1]=%ld, green[0] = %ld, green[1]=%ld, blue[0]=%ld,blue[1]=%ld\n",cofficient_judge,cofficient_red[0], cofficient_red[1],cofficient_green[0], cofficient_green[1],cofficient_blue[0],cofficient_blue[1]);

	return ((int)lx);
}

static int rgb_bh1745_als_rgbc_sample(struct rgb_bh1745_data *data, struct rgbc_parameter *rgbc)
{
    int ret;
    struct i2c_client *client = data->client;

	ret = rgb_bh1745_i2c_read(client, BH1745_MODECONTROL2, BH1745_I2C_WORD);
	if (ret < 0) {
		SENSOR_LOG_ERROR("i2c read fail, read BH1745_MODECONTROL2 error\n" );
		return ret;
	}

    if (ret &= MODECONTROL2_VALID) {
		rgbc->red   = rgb_bh1745_i2c_read(client, BH1745_RED_DATA_LSB, BH1745_I2C_WORD);
		rgbc->green = rgb_bh1745_i2c_read(client, BH1745_GREEN_DATA_LSB, BH1745_I2C_WORD);
		rgbc->blue  = rgb_bh1745_i2c_read(client, BH1745_BLUE_DATA_LSB, BH1745_I2C_WORD);
		rgbc->clear = rgb_bh1745_i2c_read(client, BH1745_CLEAR_DATA_LSB, BH1745_I2C_WORD);
		SENSOR_LOG_DEBUG("read rgbc reg success\n");
    } else {
		SENSOR_LOG_DEBUG("the data is not update\n");
		return 1;
    }
    return 0;

}
static int rgb_bh1745_enable_prepare(struct rgb_bh1745_data *data)
{
	int ret;
	struct rgb_bh1745_platform_data *pdata = data->platform_data;
	struct i2c_client *client = data->client;
	mutex_lock(&data->single_lock);
	/* Power on and initalize the device */
	if (pdata->power_on)
        pdata->power_on(true, data);

	ret = rgb_bh1745_init_client(client);
	if (ret < 0) {
        SENSOR_LOG_ERROR("Failed to init rgb_bh1745\n");
        mutex_unlock(&data->single_lock);
        return ret;
	}
	als_polling_count = 0;
	data->enable = (data->enable)|RGBC_EN_ON;
	ret = rgb_bh1745_set_enable(client, data->enable);
	if (ret < 0) {
        SENSOR_LOG_ERROR("set enable failed\n");
        mutex_unlock(&data->single_lock);
        return ret;
	}
	mutex_unlock(&data->single_lock);
	SENSOR_LOG_DEBUG("enable als sensor,data->enable=0x%x\n", data->enable);
	return 0;
}
static int rgb_bh1745_get_lux(struct rgb_bh1745_data *data, bool report_event)
{
	int ret;
	struct i2c_client *client = data->client;
	int  luxValue = 0;
	unsigned char gain = 0;
	unsigned short time = 0;
	int tmp = 0;
	unsigned char lux_is_valid = 1;

	mutex_lock(&data->single_lock);

	ret = rgb_bh1745_als_rgbc_sample(data, (struct rgbc_parameter *)&(data->rgb_data));
	if (ret < 0) {
	    mutex_unlock(&data->single_lock);
	    return -EINVAL;
	}
	if (ret > 0) {
	    goto get_lux_exit;
	}

	SENSOR_LOG_DEBUG("rgb bh1745 data->rgb_data.red(%d); data->rgb_data.green(%d);data->rgb_data.blue(%d);data->rgb_data.clear(%d)\n",
		data->rgb_data.red,
		data->rgb_data.green,
		data->rgb_data.blue,
		data->rgb_data.clear);

	if((data->rgb_data.red < 0) 
		|| (data->rgb_data.green < 0)
		|| (data->rgb_data.blue < 0)
		|| (data->rgb_data.clear < 0))
	{
		/* don't report, this is invalid lux value */
		lux_is_valid = 0;
		luxValue = data->als_prev_lux;
		SENSOR_LOG_ERROR("i2c read fail, rgb bh1745 data->rgb_data.red(%d); data->rgb_data.green(%d);data->rgb_data.blue(%d);data->rgb_data.clear(%d)\n",
		data->rgb_data.red,
		data->rgb_data.green,
		data->rgb_data.blue,
		data->rgb_data.clear);

	} else {

		tmp = rgb_bh1745_i2c_read(client, BH1745_MODECONTROL1, BH1745_I2C_BYTE);
		if (tmp < 0) {
			SENSOR_LOG_ERROR("i2c read error tmp = %d\n",tmp);
			tmp = 0;
		}
		tmp = tmp & 0x7;
		time = bh1745_atime[tmp];
		tmp = rgb_bh1745_i2c_read(client, BH1745_MODECONTROL2, BH1745_I2C_BYTE);
		if (tmp < 0) {
			SENSOR_LOG_ERROR("i2c read error tmp = %d\n", tmp);
			tmp = 0;
		}
		tmp = tmp & 0x3;
		gain = bh1745_again[tmp];
		luxValue = rgb_bh1745_calc_lx(client, &(data->rgb_data), gain, time);
	}
	if (luxValue >= 0) {
	    ret = 0;
		luxValue = luxValue < BH1745_LUX_MAX? luxValue : BH1745_LUX_MAX;
		data->als_prev_lux = luxValue;
	} else {
		SENSOR_LOG_ERROR("cal lux error, luxValue = %d lux_is_valid =%d\n", luxValue, lux_is_valid);
		/* don't report, this is invalid lux value */
		lux_is_valid = 0;
		luxValue = data->als_prev_lux;
	}

	if (als_polling_count < 5) {
		if (luxValue == BH1745_LUX_MAX) {
			luxValue = luxValue - als_polling_count%2;
		} else {
			luxValue = luxValue + als_polling_count%2;
		}
		als_polling_count++;
	}

get_lux_exit:

	mutex_unlock(&data->single_lock);
	/*remove it because we use other judge method to decide if pls close event is triggered by sunlight*/
	if (lux_is_valid && report_event) {
		if (data->rgb_cal_data.flag) {
		    mutex_lock(&data->single_lock);
		    data->als_cal_lux= data->als_prev_lux * SCALE_FACTOR(data->rgb_cal_data.base.lux,
		        data->rgb_cal_data.cur.lux);
		    mutex_unlock(&data->single_lock);
		    input_report_rel(data->input_dev_als, REL_X,  data->als_cal_lux + 1);
		    SENSOR_LOG_DEBUG("rgb bh1745 cal lux=%d\n",data->als_cal_lux);
		} else {
		    input_report_rel(data->input_dev_als, REL_X, data->als_prev_lux + 1);
		    SENSOR_LOG_DEBUG("rgb bh1745 lux=%d\n",data->als_prev_lux);
		}
		/* report the lux level */
		input_sync(data->input_dev_als);
	}
	return ret;

}
/* delete rgb_bh1745_reschedule_work, we use queue_work to replase queue_delayed_work, because flush_delayed_work
   may cause system stop work */
/* ALS polling routine */
static void rgb_bh1745_als_polling_work_handler(struct work_struct *work)
{
	int ret;
	struct rgb_bh1745_data *data = container_of(work, struct rgb_bh1745_data, als_dwork);
	ret = rgb_bh1745_get_lux(data, true);
	if (ret < 0) {
        goto restart_timer;
	}
	/* restart timer */
	/* start a work after 200ms */
restart_timer:
	// if (0 != hrtimer_start(&data->timer,
							// ktime_set(0, data->als_poll_delay * 1000000), HRTIMER_MODE_REL) )
	// {
	hrtimer_start(&data->timer, ktime_set(0, data->als_poll_delay * 1000000), HRTIMER_MODE_REL);

	//SENSOR_LOG_ERROR("hrtimer_start fail! nsec=%d\n", data->als_poll_delay);
	// }
}

/*****************************************************************
Parameters    :  timer
Return        :  HRTIMER_NORESTART
Description   :  hrtimer_start call back function,
				 use to report als data
*****************************************************************/
static enum hrtimer_restart rgb_bh1745_als_timer_func(struct hrtimer *timer)
{
	struct rgb_bh1745_data* data = container_of(timer,struct rgb_bh1745_data,timer);
	queue_work(rgb_bh1745_workqueue, &data->als_dwork);
	return HRTIMER_NORESTART;
}
static int rgb_bh1745_enter_suspend_mode(struct rgb_bh1745_data *data, bool suspended)
{
	SENSOR_LOG_INFO("enable_als_sensor = %d\n", data->enable_als_sensor);
	if (suspended) {
	    hrtimer_cancel(&data->timer);
	    cancel_work_sync(&data->als_dwork);
	    /*avoid hrtimer restart in data->als_dwork*/
	    hrtimer_cancel(&data->timer);
	} else {
	    hrtimer_start(&data->timer, ktime_set(0, data->als_poll_delay * 1000000), HRTIMER_MODE_REL);
	}
	return 0;
}
/*
 * IOCTL support
 */
static int rgb_bh1745_enable_als_sensor(struct i2c_client *client, int val)
{
	struct rgb_bh1745_data *data = i2c_get_clientdata(client);
	struct rgb_bh1745_platform_data *pdata = data->platform_data;
	int ret;

	SENSOR_LOG_INFO("enable als val=%d\n",val);

	if (val == 1) {
		/* turn on light  sensor */
		SENSOR_LOG_INFO("pdata->panel_id = %d pdata->tp_color = %d\n", pdata->panel_id,pdata->tp_color);
		SENSOR_LOG_DEBUG("lux cal parameter from dtsi  is judge[%ld], red[%ld], red[%ld], green[%ld] , green[%ld], blue[%ld],  blue[%ld]\n", cofficient_judge, cofficient_red[0],cofficient_red[1],cofficient_green[0],cofficient_green[1],cofficient_blue[0],cofficient_blue[1]);
		if (data->enable_als_sensor == 0) {
            ret = rgb_bh1745_enable_prepare(data);
            if (ret < 0) {
				SENSOR_LOG_ERROR("enable rgb failed\n");
				return ret;
            }
            mutex_lock(&data->single_lock);
            data->enable_als_sensor = 1;
            mutex_unlock(&data->single_lock);
            /* enable als sensor, start data report hrtimer */
            hrtimer_start(&data->timer, ktime_set(0, MEASURE_DELAY_320MS * 1000000), HRTIMER_MODE_REL);
		}
	} else {
		/*
		 * turn off light sensor
		 */
		 if(data->enable_als_sensor == 1) {

			mutex_lock(&data->single_lock);
			data->enable_als_sensor = 0;
			data->enable =  ADC_GAIN_X16|RGBC_EN_OFF;
			mutex_unlock(&data->single_lock);
			rgb_bh1745_set_enable(client, data->enable);

			SENSOR_LOG_DEBUG("disable rgb bh1745 als sensor,data->enable = 0x%x\n",data->enable);
			/* disable als sensor, cancne data report hrtimer */
			hrtimer_cancel(&data->timer);
			cancel_work_sync(&data->als_dwork);
			/*avoid hrtimer restart in data->als_dwork*/
			hrtimer_cancel(&data->timer);
		 }

	}
	/* Vote off  regulators if both light and prox sensor are off */
	if ((data->enable_als_sensor == 0)&&(pdata->power_on)) {
		pdata->power_on(false, data);
	}
	SENSOR_LOG_DEBUG("enable als sensor success\n");
	return 0;
}
/*
 * SysFS support
 */
 #ifdef SENSORS_CLASS_DEV
 static int rgb_bh1745_als_set_enable(struct sensors_classdev *sensors_cdev,
		unsigned int enable)
{
	int ret = 0;

	struct rgb_bh1745_data *data = container_of(sensors_cdev,struct rgb_bh1745_data, als_cdev);
	struct i2c_client *client = data->client;

	if ((enable != 0) && (enable != 1)) {
		SENSOR_LOG_ERROR("invalid value(%d)\n",enable);
		return -EINVAL;
	}
	SENSOR_LOG_DEBUG("rgb bh1745 als enable=%d\n",enable);

	/*for debug and print registers value when enable/disable the als every time*/
	if (enable == 0)
	{
		rgb_bh1745_enable_als_sensor(data->client, enable);
		rgb_bh1745_dump_register(client);
	} else {
		rgb_bh1745_enable_als_sensor(data->client, enable);
		rgb_bh1745_dump_register(client);		
	}
	return ret;
}
#endif
/*use this function to reset the poll_delay time(ms),val is the time parameter*/
static int rgb_bh1745_set_als_poll_delay(struct i2c_client *client,
		unsigned int val)
{
	struct rgb_bh1745_data *data = i2c_get_clientdata(client);
	// int ret;

	/* minimum 320ms */
	if (val < 500) {
		data->als_poll_delay = MEASURE_DELAY_320MS;
	} else if (val < 1200){
		data->als_poll_delay = MEASURE_DELAY_640MS;
	} else{
		data->als_poll_delay = MEASURE_DELAY_1280MS;
	}

	SENSOR_LOG_INFO(" poll delay %d\n" , data->als_poll_delay);

	/*
	 * If work is already scheduled then subsequent schedules will not
	 * change the scheduled time that's why we have to cancel it first.
	 */
	cancel_work_sync(&data->als_dwork);
	hrtimer_cancel(&data->timer);
	// ret = hrtimer_start(&data->timer, ktime_set(0, data->als_poll_delay * 1000000), HRTIMER_MODE_REL);
	hrtimer_start(&data->timer, ktime_set(0, data->als_poll_delay * 1000000), HRTIMER_MODE_REL);
	
	// if (ret != 0) {
	// SENSOR_LOG_ERROR("hrtimer_start fail! nsec=%d\n", data->als_poll_delay);
	// 	return ret;
	// }
	
	return 0;
}
#ifdef SENSORS_CLASS_DEV
static int rgb_bh1745_als_poll_delay(struct sensors_classdev *sensors_cdev,
		unsigned int delay_msec)
{
	struct rgb_bh1745_data *data = container_of(sensors_cdev,
			struct rgb_bh1745_data, als_cdev);
	rgb_bh1745_set_als_poll_delay(data->client, delay_msec);
	return 0;
}
#endif
static ssize_t attr_rgb_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
    struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	return snprintf(buf,32,"enable = %d\n", data->enable_als_sensor);
}
static ssize_t attr_rgb_enable_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	int err = -1;
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned long val = 0;
	SENSOR_LOG_DEBUG("enter\n");
	val = simple_strtoul(buf, NULL, 10);
	SENSOR_LOG_DEBUG("enable als sensor (%ld)\n", val);

	if ((val != 0) && (val != 1)) {
		SENSOR_LOG_INFO("enable ps sensor=%ld\n", val);
		return val;
	}
   
	err = rgb_bh1745_enable_als_sensor(client, val);
	if (err != 0) {
		SENSOR_LOG_ERROR("enable failed.\n");
	}
	SENSOR_LOG_DEBUG("exit\n");
	return count;
}

static ssize_t attr_rgb_delay_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
    struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	return snprintf(buf,32,"delay = %d\n", data->als_poll_delay );
}
static ssize_t attr_rgb_delay_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t count)
{
	int err = -1;
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	unsigned long val = simple_strtoul(buf, NULL, 10);
	SENSOR_LOG_INFO("enable als sensor (%ld)\n", val);

    err = rgb_bh1745_set_als_poll_delay(client, val);
	if (err != 0) {
		SENSOR_LOG_ERROR("set delay failed.\n");
	}

	return count;
}
static ssize_t rgb_bh1745_show_red_data(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int red_data;

	red_data = rgb_bh1745_i2c_read(client,BH1745_RED_DATA_LSB,BH1745_I2C_WORD);

	return snprintf(buf,32,"%d\n", red_data);
}



static ssize_t rgb_bh1745_show_green_data(struct device *dev,
			struct device_attribute *attr, char *buf)
{
    struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int green_data;

	green_data = rgb_bh1745_i2c_read(client,BH1745_GREEN_DATA_LSB,BH1745_I2C_WORD);

	return snprintf(buf,32, "%d\n", green_data);
}


static ssize_t rgb_bh1745_show_blue_data(struct device *dev,
			struct device_attribute *attr, char *buf)
{
    struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int blue_data;

	blue_data = rgb_bh1745_i2c_read(client, BH1745_BLUE_DATA_LSB,BH1745_I2C_WORD);
	if(blue_data <0){
		SENSOR_LOG_ERROR("read blue_data failed\n");
	}

 	return snprintf(buf,32, "%d\n", blue_data);
 }


static ssize_t rgb_bh1745_show_clear_data(struct device *dev,
			struct device_attribute *attr, char *buf)
{
    struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int clear_data;

	clear_data = rgb_bh1745_i2c_read(client, BH1745_CLEAR_DATA_LSB, BH1745_I2C_WORD);
	if (clear_data < 0){
		SENSOR_LOG_ERROR("read clear_data failed\n");
	}

 	return snprintf(buf,32, "%d\n", clear_data);
 }


/*
* set the register's value from userspace
* Usage: echo "0x08|0x12" > dump_reg
*			"reg_address|reg_value"
*/
static ssize_t rgb_bh1745_write_reg(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
    struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	int val_len_max = 4;
	char* input_str =NULL;
	char reg_addr_str[10]={'\0'};
	char reg_val_str[10]={'\0'};
	long reg_addr,reg_val;
	int addr_lenth=0,value_lenth=0,buf_len=0,ret = -1;
	char* strtok=NULL;

	buf_len = strlen(buf);
	input_str = kzalloc(buf_len, GFP_KERNEL);
	if (!input_str)
	{
		SENSOR_LOG_ERROR("kmalloc fail!\n");
		return -ENOMEM;
	}

	snprintf(input_str, 10,"%s", buf);
	/*Split the string when encounter "|", for example "0x08|0x12" will be splited "0x18" "0x12" */
	strtok = strsep(&input_str, "|");
	if (strtok!=NULL) {
		addr_lenth = strlen(strtok);
		memcpy(reg_addr_str,strtok,((addr_lenth > (val_len_max))?(val_len_max):addr_lenth));
	} else {
		SENSOR_LOG_ERROR("buf name Invalid:%s", buf);
		goto parse_fail_exit;
	}
	strtok=strsep(&input_str, "|");
	if (strtok!=NULL) {
		value_lenth = strlen(strtok);
		memcpy(reg_val_str,strtok,((value_lenth > (val_len_max))?(val_len_max):value_lenth));
	} else {
		SENSOR_LOG_ERROR("buf value Invalid:%s",buf);
		goto parse_fail_exit;
	}
	/* transform string to long int */
	ret = kstrtol(reg_addr_str,16,&reg_addr);
	if (ret)
		goto parse_fail_exit;

	ret = kstrtol(reg_val_str,16,&reg_val);
	if (ret)
		goto parse_fail_exit;

	/* write the parsed value in the register*/
	ret = rgb_bh1745_i2c_write(client,(char)reg_addr,(char)reg_val,BH1745_I2C_BYTE);
	if (ret < 0){
		goto parse_fail_exit;
	}
	return count;

parse_fail_exit:
	if (input_str)
		kfree(input_str);

	return ret;
}

/*
* show all registers' value to userspace
*/
static ssize_t rgb_bh1745_print_reg_buf(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	int i;
	char reg[BH1745_REG_LEN];
    struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;

	/* read all register value and print to user*/
	for(i = 0; i < BH1745_REG_LEN; i++ )
	{
		reg[i] = rgb_bh1745_i2c_read(client, (0x50+i), BH1745_I2C_BYTE);
		if(reg[i] <0){
			SENSOR_LOG_ERROR("read %d reg failed\n",i);
			return reg[i] ;
		}
	}

	return snprintf(buf,512,"reg[0x0~0x8]=0x%2x, 0x%2x, 0x%2x, 0x%2x, 0x%2x, 0x%2x, 0x%2x, 0x%2x, 0x%2x\n"
			"reg[0x09~0x11]0x%2x\n",
			reg[0x00],reg[0x01],reg[0x02],reg[0x03],reg[0x04],reg[0x05],reg[0x06],reg[0x07],reg[0x08],reg[0x09]);
}

static int rgb_bh1745_file_read(char *file_path, char *read_buf ,int count)
{
	struct file *file_p;
	mm_segment_t old_fs;
	int vfs_retval = -EINVAL;
	bool file_exist = true;
	char *buf = NULL;

	SENSOR_LOG_DEBUG("read infomation : size =%d\n", count);
	if (NULL == file_path) {
		SENSOR_LOG_ERROR("file_path is NULL\n");
		return -EINVAL;
	}

	file_p = filp_open(file_path, O_RDONLY , 0444);
	if (IS_ERR(file_p)) {
		file_exist = false;
		SENSOR_LOG_INFO("file does not exist\n");
		buf = kzalloc(count * sizeof(char), GFP_KERNEL);
		if (IS_ERR_OR_NULL(buf)) {
			SENSOR_LOG_ERROR("alloc mem failed\n");
			goto error;
		}
	} else {
		filp_close(file_p, NULL);
	}

	file_p = filp_open(file_path, O_CREAT|O_RDWR , 0666);
	if (IS_ERR(file_p)) {
		SENSOR_LOG_ERROR("[open file <%s>failed]\n",file_path);
		goto error;
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	if (!file_exist) {
		SENSOR_LOG_DEBUG("init file memory\n");
		if (!IS_ERR_OR_NULL(buf)) {
			vfs_retval = kernel_write(file_p, (char *)buf, sizeof(buf), &file_p->f_pos);
			// vfs_retval = vfs_write(file_p, (char *)buf, sizeof(buf), &file_p->f_pos);
			if (vfs_retval < 0) {
				SENSOR_LOG_ERROR("[write file <%s>failed]\n",file_path);
				goto file_close;
			}
		}

	}

	file_p->f_pos = 0;
	vfs_retval = kernel_read(file_p, (char*)read_buf, count, &file_p->f_pos);
	// vfs_retval = vfs_read(file_p, (char*)read_buf, count, &file_p->f_pos);
	if (vfs_retval < 0) {
		SENSOR_LOG_ERROR("[write file <%s>failed]\n",file_path);
		goto file_close;
	}

	SENSOR_LOG_INFO("read ok\n");

file_close:
	set_fs(old_fs);
	filp_close(file_p, NULL);
error:
	if (!IS_ERR_OR_NULL(buf))
		kfree(buf);
	return vfs_retval;
}

static int rgb_bh1745_file_write(char *file_path, const char *write_buf ,int count)
{
	struct file *file_p;
	mm_segment_t old_fs;
	int vfs_retval = -EINVAL;

	SENSOR_LOG_DEBUG("write infomation : size =%d\n", count);
	if (NULL == file_path) {
		SENSOR_LOG_ERROR("file_path is NULL\n");
		return -EINVAL;
	}

	file_p = filp_open(file_path, O_CREAT|O_RDWR , 0666);
	if (IS_ERR(file_p)) {
		SENSOR_LOG_ERROR("[open file <%s>failed]\n",file_path);
		goto error;
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	vfs_retval = kernel_write(file_p, (char*)write_buf, count, &file_p->f_pos);
	// vfs_retval = vfs_write(file_p, (char*)write_buf, count, &file_p->f_pos);
	if (vfs_retval < 0) {
		SENSOR_LOG_ERROR("[write file <%s>failed]\n",file_path);
		goto file_close;
	}

	SENSOR_LOG_INFO("write ok\n");

file_close:
	set_fs(old_fs);
	filp_close(file_p, NULL);
error:
	return vfs_retval;
}

static int rgb_bh1745_config_tp_parameter(struct rgb_bh1745_platform_data *pdata)
{
    int i = 0;
    int err = -1;
    for (i = 0;i < MODULE_MANUFACTURE_NUMBER;i++){
        if (pdata->panel_id == tp_module_parameter[i].tp_module_id){
            if (pdata->tp_color == GOLD) {
			    cofficient_judge = tp_module_parameter[i].gold_lux_cal_parameter.judge;
			    cofficient_red[0] = tp_module_parameter[i].gold_lux_cal_parameter.cw_r_gain;
			    cofficient_red[1] = tp_module_parameter[i].gold_lux_cal_parameter.other_r_gain;
			    cofficient_green[0] = tp_module_parameter[i].gold_lux_cal_parameter.cw_g_gain;
			    cofficient_green[1] = tp_module_parameter[i].gold_lux_cal_parameter.other_g_gain;
			    cofficient_blue[0] = tp_module_parameter[i].gold_lux_cal_parameter.cw_b_gain;
			    cofficient_blue[1] = tp_module_parameter[i].gold_lux_cal_parameter.other_b_gain;
	        } else if (pdata->tp_color == WHITE) {
			    cofficient_judge = tp_module_parameter[i].white_lux_cal_parameter.judge;
			    cofficient_red[0] = tp_module_parameter[i].white_lux_cal_parameter.cw_r_gain;
			    cofficient_red[1] = tp_module_parameter[i].white_lux_cal_parameter.other_r_gain;
			    cofficient_green[0] = tp_module_parameter[i].white_lux_cal_parameter.cw_g_gain;
			    cofficient_green[1] = tp_module_parameter[i].white_lux_cal_parameter.other_g_gain;
			    cofficient_blue[0] = tp_module_parameter[i].white_lux_cal_parameter.cw_b_gain;
			    cofficient_blue[1] = tp_module_parameter[i].white_lux_cal_parameter.other_b_gain;
	        } else if (pdata->tp_color == BLACK) {
			    cofficient_judge = tp_module_parameter[i].black_lux_cal_parameter.judge;
			    cofficient_red[0] = tp_module_parameter[i].black_lux_cal_parameter.cw_r_gain;
			    cofficient_red[1] = tp_module_parameter[i].black_lux_cal_parameter.other_r_gain;
			    cofficient_green[0] = tp_module_parameter[i].black_lux_cal_parameter.cw_g_gain;
			    cofficient_green[1] = tp_module_parameter[i].black_lux_cal_parameter.other_g_gain;
			    cofficient_blue[0] = tp_module_parameter[i].black_lux_cal_parameter.cw_b_gain;
			    cofficient_blue[1] = tp_module_parameter[i].black_lux_cal_parameter.other_b_gain;
	        } else if (pdata->tp_color == BLUE) {
			    cofficient_judge = tp_module_parameter[i].black_lux_cal_parameter.judge;
			    cofficient_red[0] = tp_module_parameter[i].black_lux_cal_parameter.cw_r_gain;
			    cofficient_red[1] = tp_module_parameter[i].black_lux_cal_parameter.other_r_gain;
			    cofficient_green[0] = tp_module_parameter[i].black_lux_cal_parameter.cw_g_gain;
			    cofficient_green[1] = tp_module_parameter[i].black_lux_cal_parameter.other_g_gain;
			    cofficient_blue[0] = tp_module_parameter[i].black_lux_cal_parameter.cw_b_gain;
			    cofficient_blue[1] = tp_module_parameter[i].black_lux_cal_parameter.other_b_gain;
	        }
            err = 0;
        }
    }
    return err;
}
/*
 *	panel_id represent junda ofilm jdi.
 *	tp_color represent golden white black blue
 *
 */
static ssize_t write_module_tpcolor(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
    struct rgb_bh1745_data *data = dev_get_drvdata(dev);
    struct rgb_bh1745_platform_data *pdata = data->platform_data;
	int err;
	u32 val;
	int valid_flag;
	err = kstrtoint(buf, 0, &val);
	if (err < 0) {
		SENSOR_LOG_ERROR("kstrtoint failed\n");
		return count;
	}
	valid_flag = val & 0xffff;
	pdata->panel_id = (val >> 16) & 0xff;
	pdata->tp_color = (val >> 24) & 0xff;
	if (valid_flag != VALID_FLAG){
		SENSOR_LOG_ERROR("valid flag error\n");
		return count;
	}
	SENSOR_LOG_INFO("panel_id = %d pdata->tp_color = %d\n",pdata->panel_id,pdata->tp_color);
	err = rgb_bh1745_config_tp_parameter(pdata);
    if (err < 0) {
        SENSOR_LOG_ERROR("init cofficient by defalut\n");
    }
	SENSOR_LOG_INFO("lux cal  parameter from dtsi  is judge[%ld], red[%ld], red[%ld], green[%ld] , green[%ld], blue[%ld],  blue[%ld]\n", cofficient_judge, cofficient_red[0],cofficient_red[1],cofficient_green[0],cofficient_green[1],cofficient_blue[0],cofficient_blue[1]);
	return count;
}
static ssize_t read_tp_parameters(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return snprintf(buf,2048,"golden0 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "white0 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "black0 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "blue0 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "golden1 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "white1 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "black1 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "blue1 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "golden2 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "white2 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "black2 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n"
	                                    "blue2 judge[%ld],cw_r_gain[%ld],other_r_gain[%ld],cw_g_gain[%ld],other_g_gain[%ld],cw_b_gain[%ld],other_b_gain[%ld]\n",
	tp_module_parameter[0].gold_lux_cal_parameter.judge,tp_module_parameter[0].gold_lux_cal_parameter.cw_r_gain,tp_module_parameter[0].gold_lux_cal_parameter.other_r_gain,
	tp_module_parameter[0].gold_lux_cal_parameter.cw_g_gain,tp_module_parameter[0].gold_lux_cal_parameter.other_g_gain,tp_module_parameter[0].gold_lux_cal_parameter.cw_b_gain,tp_module_parameter[0].gold_lux_cal_parameter.other_b_gain,
	tp_module_parameter[0].white_lux_cal_parameter.judge,tp_module_parameter[0].white_lux_cal_parameter.cw_r_gain,tp_module_parameter[0].white_lux_cal_parameter.other_r_gain,
	tp_module_parameter[0].white_lux_cal_parameter.cw_g_gain,tp_module_parameter[0].white_lux_cal_parameter.other_g_gain,tp_module_parameter[0].white_lux_cal_parameter.cw_b_gain,tp_module_parameter[0].white_lux_cal_parameter.other_b_gain,
	tp_module_parameter[0].black_lux_cal_parameter.judge,tp_module_parameter[0].black_lux_cal_parameter.cw_r_gain,tp_module_parameter[0].black_lux_cal_parameter.other_r_gain,
	tp_module_parameter[0].black_lux_cal_parameter.cw_g_gain,tp_module_parameter[0].black_lux_cal_parameter.other_g_gain,tp_module_parameter[0].black_lux_cal_parameter.cw_b_gain,tp_module_parameter[0].black_lux_cal_parameter.other_b_gain,
	tp_module_parameter[0].blue_lux_cal_parameter.judge,tp_module_parameter[0].blue_lux_cal_parameter.cw_r_gain,tp_module_parameter[0].blue_lux_cal_parameter.other_r_gain,
	tp_module_parameter[0].blue_lux_cal_parameter.cw_g_gain,tp_module_parameter[0].blue_lux_cal_parameter.other_g_gain,tp_module_parameter[0].blue_lux_cal_parameter.cw_b_gain,tp_module_parameter[0].blue_lux_cal_parameter.other_b_gain,
	tp_module_parameter[1].gold_lux_cal_parameter.judge,tp_module_parameter[1].gold_lux_cal_parameter.cw_r_gain,tp_module_parameter[1].gold_lux_cal_parameter.other_r_gain,
	tp_module_parameter[1].gold_lux_cal_parameter.cw_g_gain,tp_module_parameter[1].gold_lux_cal_parameter.other_g_gain,tp_module_parameter[1].gold_lux_cal_parameter.cw_b_gain,tp_module_parameter[1].gold_lux_cal_parameter.other_b_gain,
	tp_module_parameter[1].white_lux_cal_parameter.judge,tp_module_parameter[1].white_lux_cal_parameter.cw_r_gain,tp_module_parameter[1].white_lux_cal_parameter.other_r_gain,
	tp_module_parameter[1].white_lux_cal_parameter.cw_g_gain,tp_module_parameter[1].white_lux_cal_parameter.other_g_gain,tp_module_parameter[1].white_lux_cal_parameter.cw_b_gain,tp_module_parameter[1].white_lux_cal_parameter.other_b_gain,
	tp_module_parameter[1].black_lux_cal_parameter.judge,tp_module_parameter[1].black_lux_cal_parameter.cw_r_gain,tp_module_parameter[1].black_lux_cal_parameter.other_r_gain,
	tp_module_parameter[1].black_lux_cal_parameter.cw_g_gain,tp_module_parameter[1].black_lux_cal_parameter.other_g_gain,tp_module_parameter[1].black_lux_cal_parameter.cw_b_gain,tp_module_parameter[1].black_lux_cal_parameter.other_b_gain,
	tp_module_parameter[1].blue_lux_cal_parameter.judge,tp_module_parameter[1].blue_lux_cal_parameter.cw_r_gain,tp_module_parameter[1].blue_lux_cal_parameter.other_r_gain,
	tp_module_parameter[1].blue_lux_cal_parameter.cw_g_gain,tp_module_parameter[1].blue_lux_cal_parameter.other_g_gain,tp_module_parameter[1].blue_lux_cal_parameter.cw_b_gain,tp_module_parameter[1].blue_lux_cal_parameter.other_b_gain,
	tp_module_parameter[2].gold_lux_cal_parameter.judge,tp_module_parameter[2].gold_lux_cal_parameter.cw_r_gain,tp_module_parameter[2].gold_lux_cal_parameter.other_r_gain,
	tp_module_parameter[2].gold_lux_cal_parameter.cw_g_gain,tp_module_parameter[2].gold_lux_cal_parameter.other_g_gain,tp_module_parameter[2].gold_lux_cal_parameter.cw_b_gain,tp_module_parameter[2].gold_lux_cal_parameter.other_b_gain,
	tp_module_parameter[2].white_lux_cal_parameter.judge,tp_module_parameter[2].white_lux_cal_parameter.cw_r_gain,tp_module_parameter[2].white_lux_cal_parameter.other_r_gain,
	tp_module_parameter[2].white_lux_cal_parameter.cw_g_gain,tp_module_parameter[2].white_lux_cal_parameter.other_g_gain,tp_module_parameter[2].white_lux_cal_parameter.cw_b_gain,tp_module_parameter[2].white_lux_cal_parameter.other_b_gain,
	tp_module_parameter[2].black_lux_cal_parameter.judge,tp_module_parameter[2].black_lux_cal_parameter.cw_r_gain,tp_module_parameter[2].black_lux_cal_parameter.other_r_gain,
	tp_module_parameter[2].black_lux_cal_parameter.cw_g_gain,tp_module_parameter[2].black_lux_cal_parameter.other_g_gain,tp_module_parameter[2].black_lux_cal_parameter.cw_b_gain,tp_module_parameter[2].black_lux_cal_parameter.other_b_gain,
	tp_module_parameter[2].blue_lux_cal_parameter.judge,tp_module_parameter[2].blue_lux_cal_parameter.cw_r_gain,tp_module_parameter[2].blue_lux_cal_parameter.other_r_gain,
	tp_module_parameter[2].blue_lux_cal_parameter.cw_g_gain,tp_module_parameter[2].blue_lux_cal_parameter.other_g_gain,tp_module_parameter[2].blue_lux_cal_parameter.cw_b_gain,tp_module_parameter[2].blue_lux_cal_parameter.other_b_gain);
}

static ssize_t attr_rgb_config_tpinfo_show(struct device *dev,
                       struct device_attribute *attr, char *buf)
{
	int err;
	char cfg = 0;
	err = rgb_bh1745_file_read(COLOR_CONFIG_PATH,
                                &cfg,
                                sizeof(cfg));
	if (err < 0) {
		SENSOR_LOG_ERROR("read tpcolor parameters failed\n");
	}
	return sprintf(buf, "%x\n", cfg);
}

static ssize_t attr_rgb_config_tpinfo_store(struct device *dev,
                 struct device_attribute *attr, const char *buf, size_t count)
{
	int err = 0;
	int cfg = 0;
	int valid_flag = 0;
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct rgb_bh1745_platform_data *pdata = data->platform_data;

	err = kstrtoint(buf, 0, &cfg);
	if (err < 0) {
		SENSOR_LOG_ERROR("kstrtoint failed\n");
		return err;
	}
	valid_flag = cfg & 0x80;
	if (!valid_flag) {
		SENSOR_LOG_ERROR("valid flag error\n");
		return -EINVAL;
	}
	pdata->tp_color = cfg & 0x0f;
	pdata->panel_id = (cfg>>4) & 0x07;

	SENSOR_LOG_INFO("panel_id =%d, tp_color =%d",pdata->panel_id, pdata->tp_color);

	if ((pdata->tp_color < 0) && (pdata->tp_color >= TP_COLOR_NUMBER)) {
		SENSOR_LOG_ERROR("TP_COLOR_NUMBER invalid\n");
		return -EINVAL;
	}
	err = rgb_bh1745_config_tp_parameter(pdata);
	if (err < 0) {
		SENSOR_LOG_ERROR("init cofficient by defalut\n");
		return err;
	}
	err = rgb_bh1745_file_write(COLOR_CONFIG_PATH,
                                (const char *)&cfg,
                                sizeof(cfg));
	if (err < 0) {
		SENSOR_LOG_ERROR("save tpcolor parameters failed\n");
		return err;
	}
	return count;
}

static int rgb_bh1745_get_mean_rgbc(struct rgb_bh1745_data *data, struct rgbc_parameter *rgbc)
{
	int ret;
	int keep_cnt = 0;
	int update_cnt = 0;
	int retry_times = 30;
	struct rgbc_parameter tmp_rgbc;
	if (IS_ERR_OR_NULL(rgbc) || IS_ERR_OR_NULL(data)) {
		SENSOR_LOG_ERROR("NULL\n");
		return -EINVAL;
	}
	memset(&tmp_rgbc, 0, sizeof(struct rgbc_parameter));
	while (retry_times) {
		// rgb_bh1745_als_rgbc_sample() will return 1 if RGBC do not change a lot
		ret = rgb_bh1745_als_rgbc_sample(data, &tmp_rgbc);
        if (0 == ret) {
            update_cnt++;
            keep_cnt = 0;
        }
		if (ret > 0) {
			keep_cnt++;
		}
        if ((update_cnt > 0) && (keep_cnt > 1)) {
            break;
        }
		retry_times--;
		if (retry_times < 1) {
			SENSOR_LOG_ERROR("lux value is not steady\n");
			memset(&tmp_rgbc, 0, sizeof(struct rgbc_parameter));
			return -1;
		}
        msleep(1);
	}

	if (tmp_rgbc.red <= 0 || tmp_rgbc.green <= 0
			|| tmp_rgbc.blue <= 0 || tmp_rgbc.clear <= 0) {
		SENSOR_LOG_ERROR("invalid rgbc parameter\n");
		return -EINVAL;
	}

	SENSOR_LOG_INFO("get steady lux success\n");
	memcpy(rgbc, &tmp_rgbc, sizeof(struct rgbc_parameter));

	return 0;
}
static ssize_t attr_rgb_factory_cal_show(struct device *dev,
                           struct device_attribute *attr, char *buf)
{
	int ret;
	bool cal_success = true;
	struct rgbc_parameter rgbc;
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	memset(&rgbc, 0, sizeof(struct rgbc_parameter));
	if (!data->enable_als_sensor) {
        ret = rgb_bh1745_enable_als_sensor(data->client, 1);
	    if (ret < 0) {
		    SENSOR_LOG_ERROR("enable failed.\n");
		    return -EINVAL;
	    }
	}
	msleep(1);
	ret = rgb_bh1745_get_mean_rgbc(data, &rgbc);
	if (ret < 0) {
		    SENSOR_LOG_ERROR("get mean rgbc raw data error\n");
            cal_success = false;
            goto cal_close;
    }
	msleep(1);
cal_close:
	ret = rgb_bh1745_enable_als_sensor(data->client, data->enable_als_sensor);
	if (ret < 0) {
		SENSOR_LOG_ERROR("disable failed.\n");
        return -EINVAL;
	}
	if (!cal_success)
        return -EINVAL;
	return snprintf(buf, sizeof(struct rgbc_parameter) * 2, "%d,%d,%d,%d",
			rgbc.red,
			rgbc.green,
			rgbc.blue,
			rgbc.clear);
}
/*
 * get mean lux by update-register
 */
static ssize_t attr_rgb_get_mean_lux(struct rgb_bh1745_data *data)
{
	int retry_times = 50;
	int err = 0;
	int update_cnt = 0;
	int keep_cnt = 0;

	if (IS_ERR_OR_NULL(data)) {
		SENSOR_LOG_ERROR("NULL\n");
		return -EINVAL;
	}
	while(retry_times) {
	    err = rgb_bh1745_get_lux(data, false);
	    if (err == 0) {
	        keep_cnt++;
	    }
	    if (err > 0 && keep_cnt > 0) {
	        update_cnt++;
	    }

	    retry_times--;
	    if (!retry_times) {
	        SENSOR_LOG_ERROR("lux value is not steady.\n");
	        return -EINVAL;
	    }
	    /*mean lux request: update 1th, keep 2th*/
	    if (update_cnt > 0)
	        break;

	    msleep(10);
	}
	return data->als_prev_lux;
}
/*
 * core calibration algo impletation
 */
static ssize_t rgb_bh1745_calibrate_work(struct rgb_bh1745_data *data, const char *cal_data)
{
	int err = 0;

	if (unlikely(cal_data == NULL)) {
	    SENSOR_LOG_ERROR("NULL\n");
	    return -1;
	}

	if (!data->enable_als_sensor) {
	    err = rgb_bh1745_enable_prepare(data);
	    if (err < 0) {
		    SENSOR_LOG_ERROR("enable failed.\n");
		    goto rgb_cal_exit;
	    }
	}

	/*copy mem directly instead of parse string*/
	memcpy(&data->rgb_cal_data.base, cal_data, sizeof(data->rgb_cal_data.base));
	memcpy(&data->rgb_cal_data.cur, cal_data, sizeof(data->rgb_cal_data.cur));

	err = attr_rgb_get_mean_lux(data);
	if (err < 0) {
	    SENSOR_LOG_ERROR("get mean lux value error\n");
	    goto rgb_cal_exit;
	}
	data->rgb_cal_data.cur.lux = (err > 0) ? err : 1;

	SENSOR_LOG_INFO("rgb_cal_data.base.lux = %d\n", data->rgb_cal_data.base.lux);
	SENSOR_LOG_INFO("rgb_cal_data.cur.lux = %d\n", data->rgb_cal_data.cur.lux);

	data->rgb_cal_data.flag = (data->rgb_cal_data.base.lux > 0) ? 1 : 0;

	if (data->rgb_cal_data.flag) {
	    mutex_lock(&data->single_lock);
	    data->als_cal_lux= data->als_prev_lux * SCALE_FACTOR(data->rgb_cal_data.base.lux,
	        data->rgb_cal_data.cur.lux);
	    mutex_unlock(&data->single_lock);
	} else {
	    rgb_bh1745_cal_data_init(data);
	}

	SENSOR_LOG_INFO("rgb_cal_data.flag = %d\n", data->rgb_cal_data.flag);

	err = rgb_bh1745_file_write(RGBC_CAL_PATH,
                                (const char *)&(data->rgb_cal_data),
                                sizeof(struct rgb_fac_cal_cfg));
	if (err < 0) {
		SENSOR_LOG_ERROR("save rgb cal parameters failed\n");
		goto rgb_cal_exit;
	}

rgb_cal_exit:
	err = rgb_bh1745_enable_als_sensor(data->client, data->enable_als_sensor);
	if (err < 0) {
	    SENSOR_LOG_ERROR("disable failed.\n");
	}
	return err;
}
/*
 * calibration in factory by PC
 */
static ssize_t attr_rgb_factory_cal_store(struct device *dev,
                   struct device_attribute *attr, const char *buf, size_t count)
{
	int err;

	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	if (IS_ERR_OR_NULL(buf)) {
	    SENSOR_LOG_ERROR("NULL.\n");
	    return -EINVAL;
	}

	err = rgb_bh1745_calibrate_work(data, buf);
	if (err < 0) {
	    SENSOR_LOG_ERROR("calibrate rgb failed.\n");
	}

	return count;
}

static ssize_t attr_rgb_lux_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count;
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	if (data->enable_als_sensor) {
		if (data->rgb_cal_data.flag) {
		    input_report_rel(data->input_dev_als, REL_X,  data->als_cal_lux);
		} else {
		    input_report_rel(data->input_dev_als, REL_X, data->als_prev_lux);
		}
		/* report the lux level */
		input_sync(data->input_dev_als);
	}
	if (data->rgb_cal_data.flag) {
	    count = sprintf(buf, "%d", data->als_cal_lux);
	} else {
	    count = sprintf(buf, "%d", data->als_prev_lux);
	}
	SENSOR_LOG_INFO("als_cal_lux = %d ,als_lux =%d\n",data->als_cal_lux, data->als_prev_lux);
	return count;
}
static ssize_t attr_rgb_chipid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int id;
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	id  = rgb_bh1745_i2c_read(client, BH1745_SYSTEMCONTROL, BH1745_I2C_BYTE);
	id &= 0x3f;
	if (id == 0x0b) {
		return sprintf(buf, "%s", "bh1745");
	}
	SENSOR_LOG_INFO("ROHM BH1745 Does not exist \n");
	return -ENODEV;
}
static ssize_t attr_rgb_dev_init_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return 0;
}
/*
 * init rgb configuration when hal init func is called
 */
static ssize_t attr_rgb_dev_init_store(struct device *dev,
                   struct device_attribute *attr, const char *buf, size_t count)
{
	int err;
	char cfg = 0;
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	struct rgb_bh1745_platform_data *pdata = data->platform_data;

	err = rgb_bh1745_file_read(COLOR_CONFIG_PATH,
                                &cfg,
                                sizeof(cfg));
	if (err < 0) {
		SENSOR_LOG_ERROR("read tpcolor parameters failed\n");
	}
	if (cfg & 0x80) {
		pdata->tp_color = (cfg) & 0x0f;
		pdata->panel_id = (cfg>>4) & 0x07;
	} else {
		pdata->tp_color = 0;
		pdata->panel_id = 0;
	}
	SENSOR_LOG_INFO("panel_id =%d, tp_color =%d",pdata->panel_id, pdata->tp_color);

	err = rgb_bh1745_config_tp_parameter(pdata);
	if (err < 0) {
		SENSOR_LOG_ERROR("init cofficient by defalut\n");
		return err;
	}
	SENSOR_LOG_INFO("config tpcolor is %d\n", pdata->tp_color);

	err = rgb_bh1745_file_read(RGBC_CAL_PATH,
                               (char *)&(data->rgb_cal_data),
                               sizeof(struct rgb_fac_cal_cfg));
	if (err < 0) {
		SENSOR_LOG_ERROR("read factory cal parameters failed\n");
	}
	if (data->rgb_cal_data.cur.lux == 0) {
		rgb_bh1745_cal_data_init(data);
	}
	return count;
}
/*
 * return calibration result
 */
static ssize_t attr_lux_calibrate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	return sprintf(buf, "%x\n", data->rgb_cal_data.flag);
}
/*
 * manual calibration
 */
static ssize_t attr_lux_calibrate_store(struct device *dev,
                   struct device_attribute *attr, const char *buf, size_t count)
{
	int err;
	int val = 1;
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);

	if (IS_ERR_OR_NULL(buf)) {
	    SENSOR_LOG_ERROR("NULL.\n");
	    return -EINVAL;
	}

	err = kstrtoint(buf, 0, &val);
	if (err < 0) {
	    SENSOR_LOG_ERROR("kstrtoint failed\n");
	    return err;
	}

	err = rgb_bh1745_calibrate_work(data, (const char *)&val);
	if (err < 0) {
	    SENSOR_LOG_ERROR("rgb_bh1745_calibrate_work.\n");
	}
	return count;
}
/*
 * Initialization function
 */
static int rgb_bh1745_read_device_id(struct i2c_client *client)
{
	int id;
	id  = rgb_bh1745_i2c_read(client, BH1745_SYSTEMCONTROL, BH1745_I2C_BYTE);
	id &= 0x3f;
	if (id == 0x0b) {
		SENSOR_LOG_INFO("ROHM BH1745\n");
	} else {
		SENSOR_LOG_INFO("ROHM BH1745 Does not exist \n");
		return -ENODEV;
	}
	return 0;
}
static int rgb_bh1745_init_client(struct i2c_client *client)
{
	struct rgb_bh1745_data *data = i2c_get_clientdata(client);
	int err;
	int def_state = data->enable;

	data->enable = ADC_GAIN_X16|RGBC_EN_OFF;
	err = rgb_bh1745_set_enable(client, data->enable);
	if (err < 0) {
		SENSOR_LOG_ERROR("rgb_bh1745_set_enable FAIL\n");
		return err;
	}

	err = rgb_bh1745_set_interrupt(client, BH1745_IRQ_DISABLE);
	if (err < 0) {
		SENSOR_LOG_ERROR("rgb_bh1745_set_interrupt FAIL\n");
		return err;
	}

	err = rgb_bh1745_set_measure_time(client, MEASURE_320MS);
	if (err < 0) {
		SENSOR_LOG_ERROR("rgb_bh1745_set_measure_time FAIL\n");
		return err;
	}

	dim_flag = 0;

	err = rgb_bh1745_set_pers(client, BH1745_PPERS_1);
	if (err < 0) {
		SENSOR_LOG_ERROR("rgb_bh1745_set_pers FAIL\n");
		return err;
	}

	err = rgb_bh1745_set_control(client, MODE_CTL_FIX_VAL);
	if (err < 0) {
		SENSOR_LOG_ERROR("rgb_bh1745_set_pers FAIL\n");
		return err;
	}

	// restore default sensor state
	SENSOR_LOG_INFO("restoring default sensor enable state\n");
	data->enable = def_state;
	err = rgb_bh1745_set_enable(client, data->enable);
	if (err < 0) {
		SENSOR_LOG_ERROR("rgb_bh1745_set_enable FAIL\n");
		return err;
	}

	return 0;
}
/*qualcom updated the regulator configure functions and we add them all*/
static int sensor_regulator_configure(struct rgb_bh1745_data *data, bool on)
{
	int rc;
	SENSOR_LOG_INFO("enter.\n");
	if (!on) {
		if(!IS_ERR_OR_NULL(data->vdd)) {
			if (regulator_count_voltages(data->vdd) > 0) {
				regulator_set_voltage(data->vdd, 0, BH1745_VDD_MAX_UV);
			}
			regulator_put(data->vdd);
		}

	} else {
		data->vdd = regulator_get(&data->client->dev, "vdd");
		if (IS_ERR(data->vdd)) {
			rc = PTR_ERR(data->vdd);
			SENSOR_LOG_ERROR("Regulator get failed vdd rc=%d\n", rc);
			return rc;
		}
		if(!IS_ERR_OR_NULL(data->vdd)) {
		    if (regulator_count_voltages(data->vdd) > 0) {
			    rc = regulator_set_voltage(data->vdd, BH1745_VDD_MIN_UV, BH1745_VDD_MAX_UV);
			    if (rc) {
				    SENSOR_LOG_ERROR("Regulator set failed vdd rc=%d\n",rc);
				    goto reg_vdd_put;
			    }
			}
		}
	}
	SENSOR_LOG_INFO("exit.\n");
	return 0;

reg_vdd_put:
	regulator_put(data->vdd);
	return rc;
}
/*In suspend and resume function,we only control the als,leave pls alone*/
static int rgb_bh1745_suspend(struct device *dev)
{
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	int rc = 0;
	SENSOR_LOG_INFO("enter.\n");
	if (data->enable_als_sensor) {
	    rc = rgb_bh1745_enter_suspend_mode(data, true);
	}
	SENSOR_LOG_INFO("exit.\n");
	return rc;
}

static int rgb_bh1745_resume(struct device *dev)
{
	struct rgb_bh1745_data *data = dev_get_drvdata(dev);
	int rc = 0;
	SENSOR_LOG_INFO("enter.\n");
	if (data->enable_als_sensor) {
	    rc = rgb_bh1745_enter_suspend_mode(data, false);
	}
	SENSOR_LOG_INFO("exit.\n");
	return rc ;
}
#ifdef SENSORS_CLASS_DEV
/*pamameter subfunction of probe to reduce the complexity of probe function*/
static int rgb_bh1745_sensorclass_init(struct rgb_bh1745_data *data, struct i2c_client* client)
{
	int err;
	/* Register to sensors class */
	data->als_cdev = sensors_light_cdev;
	data->als_cdev.sensors_enable = rgb_bh1745_als_set_enable;
	data->als_cdev.sensors_poll_delay = rgb_bh1745_als_poll_delay;

	err = sensors_classdev_register(&data->input_dev_als->dev, &data->als_cdev);
	if (err) {
		SENSOR_LOG_ERROR("Unable to register to sensors class: %d\n", err);
	}

	return err;
}
#endif

// Set RGBC_EN_OFF instead of RGBC_EN_ON and set enable_als_sensor to 0
// in order to disable the sensor when the end of the module's probing phase
static void rgb_bh1745_parameter_init(struct rgb_bh1745_data *data)
{
	struct rgb_bh1745_platform_data *pdata = data->platform_data;
	data->enable = ADC_GAIN_X16|RGBC_EN_ON;	/* default mode is standard */
	data->enable_als_sensor = 1;	// default to 0
	data->als_poll_delay = MEASURE_DELAY_640MS;	// default to 640ms
	data->als_prev_lux = 100;
	pdata->panel_id = -1;
	pdata->tp_color = -1;
	rgb_bh1745_cal_data_init(data);

}
/*input init subfunction of probe to reduce the complexity of probe function*/
static int rgb_bh1745_input_init(struct rgb_bh1745_data *data)
{
	int err = 0;
	/* Register to Input Device */
	data->input_dev_als = input_allocate_device();
	if (!data->input_dev_als) {
		err = -ENOMEM;
		SENSOR_LOG_ERROR("Failed to allocate input device als\n");
		goto exit;
	}

	set_bit(EV_REL, data->input_dev_als->evbit);;
    set_bit(REL_X,  data->input_dev_als->relbit);
    set_bit(REL_Y,  data->input_dev_als->relbit);

	data->input_dev_als->name = "light";

	err = input_register_device(data->input_dev_als);
	if (err) {
		err = -ENOMEM;
		SENSOR_LOG_ERROR("Unable to register input device als: %s\n",
				data->input_dev_als->name);
		goto input_register_err;
	}
	return err;

input_register_err:
	input_free_device(data->input_dev_als);
exit:
	return err;
}

static int sensor_regulator_power_on(struct rgb_bh1745_data *data, bool on)
{
	int rc = 0;

	if (!on) {
        if (!IS_ERR_OR_NULL(data->vdd)) {
		    rc = regulator_disable(data->vdd);
		    if (rc) {
			    SENSOR_LOG_ERROR("Regulator vdd disable failed rc=%d\n",rc);
			    return rc;
		    }
        }
		return rc;
	} else {
	    if (!IS_ERR_OR_NULL(data->vdd)) {
		    rc = regulator_enable(data->vdd);
		    if (rc) {
			    SENSOR_LOG_ERROR("Regulator vdd enable failed rc=%d\n",rc);
			    return rc;
		    }
	    }
	}
	mdelay(5);
	SENSOR_LOG_DEBUG("Sensor regulator power on =%d\n",on);
	return rc;
}

static int sensor_platform_hw_power_on(bool on, struct rgb_bh1745_data *data)
{
	int err = 0;
	if (data->power_on != on) {
		if (!IS_ERR_OR_NULL(data->pinctrl)) {
			if (on)
				/*after poweron,set the INT pin the default state*/
				err = pinctrl_select_state(data->pinctrl,
					data->pin_default);
			if (err)
				SENSOR_LOG_ERROR("Can't select pinctrl state\n");
		}

		err = sensor_regulator_power_on(data, on);
		if (err)
			SENSOR_LOG_ERROR("Can't configure regulator!\n");
		else
			data->power_on = on;
	}
	SENSOR_LOG_INFO("power ops:%d.\n", (on ? 1 : 0));
	return err;
}
static int sensor_platform_hw_init(struct rgb_bh1745_data *data)
{
	int error;

	error = sensor_regulator_configure(data, true);
	if (error < 0) {
		SENSOR_LOG_ERROR("unable to configure regulator\n");
		return error;
	}

	return 0;
}

static void sensor_platform_hw_exit(struct rgb_bh1745_data *data)
{
	int err;
	err = sensor_regulator_configure(data, false);
	if (err < 0) {
		SENSOR_LOG_ERROR("unable to configure regulator\n");
		return;
	}
	return;
}

static int rgb_bh1745_pinctrl_init(struct rgb_bh1745_data *data)
{
	struct i2c_client *client = data->client;

	data->pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR_OR_NULL(data->pinctrl)) {
		SENSOR_LOG_ERROR("Failed to get pinctrl\n");
		return PTR_ERR(data->pinctrl);
	}
	//we have not set the sleep state of INT pin
	data->pin_default =
		pinctrl_lookup_state(data->pinctrl, "default");
	if (IS_ERR_OR_NULL(data->pin_default)) {
		SENSOR_LOG_ERROR("Failed to look up default state\n");
		return PTR_ERR(data->pin_default);
	}
    SENSOR_LOG_INFO("rgb_bh1745 pinctrl init ok.\n");
	return 0;
}

static int sensor_parse_dt(struct device *dev,
		struct rgb_bh1745_platform_data *pdata)
{
	struct device_node *np = dev->of_node;
	unsigned int tmp;
	int tp_moudle_count = 0;
	int index =0;
	int rc = 0;
	int array_len = 0;
	int retval = 0;
	int i = 0;
	const char *raw_data0_dts = NULL;
	long *ptr = NULL;

	/* set functions of platform data */
	pdata->init = sensor_platform_hw_init;
	pdata->exit = sensor_platform_hw_exit;
	pdata->power_on = sensor_platform_hw_power_on;

	rc = of_property_read_u32(np, "bh1745,tp_moudle_count", &tmp);
	if (rc) {
		SENSOR_LOG_ERROR("Unable to read ga_a_value\n");
		return rc;
	}

	tp_moudle_count = tmp;

	SENSOR_LOG_INFO("read lux cal parameter count from dtsi  is %d\n",tp_moudle_count);

	if (tp_moudle_count > MODULE_MANUFACTURE_NUMBER) {
		SENSOR_LOG_ERROR(" tp_moudle_count from dtsi too large: %d\n" , tp_moudle_count);
		return  -EINVAL;
	}

	for (i=0; i < tp_moudle_count; i++){
		array_len = of_property_count_strings(np, data_array_name[i]);
		if (array_len != PARSE_DTSI_NUMBER) {
			SENSOR_LOG_ERROR("bh1745,junda_data0 length invaild or dts number is larger than:%d\n",array_len);
			return array_len;
		}
		SENSOR_LOG_INFO("read lux cal parameter count from dtsi  is %d\n", array_len);

		ptr = (long *)&tp_module_parameter[i];

		for(index = 0; index < array_len; index++){
			retval = of_property_read_string_index(np, data_array_name[i], index, &raw_data0_dts);
			if (retval) {
				SENSOR_LOG_ERROR("read index = %d,raw_data0_dts = %s,retval = %d error,\n",index, raw_data0_dts, retval);
				return retval;
			}
			ptr[index]  = simple_strtol(raw_data0_dts, NULL, 10);
			SENSOR_LOG_DEBUG("lux cal parameter from dtsi  is %ld\n",ptr[index]);
		}
	}

	return 0;
}

static struct device_attribute attrs_rgb_device[] = {
	__ATTR(enable, 0664, attr_rgb_enable_show, attr_rgb_enable_store),
	__ATTR(delay, 0664, attr_rgb_delay_show, attr_rgb_delay_store),
	__ATTR(red_data, 0444, rgb_bh1745_show_red_data, NULL),
	__ATTR(green_data, 0444, rgb_bh1745_show_green_data, NULL),
	__ATTR(blue_data, 0444, rgb_bh1745_show_blue_data, NULL),
	__ATTR(clear_data, 0444, rgb_bh1745_show_clear_data, NULL),
	__ATTR(dump_reg ,0664, rgb_bh1745_print_reg_buf, rgb_bh1745_write_reg),
	__ATTR(module_tpcolor ,0644, read_tp_parameters, write_module_tpcolor),
	__ATTR(tp_cfg, 0644, attr_rgb_config_tpinfo_show, attr_rgb_config_tpinfo_store),
	__ATTR(fac_calibrate,0644, attr_rgb_factory_cal_show, attr_rgb_factory_cal_store),
	__ATTR(dev_init, 0644, attr_rgb_dev_init_show, attr_rgb_dev_init_store),
	__ATTR(light_value, 0444, attr_rgb_lux_show, NULL),
	__ATTR(chip_name, 0440, attr_rgb_chipid_show, NULL),
	__ATTR(calibrate, 0664, attr_lux_calibrate_show, attr_lux_calibrate_store),
};

static int create_sysfs_interfaces(struct device *dev)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(attrs_rgb_device); i++)
		if (device_create_file(dev, attrs_rgb_device + i))
			return -ENODEV;
	return 0;
}
static void remove_sysfs_interfaces(struct device *dev)
{
	int i;

	for (i = ARRAY_SIZE(attrs_rgb_device) - 1; i >= 0; i--)
		device_remove_file(dev, attrs_rgb_device + i);
	return;
}
static const struct i2c_device_id rgb_bh1745_id[] = {
	{ BH1745_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rgb_bh1745_id);

static struct of_device_id rgb_bh1745_match_table[] = {
	{ .compatible = "rohm,bh1745",},
	{ },
};

static struct dev_pm_ops bh1745_pm_ops = {
	.suspend = rgb_bh1745_suspend,
	.resume = rgb_bh1745_resume,
};

static struct i2c_driver rgb_bh1745_driver = {
	.driver = {
		.name   = BH1745_DRV_NAME,
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(rgb_bh1745_match_table),
		.pm = &bh1745_pm_ops,
	},
	.probe  = rgb_bh1745_probe,
	.remove = rgb_bh1745_remove,
	.id_table = rgb_bh1745_id,
};


/*
 * I2C init/probing/exit functions
 */
static int rgb_bh1745_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct rgb_bh1745_data *data = NULL;
	struct rgb_bh1745_platform_data *pdata = NULL;
	int err = 0;

	SENSOR_LOG_INFO("probe start.\n");

	data = kzalloc(sizeof(struct rgb_bh1745_data), GFP_KERNEL);
	if (IS_ERR_OR_NULL(data)) {
		SENSOR_LOG_ERROR("Failed to allocate memory\n");
		err = -ENOMEM;
		goto exit;
	}

	if (client->dev.of_node) {
		/*Memory allocated with this function is automatically freed on driver detach.*/
		pdata = devm_kzalloc(&client->dev,
				sizeof(struct rgb_bh1745_platform_data),
				GFP_KERNEL);
		if (IS_ERR_OR_NULL(pdata)) {
			SENSOR_LOG_ERROR("Failed to allocate memory\n");
			err =-ENOMEM;
			goto exit;
		}

		client->dev.platform_data = pdata;
		err = sensor_parse_dt(&client->dev, pdata);
		if (err) {
			SENSOR_LOG_ERROR("sensor_parse_dt() err\n");
			goto exit;
		}
	} else {
		pdata = client->dev.platform_data;
		if (!pdata) {
			SENSOR_LOG_ERROR("No platform data\n");
			err = -ENODEV;
			goto exit;
		}
	}

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE)) {
		SENSOR_LOG_ERROR("Failed to i2c_check_functionality\n");
		err = -EIO;
		goto exit;
	}

	data->platform_data = pdata;
	data->client = client;
	data->device_exist = false;
	i2c_set_clientdata(client, data);
	rgb_bh1745_parameter_init(data);
	

	/* h/w initialization */
	if (pdata->init)
		err = pdata->init(data);

	if (pdata->power_on)
		err = pdata->power_on(true, data);


	/* initialize pinctrl */
	err = rgb_bh1745_pinctrl_init(data);
	if (err) {
		SENSOR_LOG_ERROR("Can't initialize pinctrl\n");
		data->pinctrl = NULL;
	} else {
		SENSOR_LOG_ERROR("RGB BH1745 use pinctrl\n" );
	}

	if (!IS_ERR_OR_NULL(data->pinctrl)) {
		err = pinctrl_select_state(data->pinctrl, data->pin_default);
		if (err) {
			SENSOR_LOG_ERROR("Can't select pinctrl default state\n" );
			data->pinctrl = NULL;
			data->pin_default = NULL;
		}
		SENSOR_LOG_ERROR(" RGB BH1745 select pinctrl default state\n" );
	}

	mutex_init(&data->update_lock);
	mutex_init(&data->single_lock);

	INIT_WORK(&data->als_dwork, rgb_bh1745_als_polling_work_handler);
	/* Initialize the BH1745 chip and judge who am i*/
	err = rgb_bh1745_read_device_id(client);
	if (err < 0) {
		SENSOR_LOG_ERROR("Failed to read rgb_bh1745 for %d\n", err);
		goto exit_power_off;
	}

	/*init rgb class */
	rgb_class = class_create(THIS_MODULE, "light");
	data->rgb_dev = device_create(rgb_class, NULL, bh1745_rgb_dev_t, &rgb_bh1745_driver ,"light");
	if (IS_ERR(data->rgb_dev)) {
		err = PTR_ERR(data->rgb_dev);
		SENSOR_LOG_ERROR("device_create rgb failed\n");
		goto create_light_dev_failed;
	}

	dev_set_drvdata(data->rgb_dev, data);

	err = create_sysfs_interfaces(data->rgb_dev);
	if (err < 0) {
		SENSOR_LOG_ERROR("create sysfs interfaces failed\n");
		goto create_sysfs_interface_error;
	}


	err = rgb_bh1745_driver_reset(client);
	if (err < 0) {
		SENSOR_LOG_ERROR("Failed to reset rgb_bh1745\n");
		goto create_sysfs_interface_error;
	}
	err = rgb_bh1745_init_client(client);
	if (err) {
		SENSOR_LOG_ERROR("Failed to init rgb_bh1745\n");
		goto create_sysfs_interface_error;
	}

	err = rgb_bh1745_input_init(data);
	if (err)
		goto create_sysfs_interface_error;
#ifdef SENSORS_CLASS_DEV
	err = rgb_bh1745_sensorclass_init(data,client);
	if (err) {
		SENSOR_LOG_ERROR("Unable to register to sensors class: %d\n",err);
		goto exit_unregister_dev_als;
	}
#endif
	rgb_bh1745_workqueue = create_workqueue("rgb_bh1745_work_queue");
	if (IS_ERR_OR_NULL(rgb_bh1745_workqueue)) {
		SENSOR_LOG_ERROR("Create ps_workqueue fail.\n");
		goto exit_unregister_sensorclass;
	}

	/* init hrtimer and call back function */
	hrtimer_init(&data->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	data->timer.function = rgb_bh1745_als_timer_func;

	if (pdata->power_on)
		err = pdata->power_on(false, data);

	SENSOR_LOG_INFO("Support ver. %s enabled\n", DRIVER_VERSION);
	data->device_exist = true;

	SENSOR_LOG_INFO("probe ok.\n");

	return 0;

exit_unregister_sensorclass:
#ifdef SENSORS_CLASS_DEV
	sensors_classdev_unregister(&data->als_cdev);
exit_unregister_dev_als:
#endif
	input_unregister_device(data->input_dev_als);
create_sysfs_interface_error:
	remove_sysfs_interfaces(data->rgb_dev);
create_light_dev_failed:
	data->rgb_dev = NULL;
	device_destroy(rgb_class, bh1745_rgb_dev_t);
	class_destroy(rgb_class);
exit_power_off:
	if (pdata->power_on)
		pdata->power_on(false,data);
	if (pdata->exit)
		pdata->exit(data);
exit:
	kfree(data);
	return -ENODEV;
}

static int rgb_bh1745_remove(struct i2c_client *client)
{
	struct rgb_bh1745_data *data = i2c_get_clientdata(client);
	struct rgb_bh1745_platform_data *pdata = data->platform_data;

	data->enable = ADC_GAIN_X16|RGBC_EN_OFF;
	rgb_bh1745_set_enable(client, data->enable);
	remove_sysfs_interfaces(data->rgb_dev);
	input_unregister_device(data->input_dev_als);

	free_irq(client->irq, data);
	hrtimer_cancel(&data->timer);

	if (pdata->power_on)
		pdata->power_on(false,data);

	if (pdata->exit)
		pdata->exit(data);
	SENSOR_LOG_ERROR("remove\n");
	kfree(data);

	return 0;
}

static int __init rgb_bh1745_init(void)
{
	return i2c_add_driver(&rgb_bh1745_driver);
}

static void __exit rgb_bh1745_exit(void)
{
	if (rgb_bh1745_workqueue) {
		destroy_workqueue(rgb_bh1745_workqueue);
		rgb_bh1745_workqueue = NULL;
	}

	i2c_del_driver(&rgb_bh1745_driver);
}

MODULE_DESCRIPTION("BH1745 ambient light sensor driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);

module_init(rgb_bh1745_init);
module_exit(rgb_bh1745_exit);
