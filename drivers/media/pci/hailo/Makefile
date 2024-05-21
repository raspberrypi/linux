# SPDX-License-Identifier: GPL-2.0

COMMON_SRC_DIRECTORY=common
VDMA_SRC_DIRECTORY=vdma
UTILS_SRC_DIRECTORY=utils

obj-$(CONFIG_MEDIA_PCI_HAILO) := hailo_pci.o

hailo_pci-objs += src/pcie.o
hailo_pci-objs += src/fops.o
hailo_pci-objs += src/utils.o
hailo_pci-objs += src/sysfs.o

hailo_pci-objs += $(COMMON_SRC_DIRECTORY)/fw_validation.o
hailo_pci-objs += $(COMMON_SRC_DIRECTORY)/fw_operation.o
hailo_pci-objs += $(COMMON_SRC_DIRECTORY)/pcie_common.o
hailo_pci-objs += $(COMMON_SRC_DIRECTORY)/vdma_common.o
hailo_pci-objs += $(COMMON_SRC_DIRECTORY)/hailo_resource.o

hailo_pci-objs += $(UTILS_SRC_DIRECTORY)/logs.o

hailo_pci-objs += $(VDMA_SRC_DIRECTORY)/vdma.o
hailo_pci-objs += $(VDMA_SRC_DIRECTORY)/memory.o
hailo_pci-objs += $(VDMA_SRC_DIRECTORY)/ioctl.o

ccflags-y      += -Werror
ccflags-y      += -DHAILO_RASBERRY_PIE
ccflags-y      += -I$(srctree)/$(src)
ccflags-y      += -I$(srctree)/$(src)/include
ccflags-y      += -I$(srctree)/$(src)/common

clean-files := $(hailo_pci-objs)
