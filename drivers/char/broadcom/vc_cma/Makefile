EXTRA_CFLAGS  += -Wall -Wstrict-prototypes -Wno-trigraphs
EXTRA_CFLAGS  += -Werror
EXTRA_CFLAGS  += -I"include/linux/broadcom"
EXTRA_CFLAGS  += -I"drivers/misc/vc04_services"
EXTRA_CFLAGS  += -I"drivers/misc/vc04_services/interface/vchi"
EXTRA_CFLAGS  += -I"drivers/misc/vc04_services/interface/vchiq_arm"

EXTRA_CFLAGS  += -D__KERNEL__
EXTRA_CFLAGS  += -D__linux__
EXTRA_CFLAGS  += -Werror

obj-$(CONFIG_BCM_VC_CMA) += vc-cma.o

vc-cma-objs := vc_cma.o

