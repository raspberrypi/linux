// SPDX-License-Identifier: GPL-2.0-only
/*
 * RP1 CSI-2 Driver
 *
 * Copyright (C) 2021 - Raspberry Pi Ltd.
 *
 */

#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>

#include <media/videobuf2-dma-contig.h>

#include "csi2.h"
#include "cfe.h"

#define csi2_dbg_irq(fmt, arg...)                                 \
	do {                                                      \
		if (cfe_debug_irq)                                \
			dev_dbg(csi2->v4l2_dev->dev, fmt, ##arg); \
	} while (0)
#define csi2_dbg(fmt, arg...) dev_dbg(csi2->v4l2_dev->dev, fmt, ##arg)
#define csi2_info(fmt, arg...) dev_info(csi2->v4l2_dev->dev, fmt, ##arg)
#define csi2_err(fmt, arg...) dev_err(csi2->v4l2_dev->dev, fmt, ##arg)

/* CSI2-DMA registers */
#define CSI2_STATUS		0x000
#define CSI2_QOS		0x004
#define CSI2_DISCARDS_OVERFLOW	0x008
#define CSI2_DISCARDS_INACTIVE	0x00c
#define CSI2_DISCARDS_UNMATCHED	0x010
#define CSI2_DISCARDS_LEN_LIMIT	0x014
#define CSI2_LLEV_PANICS	0x018
#define CSI2_ULEV_PANICS	0x01c
#define CSI2_IRQ_MASK		0x020
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
#define IRQ_CH_MASK(x)		(IRQ_FS(x) | IRQ_FE(x) | IRQ_FE_ACK(x) | IRQ_LE(x) | IRQ_LE_ACK(x))
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
#define PACK_LINE		BIT(29)
#define PACK_BYTES		BIT(30)
#define CH_MODE_MASK		GENMASK(2, 1)
#define VC_MASK			GENMASK(6, 5)
#define DT_MASK			GENMASK(12, 7)
#define LC_MASK			GENMASK(27, 18)

/* CHx_COMPRESSION_CONTROL */
#define COMP_OFFSET_MASK	GENMASK(15, 0)
#define COMP_SHIFT_MASK		GENMASK(19, 16)
#define COMP_MODE_MASK		GENMASK(25, 24)

static inline u32 csi2_reg_read(struct csi2_device *csi2, u32 offset)
{
	return readl(csi2->base + offset);
}

static inline void csi2_reg_write(struct csi2_device *csi2, u32 offset, u32 val)
{
	writel(val, csi2->base + offset);
}

static inline void set_field(u32 *valp, u32 field, u32 mask)
{
	u32 val = *valp;

	val &= ~mask;
	val |= (field << __ffs(mask)) & mask;
	*valp = val;
}

static int csi2_regs_show(struct seq_file *s, void *data)
{
	struct csi2_device *csi2 = s->private;
	unsigned int i;
	int ret;

	ret = pm_runtime_resume_and_get(csi2->v4l2_dev->dev);
	if (ret)
		return ret;

#define DUMP(reg) seq_printf(s, #reg " \t0x%08x\n", csi2_reg_read(csi2, reg))
#define DUMP_CH(idx, reg) seq_printf(s, #reg "(%u) \t0x%08x\n", idx, csi2_reg_read(csi2, reg(idx)))

	DUMP(CSI2_STATUS);
	DUMP(CSI2_DISCARDS_OVERFLOW);
	DUMP(CSI2_DISCARDS_INACTIVE);
	DUMP(CSI2_DISCARDS_UNMATCHED);
	DUMP(CSI2_DISCARDS_LEN_LIMIT);
	DUMP(CSI2_LLEV_PANICS);
	DUMP(CSI2_ULEV_PANICS);
	DUMP(CSI2_IRQ_MASK);
	DUMP(CSI2_CTRL);

	for (i = 0; i < CSI2_NUM_CHANNELS; ++i) {
		DUMP_CH(i, CSI2_CH_CTRL);
		DUMP_CH(i, CSI2_CH_ADDR0);
		DUMP_CH(i, CSI2_CH_ADDR1);
		DUMP_CH(i, CSI2_CH_STRIDE);
		DUMP_CH(i, CSI2_CH_LENGTH);
		DUMP_CH(i, CSI2_CH_DEBUG);
		DUMP_CH(i, CSI2_CH_FRAME_SIZE);
		DUMP_CH(i, CSI2_CH_COMP_CTRL);
		DUMP_CH(i, CSI2_CH_FE_FRAME_ID);
	}

#undef DUMP
#undef DUMP_CH

	pm_runtime_put(csi2->v4l2_dev->dev);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(csi2_regs);

void csi2_isr(struct csi2_device *csi2, bool *sof, bool *eof, bool *lci)
{
	unsigned int i;
	u32 status;

	status = csi2_reg_read(csi2, CSI2_STATUS);
	csi2_dbg_irq("ISR: STA: 0x%x\n", status);

	/* Write value back to clear the interrupts */
	csi2_reg_write(csi2, CSI2_STATUS, status);

	for (i = 0; i < CSI2_NUM_CHANNELS; i++) {
		u32 dbg;

		if ((status & IRQ_CH_MASK(i)) == 0)
			continue;

		dbg = csi2_reg_read(csi2, CSI2_CH_DEBUG(i));

		csi2_dbg_irq("ISR: [%u], %s%s%s%s%s frame: %u line: %u\n", i,
			     (status & IRQ_FS(i)) ? "FS " : "",
			     (status & IRQ_FE(i)) ? "FE " : "",
			     (status & IRQ_FE_ACK(i)) ? "FE_ACK " : "",
			     (status & IRQ_LE(i)) ? "LE " : "",
			     (status & IRQ_LE_ACK(i)) ? "LE_ACK " : "",
			     dbg >> 16,
			     csi2->num_lines[i] ?
				     ((dbg & 0xffff) % csi2->num_lines[i]) :
				     0);

		sof[i] = !!(status & IRQ_FS(i));
		eof[i] = !!(status & IRQ_FE_ACK(i));
		lci[i] = !!(status & IRQ_LE_ACK(i));
	}
}

void csi2_set_buffer(struct csi2_device *csi2, unsigned int channel,
		     dma_addr_t dmaaddr, unsigned int stride, unsigned int size)
{
	u64 addr = dmaaddr;
	/*
	 * ADDRESS0 must be written last as it triggers the double buffering
	 * mechanism for all buffer registers within the hardware.
	 */
	addr >>= 4;
	csi2_reg_write(csi2, CSI2_CH_LENGTH(channel), size >> 4);
	csi2_reg_write(csi2, CSI2_CH_STRIDE(channel), stride >> 4);
	csi2_reg_write(csi2, CSI2_CH_ADDR1(channel), addr >> 32);
	csi2_reg_write(csi2, CSI2_CH_ADDR0(channel), addr & 0xffffffff);
}

void csi2_set_compression(struct csi2_device *csi2, unsigned int channel,
			  enum csi2_compression_mode mode, unsigned int shift,
			  unsigned int offset)
{
	u32 compression = 0;

	set_field(&compression, COMP_OFFSET_MASK, offset);
	set_field(&compression, COMP_SHIFT_MASK, shift);
	set_field(&compression, COMP_MODE_MASK, mode);
	csi2_reg_write(csi2, CSI2_CH_COMP_CTRL(channel), compression);
}

void csi2_start_channel(struct csi2_device *csi2, unsigned int channel,
			u16 dt, enum csi2_mode mode, bool auto_arm,
			bool pack_bytes, unsigned int width,
			unsigned int height)
{
	u32 ctrl;

	csi2_dbg("%s [%u]\n", __func__, channel);

	/*
	 * Disable the channel, but ensure N != 0!  Otherwise we end up with a
	 * spurious LE + LE_ACK interrupt when re-enabling the channel.
	 */
	csi2_reg_write(csi2, CSI2_CH_CTRL(channel), 0x100 << __ffs(LC_MASK));
	csi2_reg_write(csi2, CSI2_CH_DEBUG(channel), 0);
	csi2_reg_write(csi2, CSI2_STATUS, IRQ_CH_MASK(channel));

	/* Enable channel and FS/FE/LE interrupts. */
	ctrl = DMA_EN | IRQ_EN_FS | IRQ_EN_FE_ACK | IRQ_EN_LE_ACK | PACK_LINE;
	/* PACK_BYTES ensures no striding for embedded data. */
	if (pack_bytes)
		ctrl |= PACK_BYTES;

	if (auto_arm)
		ctrl |= AUTO_ARM;

	if (width && height) {
		int line_int_freq = height >> 2;

		line_int_freq = min(max(0x80, line_int_freq), 0x3ff);
		set_field(&ctrl, line_int_freq, LC_MASK);
		set_field(&ctrl, mode, CH_MODE_MASK);
		csi2_reg_write(csi2, CSI2_CH_FRAME_SIZE(channel),
			       (height << 16) | width);
	} else {
		/*
		 * Do not disable line interrupts for the embedded data channel,
		 * set it to the maximum value.  This avoids spamming the ISR
		 * with spurious line interrupts.
		 */
		set_field(&ctrl, 0x3ff, LC_MASK);
		set_field(&ctrl, 0x00, CH_MODE_MASK);
	}

	set_field(&ctrl, dt, DT_MASK);
	csi2_reg_write(csi2, CSI2_CH_CTRL(channel), ctrl);
	csi2->num_lines[channel] = height;
}

void csi2_stop_channel(struct csi2_device *csi2, unsigned int channel)
{
	csi2_dbg("%s [%u]\n", __func__, channel);

	/* Channel disable.  Use FORCE to allow stopping mid-frame. */
	csi2_reg_write(csi2, CSI2_CH_CTRL(channel),
		       (0x100 << __ffs(LC_MASK)) | FORCE);
	/* Latch the above change by writing to the ADDR0 register. */
	csi2_reg_write(csi2, CSI2_CH_ADDR0(channel), 0);
	/* Write this again, the HW needs it! */
	csi2_reg_write(csi2, CSI2_CH_ADDR0(channel), 0);
}

void csi2_open_rx(struct csi2_device *csi2)
{
	dphy_start(&csi2->dphy);

	if (!csi2->multipacket_line)
		csi2_reg_write(csi2, CSI2_CTRL, EOP_IS_EOL);
}

void csi2_close_rx(struct csi2_device *csi2)
{
	dphy_stop(&csi2->dphy);
}

static struct csi2_device *to_csi2_device(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct csi2_device, sd);
}

static int csi2_init_cfg(struct v4l2_subdev *sd,
			 struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt;

	for (unsigned int i = 0; i < CSI2_NUM_CHANNELS; ++i) {
		const struct v4l2_mbus_framefmt *def_fmt;

		/* CSI2_CH1_EMBEDDED */
		if (i == 1)
			def_fmt = &cfe_default_meta_format;
		else
			def_fmt = &cfe_default_format;

		fmt = v4l2_subdev_get_pad_format(sd, state, i);
		*fmt = *def_fmt;

		fmt = v4l2_subdev_get_pad_format(sd, state, i + CSI2_NUM_CHANNELS);
		*fmt = *def_fmt;
	}

	return 0;
}

static int csi2_pad_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *format)
{
	struct v4l2_mbus_framefmt *fmt;
	const struct cfe_fmt *cfe_fmt;

	/* TODO: format validation */

	cfe_fmt = find_format_by_code(format->format.code);
	if (!cfe_fmt)
		cfe_fmt = find_format_by_code(MEDIA_BUS_FMT_SBGGR10_1X10);

	format->format.code = cfe_fmt->code;

	fmt = v4l2_subdev_get_pad_format(sd, state, format->pad);
	*fmt = format->format;

	if (format->pad < CSI2_NUM_CHANNELS) {
		/* Propagate to the source pad */
		fmt = v4l2_subdev_get_pad_format(sd, state,
						 format->pad + CSI2_NUM_CHANNELS);
		*fmt = format->format;
	}

	return 0;
}

static int csi2_link_validate(struct v4l2_subdev *sd, struct media_link *link,
			      struct v4l2_subdev_format *source_fmt,
			      struct v4l2_subdev_format *sink_fmt)
{
	struct csi2_device *csi2 = to_csi2_device(sd);

	csi2_dbg("%s: link \"%s\":%u -> \"%s\":%u\n", __func__,
		 link->source->entity->name, link->source->index,
		 link->sink->entity->name, link->sink->index);

	if ((link->source->entity == &csi2->sd.entity &&
	     link->source->index == 1) ||
	    (link->sink->entity == &csi2->sd.entity &&
	     link->sink->index == 1)) {
		csi2_dbg("Ignore metadata pad for now\n");
		return 0;
	}

	/* The width, height and code must match. */
	if (source_fmt->format.width != sink_fmt->format.width ||
	    source_fmt->format.width != sink_fmt->format.width ||
	    source_fmt->format.code != sink_fmt->format.code) {
		csi2_err("%s: format does not match (source %ux%u 0x%x, sink %ux%u 0x%x)\n",
			 __func__,
			 source_fmt->format.width, source_fmt->format.height,
			 source_fmt->format.code,
			 sink_fmt->format.width, sink_fmt->format.height,
			 sink_fmt->format.code);
		return -EPIPE;
	}

	return 0;
}

static const struct v4l2_subdev_pad_ops csi2_subdev_pad_ops = {
	.init_cfg = csi2_init_cfg,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = csi2_pad_set_fmt,
	.link_validate = csi2_link_validate,
};

static const struct media_entity_operations csi2_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_ops csi2_subdev_ops = {
	.pad = &csi2_subdev_pad_ops,
};

int csi2_init(struct csi2_device *csi2, struct dentry *debugfs)
{
	unsigned int i, ret;

	csi2->dphy.dev = csi2->v4l2_dev->dev;
	dphy_probe(&csi2->dphy);

	debugfs_create_file("csi2_regs", 0444, debugfs, csi2, &csi2_regs_fops);

	for (i = 0; i < CSI2_NUM_CHANNELS * 2; i++)
		csi2->pad[i].flags = i < CSI2_NUM_CHANNELS ?
				     MEDIA_PAD_FL_SINK : MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&csi2->sd.entity, ARRAY_SIZE(csi2->pad),
				     csi2->pad);
	if (ret)
		return ret;

	/* Initialize subdev */
	v4l2_subdev_init(&csi2->sd, &csi2_subdev_ops);
	csi2->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	csi2->sd.entity.ops = &csi2_entity_ops;
	csi2->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	csi2->sd.owner = THIS_MODULE;
	snprintf(csi2->sd.name, sizeof(csi2->sd.name), "csi2");

	ret = v4l2_subdev_init_finalize(&csi2->sd);
	if (ret)
		goto err_entity_cleanup;

	ret = v4l2_device_register_subdev(csi2->v4l2_dev, &csi2->sd);
	if (ret) {
		csi2_err("Failed register csi2 subdev (%d)\n", ret);
		goto err_subdev_cleanup;
	}

	return 0;

err_subdev_cleanup:
	v4l2_subdev_cleanup(&csi2->sd);
err_entity_cleanup:
	media_entity_cleanup(&csi2->sd.entity);

	return ret;
}

void csi2_uninit(struct csi2_device *csi2)
{
	v4l2_device_unregister_subdev(&csi2->sd);
	v4l2_subdev_cleanup(&csi2->sd);
	media_entity_cleanup(&csi2->sd.entity);
}
