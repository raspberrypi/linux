/*
 * NVIDIA Tegra XUSB mailbox driver
 *
 * Copyright (C) 2014 NVIDIA Corporation
 * Copyright (C) 2014 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <soc/tegra/xusb.h>

#define XUSB_MBOX_NUM_CHANS			2 /* Host + PHY */

#define XUSB_CFG_ARU_MBOX_CMD			0xe4
#define  MBOX_DEST_FALC				BIT(27)
#define  MBOX_DEST_PME				BIT(28)
#define  MBOX_DEST_SMI				BIT(29)
#define  MBOX_DEST_XHCI				BIT(30)
#define  MBOX_INT_EN				BIT(31)
#define XUSB_CFG_ARU_MBOX_DATA_IN		0xe8
#define  CMD_DATA_SHIFT				0
#define  CMD_DATA_MASK				0xffffff
#define  CMD_TYPE_SHIFT				24
#define  CMD_TYPE_MASK				0xff
#define XUSB_CFG_ARU_MBOX_DATA_OUT		0xec
#define XUSB_CFG_ARU_MBOX_OWNER			0xf0
#define  MBOX_OWNER_NONE			0
#define  MBOX_OWNER_FW				1
#define  MBOX_OWNER_SW				2
#define XUSB_CFG_ARU_SMI_INTR			0x428
#define  MBOX_SMI_INTR_FW_HANG			BIT(1)
#define  MBOX_SMI_INTR_EN			BIT(3)

struct tegra_xusb_mbox {
	struct mbox_controller mbox;
	struct regmap *fpci_regs;
	spinlock_t lock;
	int irq;
};

static inline u32 mbox_readl(struct tegra_xusb_mbox *mbox, unsigned long offset)
{
	u32 val;

	regmap_read(mbox->fpci_regs, offset, &val);

	return val;
}

static inline void mbox_writel(struct tegra_xusb_mbox *mbox, u32 val,
			       unsigned long offset)
{
	regmap_write(mbox->fpci_regs, offset, val);
}

static inline struct tegra_xusb_mbox *to_tegra_mbox(struct mbox_controller *c)
{
	return container_of(c, struct tegra_xusb_mbox, mbox);
}

static inline u32 mbox_pack_msg(struct tegra_xusb_mbox_msg *msg)
{
	u32 val;

	val = (msg->cmd & CMD_TYPE_MASK) << CMD_TYPE_SHIFT;
	val |= (msg->data & CMD_DATA_MASK) << CMD_DATA_SHIFT;

	return val;
}

static inline void mbox_unpack_msg(u32 val, struct tegra_xusb_mbox_msg *msg)
{
	msg->cmd = (val >> CMD_TYPE_SHIFT) & CMD_TYPE_MASK;
	msg->data = (val >> CMD_DATA_SHIFT) & CMD_DATA_MASK;
}

static bool mbox_cmd_requires_ack(enum tegra_xusb_mbox_cmd cmd)
{
	switch (cmd) {
	case MBOX_CMD_SET_BW:
	case MBOX_CMD_ACK:
	case MBOX_CMD_NAK:
		return false;
	default:
		return true;
	}
}

static int tegra_xusb_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct tegra_xusb_mbox *mbox = to_tegra_mbox(chan->mbox);
	struct tegra_xusb_mbox_msg *msg = data;
	unsigned long flags;
	u32 reg;

	dev_dbg(mbox->mbox.dev, "TX message %#x:%#x\n", msg->cmd, msg->data);

	spin_lock_irqsave(&mbox->lock, flags);
	/*
	 * Acquire the mailbox.  The firmware still owns the mailbox for
	 * ACK/NAK messages.
	 */
	if (!(msg->cmd == MBOX_CMD_ACK || msg->cmd == MBOX_CMD_NAK)) {
		if (mbox_readl(mbox, XUSB_CFG_ARU_MBOX_OWNER) !=
		    MBOX_OWNER_NONE) {
			dev_err(mbox->mbox.dev, "Mailbox not idle\n");
			goto busy;
		}

		mbox_writel(mbox, MBOX_OWNER_SW, XUSB_CFG_ARU_MBOX_OWNER);
		if (mbox_readl(mbox, XUSB_CFG_ARU_MBOX_OWNER) !=
		    MBOX_OWNER_SW) {
			dev_err(mbox->mbox.dev, "Failed to acquire mailbox");
			goto busy;
		}
	}

	mbox_writel(mbox, mbox_pack_msg(msg), XUSB_CFG_ARU_MBOX_DATA_IN);
	reg = mbox_readl(mbox, XUSB_CFG_ARU_MBOX_CMD);
	reg |= MBOX_INT_EN | MBOX_DEST_FALC;
	mbox_writel(mbox, reg, XUSB_CFG_ARU_MBOX_CMD);

	spin_unlock_irqrestore(&mbox->lock, flags);

	return 0;
busy:
	spin_unlock_irqrestore(&mbox->lock, flags);
	return -EBUSY;
}

static int tegra_xusb_mbox_startup(struct mbox_chan *chan)
{
	return 0;
}

static void tegra_xusb_mbox_shutdown(struct mbox_chan *chan)
{
}

static bool tegra_xusb_mbox_last_tx_done(struct mbox_chan *chan)
{
	struct tegra_xusb_mbox *mbox = to_tegra_mbox(chan->mbox);

	return mbox_readl(mbox, XUSB_CFG_ARU_MBOX_OWNER) == MBOX_OWNER_NONE;
}

static const struct mbox_chan_ops tegra_xusb_mbox_chan_ops = {
	.send_data = tegra_xusb_mbox_send_data,
	.startup = tegra_xusb_mbox_startup,
	.shutdown = tegra_xusb_mbox_shutdown,
	.last_tx_done = tegra_xusb_mbox_last_tx_done,
};

static irqreturn_t tegra_xusb_mbox_irq(int irq, void *p)
{
	struct tegra_xusb_mbox *mbox = p;
	struct tegra_xusb_mbox_msg msg;
	unsigned int i;
	u32 reg;

	spin_lock(&mbox->lock);

	/* Clear mbox interrupts */
	reg = mbox_readl(mbox, XUSB_CFG_ARU_SMI_INTR);
	if (reg & MBOX_SMI_INTR_FW_HANG)
		dev_err(mbox->mbox.dev, "Controller firmware hang\n");
	mbox_writel(mbox, reg, XUSB_CFG_ARU_SMI_INTR);

	reg = mbox_readl(mbox, XUSB_CFG_ARU_MBOX_DATA_OUT);
	mbox_unpack_msg(reg, &msg);

	reg = mbox_readl(mbox, XUSB_CFG_ARU_MBOX_CMD);
	reg &= ~MBOX_DEST_SMI;
	mbox_writel(mbox, reg, XUSB_CFG_ARU_MBOX_CMD);

	/* Clear mailbox owner if no ACK/NAK is required. */
	if (!mbox_cmd_requires_ack(msg.cmd))
		mbox_writel(mbox, MBOX_OWNER_NONE, XUSB_CFG_ARU_MBOX_OWNER);

	dev_dbg(mbox->mbox.dev, "RX message %#x:%#x\n", msg.cmd, msg.data);
	for (i = 0; i < XUSB_MBOX_NUM_CHANS; i++) {
		if (mbox->mbox.chans[i].cl)
			mbox_chan_received_data(&mbox->mbox.chans[i], &msg);
	}

	spin_unlock(&mbox->lock);

	return IRQ_HANDLED;
}

static struct mbox_chan *tegra_xusb_mbox_of_xlate(struct mbox_controller *ctlr,
					const struct of_phandle_args *sp)
{
	struct tegra_xusb_mbox *mbox = to_tegra_mbox(ctlr);
	struct mbox_chan *chan = ERR_PTR(-EINVAL);
	unsigned long flags;
	unsigned int i;

	/* Pick the first available (virtual) channel. */
	spin_lock_irqsave(&mbox->lock, flags);
	for (i = 0; XUSB_MBOX_NUM_CHANS; i++) {
		if (!ctlr->chans[i].cl) {
			chan = &ctlr->chans[i];
			break;
		}
	}
	spin_unlock_irqrestore(&mbox->lock, flags);

	return chan;
}

static const struct of_device_id tegra_xusb_mbox_of_match[] = {
	{ .compatible = "nvidia,tegra124-xusb-mbox" },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_xusb_mbox_of_match);

static int tegra_xusb_mbox_probe(struct platform_device *pdev)
{
	struct tegra_xusb_mbox *mbox;
	int ret;

	mbox = devm_kzalloc(&pdev->dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;
	platform_set_drvdata(pdev, mbox);
	spin_lock_init(&mbox->lock);
	mbox->fpci_regs = dev_get_drvdata(pdev->dev.parent);

	mbox->mbox.dev = &pdev->dev;
	mbox->mbox.chans = devm_kcalloc(&pdev->dev, XUSB_MBOX_NUM_CHANS,
					sizeof(*mbox->mbox.chans), GFP_KERNEL);
	if (!mbox->mbox.chans)
		return -ENOMEM;
	mbox->mbox.num_chans = XUSB_MBOX_NUM_CHANS;
	mbox->mbox.ops = &tegra_xusb_mbox_chan_ops;
	mbox->mbox.txdone_poll = true;
	mbox->mbox.txpoll_period = 1;
	mbox->mbox.of_xlate = tegra_xusb_mbox_of_xlate;

	mbox->irq = platform_get_irq(pdev, 0);
	if (mbox->irq < 0)
		return mbox->irq;
	ret = devm_request_irq(&pdev->dev, mbox->irq, tegra_xusb_mbox_irq, 0,
			       dev_name(&pdev->dev), mbox);
	if (ret < 0)
		return ret;

	ret = mbox_controller_register(&mbox->mbox);
	if (ret < 0)
		dev_err(&pdev->dev, "failed to register mailbox: %d\n", ret);

	return ret;
}

static int tegra_xusb_mbox_remove(struct platform_device *pdev)
{
	struct tegra_xusb_mbox *mbox = platform_get_drvdata(pdev);

	synchronize_irq(mbox->irq);
	devm_free_irq(&pdev->dev, mbox->irq, mbox);
	mbox_controller_unregister(&mbox->mbox);

	return 0;
}

static struct platform_driver tegra_xusb_mbox_driver = {
	.probe = tegra_xusb_mbox_probe,
	.remove = tegra_xusb_mbox_remove,
	.driver = {
		.name = "tegra-xusb-mbox",
		.of_match_table = tegra_xusb_mbox_of_match,
	},
};
module_platform_driver(tegra_xusb_mbox_driver);

MODULE_AUTHOR("Andrew Bresticker <abrestic@chromium.org>");
MODULE_DESCRIPTION("NVIDIA Tegra XUSB mailbox driver");
MODULE_LICENSE("GPL v2");
