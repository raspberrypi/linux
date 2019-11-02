// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2012 Freescale Semiconductor, Inc.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

static unsigned long timer_interval_ns = 100000;
static struct hrtimer hr_timer;
static unsigned int timer_count = 0;

enum hrtimer_restart hrtimer_test_entry(struct hrtimer *my_timer)
{
    ktime_t currtime , interval;

    currtime  = ktime_get();
    interval = ktime_set(0,timer_interval_ns); 
    hrtimer_forward(my_timer, currtime , interval);

    if (!timer_count)
        dump_stack();

    if (timer_count % 100000 == 0)
        pr_err("lxcdebug: enter hrtimer %u , in_interrupt:%u\n",
                (unsigned int)timer_count, (unsigned int)in_interrupt());

    timer_count++;
    return HRTIMER_RESTART;
}

static void my_hrtimer_init(void)
{
    ktime_t ktime;
    ktime = ktime_set(0, timer_interval_ns);
    hrtimer_init(&hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    hr_timer.function = hrtimer_test_entry;
    hrtimer_start(&hr_timer, ms_to_ktime(10), HRTIMER_MODE_REL);
}

static ssize_t debug_status_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
    int type, order, alloc_count;
    char *memory;
    int i;
    int alloc_flag;

    sscanf(buf, "%d %d %d", &type, &order, &alloc_count);

    if (type == 0)
        alloc_flag = GFP_ATOMIC;
    else
        alloc_flag = GFP_KERNEL;

    for (i = 0; i < alloc_count; i++) {
            memory = (char *)__get_free_pages(alloc_flag, order);
            if (!memory)
                break;
    }

    if (!memory)
        pr_err("############################## alloc page failed!\n");
    
	return count;
}

static ssize_t debug_status_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sprintf(buf, "test");
}

static DEVICE_ATTR(debug_status, S_IRUSR | S_IWUSR, debug_status_show, debug_status_store);

static struct attribute *debug_status_attrs[] = {
	&dev_attr_debug_status.attr,
	NULL,
};

static const struct attribute_group debug_status_attr_group = {
	.attrs	= debug_status_attrs,
};

static int kernel_monitor_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
    int ret;

    pr_err("lxcdebug: kernel monitor probe!\n");

	ret = sysfs_create_group(&dev->kobj, &debug_status_attr_group);
    my_hrtimer_init();

	return 0;
}

static int kernel_monitor_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
    int ret;

	sysfs_remove_group(&dev->kobj, &debug_status_attr_group);
    ret = hrtimer_cancel(&hr_timer);
    if (ret)
        pr_err("cancle hrtimer failed! ret=%d\n", ret);
    pr_err("lxcdebug: module remove!\n");
	return 0;
}

static const struct of_device_id kernel_monitor_dt_ids[] = {
	{ .compatible = "lxc,kernel_monitor", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, kernel_monitor_dt_ids);

static struct platform_driver kernel_monitor_driver = {
	.driver = {
		.name = "kernel_monitor",
		.of_match_table = kernel_monitor_dt_ids,
	},
	.probe = kernel_monitor_probe,
	.remove = kernel_monitor_remove,
};
module_platform_driver(kernel_monitor_driver);

MODULE_LICENSE("GPL v2");
