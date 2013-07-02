ccflags-y  += -Wall -Wstrict-prototypes -Wno-trigraphs
ccflags-y  += -Werror
ccflags-y  += -Iinclude/linux/broadcom
ccflags-y  += -Idrivers/misc/vc04_services
ccflags-y  += -Idrivers/misc/vc04_services/interface/vchi
ccflags-y  += -Idrivers/misc/vc04_services/interface/vchiq_arm

ccflags-y  += -D__KERNEL__
ccflags-y  += -D__linux__
ccflags-y  += -Werror

obj-$(CONFIG_BCM_VC_CMA) += vc-cma.o

vc-cma-objs := vc_cma.o
