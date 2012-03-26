obj-$(CONFIG_BCM2708_VCHIQ)	+= vchiq.o

vchiq-objs := \
   interface/vchiq_arm/vchiq_core.o  \
   interface/vchiq_arm/vchiq_shim.o  \
   interface/vchiq_arm/vchiq_util.o  \
   interface/vchiq_arm/vchiq_arm.o \
   interface/vchiq_arm/vchiq_kern_lib.o \
   interface/vchiq_arm/vchiq_2835_arm.o \
   interface/vcos/linuxkernel/vcos_linuxkernel.o \
   interface/vcos/linuxkernel/vcos_thread_map.o \
   interface/vcos/linuxkernel/vcos_linuxkernel_cfg.o \
   interface/vcos/generic/vcos_generic_event_flags.o \
   interface/vcos/generic/vcos_logcat.o \
   interface/vcos/generic/vcos_mem_from_malloc.o \
   interface/vcos/generic/vcos_cmd.o

EXTRA_CFLAGS += -DVCOS_VERIFY_BKPTS=1 -DUSE_VCHIQ_ARM -D__VCCOREVER__=0x04000000 -Idrivers/misc/vc04_services -Idrivers/misc/vc04_services/interface/vcos/linuxkernel



