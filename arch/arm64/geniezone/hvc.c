// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */
#include <linux/clocksource.h>
#include <linux/kernel.h>
#include <linux/timekeeping.h>
#include <linux/soc/mediatek/gzvm_drv.h>
#include "gzvm_arch_common.h"

#define GZVM_PTP_VIRT_COUNTER 0
#define GZVM_PTP_PHYS_COUNTER 1
/**
 * gzvm_handle_ptp_time() - Sync time between host and guest VM
 * @vcpu: Pointer to struct gzvm_vcpu_run in userspace
 * @counter: Counter type from guest VM
 * Return: Always return 0 because there are no cases of failure
 *
 * The following register values will be passed to the guest VM
 * for time synchronization:
 * regs->x0 (upper 32 bits) wall clock time
 * regs->x1 (lower 32 bits) wall clock time
 * regs->x2 (upper 32 bits) cycles
 * regs->x3 (lower 32 bits) cycles
 */
static int gzvm_handle_ptp_time(struct gzvm_vcpu *vcpu, int counter)
{
	struct system_time_snapshot snapshot;
	u64 cycles = 0;

	ktime_get_snapshot(&snapshot);

	switch (counter) {
	case GZVM_PTP_VIRT_COUNTER:
		cycles = snapshot.cycles -
			 le64_to_cpu(vcpu->hwstate->vtimer_offset);
		break;
	case GZVM_PTP_PHYS_COUNTER:
		cycles = snapshot.cycles;
		break;
	default:
		break;
	}

	vcpu->run->hypercall.args[0] = upper_32_bits(snapshot.real);
	vcpu->run->hypercall.args[1] = lower_32_bits(snapshot.real);
	vcpu->run->hypercall.args[2] = upper_32_bits(cycles);
	vcpu->run->hypercall.args[3] = lower_32_bits(cycles);

	return 0;
}

/**
 * gzvm_arch_handle_guest_hvc() - Handle architecture-related guest hvc
 * @vcpu: Pointer to struct gzvm_vcpu_run in userspace
 * Return:
 * * true - This hvc has been processed, no need to back to VMM.
 * * false - This hvc has not been processed, require userspace.
 */
bool gzvm_arch_handle_guest_hvc(struct gzvm_vcpu *vcpu)
{
	int ret, counter;

	switch (vcpu->run->hypercall.args[0]) {
	case GZVM_HVC_PTP:
		counter = vcpu->run->hypercall.args[1];
		ret = gzvm_handle_ptp_time(vcpu, counter);
		return (ret == 0) ? true : false;
	default:
		break;
	}
	return false;
}
