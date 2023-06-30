# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -I$(src)			# needed for trace events

obj-$(CONFIG_ANDROID_BINDER_IPC)	+= binder_pick_impl.o
obj-$(CONFIG_ANDROID_BINDERFS_C)	+= binderfs.o
obj-$(CONFIG_ANDROID_BINDER_IPC_C)	+= binder.o binder_alloc.o
obj-$(CONFIG_ANDROID_BINDER_IPC_SELFTEST) += binder_alloc_selftest.o
obj-$(CONFIG_ANDROID_BINDERFS_RUST)	+= binder/rust_binderfs.o
obj-$(CONFIG_ANDROID_BINDER_IPC_RUST)	+= binder/rust_binder.o
obj-$(CONFIG_ANDROID_DEBUG_SYMBOLS)	+= android_debug_symbols.o
obj-$(CONFIG_ANDROID_VENDOR_HOOKS) += vendor_hooks.o
obj-$(CONFIG_ANDROID_DEBUG_KINFO)	+= debug_kinfo.o
