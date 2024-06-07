// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM Driver for DSI output on Raspberry Pi RP1
 *
 * Copyright (c) 2023 Raspberry Pi Limited.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/phy/phy-mipi-dphy.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include <drm/drm_drv.h>
#include <drm/drm_managed.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>

#include "rp1_dsi.h"
#undef DRIVER_NAME
#undef MODULE_NAME
#define DRIVER_NAME "rp1-dsi-test"
#define MODULE_NAME "rp1-dsi-test"

int num_lanes = 4;
module_param_named(num_lanes, num_lanes, int, 0600);
MODULE_PARM_DESC(num_lanes, "Number of lanes to test\n");

int mbps = 200;
module_param_named(mbps, mbps, int, 0600);
MODULE_PARM_DESC(pix_clock, "Megabits per second per lane\n");

bool lpmode = false;
module_param_named(lpmode, lpmode, bool, 0600);
MODULE_PARM_DESC(pix_clock, "Force LP mode (1 lane, low speed)\n");

static void rp1dsitest_setup_dsihost(struct rp1_dsi *dsi)
{
	struct drm_display_mode mode;
	bool use24bpp = (num_lanes == 3 || num_lanes * mbps > 3200);

	/*
	 * "mode" is largely ignored, as we won't be streaming any video,
	 * but its pixel clock (together with display_format) determines
	 * the MIPI D-PHY clock and data rate. The MIPI clock should run
	 * continuously, even when we are using LP commands.
	 */
	mode.hdisplay       = 800;
	mode.hsync_start    = 832;
	mode.hsync_end      = 840;
	mode.htotal         = 900;
	mode.vdisplay       = 480;
	mode.vsync_start    = 496;
	mode.vsync_end      = 500;
	mode.vtotal         = 525;
	mode.clock          = (1000 * num_lanes * mbps) / (use24bpp ? 24 : 16);
	dsi->lanes          = num_lanes;
	dsi->display_format = use24bpp ? MIPI_DSI_FMT_RGB888 : MIPI_DSI_FMT_RGB565;
	dsi->display_flags  = lpmode ? MIPI_DSI_MODE_LPM : 0;

	drm_info(dsi->drm, "Setup lanes=%d mbps=%d bpp=%d (pixclock %d)\n",
		 num_lanes, mbps, use24bpp ? 24 : 16, mode.clock);

	if (dsi->clocks[RP1DSI_CLOCK_CFG])
		clk_prepare_enable(dsi->clocks[RP1DSI_CLOCK_CFG]);

	rp1dsi_dsi_setup(dsi, &mode);
	dsi->dsi_running = true;
}

static void rp1dsitest_teardown_dsihost(struct rp1_dsi *dsi)
{
	if (dsi) {
		if (dsi->dsi_running) {
			drm_info(dsi->drm, "Stopping DSI\n");
			rp1dsi_dsi_stop(dsi);
			dsi->dsi_running = false;

		if (dsi->clocks[RP1DSI_CLOCK_CFG])
			clk_disable_unprepare(dsi->clocks[RP1DSI_CLOCK_CFG]);
		}
	}
}

/* SYSFS interface for running tests */

static struct rp1_dsi *the_dsi;
static size_t data_size;
static char data_buf[PAGE_SIZE];
static DEFINE_MUTEX(sysfs_mutex);

static ssize_t rp1dsitest_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	size_t sz;

	mutex_lock(&sysfs_mutex);
	sz = data_size;
	printk(KERN_INFO "DSI: show %d\n", (int)sz);
	if (sz)
		memcpy(buf, data_buf, sz);

	mutex_unlock(&sysfs_mutex);
	return sz;
}

static ssize_t rp1dsitest_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	struct rp1_dsi *my_dsi;

	mutex_lock(&sysfs_mutex);

	if (count > PAGE_SIZE)
		count = PAGE_SIZE;

	memcpy(data_buf, buf, count);
	data_size = count;

	my_dsi = the_dsi;
	if (!my_dsi) {
		mutex_unlock(&sysfs_mutex);
		return -EIO;
	}

	printk(KERN_INFO "DSI: store %d\n", (int)data_size);
	if (count > 1 || (count == 1 && buf[0])) {
		if (!my_dsi->dsi_running)
			rp1dsitest_setup_dsihost(my_dsi);
		usleep_range(50, 100); /* Allow receiver to see all lanes in LP11 */
		rp1dsi_dsi_send(my_dsi, (count<<8) | 0x29, count, buf);
		usleep_range(50, 100); /* Ensure all lanes have returned to LP11 */
	} else {
		rp1dsitest_teardown_dsihost(my_dsi);
	}

	mutex_unlock(&sysfs_mutex);

	return count;
}

static struct kobj_attribute kobj_attr = __ATTR(rp1_dsi_test, 0644, rp1dsitest_show, rp1dsitest_store);

static struct attribute *attrs[] = {
	&kobj_attr.attr,
	NULL
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static struct kobject *rp1dsitest_kobj;

static struct drm_driver rp1dsitest_driver = {
	.driver_features = 0,
	.name = "rp1-dsi-test",
	.desc = "rp1-dsi-test"
};

static int rp1dsitest_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct drm_device *drm;
	struct rp1_dsi *dsi;
	int i, ret;

	drm = drm_dev_alloc(&rp1dsitest_driver, dev);
	if (IS_ERR(drm)) {
		ret = PTR_ERR(drm);
		return ret;
	}
	dsi = drmm_kzalloc(drm, sizeof(*dsi), GFP_KERNEL);
	if (!dsi) {
		ret = -ENOMEM;
		goto err_free_drm;
	}

	init_completion(&dsi->finished);
	dsi->drm = drm;
	dsi->pdev = pdev;
	drm->dev_private = dsi;
	platform_set_drvdata(pdev, drm);

	/* Hardware resources */
	for (i = 0; i < RP1DSI_NUM_CLOCKS; i++) {
		static const char * const myclocknames[RP1DSI_NUM_CLOCKS] = {
			"cfgclk", "dpiclk", "byteclk", "refclk"
		};
		dsi->clocks[i] = devm_clk_get(dev, myclocknames[i]);
		if (IS_ERR(dsi->clocks[i])) {
			ret = PTR_ERR(dsi->clocks[i]);
			dev_err(dev, "Error getting clocks[%d]\n", i);
			return ret;
		}
	}

	for (i = 0; i < RP1DSI_NUM_HW_BLOCKS; i++) {
		dsi->hw_base[i] =
			devm_ioremap_resource(dev,
					      platform_get_resource(dsi->pdev,
								    IORESOURCE_MEM,
								    i));
		if (IS_ERR(dsi->hw_base[i])) {
			ret = PTR_ERR(dsi->hw_base[i]);
			dev_err(dev, "Error memory mapping regs[%d]\n", i);
			return ret;
		}
	}
	/* We don't need interrupt or DMA - deleted those */

	/* Enable the MIPI block and set the PHY MUX for DSI */
	rp1dsi_mipicfg_setup(dsi);

	/* XXX Yuck, global variables! */
	mutex_lock(&sysfs_mutex);
	the_dsi = dsi;
	rp1dsitest_kobj = kobject_create_and_add("rp1_dsi_test", kernel_kobj);
	mutex_unlock(&sysfs_mutex);
	if (!rp1dsitest_kobj) {
		the_dsi = NULL;
		return -ENOMEM;
	}

	ret = sysfs_create_group(rp1dsitest_kobj, &attr_group);
	if (!ret)
		return 0;

	kobject_put(rp1dsitest_kobj);
	rp1dsitest_kobj = NULL;

err_free_drm:
	dev_err(dev, "%s fail %d\n", __func__, ret);
	drm_dev_put(drm);
	return ret;
	return ret;
}

static void rp1dsitest_platform_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct rp1_dsi *dsi = drm->dev_private;

	mutex_lock(&sysfs_mutex);
	the_dsi = NULL;
	if (dsi)
		rp1dsitest_teardown_dsihost(dsi);

	mutex_unlock(&sysfs_mutex);
	if (rp1dsitest_kobj) {
		kobject_put(rp1dsitest_kobj);
		rp1dsitest_kobj = NULL;
	}
}

static int rp1dsitest_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	rp1dsitest_platform_shutdown(pdev);
	drm_dev_put(drm);
	return 0;
}

static const struct of_device_id rp1dsitest_of_match[] = {
	{
		.compatible = "raspberrypi,rp1-dsi-test",
	},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, rp1dsitest_of_match);

static struct platform_driver rp1dsitest_platform_driver = {
	.probe		= rp1dsitest_platform_probe,
	.remove		= rp1dsitest_platform_remove,
	.shutdown       = rp1dsitest_platform_shutdown,
	.driver		= {
		.name	= DRIVER_NAME,
		.owner  = THIS_MODULE,
		.of_match_table = rp1dsitest_of_match,
	},
};

module_platform_driver(rp1dsitest_platform_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DSI loopback test driver for Raspberry Pi RP1");
MODULE_AUTHOR("Nick Hollinghurst");
