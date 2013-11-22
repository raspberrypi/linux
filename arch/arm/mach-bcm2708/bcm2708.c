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
#include <linux/syscore_ops.h>
#include <linux/interrupt.h>
#include <linux/amba/bus.h>
#include <linux/amba/clcd.h>
#include <linux/clockchips.h>
#include <linux/cnt32_to_63.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/w1-gpio.h>

#include <linux/version.h>
#include <linux/clkdev.h>
#include <asm/system.h>
#include <mach/hardware.h>
#include <asm/irq.h>
#include <linux/leds.h>
#include <asm/mach-types.h>
#include <linux/sched_clock.h>

#include <asm/mach/arch.h>
#include <asm/mach/flash.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>
#include <asm/mach/map.h>

#include <mach/timex.h>
#include <mach/dma.h>
#include <mach/vcio.h>
#include <mach/system.h>

#include <linux/delay.h>

#include "bcm2708.h"
#include "armctrl.h"
#include "clock.h"

#ifdef CONFIG_BCM_VC_CMA
#include <linux/broadcom/vc_cma.h>
#endif


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

// use GPIO 4 for the one-wire GPIO pin, if enabled
#define W1_GPIO 4

/* command line parameters */
static unsigned boardrev, serial;
static unsigned uart_clock;
static unsigned disk_led_gpio = 16;
static unsigned disk_led_active_low = 1;
static unsigned reboot_part = 0;
static unsigned w1_gpio_pin = W1_GPIO;

static void __init bcm2708_init_led(void);

void __init bcm2708_init_irq(void)
{
	armctrl_init(__io_address(ARMCTRL_IC_BASE), 0, 0, 0);
}

static struct map_desc bcm2708_io_desc[] __initdata = {
	{
	 .virtual = IO_ADDRESS(ARMCTRL_BASE),
	 .pfn = __phys_to_pfn(ARMCTRL_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(UART0_BASE),
	 .pfn = __phys_to_pfn(UART0_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(UART1_BASE),
	 .pfn = __phys_to_pfn(UART1_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(DMA_BASE),
	 .pfn = __phys_to_pfn(DMA_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(MCORE_BASE),
	 .pfn = __phys_to_pfn(MCORE_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(ST_BASE),
	 .pfn = __phys_to_pfn(ST_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(USB_BASE),
	 .pfn = __phys_to_pfn(USB_BASE),
	 .length = SZ_128K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(PM_BASE),
	 .pfn = __phys_to_pfn(PM_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE},
	{
	 .virtual = IO_ADDRESS(GPIO_BASE),
	 .pfn = __phys_to_pfn(GPIO_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE}
};

void __init bcm2708_map_io(void)
{
	iotable_init(bcm2708_io_desc, ARRAY_SIZE(bcm2708_io_desc));
}

/* The STC is a free running counter that increments at the rate of 1MHz */
#define STC_FREQ_HZ 1000000

static inline uint32_t timer_read(void)
{
	/* STC: a free running counter that increments at the rate of 1MHz */
	return readl(__io_address(ST_BASE + 0x04));
}

static unsigned long bcm2708_read_current_timer(void)
{
	return timer_read();
}

static u32 notrace bcm2708_read_sched_clock(void)
{
	return timer_read();
}

static cycle_t clksrc_read(struct clocksource *cs)
{
	return timer_read();
}

static struct clocksource clocksource_stc = {
	.name = "stc",
	.rating = 300,
	.read = clksrc_read,
	.mask = CLOCKSOURCE_MASK(32),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

unsigned long frc_clock_ticks32(void)
{
	return timer_read();
}

static void __init bcm2708_clocksource_init(void)
{
	if (clocksource_register_hz(&clocksource_stc, STC_FREQ_HZ)) {
		printk(KERN_ERR "timer: failed to initialize clock "
		       "source %s\n", clocksource_stc.name);
	}
}


/*
 * These are fixed clocks.
 */
static struct clk ref24_clk = {
	.rate = UART0_CLOCK,	/* The UART is clocked at 3MHz via APB_CLK */
};

static struct clk osc_clk = {
#ifdef CONFIG_ARCH_BCM2708_CHIPIT
	.rate = 27000000,
#else
	.rate = 500000000,	/* ARM clock is set from the VideoCore booter */
#endif
};

/* warning - the USB needs a clock > 34MHz */

static struct clk sdhost_clk = {
#ifdef CONFIG_ARCH_BCM2708_CHIPIT
	.rate = 4000000,	/* 4MHz */
#else
	.rate = 250000000,	/* 250MHz */
#endif
};

static struct clk_lookup lookups[] = {
	{			/* UART0 */
	 .dev_id = "dev:f1",
	 .clk = &ref24_clk,
	 },
	{			/* USB */
	 .dev_id = "bcm2708_usb",
	 .clk = &osc_clk,
	 }, {	/* SPI */
		 .dev_id = "bcm2708_spi.0",
		 .clk = &sdhost_clk,
	 }, {	/* BSC0 */
		 .dev_id = "bcm2708_i2c.0",
		 .clk = &sdhost_clk,
	 }, {	/* BSC1 */
		 .dev_id = "bcm2708_i2c.1",
		 .clk = &sdhost_clk,
	 }
};

#define UART0_IRQ	{ IRQ_UART, 0 /*NO_IRQ*/ }
#define UART0_DMA	{ 15, 14 }

AMBA_DEVICE(uart0, "dev:f1", UART0, NULL);

static struct amba_device *amba_devs[] __initdata = {
	&uart0_device,
};

static struct resource bcm2708_dmaman_resources[] = {
	{
	 .start = DMA_BASE,
	 .end = DMA_BASE + SZ_4K - 1,
	 .flags = IORESOURCE_MEM,
	 }
};

static struct platform_device bcm2708_dmaman_device = {
	.name = BCM_DMAMAN_DRIVER_NAME,
	.id = 0,		/* first bcm2708_dma */
	.resource = bcm2708_dmaman_resources,
	.num_resources = ARRAY_SIZE(bcm2708_dmaman_resources),
};

#if defined(CONFIG_W1_MASTER_GPIO) || defined(CONFIG_W1_MASTER_GPIO_MODULE)
static struct w1_gpio_platform_data w1_gpio_pdata = {
	.pin = W1_GPIO,
	.is_open_drain = 0,
};

static struct platform_device w1_device = {
	.name = "w1-gpio",
	.id = -1,
	.dev.platform_data = &w1_gpio_pdata,
};
#endif

static u64 fb_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_fb_device = {
	.name = "bcm2708_fb",
	.id = -1,		/* only one bcm2708_fb */
	.resource = NULL,
	.num_resources = 0,
	.dev = {
		.dma_mask = &fb_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
		},
};

static struct plat_serial8250_port bcm2708_uart1_platform_data[] = {
	{
	 .mapbase = UART1_BASE + 0x40,
	 .irq = IRQ_AUX,
	 .uartclk = 125000000,
	 .regshift = 2,
	 .iotype = UPIO_MEM,
	 .flags = UPF_FIXED_TYPE | UPF_IOREMAP | UPF_SKIP_TEST,
	 .type = PORT_8250,
	 },
	{},
};

static struct platform_device bcm2708_uart1_device = {
	.name = "serial8250",
	.id = PLAT8250_DEV_PLATFORM,
	.dev = {
		.platform_data = bcm2708_uart1_platform_data,
		},
};

static struct resource bcm2708_usb_resources[] = {
	[0] = {
	       .start = USB_BASE,
	       .end = USB_BASE + SZ_128K - 1,
	       .flags = IORESOURCE_MEM,
	       },
	[1] = {
		.start = MPHI_BASE,
		.end = MPHI_BASE + SZ_4K - 1,
		.flags = IORESOURCE_MEM,
	      },
	[2] = {
	       .start = IRQ_HOSTPORT,
	       .end = IRQ_HOSTPORT,
	       .flags = IORESOURCE_IRQ,
	       },
};


static u64 usb_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_usb_device = {
	.name = "bcm2708_usb",
	.id = -1,		/* only one bcm2708_usb */
	.resource = bcm2708_usb_resources,
	.num_resources = ARRAY_SIZE(bcm2708_usb_resources),
	.dev = {
		.dma_mask = &usb_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
		},
};

static struct resource bcm2708_vcio_resources[] = {
	[0] = {			/* mailbox/semaphore/doorbell access */
	       .start = MCORE_BASE,
	       .end = MCORE_BASE + SZ_4K - 1,
	       .flags = IORESOURCE_MEM,
	       },
};

static u64 vcio_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_vcio_device = {
	.name = BCM_VCIO_DRIVER_NAME,
	.id = -1,		/* only one VideoCore I/O area */
	.resource = bcm2708_vcio_resources,
	.num_resources = ARRAY_SIZE(bcm2708_vcio_resources),
	.dev = {
		.dma_mask = &vcio_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
		},
};

#ifdef CONFIG_BCM2708_GPIO
#define BCM_GPIO_DRIVER_NAME "bcm2708_gpio"

static struct resource bcm2708_gpio_resources[] = {
	[0] = {			/* general purpose I/O */
	       .start = GPIO_BASE,
	       .end = GPIO_BASE + SZ_4K - 1,
	       .flags = IORESOURCE_MEM,
	       },
};

static u64 gpio_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_gpio_device = {
	.name = BCM_GPIO_DRIVER_NAME,
	.id = -1,		/* only one VideoCore I/O area */
	.resource = bcm2708_gpio_resources,
	.num_resources = ARRAY_SIZE(bcm2708_gpio_resources),
	.dev = {
		.dma_mask = &gpio_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
		},
};
#endif

static struct resource bcm2708_systemtimer_resources[] = {
	[0] = {			/* system timer access */
	       .start = ST_BASE,
	       .end = ST_BASE + SZ_4K - 1,
	       .flags = IORESOURCE_MEM,
	       },
	{
	 .start = IRQ_TIMER3,
	 .end = IRQ_TIMER3,
	 .flags = IORESOURCE_IRQ,
	 }

};

static u64 systemtimer_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_systemtimer_device = {
	.name = "bcm2708_systemtimer",
	.id = -1,		/* only one VideoCore I/O area */
	.resource = bcm2708_systemtimer_resources,
	.num_resources = ARRAY_SIZE(bcm2708_systemtimer_resources),
	.dev = {
		.dma_mask = &systemtimer_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
		},
};

#ifdef CONFIG_MMC_SDHCI_BCM2708	/* Arasan emmc SD */
static struct resource bcm2708_emmc_resources[] = {
	[0] = {
	       .start = EMMC_BASE,
	       .end = EMMC_BASE + SZ_256 - 1,	/* we only need this area */
	       /* the memory map actually makes SZ_4K available  */
	       .flags = IORESOURCE_MEM,
	       },
	[1] = {
	       .start = IRQ_ARASANSDIO,
	       .end = IRQ_ARASANSDIO,
	       .flags = IORESOURCE_IRQ,
	       },
};

static u64 bcm2708_emmc_dmamask = 0xffffffffUL;

struct platform_device bcm2708_emmc_device = {
	.name = "bcm2708_sdhci",
	.id = 0,
	.num_resources = ARRAY_SIZE(bcm2708_emmc_resources),
	.resource = bcm2708_emmc_resources,
	.dev = {
		.dma_mask = &bcm2708_emmc_dmamask,
		.coherent_dma_mask = 0xffffffffUL},
};
#endif /* CONFIG_MMC_SDHCI_BCM2708 */

static struct resource bcm2708_powerman_resources[] = {
	[0] = {
	       .start = PM_BASE,
	       .end = PM_BASE + SZ_256 - 1,
	       .flags = IORESOURCE_MEM,
	       },
};

static u64 powerman_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

struct platform_device bcm2708_powerman_device = {
	.name = "bcm2708_powerman",
	.id = 0,
	.num_resources = ARRAY_SIZE(bcm2708_powerman_resources),
	.resource = bcm2708_powerman_resources,
	.dev = {
		.dma_mask = &powerman_dmamask,
		.coherent_dma_mask = 0xffffffffUL},
};


static struct platform_device bcm2708_alsa_devices[] = {
	[0] = {
	       .name = "bcm2835_AUD0",
	       .id = 0,		/* first audio device */
	       .resource = 0,
	       .num_resources = 0,
	       },
	[1] = {
	       .name = "bcm2835_AUD1",
	       .id = 1,		/* second audio device */
	       .resource = 0,
	       .num_resources = 0,
	       },
	[2] = {
	       .name = "bcm2835_AUD2",
	       .id = 2,		/* third audio device */
	       .resource = 0,
	       .num_resources = 0,
	       },
	[3] = {
	       .name = "bcm2835_AUD3",
	       .id = 3,		/* forth audio device */
	       .resource = 0,
	       .num_resources = 0,
	       },
	[4] = {
	       .name = "bcm2835_AUD4",
	       .id = 4,		/* fifth audio device */
	       .resource = 0,
	       .num_resources = 0,
	       },
	[5] = {
	       .name = "bcm2835_AUD5",
	       .id = 5,		/* sixth audio device */
	       .resource = 0,
	       .num_resources = 0,
	       },
	[6] = {
	       .name = "bcm2835_AUD6",
	       .id = 6,		/* seventh audio device */
	       .resource = 0,
	       .num_resources = 0,
	       },
	[7] = {
	       .name = "bcm2835_AUD7",
	       .id = 7,		/* eighth audio device */
	       .resource = 0,
	       .num_resources = 0,
	       },
};

static struct resource bcm2708_spi_resources[] = {
	{
		.start = SPI0_BASE,
		.end = SPI0_BASE + SZ_256 - 1,
		.flags = IORESOURCE_MEM,
	}, {
		.start = IRQ_SPI,
		.end = IRQ_SPI,
		.flags = IORESOURCE_IRQ,
	}
};


static u64 bcm2708_spi_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);
static struct platform_device bcm2708_spi_device = {
	.name = "bcm2708_spi",
	.id = 0,
	.num_resources = ARRAY_SIZE(bcm2708_spi_resources),
	.resource = bcm2708_spi_resources,
	.dev = {
		.dma_mask = &bcm2708_spi_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON)},
};

#ifdef CONFIG_BCM2708_SPIDEV
static struct spi_board_info bcm2708_spi_devices[] = {
#ifdef CONFIG_SPI_SPIDEV
	{
		.modalias = "spidev",
		.max_speed_hz = 500000,
		.bus_num = 0,
		.chip_select = 0,
		.mode = SPI_MODE_0,
	}, {
		.modalias = "spidev",
		.max_speed_hz = 500000,
		.bus_num = 0,
		.chip_select = 1,
		.mode = SPI_MODE_0,
	}
#endif
};
#endif

static struct resource bcm2708_bsc0_resources[] = {
	{
		.start = BSC0_BASE,
		.end = BSC0_BASE + SZ_256 - 1,
		.flags = IORESOURCE_MEM,
	}, {
		.start = INTERRUPT_I2C,
		.end = INTERRUPT_I2C,
		.flags = IORESOURCE_IRQ,
	}
};

static struct platform_device bcm2708_bsc0_device = {
	.name = "bcm2708_i2c",
	.id = 0,
	.num_resources = ARRAY_SIZE(bcm2708_bsc0_resources),
	.resource = bcm2708_bsc0_resources,
};


static struct resource bcm2708_bsc1_resources[] = {
	{
		.start = BSC1_BASE,
		.end = BSC1_BASE + SZ_256 - 1,
		.flags = IORESOURCE_MEM,
	}, {
		.start = INTERRUPT_I2C,
		.end = INTERRUPT_I2C,
		.flags = IORESOURCE_IRQ,
	}
};

static struct platform_device bcm2708_bsc1_device = {
	.name = "bcm2708_i2c",
	.id = 1,
	.num_resources = ARRAY_SIZE(bcm2708_bsc1_resources),
	.resource = bcm2708_bsc1_resources,
};

static struct platform_device bcm2835_hwmon_device = {
	.name = "bcm2835_hwmon",
};

static struct platform_device bcm2835_thermal_device = {
	.name = "bcm2835_thermal",
};

#ifdef CONFIG_SND_BCM2708_SOC_I2S_MODULE
static struct resource bcm2708_i2s_resources[] = {
	{
		.start = I2S_BASE,
		.end = I2S_BASE + 0x20,
		.flags = IORESOURCE_MEM,
	},
        {
		.start = PCM_CLOCK_BASE,
		.end = PCM_CLOCK_BASE + 0x02,
		.flags = IORESOURCE_MEM,
	}
};

static struct platform_device bcm2708_i2s_device = {
	.name = "bcm2708-i2s",
	.id = 0,
	.num_resources = ARRAY_SIZE(bcm2708_i2s_resources),
	.resource = bcm2708_i2s_resources,
};
#endif

#if defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DAC) || defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DAC_MODULE)
static struct platform_device snd_hifiberry_dac_device = {
        .name = "snd-hifiberry-dac",
        .id = 0,
        .num_resources = 0,
};

static struct platform_device snd_pcm5102a_codec_device = {
        .name = "pcm5102a-codec",
        .id = -1,
        .num_resources = 0,
};
#endif

int __init bcm_register_device(struct platform_device *pdev)
{
	int ret;

	ret = platform_device_register(pdev);
	if (ret)
		pr_debug("Unable to register platform device '%s': %d\n",
			 pdev->name, ret);

	return ret;
}

int calc_rsts(int partition)
{
	return PM_PASSWORD |
		((partition & (1 << 0))  << 0) |
		((partition & (1 << 1))  << 1) |
		((partition & (1 << 2))  << 2) |
		((partition & (1 << 3))  << 3) |
		((partition & (1 << 4))  << 4) |
		((partition & (1 << 5))  << 5);
}

static void bcm2708_restart(enum reboot_mode mode, const char *cmd)
{
	extern char bcm2708_reboot_mode;
	uint32_t pm_rstc, pm_wdog;
	uint32_t timeout = 10;
	uint32_t pm_rsts = 0;

	if(bcm2708_reboot_mode == 'q')
	{
		// NOOBS < 1.3 booting with reboot=q
		pm_rsts = readl(__io_address(PM_RSTS));
		pm_rsts = PM_PASSWORD | pm_rsts | PM_RSTS_HADWRQ_SET;
	}
	else if(bcm2708_reboot_mode == 'p')
	{
		// NOOBS < 1.3 halting
		pm_rsts = readl(__io_address(PM_RSTS));
		pm_rsts = PM_PASSWORD | pm_rsts | PM_RSTS_HADWRH_SET;
	}
	else
	{
		pm_rsts = calc_rsts(reboot_part);
	}

	writel(pm_rsts, __io_address(PM_RSTS));

	/* Setup watchdog for reset */
	pm_rstc = readl(__io_address(PM_RSTC));

	pm_wdog = PM_PASSWORD | (timeout & PM_WDOG_TIME_SET); // watchdog timer = timer clock / 16; need password (31:16) + value (11:0)
	pm_rstc = PM_PASSWORD | (pm_rstc & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET;

	writel(pm_wdog, __io_address(PM_WDOG));
	writel(pm_rstc, __io_address(PM_RSTC));
}

/* We can't really power off, but if we do the normal reset scheme, and indicate to bootcode.bin not to reboot, then most of the chip will be powered off */
static void bcm2708_power_off(void)
{
	extern char bcm2708_reboot_mode;
	if(bcm2708_reboot_mode == 'q')
	{
		// NOOBS < v1.3
		bcm2708_restart('p', "");
	}
	else
	{
		/* partition 63 is special code for HALT the bootloader knows not to boot*/
		reboot_part = 63;
		/* continue with normal reset mechanism */
		bcm2708_restart(0, "");
	}
}

void __init bcm2708_init(void)
{
	int i;

#if defined(CONFIG_BCM_VC_CMA)
	vc_cma_early_init();
#endif
	printk("bcm2708.uart_clock = %d\n", uart_clock);
	pm_power_off = bcm2708_power_off;

	if (uart_clock)
		lookups[0].clk->rate = uart_clock;

	for (i = 0; i < ARRAY_SIZE(lookups); i++)
		clkdev_add(&lookups[i]);

	bcm_register_device(&bcm2708_dmaman_device);
	bcm_register_device(&bcm2708_vcio_device);
#ifdef CONFIG_BCM2708_GPIO
	bcm_register_device(&bcm2708_gpio_device);
#endif
#if defined(CONFIG_W1_MASTER_GPIO) || defined(CONFIG_W1_MASTER_GPIO_MODULE)
	w1_gpio_pdata.pin = w1_gpio_pin;
	platform_device_register(&w1_device);
#endif
	bcm_register_device(&bcm2708_systemtimer_device);
	bcm_register_device(&bcm2708_fb_device);
	bcm_register_device(&bcm2708_usb_device);
	bcm_register_device(&bcm2708_uart1_device);
	bcm_register_device(&bcm2708_powerman_device);

#ifdef CONFIG_MMC_SDHCI_BCM2708
	bcm_register_device(&bcm2708_emmc_device);
#endif
	bcm2708_init_led();
	for (i = 0; i < ARRAY_SIZE(bcm2708_alsa_devices); i++)
		bcm_register_device(&bcm2708_alsa_devices[i]);

	bcm_register_device(&bcm2708_spi_device);
	bcm_register_device(&bcm2708_bsc0_device);
	bcm_register_device(&bcm2708_bsc1_device);

	bcm_register_device(&bcm2835_hwmon_device);
	bcm_register_device(&bcm2835_thermal_device);

#ifdef CONFIG_SND_BCM2708_SOC_I2S_MODULE
	bcm_register_device(&bcm2708_i2s_device);
#endif

#if defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DAC) || defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DAC_MODULE)
        bcm_register_device(&snd_hifiberry_dac_device);
        bcm_register_device(&snd_pcm5102a_codec_device);
#endif

	for (i = 0; i < ARRAY_SIZE(amba_devs); i++) {
		struct amba_device *d = amba_devs[i];
		amba_device_register(d, &iomem_resource);
	}
	system_rev = boardrev;
	system_serial_low = serial;

#ifdef CONFIG_BCM2708_SPIDEV
	spi_register_board_info(bcm2708_spi_devices,
			ARRAY_SIZE(bcm2708_spi_devices));
#endif
}

static void timer_set_mode(enum clock_event_mode mode,
			   struct clock_event_device *clk)
{
	switch (mode) {
	case CLOCK_EVT_MODE_ONESHOT: /* Leave the timer disabled, .set_next_event will enable it */
	case CLOCK_EVT_MODE_SHUTDOWN:
		break;
	case CLOCK_EVT_MODE_PERIODIC:

	case CLOCK_EVT_MODE_UNUSED:
	case CLOCK_EVT_MODE_RESUME:

	default:
		printk(KERN_ERR "timer_set_mode: unhandled mode:%d\n",
		       (int)mode);
		break;
	}

}

static int timer_set_next_event(unsigned long cycles,
				struct clock_event_device *unused)
{
	unsigned long stc;

	stc = readl(__io_address(ST_BASE + 0x04));
	writel(stc + cycles, __io_address(ST_BASE + 0x18));	/* stc3 */
	return 0;
}

static struct clock_event_device timer0_clockevent = {
	.name = "timer0",
	.shift = 32,
	.features = CLOCK_EVT_FEAT_ONESHOT,
	.set_mode = timer_set_mode,
	.set_next_event = timer_set_next_event,
};

/*
 * IRQ handler for the timer
 */
static irqreturn_t bcm2708_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = &timer0_clockevent;

	writel(1 << 3, __io_address(ST_BASE + 0x00));	/* stcs clear timer int */

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct irqaction bcm2708_timer_irq = {
	.name = "BCM2708 Timer Tick",
	.flags = IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler = bcm2708_timer_interrupt,
};

/*
 * Set up timer interrupt, and return the current time in seconds.
 */

static struct delay_timer bcm2708_delay_timer = {
	.read_current_timer = bcm2708_read_current_timer,
	.freq = STC_FREQ_HZ,
};

static void __init bcm2708_timer_init(void)
{
	/* init high res timer */
	bcm2708_clocksource_init();

	/*
	 * Initialise to a known state (all timers off)
	 */
	writel(0, __io_address(ARM_T_CONTROL));
	/*
	 * Make irqs happen for the system timer
	 */
	setup_irq(IRQ_TIMER3, &bcm2708_timer_irq);

	setup_sched_clock(bcm2708_read_sched_clock, 32, STC_FREQ_HZ);

	timer0_clockevent.mult =
	    div_sc(STC_FREQ_HZ, NSEC_PER_SEC, timer0_clockevent.shift);
	timer0_clockevent.max_delta_ns =
	    clockevent_delta2ns(0xffffffff, &timer0_clockevent);
	timer0_clockevent.min_delta_ns =
	    clockevent_delta2ns(0xf, &timer0_clockevent);

	timer0_clockevent.cpumask = cpumask_of(0);
	clockevents_register_device(&timer0_clockevent);

	register_current_timer_delay(&bcm2708_delay_timer);
}

#if defined(CONFIG_LEDS_GPIO) || defined(CONFIG_LEDS_GPIO_MODULE)
#include <linux/leds.h>

static struct gpio_led bcm2708_leds[] = {
	[0] = {
	       .gpio = 16,
	       .name = "led0",
	       .default_trigger = "mmc0",
	       .active_low = 1,
	       },
};

static struct gpio_led_platform_data bcm2708_led_pdata = {
	.num_leds = ARRAY_SIZE(bcm2708_leds),
	.leds = bcm2708_leds,
};

static struct platform_device bcm2708_led_device = {
	.name = "leds-gpio",
	.id = -1,
	.dev = {
		.platform_data = &bcm2708_led_pdata,
		},
};

static void __init bcm2708_init_led(void)
{
  bcm2708_leds[0].gpio = disk_led_gpio;
  bcm2708_leds[0].active_low = disk_led_active_low;
  platform_device_register(&bcm2708_led_device);
}
#else
static inline void bcm2708_init_led(void)
{
}
#endif

void __init bcm2708_init_early(void)
{
	/*
	 * Some devices allocate their coherent buffers from atomic
	 * context. Increase size of atomic coherent pool to make sure such
	 * the allocations won't fail.
	 */
	init_dma_coherent_pool_size(SZ_4M);
}

static void __init board_reserve(void)
{
#if defined(CONFIG_BCM_VC_CMA)
	vc_cma_reserve();
#endif
}

MACHINE_START(BCM2708, "BCM2708")
    /* Maintainer: Broadcom Europe Ltd. */
	.map_io = bcm2708_map_io,
	.init_irq = bcm2708_init_irq,
	.init_time = bcm2708_timer_init,
	.init_machine = bcm2708_init,
	.init_early = bcm2708_init_early,
	.reserve = board_reserve,
	.restart	= bcm2708_restart,
MACHINE_END

module_param(boardrev, uint, 0644);
module_param(serial, uint, 0644);
module_param(uart_clock, uint, 0644);
module_param(disk_led_gpio, uint, 0644);
module_param(disk_led_active_low, uint, 0644);
module_param(reboot_part, uint, 0644);
module_param(w1_gpio_pin, uint, 0644);
