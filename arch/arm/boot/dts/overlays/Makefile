ifeq ($(CONFIG_OF),y)

# Overlays for the Raspberry Pi platform

ifeq ($(CONFIG_ARCH_BCM2708),y)
   RPI_DT_OVERLAYS=y
endif
ifeq ($(CONFIG_ARCH_BCM2709),y)
   RPI_DT_OVERLAYS=y
endif
ifeq ($(CONFIG_ARCH_BCM2835),y)
   RPI_DT_OVERLAYS=y
endif

dtb-$(RPI_DT_OVERLAYS) += ads7846-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += at86rf233-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += bmp085_i2c-sensor-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += dwc2-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += dwc-otg-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += dht11-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += enc28j60-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += gpio-ir-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += gpio-poweroff-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += hifiberry-amp-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += hifiberry-dac-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += hifiberry-dacplus-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += hifiberry-digi-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += hy28a-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += hy28b-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += i2c-rtc-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += i2c-gpio-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += i2c0-bcm2708-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += i2c1-bcm2708-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += i2s-mmap-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += iqaudio-dac-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += iqaudio-dacplus-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += lirc-rpi-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += mcp2515-can0-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += mcp2515-can1-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += mmc-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += mz61581-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += piscreen-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += piscreen2r-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += pitft28-capacitive-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += pitft28-resistive-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += pps-gpio-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += pwm-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += pwm-2chan-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += raspidac3-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += rpi-backlight-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += rpi-dac-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += rpi-display-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += rpi-ft5406-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += rpi-proto-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += rpi-sense-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += sdhost-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += sdio-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += sdtweak-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += smi-dev-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += smi-nand-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += smi-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += spi1-1cs-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += spi1-2cs-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += spi1-3cs-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += spi2-1cs-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += spi2-2cs-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += spi2-3cs-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += spi-gpio35-39-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += tinylcd35-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += uart1-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += vc4-kms-v3d-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += vga666-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += w1-gpio-overlay.dtb
dtb-$(RPI_DT_OVERLAYS) += w1-gpio-pullup-overlay.dtb

targets += dtbs dtbs_install
targets += $(dtb-y)

endif

always		:= $(dtb-y)
clean-files	:= *.dtb

# Enable fixups to support overlays on BCM2708 platforms
ifeq ($(RPI_DT_OVERLAYS),y)
	DTC_FLAGS ?= -@
endif
