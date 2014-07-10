/*
 * Driver for Broadcom BCM2708 GPIO unit (pinctrl only)
 *
 * Copyright (C) 2012 Chris Boot, Simon Arlott, Stephen Warren
 * Copyright (C) 2014 Noralf Tronnes
 *
 * This driver is a verbatim copy of the pinctrl-bcm2835 driver, except for:
 * - changed 2835 to 2708
 * - gpio_chip and IRQ part are removed
 * - Probing function is changed.
 *
 * Because armctrl sets up the gpio irqs, we use the bcm2708_gpio driver.
 * This hack is used to be able to support both DT and non-DT builds.
 * It's not possible to set trigger type and level flags for IRQs in the DT.
 *
 * This driver is inspired by:
 * pinctrl-nomadik.c, please see original file for copyright information
 * pinctrl-tegra.c, please see original file for copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitmap.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define MODULE_NAME "pinctrl-bcm2708"
#define BCM2708_NUM_GPIOS 54
#define BCM2708_NUM_BANKS 2

#define BCM2708_PIN_BITMAP_SZ \
	DIV_ROUND_UP(BCM2708_NUM_GPIOS, sizeof(unsigned long) * 8)

/* GPIO register offsets */
#define GPFSEL0		0x0	/* Function Select */
#define GPSET0		0x1c	/* Pin Output Set */
#define GPCLR0		0x28	/* Pin Output Clear */
#define GPLEV0		0x34	/* Pin Level */
#define GPEDS0		0x40	/* Pin Event Detect Status */
#define GPREN0		0x4c	/* Pin Rising Edge Detect Enable */
#define GPFEN0		0x58	/* Pin Falling Edge Detect Enable */
#define GPHEN0		0x64	/* Pin High Detect Enable */
#define GPLEN0		0x70	/* Pin Low Detect Enable */
#define GPAREN0		0x7c	/* Pin Async Rising Edge Detect */
#define GPAFEN0		0x88	/* Pin Async Falling Edge Detect */
#define GPPUD		0x94	/* Pin Pull-up/down Enable */
#define GPPUDCLK0	0x98	/* Pin Pull-up/down Enable Clock */

#define FSEL_REG(p)		(GPFSEL0 + (((p) / 10) * 4))
#define FSEL_SHIFT(p)		(((p) % 10) * 3)
#define GPIO_REG_OFFSET(p)	((p) / 32)
#define GPIO_REG_SHIFT(p)	((p) % 32)

enum bcm2708_pinconf_param {
	/* argument: bcm2708_pinconf_pull */
	BCM2708_PINCONF_PARAM_PULL,
};

enum bcm2708_pinconf_pull {
	BCM2708_PINCONFIG_PULL_NONE,
	BCM2708_PINCONFIG_PULL_DOWN,
	BCM2708_PINCONFIG_PULL_UP,
};

#define BCM2708_PINCONF_PACK(_param_, _arg_) ((_param_) << 16 | (_arg_))
#define BCM2708_PINCONF_UNPACK_PARAM(_conf_) ((_conf_) >> 16)
#define BCM2708_PINCONF_UNPACK_ARG(_conf_) ((_conf_) & 0xffff)

struct bcm2708_gpio_irqdata {
	struct bcm2708_pinctrl *pc;
	int bank;
};

struct bcm2708_pinctrl {
	struct device *dev;
	void __iomem *base;
	int irq[BCM2708_NUM_BANKS];

	/* note: locking assumes each bank will have its own unsigned long */
	unsigned long enabled_irq_map[BCM2708_NUM_BANKS];
	unsigned int irq_type[BCM2708_NUM_GPIOS];

	struct pinctrl_dev *pctl_dev;
	struct irq_domain *irq_domain;
	struct gpio_chip gpio_chip;
	struct pinctrl_gpio_range gpio_range;

	struct bcm2708_gpio_irqdata irq_data[BCM2708_NUM_BANKS];
	spinlock_t irq_lock[BCM2708_NUM_BANKS];
};

/* pins are just named GPIO0..GPIO53 */
#define BCM2708_GPIO_PIN(a) PINCTRL_PIN(a, "gpio" #a)
static struct pinctrl_pin_desc bcm2708_gpio_pins[] = {
	BCM2708_GPIO_PIN(0),
	BCM2708_GPIO_PIN(1),
	BCM2708_GPIO_PIN(2),
	BCM2708_GPIO_PIN(3),
	BCM2708_GPIO_PIN(4),
	BCM2708_GPIO_PIN(5),
	BCM2708_GPIO_PIN(6),
	BCM2708_GPIO_PIN(7),
	BCM2708_GPIO_PIN(8),
	BCM2708_GPIO_PIN(9),
	BCM2708_GPIO_PIN(10),
	BCM2708_GPIO_PIN(11),
	BCM2708_GPIO_PIN(12),
	BCM2708_GPIO_PIN(13),
	BCM2708_GPIO_PIN(14),
	BCM2708_GPIO_PIN(15),
	BCM2708_GPIO_PIN(16),
	BCM2708_GPIO_PIN(17),
	BCM2708_GPIO_PIN(18),
	BCM2708_GPIO_PIN(19),
	BCM2708_GPIO_PIN(20),
	BCM2708_GPIO_PIN(21),
	BCM2708_GPIO_PIN(22),
	BCM2708_GPIO_PIN(23),
	BCM2708_GPIO_PIN(24),
	BCM2708_GPIO_PIN(25),
	BCM2708_GPIO_PIN(26),
	BCM2708_GPIO_PIN(27),
	BCM2708_GPIO_PIN(28),
	BCM2708_GPIO_PIN(29),
	BCM2708_GPIO_PIN(30),
	BCM2708_GPIO_PIN(31),
	BCM2708_GPIO_PIN(32),
	BCM2708_GPIO_PIN(33),
	BCM2708_GPIO_PIN(34),
	BCM2708_GPIO_PIN(35),
	BCM2708_GPIO_PIN(36),
	BCM2708_GPIO_PIN(37),
	BCM2708_GPIO_PIN(38),
	BCM2708_GPIO_PIN(39),
	BCM2708_GPIO_PIN(40),
	BCM2708_GPIO_PIN(41),
	BCM2708_GPIO_PIN(42),
	BCM2708_GPIO_PIN(43),
	BCM2708_GPIO_PIN(44),
	BCM2708_GPIO_PIN(45),
	BCM2708_GPIO_PIN(46),
	BCM2708_GPIO_PIN(47),
	BCM2708_GPIO_PIN(48),
	BCM2708_GPIO_PIN(49),
	BCM2708_GPIO_PIN(50),
	BCM2708_GPIO_PIN(51),
	BCM2708_GPIO_PIN(52),
	BCM2708_GPIO_PIN(53),
};

/* one pin per group */
static const char * const bcm2708_gpio_groups[] = {
	"gpio0",
	"gpio1",
	"gpio2",
	"gpio3",
	"gpio4",
	"gpio5",
	"gpio6",
	"gpio7",
	"gpio8",
	"gpio9",
	"gpio10",
	"gpio11",
	"gpio12",
	"gpio13",
	"gpio14",
	"gpio15",
	"gpio16",
	"gpio17",
	"gpio18",
	"gpio19",
	"gpio20",
	"gpio21",
	"gpio22",
	"gpio23",
	"gpio24",
	"gpio25",
	"gpio26",
	"gpio27",
	"gpio28",
	"gpio29",
	"gpio30",
	"gpio31",
	"gpio32",
	"gpio33",
	"gpio34",
	"gpio35",
	"gpio36",
	"gpio37",
	"gpio38",
	"gpio39",
	"gpio40",
	"gpio41",
	"gpio42",
	"gpio43",
	"gpio44",
	"gpio45",
	"gpio46",
	"gpio47",
	"gpio48",
	"gpio49",
	"gpio50",
	"gpio51",
	"gpio52",
	"gpio53",
};

enum bcm2708_fsel {
	BCM2708_FSEL_GPIO_IN = 0,
	BCM2708_FSEL_GPIO_OUT = 1,
	BCM2708_FSEL_ALT0 = 4,
	BCM2708_FSEL_ALT1 = 5,
	BCM2708_FSEL_ALT2 = 6,
	BCM2708_FSEL_ALT3 = 7,
	BCM2708_FSEL_ALT4 = 3,
	BCM2708_FSEL_ALT5 = 2,
	BCM2708_FSEL_COUNT = 8,
	BCM2708_FSEL_MASK = 0x7,
};

static const char * const bcm2708_functions[BCM2708_FSEL_COUNT] = {
	[BCM2708_FSEL_GPIO_IN] = "gpio_in",
	[BCM2708_FSEL_GPIO_OUT] = "gpio_out",
	[BCM2708_FSEL_ALT0] = "alt0",
	[BCM2708_FSEL_ALT1] = "alt1",
	[BCM2708_FSEL_ALT2] = "alt2",
	[BCM2708_FSEL_ALT3] = "alt3",
	[BCM2708_FSEL_ALT4] = "alt4",
	[BCM2708_FSEL_ALT5] = "alt5",
};

static const char * const irq_type_names[] = {
	[IRQ_TYPE_NONE] = "none",
	[IRQ_TYPE_EDGE_RISING] = "edge-rising",
	[IRQ_TYPE_EDGE_FALLING] = "edge-falling",
	[IRQ_TYPE_EDGE_BOTH] = "edge-both",
	[IRQ_TYPE_LEVEL_HIGH] = "level-high",
	[IRQ_TYPE_LEVEL_LOW] = "level-low",
};

static inline u32 bcm2708_gpio_rd(struct bcm2708_pinctrl *pc, unsigned reg)
{
	return readl(pc->base + reg);
}

static inline void bcm2708_gpio_wr(struct bcm2708_pinctrl *pc, unsigned reg,
		u32 val)
{
	writel(val, pc->base + reg);
}

static inline int bcm2708_gpio_get_bit(struct bcm2708_pinctrl *pc, unsigned reg,
		unsigned bit)
{
	reg += GPIO_REG_OFFSET(bit) * 4;
	return (bcm2708_gpio_rd(pc, reg) >> GPIO_REG_SHIFT(bit)) & 1;
}

/* note NOT a read/modify/write cycle */
static inline void bcm2708_gpio_set_bit(struct bcm2708_pinctrl *pc,
		unsigned reg, unsigned bit)
{
	reg += GPIO_REG_OFFSET(bit) * 4;
	bcm2708_gpio_wr(pc, reg, BIT(GPIO_REG_SHIFT(bit)));
}

static inline enum bcm2708_fsel bcm2708_pinctrl_fsel_get(
		struct bcm2708_pinctrl *pc, unsigned pin)
{
	u32 val = bcm2708_gpio_rd(pc, FSEL_REG(pin));
	enum bcm2708_fsel status = (val >> FSEL_SHIFT(pin)) & BCM2708_FSEL_MASK;

	dev_dbg(pc->dev, "get %08x (%u => %s)\n", val, pin,
			bcm2708_functions[status]);

	return status;
}

static inline void bcm2708_pinctrl_fsel_set(
		struct bcm2708_pinctrl *pc, unsigned pin,
		enum bcm2708_fsel fsel)
{
	u32 val = bcm2708_gpio_rd(pc, FSEL_REG(pin));
	enum bcm2708_fsel cur = (val >> FSEL_SHIFT(pin)) & BCM2708_FSEL_MASK;

	dev_dbg(pc->dev, "read %08x (%u => %s)\n", val, pin,
			bcm2708_functions[cur]);

	if (cur == fsel)
		return;

	if (cur != BCM2708_FSEL_GPIO_IN && fsel != BCM2708_FSEL_GPIO_IN) {
		/* always transition through GPIO_IN */
		val &= ~(BCM2708_FSEL_MASK << FSEL_SHIFT(pin));
		val |= BCM2708_FSEL_GPIO_IN << FSEL_SHIFT(pin);

		dev_dbg(pc->dev, "trans %08x (%u <= %s)\n", val, pin,
				bcm2708_functions[BCM2708_FSEL_GPIO_IN]);
		bcm2708_gpio_wr(pc, FSEL_REG(pin), val);
	}

	val &= ~(BCM2708_FSEL_MASK << FSEL_SHIFT(pin));
	val |= fsel << FSEL_SHIFT(pin);

	dev_dbg(pc->dev, "write %08x (%u <= %s)\n", val, pin,
			bcm2708_functions[fsel]);
	bcm2708_gpio_wr(pc, FSEL_REG(pin), val);
}

static int bcm2708_pctl_get_groups_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(bcm2708_gpio_groups);
}

static const char *bcm2708_pctl_get_group_name(struct pinctrl_dev *pctldev,
		unsigned selector)
{
	return bcm2708_gpio_groups[selector];
}

static int bcm2708_pctl_get_group_pins(struct pinctrl_dev *pctldev,
		unsigned selector,
		const unsigned **pins,
		unsigned *num_pins)
{
	*pins = &bcm2708_gpio_pins[selector].number;
	*num_pins = 1;

	return 0;
}

static void bcm2708_pctl_pin_dbg_show(struct pinctrl_dev *pctldev,
		struct seq_file *s,
		unsigned offset)
{
	struct bcm2708_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);
	enum bcm2708_fsel fsel = bcm2708_pinctrl_fsel_get(pc, offset);
	const char *fname = bcm2708_functions[fsel];
	int value = bcm2708_gpio_get_bit(pc, GPLEV0, offset);
	int irq = irq_find_mapping(pc->irq_domain, offset);

	seq_printf(s, "function %s in %s; irq %d (%s)",
		fname, value ? "hi" : "lo",
		irq, irq_type_names[pc->irq_type[offset]]);
}

static void bcm2708_pctl_dt_free_map(struct pinctrl_dev *pctldev,
		struct pinctrl_map *maps, unsigned num_maps)
{
	int i;

	for (i = 0; i < num_maps; i++)
		if (maps[i].type == PIN_MAP_TYPE_CONFIGS_PIN)
			kfree(maps[i].data.configs.configs);

	kfree(maps);
}

static int bcm2708_pctl_dt_node_to_map_func(struct bcm2708_pinctrl *pc,
		struct device_node *np, u32 pin, u32 fnum,
		struct pinctrl_map **maps)
{
	struct pinctrl_map *map = *maps;

	if (fnum >= ARRAY_SIZE(bcm2708_functions)) {
		dev_err(pc->dev, "%s: invalid brcm,function %d\n",
			of_node_full_name(np), fnum);
		return -EINVAL;
	}

	map->type = PIN_MAP_TYPE_MUX_GROUP;
	map->data.mux.group = bcm2708_gpio_groups[pin];
	map->data.mux.function = bcm2708_functions[fnum];
	(*maps)++;

	return 0;
}

static int bcm2708_pctl_dt_node_to_map_pull(struct bcm2708_pinctrl *pc,
		struct device_node *np, u32 pin, u32 pull,
		struct pinctrl_map **maps)
{
	struct pinctrl_map *map = *maps;
	unsigned long *configs;

	if (pull > 2) {
		dev_err(pc->dev, "%s: invalid brcm,pull %d\n",
			of_node_full_name(np), pull);
		return -EINVAL;
	}

	configs = kzalloc(sizeof(*configs), GFP_KERNEL);
	if (!configs)
		return -ENOMEM;
	configs[0] = BCM2708_PINCONF_PACK(BCM2708_PINCONF_PARAM_PULL, pull);

	map->type = PIN_MAP_TYPE_CONFIGS_PIN;
	map->data.configs.group_or_pin = bcm2708_gpio_pins[pin].name;
	map->data.configs.configs = configs;
	map->data.configs.num_configs = 1;
	(*maps)++;

	return 0;
}

static int bcm2708_pctl_dt_node_to_map(struct pinctrl_dev *pctldev,
		struct device_node *np,
		struct pinctrl_map **map, unsigned *num_maps)
{
	struct bcm2708_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);
	struct property *pins, *funcs, *pulls;
	int num_pins, num_funcs, num_pulls, maps_per_pin;
	struct pinctrl_map *maps, *cur_map;
	int i, err;
	u32 pin, func, pull;

	pins = of_find_property(np, "brcm,pins", NULL);
	if (!pins) {
		dev_err(pc->dev, "%s: missing brcm,pins property\n",
				of_node_full_name(np));
		return -EINVAL;
	}

	funcs = of_find_property(np, "brcm,function", NULL);
	pulls = of_find_property(np, "brcm,pull", NULL);

	if (!funcs && !pulls) {
		dev_err(pc->dev,
			"%s: neither brcm,function nor brcm,pull specified\n",
			of_node_full_name(np));
		return -EINVAL;
	}

	num_pins = pins->length / 4;
	num_funcs = funcs ? (funcs->length / 4) : 0;
	num_pulls = pulls ? (pulls->length / 4) : 0;

	if (num_funcs > 1 && num_funcs != num_pins) {
		dev_err(pc->dev,
			"%s: brcm,function must have 1 or %d entries\n",
			of_node_full_name(np), num_pins);
		return -EINVAL;
	}

	if (num_pulls > 1 && num_pulls != num_pins) {
		dev_err(pc->dev,
			"%s: brcm,pull must have 1 or %d entries\n",
			of_node_full_name(np), num_pins);
		return -EINVAL;
	}

	maps_per_pin = 0;
	if (num_funcs)
		maps_per_pin++;
	if (num_pulls)
		maps_per_pin++;
	cur_map = maps = kzalloc(num_pins * maps_per_pin * sizeof(*maps),
				GFP_KERNEL);
	if (!maps)
		return -ENOMEM;

	for (i = 0; i < num_pins; i++) {
		err = of_property_read_u32_index(np, "brcm,pins", i, &pin);
		if (err)
			goto out;
		if (pin >= ARRAY_SIZE(bcm2708_gpio_pins)) {
			dev_err(pc->dev, "%s: invalid brcm,pins value %d\n",
				of_node_full_name(np), pin);
			err = -EINVAL;
			goto out;
		}

		if (num_funcs) {
			err = of_property_read_u32_index(np, "brcm,function",
					(num_funcs > 1) ? i : 0, &func);
			if (err)
				goto out;
			err = bcm2708_pctl_dt_node_to_map_func(pc, np, pin,
							func, &cur_map);
			if (err)
				goto out;
		}
		if (num_pulls) {
			err = of_property_read_u32_index(np, "brcm,pull",
					(num_funcs > 1) ? i : 0, &pull);
			if (err)
				goto out;
			err = bcm2708_pctl_dt_node_to_map_pull(pc, np, pin,
							pull, &cur_map);
			if (err)
				goto out;
		}
	}

	*map = maps;
	*num_maps = num_pins * maps_per_pin;

	return 0;

out:
	kfree(maps);
	return err;
}

static const struct pinctrl_ops bcm2708_pctl_ops = {
	.get_groups_count = bcm2708_pctl_get_groups_count,
	.get_group_name = bcm2708_pctl_get_group_name,
	.get_group_pins = bcm2708_pctl_get_group_pins,
	.pin_dbg_show = bcm2708_pctl_pin_dbg_show,
	.dt_node_to_map = bcm2708_pctl_dt_node_to_map,
	.dt_free_map = bcm2708_pctl_dt_free_map,
};

static int bcm2708_pmx_get_functions_count(struct pinctrl_dev *pctldev)
{
	return BCM2708_FSEL_COUNT;
}

static const char *bcm2708_pmx_get_function_name(struct pinctrl_dev *pctldev,
		unsigned selector)
{
	return bcm2708_functions[selector];
}

static int bcm2708_pmx_get_function_groups(struct pinctrl_dev *pctldev,
		unsigned selector,
		const char * const **groups,
		unsigned * const num_groups)
{
	/* every pin can do every function */
	*groups = bcm2708_gpio_groups;
	*num_groups = ARRAY_SIZE(bcm2708_gpio_groups);

	return 0;
}

static int bcm2708_pmx_enable(struct pinctrl_dev *pctldev,
		unsigned func_selector,
		unsigned group_selector)
{
	struct bcm2708_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);

	bcm2708_pinctrl_fsel_set(pc, group_selector, func_selector);

	return 0;
}

static void bcm2708_pmx_disable(struct pinctrl_dev *pctldev,
		unsigned func_selector,
		unsigned group_selector)
{
	struct bcm2708_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);

	/* disable by setting to GPIO_IN */
	bcm2708_pinctrl_fsel_set(pc, group_selector, BCM2708_FSEL_GPIO_IN);
}

static void bcm2708_pmx_gpio_disable_free(struct pinctrl_dev *pctldev,
		struct pinctrl_gpio_range *range,
		unsigned offset)
{
	struct bcm2708_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);

	/* disable by setting to GPIO_IN */
	bcm2708_pinctrl_fsel_set(pc, offset, BCM2708_FSEL_GPIO_IN);
}

static int bcm2708_pmx_gpio_set_direction(struct pinctrl_dev *pctldev,
		struct pinctrl_gpio_range *range,
		unsigned offset,
		bool input)
{
	struct bcm2708_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);
	enum bcm2708_fsel fsel = input ?
		BCM2708_FSEL_GPIO_IN : BCM2708_FSEL_GPIO_OUT;

	bcm2708_pinctrl_fsel_set(pc, offset, fsel);

	return 0;
}

static const struct pinmux_ops bcm2708_pmx_ops = {
	.get_functions_count = bcm2708_pmx_get_functions_count,
	.get_function_name = bcm2708_pmx_get_function_name,
	.get_function_groups = bcm2708_pmx_get_function_groups,
	.enable = bcm2708_pmx_enable,
	.disable = bcm2708_pmx_disable,
	.gpio_disable_free = bcm2708_pmx_gpio_disable_free,
	.gpio_set_direction = bcm2708_pmx_gpio_set_direction,
};

static int bcm2708_pinconf_get(struct pinctrl_dev *pctldev,
			unsigned pin, unsigned long *config)
{
	/* No way to read back config in HW */
	return -ENOTSUPP;
}

static int bcm2708_pinconf_set(struct pinctrl_dev *pctldev,
			unsigned pin, unsigned long *configs,
			unsigned num_configs)
{
	struct bcm2708_pinctrl *pc = pinctrl_dev_get_drvdata(pctldev);
	enum bcm2708_pinconf_param param;
	u16 arg;
	u32 off, bit;
	int i;

	for (i = 0; i < num_configs; i++) {
		param = BCM2708_PINCONF_UNPACK_PARAM(configs[i]);
		arg = BCM2708_PINCONF_UNPACK_ARG(configs[i]);

		dev_dbg(pc->dev, "configure pin %u (%s) = %04X\n", pin, bcm2708_gpio_groups[pin], arg);
		if (param != BCM2708_PINCONF_PARAM_PULL)
			return -EINVAL;

		off = GPIO_REG_OFFSET(pin);
		bit = GPIO_REG_SHIFT(pin);

		bcm2708_gpio_wr(pc, GPPUD, arg & 3);
		/*
		 * Docs say to wait 150 cycles, but not of what. We assume a
		 * 1 MHz clock here, which is pretty slow...
		 */
		udelay(150);
		bcm2708_gpio_wr(pc, GPPUDCLK0 + (off * 4), BIT(bit));
		udelay(150);
		bcm2708_gpio_wr(pc, GPPUDCLK0 + (off * 4), 0);
	} /* for each config */

	return 0;
}

static const struct pinconf_ops bcm2708_pinconf_ops = {
	.pin_config_get = bcm2708_pinconf_get,
	.pin_config_set = bcm2708_pinconf_set,
};

static struct pinctrl_desc bcm2708_pinctrl_desc = {
	.name = MODULE_NAME,
	.pins = bcm2708_gpio_pins,
	.npins = ARRAY_SIZE(bcm2708_gpio_pins),
	.pctlops = &bcm2708_pctl_ops,
	.pmxops = &bcm2708_pmx_ops,
	.confops = &bcm2708_pinconf_ops,
	.owner = THIS_MODULE,
};

static struct pinctrl_gpio_range bcm2708_pinctrl_gpio_range = {
	.name = MODULE_NAME,
	.npins = BCM2708_NUM_GPIOS,
};

/* bcm2708_gpio has base=0 */
static int bcm2708_pinctrl_gpiochip_find(struct gpio_chip *gc, void *data)
{
	pr_debug("%s: base = %d\n", __func__, gc->base);
	return gc->base == 0 ? 1 : 0;
}

static int bcm2708_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct bcm2708_pinctrl *pc;
	struct gpio_chip *gc;
	struct resource iomem;
	int err;
	BUILD_BUG_ON(ARRAY_SIZE(bcm2708_gpio_pins) != BCM2708_NUM_GPIOS);
	BUILD_BUG_ON(ARRAY_SIZE(bcm2708_gpio_groups) != BCM2708_NUM_GPIOS);

	/* use gpio_chip registered by the bcm2708_gpio driver */
	gc = gpiochip_find(NULL, bcm2708_pinctrl_gpiochip_find);
	if (!gc)
		return -EPROBE_DEFER;

	gc->of_node = np;
	gc->of_gpio_n_cells = 2;
	gc->of_xlate = of_gpio_simple_xlate;

	pc = devm_kzalloc(dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;

	platform_set_drvdata(pdev, pc);
	pc->dev = dev;

	err = of_address_to_resource(np, 0, &iomem);
	if (err) {
		dev_err(dev, "could not get IO memory\n");
		return err;
	}

	pc->base = devm_ioremap_resource(dev, &iomem);
	if (IS_ERR(pc->base))
		return PTR_ERR(pc->base);

	pc->gpio_chip = *gc;

	pc->pctl_dev = pinctrl_register(&bcm2708_pinctrl_desc, dev, pc);
	if (!pc->pctl_dev)
		return -EINVAL;

	pc->gpio_range = bcm2708_pinctrl_gpio_range;
	pc->gpio_range.base = pc->gpio_chip.base;
	pc->gpio_range.gc = &pc->gpio_chip;
	pinctrl_add_gpio_range(pc->pctl_dev, &pc->gpio_range);

	return 0;
}

static int bcm2708_pinctrl_remove(struct platform_device *pdev)
{
	struct bcm2708_pinctrl *pc = platform_get_drvdata(pdev);

	pinctrl_unregister(pc->pctl_dev);
	gpiochip_remove(&pc->gpio_chip);

	return 0;
}

static struct of_device_id bcm2708_pinctrl_match[] = {
	{ .compatible = "brcm,bcm2708-pinctrl" },
	{}
};
MODULE_DEVICE_TABLE(of, bcm2708_pinctrl_match);

static struct platform_driver bcm2708_pinctrl_driver = {
	.probe = bcm2708_pinctrl_probe,
	.remove = bcm2708_pinctrl_remove,
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
		.of_match_table = bcm2708_pinctrl_match,
	},
};
module_platform_driver(bcm2708_pinctrl_driver);

MODULE_AUTHOR("Chris Boot, Simon Arlott, Stephen Warren, Noralf Tronnes");
MODULE_DESCRIPTION("BCM2708 Pin control driver");
MODULE_LICENSE("GPL");
