/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 Raspberry Pi Ltd.
 * All rights reserved.
 */

#ifndef _PIO_RP1_H
#define _PIO_RP1_H

#include <uapi/misc/rp1_pio_if.h>

#define PARAM_WARNINGS_ENABLED 1

#ifdef DEBUG
#define PARAM_WARNINGS_ENABLED 1
#endif

#ifndef PARAM_WARNINGS_ENABLED
#define PARAM_WARNINGS_ENABLED 0
#endif

#define bad_params_if(client, test) \
	({ bool f = (test); if (f) pio_set_error(client, -EINVAL); \
		if (f && PARAM_WARNINGS_ENABLED) WARN_ON((test)); \
		f; })

#ifndef PARAM_ASSERTIONS_ENABLE_ALL
#define PARAM_ASSERTIONS_ENABLE_ALL 0
#endif

#ifndef PARAM_ASSERTIONS_DISABLE_ALL
#define PARAM_ASSERTIONS_DISABLE_ALL 0
#endif

#define PARAM_ASSERTIONS_ENABLED(x) \
	((PARAM_ASSERTIONS_ENABLED_ ## x || PARAM_ASSERTIONS_ENABLE_ALL) && \
	 !PARAM_ASSERTIONS_DISABLE_ALL)
#define valid_params_if(x, test) ({if (PARAM_ASSERTIONS_ENABLED(x)) WARN_ON(test); })

#include <linux/pio_instructions.h>

#define NUM_PIO_STATE_MACHINES		4
#define PIO_INSTRUCTION_COUNT		32
#define PIO_ORIGIN_ANY			((uint)(~0))
#define GPIOS_MASK			((1 << RP1_PIO_GPIO_COUNT) - 1)

#define PICO_NO_HARDWARE		0

#define pio0				pio_open_helper(0)

#define PROC_PIO_SM0_PINCTRL_OUT_BASE_BITS	0x0000001f
#define PROC_PIO_SM0_PINCTRL_OUT_BASE_LSB	0
#define PROC_PIO_SM0_PINCTRL_OUT_COUNT_BITS	0x03f00000
#define PROC_PIO_SM0_PINCTRL_OUT_COUNT_LSB	20
#define PROC_PIO_SM0_PINCTRL_SET_BASE_BITS	0x000003e0
#define PROC_PIO_SM0_PINCTRL_SET_BASE_LSB	5
#define PROC_PIO_SM0_PINCTRL_SET_COUNT_BITS	0x1c000000
#define PROC_PIO_SM0_PINCTRL_SET_COUNT_LSB	26
#define PROC_PIO_SM0_PINCTRL_IN_BASE_BITS	0x000f8000
#define PROC_PIO_SM0_PINCTRL_IN_BASE_LSB	15
#define PROC_PIO_SM0_PINCTRL_SIDESET_BASE_BITS	0x00007c00
#define PROC_PIO_SM0_PINCTRL_SIDESET_BASE_LSB	10
#define PROC_PIO_SM0_PINCTRL_SIDESET_COUNT_BITS	0xe0000000
#define PROC_PIO_SM0_PINCTRL_SIDESET_COUNT_LSB	29
#define PROC_PIO_SM0_EXECCTRL_SIDE_EN_BITS	0x40000000
#define PROC_PIO_SM0_EXECCTRL_SIDE_EN_LSB	30
#define PROC_PIO_SM0_EXECCTRL_SIDE_PINDIR_BITS	0x20000000
#define PROC_PIO_SM0_EXECCTRL_SIDE_PINDIR_LSB	29
#define PROC_PIO_SM0_CLKDIV_INT_LSB		16
#define PROC_PIO_SM0_CLKDIV_FRAC_LSB		8
#define PROC_PIO_SM0_EXECCTRL_WRAP_TOP_BITS	0x0001f000
#define PROC_PIO_SM0_EXECCTRL_WRAP_TOP_LSB	12
#define PROC_PIO_SM0_EXECCTRL_WRAP_BOTTOM_BITS	0x00000f80
#define PROC_PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB	7
#define PROC_PIO_SM0_EXECCTRL_JMP_PIN_BITS	0x1f000000
#define PROC_PIO_SM0_EXECCTRL_JMP_PIN_LSB	24
#define PROC_PIO_SM0_SHIFTCTRL_IN_SHIFTDIR_BITS	0x00040000
#define PROC_PIO_SM0_SHIFTCTRL_IN_SHIFTDIR_LSB	18
#define PROC_PIO_SM0_SHIFTCTRL_AUTOPULL_BITS	0x00020000
#define PROC_PIO_SM0_SHIFTCTRL_AUTOPULL_LSB	17
#define PROC_PIO_SM0_SHIFTCTRL_AUTOPUSH_BITS	0x00010000
#define PROC_PIO_SM0_SHIFTCTRL_AUTOPUSH_LSB	16
#define PROC_PIO_SM0_SHIFTCTRL_PUSH_THRESH_BITS	0x01f00000
#define PROC_PIO_SM0_SHIFTCTRL_PUSH_THRESH_LSB	20
#define PROC_PIO_SM0_SHIFTCTRL_OUT_SHIFTDIR_BITS	0x00080000
#define PROC_PIO_SM0_SHIFTCTRL_OUT_SHIFTDIR_LSB	19
#define PROC_PIO_SM0_SHIFTCTRL_PULL_THRESH_BITS	0x3e000000
#define PROC_PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB	25
#define PROC_PIO_SM0_SHIFTCTRL_FJOIN_TX_BITS	0x40000000
#define PROC_PIO_SM0_SHIFTCTRL_FJOIN_TX_LSB	30
#define PROC_PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS	0x80000000
#define PROC_PIO_SM0_SHIFTCTRL_FJOIN_RX_LSB	31
#define PROC_PIO_SM0_EXECCTRL_OUT_STICKY_BITS	0x00020000
#define PROC_PIO_SM0_EXECCTRL_OUT_STICKY_LSB	17
#define PROC_PIO_SM0_EXECCTRL_INLINE_OUT_EN_BITS	0x00040000
#define PROC_PIO_SM0_EXECCTRL_INLINE_OUT_EN_LSB	18
#define PROC_PIO_SM0_EXECCTRL_OUT_EN_SEL_BITS	0x00f80000
#define PROC_PIO_SM0_EXECCTRL_OUT_EN_SEL_LSB	19
#define PROC_PIO_SM0_EXECCTRL_STATUS_SEL_BITS	0x00000020
#define PROC_PIO_SM0_EXECCTRL_STATUS_SEL_LSB	5
#define PROC_PIO_SM0_EXECCTRL_STATUS_N_BITS	0x0000001f
#define PROC_PIO_SM0_EXECCTRL_STATUS_N_LSB	0

enum pio_fifo_join {
	PIO_FIFO_JOIN_NONE = 0,
	PIO_FIFO_JOIN_TX = 1,
	PIO_FIFO_JOIN_RX = 2,
};

enum pio_mov_status_type {
	STATUS_TX_LESSTHAN = 0,
	STATUS_RX_LESSTHAN = 1
};

enum pio_xfer_dir {
	PIO_DIR_TO_SM,
	PIO_DIR_FROM_SM,
	PIO_DIR_COUNT
};

enum clock_index {
	clk_sys = 5
};

typedef struct pio_program {
	const uint16_t *instructions;
	uint8_t length;
	int8_t origin; // required instruction memory origin or -1
} pio_program_t;

enum gpio_function {
	GPIO_FUNC_FSEL0 = 0,
	GPIO_FUNC_FSEL1 = 1,
	GPIO_FUNC_FSEL2 = 2,
	GPIO_FUNC_FSEL3 = 3,
	GPIO_FUNC_FSEL4 = 4,
	GPIO_FUNC_FSEL5 = 5,
	GPIO_FUNC_FSEL6 = 6,
	GPIO_FUNC_FSEL7 = 7,
	GPIO_FUNC_FSEL8 = 8,
	GPIO_FUNC_NULL = 0x1f,

	// Name a few
	GPIO_FUNC_SYS_RIO = 5,
	GPIO_FUNC_PROC_RIO = 6,
	GPIO_FUNC_PIO = 7,
};

enum gpio_irq_level {
	GPIO_IRQ_LEVEL_LOW = 0x1u,
	GPIO_IRQ_LEVEL_HIGH = 0x2u,
	GPIO_IRQ_EDGE_FALL = 0x4u,
	GPIO_IRQ_EDGE_RISE = 0x8u,
};

enum gpio_override {
	GPIO_OVERRIDE_NORMAL = 0,
	GPIO_OVERRIDE_INVERT = 1,
	GPIO_OVERRIDE_LOW = 2,
	GPIO_OVERRIDE_HIGH = 3,
};
enum gpio_slew_rate {
	GPIO_SLEW_RATE_SLOW = 0,
	GPIO_SLEW_RATE_FAST = 1
};

enum gpio_drive_strength {
	GPIO_DRIVE_STRENGTH_2MA = 0,
	GPIO_DRIVE_STRENGTH_4MA = 1,
	GPIO_DRIVE_STRENGTH_8MA = 2,
	GPIO_DRIVE_STRENGTH_12MA = 3
};

typedef rp1_pio_sm_config pio_sm_config;

typedef struct rp1_pio_client *PIO;

void pio_set_error(struct rp1_pio_client *client, int err);
int pio_get_error(const struct rp1_pio_client *client);
void pio_clear_error(struct rp1_pio_client *client);

int rp1_pio_can_add_program(struct rp1_pio_client *client, void *param);
int rp1_pio_add_program(struct rp1_pio_client *client, void *param);
int rp1_pio_remove_program(struct rp1_pio_client *client, void *param);
int rp1_pio_clear_instr_mem(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_claim(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_unclaim(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_is_claimed(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_init(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_set_config(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_exec(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_clear_fifos(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_set_clkdiv(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_set_pins(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_set_pindirs(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_set_enabled(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_restart(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_clkdiv_restart(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_enable_sync(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_put(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_get(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_set_dmactrl(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_fifo_state(struct rp1_pio_client *client, void *param);
int rp1_pio_sm_drain_tx(struct rp1_pio_client *client, void *param);
int rp1_pio_gpio_init(struct rp1_pio_client *client, void *param);
int rp1_pio_gpio_set_function(struct rp1_pio_client *client, void *param);
int rp1_pio_gpio_set_pulls(struct rp1_pio_client *client, void *param);
int rp1_pio_gpio_set_outover(struct rp1_pio_client *client, void *param);
int rp1_pio_gpio_set_inover(struct rp1_pio_client *client, void *param);
int rp1_pio_gpio_set_oeover(struct rp1_pio_client *client, void *param);
int rp1_pio_gpio_set_input_enabled(struct rp1_pio_client *client, void *param);
int rp1_pio_gpio_set_drive_strength(struct rp1_pio_client *client, void *param);

int pio_init(void);
PIO pio_open(void);
void pio_close(PIO pio);

int pio_sm_config_xfer(PIO pio, uint sm, uint dir, uint buf_size, uint buf_count);
int pio_sm_xfer_data(PIO pio, uint sm, uint dir, uint data_bytes, void *data);

static inline bool pio_can_add_program(struct rp1_pio_client *client,
				       const pio_program_t *program)
{
	struct rp1_pio_add_program_args args;

	if (bad_params_if(client, program->length > PIO_INSTRUCTION_COUNT))
		return false;
	args.origin = (program->origin == -1) ? PIO_ORIGIN_ANY : program->origin;
	args.num_instrs = program->length;

	memcpy(args.instrs, program->instructions, args.num_instrs * sizeof(args.instrs[0]));
	return rp1_pio_can_add_program(client, &args);
}

static inline bool pio_can_add_program_at_offset(struct rp1_pio_client *client,
						 const pio_program_t *program, uint offset)
{
	struct rp1_pio_add_program_args args;

	if (bad_params_if(client, program->length > PIO_INSTRUCTION_COUNT ||
			  offset >= PIO_INSTRUCTION_COUNT))
		return false;
	args.origin = offset;
	args.num_instrs = program->length;

	memcpy(args.instrs, program->instructions, args.num_instrs * sizeof(args.instrs[0]));
	return !rp1_pio_can_add_program(client, &args);
}

static inline uint pio_add_program(struct rp1_pio_client *client, const pio_program_t *program)
{
	struct rp1_pio_add_program_args args;
	int offset;

	if (bad_params_if(client, program->length > PIO_INSTRUCTION_COUNT))
		return PIO_ORIGIN_ANY;
	args.origin = (program->origin == -1) ? PIO_ORIGIN_ANY : program->origin;
	args.num_instrs = program->length;

	memcpy(args.instrs, program->instructions, args.num_instrs * sizeof(args.instrs[0]));
	offset = rp1_pio_add_program(client, &args);
	return (offset >= 0) ? offset : PIO_ORIGIN_ANY;
}

static inline int pio_add_program_at_offset(struct rp1_pio_client *client,
					    const pio_program_t *program, uint offset)
{
	struct rp1_pio_add_program_args args;

	if (bad_params_if(client, program->length > PIO_INSTRUCTION_COUNT ||
				  offset >= PIO_INSTRUCTION_COUNT))
		return -EINVAL;
	args.origin = offset;
	args.num_instrs = program->length;

	memcpy(args.instrs, program->instructions, args.num_instrs * sizeof(args.instrs[0]));
	return rp1_pio_add_program(client, &args);
}

static inline int pio_remove_program(struct rp1_pio_client *client, const pio_program_t *program,
				     uint loaded_offset)
{
	struct rp1_pio_remove_program_args args;

	args.origin = loaded_offset;
	args.num_instrs = program->length;

	return rp1_pio_remove_program(client, &args);
}

static inline int pio_clear_instruction_memory(struct rp1_pio_client *client)
{
	return rp1_pio_clear_instr_mem(client, NULL);
}

static inline int pio_sm_claim(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_claim_args args = { .mask = 1 << sm };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;

	return rp1_pio_sm_claim(client, &args);
}

static inline int pio_claim_sm_mask(struct rp1_pio_client *client, uint mask)
{
	struct rp1_pio_sm_claim_args args = { .mask = mask };

	if (bad_params_if(client, mask >= (1 << NUM_PIO_STATE_MACHINES)))
		return -EINVAL;

	return rp1_pio_sm_claim(client, &args);
}

static inline int pio_sm_unclaim(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_claim_args args = { .mask = 1 << sm };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;

	return rp1_pio_sm_claim(client, &args);
}

static inline int pio_claim_unused_sm(struct rp1_pio_client *client, bool required)
{
	struct rp1_pio_sm_claim_args args = { .mask = 0 };
	int sm;

	sm = rp1_pio_sm_claim(client, &args);
	if (sm < 0 && required)
		WARN_ON("No PIO state machines are available");
	return sm;
}

static inline bool pio_sm_is_claimed(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_claim_args args = { .mask = (1 << sm) };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return true;
	return rp1_pio_sm_is_claimed(client, &args);
}

static inline int pio_sm_init(struct rp1_pio_client *client, uint sm, uint initial_pc,
			      const pio_sm_config *config)
{
	struct rp1_pio_sm_init_args args = { .sm = sm, .initial_pc = initial_pc,
					     .config = *config };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES ||
				  initial_pc >= PIO_INSTRUCTION_COUNT))
		return -EINVAL;

	return rp1_pio_sm_init(client, &args);
}

static inline int pio_sm_set_config(struct rp1_pio_client *client, uint sm,
				    const pio_sm_config *config)
{
	struct rp1_pio_sm_init_args args = { .sm = sm, .config = *config };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;

	return rp1_pio_sm_set_config(client, &args);
}

static inline int pio_sm_exec(struct rp1_pio_client *client, uint sm, uint instr)
{
	struct rp1_pio_sm_exec_args args = { .sm = sm, .instr = instr, .blocking = false };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES || instr > (uint16_t)~0))
		return -EINVAL;

	return rp1_pio_sm_exec(client, &args);
}

static inline int pio_sm_exec_wait_blocking(struct rp1_pio_client *client, uint sm, uint instr)
{
	struct rp1_pio_sm_exec_args args = { .sm = sm, .instr = instr, .blocking = true };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES || instr > (uint16_t)~0))
		return -EINVAL;

	return rp1_pio_sm_exec(client, &args);
}

static inline int pio_sm_clear_fifos(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_clear_fifos_args args = { .sm = sm };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_clear_fifos(client, &args);
}

static inline bool pio_calculate_clkdiv_from_float(float div, uint16_t *div_int,
						   uint8_t *div_frac)
{
	if (bad_params_if(NULL, div < 1 || div > 65536))
		return false;
	*div_int = (uint16_t)div;
	if (*div_int == 0)
		*div_frac = 0;
	else
		*div_frac = (uint8_t)((div - (float)*div_int) * (1u << 8u));
	return true;
}

static inline int pio_sm_set_clkdiv_int_frac(struct rp1_pio_client *client, uint sm,
					     uint16_t div_int, uint8_t div_frac)
{
	struct rp1_pio_sm_set_clkdiv_args args = { .sm = sm, .div_int = div_int,
						   .div_frac = div_frac };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES ||
			  (div_int == 0 && div_frac != 0)))
		return -EINVAL;
	return rp1_pio_sm_set_clkdiv(client, &args);
}

static inline int pio_sm_set_clkdiv(struct rp1_pio_client *client, uint sm, float div)
{
	struct rp1_pio_sm_set_clkdiv_args args = { .sm = sm };

	if (!pio_calculate_clkdiv_from_float(div, &args.div_int, &args.div_frac))
		return -EINVAL;
	return rp1_pio_sm_set_clkdiv(client, &args);
}

static inline int pio_sm_set_pins(struct rp1_pio_client *client, uint sm, uint32_t pin_values)
{
	struct rp1_pio_sm_set_pins_args args = { .sm = sm, .values = pin_values,
						 .mask = GPIOS_MASK };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_set_pins(client, &args);
}

static inline int pio_sm_set_pins_with_mask(struct rp1_pio_client *client, uint sm,
					    uint32_t pin_values, uint32_t pin_mask)
{
	struct rp1_pio_sm_set_pins_args args = { .sm = sm, .values = pin_values,
						 .mask = pin_mask };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_set_pins(client, &args);
}

static inline int pio_sm_set_pindirs_with_mask(struct rp1_pio_client *client, uint sm,
					       uint32_t pin_dirs, uint32_t pin_mask)
{
	struct rp1_pio_sm_set_pindirs_args args = { .sm = sm, .dirs = pin_dirs,
						    .mask = pin_mask };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES ||
			      (pin_dirs & GPIOS_MASK) != pin_dirs ||
			      (pin_mask & pin_mask) != pin_mask))
		return -EINVAL;
	return rp1_pio_sm_set_pindirs(client, &args);
}

static inline int pio_sm_set_consecutive_pindirs(struct rp1_pio_client *client, uint sm,
						 uint pin_base, uint pin_count, bool is_out)
{
	uint32_t mask = ((1 << pin_count) - 1) << pin_base;
	struct rp1_pio_sm_set_pindirs_args args = { .sm = sm, .dirs = is_out ? mask : 0,
						    .mask = mask };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES ||
			      pin_base >= RP1_PIO_GPIO_COUNT ||
			      pin_count > RP1_PIO_GPIO_COUNT ||
			      (pin_base + pin_count) > RP1_PIO_GPIO_COUNT))
		return -EINVAL;
	return rp1_pio_sm_set_pindirs(client, &args);
}

static inline int pio_sm_set_enabled(struct rp1_pio_client *client, uint sm, bool enabled)
{
	struct rp1_pio_sm_set_enabled_args args = { .mask = (1 << sm), .enable = enabled };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_set_enabled(client, &args);
}

static inline int pio_set_sm_mask_enabled(struct rp1_pio_client *client, uint32_t mask,
					  bool enabled)
{
	struct rp1_pio_sm_set_enabled_args args = { .mask = mask, .enable = enabled };

	if (bad_params_if(client, mask >= (1 << NUM_PIO_STATE_MACHINES)))
		return -EINVAL;
	return rp1_pio_sm_set_enabled(client, &args);
}

static inline int pio_sm_restart(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_restart_args args = { .mask = (1 << sm) };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_restart(client, &args);
}

static inline int pio_restart_sm_mask(struct rp1_pio_client *client, uint32_t mask)
{
	struct rp1_pio_sm_restart_args args = { .mask = (uint16_t)mask };

	if (bad_params_if(client, mask >= (1 << NUM_PIO_STATE_MACHINES)))
		return -EINVAL;
	return rp1_pio_sm_restart(client, &args);
}

static inline int pio_sm_clkdiv_restart(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_restart_args args = { .mask = (1 << sm) };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_clkdiv_restart(client, &args);
}

static inline int pio_clkdiv_restart_sm_mask(struct rp1_pio_client *client, uint32_t mask)
{
	struct rp1_pio_sm_restart_args args = { .mask = (uint16_t)mask };

	if (bad_params_if(client, mask >= (1 << NUM_PIO_STATE_MACHINES)))
		return -EINVAL;
	return rp1_pio_sm_clkdiv_restart(client, &args);
}

static inline int pio_enable_sm_in_sync_mask(struct rp1_pio_client *client, uint32_t mask)
{
	struct rp1_pio_sm_enable_sync_args args = { .mask = (uint16_t)mask };

	if (bad_params_if(client, mask >= (1 << NUM_PIO_STATE_MACHINES)))
		return -EINVAL;
	return rp1_pio_sm_enable_sync(client, &args);
}

static inline int pio_sm_set_dmactrl(struct rp1_pio_client *client, uint sm, bool is_tx,
				     uint32_t ctrl)
{
	struct rp1_pio_sm_set_dmactrl_args args = { .sm = sm, .is_tx = is_tx, .ctrl = ctrl };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_set_dmactrl(client, &args);
};

static inline int pio_sm_drain_tx_fifo(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_clear_fifos_args args = { .sm = sm };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_drain_tx(client, &args);
};

static inline int pio_sm_put(struct rp1_pio_client *client, uint sm, uint32_t data)
{
	struct rp1_pio_sm_put_args args = { .sm = (uint16_t)sm, .blocking = false, .data = data };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_put(client, &args);
}

static inline int pio_sm_put_blocking(struct rp1_pio_client *client, uint sm, uint32_t data)
{
	struct rp1_pio_sm_put_args args = { .sm = (uint16_t)sm, .blocking = true, .data = data };

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	return rp1_pio_sm_put(client, &args);
}

static inline uint32_t pio_sm_get(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_get_args args = { .sm = (uint16_t)sm, .blocking = false };

	if (!bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		rp1_pio_sm_get(client, &args);
	return args.data;
}

static inline uint32_t pio_sm_get_blocking(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_get_args args = { .sm = (uint16_t)sm, .blocking = true };

	if (!bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		rp1_pio_sm_get(client, &args);
	return args.data;
}

static inline int pio_sm_is_rx_fifo_empty(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_fifo_state_args args = { .sm = sm, .tx = false };
	int ret;

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	ret = rp1_pio_sm_fifo_state(client, &args);
	if (ret == sizeof(args))
		ret = args.empty;
	return ret;
};

static inline int pio_sm_is_rx_fifo_full(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_fifo_state_args args = { .sm = sm, .tx = false };
	int ret;

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	ret = rp1_pio_sm_fifo_state(client, &args);
	if (ret == sizeof(args))
		ret = args.full;
	return ret;
};

static inline int pio_sm_rx_fifo_level(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_fifo_state_args args = { .sm = sm, .tx = false };
	int ret;

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	ret = rp1_pio_sm_fifo_state(client, &args);
	if (ret == sizeof(args))
		ret = args.level;
	return ret;
};

static inline int pio_sm_is_tx_fifo_empty(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_fifo_state_args args = { .sm = sm, .tx = true };
	int ret;

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	ret = rp1_pio_sm_fifo_state(client, &args);
	if (ret == sizeof(args))
		ret = args.empty;
	return ret;
};

static inline int pio_sm_is_tx_fifo_full(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_fifo_state_args args = { .sm = sm, .tx = true };
	int ret;

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	ret = rp1_pio_sm_fifo_state(client, &args);
	if (ret == sizeof(args))
		ret = args.full;
	return ret;
};

static inline int pio_sm_tx_fifo_level(struct rp1_pio_client *client, uint sm)
{
	struct rp1_pio_sm_fifo_state_args args = { .sm = sm, .tx = true };
	int ret;

	if (bad_params_if(client, sm >= NUM_PIO_STATE_MACHINES))
		return -EINVAL;
	ret = rp1_pio_sm_fifo_state(client, &args);
	if (ret == sizeof(args))
		ret = args.level;
	return ret;
};

static inline void sm_config_set_out_pins(pio_sm_config *c, uint out_base, uint out_count)
{
	if (bad_params_if(NULL, out_base >= RP1_PIO_GPIO_COUNT ||
				    out_count > RP1_PIO_GPIO_COUNT))
		return;

	c->pinctrl = (c->pinctrl & ~(PROC_PIO_SM0_PINCTRL_OUT_BASE_BITS |
				     PROC_PIO_SM0_PINCTRL_OUT_COUNT_BITS)) |
			(out_base << PROC_PIO_SM0_PINCTRL_OUT_BASE_LSB) |
			(out_count << PROC_PIO_SM0_PINCTRL_OUT_COUNT_LSB);
}

static inline void sm_config_set_set_pins(pio_sm_config *c, uint set_base, uint set_count)
{
	if (bad_params_if(NULL, set_base >= RP1_PIO_GPIO_COUNT ||
				    set_count > 5))
		return;

	c->pinctrl = (c->pinctrl & ~(PROC_PIO_SM0_PINCTRL_SET_BASE_BITS |
				     PROC_PIO_SM0_PINCTRL_SET_COUNT_BITS)) |
			(set_base << PROC_PIO_SM0_PINCTRL_SET_BASE_LSB) |
			(set_count << PROC_PIO_SM0_PINCTRL_SET_COUNT_LSB);
}


static inline void sm_config_set_in_pins(pio_sm_config *c, uint in_base)
{
	if (bad_params_if(NULL, in_base >= RP1_PIO_GPIO_COUNT))
		return;

	c->pinctrl = (c->pinctrl & ~PROC_PIO_SM0_PINCTRL_IN_BASE_BITS) |
			(in_base << PROC_PIO_SM0_PINCTRL_IN_BASE_LSB);
}

static inline void sm_config_set_sideset_pins(pio_sm_config *c, uint sideset_base)
{
	if (bad_params_if(NULL, sideset_base >= RP1_PIO_GPIO_COUNT))
		return;

	c->pinctrl = (c->pinctrl & ~PROC_PIO_SM0_PINCTRL_SIDESET_BASE_BITS) |
			(sideset_base << PROC_PIO_SM0_PINCTRL_SIDESET_BASE_LSB);
}

static inline void sm_config_set_sideset(pio_sm_config *c, uint bit_count, bool optional,
					 bool pindirs)
{
	if (bad_params_if(NULL, bit_count > 5 ||
				    (optional && (bit_count == 0))))
		return;
	c->pinctrl = (c->pinctrl & ~PROC_PIO_SM0_PINCTRL_SIDESET_COUNT_BITS) |
			(bit_count << PROC_PIO_SM0_PINCTRL_SIDESET_COUNT_LSB);

	c->execctrl = (c->execctrl & ~(PROC_PIO_SM0_EXECCTRL_SIDE_EN_BITS |
				       PROC_PIO_SM0_EXECCTRL_SIDE_PINDIR_BITS)) |
			(optional << PROC_PIO_SM0_EXECCTRL_SIDE_EN_LSB) |
			(pindirs << PROC_PIO_SM0_EXECCTRL_SIDE_PINDIR_LSB);
}

static inline void sm_config_set_clkdiv_int_frac(pio_sm_config *c, uint16_t div_int,
						 uint8_t div_frac)
{
	if (bad_params_if(NULL, div_int == 0 && div_frac != 0))
		return;

	c->clkdiv =
		(((uint)div_frac) << PROC_PIO_SM0_CLKDIV_FRAC_LSB) |
		(((uint)div_int) << PROC_PIO_SM0_CLKDIV_INT_LSB);
}

static inline void sm_config_set_clkdiv(pio_sm_config *c, float div)
{
	uint16_t div_int;
	uint8_t div_frac;

	pio_calculate_clkdiv_from_float(div, &div_int, &div_frac);
	sm_config_set_clkdiv_int_frac(c, div_int, div_frac);
}

static inline void sm_config_set_wrap(pio_sm_config *c, uint wrap_target, uint wrap)
{
	if (bad_params_if(NULL, wrap >= PIO_INSTRUCTION_COUNT ||
				    wrap_target >= PIO_INSTRUCTION_COUNT))
		return;

	c->execctrl = (c->execctrl & ~(PROC_PIO_SM0_EXECCTRL_WRAP_TOP_BITS |
				       PROC_PIO_SM0_EXECCTRL_WRAP_BOTTOM_BITS)) |
			(wrap_target << PROC_PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB) |
			(wrap << PROC_PIO_SM0_EXECCTRL_WRAP_TOP_LSB);
}

static inline void sm_config_set_jmp_pin(pio_sm_config *c, uint pin)
{
	if (bad_params_if(NULL, pin >= RP1_PIO_GPIO_COUNT))
		return;

	c->execctrl = (c->execctrl & ~PROC_PIO_SM0_EXECCTRL_JMP_PIN_BITS) |
		(pin << PROC_PIO_SM0_EXECCTRL_JMP_PIN_LSB);
}

static inline void sm_config_set_in_shift(pio_sm_config *c, bool shift_right, bool autopush,
					  uint push_threshold)
{
	if (bad_params_if(NULL, push_threshold > 32))
		return;

	c->shiftctrl = (c->shiftctrl &
		~(PROC_PIO_SM0_SHIFTCTRL_IN_SHIFTDIR_BITS |
		PROC_PIO_SM0_SHIFTCTRL_AUTOPUSH_BITS |
		PROC_PIO_SM0_SHIFTCTRL_PUSH_THRESH_BITS)) |
		(shift_right << PROC_PIO_SM0_SHIFTCTRL_IN_SHIFTDIR_LSB) |
		(autopush << PROC_PIO_SM0_SHIFTCTRL_AUTOPUSH_LSB) |
		((push_threshold & 0x1fu) << PROC_PIO_SM0_SHIFTCTRL_PUSH_THRESH_LSB);
}

static inline void sm_config_set_out_shift(pio_sm_config *c, bool shift_right, bool autopull,
					   uint pull_threshold)
{
	if (bad_params_if(NULL, pull_threshold > 32))
		return;

	c->shiftctrl = (c->shiftctrl &
		~(PROC_PIO_SM0_SHIFTCTRL_OUT_SHIFTDIR_BITS |
		PROC_PIO_SM0_SHIFTCTRL_AUTOPULL_BITS |
		PROC_PIO_SM0_SHIFTCTRL_PULL_THRESH_BITS)) |
		(shift_right << PROC_PIO_SM0_SHIFTCTRL_OUT_SHIFTDIR_LSB) |
		(autopull << PROC_PIO_SM0_SHIFTCTRL_AUTOPULL_LSB) |
		((pull_threshold & 0x1fu) << PROC_PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB);
}

static inline void sm_config_set_fifo_join(pio_sm_config *c, enum pio_fifo_join join)
{
	if (bad_params_if(NULL, join != PIO_FIFO_JOIN_NONE &&
				    join != PIO_FIFO_JOIN_TX &&
				    join != PIO_FIFO_JOIN_RX))
		return;

	c->shiftctrl = (c->shiftctrl & (uint)~(PROC_PIO_SM0_SHIFTCTRL_FJOIN_TX_BITS |
					       PROC_PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS)) |
		(((uint)join) << PROC_PIO_SM0_SHIFTCTRL_FJOIN_TX_LSB);
}

static inline void sm_config_set_out_special(pio_sm_config *c, bool sticky, bool has_enable_pin,
					     uint enable_pin_index)
{
	c->execctrl = (c->execctrl &
		(uint)~(PROC_PIO_SM0_EXECCTRL_OUT_STICKY_BITS |
			PROC_PIO_SM0_EXECCTRL_INLINE_OUT_EN_BITS |
		PROC_PIO_SM0_EXECCTRL_OUT_EN_SEL_BITS)) |
		(sticky << PROC_PIO_SM0_EXECCTRL_OUT_STICKY_LSB) |
		(has_enable_pin << PROC_PIO_SM0_EXECCTRL_INLINE_OUT_EN_LSB) |
		((enable_pin_index << PROC_PIO_SM0_EXECCTRL_OUT_EN_SEL_LSB) &
		 PROC_PIO_SM0_EXECCTRL_OUT_EN_SEL_BITS);
}

static inline void sm_config_set_mov_status(pio_sm_config *c, enum pio_mov_status_type status_sel,
					    uint status_n)
{
	if (bad_params_if(NULL, status_sel != STATUS_TX_LESSTHAN &&
				status_sel != STATUS_RX_LESSTHAN))
		return;

	c->execctrl = (c->execctrl
		& ~(PROC_PIO_SM0_EXECCTRL_STATUS_SEL_BITS | PROC_PIO_SM0_EXECCTRL_STATUS_N_BITS))
		| ((((uint)status_sel) << PROC_PIO_SM0_EXECCTRL_STATUS_SEL_LSB) &
		   PROC_PIO_SM0_EXECCTRL_STATUS_SEL_BITS)
		| ((status_n << PROC_PIO_SM0_EXECCTRL_STATUS_N_LSB) &
		   PROC_PIO_SM0_EXECCTRL_STATUS_N_BITS);
}

static inline pio_sm_config pio_get_default_sm_config(void)
{
	pio_sm_config c = { 0 };

	sm_config_set_clkdiv_int_frac(&c, 1, 0);
	sm_config_set_wrap(&c, 0, 31);
	sm_config_set_in_shift(&c, true, false, 32);
	sm_config_set_out_shift(&c, true, false, 32);
	return c;
}

static inline uint32_t clock_get_hz(enum clock_index clk_index)
{
	const uint32_t MHZ = 1000000;

	if (bad_params_if(NULL, clk_index != clk_sys))
		return 0;
	return 200 * MHZ;
}

static inline int pio_gpio_set_function(struct rp1_pio_client *client, uint gpio,
					enum gpio_function fn)
{
	struct rp1_gpio_set_function_args args = { .gpio = gpio, .fn = fn };

	if (bad_params_if(client, gpio >= RP1_PIO_GPIO_COUNT))
		return -EINVAL;
	return rp1_pio_gpio_set_function(client, &args);
}

static inline int pio_gpio_init(struct rp1_pio_client *client, uint gpio)
{
	struct rp1_gpio_init_args args = { .gpio = gpio };
	int ret;

	if (bad_params_if(client, gpio >= RP1_PIO_GPIO_COUNT))
		return -EINVAL;
	ret = rp1_pio_gpio_init(client, &args);
	if (ret)
		return ret;
	return pio_gpio_set_function(client, gpio, RP1_GPIO_FUNC_PIO);
}

static inline int pio_gpio_set_pulls(struct rp1_pio_client *client, uint gpio, bool up, bool down)
{
	struct rp1_gpio_set_pulls_args args = { .gpio = gpio, .up = up, .down = down };

	if (bad_params_if(client, gpio >= RP1_PIO_GPIO_COUNT))
		return -EINVAL;
	return rp1_pio_gpio_set_pulls(client, &args);
}

static inline int pio_gpio_set_outover(struct rp1_pio_client *client, uint gpio, uint value)
{
	struct rp1_gpio_set_args args = { .gpio = gpio, .value = value };

	if (bad_params_if(client, gpio >= RP1_PIO_GPIO_COUNT))
		return -EINVAL;
	return rp1_pio_gpio_set_outover(client, &args);
}

static inline int pio_gpio_set_inover(struct rp1_pio_client *client, uint gpio, uint value)
{
	struct rp1_gpio_set_args args = { .gpio = gpio, .value = value };

	if (bad_params_if(client, gpio >= RP1_PIO_GPIO_COUNT))
		return -EINVAL;
	return rp1_pio_gpio_set_inover(client, &args);
}

static inline int pio_gpio_set_oeover(struct rp1_pio_client *client, uint gpio, uint value)
{
	struct rp1_gpio_set_args args = { .gpio = gpio, .value = value };

	if (bad_params_if(client, gpio >= RP1_PIO_GPIO_COUNT))
		return -EINVAL;
	return rp1_pio_gpio_set_oeover(client, &args);
}

static inline int pio_gpio_set_input_enabled(struct rp1_pio_client *client, uint gpio,
					     bool enabled)
{
	struct rp1_gpio_set_args args = { .gpio = gpio, .value = enabled };

	if (bad_params_if(client, gpio >= RP1_PIO_GPIO_COUNT))
		return -EINVAL;
	return rp1_pio_gpio_set_input_enabled(client, &args);
}

static inline int pio_gpio_set_drive_strength(struct rp1_pio_client *client, uint gpio,
					      enum gpio_drive_strength drive)
{
	struct rp1_gpio_set_args args = { .gpio = gpio, .value = drive };

	if (bad_params_if(client, gpio >= RP1_PIO_GPIO_COUNT))
		return -EINVAL;
	return rp1_pio_gpio_set_drive_strength(client, &args);
}

static inline int pio_gpio_pull_up(struct rp1_pio_client *client, uint gpio)
{
	return pio_gpio_set_pulls(client, gpio, true, false);
}

static inline int pio_gpio_pull_down(struct rp1_pio_client *client, uint gpio)
{
	return pio_gpio_set_pulls(client, gpio, false, true);
}

static inline int pio_gpio_disable_pulls(struct rp1_pio_client *client, uint gpio)
{
	return pio_gpio_set_pulls(client, gpio, false, false);
}

#endif
