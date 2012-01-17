/*
 *  linux/arch/arm/mach-bcm2708/bcm2708.c
 *
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/serial_8250.h>
#include <linux/platform_device.h>
#include <linux/sysdev.h>
#include <linux/interrupt.h>
#include <linux/amba/bus.h>
#include <linux/amba/clcd.h>
#include <linux/clockchips.h>
#include <linux/cnt32_to_63.h>
#include <linux/io.h>

#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,38)
#include <linux/clkdev.h>
#else
#include <asm/clkdev.h>
#endif
#include <asm/system.h>
#include <mach/hardware.h>
#include <asm/irq.h>
#include <linux/leds.h>
#include <asm/mach-types.h>

#include <asm/mach/arch.h>
#include <asm/mach/flash.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>
#include <asm/mach/map.h>

#include <mach/timex.h>
#include <mach/dma.h>
#include <mach/vcio.h>

#include "bcm2708.h"
#include "armctrl.h"
#include "clock.h"

/* Effectively we have an IOMMU (ARM<->VideoCore map) that is set up to
 * give us IO access only to 64Mbytes of physical memory (26 bits).  We could
 * represent this window by setting our dmamasks to 26 bits but, in fact
 * we're not going to use addresses outside this range (they're not in real
 * memory) so we don't bother.
 *
 * In the future we might include code to use this IOMMU to remap other
 * physical addresses onto VideoCore memory then the use of 32-bits would be
 * more legitimate.
 */
#define DMA_MASK_BITS_COMMON 32

static void __init bcm2708_init_led(void);

void __init bcm2708_init_irq(void)
{
	armctrl_init(__io_address(ARMCTRL_IC_BASE), 0, 0, 0);
}

static struct map_desc bcm2708_io_desc[] __initdata = {
	{
		.virtual	= IO_ADDRESS(ARMCTRL_BASE),
		.pfn		= __phys_to_pfn(ARMCTRL_BASE),
		.length		= SZ_4K,
		.type		= MT_DEVICE
	}, {
		.virtual	= IO_ADDRESS(UART0_BASE),
		.pfn		= __phys_to_pfn(UART0_BASE),
		.length		= SZ_4K,
		.type		= MT_DEVICE
	}, {
		.virtual	= IO_ADDRESS(UART1_BASE),
		.pfn		= __phys_to_pfn(UART1_BASE),
		.length		= SZ_4K,
		.type		= MT_DEVICE
	}, {
#ifdef CONFIG_MMC_BCM2708 /* broadcom legacy SD */
		.virtual	= IO_ADDRESS(MMCI0_BASE),
		.pfn		= __phys_to_pfn(MMCI0_BASE),
		.length		= SZ_4K,
		.type		= MT_DEVICE
	}, {
#endif
		.virtual	= IO_ADDRESS(DMA_BASE),
		.pfn		= __phys_to_pfn(DMA_BASE),
		.length		= SZ_4K,
		.type		= MT_DEVICE
	}, {
		.virtual	= IO_ADDRESS(MCORE_BASE),
		.pfn		= __phys_to_pfn(MCORE_BASE),
		.length		= SZ_4K,
		.type		= MT_DEVICE
	}, {
		.virtual	= IO_ADDRESS(ST_BASE),
		.pfn		= __phys_to_pfn(ST_BASE),
		.length		= SZ_4K,
		.type		= MT_DEVICE
	 }, {
		.virtual	= IO_ADDRESS(USB_BASE),
		.pfn		= __phys_to_pfn(USB_BASE),
		.length		= SZ_128K,
		.type		= MT_DEVICE
	 }, {
		.virtual        = IO_ADDRESS(PM_BASE),
		.pfn	        = __phys_to_pfn(PM_BASE),
		.length	        = SZ_4K,
		.type	        = MT_DEVICE
	}, {
		.virtual	= IO_ADDRESS(GPIO_BASE),
		.pfn		= __phys_to_pfn(GPIO_BASE),
		.length		= SZ_4K,
		.type		= MT_DEVICE
	 }
};

void __init bcm2708_map_io(void)
{
	iotable_init(bcm2708_io_desc, ARRAY_SIZE(bcm2708_io_desc));
}

unsigned long frc_clock_ticks32(void)
{
	/* STC: a free running counter that increments at the rate of 1MHz */
	return readl(__io_address(ST_BASE+0x04));
}

unsigned long long frc_clock_ticks63(void)
{
	unsigned long t = frc_clock_ticks32();
	/* For cnt32_to_63 to work correctly we MUST call this routine
	 * at least once every half-32-bit-wraparound period - that's once
	 * every 35minutes or so - using it in sched_clock() should ensure this
	 */
	return cnt32_to_63(t);
}

unsigned long long sched_clock(void)
{
	return 1000ull * frc_clock_ticks63();
}

/*
 * These are fixed clocks.
 */
static struct clk ref24_clk = {
	.rate	= 3000000,  /* The UART is clocked at 3MHz via APB_CLK */
};
static struct clk osc_clk = {
#ifdef CONFIG_ARCH_BCM2708_CHIPIT
	.rate	= 27000000,
#else
	.rate	= 500000000,  /* ARM clock is set from the VideoCore booter */
#endif
};
/* warning - the USB needs a clock > 34MHz */

#ifdef CONFIG_MMC_BCM2708
static struct clk sdhost_clk = {
#ifdef CONFIG_ARCH_BCM2708_CHIPIT
	.rate	=   4000000, /* 4MHz */
#else
	.rate	= 250000000, /* 250MHz */
#endif
};
#endif

static struct clk_lookup lookups[] = {
	{	/* UART0 */
		.dev_id		= "dev:f1",
		.clk		= &ref24_clk,
	},
	{	/* USB */
		.dev_id		= "bcm2708_usb",
		.clk		= &osc_clk,
#ifdef CONFIG_MMC_BCM2708
	},
	{	/* MCI */
		.dev_id		= "bcm2708_mci.0",
		.clk		= &sdhost_clk,
#endif
	}
};


#define UART0_IRQ	{ IRQ_UART, NO_IRQ }
#define UART0_DMA	{ 15, 14 }

AMBA_DEVICE(uart0, "dev:f1",  UART0,    NULL);

static struct amba_device *amba_devs[] __initdata = {
	&uart0_device,
};

static struct resource bcm2708_dmaman_resources[] = {
	{
		.start			= DMA_BASE,
		.end			= DMA_BASE + SZ_4K - 1,
		.flags			= IORESOURCE_MEM,
	}
};

static struct platform_device bcm2708_dmaman_device = {
   .name			= BCM_DMAMAN_DRIVER_NAME,
	.id			= 0, /* first bcm2708_dma */
	.resource		= bcm2708_dmaman_resources,
	.num_resources		= ARRAY_SIZE(bcm2708_dmaman_resources),
};

#ifdef CONFIG_MMC_BCM2708
static struct resource bcm2708_mci_resources[] = {
	{
		.start			= MMCI0_BASE,
		.end			= MMCI0_BASE + SZ_4K - 1,
		.flags			= IORESOURCE_MEM,
	}, {
		.start                  = IRQ_SDIO,
		.end                    = IRQ_SDIO,
		.flags                  = IORESOURCE_IRQ,
	}
};


static struct platform_device bcm2708_mci_device = {
	.name			= "bcm2708_mci",
	.id			= 0, /* first bcm2708_mci */
	.resource		= bcm2708_mci_resources,
	.num_resources		= ARRAY_SIZE(bcm2708_mci_resources),
	.dev			= {
	.coherent_dma_mask      = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
	},
};
#endif /* CONFIG_MMC_BCM2708 */


static u64 fb_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_fb_device = {
	.name			= "bcm2708_fb",
	.id			= -1,  /* only one bcm2708_fb */
	.resource               = NULL,
	.num_resources          = 0,
	.dev			= {
		.dma_mask               = &fb_dmamask,
		.coherent_dma_mask      = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
	},
};

static struct plat_serial8250_port bcm2708_uart1_platform_data[] = {
	{
		.mapbase	= UART1_BASE + 0x40,
		.irq		= IRQ_AUX,
		.uartclk	= 125000000,
		.regshift	= 2,
		.iotype		= UPIO_MEM,
		.flags		= UPF_FIXED_TYPE | UPF_IOREMAP | UPF_SKIP_TEST,
		.type		= PORT_8250,
	},
	{ },
};

static struct platform_device bcm2708_uart1_device = {
	.name			= "serial8250",
	.id			= PLAT8250_DEV_PLATFORM,
	.dev			= {
		.platform_data	= bcm2708_uart1_platform_data,
	},
};

static struct resource bcm2708_usb_resources[] = {
	[0] =	{
		.start			= USB_BASE,
		.end			= USB_BASE + SZ_128K - 1,
		.flags			= IORESOURCE_MEM,
		},
	[1] =	{
		.start                  = IRQ_USB,
		.end                    = IRQ_USB,
		.flags                  = IORESOURCE_IRQ,
		},
};

static u64 usb_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_usb_device = {
	.name			= "bcm2708_usb",
	.id			= -1, /* only one bcm2708_usb */
	.resource		= bcm2708_usb_resources,
	.num_resources		= ARRAY_SIZE(bcm2708_usb_resources),
	.dev			= {
		.dma_mask               = &usb_dmamask,
		.coherent_dma_mask      = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
	},
};

static struct resource bcm2708_vcio_resources[] = {
	[0] =	{                       /* mailbox/semaphore/doorbell access */
		.start			= MCORE_BASE,
		.end			= MCORE_BASE + SZ_4K - 1,
		.flags			= IORESOURCE_MEM,
	},
};

static u64 vcio_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_vcio_device = {
	.name                   = BCM_VCIO_DRIVER_NAME,
	.id                     = -1, /* only one VideoCore I/O area */
	.resource               = bcm2708_vcio_resources,
	.num_resources          = ARRAY_SIZE(bcm2708_vcio_resources),
	.dev			= {
		.dma_mask               = &vcio_dmamask,
		.coherent_dma_mask      = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
	},
};

#ifdef CONFIG_BCM2708_GPIO
#define BCM_GPIO_DRIVER_NAME "bcm2708_gpio"

static struct resource bcm2708_gpio_resources[] = {
	[0] =	{                       /* general purpose I/O */
		.start			= GPIO_BASE,
		.end			= GPIO_BASE + SZ_4K - 1,
		.flags			= IORESOURCE_MEM,
	},
};

static u64 gpio_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_gpio_device = {
	.name                   = BCM_GPIO_DRIVER_NAME,
	.id                     = -1, /* only one VideoCore I/O area */
	.resource               = bcm2708_gpio_resources,
	.num_resources          = ARRAY_SIZE(bcm2708_gpio_resources),
	.dev			= {
		.dma_mask               = &gpio_dmamask,
		.coherent_dma_mask      = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
	},
};
#endif

#ifdef CONFIG_BCM2708_BUTTONS
static struct resource bcm2708_vcbuttons_resources[] = {
};

static u64 vcbuttons_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_vcbuttons_device = {
	.name                   = "bcm2708_vcbuttons",
	.id                     = -1, /* only one VideoCore I/O area */
	.resource               = bcm2708_vcbuttons_resources,
	.num_resources          = ARRAY_SIZE(bcm2708_vcbuttons_resources),
	.dev			= {
		.dma_mask               = &vcbuttons_dmamask,
		.coherent_dma_mask      = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
	},
};
#endif

#ifdef CONFIG_BCM2708_TOUCHSCREEN
static struct resource bcm2708_vctouch_resources[] = {
};

static u64 vctouch_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_vctouch_device = {
	.name                   = "bcm2708_vctouch",
	.id                     = -1, /* only one VideoCore I/O area */
	.resource               = bcm2708_vctouch_resources,
	.num_resources          = ARRAY_SIZE(bcm2708_vctouch_resources),
	.dev			= {
		.dma_mask               = &vctouch_dmamask,
		.coherent_dma_mask      = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
	},
};
#endif

static struct resource bcm2708_systemtimer_resources[] = {
	[0] =	{                       /* system timer access */
		.start			= ST_BASE,
		.end			= ST_BASE + SZ_4K - 1,
		.flags			= IORESOURCE_MEM,
	}, {
		.start                  = IRQ_TIMER3,
		.end                    = IRQ_TIMER3,
		.flags                  = IORESOURCE_IRQ,
	}


};

static u64 systemtimer_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_systemtimer_device = {
	.name                   = "bcm2708_systemtimer",
	.id                     = -1, /* only one VideoCore I/O area */
	.resource               = bcm2708_systemtimer_resources,
	.num_resources          = ARRAY_SIZE(bcm2708_systemtimer_resources),
	.dev			= {
		.dma_mask               = &systemtimer_dmamask,
		.coherent_dma_mask      = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
	},
};

#ifdef CONFIG_MMC_SDHCI_BCM2708 /* Arasan emmc SD */
static struct resource bcm2708_emmc_resources[] = {
	[0] = {
		.start = EMMC_BASE,
		.end   = EMMC_BASE + SZ_256 - 1, /* we only need this area */
		/* the memory map actually makes SZ_4K available  */
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_ARASANSDIO,
		.end   = IRQ_ARASANSDIO,
		.flags = IORESOURCE_IRQ,
	},
};

static u64 bcm2708_emmc_dmamask = 0xffffffffUL;

struct platform_device bcm2708_emmc_device = {
	.name		= "bcm2708_sdhci",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(bcm2708_emmc_resources),
	.resource	= bcm2708_emmc_resources,
	.dev		= {
		.dma_mask		= &bcm2708_emmc_dmamask,
		.coherent_dma_mask	= 0xffffffffUL
	},
};
#endif /* CONFIG_MMC_SDHCI_BCM2708 */

static struct resource bcm2708_powerman_resources[] = {
	[0] = {
		.start = PM_BASE,
		.end   = PM_BASE + SZ_256 - 1,
		.flags = IORESOURCE_MEM,
	},
};

static u64 powerman_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

struct platform_device bcm2708_powerman_device = {
	.name		= "bcm2708_powerman",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(bcm2708_powerman_resources),
	.resource	= bcm2708_powerman_resources,
	.dev		= {
		.dma_mask     = &powerman_dmamask,
		.coherent_dma_mask = 0xffffffffUL
	},
};

int __init bcm_register_device(struct platform_device *pdev)
{
	int ret;

	ret = platform_device_register(pdev);
	if (ret)
		pr_debug("Unable to register platform device '%s': %d\n",
			 pdev->name, ret);

	return ret;
}

void __init bcm2708_init(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(lookups); i++)
		clkdev_add(&lookups[i]);

	bcm_register_device(&bcm2708_dmaman_device);
	bcm_register_device(&bcm2708_vcio_device);
#ifdef CONFIG_BCM2708_GPIO
	bcm_register_device(&bcm2708_gpio_device);
#endif
	bcm_register_device(&bcm2708_systemtimer_device);
#ifdef CONFIG_MMC_BCM2708
	bcm_register_device(&bcm2708_mci_device);
#endif
	bcm_register_device(&bcm2708_fb_device);
	bcm_register_device(&bcm2708_usb_device);
	bcm_register_device(&bcm2708_uart1_device);
#ifdef CONFIG_BCM2708_BUTTONS
	bcm_register_device(&bcm2708_vcbuttons_device);
#endif
#ifdef CONFIG_BCM2708_TOUCHSCREEN
	bcm_register_device(&bcm2708_vctouch_device);
#endif
	bcm_register_device(&bcm2708_powerman_device);
#ifdef CONFIG_MMC_SDHCI_BCM2708
	bcm_register_device(&bcm2708_emmc_device);
#endif
        bcm2708_init_led();
#ifdef CONFIG_BCM2708_VCMEM
{
	extern void vc_mem_connected_init(void);
        vc_mem_connected_init();
}
#endif
	for (i = 0; i < ARRAY_SIZE(amba_devs); i++) {
		struct amba_device *d = amba_devs[i];
		amba_device_register(d, &iomem_resource);
	}
}

#define TIMER_PERIOD 10000 /* HZ in microsecs */

static void timer_set_mode(enum clock_event_mode mode,
			   struct clock_event_device *clk)
{
	unsigned long stc;

	switch (mode) {
	case CLOCK_EVT_MODE_PERIODIC:
		stc = readl(__io_address(ST_BASE+0x04));
		writel(stc + TIMER_PERIOD,
			__io_address(ST_BASE+0x18));/* stc3 */
		break;
	case CLOCK_EVT_MODE_ONESHOT:
	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_SHUTDOWN:
	default:
		printk(KERN_ERR "timer_set_mode: unhandled mode:%d\n",
			(int)mode);
		break;
	}

}

static int timer_set_next_event(unsigned long evt,
				struct clock_event_device *unused)
{
	unsigned long stc;

	 stc = readl(__io_address(ST_BASE + 0x04));
	 writel(stc + TIMER_PERIOD, __io_address(ST_BASE+0x18)); /* stc3 */
	return 0;
}

static struct clock_event_device timer0_clockevent =	 {
	.name		= "timer0",
	.shift		= 32,
	.features       = CLOCK_EVT_FEAT_ONESHOT,
	.set_mode	= timer_set_mode,
	.set_next_event	= timer_set_next_event,
};

/*
 * IRQ handler for the timer
 */
static irqreturn_t bcm2708_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = &timer0_clockevent;

	writel(1<<3, __io_address(ST_BASE+0x00)); /* stcs clear timer int */

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct irqaction bcm2708_timer_irq = {
	.name		= "BCM2708 Timer Tick",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= bcm2708_timer_interrupt,
};

/*
 * Set up timer interrupt, and return the current time in seconds.
 */
static void __init bcm2708_timer_init(void)
{
	/*
	 * Initialise to a known state (all timers off)
	 */
	writel(0, __io_address(ARM_T_CONTROL));
	/*
	 * Make irqs happen for the system timer
	 */
	setup_irq(IRQ_TIMER3, &bcm2708_timer_irq);

	timer0_clockevent.mult =
		div_sc(1000000, NSEC_PER_SEC, timer0_clockevent.shift);
	timer0_clockevent.max_delta_ns =
		clockevent_delta2ns(0xffffffff, &timer0_clockevent);
	timer0_clockevent.min_delta_ns =
		clockevent_delta2ns(0xf, &timer0_clockevent);

	timer0_clockevent.cpumask = cpumask_of(0);
	clockevents_register_device(&timer0_clockevent);
}

struct sys_timer bcm2708_timer = {
	.init		= bcm2708_timer_init,
};

#if defined(CONFIG_LEDS_GPIO) || defined(CONFIG_LEDS_GPIO_MODULE)
#include <linux/leds.h>

static struct gpio_led bcm2708_leds[] = {
	[0] = {
		.gpio			= 16,
		.name			= "led0",
		.default_trigger	= "mmc0",
		.active_low		= 0,
	},
};

static struct gpio_led_platform_data bcm2708_led_pdata = {
	.num_leds	= ARRAY_SIZE(bcm2708_leds),
	.leds		= bcm2708_leds,
};

static struct platform_device bcm2708_led_device = {
	.name		= "leds-gpio",
	.id		= -1,
	.dev		= {
		.platform_data	= &bcm2708_led_pdata,
	},
};

static void __init bcm2708_init_led(void)
{
	platform_device_register(&bcm2708_led_device);
}
#else
static inline void bcm2708_init_led(void) {}
#endif


MACHINE_START(BCM2708, "BCM2708")
	/* Maintainer: Broadcom Europe Ltd. */
	.map_io		= bcm2708_map_io,
	.init_irq	= bcm2708_init_irq,
	.timer		= &bcm2708_timer,
	.init_machine	= bcm2708_init,
MACHINE_END
