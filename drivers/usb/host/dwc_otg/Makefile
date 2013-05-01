#
# Makefile for DWC_otg Highspeed USB controller driver
#

ifneq ($(KERNELRELEASE),)

# Use the BUS_INTERFACE variable to compile the software for either
# PCI(PCI_INTERFACE) or LM(LM_INTERFACE) bus.
ifeq ($(BUS_INTERFACE),)
#	BUS_INTERFACE = -DPCI_INTERFACE
#	BUS_INTERFACE = -DLM_INTERFACE
        BUS_INTERFACE = -DPLATFORM_INTERFACE
endif

#ccflags-y	+= -DDEBUG
#ccflags-y	+= -DDWC_OTG_DEBUGLEV=1 # reduce common debug msgs

# Use one of the following flags to compile the software in host-only or
# device-only mode.
#ccflags-y        += -DDWC_HOST_ONLY
#ccflags-y        += -DDWC_DEVICE_ONLY

ccflags-y	+= -Dlinux -DDWC_HS_ELECT_TST
#ccflags-y	+= -DDWC_EN_ISOC
ccflags-y   	+= -I$(srctree)/drivers/usb/host/dwc_common_port
#ccflags-y   	+= -I$(PORTLIB)
ccflags-y   	+= -DDWC_LINUX
ccflags-y   	+= $(CFI)
ccflags-y	+= $(BUS_INTERFACE)
#ccflags-y	+= -DDWC_DEV_SRPCAP

obj-$(CONFIG_USB_DWCOTG) += dwc_otg.o

dwc_otg-objs	:= dwc_otg_driver.o dwc_otg_attr.o
dwc_otg-objs	+= dwc_otg_cil.o dwc_otg_cil_intr.o
dwc_otg-objs	+= dwc_otg_pcd_linux.o dwc_otg_pcd.o dwc_otg_pcd_intr.o
dwc_otg-objs	+= dwc_otg_hcd.o dwc_otg_hcd_linux.o dwc_otg_hcd_intr.o dwc_otg_hcd_queue.o dwc_otg_hcd_ddma.o
dwc_otg-objs	+= dwc_otg_adp.o
dwc_otg-objs	+= dwc_otg_fiq_fsm.o
ifneq ($(CONFIG_ARM64),y)
dwc_otg-objs	+= dwc_otg_fiq_stub.o
endif

ifneq ($(CFI),)
dwc_otg-objs	+= dwc_otg_cfi.o
endif

kernrelwd := $(subst ., ,$(KERNELRELEASE))
kernrel3 := $(word 1,$(kernrelwd)).$(word 2,$(kernrelwd)).$(word 3,$(kernrelwd))

ifneq ($(kernrel3),2.6.20)
ccflags-y += $(CPPFLAGS)
endif

else

PWD		:= $(shell pwd)
PORTLIB		:= $(PWD)/../dwc_common_port

# Command paths
CTAGS		:= $(CTAGS)
DOXYGEN		:= $(DOXYGEN)

default: portlib
	$(MAKE) -C$(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

install: default
	$(MAKE) -C$(KDIR) M=$(PORTLIB) modules_install
	$(MAKE) -C$(KDIR) M=$(PWD) modules_install

portlib:
	$(MAKE) -C$(KDIR) M=$(PORTLIB) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	cp $(PORTLIB)/Module.symvers $(PWD)/

docs:	$(wildcard *.[hc]) doc/doxygen.cfg
	$(DOXYGEN) doc/doxygen.cfg

tags:	$(wildcard *.[hc])
	$(CTAGS) -e $(wildcard *.[hc]) $(wildcard linux/*.[hc]) $(wildcard $(KDIR)/include/linux/usb*.h)


clean:
	rm -rf   *.o *.ko .*cmd *.mod.c .tmp_versions Module.symvers

endif
