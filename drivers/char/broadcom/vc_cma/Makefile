ccflags-$(CONFIG_BCM_VC_CMA)  += -Wall -Wstrict-prototypes -Wno-trigraphs -Werror
ccflags-$(CONFIG_BCM_VC_CMA)  += -Iinclude/linux/broadcom -Idrivers/staging/vc04_services -Idrivers/staging/vc04_services/interface/vchi -Idrivers/staging/vc04_services/interface/vchiq_arm
ccflags-$(CONFIG_BCM_VC_CMA)  += -D__KERNEL__ -D__linux__

obj-$(CONFIG_BCM_VC_CMA) += vc-cma.o

vc-cma-objs := vc_cma.o
