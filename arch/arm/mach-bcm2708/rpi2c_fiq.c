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

extern int rpi2c_my_addr;
extern int rpi2c_sda_gpio_a;
extern int rpi2c_sda_gpio_b;
extern int rpi2c_scl_gpio;

extern void __iomem *rpi2c_base;
extern void __iomem *rpi2c_power_base; // power control, current driving, hysteresis, etc

unsigned int rpi2c_fiq_count;
unsigned int rpi2c_fiq_count_raw;

#ifdef RPI2C_DEBUG
unsigned char rpi2c_i2c_type[512];
unsigned long long rpi2c_i2c_timing[512];
unsigned char rpi2c_i2c_data1[512];
unsigned char rpi2c_i2c_data2[512];
#endif

unsigned int rpi2c_data_pos_fiq;// Current packed processed by FIQcannot be more then MAX_FIQ_PACKETS-1
struct i2c_data rpi2c_data[MAX_FIQ_PACKETS];
static unsigned int state; //Bus state: 0 idle 1 Busy
static unsigned int databit;
static unsigned char curdata[MAX_I2C_DATA]; // current i2c data
unsigned long long rpi2c_sda_ack_time;

struct fiq_stack_s rpi2c_fiq_stack;

extern unsigned long long rpi2c_total_cycles;//,total_count1,total_count2;

static void __set_function(unsigned int offset,
                                int function)
{
        unsigned int gpiodir;
        unsigned int gpio_bank = offset / 10;
        unsigned int gpio_field_offset = (offset - 10 * gpio_bank) * 3;

        gpiodir = __raw_readl(rpi2c_base + GPIOFSEL(gpio_bank));
        gpiodir &= ~(7 << gpio_field_offset);
        gpiodir |= function << gpio_field_offset;
        __raw_writel(gpiodir, rpi2c_base + GPIOFSEL(gpio_bank));
        //gpiodir = __raw_readl(rpi2c_base + GPIOFSEL(gpio_bank));
}

static unsigned int cycles;//,count1,count2;
static unsigned int old_cycles;//,old_count1=0,old_count2=0;
unsigned long long rpi2c_total_cycles;//,total_count1,total_count2;

void rpi2c_update_counters(void) {
        asm volatile("mrc p15, 0, %0, c15, c12, 1\n"
                     : "=r" (cycles));

        if (cycles<old_cycles) {
                rpi2c_total_cycles+=(0xffffffff-old_cycles)+cycles+1;
        } else {
                rpi2c_total_cycles+=(cycles-old_cycles);
        }
        old_cycles=cycles;
}

static void __update_counters(void) {
        asm volatile("mrc p15, 0, %0, c15, c12, 1\n"
                     : "=r" (cycles));

        if (cycles<old_cycles) {
                rpi2c_total_cycles+=(0xffffffff-old_cycles)+cycles+1;
        } else {
                rpi2c_total_cycles+=(cycles-old_cycles);
        }
        old_cycles=cycles;
}


static unsigned int i,j,k,bit,byte,lev,scl,sda;
static unsigned long long t;
static unsigned long edsr;

void __attribute__ ((naked)) notrace rpi2c_fiq(void)
{

        /* entry takes care to store registers we will be treading on here */
        asm __volatile__ (
                "mov     ip, sp ;"
                /* stash FIQ and normal regs */
                "stmdb  sp!, {r0-r12,  lr};"
                /* !! THIS SETS THE FRAME, adjust to > sizeof locals */
                "sub     fp, ip, #512 ;"
                );

        // Cannot put local variables at the beginning of the function
        // because otherwise 'C' will play with the stack pointer. any locals
        // need to be inside the following block
        do
        {
        	
		lev = __raw_readl(rpi2c_base + GPIOLEV(0));
		scl = 0x1 & (lev >> rpi2c_scl_gpio);
		sda = 0x1 & (lev >> rpi2c_sda_gpio_a);
		edsr = __raw_readl(rpi2c_base + GPIOEDS(0));
		// clear only INT_GPIO0 interrupts (GPIO[0..27])
		__raw_writel(edsr & (1<<rpi2c_sda_gpio_a|1<<rpi2c_scl_gpio), rpi2c_base + GPIOEDS(0));
		rpi2c_fiq_count_raw++;
		rpi2c_update_counters();

                if(edsr & 1<<rpi2c_scl_gpio) {
#ifdef RPI2C_DEBUG
                        i = rpi2c_fiq_count % 512;
#endif
                        byte = databit / 9;
                        bit = databit % 9;
                        rpi2c_fiq_count++;
#ifdef RPI2C_DEBUG
                        rpi2c_i2c_type[i] = 1; // SCL
                        rpi2c_i2c_timing[i] = rpi2c_total_cycles;
                        rpi2c_i2c_data1[i] = scl;
#endif
			if(state && scl) {
				if(bit <= 7 && byte < MAX_I2C_DATA ) {
					curdata[byte] |= (sda << (7 - bit));
				}
				databit++;
				if(rpi2c_sda_ack_time) {
                                	t = rpi2c_total_cycles;
					__update_counters();
                                	while(rpi2c_total_cycles - t < 3500) {__update_counters();}
                                	// SDA = 1
                                	__raw_writel(1<<rpi2c_sda_gpio_a, rpi2c_base + GPIOSET(0));
                                	__set_function(rpi2c_sda_gpio_a, GPIO_FSEL_INPUT);
                                	rpi2c_sda_ack_time = 0;
				}
			} else if(state && !scl && bit == 8 &&(curdata[0] >> 1) == rpi2c_my_addr) { // Process ACK
				t = rpi2c_total_cycles;
				__update_counters();
                                while(rpi2c_total_cycles - t < 1500) {__update_counters();}
				// SDA = 0
				__set_function(rpi2c_sda_gpio_a, GPIO_FSEL_OUTPUT);
				__raw_writel(1<<rpi2c_sda_gpio_a, rpi2c_base + GPIOCLR(0));
				rpi2c_sda_ack_time = rpi2c_total_cycles;
			}
                }
		if((edsr & 1<<rpi2c_sda_gpio_a) && !rpi2c_sda_ack_time) {
#ifdef RPI2C_DEBUG
			i = rpi2c_fiq_count % 512;
#endif
                        rpi2c_fiq_count++;
#ifdef RPI2C_DEBUG
        		rpi2c_i2c_type[i] = 0;// SDA
        		rpi2c_i2c_timing[i] = total_cycles;
        		rpi2c_i2c_data1[i] = sda;
#endif
                        if(!sda && scl) {
                                // START condition detected
                                state = 1;
                                databit = 0;
                                for(k = 0; k < MAX_I2C_DATA; k++){
                                       curdata[k] = 0;
                                }
                        } else if (sda && scl && state) {
				// STOP condition detected
                                j = rpi2c_data_pos_fiq % MAX_FIQ_PACKETS;
                                rpi2c_data_pos_fiq++;
				if(rpi2c_data_pos_fiq >= MAX_FIQ_PACKETS) {
					rpi2c_data_pos_fiq = 0;
				}
				for(k = 0; k < MAX_I2C_DATA; k++){
                                	rpi2c_data[j].bytes[k] = curdata[k];
					curdata[k] = 0;
				}
				rpi2c_data[j].count = min((unsigned int)(databit / 9), (unsigned int)MAX_I2C_DATA);
                                state = 0;
                                databit = 0;
                        }
		}
        }
        while(0);

        mb();

        /* exit back to normal mode restoring everything */
        asm __volatile__ (
                /* return FIQ regs back to pristine state
                 * and get normal regs back
                 */
                "ldmia  sp!, {r0-r12, lr};"

                /* return */
                "subs   pc, lr, #4;"
        );
}


