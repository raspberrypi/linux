# SPDX-License-Identifier: GPL-2.0

# Enables support for device-tree overlays
DTC_FLAGS := -@

dtb-$(CONFIG_ARCH_BCM2835) += bcm2711-rpi-400.dtb \
			      bcm2711-rpi-4-b.dtb \
			      bcm2711-rpi-cm4-io.dtb \
			      bcm2712-rpi-5-b.dtb \
			      bcm2712-d-rpi-5-b.dtb \
			      bcm2837-rpi-2-b.dtb \
			      bcm2837-rpi-3-a-plus.dtb \
			      bcm2837-rpi-3-b.dtb \
			      bcm2837-rpi-3-b-plus.dtb \
			      bcm2837-rpi-cm3-io3.dtb \
			      bcm2837-rpi-zero-2-w.dtb

dtb-$(CONFIG_ARCH_BCM2835) += bcm2710-rpi-zero-2.dtb \
			      bcm2710-rpi-zero-2-w.dtb \
			      bcm2710-rpi-2-b.dtb \
			      bcm2710-rpi-3-b.dtb \
			      bcm2710-rpi-3-b-plus.dtb \
			      bcm2710-rpi-cm0.dtb \
			      bcm2710-rpi-cm3.dtb \
			      bcm2711-rpi-cm4.dtb \
			      bcm2711-rpi-cm4s.dtb \
			      bcm2712-rpi-5-b.dtb \
			      bcm2712d0-rpi-5-b.dtb \
			      bcm2712-rpi-500.dtb \
			      bcm2712-rpi-cm5-cm5io.dtb \
			      bcm2712-rpi-cm5-cm4io.dtb \
			      bcm2712-rpi-cm5l-cm5io.dtb \
			      bcm2712-rpi-cm5l-cm4io.dtb

subdir-y	+= bcmbca
subdir-y	+= northstar2
subdir-y	+= stingray

ifeq ($(CONFIG_ARCH_BCM2835),y)
	DTC_FLAGS += -Wno-gpios_property
endif
