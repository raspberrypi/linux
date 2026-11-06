/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PINCTRL_RP1_H
#define __LINUX_PINCTRL_RP1_H

#include <linux/types.h>

struct device;
struct device_node;
struct rp1_pinctrl_pio_pin;

struct rp1_pinctrl_pio_pin *
rp1_pinctrl_pio_request(struct device *consumer, struct device_node *provider_node,
			unsigned int gpio);
int rp1_pinctrl_pio_set_output(struct rp1_pinctrl_pio_pin *pin, bool enable);
int rp1_pinctrl_pio_release(struct rp1_pinctrl_pio_pin *pin);

#endif /* __LINUX_PINCTRL_RP1_H */
