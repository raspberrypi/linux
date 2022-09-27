/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 Google LLC
 */
#include <asm/kvm_host.h>
#include <asm/kvm_pkvm_module.h>

#include <nvhe/modules.h>
#include <nvhe/mm.h>
#include <nvhe/serial.h>
#include <nvhe/spinlock.h>
#include <nvhe/trap_handler.h>

const struct pkvm_module_ops module_ops = {
	.create_private_mapping = __pkvm_create_private_mapping,
	.register_serial_driver = __pkvm_register_serial_driver,
	.putc = hyp_putc,
	.puts = hyp_puts,
	.putx64 = hyp_putx64,
};

int __pkvm_init_module(void *module_init)
{
	int (*do_module_init)(const struct pkvm_module_ops *ops) = module_init;
	int ret;

	ret = do_module_init(&module_ops);
	return ret;
}

#define MAX_DYNAMIC_HCALLS 128

atomic_t num_dynamic_hcalls = ATOMIC_INIT(0);
DEFINE_HYP_SPINLOCK(dyn_hcall_lock);

static dyn_hcall_t host_dynamic_hcalls[MAX_DYNAMIC_HCALLS];

int handle_host_dynamic_hcall(struct kvm_cpu_context *host_ctxt)
{
	DECLARE_REG(unsigned long, id, host_ctxt, 0);
	dyn_hcall_t hfn;
	int dyn_id;

	/*
	 * TODO: static key to protect when no dynamic hcall is registered?
	 */

	dyn_id = (int)(id - KVM_HOST_SMCCC_ID(0)) -
		 __KVM_HOST_SMCCC_FUNC___dynamic_hcalls;
	if (dyn_id < 0)
		return HCALL_UNHANDLED;

	cpu_reg(host_ctxt, 0) = SMCCC_RET_NOT_SUPPORTED;

	/*
	 * Order access to num_dynamic_hcalls and host_dynamic_hcalls. Paired
	 * with __pkvm_register_hcall().
	 */
	if (dyn_id >= atomic_read_acquire(&num_dynamic_hcalls))
		goto end;

	hfn = READ_ONCE(host_dynamic_hcalls[dyn_id]);
	if (!hfn)
		goto end;

	cpu_reg(host_ctxt, 0) = SMCCC_RET_SUCCESS;
	hfn(&host_ctxt->regs);
end:
	return HCALL_HANDLED;
}

int __pkvm_register_hcall(unsigned long hvn_hyp_va)
{
	dyn_hcall_t hfn = (void *)hvn_hyp_va;
	int reserved_id;

	hyp_spin_lock(&dyn_hcall_lock);

	reserved_id = atomic_read(&num_dynamic_hcalls);

	if (reserved_id >= MAX_DYNAMIC_HCALLS) {
		hyp_spin_unlock(&dyn_hcall_lock);
		return -ENOMEM;
	}

	WRITE_ONCE(host_dynamic_hcalls[reserved_id], hfn);

	/*
	 * Order access to num_dynamic_hcalls and host_dynamic_hcalls. Paired
	 * with handle_host_dynamic_hcall.
	 */
	atomic_set_release(&num_dynamic_hcalls, reserved_id + 1);

	hyp_spin_unlock(&dyn_hcall_lock);

	return reserved_id + __KVM_HOST_SMCCC_FUNC___dynamic_hcalls;
};
