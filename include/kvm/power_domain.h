/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_POWER_DOMAIN_H
#define __KVM_POWER_DOMAIN_H

enum kvm_power_domain_type {
	KVM_POWER_DOMAIN_NONE,
	KVM_POWER_DOMAIN_HOST_HVC,
};

struct kvm_power_domain {
	enum kvm_power_domain_type	type;
	union {
		u64 device_id; /* HOST_HVC device ID*/
	};
};

#endif /* __KVM_POWER_DOMAIN_H */
