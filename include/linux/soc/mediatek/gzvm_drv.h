/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#ifndef __GZVM_DRV_H__
#define __GZVM_DRV_H__

#include <linux/eventfd.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/gzvm.h>
#include <linux/srcu.h>
#include <linux/rbtree.h>

/*
 * For the normal physical address, the highest 12 bits should be zero, so we
 * can mask bit 62 ~ bit 52 to indicate the error physical address
 */
#define GZVM_PA_ERR_BAD (0x7ffULL << 52)

#define GZVM_VCPU_MMAP_SIZE  PAGE_SIZE
#define INVALID_VM_ID   0xffff

/*
 * These are the definitions of APIs between GenieZone hypervisor and driver,
 * there's no need to be visible to uapi. Furthermore, we need GenieZone
 * specific error code in order to map to Linux errno
 */
#define NO_ERROR                (0)
#define ERR_NO_MEMORY           (-5)
#define ERR_INVALID_ARGS        (-8)
#define ERR_NOT_SUPPORTED       (-24)
#define ERR_NOT_IMPLEMENTED     (-27)
#define ERR_FAULT               (-40)
#define GZVM_IRQFD_RESAMPLE_IRQ_SOURCE_ID       1

/*
 * The following data structures are for data transferring between driver and
 * hypervisor, and they're aligned with hypervisor definitions
 */
#define GZVM_MAX_VCPUS		 8
#define GZVM_MAX_MEM_REGION	10

#define GZVM_VCPU_RUN_MAP_SIZE		(PAGE_SIZE * 2)

#define GZVM_BLOCK_BASED_DEMAND_PAGE_SIZE	(2 * 1024 * 1024) /* 2MB */

#define GZVM_MAX_DEBUGFS_DIR_NAME_SIZE  20
#define GZVM_MAX_DEBUGFS_VALUE_SIZE	20

enum gzvm_demand_paging_mode {
	GZVM_FULLY_POPULATED = 0,
	GZVM_DEMAND_PAGING = 1,
};

/**
 * struct mem_region_addr_range: identical to ffa memory constituent
 * @address: the base IPA of the constituent memory region, aligned to 4 kiB
 * @pg_cnt: the number of 4 kiB pages in the constituent memory region
 * @reserved: reserved for 64bit alignment
 */
struct mem_region_addr_range {
	__u64 address;
	__u32 pg_cnt;
	__u32 reserved;
};

struct gzvm_memory_region_ranges {
	__u32 slot;
	__u32 constituent_cnt;
	__u64 total_pages;
	__u64 gpa;
	struct mem_region_addr_range constituents[];
};

/**
 * struct gzvm_memslot: VM's memory slot descriptor
 * @base_gfn: begin of guest page frame
 * @npages: number of pages this slot covers
 * @userspace_addr: corresponding userspace va
 * @vma: vma related to this userspace addr
 * @flags: define the usage of memory region. Ex. guest memory or
 * firmware protection
 * @slot_id: the id is used to identify the memory slot
 */
struct gzvm_memslot {
	u64 base_gfn;
	unsigned long npages;
	unsigned long userspace_addr;
	struct vm_area_struct *vma;
	u32 flags;
	u32 slot_id;
};

struct gzvm_vcpu {
	struct gzvm *gzvm;
	int vcpuid;
	/* lock of vcpu*/
	struct mutex lock;
	struct gzvm_vcpu_run *run;
	struct gzvm_vcpu_hwstate *hwstate;
};

struct gzvm_pinned_page {
	struct rb_node node;
	struct page *page;
	u64 ipa;
};

struct gzvm_vm_stat {
	u64 protected_hyp_mem;
	u64 protected_shared_mem;
};

/**
 * struct gzvm: the following data structures are for data transferring between
 * driver and hypervisor, and they're aligned with hypervisor definitions.
 * @vcpus: VM's cpu descriptors
 * @mm: userspace tied to this vm
 * @memslot: VM's memory slot descriptor
 * @lock: lock for list_add
 * @irqfds: the data structure is used to keep irqfds's information
 * @ioevents: list head for ioevents
 * @vm_list: list head for vm list
 * @vm_id: vm id
 * @irq_ack_notifier_list: list head for irq ack notifier
 * @irq_srcu: structure data for SRCU(sleepable rcu)
 * @irq_lock: lock for irq injection
 * @mem_alloc_mode: memory allocation mode - fully allocated or demand paging
 * @demand_page_gran: demand page granularity: how much memory we allocate for
 * VM in a single page fault
 * @demand_page_buffer: the mailbox for transferring large portion pages
 * @demand_paging_lock: lock for preventing multiple cpu using the same demand
 * page mailbox at the same time
 * @pinned_pages: use rb-tree to record pin/unpin page
 * @mem_lock: lock for memory operations
 * @stat: information for VM memory statistics
 * @debug_dir: debugfs directory node for VM memory statistics
 */
struct gzvm {
	struct gzvm_vcpu *vcpus[GZVM_MAX_VCPUS];
	struct mm_struct *mm;
	struct gzvm_memslot memslot[GZVM_MAX_MEM_REGION];
	struct mutex lock;

	struct {
		spinlock_t        lock;
		struct list_head  items;
		struct list_head  resampler_list;
		struct mutex      resampler_lock;
	} irqfds;

	struct list_head ioevents;

	struct list_head vm_list;
	u16 vm_id;

	struct hlist_head irq_ack_notifier_list;
	struct srcu_struct irq_srcu;
	struct mutex irq_lock;
	u32 mem_alloc_mode;

	u32 demand_page_gran;
	u64 *demand_page_buffer;
	struct mutex  demand_paging_lock;

	struct rb_root pinned_pages;
	struct mutex mem_lock;

	struct gzvm_vm_stat stat;
	struct dentry *debug_dir;
};

long gzvm_dev_ioctl_check_extension(struct gzvm *gzvm, unsigned long args);
int gzvm_dev_ioctl_create_vm(unsigned long vm_type);

int gzvm_err_to_errno(unsigned long err);

void gzvm_destroy_all_vms(void);

void gzvm_destroy_vcpus(struct gzvm *gzvm);

/* arch-dependant functions */
int gzvm_arch_probe(void);
int gzvm_arch_set_memregion(u16 vm_id, size_t buf_size,
			    phys_addr_t region);
int gzvm_arch_check_extension(struct gzvm *gzvm, __u64 cap, void __user *argp);
int gzvm_arch_create_vm(unsigned long vm_type);
int gzvm_arch_destroy_vm(u16 vm_id);
int gzvm_arch_map_guest(u16 vm_id, int memslot_id, u64 pfn, u64 gfn,
			u64 nr_pages);
int gzvm_arch_map_guest_block(u16 vm_id, int memslot_id, u64 gfn, u64 nr_pages);
int gzvm_arch_get_statistics(struct gzvm *gzvm);
int gzvm_vm_ioctl_arch_enable_cap(struct gzvm *gzvm,
				  struct gzvm_enable_cap *cap,
				  void __user *argp);

u64 gzvm_hva_to_pa_arch(u64 hva);
u64 hva_to_pa_fast(u64 hva);
u64 hva_to_pa_slow(u64 hva);
int gzvm_gfn_to_pfn_memslot(struct gzvm_memslot *memslot, u64 gfn, u64 *pfn);
int gzvm_gfn_to_hva_memslot(struct gzvm_memslot *memslot, u64 gfn,
			    u64 *hva_memslot);
int gzvm_vm_populate_mem_region(struct gzvm *gzvm, int slot_id);
int gzvm_vm_allocate_guest_page(struct gzvm *gzvm, struct gzvm_memslot *slot,
				u64 gfn, u64 *pfn);

int gzvm_vm_ioctl_create_vcpu(struct gzvm *gzvm, u32 cpuid);
int gzvm_arch_vcpu_update_one_reg(struct gzvm_vcpu *vcpu, __u64 reg_id,
				  bool is_write, __u64 *data);
int gzvm_arch_create_vcpu(u16 vm_id, int vcpuid, void *run);
int gzvm_arch_vcpu_run(struct gzvm_vcpu *vcpu, __u64 *exit_reason);
int gzvm_arch_destroy_vcpu(u16 vm_id, int vcpuid);
int gzvm_arch_inform_exit(u16 vm_id);

int gzvm_drv_debug_init(void);
void gzvm_drv_debug_exit(void);

int gzvm_find_memslot(struct gzvm *vm, u64 gpa);
int gzvm_handle_page_fault(struct gzvm_vcpu *vcpu);
bool gzvm_handle_guest_exception(struct gzvm_vcpu *vcpu);
int gzvm_handle_relinquish(struct gzvm_vcpu *vcpu, phys_addr_t ipa);
bool gzvm_handle_guest_hvc(struct gzvm_vcpu *vcpu);
bool gzvm_arch_handle_guest_hvc(struct gzvm_vcpu *vcpu);

int gzvm_arch_create_device(u16 vm_id, struct gzvm_create_device *gzvm_dev);
int gzvm_arch_inject_irq(struct gzvm *gzvm, unsigned int vcpu_idx,
			 u32 irq, bool level);

void gzvm_notify_acked_irq(struct gzvm *gzvm, unsigned int gsi);
int gzvm_irqfd(struct gzvm *gzvm, struct gzvm_irqfd *args);
int gzvm_drv_irqfd_init(void);
void gzvm_drv_irqfd_exit(void);
int gzvm_vm_irqfd_init(struct gzvm *gzvm);
void gzvm_vm_irqfd_release(struct gzvm *gzvm);

int gzvm_arch_memregion_purpose(struct gzvm *gzvm,
				struct gzvm_userspace_memory_region *mem);
int gzvm_arch_set_dtb_config(struct gzvm *gzvm, struct gzvm_dtb_config *args);

int gzvm_init_ioeventfd(struct gzvm *gzvm);
int gzvm_ioeventfd(struct gzvm *gzvm, struct gzvm_ioeventfd *args);
bool gzvm_ioevent_write(struct gzvm_vcpu *vcpu, __u64 addr, int len,
			const void *val);
void eventfd_ctx_do_read(struct eventfd_ctx *ctx, __u64 *cnt);
struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr);
void add_wait_queue_priority(struct wait_queue_head *wq_head,
			     struct wait_queue_entry *wq_entry);

#endif /* __GZVM_DRV_H__ */
