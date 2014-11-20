/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/io.h>

#include "uapi/drm/vc4_drm.h"
#include "vc4_drv.h"
#include "vc4_regs.h"

static void
vc4_queue_hangcheck(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	mod_timer(&vc4->hangcheck.timer,
		  round_jiffies_up(jiffies + msecs_to_jiffies(100)));
}

static void
vc4_reset(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	DRM_INFO("Resetting GPU.\n");
	vc4_v3d_set_power(vc4, false);
	vc4_v3d_set_power(vc4, true);

	vc4_irq_reset(dev);
}

static void
vc4_reset_work(struct work_struct *work)
{
	struct vc4_dev *vc4 =
		container_of(work, struct vc4_dev, hangcheck.reset_work);

	vc4_reset(vc4->dev);
}

static void
vc4_hangcheck_elapsed(unsigned long data)
{
	struct drm_device *dev = (struct drm_device *)data;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	uint32_t ct0ca, ct1ca;

	/* If idle, we can stop watching for hangs. */
	if (vc4->frame_done)
		return;

	ct0ca = V3D_READ(V3D_CTNCA(0));
	ct1ca = V3D_READ(V3D_CTNCA(1));

	/* If we've made any progress in execution, rearm the timer
	 * and wait.
	 */
	if (ct0ca != vc4->hangcheck.last_ct0ca ||
	    ct1ca != vc4->hangcheck.last_ct1ca) {
		vc4->hangcheck.last_ct0ca = ct0ca;
		vc4->hangcheck.last_ct1ca = ct1ca;
		vc4_queue_hangcheck(dev);
		return;
	}

	/* We've gone too long with no progress, reset.  This has to
	 * be done from a work struct, since resetting can sleep and
	 * this timer hook isn't allowed to.
	 */
	schedule_work(&vc4->hangcheck.reset_work);
}

static void
submit_cl(struct drm_device *dev, uint32_t thread, uint32_t start, uint32_t end)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	/* Stop any existing thread and set state to "stopped at halt" */
	V3D_WRITE(V3D_CTNCS(thread), V3D_CTRUN);
	barrier();

	V3D_WRITE(V3D_CTNCA(thread), start);
	barrier();

	/* Set the end address of the control list.  Writing this
	 * register is what starts the job.
	 */
	V3D_WRITE(V3D_CTNEA(thread), end);
	barrier();
}

static int
vc4_wait_for_job(struct drm_device *dev, struct vc4_exec_info *exec,
		 unsigned timeout)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int ret = 0;
	unsigned long timeout_expire;
	DEFINE_WAIT(wait);

	if (vc4->frame_done)
		return 0;

	timeout_expire = jiffies + msecs_to_jiffies(timeout);

	for (;;) {
		prepare_to_wait(&vc4->frame_done_queue, &wait,
				TASK_UNINTERRUPTIBLE);

		if (time_after_eq(jiffies, timeout_expire)) {
			ret = -ETIME;
			break;
		}

		if (vc4->frame_done)
			break;

		schedule_timeout(timeout_expire - jiffies);
	}

	finish_wait(&vc4->frame_done_queue, &wait);

	if (ret) {
		DRM_ERROR("timeout waiting for render thread idle\n");
		return ret;
	}

	return 0;
}

static void
vc4_flush_caches(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	/* Flush the GPU L2 caches.  These caches sit on top of system
	 * L3 (the 128kb or so shared with the CPU), and are
	 * non-allocating in the L3.
	 */
	V3D_WRITE(V3D_L2CACTL,
		  V3D_L2CACTL_L2CCLR);

	V3D_WRITE(V3D_SLCACTL,
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_T1CC) |
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_T0CC) |
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_UCC) |
		  VC4_SET_FIELD(0xf, V3D_SLCACTL_ICC));

	/* Flush the CPU L1/L2 caches.  Since the GPU reads from L3
	 * don't snoop up the L1/L2, we have to either do this or
	 * manually clflush the cachelines we (and userspace) dirtied.
	 */
	flush_cache_all();

	barrier();
}

static int
vc4_submit(struct drm_device *dev, struct vc4_exec_info *exec)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	uint32_t ct0ca = exec->ct0ca, ct0ea = exec->ct0ea;
	uint32_t ct1ca = exec->ct1ca, ct1ea = exec->ct1ea;
	int ret;

	vc4_flush_caches(dev);

	/* Disable the binner's pre-loaded overflow memory address */
	V3D_WRITE(V3D_BPOA, 0);
	V3D_WRITE(V3D_BPOS, 0);

	vc4->frame_done = false;
	submit_cl(dev, 0, ct0ca, ct0ea);
	submit_cl(dev, 1, ct1ca, ct1ea);

	vc4_queue_hangcheck(dev);

	ret = vc4_wait_for_job(dev, exec, 10000);
	if (ret)
		return ret;

	return 0;
}

/**
 * Looks up a bunch of GEM handles for BOs and stores the array for
 * use in the command validator that actually writes relocated
 * addresses pointing to them.
 */
static int
vc4_cl_lookup_bos(struct drm_device *dev,
		  struct drm_file *file_priv,
		  struct vc4_exec_info *exec)
{
	struct drm_vc4_submit_cl *args = exec->args;
	uint32_t *handles;
	int ret = 0;
	int i;

	exec->bo_count = args->bo_handle_count;

	if (!exec->bo_count) {
		/* See comment on bo_index for why we have to check
		 * this.
		 */
		DRM_ERROR("Rendering requires BOs to validate\n");
		return -EINVAL;
	}

	exec->bo = kcalloc(exec->bo_count, sizeof(struct vc4_bo_exec_state),
			   GFP_KERNEL);
	if (!exec->bo) {
		DRM_ERROR("Failed to allocate validated BO pointers\n");
		return -ENOMEM;
	}

	handles = drm_malloc_ab(exec->bo_count, sizeof(uint32_t));
	if (!handles) {
		DRM_ERROR("Failed to allocate incoming GEM handles\n");
		goto fail;
	}

	ret = copy_from_user(handles, args->bo_handles,
			     exec->bo_count * sizeof(uint32_t));
	if (ret) {
		DRM_ERROR("Failed to copy in GEM handles\n");
		goto fail;
	}

	for (i = 0; i < exec->bo_count; i++) {
		struct drm_gem_object *bo;

		bo = drm_gem_object_lookup(dev, file_priv, handles[i]);
		if (!bo) {
			DRM_ERROR("Failed to look up GEM BO %d: %d\n",
				  i, handles[i]);
			ret = -EINVAL;
			goto fail;
		}
		exec->bo[i].bo = (struct drm_gem_cma_object *)bo;
	}

fail:
	kfree(handles);
	return 0;
}

static int
vc4_cl_validate(struct drm_device *dev, struct vc4_exec_info *exec)
{
	struct drm_vc4_submit_cl *args = exec->args;
	void *temp = NULL;
	void *bin, *render;
	int ret = 0;
	uint32_t bin_offset = 0;
	uint32_t render_offset = bin_offset + args->bin_cl_size;
	uint32_t shader_rec_offset = roundup(render_offset +
					     args->render_cl_size, 16);
	uint32_t uniforms_offset = shader_rec_offset + args->shader_rec_size;
	uint32_t exec_size = uniforms_offset + args->uniforms_size;
	uint32_t temp_size = exec_size + (sizeof(struct vc4_shader_state) *
					  args->shader_rec_count);

	if (shader_rec_offset < render_offset ||
	    uniforms_offset < shader_rec_offset ||
	    exec_size < uniforms_offset ||
	    args->shader_rec_count >= (UINT_MAX /
					  sizeof(struct vc4_shader_state)) ||
	    temp_size < exec_size) {
		DRM_ERROR("overflow in exec arguments\n");
		goto fail;
	}

	/* Allocate space where we'll store the copied in user command lists
	 * and shader records.
	 *
	 * We don't just copy directly into the BOs because we need to
	 * read the contents back for validation, and I think the
	 * bo->vaddr is uncached access.
	 */
	temp = kmalloc(temp_size, GFP_KERNEL);
	if (!temp) {
		DRM_ERROR("Failed to allocate storage for copying "
			  "in bin/render CLs.\n");
		ret = -ENOMEM;
		goto fail;
	}
	bin = temp + bin_offset;
	render = temp + render_offset;
	exec->shader_rec_u = temp + shader_rec_offset;
	exec->uniforms_u = temp + uniforms_offset;
	exec->shader_state = temp + exec_size;
	exec->shader_state_size = args->shader_rec_count;

	ret = copy_from_user(bin, args->bin_cl, args->bin_cl_size);
	if (ret) {
		DRM_ERROR("Failed to copy in bin cl\n");
		goto fail;
	}

	ret = copy_from_user(render, args->render_cl, args->render_cl_size);
	if (ret) {
		DRM_ERROR("Failed to copy in render cl\n");
		goto fail;
	}

	ret = copy_from_user(exec->shader_rec_u, args->shader_rec,
			     args->shader_rec_size);
	if (ret) {
		DRM_ERROR("Failed to copy in shader recs\n");
		goto fail;
	}

	ret = copy_from_user(exec->uniforms_u, args->uniforms,
			     args->uniforms_size);
	if (ret) {
		DRM_ERROR("Failed to copy in uniforms cl\n");
		goto fail;
	}

	exec->exec_bo = drm_gem_cma_create(dev, exec_size);
	if (IS_ERR(exec->exec_bo)) {
		DRM_ERROR("Couldn't allocate BO for exec\n");
		ret = PTR_ERR(exec->exec_bo);
		exec->exec_bo = NULL;
		goto fail;
	}

	exec->ct0ca = exec->exec_bo->paddr + bin_offset;
	exec->ct1ca = exec->exec_bo->paddr + render_offset;

	exec->shader_rec_v = exec->exec_bo->vaddr + shader_rec_offset;
	exec->shader_rec_p = exec->exec_bo->paddr + shader_rec_offset;
	exec->shader_rec_size = args->shader_rec_size;

	exec->uniforms_v = exec->exec_bo->vaddr + uniforms_offset;
	exec->uniforms_p = exec->exec_bo->paddr + uniforms_offset;
	exec->uniforms_size = args->uniforms_size;

	ret = vc4_validate_cl(dev,
			      exec->exec_bo->vaddr + bin_offset,
			      bin,
			      args->bin_cl_size,
			      true,
			      exec);
	if (ret)
		goto fail;

	ret = vc4_validate_cl(dev,
			      exec->exec_bo->vaddr + render_offset,
			      render,
			      args->render_cl_size,
			      false,
			      exec);
	if (ret)
		goto fail;

	ret = vc4_validate_shader_recs(dev, exec);

fail:
	kfree(temp);
	return ret;
}

/**
 * Submits a command list to the VC4.
 *
 * This is what is called batchbuffer emitting on other hardware.
 */
int
vc4_submit_cl_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file_priv)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_exec_info exec;
	int ret;
	int i;

	memset(&exec, 0, sizeof(exec));
	exec.args = data;

	mutex_lock(&dev->struct_mutex);

	ret = vc4_cl_lookup_bos(dev, file_priv, &exec);
	if (ret)
		goto fail;

	ret = vc4_cl_validate(dev, &exec);
	if (ret)
		goto fail;

	ret = vc4_submit(dev, &exec);
	if (ret)
		goto fail;

fail:
	if (exec.bo) {
		for (i = 0; i < exec.bo_count; i++)
			drm_gem_object_unreference(&exec.bo[i].bo->base);
		kfree(exec.bo);
	}

	/* Release all but the very last overflow list entry (which is
	 * still sitting in the BPOS/BPOA registers for use by the
	 * next job).
	 */
	while (true) {
		struct vc4_bo_list_entry *entry =
			list_first_entry(&vc4->overflow_list,
					 struct vc4_bo_list_entry, head);

		if (entry->head.next == &vc4->overflow_list)
			break;

		drm_gem_object_unreference(&entry->bo->base);
		list_del(&entry->head);
		kfree(entry);
	}

	drm_gem_object_unreference(&exec.exec_bo->base);

	mutex_unlock(&dev->struct_mutex);

	return ret;
}

void
vc4_gem_init(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	INIT_LIST_HEAD(&vc4->overflow_list);

	INIT_WORK(&vc4->hangcheck.reset_work, vc4_reset_work);
	setup_timer(&vc4->hangcheck.timer,
		    vc4_hangcheck_elapsed,
		    (unsigned long) dev);
}
