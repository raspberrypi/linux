/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IOMMU driver for BCM2712
 *
 * Copyright (c) 2023-2025 Raspberry Pi Ltd.
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
	u64 dma_iova_offset;  /* Hack for IOMMU attached to PCIe RC, included below */
	u64 aperture_base;    /* Base of IOVA region as passed to DMA peripherals */
	u64 aperture_end;     /* End of IOVA region as passed to DMA peripherals */
	u32 **tables;         /* For each Linux page of L2 tables, kernel virtual address */
	u32 *top_table;       /* Top-level table holding IOMMU page-numbers of L2 tables */
	u32 *default_page;    /* Page used to trap illegal reads and writes */
	struct bcm2712_iommu_cache *cache;
	spinlock_t hw_lock;   /* to protect HW registers */
	void __iomem *reg_base;
	u32 bigpage_mask;
	u32 superpage_mask;
	unsigned int nmapped_pages;
};

struct bcm2712_iommu_domain {
	struct iommu_domain base;
	struct bcm2712_iommu *mmu;
};

#endif
