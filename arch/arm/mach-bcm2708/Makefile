#
# Makefile for the linux kernel.
#

obj-$(CONFIG_MACH_BCM2708) 	+= clock.o bcm2708.o armctrl.o vcio.o power.o dma.o
obj-$(CONFIG_BCM2708_GPIO)	+= bcm2708_gpio.o
obj-$(CONFIG_BCM2708_VCMEM)	+= vc_mem.o

obj-$(CONFIG_BCM2708_DMAER)	+= dmaer_master.o
dmaer_master-objs		:= dmaer.o vc_support.o

