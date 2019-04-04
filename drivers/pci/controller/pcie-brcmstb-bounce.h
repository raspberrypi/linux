/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2019 Raspberry Pi (Trading) Ltd.
 */

#ifndef _PCIE_BRCMSTB_BOUNCE_H
#define _PCIE_BRCMSTB_BOUNCE_H

#ifdef CONFIG_ARM

int brcm_pcie_bounce_init(struct device *dev, unsigned long buffer_size,
			  dma_addr_t threshold);
int brcm_pcie_bounce_uninit(struct device *dev);
int brcm_pcie_bounce_register_dev(struct device *dev);

#else

static inline int brcm_pcie_bounce_init(struct device *dev,
					unsigned long buffer_size,
					dma_addr_t threshold)
{
	return 0;
}

static inline int brcm_pcie_bounce_uninit(struct device *dev)
{
	return 0;
}

static inline int brcm_pcie_bounce_register_dev(struct device *dev)
{
	return 0;
}

#endif

#endif /* _PCIE_BRCMSTB_BOUNCE_H */
