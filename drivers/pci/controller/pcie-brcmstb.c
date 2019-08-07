// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2009 - 2017 Broadcom */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <soc/brcmstb/memory_api.h>
#include <linux/string.h>
#include <linux/types.h>
#include "../pci.h"
#include "pcie-brcmstb-bounce.h"

/* BRCM_PCIE_CAP_REGS - Offset for the mandatory capability config regs */
#define BRCM_PCIE_CAP_REGS				0x00ac

/*
 * Broadcom Settop Box PCIe Register Offsets. The names are from
 * the chip's RDB and we use them here so that a script can correlate
 * this code and the RDB to prevent discrepancies.
 */
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1		0x0188
#define PCIE_RC_CFG_PRIV1_ID_VAL3			0x043c
#define PCIE_RC_DL_MDIO_ADDR				0x1100
#define PCIE_RC_DL_MDIO_WR_DATA				0x1104
#define PCIE_RC_DL_MDIO_RD_DATA				0x1108
#define PCIE_MISC_MISC_CTRL				0x4008
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO		0x400c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI		0x4010
#define PCIE_MISC_RC_BAR1_CONFIG_LO			0x402c
#define PCIE_MISC_RC_BAR2_CONFIG_LO			0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI			0x4038
#define PCIE_MISC_RC_BAR3_CONFIG_LO			0x403c
#define PCIE_MISC_MSI_BAR_CONFIG_LO			0x4044
#define PCIE_MISC_MSI_BAR_CONFIG_HI			0x4048
#define PCIE_MISC_MSI_DATA_CONFIG			0x404c
#define PCIE_MISC_EOI_CTRL				0x4060
#define PCIE_MISC_PCIE_CTRL				0x4064
#define PCIE_MISC_PCIE_STATUS				0x4068
#define PCIE_MISC_REVISION				0x406c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT	0x4070
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI		0x4080
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI		0x4084
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG			0x4204
#define PCIE_INTR2_CPU_BASE				0x4300
#define PCIE_MSI_INTR2_BASE				0x4500

/*
 * Broadcom Settop Box PCIe Register Field shift and mask info. The
 * names are from the chip's RDB and we use them here so that a script
 * can correlate this code and the RDB to prevent discrepancies.
 */
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_MASK	0xc
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1_ENDIAN_MODE_BAR2_SHIFT	0x2
#define PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_MASK		0xffffff
#define PCIE_RC_CFG_PRIV1_ID_VAL3_CLASS_CODE_SHIFT		0x0
#define PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_MASK			0x1000
#define PCIE_MISC_MISC_CTRL_SCB_ACCESS_EN_SHIFT			0xc
#define PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_MASK		0x2000
#define PCIE_MISC_MISC_CTRL_CFG_READ_UR_MODE_SHIFT		0xd
#define PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_MASK			0x300000
#define PCIE_MISC_MISC_CTRL_MAX_BURST_SIZE_SHIFT		0x14
#define PCIE_MISC_MISC_CTRL_SCB0_SIZE_MASK			0xf8000000
#define PCIE_MISC_MISC_CTRL_SCB0_SIZE_SHIFT			0x1b
#define PCIE_MISC_MISC_CTRL_SCB1_SIZE_MASK			0x7c00000
#define PCIE_MISC_MISC_CTRL_SCB1_SIZE_SHIFT			0x16
#define PCIE_MISC_MISC_CTRL_SCB2_SIZE_MASK			0x1f
#define PCIE_MISC_MISC_CTRL_SCB2_SIZE_SHIFT			0x0
#define PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_MASK			0x1f
#define PCIE_MISC_RC_BAR1_CONFIG_LO_SIZE_SHIFT			0x0
#define PCIE_MISC_RC_BAR2_CONFIG_LO_SIZE_MASK			0x1f
#define PCIE_MISC_RC_BAR2_CONFIG_LO_SIZE_SHIFT			0x0
#define PCIE_MISC_RC_BAR3_CONFIG_LO_SIZE_MASK			0x1f
#define PCIE_MISC_RC_BAR3_CONFIG_LO_SIZE_SHIFT			0x0
#define PCIE_MISC_PCIE_CTRL_PCIE_PERSTB_MASK			0x4
#define PCIE_MISC_PCIE_CTRL_PCIE_PERSTB_SHIFT			0x2
#define PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_MASK		0x1
#define PCIE_MISC_PCIE_CTRL_PCIE_L23_REQUEST_SHIFT		0x0
#define PCIE_MISC_PCIE_STATUS_PCIE_PORT_MASK			0x80
#define PCIE_MISC_PCIE_STATUS_PCIE_PORT_SHIFT			0x7
#define PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_MASK		0x20
#define PCIE_MISC_PCIE_STATUS_PCIE_DL_ACTIVE_SHIFT		0x5
#define PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_MASK		0x10
#define PCIE_MISC_PCIE_STATUS_PCIE_PHYLINKUP_SHIFT		0x4
#define PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_MASK		0x40
#define PCIE_MISC_PCIE_STATUS_PCIE_LINK_IN_L23_SHIFT		0x6
#define PCIE_MISC_REVISION_MAJMIN_MASK				0xffff
#define PCIE_MISC_REVISION_MAJMIN_SHIFT				0
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_MASK	0xfff00000
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_LIMIT_SHIFT	0x14
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_MASK	0xfff0
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_BASE_SHIFT	0x4
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_NUM_MASK_BITS	0xc
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_MASK		0xff
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI_BASE_SHIFT	0x0
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_MASK	0xff
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI_LIMIT_SHIFT	0x0
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_MASK	0x2
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_CLKREQ_DEBUG_ENABLE_SHIFT 0x1
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_MASK		0x08000000
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG_SERDES_IDDQ_SHIFT	0x1b
#define PCIE_RGR1_SW_INIT_1_PERST_MASK				0x1
#define PCIE_RGR1_SW_INIT_1_PERST_SHIFT				0x0

#define BRCM_NUM_PCIE_OUT_WINS		0x4
#define BRCM_MAX_SCB			0x4
#define BRCM_INT_PCI_MSI_NR		32
#define BRCM_PCIE_HW_REV_33		0x0303

#define BRCM_MSI_TARGET_ADDR_LT_4GB	0x0fffffffcULL
#define BRCM_MSI_TARGET_ADDR_GT_4GB	0xffffffffcULL

#define BURST_SIZE_128			0
#define BURST_SIZE_256			1
#define BURST_SIZE_512			2

/* Offsets from PCIE_INTR2_CPU_BASE */
#define STATUS				0x0
#define SET				0x4
#define CLR				0x8
#define MASK_STATUS			0xc
#define MASK_SET			0x10
#define MASK_CLR			0x14

#define PCIE_BUSNUM_SHIFT		20
#define PCIE_SLOT_SHIFT			15
#define PCIE_FUNC_SHIFT			12

#if defined(__BIG_ENDIAN)
#define	DATA_ENDIAN			2	/* PCIe->DDR inbound traffic */
#define MMIO_ENDIAN			2	/* CPU->PCIe outbound traffic */
#else
#define	DATA_ENDIAN			0
#define MMIO_ENDIAN			0
#endif

#define MDIO_PORT0			0x0
#define MDIO_DATA_MASK			0x7fffffff
#define MDIO_DATA_SHIFT			0x0
#define MDIO_PORT_MASK			0xf0000
#define MDIO_PORT_SHIFT			0x16
#define MDIO_REGAD_MASK			0xffff
#define MDIO_REGAD_SHIFT		0x0
#define MDIO_CMD_MASK			0xfff00000
#define MDIO_CMD_SHIFT			0x14
#define MDIO_CMD_READ			0x1
#define MDIO_CMD_WRITE			0x0
#define MDIO_DATA_DONE_MASK		0x80000000
#define MDIO_RD_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 1 : 0)
#define MDIO_WT_DONE(x)			(((x) & MDIO_DATA_DONE_MASK) ? 0 : 1)
#define SSC_REGS_ADDR			0x1100
#define SET_ADDR_OFFSET			0x1f
#define SSC_CNTL_OFFSET			0x2
#define SSC_CNTL_OVRD_EN_MASK		0x8000
#define SSC_CNTL_OVRD_EN_SHIFT		0xf
#define SSC_CNTL_OVRD_VAL_MASK		0x4000
#define SSC_CNTL_OVRD_VAL_SHIFT		0xe
#define SSC_STATUS_OFFSET		0x1
#define SSC_STATUS_SSC_MASK		0x400
#define SSC_STATUS_SSC_SHIFT		0xa
#define SSC_STATUS_PLL_LOCK_MASK	0x800
#define SSC_STATUS_PLL_LOCK_SHIFT	0xb

#define IDX_ADDR(pcie)	\
	((pcie)->reg_offsets[EXT_CFG_INDEX])
#define DATA_ADDR(pcie)	\
	((pcie)->reg_offsets[EXT_CFG_DATA])
#define PCIE_RGR1_SW_INIT_1(pcie) \
	((pcie)->reg_offsets[RGR1_SW_INIT_1])

enum {
	RGR1_SW_INIT_1,
	EXT_CFG_INDEX,
	EXT_CFG_DATA,
};

enum {
	RGR1_SW_INIT_1_INIT_MASK,
	RGR1_SW_INIT_1_INIT_SHIFT,
	RGR1_SW_INIT_1_PERST_MASK,
	RGR1_SW_INIT_1_PERST_SHIFT,
};

enum pcie_type {
	BCM7425,
	BCM7435,
	GENERIC,
	BCM7278,
};

struct brcm_window {
	dma_addr_t pcie_addr;
	phys_addr_t cpu_addr;
	dma_addr_t size;
};

struct brcm_msi {
	struct device		*dev;
	void __iomem		*base;
	struct device_node	*dn;
	struct irq_domain	*msi_domain;
	struct irq_domain	*inner_domain;
	struct mutex		lock; /* guards the alloc/free operations */
	u64			target_addr;
	int			irq;

	/* intr_base is the base pointer for interrupt status/set/clr regs */
	void __iomem		*intr_base;

	/* intr_legacy_mask indicates how many bits are MSI interrupts */
	u32			intr_legacy_mask;

	/*
	 * intr_legacy_offset indicates bit position of MSI_01. It is
	 * to map the register bit position to a hwirq that starts at 0.
	 */
	u32			intr_legacy_offset;

	/* used indicates which MSI interrupts have been alloc'd */
	unsigned long		used;
	unsigned int		rev;
};

/* Internal PCIe Host Controller Information.*/
struct brcm_pcie {
	struct device		*dev;
	void __iomem		*base;
	struct list_head	resources;
	int			irq;
	struct clk		*clk;
	struct pci_bus		*root_bus;
	struct device_node	*dn;
	int			id;
	bool			suspended;
	int			num_out_wins;
	bool			ssc;
	int			gen;
	u64			msi_target_addr;
	struct brcm_window	out_wins[BRCM_NUM_PCIE_OUT_WINS];
	struct brcm_msi		*msi;
	bool			msi_internal;
	unsigned int		rev;
	const int		*reg_offsets;
	const int		*reg_field_info;
	u32			max_burst_size;
	enum pcie_type		type;
};

struct pcie_cfg_data {
	const int		*reg_field_info;
	const int		*offsets;
	const u32		max_burst_size;
	const enum pcie_type	type;
};

static const int pcie_reg_field_info[] = {
	[RGR1_SW_INIT_1_INIT_MASK] = 0x2,
	[RGR1_SW_INIT_1_INIT_SHIFT] = 0x1,
};

static const int pcie_reg_field_info_bcm7278[] = {
	[RGR1_SW_INIT_1_INIT_MASK] = 0x1,
	[RGR1_SW_INIT_1_INIT_SHIFT] = 0x0,
};

static const int pcie_offset_bcm7425[] = {
	[RGR1_SW_INIT_1] = 0x8010,
	[EXT_CFG_INDEX]  = 0x8300,
	[EXT_CFG_DATA]   = 0x8304,
};

static const struct pcie_cfg_data bcm7425_cfg = {
	.reg_field_info	= pcie_reg_field_info,
	.offsets	= pcie_offset_bcm7425,
	.max_burst_size	= BURST_SIZE_256,
	.type		= BCM7425,
};

static const int pcie_offsets[] = {
	[RGR1_SW_INIT_1] = 0x9210,
	[EXT_CFG_INDEX]  = 0x9000,
	[EXT_CFG_DATA]   = 0x8000,
};

static const struct pcie_cfg_data bcm7435_cfg = {
	.reg_field_info	= pcie_reg_field_info,
	.offsets	= pcie_offsets,
	.max_burst_size	= BURST_SIZE_256,
	.type		= BCM7435,
};

static const struct pcie_cfg_data generic_cfg = {
	.reg_field_info	= pcie_reg_field_info,
	.offsets	= pcie_offsets,
	.max_burst_size	= BURST_SIZE_128, // before BURST_SIZE_512
	.type		= GENERIC,
};

static const int pcie_offset_bcm7278[] = {
	[RGR1_SW_INIT_1] = 0xc010,
	[EXT_CFG_INDEX] = 0x9000,
	[EXT_CFG_DATA] = 0x9004,
};

static const struct pcie_cfg_data bcm7278_cfg = {
	.reg_field_info = pcie_reg_field_info_bcm7278,
	.offsets	= pcie_offset_bcm7278,
	.max_burst_size	= BURST_SIZE_512,
	.type		= BCM7278,
};

static void __iomem *brcm_pcie_map_conf(struct pci_bus *bus, unsigned int devfn,
					int where);

static struct pci_ops brcm_pcie_ops = {
	.map_bus = brcm_pcie_map_conf,
	.read = pci_generic_config_read,
	.write = pci_generic_config_write,
};

#if defined(CONFIG_MIPS)
/* Broadcom MIPs HW implicitly does the swapping if necessary */
#define bcm_readl(a)		__raw_readl(a)
#define bcm_writel(d, a)	__raw_writel(d, a)
#define bcm_readw(a)		__raw_readw(a)
#define bcm_writew(d, a)	__raw_writew(d, a)
#else
#define bcm_readl(a)		readl(a)
#define bcm_writel(d, a)	writel(d, a)
#define bcm_readw(a)		readw(a)
#define bcm_writew(d, a)	writew(d, a)
#endif

/* These macros extract/insert fields to host controller's register set. */
#define RD_FLD(base, reg, field) \
	rd_fld(base + reg, reg##_##field##_MASK, reg##_##field##_SHIFT)
#define WR_FLD(base, reg, field, val) \
	wr_fld(base + reg, reg##_##field##_MASK, reg##_##field##_SHIFT, val)
#define WR_FLD_RB(base, reg, field, val) \
	wr_fld_rb(base + reg, reg##_##field##_MASK, reg##_##field##_SHIFT, val)
#define WR_FLD_WITH_OFFSET(base, off, reg, field, val) \
	wr_fld(base + reg + off, reg##_##field##_MASK, \
	       reg##_##field##_SHIFT, val)
#define EXTRACT_FIELD(val, reg, field) \
	((val & reg##_##field##_MASK) >> reg##_##field##_SHIFT)
#define INSERT_FIELD(val, reg, field, field_val) \
	((val & ~reg##_##field##_MASK) | \
	 (reg##_##field##_MASK & (field_val << reg##_##field##_SHIFT)))

static const struct dma_map_ops *arch_dma_ops;
static struct of_pci_range *dma_ranges;
static int num_dma_ranges;

static phys_addr_t scb_size[BRCM_MAX_SCB];
static int num_memc;
static int num_pcie;
static DEFINE_MUTEX(brcm_pcie_lock);

static unsigned int bounce_buffer = 32*1024*1024;
module_param(bounce_buffer, uint, 0644);
MODULE_PARM_DESC(bounce_buffer, "Size of bounce buffer");

static unsigned int bounce_threshold = 0xc0000000;
module_param(bounce_threshold, uint, 0644);
MODULE_PARM_DESC(bounce_threshold, "Bounce threshold");

static struct brcm_pcie *g_pcie;

static dma_addr_t brcm_to_pci(dma_addr_t addr)
{
	struct of_pci_range *p;

	if (!num_dma_ranges)
		return addr;

	for (p = dma_ranges; p < &dma_ranges[num_dma_ranges]; p++)
		if (addr >= p->cpu_addr && addr < (p->cpu_addr + p->size))
			return addr - p->cpu_addr + p->pci_addr;

	return addr;
}

static dma_addr_t brcm_to_cpu(dma_addr_t addr)
{
	struct of_pci_range *p;

	if (!num_dma_ranges)
		return addr;

	for (p = dma_ranges; p < &dma_ranges[num_dma_ranges]; p++)
		if (addr >= p->pci_addr && addr < (p->pci_addr + p->size))
			return addr - p->pci_addr + p->cpu_addr;

	return addr;
}

static void *brcm_alloc(struct device *dev, size_t size, dma_addr_t *handle,
			gfp_t gfp, unsigned long attrs)
{
	void *ret;

	ret = arch_dma_ops->alloc(dev, size, handle, gfp, attrs);
	if (ret)
		*handle = brcm_to_pci(*handle);
	return ret;
}

static void brcm_free(struct device *dev, size_t size, void *cpu_addr,
		      dma_addr_t handle, unsigned long attrs)
{
	handle = brcm_to_cpu(handle);
	arch_dma_ops->free(dev, size, cpu_addr, handle, attrs);
}

static int brcm_mmap(struct device *dev, struct vm_area_struct *vma,
		     void *cpu_addr, dma_addr_t dma_addr, size_t size,
		     unsigned long attrs)
{
	dma_addr = brcm_to_cpu(dma_addr);
	return arch_dma_ops->mmap(dev, vma, cpu_addr, dma_addr, size, attrs);
}

static int brcm_get_sgtable(struct device *dev, struct sg_table *sgt,
			    void *cpu_addr, dma_addr_t handle, size_t size,
			    unsigned long attrs)
{
	handle = brcm_to_cpu(handle);
	return arch_dma_ops->get_sgtable(dev, sgt, cpu_addr, handle, size,
				       attrs);
}

static dma_addr_t brcm_map_page(struct device *dev, struct page *page,
				unsigned long offset, size_t size,
				enum dma_data_direction dir,
				unsigned long attrs)
{
	return brcm_to_pci(arch_dma_ops->map_page(dev, page, offset, size,
						  dir, attrs));
}

static void brcm_unmap_page(struct device *dev, dma_addr_t handle,
			    size_t size, enum dma_data_direction dir,
			    unsigned long attrs)
{
	handle = brcm_to_cpu(handle);
	arch_dma_ops->unmap_page(dev, handle, size, dir, attrs);
}

static int brcm_map_sg(struct device *dev, struct scatterlist *sgl,
		       int nents, enum dma_data_direction dir,
		       unsigned long attrs)
{
	int i, j;
	struct scatterlist *sg;

	for_each_sg(sgl, sg, nents, i) {
		sg_dma_len(sg) = sg->length;
		sg->dma_address =
			brcm_map_page(dev, sg_page(sg), sg->offset,
				      sg->length, dir, attrs);
		if (dma_mapping_error(dev, sg->dma_address))
			goto bad_mapping;
	}
	return nents;

bad_mapping:
	for_each_sg(sgl, sg, i, j)
		brcm_unmap_page(dev, sg_dma_address(sg),
				sg_dma_len(sg), dir, attrs);
	return 0;
}

static void brcm_unmap_sg(struct device *dev,
			  struct scatterlist *sgl, int nents,
			  enum dma_data_direction dir,
			  unsigned long attrs)
{
	int i;
	struct scatterlist *sg;

	for_each_sg(sgl, sg, nents, i)
		brcm_unmap_page(dev, sg_dma_address(sg),
				sg_dma_len(sg), dir, attrs);
}

static void brcm_sync_single_for_cpu(struct device *dev,
				     dma_addr_t handle, size_t size,
				     enum dma_data_direction dir)
{
	handle = brcm_to_cpu(handle);
	arch_dma_ops->sync_single_for_cpu(dev, handle, size, dir);
}

static void brcm_sync_single_for_device(struct device *dev,
					dma_addr_t handle, size_t size,
					enum dma_data_direction dir)
{
	handle = brcm_to_cpu(handle);
	arch_dma_ops->sync_single_for_device(dev, handle, size, dir);
}

static dma_addr_t brcm_map_resource(struct device *dev, phys_addr_t phys,
				    size_t size,
				    enum dma_data_direction dir,
				    unsigned long attrs)
{
	if (arch_dma_ops->map_resource)
		return brcm_to_pci(arch_dma_ops->map_resource
				   (dev, phys, size, dir, attrs));
	return brcm_to_pci((dma_addr_t)phys);
}

static void brcm_unmap_resource(struct device *dev, dma_addr_t handle,
				size_t size, enum dma_data_direction dir,
				unsigned long attrs)
{
	if (arch_dma_ops->unmap_resource)
		arch_dma_ops->unmap_resource(dev, brcm_to_cpu(handle), size,
					     dir, attrs);
}

void brcm_sync_sg_for_cpu(struct device *dev, struct scatterlist *sgl,
			  int nents, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		brcm_sync_single_for_cpu(dev, sg_dma_address(sg),
					 sg->length, dir);
}

void brcm_sync_sg_for_device(struct device *dev, struct scatterlist *sgl,
			     int nents, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		brcm_sync_single_for_device(dev,
					    sg_dma_address(sg),
					    sg->length, dir);
}

static int brcm_dma_supported(struct device *dev, u64 mask)
{
	if (num_dma_ranges) {
		/*
		 * It is our translated addresses that the EP will "see", so
		 * we check all of the ranges for the largest possible value.
		 */
		int i;

		for (i = 0; i < num_dma_ranges; i++)
			if (dma_ranges[i].pci_addr + dma_ranges[i].size - 1
			    > mask)
				return 0;
		return 1;
	}

	return arch_dma_ops->dma_supported(dev, mask);
}

#ifdef ARCH_HAS_DMA_GET_REQUIRED_MASK
u64 brcm_get_required_mask(struct device *dev)
{
	return arch_dma_ops->get_required_mask(dev);
}
#endif

static const struct dma_map_ops brcm_dma_ops = {
	.alloc			= brcm_alloc,
	.free			= brcm_free,
	.mmap			= brcm_mmap,
	.get_sgtable		= brcm_get_sgtable,
	.map_page		= brcm_map_page,
	.unmap_page		= brcm_unmap_page,
	.map_sg			= brcm_map_sg,
	.unmap_sg		= brcm_unmap_sg,
	.map_resource		= brcm_map_resource,
	.unmap_resource		= brcm_unmap_resource,
	.sync_single_for_cpu	= brcm_sync_single_for_cpu,
	.sync_single_for_device	= brcm_sync_single_for_device,
	.sync_sg_for_cpu	= brcm_sync_sg_for_cpu,
	.sync_sg_for_device	= brcm_sync_sg_for_device,
	.dma_supported		= brcm_dma_supported,
#ifdef ARCH_HAS_DMA_GET_REQUIRED_MASK
	.get_required_mask	= brcm_get_required_mask,
#endif
};

static void brcm_set_dma_ops(struct device *dev)
{
	int ret;

	if (IS_ENABLED(CONFIG_ARM64)) {
		/*
		 * We are going to invoke get_dma_ops().  That
		 * function, at this point in time, invokes
		 * get_arch_dma_ops(), and for ARM64 that function
		 * returns a pointer to dummy_dma_ops.  So then we'd
		 * like to call arch_setup_dma_ops(), but that isn't
		 * exported.  Instead, we call of_dma_configure(),
		 * which is exported, and this calls
		 * arch_setup_dma_ops().  Once we do this the call to
		 * get_dma_ops() will work properly because
		 * dev->dma_ops will be set.
		 */
		ret = of_dma_configure(dev, dev->of_node, true);
		if (ret) {
			dev_err(dev, "of_dma_configure() failed: %d\n", ret);
			return;
		}
	}

	arch_dma_ops = get_dma_ops(dev);
	if (!arch_dma_ops) {
		dev_err(dev, "failed to get arch_dma_ops\n");
		return;
	}

	set_dma_ops(dev, &brcm_dma_ops);
}

static inline void brcm_pcie_perst_set(struct brcm_pcie *pcie,
				       unsigned int val);

static int brcmstb_platform_notifier(struct notifier_block *nb,
				     unsigned long event, void *__dev)
{
	extern unsigned long max_pfn;
	struct device *dev = __dev;
	const char *rc_name = "0000:00:00.0";

	switch (event) {
	case BUS_NOTIFY_ADD_DEVICE:
		if (max_pfn > (bounce_threshold/PAGE_SIZE) &&
		    strcmp(dev->kobj.name, rc_name)) {
			int ret;

			ret = brcm_pcie_bounce_register_dev(dev);
			if (ret) {
				dev_err(dev,
					"brcm_pcie_bounce_register_dev() failed: %d\n",
					ret);
				return ret;
			}
			brcm_set_dma_ops(dev);
		}
		return NOTIFY_OK;

	case BUS_NOTIFY_DEL_DEVICE:
		if (!strcmp(dev->kobj.name, rc_name) && g_pcie) {
			/* Force a bus reset */
			brcm_pcie_perst_set(g_pcie, 1);
			msleep(100);
			brcm_pcie_perst_set(g_pcie, 0);
		}
		return NOTIFY_OK;

	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block brcmstb_platform_nb = {
	.notifier_call = brcmstb_platform_notifier,
};

static int brcm_register_notifier(void)
{
	return bus_register_notifier(&pci_bus_type, &brcmstb_platform_nb);
}

static int brcm_unregister_notifier(void)
{
	return bus_unregister_notifier(&pci_bus_type, &brcmstb_platform_nb);
}

static u32 rd_fld(void __iomem *p, u32 mask, int shift)
{
	return (bcm_readl(p) & mask) >> shift;
}

static void wr_fld(void __iomem *p, u32 mask, int shift, u32 val)
{
	u32 reg = bcm_readl(p);

	reg = (reg & ~mask) | ((val << shift) & mask);
	bcm_writel(reg, p);
}

static void wr_fld_rb(void __iomem *p, u32 mask, int shift, u32 val)
{
	wr_fld(p, mask, shift, val);
	(void)bcm_readl(p);
}

static const char *link_speed_to_str(int s)
{
	switch (s) {
	case 1:
		return "2.5";
	case 2:
		return "5.0";
	case 3:
		return "8.0";
	default:
		break;
	}
	return "???";
}

/*
 * The roundup_pow_of_two() from log2.h invokes
 * __roundup_pow_of_two(unsigned long), but we really need a
 * such a function to take a native u64 since unsigned long
 * is 32 bits on some configurations.  So we provide this helper
 * function below.
 */
static u64 roundup_pow_of_two_64(u64 n)
{
	return 1ULL << fls64(n - 1);
}

/*
 * This is to convert the size of the inbound "BAR" region to the
 * non-linear values of PCIE_X_MISC_RC_BAR[123]_CONFIG_LO.SIZE
 */
int encode_ibar_size(u64 size)
{
	int log2_in = ilog2(size);

	if (log2_in >= 12 && log2_in <= 15)
		/* Covers 4KB to 32KB (inclusive) */
		return (log2_in - 12) + 0x1c;
	else if (log2_in >= 16 && log2_in <= 37)
		/* Covers 64KB to 32GB, (inclusive) */
		return log2_in - 15;
	/* Something is awry so disable */
	return 0;
}

static u32 mdio_form_pkt(int port, int regad, int cmd)
{
	u32 pkt = 0;

	pkt |= (port << MDIO_PORT_SHIFT) & MDIO_PORT_MASK;
	pkt |= (regad << MDIO_REGAD_SHIFT) & MDIO_REGAD_MASK;
	pkt |= (cmd << MDIO_CMD_SHIFT) & MDIO_CMD_MASK;

	return pkt;
}

/* negative return value indicates error */
static int mdio_read(void __iomem *base, u8 port, u8 regad)
{
	int tries;
	u32 data;

	bcm_writel(mdio_form_pkt(port, regad, MDIO_CMD_READ),
		   base + PCIE_RC_DL_MDIO_ADDR);
	bcm_readl(base + PCIE_RC_DL_MDIO_ADDR);

	data = bcm_readl(base + PCIE_RC_DL_MDIO_RD_DATA);
	for (tries = 0; !MDIO_RD_DONE(data) && tries < 10; tries++) {
		udelay(10);
		data = bcm_readl(base + PCIE_RC_DL_MDIO_RD_DATA);
	}

	return MDIO_RD_DONE(data)
		? (data & MDIO_DATA_MASK) >> MDIO_DATA_SHIFT
		: -EIO;
}

/* negative return value indicates error */
static int mdio_write(void __iomem *base, u8 port, u8 regad, u16 wrdata)
{
	int tries;
	u32 data;

	bcm_writel(mdio_form_pkt(port, regad, MDIO_CMD_WRITE),
		   base + PCIE_RC_DL_MDIO_ADDR);
	bcm_readl(base + PCIE_RC_DL_MDIO_ADDR);
	bcm_writel(MDIO_DATA_DONE_MASK | wrdata,
		   base + PCIE_RC_DL_MDIO_WR_DATA);

	data = bcm_readl(base + PCIE_RC_DL_MDIO_WR_DATA);
	for (tries = 0; !MDIO_WT_DONE(data) && tries < 10; tries++) {
		udelay(10);
		data = bcm_readl(base + PCIE_RC_DL_MDIO_WR_DATA);
	}

	return MDIO_WT_DONE(data) ? 0 : -EIO;
}

/*
 * Configures device for Spread Spectrum Clocking (SSC) mode; a negative
 * return value indicates error.
 */
static int set_ssc(void __iomem *base)
{
	int tmp;
	u16 wrdata;
	int pll, ssc;

	tmp = mdio_write(base, MDIO_PORT0, SET_ADDR_OFFSET, SSC_REGS_ADDR);
	if (tmp < 0)
		return tmp;

	tmp = mdio_read(base, MDIO_PORT0, SSC_CNTL_OFFSET);
	if (tmp < 0)
		return tmp;

	wrdata = INSERT_FIELD(tmp, SSC_CNTL_OVRD, EN, 1);
	wrdata = INSERT_FIELD(wrdata, SSC_CNTL_OVRD, VAL, 1);
	tmp = mdio_write(base, MDIO_PORT0, SSC_CNTL_OFFSET, wrdata);
	if (tmp < 0)
		return tmp;

	usleep_range(1000, 2000);
	tmp = mdio_read(base, MDIO_PORT0, SSC_STATUS_OFFSET);
	if (tmp < 0)
		return tmp;

	ssc = EXTRACT_FIELD(tmp, SSC_STATUS, SSC);
	pll = EXTRACT_FIELD(tmp, SSC_STATUS, PLL_LOCK);

	return (ssc && pll) ? 0 : -EIO;
}

/* Limits operation to a specific generation (1, 2, or 3) */
static void set_gen(void __iomem *base, int gen)
{
	u32 lnkcap = bcm_readl(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP);
	u16 lnkctl2 = bcm_readw(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);

	lnkcap = (lnkcap & ~PCI_EXP_LNKCAP_SLS) | gen;
	bcm_writel(lnkcap, base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP);

	lnkctl2 = (lnkctl2 & ~0xf) | gen;
	bcm_writew(lnkctl2, base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);
}

static void brcm_pcie_set_outbound_win(struct brcm_pcie *pcie,
				       unsigned int win, phys_addr_t cpu_addr,
				       dma_addr_t  pcie_addr, dma_addr_t size)
{
	void __iomem *base = pcie->base;
	phys_addr_t cpu_addr_mb, limit_addr_mb;
	u32 tmp;

	/* Set the base of the pcie_addr window */
	bcm_writel(lower_32_bits(pcie_addr) + MMIO_ENDIAN,
		   base + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO + (win * 8));
	bcm_writel(upper_32_bits(pcie_addr),
		   base + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI + (win * 8));

	cpu_addr_mb = cpu_addr >> 20;
	limit_addr_mb = (cpu_addr + size - 1) >> 20;

	/* Write the addr base low register */
	WR_FLD_WITH_OFFSET(base, (win * 4),
			   PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT,
			   BASE, cpu_addr_mb);
	/* Write the addr limit low register */
	WR_FLD_WITH_OFFSET(base, (win * 4),
			   PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT,
			   LIMIT, limit_addr_mb);

	if (pcie->type != BCM7435 && pcie->type != BCM7425) {
		/* Write the cpu addr high register */
		tmp = (u32)(cpu_addr_mb >>
			PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_NUM_MASK_BITS);
		WR_FLD_WITH_OFFSET(base, (win * 8),
				   PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI,
				   BASE, tmp);
		/* Write the cpu limit high register */
		tmp = (u32)(limit_addr_mb >>
			PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT_NUM_MASK_BITS);
		WR_FLD_WITH_OFFSET(base, (win * 8),
				   PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI,
				   LIMIT, tmp);
	}
}

static struct irq_chip brcm_msi_irq_chip = {
	.name = "Brcm_MSI",
	.irq_mask = pci_msi_mask_irq,
	.irq_unmask = pci_msi_unmask_irq,
};

static struct msi_domain_info brcm_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		   MSI_FLAG_PCI_MSIX),
	.chip	= &brcm_msi_irq_chip,
};

static void brcm_pcie_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct brcm_msi *msi;
	unsigned long status, virq;
	u32 mask, bit, hwirq;
	struct device *dev;

	chained_irq_enter(chip, desc);
	msi = irq_desc_get_handler_data(desc);
	mask = msi->intr_legacy_mask;
	dev = msi->dev;

	while ((status = bcm_readl(msi->intr_base + STATUS) & mask)) {
		for_each_set_bit(bit, &status, BRCM_INT_PCI_MSI_NR) {
			/* clear the interrupt */
			bcm_writel(1 << bit, msi->intr_base + CLR);

			/* Account for legacy interrupt offset */
			hwirq = bit - msi->intr_legacy_offset;

			virq = irq_find_mapping(msi->inner_domain, hwirq);
			if (virq) {
				if (msi->used & (1 << hwirq))
					generic_handle_irq(virq);
				else
					dev_info(dev, "unhandled MSI %d\n",
						 hwirq);
			} else {
				/* Unknown MSI, just clear it */
				dev_dbg(dev, "unexpected MSI\n");
			}
		}
	}
	chained_irq_exit(chip, desc);
	bcm_writel(1, msi->base + PCIE_MISC_EOI_CTRL);
}

static void brcm_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct brcm_msi *msi = irq_data_get_irq_chip_data(data);
	u32 temp;

	msg->address_lo = lower_32_bits(msi->target_addr);
	msg->address_hi = upper_32_bits(msi->target_addr);
	temp = bcm_readl(msi->base + PCIE_MISC_MSI_DATA_CONFIG);
	msg->data = ((temp >> 16) & (temp & 0xffff)) | data->hwirq;
}

static int brcm_msi_set_affinity(struct irq_data *irq_data,
				 const struct cpumask *mask, bool force)
{
	struct brcm_msi *msi = irq_data_get_irq_chip_data(irq_data);
	return __irq_set_affinity(msi->irq, mask, force);
}

static struct irq_chip brcm_msi_bottom_irq_chip = {
	.name			= "Brcm_MSI",
	.irq_compose_msi_msg	= brcm_compose_msi_msg,
	.irq_set_affinity	= brcm_msi_set_affinity,
};

static int brcm_msi_alloc(struct brcm_msi *msi)
{
	int bit, hwirq;

	mutex_lock(&msi->lock);
	bit = ~msi->used ? ffz(msi->used) : -1;

	if (bit >= 0 && bit < BRCM_INT_PCI_MSI_NR) {
		msi->used |= (1 << bit);
		hwirq = bit - msi->intr_legacy_offset;
	} else {
		hwirq = -ENOSPC;
	}

	mutex_unlock(&msi->lock);
	return hwirq;
}

static void brcm_msi_free(struct brcm_msi *msi, unsigned long hwirq)
{
	mutex_lock(&msi->lock);
	msi->used &= ~(1 << (hwirq + msi->intr_legacy_offset));
	mutex_unlock(&msi->lock);
}

static int brcm_irq_domain_alloc(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs, void *args)
{
	struct brcm_msi *msi = domain->host_data;
	int hwirq;

	hwirq = brcm_msi_alloc(msi);

	if (hwirq < 0)
		return hwirq;

	irq_domain_set_info(domain, virq, (irq_hw_number_t)hwirq,
			    &brcm_msi_bottom_irq_chip, domain->host_data,
			    handle_simple_irq, NULL, NULL);
	return 0;
}

static void brcm_irq_domain_free(struct irq_domain *domain,
				 unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct brcm_msi *msi = irq_data_get_irq_chip_data(d);

	brcm_msi_free(msi, d->hwirq);
}

static void brcm_msi_set_regs(struct brcm_msi *msi)
{
	u32 data_val, msi_lo, msi_hi;

	if (msi->rev >= BRCM_PCIE_HW_REV_33) {
		/*
		 * ffe0 -- least sig 5 bits are 0 indicating 32 msgs
		 * 6540 -- this is our arbitrary unique data value
		 */
		data_val = 0xffe06540;
	} else {
		/*
		 * fff8 -- least sig 3 bits are 0 indicating 8 msgs
		 * 6540 -- this is our arbitrary unique data value
		 */
		data_val = 0xfff86540;
	}

	/*
	 * Make sure we are not masking MSIs.  Note that MSIs can be masked,
	 * but that occurs on the PCIe EP device
	 */
	bcm_writel(0xffffffff & msi->intr_legacy_mask,
		   msi->intr_base + MASK_CLR);

	msi_lo = lower_32_bits(msi->target_addr);
	msi_hi = upper_32_bits(msi->target_addr);
	/*
	 * The 0 bit of PCIE_MISC_MSI_BAR_CONFIG_LO is repurposed to MSI
	 * enable, which we set to 1.
	 */
	bcm_writel(msi_lo | 1, msi->base + PCIE_MISC_MSI_BAR_CONFIG_LO);
	bcm_writel(msi_hi, msi->base + PCIE_MISC_MSI_BAR_CONFIG_HI);
	bcm_writel(data_val, msi->base + PCIE_MISC_MSI_DATA_CONFIG);
}

static const struct irq_domain_ops msi_domain_ops = {
	.alloc	= brcm_irq_domain_alloc,
	.free	= brcm_irq_domain_free,
};

static int brcm_allocate_domains(struct brcm_msi *msi)
{
	struct fwnode_handle *fwnode = of_node_to_fwnode(msi->dn);
	struct device *dev = msi->dev;

	msi->inner_domain = irq_domain_add_linear(NULL, BRCM_INT_PCI_MSI_NR,
						  &msi_domain_ops, msi);
	if (!msi->inner_domain) {
		dev_err(dev, "failed to create IRQ domain\n");
		return -ENOMEM;
	}

	msi->msi_domain = pci_msi_create_irq_domain(fwnode,
						    &brcm_msi_domain_info,
						    msi->inner_domain);
	if (!msi->msi_domain) {
		dev_err(dev, "failed to create MSI domain\n");
		irq_domain_remove(msi->inner_domain);
		return -ENOMEM;
	}

	return 0;
}

static void brcm_free_domains(struct brcm_msi *msi)
{
	irq_domain_remove(msi->msi_domain);
	irq_domain_remove(msi->inner_domain);
}

static void brcm_msi_remove(struct brcm_pcie *pcie)
{
	struct brcm_msi *msi = pcie->msi;

	if (!msi)
		return;
	irq_set_chained_handler(msi->irq, NULL);
	irq_set_handler_data(msi->irq, NULL);
	brcm_free_domains(msi);
}

static int brcm_pcie_enable_msi(struct brcm_pcie *pcie)
{
	struct brcm_msi *msi;
	int irq, ret;
	struct device *dev = pcie->dev;

	irq = irq_of_parse_and_map(dev->of_node, 1);
	if (irq <= 0) {
		dev_err(dev, "cannot map msi intr\n");
		return -ENODEV;
	}

	msi = devm_kzalloc(dev, sizeof(struct brcm_msi), GFP_KERNEL);
	if (!msi)
		return -ENOMEM;

	msi->dev = dev;
	msi->base = pcie->base;
	msi->rev =  pcie->rev;
	msi->dn = pcie->dn;
	msi->target_addr = pcie->msi_target_addr;
	msi->irq = irq;

	ret = brcm_allocate_domains(msi);
	if (ret)
		return ret;

	irq_set_chained_handler_and_data(msi->irq, brcm_pcie_msi_isr, msi);

	if (msi->rev >= BRCM_PCIE_HW_REV_33) {
		msi->intr_base = msi->base + PCIE_MSI_INTR2_BASE;
		/*
		 * This version of PCIe hw has only 32 intr bits
		 * starting at bit position 0.
		 */
		msi->intr_legacy_mask = 0xffffffff;
		msi->intr_legacy_offset = 0x0;
		msi->used = 0x0;

	} else {
		msi->intr_base = msi->base + PCIE_INTR2_CPU_BASE;
		/*
		 * This version of PCIe hw has only 8 intr bits starting
		 * at bit position 24.
		 */
		msi->intr_legacy_mask = 0xff000000;
		msi->intr_legacy_offset = 24;
		msi->used = 0x00ffffff;
	}

	brcm_msi_set_regs(msi);
	pcie->msi = msi;

	return 0;
}

/* Configuration space read/write support */
static int cfg_index(int busnr, int devfn, int reg)
{
	return ((PCI_SLOT(devfn) & 0x1f) << PCIE_SLOT_SHIFT)
		| ((PCI_FUNC(devfn) & 0x07) << PCIE_FUNC_SHIFT)
		| (busnr << PCIE_BUSNUM_SHIFT)
		| (reg & ~3);
}

/* The controller is capable of serving in both RC and EP roles */
static bool brcm_pcie_rc_mode(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	u32 val = bcm_readl(base + PCIE_MISC_PCIE_STATUS);

	return !!EXTRACT_FIELD(val, PCIE_MISC_PCIE_STATUS, PCIE_PORT);
}

static bool brcm_pcie_link_up(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	u32 val = bcm_readl(base + PCIE_MISC_PCIE_STATUS);
	u32 dla = EXTRACT_FIELD(val, PCIE_MISC_PCIE_STATUS, PCIE_DL_ACTIVE);
	u32 plu = EXTRACT_FIELD(val, PCIE_MISC_PCIE_STATUS, PCIE_PHYLINKUP);

	return  (dla && plu) ? true : false;
}

static void __iomem *brcm_pcie_map_conf(struct pci_bus *bus, unsigned int devfn,
					int where)
{
	struct brcm_pcie *pcie = bus->sysdata;
	void __iomem *base = pcie->base;
	int idx;

	/* Accesses to the RC go right to the RC registers if slot==0 */
	if (pci_is_root_bus(bus))
		return PCI_SLOT(devfn) ? NULL : base + where;

	/* For devices, write to the config space index register */
	idx = cfg_index(bus->number, devfn, 0);
	bcm_writel(idx, pcie->base + IDX_ADDR(pcie));
	return base + DATA_ADDR(pcie) + where;
}

static inline void brcm_pcie_bridge_sw_init_set(struct brcm_pcie *pcie,
						unsigned int val)
{
	unsigned int shift = pcie->reg_field_info[RGR1_SW_INIT_1_INIT_SHIFT];
	u32 mask =  pcie->reg_field_info[RGR1_SW_INIT_1_INIT_MASK];

	wr_fld_rb(pcie->base + PCIE_RGR1_SW_INIT_1(pcie), mask, shift, val);
}

static inline void brcm_pcie_perst_set(struct brcm_pcie *pcie,
				       unsigned int val)
{
	if (pcie->type != BCM7278)
		wr_fld_rb(pcie->base + PCIE_RGR1_SW_INIT_1(pcie),
			  PCIE_RGR1_SW_INIT_1_PERST_MASK,
			  PCIE_RGR1_SW_INIT_1_PERST_SHIFT, val);
	else
		/* Assert = 0, de-assert = 1 on 7278 */
		WR_FLD_RB(pcie->base, PCIE_MISC_PCIE_CTRL, PCIE_PERSTB, !val);
}

static int pci_dma_range_parser_init(struct of_pci_range_parser *parser,
				     struct device_node *node)
{
	const int na = 3, ns = 2;
	int rlen;

	parser->node = node;
	parser->pna = of_n_addr_cells(node);
	parser->np = parser->pna + na + ns;

	parser->range = of_get_property(node, "dma-ranges", &rlen);
	if (!parser->range)
		return -ENOENT;

	parser->end = parser->range + rlen / sizeof(__be32);

	return 0;
}

static int brcm_pcie_parse_map_dma_ranges(struct brcm_pcie *pcie)
{
	int i;
	struct of_pci_range_parser parser;
	struct device_node *dn = pcie->dn;

	/*
	 * Parse dma-ranges property if present.  If there are multiple
	 * PCIe controllers, we only have to parse from one of them since
	 * the others will have an identical mapping.
	 */
	if (!pci_dma_range_parser_init(&parser, dn)) {
		unsigned int max_ranges
			= (parser.end - parser.range) / parser.np;

		dma_ranges = kcalloc(max_ranges, sizeof(struct of_pci_range),
				     GFP_KERNEL);
		if (!dma_ranges)
			return -ENOMEM;

		for (i = 0; of_pci_range_parser_one(&parser, dma_ranges + i);
		     i++)
			num_dma_ranges++;
	}

	return 0;
}

static int brcm_pcie_add_controller(struct brcm_pcie *pcie)
{
	int i, ret = 0;
	struct device *dev = pcie->dev;

	mutex_lock(&brcm_pcie_lock);
	if (num_pcie > 0) {
		num_pcie++;
		goto done;
	}

	ret = brcm_register_notifier();
	if (ret) {
		dev_err(dev, "failed to register pci bus notifier\n");
		goto done;
	}
	ret = brcm_pcie_parse_map_dma_ranges(pcie);
	if (ret)
		goto done;

	if (!num_dma_ranges) {
		/* Determine num_memc and their sizes by other means */
		for (i = 0, num_memc = 0; i < BRCM_MAX_SCB; i++) {
			u64 size = brcmstb_memory_memc_size(i);

			if (size == (u64)-1) {
				dev_err(dev, "cannot get memc%d size\n", i);
				ret = -EINVAL;
				goto done;
			} else if (size) {
				scb_size[i] = roundup_pow_of_two_64(size);
			} else {
				break;
			}
		}
		num_memc = i;
	}

	g_pcie = pcie;
	num_pcie++;
done:
	mutex_unlock(&brcm_pcie_lock);
	return ret;
}

static void brcm_pcie_remove_controller(struct brcm_pcie *pcie)
{
	mutex_lock(&brcm_pcie_lock);
	if (--num_pcie > 0)
		goto out;

	g_pcie = NULL;
	if (brcm_unregister_notifier())
		dev_err(pcie->dev, "failed to unregister pci bus notifier\n");
	kfree(dma_ranges);
	dma_ranges = NULL;
	num_dma_ranges = 0;
	num_memc = 0;
out:
	mutex_unlock(&brcm_pcie_lock);
}

static int brcm_pcie_parse_request_of_pci_ranges(struct brcm_pcie *pcie)
{
	struct resource_entry *win;
	int ret;

	ret = devm_of_pci_get_host_bridge_resources(pcie->dev, 0, 0xff,
						    &pcie->resources, NULL);
	if (ret) {
		dev_err(pcie->dev, "failed to get host resources\n");
		return ret;
	}

	resource_list_for_each_entry(win, &pcie->resources) {
		struct resource *parent, *res = win->res;
		dma_addr_t offset = (dma_addr_t)win->offset;

		if (resource_type(res) == IORESOURCE_IO) {
			parent = &ioport_resource;
		} else if (resource_type(res) == IORESOURCE_MEM) {
			if (pcie->num_out_wins >= BRCM_NUM_PCIE_OUT_WINS) {
				dev_err(pcie->dev, "too many outbound wins\n");
				return -EINVAL;
			}
			pcie->out_wins[pcie->num_out_wins].cpu_addr
				= (phys_addr_t)res->start;
			pcie->out_wins[pcie->num_out_wins].pcie_addr
				= (dma_addr_t)(res->start
					       - (phys_addr_t)offset);
			pcie->out_wins[pcie->num_out_wins].size
				= (dma_addr_t)(res->end - res->start + 1);
			pcie->num_out_wins++;
			parent = &iomem_resource;
		} else {
			continue;
		}

		ret = devm_request_resource(pcie->dev, parent, res);
		if (ret) {
			dev_err(pcie->dev, "failed to get res %pR\n", res);
			return ret;
		}
	}
	return 0;
}

static int brcm_pcie_setup(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	unsigned int scb_size_val;
	u64 rc_bar2_offset, rc_bar2_size, total_mem_size = 0;
	u32 tmp;
	int i, j, ret, limit;
	u16 nlw, cls, lnksta;
	bool ssc_good = false;
	struct device *dev = pcie->dev;
	u64 msi_target_addr;

	/* Reset the bridge */
	brcm_pcie_bridge_sw_init_set(pcie, 1);

	/*
	 * Ensure that the fundamental reset is asserted, except for 7278,
	 * which fails if we do this.
	 */
	if (pcie->type != BCM7278)
		brcm_pcie_perst_set(pcie, 1);

	usleep_range(100, 200);

	/* Take the bridge out of reset */
	brcm_pcie_bridge_sw_init_set(pcie, 0);

	WR_FLD_RB(base, PCIE_MISC_HARD_PCIE_HARD_DEBUG, SERDES_IDDQ, 0);
	/* Wait for SerDes to be stable */
	usleep_range(100, 200);

	/* Grab the PCIe hw revision number */
	tmp = bcm_readl(base + PCIE_MISC_REVISION);
	pcie->rev = EXTRACT_FIELD(tmp, PCIE_MISC_REVISION, MAJMIN);

	/* Set SCB_MAX_BURST_SIZE, CFG_READ_UR_MODE, SCB_ACCESS_EN */
	tmp = INSERT_FIELD(0, PCIE_MISC_MISC_CTRL, SCB_ACCESS_EN, 1);
	tmp = INSERT_FIELD(tmp, PCIE_MISC_MISC_CTRL, CFG_READ_UR_MODE, 1);
	tmp = INSERT_FIELD(tmp, PCIE_MISC_MISC_CTRL, MAX_BURST_SIZE,
			   pcie->max_burst_size);
	bcm_writel(tmp, base + PCIE_MISC_MISC_CTRL);

	/*
	 * Set up inbound memory view for the EP (called RC_BAR2,
	 * not to be confused with the BARs that are advertised by
	 * the EP).
	 *
	 * The PCIe host controller by design must set the inbound
	 * viewport to be a contiguous arrangement of all of the
	 * system's memory.  In addition, its size mut be a power of
	 * two.  Further, the MSI target address must NOT be placed
	 * inside this region, as the decoding logic will consider its
	 * address to be inbound memory traffic.  To further
	 * complicate matters, the viewport must start on a
	 * pcie-address that is aligned on a multiple of its size.
	 * If a portion of the viewport does not represent system
	 * memory -- e.g. 3GB of memory requires a 4GB viewport --
	 * we can map the outbound memory in or after 3GB and even
	 * though the viewport will overlap the outbound memory
	 * the controller will know to send outbound memory downstream
	 * and everything else upstream.
	 */

	if (num_dma_ranges) {
		/*
		 * Use the base address and size(s) provided in the dma-ranges
		 * property.
		 */
		for (i = 0; i < num_dma_ranges; i++)
			scb_size[i] = roundup_pow_of_two_64(dma_ranges[i].size);

		num_memc = num_dma_ranges;
		rc_bar2_offset = dma_ranges[0].pci_addr;
	} else if (num_memc) {
		/*
		 * Set simple configuration based on memory sizes
		 * only.  We always start the viewport at address 0.
		 */
		rc_bar2_offset = 0;
	} else {
		return -EINVAL;
	}

	for (i = 0; i < num_memc; i++)
		total_mem_size += scb_size[i];

	rc_bar2_size = roundup_pow_of_two_64(total_mem_size);

	/* Verify the alignment is correct */
	if (rc_bar2_offset & (rc_bar2_size - 1)) {
		dev_err(dev, "inbound window is misaligned\n");
		return -EINVAL;
	}

	/*
	 * Position the MSI target low if possible.
	 *
	 * TO DO: Consider outbound window when choosing MSI target and
	 * verifying configuration.
	 */
	msi_target_addr = BRCM_MSI_TARGET_ADDR_LT_4GB;
	if (rc_bar2_offset <= msi_target_addr &&
	    rc_bar2_offset + rc_bar2_size > msi_target_addr)
		msi_target_addr = BRCM_MSI_TARGET_ADDR_GT_4GB;

	pcie->msi_target_addr = msi_target_addr;

	tmp = lower_32_bits(rc_bar2_offset);
	tmp = INSERT_FIELD(tmp, PCIE_MISC_RC_BAR2_CONFIG_LO, SIZE,
			   encode_ibar_size(rc_bar2_size));
	bcm_writel(tmp, base + PCIE_MISC_RC_BAR2_CONFIG_LO);
	bcm_writel(upper_32_bits(rc_bar2_offset),
		   base + PCIE_MISC_RC_BAR2_CONFIG_HI);

	scb_size_val = scb_size[0]
		? ilog2(scb_size[0]) - 15 : 0xf; /* 0xf is 1GB */
	WR_FLD(base, PCIE_MISC_MISC_CTRL, SCB0_SIZE, scb_size_val);

	if (num_memc > 1) {
		scb_size_val = scb_size[1]
			? ilog2(scb_size[1]) - 15 : 0xf; /* 0xf is 1GB */
		WR_FLD(base, PCIE_MISC_MISC_CTRL, SCB1_SIZE, scb_size_val);
	}

	if (num_memc > 2) {
		scb_size_val = scb_size[2]
			? ilog2(scb_size[2]) - 15 : 0xf; /* 0xf is 1GB */
		WR_FLD(base, PCIE_MISC_MISC_CTRL, SCB2_SIZE, scb_size_val);
	}

	/* disable the PCIe->GISB memory window (RC_BAR1) */
	WR_FLD(base, PCIE_MISC_RC_BAR1_CONFIG_LO, SIZE, 0);

	/* disable the PCIe->SCB memory window (RC_BAR3) */
	WR_FLD(base, PCIE_MISC_RC_BAR3_CONFIG_LO, SIZE, 0);

	if (!pcie->suspended) {
		/* clear any interrupts we find on boot */
		bcm_writel(0xffffffff, base + PCIE_INTR2_CPU_BASE + CLR);
		(void)bcm_readl(base + PCIE_INTR2_CPU_BASE + CLR);
	}

	/* Mask all interrupts since we are not handling any yet */
	bcm_writel(0xffffffff, base + PCIE_INTR2_CPU_BASE + MASK_SET);
	(void)bcm_readl(base + PCIE_INTR2_CPU_BASE + MASK_SET);

	if (pcie->gen)
		set_gen(base, pcie->gen);

	/* Unassert the fundamental reset */
	brcm_pcie_perst_set(pcie, 0);

	/*
	 * Give the RC/EP time to wake up, before trying to configure RC.
	 * Intermittently check status for link-up, up to a total of 100ms
	 * when we don't know if the device is there, and up to 1000ms if
	 * we do know the device is there.
	 */
	limit = pcie->suspended ? 1000 : 100;
	for (i = 1, j = 0; j < limit && !brcm_pcie_link_up(pcie);
	     j += i, i = i * 2)
		msleep(i + j > limit ? limit - j : i);

	if (!brcm_pcie_link_up(pcie)) {
		dev_info(dev, "link down\n");
		return -ENODEV;
	}

	if (!brcm_pcie_rc_mode(pcie)) {
		dev_err(dev, "PCIe misconfigured; is in EP mode\n");
		return -EINVAL;
	}

	for (i = 0; i < pcie->num_out_wins; i++)
		brcm_pcie_set_outbound_win(pcie, i, pcie->out_wins[i].cpu_addr,
					   pcie->out_wins[i].pcie_addr,
					   pcie->out_wins[i].size);

	/*
	 * For config space accesses on the RC, show the right class for
	 * a PCIe-PCIe bridge (the default setting is to be EP mode).
	 */
	WR_FLD_RB(base, PCIE_RC_CFG_PRIV1_ID_VAL3, CLASS_CODE, 0x060400);

	if (pcie->ssc) {
		ret = set_ssc(base);
		if (ret == 0)
			ssc_good = true;
		else
			dev_err(dev, "failed attempt to enter ssc mode\n");
	}

	lnksta = bcm_readw(base + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKSTA);
	cls = lnksta & PCI_EXP_LNKSTA_CLS;
	nlw = (lnksta & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT;
	dev_info(dev, "link up, %s Gbps x%u %s\n", link_speed_to_str(cls),
		 nlw, ssc_good ? "(SSC)" : "(!SSC)");

	/* PCIe->SCB endian mode for BAR */
	/* field ENDIAN_MODE_BAR2 = DATA_ENDIAN */
	WR_FLD_RB(base, PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1,
		  ENDIAN_MODE_BAR2, DATA_ENDIAN);

	/*
	 * Refclk from RC should be gated with CLKREQ# input when ASPM L0s,L1
	 * is enabled =>  setting the CLKREQ_DEBUG_ENABLE field to 1.
	 */
	WR_FLD_RB(base, PCIE_MISC_HARD_PCIE_HARD_DEBUG, CLKREQ_DEBUG_ENABLE, 1);

	return 0;
}

/* L23 is a low-power PCIe link state */
static void enter_l23(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	int tries, l23;

	/* assert request for L23 */
	WR_FLD_RB(base, PCIE_MISC_PCIE_CTRL, PCIE_L23_REQUEST, 1);
	/* poll L23 status */
	for (tries = 0, l23 = 0; tries < 1000 && !l23; tries++)
		l23 = RD_FLD(base, PCIE_MISC_PCIE_STATUS, PCIE_LINK_IN_L23);
	if (!l23)
		dev_err(pcie->dev, "failed to enter L23\n");
}

static void turn_off(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;

	if (brcm_pcie_link_up(pcie))
		enter_l23(pcie);
	/* Assert fundamental reset */
	brcm_pcie_perst_set(pcie, 1);
	/* Deassert request for L23 in case it was asserted */
	WR_FLD_RB(base, PCIE_MISC_PCIE_CTRL, PCIE_L23_REQUEST, 0);
	/* Turn off SerDes */
	WR_FLD_RB(base, PCIE_MISC_HARD_PCIE_HARD_DEBUG, SERDES_IDDQ, 1);
	/* Shutdown PCIe bridge */
	brcm_pcie_bridge_sw_init_set(pcie, 1);
}

static int brcm_pcie_suspend(struct device *dev)
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev);

	turn_off(pcie);
	clk_disable_unprepare(pcie->clk);
	pcie->suspended = true;

	return 0;
}

static int brcm_pcie_resume(struct device *dev)
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev);
	void __iomem *base;
	int ret;

	base = pcie->base;
	clk_prepare_enable(pcie->clk);

	/* Take bridge out of reset so we can access the SerDes reg */
	brcm_pcie_bridge_sw_init_set(pcie, 0);

	/* Turn on SerDes */
	WR_FLD_RB(base, PCIE_MISC_HARD_PCIE_HARD_DEBUG, SERDES_IDDQ, 0);
	/* Wait for SerDes to be stable */
	usleep_range(100, 200);

	ret = brcm_pcie_setup(pcie);
	if (ret)
		return ret;

	if (pcie->msi && pcie->msi_internal)
		brcm_msi_set_regs(pcie->msi);

	pcie->suspended = false;

	return 0;
}

static void _brcm_pcie_remove(struct brcm_pcie *pcie)
{
	brcm_msi_remove(pcie);
	turn_off(pcie);
	clk_disable_unprepare(pcie->clk);
	clk_put(pcie->clk);
	brcm_pcie_remove_controller(pcie);
}

static int brcm_pcie_remove(struct platform_device *pdev)
{
	struct brcm_pcie *pcie = platform_get_drvdata(pdev);

	pci_stop_root_bus(pcie->root_bus);
	pci_remove_root_bus(pcie->root_bus);
	_brcm_pcie_remove(pcie);

	return 0;
}

static const struct of_device_id brcm_pcie_match[] = {
	{ .compatible = "brcm,bcm7425-pcie", .data = &bcm7425_cfg },
	{ .compatible = "brcm,bcm7435-pcie", .data = &bcm7435_cfg },
	{ .compatible = "brcm,bcm7278-pcie", .data = &bcm7278_cfg },
	{ .compatible = "brcm,bcm7445-pcie", .data = &generic_cfg },
	{},
};
MODULE_DEVICE_TABLE(of, brcm_pcie_match);

static int brcm_pcie_probe(struct platform_device *pdev)
{
	struct device_node *dn = pdev->dev.of_node, *msi_dn;
	const struct of_device_id *of_id;
	const struct pcie_cfg_data *data;
	int ret;
	struct brcm_pcie *pcie;
	struct resource *res;
	void __iomem *base;
	struct pci_host_bridge *bridge;
	struct pci_bus *child;
	extern unsigned long max_pfn;

	bridge = devm_pci_alloc_host_bridge(&pdev->dev, sizeof(*pcie));
	if (!bridge)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(bridge);
	INIT_LIST_HEAD(&pcie->resources);

	of_id = of_match_node(brcm_pcie_match, dn);
	if (!of_id) {
		dev_err(&pdev->dev, "failed to look up compatible string\n");
		return -EINVAL;
	}

	data = of_id->data;
	pcie->reg_offsets = data->offsets;
	pcie->reg_field_info = data->reg_field_info;
	pcie->max_burst_size = data->max_burst_size;
	pcie->type = data->type;
	pcie->dn = dn;
	pcie->dev = &pdev->dev;

	/* We use the domain number as our controller number */
	pcie->id = of_get_pci_domain_nr(dn);
	if (pcie->id < 0)
		return pcie->id;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/* To Do: Add hardware check if this ever gets fixed */
	if (max_pfn > (bounce_threshold/PAGE_SIZE)) {
		int ret;
		ret = brcm_pcie_bounce_init(&pdev->dev, bounce_buffer,
					    (dma_addr_t)bounce_threshold);
		if (ret) {
			if (ret != -EPROBE_DEFER)
				dev_err(&pdev->dev,
					"could not init bounce buffers: %d\n",
					ret);
			return ret;
		}
	}

	pcie->clk = of_clk_get_by_name(dn, "sw_pcie");
	if (IS_ERR(pcie->clk)) {
		dev_warn(&pdev->dev, "could not get clock\n");
		pcie->clk = NULL;
	}
	pcie->base = base;

	ret = of_pci_get_max_link_speed(dn);
	pcie->gen = (ret < 0) ? 0 : ret;

	pcie->ssc = of_property_read_bool(dn, "brcm,enable-ssc");

	ret = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (ret == 0)
		/* keep going, as we don't use this intr yet */
		dev_warn(pcie->dev, "cannot get PCIe interrupt\n");
	else
		pcie->irq = ret;

	ret = brcm_pcie_parse_request_of_pci_ranges(pcie);
	if (ret)
		return ret;

	ret = clk_prepare_enable(pcie->clk);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "could not enable clock\n");
		return ret;
	}

	ret = brcm_pcie_add_controller(pcie);
	if (ret)
		return ret;

	ret = brcm_pcie_setup(pcie);
	if (ret)
		goto fail;

	msi_dn = of_parse_phandle(pcie->dn, "msi-parent", 0);
	/* Use the internal MSI if no msi-parent property */
	if (!msi_dn)
		msi_dn = pcie->dn;

	if (pci_msi_enabled() && msi_dn == pcie->dn) {
		ret = brcm_pcie_enable_msi(pcie);
		if (ret)
			dev_err(pcie->dev,
				"probe of internal MSI failed: %d)", ret);
		else
			pcie->msi_internal = true;
	}

	list_splice_init(&pcie->resources, &bridge->windows);
	bridge->dev.parent = &pdev->dev;
	bridge->busnr = 0;
	bridge->ops = &brcm_pcie_ops;
	bridge->sysdata = pcie;
	bridge->map_irq = of_irq_parse_and_map_pci;
	bridge->swizzle_irq = pci_common_swizzle;

	ret = pci_scan_root_bus_bridge(bridge);
	if (ret < 0) {
		dev_err(pcie->dev, "Scanning root bridge failed\n");
		goto fail;
	}

	pci_assign_unassigned_bus_resources(bridge->bus);
	list_for_each_entry(child, &bridge->bus->children, node)
		pcie_bus_configure_settings(child);
	pci_bus_add_devices(bridge->bus);
	platform_set_drvdata(pdev, pcie);
	pcie->root_bus = bridge->bus;

	return 0;
fail:
	_brcm_pcie_remove(pcie);
	return ret;
}

static const struct dev_pm_ops brcm_pcie_pm_ops = {
	.suspend_noirq = brcm_pcie_suspend,
	.resume_noirq = brcm_pcie_resume,
};

static struct platform_driver brcm_pcie_driver = {
	.probe = brcm_pcie_probe,
	.remove = brcm_pcie_remove,
	.driver = {
		.name = "brcm-pcie",
		.of_match_table = brcm_pcie_match,
		.pm = &brcm_pcie_pm_ops,
	},
};

module_platform_driver(brcm_pcie_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Broadcom STB PCIe RC driver");
MODULE_AUTHOR("Broadcom");
