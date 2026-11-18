# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Atmel network device drivers.
#
macb-y	:= macb_main.o

ifeq ($(CONFIG_MACB_USE_HWSTAMP),y)
macb-y	+= macb_ptp.o
endif

ifeq ($(CONFIG_MACB_RP1_PPS),y)
macb-y	+= macb_rp1_ptp.o
endif

obj-$(CONFIG_MACB) += macb.o
obj-$(CONFIG_MACB_PCI) += macb_pci.o
