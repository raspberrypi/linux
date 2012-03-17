#
# Makefile for DWC_common library
#

ifneq ($(KERNELRELEASE),)

#CPPFLAGS	+= -DDEBUG_MEMORY

ifeq ($(CONFIG_USB_DEBUG),y)
CPPFLAGS	+= -DDEBUG
endif
CPPFLAGS	+= -DDWC_LINUX

obj-$(CONFIG_USB_DWCOTG)	+= dwc_common_port_lib.o
dwc_common_port_lib-objs	:= dwc_cc.o dwc_modpow.o dwc_dh.o \
				   dwc_crypto.o dwc_notifier.o \
				   dwc_common_linux.o dwc_mem.o

kernrelwd := $(subst ., ,$(KERNELRELEASE))
kernrel3 := $(word 1,$(kernrelwd)).$(word 2,$(kernrelwd)).$(word 3,$(kernrelwd))

ifneq ($(kernrel3),2.6.20)
# grayg - I only know that we use EXTRA_CFLAGS in 2.6.31 actually
EXTRA_CFLAGS += $(CPPFLAGS)
endif

else

ifeq ($(DOXYGEN),)
DOXYGEN		:= $(DOXYGEN)
endif

default:
	$(MAKE) -C$(KDIR) M=$(PWD) modules

docs:	$(wildcard *.[hc]) doc/doxygen.cfg
	$(DOXYGEN) doc/doxygen.cfg

tags:	$(wildcard *.[hc])
	$(CTAGS) -e $(wildcard *.[hc]) $(wildcard linux/*.[hc]) $(wildcard $(KDIR)/include/linux/usb*.h)

endif

clean:
	rm -rf   *.o *.ko .*cmd *.mod.c .tmp_versions Module.symvers

