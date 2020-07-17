/*
 * Copyright (C) 2014 BMW Car IT GmbH, Daniel Wagner daniel.wagner@bmw-carit.de
 *
 * Provides a framework for enqueuing callbacks from irq context
 * PREEMPT_RT_FULL safe. The callbacks are executed in kthread context.
 */

#include <linux/swait.h>
#include <linux/swork.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/export.h>

#define SWORK_EVENT_PENDING     1

static DEFINE_MUTEX(worker_mutex);
static struct sworker *glob_worker;

struct sworker {
	struct list_head events;
	struct swait_queue_head wq;

	raw_spinlock_t lock;

	struct task_struct *task;
	int refs;
};

static bool swork_readable(struct sworker *worker)
{
	bool r;

	if (kthread_should_stop())
		return true;

	raw_spin_lock_irq(&worker->lock);
	r = !list_empty(&worker->events);
	raw_spin_unlock_irq(&worker->lock);

	return r;
}

static int swork_kthread(void *arg)
{
	struct sworker *worker = arg;

	for (;;) {
		swait_event_interruptible_exclusive(worker->wq,
						    swork_readable(worker));
		if (kthread_should_stop())
			break;

		raw_spin_lock_irq(&worker->lock);
		while (!list_empty(&worker->events)) {
			struct swork_event *sev;

			sev = list_first_entry(&worker->events,
					struct swork_event, item);
			list_del(&sev->item);
			raw_spin_unlock_irq(&worker->lock);

			WARN_ON_ONCE(!test_and_clear_bit(SWORK_EVENT_PENDING,
							 &sev->flags));
			sev->func(sev);
			raw_spin_lock_irq(&worker->lock);
		}
		raw_spin_unlock_irq(&worker->lock);
	}
	return 0;
}

static struct sworker *swork_create(void)
{
	struct sworker *worker;

	worker = kzalloc(sizeof(*worker), GFP_KERNEL);
	if (!worker)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&worker->events);
	raw_spin_lock_init(&worker->lock);
	init_swait_queue_head(&worker->wq);

	worker->task = kthread_run(swork_kthread, worker, "kswork");
	if (IS_ERR(worker->task)) {
		kfree(worker);
		return ERR_PTR(-ENOMEM);
	}

	return worker;
}

static void swork_destroy(struct sworker *worker)
{
	kthread_stop(worker->task);

	WARN_ON(!list_empty(&worker->events));
	kfree(worker);
}

/**
 * swork_queue - queue swork
 *
 * Returns %false if @work was already on a queue, %true otherwise.
 *
 * The work is queued and processed on a random CPU
 */
bool swork_queue(struct swork_event *sev)
{
	unsigned long flags;

	if (test_and_set_bit(SWORK_EVENT_PENDING, &sev->flags))
		return false;

	raw_spin_lock_irqsave(&glob_worker->lock, flags);
	list_add_tail(&sev->item, &glob_worker->events);
	raw_spin_unlock_irqrestore(&glob_worker->lock, flags);

	swake_up_one(&glob_worker->wq);
	return true;
}
EXPORT_SYMBOL_GPL(swork_queue);

/**
 * swork_get - get an instance of the sworker
 *
 * Returns an negative error code if the initialization if the worker did not
 * work, %0 otherwise.
 *
 */
int swork_get(void)
{
	struct sworker *worker;

	mutex_lock(&worker_mutex);
	if (!glob_worker) {
		worker = swork_create();
		if (IS_ERR(worker)) {
			mutex_unlock(&worker_mutex);
			return -ENOMEM;
		}

		glob_worker = worker;
	}

	glob_worker->refs++;
	mutex_unlock(&worker_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(swork_get);

/**
 * swork_put - puts an instance of the sworker
 *
 * Will destroy the sworker thread. This function must not be called until all
 * queued events have been completed.
 */
void swork_put(void)
{
	mutex_lock(&worker_mutex);

	glob_worker->refs--;
	if (glob_worker->refs > 0)
		goto out;

	swork_destroy(glob_worker);
	glob_worker = NULL;
out:
	mutex_unlock(&worker_mutex);
}
EXPORT_SYMBOL_GPL(swork_put);
