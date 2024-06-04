/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Broadcom BCM2835 ISP driver
 *
 * Copyright © 2019-2020 Raspberry Pi (Trading) Ltd.
 *
 * Author: Naushir Patuck (naush@raspberrypi.com)
 *
 */

#ifndef BCM2835_ISP_FMTS
#define BCM2835_ISP_FMTS

#include <linux/videodev2.h>
#include "../vchiq-mmal/mmal-encodings.h"

struct bcm2835_isp_fmt {
	u32 fourcc;
	int depth;
	int bytesperline_align;
	u32 mmal_fmt;
	int size_multiplier_x2;
	u32 colorspace_mask;
	enum v4l2_colorspace colorspace_default;
	unsigned int step_size;
};

#define V4L2_COLORSPACE_MASK(colorspace) BIT(colorspace)

#define V4L2_COLORSPACE_MASK_JPEG V4L2_COLORSPACE_MASK(V4L2_COLORSPACE_JPEG)
#define V4L2_COLORSPACE_MASK_SMPTE170M V4L2_COLORSPACE_MASK(V4L2_COLORSPACE_SMPTE170M)
#define V4L2_COLORSPACE_MASK_REC709 V4L2_COLORSPACE_MASK(V4L2_COLORSPACE_REC709)
#define V4L2_COLORSPACE_MASK_SRGB V4L2_COLORSPACE_MASK(V4L2_COLORSPACE_SRGB)
#define V4L2_COLORSPACE_MASK_RAW V4L2_COLORSPACE_MASK(V4L2_COLORSPACE_RAW)

/*
 * All three colour spaces JPEG, SMPTE170M and REC709 are fundamentally sRGB
 * underneath (as near as makes no difference to us), just with different YCbCr
 * encodings. Therefore the ISP can generate sRGB on its main output and any of
 * the others on its low resolution output. Applications should, when using both
 * outputs, program the colour spaces on them to be the same, matching whatever
 * is requested for the low resolution output, even if the main output is
 * producing an RGB format. In turn this requires us to allow all these colour
 * spaces for every YUV/RGB output format.
 */
#define V4L2_COLORSPACE_MASK_ALL_SRGB (V4L2_COLORSPACE_MASK_JPEG |	\
				       V4L2_COLORSPACE_MASK_SRGB |	\
				       V4L2_COLORSPACE_MASK_SMPTE170M |	\
				       V4L2_COLORSPACE_MASK_REC709)

static const struct bcm2835_isp_fmt supported_formats[] = {
	{
		/* YUV formats */
		.fourcc		    = V4L2_PIX_FMT_YUV420,
		.depth		    = 8,
		.bytesperline_align = 64,
		.mmal_fmt	    = MMAL_ENCODING_I420,
		.size_multiplier_x2 = 3,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_JPEG,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_YVU420,
		.depth		    = 8,
		.bytesperline_align = 64,
		.mmal_fmt	    = MMAL_ENCODING_YV12,
		.size_multiplier_x2 = 3,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SMPTE170M,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_NV12,
		.depth		    = 8,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_NV12,
		.size_multiplier_x2 = 3,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SMPTE170M,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_NV21,
		.depth		    = 8,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_NV21,
		.size_multiplier_x2 = 3,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SMPTE170M,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_YUYV,
		.depth		    = 16,
		.bytesperline_align = 64,
		.mmal_fmt	    = MMAL_ENCODING_YUYV,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SMPTE170M,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_UYVY,
		.depth		    = 16,
		.bytesperline_align = 64,
		.mmal_fmt	    = MMAL_ENCODING_UYVY,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SMPTE170M,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_YVYU,
		.depth		    = 16,
		.bytesperline_align = 64,
		.mmal_fmt	    = MMAL_ENCODING_YVYU,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SMPTE170M,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_VYUY,
		.depth		    = 16,
		.bytesperline_align = 64,
		.mmal_fmt	    = MMAL_ENCODING_VYUY,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SMPTE170M,
		.step_size	    = 2,
	}, {
		/* RGB formats */
		.fourcc		    = V4L2_PIX_FMT_RGB24,
		.depth		    = 24,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_RGB24,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SRGB,
		.step_size	    = 1,
	}, {
		.fourcc		    = V4L2_PIX_FMT_RGB565,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_RGB16,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SRGB,
		.step_size	    = 1,
	}, {
		.fourcc		    = V4L2_PIX_FMT_BGR24,
		.depth		    = 24,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BGR24,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SRGB,
		.step_size	    = 1,
	}, {
		.fourcc		    = V4L2_PIX_FMT_XBGR32,
		.depth		    = 32,
		.bytesperline_align = 64,
		.mmal_fmt	    = MMAL_ENCODING_BGRA,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SRGB,
		.step_size	    = 1,
	}, {
		.fourcc		    = V4L2_PIX_FMT_RGBX32,
		.depth		    = 32,
		.bytesperline_align = 64,
		.mmal_fmt	    = MMAL_ENCODING_RGBA,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_ALL_SRGB,
		.colorspace_default = V4L2_COLORSPACE_SRGB,
		.step_size	    = 1,
	}, {
		/* Bayer formats */
		/* 8 bit */
		.fourcc		    = V4L2_PIX_FMT_SRGGB8,
		.depth		    = 8,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SRGGB8,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SBGGR8,
		.depth		    = 8,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SBGGR8,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGRBG8,
		.depth		    = 8,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGRBG8,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGBRG8,
		.depth		    = 8,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGBRG8,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 10 bit */
		.fourcc		    = V4L2_PIX_FMT_SRGGB10P,
		.depth		    = 10,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SRGGB10P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SBGGR10P,
		.depth		    = 10,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SBGGR10P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGRBG10P,
		.depth		    = 10,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGRBG10P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGBRG10P,
		.depth		    = 10,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGBRG10P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 12 bit */
		.fourcc		    = V4L2_PIX_FMT_SRGGB12P,
		.depth		    = 12,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SRGGB12P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SBGGR12P,
		.depth		    = 12,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SBGGR12P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGRBG12P,
		.depth		    = 12,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGRBG12P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGBRG12P,
		.depth		    = 12,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGBRG12P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 14 bit */
		.fourcc		    = V4L2_PIX_FMT_SRGGB14P,
		.depth		    = 14,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SRGGB14P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SBGGR14P,
		.depth		    = 14,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SBGGR14P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGRBG14P,
		.depth		    = 14,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGRBG14P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGBRG14P,
		.depth		    = 14,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGBRG14P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 16 bit */
		.fourcc		    = V4L2_PIX_FMT_SRGGB16,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SRGGB16,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SBGGR16,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SBGGR16,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGRBG16,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGRBG16,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGBRG16,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGBRG16,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* Bayer formats unpacked to 16bpp */
		/* 10 bit */
		.fourcc		    = V4L2_PIX_FMT_SRGGB10,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SRGGB10,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SBGGR10,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SBGGR10,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGRBG10,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGRBG10,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGBRG10,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGBRG10,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 12 bit */
		.fourcc		    = V4L2_PIX_FMT_SRGGB12,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SRGGB12,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SBGGR12,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SBGGR12,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGRBG12,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGRBG12,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGBRG12,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGBRG12,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 14 bit */
		.fourcc		    = V4L2_PIX_FMT_SRGGB14,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SRGGB14,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SBGGR14,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SBGGR14,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGRBG14,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGRBG14,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_PIX_FMT_SGBRG14,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_BAYER_SGBRG14,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* Monochrome MIPI formats */
		/* 8 bit */
		.fourcc		    = V4L2_PIX_FMT_GREY,
		.depth		    = 8,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_GREY,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 10 bit */
		.fourcc		    = V4L2_PIX_FMT_Y10P,
		.depth		    = 10,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_Y10P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 12 bit */
		.fourcc		    = V4L2_PIX_FMT_Y12P,
		.depth		    = 12,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_Y12P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 14 bit */
		.fourcc		    = V4L2_PIX_FMT_Y14P,
		.depth		    = 14,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_Y14P,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 16 bit */
		.fourcc		    = V4L2_PIX_FMT_Y16,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_Y16,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 10 bit as 16bpp */
		.fourcc		    = V4L2_PIX_FMT_Y10,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_Y10,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 12 bit as 16bpp */
		.fourcc		    = V4L2_PIX_FMT_Y12,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_Y12,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		/* 14 bit as 16bpp */
		.fourcc		    = V4L2_PIX_FMT_Y14,
		.depth		    = 16,
		.bytesperline_align = 32,
		.mmal_fmt	    = MMAL_ENCODING_Y14,
		.size_multiplier_x2 = 2,
		.colorspace_mask    = V4L2_COLORSPACE_MASK_RAW,
		.colorspace_default = V4L2_COLORSPACE_RAW,
		.step_size	    = 2,
	}, {
		.fourcc		    = V4L2_META_FMT_BCM2835_ISP_STATS,
		.depth		    = 8,
		.mmal_fmt	    = MMAL_ENCODING_BRCM_STATS,
		/* The rest are not valid fields for stats. */
	}
};

#endif
