// SPDX-License-Identifier: ISC
/*
 * Copyright (c) 2014 Broadcom Corporation
 */
#ifdef CONFIG_OF
void brcmf_of_probe(struct device *dev, enum brcmf_bus_type bus_type,
		    struct brcmf_mp_device *settings);
struct brcmf_firmware_mapping *
brcmf_of_fwnames(struct device *dev, u32 *map_count);
#else
static void brcmf_of_probe(struct device *dev, enum brcmf_bus_type bus_type,
			   struct brcmf_mp_device *settings)
{
}
static struct brcmf_firmware_mapping *
brcmf_of_fwnames(struct device *dev, u32 *map_count)
{
	return NULL;
}
#endif /* CONFIG_OF */
