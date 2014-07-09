# SPDX-License-Identifier: GPL-2.0

# Enables support for device-tree overlays
DTC_FLAGS := -@

dtb-$(CONFIG_ARCH_BCM2835) += bcm2711-rpi-400.dtb \
			      bcm2711-rpi-4-b.dtb \
			      bcm2711-rpi-cm4-io.dtb \
			      bcm2837-rpi-3-a-plus.dtb \
			      bcm2837-rpi-3-b.dtb \
			      bcm2837-rpi-3-b-plus.dtb \
			      bcm2837-rpi-cm3-io3.dtb \
			      bcm2837-rpi-zero-2-w.dtb

dtb-$(CONFIG_ARCH_BCM2835) += bcm2710-rpi-zero-2.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2710-rpi-zero-2-w.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2710-rpi-2-b.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2710-rpi-3-b.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2710-rpi-3-b-plus.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2710-rpi-cm3.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2711-rpi-cm4.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2711-rpi-cm4s.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2712-rpi-5-b.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2712d0-rpi-5-b.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2712-rpi-cm5-cm5io.dtb
dtb-$(CONFIG_ARCH_BCM2835) += bcm2712-rpi-cm5-cm4io.dtb

subdir-y	+= bcmbca
subdir-y	+= northstar2
subdir-y	+= stingray

# Enable fixups to support overlays on BCM2835 platforms
ifeq ($(CONFIG_ARCH_BCM2835),y)
	DTC_FLAGS += -@
endif
