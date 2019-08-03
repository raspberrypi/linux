# SPDX-License-Identifier: GPL-2.0
bcm2835-codec-objs := bcm2835-v4l2-codec.o

obj-$(CONFIG_VIDEO_CODEC_BCM2835) += bcm2835-codec.o

ccflags-y += \
	-I$(srctree)/drivers/staging/vc04_services \
	-D__VCCOREVER__=0x04000000
