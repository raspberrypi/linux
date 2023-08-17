# SPDX-License-Identifier: GPL-2.0

lib-objs := clear_page.o copy_page.o memcpy.o memset.o
lib-objs := $(addprefix ../../../lib/, $(lib-objs))

hyp-obj-y := timer-sr.o sysreg-sr.o debug-sr.o switch.o tlb.o hyp-init.o host.o \
	 hyp-main.o hyp-smp.o psci-relay.o alloc.o early_alloc.o page_alloc.o \
	 cache.o setup.o mm.o mem_protect.o sys_regs.o pkvm.o stacktrace.o ffa.o \
	 serial.o alloc_mgt.o
hyp-obj-y += ../vgic-v3-sr.o ../aarch32.o ../vgic-v2-cpuif-proxy.o ../entry.o \
	 ../fpsimd.o ../hyp-entry.o ../exception.o ../pgtable.o  iommu/iommu.o
hyp-obj-$(CONFIG_LIST_HARDENED) += list_debug.o
hyp-obj-$(CONFIG_TRACING) += clock.o events.o trace.o
hyp-obj-$(CONFIG_MODULES) += modules.o
hyp-obj-y += $(lib-objs)

#Only for built-in, not modules
ifeq ($(CONFIG_ARM_SMMU_V3_PKVM),y)
# There is no way to compile this separately as calling Makefile.nvhe again would
# create duplicate sections
hyp-obj-$(CONFIG_ARM_SMMU_V3_PKVM) += ../../../../../drivers/iommu/arm/arm-smmu-v3/pkvm/arm-smmu-v3.o
hyp-obj-$(CONFIG_ARM_SMMU_V3_PKVM) += ../../../../../drivers/iommu/arm/arm-smmu-v3/pkvm/io-pgtable-arm.o
hyp-obj-$(CONFIG_ARM_SMMU_V3_PKVM) += ../../../../../drivers/iommu/io-pgtable-arm-common.o
endif


hyp-obj-y += power/hvc.o

$(obj)/hyp.lds: $(src)/hyp.lds.S FORCE
	$(call if_changed_dep,cpp_lds_S)

include $(srctree)/arch/arm64/kvm/hyp/nvhe/Makefile.nvhe
obj-y := kvm_nvhe.o
