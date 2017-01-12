# Enable fixups to support overlays on BCM2835 platforms

ifeq ($(CONFIG_ARCH_BCM2835),y)
DTC_FLAGS ?= -@ -H epapr
endif

dtb-$(CONFIG_ARCH_BCM2835) += bcm2837-rpi-3-b.dtb
dtb-$(CONFIG_ARCH_BCM_IPROC) += ns2-svk.dtb
dtb-$(CONFIG_ARCH_VULCAN) += vulcan-eval.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2710-rpi-3-b.dtb

dts-dirs += ../overlays

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb
