// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2014 Broadcom Corporation
 */
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_irq.h>

#include <defs.h>
#include "debug.h"
#include "core.h"
#include "common.h"
#include "firmware.h"
#include "of.h"

void brcmf_of_probe(struct device *dev, enum brcmf_bus_type bus_type,
		    struct brcmf_mp_device *settings)
{
	struct brcmfmac_sdio_pd *sdio = &settings->bus.sdio;
	struct device_node *root, *np = dev->of_node;
	int irq;
	u32 irqf;
	u32 val;

	/* Set board-type to the first string of the machine compatible prop */
	root = of_find_node_by_path("/");
	if (root) {
		int i, len;
		char *board_type;
		const char *tmp;

		of_property_read_string_index(root, "compatible", 0, &tmp);

		/* get rid of '/' in the compatible string to be able to find the FW */
		len = strlen(tmp) + 1;
		board_type = devm_kzalloc(dev, len, GFP_KERNEL);
		strscpy(board_type, tmp, len);
		for (i = 0; i < board_type[i]; i++) {
			if (board_type[i] == '/')
				board_type[i] = '-';
		}
		settings->board_type = board_type;

		of_node_put(root);
	}

	if (!np || bus_type != BRCMF_BUSTYPE_SDIO ||
	    !of_device_is_compatible(np, "brcm,bcm4329-fmac"))
		return;

	if (of_property_read_u32(np, "brcm,drive-strength", &val) == 0)
		sdio->drive_strength = val;

	/* make sure there are interrupts defined in the node */
	if (!of_find_property(np, "interrupts", NULL))
		return;

	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		brcmf_err("interrupt could not be mapped\n");
		return;
	}
	irqf = irqd_get_trigger_type(irq_get_irq_data(irq));

	sdio->oob_irq_supported = true;
	sdio->oob_irq_nr = irq;
	sdio->oob_irq_flags = irqf;
}

struct brcmf_firmware_mapping *
brcmf_of_fwnames(struct device *dev, u32 *fwname_count)
{
	struct device_node *np = dev->of_node;
	struct brcmf_firmware_mapping *fwnames;
	struct device_node *map_np, *fw_np;
	int of_count;
	int count = 0;

	map_np = of_get_child_by_name(np, "firmwares");
	of_count = of_get_child_count(map_np);
	if (!of_count)
		return NULL;

	fwnames = devm_kcalloc(dev, of_count,
			       sizeof(struct brcmf_firmware_mapping),
			       GFP_KERNEL);

	for_each_child_of_node(map_np, fw_np)
	{
		struct brcmf_firmware_mapping *cur = &fwnames[count];

		if (of_property_read_u32(fw_np, "chipid", &cur->chipid) ||
		    of_property_read_u32(fw_np, "revmask", &cur->revmask))
			continue;
		cur->fw_base = of_get_property(fw_np, "fw_base", NULL);
		if (cur->fw_base)
			count++;
	}

	*fwname_count = count;

	return count ? fwnames : NULL;
}
