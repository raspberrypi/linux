// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Waveshare International Limited
 *
 * Based on panel-raspberrypi-touchscreen by Broadcom
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

struct ws_panel_desc {
	const struct panel_init_cmd *init;
	const struct drm_display_mode *mode;
	const unsigned long mode_flags;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
};

struct ws_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct ws_panel_desc *desc;

	struct gpio_desc *reset;
	struct regulator *iovcc;
	struct regulator *avdd;
	bool prepared;

	enum drm_panel_orientation orientation;
};

enum dsi_cmd_type {
	INIT_DCS_CMD,
	DELAY_CMD,
};

struct panel_init_cmd {
	enum dsi_cmd_type type;
	size_t len;
	const char *data;
};

#define _INIT_DCS_CMD(...)                                                    \
	{                                                                     \
		.type = INIT_DCS_CMD, .len = sizeof((char[]){ __VA_ARGS__ }), \
		.data = (char[])                                              \
		{                                                             \
			__VA_ARGS__                                           \
		}                                                             \
	}

#define _INIT_DELAY_CMD(...)                                               \
	{                                                                  \
		.type = DELAY_CMD, .len = sizeof((char[]){ __VA_ARGS__ }), \
		.data = (char[])                                           \
		{                                                          \
			__VA_ARGS__                                        \
		}                                                          \
	}

static const struct panel_init_cmd ws_panel_10_1_e_4lane_init[] = {
	_INIT_DCS_CMD(0x11),
	_INIT_DELAY_CMD(120),
	_INIT_DCS_CMD(0x29),
	_INIT_DELAY_CMD(50),
	{},
};

static const struct panel_init_cmd ws_panel_7_a_init[] = {
	_INIT_DCS_CMD(0xFF, 0x98, 0x81, 0x03),
	_INIT_DCS_CMD(0x01, 0x00),
	_INIT_DCS_CMD(0x02, 0x00),
	_INIT_DCS_CMD(0x03, 0x73),
	_INIT_DCS_CMD(0x04, 0x00),
	_INIT_DCS_CMD(0x05, 0x00),
	_INIT_DCS_CMD(0x06, 0x0A),
	_INIT_DCS_CMD(0x07, 0x00),
	_INIT_DCS_CMD(0x08, 0x00),
	_INIT_DCS_CMD(0x09, 0x61),
	_INIT_DCS_CMD(0x0A, 0x00),
	_INIT_DCS_CMD(0x0B, 0x00),
	_INIT_DCS_CMD(0x0C, 0x01),
	_INIT_DCS_CMD(0x0D, 0x00),
	_INIT_DCS_CMD(0x0E, 0x00),
	_INIT_DCS_CMD(0x0F, 0x61),
	_INIT_DCS_CMD(0x10, 0x61),
	_INIT_DCS_CMD(0x11, 0x00),
	_INIT_DCS_CMD(0x12, 0x00),
	_INIT_DCS_CMD(0x13, 0x00),
	_INIT_DCS_CMD(0x14, 0x00),
	_INIT_DCS_CMD(0x15, 0x00),
	_INIT_DCS_CMD(0x16, 0x00),
	_INIT_DCS_CMD(0x17, 0x00),
	_INIT_DCS_CMD(0x18, 0x00),
	_INIT_DCS_CMD(0x19, 0x00),
	_INIT_DCS_CMD(0x1A, 0x00),
	_INIT_DCS_CMD(0x1B, 0x00),
	_INIT_DCS_CMD(0x1C, 0x00),
	_INIT_DCS_CMD(0x1D, 0x00),
	_INIT_DCS_CMD(0x1E, 0x40),
	_INIT_DCS_CMD(0x1F, 0x80),
	_INIT_DCS_CMD(0x20, 0x06),
	_INIT_DCS_CMD(0x21, 0x01),
	_INIT_DCS_CMD(0x22, 0x00),
	_INIT_DCS_CMD(0x23, 0x00),
	_INIT_DCS_CMD(0x24, 0x00),
	_INIT_DCS_CMD(0x25, 0x00),
	_INIT_DCS_CMD(0x26, 0x00),
	_INIT_DCS_CMD(0x27, 0x00),
	_INIT_DCS_CMD(0x28, 0x33),
	_INIT_DCS_CMD(0x29, 0x03),
	_INIT_DCS_CMD(0x2A, 0x00),
	_INIT_DCS_CMD(0x2B, 0x00),
	_INIT_DCS_CMD(0x2C, 0x00),
	_INIT_DCS_CMD(0x2D, 0x00),
	_INIT_DCS_CMD(0x2E, 0x00),
	_INIT_DCS_CMD(0x2F, 0x00),
	_INIT_DCS_CMD(0x30, 0x00),
	_INIT_DCS_CMD(0x31, 0x00),
	_INIT_DCS_CMD(0x32, 0x00),
	_INIT_DCS_CMD(0x33, 0x00),
	_INIT_DCS_CMD(0x34, 0x04),
	_INIT_DCS_CMD(0x35, 0x00),
	_INIT_DCS_CMD(0x36, 0x00),
	_INIT_DCS_CMD(0x37, 0x00),
	_INIT_DCS_CMD(0x38, 0x3C),
	_INIT_DCS_CMD(0x39, 0x00),
	_INIT_DCS_CMD(0x3A, 0x00),
	_INIT_DCS_CMD(0x3B, 0x00),
	_INIT_DCS_CMD(0x3C, 0x00),
	_INIT_DCS_CMD(0x3D, 0x00),
	_INIT_DCS_CMD(0x3E, 0x00),
	_INIT_DCS_CMD(0x3F, 0x00),
	_INIT_DCS_CMD(0x40, 0x00),
	_INIT_DCS_CMD(0x41, 0x00),
	_INIT_DCS_CMD(0x42, 0x00),
	_INIT_DCS_CMD(0x43, 0x00),
	_INIT_DCS_CMD(0x44, 0x00),
	_INIT_DCS_CMD(0x50, 0x10),
	_INIT_DCS_CMD(0x51, 0x32),
	_INIT_DCS_CMD(0x52, 0x54),
	_INIT_DCS_CMD(0x53, 0x76),
	_INIT_DCS_CMD(0x54, 0x98),
	_INIT_DCS_CMD(0x55, 0xBA),
	_INIT_DCS_CMD(0x56, 0x10),
	_INIT_DCS_CMD(0x57, 0x32),
	_INIT_DCS_CMD(0x58, 0x54),
	_INIT_DCS_CMD(0x59, 0x76),
	_INIT_DCS_CMD(0x5A, 0x98),
	_INIT_DCS_CMD(0x5B, 0xBA),
	_INIT_DCS_CMD(0x5C, 0xDC),
	_INIT_DCS_CMD(0x5D, 0xFE),
	_INIT_DCS_CMD(0x5E, 0x00),
	_INIT_DCS_CMD(0x5F, 0x0E),
	_INIT_DCS_CMD(0x60, 0x0F),
	_INIT_DCS_CMD(0x61, 0x0C),
	_INIT_DCS_CMD(0x62, 0x0D),
	_INIT_DCS_CMD(0x63, 0x06),
	_INIT_DCS_CMD(0x64, 0x07),
	_INIT_DCS_CMD(0x65, 0x02),
	_INIT_DCS_CMD(0x66, 0x02),
	_INIT_DCS_CMD(0x67, 0x02),
	_INIT_DCS_CMD(0x68, 0x02),
	_INIT_DCS_CMD(0x69, 0x01),
	_INIT_DCS_CMD(0x6A, 0x00),
	_INIT_DCS_CMD(0x6B, 0x02),
	_INIT_DCS_CMD(0x6C, 0x15),
	_INIT_DCS_CMD(0x6D, 0x14),
	_INIT_DCS_CMD(0x6E, 0x02),
	_INIT_DCS_CMD(0x6F, 0x02),
	_INIT_DCS_CMD(0x70, 0x02),
	_INIT_DCS_CMD(0x71, 0x02),
	_INIT_DCS_CMD(0x72, 0x02),
	_INIT_DCS_CMD(0x73, 0x02),
	_INIT_DCS_CMD(0x74, 0x02),
	_INIT_DCS_CMD(0x75, 0x0E),
	_INIT_DCS_CMD(0x76, 0x0F),
	_INIT_DCS_CMD(0x77, 0x0C),
	_INIT_DCS_CMD(0x78, 0x0D),
	_INIT_DCS_CMD(0x79, 0x06),
	_INIT_DCS_CMD(0x7A, 0x07),
	_INIT_DCS_CMD(0x7B, 0x02),
	_INIT_DCS_CMD(0x7C, 0x02),
	_INIT_DCS_CMD(0x7D, 0x02),
	_INIT_DCS_CMD(0x7E, 0x02),
	_INIT_DCS_CMD(0x7F, 0x01),
	_INIT_DCS_CMD(0x80, 0x00),
	_INIT_DCS_CMD(0x81, 0x02),
	_INIT_DCS_CMD(0x82, 0x14),
	_INIT_DCS_CMD(0x83, 0x15),
	_INIT_DCS_CMD(0x84, 0x02),
	_INIT_DCS_CMD(0x85, 0x02),
	_INIT_DCS_CMD(0x86, 0x02),
	_INIT_DCS_CMD(0x87, 0x02),
	_INIT_DCS_CMD(0x88, 0x02),
	_INIT_DCS_CMD(0x89, 0x02),
	_INIT_DCS_CMD(0x8A, 0x02),
	_INIT_DCS_CMD(0xFF, 0x98, 0x81, 0x04),
	_INIT_DCS_CMD(0x38, 0x01),
	_INIT_DCS_CMD(0x39, 0x00),
	_INIT_DCS_CMD(0x6C, 0x15),
	_INIT_DCS_CMD(0x6E, 0x2A),
	_INIT_DCS_CMD(0x6F, 0x33),
	_INIT_DCS_CMD(0x3A, 0x94),
	_INIT_DCS_CMD(0x8D, 0x14),
	_INIT_DCS_CMD(0x87, 0xBA),
	_INIT_DCS_CMD(0x26, 0x76),
	_INIT_DCS_CMD(0xB2, 0xD1),
	_INIT_DCS_CMD(0xB5, 0x06),
	_INIT_DCS_CMD(0x3B, 0x98),
	_INIT_DCS_CMD(0xFF, 0x98, 0x81, 0x01),
	_INIT_DCS_CMD(0x22, 0x0A),
	_INIT_DCS_CMD(0x31, 0x00),
	_INIT_DCS_CMD(0x53, 0x71),
	_INIT_DCS_CMD(0x55, 0x8F),
	_INIT_DCS_CMD(0x40, 0x33),
	_INIT_DCS_CMD(0x50, 0x96),
	_INIT_DCS_CMD(0x51, 0x96),
	_INIT_DCS_CMD(0x60, 0x23),
	_INIT_DCS_CMD(0xA0, 0x08),
	_INIT_DCS_CMD(0xA1, 0x1D),
	_INIT_DCS_CMD(0xA2, 0x2A),
	_INIT_DCS_CMD(0xA3, 0x10),
	_INIT_DCS_CMD(0xA4, 0x15),
	_INIT_DCS_CMD(0xA5, 0x28),
	_INIT_DCS_CMD(0xA6, 0x1C),
	_INIT_DCS_CMD(0xA7, 0x1D),
	_INIT_DCS_CMD(0xA8, 0x7E),
	_INIT_DCS_CMD(0xA9, 0x1D),
	_INIT_DCS_CMD(0xAA, 0x29),
	_INIT_DCS_CMD(0xAB, 0x6B),
	_INIT_DCS_CMD(0xAC, 0x1A),
	_INIT_DCS_CMD(0xAD, 0x18),
	_INIT_DCS_CMD(0xAE, 0x4B),
	_INIT_DCS_CMD(0xAF, 0x20),
	_INIT_DCS_CMD(0xB0, 0x27),
	_INIT_DCS_CMD(0xB1, 0x50),
	_INIT_DCS_CMD(0xB2, 0x64),
	_INIT_DCS_CMD(0xB3, 0x39),
	_INIT_DCS_CMD(0xC0, 0x08),
	_INIT_DCS_CMD(0xC1, 0x1D),
	_INIT_DCS_CMD(0xC2, 0x2A),
	_INIT_DCS_CMD(0xC3, 0x10),
	_INIT_DCS_CMD(0xC4, 0x15),
	_INIT_DCS_CMD(0xC5, 0x28),
	_INIT_DCS_CMD(0xC6, 0x1C),
	_INIT_DCS_CMD(0xC7, 0x1D),
	_INIT_DCS_CMD(0xC8, 0x7E),
	_INIT_DCS_CMD(0xC9, 0x1D),
	_INIT_DCS_CMD(0xCA, 0x29),
	_INIT_DCS_CMD(0xCB, 0x6B),
	_INIT_DCS_CMD(0xCC, 0x1A),
	_INIT_DCS_CMD(0xCD, 0x18),
	_INIT_DCS_CMD(0xCE, 0x4B),
	_INIT_DCS_CMD(0xCF, 0x20),
	_INIT_DCS_CMD(0xD0, 0x27),
	_INIT_DCS_CMD(0xD1, 0x50),
	_INIT_DCS_CMD(0xD2, 0x64),
	_INIT_DCS_CMD(0xD3, 0x39),
	_INIT_DCS_CMD(0xFF, 0x98, 0x81, 0x00),
	_INIT_DCS_CMD(0x3A, 0x77),
	_INIT_DCS_CMD(0x36, 0x00),
	_INIT_DCS_CMD(0x35, 0x00),
	_INIT_DCS_CMD(0x11),
	_INIT_DELAY_CMD(120),
	_INIT_DCS_CMD(0x29),
	_INIT_DELAY_CMD(60),
	{},
};

static const struct panel_init_cmd ws_panel_7_c_init[] = {
	_INIT_DCS_CMD(0x80, 0x8B),
	_INIT_DCS_CMD(0x81, 0x78),
	_INIT_DCS_CMD(0x82, 0x84),
	_INIT_DCS_CMD(0x83, 0x88),
	_INIT_DCS_CMD(0x84, 0xA8),
	_INIT_DCS_CMD(0x85, 0xE3),
	_INIT_DCS_CMD(0x86, 0x88),
	_INIT_DCS_CMD(0x87, 0x5A),
	_INIT_DCS_CMD(0xB0, 0x80),
	_INIT_DCS_CMD(0xB1, 0x30),
	_INIT_DCS_CMD(0xB2, 0x50),
	_INIT_DCS_CMD(0x11),
	_INIT_DELAY_CMD(120),
	_INIT_DCS_CMD(0x29),
	_INIT_DELAY_CMD(20),
	{},
};

static const struct panel_init_cmd ws_panel_6_25_a_init[] = {
	_INIT_DCS_CMD(0x60, 0x71, 0x23, 0xa2),
	_INIT_DCS_CMD(0x60, 0x71, 0x23, 0xa3),
	_INIT_DCS_CMD(0x60, 0x71, 0x23, 0xa4),
	_INIT_DCS_CMD(0xB0, 0x22, 0x61, 0x11, 0x6B, 0x25, 0x39, 0x39),
	_INIT_DCS_CMD(0xB7, 0x00, 0x00, 0x64, 0x64),
	_INIT_DCS_CMD(0xBF, 0x95, 0x95),
	_INIT_DCS_CMD(0xA4, 0x31),
	_INIT_DCS_CMD(0xC8, 0x00, 0x00, 0x10, 0x1F, 0x3A, 0x00, 0x66, 0x03,
		      0xA8, 0x04, 0x11, 0x02, 0x06, 0x73, 0x01, 0x21, 0xC1,
		      0x02, 0x01, 0x02, 0x22, 0x3E, 0x00, 0x84, 0x09, 0x32,
		      0xE9, 0x0B, 0x3B, 0x0D, 0xF3, 0x7E, 0x0D, 0xB2, 0xE6,
		      0x03, 0xFC),
	_INIT_DCS_CMD(0xC9, 0x00, 0x00, 0x10, 0x1F, 0x3A, 0x00, 0x66, 0x03,
		      0xA8, 0x04, 0x11, 0x02, 0x06, 0x73, 0x01, 0x21, 0xC1,
		      0x02, 0x01, 0x02, 0x22, 0x3E, 0x00, 0x84, 0x09, 0x32,
		      0xE9, 0x0B, 0x3B, 0x0D, 0xF3, 0x7E, 0x0D, 0xB2, 0xE6,
		      0x03, 0xFC),
	_INIT_DCS_CMD(0xD7, 0x10, 0x12, 0x49, 0x3B, 0x0C, 0x0C),
	_INIT_DCS_CMD(0x90, 0x71, 0x23, 0x5A, 0x20, 0x24, 0x09, 0x09),
	_INIT_DCS_CMD(0xA3, 0x80, 0x01, 0x88, 0x30, 0x05, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x46, 0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80,
		      0x00, 0x4F, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
		      0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x6F, 0x58,
		      0x00, 0x00, 0x00, 0xFF),
	_INIT_DCS_CMD(0xA6, 0x50, 0x00, 0x24, 0x55, 0x3A, 0x00, 0x00, 0x01,
		      0x54, 0x54, 0x91, 0xFF, 0x00, 0x24, 0x55, 0x3B, 0x00,
		      0x00, 0x00, 0x54, 0x54, 0x91, 0xFF, 0x00, 0x24, 0x11,
		      0x00, 0x00, 0x00, 0x00, 0x54, 0x54, 0x91, 0xFF, 0x00,
		      0xAC, 0x11, 0x00, 0x00, 0x00, 0x00, 0x54, 0x54, 0xFF,
		      0xFF, 0x00, 0x08, 0x80, 0x08, 0x80, 0x06, 0x00, 0x00,
		      0x00, 0x00),
	_INIT_DCS_CMD(0xA7, 0x1D, 0x1D, 0x80, 0x64, 0x40, 0x07, 0x16, 0x41,
		      0x02, 0x48, 0x07, 0x54, 0x54, 0x91, 0xFF, 0x08, 0x80,
		      0x64, 0x40, 0x25, 0x34, 0x43, 0x04, 0x06, 0x05, 0x54,
		      0x54, 0x91, 0xFF, 0x08, 0x80, 0x64, 0x40, 0x00, 0x00,
		      0x40, 0x00, 0x00, 0x00, 0x54, 0x54, 0x91, 0xFF, 0x08,
		      0x80, 0x64, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00,
		      0x54, 0x54, 0x84, 0xFF, 0x08, 0x80, 0x44),
	_INIT_DCS_CMD(0xAC, 0x1C, 0x02, 0x09, 0x0B, 0x12, 0x10, 0x18, 0x19,
		      0x1C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x08, 0x0A,
		      0x13, 0x11, 0x18, 0x19, 0x1C, 0x03, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	_INIT_DCS_CMD(0xAD, 0xF0, 0x00, 0x46, 0x00, 0x05, 0x50, 0x50, 0xFF,
		      0xFF, 0xF0, 0x40, 0x06, 0x01, 0x07, 0x42, 0x42, 0xFF,
		      0xFF, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF),
	_INIT_DCS_CMD(0xAE, 0xFF, 0x03, 0x00, 0xFF, 0x03, 0x00, 0x01),
	_INIT_DCS_CMD(0xB2, 0x15, 0x19, 0x05, 0x23, 0x49, 0xAF, 0x03, 0x2E,
		      0x5C, 0xD2, 0xFF, 0x10, 0x20, 0xFD, 0x20, 0xC0, 0x00),
	_INIT_DCS_CMD(0xE8, 0x20, 0x6F, 0x04, 0x7C, 0x7C, 0x3E, 0x04, 0xDC,
		      0xDC, 0x3E, 0x06, 0xFA, 0x26, 0x3E),
	_INIT_DCS_CMD(0x75, 0x03, 0x04),
	_INIT_DCS_CMD(0xE7, 0x3B, 0x00, 0x00, 0x7C, 0xA1, 0x8C, 0x20, 0x1A,
		      0xF0, 0xB1, 0x50, 0x00, 0x50, 0xB1, 0x50, 0xB1, 0x50,
		      0xD8, 0x00, 0x55, 0x00, 0xB1, 0x00, 0x45, 0xC9, 0x6A,
		      0xFF, 0x5A, 0xD8, 0x18, 0x88, 0x15, 0xB1, 0x01, 0x01,
		      0x77),
	_INIT_DCS_CMD(0xEA, 0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2C),
	_INIT_DCS_CMD(0x60, 0x71, 0x23, 0x00),
	_INIT_DCS_CMD(0x11),
	_INIT_DELAY_CMD(120),
	_INIT_DCS_CMD(0x29),
	_INIT_DELAY_CMD(50),
	{},
};

static const struct panel_init_cmd ws_panel_5_b_init[] = {
	_INIT_DCS_CMD(0x11),
	_INIT_DELAY_CMD(120),
	_INIT_DCS_CMD(0x29),
	_INIT_DELAY_CMD(30),
	{},
};

static const struct panel_init_cmd ws_panel_4_3_a_init[] = {
	_INIT_DCS_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x13),
	_INIT_DCS_CMD(0xEF, 0x08),
	_INIT_DCS_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x10),
	_INIT_DCS_CMD(0xC0, 0x63, 0x00),
	_INIT_DCS_CMD(0xC1, 0x0D, 0x02),
	_INIT_DCS_CMD(0xC2, 0x17, 0x08),
	_INIT_DCS_CMD(0xCC, 0x10),
	_INIT_DCS_CMD(0xB0, 0x40, 0xC9, 0x94, 0x0E, 0x10, 0x05, 0x0B, 0x09,
		      0x08, 0x26, 0x04, 0x52, 0x10, 0x69, 0x6B, 0x69),
	_INIT_DCS_CMD(0xB1, 0x40, 0xD2, 0x98, 0x0C, 0x92, 0x07, 0x09, 0x08,
		      0x07, 0x25, 0x02, 0x0E, 0x0C, 0x6E, 0x78, 0x55),
	_INIT_DCS_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x11),
	_INIT_DCS_CMD(0xB0, 0x5D),
	_INIT_DCS_CMD(0xB1, 0x4E),
	_INIT_DCS_CMD(0xB2, 0x87),
	_INIT_DCS_CMD(0xB3, 0x80),
	_INIT_DCS_CMD(0xB5, 0x4E),
	_INIT_DCS_CMD(0xB7, 0x85),
	_INIT_DCS_CMD(0xB8, 0x21),
	_INIT_DCS_CMD(0xB9, 0x10, 0x1F),
	_INIT_DCS_CMD(0xBB, 0x03),
	_INIT_DCS_CMD(0xBC, 0x00),
	_INIT_DCS_CMD(0xC1, 0x78),
	_INIT_DCS_CMD(0xC2, 0x78),
	_INIT_DCS_CMD(0xD0, 0x88),
	_INIT_DCS_CMD(0xE0, 0x00, 0x3A, 0x02),
	_INIT_DCS_CMD(0xE1, 0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0,
		      0x00, 0x40, 0x40),
	_INIT_DCS_CMD(0xE2, 0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0,
		      0x00, 0xA0, 0x00, 0xA0, 0x00),
	_INIT_DCS_CMD(0xE3, 0x00, 0x00, 0x33, 0x33),
	_INIT_DCS_CMD(0xE4, 0x44, 0x44),
	_INIT_DCS_CMD(0xE5, 0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0,
		      0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0),
	_INIT_DCS_CMD(0xE6, 0x00, 0x00, 0x33, 0x33),
	_INIT_DCS_CMD(0xE7, 0x44, 0x44),
	_INIT_DCS_CMD(0xE8, 0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0,
		      0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0),
	_INIT_DCS_CMD(0xEB, 0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00),
	_INIT_DCS_CMD(0xEC, 0x08, 0x01),
	_INIT_DCS_CMD(0xED, 0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF,
		      0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B),
	_INIT_DCS_CMD(0xEF, 0x08, 0x08, 0x08, 0x45, 0x3F, 0x54),
	_INIT_DCS_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x00),
	_INIT_DCS_CMD(0x11),
	_INIT_DELAY_CMD(120),
	_INIT_DCS_CMD(0x29),
	_INIT_DELAY_CMD(20),
	{},
};

static const struct panel_init_cmd ws_panel_4_a_init[] = {
	_INIT_DCS_CMD(0x11),
	_INIT_DELAY_CMD(200),
	_INIT_DCS_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x10),
	_INIT_DCS_CMD(0xC0, 0x63, 0x00),
	_INIT_DCS_CMD(0xC1, 0x0A, 0x02),
	_INIT_DCS_CMD(0xC2, 0x31, 0x08),
	_INIT_DCS_CMD(0xB0, 0x00, 0x11, 0x19, 0x0C, 0x10, 0x06, 0x07, 0x0A,
		      0x09, 0x22, 0x04, 0x10, 0x0E, 0x28, 0x30, 0x1C),
	_INIT_DCS_CMD(0xB1, 0x00, 0x12, 0x19, 0x0D, 0x10, 0x04, 0x06, 0x07,
		      0x08, 0x23, 0x04, 0x12, 0x11, 0x28, 0x30, 0x1C),
	_INIT_DCS_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x11),
	_INIT_DCS_CMD(0xB0, 0x4D),
	_INIT_DCS_CMD(0xB1, 0x3E),
	_INIT_DCS_CMD(0xB2, 0x07),
	_INIT_DCS_CMD(0xB3, 0x80),
	_INIT_DCS_CMD(0xB5, 0x47),
	_INIT_DCS_CMD(0xB7, 0x8A),
	_INIT_DCS_CMD(0xB8, 0x21),
	_INIT_DCS_CMD(0xC1, 0x78),
	_INIT_DCS_CMD(0xC2, 0x78),
	_INIT_DCS_CMD(0xD0, 0x88),
	_INIT_DELAY_CMD(100),
	_INIT_DCS_CMD(0xE0, 0x00, 0x00, 0x02),
	_INIT_DCS_CMD(0xE1, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
		      0x00, 0x20, 0x20),
	_INIT_DCS_CMD(0xE2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		      0x00, 0x00, 0x00, 0x00, 0x00),
	_INIT_DCS_CMD(0xE3, 0x00, 0x00, 0x33, 0x00),
	_INIT_DCS_CMD(0xE4, 0x22, 0x00),
	_INIT_DCS_CMD(0xE5, 0x04, 0x34, 0xAA, 0xAA, 0x06, 0x34, 0xAA, 0xAA,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	_INIT_DCS_CMD(0xE6, 0x00, 0x00, 0x33, 0x00),
	_INIT_DCS_CMD(0xE7, 0x22, 0x00),
	_INIT_DCS_CMD(0xE8, 0x05, 0x34, 0xAA, 0xAA, 0x07, 0x34, 0xAA, 0xAA,
		      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
	_INIT_DCS_CMD(0xEB, 0x02, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00),
	_INIT_DCS_CMD(0xED, 0xFA, 0x45, 0x0B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xB0, 0x54, 0xAF),
	_INIT_DCS_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x00),
	_INIT_DELAY_CMD(10),
	_INIT_DCS_CMD(0x29),
	{},
};

static inline struct ws_panel *panel_to_ws(struct drm_panel *panel)
{
	return container_of(panel, struct ws_panel, panel);
}

static int ws_panel_init_dcs_cmd(struct ws_panel *ts)
{
	struct mipi_dsi_device *dsi = ts->dsi;
	struct drm_panel *panel = &ts->panel;
	int i, err = 0;

	if (ts->desc->init) {
		const struct panel_init_cmd *init_cmds = ts->desc->init;

		for (i = 0; init_cmds[i].len != 0; i++) {
			const struct panel_init_cmd *cmd = &init_cmds[i];

			switch (cmd->type) {
			case DELAY_CMD:
				msleep(cmd->data[0]);
				err = 0;
				break;

			case INIT_DCS_CMD:
				err = mipi_dsi_dcs_write(
					dsi, cmd->data[0],
					cmd->len <= 1 ? NULL : &cmd->data[1],
					cmd->len - 1);
				break;

			default:
				err = -EINVAL;
			}

			if (err < 0) {
				dev_err(panel->dev,
					"failed to write command %u\n", i);
				return err;
			}
		}
	}
	return 0;
}

static void ws_panel_power_off(struct ws_panel *ctx)
{
	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		msleep(20);
	}

	regulator_disable(ctx->avdd);
	msleep(20);

	regulator_disable(ctx->iovcc);
	msleep(20);

	ctx->prepared = false;
}

static int ws_panel_prepare(struct drm_panel *panel)
{
	struct ws_panel *ctx = panel_to_ws(panel);
	int ret;

	ret = regulator_enable(ctx->iovcc);
	if (ret)
		return ret;
	msleep(20);

	ret = regulator_enable(ctx->avdd);
	if (ret) {
		regulator_disable(ctx->iovcc);
		return ret;
	}
	msleep(20);

	/* And reset it */
	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		msleep(60);
		gpiod_set_value_cansleep(ctx->reset, 0);
		msleep(60);
	}

	ctx->prepared = true;

	ret = ws_panel_init_dcs_cmd(ctx);
	if (ret < 0)
		dev_err(panel->dev, "failed to init panel: %d\n", ret);

	return 0;
}

static int ws_panel_unprepare(struct drm_panel *panel)
{
	struct ws_panel *ctx = panel_to_ws(panel);

	mipi_dsi_dcs_set_display_off(ctx->dsi);
	mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);

	ws_panel_power_off(ctx);

	return 0;
}

static const struct drm_display_mode ws_panel_10_1_e_4lane_mode = {
	.clock = 155000,
	.hdisplay = 1200,
	.hsync_start = 1200 + 28,
	.hsync_end = 1200 + 28 + 10,
	.htotal = 1200 + 28 + 10 + 30,
	.vdisplay = 1920,
	.vsync_start = 1920 + 80,
	.vsync_end = 1920 + 80 + 4,
	.vtotal = 1920 + 80 + 4 + 28,
	.width_mm = 135,
	.height_mm = 216,
};

static const struct drm_display_mode ws_panel_7_a_mode = {
	.clock = 83333,
	.hdisplay = 720,
	.hsync_start = 720 + 120,
	.hsync_end = 720 + 120 + 100,
	.htotal = 720 + 120 + 100 + 100,
	.vdisplay = 1280,
	.vsync_start = 1280 + 10,
	.vsync_end = 1280 + 10 + 10,
	.vtotal = 1280 + 10 + 10 + 10,
	.width_mm = 85,
	.height_mm = 154,
};

static const struct drm_display_mode ws_panel_7_c_mode = {
	.clock = 51700,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 10,
	.htotal = 1024 + 160 + 10 + 160,
	.vdisplay = 600,
	.vsync_start = 600 + 12,
	.vsync_end = 600 + 12 + 1,
	.vtotal = 600 + 12 + 1 + 23,
	.width_mm = 154,
	.height_mm = 85,
};

static const struct drm_display_mode ws_panel_6_25_a_mode = {
	.clock = 83000,
	.hdisplay = 720,
	.hsync_start = 720 + 20,
	.hsync_end = 720 + 20 + 20,
	.htotal = 720 + 20 + 20 + 20,
	.vdisplay = 1560,
	.vsync_start = 1560 + 200,
	.vsync_end = 1560 + 200 + 8,
	.vtotal = 1560 + 200 + 8 + 10,
	.width_mm = 62,
	.height_mm = 110,
};

static const struct drm_display_mode ws_panel_5_b_mode = {
	.clock = 70000,
	.hdisplay = 720,
	.hsync_start = 720 + 80,
	.hsync_end = 720 + 80 + 10,
	.htotal = 720 + 80 + 10 + 30,
	.vdisplay = 1280,
	.vsync_start = 1280 + 6,
	.vsync_end = 1280 + 6 + 6,
	.vtotal = 1280 + 6 + 6 + 12,
	.width_mm = 62,
	.height_mm = 110,
};

static const struct drm_display_mode ws_panel_4_3_a_mode = {
	.clock = 30000,
	.hdisplay = 480,
	.hsync_start = 480 + 42,
	.hsync_end = 480 + 42 + 12,
	.htotal = 480 + 42 + 12 + 42,
	.vdisplay = 800,
	.vsync_start = 800 + 60,
	.vsync_end = 800 + 60 + 8,
	.vtotal = 800 + 60 + 8 + 2,
	.width_mm = 56,
	.height_mm = 93,
};

static const struct drm_display_mode ws_panel_4_a_mode = {
	.clock = 27200,
	.hdisplay = 480,
	.hsync_start = 480 + 30,
	.hsync_end = 480 + 30 + 30,
	.htotal = 480 + 30 + 30 + 8,
	.vdisplay = 800,
	.vsync_start = 800 + 12,
	.vsync_end = 800 + 12 + 12,
	.vtotal = 800 + 12 + 12 + 2,
	.width_mm = 51,
	.height_mm = 86,
};

static int ws_panel_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct ws_panel *ctx = panel_to_ws(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, ctx->desc->mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "failed to add mode %ux%ux@%u\n",
			ctx->desc->mode->hdisplay, ctx->desc->mode->vdisplay,
			drm_mode_vrefresh(ctx->desc->mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	drm_connector_set_panel_orientation(connector, ctx->orientation);

	return 1;
}

static enum drm_panel_orientation
ws_panel_get_orientation(struct drm_panel *panel)
{
	struct ws_panel *ctx = panel_to_ws(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs ws_panel_funcs = {
	.prepare = ws_panel_prepare,
	.unprepare = ws_panel_unprepare,
	.get_modes = ws_panel_get_modes,
	.get_orientation = ws_panel_get_orientation,
};

static int ws_panel_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct ws_panel *ctx;
	int ret;

	dev_info(&dsi->dev, "dsi panel: %s\n",
		 (char *)of_get_property(dsi->dev.of_node, "compatible", NULL));

	ctx = devm_drm_panel_alloc(&dsi->dev, struct ws_panel, panel,
				   &ws_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;
	ctx->desc = of_device_get_match_data(&dsi->dev);

	ctx->panel.prepare_prev_first = true;

	ctx->reset = devm_gpiod_get_optional(&dsi->dev, "reset", GPIOD_ASIS);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(&dsi->dev, PTR_ERR(ctx->reset),
				     "Couldn't get our reset GPIO\n");

	ctx->iovcc = devm_regulator_get(&dsi->dev, "iovcc");
	if (IS_ERR(ctx->iovcc))
		return dev_err_probe(&dsi->dev, PTR_ERR(ctx->iovcc),
				     "Couldn't get our iovcc supply\n");

	ctx->avdd = devm_regulator_get(&dsi->dev, "avdd");
	if (IS_ERR(ctx->avdd))
		return dev_err_probe(&dsi->dev, PTR_ERR(ctx->avdd),
				     "Couldn't get our avdd supply\n");

	ret = drm_of_get_panel_orientation(dsi->dev.of_node, &ctx->orientation);
	if (ret) {
		dev_err(&dsi->dev, "%pOF: failed to get orientation: %d\n",
			dsi->dev.of_node, ret);
		return ret;
	}

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	dsi->mode_flags = ctx->desc->mode_flags;
	dsi->format = ctx->desc->format;
	dsi->lanes = ctx->desc->lanes;
	dev_info(&dsi->dev, "lanes: %d\n", dsi->lanes);

	ret = mipi_dsi_attach(dsi);
	if (ret)
		drm_panel_remove(&ctx->panel);

	return ret;
}

static void ws_panel_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct ws_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
	if (ctx->prepared)
		ws_panel_power_off(ctx);
}

static void ws_panel_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	struct ws_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (ctx->prepared)
		ws_panel_power_off(ctx);
}

static const struct ws_panel_desc ws_panel_10_1_inch_e_4lane_desc = {
	.init = ws_panel_10_1_e_4lane_init,
	.mode = &ws_panel_10_1_e_4lane_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.lanes = 4,
	.format = MIPI_DSI_FMT_RGB888,
};

static const struct ws_panel_desc ws_panel_7_inch_a_desc = {
	.init = ws_panel_7_a_init,
	.mode = &ws_panel_7_a_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
};

static const struct ws_panel_desc ws_panel_7_inch_c_desc = {
	.init = ws_panel_7_c_init,
	.mode = &ws_panel_7_c_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
};

static const struct ws_panel_desc ws_panel_6_25_inch_a_desc = {
	.init = ws_panel_6_25_a_init,
	.mode = &ws_panel_6_25_a_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
};

static const struct ws_panel_desc ws_panel_5_inch_b_desc = {
	.init = ws_panel_5_b_init,
	.mode = &ws_panel_5_b_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
};

static const struct ws_panel_desc ws_panel_4_3_inch_a_desc = {
	.init = ws_panel_4_3_a_init,
	.mode = &ws_panel_4_3_a_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
};

static const struct ws_panel_desc ws_panel_4_inch_a_desc = {
	.init = ws_panel_4_a_init,
	.mode = &ws_panel_4_a_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.lanes = 2,
	.format = MIPI_DSI_FMT_RGB888,
};

static const struct of_device_id ws_panel_of_match[] = {
	{ .compatible = "waveshare,10.1-dsi-touch-e-4lane",
	  &ws_panel_10_1_inch_e_4lane_desc },
	{ .compatible = "waveshare,7.0-dsi-touch-b", &ws_panel_7_inch_a_desc },
	{ .compatible = "waveshare,7.0-dsi-touch-c", &ws_panel_7_inch_c_desc },
	{ .compatible = "waveshare,6.25-dsi-touch-a", &ws_panel_6_25_inch_a_desc },
	{ .compatible = "waveshare,5.0-dsi-touch-a-v2", &ws_panel_5_inch_b_desc },
	{ .compatible = "waveshare,5.0-dsi-touch-b", &ws_panel_5_inch_b_desc },
	{ .compatible = "waveshare,4.3-dsi-touch-a", &ws_panel_4_3_inch_a_desc },
	{ .compatible = "waveshare,4.0-dsi-touch-a", &ws_panel_4_inch_a_desc },
	{}
};
MODULE_DEVICE_TABLE(of, ws_panel_of_match);

static struct mipi_dsi_driver ws_panel_dsi_driver = {
	.probe		= ws_panel_dsi_probe,
	.remove		= ws_panel_dsi_remove,
	.shutdown	= ws_panel_dsi_shutdown,
	.driver = {
		.name		= "waveshare-dsi",
		.of_match_table	= ws_panel_of_match,
	},
};
module_mipi_dsi_driver(ws_panel_dsi_driver);

MODULE_AUTHOR("Waveshare Team <support@waveshare.com>");
MODULE_DESCRIPTION("Waveshare DSI panel driver");
MODULE_LICENSE("GPL");
