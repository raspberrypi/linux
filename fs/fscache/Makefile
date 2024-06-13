# SPDX-License-Identifier: GPL-2.0
#
# Makefile for general filesystem caching code
#

ccflags-y += -DDEFAULT_SYMBOL_NAMESPACE=ANDROID_GKI_VFS_EXPORT_ONLY

fscache-y := \
	cache.o \
	cookie.o \
	io.o \
	main.o \
	volume.o

fscache-$(CONFIG_PROC_FS) += proc.o
fscache-$(CONFIG_FSCACHE_STATS) += stats.o

obj-$(CONFIG_FSCACHE) := fscache.o
