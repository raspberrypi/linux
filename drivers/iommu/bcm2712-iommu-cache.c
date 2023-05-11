// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU driver for BCM2712
 *
 * Copyright (c) 2023 Raspberry Pi Ltd.
 */

#include "bcm2712-iommu.h"

#include <linux/err.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define MMUC_CONTROL_ENABLE   1
#define MMUC_CONTROL_FLUSH    2
#define MMUC_CONTROL_FLUSHING 4

void bcm2712_iommu_cache_flush(struct bcm2712_iommu_cache *cache)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&cache->hw_lock, flags);
	if (cache->reg_base) {
		/* Enable and flush the TLB cache */
		writel(MMUC_CONTROL_ENABLE | MMUC_CONTROL_FLUSH,
		       cache->reg_base);

		/* Wait for flush to complete: it should be very quick */
		for (i = 0; i < 1024; i++) {
			if (!(MMUC_CONTROL_FLUSHING & readl(cache->reg_base)))
				break;
			cpu_relax();
		}
	}
	spin_unlock_irqrestore(&cache->hw_lock, flags);
}

static int bcm2712_iommu_cache_probe(struct platform_device *pdev)
{
	struct bcm2712_iommu_cache *cache;

	dev_info(&pdev->dev, __func__);
	cache = devm_kzalloc(&pdev->dev, sizeof(*cache), GFP_KERNEL);
	if (!cache)
		return -ENOMEM;

	cache->dev = &pdev->dev;
	platform_set_drvdata(pdev, cache);
	spin_lock_init(&cache->hw_lock);

	/* Get IOMMUC registers; we only use the first register (IOMMUC_CTRL) */
	cache->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cache->reg_base)) {
		dev_err(&pdev->dev, "Failed to get IOMMU Cache registers address\n");
		cache->reg_base = NULL;
	}
	return 0;
}

static const struct of_device_id bcm2712_iommu_cache_of_match[] = {
	{
		. compatible = "brcm,bcm2712-iommuc"
	},
	{ /* sentinel */ },
};

static struct platform_driver bcm2712_iommu_cache_driver = {
	.probe = bcm2712_iommu_cache_probe,
	.driver = {
		.name = "bcm2712-iommu-cache",
		.of_match_table = bcm2712_iommu_cache_of_match
	},
};

builtin_platform_driver(bcm2712_iommu_cache_driver);
