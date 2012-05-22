/*
 * BCM2708 master mode driver
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define BCM2708_I2C_C		0x0
#define BCM2708_I2C_S		0x4
#define BCM2708_I2C_DLEN	0x8
#define BCM2708_I2C_A		0xc
#define BCM2708_I2C_FIFO	0x10
#define BCM2708_I2C_DIV		0x14
#define BCM2708_I2C_DEL		0x18
#define BCM2708_I2C_CLKT	0x1c

#define BCM2708_I2C_C_READ	BIT(0)
#define BCM2708_I2C_C_CLEAR	BIT(4) /* bits 4 and 5 both clear */
#define BCM2708_I2C_C_ST	BIT(7)
#define BCM2708_I2C_C_INTD	BIT(8)
#define BCM2708_I2C_C_INTT	BIT(9)
#define BCM2708_I2C_C_INTR	BIT(10)
#define BCM2708_I2C_C_I2CEN	BIT(15)

#define BCM2708_I2C_S_TA	BIT(0)
#define BCM2708_I2C_S_DONE	BIT(1)
#define BCM2708_I2C_S_TXW	BIT(2)
#define BCM2708_I2C_S_RXR	BIT(3)
#define BCM2708_I2C_S_TXD	BIT(4)
#define BCM2708_I2C_S_RXD	BIT(5)
#define BCM2708_I2C_S_TXE	BIT(6)
#define BCM2708_I2C_S_RXF	BIT(7)
#define BCM2708_I2C_S_ERR	BIT(8)
#define BCM2708_I2C_S_CLKT	BIT(9)
#define BCM2708_I2C_S_LEN	BIT(10) /* Fake bit for SW error reporting */

#define BCM2708_I2C_TIMEOUT (msecs_to_jiffies(1000))

struct bcm2708_i2c_dev {
	struct device *dev;
	void __iomem *regs;
	struct i2c_adapter adapter;
	struct completion completion;
	u32 msg_err;
	u8 *msg_buf;
	size_t msg_buf_remaining;
};

static inline void bcm2708_i2c_writel(struct bcm2708_i2c_dev *i2c_dev,
				      u32 reg, u32 val)
{
	writel(val, i2c_dev->regs + reg);
}

static inline u32 bcm2708_i2c_readl(struct bcm2708_i2c_dev *i2c_dev, u32 reg)
{
	return __raw_readw(i2c_dev->regs + reg);
}

static void bcm2708_fill_txfifo(struct bcm2708_i2c_dev *i2c_dev)
{
	u32 val;

	for (;;) {
		if (!i2c_dev->msg_buf_remaining)
			return;
		val = bcm2708_i2c_readl(i2c_dev, BCM2708_I2C_S);
		if (!(val & BCM2708_I2C_S_TXD))
			break;
		bcm2708_i2c_writel(i2c_dev, BCM2708_I2C_FIFO,
				   *i2c_dev->msg_buf);
		i2c_dev->msg_buf++;
		i2c_dev->msg_buf_remaining--;
	}
}

static void bcm2708_drain_rxfifo(struct bcm2708_i2c_dev *i2c_dev)
{
	u32 val;

	for (;;) {
		if (!i2c_dev->msg_buf_remaining)
			return;
		val = bcm2708_i2c_readl(i2c_dev, BCM2708_I2C_S);
		if (!(val & BCM2708_I2C_S_RXD))
			break;
		*i2c_dev->msg_buf = bcm2708_i2c_readl(i2c_dev,
						      BCM2708_I2C_FIFO);
		i2c_dev->msg_buf++;
		i2c_dev->msg_buf_remaining--;
	}
}

static irqreturn_t bcm2708_i2c_isr(int this_irq, void *data)
{
	struct bcm2708_i2c_dev *i2c_dev = data;
	u32 val, err;

	val = bcm2708_i2c_readl(i2c_dev, BCM2708_I2C_S);
	bcm2708_i2c_writel(i2c_dev, BCM2708_I2C_S, val);

	err = val & (BCM2708_I2C_S_CLKT | BCM2708_I2C_S_ERR);
	if (err) {
		i2c_dev->msg_err = err;
		complete(&i2c_dev->completion);
		return IRQ_HANDLED;
	}

	if (val & BCM2708_I2C_S_RXD) {
		bcm2708_drain_rxfifo(i2c_dev);
		if (!(val & BCM2708_I2C_S_DONE))
			return IRQ_HANDLED;
	}

	if (val & BCM2708_I2C_S_DONE) {
		if (i2c_dev->msg_buf_remaining)
			i2c_dev->msg_err = BCM2708_I2C_S_LEN;
		else
			i2c_dev->msg_err = 0;
		complete(&i2c_dev->completion);
		return IRQ_HANDLED;
	}

	if (val & BCM2708_I2C_S_TXD) {
		bcm2708_fill_txfifo(i2c_dev);
		return IRQ_NONE;
	}

	return IRQ_NONE;
}

static int bcm2708_i2c_xfer_msg(struct bcm2708_i2c_dev *i2c_dev,
				struct i2c_msg *msg)
{
	u32 c;
	int ret;

	if (msg->len == 0)
		return -EINVAL;

	i2c_dev->msg_buf = msg->buf;
	i2c_dev->msg_buf_remaining = msg->len;
	INIT_COMPLETION(i2c_dev->completion);

	bcm2708_i2c_writel(i2c_dev, BCM2708_I2C_C, BCM2708_I2C_C_CLEAR);

	if (msg->flags & I2C_M_RD) {
		c = BCM2708_I2C_C_READ | BCM2708_I2C_C_INTR;
	} else {
		c = BCM2708_I2C_C_INTT;
		bcm2708_fill_txfifo(i2c_dev);
	}
	c |= BCM2708_I2C_C_ST | BCM2708_I2C_C_INTD | BCM2708_I2C_C_I2CEN;

	bcm2708_i2c_writel(i2c_dev, BCM2708_I2C_A, msg->addr);
	bcm2708_i2c_writel(i2c_dev, BCM2708_I2C_DLEN, msg->len);
	bcm2708_i2c_writel(i2c_dev, BCM2708_I2C_C, c);

	ret = wait_for_completion_timeout(&i2c_dev->completion,
					  BCM2708_I2C_TIMEOUT);
	bcm2708_i2c_writel(i2c_dev, BCM2708_I2C_C, BCM2708_I2C_C_CLEAR);
	if (WARN_ON(ret == 0)) {
		dev_err(i2c_dev->dev, "i2c transfer timed out\n");
		return -ETIMEDOUT;
	}

	if (likely(!i2c_dev->msg_err))
		return 0;

	if ((i2c_dev->msg_err & BCM2708_I2C_S_ERR) &&
	    (msg->flags & I2C_M_IGNORE_NAK))
		return 0;

	dev_err(i2c_dev->dev, "i2c transfer failed: %x\n", i2c_dev->msg_err);

	if (i2c_dev->msg_err & BCM2708_I2C_S_ERR)
		return -EREMOTEIO;
	else
		return -EIO;
}

static int bcm2708_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[],
			    int num)
{
	struct bcm2708_i2c_dev *i2c_dev = i2c_get_adapdata(adap);
	int i;
	int ret = 0;

	for (i = 0; i < num; i++) {
		ret = bcm2708_i2c_xfer_msg(i2c_dev, &msgs[i]);
		if (ret)
			break;
	}

	return ret ?: i;
}

static u32 bcm2708_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm bcm2708_i2c_algo = {
	.master_xfer	= bcm2708_i2c_xfer,
	.functionality	= bcm2708_i2c_func,
};

static int __devinit bcm2708_i2c_probe(struct platform_device *pdev)
{
	struct bcm2708_i2c_dev *i2c_dev;
	struct resource *mem, *requested, *irq;
	int ret;
	struct i2c_adapter *adap;

	i2c_dev = devm_kzalloc(&pdev->dev, sizeof(*i2c_dev), GFP_KERNEL);
	if (!i2c_dev) {
		dev_err(&pdev->dev, "Cannot allocate i2c_dev\n");
		return -ENOMEM;
	}
	platform_set_drvdata(pdev, i2c_dev);
	i2c_dev->dev = &pdev->dev;
	init_completion(&i2c_dev->completion);

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "No mem resource\n");
		return -ENODEV;
	}

	requested = devm_request_mem_region(&pdev->dev, mem->start,
					    resource_size(mem),
					    dev_name(&pdev->dev));
	if (!requested) {
		dev_err(&pdev->dev, "Could not claim register region\n");
		return -EBUSY;
	}

	i2c_dev->regs = devm_ioremap(&pdev->dev, mem->start,
				     resource_size(mem));
	if (!i2c_dev->regs) {
		dev_err(&pdev->dev, "Could not map registers\n");
		return -ENOMEM;
	}

	irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!irq) {
		dev_err(&pdev->dev, "No IRQ resource\n");
		return -ENODEV;
	}

	ret = devm_request_irq(&pdev->dev, irq->start, bcm2708_i2c_isr,
			       IRQF_SHARED, dev_name(&pdev->dev), i2c_dev);
	if (ret) {
		dev_err(&pdev->dev, "Could not request IRQ\n");
		return -ENODEV;
	}

	adap = &i2c_dev->adapter;
	i2c_set_adapdata(adap, i2c_dev);
	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_HWMON;
	strlcpy(adap->name, "bcm2708 I2C adapter", sizeof(adap->name));
	adap->algo = &bcm2708_i2c_algo;
	adap->dev.parent = &pdev->dev;
	adap->nr = -1;

	bcm2708_i2c_writel(i2c_dev, BCM2708_I2C_C, 0);

	ret = i2c_add_numbered_adapter(adap);
	if (ret) {
		dev_err(&pdev->dev, "Could not add adapter\n");
		return ret;
	}

	return 0;
}

static int bcm2708_i2c_remove(struct platform_device *pdev)
{
	struct bcm2708_i2c_dev *i2c_dev = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c_dev->adapter);

	return 0;
}

static struct platform_driver bcm2708_i2c_driver = {
	.probe		= bcm2708_i2c_probe,
	.remove		= bcm2708_i2c_remove,
	.driver		= {
		.name	= "i2c-bcm2708",
		.owner	= THIS_MODULE,
	},
};

static int __init bcm2708_i2c_init_driver(void)
{
	return platform_driver_register(&bcm2708_i2c_driver);
}
module_init(bcm2708_i2c_init_driver);

static void __exit bcm2708_i2c_exit_driver(void)
{
	platform_driver_unregister(&bcm2708_i2c_driver);
}
module_exit(bcm2708_i2c_exit_driver);

MODULE_AUTHOR("Stephen Warren <swarren@wwwdotorg.org>");
MODULE_DESCRIPTION("BCM2708 I2C bus adapter");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-bcm2708");
