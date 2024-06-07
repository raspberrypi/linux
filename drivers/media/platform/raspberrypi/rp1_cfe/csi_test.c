// SPDX-License-Identifier: GPL-2.0-only
/*
 * RP1 Camera Front End Driver
 *
 * Copyright (C) 2021-2022 - Raspberry Pi Ltd.
 *
 */

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/phy/phy.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/videodev2.h>

#include "dphy.h"

#define CSI_TEST_MODULE_NAME "rp1-csi-test"
#define CSI_TEST_VERSION     "1.0"

int num_lanes = 4;
module_param_named(num_lanes, num_lanes, int, 0600);
MODULE_PARM_DESC(num_lanes, "Number of lanes to test\n");

int mbps = 200;
module_param_named(mbps, mbps, int, 0600);
MODULE_PARM_DESC(pix_clock, "Megabits per second per lane\n");

/* MIPICFG registers */
#define MIPICFG_CFG		0x004
#define MIPICFG_INTR		0x028
#define MIPICFG_INTE		0x02c
#define MIPICFG_INTF		0x030
#define MIPICFG_INTS		0x034

#define MIPICFG_CFG_SEL_CSI	BIT(0)

#define MIPICFG_INT_CSI_DMA	BIT(0)
#define MIPICFG_INT_CSI_HOST	BIT(2)
#define MIPICFG_INT_PISP_FE	BIT(4)

/* CSI2-DMA registers */
#define CSI2_STATUS		0x000
#define CSI2_QOS		0x004
#define CSI2_DISCARDS_OVERFLOW	0x008
#define CSI2_DISCARDS_INACTIVE	0x00c
#define CSI2_DISCARDS_UNMATCHED	0x010
#define CSI2_DISCARDS_LEN_LIMIT	0x014

#define CSI2_DISCARDS_AMOUNT_SHIFT	0
#define CSI2_DISCARDS_AMOUNT_MASK	GENMASK(23, 0)
#define CSI2_DISCARDS_DT_SHIFT		24
#define CSI2_DISCARDS_DT_MASK		GENMASK(29, 24)
#define CSI2_DISCARDS_VC_SHIFT		30
#define CSI2_DISCARDS_VC_MASK		GENMASK(31, 30)

#define CSI2_LLEV_PANICS	0x018
#define CSI2_ULEV_PANICS	0x01c
#define CSI2_IRQ_MASK		0x020
#define CSI2_IRQ_MASK_IRQ_OVERFLOW		BIT(0)
#define CSI2_IRQ_MASK_IRQ_DISCARD_OVERFLOW	BIT(1)
#define CSI2_IRQ_MASK_IRQ_DISCARD_LENGTH_LIMIT	BIT(2)
#define CSI2_IRQ_MASK_IRQ_DISCARD_UNMATCHED	BIT(3)
#define CSI2_IRQ_MASK_IRQ_DISCARD_INACTIVE	BIT(4)
#define CSI2_IRQ_MASK_IRQ_ALL                                              \
	(CSI2_IRQ_MASK_IRQ_OVERFLOW | CSI2_IRQ_MASK_IRQ_DISCARD_OVERFLOW | \
	 CSI2_IRQ_MASK_IRQ_DISCARD_LENGTH_LIMIT |                          \
	 CSI2_IRQ_MASK_IRQ_DISCARD_UNMATCHED |                             \
	 CSI2_IRQ_MASK_IRQ_DISCARD_INACTIVE)

#define CSI2_CTRL		0x024
#define CSI2_CH_CTRL(x)		((x) * 0x40 + 0x28)
#define CSI2_CH_ADDR0(x)	((x) * 0x40 + 0x2c)
#define CSI2_CH_ADDR1(x)	((x) * 0x40 + 0x3c)
#define CSI2_CH_STRIDE(x)	((x) * 0x40 + 0x30)
#define CSI2_CH_LENGTH(x)	((x) * 0x40 + 0x34)
#define CSI2_CH_DEBUG(x)	((x) * 0x40 + 0x38)
#define CSI2_CH_FRAME_SIZE(x)	((x) * 0x40 + 0x40)
#define CSI2_CH_COMP_CTRL(x)	((x) * 0x40 + 0x44)
#define CSI2_CH_FE_FRAME_ID(x)	((x) * 0x40 + 0x48)

/* CSI2_STATUS */
#define IRQ_FS(x)		(BIT(0) << (x))
#define IRQ_FE(x)		(BIT(4) << (x))
#define IRQ_FE_ACK(x)		(BIT(8) << (x))
#define IRQ_LE(x)		(BIT(12) << (x))
#define IRQ_LE_ACK(x)		(BIT(16) << (x))
#define IRQ_CH_MASK(x)		(0x11111 << (x))
#define IRQ_OVERFLOW		BIT(20)
#define IRQ_DISCARD_OVERFLOW	BIT(21)
#define IRQ_DISCARD_LEN_LIMIT	BIT(22)
#define IRQ_DISCARD_UNMATCHED	BIT(23)
#define IRQ_DISCARD_INACTIVE	BIT(24)

/* CSI2_CTRL */
#define EOP_IS_EOL		BIT(0)

/* CSI2_CH_CTRL */
#define DMA_EN			BIT(0)
#define FORCE			BIT(3)
#define AUTO_ARM		BIT(4)
#define IRQ_EN_FS		BIT(13)
#define IRQ_EN_FE		BIT(14)
#define IRQ_EN_FE_ACK		BIT(15)
#define IRQ_EN_LE		BIT(16)
#define IRQ_EN_LE_ACK		BIT(17)
#define FLUSH_FE		BIT(28)
#define PACK_LINES		BIT(29)
#define PACK_BYTES		BIT(30)
#define CH_MODE_MASK		GENMASK(2, 1)
#define VC_MASK			GENMASK(6, 5)
#define DT_MASK			GENMASK(12, 7)
#define LC_MASK			GENMASK(27, 18)

/* CHx_COMPRESSION_CONTROL */
#define COMP_OFFSET_MASK	GENMASK(15, 0)
#define COMP_SHIFT_MASK		GENMASK(19, 16)
#define COMP_MODE_MASK		GENMASK(25, 24)

struct csitest_device {
	/* peripheral base addresses */
	void __iomem *mipi_cfg_base;
	void __iomem *csi2_base;

	struct clk *clk;
	spinlock_t state_lock; /* we don't use this but probably should */

	/* parent device */
	struct platform_device *pdev;

	struct dphy_data dphy;

	/* working state */
	struct sg_table *sgt;
	u8 *buf;
};

static inline u32 cfg_reg_read(struct csitest_device *cfe, u32 offset)
{
	return readl(cfe->mipi_cfg_base + offset);
}

static inline void cfg_reg_write(struct csitest_device *cfe, u32 offset, u32 val)
{
	writel(val, cfe->mipi_cfg_base + offset);
}

static inline u32 csi2_reg_read(struct csitest_device *cfe, u32 offset)
{
	return readl(cfe->csi2_base + offset);
}

static inline void csi2_reg_write(struct csitest_device *cfe, u32 offset, u32 val)
{
	writel(val, cfe->csi2_base + offset);
}

static inline void set_field(u32 *valp, u32 field, u32 mask)
{
	u32 val = *valp;

	val &= ~mask;
	val |= (field << __ffs(mask)) & mask;
	*valp = val;
}

static irqreturn_t csitest_isr(int irq, void *dev)
{
	//struct csitest_device *cfe = dev;
	//unsigned int i;

	// TODO
	return IRQ_HANDLED;
}

static void csitest_start(struct csitest_device *cfe)
{
	struct sg_dma_page_iter it;
	u64 addr;
	u32 ctrl = 0;

	csi2_reg_write(cfe, CSI2_STATUS,     -1);
	csi2_reg_write(cfe, CSI2_IRQ_MASK,    0);

	cfe->dphy.dphy_rate = mbps;
	cfe->dphy.active_lanes = num_lanes;

	clk_prepare_enable(cfe->clk);
	dphy_start(&cfe->dphy);

	dma_sync_sgtable_for_cpu(&cfe->pdev->dev, cfe->sgt, DMA_FROM_DEVICE);
	memset(cfe->buf, '?', PAGE_SIZE);
	dma_sync_sgtable_for_device(&cfe->pdev->dev, cfe->sgt, DMA_FROM_DEVICE);
	__sg_page_iter_start(&it.base, cfe->sgt->sgl, cfe->sgt->nents, 0);
	addr = sg_page_iter_dma_address(&it);

	csi2_reg_write(cfe, CSI2_CTRL,        EOP_IS_EOL);
	csi2_reg_write(cfe, CSI2_CH_CTRL(1),  0);
	csi2_reg_write(cfe, CSI2_CH_CTRL(2),  0);
	csi2_reg_write(cfe, CSI2_CH_CTRL(3),  0);
	csi2_reg_write(cfe, CSI2_CH_DEBUG(0), 0);

	ctrl = DMA_EN | FORCE | PACK_LINES;
	csi2_reg_write(cfe, CSI2_CH_CTRL(0), ctrl);

	csi2_reg_write(cfe, CSI2_CH_LENGTH(0), (PAGE_SIZE - 32) >> 4);
	csi2_reg_write(cfe, CSI2_CH_STRIDE(0),  64 >> 4);

	addr >>= 4;
	csi2_reg_write(cfe, CSI2_CH_ADDR1(0), addr >> 32);
	csi2_reg_write(cfe, CSI2_CH_ADDR0(0), addr & 0xffffffff);
}

static void csitest_stop(struct csitest_device *cfe)
{
	csi2_reg_write(cfe, CSI2_CH_CTRL(0), FORCE);
	csi2_reg_write(cfe, CSI2_CH_ADDR1(0), 0);
	csi2_reg_write(cfe, CSI2_CH_ADDR0(0), 0);
	csi2_reg_write(cfe, CSI2_CH_ADDR0(0), 0);
	dphy_stop(&cfe->dphy);
	//clk_disable_unprepare(cfe->clk);
}

static size_t csitest_get_buffer_content(struct csitest_device *cfe, char *buf)
{
	u32 p, q;

	dma_sync_sgtable_for_cpu(&cfe->pdev->dev, cfe->sgt, DMA_FROM_DEVICE);
	memcpy(buf, cfe->buf, PAGE_SIZE);
	buf[PAGE_SIZE - 1] = '\0';
	dma_sync_sgtable_for_device(&cfe->pdev->dev, cfe->sgt, DMA_FROM_DEVICE);

	p = readl(cfe->dphy.base + 0x048);
	q = readl(cfe->dphy.base + 0x04c);
	dev_info(&cfe->pdev->dev, "CSI: phy_rx=0x%08x, stopstate=0x%08x\n", p, q);
	dev_info(&cfe->pdev->dev, "CSI: status 0x%08x, discards 0x%08x, 0x%08x, 0x%08x, 0x%08x\n",
		 csi2_reg_read(cfe, CSI2_STATUS),
		 csi2_reg_read(cfe, 0x8),
		 csi2_reg_read(cfe, 0xc),
		 csi2_reg_read(cfe, 0x10),
		 csi2_reg_read(cfe, 0x14));
	dev_info(&cfe->pdev->dev, "CSI: get_buffer_content, CTRL=0x%08x, DEBUG=0x%08x\n",
		 csi2_reg_read(cfe, CSI2_CH_CTRL(0)),
		 csi2_reg_read(cfe, CSI2_CH_DEBUG(0)));
	return PAGE_SIZE - 1;
}

/* SYSFS interface for running tests */

static struct csitest_device *the_cfe;
static DEFINE_MUTEX(sysfs_mutex);

static ssize_t csitest_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	size_t sz = 0;
	struct csitest_device *my_cfe;

	mutex_lock(&sysfs_mutex);
	my_cfe = the_cfe;
	*buf = '\0';
	if (my_cfe)
		sz = csitest_get_buffer_content(my_cfe, buf);
	mutex_unlock(&sysfs_mutex);

	return sz;
}

static ssize_t csitest_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	struct csitest_device *my_cfe;

	mutex_lock(&sysfs_mutex);
	my_cfe = the_cfe;
	if (my_cfe) {
		csitest_stop(my_cfe);
		csitest_start(my_cfe);
	}
	mutex_unlock(&sysfs_mutex);

	return (count > PAGE_SIZE) ? PAGE_SIZE : count;
}

static struct kobj_attribute kobj_attr =
	__ATTR(rp1_csi_test, 0644, csitest_show, csitest_store);

static struct attribute *attrs[] = {
	&kobj_attr.attr,
	NULL
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static struct kobject *csitest_kobj;

static int csitest_probe(struct platform_device *pdev)
{
	struct csitest_device *cfe;
	int ret;

	cfe = devm_kzalloc(&pdev->dev, sizeof(*cfe), GFP_KERNEL);
	if (!cfe)
		return -ENOMEM;

	platform_set_drvdata(pdev, cfe);
	cfe->pdev = pdev;
	spin_lock_init(&cfe->state_lock);

	cfe->csi2_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cfe->csi2_base)) {
		dev_err(&pdev->dev, "Failed to get dma io block\n");
		ret = PTR_ERR(cfe->csi2_base);
		goto err_cfe_put;
	}

	cfe->dphy.base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(cfe->dphy.base)) {
		dev_err(&pdev->dev, "Failed to get host io block\n");
		ret = PTR_ERR(cfe->dphy.base);
		goto err_cfe_put;
	}

	cfe->mipi_cfg_base = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(cfe->mipi_cfg_base)) {
		dev_err(&pdev->dev, "Failed to get mipi cfg io block\n");
		ret = PTR_ERR(cfe->mipi_cfg_base);
		goto err_cfe_put;
	}

	ret = platform_get_irq(pdev, 0);
	if (ret <= 0) {
		dev_err(&pdev->dev, "No IRQ resource\n");
		ret = -EINVAL;
		goto err_cfe_put;
	}

	ret = devm_request_irq(&pdev->dev, ret, csitest_isr, 0, "rp1-cfe", cfe);
	if (ret) {
		dev_err(&pdev->dev, "Unable to request interrupt\n");
		ret = -EINVAL;
		goto err_cfe_put;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(&pdev->dev, "DMA enable failed\n");
		goto err_cfe_put;
	}
	cfe->sgt = dma_alloc_noncontiguous(&pdev->dev, PAGE_SIZE,
					   DMA_FROM_DEVICE, GFP_KERNEL,
					   DMA_ATTR_ALLOC_SINGLE_PAGES);
	if (!cfe->sgt) {
		ret = -ENOMEM;
		goto err_cfe_put;
	}
	cfe->buf = dma_vmap_noncontiguous(&pdev->dev, PAGE_SIZE,
					  cfe->sgt);
	if (!cfe->buf) {
		ret = -ENOMEM;
		goto err_cfe_put;
	}

	/* TODO: Enable clock only when running. */
	cfe->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(cfe->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(cfe->clk),
				     "clock not found\n");

	/* Enable the MIPI block, set the PHY MUX for CSI2, probe the PHY */
	cfg_reg_write(cfe, MIPICFG_CFG, MIPICFG_CFG_SEL_CSI);
	cfg_reg_write(cfe, MIPICFG_INTE, MIPICFG_INT_CSI_DMA);
	cfe->dphy.dev = &pdev->dev;
	cfe->dphy.dphy_rate = mbps;
	cfe->dphy.max_lanes = 4;
	cfe->dphy.active_lanes = num_lanes;
	dphy_probe(&cfe->dphy);

	/* Start the test running. Any write to the sysfs file will reset the buffer. */
	csitest_start(cfe);

	/* XXX Yuck, global variables! */
	mutex_lock(&sysfs_mutex);
	the_cfe = cfe;
	csitest_kobj = kobject_create_and_add("rp1_csi_test", kernel_kobj);
	mutex_unlock(&sysfs_mutex);
	if (!csitest_kobj) {
		the_cfe = NULL;
		return -ENOMEM;
	}

	ret = sysfs_create_group(csitest_kobj, &attr_group);
	if (!ret)
		return 0;

err_cfe_put:
	return ret;
}

static int csitest_remove(struct platform_device *pdev)
{
	struct csitest_device *cfe = platform_get_drvdata(pdev);

	csitest_stop(cfe);

	mutex_lock(&sysfs_mutex);
	the_cfe = NULL;
	mutex_unlock(&sysfs_mutex);
	if (csitest_kobj) {
		kobject_put(csitest_kobj);
		csitest_kobj = NULL;
	}
	return 0;
}

static const struct of_device_id csitest_of_match[] = {
	{ .compatible = "raspberrypi,rp1-csi-test" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, csitest_of_match);

static struct platform_driver csi_test_driver = {
	.probe		= csitest_probe,
	.remove		= csitest_remove,
	.driver = {
		.name	= CSI_TEST_MODULE_NAME,
		.of_match_table = csitest_of_match,
	},
};

module_platform_driver(csi_test_driver);

MODULE_DESCRIPTION("RP1 CSI test driver");
MODULE_LICENSE("GPL");
