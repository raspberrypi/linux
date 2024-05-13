// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt)	"ioremap: " fmt

#include <linux/maple_tree.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/arm-smccc.h>

#include <asm/hypervisor.h>

#ifndef ARM_SMCCC_KVM_FUNC_MMIO_GUARD_INFO
#define ARM_SMCCC_KVM_FUNC_MMIO_GUARD_INFO	5

#define ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_INFO_FUNC_ID		\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_MMIO_GUARD_INFO)
#endif	/* ARM_SMCCC_KVM_FUNC_MMIO_GUARD_INFO */

#ifndef ARM_SMCCC_KVM_FUNC_MMIO_GUARD_ENROLL
#define ARM_SMCCC_KVM_FUNC_MMIO_GUARD_ENROLL	6

#define ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_ENROLL_FUNC_ID		\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_MMIO_GUARD_ENROLL)
#endif	/* ARM_SMCCC_KVM_FUNC_MMIO_GUARD_ENROLL */

#ifndef ARM_SMCCC_KVM_FUNC_MMIO_GUARD_MAP
#define ARM_SMCCC_KVM_FUNC_MMIO_GUARD_MAP	7

#define ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_MAP_FUNC_ID			\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_MMIO_GUARD_MAP)
#endif	/* ARM_SMCCC_KVM_FUNC_MMIO_GUARD_MAP */

#ifndef ARM_SMCCC_KVM_FUNC_MMIO_GUARD_UNMAP
#define ARM_SMCCC_KVM_FUNC_MMIO_GUARD_UNMAP	8

#define ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_UNMAP_FUNC_ID		\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_MMIO_GUARD_UNMAP)
#endif	/* ARM_SMCCC_KVM_FUNC_MMIO_GUARD_UNMAP */

#ifndef ARM_SMCCC_KVM_FUNC_MMIO_RGUARD_MAP
#define ARM_SMCCC_KVM_FUNC_MMIO_RGUARD_MAP	10

#define ARM_SMCCC_VENDOR_HYP_KVM_MMIO_RGUARD_MAP_FUNC_ID		\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_MMIO_RGUARD_MAP)
#endif	/* ARM_SMCCC_KVM_FUNC_MMIO_GUARD_UNMAP */

#ifndef ARM_SMCCC_KVM_FUNC_MMIO_RGUARD_UNMAP
#define ARM_SMCCC_KVM_FUNC_MMIO_RGUARD_UNMAP	11

#define ARM_SMCCC_VENDOR_HYP_KVM_MMIO_RGUARD_UNMAP_FUNC_ID		\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,				\
			   ARM_SMCCC_SMC_64,				\
			   ARM_SMCCC_OWNER_VENDOR_HYP,			\
			   ARM_SMCCC_KVM_FUNC_MMIO_RGUARD_UNMAP)
#endif	/* ARM_SMCCC_KVM_FUNC_MMIO_GUARD_UNMAP */

static DEFINE_STATIC_KEY_FALSE(ioremap_guard_key);
static DEFINE_MTREE(ioremap_guard_refcount);
static DEFINE_MUTEX(ioremap_guard_lock);

static bool ioremap_guard __ro_after_init;
static size_t guard_granule __ro_after_init;
static bool guard_has_range __ro_after_init;

static int __init ioremap_guard_setup(char *str)
{
	ioremap_guard = true;

	return 0;
}
early_param("ioremap_guard", ioremap_guard_setup);

void kvm_init_ioremap_services(void)
{
	struct arm_smccc_res res;
	size_t granule;

	if (!ioremap_guard)
		return;

	/* We need all the functions to be implemented */
	if (!kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_MMIO_GUARD_INFO) ||
	    !kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_MMIO_GUARD_ENROLL) ||
	    !kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_MMIO_GUARD_MAP) ||
	    !kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_MMIO_GUARD_UNMAP))
		return;

	arm_smccc_1_1_invoke(ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_INFO_FUNC_ID,
			     0, 0, 0, &res);
	granule = res.a0;
	if (!granule || (granule & (granule - 1))) {
		pr_warn("KVM MMIO guard initialization failed: "
			"guard granule (%lu), page size (%lu)\n",
			granule, PAGE_SIZE);
		return;
	}

	guard_has_range = res.a1 & KVM_FUNC_HAS_RANGE;

	arm_smccc_1_1_invoke(ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_ENROLL_FUNC_ID,
			     &res);
	if (res.a0 == SMCCC_RET_SUCCESS) {
		guard_granule = granule;
		static_branch_enable(&ioremap_guard_key);
		pr_info("Using KVM MMIO guard for ioremap\n");
	} else {
		pr_warn("KVM MMIO guard registration failed (%ld)\n", res.a0);
	}
}

static int __invoke_mmioguard(phys_addr_t phys_addr, int nr_granules, bool map,
			      int *done)
{
	u64 nr_granules_arg = guard_has_range ? nr_granules : 0;
	struct arm_smccc_res res;
	u32 func_id;

	if (guard_has_range && map)
		func_id = ARM_SMCCC_VENDOR_HYP_KVM_MMIO_RGUARD_MAP_FUNC_ID;
	else if (guard_has_range && !map)
		func_id = ARM_SMCCC_VENDOR_HYP_KVM_MMIO_RGUARD_UNMAP_FUNC_ID;
	/* Legacy kernels */
	else if (!guard_has_range && map)
		func_id = ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_MAP_FUNC_ID;
	else
		func_id = ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_UNMAP_FUNC_ID;

	arm_smccc_1_1_hvc(func_id, phys_addr, nr_granules_arg, 0, &res);
	if (res.a0 != SMCCC_RET_SUCCESS)
		return -EINVAL;

	*done = guard_has_range ? res.a1 : 1;

	return 0;
}

static int __do_xmap_granules(phys_addr_t phys_addr, int nr_granules, bool map)
{
	int ret, nr_xmapped = 0, __nr_xmapped;

	while (nr_granules) {
		ret = __invoke_mmioguard(phys_addr, nr_granules, map,
					 &__nr_xmapped);
		if (ret)
			break;

		nr_xmapped += __nr_xmapped;

		if (WARN_ON(__nr_xmapped > nr_granules))
			break;

		phys_addr += __nr_xmapped * guard_granule;
		nr_granules -= __nr_xmapped;
	}

	return nr_xmapped;
}

static int ioremap_unregister_phys_range(phys_addr_t phys_addr, size_t size)
{
	int nr_granules, unmapped;

	if (!IS_ALIGNED(phys_addr, guard_granule) ||
	    size % guard_granule)
		return -ERANGE;

	nr_granules = size / guard_granule;

	unmapped = __do_xmap_granules(phys_addr, nr_granules, false);

	return unmapped == nr_granules ? 0 : -EINVAL;
}

static int ioremap_register_phys_range(phys_addr_t phys_addr, size_t size)
{
	int nr_granules, mapped;

	if (!IS_ALIGNED(phys_addr, guard_granule) ||
	    size % guard_granule)
		return -ERANGE;

	nr_granules = size / guard_granule;

	mapped = __do_xmap_granules(phys_addr, nr_granules, true);
	if (mapped != nr_granules) {
		pr_err("Failed to register %llx:%llx\n",
		       phys_addr, phys_addr + size);

		WARN_ON(ioremap_unregister_phys_range(phys_addr,
						      mapped * guard_granule));
		return -EINVAL;
	}

	return 0;
}

static unsigned long mas_end(phys_addr_t phys_addr, size_t size)
{
	return phys_addr + (unsigned long)size - 1;
}

static size_t mas_size(const struct ma_state *mas)
{
	return mas->last - mas->index + 1;
}

static int mas_intersect(struct ma_state *mas, phys_addr_t phys_addr, size_t size)
{
	unsigned long start = max(mas->index, (unsigned long)phys_addr);
	unsigned long end = min(mas->last, mas_end(phys_addr, size));

	/* No intersection */
	if (WARN_ON(mas->last < (unsigned long)phys_addr) ||
	    WARN_ON(mas->index > mas_end(phys_addr, size)))
		return -ERANGE;

	mas_set_range(mas, start, end);

	return 0;
}

static int mas_store_refcount(struct ma_state *mas, int count)
{
	int ret;

	/*
	 * It is acceptable for the allocation to fail, specially
	 * if trying to ioremap something very early on, like with
	 * earlycon, which happens long before kmem_cache_init.
	 * This page will be permanently accessible, similar to a
	 * saturated refcount.
	 */
	if (!slab_is_available())
		return 0;

	ret = mas_store_gfp(mas, xa_mk_value(count), GFP_KERNEL);
	if (ret) {
		pr_err("Failed to set refcount for 0x%lx:0x%lx\n",
		       mas->index, mas->last + 1);
	}

	return ret;
}

void ioremap_phys_range_hook(phys_addr_t phys_addr, size_t size, pgprot_t prot)
{
	MA_STATE(mas, &ioremap_guard_refcount, phys_addr, ULONG_MAX);

	if (!static_branch_unlikely(&ioremap_guard_key))
		return;

	VM_BUG_ON(!PAGE_ALIGNED(phys_addr) || !PAGE_ALIGNED(size));

	mutex_lock(&ioremap_guard_lock);
	mas_lock(&mas);

	while (size) {
		void *entry = mas_find(&mas, mas_end(phys_addr, size));
		size_t sub_size = size;
		int ret;

		if (entry) {
			if (mas.index <= phys_addr) {
				mas_intersect(&mas, phys_addr, size);
				sub_size = mas_size(&mas);
				mas_store_refcount(&mas, xa_to_value(entry) + 1);
				goto next;
			}

			sub_size = mas.last - phys_addr + 1;
		}

		/* Newly guarded region */
		ret = ioremap_register_phys_range(phys_addr, sub_size);
		if (ret)
			break;

		mas_set_range(&mas, phys_addr, mas_end(phys_addr, sub_size));
		mas_store_refcount(&mas, 1);
next:
		size = size_sub(size, sub_size);
		phys_addr += sub_size;
	}

	mas_unlock(&mas);
	mutex_unlock(&ioremap_guard_lock);
}

void iounmap_phys_range_hook(phys_addr_t phys_addr, size_t size)
{
	MA_STATE(mas, &ioremap_guard_refcount, phys_addr, ULONG_MAX);

	if (!static_branch_unlikely(&ioremap_guard_key))
		return;

	VM_BUG_ON(!PAGE_ALIGNED(phys_addr) || !PAGE_ALIGNED(size));

	mutex_lock(&ioremap_guard_lock);
	mas_lock(&mas);

	while (size) {
		void *entry = mas_find(&mas, phys_addr + size - 1);
		unsigned long refcount;
		size_t sub_size = size;

		/*
		 * Untracked region, could happen if registered before
		 * slab_is_available(). Ignore.
		 */
		if (!entry)
			break;

		if (mas.index > phys_addr) {
			sub_size = mas.index - phys_addr;
			goto next;
		}

		refcount = xa_to_value(entry);
		if (WARN_ON(!refcount))
			break;

		mas_intersect(&mas, phys_addr, size);
		sub_size = mas_size(&mas);

		if (refcount == 1) {
			if (WARN_ON(ioremap_unregister_phys_range(phys_addr, sub_size)))
				break;

			/* Split the existing mas if needed before deletion */
			mas_store_refcount(&mas, refcount - 1);
			mas_erase(&mas);
		} else {
			mas_store_refcount(&mas, refcount - 1);
		}
next:
		size = size_sub(size, sub_size);
		phys_addr += sub_size;
	}

	mas_unlock(&mas);
	mutex_unlock(&ioremap_guard_lock);
}

void __iomem *ioremap_prot(phys_addr_t phys_addr, size_t size,
			   unsigned long prot)
{
	unsigned long last_addr = phys_addr + size - 1;

	/* Don't allow outside PHYS_MASK */
	if (last_addr & ~PHYS_MASK)
		return NULL;

	/* Don't allow RAM to be mapped. */
	if (WARN_ON(pfn_is_map_memory(__phys_to_pfn(phys_addr))))
		return NULL;

	return generic_ioremap_prot(phys_addr, size, __pgprot(prot));
}
EXPORT_SYMBOL(ioremap_prot);

/*
 * Must be called after early_fixmap_init
 */
void __init early_ioremap_init(void)
{
	early_ioremap_setup();
}

bool arch_memremap_can_ram_remap(resource_size_t offset, size_t size,
				 unsigned long flags)
{
	unsigned long pfn = PHYS_PFN(offset);

	return pfn_is_map_memory(pfn);
}
