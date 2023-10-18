// SPDX-License-Identifier: GPL-2.0+
/*
 * Watchdog driver for Broadcom bcm2711
 *
 * Based on bcm2835_wdt.c by Lubomir Rintel <lkundrak@v3.sk>
 *
 * Copyright (C) 2023 Dom Cobley <popcornmix@gmail.com>
 *
 */

#include <linux/delay.h>
#include <linux/types.h>
#include <linux/mfd/bcm2835-pm.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/watchdog.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>



#define TIMER_WDTIMEOUT			0x28
#define TIMER_WDCMD			0x2c
#define TIMER_WDCHIPRST_CNT		0x30
#define TIMER_WDCTRL			0x3c

#define PM_PASSWORD			0x5a000000
#define PM_RSTS_PARTITION_CLR		0xfffffaaa

#define PM_RSTC                         0x1c
#define PM_RSTS                         0x20

#define OSC 27000000
#define SECS_TO_WDOG_TICKS(x) ((x) * OSC)
#define WDOG_TICKS_TO_SECS(x) ((x) / OSC)

struct bcm2711_wdt {
	void __iomem		*base;
	void __iomem		*pm_base;
	spinlock_t		lock;
};

static struct bcm2711_wdt *bcm2711_power_off_wdt;

static unsigned int heartbeat;
static bool nowayout = WATCHDOG_NOWAYOUT;

static bool bcm2711_wdt_is_running(struct bcm2711_wdt *wdt)
{
	return readl(wdt->base + TIMER_WDCTRL) != 0;
}

static int bcm2711_wdt_start(struct watchdog_device *wdog)
{
	struct bcm2711_wdt *wdt = watchdog_get_drvdata(wdog);
	unsigned long flags;

	spin_lock_irqsave(&wdt->lock, flags);

	writel_relaxed(SECS_TO_WDOG_TICKS(wdog->timeout), wdt->base + TIMER_WDTIMEOUT);

	/* special sequence to start watchdog */
	writel_relaxed(0xff00, wdt->base + TIMER_WDCMD);
	writel_relaxed(0x00ff, wdt->base + TIMER_WDCMD);

	spin_unlock_irqrestore(&wdt->lock, flags);

	return 0;
}

static int bcm2711_wdt_stop(struct watchdog_device *wdog)
{
	struct bcm2711_wdt *wdt = watchdog_get_drvdata(wdog);

	/* special sequence to stop watchdog */
	writel_relaxed(0xee00, wdt->base + TIMER_WDCMD);
	writel_relaxed(0x00ee, wdt->base + TIMER_WDCMD);

	return 0;
}

static unsigned int bcm2711_wdt_get_timeleft(struct watchdog_device *wdog)
{
	struct bcm2711_wdt *wdt = watchdog_get_drvdata(wdog);
	uint32_t ret;

	ret = readl_relaxed(wdt->base + TIMER_WDTIMEOUT);
	return WDOG_TICKS_TO_SECS(ret);
}

/*
 * The Raspberry Pi firmware uses the RSTS register to know which partition
 * to boot from. The partition value is spread into bits 0, 2, 4, 6, 8, 10.
 * Partition 63 is a special partition used by the firmware to indicate halt.
 */

static int bcm2711_restart(struct watchdog_device *wdog,
			   unsigned long action, void *data)
{
	int val;
	u8 partition = 0;
	u32 rsts;

	// Allow extra arguments separated by spaces after
	// the partition number.
	if (data) {
		if (kstrtoint(data, 0, &val) == 0 && val < 63)
			partition = val;
	}
	rsts = (partition & BIT(0)) | ((partition & BIT(1)) << 1) |
	       ((partition & BIT(2)) << 2) | ((partition & BIT(3)) << 3) |
	       ((partition & BIT(4)) << 4) | ((partition & BIT(5)) << 5);

	/* use a timeout of 1 second */
	wdog->timeout = 1;
	bcm2711_wdt_start(wdog);

	/* No sleeping, possibly atomic. */
	mdelay(1000);

	return 0;
}

static const struct watchdog_ops bcm2711_wdt_ops = {
	.owner =	THIS_MODULE,
	.start =	bcm2711_wdt_start,
	.stop =		bcm2711_wdt_stop,
	.get_timeleft =	bcm2711_wdt_get_timeleft,
	.restart =	bcm2711_restart,
};

static const struct watchdog_info bcm2711_wdt_info = {
	.options =	WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE |
			WDIOF_KEEPALIVEPING,
	.identity =	"Broadcom bcm2711 Watchdog timer",
};

static struct watchdog_device bcm2711_wdt_wdd = {
	.info =		&bcm2711_wdt_info,
	.ops =		&bcm2711_wdt_ops,
	.min_timeout =	1,
	.max_timeout =	WDOG_TICKS_TO_SECS(0xffffffff),
	.timeout =	WDOG_TICKS_TO_SECS(0xffffffff),
};

/*
 * We can't really power off, but if we do the normal reset scheme, and
 * indicate to bootcode.bin not to reboot, then most of the chip will be
 * powered off.
 */
static void bcm2711_power_off(void)
{
	//struct bcm2711_wdt *wdt = bcm2711_power_off_wdt;

	/* Partition 63 tells the firmware that this is a halt */
	//__bcm2711_restart(wdt, 63);
}

static int bcm2711_wdt_probe(struct platform_device *pdev)
{
	struct bcm2835_pm *pm = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct bcm2711_wdt *wdt;
	int err;

	wdt = devm_kzalloc(dev, sizeof(struct bcm2711_wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	spin_lock_init(&wdt->lock);

	wdt->pm_base = pm->base;
	wdt->base = pm->wdt_base;

	watchdog_set_drvdata(&bcm2711_wdt_wdd, wdt);
	watchdog_init_timeout(&bcm2711_wdt_wdd, heartbeat, dev);
	watchdog_set_nowayout(&bcm2711_wdt_wdd, nowayout);
	bcm2711_wdt_wdd.parent = dev;
	if (bcm2711_wdt_is_running(wdt)) {
		/*
		 * The currently active timeout value (set by the
		 * bootloader) may be different from the module
		 * heartbeat parameter or the value in device
		 * tree. But we just need to set WDOG_HW_RUNNING,
		 * because then the framework will "immediately" ping
		 * the device, updating the timeout.
		 */
		set_bit(WDOG_HW_RUNNING, &bcm2711_wdt_wdd.status);
	}

	watchdog_set_restart_priority(&bcm2711_wdt_wdd, 128);

	watchdog_stop_on_reboot(&bcm2711_wdt_wdd);
	err = devm_watchdog_register_device(dev, &bcm2711_wdt_wdd);
	if (err)
		return err;

	if (of_device_is_system_power_controller(pdev->dev.parent->of_node)) {
		if (!pm_power_off) {
			pm_power_off = bcm2711_power_off;
			bcm2711_power_off_wdt = wdt;
		} else {
			dev_info(dev, "Poweroff handler already present!\n");
		}
	}

	dev_info(dev, "Broadcom bcm2711 watchdog timer");
	return 0;
}

static int bcm2711_wdt_remove(struct platform_device *pdev)
{
	if (pm_power_off == bcm2711_power_off)
		pm_power_off = NULL;

	return 0;
}

static struct platform_driver bcm2711_wdt_driver = {
	.probe		= bcm2711_wdt_probe,
	.remove		= bcm2711_wdt_remove,
	.driver = {
		.name =		"bcm2711-wdt",
	},
};
module_platform_driver(bcm2711_wdt_driver);

module_param(heartbeat, uint, 0);
MODULE_PARM_DESC(heartbeat, "Initial watchdog heartbeat in seconds");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

MODULE_ALIAS("platform:bcm2711-wdt");
MODULE_AUTHOR("Dom Cobley <popcornmix@gmail.com>");
MODULE_DESCRIPTION("Driver for Broadcom bcm2711 watchdog timer");
MODULE_LICENSE("GPL");
