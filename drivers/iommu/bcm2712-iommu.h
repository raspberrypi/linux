/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IOMMU driver for BCM2712
 *
 * Copyright (c) 2023 Raspberry Pi Ltd.
 */

#ifndef _BCM2712_IOMMU_H
#define _BCM2712_IOMMU_H

#include <linux/iommu.h>
#include <linux/scatterlist.h>

struct bcm2712_iommu_cache {
	struct device *dev;
	spinlock_t hw_lock; /* to protect HW registers */
	void __iomem *reg_base;
};

void bcm2712_iommu_cache_flush(struct bcm2712_iommu_cache *cache);

struct bcm2712_iommu {
	struct device *dev;
	struct iommu_device iommu;
	struct iommu_group *group;
	struct bcm2712_iommu_domain *domain;
	char const *name;
	struct sg_table *sgt; /* allocated memory for page tables */
	u32 *tables;          /* kernel mapping for page tables */
	struct bcm2712_iommu_cache *cache;
	spinlock_t hw_lock;   /* to protect HW registers */
	void __iomem *reg_base;
	u64 dma_iova_offset; /* Hack for IOMMU attached to PCIe RC */
	u32 bigpage_mask;
	u32 superpage_mask;
	unsigned int nmapped_pages;
	bool dirty; /* true when tables are oriented towards CPU */
};

struct bcm2712_iommu_domain {
	struct iommu_domain base;
	struct bcm2712_iommu *mmu;
};

#endif
