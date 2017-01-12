# Enable fixups to support overlays on BCM2835 platforms

ifeq ($(CONFIG_ARCH_BCM2835),y)
DTC_FLAGS ?= -@ -H epapr
endif

dtb-$(CONFIG_ARCH_BCM2835) += bcm2837-rpi-3-b.dtb
dtb-$(CONFIG_ARCH_BCM_IPROC) += ns2-svk.dtb ns2-xmc.dtb
dtb-$(CONFIG_ARCH_BCM2709) += bcm2710-rpi-3-b.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2710-rpi-3-b.dtb

dts-dirs += ../overlays

dts-dirs	+= stingray
always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb
