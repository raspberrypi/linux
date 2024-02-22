/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _ASM_GUNYAH_H
#define _ASM_GUNYAH_H

#include <linux/irq.h>
#include <linux/irqdomain.h>

static inline int arch_gunyah_fill_irq_fwspec_params(u32 virq,
						 struct irq_fwspec *fwspec)
{
	/* Assume that Gunyah gave us an SPI or ESPI; defensively check it */
	if (WARN(virq < 32, "Unexpected virq: %d\n", virq)) {
		return -EINVAL;
	} else if (virq <= 1019) {
		fwspec->param_count = 3;
		fwspec->param[0] = 0; /* GIC_SPI */
		fwspec->param[1] = virq - 32; /* virq 32 -> SPI 0 */
		fwspec->param[2] = IRQ_TYPE_EDGE_RISING;
	} else if (WARN(virq < 4096, "Unexpected virq: %d\n", virq)) {
		return -EINVAL;
	} else if (virq < 5120) {
		fwspec->param_count = 3;
		fwspec->param[0] = 2; /* GIC_ESPI */
		fwspec->param[1] = virq - 4096; /* virq 4096 -> ESPI 0 */
		fwspec->param[2] = IRQ_TYPE_EDGE_RISING;
	} else {
		WARN(1, "Unexpected virq: %d\n", virq);
		return -EINVAL;
	}
	return 0;
}

#endif
