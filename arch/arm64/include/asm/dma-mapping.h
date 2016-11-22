/*
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_DMA_MAPPING_H
#define __ASM_DMA_MAPPING_H

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/dma-attrs.h>
#include <linux/dma-debug.h>

#include <asm/memory.h>

#include <xen/xen.h>
#include <asm/xen/hypervisor.h>

#define DMA_ERROR_CODE	(~(dma_addr_t)0)
extern struct dma_map_ops dummy_dma_ops;

static inline struct dma_map_ops *__generic_dma_ops(struct device *dev)
{
	if (dev && dev->archdata.dma_ops)
		return dev->archdata.dma_ops;

	/*
	 * We expect no ISA devices, and all other DMA masters are expected to
	 * have someone call arch_setup_dma_ops at device creation time.
	 */
	return &dummy_dma_ops;
}

static inline struct dma_map_ops *get_dma_ops(struct device *dev)
{
	if (xen_initial_domain())
		return xen_dma_ops;
	else
		return __generic_dma_ops(dev);
}

void arch_setup_dma_ops(struct device *dev, u64 dma_base, u64 size,
			struct iommu_ops *iommu, bool coherent);
#define arch_setup_dma_ops	arch_setup_dma_ops

#ifdef CONFIG_IOMMU_DMA
void arch_teardown_dma_ops(struct device *dev);
#define arch_teardown_dma_ops	arch_teardown_dma_ops
#endif

/*
 * dma_to_pfn/pfn_to_dma/dma_to_virt/virt_to_dma are architecture private
 * functions used internally by the DMA-mapping API to provide DMA
 * addresses. They must not be used by drivers.
 */
#ifndef __arch_pfn_to_dma
static inline dma_addr_t pfn_to_dma(struct device *dev, unsigned long pfn)
{
	if (dev)
		pfn -= dev->dma_pfn_offset;
	return (dma_addr_t)__pfn_to_bus(pfn);
}

static inline unsigned long dma_to_pfn(struct device *dev, dma_addr_t addr)
{
	unsigned long pfn = __bus_to_pfn(addr);

	if (dev)
		pfn += dev->dma_pfn_offset;

	return pfn;
}

static inline void *dma_to_virt(struct device *dev, dma_addr_t addr)
{
	if (dev) {
		unsigned long pfn = dma_to_pfn(dev, addr);

		return phys_to_virt(__pfn_to_phys(pfn));
	}

	return (void *)__bus_to_virt((unsigned long)addr);
}

static inline dma_addr_t virt_to_dma(struct device *dev, void *addr)
{
	if (dev)
		return pfn_to_dma(dev, virt_to_pfn(addr));

	return (dma_addr_t)__virt_to_bus((unsigned long)(addr));
}

#else
static inline dma_addr_t pfn_to_dma(struct device *dev, unsigned long pfn)
{
	return __arch_pfn_to_dma(dev, pfn);
}

static inline unsigned long dma_to_pfn(struct device *dev, dma_addr_t addr)
{
	return __arch_dma_to_pfn(dev, addr);
}

static inline void *dma_to_virt(struct device *dev, dma_addr_t addr)
{
	return __arch_dma_to_virt(dev, addr);
}

static inline dma_addr_t virt_to_dma(struct device *dev, void *addr)
{
	return __arch_virt_to_dma(dev, addr);
}
#endif

/* The ARM override for dma_max_pfn() */
static inline unsigned long dma_max_pfn(struct device *dev)
{
	return PHYS_PFN_OFFSET + dma_to_pfn(dev, *dev->dma_mask);
}
#define dma_max_pfn(dev) dma_max_pfn(dev)

#define arch_setup_dma_ops arch_setup_dma_ops
extern void arch_setup_dma_ops(struct device *dev, u64 dma_base, u64 size,
			       struct iommu_ops *iommu, bool coherent);

/* do not use this function in a driver */
static inline bool is_device_dma_coherent(struct device *dev)
{
	if (!dev)
		return false;
	return dev->archdata.dma_coherent;
}

static inline dma_addr_t phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	unsigned int offset = paddr & ~PAGE_MASK;
	return pfn_to_dma(dev, __phys_to_pfn(paddr)) + offset;
}

static inline phys_addr_t dma_to_phys(struct device *dev, dma_addr_t dev_addr)
{
	unsigned int offset = dev_addr & ~PAGE_MASK;
	return __pfn_to_phys(dma_to_pfn(dev, dev_addr)) + offset;
}

static inline bool dma_capable(struct device *dev, dma_addr_t addr, size_t size)
{
	u64 limit, mask;

	if (!dev->dma_mask)
		return 0;

	mask = *dev->dma_mask;

	limit = (mask + 1) & ~mask;
	if (limit && size > limit)
		return 0;

	if ((addr | (addr + size - 1)) & ~mask)
		return 0;

	return 1;
}

static inline void dma_mark_clean(void *addr, size_t size) { }

#endif	/* __KERNEL__ */
#endif	/* __ASM_DMA_MAPPING_H */
