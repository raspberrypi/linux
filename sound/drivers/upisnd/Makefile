# Pisound Micro Linux kernel module.
# Copyright (C) 2017-2025  Vilniaus Blokas UAB, https://blokas.io/
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; version 2 of the
# License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#

-include $(shell pwd)/Makefile.dev

ifdef M
-include $(M)/Makefile.dev
endif

obj-$(CONFIG_SND_PISOUND_MICRO) := snd-soc-upisnd-ctrl.o snd-soc-upisnd-codec.o
snd-soc-upisnd-ctrl-objs := upisnd_module.o upisnd_ctrl.o upisnd_comm.o upisnd_sound.o upisnd_midi.o upisnd_pins.o upisnd_gpio.o upisnd_sysfs.o upisnd_utils.o
snd-soc-upisnd-codec-objs := upisnd_codec.o
