ifeq ($(CONFIG_MACH_BCM2708),y)

##############################################################################
#
# This section is for building the RaspberryPi model
#
##############################################################################

obj-$(CONFIG_BCM2708_VCHIQ)	+= vchiq.o

vchiq-objs := \
   interface/vchiq_arm/vchiq_core.o  \
   interface/vchiq_arm/vchiq_arm.o \
   interface/vchiq_arm/vchiq_kern_lib.o \
   interface/vchiq_arm/vchiq_2835_arm.o \
   interface/vchiq_arm/vchiq_proc.o \
   interface/vchiq_arm/vchiq_shim.o \
   interface/vchiq_arm/vchiq_util.o \
   interface/vchiq_arm/vchiq_connected.o \

EXTRA_CFLAGS += -DVCOS_VERIFY_BKPTS=1 -Idrivers/misc/vc04_services -DUSE_VCHIQ_ARM -D__VCCOREVER__=0x04000000

endif

ifeq ($(CONFIG_ARCH_KONA),y)

##############################################################################
#
# This section is for building the Capri/Island model
#
##############################################################################

# Remove the quotes from the platform config name
VCHIQ_PLATFORM_CONFIG = $(patsubst "%",%,$(CONFIG_VCHIQ_PLATFORM_CFG_NAME))

$(info VCHIQ_PLATFORM_CONFIG = $(VCHIQ_PLATFORM_CONFIG))
include $(srctree)/drivers/misc/vc04_services/interface/vchiq_arm/platform-cfg/$(VCHIQ_PLATFORM_CONFIG).mk

$(info Building for MAP build system)
$(info CONFIG_ARCH_KONA    = $(CONFIG_ARCH_KONA))

EXTRA_CFLAGS  += -Wall -Wstrict-prototypes -Wno-trigraphs -O2 -Wno-declaration-after-statement -Wno-parentheses

EXTRA_CFLAGS  += -I"drivers/misc/vc04_services/"
EXTRA_CFLAGS  += -I"drivers/misc/vc04_services/interface/vchiq_arm"
EXTRA_CFLAGS  += -I"drivers/misc/vc04_services/interface/debug_sym"
EXTRA_CFLAGS  += -I"include/linux/broadcom"

EXTRA_CFLAGS  += -Werror

VCHIQ_HOST_BUS_ALIGNMENT = 3

EXTRA_CFLAGS  += -DOS_ASSERT_FAILURE
EXTRA_CFLAGS  += -D__STDC_VERSION=199901L
EXTRA_CFLAGS  += -D__STDC_VERSION__=199901L
EXTRA_CFLAGS  += -D__VCCOREVER__=0
EXTRA_CFLAGS  += -D__linux__
EXTRA_CFLAGS  += -D__KERNEL__
EXTRA_CFLAGS  += -DVCHIQ_HOST_BUS_ALIGNMENT=$(VCHIQ_HOST_BUS_ALIGNMENT)
EXTRA_CFLAGS  += -DVCHIQ_SLOT_PADDING=16
EXTRA_CFLAGS  += -DVCHIQ_ARM_SIDE
EXTRA_CFLAGS  += -DUSE_VCHIQ_ARM
ifeq ($(VCHIQ_SM_ALLOC),vcddr)
EXTRA_CFLAGS  += -DVCHIQ_SM_ALLOC_VCDDR
endif

obj-$(CONFIG_KONA_VCHIQ) += vc-vchiq.o
vc-vchiq-objs := \
    interface/vchiq_arm/vchiq_arm.o \
    interface/vchiq_arm/vchiq_connected.o \
    interface/vchiq_arm/vchiq_core.o \
    interface/vchiq_arm/vchiq_kern_lib.o \
    interface/vchiq_arm/vchiq_shim.o \
    interface/vchiq_arm/vchiq_util.o \
    interface/vchiq_arm/vchiq_kona_arm.o \
    interface/vchiq_arm/vchiq_$(VCHIQ_PLATFORM)_arm.o \
    interface/vchiq_arm/vchiq_version.o \
    interface/vchiq_arm/vchiq_proc.o
ifeq ($(VCHIQ_SM_ALLOC),vcddr)
vc-vchiq-objs += \
    interface/debug_sym/debug_sym.o
endif

obj-$(CONFIG_KONA_VCHIQ_MEMDRV) += vc-vchiq-memdrv-kona.o
vc-vchiq-memdrv-kona-objs := \
    interface/vchiq_arm/vchiq_memdrv_kona.o

genversion = drivers/misc/vc04_services/interface/vchiq_arm/vchiq_genversion

GENVERSION_SRC_ROOT_DIR = .
ifeq ($(CONFIG_ANDROID),y)
GENVERSION_SRC_ROOT_DIR = $(srctree)
endif

$(obj)/interface/vchiq_arm/vchiq_version.c: $(genversion) FORCE
	echo "(new) Generating $@ ..."
	perl $(srctree)/$(genversion) vchiq $(GENVERSION_SRC_ROOT_DIR) > $@

clean-files := vchiq_version.c

endif
