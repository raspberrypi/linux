// SPDX-License-Identifier: GPL-2.0
/*
 * VideoCore Shared Memory driver using CMA.
 *
 * Copyright: 2018, Raspberry Pi (Trading) Ltd
 * Dave Stevenson <dave.stevenson@raspberrypi.org>
 *
 * Based on vmcs_sm driver from Broadcom Corporation for some API,
 * and taking some code for buffer allocation and dmabuf handling from
 * videobuf2.
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
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/syscalls.h>
#include <linux/types.h>
#include <asm/cacheflush.h>

#include "vchiq_connected.h"
#include "vc_sm_cma_vchi.h"

#include "vc_sm.h"
#include "vc_sm_knl.h"
#include <linux/broadcom/vc_sm_cma_ioctl.h>

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

	struct miscdevice misc_dev;

	struct sm_instance *sm_handle;	/* Handle for videocore service. */

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
	struct sg_table sg_table;
	struct list_head list;
	enum dma_data_direction	dma_dir;
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
			} else {
				seq_printf(s, "           SGT          %p\n",
					   resource->alloc.sg_table);
			}
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
	pr_debug("[%s]: buffer %p (name %s, size %zu), imported %u\n",
		 __func__, buffer, buffer->name, buffer->size,
		 buffer->imported);

	if (buffer->vc_handle) {
		/* We've sent the unmap request but not had the response. */
		pr_debug("[%s]: Waiting for VPU unmap response on %p\n",
			 __func__, buffer);
		goto defer;
	}
	if (buffer->in_use) {
		/* dmabuf still in use - we await the release */
		pr_debug("[%s]: buffer %p is still in use\n", __func__, buffer);
		goto defer;
	}

	/* Release the allocation (whether imported dmabuf or CMA allocation) */
	if (buffer->imported) {
		if (buffer->import.dma_buf)
			dma_buf_put(buffer->import.dma_buf);
		else
			pr_err("%s: Imported dmabuf already been put for buf %p\n",
			       __func__, buffer);
		buffer->import.dma_buf = NULL;
	} else {
		dma_free_coherent(&sm_state->pdev->dev, buffer->size,
				  buffer->cookie, buffer->dma_addr);
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

/* Dma buf operations for use with our own allocations */

static int vc_sm_dma_buf_attach(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)

{
	struct vc_sm_dma_buf_attachment *a;
	struct sg_table *sgt;
	struct vc_sm_buffer *buf = dmabuf->priv;
	struct scatterlist *rd, *wr;
	int ret, i;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	pr_debug("%s dmabuf %p attachment %p\n", __func__, dmabuf, attachment);

	mutex_lock(&buf->lock);

	INIT_LIST_HEAD(&a->list);

	sgt = &a->sg_table;

	/* Copy the buf->base_sgt scatter list to the attachment, as we can't
	 * map the same scatter list to multiple attachments at the same time.
	 */
	ret = sg_alloc_table(sgt, buf->alloc.sg_table->orig_nents, GFP_KERNEL);
	if (ret) {
		kfree(a);
		return -ENOMEM;
	}

	rd = buf->alloc.sg_table->sgl;
	wr = sgt->sgl;
	for (i = 0; i < sgt->orig_nents; ++i) {
		sg_set_page(wr, sg_page(rd), rd->length, rd->offset);
		rd = sg_next(rd);
		wr = sg_next(wr);
	}

	a->dma_dir = DMA_NONE;
	attachment->priv = a;

	list_add(&a->list, &buf->attachments);
	mutex_unlock(&buf->lock);

	return 0;
}

static void vc_sm_dma_buf_detach(struct dma_buf *dmabuf,
				 struct dma_buf_attachment *attachment)
{
	struct vc_sm_dma_buf_attachment *a = attachment->priv;
	struct vc_sm_buffer *buf = dmabuf->priv;
	struct sg_table *sgt;

	pr_debug("%s dmabuf %p attachment %p\n", __func__, dmabuf, attachment);
	if (!a)
		return;

	sgt = &a->sg_table;

	/* release the scatterlist cache */
	if (a->dma_dir != DMA_NONE)
		dma_unmap_sg(attachment->dev, sgt->sgl, sgt->orig_nents,
			     a->dma_dir);
	sg_free_table(sgt);

	mutex_lock(&buf->lock);
	list_del(&a->list);
	mutex_unlock(&buf->lock);

	kfree(a);
}

static struct sg_table *vc_sm_map_dma_buf(struct dma_buf_attachment *attachment,
					  enum dma_data_direction direction)
{
	struct vc_sm_dma_buf_attachment *a = attachment->priv;
	/* stealing dmabuf mutex to serialize map/unmap operations */
	struct mutex *lock = &attachment->dmabuf->lock;
	struct sg_table *table;

	mutex_lock(lock);
	pr_debug("%s attachment %p\n", __func__, attachment);
	table = &a->sg_table;

	/* return previously mapped sg table */
	if (a->dma_dir == direction) {
		mutex_unlock(lock);
		return table;
	}

	/* release any previous cache */
	if (a->dma_dir != DMA_NONE) {
		dma_unmap_sg(attachment->dev, table->sgl, table->orig_nents,
			     a->dma_dir);
		a->dma_dir = DMA_NONE;
	}

	/* mapping to the client with new direction */
	table->nents = dma_map_sg(attachment->dev, table->sgl,
				  table->orig_nents, direction);
	if (!table->nents) {
		pr_err("failed to map scatterlist\n");
		mutex_unlock(lock);
		return ERR_PTR(-EIO);
	}

	a->dma_dir = direction;
	mutex_unlock(lock);

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
	int ret;

	pr_debug("%s dmabuf %p, buf %p, vm_start %08lX\n", __func__, dmabuf,
		 buf, vma->vm_start);

	mutex_lock(&buf->lock);

	/* now map it to userspace */
	vma->vm_pgoff = 0;

	ret = dma_mmap_coherent(&sm_state->pdev->dev, vma, buf->cookie,
				buf->dma_addr, buf->size);

	if (ret) {
		pr_err("Remapping memory failed, error: %d\n", ret);
		return ret;
	}

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;

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

	/* buffer->lock will be destroyed by vc_sm_release_resource if finished
	 * with, otherwise unlocked. Do NOT unlock here.
	 */
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
		dma_sync_sg_for_cpu(a->dev, a->sg_table.sgl,
				    a->sg_table.nents, direction);
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
		dma_sync_sg_for_device(a->dev, a->sg_table.sgl,
				       a->sg_table.nents, direction);
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
	.detach = vc_sm_dma_buf_detach,
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
				 int fd,
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
	pr_debug("%s: importing dma_buf %p/fd %d\n", __func__, dma_buf, fd);

	if (fd < 0)
		get_dma_buf(dma_buf);
	else
		dma_buf = dma_buf_get(fd);

	if (!dma_buf)
		return -EINVAL;

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
	import.addr = (u32)dma_addr;
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

static int vc_sm_cma_vpu_alloc(u32 size, u32 align, const char *name,
			       u32 mem_handle, struct vc_sm_buffer **ret_buffer)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct vc_sm_buffer *buffer = NULL;
	struct sg_table *sgt;
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
	/* Acquire the mutex as vc_sm_release_resource will release it in the
	 * error path.
	 */
	mutex_lock(&buffer->lock);

	buffer->cookie = dma_alloc_coherent(&sm_state->pdev->dev,
					    aligned_size, &buffer->dma_addr,
					    GFP_KERNEL);
	if (!buffer->cookie) {
		pr_err("[%s]: dma_alloc_coherent alloc of %d bytes failed\n",
		       __func__, aligned_size);
		ret = -ENOMEM;
		goto error;
	}

	pr_debug("[%s]: alloc of %d bytes success\n",
		 __func__, aligned_size);

	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto error;
	}

	ret = dma_get_sgtable(&sm_state->pdev->dev, sgt, buffer->cookie,
			      buffer->dma_addr, buffer->size);
	if (ret < 0) {
		pr_err("failed to get scatterlist from DMA API\n");
		kfree(sgt);
		ret = -ENOMEM;
		goto error;
	}
	buffer->alloc.sg_table = sgt;

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
	buffer->dma_addr = (u32)sg_dma_address(buffer->alloc.sg_table->sgl);
	if ((buffer->dma_addr & 0xC0000000) != 0xC0000000) {
		pr_warn_once("%s: Expecting an uncached alias for dma_addr %pad\n",
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

	mutex_unlock(&buffer->lock);

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

/* Userspace handling */
/*
 * Open the device.  Creates a private state to help track all allocation
 * associated with this device.
 */
static int vc_sm_cma_open(struct inode *inode, struct file *file)
{
	/* Make sure the device was started properly. */
	if (!sm_state) {
		pr_err("[%s]: invalid device\n", __func__);
		return -EPERM;
	}

	file->private_data = vc_sm_cma_create_priv_data(current->tgid);
	if (!file->private_data) {
		pr_err("[%s]: failed to create data tracker\n", __func__);

		return -ENOMEM;
	}

	return 0;
}

/*
 * Close the vcsm-cma device.
 * All allocations are file descriptors to the dmabuf objects, so we will get
 * the clean up request on those as those are cleaned up.
 */
static int vc_sm_cma_release(struct inode *inode, struct file *file)
{
	struct vc_sm_privdata_t *file_data =
	    (struct vc_sm_privdata_t *)file->private_data;
	int ret = 0;

	/* Make sure the device was started properly. */
	if (!sm_state || !file_data) {
		pr_err("[%s]: invalid device\n", __func__);
		ret = -EPERM;
		goto out;
	}

	pr_debug("[%s]: using private data %p\n", __func__, file_data);

	/* Terminate the private data. */
	kfree(file_data);

out:
	return ret;
}

/*
 * Allocate a shared memory handle and block.
 * Allocation is from CMA, and then imported into the VPU mappings.
 */
int vc_sm_cma_ioctl_alloc(struct vc_sm_privdata_t *private,
			  struct vc_sm_cma_ioctl_alloc *ioparam)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct vc_sm_buffer *buffer = NULL;
	struct vc_sm_import import = { 0 };
	struct vc_sm_import_result result = { 0 };
	struct dma_buf *dmabuf = NULL;
	struct sg_table *sgt;
	int aligned_size;
	int ret = 0;
	int status;
	int fd = -1;

	aligned_size = PAGE_ALIGN(ioparam->size);

	if (!aligned_size)
		return -EINVAL;

	/* Allocate local buffer to track this allocation. */
	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto error;
	}

	buffer->cookie = dma_alloc_coherent(&sm_state->pdev->dev,
					    aligned_size,
					    &buffer->dma_addr,
					    GFP_KERNEL);
	if (!buffer->cookie) {
		pr_err("[%s]: dma_alloc_coherent alloc of %d bytes failed\n",
		       __func__, aligned_size);
		ret = -ENOMEM;
		goto error;
	}

	import.type = VC_SM_ALLOC_NON_CACHED;
	import.allocator = current->tgid;

	if (*ioparam->name)
		memcpy(import.name, ioparam->name, sizeof(import.name) - 1);
	else
		memcpy(import.name, VC_SM_RESOURCE_NAME_DEFAULT,
		       sizeof(VC_SM_RESOURCE_NAME_DEFAULT));

	mutex_init(&buffer->lock);
	INIT_LIST_HEAD(&buffer->attachments);
	memcpy(buffer->name, import.name,
	       min(sizeof(buffer->name), sizeof(import.name) - 1));

	exp_info.ops = &dma_buf_ops;
	exp_info.size = aligned_size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buffer;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto error;
	}
	buffer->dma_buf = dmabuf;

	import.addr = buffer->dma_addr;
	import.size = aligned_size;
	import.kernel_id = get_kernel_id(buffer);

	/* Wrap it into a videocore buffer. */
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
		pr_err("[%s]: failed to import memory on videocore (status: %u, trans_id: %u)\n",
		       __func__, status, sm_state->int_trans_id);
		ret = -ENOMEM;
		goto error;
	}

	/* Keep track of the buffer we created. */
	buffer->private = private;
	buffer->vc_handle = result.res_handle;
	buffer->size = import.size;
	buffer->vpu_state = VPU_MAPPED;
	buffer->kernel_id = import.kernel_id;

	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto error;
	}

	ret = dma_get_sgtable(&sm_state->pdev->dev, sgt, buffer->cookie,
			      buffer->dma_addr, buffer->size);
	if (ret < 0) {
		/* FIXME: error handling */
		pr_err("failed to get scatterlist from DMA API\n");
		kfree(sgt);
		ret = -ENOMEM;
		goto error;
	}
	buffer->alloc.sg_table = sgt;

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		goto error;

	vc_sm_add_resource(private, buffer);

	pr_debug("[%s]: Added resource as fd %d, buffer %p, private %p, dma_addr %pad\n",
		 __func__, fd, buffer, private, &buffer->dma_addr);

	/* We're done */
	ioparam->handle = fd;
	ioparam->vc_handle = buffer->vc_handle;
	ioparam->dma_addr = buffer->dma_addr;
	return 0;

error:
	pr_err("[%s]: something failed - cleanup. ret %d\n", __func__, ret);

	if (dmabuf) {
		/* dmabuf has been exported, therefore allow dmabuf cleanup to
		 * deal with this
		 */
		dma_buf_put(dmabuf);
	} else {
		/* No dmabuf, therefore just free the buffer here */
		if (buffer->cookie)
			dma_free_coherent(&sm_state->pdev->dev, buffer->size,
					  buffer->cookie, buffer->dma_addr);
		kfree(buffer);
	}
	return ret;
}

#ifndef CONFIG_ARM64
/* Converts VCSM_CACHE_OP_* to an operating function. */
static void (*cache_op_to_func(const unsigned int cache_op))
						(const void*, const void*)
{
	switch (cache_op) {
	case VC_SM_CACHE_OP_NOP:
		return NULL;

	case VC_SM_CACHE_OP_INV:
		return dmac_inv_range;

	case VC_SM_CACHE_OP_CLEAN:
		return dmac_clean_range;

	case VC_SM_CACHE_OP_FLUSH:
		return dmac_flush_range;

	default:
		pr_err("[%s]: Invalid cache_op: 0x%08x\n", __func__, cache_op);
		return NULL;
	}
}

/*
 * Clean/invalid/flush cache of which buffer is already pinned (i.e. accessed).
 */
static int clean_invalid_contig_2d(const void __user *addr,
				   const size_t block_count,
				   const size_t block_size,
				   const size_t stride,
				   const unsigned int cache_op)
{
	size_t i;
	void (*op_fn)(const void *start, const void *end);

	if (!block_size) {
		pr_err("[%s]: size cannot be 0\n", __func__);
		return -EINVAL;
	}

	op_fn = cache_op_to_func(cache_op);
	if (!op_fn)
		return -EINVAL;

	for (i = 0; i < block_count; i ++, addr += stride)
		op_fn(addr, addr + block_size);

	return 0;
}

static int vc_sm_cma_clean_invalid2(unsigned int cmdnr, unsigned long arg)
{
	struct vc_sm_cma_ioctl_clean_invalid2 ioparam;
	struct vc_sm_cma_ioctl_clean_invalid_block *block = NULL;
	int i, ret = 0;

	/* Get parameter data. */
	if (copy_from_user(&ioparam, (void *)arg, sizeof(ioparam))) {
		pr_err("[%s]: failed to copy-from-user header for cmd %x\n",
		       __func__, cmdnr);
		return -EFAULT;
	}
	block = kmalloc(ioparam.op_count * sizeof(*block), GFP_KERNEL);
	if (!block)
		return -EFAULT;

	if (copy_from_user(block, (void *)(arg + sizeof(ioparam)),
			   ioparam.op_count * sizeof(*block)) != 0) {
		pr_err("[%s]: failed to copy-from-user payload for cmd %x\n",
		       __func__, cmdnr);
		ret = -EFAULT;
		goto out;
	}

	for (i = 0; i < ioparam.op_count; i++) {
		const struct vc_sm_cma_ioctl_clean_invalid_block * const op =
								block + i;

		if (op->invalidate_mode == VC_SM_CACHE_OP_NOP)
			continue;

		ret = clean_invalid_contig_2d((void __user *)op->start_address,
					      op->block_count, op->block_size,
					      op->inter_block_stride,
					      op->invalidate_mode);
		if (ret)
			break;
	}
out:
	kfree(block);

	return ret;
}
#endif

static long vc_sm_cma_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	int ret = 0;
	unsigned int cmdnr = _IOC_NR(cmd);
	struct vc_sm_privdata_t *file_data =
	    (struct vc_sm_privdata_t *)file->private_data;

	/* Validate we can work with this device. */
	if (!sm_state || !file_data) {
		pr_err("[%s]: invalid device\n", __func__);
		return -EPERM;
	}

	/* Action is a re-post of a previously interrupted action? */
	if (file_data->restart_sys == -EINTR) {
		struct vc_sm_action_clean_t action_clean;

		pr_debug("[%s]: clean up of action %u (trans_id: %u) following EINTR\n",
			 __func__, file_data->int_action,
			 file_data->int_trans_id);

		action_clean.res_action = file_data->int_action;
		action_clean.action_trans_id = file_data->int_trans_id;

		file_data->restart_sys = 0;
	}

	/* Now process the command. */
	switch (cmdnr) {
		/* New memory allocation.
		 */
	case VC_SM_CMA_CMD_ALLOC:
	{
		struct vc_sm_cma_ioctl_alloc ioparam;

		/* Get the parameter data. */
		if (copy_from_user
		    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
			pr_err("[%s]: failed to copy-from-user for cmd %x\n",
			       __func__, cmdnr);
			ret = -EFAULT;
			break;
		}

		ret = vc_sm_cma_ioctl_alloc(file_data, &ioparam);
		if (!ret &&
		    (copy_to_user((void *)arg, &ioparam,
				  sizeof(ioparam)) != 0)) {
			/* FIXME: Release allocation */
			pr_err("[%s]: failed to copy-to-user for cmd %x\n",
			       __func__, cmdnr);
			ret = -EFAULT;
		}
		break;
	}

	case VC_SM_CMA_CMD_IMPORT_DMABUF:
	{
		struct vc_sm_cma_ioctl_import_dmabuf ioparam;
		struct dma_buf *new_dmabuf;

		/* Get the parameter data. */
		if (copy_from_user
		    (&ioparam, (void *)arg, sizeof(ioparam)) != 0) {
			pr_err("[%s]: failed to copy-from-user for cmd %x\n",
			       __func__, cmdnr);
			ret = -EFAULT;
			break;
		}

		ret = vc_sm_cma_import_dmabuf_internal(file_data,
						       NULL,
						       ioparam.dmabuf_fd,
						       &new_dmabuf);

		if (!ret) {
			struct vc_sm_buffer *buf = new_dmabuf->priv;

			ioparam.size = buf->size;
			ioparam.handle = dma_buf_fd(new_dmabuf,
						    O_CLOEXEC);
			ioparam.vc_handle = buf->vc_handle;
			ioparam.dma_addr = buf->dma_addr;

			if (ioparam.handle < 0 ||
			    (copy_to_user((void *)arg, &ioparam,
					  sizeof(ioparam)) != 0)) {
				dma_buf_put(new_dmabuf);
				/* FIXME: Release allocation */
				ret = -EFAULT;
			}
		}
		break;
	}

#ifndef CONFIG_ARM64
	/*
	 * Flush/Invalidate the cache for a given mapping.
	 * Blocks must be pinned (i.e. accessed) before this call.
	 */
	case VC_SM_CMA_CMD_CLEAN_INVALID2:
		ret = vc_sm_cma_clean_invalid2(cmdnr, arg);
		break;
#endif

	default:
		pr_debug("[%s]: cmd %x tgid %u, owner %u\n", __func__, cmdnr,
			 current->tgid, file_data->pid);

		ret = -EINVAL;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
struct vc_sm_cma_ioctl_clean_invalid2_32 {
	u32 op_count;
	struct vc_sm_cma_ioctl_clean_invalid_block_32 {
		u16 invalidate_mode;
		u16 block_count;
		compat_uptr_t start_address;
		u32 block_size;
		u32 inter_block_stride;
	} s[0];
};

#define VC_SM_CMA_CMD_CLEAN_INVALID2_32\
	_IOR(VC_SM_CMA_MAGIC_TYPE, VC_SM_CMA_CMD_CLEAN_INVALID2,\
	 struct vc_sm_cma_ioctl_clean_invalid2_32)

static long vc_sm_cma_compat_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	switch (cmd) {
	case VC_SM_CMA_CMD_CLEAN_INVALID2_32:
		/* FIXME */
		return -EINVAL;

	default:
		return vc_sm_cma_ioctl(file, cmd, arg);
	}
}
#endif

/* Device operations that we managed in this driver. */
static const struct file_operations vc_sm_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = vc_sm_cma_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = vc_sm_cma_compat_ioctl,
#endif
	.open = vc_sm_cma_open,
	.release = vc_sm_cma_release,
};

/* Driver load/unload functions */
/* Videocore connected.  */
static void vc_sm_connected_init(void)
{
	int ret;
	VCHI_INSTANCE_T vchi_instance;
	struct vc_sm_version version;
	struct vc_sm_result_t version_result;

	pr_info("[%s]: start\n", __func__);

	/*
	 * Initialize and create a VCHI connection for the shared memory service
	 * running on videocore.
	 */
	ret = vchi_initialise(&vchi_instance);
	if (ret) {
		pr_err("[%s]: failed to initialise VCHI instance (ret=%d)\n",
		       __func__, ret);

		return;
	}

	ret = vchi_connect(vchi_instance);
	if (ret) {
		pr_err("[%s]: failed to connect VCHI instance (ret=%d)\n",
		       __func__, ret);

		return;
	}

	/* Initialize an instance of the shared memory service. */
	sm_state->sm_handle = vc_sm_cma_vchi_init(vchi_instance, 1,
						  vc_sm_vpu_event);
	if (!sm_state->sm_handle) {
		pr_err("[%s]: failed to initialize shared memory service\n",
		       __func__);

		return;
	}

	/* Create a debug fs directory entry (root). */
	sm_state->dir_root = debugfs_create_dir(VC_SM_DIR_ROOT_NAME, NULL);

	sm_state->dir_state.show = &vc_sm_cma_global_state_show;
	sm_state->dir_state.dir_entry =
		debugfs_create_file(VC_SM_STATE, 0444, sm_state->dir_root,
				    &sm_state->dir_state,
				    &vc_sm_cma_debug_fs_fops);

	INIT_LIST_HEAD(&sm_state->buffer_list);

	/* Create a shared memory device. */
	sm_state->misc_dev.minor = MISC_DYNAMIC_MINOR;
	sm_state->misc_dev.name = DEVICE_NAME;
	sm_state->misc_dev.fops = &vc_sm_ops;
	sm_state->misc_dev.parent = NULL;
	/* Temporarily set as 666 until udev rules have been sorted */
	sm_state->misc_dev.mode = 0666;
	ret = misc_register(&sm_state->misc_dev);
	if (ret) {
		pr_err("vcsm-cma: failed to register misc device.\n");
		goto err_remove_debugfs;
	}

	sm_state->data_knl = vc_sm_cma_create_priv_data(0);
	if (!sm_state->data_knl) {
		pr_err("[%s]: failed to create kernel private data tracker\n",
		       __func__);
		goto err_remove_misc_dev;
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

err_remove_misc_dev:
	misc_deregister(&sm_state->misc_dev);
err_remove_debugfs:
	debugfs_remove_recursive(sm_state->dir_root);
	vc_sm_cma_vchi_stop(&sm_state->sm_handle);
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
		misc_deregister(&sm_state->misc_dev);

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

/* Kernel API calls */
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
					       -1, &new_dma_buf);

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
