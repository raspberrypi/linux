/*
 *  Copyright (C) 2013 VKorehov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */


#include <linux/cdev.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <asm/tlbflush.h>
#include <linux/mm.h>
#include <linux/kdev_t.h>
#include <linux/semaphore.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <asm/io.h>
#include <asm/fiq.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <asm/delay.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/wait.h>
#include <linux/device.h>
#include <linux/moduleparam.h>
#include <asm/uaccess.h>
#define BCM_NR_GPIOS 54 // number of gpio lines
#include <mach/platform.h>
#include <mach/irqs.h>
#include "rpi2c.h"

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("vkorehov");
MODULE_DESCRIPTION("Raspbery Pi module for i2c slave (not master), it uses existing gpio module");
MODULE_VERSION("1.0.0");

void rpi2c_fiq(void);
void rpi2c_update_counters(void);

unsigned int rpi2c_my_addr = 0x40;
unsigned int rpi2c_sda_gpio_a = 4;
unsigned int rpi2c_sda_gpio_b = 31;
unsigned int rpi2c_scl_gpio = 17;

DECLARE_WAIT_QUEUE_HEAD(read_queue);

void __iomem *rpi2c_base;
void __iomem *rpi2c_power_base; // power control, current driving, hysteresis, etc

extern unsigned int rpi2c_fiq_count;
extern unsigned int rpi2c_fiq_count_raw;

#ifdef RPI2C_DEBUG
extern unsigned char rpi2c_i2c_type[];
extern unsigned long long rpi2c_i2c_timing[];
extern unsigned char rpi2c_i2c_data1[];
extern unsigned char rpi2c_i2c_data2[];
#endif
// IRQ Control
static unsigned long rising;
static unsigned long falling;
static unsigned long low;
static unsigned long high;

extern unsigned int rpi2c_data_pos_fiq;// Current packed processed by FIQcannot be more then MAX_FIQ_PACKETS-1
extern struct i2c_data rpi2c_data[];
extern unsigned long long rpi2c_sda_ack_time;

static unsigned int sda_ack_recovered = 0;
static dev_t cdevid; // character device identifier
static struct cdev cdev; // character device itself
static struct class *cdev_class;

struct rpi2c_file {
	unsigned int data_pos_file; // data pos as seen by file reader
};

static struct fiq_handler fh = {
  .name = "rpi2c-fiq",
};
extern struct fiq_stack_s rpi2c_fiq_stack;

extern unsigned long long rpi2c_total_cycles;//,total_count1,total_count2;
static struct timer_list overflow_timer;

static int rpi2c_set_function(unsigned offset,
                                int function);

static void avoid_overflow(unsigned long d) {
	mod_timer(&overflow_timer, jiffies + msecs_to_jiffies(250));
	rpi2c_update_counters();
        if(rpi2c_sda_ack_time && rpi2c_total_cycles - rpi2c_sda_ack_time > 20000) { // clear hanging SDA-ack seq
		rpi2c_sda_ack_time = 0;
		mb();
                writel(1<<rpi2c_sda_gpio_a, rpi2c_base + GPIOSET(0));
                rpi2c_set_function(rpi2c_sda_gpio_a, GPIO_FSEL_INPUT);
                sda_ack_recovered++;
        }
}
/* Quite elegant solution to hardwire both pins and process them in FIQ and in IRQ!*/
static irqreturn_t notrace wakeup_readers_irq(int irq, void *dev_id)
{
	irqreturn_t result = IRQ_NONE;
	unsigned long edsr = __raw_readl(rpi2c_base + GPIOEDS(0));
        // clear only INT_GPIO0 interrupts (GPIO[0..27])
	if(edsr & (1<<rpi2c_sda_gpio_b)) {
        	writel(edsr & (1<<rpi2c_sda_gpio_b), rpi2c_base + GPIOEDS(0));
		result = IRQ_HANDLED;
	}
	wake_up_all(&read_queue);
	return result;
}

static int read_proc(char *buf, char **start, off_t offset, int count, int *eof, void *d)
{
	int len = 0, i;
	int limit = count - 48; /* Don't print more than this */
	if(len >= limit) {
		return -EFAULT;
	}
	len += sprintf(buf+len,"\nFIQs:%d", rpi2c_fiq_count);
        len += sprintf(buf+len,"\nRFIQs:%d", rpi2c_fiq_count_raw);
	len += sprintf(buf+len,"\nACK recovery:%d", sda_ack_recovered);
	len += sprintf(buf+len,"\nrising:%lx falling:%lx", rising, falling);
	len += sprintf(buf+len,"\nFSEL:%x", readl(rpi2c_base + GPIOFSEL(0)));
        len += sprintf(buf+len,"\nAREN:%x", readl(rpi2c_base + GPIOAREN(0)));
        len += sprintf(buf+len,"\nAFEN:%x", readl(rpi2c_base + GPIOAFEN(0)));
        len += sprintf(buf+len,"\nLEV:%x",  readl(rpi2c_base + GPIOLEV(0)));

	len += sprintf(buf+len,"\nARM_IRQ_DIBL1:%x",  readl(__io_address(ARM_IRQ_DIBL1)));
	len += sprintf(buf+len,"\nARM_IRQ_DIBL2:%x",  readl(__io_address(ARM_IRQ_DIBL2)));
	len += sprintf(buf+len,"\nARM_IRQ_DIBL3:%x",  readl(__io_address(ARM_IRQ_DIBL3)));
	len += sprintf(buf+len,"\nARM_IRQ_FAST:%x",   readl(__io_address(ARM_IRQ_FAST)));

#ifdef RPI2C_DEBUG
	for(i = 0; i < 512 && len <= limit; i++) {
		len += sprintf(buf+len,"\n%d %lld %d %d", (int)rpi2c_i2c_type[i], rpi2c_i2c_timing[i], (int)rpi2c_i2c_data1[i], (int)rpi2c_i2c_data2[i]);
	}
#endif
        for(i = 0; i < MAX_FIQ_PACKETS && len <= limit; i++) {
                len += sprintf(buf+len,"\n A:%x D1:%x D2:%x D3:%x D4:%x",
			(int) rpi2c_data[i].bytes[0],
			(int) rpi2c_data[i].bytes[1],
			(int) rpi2c_data[i].bytes[2],
			(int) rpi2c_data[i].bytes[3],
			(int) rpi2c_data[i].bytes[4]);
        }
	len += sprintf(buf+len,"\n");
	*eof = 1;
	return len;
}

static ssize_t read(struct file *f, char __user *buf, size_t sz, loff_t *pos)
{
	size_t result = 0; size_t len = 0; size_t limit = sz - (MAX_I2C_DATA_HEX << 1);
	struct rpi2c_file* d = f->private_data;
	unsigned int local_data_pos_fiq;
	unsigned int local_data_pos;
	unsigned char* kbuff=0; //  kernel buffer used for output encoding
#ifdef RPI2C_DEBUG
	printk(KERN_INFO DRIVER_NAME ": read %p data_pos_fiq=%d data_pos_file=%d\n", d, rpi2c_data_pos_fiq, d->data_pos_file);
#endif
	while( d->data_pos_file == rpi2c_data_pos_fiq) {
		if (f->f_flags & O_NONBLOCK) {
			result = -EAGAIN;
			goto out;
		}
		if(wait_event_interruptible(read_queue, d->data_pos_file != rpi2c_data_pos_fiq)) {
			result = ERESTARTSYS;
			goto out;
		}
	}
	// Ok Data is here!
	local_data_pos_fiq = rpi2c_data_pos_fiq; // snapshot
	local_data_pos     = d->data_pos_file;
	if(local_data_pos_fiq > local_data_pos) { // normal operation
		unsigned int c = local_data_pos_fiq - local_data_pos;
		unsigned int i,j;
		kbuff = kmalloc(c * MAX_I2C_DATA_HEX, GFP_KERNEL);
		if(!kbuff) {
			result = -ENOMEM;
			goto out;
		}
		for(i = local_data_pos; i < local_data_pos_fiq && len < limit; i++) {
			for(j = 0; j < min(rpi2c_data[i].count, (unsigned int)MAX_I2C_DATA); j++) {
				len += sprintf(kbuff+len,"%02x", (unsigned int)rpi2c_data[i].bytes[j]);
			}
			local_data_pos++;
			len += sprintf(kbuff+len,"\n");
		}
	} else if(local_data_pos_fiq < local_data_pos) { // Wrapping occured
		unsigned int c1 = MAX_FIQ_PACKETS - local_data_pos;
		unsigned int c2 = local_data_pos_fiq;
		unsigned int i,j;
		kbuff = kmalloc((c1+c2) * MAX_I2C_DATA_HEX, GFP_KERNEL);
                if(!kbuff) {
                        result = -ENOMEM;
                        goto out;
                }
		for(i = local_data_pos; i < MAX_FIQ_PACKETS && len < limit; i++) {
                        for(j = 0; j < min(rpi2c_data[i].count, (unsigned int)MAX_I2C_DATA); j++) {
                                len += sprintf(kbuff+len,"%02x", (unsigned int)rpi2c_data[i].bytes[j]);
                        }
                        local_data_pos++;
                        len += sprintf(kbuff+len,"\n");
                }
                if(local_data_pos >= MAX_FIQ_PACKETS) {
                        local_data_pos = 0;
                }
                for(i = 0; i < local_data_pos_fiq && len < limit; i++) {
                        for(j = 0; j < min(rpi2c_data[i].count, (unsigned int)MAX_I2C_DATA); j++) {
                                len += sprintf(kbuff+len,"%02x", (unsigned int)rpi2c_data[i].bytes[j]);
                        }
			local_data_pos++;
                        len += sprintf(kbuff+len,"\n");
                }
	}

	if(copy_to_user(buf, kbuff, len)) {
		result = -EFAULT;
		goto out;
	}
	// if everything is ok, sync data pos
	d->data_pos_file = local_data_pos;
	result = len;
out:
	if(kbuff) {
		kfree(kbuff);
	}
	return result;
}


static ssize_t write(struct file *f, const char __user *buf, size_t sz, loff_t *pos)
{
    return -EFAULT;
}

static int open(struct inode *in, struct file *f)
{
	f->private_data = kmalloc(sizeof(struct rpi2c_file), GFP_KERNEL); /* for other methods */
	memset(f->private_data, 0, sizeof(struct rpi2c_file));
	if(!f->private_data) {
		return -ENOMEM;
	}
#ifdef RPI2C_DEBUG
	printk(KERN_INFO DRIVER_NAME ": open %p data_pos_fiq=%d data_pos_file=%d\n", f->private_data, rpi2c_data_pos_fiq, 0);
#endif
	return 0;
}

static int release(struct inode *in, struct file *f)
{
#ifdef RPI2C_DEBUG
	printk(KERN_INFO DRIVER_NAME ": release %p data_pos_fiq=%d data_pos_file=%d\n", f->private_data, rpi2c_data_pos_fiq, ((struct rpi2c_file*)f->private_data)->data_pos_file);
#endif
	kfree(f->private_data);
	f->private_data = NULL;
	return 0;
}

static struct file_operations rpi2c_fops = {
 .owner = THIS_MODULE,
 .read = read,
 .write = write,
 .open = open,
 .release = release,
};

static int rpi2c_set_function(unsigned offset,
				int function)
{
	unsigned gpiodir;
	unsigned gpio_bank = offset / 10;
	unsigned gpio_field_offset = (offset - 10 * gpio_bank) * 3;

	gpiodir = readl(rpi2c_base + GPIOFSEL(gpio_bank));
	gpiodir &= ~(7 << gpio_field_offset);
	gpiodir |= function << gpio_field_offset;
	writel(gpiodir, rpi2c_base + GPIOFSEL(gpio_bank));
	gpiodir = readl(rpi2c_base + GPIOFSEL(gpio_bank));

	return 0;
}


static int rpi2c_get(unsigned offset)
{
	unsigned gpio_bank = offset / 32;
	unsigned gpio_field_offset = (offset - 32 * gpio_bank);
	unsigned lev;

	lev = readl(rpi2c_base + GPIOLEV(gpio_bank));
	return 0x1 & (lev >> gpio_field_offset);
}

static void rpi2c_set(unsigned offset, int value)
{
	unsigned gpio_bank = offset / 32;
	unsigned gpio_field_offset = (offset - 32 * gpio_bank);
	if (value)
		writel(1 << gpio_field_offset, rpi2c_base + GPIOSET(gpio_bank));
	else
		writel(1 << gpio_field_offset, rpi2c_base + GPIOCLR(gpio_bank));
}

/*************************************************************************************************************************
 * GPIO IRQ
 */

static int rpi2c_irq_set_type(unsigned gpio, unsigned type)
{

	if (type & ~(IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING | IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
		return -EINVAL;

	if (type & IRQ_TYPE_EDGE_RISING) {
		rising |= (1 << gpio);
	} else {
		rising &= ~(1 << gpio);
	}

	if (type & IRQ_TYPE_EDGE_FALLING) {
		falling |= (1 << gpio);
	} else {
		falling &= ~(1 << gpio);
	}

        if (type & IRQ_TYPE_LEVEL_LOW) {
                low |= (1 << gpio);
        } else {
                low &= ~(1 << gpio);
        }

        if (type & IRQ_TYPE_LEVEL_HIGH) {
                high |= (1 << gpio);
        } else {
                high &= ~(1 << gpio);
        }

	return 0;
}

static void rpi2c_irq_mask(unsigned gpio)
{
	unsigned gn = gpio;
	unsigned gb = gn / 32;
	unsigned long rising = readl(rpi2c_base + GPIOAREN(gb));
	unsigned long falling = readl(rpi2c_base + GPIOAFEN(gb));
        unsigned long low = readl(rpi2c_base + GPIOLEN(gb));
        unsigned long high = readl(rpi2c_base + GPIOHEN(gb));

	gn = gn % 32;

	writel(rising & ~(1 << gn), rpi2c_base + GPIOAREN(gb));
	writel(falling & ~(1 << gn), rpi2c_base + GPIOAFEN(gb));
        writel(low & ~(1 << gn), rpi2c_base + GPIOLEN(gb));
        writel(high & ~(1 << gn), rpi2c_base + GPIOHEN(gb));
	// clear events if they managed to stackup
	writel(1 << gn, rpi2c_base + GPIOEDS(gb));
}

static void rpi2c_irq_unmask(unsigned gpio)
{
	unsigned gn = gpio;
	unsigned gb = gn / 32;
	unsigned long r = readl(rpi2c_base + GPIOAREN(gb));
	unsigned long f = readl(rpi2c_base + GPIOAFEN(gb));
        unsigned long l = readl(rpi2c_base + GPIOLEN(gb));
        unsigned long h = readl(rpi2c_base + GPIOHEN(gb));
	gn = gn % 32;
	// clear events if they managed to stackup
	writel(1 << gn, rpi2c_base + GPIOEDS(gb));

	if (rising & (1 << gn)) {
		writel(r | (1 << gn), rpi2c_base + GPIOAREN(gb));
	} else {
		writel(r & ~(1 << gn), rpi2c_base + GPIOAREN(gb));
	}

	if (falling & (1 << gn)) {
		writel(f | (1 << gn), rpi2c_base + GPIOAFEN(gb));
	} else {
		writel(f & ~(1 << gn), rpi2c_base + GPIOAFEN(gb));
	}

        if (low & (1 << gn)) {
                writel(l | (1 << gn), rpi2c_base + GPIOLEN(gb));
        } else {
                writel(l & ~(1 << gn), rpi2c_base + GPIOLEN(gb));
        }

        if (high & (1 << gn)) {
                writel(h | (1 << gn), rpi2c_base + GPIOHEN(gb));
        } else {
                writel(h & ~(1 << gn), rpi2c_base + GPIOHEN(gb));
        }
}

static int __init rpi2c_init(void)
{
	int err = 0;
	unsigned int control = 0, rpi2c_major;
	unsigned long long  t;
	struct pt_regs regs;

	if(rpi2c_my_addr > 0xff) {
		err = -EFAULT;
		printk(KERN_ERR DRIVER_NAME ": rpi2c_my_addr is invalid %x\n", rpi2c_my_addr);
		goto out;
	}
        if(rpi2c_sda_gpio_a > 27) {
                err = -EFAULT;
                printk(KERN_ERR DRIVER_NAME ": rpi2c_sda_gpio_a must be within [0..27] range, is invalid: %x\n", rpi2c_sda_gpio_a);
                goto out;
        }
        if(rpi2c_sda_gpio_b < 28 || rpi2c_sda_gpio_b > 31) {
                err = -EFAULT;
		printk(KERN_ERR DRIVER_NAME ": rpi2c_sda_gpio_b must be within [28..31] range, is invalid: %x\n", rpi2c_sda_gpio_b);
                goto out;
        }
        if(rpi2c_scl_gpio > 27) {
                err = -EFAULT;
		printk(KERN_ERR DRIVER_NAME ": rpi2c_scl_gpio must be within [0..27] range, is invalid: %x\n", rpi2c_scl_gpio);
                goto out;
        }
        /* x = 0 */
        /* CCR overflow interrupts = off = 0 */
        /* 0 */
        /* ECC overflow interrupts = off = 0 */
        /* D div/64 = 0 = off */
        control|=(1<<2); /* reset cycle-count register */
        //control|=(1<<1); /* reset count registers */
        control|=(1<<0); /* start counters */
	rpi2c_total_cycles=0;
	/* start the timer, 250 ms */
	setup_timer(&overflow_timer, avoid_overflow, 0);
	mod_timer(&overflow_timer, jiffies + msecs_to_jiffies(250));
        /* start the counters */
        asm volatile("mcr p15, 0, %0, c15, c12, 0\n"
                     : "+r" (control));

	create_proc_read_entry("rpi2cev", 0 /* default mode */,
		NULL /* parent dir */, read_proc,
		NULL /* client data */);

        err = alloc_chrdev_region(&cdevid, 0, 1, "rpi2ch");
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to allocate character device %d\n", err);
                goto exit;
        }
        rpi2c_major = MAJOR(cdevid);
	printk(KERN_INFO DRIVER_NAME ": allocated cdev region with major number %d\n", rpi2c_major);
	cdev_init(&cdev, &rpi2c_fops);
        cdev.owner = THIS_MODULE;
        cdev.ops = &rpi2c_fops;

        err = cdev_add(&cdev, cdevid, 1);
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to add cdev %d\n", err);
                goto uregcdev;
        }

	cdev_class = class_create(THIS_MODULE, "i2c-sniffer");
	device_create(cdev_class, NULL, cdevid, NULL, "rpi2ch0");

        rpi2c_base = __io_address(GPIO_BASE);
	rpi2c_power_base = __io_address(PM_BASE);
	// tune pullup controls!
	writel(0b10 /*Enable Pull Up control */, rpi2c_base + GPIOUD(0));
	// wait at least 150 cycles
	t = rpi2c_total_cycles;
	while(rpi2c_total_cycles - t < 200) { rpi2c_update_counters(); }
	// Enable driving of specific lines
	writel(1<<rpi2c_sda_gpio_a | 1<<rpi2c_scl_gpio | 1<<rpi2c_sda_gpio_b /* GPIOsda_gpio_a and GPIOscl_gpio and GPIOsda_gpio_b */,
		rpi2c_base + GPIOUDCLK(0));
	// enable shmidt trigger for GPIO[0..27]
	writel(0x5a000000 | 0b001 /* 2ma */ | 1<<3 /*hyst*/ | 0<<4 /* slew*/ , rpi2c_power_base + PM_PADS(0));
	writel(0x5a000000 | 0b001 /* 2ma */ | 1<<3 /*hyst*/ | 0<<4 /* slew*/ , rpi2c_power_base + PM_PADS(1));

	err = rpi2c_set_function(rpi2c_sda_gpio_a, GPIO_FSEL_INPUT);
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to set gpiosda_gpio_a direction as input %d\n", err);
                goto unregall;
        }
        err = rpi2c_set_function(rpi2c_scl_gpio, GPIO_FSEL_INPUT);
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to set gpioscl_gpio direction as input %d\n", err);
                goto unregall;
        }
        err = rpi2c_set_function(rpi2c_sda_gpio_b, GPIO_FSEL_INPUT);
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to set gpiosda_gpio_b direction as input %d\n", err);
                goto unregall;
        }

        printk(KERN_INFO DRIVER_NAME ": current value gpiosda_gpio_a=%d gpioscl_gpio=%d gpiosda_gpio_b=%d\n", rpi2c_get(rpi2c_sda_gpio_a), rpi2c_get(rpi2c_scl_gpio), rpi2c_get(rpi2c_sda_gpio_b));

        err = rpi2c_irq_set_type(rpi2c_scl_gpio, IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING);
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to set IRQ type %d\n", err);
                goto unregall;
        }

        err = rpi2c_irq_set_type(rpi2c_sda_gpio_a, IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING);
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to set IRQ type %d\n", err);
                goto unregall;
        }

        err = rpi2c_irq_set_type(rpi2c_sda_gpio_b, IRQ_TYPE_EDGE_RISING);
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to set IRQ type %d\n", err);
                goto unregall;
        }

        // Set up fiq
        //init_FIQ(FIQ_START);
        err = claim_fiq(&fh);
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to claim fiq, already using fiq for something else? err=%d\n", err);
                goto unregall;
        }

        err = request_irq(IRQ_GPIO1, wakeup_readers_irq, IRQF_SHARED, "rpi2c-wakup", wakeup_readers_irq);
        if(err != 0) {
                printk(KERN_ERR DRIVER_NAME ": failed to request GPIO1 irq %d\n", err);
                goto unregall;
        }
	disable_irq(IRQ_GPIO0);
        rpi2c_irq_unmask(rpi2c_sda_gpio_a);
        rpi2c_irq_unmask(rpi2c_scl_gpio);
        rpi2c_irq_unmask(rpi2c_sda_gpio_b);

        set_fiq_handler(__FIQ_Branch, 4);
        memset(&regs,0,sizeof(regs));
        regs.ARM_r8 = (long)rpi2c_fiq;
        regs.ARM_r9 = (long)0;
        regs.ARM_sp = (long)rpi2c_fiq_stack.stack + sizeof(rpi2c_fiq_stack.stack) - 4;
        rpi2c_fiq_stack.magic1 = 0xdeadbeef;
        rpi2c_fiq_stack.magic2 = 0xaa995566;
	set_fiq_regs(&regs);
        enable_fiq(INTERRUPT_GPIO0);
	return 0;
unregall:
        device_destroy(cdev_class, cdevid);
        class_destroy(cdev_class);
	cdev_del(&cdev);
uregcdev:
	unregister_chrdev_region(cdevid, 1);
	remove_proc_entry("rpi2cev", NULL /* parent dir */);
exit:
        /* disable */
        control=0;
        asm volatile("mcr p15, 0, %0, c15, c12, 0\n"
                     : "+r" (control));
	del_timer(&overflow_timer);
out:
	return err;
}

static void __exit rpi2c_exit(void)
{
	unsigned int control = 0;
	rpi2c_irq_mask(rpi2c_sda_gpio_b);
	rpi2c_irq_mask(rpi2c_scl_gpio);
        rpi2c_irq_mask(rpi2c_sda_gpio_a);
        disable_fiq(INTERRUPT_GPIO0);
        enable_irq(IRQ_GPIO0);
	free_irq(IRQ_GPIO1,wakeup_readers_irq);
        release_fiq(&fh);
	//
	device_destroy(cdev_class, cdevid);
	class_destroy(cdev_class);
	cdev_del(&cdev);
	unregister_chrdev_region(cdevid, 1);
	remove_proc_entry("rpi2cev", NULL /* parent dir */);

	/* disable */
        asm volatile("mcr p15, 0, %0, c15, c12, 0\n"
                     : "+r" (control));
	del_timer(&overflow_timer);

}

module_init(rpi2c_init);
module_exit(rpi2c_exit);

module_param(rpi2c_my_addr, uint, S_IRUGO);
module_param(rpi2c_sda_gpio_a, uint, S_IRUGO);
module_param(rpi2c_sda_gpio_b, uint, S_IRUGO);
module_param(rpi2c_scl_gpio, uint, S_IRUGO);
