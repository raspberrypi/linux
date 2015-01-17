/*
 * Copyright © 2014-2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * VC4 GEM BO management support.
 *
 * The VC4 GPU architecture (both scanout and rendering) has direct
 * access to system memory with no MMU in between.  To support it, we
 * use the GEM CMA helper functions to allocate contiguous ranges of
 * physical memory for our BOs.
 */

#include "vc4_drv.h"
#include "uapi/drm/vc4_drm.h"

static uint32_t
bo_page_index(size_t size)
{
	return (size / PAGE_SIZE) - 1;
}

static struct list_head *
vc4_get_cache_list_for_size(struct drm_device *dev, size_t size)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	uint32_t page_index = bo_page_index(size);

	if (vc4->bo_cache.size_list_size <= page_index) {
		uint32_t new_size = max(vc4->bo_cache.size_list_size * 2,
					page_index + 1);
		struct list_head *new_list;
		uint32_t i;

		new_list = kmalloc(new_size * sizeof(struct list_head),
				   GFP_KERNEL);
		if (!new_list)
			return NULL;

		/* Rebase the old cached BO lists to their new list
		 * head locations.
		 */
		for (i = 0; i < vc4->bo_cache.size_list_size; i++) {
			struct list_head *old_list = &vc4->bo_cache.size_list[i];
			if (list_empty(old_list))
				INIT_LIST_HEAD(&new_list[i]);
			else
				list_replace(old_list, &new_list[i]);
		}
		/* And initialize the brand new BO list heads. */
		for (i = vc4->bo_cache.size_list_size; i < new_size; i++)
			INIT_LIST_HEAD(&new_list[i]);

		kfree(vc4->bo_cache.size_list);
		vc4->bo_cache.size_list = new_list;
		vc4->bo_cache.size_list_size = new_size;
	}

	return &vc4->bo_cache.size_list[page_index];
}

struct vc4_bo *
vc4_bo_create(struct drm_device *dev, size_t size)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	uint32_t page_index = bo_page_index(size);
	struct vc4_bo *bo = NULL;
	struct drm_gem_cma_object *cma_obj;

	/* First, try to get a vc4_bo from the kernel BO cache. */
	if (vc4->bo_cache.size_list_size > page_index) {
		if (!list_empty(&vc4->bo_cache.size_list[page_index])) {
			bo = list_first_entry(&vc4->bo_cache.size_list[page_index],
					      struct vc4_bo, size_head);
			list_del(&bo->size_head);
			list_del(&bo->unref_head);
		}
	}
	if (bo) {
		kref_init(&bo->base.base.refcount);
		return bo;
	}

	/* Otherwise, make a new BO. */
	cma_obj = drm_gem_cma_create(dev, size);
	if (IS_ERR(cma_obj))
		return NULL;
	else
		return to_vc4_bo(&cma_obj->base);
}

int
vc4_dumb_create(struct drm_file *file_priv,
		struct drm_device *dev,
		struct drm_mode_create_dumb *args)
{
	int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	struct vc4_bo *bo = NULL;
	int ret;

	if (args->pitch < min_pitch)
		args->pitch = min_pitch;

	if (args->size < args->pitch * args->height)
		args->size = args->pitch * args->height;

	mutex_lock(&dev->struct_mutex);
	bo = vc4_bo_create(dev, roundup(args->size, PAGE_SIZE));
	mutex_unlock(&dev->struct_mutex);
	if (!bo)
		return -ENOMEM;

	ret = drm_gem_handle_create(file_priv, &bo->base.base, &args->handle);
	drm_gem_object_unreference_unlocked(&bo->base.base);

	return ret;
}

static void
vc4_bo_cache_free_old(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	unsigned long expire_time = jiffies - msecs_to_jiffies(1000);

	while (!list_empty(&vc4->bo_cache.time_list)) {
		struct vc4_bo *bo = list_last_entry(&vc4->bo_cache.time_list,
						    struct vc4_bo, unref_head);
		if (time_before(expire_time, bo->free_time)) {
			mod_timer(&vc4->bo_cache.time_timer,
				  round_jiffies_up(jiffies +
						   msecs_to_jiffies(1000)));
			return;
		}

		list_del(&bo->unref_head);
		list_del(&bo->size_head);
		drm_gem_cma_free_object(&bo->base.base);
	}
}

/* Called on the last userspace/kernel unreference of the BO.  Returns
 * it to the BO cache if possible, otherwise frees it.
 *
 * Note that this is called with the struct_mutex held.
 */
void
vc4_free_object(struct drm_gem_object *gem_bo)
{
	struct drm_device *dev = gem_bo->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_bo *bo = to_vc4_bo(gem_bo);
	struct list_head *cache_list;

	/* If the object references someone else's memory, we can't cache it.
	 */
	if (gem_bo->import_attach) {
		drm_gem_cma_free_object(gem_bo);
		return;
	}

	/* Don't cache if it was publicly named. */
	if (gem_bo->name) {
		drm_gem_cma_free_object(gem_bo);
		return;
	}

	cache_list = vc4_get_cache_list_for_size(dev, gem_bo->size);
	if (!cache_list) {
		drm_gem_cma_free_object(gem_bo);
		return;
	}

	kfree(bo->validated_shader);
	bo->validated_shader = NULL;

	/* If the BO was exported, and it's made it to this point,
	 * then the dmabuf usage has been completely finished (so it's
	 * safe now to let it turn into a shader again).
	 */
	bo->dma_buf_import_export = false;

	bo->free_time = jiffies;
	list_add(&bo->size_head, cache_list);
	list_add(&bo->unref_head, &vc4->bo_cache.time_list);

	vc4_bo_cache_free_old(dev);
}

static void
vc4_bo_cache_time_work(struct work_struct *work)
{
	struct vc4_dev *vc4 =
		container_of(work, struct vc4_dev, bo_cache.time_work);
	struct drm_device *dev = vc4->dev;

	mutex_lock(&dev->struct_mutex);
	vc4_bo_cache_free_old(dev);
	mutex_unlock(&dev->struct_mutex);
}

static void
vc4_bo_cache_time_timer(unsigned long data)
{
	struct drm_device *dev = (struct drm_device *)data;
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	schedule_work(&vc4->bo_cache.time_work);
}

void
vc4_bo_cache_init(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	INIT_LIST_HEAD(&vc4->bo_cache.time_list);

	INIT_WORK(&vc4->bo_cache.time_work, vc4_bo_cache_time_work);
	setup_timer(&vc4->bo_cache.time_timer,
		    vc4_bo_cache_time_timer,
		    (unsigned long) dev);
}

struct drm_gem_object *
vc4_prime_import(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct drm_gem_object *obj = drm_gem_prime_import(dev, dma_buf);

	if (!IS_ERR_OR_NULL(obj)) {
		struct vc4_bo *bo = to_vc4_bo(obj);
		bo->dma_buf_import_export = true;
	}

	return obj;
}

struct dma_buf *
vc4_prime_export(struct drm_device *dev, struct drm_gem_object *obj, int flags)
{
	struct vc4_bo *bo = to_vc4_bo(obj);

	mutex_lock(&dev->struct_mutex);
	if (bo->validated_shader) {
		mutex_unlock(&dev->struct_mutex);
		DRM_ERROR("Attempting to export shader BO\n");
		return ERR_PTR(-EINVAL);
	}
	bo->dma_buf_import_export = true;
	mutex_unlock(&dev->struct_mutex);

	return drm_gem_prime_export(dev, obj, flags);
}


/* vc4_gem_fault - fault handler for user mappings of objects.
 *
 * We don't just use the GEM helpers because we have to make sure that
 * the user can't touch shader contents while they're being executed.
 */
static int
vc4_gem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct drm_gem_object *gem_bo = vma->vm_private_data;
	struct vc4_bo *bo = to_vc4_bo(gem_bo);
	struct drm_device *dev = gem_bo->dev;
	pgoff_t page_offset;
	unsigned long pfn;
	int ret = 0;

	/* We don't use vmf->pgoff since that has the fake offset */
	page_offset = ((unsigned long)vmf->virtual_address - vma->vm_start) >>
		PAGE_SHIFT;

	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		goto out;

	if (bo->validated_shader) {
		ret = vc4_wait_for_seqno(dev, bo->seqno, ~0ull);
		if (ret)
			goto unlock;

		kfree(bo->validated_shader);
		bo->validated_shader = NULL;
	}

	pfn = (bo->base.paddr >> PAGE_SHIFT) + page_offset;

	ret = vm_insert_mixed(vma, (unsigned long)vmf->virtual_address, pfn);
unlock:
	mutex_unlock(&dev->struct_mutex);
out:
	switch (ret) {
	case 0:
	case -ERESTARTSYS:
	case -EINTR:
	case -EBUSY:
		ret = VM_FAULT_NOPAGE;
		break;
	case -ENOMEM:
		ret = VM_FAULT_OOM;
		break;
	case -ENOSPC:
	case -EFAULT:
		ret = VM_FAULT_SIGBUS;
		break;
	default:
		WARN_ONCE(ret, "unhandled error in vc4_gem_fault: %i\n", ret);
		ret = VM_FAULT_SIGBUS;
		break;
	}

	return ret;
}

const struct vm_operations_struct vc4_vm_ops = {
	.fault = vc4_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

int
vc4_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

	/* Since our objects all come from normal system memory, clear
	 * PFNMAP that was defaulted by drm_gem_mmap_obj() to indicate
	 * that they have a "struct page" managing them.
	 */
	vma->vm_flags &= ~VM_PFNMAP;

	/* Not sure why we need to do this. */
	vma->vm_flags |= VM_MIXEDMAP;

	/* We only do whole-object mappings. */
	vma->vm_pgoff = 0;

	return 0;
}

/* Removes all user mappings of the object.
 *
 * This is used to ensure that the user can't modify shaders while the
 * GPU is executing them.  If the user tries to access these unmapped
 * pages, they'll hit a pagefault and end up in vc4_gem_fault(), which
 * then can wait for execution to finish.
 */
void
vc4_force_user_unmap(struct drm_gem_object *gem_obj)
{
	struct drm_device *dev = gem_obj->dev;

	drm_vma_node_unmap(&gem_obj->vma_node, dev->anon_inode->i_mapping);
}

int
vc4_create_bo_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file_priv)
{
	struct drm_vc4_create_bo *args = data;
	struct vc4_bo *bo = NULL;
	int ret;

	args->size = roundup(args->size, PAGE_SIZE);
	if (args->size == 0)
		return -EINVAL;

	mutex_lock(&dev->struct_mutex);
	bo = vc4_bo_create(dev, args->size);
	mutex_unlock(&dev->struct_mutex);
	if (!bo)
		return -ENOMEM;

	ret = drm_gem_handle_create(file_priv, &bo->base.base, &args->handle);
	drm_gem_object_unreference_unlocked(&bo->base.base);

	return ret;
}

int
vc4_mmap_bo_ioctl(struct drm_device *dev, void *data,
		  struct drm_file *file_priv)
{
	struct drm_vc4_mmap_bo *args = data;
	struct drm_gem_object *gem_obj;

	gem_obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (!gem_obj) {
		DRM_ERROR("Failed to look up GEM BO %d\n", args->handle);
		return -EINVAL;
	}

	/* The mmap offset was set up at BO allocation time. */
	args->offset = drm_vma_node_offset_addr(&gem_obj->vma_node);

	drm_gem_object_unreference(gem_obj);
	return 0;
}
