EXTRA_CFLAGS  += -Wall -Wstrict-prototypes -Wno-trigraphs -O2

EXTRA_CFLAGS  += -I"drivers/misc/vc04_services"
EXTRA_CFLAGS  += -I"drivers/misc/vc04_services/interface/vchi"
EXTRA_CFLAGS  += -I"drivers/misc/vc04_services/interface/vchiq_arm"
EXTRA_CFLAGS  += -I"$(srctree)/fs/"

EXTRA_CFLAGS  += -DOS_ASSERT_FAILURE
EXTRA_CFLAGS  += -D__STDC_VERSION=199901L
EXTRA_CFLAGS  += -D__STDC_VERSION__=199901L
EXTRA_CFLAGS  += -D__VCCOREVER__=0
EXTRA_CFLAGS  += -D__KERNEL__
EXTRA_CFLAGS  += -D__linux__
EXTRA_CFLAGS  += -Werror

obj-$(CONFIG_BCM_VC_SM) := vc-sm.o

vc-sm-objs := \
    vmcs_sm.o \
    vc_vchi_sm.o
