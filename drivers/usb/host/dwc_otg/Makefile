#
# Makefile for DWC_otg Highspeed USB controller driver
#

ifneq ($(KERNELRELEASE),)

ifeq ($(BUS_INTERFACE),)
	# BUS_INTERFACE = -DLM_INTERFACE
	BUS_INTERFACE = -DPLATFORM_INTERFACE=1
endif

CPPFLAGS	+= -DDEBUG 

# Use one of the following flags to compile the software in host-only or
# device-only mode.
#CPPFLAGS        += -DDWC_HOST_ONLY
#CPPFLAGS        += -DDWC_DEVICE_ONLY

CPPFLAGS	+= -Dlinux -DDWC_HS_ELECT_TST
#CGG: CPPFLAGS	+= -DDWC_EN_ISOC
CPPFLAGS   	+= -I$(obj)/../dwc_common_port
#CPPFLAGS   	+= -I$(PORTLIB)
CPPFLAGS   	+= -DDWC_LINUX
CPPFLAGS   	+= $(CFI)
CPPFLAGS	+= $(BUS_INTERFACE)

obj-$(CONFIG_USB_DWCOTG) += dwc_otg.o

dwc_otg-objs	:= dwc_otg_driver.o dwc_otg_attr.o 
dwc_otg-objs	+= dwc_otg_cil.o dwc_otg_cil_intr.o 
dwc_otg-objs	+= dwc_otg_pcd_linux.o dwc_otg_pcd.o dwc_otg_pcd_intr.o 
dwc_otg-objs	+= dwc_otg_hcd.o dwc_otg_hcd_linux.o dwc_otg_hcd_intr.o dwc_otg_hcd_queue.o dwc_otg_hcd_ddma.o
ifneq ($(CFI),)
dwc_otg-objs	+= dwc_otg_cfi.o
endif

kernrelwd := $(subst ., ,$(KERNELRELEASE))
kernrel3 := $(word 1,$(kernrelwd)).$(word 2,$(kernrelwd)).$(word 3,$(kernrelwd))

ifneq ($(kernrel3),2.6.20)
EXTRA_CFLAGS += $(CPPFLAGS)
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
ifneq ($(INSTALL_MOD_PATH),)
	$(MAKE) -C$(KDIR) M=$(PORTLIB) modules_install
	$(MAKE) -C$(KDIR) M=$(PWD) modules_install	
else
	@echo "No install path defined"
endif

portlib:
	$(MAKE) -C$(KDIR) M=$(PORTLIB) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	cp $(PORTLIB)/Module.symvers $(PWD)/

docs:	$(wildcard *.[hc]) doc/doxygen.cfg
	$(DOXYGEN) doc/doxygen.cfg

tags:	$(wildcard *.[hc])
	$(CTAGS) -e $(wildcard *.[hc]) $(wildcard linux/*.[hc]) $(wildcard $(KDIR)/include/linux/usb*.h)


clean:
	rm -rf   *.o *.ko .*cmd *.mod.c .tmp_versions

endif
