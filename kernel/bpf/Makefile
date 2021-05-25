# SPDX-License-Identifier: GPL-2.0
obj-y := core.o
ifneq ($(CONFIG_BPF_JIT_ALWAYS_ON),y)
# ___bpf_prog_run() needs GCSE disabled on x86; see 3193c0836f203 for details
cflags-nogcse-$(CONFIG_X86)$(CONFIG_CC_IS_GCC) := -fno-gcse
endif
CFLAGS_core.o += $(call cc-disable-warning, override-init) $(cflags-nogcse-yy)

obj-$(CONFIG_BPF_SYSCALL) += syscall.o verifier.o inode.o helpers.o tnum.o
obj-$(CONFIG_BPF_SYSCALL) += hashtab.o arraymap.o percpu_freelist.o bpf_lru_list.o lpm_trie.o map_in_map.o
obj-$(CONFIG_BPF_SYSCALL) += local_storage.o queue_stack_maps.o
obj-$(CONFIG_BPF_SYSCALL) += disasm.o
obj-$(CONFIG_BPF_SYSCALL) += btf.o
ifeq ($(CONFIG_NET),y)
obj-$(CONFIG_BPF_SYSCALL) += devmap.o
obj-$(CONFIG_BPF_SYSCALL) += cpumap.o
ifeq ($(CONFIG_XDP_SOCKETS),y)
obj-$(CONFIG_BPF_SYSCALL) += xskmap.o
endif
obj-$(CONFIG_BPF_SYSCALL) += offload.o
endif
ifeq ($(CONFIG_PERF_EVENTS),y)
obj-$(CONFIG_BPF_SYSCALL) += stackmap.o
endif
obj-$(CONFIG_CGROUP_BPF) += cgroup.o
ifeq ($(CONFIG_INET),y)
obj-$(CONFIG_BPF_SYSCALL) += reuseport_array.o
endif
ifeq ($(CONFIG_SYSFS),y)
obj-$(CONFIG_DEBUG_INFO_BTF) += sysfs_btf.o
endif
