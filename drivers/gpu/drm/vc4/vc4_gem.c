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

	/* Rearm the hangcheck -- another job might have been waiting
	 * for our hung one to get kicked off, and vc4_irq_reset()
	 * would have started it.
	 */
	vc4_queue_hangcheck(dev);
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
	if (list_empty(&vc4->job_list))
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
vc4_wait_for_seqno(struct drm_device *dev, uint64_t seqno, uint64_t timeout_ns)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int ret = 0;
	unsigned long timeout_expire;
	DEFINE_WAIT(wait);

	if (vc4->finished_seqno >= seqno)
		return 0;

	if (timeout_ns == 0)
		return -ETIME;

	timeout_expire = jiffies + nsecs_to_jiffies(timeout_ns);

	for (;;) {
		prepare_to_wait(&vc4->job_wait_queue, &wait,
				TASK_INTERRUPTIBLE);

		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}

		if (time_after_eq(jiffies, timeout_expire)) {
			ret = -ETIME;
			break;
		}

		if (vc4->finished_seqno >= seqno)
			break;

		schedule_timeout(timeout_expire - jiffies);
	}

	finish_wait(&vc4->job_wait_queue, &wait);

	if (ret && ret != -ERESTARTSYS) {
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

/* Sets the registers for the next job to be actually be executed in
 * the hardware.
 *
 * The job_lock should be held during this.
 */
void
vc4_submit_next_job(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_exec_info *exec = vc4_first_job(vc4);

	if (!exec)
		return;

	vc4_flush_caches(dev);

	/* Disable the binner's pre-loaded overflow memory address */
	V3D_WRITE(V3D_BPOA, 0);
	V3D_WRITE(V3D_BPOS, 0);

	submit_cl(dev, 0, exec->ct0ca, exec->ct0ea);
	submit_cl(dev, 1, exec->ct1ca, exec->ct1ea);
}

static void
vc4_update_bo_seqnos(struct vc4_exec_info *exec, uint64_t seqno)
{
	struct vc4_bo *bo;
	unsigned i;

	for (i = 0; i < exec->bo_count; i++) {
		bo = to_vc4_bo(&exec->bo[i].bo->base);
		bo->seqno = seqno;
	}

	list_for_each_entry(bo, &exec->unref_list, unref_head) {
		bo->seqno = seqno;
	}
}

/* Queues a struct vc4_exec_info for execution.  If no job is
 * currently executing, then submits it.
 *
 * Unlike most GPUs, our hardware only handles one command list at a
 * time.  To queue multiple jobs at once, we'd need to edit the
 * previous command list to have a jump to the new one at the end, and
 * then bump the end address.  That's a change for a later date,
 * though.
 */
static void
vc4_queue_submit(struct drm_device *dev, struct vc4_exec_info *exec)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	uint64_t seqno = ++vc4->emit_seqno;

	exec->seqno = seqno;
	vc4_update_bo_seqnos(exec, seqno);

	spin_lock(&vc4->job_lock);
	list_add_tail(&exec->head, &vc4->job_list);

	/* If no job was executing, kick ours off.  Otherwise, it'll
	 * get started when the previous job's frame done interrupt
	 * occurs.
	 */
	if (vc4_first_job(vc4) == exec) {
		vc4_submit_next_job(dev);
		vc4_queue_hangcheck(dev);
	}

	spin_unlock(&vc4->job_lock);
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

	list_add_tail(&to_vc4_bo(&exec->exec_bo->base)->unref_head,
		      &exec->unref_list);

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

static void
vc4_complete_exec(struct vc4_exec_info *exec)
{
	unsigned i;

	if (exec->bo) {
		for (i = 0; i < exec->bo_count; i++)
			drm_gem_object_unreference(&exec->bo[i].bo->base);
		kfree(exec->bo);
	}

	while (!list_empty(&exec->unref_list)) {
		struct vc4_bo *bo = list_first_entry(&exec->unref_list,
						     struct vc4_bo, unref_head);
		list_del(&bo->unref_head);
		drm_gem_object_unreference(&bo->base.base);
	}

	kfree(exec);
}

/* Scheduled when any job has been completed, this walks the list of
 * jobs that had completed and unrefs their BOs and frees their exec
 * structs.
 */
static void
vc4_job_done_work(struct work_struct *work)
{
	struct vc4_dev *vc4 =
		container_of(work, struct vc4_dev, job_done_work);
	struct drm_device *dev = vc4->dev;

	/* Need the struct lock for drm_gem_object_unreference(). */
	mutex_lock(&dev->struct_mutex);

	spin_lock(&vc4->job_lock);
	while (!list_empty(&vc4->job_done_list)) {
		struct vc4_exec_info *exec =
			list_first_entry(&vc4->job_done_list,
					 struct vc4_exec_info, head);
		list_del(&exec->head);

		spin_unlock(&vc4->job_lock);
		vc4_complete_exec(exec);
		spin_lock(&vc4->job_lock);
	}
	spin_unlock(&vc4->job_lock);

	mutex_unlock(&dev->struct_mutex);
}

int
vc4_wait_seqno_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file_priv)
{
	struct drm_vc4_wait_seqno *args = data;

	return vc4_wait_for_seqno(dev, args->seqno, args->timeout_ns);
}

int
vc4_wait_bo_ioctl(struct drm_device *dev, void *data,
		  struct drm_file *file_priv)
{
	int ret;
	struct drm_vc4_wait_bo *args = data;
	struct drm_gem_object *gem_obj;
	struct vc4_bo *bo;

	gem_obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (!gem_obj) {
		DRM_ERROR("Failed to look up GEM BO %d\n", args->handle);
		return -EINVAL;
	}
	bo = to_vc4_bo(gem_obj);

	ret = vc4_wait_for_seqno(dev, bo->seqno, args->timeout_ns);

	drm_gem_object_unreference(gem_obj);
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
	struct drm_vc4_submit_cl *args = data;
	struct vc4_exec_info *exec;
	int ret;

	exec = kcalloc(1, sizeof(*exec), GFP_KERNEL);
	if (!exec) {
		DRM_ERROR("malloc failure on exec struct\n");
		return -ENOMEM;
	}

	exec->args = args;
	INIT_LIST_HEAD(&exec->unref_list);

	mutex_lock(&dev->struct_mutex);

	ret = vc4_cl_lookup_bos(dev, file_priv, exec);
	if (ret)
		goto fail;

	ret = vc4_cl_validate(dev, exec);
	if (ret)
		goto fail;

	/* Return the seqno for our job. */
	args->seqno = vc4->emit_seqno;

	/* Clear this out of the struct we'll be putting in the queue,
	 * since it's part of our stack.
	 */
	exec->args = NULL;

	vc4_queue_submit(dev, exec);

	mutex_unlock(&dev->struct_mutex);
	return 0;

fail:
	vc4_complete_exec(exec);

	mutex_unlock(&dev->struct_mutex);

	return ret;
}

void
vc4_gem_init(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	INIT_LIST_HEAD(&vc4->job_list);
	INIT_LIST_HEAD(&vc4->job_done_list);
	spin_lock_init(&vc4->job_lock);

	INIT_WORK(&vc4->hangcheck.reset_work, vc4_reset_work);
	setup_timer(&vc4->hangcheck.timer,
		    vc4_hangcheck_elapsed,
		    (unsigned long) dev);

	INIT_WORK(&vc4->job_done_work, vc4_job_done_work);
}
