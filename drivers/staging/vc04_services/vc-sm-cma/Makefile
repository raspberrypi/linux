ccflags-y += -Idrivers/staging/vc04_services -Idrivers/staging/vc04_services/interface/vchi -Idrivers/staging/vc04_services/interface/vchiq_arm
# -I"drivers/staging/android/ion/" -I"$(srctree)/fs/"
ccflags-y += -D__VCCOREVER__=0

vc-sm-cma-$(CONFIG_BCM_VC_SM_CMA) := \
	vc_sm.o vc_sm_cma_vchi.o vc_sm_cma.o

obj-$(CONFIG_BCM_VC_SM_CMA) += vc-sm-cma.o
