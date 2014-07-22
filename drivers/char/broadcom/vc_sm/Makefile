ccflags-$(CONFIG_BCM_VC_SM) += -Werror -Wall -Wstrict-prototypes -Wno-trigraphs -O2
ccflags-$(CONFIG_BCM_VC_SM) += -I$(srctree)/"drivers/staging/vc04_services" -I$(srctree)/"drivers/staging/vc04_services/interface/vchi" -I$(srctree)/"drivers/staging/vc04_services/interface/vchiq_arm" -I$(srctree)/"fs"
ccflags-$(CONFIG_BCM_VC_SM) += -DOS_ASSERT_FAILURE -D__STDC_VERSION=199901L -D__STDC_VERSION__=199901L -D__VCCOREVER__=0 -D__KERNEL__ -D__linux__

obj-$(CONFIG_BCM_VC_SM) := vc-sm.o

vc-sm-objs := \
    vmcs_sm.o \
    vc_vchi_sm.o
