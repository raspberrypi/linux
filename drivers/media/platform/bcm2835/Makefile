bcm2835-v4l2-objs := bcm2835-camera.o controls.o mmal-vchiq.o

obj-$(CONFIG_VIDEO_BCM2835_MMAL) += bcm2835-v4l2.o

ccflags-$(CONFIG_VIDEO_BCM2835) += -Idrivers/misc/vc04_services -Idrivers/misc/vc04_services/interface/vcos/linuxkernel -D__VCCOREVER__=0x04000000
