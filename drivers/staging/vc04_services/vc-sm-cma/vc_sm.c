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
 * This is cut down version to only support import of dma_bufs from
 * other kernel drivers. A more complete implementation of the old
 * vmcs_sm functionality can follow later.
 *
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

#include "vchiq_connected.h"
#include "vc_sm_cma_vchi.h"

#include "vc_sm.h"
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

	struct miscdevice dev;
	struct sm_instance *sm_handle;	/* Handle for videocore service. */

	struct mutex map_lock;          /* Global map lock. */
	struct list_head buffer_list;	/* List of buffer. */

	struct vc_sm_privdata_t *data_knl;  /* Kernel internal data tracking. */
	struct dentry *dir_root;	/* Debug fs entries root. */
	struct sm_pde_t dir_state;	/* Debug fs entries state sub-tree. */

	bool require_released_callback;	/* VPU will send a released msg when it
					 * has finished with a resource.
					 */
	u32 int_trans_id;		/* Interrupted transaction. */
};

/* ---- Private Variables ----------------------------------------------- */

static struct sm_state_t *sm_state;
static int sm_inited;

/* ---- Private Function Prototypes -------------------------------------- */

/* ---- Private Functions ------------------------------------------------ */

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

	seq_printf(s, "\nVC-ServiceHandle     0x%x\n",
		   (unsigned int)sm_state->sm_handle);

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
			seq_printf(s, "           SIZE         %d\n",
				   resource->size);
			seq_printf(s, "           DMABUF       %p\n",
				   resource->dma_buf);
			seq_printf(s, "           ATTACH       %p\n",
				   resource->attach);
			seq_printf(s, "           SG_TABLE     %p\n",
				   resource->sg_table);
			seq_printf(s, "           SGT          %p\n",
				   resource->sgt);
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

	pr_debug("[%s]: added buffer %p (name %s, size %d)\n",
		 __func__, buffer, buffer->name, buffer->size);
}

/*
 * Release an allocation.
 * All refcounting is done via the dma buf object.
 */
static void vc_sm_release_resource(struct vc_sm_buffer *buffer, int force)
{
	mutex_lock(&sm_state->map_lock);
	mutex_lock(&buffer->lock);

	pr_debug("[%s]: buffer %p (name %s, size %d)\n",
		 __func__, buffer, buffer->name, buffer->size);

	if (buffer->vc_handle && buffer->vpu_state == VPU_MAPPED) {
		struct vc_sm_free_t free = { buffer->vc_handle, 0 };
		int status = vc_sm_cma_vchi_free(sm_state->sm_handle, &free,
					     &sm_state->int_trans_id);
		if (status != 0 && status != -EINTR) {
			pr_err("[%s]: failed to free memory on videocore (status: %u, trans_id: %u)\n",
			       __func__, status, sm_state->int_trans_id);
		}

		if (sm_state->require_released_callback) {
			/* Need to wait for the VPU to confirm the free */

			/* Retain a reference on this until the VPU has
			 * released it
			 */
			buffer->vpu_state = VPU_UNMAPPING;
			goto defer;
		}
		buffer->vpu_state = VPU_NOT_MAPPED;
		buffer->vc_handle = 0;
	}
	if (buffer->vc_handle) {
		/* We've sent the unmap request but not had the response. */
		pr_err("[%s]: Waiting for VPU unmap response on %p\n",
		       __func__, buffer);
		goto defer;
	}
	if (buffer->in_use) {
		/* Don't release dmabuf here - we await the release */
		pr_err("[%s]: buffer %p is still in use\n",
		       __func__, buffer);
		goto defer;
	}

	/* Handle cleaning up imported dmabufs */
	if (buffer->sgt) {
		dma_buf_unmap_attachment(buffer->attach, buffer->sgt,
					 DMA_BIDIRECTIONAL);
		buffer->sgt = NULL;
	}
	if (buffer->attach) {
		dma_buf_detach(buffer->dma_buf, buffer->attach);
		buffer->attach = NULL;
	}

	/* Release the dma_buf (whether ours or imported) */
	if (buffer->import_dma_buf) {
		dma_buf_put(buffer->import_dma_buf);
		buffer->import_dma_buf = NULL;
		buffer->dma_buf = NULL;
	} else if (buffer->dma_buf) {
		dma_buf_put(buffer->dma_buf);
		buffer->dma_buf = NULL;
	}

	if (buffer->sg_table && !buffer->import_dma_buf) {
		/* Our own allocation that we need to dma_unmap_sg */
		dma_unmap_sg(&sm_state->pdev->dev, buffer->sg_table->sgl,
			     buffer->sg_table->nents, DMA_BIDIRECTIONAL);
	}

	/* Free the local resource. Start by removing it from the list */
	buffer->private = NULL;
	list_del(&buffer->global_buffer_list);

	mutex_unlock(&buffer->lock);
	mutex_unlock(&sm_state->map_lock);

	mutex_destroy(&buffer->lock);

	kfree(buffer);
	return;

defer:
	mutex_unlock(&buffer->lock);
	mutex_unlock(&sm_state->map_lock);
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

/* Dma_buf operations for chaining through to an imported dma_buf */
static
int vc_sm_import_dma_buf_attach(struct dma_buf *dmabuf,
				struct dma_buf_attachment *attachment)
{
	struct vc_sm_buffer *res = dmabuf->priv;

	if (!res->import_dma_buf)
		return -EINVAL;
	return res->import_dma_buf->ops->attach(res->import_dma_buf,
						attachment);
}

static
void vc_sm_import_dma_buf_detatch(struct dma_buf *dmabuf,
				  struct dma_buf_attachment *attachment)
{
	struct vc_sm_buffer *res = dmabuf->priv;

	if (!res->import_dma_buf)
		return;
	res->import_dma_buf->ops->detach(res->import_dma_buf, attachment);
}

static
struct sg_table *vc_sm_import_map_dma_buf(struct dma_buf_attachment *attachment,
					  enum dma_data_direction direction)
{
	struct vc_sm_buffer *res = attachment->dmabuf->priv;

	if (!res->import_dma_buf)
		return NULL;
	return res->import_dma_buf->ops->map_dma_buf(attachment, direction);
}

static
void vc_sm_import_unmap_dma_buf(struct dma_buf_attachment *attachment,
				struct sg_table *table,
				enum dma_data_direction direction)
{
	struct vc_sm_buffer *res = attachment->dmabuf->priv;

	if (!res->import_dma_buf)
		return;
	res->import_dma_buf->ops->unmap_dma_buf(attachment, table, direction);
}

static
int vc_sm_import_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct vc_sm_buffer *res = dmabuf->priv;

	pr_debug("%s: mmap dma_buf %p, res %p, imported db %p\n", __func__,
		 dmabuf, res, res->import_dma_buf);
	if (!res->import_dma_buf) {
		pr_err("%s: mmap dma_buf %p- not an imported buffer\n",
		       __func__, dmabuf);
		return -EINVAL;
	}
	return res->import_dma_buf->ops->mmap(res->import_dma_buf, vma);
}

static
void vc_sm_import_dma_buf_release(struct dma_buf *dmabuf)
{
	struct vc_sm_buffer *res = dmabuf->priv;

	pr_debug("%s: Relasing dma_buf %p\n", __func__, dmabuf);
	if (!res->import_dma_buf)
		return;

	res->in_use = 0;

	vc_sm_release_resource(res, 0);
}

static
void *vc_sm_import_dma_buf_kmap(struct dma_buf *dmabuf,
				unsigned long offset)
{
	struct vc_sm_buffer *res = dmabuf->priv;

	if (!res->import_dma_buf)
		return NULL;
	return res->import_dma_buf->ops->map(res->import_dma_buf,
						      offset);
}

static
void vc_sm_import_dma_buf_kunmap(struct dma_buf *dmabuf,
				 unsigned long offset, void *ptr)
{
	struct vc_sm_buffer *res = dmabuf->priv;

	if (!res->import_dma_buf)
		return;
	res->import_dma_buf->ops->unmap(res->import_dma_buf,
					       offset, ptr);
}

static
int vc_sm_import_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					  enum dma_data_direction direction)
{
	struct vc_sm_buffer *res = dmabuf->priv;

	if (!res->import_dma_buf)
		return -EINVAL;
	return res->import_dma_buf->ops->begin_cpu_access(res->import_dma_buf,
							    direction);
}

static
int vc_sm_import_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	struct vc_sm_buffer *res = dmabuf->priv;

	if (!res->import_dma_buf)
		return -EINVAL;
	return res->import_dma_buf->ops->end_cpu_access(res->import_dma_buf,
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
	import.addr = (uint32_t)sg_dma_address(sgt->sgl);
	if ((import.addr & 0xC0000000) != 0xC0000000) {
		pr_err("%s: Expecting an uncached alias for dma_addr %08x\n",
		       __func__, import.addr);
		import.addr |= 0xC0000000;
	}
	import.size = sg_dma_len(sgt->sgl);
	import.allocator = current->tgid;
	import.kernel_id = (uint32_t)buffer;	//FIXME: 64 bit support needed.

	memcpy(import.name, VC_SM_RESOURCE_NAME_DEFAULT,
	       sizeof(VC_SM_RESOURCE_NAME_DEFAULT));

	pr_debug("[%s]: attempt to import \"%s\" data - type %u, addr %p, size %u\n",
		 __func__, import.name, import.type, (void *)import.addr,
		 import.size);

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

	buffer->import_dma_buf = dma_buf;

	buffer->attach = attach;
	buffer->sgt = sgt;
	buffer->dma_addr = sg_dma_address(sgt->sgl);
	buffer->in_use = 1;

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
	kfree(buffer);
	if (sgt)
		dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
	if (attach)
		dma_buf_detach(dma_buf, attach);
	dma_buf_put(dma_buf);
	return ret;
}

/* FIXME: Pass a function pointer to this into vc_vchi_sm.c */
void
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
				(struct vc_sm_buffer *)release->kernel_id;

		/*
		 * FIXME: Need to check buffer is still valid and allocated
		 * before continuing
		 */
		pr_debug("%s: Released addr %08x, size %u, id %08x, mem_handle %08x\n",
			 __func__, release->addr, release->size,
			 release->kernel_id, release->vc_handle);
		mutex_lock(&buffer->lock);
		buffer->vc_handle = 0;
		buffer->vpu_state = VPU_NOT_MAPPED;
		mutex_unlock(&buffer->lock);

		vc_sm_release_resource(buffer, 0);
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

	/*
	 * Initialize and create a VCHI connection for the shared memory service
	 * running on videocore.
	 */
	ret = vchi_initialise(&vchi_instance);
	if (ret) {
		pr_err("[%s]: failed to initialise VCHI instance (ret=%d)\n",
		       __func__, ret);

		ret = -EIO;
		goto err_free_mem;
	}

	ret = vchi_connect(vchi_instance);
	if (ret) {
		pr_err("[%s]: failed to connect VCHI instance (ret=%d)\n",
		       __func__, ret);

		ret = -EIO;
		goto err_free_mem;
	}

	/* Initialize an instance of the shared memory service. */
	sm_state->sm_handle = vc_sm_cma_vchi_init(vchi_instance, 1,
						  vc_sm_vpu_event);
	if (!sm_state->sm_handle) {
		pr_err("[%s]: failed to initialize shared memory service\n",
		       __func__);

		ret = -EPERM;
		goto err_free_mem;
	}

	/* Create a debug fs directory entry (root). */
	sm_state->dir_root = debugfs_create_dir(VC_SM_DIR_ROOT_NAME, NULL);
	if (!sm_state->dir_root) {
		pr_err("[%s]: failed to create \'%s\' directory entry\n",
		       __func__, VC_SM_DIR_ROOT_NAME);

		ret = -EPERM;
		goto err_stop_sm_service;
	}

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

	version.version = 1;
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
err_stop_sm_service:
	vc_sm_cma_vchi_stop(&sm_state->sm_handle);
err_free_mem:
	kfree(sm_state);
	pr_info("[%s]: failed, ret %d\n", __func__, ret);
}

/* Driver loading. */
static int bcm2835_vc_sm_cma_probe(struct platform_device *pdev)
{
	pr_info("%s: Videocore shared memory driver\n", __func__);

	sm_state = kzalloc(sizeof(*sm_state), GFP_KERNEL);
	if (!sm_state)
		return -ENOMEM;
	sm_state->pdev = pdev;
	mutex_init(&sm_state->map_lock);

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
		/* Remove shared memory device. */
		misc_deregister(&sm_state->dev);

		/* Remove all proc entries. */
		//debugfs_remove_recursive(sm_state->dir_root);

		/* Stop the videocore shared memory service. */
		vc_sm_cma_vchi_stop(&sm_state->sm_handle);

		/* Free the memory for the state structure. */
		mutex_destroy(&sm_state->map_lock);
		kfree(sm_state);
	}

	pr_debug("[%s]: end\n", __func__);
	return 0;
}

/* Get an internal resource handle mapped from the external one. */
int vc_sm_cma_int_handle(int handle)
{
	struct dma_buf *dma_buf = (struct dma_buf *)handle;
	struct vc_sm_buffer *res;

	/* Validate we can work with this device. */
	if (!sm_state || !handle) {
		pr_err("[%s]: invalid input\n", __func__);
		return 0;
	}

	res = (struct vc_sm_buffer *)dma_buf->priv;
	return res->vc_handle;
}
EXPORT_SYMBOL_GPL(vc_sm_cma_int_handle);

/* Free a previously allocated shared memory handle and block. */
int vc_sm_cma_free(int handle)
{
	struct dma_buf *dma_buf = (struct dma_buf *)handle;

	/* Validate we can work with this device. */
	if (!sm_state || !handle) {
		pr_err("[%s]: invalid input\n", __func__);
		return -EPERM;
	}

	pr_debug("%s: handle %08x/dmabuf %p\n", __func__, handle, dma_buf);

	dma_buf_put(dma_buf);

	return 0;
}
EXPORT_SYMBOL_GPL(vc_sm_cma_free);

/* Import a dmabuf to be shared with VC. */
int vc_sm_cma_import_dmabuf(struct dma_buf *src_dmabuf, int *handle)
{
	struct dma_buf *new_dma_buf;
	struct vc_sm_buffer *res;
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
		res = (struct vc_sm_buffer *)new_dma_buf->priv;

		/* Assign valid handle at this time.*/
		*handle = (int)new_dma_buf;
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
