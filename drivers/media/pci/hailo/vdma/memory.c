// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
 **/

#define pr_fmt(fmt) "hailo: " fmt

#include "memory.h"
#include "utils/compact.h"

#include <linux/highmem-internal.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>

#define SGL_MAX_SEGMENT_SIZE 	(0x10000)
// See linux/mm.h
#define MMIO_AND_NO_PAGES_VMA_MASK (VM_IO | VM_PFNMAP)

static int map_mmio_address(void __user* user_address, u32 size, struct vm_area_struct *vma,
    struct sg_table *sgt);
static int prepare_sg_table(struct sg_table *sg_table, void __user* user_address, u32 size,
    struct hailo_vdma_low_memory_buffer *low_mem_driver_allocated_buffer);
static void clear_sg_table(struct sg_table *sgt);

struct hailo_vdma_buffer *hailo_vdma_buffer_map(struct device *dev,
    void __user *user_address, size_t size, enum dma_data_direction direction,
    struct hailo_vdma_low_memory_buffer *low_mem_driver_allocated_buffer)
{
    int ret = -EINVAL;
    struct hailo_vdma_buffer *mapped_buffer = NULL;
    struct sg_table sgt = {0};
    struct vm_area_struct *vma = NULL;
    bool is_mmio = false;

    mapped_buffer = kzalloc(sizeof(*mapped_buffer), GFP_KERNEL);
    if (NULL == mapped_buffer) {
        dev_err(dev, "memory alloc failed\n");
        ret = -ENOMEM;
        goto cleanup;
    }

    if (IS_ENABLED(HAILO_SUPPORT_MMIO_DMA_MAPPING)) {
        vma = find_vma(current->mm, (uintptr_t)user_address);
        if (NULL == vma) {
            dev_err(dev, "no vma for virt_addr/size = 0x%08lx/0x%08zx\n", (uintptr_t)user_address, size);
            ret = -EFAULT;
            goto cleanup;
        }
    }

    if (IS_ENABLED(HAILO_SUPPORT_MMIO_DMA_MAPPING) &&
            (MMIO_AND_NO_PAGES_VMA_MASK == (vma->vm_flags & MMIO_AND_NO_PAGES_VMA_MASK))) {
        // user_address represents memory mapped I/O and isn't backed by 'struct page' (only by pure pfn)
        if (NULL != low_mem_driver_allocated_buffer) {
            // low_mem_driver_allocated_buffer are backed by regular 'struct page' addresses, just in low memory
            dev_err(dev, "low_mem_driver_allocated_buffer shouldn't be provided with an mmio address\n");
            ret = -EINVAL;
            goto free_buffer_struct;
        }

        ret = map_mmio_address(user_address, size, vma, &sgt);
        if (ret < 0) {
            dev_err(dev, "failed to map mmio address %d\n", ret);
            goto free_buffer_struct;
        }

        is_mmio = true;
    } else {
        // user_address is a standard 'struct page' backed memory address
        ret = prepare_sg_table(&sgt, user_address, size, low_mem_driver_allocated_buffer);
        if (ret < 0) {
            dev_err(dev, "failed to set sg list for user buffer %d\n", ret);
            goto free_buffer_struct;
        }
        sgt.nents = dma_map_sg(dev, sgt.sgl, sgt.orig_nents, direction);
        if (0 == sgt.nents) {
            dev_err(dev, "failed to map sg list for user buffer\n");
            ret = -ENXIO;
            goto clear_sg_table;
        }
    }

    kref_init(&mapped_buffer->kref);
    mapped_buffer->device = dev;
    mapped_buffer->user_address = user_address;
    mapped_buffer->size = size;
    mapped_buffer->data_direction = direction;
    mapped_buffer->sg_table = sgt;
    mapped_buffer->is_mmio = is_mmio;

    return mapped_buffer;

clear_sg_table:
    clear_sg_table(&sgt);
free_buffer_struct:
    kfree(mapped_buffer);
cleanup:
    return ERR_PTR(ret);
}

static void unmap_buffer(struct kref *kref)
{
    struct hailo_vdma_buffer *buf = container_of(kref, struct hailo_vdma_buffer, kref);

    if (!buf->is_mmio) {
        dma_unmap_sg(buf->device, buf->sg_table.sgl, buf->sg_table.orig_nents, buf->data_direction);
    }

    clear_sg_table(&buf->sg_table);
    kfree(buf);
}

void hailo_vdma_buffer_get(struct hailo_vdma_buffer *buf)
{
    kref_get(&buf->kref);
}

void hailo_vdma_buffer_put(struct hailo_vdma_buffer *buf)
{
    kref_put(&buf->kref, unmap_buffer);
}

static void vdma_sync_entire_buffer(struct hailo_vdma_controller *controller,
    struct hailo_vdma_buffer *mapped_buffer, enum hailo_vdma_buffer_sync_type sync_type)
{
    if (sync_type == HAILO_SYNC_FOR_CPU) {
        dma_sync_sg_for_cpu(controller->dev, mapped_buffer->sg_table.sgl, mapped_buffer->sg_table.nents,
            mapped_buffer->data_direction);
    } else {
        dma_sync_sg_for_device(controller->dev, mapped_buffer->sg_table.sgl, mapped_buffer->sg_table.nents,
            mapped_buffer->data_direction);
    }
}

typedef void (*dma_sync_single_callback)(struct device *, dma_addr_t, size_t, enum dma_data_direction);
// Map sync_info->count bytes starting at sync_info->offset
static void vdma_sync_buffer_interval(struct hailo_vdma_controller *controller,
    struct hailo_vdma_buffer *mapped_buffer,
    size_t offset, size_t size, enum hailo_vdma_buffer_sync_type sync_type)
{
    size_t sync_start_offset = offset;
    size_t sync_end_offset = offset + size;
    dma_sync_single_callback dma_sync_single = (sync_type == HAILO_SYNC_FOR_CPU) ?
        dma_sync_single_for_cpu :
        dma_sync_single_for_device;
    struct scatterlist* sg_entry = NULL;
    size_t current_iter_offset = 0;
    int i = 0;

    for_each_sg(mapped_buffer->sg_table.sgl, sg_entry, mapped_buffer->sg_table.nents, i) {
        // Check if the intervals: [current_iter_offset, sg_dma_len(sg_entry)] and [sync_start_offset, sync_end_offset]
        // have any intersection. If offset isn't at the start of a sg_entry, we still want to sync it.
        if (max(sync_start_offset, current_iter_offset) <= min(sync_end_offset, current_iter_offset + sg_dma_len(sg_entry))) {
            dma_sync_single(controller->dev, sg_dma_address(sg_entry), sg_dma_len(sg_entry),
                mapped_buffer->data_direction);
        }

        current_iter_offset += sg_dma_len(sg_entry);
    }
}

void hailo_vdma_buffer_sync(struct hailo_vdma_controller *controller,
    struct hailo_vdma_buffer *mapped_buffer, enum hailo_vdma_buffer_sync_type sync_type,
    size_t offset, size_t size)
{
    if (IS_ENABLED(HAILO_SUPPORT_MMIO_DMA_MAPPING) && mapped_buffer->is_mmio) {
        // MMIO buffers don't need to be sync'd
        return;
    }

    if ((offset == 0) && (size == mapped_buffer->size)) {
        vdma_sync_entire_buffer(controller, mapped_buffer, sync_type);
    } else {
        vdma_sync_buffer_interval(controller, mapped_buffer, offset, size, sync_type);
    }
}

// Similar to vdma_buffer_sync, allow circular sync of the buffer.
void hailo_vdma_buffer_sync_cyclic(struct hailo_vdma_controller *controller,
    struct hailo_vdma_buffer *mapped_buffer, enum hailo_vdma_buffer_sync_type sync_type,
    size_t offset, size_t size)
{
    size_t size_to_end = min(size, mapped_buffer->size - offset);

    hailo_vdma_buffer_sync(controller, mapped_buffer, sync_type, offset, size_to_end);

    if (size_to_end < size) {
        hailo_vdma_buffer_sync(controller, mapped_buffer, sync_type, 0, size - size_to_end);
    }
}

struct hailo_vdma_buffer* hailo_vdma_find_mapped_user_buffer(struct hailo_vdma_file_context *context,
    size_t buffer_handle)
{
    struct hailo_vdma_buffer *cur = NULL;
    list_for_each_entry(cur, &context->mapped_user_buffer_list, mapped_user_buffer_list) {
        if (cur->handle == buffer_handle) {
            return cur;
        }
    }
    return NULL;
}

void hailo_vdma_clear_mapped_user_buffer_list(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller)
{
    struct hailo_vdma_buffer *cur = NULL, *next = NULL;
    list_for_each_entry_safe(cur, next, &context->mapped_user_buffer_list, mapped_user_buffer_list) {
        list_del(&cur->mapped_user_buffer_list);
        hailo_vdma_buffer_put(cur);
    }
}


int hailo_desc_list_create(struct device *dev, u32 descriptors_count, u16 desc_page_size,
    uintptr_t desc_handle, bool is_circular, struct hailo_descriptors_list_buffer *descriptors)
{
    size_t buffer_size = 0;
    const u64 align = VDMA_DESCRIPTOR_LIST_ALIGN; //First addr must be aligned on 64 KB  (from the VDMA registers documentation)

    buffer_size = descriptors_count * sizeof(struct hailo_vdma_descriptor);
    buffer_size = ALIGN(buffer_size, align);

    descriptors->kernel_address = dma_alloc_coherent(dev, buffer_size,
        &descriptors->dma_address, GFP_KERNEL | __GFP_ZERO);
    if (descriptors->kernel_address == NULL) {
        dev_err(dev, "Failed to allocate descriptors list, desc_count 0x%x, buffer_size 0x%zx, This failure means there is not a sufficient amount of CMA memory "
            "(contiguous physical memory), This usually is caused by lack of general system memory. Please check you have sufficent memory.\n",
            descriptors_count, buffer_size);
        return -ENOMEM;
    }

    descriptors->buffer_size = buffer_size;
    descriptors->handle = desc_handle;

    descriptors->desc_list.desc_list = descriptors->kernel_address;
    descriptors->desc_list.desc_count = descriptors_count;
    descriptors->desc_list.desc_page_size = desc_page_size;
    descriptors->desc_list.is_circular = is_circular;

    return 0;
}

void hailo_desc_list_release(struct device *dev, struct hailo_descriptors_list_buffer *descriptors)
{
    dma_free_coherent(dev, descriptors->buffer_size, descriptors->kernel_address, descriptors->dma_address);
}

struct hailo_descriptors_list_buffer* hailo_vdma_find_descriptors_buffer(struct hailo_vdma_file_context *context,
    uintptr_t desc_handle)
{
    struct hailo_descriptors_list_buffer *cur = NULL;
    list_for_each_entry(cur, &context->descriptors_buffer_list, descriptors_buffer_list) {
        if (cur->handle == desc_handle) {
            return cur;
        }
    }
    return NULL;
}

void hailo_vdma_clear_descriptors_buffer_list(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller)
{
    struct hailo_descriptors_list_buffer *cur = NULL, *next = NULL;
    list_for_each_entry_safe(cur, next, &context->descriptors_buffer_list, descriptors_buffer_list) {
        list_del(&cur->descriptors_buffer_list);
        hailo_desc_list_release(controller->dev, cur);
        kfree(cur);
    }
}

int hailo_vdma_low_memory_buffer_alloc(size_t size, struct hailo_vdma_low_memory_buffer *low_memory_buffer)
{
    int ret = -EINVAL;
    void *kernel_address = NULL;
    size_t pages_count = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    size_t num_allocated = 0, i = 0;
    void **pages = NULL;

    pages = kcalloc(pages_count, sizeof(*pages), GFP_KERNEL);
    if (NULL == pages) {
        pr_err("Failed to allocate pages for buffer (size %zu)\n", size);
        ret = -ENOMEM;
        goto cleanup;
    }

    for (num_allocated = 0; num_allocated < pages_count; num_allocated++) {
        // __GFP_DMA32 flag is used to limit system memory allocations to the lowest 4 GB of physical memory in order to guarantee DMA 
        // Operations will not have to use bounce buffers on certain architectures (e.g 32-bit DMA enabled architectures)
        kernel_address = (void*)__get_free_page(__GFP_DMA32);
        if (NULL == kernel_address) {
            pr_err("Failed to allocate %zu coherent bytes\n", (size_t)PAGE_SIZE);
            ret = -ENOMEM;
            goto cleanup;
        }

        pages[num_allocated] = kernel_address;
    }

    low_memory_buffer->pages_count = pages_count;
    low_memory_buffer->pages_address = pages;

    return 0;

cleanup:
    if (NULL != pages) {
        for (i = 0; i < num_allocated; i++) {
            free_page((long unsigned)pages[i]);
        }

        kfree(pages);
    }

    return ret;
}

void hailo_vdma_low_memory_buffer_free(struct hailo_vdma_low_memory_buffer *low_memory_buffer)
{
    size_t i = 0;
    if (NULL == low_memory_buffer) {
        return;
    }

    for (i = 0; i < low_memory_buffer->pages_count; i++) {
        free_page((long unsigned)low_memory_buffer->pages_address[i]);
    }

    kfree(low_memory_buffer->pages_address);
}

struct hailo_vdma_low_memory_buffer* hailo_vdma_find_low_memory_buffer(struct hailo_vdma_file_context *context,
    uintptr_t buf_handle)
{
    struct hailo_vdma_low_memory_buffer *cur = NULL;
    list_for_each_entry(cur, &context->vdma_low_memory_buffer_list, vdma_low_memory_buffer_list) {
        if (cur->handle == buf_handle) {
            return cur;
        }
    }

    return NULL;
}

void hailo_vdma_clear_low_memory_buffer_list(struct hailo_vdma_file_context *context)
{
    struct hailo_vdma_low_memory_buffer *cur = NULL, *next = NULL;
    list_for_each_entry_safe(cur, next, &context->vdma_low_memory_buffer_list, vdma_low_memory_buffer_list) {
        list_del(&cur->vdma_low_memory_buffer_list);
        hailo_vdma_low_memory_buffer_free(cur);
        kfree(cur);
    }
}

int hailo_vdma_continuous_buffer_alloc(struct device *dev, size_t size,
    struct hailo_vdma_continuous_buffer *continuous_buffer)
{
    dma_addr_t dma_address = 0;
    void *kernel_address = NULL;

    kernel_address = dma_alloc_coherent(dev, size, &dma_address, GFP_KERNEL);
    if (NULL == kernel_address) {
        dev_warn(dev, "Failed to allocate continuous buffer, size 0x%zx. This failure means there is not a sufficient amount of CMA memory "
            "(contiguous physical memory), This usually is caused by lack of general system memory. Please check you have sufficent memory.\n", size);
        return -ENOMEM;
    }

    continuous_buffer->kernel_address = kernel_address;
    continuous_buffer->dma_address = dma_address;
    continuous_buffer->size = size;
    return 0;
}

void hailo_vdma_continuous_buffer_free(struct device *dev,
    struct hailo_vdma_continuous_buffer *continuous_buffer)
{
    dma_free_coherent(dev, continuous_buffer->size, continuous_buffer->kernel_address,
        continuous_buffer->dma_address);
}

struct hailo_vdma_continuous_buffer* hailo_vdma_find_continuous_buffer(struct hailo_vdma_file_context *context,
    uintptr_t buf_handle)
{
    struct hailo_vdma_continuous_buffer *cur = NULL;
    list_for_each_entry(cur, &context->continuous_buffer_list, continuous_buffer_list) {
        if (cur->handle == buf_handle) {
            return cur;
        }
    }

    return NULL;
}

void hailo_vdma_clear_continuous_buffer_list(struct hailo_vdma_file_context *context,
    struct hailo_vdma_controller *controller)
{
    struct hailo_vdma_continuous_buffer *cur = NULL, *next = NULL;
    list_for_each_entry_safe(cur, next, &context->continuous_buffer_list, continuous_buffer_list) {
        list_del(&cur->continuous_buffer_list);
        hailo_vdma_continuous_buffer_free(controller->dev, cur);
        kfree(cur);
    }
}

/**
 * follow_pfn - look up PFN at a user virtual address
 * @vma: memory mapping
 * @address: user virtual address
 * @pfn: location to store found PFN
 *
 * Only IO mappings and raw PFN mappings are allowed.
 *
 * This function does not allow the caller to read the permissions
 * of the PTE.  Do not use it.
 *
 * Return: zero and the pfn at @pfn on success, -ve otherwise.
 */
static int follow_pfn(struct vm_area_struct *vma, unsigned long address,
       unsigned long *pfn)
{
       int ret = -EINVAL;
       spinlock_t *ptl;
       pte_t *ptep;

       if (!(vma->vm_flags & (VM_IO | VM_PFNMAP)))
               return ret;

       ret = follow_pte(vma, address, &ptep, &ptl);
       if (ret)
               return ret;
       *pfn = pte_pfn(ptep_get(ptep));
       pte_unmap_unlock(ptep, ptl);
       return 0;
}


// Assumes the provided user_address belongs to the vma and that MMIO_AND_NO_PAGES_VMA_MASK bits are set under
// vma->vm_flags. This is validated in hailo_vdma_buffer_map, and won't be checked here
static int map_mmio_address(void __user* user_address, u32 size, struct vm_area_struct *vma,
    struct sg_table *sgt)
{
    int ret = -EINVAL;
    unsigned long i = 0;
    unsigned long pfn = 0;
    unsigned long next_pfn = 0;
    phys_addr_t phys_addr = 0;
    dma_addr_t mmio_dma_address = 0;
    const uintptr_t virt_addr = (uintptr_t)user_address;
    const u32 vma_size = vma->vm_end - vma->vm_start + 1;
    const uintptr_t num_pages = PFN_UP(virt_addr + size) - PFN_DOWN(virt_addr);

    // Check that the vma that was marked as MMIO_AND_NO_PAGES_VMA_MASK is big enough
    if (vma_size < size) {
        pr_err("vma (%u bytes) smaller than provided buffer (%u bytes)\n", vma_size, size);
        return -EINVAL;
    }

    // Get the physical address of user_address
    ret = follow_pfn(vma, virt_addr, &pfn);
    if (ret) {
        pr_err("follow_pfn failed with %d\n", ret);
        return ret;
    }
    phys_addr = __pfn_to_phys(pfn) + offset_in_page(virt_addr);

    // Make sure the physical memory is contiguous
    for (i = 1; i < num_pages; ++i) {
        ret = follow_pfn(vma, virt_addr + (i << PAGE_SHIFT), &next_pfn);
        if (ret < 0) {
            pr_err("follow_pfn failed with %d\n", ret);
            return ret;
        }
        if (next_pfn != pfn + 1) {
            pr_err("non-contiguous physical memory\n");
            return -EFAULT;
        }
        pfn = next_pfn;
    }

    // phys_addr to dma
    // TODO: need dma_map_resource here? doesn't work currently (we get dma_mapping_error on the returned dma addr)
    //       (HRT-12521)
    mmio_dma_address = (dma_addr_t)phys_addr;

    // Create a page-less scatterlist.
    ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
    if (ret < 0) {
        return ret;
    }

    sg_assign_page(sgt->sgl, NULL);
    sg_dma_address(sgt->sgl) = mmio_dma_address;
    sg_dma_len(sgt->sgl) = size;

    return 0;
}

static int prepare_sg_table(struct sg_table *sg_table, void __user *user_address, u32 size,
    struct hailo_vdma_low_memory_buffer *low_mem_driver_allocated_buffer)
{
    int ret = -EINVAL;
    int pinned_pages = 0;
    size_t npages = 0;
    struct page **pages = NULL;
    int i = 0;
    struct scatterlist *sg_alloc_res = NULL;

    npages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
    if (!pages) {
        return -ENOMEM;
    }

    // Check whether mapping user allocated buffer or driver allocated low memory buffer
    if (NULL == low_mem_driver_allocated_buffer) {
        mmap_read_lock(current->mm);
        pinned_pages = get_user_pages_compact((unsigned long)user_address,
            npages, FOLL_WRITE | FOLL_FORCE, pages);
        mmap_read_unlock(current->mm);

        if (pinned_pages < 0) {
            pr_err("get_user_pages failed with %d\n", pinned_pages);
            ret = pinned_pages;
            goto exit;
        } else if (pinned_pages != npages) {
            pr_err("Pinned %d out of %zu\n", pinned_pages, npages);
            ret = -EINVAL;
            goto release_pages;
        }
    } else {
        // Check to make sure in case user provides wrong buffer
        if (npages != low_mem_driver_allocated_buffer->pages_count) {
            pr_err("Received wrong amount of pages %zu to map expected %zu\n",
                npages, low_mem_driver_allocated_buffer->pages_count);
            ret = -EINVAL;
            goto exit;
        }

        for (i = 0; i < npages; i++) {
            pages[i] = virt_to_page(low_mem_driver_allocated_buffer->pages_address[i]);
            get_page(pages[i]);
        }
    }

    sg_alloc_res = sg_alloc_table_from_pages_segment_compat(sg_table, pages, npages,
        0, size, SGL_MAX_SEGMENT_SIZE, NULL, 0, GFP_KERNEL);
    if (IS_ERR(sg_alloc_res)) {
        ret = PTR_ERR(sg_alloc_res);
        pr_err("sg table alloc failed (err %d)..\n", ret);
        goto release_pages;
    }

    ret = 0;
    goto exit;
release_pages:
    for (i = 0; i < pinned_pages; i++) {
        if (!PageReserved(pages[i])) {
            SetPageDirty(pages[i]);
        }
        put_page(pages[i]);
    }
exit:
    kvfree(pages);
    return ret;
}

static void clear_sg_table(struct sg_table *sgt)
{
    struct sg_page_iter iter;
    struct page *page = NULL;

    for_each_sg_page(sgt->sgl, &iter, sgt->orig_nents, 0) {
        page = sg_page_iter_page(&iter);
        if (page) {
            if (!PageReserved(page)) {
                SetPageDirty(page);
            }
            put_page(page);
        }
    }

    sg_free_table(sgt);
}
