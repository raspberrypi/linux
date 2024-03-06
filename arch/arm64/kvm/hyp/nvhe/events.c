// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 */

#include <nvhe/mm.h>
#include <nvhe/trace/trace.h>

#include <nvhe/trace/define_events.h>

extern struct hyp_event_id __hyp_event_ids_start[];
extern struct hyp_event_id __hyp_event_ids_end[];

#define MAX_EVENT_ID_MOD 128

static atomic_t num_event_id_mod = ATOMIC_INIT(0);
static DEFINE_HYP_SPINLOCK(event_id_mod_lock);
static struct {
	struct hyp_event_id	*start;
	struct hyp_event_id	*end;
} event_id_mod[MAX_EVENT_ID_MOD];

static void hyp_set_key(atomic_t *key, int val)
{
	atomic_t *__key = hyp_fixmap_map(__pkvm_private_range_pa(key));

	atomic_set(__key, val);
	hyp_fixmap_unmap();
}

static bool __try_set_event(unsigned short id, bool enable,
			    struct hyp_event_id *event_id,
			    struct hyp_event_id *end)
{
	atomic_t *enable_key;

	for (; event_id < end; event_id++) {
		if (event_id->id != id)
			continue;

		enable_key = (atomic_t *)event_id->data;
		hyp_set_key(enable_key, enable);

		return true;
	}

	return false;
}

static bool try_set_event(unsigned short id, bool enable)
{
	return __try_set_event(id, enable, __hyp_event_ids_start,
			       __hyp_event_ids_end);
}

static bool try_set_mod_event(unsigned short id, bool enable)
{
	int i, nr_mod;

	/*
	 * Order access between num_event_id_mod and event_id_mod.
	 * Paired with register_hyp_event_ids()
	 */
	nr_mod = atomic_read_acquire(&num_event_id_mod);

	for (i = 0; i < nr_mod; i++) {
		if (__try_set_event(id, enable, event_id_mod[i].start,
				    event_id_mod[i].end))
			return true;
	}

	return false;
}

int register_hyp_event_ids(unsigned long start, unsigned long end)
{
	int mod, ret = -ENOMEM;

	hyp_spin_lock(&event_id_mod_lock);

	mod = atomic_read(&num_event_id_mod);
	if (mod < MAX_EVENT_ID_MOD) {
		event_id_mod[mod].start = (struct hyp_event_id *)start;
		event_id_mod[mod].end = (struct hyp_event_id *)end;
		/*
		 * Order access between num_event_id_mod and event_id_mod.
		 * Paired with try_set_mod_event()
		 */
		atomic_set_release(&num_event_id_mod, mod + 1);
		ret = 0;
	}

	hyp_spin_unlock(&event_id_mod_lock);

	return ret;
}

int __pkvm_enable_event(unsigned short id, bool enable)
{
	if (try_set_event(id, enable))
		return 0;

	if (try_set_mod_event(id, enable))
		return 0;

	return -EINVAL;
}
