/*
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <drm/drm_modes.h>
#include <drm/drm_print.h>


/**
 * Low dotclock static modelines
 * 2 duplicate modes that vary with H freq : 320x240 and 1024x768
 */
static struct drm_display_mode drm_low_dotclock_modes[] = {
	/* 320x240@60.00 15.660 Khz */
	{ DRM_MODE("320x240", DRM_MODE_TYPE_DRIVER, 6640, 320, 336,
			368, 424, 0, 240, 242, 245, 261, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 384x288 15 Khz */
	{ DRM_MODE("384x288", DRM_MODE_TYPE_DRIVER, 78876, 384, 400,
			440, 504, 0, 288, 292, 295, 313, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 640x240@60.00 15 Khz */
	{ DRM_MODE("640x240", DRM_MODE_TYPE_DRIVER, 13220, 640, 672,
			736, 832, 0, 240, 243, 246, 265, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 640x480@60.00 15.750 Khz */
	{ DRM_MODE("640x480i", DRM_MODE_TYPE_DRIVER, 13104, 640, 664,
			728, 832, 0, 480, 484, 490, 525, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | 
                       DRM_MODE_FLAG_INTERLACE) },
	/* 648x480@60.00 13.129 Khz */
	{ DRM_MODE("648x480i", DRM_MODE_TYPE_DRIVER, 13129, 648, 672,
			736, 840, 0, 480, 482, 488, 521, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | 
                       DRM_MODE_FLAG_INTERLACE) },
	/* 720x480@59.95 15.7369 Khz */
	{ DRM_MODE("720x480i", DRM_MODE_TYPE_DRIVER, 14856, 720, 752,
			824, 944, 0, 480, 484, 490, 525, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | 
			DRM_MODE_FLAG_INTERLACE) },
	/* 768x576 15.6250 Khz */
	{ DRM_MODE("768x576i", DRM_MODE_TYPE_DRIVER, 15625, 768, 800,
			872, 1000, 0, 576, 582, 588, 625, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | 
			DRM_MODE_FLAG_INTERLACE) },
	/* 800x576@50.00 15.725 Khz */
	{ DRM_MODE("800x576i", DRM_MODE_TYPE_DRIVER, 16354, 800, 832,
			912, 1040, 0, 576, 584, 590, 629, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | 
			DRM_MODE_FLAG_INTERLACE) },
	/* 1280x480@60.00 15.690 Khz - 25MHz dotclock for i915+nouveau*/
	{ DRM_MODE("1280x480i", DRM_MODE_TYPE_DRIVER, 25983, 1280, 1328,
			1448, 1656, 0, 480, 483, 489, 523, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | 
			DRM_MODE_FLAG_INTERLACE) },
	/* 512x384@58.59 24.960 Khz */
	{ DRM_MODE("512x384", DRM_MODE_TYPE_DRIVER, 16972, 512, 560,
			608, 680, 0, 384, 395, 399, 426, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 800x600@60.00 24.990 Khz */
	{ DRM_MODE("800x600i", DRM_MODE_TYPE_DRIVER, 26989, 800, 880,
			960, 1080, 0, 600, 697, 705, 833, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | 
			DRM_MODE_FLAG_INTERLACE) },
	/* 1024x768@50.00 24.975 Khz */
	{ DRM_MODE("1024x768i", DRM_MODE_TYPE_DRIVER, 34165, 1024, 1120,
			1216, 1368, 0, 768, 864, 872, 999, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC | 
			DRM_MODE_FLAG_INTERLACE) },
	/* 1280x240@60.00 24.900 Khz */
	{ DRM_MODE("1280x240", DRM_MODE_TYPE_DRIVER, 39790, 1280, 1312,
			1471, 1598, 0, 240, 314, 319, 415, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 640x480@60.00 31 Khz */
	{ DRM_MODE("1280x240", DRM_MODE_TYPE_DRIVER, 25200, 640, 656,
	 752, 800, 0, 480, 489, 492, 525, 0,
	 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) }
};

/**
 * drm_display_mode - get a fixed modeline
 * @dev: drm device
 * @hdisplay: hdisplay size
 * @vdisplay: vdisplay size
 * @interlaced: whether to compute an interlaced mode
 *
 * This function returns a modeline among predefined low dotclock modes. These are fixed
 * modelines, until automatic mode generation gets added into kernel.
 * No need to specify 15 or 25kHz, not the vertical refresh rate as it's only 60Hz for now.
 * This is a very basic function. Duplicate modes (320x240 and 1024x768) are not
 * handled, the first result will be returned.
 * 
 * Returns:
 * A low dotclock drm modeline
 */
struct drm_display_mode *drm_mode_low_dotclock_res(struct drm_device *dev,
				int hsize, int vsize, bool interlace)
{
	int i;

	DRM_DEBUG_KMS("Entering drm_mode_low_dotclock_res for resolution %dx%d (interlace: %s)", hsize, vsize, interlace ? "true" : "false");
	for (i = 0; i < ARRAY_SIZE(drm_low_dotclock_modes); i++) {
		const struct drm_display_mode *ptr = &drm_low_dotclock_modes[i];
		if (hsize != ptr->hdisplay)
			continue;
		if (vsize != ptr->vdisplay)
			continue;
		//if ((refresh != 0) && (refresh != drm_mode_vrefresh(ptr)))
		//	continue;
		if (((! interlace) && (ptr->flags & DRM_MODE_FLAG_INTERLACE)) \
		    || ((interlace) && ! (ptr->flags & DRM_MODE_FLAG_INTERLACE)))
			continue;
		DRM_INFO("Found a low dotclock mode for %dx%d (interlace: %d)", hsize, vsize, interlace);
		drm_mode_debug_printmodeline(ptr);
		return drm_mode_duplicate(dev, ptr);
	}
	return NULL;
}
EXPORT_SYMBOL(drm_mode_low_dotclock_res);
