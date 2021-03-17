// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2013-2015 Mentor Graphics Inc.
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc.
 * Copyright (C) 2010, Guennadi Liakhovetski <g.liakhovetski@gmx.de>
 */

#ifndef __DRM_HDMI_H_
#define __DRM_HDMI_H_

#include <linux/types.h>

struct drm_bus_cfg;
struct hdmi_avi_infoframe;

bool drm_hdmi_bus_fmt_is_rgb(u32 bus_format);
bool drm_hdmi_bus_fmt_is_yuv444(u32 bus_format);
bool drm_hdmi_bus_fmt_is_yuv422(u32 bus_format);
bool drm_hdmi_bus_fmt_is_yuv420(u32 bus_format);
int drm_hdmi_bus_fmt_color_depth(u32 bus_format);
int drm_hdmi_avi_infoframe_output_colorspace(struct hdmi_avi_infoframe *frame,
					     struct drm_bus_cfg *out_bus_cfg);

#endif // __DRM_HDMI_H_
