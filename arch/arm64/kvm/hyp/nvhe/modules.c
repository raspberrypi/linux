/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 Google LLC
 */
#include <asm/kvm_host.h>
#include <asm/kvm_pkvm_module.h>

#include <nvhe/alloc.h>
#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/modules.h>
#include <nvhe/mm.h>
#include <nvhe/serial.h>
#include <nvhe/spinlock.h>
#include <nvhe/trace/trace.h>
#include <nvhe/trap_handler.h>

static void *__pkvm_module_memcpy(void *to, const void *from, size_t count)
{
	return memcpy(to, from, count);
}

static void *__pkvm_module_memset(void *dst, int c, size_t count)
{
	return memset(dst, c, count);
}

static void __kvm_flush_dcache_to_poc(void *addr, size_t size)
{
	kvm_flush_dcache_to_poc((unsigned long)addr, (unsigned long)size);
}

static void __update_hcr_el2(unsigned long set_mask, unsigned long clear_mask)
{
	struct kvm_nvhe_init_params *params = this_cpu_ptr(&kvm_init_params);

	params->hcr_el2 |= set_mask;
	params->hcr_el2 &= ~clear_mask;
	__kvm_flush_dcache_to_poc(params, sizeof(*params));
	write_sysreg(params->hcr_el2, hcr_el2);
}

static void __update_hfgwtr_el2(unsigned long set_mask, unsigned long clear_mask)
{
	struct kvm_nvhe_init_params *params = this_cpu_ptr(&kvm_init_params);

	params->hfgwtr_el2 |= set_mask;
	params->hfgwtr_el2 &= ~clear_mask;
	__kvm_flush_dcache_to_poc(params, sizeof(*params));
	write_sysreg_s(params->hfgwtr_el2, SYS_HFGWTR_EL2);
}

static atomic_t early_lm_pages;
static void *__pkvm_linear_map_early(phys_addr_t phys, size_t size, enum kvm_pgtable_prot prot)
{
	void *addr = NULL;
	int ret;

	if (!PAGE_ALIGNED(phys) || !PAGE_ALIGNED(size))
		return NULL;

	addr = __hyp_va(phys);
	ret = pkvm_create_mappings(addr, addr + size, prot);
	if (ret)
		addr = NULL;
	else
		atomic_add(size, &early_lm_pages);

	return addr;
}

static void __pkvm_linear_unmap_early(void *addr, size_t size)
{
	pkvm_remove_mappings(addr, addr + size);
	atomic_sub(size, &early_lm_pages);
}

void __pkvm_close_module_registration(void)
{
	/*
	 * Page ownership tracking might go out of sync if there are stale
	 * entries in pKVM's linear map range, so they must really be gone by
	 * now.
	 */
	WARN_ON_ONCE(atomic_read(&early_lm_pages));

	/*
	 * Nothing else to do, module loading HVCs are only accessible before
	 * deprivilege
	 */
}

static int _hyp_smp_processor_id(void)
{
	return hyp_smp_processor_id();
}

const struct pkvm_module_ops module_ops = {
	.create_private_mapping = __pkvm_create_private_mapping,
	.alloc_module_va = __pkvm_alloc_module_va,
	.map_module_page = __pkvm_map_module_page,
	.register_serial_driver = __pkvm_register_serial_driver,
	.putc = hyp_putc,
	.puts = hyp_puts,
	.putx64 = hyp_putx64,
	.fixmap_map = hyp_fixmap_map,
	.fixmap_unmap = hyp_fixmap_unmap,
	.linear_map_early = __pkvm_linear_map_early,
	.linear_unmap_early = __pkvm_linear_unmap_early,
	.flush_dcache_to_poc = __kvm_flush_dcache_to_poc,
	.update_hcr_el2 = __update_hcr_el2,
	.update_hfgwtr_el2 = __update_hfgwtr_el2,
	.register_host_perm_fault_handler = hyp_register_host_perm_fault_handler,
	.host_stage2_mod_prot = module_change_host_page_prot,
	.host_stage2_get_leaf = host_stage2_get_leaf,
	.register_host_smc_handler = __pkvm_register_host_smc_handler,
	.register_default_trap_handler = __pkvm_register_default_trap_handler,
	.register_illegal_abt_notifier = __pkvm_register_illegal_abt_notifier,
	.register_psci_notifier = __pkvm_register_psci_notifier,
	.register_hyp_panic_notifier = __pkvm_register_hyp_panic_notifier,
	.register_unmask_serror = __pkvm_register_unmask_serror,
	.host_donate_hyp = ___pkvm_host_donate_hyp,
	.host_donate_hyp_prot = ___pkvm_host_donate_hyp_prot,
	.hyp_donate_host = __pkvm_hyp_donate_host,
	.host_share_hyp = __pkvm_host_share_hyp,
	.host_unshare_hyp = __pkvm_host_unshare_hyp,
	.pin_shared_mem = hyp_pin_shared_mem,
	.unpin_shared_mem = hyp_unpin_shared_mem,
	.memcpy = __pkvm_module_memcpy,
	.memset = __pkvm_module_memset,
	.hyp_pa = hyp_virt_to_phys,
	.hyp_va = hyp_phys_to_virt,
	.kern_hyp_va = __kern_hyp_va,
	.hyp_alloc = hyp_alloc,
	.hyp_alloc_errno = hyp_alloc_errno,
	.hyp_free = hyp_free,
	.iommu_donate_pages = kvm_iommu_donate_pages,
	.iommu_reclaim_pages = kvm_iommu_reclaim_pages,
	.iommu_request = kvm_iommu_request,
	.iommu_init_device = kvm_iommu_init_device,
	.udelay = pkvm_udelay,
	.hyp_alloc_missing_donations = hyp_alloc_missing_donations,
#ifdef CONFIG_LIST_HARDENED
	.list_add_valid_or_report = __list_add_valid_or_report,
	.list_del_entry_valid_or_report = __list_del_entry_valid_or_report,
#endif
	.iommu_iotlb_gather_add_page = kvm_iommu_iotlb_gather_add_page,
	.register_hyp_event_ids = register_hyp_event_ids,
	.tracing_reserve_entry = tracing_reserve_entry,
	.tracing_commit_entry = tracing_commit_entry,
	.iommu_donate_pages_atomic = kvm_iommu_donate_pages_atomic,
	.iommu_reclaim_pages_atomic = kvm_iommu_reclaim_pages_atomic,
	.iommu_snapshot_host_stage2 = kvm_iommu_snapshot_host_stage2,
	.hyp_smp_processor_id = _hyp_smp_processor_id,
};

int __pkvm_init_module(void *module_init)
{
	int (*do_module_init)(const struct pkvm_module_ops *ops) = module_init;

	return do_module_init(&module_ops);
}

#define MAX_DYNAMIC_HCALLS 128

atomic_t num_dynamic_hcalls = ATOMIC_INIT(0);
DEFINE_HYP_SPINLOCK(dyn_hcall_lock);

static dyn_hcall_t host_dynamic_hcalls[MAX_DYNAMIC_HCALLS];

int handle_host_dynamic_hcall(struct user_pt_regs *regs, int id)
{
	dyn_hcall_t hfn;
	int dyn_id;

	/*
	 * TODO: static key to protect when no dynamic hcall is registered?
	 */

	dyn_id = id - __KVM_HOST_SMCCC_FUNC___dynamic_hcalls;
	if (dyn_id < 0)
		return HCALL_UNHANDLED;

	/*
	 * Order access to num_dynamic_hcalls and host_dynamic_hcalls. Paired
	 * with __pkvm_register_hcall().
	 */
	if (dyn_id >= atomic_read_acquire(&num_dynamic_hcalls))
		return HCALL_UNHANDLED;

	hfn = READ_ONCE(host_dynamic_hcalls[dyn_id]);
	if (!hfn)
		return HCALL_UNHANDLED;

	hfn(regs);

	return HCALL_HANDLED;
}

int __pkvm_register_hcall(unsigned long hvn_hyp_va)
{
	dyn_hcall_t hfn = (void *)hvn_hyp_va;
	int reserved_id, ret;

	assert_in_mod_range(hvn_hyp_va);

	hyp_spin_lock(&dyn_hcall_lock);

	reserved_id = atomic_read(&num_dynamic_hcalls);

	if (reserved_id >= MAX_DYNAMIC_HCALLS) {
		ret = -ENOMEM;
		goto err_hcall_unlock;
	}

	WRITE_ONCE(host_dynamic_hcalls[reserved_id], hfn);

	/*
	 * Order access to num_dynamic_hcalls and host_dynamic_hcalls. Paired
	 * with handle_host_dynamic_hcall.
	 */
	atomic_set_release(&num_dynamic_hcalls, reserved_id + 1);

	ret = reserved_id + __KVM_HOST_SMCCC_FUNC___dynamic_hcalls;
err_hcall_unlock:
	hyp_spin_unlock(&dyn_hcall_lock);

	return ret;
};
