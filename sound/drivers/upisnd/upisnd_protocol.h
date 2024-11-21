/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Pisound Micro Linux kernel module.
 * Copyright (C) 2017-2025  Vilniaus Blokas UAB, https://blokas.io/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef UPISND_PROTOCOL_H
#define UPISND_PROTOCOL_H

#ifndef __KERNEL__
#include <stdint.h>
#include <stdbool.h>

enum upisnd_irq_type_e {
	IRQ_TYPE_NONE         = 0,
	IRQ_TYPE_EDGE_RISING  = 1,
	IRQ_TYPE_EDGE_FALLING = 2,
	IRQ_TYPE_EDGES_BOTH   = 3,
};
#endif

enum { UPISND_ON_BIT_MASK  = 0x80 };
enum { UPISND_IRQ_NUM_MASK = 0x7f };
enum { UPISND_PIN_MASK     = 0x3f };

enum upisnd_irq_num_e {
	UPISND_IRQ_GPIO_START       = 0,
	// IRQ numbers match GPIO numbers.
	UPISND_IRQ_GPIO_END         = UPISND_PIN_MASK,

	UPISND_IRQ_VGND_SHORT_ALERT = 0x7f,
};

typedef u8 upisnd_pin_t;
typedef u8 upisnd_irq_num_t;
typedef u8 upisnd_msg_id_t;

typedef u32 upisnd_setup_t;

enum upisnd_activity_type_e {
	UPISND_ACTIVITY_TYPE_MIDI_IN  = 0,
	UPISND_ACTIVITY_TYPE_MIDI_OUT = 1,
};

enum upisnd_pin_pull_e {
	UPISND_PIN_PULL_NONE = 0,
	UPISND_PIN_PULL_UP   = 1,
	UPISND_PIN_PULL_DOWN = 2
};

enum upisnd_pin_direction_e {
	UPISND_PIN_DIR_INPUT  = 0,
	UPISND_PIN_DIR_OUTPUT = 1
};

enum upisnd_element_type_e {
	UPISND_ELEMENT_TYPE_NONE          = 0,
	UPISND_ELEMENT_TYPE_ENCODER       = 1,
	UPISND_ELEMENT_TYPE_ANALOG_IN     = 2,
	UPISND_ELEMENT_TYPE_GPIO          = 3,
	UPISND_ELEMENT_TYPE_ACTIVITY      = 4,
};

#define UPISND_DEFINE_SETUP_FIELD(shift, bits, type, name) \
	static inline type upisnd_setup_get_ ## name(upisnd_setup_t setup) \
	{ \
		return (type)(((setup) & (((1 << (bits)) - 1) << (shift))) >> (shift)); \
	} \
	static inline void upisnd_setup_set_ ## name(upisnd_setup_t *setup, type value) \
	{ \
		*(setup) = ((*(setup)) & ~(((1 << (bits)) - 1) << (shift))) | \
			 (((value) & ((1 << (bits)) - 1)) << (shift)); \
	}

UPISND_DEFINE_SETUP_FIELD(0, 3, enum upisnd_element_type_e, element_type);
UPISND_DEFINE_SETUP_FIELD(3, 8, upisnd_pin_t, pin_id);
UPISND_DEFINE_SETUP_FIELD(11, 2, enum upisnd_pin_pull_e, gpio_pull);
UPISND_DEFINE_SETUP_FIELD(13, 1, enum upisnd_pin_direction_e, gpio_dir);
UPISND_DEFINE_SETUP_FIELD(12, 1, bool, gpio_output);
UPISND_DEFINE_SETUP_FIELD(13, 8, upisnd_pin_t, encoder_pin_b_id);
UPISND_DEFINE_SETUP_FIELD(21, 2, enum upisnd_pin_pull_e, encoder_pin_b_pull);
UPISND_DEFINE_SETUP_FIELD(11, 2, enum upisnd_activity_type_e, activity_type);

#undef UPISND_DEFINE_SETUP_FIELD

struct upisnd_irq_type_config_t {
	u8 irq_types[10];
} __packed;

static inline void upisnd_irq_config_set_irq_type(struct upisnd_irq_type_config_t *cfg,
						  upisnd_pin_t pin,
						  unsigned int type)
{
	u8 *d = &cfg->irq_types[pin >> 2];
	*d = (*d & ~(0x3 << (pin & 0x3))) | ((type & 0x3) << (pin & 0x3));
}

static inline unsigned int upisnd_irq_config_get_irq_type(const struct upisnd_irq_type_config_t
							  *cfg, upisnd_pin_t pin)
{
	return (cfg->irq_types[pin >> 2] >> (pin & 0x3)) & 0x3;
}

struct upisnd_irq_mask_config_t {
	u8 irq_mask[5];
} __packed;

static inline void upisnd_irq_mask_config_set(struct upisnd_irq_mask_config_t *cfg,
					      upisnd_pin_t pin,
					      bool mask)
{
	u8 *d = &cfg->irq_mask[pin >> 3];

	if (mask)
		*d |= 1 << (pin & 0x7);
	else
		*d &= ~(1 << (pin & 0x7));
}

static inline bool upisnd_irq_mask_config_get(const struct upisnd_irq_mask_config_t *cfg,
					      upisnd_pin_t pin)
{
	return (cfg->irq_mask[pin >> 3] & (1 << (pin & 0x7))) != 0;
}

struct upisnd_all_gpio_state_t {
	u8 state[5];
} __packed;

enum { UPISND_MAX_PACKET_LENGTH   = 16   };
enum { UPISND_MSG_RESPONSE_FLAG   = 0x80 };
enum { UPISND_MSG_ID_MASK         = 0x7f };
enum { UPISND_MSG_ID_INVALID      = 0x00 };

enum upisnd_cmd_type_e {
	UPISND_CMD_MIDI               = 0x00,
	UPISND_CMD_SETUP              = 0x10,
	UPISND_CMD_SET_IRQ_TYPES      = 0x20,
	UPISND_CMD_SET_IRQ_MASKS      = 0x30,
	UPISND_CMD_SET_GPIO           = 0x40,
	UPISND_CMD_GET_GPIO           = 0x50,
	UPISND_CMD_SET_ALL_GPIOS      = 0x60,
	UPISND_CMD_GET_ALL_GPIOS      = 0x70,
	UPISND_CMD_RESULT             = 0x80,
	UPISND_CMD_SET_SUBSCRIPTION   = 0x90,
	UPISND_CMD_IRQ_EVENT          = 0xa0,
	UPISND_CMD_GET                = 0xb0,
	UPISND_CMD_SET                = 0xc0,
	UPISND_CMD_CONTROL_EVENT      = 0xd0,

	UPISND_CMD_INVALID            = 0xff
};

enum { UPISND_CMD_TYPE_MASK       = 0xf0 };
enum { UPISND_CMD_LENGTH_MASK     = 0x0f };

enum upisnd_value_id_t {
	UPISND_VALUE_INVALID          = 0x00,
	UPISND_VALUE_VERSION_INFO     = 0x01,
	UPISND_VALUE_ADC_OFFSET       = 0x02,
	UPISND_VALUE_ADC_GAIN         = 0x03,
	UPISND_VALUE_SERIAL_NUMBER    = 0x04,
	UPISND_VALUE_ELEMENT_VAL_BASE = 0x80,
};

struct upisnd_cmd_t {
	u8             cmd_and_size;
	u8             flags_and_msg_id;
} __packed;

static inline bool upisnd_cmd_is_response(const struct upisnd_cmd_t *cmd)
{
	return (cmd->flags_and_msg_id & UPISND_MSG_RESPONSE_FLAG) != 0;
}

struct upisnd_cmd_setup_t {
	struct upisnd_cmd_t cmd;
	upisnd_setup_t      setup;
} __packed;

struct upisnd_cmd_set_gpio_t {
	u8             cmd_and_size;
	u8             on_and_pin;
} __packed;

struct upisnd_cmd_get_gpio_t {
	struct upisnd_cmd_t cmd;
	u8             pin;
} __packed;

struct upisnd_cmd_set_all_gpios_t {
	u8                        cmd_and_size;
	struct upisnd_all_gpio_state_t state;
} __packed;

struct upisnd_cmd_get_all_gpios_t {
	struct upisnd_cmd_t            cmd;
} __packed;

struct upisnd_cmd_get_all_gpios_response_t {
	struct upisnd_cmd_t            cmd;
	struct upisnd_all_gpio_state_t state;
} __packed;

struct upisnd_cmd_set_irq_types_t {
	struct upisnd_cmd_t             cmd;
	struct upisnd_irq_type_config_t config;
} __packed;

struct upisnd_cmd_set_irq_masks_t {
	struct upisnd_cmd_t             cmd;
	struct upisnd_irq_mask_config_t config;
} __packed;

struct upisnd_cmd_set_subscription_t {
	struct upisnd_cmd_t cmd;
	u8             on_and_irq_num;
} __packed;

struct upisnd_cmd_result_t {
	struct upisnd_cmd_t cmd;
	s8              result;
} __packed;

struct upisnd_cmd_get_t {
	struct upisnd_cmd_t cmd;
	u8             value_id;
} __packed;

enum { UPISND_CMD_GET_RESPONSE_INT32_SIZE    = sizeof(struct upisnd_cmd_get_t) + sizeof(int8_t) +
					       sizeof(int32_t) };
enum { UPISND_CMD_GET_RESPONSE_DATA_MIN_SIZE = sizeof(struct upisnd_cmd_get_t) + sizeof(int8_t) };

struct upisnd_cmd_get_response_t {
	struct upisnd_cmd_get_t cmd_get;
	s8                  result;
	union {
		s32             value;
		u8             data[12];
	};
} __packed;

struct upisnd_cmd_set_t {
	struct upisnd_cmd_t cmd;
	u8             value_id;
	s32             value;
} __packed;

struct upisnd_cmd_control_event_t {
	u8             cmd_and_size;
	u16            values[7];
} __packed;

static inline enum upisnd_cmd_type_e upisnd_cmd_decode_type(u8 b)
{
	return (enum upisnd_cmd_type_e)(b & UPISND_CMD_TYPE_MASK);
}

static inline u8 upisnd_cmd_decode_length(u8 b)
{
	return (b & UPISND_CMD_LENGTH_MASK) + 1u;
}

static inline u8 upisnd_cmd_encode(enum upisnd_cmd_type_e type, u8 size)
{
	return (size < UPISND_MAX_PACKET_LENGTH && size > 0) ? (type | (size - 1)) : 0xff;
}

static inline u8 upisnd_msg_id_encode(upisnd_msg_id_t msg_id, bool response)
{
	return (response ? 0x80 : 0x00) | (msg_id & UPISND_MSG_ID_MASK);
}

static inline upisnd_msg_id_t upisnd_msg_id_decode_id(u8 b)
{
	return b & UPISND_MSG_ID_MASK;
}

static inline void upisnd_cmd_prepare(struct upisnd_cmd_t *cmd,
				      enum upisnd_cmd_type_e type,
				      u8 size,
				      upisnd_msg_id_t msg_id,
				      bool response)
{
	cmd->cmd_and_size = upisnd_cmd_encode(type, size);
	cmd->flags_and_msg_id = upisnd_msg_id_encode(msg_id, response);
}

#define upisnd_cmd_matches(_cmd, type, size) \
	(upisnd_cmd_decode_type((_cmd)->cmd_and_size) == (type) && \
	upisnd_cmd_decode_length((_cmd)->cmd_and_size) >= (size))

static inline bool upisnd_cmd_type_has_msg_id(enum upisnd_cmd_type_e type)
{
	switch (type) {
	default:
		return false;
	case UPISND_CMD_SETUP:
	case UPISND_CMD_SET_IRQ_TYPES:
	case UPISND_CMD_SET_IRQ_MASKS:
	case UPISND_CMD_GET_GPIO:
	case UPISND_CMD_GET_ALL_GPIOS:
	case UPISND_CMD_RESULT:
	case UPISND_CMD_GET:
	case UPISND_CMD_SET:
		return true;
	}
}

#endif // UPISND_PROTOCOL_H

/* vim: set ts=8 sw=8 noexpandtab: */
