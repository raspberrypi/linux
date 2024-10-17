// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _HAILO_PCI_COMPACT_H_
#define _HAILO_PCI_COMPACT_H_

#include <linux/version.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define class_create_compat class_create
#else
#define class_create_compat(name) class_create(THIS_MODULE, name)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
#define pci_printk(level, pdev, fmt, arg...) \
	dev_printk(level, &(pdev)->dev, fmt, ##arg)
#define pci_emerg(pdev, fmt, arg...)	dev_emerg(&(pdev)->dev, fmt, ##arg)
#define pci_alert(pdev, fmt, arg...)	dev_alert(&(pdev)->dev, fmt, ##arg)
#define pci_crit(pdev, fmt, arg...)	dev_crit(&(pdev)->dev, fmt, ##arg)
#define pci_err(pdev, fmt, arg...)	dev_err(&(pdev)->dev, fmt, ##arg)
#define pci_warn(pdev, fmt, arg...)	dev_warn(&(pdev)->dev, fmt, ##arg)
#define pci_notice(pdev, fmt, arg...)	dev_notice(&(pdev)->dev, fmt, ##arg)
#define pci_info(pdev, fmt, arg...)	dev_info(&(pdev)->dev, fmt, ##arg)
#define pci_dbg(pdev, fmt, arg...)	dev_dbg(&(pdev)->dev, fmt, ##arg)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
#define get_user_pages_compact get_user_pages
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
#define get_user_pages_compact(start, nr_pages, gup_flags, pages) \
    get_user_pages(start, nr_pages, gup_flags, pages, NULL)
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 168)) && (LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0))
#define get_user_pages_compact(start, nr_pages, gup_flags, pages) \
    get_user_pages(current, current->mm, start, nr_pages, gup_flags, pages, NULL)
#else
static inline long get_user_pages_compact(unsigned long start, unsigned long nr_pages,
    unsigned int gup_flags, struct page **pages)
{
    int write = !!((gup_flags & FOLL_WRITE) == FOLL_WRITE);
    int force = !!((gup_flags & FOLL_FORCE) == FOLL_FORCE);
    return get_user_pages(current, current->mm, start, nr_pages, write, force,
        pages, NULL);
}
#endif

#ifndef _LINUX_MMAP_LOCK_H
static inline void mmap_read_lock(struct mm_struct *mm)
{
    down_read(&mm->mmap_sem);
}

static inline void mmap_read_unlock(struct mm_struct *mm)
{
    up_read(&mm->mmap_sem);
}
#endif /* _LINUX_MMAP_LOCK_H */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#define sg_alloc_table_from_pages_segment_compat __sg_alloc_table_from_pages
#else
static inline struct scatterlist *sg_alloc_table_from_pages_segment_compat(struct sg_table *sgt,
    struct page **pages, unsigned int n_pages, unsigned int offset,
    unsigned long size, unsigned int max_segment,
    struct scatterlist *prv, unsigned int left_pages,
    gfp_t gfp_mask)
{
    int res = 0;

    if (NULL != prv) {
        // prv not suported
        return ERR_PTR(-EINVAL);
    }

    if (0 != left_pages) {
        // Left pages not supported
        return ERR_PTR(-EINVAL);
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
    res = sg_alloc_table_from_pages_segment(sgt, pages, n_pages, offset, size, max_segment, gfp_mask);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
    res = __sg_alloc_table_from_pages(sgt, pages, n_pages, offset, size, max_segment, gfp_mask);
#else
    res = sg_alloc_table_from_pages(sgt, pages, n_pages, offset, size, gfp_mask);
#endif
    if (res < 0) {
        return ERR_PTR(res);
    }

    return sgt->sgl;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION( 5, 0, 0 )
#define compatible_access_ok(a,b,c) access_ok(b, c)
#else
#define compatible_access_ok(a,b,c) access_ok(a, b, c)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
#define PCI_DEVICE_DATA(vend, dev, data) \
	.vendor = PCI_VENDOR_ID_##vend, .device = PCI_DEVICE_ID_##vend##_##dev, \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0, \
	.driver_data = (kernel_ulong_t)(data)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
// On kernels < 4.1.12,  kvmalloc, kvfree is not implemented. For simplicity, instead of implement our own
// kvmalloc/kvfree, just using vmalloc and vfree (It may reduce allocate/access performance, but it worth it).
static inline void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
    (void)flags; //ignore
    return vmalloc(n * size);
}

#define kvfree vfree
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
static inline bool is_dma_capable(struct device *dev, dma_addr_t dma_addr, size_t size)
{
// Case for Rasberry Pie kernel versions 5.4.83 <=> 5.5.0 - already changed bus_dma_mask -> bus_dma_limit
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 5, 0)) || (defined(HAILO_RASBERRY_PIE) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 83))
    const u64 bus_dma_limit = dev->bus_dma_limit;
#else
    const u64 bus_dma_limit = dev->bus_dma_mask;
#endif

    return (dma_addr <= min_not_zero(*dev->dma_mask, bus_dma_limit));
}
#else
static inline bool is_dma_capable(struct device *dev, dma_addr_t dma_addr, size_t size)
{
    // Implementation of dma_capable from linux kernel
    const u64 bus_dma_limit = (*dev->dma_mask + 1) & ~(*dev->dma_mask);
	if (bus_dma_limit && size > bus_dma_limit) {
        return false;
    }

	if ((dma_addr | (dma_addr + size - 1)) & ~(*dev->dma_mask)) {
        return false;
    }

    return true;
}
#endif // LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)

#endif /* _HAILO_PCI_COMPACT_H_ */