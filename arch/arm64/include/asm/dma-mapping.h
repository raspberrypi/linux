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
#include <linux/vmalloc.h>

#include <xen/xen.h>
#include <asm/xen/hypervisor.h>

extern void *arm64_dma_alloc(struct device *dev, size_t size, dma_addr_t *handle,
			   gfp_t gfp, unsigned long attrs);
extern void arm64_dma_free(struct device *dev, size_t size, void *cpu_addr,
			 dma_addr_t handle, unsigned long attrs);
extern int arm64_dma_mmap(struct device *dev, struct vm_area_struct *vma,
			void *cpu_addr, dma_addr_t dma_addr, size_t size,
			unsigned long attrs);
extern int arm64_dma_get_sgtable(struct device *dev, struct sg_table *sgt,
		void *cpu_addr, dma_addr_t dma_addr, size_t size,
		unsigned long attrs);
extern int arm64_dma_map_sg(struct device *dev, struct scatterlist *sgl, int nelems,
		enum dma_data_direction dir, unsigned long attrs);
extern void arm64_dma_unmap_sg(struct device *dev, struct scatterlist *sgl, int,
		enum dma_data_direction dir, unsigned long attrs);
extern void arm64_dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sgl, int nelems,
		enum dma_data_direction dir);
extern void arm64_dma_sync_sg_for_device(struct device *dev, struct scatterlist *sgl, int nelems,
		enum dma_data_direction dir);



extern const struct dma_map_ops dummy_dma_ops;

static inline const struct dma_map_ops *get_arch_dma_ops(struct bus_type *bus)
{
	/*
	 * We expect no ISA devices, and all other DMA masters are expected to
	 * have someone call arch_setup_dma_ops at device creation time.
	 */
	return &dummy_dma_ops;
}

void arch_setup_dma_ops(struct device *dev, u64 dma_base, u64 size,
			const struct iommu_ops *iommu, bool coherent);
#define arch_setup_dma_ops	arch_setup_dma_ops

#ifdef CONFIG_IOMMU_DMA
void arch_teardown_dma_ops(struct device *dev);
#define arch_teardown_dma_ops	arch_teardown_dma_ops
#endif

/* do not use this function in a driver */
static inline bool is_device_dma_coherent(struct device *dev)
{
	return dev->archdata.dma_coherent;
}

#endif	/* __KERNEL__ */
#endif	/* __ASM_DMA_MAPPING_H */
