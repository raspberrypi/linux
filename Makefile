# BCM2835 Platform Support
snd-soc-bcm2835-i2s-objs := bcm2835-i2s.o

obj-$(CONFIG_SND_BCM2835_SOC_I2S) += snd-soc-bcm2835-i2s.o

# BCM2708 Machine Support

snd-soc-dionaudio-kiwi-objs := dionaudio_kiwi.o

obj-$(CONFIG_SND_BCM2708_SOC_DIONAUDIO_KIWI) += snd-soc-dionaudio-kiwi.o
