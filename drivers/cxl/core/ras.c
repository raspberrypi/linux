// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2021 Intel Corporation. All rights reserved. */

#include <linux/aer.h>
#include <cxlmem.h>
#include <cxlpci.h>
#include "core.h"
#include "trace.h"

void cxl_handle_cor_ras(struct cxl_dev_state *cxlds, void __iomem *ras_base)
{
	void __iomem *addr;
	u32 status;

	if (!ras_base)
		return;

	addr = ras_base + CXL_RAS_CORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	if (status & CXL_RAS_CORRECTABLE_STATUS_MASK) {
		writel(status & CXL_RAS_CORRECTABLE_STATUS_MASK, addr);
		trace_cxl_aer_correctable_error(cxlds->cxlmd, status);
	}
}

/* CXL spec rev3.0 8.2.4.16.1 */
static void header_log_copy(void __iomem *ras_base, u32 *log)
{
	void __iomem *addr;
	u32 *log_addr;
	int i, log_u32_size = CXL_HEADERLOG_SIZE / sizeof(u32);

	addr = ras_base + CXL_RAS_HEADER_LOG_OFFSET;
	log_addr = log;

	for (i = 0; i < log_u32_size; i++) {
		*log_addr = readl(addr);
		log_addr++;
		addr += sizeof(u32);
	}
}

/*
 * Log the state of the RAS status registers and prepare them to log the
 * next error status. Return 1 if reset needed.
 */
bool cxl_handle_ras(struct cxl_dev_state *cxlds, void __iomem *ras_base)
{
	u32 hl[CXL_HEADERLOG_SIZE_U32];
	void __iomem *addr;
	u32 status;
	u32 fe;

	if (!ras_base)
		return false;

	addr = ras_base + CXL_RAS_UNCORRECTABLE_STATUS_OFFSET;
	status = readl(addr);
	if (!(status & CXL_RAS_UNCORRECTABLE_STATUS_MASK))
		return false;

	/* If multiple errors, log header points to first error from ctrl reg */
	if (hweight32(status) > 1) {
		void __iomem *rcc_addr =
			ras_base + CXL_RAS_CAP_CONTROL_OFFSET;

		fe = BIT(FIELD_GET(CXL_RAS_CAP_CONTROL_FE_MASK,
				   readl(rcc_addr)));
	} else {
		fe = status;
	}

	header_log_copy(ras_base, hl);
	trace_cxl_aer_uncorrectable_error(cxlds->cxlmd, status, fe, hl);
	writel(status & CXL_RAS_UNCORRECTABLE_STATUS_MASK, addr);

	return true;
}

#ifdef CONFIG_PCIEAER_CXL
static void cxl_dport_map_ras(struct cxl_dport *dport)
{
	struct cxl_register_map *map = &dport->reg_map;
	struct device *dev = dport->dport_dev;

	if (!map->component_map.ras.valid)
		dev_dbg(dev, "RAS registers not found\n");
	else if (cxl_map_component_regs(map, &dport->regs.component,
					BIT(CXL_CM_CAP_CAP_ID_RAS)))
		dev_dbg(dev, "Failed to map RAS capability.\n");
}

/**
 * cxl_dport_init_ras_reporting - Setup CXL RAS report on this dport
 * @dport: the cxl_dport that needs to be initialized
 * @host: host device for devm operations
 */
void cxl_dport_init_ras_reporting(struct cxl_dport *dport, struct device *host)
{
	dport->reg_map.host = host;
	cxl_dport_map_ras(dport);

	if (dport->rch) {
		struct pci_host_bridge *host_bridge =
			to_pci_host_bridge(dport->dport_dev);

		if (!host_bridge->native_aer)
			return;

		cxl_dport_map_rch_aer(dport);
		cxl_disable_rch_root_ints(dport);
	}
}
EXPORT_SYMBOL_NS_GPL(cxl_dport_init_ras_reporting, CXL);
#endif

void cxl_cor_error_detected(struct pci_dev *pdev)
{
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct device *dev = &cxlds->cxlmd->dev;

	scoped_guard(device, dev) {
		if (!dev->driver) {
			dev_warn(&pdev->dev,
				 "%s: memdev disabled, abort error handling\n",
				 dev_name(dev));
			return;
		}

		if (cxlds->rcd)
			cxl_handle_rdport_errors(cxlds);

		cxl_handle_cor_ras(cxlds, cxlds->regs.ras);
	}
}
EXPORT_SYMBOL_NS_GPL(cxl_cor_error_detected, CXL);

pci_ers_result_t cxl_error_detected(struct pci_dev *pdev,
				    pci_channel_state_t state)
{
	struct cxl_dev_state *cxlds = pci_get_drvdata(pdev);
	struct cxl_memdev *cxlmd = cxlds->cxlmd;
	struct device *dev = &cxlmd->dev;
	bool ue;

	scoped_guard(device, dev) {
		if (!dev->driver) {
			dev_warn(&pdev->dev,
				 "%s: memdev disabled, abort error handling\n",
				 dev_name(dev));
			return PCI_ERS_RESULT_DISCONNECT;
		}

		if (cxlds->rcd)
			cxl_handle_rdport_errors(cxlds);
		/*
		 * A frozen channel indicates an impending reset which is fatal to
		 * CXL.mem operation, and will likely crash the system. On the off
		 * chance the situation is recoverable dump the status of the RAS
		 * capability registers and bounce the active state of the memdev.
		 */
		ue = cxl_handle_ras(cxlds, cxlds->regs.ras);
	}


	switch (state) {
	case pci_channel_io_normal:
		if (ue) {
			device_release_driver(dev);
			return PCI_ERS_RESULT_NEED_RESET;
		}
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		dev_warn(&pdev->dev,
			 "%s: frozen state error detected, disable CXL.mem\n",
			 dev_name(dev));
		device_release_driver(dev);
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		dev_warn(&pdev->dev,
			 "failure state error detected, request disconnect\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}
EXPORT_SYMBOL_NS_GPL(cxl_error_detected, CXL);
