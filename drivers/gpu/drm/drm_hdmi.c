#include <linux/errno.h>
#include <linux/hdmi.h>
#include <linux/media-bus-format.h>
#include <linux/types.h>

#include <drm/drm_atomic.h>
#include <drm/drm_hdmi.h>

/**
 * drm_hdmi_bus_fmt_is_rgb() - Is the media bus format an RGB format?
 * @bus_format: MEDIA_BUS_FMT* to test
 *
 * Checks if the media bus format is an RGB one
 *
 * RETURNS:
 * True if the format is an RGB one, false otherwise
 */
bool drm_hdmi_bus_fmt_is_rgb(u32 bus_format)
{
	switch (bus_format) {
	case MEDIA_BUS_FMT_RGB888_1X24:
	case MEDIA_BUS_FMT_RGB101010_1X30:
	case MEDIA_BUS_FMT_RGB121212_1X36:
	case MEDIA_BUS_FMT_RGB161616_1X48:
		return true;

	default:
		return false;
	}
}
EXPORT_SYMBOL(drm_hdmi_bus_fmt_is_rgb);

/**
 * drm_hdmi_bus_fmt_is_yuv444() - Is the media bus format an YUV444 format?
 * @bus_format: MEDIA_BUS_FMT* to test
 *
 * Checks if the media bus format is an YUV444 one
 *
 * RETURNS:
 * True if the format is an YUV444 one, false otherwise
 */
bool drm_hdmi_bus_fmt_is_yuv444(u32 bus_format)
{
	switch (bus_format) {
	case MEDIA_BUS_FMT_YUV8_1X24:
	case MEDIA_BUS_FMT_YUV10_1X30:
	case MEDIA_BUS_FMT_YUV12_1X36:
	case MEDIA_BUS_FMT_YUV16_1X48:
		return true;

	default:
		return false;
	}
}
EXPORT_SYMBOL(drm_hdmi_bus_fmt_is_yuv444);

/**
 * drm_hdmi_bus_fmt_is_yuv422() - Is the media bus format an YUV422 format?
 * @bus_format: MEDIA_BUS_FMT* to test
 *
 * Checks if the media bus format is an YUV422 one
 *
 * RETURNS:
 * True if the format is an YUV422 one, false otherwise
 */
bool drm_hdmi_bus_fmt_is_yuv422(u32 bus_format)
{
	switch (bus_format) {
	case MEDIA_BUS_FMT_UYVY8_1X16:
	case MEDIA_BUS_FMT_UYVY10_1X20:
	case MEDIA_BUS_FMT_UYVY12_1X24:
		return true;

	default:
		return false;
	}
}
EXPORT_SYMBOL(drm_hdmi_bus_fmt_is_yuv422);

/**
 * drm_hdmi_bus_fmt_is_yuv420() - Is the media bus format an YUV420 format?
 * @bus_format: MEDIA_BUS_FMT* to test
 *
 * Checks if the media bus format is an YUV420 one
 *
 * RETURNS:
 * True if the format is an YUV420 one, false otherwise
 */
bool drm_hdmi_bus_fmt_is_yuv420(u32 bus_format)
{
	switch (bus_format) {
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
	case MEDIA_BUS_FMT_UYYVYY12_0_5X36:
	case MEDIA_BUS_FMT_UYYVYY16_0_5X48:
		return true;

	default:
		return false;
	}
}
EXPORT_SYMBOL(drm_hdmi_bus_fmt_is_yuv420);

/**
 * drm_hdmi_bus_fmt_color_depth() - Returns the color depth in bits
 * @bus_format: MEDIA_BUS_FMT* to test
 *
 * Computes the number of bits per color for a given media bus format
 *
 * RETURNS:
 * The number of bits per color
 */
int drm_hdmi_bus_fmt_color_depth(u32 bus_format)
{
	switch (bus_format) {
	case MEDIA_BUS_FMT_RGB888_1X24:
	case MEDIA_BUS_FMT_YUV8_1X24:
	case MEDIA_BUS_FMT_UYVY8_1X16:
	case MEDIA_BUS_FMT_UYYVYY8_0_5X24:
		return 8;

	case MEDIA_BUS_FMT_RGB101010_1X30:
	case MEDIA_BUS_FMT_YUV10_1X30:
	case MEDIA_BUS_FMT_UYVY10_1X20:
	case MEDIA_BUS_FMT_UYYVYY10_0_5X30:
		return 10;

	case MEDIA_BUS_FMT_RGB121212_1X36:
	case MEDIA_BUS_FMT_YUV12_1X36:
	case MEDIA_BUS_FMT_UYVY12_1X24:
	case MEDIA_BUS_FMT_UYYVYY12_0_5X36:
		return 12;

	case MEDIA_BUS_FMT_RGB161616_1X48:
	case MEDIA_BUS_FMT_YUV16_1X48:
	case MEDIA_BUS_FMT_UYYVYY16_0_5X48:
		return 16;

	default:
		return 0;
	}
}
EXPORT_SYMBOL(drm_hdmi_bus_fmt_color_depth);

/**
 * drm_hdmi_bus_fmt_color_depth() - Returns the color depth in bits
 * @bus_format: MEDIA_BUS_FMT* to test
 *
 * Computes the number of bits per color for a given media bus format
 *
 * RETURNS:
 * The number of bits per color
 */
int drm_hdmi_avi_infoframe_output_colorspace(struct hdmi_avi_infoframe *frame,
					     struct drm_bus_cfg *out_bus_cfg)
{
	if (drm_hdmi_bus_fmt_is_yuv444(out_bus_cfg->format))
		frame->colorspace = HDMI_COLORSPACE_YUV444;
	else if (drm_hdmi_bus_fmt_is_yuv422(out_bus_cfg->format))
		frame->colorspace = HDMI_COLORSPACE_YUV422;
	else if (drm_hdmi_bus_fmt_is_yuv420(out_bus_cfg->format))
		frame->colorspace = HDMI_COLORSPACE_YUV420;
	else if (drm_hdmi_bus_fmt_is_rgb(out_bus_cfg->format))
		frame->colorspace = HDMI_COLORSPACE_RGB;
	else
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL(drm_hdmi_avi_infoframe_output_colorspace);
