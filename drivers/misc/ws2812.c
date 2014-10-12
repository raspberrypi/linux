/*
 * Raspberry Pi WS2812 PWM driver
 *
 * Written by: Gordon Hollingworth <gordon@fiveninjas.com>
 * Based on DMA PWM driver from Jonathan Bell <jonathan@raspberrypi.org>
 *
 * Copyright (C) 2014 Raspberry Pi Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * To use this driver you need to make sure that the PWM clock is set to 2.4MHz
 * and the correct PWM0 output is connected.  The best way to do this is to
 * create a dt-blob.bin on your RaspberryPi, start by downloading the default
 * dt-blob.dts from 
 *
 * Note, this uses the same PWM hardware as the standard audio output on the Pi
 * so you cannot use both simultaneously.
 * 
 * http://www.raspberrypi.org/documentation/configuration/pin-configuration.md 
 * 
 * (Copy the bit from /dts-v1/; through to the end...  This will contain the pin
 * configuration for all the Raspberry Pi versions (since they are different.
 * You can get rid of the ones you don't care about.  Next alter the PWM0 output
 * you want to use.
 * 
 * http://www.raspberrypi.org/documentation/hardware/raspberrypi/bcm2835/BCM2835-ARM-Peripherals.pdf
 * 
 * The link above will help understand what the GPIOs can do, check out page 102
 * You can use: GPIO12, GPIO18 or GPIO40, so for the Slice board we use GPIO40 so
 * we have the following in the dts file
 *
 * pin@p40 {
 * 	function = "pwm";
 * 	termination = "no_pulling";
 * };
 *
 * And at the bottom of the dts file, although still in the 'videocore' block we
 * have:
 *
 * clock_setup {
 * 	clock@PWM { freq = <2400000>; };
 * };
 *
 * To check whether the changes are correct you can use 'vcgencmd measure_clock 25'
 * This should return the value 2400000
 *
 * Also if you use wiringPi then you can do 'gpio readall' to check that the pin
 * alternate setting is set correctly.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <mach/platform.h>
#include <mach/dma.h>
#include <linux/uaccess.h>
#include <asm-generic/ioctl.h>

MODULE_LICENSE("GPL");

int invert_output = 1;
module_param(invert_output, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(invert_output, "Invert WS2812B output if there is a buffer inserted");

struct ws2812_state {
	/* Single-user */
	bool open;
	void __iomem *         dma_chan_base;
	int                    dma_chan_irq;
	void __iomem *         pwm_base;
	int                    dma_chan;
	wait_queue_head_t      writeq;
	void *                 buffer;
	dma_addr_t             buffer_phys;
	struct bcm2708_dma_cb *scb;
	dma_addr_t             scb_phys;
	uint32_t *             pixbuf;
	struct cdev            cdev;
	struct class *         cl;
};

#define CTL 0x0
#define STA 0x4
#define PWM_DMAC 0x8
#define RNG1 0x10
#define DAT1 0x14
#define FIFO1 0x18

struct ws2812_state *state;
size_t pixbuf_size = PAGE_SIZE;
size_t scb_len = (PAGE_SIZE/sizeof(int)) * 12;
int N_LEDS = 25;

// Open / Release mutex to force single user
DEFINE_MUTEX(ws2812_mutex);

static dev_t devid = MKDEV(1337, 0);

int brightness = 255;

/* WS2812B gamma correction */
unsigned char gamma_(unsigned char val)
{
	int bright = val;
	unsigned char GammaE[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2,
	2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
	6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11,
	11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18,
	19, 19, 20, 21, 21, 22, 22, 23, 23, 24, 25, 25, 26, 27, 27, 28,
	29, 29, 30, 31, 31, 32, 33, 34, 34, 35, 36, 37, 37, 38, 39, 40,
	40, 41, 42, 43, 44, 45, 46, 46, 47, 48, 49, 50, 51, 52, 53, 54,
	55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70,
	71, 72, 73, 74, 76, 77, 78, 79, 80, 81, 83, 84, 85, 86, 88, 89,
	90, 91, 93, 94, 95, 96, 98, 99,100,102,103,104,106,107,109,110,
	111,113,114,116,117,119,120,121,123,124,126,128,129,131,132,134,
	135,137,138,140,142,143,145,146,148,150,151,153,155,157,158,160,
	162,163,165,167,169,170,172,174,176,178,179,181,183,185,187,189,
	191,193,194,196,198,200,202,204,206,208,210,212,214,216,218,220,
	222,224,227,229,231,233,235,237,239,241,244,246,248,250,252,255};
	bright = (bright * brightness) / 255;
	return GammaE[bright];
}

// LED serial output
// 4 bits make up a single bit of the output
// 1 1 1 0  -- 1
// 1 0 0 0  -- 0
//
// Plus require a space of 150 zeros for reset
// 24 bits per led
//
// (24 * 4) / 8 = 12 bytes per led
//
//  red = 0xff0000 == 0xeeeeeeee 0x88888888 0x88888888
unsigned char * led_encode(int rgb, unsigned char *buf)
{
	int i;
	unsigned char red = gamma_(rgb >> 8);
	unsigned char blu = gamma_(rgb);
	unsigned char grn = gamma_(rgb >> 16);
	int rearrange =  red +
			(blu << 8) +
			(grn << 16);
	for(i = 11; i >= 0; i--)
	{
		switch(rearrange & 3)
		{
			case 0: *buf++ = invert_output ? 0x77 : 0x88; break;
			case 1: *buf++ = invert_output ? 0x71 : 0x8e; break;
			case 2: *buf++ = invert_output ? 0x17 : 0xe8; break;
			case 3: *buf++ = invert_output ? 0x11 : 0xee; break;
		}
		rearrange >>= 2;
	}

	return buf;
}

static int ws2812_release(struct inode *inode, struct file *file)
{
	state->open = 0;
	
	return 0;
}

static int ws2812_open(struct inode *inode, struct file *file)
{
	printk(KERN_ERR "ws2812: open");
	
	mutex_lock(&ws2812_mutex);
	if(state->open)
	{
		mutex_unlock(&ws2812_mutex);
		return -EBUSY;
	}
	
	state->open = 1;
	mutex_unlock(&ws2812_mutex);
	
	return 0;
}

int dma_busy(void)
{
	return readl(state->dma_chan_base + BCM2708_DMA_CS) & BCM2708_DMA_ACTIVE;
}

// Write to the PWM through DMA
ssize_t ws2812_write(struct file *filp, const char __user *buf, size_t count, loff_t *pos) 
{
	int32_t *p_rgb;
	int8_t *p_buffer;
	int i;
	
	while (dma_busy()) {
		printk(KERN_ERR "waiting for dma to finish");
		if (wait_event_interruptible(state->writeq, !dma_busy())) {
			pr_info("bugged\n");
			return -ERESTARTSYS;
		}
	}
		
	if(copy_from_user(state->pixbuf, buf, min(count, pixbuf_size)))
		return -EFAULT;

	p_rgb = state->pixbuf;
	p_buffer = state->buffer;
	for(i = 0; i < min(count, pixbuf_size) / 4; i++)
		p_buffer = led_encode(*p_rgb++, p_buffer);
	memset(p_buffer, invert_output ? 0xff : 0x00, scb_len - ((int) state->buffer - (int) p_buffer));
	
	state->scb->next = 0;
	state->scb->length = (int) p_buffer - (int) state->buffer + 16;
	writel(state->scb_phys, state->dma_chan_base + BCM2708_DMA_ADDR);
	/* Setup DMA engine */
	writel(BCM2708_DMA_ERR | BCM2708_DMA_INT | BCM2708_DMA_ACTIVE, state->dma_chan_base + BCM2708_DMA_CS);

	//pr_info("Write count=%d readptr=%08x writeptr=%08x\n", count, state->circ_readptr, state->circ_writeptr);
	return count;
}


static void prep_scb(struct ws2812_state *state) 
{
	u32 info = BCM2708_DMA_INT_EN | BCM2708_DMA_S_INC |
			     BCM2708_DMA_D_DREQ | BCM2708_DMA_PER_MAP(5);

	state->scb->info = info;
	state->scb->src = state->buffer_phys;
	/* PWM block FIFO (phys address) */
	state->scb->dst = 0x7E20C018;
	state->scb->length = scb_len;
	state->scb->stride = 0;
	state->scb->next = 0;
}

static irqreturn_t ws2812_irq(int irq, void *dev_id)
{
	/* read the DMA IRQ reason */
	printk(KERN_ERR "wake up");
	wake_up(&state->writeq);
	/* Ack DMA IRQ */
	writel(readl(state->dma_chan_base+BCM2708_DMA_CS) | (1 << 2), state->dma_chan_base + BCM2708_DMA_CS);
	return IRQ_HANDLED;
}

struct file_operations ws2812_fops = {
	.owner = THIS_MODULE,
	.llseek = NULL,
	.read = NULL,
	.write = ws2812_write,
	.open = ws2812_open,
	.release = ws2812_release,
};

static int __init ws2812_init(void)
{
	u32 reg = 0;

	printk(KERN_ERR "ws2812_init");

	state = kmalloc(sizeof(struct ws2812_state), GFP_KERNEL);
	if (!state) {
		pr_err("Can't allocate state\n");
		goto fail;
	}
	state->open = 0;
	
	state->scb = dma_alloc_coherent(NULL, sizeof(struct bcm2708_dma_cb), &state->scb_phys, GFP_KERNEL);
	if (!state->scb) {
		pr_err("can't allocate SCB\n");
		kfree(state);
		goto fail;
	}
	/* request a DMA channel */
	state->dma_chan = bcm_dma_chan_alloc(0, &state->dma_chan_base, &state->dma_chan_irq);
	if (state->dma_chan < 0) {
		pr_err("Can't allocate DMA channel\n");
		dma_free_coherent(NULL, sizeof(struct bcm2708_dma_cb), state->scb, state->scb_phys);
		kfree(state);
		goto fail;
	} else {
		pr_info("Got channel %d\n", state->dma_chan);
	}
	state->buffer = dma_alloc_coherent(NULL, (size_t) scb_len, &state->buffer_phys, GFP_KERNEL);
	if (!state->buffer) {
		pr_err("can't allocate DMA mem\n");
		dma_free_coherent(NULL, sizeof(struct bcm2708_dma_cb), state->scb, state->scb_phys);
		kfree(state);
		goto fail;
	}
	state->pixbuf = kmalloc(pixbuf_size, GFP_KERNEL);
	if(!state->pixbuf)
	{
		pr_err("can't allocate pix buf");
		dma_free_coherent(NULL, (size_t) scb_len, state->buffer, state->buffer_phys);
		dma_free_coherent(NULL, sizeof(struct bcm2708_dma_cb), state->scb, state->scb_phys);
		kfree(state);
	}
		
	init_waitqueue_head(&state->writeq);
	if(request_irq(state->dma_chan_irq, &ws2812_irq, 0, "PWM DMA IRQ", NULL)) {
		pr_err("Can't request IRQ %d\n", state->dma_chan_irq);
	}

	state->pwm_base = ioremap(PWM_BASE, SZ_4K);
	/* setup PWM block */
	
	// serial 32 bits per word
	writel(32, state->pwm_base + RNG1);
	// Clear 
	writel(0, state->pwm_base + DAT1);

	reg = (1 << 0) | /* CH1EN */
	      (1 << 1) | /* serialiser */
	      (0 << 2) | /* don't repeat last word */
	      (0 << 3) | /* silence is zero */
	      (0 << 4) | /* normal polarity */
	      (1 << 5) | /* use fifo */
	      (1 << 6) | /* Clear fifo */
	      (1 << 7) | /* MSEN - Mask space enable */
	      ((invert_output ? 1 : 0) << 11); /* Silence bit = 1 */
	writel(reg, state->pwm_base + CTL);
	reg = (1 << 31) | /* DMA enabled */
	      (4 << 8)  | /* Threshold for panic */
	      (8 << 0);   /* Threshold for dreq */
	writel(reg, state->pwm_base + PWM_DMAC);
	
	/* Setup SCBs */
	prep_scb(state);
	
	/* Start with clearing down the leds */
	memset(state->buffer, invert_output ? 0x77 : 0x88, 12 * N_LEDS);
	memset(((unsigned char *) state->buffer) + 12 * N_LEDS, invert_output ? 0xff : 0x00, 150); 

	state->scb->next = 0;
	state->scb->length = 12 * N_LEDS + 150;
	writel(state->scb_phys, state->dma_chan_base + BCM2708_DMA_ADDR);
	/* Setup DMA engine */
	writel(BCM2708_DMA_ERR | BCM2708_DMA_INT | BCM2708_DMA_ACTIVE, state->dma_chan_base + BCM2708_DMA_CS);

	// Create character device interface /dev/ws2812

	if(alloc_chrdev_region(&devid, 0, 1, "ws2812") < 0)
	{
		pr_err("Unable to create chrdev region");
		goto fail;
	}
	if((state->cl = class_create(THIS_MODULE, "ws2812")) == NULL)
	{
		unregister_chrdev_region(devid, 1);
		pr_err("Unable to create class ws2812");
		goto fail;
	}
	if(device_create(state->cl, NULL, devid, NULL, "ws2812") == NULL)
	{
		class_destroy(state->cl);
		unregister_chrdev_region(devid, 1);
		pr_err("Unable to create device ws2812");
		goto fail;
	}

	state->cdev.owner = THIS_MODULE;
	cdev_init(&state->cdev, &ws2812_fops);
	
	if(cdev_add(&state->cdev, devid, 1)) {
		pr_err("CDEV failed\n");
	}

	return 0;
fail:
	return -1;
}


static void __exit ws2812_exit(void)
{
	/* Disable DMA */
	cdev_del(&state->cdev);
	disable_irq(state->dma_chan_irq);
	free_irq(state->dma_chan_irq, NULL);
	unregister_chrdev(devid, "ws2812");
	bcm_dma_abort(state->dma_chan_base);
	dma_free_coherent(NULL, (size_t) scb_len, state->buffer, state->buffer_phys);
	bcm_dma_chan_free(state->dma_chan);
	dma_free_coherent(NULL, sizeof(struct bcm2708_dma_cb), state->scb, state->scb_phys);
}

module_init(ws2812_init);
module_exit(ws2812_exit);
