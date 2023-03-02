// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2018 Broadcom */

/**
 * DOC: Broadcom V3D scheduling
 *
 * The shared DRM GPU scheduler is used to coordinate submitting jobs
 * to the hardware.  Each DRM fd (roughly a client process) gets its
 * own scheduler entity, which will process jobs in order.  The GPU
 * scheduler will round-robin between clients to submit the next job.
 *
 * For simplicity, and in order to keep latency low for interactive
 * jobs when bulk background jobs are queued up, we submit a new job
 * to the HW only when it has completed the last one, instead of
 * filling up the CT[01]Q FIFOs with jobs.  Similarly, we use
 * drm_sched_job_add_dependency() to manage the dependency between bin and
 * render, instead of having the clients submit jobs using the HW's
 * semaphores to interlock between them.
 */

#include <linux/kthread.h>
#include <linux/sched/clock.h>

#include "v3d_drv.h"
#include "v3d_regs.h"
#include "v3d_trace.h"

static struct v3d_job *
to_v3d_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct v3d_job, base);
}

static struct v3d_bin_job *
to_bin_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct v3d_bin_job, base.base);
}

static struct v3d_render_job *
to_render_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct v3d_render_job, base.base);
}

static struct v3d_tfu_job *
to_tfu_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct v3d_tfu_job, base.base);
}

static struct v3d_csd_job *
to_csd_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct v3d_csd_job, base.base);
}

static void
v3d_sched_job_free(struct drm_sched_job *sched_job)
{
	struct v3d_job *job = to_v3d_job(sched_job);

	v3d_job_cleanup(job);
}

static void
v3d_switch_perfmon(struct v3d_dev *v3d, struct v3d_job *job)
{
	if (job->perfmon != v3d->active_perfmon)
		v3d_perfmon_stop(v3d, v3d->active_perfmon, true);

	if (job->perfmon && v3d->active_perfmon != job->perfmon)
		v3d_perfmon_start(v3d, job->perfmon);
}

/*
 * Updates the scheduling stats of the gpu queues runtime for completed jobs.
 *
 * It should be called before any new job submission to the queue or before
 * accessing the stats from the debugfs interface.
 *
 * It is expected that calls to this function are done with queue_stats->lock
 * locked.
 */
void
v3d_sched_stats_update(struct v3d_queue_stats *queue_stats)
{
	struct list_head *pid_stats_list = &queue_stats->pid_stats_list;
	struct v3d_queue_pid_stats *cur, *tmp;
	u64 runtime = 0;
	bool store_pid_stats =
		time_is_after_jiffies(queue_stats->gpu_pid_stats_timeout);

	/* If debugfs stats gpu_pid_usage has not been polled for a period,
	 * the pid stats collection is stopped and we purge any existing
	 * pid_stats.
	 *
	 * pid_stats are also purged for clients that have reached the
	 * timeout_purge because the process probably does not exist anymore.
	 */
	list_for_each_entry_safe_reverse(cur, tmp, pid_stats_list, list) {
		if (!store_pid_stats || time_is_before_jiffies(cur->timeout_purge)) {
			list_del(&cur->list);
			kfree(cur);
		} else {
			break;
		}
	}
	/* If a job has finished its stats are updated. */
	if (queue_stats->last_pid && queue_stats->last_exec_end) {
		runtime = queue_stats->last_exec_end -
			  queue_stats->last_exec_start;
		queue_stats->runtime += runtime;

		if (store_pid_stats) {
			struct v3d_queue_pid_stats *pid_stats;
			/* Last job info is always at the head of the list */
			pid_stats = list_first_entry_or_null(pid_stats_list,
				struct v3d_queue_pid_stats, list);
			if (pid_stats &&
			    pid_stats->pid == queue_stats->last_pid) {
				pid_stats->runtime += runtime;
			}
		}
		queue_stats->last_pid = 0;
	}
}

/*
 * Updates the queue usage adding the information of a new job that is
 * about to be sent to the GPU to be executed.
 */
int
v3d_sched_stats_add_job(struct v3d_queue_stats *queue_stats,
			struct drm_sched_job *sched_job)
{

	struct v3d_queue_pid_stats *pid_stats = NULL;
	struct v3d_job *job = sched_job?to_v3d_job(sched_job):NULL;
	struct v3d_queue_pid_stats *cur;
	struct list_head *pid_stats_list = &queue_stats->pid_stats_list;
	int ret = 0;

	mutex_lock(&queue_stats->lock);

	/* Completion of previous job requires an update of its runtime stats */
	v3d_sched_stats_update(queue_stats);

	queue_stats->last_exec_start = local_clock();
	queue_stats->last_exec_end = 0;
	queue_stats->jobs_sent++;
	queue_stats->last_pid = job->client_pid;

	/* gpu usage stats by process are being collected */
	if (time_is_after_jiffies(queue_stats->gpu_pid_stats_timeout)) {
		list_for_each_entry(cur, pid_stats_list, list) {
			if (cur->pid == job->client_pid) {
				pid_stats = cur;
				break;
			}
		}
		/* pid_stats of this client is moved to the head of the list. */
		if (pid_stats) {
			list_move(&pid_stats->list, pid_stats_list);
		} else {
			pid_stats = kzalloc(sizeof(struct v3d_queue_pid_stats),
					    GFP_KERNEL);
			if (!pid_stats) {
				ret = -ENOMEM;
				goto err_mem;
			}
			pid_stats->pid = job->client_pid;
			list_add(&pid_stats->list, pid_stats_list);
		}
		pid_stats->jobs_sent++;
		pid_stats->timeout_purge = jiffies + V3D_QUEUE_STATS_TIMEOUT;
	}

err_mem:
	mutex_unlock(&queue_stats->lock);
	return ret;
}

static struct dma_fence *v3d_bin_job_run(struct drm_sched_job *sched_job)
{
	struct v3d_bin_job *job = to_bin_job(sched_job);
	struct v3d_dev *v3d = job->base.v3d;
	struct drm_device *dev = &v3d->drm;
	struct dma_fence *fence;
	unsigned long irqflags;

	if (unlikely(job->base.base.s_fence->finished.error))
		return NULL;

	/* Lock required around bin_job update vs
	 * v3d_overflow_mem_work().
	 */
	spin_lock_irqsave(&v3d->job_lock, irqflags);
	v3d->bin_job = job;
	/* Clear out the overflow allocation, so we don't
	 * reuse the overflow attached to a previous job.
	 */
	V3D_CORE_WRITE(0, V3D_PTB_BPOS, 0);
	spin_unlock_irqrestore(&v3d->job_lock, irqflags);

	v3d_invalidate_caches(v3d);

	fence = v3d_fence_create(v3d, V3D_BIN);
	if (IS_ERR(fence))
		return NULL;

	if (job->base.irq_fence)
		dma_fence_put(job->base.irq_fence);
	job->base.irq_fence = dma_fence_get(fence);

	trace_v3d_submit_cl(dev, false, to_v3d_fence(fence)->seqno,
			    job->start, job->end);

	v3d_sched_stats_add_job(&v3d->gpu_queue_stats[V3D_BIN], sched_job);
	v3d_switch_perfmon(v3d, &job->base);

	/* Set the current and end address of the control list.
	 * Writing the end register is what starts the job.
	 */
	if (job->qma) {
		V3D_CORE_WRITE(0, V3D_CLE_CT0QMA, job->qma);
		V3D_CORE_WRITE(0, V3D_CLE_CT0QMS, job->qms);
	}
	if (job->qts) {
		V3D_CORE_WRITE(0, V3D_CLE_CT0QTS,
			       V3D_CLE_CT0QTS_ENABLE |
			       job->qts);
	}
	V3D_CORE_WRITE(0, V3D_CLE_CT0QBA, job->start);
	V3D_CORE_WRITE(0, V3D_CLE_CT0QEA, job->end);

	return fence;
}

static struct dma_fence *v3d_render_job_run(struct drm_sched_job *sched_job)
{
	struct v3d_render_job *job = to_render_job(sched_job);
	struct v3d_dev *v3d = job->base.v3d;
	struct drm_device *dev = &v3d->drm;
	struct dma_fence *fence;

	if (unlikely(job->base.base.s_fence->finished.error))
		return NULL;

	v3d->render_job = job;

	/* Can we avoid this flush?  We need to be careful of
	 * scheduling, though -- imagine job0 rendering to texture and
	 * job1 reading, and them being executed as bin0, bin1,
	 * render0, render1, so that render1's flush at bin time
	 * wasn't enough.
	 */
	v3d_invalidate_caches(v3d);

	fence = v3d_fence_create(v3d, V3D_RENDER);
	if (IS_ERR(fence))
		return NULL;

	if (job->base.irq_fence)
		dma_fence_put(job->base.irq_fence);
	job->base.irq_fence = dma_fence_get(fence);

	trace_v3d_submit_cl(dev, true, to_v3d_fence(fence)->seqno,
			    job->start, job->end);

	v3d_sched_stats_add_job(&v3d->gpu_queue_stats[V3D_RENDER], sched_job);
	v3d_switch_perfmon(v3d, &job->base);

	/* XXX: Set the QCFG */

	/* Set the current and end address of the control list.
	 * Writing the end register is what starts the job.
	 */
	V3D_CORE_WRITE(0, V3D_CLE_CT1QBA, job->start);
	V3D_CORE_WRITE(0, V3D_CLE_CT1QEA, job->end);

	return fence;
}

#define V3D_TFU_REG(name) ((v3d->ver < 71) ? V3D_TFU_ ## name : V3D_V7_TFU_ ## name)

static struct dma_fence *
v3d_tfu_job_run(struct drm_sched_job *sched_job)
{
	struct v3d_tfu_job *job = to_tfu_job(sched_job);
	struct v3d_dev *v3d = job->base.v3d;
	struct drm_device *dev = &v3d->drm;
	struct dma_fence *fence;

	fence = v3d_fence_create(v3d, V3D_TFU);
	if (IS_ERR(fence))
		return NULL;

	v3d->tfu_job = job;
	if (job->base.irq_fence)
		dma_fence_put(job->base.irq_fence);
	job->base.irq_fence = dma_fence_get(fence);

	trace_v3d_submit_tfu(dev, to_v3d_fence(fence)->seqno);

	v3d_sched_stats_add_job(&v3d->gpu_queue_stats[V3D_TFU], sched_job);
	V3D_WRITE(V3D_TFU_REG(IIA), job->args.iia);
	V3D_WRITE(V3D_TFU_REG(IIS), job->args.iis);
	V3D_WRITE(V3D_TFU_REG(ICA), job->args.ica);
	V3D_WRITE(V3D_TFU_REG(IUA), job->args.iua);
	V3D_WRITE(V3D_TFU_REG(IOA), job->args.ioa);
	if (v3d->ver >= 71)
		V3D_WRITE(V3D_V7_TFU_IOC, job->args.v71.ioc);
	V3D_WRITE(V3D_TFU_REG(IOS), job->args.ios);
	V3D_WRITE(V3D_TFU_REG(COEF0), job->args.coef[0]);
	if (v3d->ver >= 71 || (job->args.coef[0] & V3D_TFU_COEF0_USECOEF)) {
		V3D_WRITE(V3D_TFU_REG(COEF1), job->args.coef[1]);
		V3D_WRITE(V3D_TFU_REG(COEF2), job->args.coef[2]);
		V3D_WRITE(V3D_TFU_REG(COEF3), job->args.coef[3]);
	}
	/* ICFG kicks off the job. */
	V3D_WRITE(V3D_TFU_REG(ICFG), job->args.icfg | V3D_TFU_ICFG_IOC);

	return fence;
}

static struct dma_fence *
v3d_csd_job_run(struct drm_sched_job *sched_job)
{
	struct v3d_csd_job *job = to_csd_job(sched_job);
	struct v3d_dev *v3d = job->base.v3d;
	struct drm_device *dev = &v3d->drm;
	struct dma_fence *fence;
	int i, csd_cfg0_reg, csd_cfg_reg_count;

	v3d->csd_job = job;

	v3d_invalidate_caches(v3d);

	fence = v3d_fence_create(v3d, V3D_CSD);
	if (IS_ERR(fence))
		return NULL;

	if (job->base.irq_fence)
		dma_fence_put(job->base.irq_fence);
	job->base.irq_fence = dma_fence_get(fence);

	trace_v3d_submit_csd(dev, to_v3d_fence(fence)->seqno);

	v3d_sched_stats_add_job(&v3d->gpu_queue_stats[V3D_CSD], sched_job);
	v3d_switch_perfmon(v3d, &job->base);

	csd_cfg0_reg = v3d->ver < 71 ? V3D_CSD_QUEUED_CFG0 : V3D_V7_CSD_QUEUED_CFG0;
	csd_cfg_reg_count = v3d->ver < 71 ? 6 : 7;
	for (i = 1; i <= csd_cfg_reg_count; i++)
		V3D_CORE_WRITE(0, csd_cfg0_reg + 4 * i, job->args.cfg[i]);
	/* CFG0 write kicks off the job. */
	V3D_CORE_WRITE(0, csd_cfg0_reg, job->args.cfg[0]);

	return fence;
}

static struct dma_fence *
v3d_cache_clean_job_run(struct drm_sched_job *sched_job)
{
	struct v3d_job *job = to_v3d_job(sched_job);
	struct v3d_dev *v3d = job->v3d;

	v3d_sched_stats_add_job(&v3d->gpu_queue_stats[V3D_CACHE_CLEAN],
				sched_job);
	v3d_clean_caches(v3d);
	v3d->gpu_queue_stats[V3D_CACHE_CLEAN].last_exec_end = local_clock();

	return NULL;
}

static enum drm_gpu_sched_stat
v3d_gpu_reset_for_timeout(struct v3d_dev *v3d, struct drm_sched_job *sched_job)
{
	enum v3d_queue q;

	mutex_lock(&v3d->reset_lock);

	/* block scheduler */
	for (q = 0; q < V3D_MAX_QUEUES; q++)
		drm_sched_stop(&v3d->queue[q].sched, sched_job);

	if (sched_job)
		drm_sched_increase_karma(sched_job);

	/* get the GPU back into the init state */
	v3d_reset(v3d);

	for (q = 0; q < V3D_MAX_QUEUES; q++)
		drm_sched_resubmit_jobs(&v3d->queue[q].sched);

	/* Unblock schedulers and restart their jobs. */
	for (q = 0; q < V3D_MAX_QUEUES; q++) {
		drm_sched_start(&v3d->queue[q].sched, true);
	}

	mutex_unlock(&v3d->reset_lock);

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

/* If the current address or return address have changed, then the GPU
 * has probably made progress and we should delay the reset.  This
 * could fail if the GPU got in an infinite loop in the CL, but that
 * is pretty unlikely outside of an i-g-t testcase.
 */
static enum drm_gpu_sched_stat
v3d_cl_job_timedout(struct drm_sched_job *sched_job, enum v3d_queue q,
		    u32 *timedout_ctca, u32 *timedout_ctra)
{
	struct v3d_job *job = to_v3d_job(sched_job);
	struct v3d_dev *v3d = job->v3d;
	u32 ctca = V3D_CORE_READ(0, V3D_CLE_CTNCA(q));
	u32 ctra = V3D_CORE_READ(0, V3D_CLE_CTNRA(q));

	if (*timedout_ctca != ctca || *timedout_ctra != ctra) {
		*timedout_ctca = ctca;
		*timedout_ctra = ctra;
		return DRM_GPU_SCHED_STAT_NOMINAL;
	}

	return v3d_gpu_reset_for_timeout(v3d, sched_job);
}

static enum drm_gpu_sched_stat
v3d_bin_job_timedout(struct drm_sched_job *sched_job)
{
	struct v3d_bin_job *job = to_bin_job(sched_job);

	return v3d_cl_job_timedout(sched_job, V3D_BIN,
				   &job->timedout_ctca, &job->timedout_ctra);
}

static enum drm_gpu_sched_stat
v3d_render_job_timedout(struct drm_sched_job *sched_job)
{
	struct v3d_render_job *job = to_render_job(sched_job);

	return v3d_cl_job_timedout(sched_job, V3D_RENDER,
				   &job->timedout_ctca, &job->timedout_ctra);
}

static enum drm_gpu_sched_stat
v3d_generic_job_timedout(struct drm_sched_job *sched_job)
{
	struct v3d_job *job = to_v3d_job(sched_job);

	return v3d_gpu_reset_for_timeout(job->v3d, sched_job);
}

static enum drm_gpu_sched_stat
v3d_csd_job_timedout(struct drm_sched_job *sched_job)
{
	struct v3d_csd_job *job = to_csd_job(sched_job);
	struct v3d_dev *v3d = job->base.v3d;
	u32 batches = V3D_CORE_READ(0, (v3d->ver < 71 ? V3D_CSD_CURRENT_CFG4 :
							V3D_V7_CSD_CURRENT_CFG4));

	/* If we've made progress, skip reset and let the timer get
	 * rearmed.
	 */
	if (job->timedout_batches != batches) {
		job->timedout_batches = batches;
		return DRM_GPU_SCHED_STAT_NOMINAL;
	}

	return v3d_gpu_reset_for_timeout(v3d, sched_job);
}

static const struct drm_sched_backend_ops v3d_bin_sched_ops = {
	.run_job = v3d_bin_job_run,
	.timedout_job = v3d_bin_job_timedout,
	.free_job = v3d_sched_job_free,
};

static const struct drm_sched_backend_ops v3d_render_sched_ops = {
	.run_job = v3d_render_job_run,
	.timedout_job = v3d_render_job_timedout,
	.free_job = v3d_sched_job_free,
};

static const struct drm_sched_backend_ops v3d_tfu_sched_ops = {
	.run_job = v3d_tfu_job_run,
	.timedout_job = v3d_generic_job_timedout,
	.free_job = v3d_sched_job_free,
};

static const struct drm_sched_backend_ops v3d_csd_sched_ops = {
	.run_job = v3d_csd_job_run,
	.timedout_job = v3d_csd_job_timedout,
	.free_job = v3d_sched_job_free
};

static const struct drm_sched_backend_ops v3d_cache_clean_sched_ops = {
	.run_job = v3d_cache_clean_job_run,
	.timedout_job = v3d_generic_job_timedout,
	.free_job = v3d_sched_job_free
};

int
v3d_sched_init(struct v3d_dev *v3d)
{
	int hw_jobs_limit = 1;
	int job_hang_limit = 0;
	int hang_limit_ms = 500;
	enum v3d_queue q;
	int ret;

	for (q = 0; q < V3D_MAX_QUEUES; q++) {
		INIT_LIST_HEAD(&v3d->gpu_queue_stats[q].pid_stats_list);
		/* Setting timeout before current jiffies disables collecting
		 * pid_stats on scheduling init.
		 */
		v3d->gpu_queue_stats[q].gpu_pid_stats_timeout = jiffies - 1;
		mutex_init(&v3d->gpu_queue_stats[q].lock);
	}

	ret = drm_sched_init(&v3d->queue[V3D_BIN].sched,
			     &v3d_bin_sched_ops,
			     hw_jobs_limit, job_hang_limit,
			     msecs_to_jiffies(hang_limit_ms), NULL,
			     NULL, "v3d_bin", v3d->drm.dev);
	if (ret)
		return ret;

	ret = drm_sched_init(&v3d->queue[V3D_RENDER].sched,
			     &v3d_render_sched_ops,
			     hw_jobs_limit, job_hang_limit,
			     msecs_to_jiffies(hang_limit_ms), NULL,
			     NULL, "v3d_render", v3d->drm.dev);
	if (ret)
		goto fail;

	ret = drm_sched_init(&v3d->queue[V3D_TFU].sched,
			     &v3d_tfu_sched_ops,
			     hw_jobs_limit, job_hang_limit,
			     msecs_to_jiffies(hang_limit_ms), NULL,
			     NULL, "v3d_tfu", v3d->drm.dev);
	if (ret)
		goto fail;

	if (v3d_has_csd(v3d)) {
		ret = drm_sched_init(&v3d->queue[V3D_CSD].sched,
				     &v3d_csd_sched_ops,
				     hw_jobs_limit, job_hang_limit,
				     msecs_to_jiffies(hang_limit_ms), NULL,
				     NULL, "v3d_csd", v3d->drm.dev);
		if (ret)
			goto fail;

		ret = drm_sched_init(&v3d->queue[V3D_CACHE_CLEAN].sched,
				     &v3d_cache_clean_sched_ops,
				     hw_jobs_limit, job_hang_limit,
				     msecs_to_jiffies(hang_limit_ms), NULL,
				     NULL, "v3d_cache_clean", v3d->drm.dev);
		if (ret)
			goto fail;
	}

	return 0;

fail:
	v3d_sched_fini(v3d);
	return ret;
}

void
v3d_sched_fini(struct v3d_dev *v3d)
{
	enum v3d_queue q;
	struct v3d_queue_stats *queue_stats;

	for (q = 0; q < V3D_MAX_QUEUES; q++) {
		if (v3d->queue[q].sched.ready) {
			queue_stats = &v3d->gpu_queue_stats[q];
			mutex_lock(&queue_stats->lock);
			/* Setting gpu_pid_stats_timeout to jiffies-1 will
			 * make v3d_sched_stats_update to purge all
			 * allocated pid_stats.
			 */
			queue_stats->gpu_pid_stats_timeout = jiffies - 1;
			v3d_sched_stats_update(queue_stats);
			mutex_unlock(&queue_stats->lock);
			drm_sched_fini(&v3d->queue[q].sched);
		}
	}
}
