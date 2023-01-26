// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 */

#include <nvhe/mm.h>
#include <nvhe/trace.h>

#include <nvhe/define_events.h>

extern struct hyp_event_id __hyp_event_ids_start[];
extern struct hyp_event_id __hyp_event_ids_end[];

int __pkvm_enable_event(unsigned short id, bool enable)
{
	struct hyp_event_id *event_id = __hyp_event_ids_start;
	atomic_t *enable_key;

	for (; (unsigned long)event_id < (unsigned long)__hyp_event_ids_end;
	     event_id++) {
		if (event_id->id != id)
			continue;

		enable_key = (atomic_t *)event_id->data;
		enable_key = hyp_fixmap_map(__hyp_pa(enable_key));

		atomic_set(enable_key, enable);

		hyp_fixmap_unmap();

		return 0;
	}

	return -EINVAL;
}
