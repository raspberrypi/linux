/*
 * Copyright (C) 2015 Broadcom
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

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clk/bcm2835.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/of_irq.h>
#include <dt-bindings/clock/bcm2835-aux.h>

#define BCM2835_AUXIRQ		0x00
#define BCM2835_AUXENB		0x04

#define BCM2835_AUXIRQ_NUM_IRQS  3

#define BCM2835_AUXIRQ_UART_IRQ  0
#define BCM2835_AUXIRQ_SPI1_IRQ  1
#define BCM2835_AUXIRQ_SPI2_IRQ  2

#define BCM2835_AUXIRQ_UART_MASK 0x01
#define BCM2835_AUXIRQ_SPI1_MASK 0x02
#define BCM2835_AUXIRQ_SPI2_MASK 0x04

#define BCM2835_AUXIRQ_ALL_MASK \
	(BCM2835_AUXIRQ_UART_MASK | \
	 BCM2835_AUXIRQ_SPI1_MASK | \
	 BCM2835_AUXIRQ_SPI2_MASK)

struct auxirq_state {
	void __iomem      *status;
	u32                enables;
	struct irq_domain *domain;
	struct regmap     *local_regmap;
};

static struct auxirq_state auxirq __read_mostly;

static irqreturn_t bcm2835_auxirq_handler(int irq, void *dev_id)
{
	u32 stat = readl_relaxed(auxirq.status);
	u32 masked = stat & auxirq.enables;

	if (masked & BCM2835_AUXIRQ_UART_MASK)
		generic_handle_irq(irq_linear_revmap(auxirq.domain,
						     BCM2835_AUXIRQ_UART_IRQ));

	if (masked & BCM2835_AUXIRQ_SPI1_MASK)
		generic_handle_irq(irq_linear_revmap(auxirq.domain,
						     BCM2835_AUXIRQ_SPI1_IRQ));

	if (masked & BCM2835_AUXIRQ_SPI2_MASK)
		generic_handle_irq(irq_linear_revmap(auxirq.domain,
						     BCM2835_AUXIRQ_SPI2_IRQ));

	return (masked & BCM2835_AUXIRQ_ALL_MASK) ? IRQ_HANDLED : IRQ_NONE;
}

static int bcm2835_auxirq_xlate(struct irq_domain *d,
				 struct device_node *ctrlr,
				 const u32 *intspec, unsigned int intsize,
				 unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (WARN_ON(intsize != 1))
		return -EINVAL;

	if (WARN_ON(intspec[0] >= BCM2835_AUXIRQ_NUM_IRQS))
		return -EINVAL;

	*out_hwirq = intspec[0];
	*out_type = IRQ_TYPE_NONE;
	return 0;
}

static void bcm2835_auxirq_mask(struct irq_data *data)
{
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	auxirq.enables &= ~(1 << hwirq);
}

static void bcm2835_auxirq_unmask(struct irq_data *data)
{
	irq_hw_number_t hwirq = irqd_to_hwirq(data);

	auxirq.enables |= (1 << hwirq);
}

static struct irq_chip bcm2835_auxirq_chip = {
	.name = "bcm2835-auxirq",
	.irq_mask = bcm2835_auxirq_mask,
	.irq_unmask = bcm2835_auxirq_unmask,
};

static const struct irq_domain_ops bcm2835_auxirq_ops = {
	.xlate = bcm2835_auxirq_xlate//irq_domain_xlate_onecell
};

static int bcm2835_aux_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct clk_hw_onecell_data *onecell;
	const char *parent;
	struct clk *parent_clk;
	int parent_irq;
	struct resource *res;
	void __iomem *reg, *gate;

	parent_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(parent_clk))
		return PTR_ERR(parent_clk);
	parent = __clk_get_name(parent_clk);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	reg = devm_ioremap_resource(dev, res);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	parent_irq = irq_of_parse_and_map(node, 0);
	if (parent_irq) {
		int ret;
		int i;

		/* Manage the AUX irq as well */
		auxirq.status = reg + BCM2835_AUXIRQ;
		auxirq.domain = irq_domain_add_linear(node,
						      BCM2835_AUXIRQ_NUM_IRQS,
						      &bcm2835_auxirq_ops,
						      NULL);
		if (!auxirq.domain)
			return -ENXIO;

		for (i = 0; i < BCM2835_AUXIRQ_NUM_IRQS; i++) {
			unsigned int irq = irq_create_mapping(auxirq.domain, i);

			if (irq == 0)
				return -ENXIO;

			irq_set_chip_and_handler(irq, &bcm2835_auxirq_chip,
						 handle_level_irq);
		}

		ret = devm_request_irq(dev, parent_irq, bcm2835_auxirq_handler,
				       0, "bcm2835-auxirq", NULL);
		if (ret)
			return ret;
	}

	onecell = devm_kmalloc(dev, sizeof(*onecell) + sizeof(*onecell->hws) *
			       BCM2835_AUX_CLOCK_COUNT, GFP_KERNEL);
	if (!onecell)
		return -ENOMEM;
	onecell->num = BCM2835_AUX_CLOCK_COUNT;

	gate = reg + BCM2835_AUXENB;
	onecell->hws[BCM2835_AUX_CLOCK_UART] =
		clk_hw_register_gate(dev, "aux_uart", parent, 0, gate, 0, 0, NULL);

	onecell->hws[BCM2835_AUX_CLOCK_SPI1] =
		clk_hw_register_gate(dev, "aux_spi1", parent, 0, gate, 1, 0, NULL);

	onecell->hws[BCM2835_AUX_CLOCK_SPI2] =
		clk_hw_register_gate(dev, "aux_spi2", parent, 0, gate, 2, 0, NULL);

	return of_clk_add_hw_provider(pdev->dev.of_node, of_clk_hw_onecell_get,
				      onecell);
}

static const struct of_device_id bcm2835_aux_clk_of_match[] = {
	{ .compatible = "brcm,bcm2835-aux", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_aux_clk_of_match);

static struct platform_driver bcm2835_aux_clk_driver = {
	.driver = {
		.name = "bcm2835-aux-clk",
		.of_match_table = bcm2835_aux_clk_of_match,
	},
	.probe          = bcm2835_aux_clk_probe,
};
builtin_platform_driver(bcm2835_aux_clk_driver);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("BCM2835 auxiliary peripheral clock driver");
MODULE_LICENSE("GPL v2");
