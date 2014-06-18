ccflags-y := -Iinclude/drm

# Please keep these build lists sorted!

# core driver code
vc4-y := \
	vc4_drv.o \
	vc4_display.o \
	vc4_gem.o \
	vc4_validate.o \
	$()

vc4-$(CONFIG_DEBUG_FS) += vc4_debugfs.o

obj-$(CONFIG_DRM_VC4)  += vc4.o

CFLAGS_vc4_trace_points.o := -I$(src)
