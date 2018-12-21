// SPDX-License-Identifier: GPL-2.0
/*
 * VideoCore Shared Memory driver using CMA.
 *
 * Copyright: 2018, Raspberry Pi (Trading) Ltd
 * Dave Stevenson <dave.stevenson@raspberrypi.org>
 *
 * Based on vmcs_sm driver from Broadcom Corporation for some API,
 * and taking some code for CMA/dmabuf handling from the Android Ion
 * driver (Google/Linaro).
 *
 *
 * This driver has 3 main uses:
 * 1) Allocating buffers for the kernel or userspace that can be shared with the
 *    VPU.
 * 2) Importing dmabufs from elsewhere for sharing with the VPU.
 * 3) Allocating buffers for use by the VPU.
 *
 * In the first and second cases the native handle is a dmabuf. Releasing the
 * resource inherently comes from releasing the dmabuf, and this will trigger
 * unmapping on the VPU. The underlying allocation and our buffer structure are
 * retained until the VPU has confirmed that it has finished with it.
 *
 * For the VPU allocations the VPU is responsible for triggering the release,
 * and therefore the released message decrements the dma_buf refcount (with the
 * VPU mapping having already been marked as released).
 */

/* ---- Include Files ----------------------------------------------------- */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/syscalls.h>
#include <linux/types.h>

#include "vchiq_connected.h"
#include "vc_sm_cma_vchi.h"

#include "vc_sm.h"
#include "vc_sm_cma.h"
#include "vc_sm_knl.h"

/* ---- Private Constants and Types --------------------------------------- */

#define DEVICE_NAME		"vcsm-cma"
#define DEVICE_MINOR		0

#define VC_SM_RESOURCE_NAME_DEFAULT       "sm-host-resource"

#define VC_SM_DIR_ROOT_NAME	"vcsm-cma"
#define VC_SM_STATE		"state"

/* Private file data associated with each opened device. */
struct vc_sm_privdata_t {
	pid_t pid;                      /* PID of creator. */

	int restart_sys;		/* Tracks restart on interrupt. */
	enum vc_sm_msg_type int_action;	/* Interrupted action. */
	u32 int_trans_id;		/* Interrupted transaction. */
};

typedef int (*VC_SM_SHOW) (struct seq_file *s, void *v);
struct sm_pde_t {
	VC_SM_SHOW show;          /* Debug fs function hookup. */
	struct dentry *dir_entry; /* Debug fs directory entry. */
	void *priv_data;          /* Private data */
};

/* Global state information. */
struct sm_state_t {
	struct platform_device *pdev;

	struct sm_instance *sm_handle;	/* Handle for videocore service. */
	struct cma *cma_heap;

	spinlock_t kernelid_map_lock;	/* Spinlock protecting kernelid_map */
	struct idr kernelid_map;

	struct mutex map_lock;          /* Global map lock. */
	struct list_head buffer_list;	/* List of buffer. */

	struct vc_sm_privdata_t *data_knl;  /* Kernel internal data tracking. */
	struct vc_sm_privdata_t *vpu_allocs; /* All allocations from the VPU */
	struct dentry *dir_root;	/* Debug fs entries root. */
	struct sm_pde_t dir_state;	/* Debug fs entries state sub-tree. */

	bool require_released_callback;	/* VPU will send a released msg when it
					 * has finished with a resource.
					 */
	u32 int_trans_id;		/* Interrupted transaction. */
};

struct vc_sm_dma_buf_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
};

/* ---- Private Variables ----------------------------------------------- */

static struct sm_state_t *sm_state;
static int sm_inited;

/* ---- Private Function Prototypes -------------------------------------- */

/* ---- Private Functions ------------------------------------------------ */

static int get_kernel_id(struct vc_sm_buffer *buffer)
{
	int handle;

	spin_lock(&sm_state->kernelid_map_lock);
	handle = idr_alloc(&sm_state->kernelid_map, buffer, 0, 0, GFP_KERNEL);
	spin_unlock(&sm_state->kernelid_map_lock);

	return handle;
}

static struct vc_sm_buffer *lookup_kernel_id(int handle)
{
	return idr_find(&sm_state->kernelid_map, handle);
}

static void free_kernel_id(int handle)
{
	spin_lock(&sm_state->kernelid_map_lock);
	idr_remove(&sm_state->kernelid_map, handle);
	spin_unlock(&sm_state->kernelid_map_lock);
}

static int vc_sm_cma_seq_file_show(struct seq_file *s, void *v)
{
	struct sm_pde_t *sm_pde;

	sm_pde = (struct sm_pde_t *)(s->private);

	if (sm_pde && sm_pde->show)
		sm_pde->show(s, v);

	return 0;
}

static int vc_sm_cma_single_open(struct inode *inode, struct file *file)
{
	return single_open(file, vc_sm_cma_seq_file_show, inode->i_private);
}

static const struct file_operations vc_sm_cma_debug_fs_fops = {
	.open = vc_sm_cma_single_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int vc_sm_cma_global_state_show(struct seq_file *s, void *v)
{
	struct vc_sm_buffer *resource = NULL;
	int resource_count = 0;

	if (!sm_state)
		return 0;

	seq_printf(s, "\nVC-ServiceHandle     %p\n", sm_state->sm_handle);

	/* Log all applicable mapping(s). */

	mutex_lock(&sm_state->map_lock);
	seq_puts(s, "\nResources\n");
	if (!list_empty(&sm_state->buffer_list)) {
		list_for_each_entry(resource, &sm_state->buffer_list,
				    global_buffer_list) {
			resource_count++;

			seq_printf(s, "\nResource                %p\n",
				   resource);
			seq_printf(s, "           NAME         %s\n",
				   resource->name);
			seq_printf(s, "           SIZE         %zu\n",
				   resource->size);
			seq_printf(s, "           DMABUF       %p\n",
				   resource->dma_buf);
			if (resource->imported) {
				seq_printf(s, "           ATTACH       %p\n",
					   resource->import.attach);
				seq_printf(s, "           SGT          %p\n",
					   resource->import.sgt);
			}
			seq_printf(s, "           SG_TABLE     %p\n",
				   resource->sg_table);
			seq_printf(s, "           DMA_ADDR     %pad\n",
				   &resource->dma_addr);
			seq_printf(s, "           VC_HANDLE     %08x\n",
				   resource->vc_handle);
			seq_printf(s, "           VC_MAPPING    %d\n",
				   resource->vpu_state);
		}
	}
	seq_printf(s, "\n\nTotal resource count:   %d\n\n", resource_count);

	mutex_unlock(&sm_state->map_lock);

	return 0;
}

/*
 * Adds a buffer to the private data list which tracks all the allocated
 * data.
 */
static void vc_sm_add_resource(struct vc_sm_privdata_t *privdata,
			       struct vc_sm_buffer *buffer)
{
	mutex_lock(&sm_state->map_lock);
	list_add(&buffer->global_buffer_list, &sm_state->buffer_list);
	mutex_unlock(&sm_state->map_lock);

	pr_debug("[%s]: added buffer %p (name %s, size %zu)\n",
		 __func__, buffer, buffer->name, buffer->size);
}

/*
 * Cleans up imported dmabuf.
 */
static void vc_sm_clean_up_dmabuf(struct vc_sm_buffer *buffer)
{
	if (!buffer->imported)
		return;

	/* Handle cleaning up imported dmabufs */
	mutex_lock(&buffer->lock);
	if (buffer->import.sgt) {
		dma_buf_unmap_attachment(buffer->import.attach,
					 buffer->import.sgt,
					 DMA_BIDIRECTIONAL);
		buffer->import.sgt = NULL;
	}
	if (buffer->import.attach) {
		dma_buf_detach(buffer->dma_buf, buffer->import.attach);
		buffer->import.attach = NULL;
	}
	mutex_unlock(&buffer->lock);
}

/*
 * Instructs VPU to decrement the refcount on a buffer.
 */
static void vc_sm_vpu_free(struct vc_sm_buffer *buffer)
{
	if (buffer->vc_handle && buffer->vpu_state == VPU_MAPPED) {
		struct vc_sm_free_t free = { buffer->vc_handle, 0 };
		int status = vc_sm_cma_vchi_free(sm_state->sm_handle, &free,
					     &sm_state->int_trans_id);
		if (status != 0 && status != -EINTR) {
			pr_err("[%s]: failed to free memory on videocore (status: %u, trans_id: %u)\n",
			       __func__, status, sm_state->int_trans_id);
		}

		if (sm_state->require_released_callback) {
			/* Need to wait for the VPU to confirm the free. */

			/* Retain a reference on this until the VPU has
			 * released it
			 */
			buffer->vpu_state = VPU_UNMAPPING;
		} else {
			buffer->vpu_state = VPU_NOT_MAPPED;
			buffer->vc_handle = 0;
		}
	}
}

/*
 * Release an allocation.
 * All refcounting is done via the dma buf object.
 *
 * Must be called with the mutex held. The function will either release the
 * mutex (if defering the release) or destroy it. The caller must therefore not
 * reuse the buffer on return.
 */
static void vc_sm_release_resource(struct vc_sm_buffer *buffer)
{
	pr_debug("[%s]: buffer %p (name %s, size %zu)\n",
		 __func__, buffer, buffer->name, buffer->size);

	if (buffer->vc_handle) {
		/* We've sent the unmap request but not had the response. */
		pr_err("[%s]: Waiting for VPU unmap response on %p\n",
		       __func__, buffer);
		goto defer;
	}
	if (buffer->in_use) {
		/* dmabuf still in use - we await the release */
		pr_err("[%s]: buffer %p is still in use\n",
		       __func__, buffer);
		goto defer;
	}

	/* Release the allocation (whether imported dmabuf or CMA allocation) */
	if (buffer->imported) {
		pr_debug("%s: Release imported dmabuf %p\n", __func__,
			 buffer->import.dma_buf);
		if (buffer->import.dma_buf)
			dma_buf_put(buffer->import.dma_buf);
		else
			pr_err("%s: Imported dmabuf already been put for buf %p\n",
			       __func__, buffer);
		buffer->import.dma_buf = NULL;
	} else {
		if (buffer->sg_table) {
			/* Our own allocation that we need to dma_unmap_sg */
			dma_unmap_sg(&sm_state->pdev->dev,
				     buffer->sg_table->sgl,
				     buffer->sg_table->nents,
				     DMA_BIDIRECTIONAL);
		}
		pr_debug("%s: Release our allocation\n", __func__);
		vc_sm_cma_buffer_free(&buffer->alloc);
		pr_debug("%s: Release our allocation - done\n", __func__);
	}


	/* Free our buffer. Start by removing it from the list */
	mutex_lock(&sm_state->map_lock);
	list_del(&buffer->global_buffer_list);
	mutex_unlock(&sm_state->map_lock);

	pr_debug("%s: Release our allocation - done\n", __func__);
	mutex_unlock(&buffer->lock);

	mutex_destroy(&buffer->lock);

	kfree(buffer);
	return;

defer:
	mutex_unlock(&buffer->lock);
	return;
}

/* Create support for private data tracking. */
static struct vc_sm_privdata_t *vc_sm_cma_create_priv_data(pid_t id)
{
	char alloc_name[32];
	struct vc_sm_privdata_t *file_data = NULL;

	/* Allocate private structure. */
	file_data = kzalloc(sizeof(*file_data), GFP_KERNEL);

	if (!file_data)
		return NULL;

	snprintf(alloc_name, sizeof(alloc_name), "%d", id);

	file_data->pid = id;

	return file_data;
}

static struct sg_table *dup_sg_table(struct sg_table *table)
{
	struct sg_table *new_table;
	int ret, i;
	struct scatterlist *sg, *new_sg;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, table->nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	new_sg = new_table->sgl;
	for_each_sg(table->sgl, sg, table->nents, i) {
		memcpy(new_sg, sg, sizeof(*sg));
		sg->dma_address = 0;
		new_sg = sg_next(new_sg);
	}

	return new_table;
}

static void free_duped_table(struct sg_table *table)
{
	sg_free_table(table);
	kfree(table);
}

/* Dma buf operations for use with our own allocations */

static int vc_sm_dma_buf_attach(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)

{
	struct vc_sm_dma_buf_attachment *a;
	struct sg_table *table;
	struct vc_sm_buffer *buf = dmabuf->priv;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = dup_sg_table(buf->sg_table);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->table = table;
	INIT_LIST_HEAD(&a->list);

	attachment->priv = a;

	mutex_lock(&buf->lock);
	list_add(&a->list, &buf->attachments);
	mutex_unlock(&buf->lock);
	pr_debug("%s dmabuf %p attachment %p\n", __func__, dmabuf, attachment);

	return 0;
}

static void vc_sm_dma_buf_detatch(struct dma_buf *dmabuf,
				  struct dma_buf_attachment *attachment)
{
	struct vc_sm_dma_buf_attachment *a = attachment->priv;
	struct vc_sm_buffer *buf = dmabuf->priv;

	pr_debug("%s dmabuf %p attachment %p\n", __func__, dmabuf, attachment);
	free_duped_table(a->table);
	mutex_lock(&buf->lock);
	list_del(&a->list);
	mutex_unlock(&buf->lock);

	kfree(a);
}

static struct sg_table *vc_sm_map_dma_buf(struct dma_buf_attachment *attachment,
					  enum dma_data_direction direction)
{
	struct vc_sm_dma_buf_attachment *a = attachment->priv;
	struct sg_table *table;

	table = a->table;

	if (!dma_map_sg(attachment->dev, table->sgl, table->nents,
			direction))
		return ERR_PTR(-ENOMEM);

	pr_debug("%s attachment %p\n", __func__, attachment);
	return table;
}

static void vc_sm_unmap_dma_buf(struct dma_buf_attachment *attachment,
				struct sg_table *table,
				enum dma_data_direction direction)
{
	pr_debug("%s attachment %p\n", __func__, attachment);
	dma_unmap_sg(attachment->dev, table->sgl, table->nents, direction);
}

static int vc_sm_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct vc_sm_buffer *buf = dmabuf->priv;
	struct sg_table *table = buf->sg_table;
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	struct scatterlist *sg;
	int i;
	int ret = 0;

	pr_debug("%s dmabuf %p, buf %p, vm_start %08lX\n", __func__, dmabuf,
		 buf, addr);

	mutex_lock(&buf->lock);

	/* now map it to userspace */
	for_each_sg(table->sgl, sg, table->nents, i) {
		struct page *page = sg_page(sg);
		unsigned long remainder = vma->vm_end - addr;
		unsigned long len = sg->length;

		if (offset >= sg->length) {
			offset -= sg->length;
			continue;
		} else if (offset) {
			page += offset / PAGE_SIZE;
			len = sg->length - offset;
			offset = 0;
		}
		len = min(len, remainder);
		ret = remap_pfn_range(vma, addr, page_to_pfn(page), len,
				      vma->vm_page_prot);
		if (ret)
			break;
		addr += len;
		if (addr >= vma->vm_end)
			break;
	}
	mutex_unlock(&buf->lock);

	if (ret)
		pr_err("%s: failure mapping buffer to userspace\n",
		       __func__);

	return ret;
}

static void vc_sm_dma_buf_release(struct dma_buf *dmabuf)
{
	struct vc_sm_buffer *buffer;

	if (!dmabuf)
		return;

	buffer = (struct vc_sm_buffer *)dmabuf->priv;

	mutex_lock(&buffer->lock);

	pr_debug("%s dmabuf %p, buffer %p\n", __func__, dmabuf, buffer);

	buffer->in_use = 0;

	/* Unmap on the VPU */
	vc_sm_vpu_free(buffer);
	pr_debug("%s vpu_free done\n", __func__);

	/* Unmap our dma_buf object (the vc_sm_buffer remains until released
	 * on the VPU).
	 */
	vc_sm_clean_up_dmabuf(buffer);
	pr_debug("%s clean_up dmabuf done\n", __func__);

	vc_sm_release_resource(buffer);
	pr_debug("%s done\n", __func__);
}

static int vc_sm_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					  enum dma_data_direction direction)
{
	struct vc_sm_buffer *buf;
	struct vc_sm_dma_buf_attachment *a;

	if (!dmabuf)
		return -EFAULT;

	buf = dmabuf->priv;
	if (!buf)
		return -EFAULT;

	mutex_lock(&buf->lock);

	list_for_each_entry(a, &buf->attachments, list) {
		dma_sync_sg_for_cpu(a->dev, a->table->sgl, a->table->nents,
				    direction);
	}
	mutex_unlock(&buf->lock);

	return 0;
}

static int vc_sm_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	struct vc_sm_buffer *buf;
	struct vc_sm_dma_buf_attachment *a;

	if (!dmabuf)
		return -EFAULT;
	buf = dmabuf->priv;
	if (!buf)
		return -EFAULT;

	mutex_lock(&buf->lock);

	list_for_each_entry(a, &buf->attachments, list) {
		dma_sync_sg_for_device(a->dev, a->table->sgl, a->table->nents,
				       direction);
	}
	mutex_unlock(&buf->lock);

	return 0;
}

static void *vc_sm_dma_buf_kmap(struct dma_buf *dmabuf, unsigned long offset)
{
	/* FIXME */
	return NULL;
}

static void vc_sm_dma_buf_kunmap(struct dma_buf *dmabuf, unsigned long offset,
				 void *ptr)
{
	/* FIXME */
}

static const struct dma_buf_ops dma_buf_ops = {
	.map_dma_buf = vc_sm_map_dma_buf,
	.unmap_dma_buf = vc_sm_unmap_dma_buf,
	.mmap = vc_sm_dmabuf_mmap,
	.release = vc_sm_dma_buf_release,
	.attach = vc_sm_dma_buf_attach,
	.detach = vc_sm_dma_buf_detatch,
	.begin_cpu_access = vc_sm_dma_buf_begin_cpu_access,
	.end_cpu_access = vc_sm_dma_buf_end_cpu_access,
	.map = vc_sm_dma_buf_kmap,
	.unmap = vc_sm_dma_buf_kunmap,
};
/* Dma_buf operations for chaining through to an imported dma_buf */
static
int vc_sm_import_dma_buf_attach(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)
{
	struct vc_sm_buffer *buf = dmabuf->priv;

	if (!buf->imported)
		return -EINVAL;
	return buf->import.dma_buf->ops->attach(buf->import.dma_buf,
						attachment);
}

static
void vc_sm_import_dma_buf_detatch(struct dma_buf *dmabuf,
				  struct dma_buf_attachment *attachment)
{
	struct vc_sm_buffer *buf = dmabuf->priv;

	if (!buf->imported)
		return;
	buf->import.dma_buf->ops->detach(buf->import.dma_buf, attachment);
}

static
struct sg_table *vc_sm_import_map_dma_buf(struct dma_buf_attachment *attachment,
					  enum dma_data_direction direction)
{
	struct vc_sm_buffer *buf = attachment->dmabuf->priv;

	if (!buf->imported)
		return NULL;
	return buf->import.dma_buf->ops->map_dma_buf(attachment,
						     direction);
}

static
void vc_sm_import_unmap_dma_buf(struct dma_buf_attachment *attachment,
				struct sg_table *table,
				enum dma_data_direction direction)
{
	struct vc_sm_buffer *buf = attachment->dmabuf->priv;

	if (!buf->imported)
		return;
	buf->import.dma_buf->ops->unmap_dma_buf(attachment, table, direction);
}

static
int vc_sm_import_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct vc_sm_buffer *buf = dmabuf->priv;

	pr_debug("%s: mmap dma_buf %p, buf %p, imported db %p\n", __func__,
		 dmabuf, buf, buf->import.dma_buf);
	if (!buf->imported) {
		pr_err("%s: mmap dma_buf %p- not an imported buffer\n",
		       __func__, dmabuf);
		return -EINVAL;
	}
	return buf->import.dma_buf->ops->mmap(buf->import.dma_buf, vma);
}

static
void vc_sm_import_dma_buf_release(struct dma_buf *dmabuf)
{
	struct vc_sm_buffer *buf = dmabuf->priv;

	pr_debug("%s: Relasing dma_buf %p\n", __func__, dmabuf);
	mutex_lock(&buf->lock);
	if (!buf->imported)
		return;

	buf->in_use = 0;

	vc_sm_vpu_free(buf);

	vc_sm_release_resource(buf);
}

static
void *vc_sm_import_dma_buf_kmap(struct dma_buf *dmabuf,
				unsigned long offset)
{
	struct vc_sm_buffer *buf = dmabuf->priv;

	if (!buf->imported)
		return NULL;
	return buf->import.dma_buf->ops->map(buf->import.dma_buf, offset);
}

static
void vc_sm_import_dma_buf_kunmap(struct dma_buf *dmabuf,
				 unsigned long offset, void *ptr)
{
	struct vc_sm_buffer *buf = dmabuf->priv;

	if (!buf->imported)
		return;
	buf->import.dma_buf->ops->unmap(buf->import.dma_buf, offset, ptr);
}

static
int vc_sm_import_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					  enum dma_data_direction direction)
{
	struct vc_sm_buffer *buf = dmabuf->priv;

	if (!buf->imported)
		return -EINVAL;
	return buf->import.dma_buf->ops->begin_cpu_access(buf->import.dma_buf,
							  direction);
}

static
int vc_sm_import_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	struct vc_sm_buffer *buf = dmabuf->priv;

	if (!buf->imported)
		return -EINVAL;
	return buf->import.dma_buf->ops->end_cpu_access(buf->import.dma_buf,
							  direction);
}

static const struct dma_buf_ops dma_buf_import_ops = {
	.map_dma_buf = vc_sm_import_map_dma_buf,
	.unmap_dma_buf = vc_sm_import_unmap_dma_buf,
	.mmap = vc_sm_import_dmabuf_mmap,
	.release = vc_sm_import_dma_buf_release,
	.attach = vc_sm_import_dma_buf_attach,
	.detach = vc_sm_import_dma_buf_detatch,
	.begin_cpu_access = vc_sm_import_dma_buf_begin_cpu_access,
	.end_cpu_access = vc_sm_import_dma_buf_end_cpu_access,
	.map = vc_sm_import_dma_buf_kmap,
	.unmap = vc_sm_import_dma_buf_kunmap,
};

/* Import a dma_buf to be shared with VC. */
int
vc_sm_cma_import_dmabuf_internal(struct vc_sm_privdata_t *private,
				 struct dma_buf *dma_buf,
				 struct dma_buf **imported_buf)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct vc_sm_buffer *buffer = NULL;
	struct vc_sm_import import = { };
	struct vc_sm_import_result result = { };
	struct dma_buf_attachment *attach = NULL;
	struct sg_table *sgt = NULL;
	dma_addr_t dma_addr;
	int ret = 0;
	int status;

	/* Setup our allocation parameters */
	pr_debug("%s: importing dma_buf %p\n", __func__, dma_buf);

	get_dma_buf(dma_buf);
	dma_buf = dma_buf;

	attach = dma_buf_attach(dma_buf, &sm_state->pdev->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto error;
	}

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto error;
	}

	/* Verify that the address block is contiguous */
	if (sgt->nents != 1) {
		ret = -ENOMEM;
		goto error;
	}

	/* Allocate local buffer to track this allocation. */
	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto error;
	}

	import.type = VC_SM_ALLOC_NON_CACHED;
	dma_addr = sg_dma_address(sgt->sgl);
	import.addr = (uint32_t)dma_addr;
	if ((import.addr & 0xC0000000) != 0xC0000000) {
		pr_err("%s: Expecting an uncached alias for dma_addr %pad\n",
		       __func__, &dma_addr);
		import.addr |= 0xC0000000;
	}
	import.size = sg_dma_len(sgt->sgl);
	import.allocator = current->tgid;
	import.kernel_id = get_kernel_id(buffer);

	memcpy(import.name, VC_SM_RESOURCE_NAME_DEFAULT,
	       sizeof(VC_SM_RESOURCE_NAME_DEFAULT));

	pr_debug("[%s]: attempt to import \"%s\" data - type %u, addr %pad, size %u.\n",
		 __func__, import.name, import.type, &dma_addr, import.size);

	/* Allocate the videocore buffer. */
	status = vc_sm_cma_vchi_import(sm_state->sm_handle, &import, &result,
				       &sm_state->int_trans_id);
	if (status == -EINTR) {
		pr_debug("[%s]: requesting import memory action restart (trans_id: %u)\n",
			 __func__, sm_state->int_trans_id);
		ret = -ERESTARTSYS;
		private->restart_sys = -EINTR;
		private->int_action = VC_SM_MSG_TYPE_IMPORT;
		goto error;
	} else if (status || !result.res_handle) {
		pr_debug("[%s]: failed to import memory on videocore (status: %u, trans_id: %u)\n",
			 __func__, status, sm_state->int_trans_id);
		ret = -ENOMEM;
		goto error;
	}

	mutex_init(&buffer->lock);
	INIT_LIST_HEAD(&buffer->attachments);
	memcpy(buffer->name, import.name,
	       min(sizeof(buffer->name), sizeof(import.name) - 1));

	/* Keep track of the buffer we created. */
	buffer->private = private;
	buffer->vc_handle = result.res_handle;
	buffer->size = import.size;
	buffer->vpu_state = VPU_MAPPED;

	buffer->imported = 1;
	buffer->import.dma_buf = dma_buf;

	buffer->import.attach = attach;
	buffer->import.sgt = sgt;
	buffer->dma_addr = dma_addr;
	buffer->in_use = 1;
	buffer->kernel_id = import.kernel_id;

	/*
	 * We're done - we need to export a new dmabuf chaining through most
	 * functions, but enabling us to release our own internal references
	 * here.
	 */
	exp_info.ops = &dma_buf_import_ops;
	exp_info.size = import.size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buffer;

	buffer->dma_buf = dma_buf_export(&exp_info);
	if (IS_ERR(buffer->dma_buf)) {
		ret = PTR_ERR(buffer->dma_buf);
		goto error;
	}

	vc_sm_add_resource(private, buffer);

	*imported_buf = buffer->dma_buf;

	return 0;

error:
	if (result.res_handle) {
		struct vc_sm_free_t free = { result.res_handle, 0 };

		vc_sm_cma_vchi_free(sm_state->sm_handle, &free,
				    &sm_state->int_trans_id);
	}
	free_kernel_id(import.kernel_id);
	kfree(buffer);
	if (sgt)
		dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
	if (attach)
		dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);
	return ret;
}

static int vc_sm_cma_vpu_alloc(u32 size, uint32_t align, const char *name,
			       u32 mem_handle, struct vc_sm_buffer **ret_buffer)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct vc_sm_buffer *buffer = NULL;
	int aligned_size;
	int ret = 0;

	/* Align to the user requested align */
	aligned_size = ALIGN(size, align);
	/* and then to a page boundary */
	aligned_size = PAGE_ALIGN(aligned_size);

	if (!aligned_size)
		return -EINVAL;

	/* Allocate local buffer to track this allocation. */
	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	mutex_init(&buffer->lock);

	if (vc_sm_cma_buffer_allocate(sm_state->cma_heap, &buffer->alloc,
				      aligned_size)) {
		pr_err("[%s]: cma alloc of %d bytes failed\n",
		       __func__, aligned_size);
		ret = -ENOMEM;
		goto error;
	}
	buffer->sg_table = buffer->alloc.sg_table;

	pr_debug("[%s]: cma alloc of %d bytes success\n",
		 __func__, aligned_size);

	if (dma_map_sg(&sm_state->pdev->dev, buffer->sg_table->sgl,
		       buffer->sg_table->nents, DMA_BIDIRECTIONAL) <= 0) {
		pr_err("[%s]: dma_map_sg failed\n", __func__);
		goto error;
	}

	INIT_LIST_HEAD(&buffer->attachments);

	memcpy(buffer->name, name,
	       min(sizeof(buffer->name), strlen(name)));

	exp_info.ops = &dma_buf_ops;
	exp_info.size = aligned_size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buffer;

	buffer->dma_buf = dma_buf_export(&exp_info);
	if (IS_ERR(buffer->dma_buf)) {
		ret = PTR_ERR(buffer->dma_buf);
		goto error;
	}
	buffer->dma_addr = (uint32_t)sg_dma_address(buffer->sg_table->sgl);
	if ((buffer->dma_addr & 0xC0000000) != 0xC0000000) {
		pr_err("%s: Expecting an uncached alias for dma_addr %pad\n",
		       __func__, &buffer->dma_addr);
		buffer->dma_addr |= 0xC0000000;
	}
	buffer->private = sm_state->vpu_allocs;

	buffer->vc_handle = mem_handle;
	buffer->vpu_state = VPU_MAPPED;
	buffer->vpu_allocated = 1;
	buffer->size = size;
	/*
	 * Create an ID that will be passed along with our message so
	 * that when we service the release reply, we can look up which
	 * resource is being released.
	 */
	buffer->kernel_id = get_kernel_id(buffer);

	vc_sm_add_resource(sm_state->vpu_allocs, buffer);

	*ret_buffer = buffer;
	return 0;
error:
	if (buffer)
		vc_sm_release_resource(buffer);
	return ret;
}

static void
vc_sm_vpu_event(struct sm_instance *instance, struct vc_sm_result_t *reply,
		int reply_len)
{
	switch (reply->trans_id & ~0x80000000) {
	case VC_SM_MSG_TYPE_CLIENT_VERSION:
	{
		/* Acknowledge that the firmware supports the version command */
		pr_debug("%s: firmware acked version msg. Require release cb\n",
			 __func__);
		sm_state->require_released_callback = true;
	}
	break;
	case VC_SM_MSG_TYPE_RELEASED:
	{
		struct vc_sm_released *release = (struct vc_sm_released *)reply;
		struct vc_sm_buffer *buffer =
					lookup_kernel_id(release->kernel_id);
		if (!buffer) {
			pr_err("%s: VC released a buffer that is already released, kernel_id %d\n",
			       __func__, release->kernel_id);
			break;
		}
		mutex_lock(&buffer->lock);

		pr_debug("%s: Released addr %08x, size %u, id %08x, mem_handle %08x\n",
			 __func__, release->addr, release->size,
			 release->kernel_id, release->vc_handle);

		buffer->vc_handle = 0;
		buffer->vpu_state = VPU_NOT_MAPPED;
		free_kernel_id(release->kernel_id);

		if (buffer->vpu_allocated) {
			/* VPU allocation, so release the dmabuf which will
			 * trigger the clean up.
			 */
			mutex_unlock(&buffer->lock);
			dma_buf_put(buffer->dma_buf);
		} else {
			vc_sm_release_resource(buffer);
		}
	}
	break;
	case VC_SM_MSG_TYPE_VC_MEM_REQUEST:
	{
		struct vc_sm_buffer *buffer = NULL;
		struct vc_sm_vc_mem_request *req =
					(struct vc_sm_vc_mem_request *)reply;
		struct vc_sm_vc_mem_request_result reply;
		int ret;

		pr_debug("%s: Request %u bytes of memory, align %d name %s, trans_id %08x\n",
			 __func__, req->size, req->align, req->name,
			 req->trans_id);
		ret = vc_sm_cma_vpu_alloc(req->size, req->align, req->name,
					  req->vc_handle, &buffer);

		reply.trans_id = req->trans_id;
		if (!ret) {
			reply.addr = buffer->dma_addr;
			reply.kernel_id = buffer->kernel_id;
			pr_debug("%s: Allocated resource buffer %p, addr %pad\n",
				 __func__, buffer, &buffer->dma_addr);
		} else {
			pr_err("%s: Allocation failed size %u, name %s, vc_handle %u\n",
			       __func__, req->size, req->name, req->vc_handle);
			reply.addr = 0;
			reply.kernel_id = 0;
		}
		vc_sm_vchi_client_vc_mem_req_reply(sm_state->sm_handle, &reply,
						   &sm_state->int_trans_id);
		break;
	}
	break;
	default:
		pr_err("%s: Unknown vpu cmd %x\n", __func__, reply->trans_id);
		break;
	}
}

/* Videocore connected.  */
static void vc_sm_connected_init(void)
{
	int ret;
	VCHI_INSTANCE_T vchi_instance;
	struct vc_sm_version version;
	struct vc_sm_result_t version_result;

	pr_info("[%s]: start\n", __func__);

	if (vc_sm_cma_add_heaps(&sm_state->cma_heap) ||
	    !sm_state->cma_heap) {
		pr_err("[%s]: failed to initialise CMA heaps\n",
		       __func__);
		ret = -EIO;
		goto err_free_mem;
	}

	/*
	 * Initialize and create a VCHI connection for the shared memory service
	 * running on videocore.
	 */
	ret = vchi_initialise(&vchi_instance);
	if (ret) {
		pr_err("[%s]: failed to initialise VCHI instance (ret=%d)\n",
		       __func__, ret);

		ret = -EIO;
		goto err_failed;
	}

	ret = vchi_connect(vchi_instance);
	if (ret) {
		pr_err("[%s]: failed to connect VCHI instance (ret=%d)\n",
		       __func__, ret);

		ret = -EIO;
		goto err_failed;
	}

	/* Initialize an instance of the shared memory service. */
	sm_state->sm_handle = vc_sm_cma_vchi_init(vchi_instance, 1,
						  vc_sm_vpu_event);
	if (!sm_state->sm_handle) {
		pr_err("[%s]: failed to initialize shared memory service\n",
		       __func__);

		ret = -EPERM;
		goto err_failed;
	}

	/* Create a debug fs directory entry (root). */
	sm_state->dir_root = debugfs_create_dir(VC_SM_DIR_ROOT_NAME, NULL);

	sm_state->dir_state.show = &vc_sm_cma_global_state_show;
	sm_state->dir_state.dir_entry =
		debugfs_create_file(VC_SM_STATE, 0444, sm_state->dir_root,
				    &sm_state->dir_state,
				    &vc_sm_cma_debug_fs_fops);

	INIT_LIST_HEAD(&sm_state->buffer_list);

	sm_state->data_knl = vc_sm_cma_create_priv_data(0);
	if (!sm_state->data_knl) {
		pr_err("[%s]: failed to create kernel private data tracker\n",
		       __func__);
		goto err_remove_shared_memory;
	}

	version.version = 2;
	ret = vc_sm_cma_vchi_client_version(sm_state->sm_handle, &version,
					    &version_result,
					    &sm_state->int_trans_id);
	if (ret) {
		pr_err("[%s]: Failed to send version request %d\n", __func__,
		       ret);
	}

	/* Done! */
	sm_inited = 1;
	pr_info("[%s]: installed successfully\n", __func__);
	return;

err_remove_shared_memory:
	debugfs_remove_recursive(sm_state->dir_root);
	vc_sm_cma_vchi_stop(&sm_state->sm_handle);
err_failed:
	pr_info("[%s]: failed, ret %d\n", __func__, ret);
}

/* Driver loading. */
static int bcm2835_vc_sm_cma_probe(struct platform_device *pdev)
{
	pr_info("%s: Videocore shared memory driver\n", __func__);

	sm_state = devm_kzalloc(&pdev->dev, sizeof(*sm_state), GFP_KERNEL);
	if (!sm_state)
		return -ENOMEM;
	sm_state->pdev = pdev;
	mutex_init(&sm_state->map_lock);

	spin_lock_init(&sm_state->kernelid_map_lock);
	idr_init_base(&sm_state->kernelid_map, 1);

	pdev->dev.dma_parms = devm_kzalloc(&pdev->dev,
					   sizeof(*pdev->dev.dma_parms),
					   GFP_KERNEL);
	/* dma_set_max_seg_size checks if dma_parms is NULL. */
	dma_set_max_seg_size(&pdev->dev, 0x3FFFFFFF);

	vchiq_add_connected_callback(vc_sm_connected_init);
	return 0;
}

/* Driver unloading. */
static int bcm2835_vc_sm_cma_remove(struct platform_device *pdev)
{
	pr_debug("[%s]: start\n", __func__);
	if (sm_inited) {
		/* Remove all proc entries. */
		debugfs_remove_recursive(sm_state->dir_root);

		/* Stop the videocore shared memory service. */
		vc_sm_cma_vchi_stop(&sm_state->sm_handle);
	}

	if (sm_state) {
		idr_destroy(&sm_state->kernelid_map);

		/* Free the memory for the state structure. */
		mutex_destroy(&sm_state->map_lock);
	}

	pr_debug("[%s]: end\n", __func__);
	return 0;
}

/* Get an internal resource handle mapped from the external one. */
int vc_sm_cma_int_handle(void *handle)
{
	struct dma_buf *dma_buf = (struct dma_buf *)handle;
	struct vc_sm_buffer *buf;

	/* Validate we can work with this device. */
	if (!sm_state || !handle) {
		pr_err("[%s]: invalid input\n", __func__);
		return 0;
	}

	buf = (struct vc_sm_buffer *)dma_buf->priv;
	return buf->vc_handle;
}
EXPORT_SYMBOL_GPL(vc_sm_cma_int_handle);

/* Free a previously allocated shared memory handle and block. */
int vc_sm_cma_free(void *handle)
{
	struct dma_buf *dma_buf = (struct dma_buf *)handle;

	/* Validate we can work with this device. */
	if (!sm_state || !handle) {
		pr_err("[%s]: invalid input\n", __func__);
		return -EPERM;
	}

	pr_debug("%s: handle %p/dmabuf %p\n", __func__, handle, dma_buf);

	dma_buf_put(dma_buf);

	return 0;
}
EXPORT_SYMBOL_GPL(vc_sm_cma_free);

/* Import a dmabuf to be shared with VC. */
int vc_sm_cma_import_dmabuf(struct dma_buf *src_dmabuf, void **handle)
{
	struct dma_buf *new_dma_buf;
	struct vc_sm_buffer *buf;
	int ret;

	/* Validate we can work with this device. */
	if (!sm_state || !src_dmabuf || !handle) {
		pr_err("[%s]: invalid input\n", __func__);
		return -EPERM;
	}

	ret = vc_sm_cma_import_dmabuf_internal(sm_state->data_knl, src_dmabuf,
					       &new_dma_buf);

	if (!ret) {
		pr_debug("%s: imported to ptr %p\n", __func__, new_dma_buf);
		buf = (struct vc_sm_buffer *)new_dma_buf->priv;

		/* Assign valid handle at this time.*/
		*handle = new_dma_buf;
	} else {
		/*
		 * succeeded in importing the dma_buf, but then
		 * failed to look it up again. How?
		 * Release the fd again.
		 */
		pr_err("%s: imported vc_sm_cma_get_buffer failed %d\n",
		       __func__, ret);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(vc_sm_cma_import_dmabuf);

static struct platform_driver bcm2835_vcsm_cma_driver = {
	.probe = bcm2835_vc_sm_cma_probe,
	.remove = bcm2835_vc_sm_cma_remove,
	.driver = {
		   .name = DEVICE_NAME,
		   .owner = THIS_MODULE,
		   },
};

module_platform_driver(bcm2835_vcsm_cma_driver);

MODULE_AUTHOR("Dave Stevenson");
MODULE_DESCRIPTION("VideoCore CMA Shared Memory Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:vcsm-cma");
