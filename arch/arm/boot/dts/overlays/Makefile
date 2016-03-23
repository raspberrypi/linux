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

dtbo-$(RPI_DT_OVERLAYS) += ads7846.dtbo
dtbo-$(RPI_DT_OVERLAYS) += at86rf233.dtbo
dtbo-$(RPI_DT_OVERLAYS) += bmp085_i2c-sensor.dtbo
dtbo-$(RPI_DT_OVERLAYS) += dwc2.dtbo
dtbo-$(RPI_DT_OVERLAYS) += dwc-otg.dtbo
dtbo-$(RPI_DT_OVERLAYS) += dht11.dtbo
dtbo-$(RPI_DT_OVERLAYS) += enc28j60.dtbo
dtbo-$(RPI_DT_OVERLAYS) += gpio-ir.dtbo
dtbo-$(RPI_DT_OVERLAYS) += gpio-poweroff.dtbo
dtbo-$(RPI_DT_OVERLAYS) += hifiberry-amp.dtbo
dtbo-$(RPI_DT_OVERLAYS) += hifiberry-dac.dtbo
dtbo-$(RPI_DT_OVERLAYS) += hifiberry-dacplus.dtbo
dtbo-$(RPI_DT_OVERLAYS) += hifiberry-digi.dtbo
dtbo-$(RPI_DT_OVERLAYS) += hy28a.dtbo
dtbo-$(RPI_DT_OVERLAYS) += hy28b.dtbo
dtbo-$(RPI_DT_OVERLAYS) += i2c-rtc.dtbo
dtbo-$(RPI_DT_OVERLAYS) += i2c-gpio.dtbo
dtbo-$(RPI_DT_OVERLAYS) += i2c0-bcm2708.dtbo
dtbo-$(RPI_DT_OVERLAYS) += i2c1-bcm2708.dtbo
dtbo-$(RPI_DT_OVERLAYS) += i2s-mmap.dtbo
dtbo-$(RPI_DT_OVERLAYS) += iqaudio-dac.dtbo
dtbo-$(RPI_DT_OVERLAYS) += iqaudio-dacplus.dtbo
dtbo-$(RPI_DT_OVERLAYS) += lirc-rpi.dtbo
dtbo-$(RPI_DT_OVERLAYS) += mcp2515-can0.dtbo
dtbo-$(RPI_DT_OVERLAYS) += mcp2515-can1.dtbo
dtbo-$(RPI_DT_OVERLAYS) += mmc.dtbo
dtbo-$(RPI_DT_OVERLAYS) += mz61581.dtbo
dtbo-$(RPI_DT_OVERLAYS) += pi3-act-led.dtbo
dtbo-$(RPI_DT_OVERLAYS) += pi3-disable-bt.dtbo
dtbo-$(RPI_DT_OVERLAYS) += pi3-miniuart-bt.dtbo
dtbo-$(RPI_DT_OVERLAYS) += piscreen.dtbo
dtbo-$(RPI_DT_OVERLAYS) += piscreen2r.dtbo
dtbo-$(RPI_DT_OVERLAYS) += pitft28-capacitive.dtbo
dtbo-$(RPI_DT_OVERLAYS) += pitft28-resistive.dtbo
dtbo-$(RPI_DT_OVERLAYS) += pps-gpio.dtbo
dtbo-$(RPI_DT_OVERLAYS) += pwm.dtbo
dtbo-$(RPI_DT_OVERLAYS) += pwm-2chan.dtbo
dtbo-$(RPI_DT_OVERLAYS) += qca7000.dtbo
dtbo-$(RPI_DT_OVERLAYS) += raspidac3.dtbo
dtbo-$(RPI_DT_OVERLAYS) += rpi-backlight.dtbo
dtbo-$(RPI_DT_OVERLAYS) += rpi-dac.dtbo
dtbo-$(RPI_DT_OVERLAYS) += rpi-display.dtbo
dtbo-$(RPI_DT_OVERLAYS) += rpi-ft5406.dtbo
dtbo-$(RPI_DT_OVERLAYS) += rpi-proto.dtbo
dtbo-$(RPI_DT_OVERLAYS) += rpi-sense.dtbo
dtbo-$(RPI_DT_OVERLAYS) += sdhost.dtbo
dtbo-$(RPI_DT_OVERLAYS) += sdio.dtbo
dtbo-$(RPI_DT_OVERLAYS) += sdtweak.dtbo
dtbo-$(RPI_DT_OVERLAYS) += smi-dev.dtbo
dtbo-$(RPI_DT_OVERLAYS) += smi-nand.dtbo
dtbo-$(RPI_DT_OVERLAYS) += smi.dtbo
dtbo-$(RPI_DT_OVERLAYS) += spi1-1cs.dtbo
dtbo-$(RPI_DT_OVERLAYS) += spi1-2cs.dtbo
dtbo-$(RPI_DT_OVERLAYS) += spi1-3cs.dtbo
dtbo-$(RPI_DT_OVERLAYS) += spi2-1cs.dtbo
dtbo-$(RPI_DT_OVERLAYS) += spi2-2cs.dtbo
dtbo-$(RPI_DT_OVERLAYS) += spi2-3cs.dtbo
dtbo-$(RPI_DT_OVERLAYS) += spi-gpio35-39.dtbo
dtbo-$(RPI_DT_OVERLAYS) += tinylcd35.dtbo
dtbo-$(RPI_DT_OVERLAYS) += uart1.dtbo
dtbo-$(RPI_DT_OVERLAYS) += vc4-kms-v3d.dtbo
dtbo-$(RPI_DT_OVERLAYS) += vga666.dtbo
dtbo-$(RPI_DT_OVERLAYS) += w1-gpio.dtbo
dtbo-$(RPI_DT_OVERLAYS) += w1-gpio-pullup.dtbo
dtbo-$(RPI_DT_OVERLAYS) += wittypi.dtbo

targets += dtbs dtbs_install
targets += $(dtbo-y)

endif

always		:= $(dtbo-y)
clean-files	:= *.dtbo
