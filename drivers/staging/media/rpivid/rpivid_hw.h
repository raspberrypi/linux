/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Raspberry Pi HEVC driver
 *
 * Copyright (C) 2020 Raspberry Pi (Trading) Ltd
 *
 * Based on the Cedrus VPU driver, that is:
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 */

#ifndef _RPIVID_HW_H_
#define _RPIVID_HW_H_

struct rpivid_hw_irq_ent {
	struct rpivid_hw_irq_ent *next;
	rpivid_irq_callback cb;
	void *v;
};

/* Phase 1 Register offsets */

#define RPI_SPS0 0
#define RPI_SPS1 4
#define RPI_PPS 8
#define RPI_SLICE 12
#define RPI_TILESTART 16
#define RPI_TILEEND 20
#define RPI_SLICESTART 24
#define RPI_MODE 28
#define RPI_LEFT0 32
#define RPI_LEFT1 36
#define RPI_LEFT2 40
#define RPI_LEFT3 44
#define RPI_QP 48
#define RPI_CONTROL 52
#define RPI_STATUS 56
#define RPI_VERSION 60
#define RPI_BFBASE 64
#define RPI_BFNUM 68
#define RPI_BFCONTROL 72
#define RPI_BFSTATUS 76
#define RPI_PUWBASE 80
#define RPI_PUWSTRIDE 84
#define RPI_COEFFWBASE 88
#define RPI_COEFFWSTRIDE 92
#define RPI_SLICECMDS 96
#define RPI_BEGINTILEEND 100
#define RPI_TRANSFER 104
#define RPI_CFBASE 108
#define RPI_CFNUM 112
#define RPI_CFSTATUS 116

/* Phase 2 Register offsets */

#define RPI_PURBASE 0x8000
#define RPI_PURSTRIDE 0x8004
#define RPI_COEFFRBASE 0x8008
#define RPI_COEFFRSTRIDE 0x800C
#define RPI_NUMROWS 0x8010
#define RPI_CONFIG2 0x8014
#define RPI_OUTYBASE 0x8018
#define RPI_OUTYSTRIDE 0x801C
#define RPI_OUTCBASE 0x8020
#define RPI_OUTCSTRIDE 0x8024
#define RPI_STATUS2 0x8028
#define RPI_FRAMESIZE 0x802C
#define RPI_MVBASE 0x8030
#define RPI_MVSTRIDE 0x8034
#define RPI_COLBASE 0x8038
#define RPI_COLSTRIDE 0x803C
#define RPI_CURRPOC 0x8040

/*
 * Write a general register value
 * Order is unimportant
 */
static inline void apb_write(const struct rpivid_dev * const dev,
			     const unsigned int offset, const u32 val)
{
	writel_relaxed(val, dev->base_h265 + offset);
}

/* Write the final register value that actually starts the phase */
static inline void apb_write_final(const struct rpivid_dev * const dev,
				   const unsigned int offset, const u32 val)
{
	writel(val, dev->base_h265 + offset);
}

static inline u32 apb_read(const struct rpivid_dev * const dev,
			   const unsigned int offset)
{
	return readl(dev->base_h265 + offset);
}

static inline void irq_write(const struct rpivid_dev * const dev,
			     const unsigned int offset, const u32 val)
{
	writel(val, dev->base_irq + offset);
}

static inline u32 irq_read(const struct rpivid_dev * const dev,
			   const unsigned int offset)
{
	return readl(dev->base_irq + offset);
}

static inline void apb_write_vc_addr(const struct rpivid_dev * const dev,
				     const unsigned int offset,
				     const dma_addr_t a)
{
	apb_write(dev, offset, (u32)(a >> 6));
}

static inline void apb_write_vc_addr_final(const struct rpivid_dev * const dev,
					   const unsigned int offset,
					   const dma_addr_t a)
{
	apb_write_final(dev, offset, (u32)(a >> 6));
}

static inline void apb_write_vc_len(const struct rpivid_dev * const dev,
				    const unsigned int offset,
				    const unsigned int x)
{
	apb_write(dev, offset, (x + 63) >> 6);
}

/* *ARG_IC_ICTRL - Interrupt control for ARGON Core*
 * Offset (byte space) = 40'h2b10000
 * Physical Address (byte space) = 40'h7eb10000
 * Verilog Macro Address = `ARG_IC_REG_START + `ARGON_INTCTRL_ICTRL
 * Reset Value = 32'b100x100x_100xxxxx_xxxxxxx0_x100x100
 * Access = RW (32-bit only)
 * Interrupt control logic for ARGON Core.
 */
#define ARG_IC_ICTRL 0

/* acc=LWC ACTIVE1_INT FIELD ACCESS: LWC
 *
 * Interrupt 1
 * This is set and held when an hevc_active1 interrupt edge is detected
 * The polarity of the edge is set by the ACTIVE1_EDGE field
 * Write a 1 to this bit to clear down the latched interrupt
 * The latched interrupt is only enabled out onto the interrupt line if
 * ACTIVE1_EN is set
 * Reset value is *0* decimal.
 */
#define ARG_IC_ICTRL_ACTIVE1_INT_SET		BIT(0)

/* ACTIVE1_EDGE Sets the polarity of the interrupt edge detection logic
 * This logic detects edges of the hevc_active1 line from the argon core
 * 0 = negedge, 1 = posedge
 * Reset value is *0* decimal.
 */
#define ARG_IC_ICTRL_ACTIVE1_EDGE_SET		BIT(1)

/* ACTIVE1_EN Enables ACTIVE1_INT out onto the argon interrupt line.
 * If this isn't set, the interrupt logic will work but no interrupt will be
 * set to the interrupt controller
 * Reset value is *1* decimal.
 *
 * [JC] The above appears to be a lie - if unset then b0 is never set
 */
#define ARG_IC_ICTRL_ACTIVE1_EN_SET		BIT(2)

/* acc=RO ACTIVE1_STATUS FIELD ACCESS: RO
 *
 * The current status of the hevc_active1 signal
 */
#define ARG_IC_ICTRL_ACTIVE1_STATUS_SET		BIT(3)

/* acc=LWC ACTIVE2_INT FIELD ACCESS: LWC
 *
 * Interrupt 2
 * This is set and held when an hevc_active2 interrupt edge is detected
 * The polarity of the edge is set by the ACTIVE2_EDGE field
 * Write a 1 to this bit to clear down the latched interrupt
 * The latched interrupt is only enabled out onto the interrupt line if
 * ACTIVE2_EN is set
 * Reset value is *0* decimal.
 */
#define ARG_IC_ICTRL_ACTIVE2_INT_SET		BIT(4)

/* ACTIVE2_EDGE Sets the polarity of the interrupt edge detection logic
 * This logic detects edges of the hevc_active2 line from the argon core
 * 0 = negedge, 1 = posedge
 * Reset value is *0* decimal.
 */
#define ARG_IC_ICTRL_ACTIVE2_EDGE_SET		BIT(5)

/* ACTIVE2_EN Enables ACTIVE2_INT out onto the argon interrupt line.
 * If this isn't set, the interrupt logic will work but no interrupt will be
 * set to the interrupt controller
 * Reset value is *1* decimal.
 */
#define ARG_IC_ICTRL_ACTIVE2_EN_SET		BIT(6)

/* acc=RO ACTIVE2_STATUS FIELD ACCESS: RO
 *
 * The current status of the hevc_active2 signal
 */
#define ARG_IC_ICTRL_ACTIVE2_STATUS_SET		BIT(7)

/* TEST_INT Forces the argon int high for test purposes.
 * Reset value is *0* decimal.
 */
#define ARG_IC_ICTRL_TEST_INT			BIT(8)
#define ARG_IC_ICTRL_SPARE			BIT(9)

/* acc=RO VP9_INTERRUPT_STATUS FIELD ACCESS: RO
 *
 * The current status of the vp9_interrupt signal
 */
#define ARG_IC_ICTRL_VP9_INTERRUPT_STATUS	BIT(10)

/* AIO_INT_ENABLE 1 = Or the AIO int in with the Argon int so the VPU can see
 * it
 * 0 = the AIO int is masked. (It should still be connected to the GIC though).
 */
#define ARG_IC_ICTRL_AIO_INT_ENABLE		BIT(20)
#define ARG_IC_ICTRL_H264_ACTIVE_INT		BIT(21)
#define ARG_IC_ICTRL_H264_ACTIVE_EDGE		BIT(22)
#define ARG_IC_ICTRL_H264_ACTIVE_EN		BIT(23)
#define ARG_IC_ICTRL_H264_ACTIVE_STATUS		BIT(24)
#define ARG_IC_ICTRL_H264_INTERRUPT_INT		BIT(25)
#define ARG_IC_ICTRL_H264_INTERRUPT_EDGE	BIT(26)
#define ARG_IC_ICTRL_H264_INTERRUPT_EN		BIT(27)

/* acc=RO H264_INTERRUPT_STATUS FIELD ACCESS: RO
 *
 * The current status of the h264_interrupt signal
 */
#define ARG_IC_ICTRL_H264_INTERRUPT_STATUS	BIT(28)

/* acc=LWC VP9_INTERRUPT_INT FIELD ACCESS: LWC
 *
 * Interrupt 1
 * This is set and held when an vp9_interrupt interrupt edge is detected
 * The polarity of the edge is set by the VP9_INTERRUPT_EDGE field
 * Write a 1 to this bit to clear down the latched interrupt
 * The latched interrupt is only enabled out onto the interrupt line if
 * VP9_INTERRUPT_EN is set
 * Reset value is *0* decimal.
 */
#define ARG_IC_ICTRL_VP9_INTERRUPT_INT		BIT(29)

/* VP9_INTERRUPT_EDGE Sets the polarity of the interrupt edge detection logic
 * This logic detects edges of the vp9_interrupt line from the argon h264 core
 * 0 = negedge, 1 = posedge
 * Reset value is *0* decimal.
 */
#define ARG_IC_ICTRL_VP9_INTERRUPT_EDGE		BIT(30)

/* VP9_INTERRUPT_EN Enables VP9_INTERRUPT_INT out onto the argon interrupt line.
 * If this isn't set, the interrupt logic will work but no interrupt will be
 * set to the interrupt controller
 * Reset value is *1* decimal.
 */
#define ARG_IC_ICTRL_VP9_INTERRUPT_EN		BIT(31)

/* Bits 19:12, 11 reserved - read ?, write 0 */
#define ARG_IC_ICTRL_SET_ZERO_MASK		((0xff << 12) | BIT(11))

/* All IRQ bits */
#define ARG_IC_ICTRL_ALL_IRQ_MASK   (\
		ARG_IC_ICTRL_VP9_INTERRUPT_INT  |\
		ARG_IC_ICTRL_H264_INTERRUPT_INT |\
		ARG_IC_ICTRL_ACTIVE1_INT_SET    |\
		ARG_IC_ICTRL_ACTIVE2_INT_SET)

/* Regulate claim Q */
void rpivid_hw_irq_active1_enable_claim(struct rpivid_dev *dev,
					int n);
/* Auto release once all CBs called */
void rpivid_hw_irq_active1_claim(struct rpivid_dev *dev,
				 struct rpivid_hw_irq_ent *ient,
				 rpivid_irq_callback ready_cb, void *ctx);
/* May only be called in claim cb */
void rpivid_hw_irq_active1_irq(struct rpivid_dev *dev,
			       struct rpivid_hw_irq_ent *ient,
			       rpivid_irq_callback irq_cb, void *ctx);
/* May only be called in irq cb */
void rpivid_hw_irq_active1_thread(struct rpivid_dev *dev,
				  struct rpivid_hw_irq_ent *ient,
				  rpivid_irq_callback thread_cb, void *ctx);

/* Auto release once all CBs called */
void rpivid_hw_irq_active2_claim(struct rpivid_dev *dev,
				 struct rpivid_hw_irq_ent *ient,
				 rpivid_irq_callback ready_cb, void *ctx);
/* May only be called in claim cb */
void rpivid_hw_irq_active2_irq(struct rpivid_dev *dev,
			       struct rpivid_hw_irq_ent *ient,
			       rpivid_irq_callback irq_cb, void *ctx);

int rpivid_hw_probe(struct rpivid_dev *dev);
void rpivid_hw_remove(struct rpivid_dev *dev);

#endif
