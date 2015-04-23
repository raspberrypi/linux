/*
 *  linux/drivers/video/bcm2708_fb.c
 *
 * Copyright (C) 2010 Broadcom
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 * Broadcom simple framebuffer driver
 *
 * This file is derived from cirrusfb.c
 * Copyright 1999-2001 Jeff Garzik <jgarzik@pobox.com>
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/printk.h>
#include <linux/console.h>
#include <linux/debugfs.h>

#include <mach/dma.h>
#include <mach/platform.h>
#include <mach/vcio.h>

#include <asm/sizes.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>

//#define BCM2708_FB_DEBUG
#define MODULE_NAME "bcm2708_fb"

#ifdef BCM2708_FB_DEBUG
#define print_debug(fmt,...) pr_debug("%s:%s:%d: "fmt, MODULE_NAME, __func__, __LINE__, ##__VA_ARGS__)
#else
#define print_debug(fmt,...)
#endif

/* This is limited to 16 characters when displayed by X startup */
static const char *bcm2708_name = "BCM2708 FB";

#define DRIVER_NAME "bcm2708_fb"

static int fbwidth = 800;  /* module parameter */
static int fbheight = 480; /* module parameter */
static int fbdepth = 16;   /* module parameter */
static int fbswap = 0;     /* module parameter */

static u32 dma_busy_wait_threshold = 1<<15;
module_param(dma_busy_wait_threshold, int, 0644);
MODULE_PARM_DESC(dma_busy_wait_threshold, "Busy-wait for DMA completion below this area");

/* this data structure describes each frame buffer device we find */

struct fbinfo_s {
	u32 xres, yres, xres_virtual, yres_virtual;
	u32 pitch, bpp;
	u32 xoffset, yoffset;
	u32 base;
	u32 screen_size;
	u16 cmap[256];
};

struct bcm2708_fb_stats {
	struct debugfs_regset32 regset;
	u32 dma_copies;
	u32 dma_irqs;
};

struct bcm2708_fb {
	struct fb_info fb;
	struct platform_device *dev;
	struct fbinfo_s *info;
	dma_addr_t dma;
	u32 cmap[16];
	int dma_chan;
	int dma_irq;
	void __iomem *dma_chan_base;
	void *cb_base;		/* DMA control blocks */
	dma_addr_t cb_handle;
	struct dentry *debugfs_dir;
	wait_queue_head_t dma_waitq;
	struct bcm2708_fb_stats stats;
	unsigned long fb_bus_address;
};

#define to_bcm2708(info)	container_of(info, struct bcm2708_fb, fb)

static void bcm2708_fb_debugfs_deinit(struct bcm2708_fb *fb)
{
	debugfs_remove_recursive(fb->debugfs_dir);
	fb->debugfs_dir = NULL;
}

static int bcm2708_fb_debugfs_init(struct bcm2708_fb *fb)
{
	static struct debugfs_reg32 stats_registers[] = {
		{
			"dma_copies",
			offsetof(struct bcm2708_fb_stats, dma_copies)
		},
		{
			"dma_irqs",
			offsetof(struct bcm2708_fb_stats, dma_irqs)
		},
	};

	fb->debugfs_dir = debugfs_create_dir(DRIVER_NAME, NULL);
	if (!fb->debugfs_dir) {
		pr_warn("%s: could not create debugfs entry\n",
			__func__);
		return -EFAULT;
	}

	fb->stats.regset.regs = stats_registers;
	fb->stats.regset.nregs = ARRAY_SIZE(stats_registers);
	fb->stats.regset.base = &fb->stats;

	if (!debugfs_create_regset32(
		"stats", 0444, fb->debugfs_dir, &fb->stats.regset)) {
		pr_warn("%s: could not create statistics registers\n",
			__func__);
		goto fail;
	}
	return 0;

fail:
	bcm2708_fb_debugfs_deinit(fb);
	return -EFAULT;
}

static int bcm2708_fb_set_bitfields(struct fb_var_screeninfo *var)
{
	int ret = 0;

	memset(&var->transp, 0, sizeof(var->transp));

	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;

	switch (var->bits_per_pixel) {
	case 1:
	case 2:
	case 4:
	case 8:
		var->red.length = var->bits_per_pixel;
		var->red.offset = 0;
		var->green.length = var->bits_per_pixel;
		var->green.offset = 0;
		var->blue.length = var->bits_per_pixel;
		var->blue.offset = 0;
		break;
	case 16:
		var->red.length = 5;
		var->blue.length = 5;
		/*
		 * Green length can be 5 or 6 depending whether
		 * we're operating in RGB555 or RGB565 mode.
		 */
		if (var->green.length != 5 && var->green.length != 6)
			var->green.length = 6;
		break;
	case 24:
		var->red.length = 8;
		var->blue.length = 8;
		var->green.length = 8;
		break;
	case 32:
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->transp.length = 8;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	/*
	 * >= 16bpp displays have separate colour component bitfields
	 * encoded in the pixel data.  Calculate their position from
	 * the bitfield length defined above.
	 */
	if (ret == 0 && var->bits_per_pixel >= 24 && fbswap) {
		var->blue.offset = 0;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;
		var->transp.offset = var->red.offset + var->red.length;
	} else if (ret == 0 && var->bits_per_pixel >= 24) {
		var->red.offset = 0;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;
		var->transp.offset = var->blue.offset + var->blue.length;
	} else if (ret == 0 && var->bits_per_pixel >= 16) {
		var->blue.offset = 0;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;
		var->transp.offset = var->red.offset + var->red.length;
	}

	return ret;
}

static int bcm2708_fb_check_var(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	/* info input, var output */
	int yres;

	/* info input, var output */
	print_debug("bcm2708_fb_check_var info(%p) %dx%d (%dx%d), %d, %d\n", info,
		info->var.xres, info->var.yres, info->var.xres_virtual,
		info->var.yres_virtual, (int)info->screen_size,
		info->var.bits_per_pixel);
	print_debug("bcm2708_fb_check_var var(%p) %dx%d (%dx%d), %d\n", var,
		var->xres, var->yres, var->xres_virtual, var->yres_virtual,
		var->bits_per_pixel);

	if (!var->bits_per_pixel)
		var->bits_per_pixel = 16;

	if (bcm2708_fb_set_bitfields(var) != 0) {
		pr_err("bcm2708_fb_check_var: invalid bits_per_pixel %d\n",
		     var->bits_per_pixel);
		return -EINVAL;
	}


	if (var->xres_virtual < var->xres)
		var->xres_virtual = var->xres;
	/* use highest possible virtual resolution */
	if (var->yres_virtual == -1) {
		var->yres_virtual = 480;

		pr_err
		    ("bcm2708_fb_check_var: virtual resolution set to maximum of %dx%d\n",
		     var->xres_virtual, var->yres_virtual);
	}
	if (var->yres_virtual < var->yres)
		var->yres_virtual = var->yres;

	if (var->xoffset < 0)
		var->xoffset = 0;
	if (var->yoffset < 0)
		var->yoffset = 0;

	/* truncate xoffset and yoffset to maximum if too high */
	if (var->xoffset > var->xres_virtual - var->xres)
		var->xoffset = var->xres_virtual - var->xres - 1;
	if (var->yoffset > var->yres_virtual - var->yres)
		var->yoffset = var->yres_virtual - var->yres - 1;

	yres = var->yres;
	if (var->vmode & FB_VMODE_DOUBLE)
		yres *= 2;
	else if (var->vmode & FB_VMODE_INTERLACED)
		yres = (yres + 1) / 2;

	return 0;
}

static int bcm2708_fb_set_par(struct fb_info *info)
{
	uint32_t val = 0;
	struct bcm2708_fb *fb = to_bcm2708(info);
	volatile struct fbinfo_s *fbinfo = fb->info;
	fbinfo->xres = info->var.xres;
	fbinfo->yres = info->var.yres;
	fbinfo->xres_virtual = info->var.xres_virtual;
	fbinfo->yres_virtual = info->var.yres_virtual;
	fbinfo->bpp = info->var.bits_per_pixel;
	fbinfo->xoffset = info->var.xoffset;
	fbinfo->yoffset = info->var.yoffset;
	fbinfo->base = 0;	/* filled in by VC */
	fbinfo->pitch = 0;	/* filled in by VC */

	print_debug("bcm2708_fb_set_par info(%p) %dx%d (%dx%d), %d, %d\n", info,
		info->var.xres, info->var.yres, info->var.xres_virtual,
		info->var.yres_virtual, (int)info->screen_size,
		info->var.bits_per_pixel);

	/* ensure last write to fbinfo is visible to GPU */
	wmb();

	/* inform vc about new framebuffer */
	bcm_mailbox_write(MBOX_CHAN_FB, fb->dma);

	/* TODO: replace fb driver with vchiq version */
	/* wait for response */
	bcm_mailbox_read(MBOX_CHAN_FB, &val);

	/* ensure GPU writes are visible to us */
	rmb();

        if (val == 0) {
		fb->fb.fix.line_length = fbinfo->pitch;

		if (info->var.bits_per_pixel <= 8)
			fb->fb.fix.visual = FB_VISUAL_PSEUDOCOLOR;
		else
			fb->fb.fix.visual = FB_VISUAL_TRUECOLOR;

		fb->fb_bus_address = fbinfo->base;
		fbinfo->base &= ~0xc0000000;
		fb->fb.fix.smem_start = fbinfo->base;
		fb->fb.fix.smem_len = fbinfo->pitch * fbinfo->yres_virtual;
		fb->fb.screen_size = fbinfo->screen_size;
		if (fb->fb.screen_base)
			iounmap(fb->fb.screen_base);
		fb->fb.screen_base =
			(void *)ioremap_wc(fbinfo->base, fb->fb.screen_size);
		if (!fb->fb.screen_base) {
			/* the console may currently be locked */
			console_trylock();
			console_unlock();

			BUG();		/* what can we do here */
		}
	}
	print_debug
	    ("BCM2708FB: start = %p,%p width=%d, height=%d, bpp=%d, pitch=%d size=%d success=%d\n",
	     (void *)fb->fb.screen_base, (void *)fb->fb_bus_address,
	     fbinfo->xres, fbinfo->yres, fbinfo->bpp,
	     fbinfo->pitch, (int)fb->fb.screen_size, val);

	return val;
}

static inline u32 convert_bitfield(int val, struct fb_bitfield *bf)
{
	unsigned int mask = (1 << bf->length) - 1;

	return (val >> (16 - bf->length) & mask) << bf->offset;
}


static int bcm2708_fb_setcolreg(unsigned int regno, unsigned int red,
				unsigned int green, unsigned int blue,
				unsigned int transp, struct fb_info *info)
{
	struct bcm2708_fb *fb = to_bcm2708(info);

	/*print_debug("BCM2708FB: setcolreg %d:(%02x,%02x,%02x,%02x) %x\n", regno, red, green, blue, transp, fb->fb.fix.visual);*/
	if (fb->fb.var.bits_per_pixel <= 8) {
		if (regno < 256) {
			/* blue [0:4], green [5:10], red [11:15] */
			fb->info->cmap[regno] = ((red   >> (16-5)) & 0x1f) << 11 |
						((green >> (16-6)) & 0x3f) << 5 |
						((blue  >> (16-5)) & 0x1f) << 0;
		}
		/* Hack: we need to tell GPU the palette has changed, but currently bcm2708_fb_set_par takes noticable time when called for every (256) colour */
		/* So just call it for what looks like the last colour in a list for now. */
		if (regno == 15 || regno == 255)
			bcm2708_fb_set_par(info);
        } else if (regno < 16) {
		fb->cmap[regno] = convert_bitfield(transp, &fb->fb.var.transp) |
		    convert_bitfield(blue, &fb->fb.var.blue) |
		    convert_bitfield(green, &fb->fb.var.green) |
		    convert_bitfield(red, &fb->fb.var.red);
	}
	return regno > 255;
}

static int bcm2708_fb_blank(int blank_mode, struct fb_info *info)
{
	s32 result = -1;
	u32 p[7];
	if ( 	(blank_mode == FB_BLANK_NORMAL) || 
		(blank_mode == FB_BLANK_UNBLANK)) {

		p[0] = 28; //  size = sizeof u32 * length of p
		p[1] = VCMSG_PROCESS_REQUEST; // process request
		p[2] = VCMSG_SET_BLANK_SCREEN; // (the tag id)
		p[3] = 4; // (size of the response buffer)
		p[4] = 4; // (size of the request data)
		p[5] = blank_mode;
		p[6] = VCMSG_PROPERTY_END; // end tag
	
		bcm_mailbox_property(&p, p[0]);
	
		if ( p[1] == VCMSG_REQUEST_SUCCESSFUL )
			result = 0;
		else
			pr_err("bcm2708_fb_blank(%d) returns=%d p[1]=0x%x\n", blank_mode, p[5], p[1]);
	}
	return result;
}

static int bcm2708_fb_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
	s32 result = -1;
	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;
	result = bcm2708_fb_set_par(info);
	if (result != 0)
		pr_err("bcm2708_fb_pan_display(%d,%d) returns=%d\n", var->xoffset, var->yoffset, result);
	return result;
}

static int bcm2708_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	s32 result = -1;
	u32 p[7];
	if (cmd == FBIO_WAITFORVSYNC) {
		p[0] = 28; //  size = sizeof u32 * length of p
		p[1] = VCMSG_PROCESS_REQUEST; // process request
		p[2] = VCMSG_SET_VSYNC; // (the tag id)
		p[3] = 4; // (size of the response buffer)
		p[4] = 4; // (size of the request data)
		p[5] = 0; // dummy
		p[6] = VCMSG_PROPERTY_END; // end tag

		bcm_mailbox_property(&p, p[0]);

		if ( p[1] == VCMSG_REQUEST_SUCCESSFUL )
			result = 0;
		else
			pr_err("bcm2708_fb_ioctl %x,%lx returns=%d p[1]=0x%x\n", cmd, arg, p[5], p[1]);
	}
	return result;
}
static void bcm2708_fb_fillrect(struct fb_info *info,
				const struct fb_fillrect *rect)
{
	/* (is called) print_debug("bcm2708_fb_fillrect\n"); */
	cfb_fillrect(info, rect);
}

/* A helper function for configuring dma control block */
static void set_dma_cb(struct bcm2708_dma_cb *cb,
		       int        burst_size,
		       dma_addr_t dst,
		       int        dst_stride,
		       dma_addr_t src,
		       int        src_stride,
		       int        w,
		       int        h)
{
	cb->info = BCM2708_DMA_BURST(burst_size) | BCM2708_DMA_S_WIDTH |
		   BCM2708_DMA_S_INC | BCM2708_DMA_D_WIDTH |
		   BCM2708_DMA_D_INC | BCM2708_DMA_TDMODE;
	cb->dst = dst;
	cb->src = src;
	/*
	 * This is not really obvious from the DMA documentation,
	 * but the top 16 bits must be programmmed to "height -1"
	 * and not "height" in 2D mode.
	 */
	cb->length = ((h - 1) << 16) | w;
	cb->stride = ((dst_stride - w) << 16) | (u16)(src_stride - w);
	cb->pad[0] = 0;
	cb->pad[1] = 0;
}

static void bcm2708_fb_copyarea(struct fb_info *info,
				const struct fb_copyarea *region)
{
	struct bcm2708_fb *fb = to_bcm2708(info);
	struct bcm2708_dma_cb *cb = fb->cb_base;
	int bytes_per_pixel = (info->var.bits_per_pixel + 7) >> 3;
	/* Channel 0 supports larger bursts and is a bit faster */
	int burst_size = (fb->dma_chan == 0) ? 8 : 2;
	int pixels = region->width * region->height;

	/* Fallback to cfb_copyarea() if we don't like something */
	if (in_atomic() ||
	    bytes_per_pixel > 4 ||
	    info->var.xres * info->var.yres > 1920 * 1200 ||
	    region->width <= 0 || region->width > info->var.xres ||
	    region->height <= 0 || region->height > info->var.yres ||
	    region->sx < 0 || region->sx >= info->var.xres ||
	    region->sy < 0 || region->sy >= info->var.yres ||
	    region->dx < 0 || region->dx >= info->var.xres ||
	    region->dy < 0 || region->dy >= info->var.yres ||
	    region->sx + region->width > info->var.xres ||
	    region->dx + region->width > info->var.xres ||
	    region->sy + region->height > info->var.yres ||
	    region->dy + region->height > info->var.yres) {
		cfb_copyarea(info, region);
		return;
	}

	if (region->dy == region->sy && region->dx > region->sx) {
		/*
		 * A difficult case of overlapped copy. Because DMA can't
		 * copy individual scanlines in backwards direction, we need
		 * two-pass processing. We do it by programming a chain of dma
		 * control blocks in the first 16K part of the buffer and use
		 * the remaining 48K as the intermediate temporary scratch
		 * buffer. The buffer size is sufficient to handle up to
		 * 1920x1200 resolution at 32bpp pixel depth.
		 */
		int y;
		dma_addr_t control_block_pa = fb->cb_handle;
		dma_addr_t scratchbuf = fb->cb_handle + 16 * 1024;
		int scanline_size = bytes_per_pixel * region->width;
		int scanlines_per_cb = (64 * 1024 - 16 * 1024) / scanline_size;

		for (y = 0; y < region->height; y += scanlines_per_cb) {
			dma_addr_t src =
				fb->fb_bus_address +
				bytes_per_pixel * region->sx +
				(region->sy + y) * fb->fb.fix.line_length;
			dma_addr_t dst =
				fb->fb_bus_address +
				bytes_per_pixel * region->dx +
				(region->dy + y) * fb->fb.fix.line_length;

			if (region->height - y < scanlines_per_cb)
				scanlines_per_cb = region->height - y;

			set_dma_cb(cb, burst_size, scratchbuf, scanline_size,
				   src, fb->fb.fix.line_length,
				   scanline_size, scanlines_per_cb);
			control_block_pa += sizeof(struct bcm2708_dma_cb);
			cb->next = control_block_pa;
			cb++;

			set_dma_cb(cb, burst_size, dst, fb->fb.fix.line_length,
				   scratchbuf, scanline_size,
				   scanline_size, scanlines_per_cb);
			control_block_pa += sizeof(struct bcm2708_dma_cb);
			cb->next = control_block_pa;
			cb++;
		}
		/* move the pointer back to the last dma control block */
		cb--;
	} else {
		/* A single dma control block is enough. */
		int sy, dy, stride;
		if (region->dy <= region->sy) {
			/* processing from top to bottom */
			dy = region->dy;
			sy = region->sy;
			stride = fb->fb.fix.line_length;
		} else {
			/* processing from bottom to top */
			dy = region->dy + region->height - 1;
			sy = region->sy + region->height - 1;
			stride = -fb->fb.fix.line_length;
		}
		set_dma_cb(cb, burst_size,
			   fb->fb_bus_address + dy * fb->fb.fix.line_length +
						   bytes_per_pixel * region->dx,
			   stride,
			   fb->fb_bus_address + sy * fb->fb.fix.line_length +
						   bytes_per_pixel * region->sx,
			   stride,
			   region->width * bytes_per_pixel,
			   region->height);
	}

	/* end of dma control blocks chain */
	cb->next = 0;


	if (pixels < dma_busy_wait_threshold) {
		bcm_dma_start(fb->dma_chan_base, fb->cb_handle);
		bcm_dma_wait_idle(fb->dma_chan_base);
	} else {
		void __iomem *dma_chan = fb->dma_chan_base;
		cb->info |= BCM2708_DMA_INT_EN;
		bcm_dma_start(fb->dma_chan_base, fb->cb_handle);
		while (bcm_dma_is_busy(dma_chan)) {
			wait_event_interruptible(
				fb->dma_waitq,
				!bcm_dma_is_busy(dma_chan));
		}
		fb->stats.dma_irqs++;
	}
	fb->stats.dma_copies++;
}

static void bcm2708_fb_imageblit(struct fb_info *info,
				 const struct fb_image *image)
{
	/* (is called) print_debug("bcm2708_fb_imageblit\n"); */
	cfb_imageblit(info, image);
}

static irqreturn_t bcm2708_fb_dma_irq(int irq, void *cxt)
{
	struct bcm2708_fb *fb = cxt;

	/* FIXME: should read status register to check if this is
	 * actually interrupting us or not, in case this interrupt
	 * ever becomes shared amongst several DMA channels
	 *
	 * readl(dma_chan_base + BCM2708_DMA_CS) & BCM2708_DMA_IRQ;
	 */

	/* acknowledge the interrupt */
	writel(BCM2708_DMA_INT, fb->dma_chan_base + BCM2708_DMA_CS);

	wake_up(&fb->dma_waitq);
	return IRQ_HANDLED;
}

static struct fb_ops bcm2708_fb_ops = {
	.owner = THIS_MODULE,
	.fb_check_var = bcm2708_fb_check_var,
	.fb_set_par = bcm2708_fb_set_par,
	.fb_setcolreg = bcm2708_fb_setcolreg,
	.fb_blank = bcm2708_fb_blank,
	.fb_fillrect = bcm2708_fb_fillrect,
	.fb_copyarea = bcm2708_fb_copyarea,
	.fb_imageblit = bcm2708_fb_imageblit,
	.fb_pan_display = bcm2708_fb_pan_display,
	.fb_ioctl = bcm2708_ioctl,
};

static int bcm2708_fb_register(struct bcm2708_fb *fb)
{
	int ret;
	dma_addr_t dma;
	void *mem;

	mem =
	    dma_alloc_coherent(NULL, PAGE_ALIGN(sizeof(*fb->info)), &dma,
			       GFP_KERNEL);

	if (NULL == mem) {
		pr_err(": unable to allocate fbinfo buffer\n");
		ret = -ENOMEM;
	} else {
		fb->info = (struct fbinfo_s *)mem;
		fb->dma = dma;
	}
	fb->fb.fbops = &bcm2708_fb_ops;
	fb->fb.flags = FBINFO_FLAG_DEFAULT | FBINFO_HWACCEL_COPYAREA;
	fb->fb.pseudo_palette = fb->cmap;

	strncpy(fb->fb.fix.id, bcm2708_name, sizeof(fb->fb.fix.id));
	fb->fb.fix.type = FB_TYPE_PACKED_PIXELS;
	fb->fb.fix.type_aux = 0;
	fb->fb.fix.xpanstep = 1;
	fb->fb.fix.ypanstep = 1;
	fb->fb.fix.ywrapstep = 0;
	fb->fb.fix.accel = FB_ACCEL_NONE;

	fb->fb.var.xres = fbwidth;
	fb->fb.var.yres = fbheight;
	fb->fb.var.xres_virtual = fbwidth;
	fb->fb.var.yres_virtual = fbheight;
	fb->fb.var.bits_per_pixel = fbdepth;
	fb->fb.var.vmode = FB_VMODE_NONINTERLACED;
	fb->fb.var.activate = FB_ACTIVATE_NOW;
	fb->fb.var.nonstd = 0;
	fb->fb.var.height = -1;		/* height of picture in mm    */
	fb->fb.var.width = -1;		/* width of picture in mm    */
	fb->fb.var.accel_flags = 0;

	fb->fb.monspecs.hfmin = 0;
	fb->fb.monspecs.hfmax = 100000;
	fb->fb.monspecs.vfmin = 0;
	fb->fb.monspecs.vfmax = 400;
	fb->fb.monspecs.dclkmin = 1000000;
	fb->fb.monspecs.dclkmax = 100000000;

	bcm2708_fb_set_bitfields(&fb->fb.var);
	init_waitqueue_head(&fb->dma_waitq);

	/*
	 * Allocate colourmap.
	 */

	fb_set_var(&fb->fb, &fb->fb.var);
	bcm2708_fb_set_par(&fb->fb);

	print_debug("BCM2708FB: registering framebuffer (%dx%d@%d) (%d)\n", fbwidth,
		fbheight, fbdepth, fbswap);

	ret = register_framebuffer(&fb->fb);
	print_debug("BCM2708FB: register framebuffer (%d)\n", ret);
	if (ret == 0)
		goto out;

	print_debug("BCM2708FB: cannot register framebuffer (%d)\n", ret);
out:
	return ret;
}

static int bcm2708_fb_probe(struct platform_device *dev)
{
	struct bcm2708_fb *fb;
	int ret;

	fb = kzalloc(sizeof(struct bcm2708_fb), GFP_KERNEL);
	if (!fb) {
		dev_err(&dev->dev,
			"could not allocate new bcm2708_fb struct\n");
		ret = -ENOMEM;
		goto free_region;
	}

	bcm2708_fb_debugfs_init(fb);

	fb->cb_base = dma_alloc_writecombine(&dev->dev, SZ_64K,
					     &fb->cb_handle, GFP_KERNEL);
	if (!fb->cb_base) {
		dev_err(&dev->dev, "cannot allocate DMA CBs\n");
		ret = -ENOMEM;
		goto free_fb;
	}

	pr_info("BCM2708FB: allocated DMA memory %08x\n",
	       fb->cb_handle);

	ret = bcm_dma_chan_alloc(BCM_DMA_FEATURE_BULK,
				 &fb->dma_chan_base, &fb->dma_irq);
	if (ret < 0) {
		dev_err(&dev->dev, "couldn't allocate a DMA channel\n");
		goto free_cb;
	}
	fb->dma_chan = ret;

	ret = request_irq(fb->dma_irq, bcm2708_fb_dma_irq,
			  0, "bcm2708_fb dma", fb);
	if (ret) {
		pr_err("%s: failed to request DMA irq\n", __func__);
		goto free_dma_chan;
	}


	pr_info("BCM2708FB: allocated DMA channel %d @ %p\n",
	       fb->dma_chan, fb->dma_chan_base);

	fb->dev = dev;

	ret = bcm2708_fb_register(fb);
	if (ret == 0) {
		platform_set_drvdata(dev, fb);
		goto out;
	}

free_dma_chan:
	bcm_dma_chan_free(fb->dma_chan);
free_cb:
	dma_free_writecombine(&dev->dev, SZ_64K, fb->cb_base, fb->cb_handle);
free_fb:
	kfree(fb);
free_region:
	dev_err(&dev->dev, "probe failed, err %d\n", ret);
out:
	return ret;
}

static int bcm2708_fb_remove(struct platform_device *dev)
{
	struct bcm2708_fb *fb = platform_get_drvdata(dev);

	platform_set_drvdata(dev, NULL);

	if (fb->fb.screen_base)
		iounmap(fb->fb.screen_base);
	unregister_framebuffer(&fb->fb);

	dma_free_writecombine(&dev->dev, SZ_64K, fb->cb_base, fb->cb_handle);
	bcm_dma_chan_free(fb->dma_chan);

	dma_free_coherent(NULL, PAGE_ALIGN(sizeof(*fb->info)), (void *)fb->info,
			  fb->dma);
	bcm2708_fb_debugfs_deinit(fb);

	free_irq(fb->dma_irq, fb);

	kfree(fb);

	return 0;
}

static struct platform_driver bcm2708_fb_driver = {
	.probe = bcm2708_fb_probe,
	.remove = bcm2708_fb_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   },
};

static int __init bcm2708_fb_init(void)
{
	return platform_driver_register(&bcm2708_fb_driver);
}

module_init(bcm2708_fb_init);

static void __exit bcm2708_fb_exit(void)
{
	platform_driver_unregister(&bcm2708_fb_driver);
}

module_exit(bcm2708_fb_exit);

module_param(fbwidth, int, 0644);
module_param(fbheight, int, 0644);
module_param(fbdepth, int, 0644);
module_param(fbswap, int, 0644);

MODULE_DESCRIPTION("BCM2708 framebuffer driver");
MODULE_LICENSE("GPL");

MODULE_PARM_DESC(fbwidth, "Width of ARM Framebuffer");
MODULE_PARM_DESC(fbheight, "Height of ARM Framebuffer");
MODULE_PARM_DESC(fbdepth, "Bit depth of ARM Framebuffer");
MODULE_PARM_DESC(fbswap, "Swap order of red and blue in 24 and 32 bit modes");
