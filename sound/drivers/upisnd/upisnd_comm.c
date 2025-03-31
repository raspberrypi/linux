// SPDX-License-Identifier: GPL-2.0-only
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

#include "upisnd_common.h"

DECLARE_CRC8_TABLE(upisnd_crc8_table);
enum { UPISND_CRC8_POLYNOMIAL = 0x83 };

static upisnd_msg_id_t upisnd_next_msg_id(struct upisnd_instance *instance)
{
	instance->comm.msg_id_counter = instance->comm.msg_id_counter != 127 ?
		instance->comm.msg_id_counter + 1 : 1;

	return instance->comm.msg_id_counter;
}

struct upisnd_command_task_t {
	struct list_head   list;
	upisnd_msg_id_t    msg_id;
	struct completion  done;
	int                result;
	u8                 cmd[UPISND_MAX_PACKET_LENGTH];
	u8                 response[UPISND_MAX_PACKET_LENGTH];
};

static void upisnd_command_response(struct upisnd_command_task_t *task,
				    const struct upisnd_cmd_t *response,
				    unsigned int n)
{
	memcpy(task->response, response, min_t(unsigned int, n, UPISND_MAX_PACKET_LENGTH));
	if (upisnd_cmd_matches(response, UPISND_CMD_RESULT, sizeof(struct upisnd_cmd_result_t))) {
		int response_code = ((const struct upisnd_cmd_result_t *)response)->result;

		printd("hi %p %d %d", task, response_code == -ETIME, response_code);
		task->result = response_code;
	} else {
		task->result = 0;
	}
}

static void upisnd_command_timeout(struct upisnd_command_task_t *task)
{
	struct upisnd_cmd_result_t result;

	result.cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_RESULT, sizeof(result));
	result.cmd.flags_and_msg_id = upisnd_msg_id_encode(task->msg_id, true);
	result.result = -ETIME;
	upisnd_command_response(task, &result.cmd, sizeof(result));
}

static int upisnd_i2c_send(struct i2c_client *client, const void *data, int n)
{
	u8 buffer[UPISND_MAX_PACKET_LENGTH + 1];

	memcpy(buffer, data, n);
	buffer[n] = ~crc8(upisnd_crc8_table, buffer, n, CRC8_INIT_VALUE);
	++n;

#ifdef UPISOUND_DEBUG
	printk(KERN_DEBUG "%s: Sending %d bytes:", __func__, n);
	int i;

	for (i = 0; i < n; ++i)
		printk(KERN_CONT " %02x", buffer[i]);
	printk(KERN_CONT "\n");

	u8 check = crc8(upisnd_crc8_table, buffer, n, CRC8_INIT_VALUE);

	printd("CRC CHECK: %u, good: %u", check, CRC8_GOOD_VALUE(upisnd_crc8_table));
#endif

	return i2c_master_send(client, buffer, n);
}

static void upisnd_command_send(struct upisnd_instance *instance,
				struct upisnd_command_task_t *task,
				struct upisnd_cmd_t *cmd)
{
	mutex_lock(&instance->comm.comm_lock);
	upisnd_i2c_send(instance->comm.client, cmd, upisnd_cmd_decode_length(cmd->cmd_and_size));
	mutex_unlock(&instance->comm.comm_lock);
}

static void upisnd_init_command_task(struct upisnd_command_task_t *task)
{
	memset(task, 0, sizeof(struct upisnd_command_task_t));
	init_completion(&task->done);
	task->result = -EINPROGRESS;
	task->cmd[0] = UPISND_CMD_INVALID;
}

static int upisnd_execute_void_command(struct upisnd_instance *instance, const void *cmd)
{
	mutex_lock(&instance->comm.comm_lock);
	int result = upisnd_i2c_send(instance->comm.client, cmd,
		upisnd_cmd_decode_length(*(const uint8_t *)cmd));

	mutex_unlock(&instance->comm.comm_lock);
	return result;
}

static int upisnd_execute_command(struct upisnd_instance *instance,
				  struct upisnd_command_task_t *task)
{
	struct upisnd_cmd_t *cmd = (struct upisnd_cmd_t *)task->cmd;

	mutex_lock(&instance->comm.msg_lock);
	upisnd_msg_id_t msg_id = upisnd_next_msg_id(instance);

	cmd->flags_and_msg_id = upisnd_msg_id_encode(msg_id, false);
	task->msg_id = msg_id;
	list_add_tail(&task->list, &instance->comm.msg_handlers);
	mutex_unlock(&instance->comm.msg_lock);

	upisnd_command_send(instance, task, cmd);

	printd("Before wait");
	unsigned long t = wait_for_completion_io_timeout(&task->done, msecs_to_jiffies(5000u));
	(void)t;

	mutex_lock(&instance->comm.msg_lock);
	if (!completion_done(&task->done)) {
		printe("Message %d timed out!", msg_id);
		upisnd_command_timeout(task);
	}
	list_del(&task->list);
	mutex_unlock(&instance->comm.msg_lock);
	printd("Wait complete! %lu %d", t, task->result);

	return task->result;
}

static void upisnd_handle_response(struct upisnd_instance *instance,
				   const struct upisnd_cmd_t *response,
				   unsigned int n)
{
	upisnd_msg_id_t msg_id = upisnd_msg_id_decode_id(response->flags_and_msg_id);

	printd("Response to %d", msg_id);

	struct list_head *p;
	struct upisnd_command_task_t *t;

	mutex_lock(&instance->comm.msg_lock);
	list_for_each(p, &instance->comm.msg_handlers) {
		t = list_entry(p, struct upisnd_command_task_t, list);
		if (t->msg_id == msg_id) {
			upisnd_command_response(t, response, n);
			t->msg_id = UPISND_MSG_ID_INVALID;
			complete_all(&t->done);
			break;
		}
	}
	mutex_unlock(&instance->comm.msg_lock);
}

void upisnd_handle_comm_interrupt(struct upisnd_instance *instance)
{
	printd("Comm handler");

	u8 data[UPISND_MAX_PACKET_LENGTH + 1];

	mutex_lock(&instance->comm.comm_lock);
	int i, n = 0, err;

	err = i2c_master_recv(instance->comm.client, data, 3);

	if (err == 3 && data[0] != UPISND_CMD_INVALID) {
		n = upisnd_cmd_decode_length(data[0]) + 1; // Includes CRC8 byte at the end.
		if (n > 3) {
			err = i2c_master_recv(instance->comm.client, data + 3, n - 3);
			if (err != n - 3) {
				printe("Error occurred when receiving data over I2C! (%d)", err);
				mutex_unlock(&instance->comm.comm_lock);
				return;
			}
		}
	} else {
		printe("Error occurred when receiving data over I2C! (%d)", err);
	}

#ifdef UPISOUND_DEBUG
	printk(KERN_DEBUG "upisnd_handle_comm_interrupt: Read %d bytes:", n);
	for (i = 0; i < n; ++i)
		printk(KERN_CONT " %02x", data[i]);
	printk(KERN_CONT "\n");
#endif

	mutex_unlock(&instance->comm.comm_lock);

	if (n <= 0 || data[0] == UPISND_CMD_INVALID) {
		printe("Error occurred when receiving data over I2C! (%d)", n);
		return;
	}

	u8 crc = crc8(upisnd_crc8_table, data, n, CRC8_INIT_VALUE);

	if (crc != CRC8_GOOD_VALUE(upisnd_crc8_table)) {
		printe("CRC check failed, calculated value: %02x (expected value: %02x)",
		       crc, CRC8_GOOD_VALUE(upisnd_crc8_table));
		return;
	}

	enum upisnd_cmd_type_e type = upisnd_cmd_decode_type(data[0]);

	if (upisnd_cmd_type_has_msg_id(type)) {
		upisnd_handle_response(instance, (const struct upisnd_cmd_t *)data, n);
	} else {
		switch (type) {
		case UPISND_CMD_MIDI:
			instance->comm.handler_ops->handle_midi_data(instance,
				&data[1],
				upisnd_cmd_decode_length(data[0]) - 1
			);
			break;
		case UPISND_CMD_IRQ_EVENT:
			{
				int j = 0;
				int k = 0;
				struct irq_event_t gpio_events[15];
				struct irq_event_t sound_events[15];
				unsigned int n = upisnd_cmd_decode_length(data[0]) - 1;

				for (i = 0; i < n; ++i) {
					struct irq_event_t e;

					e.num = data[i + 1] & UPISND_IRQ_NUM_MASK;
					e.high = (data[i + 1] & UPISND_ON_BIT_MASK) != 0;

					switch (e.num) {
					case UPISND_IRQ_VGND_SHORT_ALERT:
						sound_events[j++] = e;
						break;
					case UPISND_IRQ_GPIO_START...UPISND_IRQ_GPIO_END:
						gpio_events[k++] = e;
						break;
					default:
						printe("Unknown IRQ event %d", e.num);
						continue;
					}
				}

				if (j > 0) {
					instance->comm.handler_ops->handle_sound_irq_events
						(instance,
						sound_events,
						j);
				}
				if (k > 0) {
					instance->comm.handler_ops->handle_gpio_irq_events
						(instance,
						gpio_events,
						k);
				}
			}
			break;
		case UPISND_CMD_CONTROL_EVENT:
			{
				const struct upisnd_cmd_control_event_t *cmd =
					(const struct upisnd_cmd_control_event_t *)data;
				int i;
				unsigned int n = upisnd_cmd_decode_length(data[0])
					/ sizeof(uint16_t);
				struct control_event_t events[7];

				printd("Received %u control events", n);
				for (i = 0; i < n; ++i) {
					u16 e = ntohs(cmd->values[i]);

					events[i].pin = e >> 10;
					events[i].raw_value = e & 0x3ff;
					printd("%d: %d 0x%04x u=%u d=%d", i, events[i].pin,
					       events[i].raw_value, events[i].raw_value,
						events[i].raw_value);
				}

				printd("Start handling control events");
				instance->comm.handler_ops->handle_control_events(instance,
										  events,
										  n);
				printd("Done handling control events");
			}
			break;
		default:
			printe("Unknown command received (%02x)", data[0]);
			break;
		}
	}
}

int upisnd_comm_module_init(void)
{
	crc8_populate_msb(upisnd_crc8_table, UPISND_CRC8_POLYNOMIAL);
	return 0;
}

int upisnd_comm_init(struct upisnd_instance *instance,
		     struct i2c_client *client,
		     const struct upisnd_comm_handler_ops *ops)
{
	struct upisnd_comm *comm = &instance->comm;

	mutex_init(&comm->comm_lock);
	mutex_init(&comm->msg_lock);

	comm->client = client;
	comm->handler_ops = ops;

	INIT_LIST_HEAD(&comm->msg_handlers);
	comm->msg_id_counter = 0;

	return 0;
}

int upisnd_comm_send_midi(struct upisnd_instance *instance, const void *data, unsigned int n)
{
	if (n == 0 || n >= UPISND_MAX_PACKET_LENGTH)
		return -EINVAL;

	u8 buffer[UPISND_MAX_PACKET_LENGTH];

	buffer[0] = upisnd_cmd_encode(UPISND_CMD_MIDI, n + 1);
	memcpy(&buffer[1], data, n);

	mutex_lock(&instance->comm.comm_lock);
	int err = upisnd_i2c_send(instance->comm.client, buffer, n + 1);

	mutex_unlock(&instance->comm.comm_lock);

	if (err < 0)
		printe("Error occurred when sending MIDI data over I2C! (%d)", err);

	return err;
}

int upisnd_comm_commit_setup(struct upisnd_instance *instance, upisnd_setup_t setup)
{
	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_setup_t *cmd = (struct upisnd_cmd_setup_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_SETUP, sizeof(*cmd));
	cmd->setup = htonl(setup);

	int result = upisnd_execute_command(instance, &task);

	return result <= 0 ? result : 0;
}

int upisnd_comm_gpio_set(struct upisnd_instance *instance, uint8_t pin, bool high)
{
	struct upisnd_cmd_set_gpio_t cmd;

	cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_SET_GPIO, sizeof(cmd));
	cmd.on_and_pin = pin | (high ? UPISND_ON_BIT_MASK : 0x00);

	return upisnd_execute_void_command(instance, &cmd);
}

int upisnd_comm_gpio_get(struct upisnd_instance *instance, uint8_t pin)
{
	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_get_gpio_t *cmd = (struct upisnd_cmd_get_gpio_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_GET_GPIO, sizeof(*cmd));
	cmd->pin = pin;

	return upisnd_execute_command(instance, &task);
}

int upisnd_comm_gpio_set_all(struct upisnd_instance *instance,
			     const struct upisnd_all_gpio_state_t *state)
{
	struct upisnd_cmd_set_all_gpios_t cmd;

	cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_SET_ALL_GPIOS, sizeof(cmd));

	memcpy(cmd.state.state, state->state, sizeof(cmd.state));

	return upisnd_execute_void_command(instance, &cmd);
}

int upisnd_comm_gpio_get_all(struct upisnd_instance *instance,
			     struct upisnd_all_gpio_state_t *result)
{
	int err;
	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_get_all_gpios_t *cmd = (struct upisnd_cmd_get_all_gpios_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_GET_ALL_GPIOS,
						  sizeof(struct upisnd_cmd_get_all_gpios_t));

	err = upisnd_execute_command(instance, &task);
	if (err >= 0) {
		const struct upisnd_cmd_get_all_gpios_response_t *resp =
			(const struct upisnd_cmd_get_all_gpios_response_t *)task.response;

		if (upisnd_cmd_is_response(&resp->cmd) &&
		    upisnd_cmd_decode_type(resp->cmd.cmd_and_size) == UPISND_CMD_GET_ALL_GPIOS &&
		    upisnd_cmd_decode_length(resp->cmd.cmd_and_size) ==
		    sizeof(struct upisnd_cmd_get_all_gpios_response_t)) {
			memcpy(result->state, resp->state.state, sizeof(result->state));
			err = 0;
		} else {
			err = -EINVAL;
		}
	}

	return err;
}

int upisnd_comm_set_irq_types(struct upisnd_instance *instance,
			      const struct upisnd_irq_type_config_t *irq_type_config)
{
	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_set_irq_types_t *cmd = (struct upisnd_cmd_set_irq_types_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_SET_IRQ_TYPES, sizeof(*cmd));
	memcpy(&cmd->config, irq_type_config, sizeof(struct upisnd_irq_type_config_t));

	return upisnd_execute_command(instance, &task);
}

int upisnd_comm_set_irq_masks(struct upisnd_instance *instance,
			      const struct upisnd_irq_mask_config_t *irq_mask_config)
{
	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_set_irq_masks_t *cmd = (struct upisnd_cmd_set_irq_masks_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_SET_IRQ_MASKS, sizeof(*cmd));
	memcpy(&cmd->config, irq_mask_config, sizeof(struct upisnd_irq_mask_config_t));

	return upisnd_execute_command(instance, &task);
}

int upisnd_comm_set_subscription(struct upisnd_instance *instance, upisnd_irq_num_t irq, bool on)
{
	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_set_subscription_t *cmd =
		(struct upisnd_cmd_set_subscription_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_SET_SUBSCRIPTION, sizeof(*cmd));
	cmd->on_and_irq_num = (irq & UPISND_IRQ_NUM_MASK) | (on ? UPISND_ON_BIT_MASK : 0);

	return upisnd_execute_command(instance, &task);
}

int upisnd_comm_get_version(struct upisnd_instance *instance, struct upisnd_version_t *result)
{
	memset(result, 0, sizeof(*result));

	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_get_t *cmd = (struct upisnd_cmd_get_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_GET, sizeof(*cmd));
	cmd->value_id = UPISND_VALUE_VERSION_INFO;

	int err = upisnd_execute_command(instance, &task);

	if (err >= 0) {
		const struct upisnd_cmd_get_response_t *cmd =
			(const struct upisnd_cmd_get_response_t *)task.response;

		if (upisnd_cmd_decode_length(cmd->cmd_get.cmd.cmd_and_size) ==
			UPISND_CMD_GET_RESPONSE_INT32_SIZE &&
			upisnd_cmd_is_response(&cmd->cmd_get.cmd) &&
			cmd->cmd_get.value_id == UPISND_VALUE_VERSION_INFO) {
			u32 ver = (uint32_t)ntohl(cmd->value);

			result->bootloader_mode = (ver & 0x80000000) >> 31;
			result->hwrev = (ver & 0x7f000000) >> 24;
			result->major = (ver & 0x00ff0000) >> 16;
			result->minor = (ver & 0x0000ff00) >> 8;
			result->build = ver & 0x000000ff;
			err = cmd->result;
		} else {
			err = -EINVAL;
		}
	}

	return err;
}

int upisnd_comm_get_serial_number(struct upisnd_instance *instance, char result[12])
{
	memset(result, 0, 12);

	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_get_t *cmd = (struct upisnd_cmd_get_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_GET, sizeof(*cmd));
	cmd->value_id = UPISND_VALUE_SERIAL_NUMBER;

	int err = upisnd_execute_command(instance, &task);

	if (err >= 0) {
		const struct upisnd_cmd_get_response_t *cmd =
			(const struct upisnd_cmd_get_response_t *)task.response;

		if (upisnd_cmd_decode_length(cmd->cmd_get.cmd.cmd_and_size) ==
			(UPISND_CMD_GET_RESPONSE_DATA_MIN_SIZE + 11) &&
			upisnd_cmd_is_response(&cmd->cmd_get.cmd) &&
			cmd->cmd_get.value_id == UPISND_VALUE_SERIAL_NUMBER) {
			memcpy(result, cmd->data, 11);
			err = cmd->result;
		} else {
			err = -EINVAL;
		}
	}

	return err;
}

int upisnd_comm_get_element_value(struct upisnd_instance *instance, uint8_t pin, int32_t *result)
{
	if (pin >= UPISND_NUM_GPIOS)
		return -EINVAL;

	*result = 0;

	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_get_t *cmd = (struct upisnd_cmd_get_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_GET, sizeof(*cmd));
	cmd->value_id = UPISND_VALUE_ELEMENT_VAL_BASE + pin;

	int err = upisnd_execute_command(instance, &task);

	if (err >= 0) {
		const struct upisnd_cmd_get_response_t *cmd =
			(const struct upisnd_cmd_get_response_t *)task.response;

		if (upisnd_cmd_decode_length(cmd->cmd_get.cmd.cmd_and_size) ==
			UPISND_CMD_GET_RESPONSE_INT32_SIZE &&
			upisnd_cmd_is_response(&cmd->cmd_get.cmd) &&
			cmd->cmd_get.value_id == UPISND_VALUE_ELEMENT_VAL_BASE + pin) {
			*result = (int32_t)ntohl(cmd->value);
			err = cmd->result;
		} else {
			err = -EINVAL;
		}
	}

	return err;
}

int upisnd_comm_get_value(struct upisnd_instance *instance,
			  enum upisnd_value_id_t value_id,
			  int32_t *result)
{
	memset(result, 0, sizeof(*result));

	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_get_t *cmd = (struct upisnd_cmd_get_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_GET, sizeof(*cmd));
	cmd->value_id = value_id;

	int err = upisnd_execute_command(instance, &task);

	if (err >= 0) {
		const struct upisnd_cmd_get_response_t *cmd =
			(const struct upisnd_cmd_get_response_t *)task.response;

		if (upisnd_cmd_decode_length(cmd->cmd_get.cmd.cmd_and_size) ==
			UPISND_CMD_GET_RESPONSE_INT32_SIZE &&
			upisnd_cmd_is_response(&cmd->cmd_get.cmd) &&
			cmd->cmd_get.value_id == value_id) {
			*result = (int32_t)ntohl(cmd->value);
			err = cmd->result;
		} else {
			err = -EINVAL;
		}
	}

	return err;
}

int upisnd_comm_set_value(struct upisnd_instance *instance,
			  enum upisnd_value_id_t value_id,
			  int32_t value)
{
	struct upisnd_command_task_t task;

	upisnd_init_command_task(&task);

	struct upisnd_cmd_set_t *cmd = (struct upisnd_cmd_set_t *)task.cmd;

	cmd->cmd.cmd_and_size = upisnd_cmd_encode(UPISND_CMD_SET, sizeof(*cmd));
	cmd->value_id = value_id;
	cmd->value = htonl(value);

	return upisnd_execute_command(instance, &task);
}

/* vim: set ts=8 sw=8 noexpandtab: */
