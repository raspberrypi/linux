/*
 * Raspberry Pi Sense HAT framebuffer driver
 * http://raspberrypi.org
 *
 * Copyright (C) 2015 Raspberry Pi
 *
 * Author: Serge Schneider
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>

#include <linux/mfd/rpisense/framebuffer.h>
#include <linux/mfd/rpisense/core.h>

static bool lowlight;
module_param(lowlight, bool, 0);
MODULE_PARM_DESC(lowlight, "Reduce LED matrix brightness to one third");

static struct rpisense *rpisense;

struct rpisense_fb_param {
	char __iomem *vmem;
	u8 *vmem_work;
	u32 vmemsize;
	u8 *gamma;
};

static u8 gamma_default[32] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
			       0x02, 0x02, 0x03, 0x03, 0x04, 0x05, 0x06, 0x07,
			       0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0E, 0x0F, 0x11,
			       0x12, 0x14, 0x15, 0x17, 0x19, 0x1B, 0x1D, 0x1F,};

static u8 gamma_low[32] = {0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
			   0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02,
			   0x03, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06,
			   0x06, 0x07, 0x07, 0x08, 0x08, 0x09, 0x0A, 0x0A,};

static u8 gamma_user[32];

static u32 pseudo_palette[16];

static struct rpisense_fb_param rpisense_fb_param = {
	.vmem = NULL,
	.vmemsize = 128,
	.gamma = gamma_default,
};

static struct fb_deferred_io rpisense_fb_defio;

static struct fb_fix_screeninfo rpisense_fb_fix = {
	.id =		"RPi-Sense FB",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.xpanstep =	0,
	.ypanstep =	0,
	.ywrapstep =	0,
	.accel =	FB_ACCEL_NONE,
	.line_length =	16,
};

static struct fb_var_screeninfo rpisense_fb_var = {
	.xres		= 8,
	.yres		= 8,
	.xres_virtual	= 8,
	.yres_virtual	= 8,
	.bits_per_pixel = 16,
	.red		= {11, 5, 0},
	.green		= {5, 6, 0},
	.blue		= {0, 5, 0},
};

static ssize_t rpisense_fb_write(struct fb_info *info,
				 const char __user *buf, size_t count,
				 loff_t *ppos)
{
	ssize_t res = fb_sys_write(info, buf, count, ppos);

	schedule_delayed_work(&info->deferred_work, rpisense_fb_defio.delay);
	return res;
}

static void rpisense_fb_fillrect(struct fb_info *info,
				 const struct fb_fillrect *rect)
{
	sys_fillrect(info, rect);
	schedule_delayed_work(&info->deferred_work, rpisense_fb_defio.delay);
}

static void rpisense_fb_copyarea(struct fb_info *info,
				 const struct fb_copyarea *area)
{
	sys_copyarea(info, area);
	schedule_delayed_work(&info->deferred_work, rpisense_fb_defio.delay);
}

static void rpisense_fb_imageblit(struct fb_info *info,
				  const struct fb_image *image)
{
	sys_imageblit(info, image);
	schedule_delayed_work(&info->deferred_work, rpisense_fb_defio.delay);
}

static void rpisense_fb_deferred_io(struct fb_info *info,
				struct list_head *pagelist)
{
	int i;
	int j;
	u8 *vmem_work = rpisense_fb_param.vmem_work;
	u16 *mem = (u16 *)rpisense_fb_param.vmem;
	u8 *gamma = rpisense_fb_param.gamma;

	vmem_work[0] = 0;
	for (j = 0; j < 8; j++) {
		for (i = 0; i < 8; i++) {
			vmem_work[(j * 24) + i + 1] =
				gamma[(mem[(j * 8) + i] >> 11) & 0x1F];
			vmem_work[(j * 24) + (i + 8) + 1] =
				gamma[(mem[(j * 8) + i] >> 6) & 0x1F];
			vmem_work[(j * 24) + (i + 16) + 1] =
				gamma[(mem[(j * 8) + i]) & 0x1F];
		}
	}
	rpisense_block_write(rpisense, vmem_work, 193);
}

static struct fb_deferred_io rpisense_fb_defio = {
	.delay		= HZ/100,
	.deferred_io	= rpisense_fb_deferred_io,
};

static int rpisense_fb_ioctl(struct fb_info *info, unsigned int cmd,
			     unsigned long arg)
{
	switch (cmd) {
	case SENSEFB_FBIOGET_GAMMA:
		if (copy_to_user((void __user *) arg, rpisense_fb_param.gamma,
				 sizeof(u8[32])))
			return -EFAULT;
		return 0;
	case SENSEFB_FBIOSET_GAMMA:
		if (copy_from_user(gamma_user, (void __user *)arg,
				   sizeof(u8[32])))
			return -EFAULT;
		rpisense_fb_param.gamma = gamma_user;
		schedule_delayed_work(&info->deferred_work,
				      rpisense_fb_defio.delay);
		return 0;
	case SENSEFB_FBIORESET_GAMMA:
		switch (arg) {
		case 0:
			rpisense_fb_param.gamma = gamma_default;
			break;
		case 1:
			rpisense_fb_param.gamma = gamma_low;
			break;
		case 2:
			rpisense_fb_param.gamma = gamma_user;
			break;
		default:
			return -EINVAL;
		}
		schedule_delayed_work(&info->deferred_work,
				      rpisense_fb_defio.delay);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static struct fb_ops rpisense_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= rpisense_fb_write,
	.fb_fillrect	= rpisense_fb_fillrect,
	.fb_copyarea	= rpisense_fb_copyarea,
	.fb_imageblit	= rpisense_fb_imageblit,
	.fb_ioctl	= rpisense_fb_ioctl,
};

static int rpisense_fb_probe(struct platform_device *pdev)
{
	struct fb_info *info;
	int ret = -ENOMEM;
	struct rpisense_fb *rpisense_fb;

	rpisense = rpisense_get_dev();
	rpisense_fb = &rpisense->framebuffer;

	rpisense_fb_param.vmem = vzalloc(rpisense_fb_param.vmemsize);
	if (!rpisense_fb_param.vmem)
		return ret;

	rpisense_fb_param.vmem_work = devm_kmalloc(&pdev->dev, 193, GFP_KERNEL);
	if (!rpisense_fb_param.vmem_work)
		goto err_malloc;

	info = framebuffer_alloc(0, &pdev->dev);
	if (!info) {
		dev_err(&pdev->dev, "Could not allocate framebuffer.\n");
		goto err_malloc;
	}
	rpisense_fb->info = info;

	rpisense_fb_fix.smem_start = (unsigned long)rpisense_fb_param.vmem;
	rpisense_fb_fix.smem_len = rpisense_fb_param.vmemsize;

	info->fbops = &rpisense_fb_ops;
	info->fix = rpisense_fb_fix;
	info->var = rpisense_fb_var;
	info->fbdefio = &rpisense_fb_defio;
	info->flags = FBINFO_FLAG_DEFAULT | FBINFO_VIRTFB;
	info->screen_base = rpisense_fb_param.vmem;
	info->screen_size = rpisense_fb_param.vmemsize;
	info->pseudo_palette = pseudo_palette;

	if (lowlight)
		rpisense_fb_param.gamma = gamma_low;

	fb_deferred_io_init(info);

	ret = register_framebuffer(info);
	if (ret < 0) {
		dev_err(&pdev->dev, "Could not register framebuffer.\n");
		goto err_fballoc;
	}

	fb_info(info, "%s frame buffer device\n", info->fix.id);
	schedule_delayed_work(&info->deferred_work, rpisense_fb_defio.delay);
	return 0;
err_fballoc:
	framebuffer_release(info);
err_malloc:
	vfree(rpisense_fb_param.vmem);
	return ret;
}

static int rpisense_fb_remove(struct platform_device *pdev)
{
	struct rpisense_fb *rpisense_fb = &rpisense->framebuffer;
	struct fb_info *info = rpisense_fb->info;

	if (info) {
		unregister_framebuffer(info);
		fb_deferred_io_cleanup(info);
		framebuffer_release(info);
		vfree(rpisense_fb_param.vmem);
	}

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id rpisense_fb_id[] = {
	{ .compatible = "rpi,rpi-sense-fb" },
	{ },
};
MODULE_DEVICE_TABLE(of, rpisense_fb_id);
#endif

static struct platform_device_id rpisense_fb_device_id[] = {
	{ .name = "rpi-sense-fb" },
	{ },
};
MODULE_DEVICE_TABLE(platform, rpisense_fb_device_id);

static struct platform_driver rpisense_fb_driver = {
	.probe = rpisense_fb_probe,
	.remove = rpisense_fb_remove,
	.driver = {
		.name = "rpi-sense-fb",
		.owner = THIS_MODULE,
	},
};

module_platform_driver(rpisense_fb_driver);

MODULE_DESCRIPTION("Raspberry Pi Sense HAT framebuffer driver");
MODULE_AUTHOR("Serge Schneider <serge@raspberrypi.org>");
MODULE_LICENSE("GPL");

