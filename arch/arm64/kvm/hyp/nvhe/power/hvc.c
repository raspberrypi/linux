// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Google LLC
 * Author: Mostafa Saleh <smostafa@google.com>
 */

#include <nvhe/pkvm.h>

struct hvc_power_domain {
	struct kvm_power_domain			*pd;
	const struct kvm_power_domain_ops	*ops;
};

struct hvc_power_domain handlers[MAX_POWER_DOMAINS];

int pkvm_init_hvc_pd(struct kvm_power_domain *pd,
		     const struct kvm_power_domain_ops *ops)
{
	if (pd->device_id >= MAX_POWER_DOMAINS)
		return -E2BIG;

	handlers[pd->device_id].ops = ops;
	handlers[pd->device_id].pd = pd;

	return 0;
}

int pkvm_host_hvc_pd(u64 device_id, u64 on)
{
	struct hvc_power_domain *pd;

	if (device_id >= MAX_POWER_DOMAINS)
		return -E2BIG;

	device_id = array_index_nospec(device_id, MAX_POWER_DOMAINS);
	pd = &handlers[device_id];

	if (!pd->ops)
		return -ENOENT;

	if (on)
		pd->ops->power_on(pd->pd);
	else
		pd->ops->power_off(pd->pd);

	return 0;
}
