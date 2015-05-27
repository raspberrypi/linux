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
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/clockchips.h>
#include <linux/cnt32_to_63.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/spi/spi.h>
#include <linux/gpio/machine.h>
#include <linux/w1-gpio.h>
#include <linux/pps-gpio.h>

#include <linux/version.h>
#include <linux/clkdev.h>
#include <asm/system_info.h>
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
#include <mach/system.h>

#include <linux/delay.h>

#include "bcm2708.h"
#include "armctrl.h"

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
// ensure one-wire GPIO pullup is disabled by default
#define W1_PULLUP -1

/* command line parameters */
static unsigned boardrev, serial;
static unsigned uart_clock = UART0_CLOCK;
static unsigned disk_led_gpio = 16;
static unsigned disk_led_active_low = 1;
static unsigned reboot_part = 0;
static unsigned w1_gpio_pin = W1_GPIO;
static unsigned w1_gpio_pullup = W1_PULLUP;
static bool vc_i2c_override = false;
static int pps_gpio_pin = -1;

static unsigned use_dt = 0;

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

static u64 notrace bcm2708_read_sched_clock(void)
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

struct clk __init *bcm2708_clk_register(const char *name, unsigned long fixed_rate)
{
	struct clk *clk;

	clk = clk_register_fixed_rate(NULL, name, NULL, CLK_IS_ROOT,
						fixed_rate);
	if (IS_ERR(clk))
		pr_err("%s not registered\n", name);

	return clk;
}

void __init bcm2708_register_clkdev(struct clk *clk, const char *name)
{
	int ret;

	ret = clk_register_clkdev(clk, NULL, name);
	if (ret)
		pr_err("%s alias not registered\n", name);
}

void __init bcm2708_init_clocks(void)
{
	struct clk *clk;

	clk = bcm2708_clk_register("uart0_clk", uart_clock);
	bcm2708_register_clkdev(clk, "dev:f1");

	clk = bcm2708_clk_register("sdhost_clk", 250000000);
	bcm2708_register_clkdev(clk, "mmc-bcm2835.0");
	bcm2708_register_clkdev(clk, "bcm2708_spi.0");
	bcm2708_register_clkdev(clk, "bcm2708_i2c.0");
	bcm2708_register_clkdev(clk, "bcm2708_i2c.1");
}

#define UART0_IRQ	{ IRQ_UART, 0 /*NO_IRQ*/ }
#define UART0_DMA	{ 15, 14 }

AMBA_DEVICE(uart0, "dev:f1", UART0, NULL);

static struct amba_device *amba_devs[] __initdata = {
	&uart0_device,
};

static struct resource bcm2708_dmaengine_resources[] = {
	{
		.start = DMA_BASE,
		.end = DMA_BASE + SZ_4K - 1,
		.flags = IORESOURCE_MEM,
	}, {
		.start = IRQ_DMA0,
		.end = IRQ_DMA0,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA1,
		.end = IRQ_DMA1,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA2,
		.end = IRQ_DMA2,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA3,
		.end = IRQ_DMA3,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA4,
		.end = IRQ_DMA4,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA5,
		.end = IRQ_DMA5,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA6,
		.end = IRQ_DMA6,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA7,
		.end = IRQ_DMA7,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA8,
		.end = IRQ_DMA8,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA9,
		.end = IRQ_DMA9,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA10,
		.end = IRQ_DMA10,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA11,
		.end = IRQ_DMA11,
		.flags = IORESOURCE_IRQ,
	}, {
		.start = IRQ_DMA12,
		.end = IRQ_DMA12,
		.flags = IORESOURCE_IRQ,
	}
};

static struct platform_device bcm2708_dmaengine_device = {
	.name = "bcm2708-dmaengine",
	.id = -1,
	.resource = bcm2708_dmaengine_resources,
	.num_resources = ARRAY_SIZE(bcm2708_dmaengine_resources),
};

#if defined(CONFIG_W1_MASTER_GPIO) || defined(CONFIG_W1_MASTER_GPIO_MODULE)
static struct w1_gpio_platform_data w1_gpio_pdata = {
	.pin = W1_GPIO,
        .ext_pullup_enable_pin = W1_PULLUP,
	.is_open_drain = 0,
};

static struct platform_device w1_device = {
	.name = "w1-gpio",
	.id = -1,
	.dev.platform_data = &w1_gpio_pdata,
};
#endif

static struct pps_gpio_platform_data pps_gpio_info = {
	.assert_falling_edge = false,
	.capture_clear = false,
	.gpio_pin = -1,
	.gpio_label = "PPS",
};

static struct platform_device pps_gpio_device = {
	.name = "pps-gpio",
	.id = PLATFORM_DEVID_NONE,
	.dev.platform_data = &pps_gpio_info,
};

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
	[3] = {
		.start = IRQ_USB,
		.end = IRQ_USB,
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
	{
		.start = ARMCTRL_0_MAIL0_BASE,
		.end = ARMCTRL_0_MAIL0_BASE + SZ_64 - 1,
		.flags = IORESOURCE_MEM,
	}, {
		.start = IRQ_ARM_MAILBOX,
		.end = IRQ_ARM_MAILBOX,
		.flags = IORESOURCE_IRQ,
	},
};

static u64 vcio_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_vcio_device = {
	.name = "bcm2708_vcio",
	.id = -1,		/* only one VideoCore I/O area */
	.resource = bcm2708_vcio_resources,
	.num_resources = ARRAY_SIZE(bcm2708_vcio_resources),
	.dev = {
		.dma_mask = &vcio_dmamask,
		.coherent_dma_mask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON),
		},
};

static struct resource bcm2708_vchiq_resources[] = {
	{
		.start = ARMCTRL_0_BELL_BASE,
		.end = ARMCTRL_0_BELL_BASE + 16,
		.flags = IORESOURCE_MEM,
	}, {
		.start = IRQ_ARM_DOORBELL_0,
		.end = IRQ_ARM_DOORBELL_0,
		.flags = IORESOURCE_IRQ,
	},
};

static u64 vchiq_dmamask = DMA_BIT_MASK(DMA_MASK_BITS_COMMON);

static struct platform_device bcm2708_vchiq_device = {
	.name = "bcm2835_vchiq",
	.id = -1,
	.resource = bcm2708_vchiq_resources,
	.num_resources = ARRAY_SIZE(bcm2708_vchiq_resources),
	.dev = {
		.dma_mask = &vchiq_dmamask,
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

#ifdef CONFIG_MMC_BCM2835	/* Arasan emmc SD (new) */
static struct resource bcm2835_emmc_resources[] = {
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

static u64 bcm2835_emmc_dmamask = 0xffffffffUL;

struct platform_device bcm2835_emmc_device = {
	.name = "mmc-bcm2835",
	.id = 0,
	.num_resources = ARRAY_SIZE(bcm2835_emmc_resources),
	.resource = bcm2835_emmc_resources,
	.dev = {
		.dma_mask = &bcm2835_emmc_dmamask,
		.coherent_dma_mask = 0xffffffffUL},
};
#endif /* CONFIG_MMC_BCM2835 */

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

static struct platform_device bcm2835_thermal_device = {
	.name = "bcm2835_thermal",
};

#if defined(CONFIG_SND_BCM2708_SOC_I2S) || defined(CONFIG_SND_BCM2708_SOC_I2S_MODULE)
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

#if defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DACPLUS) || defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DACPLUS_MODULE)
static struct platform_device snd_rpi_hifiberry_dacplus_device = {
        .name = "snd-rpi-hifiberry-dacplus",
        .id = 0,
        .num_resources = 0,
};

static struct i2c_board_info __initdata snd_pcm512x_hbdacplus_i2c_devices[] = {
        {
                I2C_BOARD_INFO("pcm5122", 0x4d)
        },
};
#endif

#if defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DIGI) || defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DIGI_MODULE)
static struct platform_device snd_hifiberry_digi_device = {
        .name = "snd-hifiberry-digi",
        .id = 0,
        .num_resources = 0,
};

static struct i2c_board_info __initdata snd_wm8804_i2c_devices[] = {
        {
                I2C_BOARD_INFO("wm8804", 0x3b)
        },
};

#endif

#if defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_AMP) || defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_AMP_MODULE)
static struct platform_device snd_hifiberry_amp_device = {
        .name = "snd-hifiberry-amp",
        .id = 0,
        .num_resources = 0,
};

static struct i2c_board_info __initdata snd_tas5713_i2c_devices[] = {
        {
                I2C_BOARD_INFO("tas5713", 0x1b)
        },
};
#endif

#if defined(CONFIG_SND_BCM2708_SOC_RPI_DAC) || defined(CONFIG_SND_BCM2708_SOC_RPI_DAC_MODULE)
static struct platform_device snd_rpi_dac_device = {
        .name = "snd-rpi-dac",
        .id = 0,
        .num_resources = 0,
};

static struct platform_device snd_pcm1794a_codec_device = {
        .name = "pcm1794a-codec",
        .id = -1,
        .num_resources = 0,
};
#endif


#if defined(CONFIG_SND_BCM2708_SOC_IQAUDIO_DAC) || defined(CONFIG_SND_BCM2708_SOC_IQAUDIO_DAC_MODULE)
static struct platform_device snd_rpi_iqaudio_dac_device = {
        .name = "snd-rpi-iqaudio-dac",
        .id = 0,
        .num_resources = 0,
};

// Use the actual device name rather than generic driver name
static struct i2c_board_info __initdata snd_pcm512x_i2c_devices[] = {
	{
		I2C_BOARD_INFO("pcm5122", 0x4c)
	},
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

/*
 * Use these macros for platform and i2c devices that are present in the
 * Device Tree. This way the devices are only added on non-DT systems.
 */
#define bcm_register_device_dt(pdev) \
    if (!use_dt) bcm_register_device(pdev)

#define i2c_register_board_info_dt(busnum, info, n) \
    if (!use_dt) i2c_register_board_info(busnum, info, n)

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

static void __init bcm2708_init_uart1(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "brcm,bcm2835-aux-uart");
	if (of_device_is_available(np)) {
		pr_info("bcm2708: Mini UART enabled\n");
		writel(1, __io_address(UART1_BASE + 0x4));
	}
}

#ifdef CONFIG_OF
static void __init bcm2708_dt_init(void)
{
	int ret;

	of_clk_init(NULL);
	ret = of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
	if (ret) {
		pr_err("of_platform_populate failed: %d\n", ret);
		/* Proceed as if CONFIG_OF was not defined */
	} else {
		use_dt = 1;
	}
}
#else
static void __init bcm2708_dt_init(void) { }
#endif /* CONFIG_OF */

void __init bcm2708_init(void)
{
	int i;

#if defined(CONFIG_BCM_VC_CMA)
	vc_cma_early_init();
#endif
	printk("bcm2708.uart_clock = %d\n", uart_clock);
	pm_power_off = bcm2708_power_off;

	bcm2708_init_clocks();
	bcm2708_dt_init();

	bcm_register_device_dt(&bcm2708_dmaengine_device);
	bcm_register_device_dt(&bcm2708_vcio_device);
	bcm_register_device_dt(&bcm2708_vchiq_device);
#ifdef CONFIG_BCM2708_GPIO
	bcm_register_device_dt(&bcm2708_gpio_device);
#endif

#if defined(CONFIG_PPS_CLIENT_GPIO) || defined(CONFIG_PPS_CLIENT_GPIO_MODULE)
	if (!use_dt && (pps_gpio_pin >= 0)) {
		pr_info("bcm2708: GPIO %d setup as pps-gpio device\n", pps_gpio_pin);
		pps_gpio_info.gpio_pin = pps_gpio_pin;
		pps_gpio_device.id = pps_gpio_pin;
		bcm_register_device(&pps_gpio_device);
	}
#endif

#if defined(CONFIG_W1_MASTER_GPIO) || defined(CONFIG_W1_MASTER_GPIO_MODULE)
	w1_gpio_pdata.pin = w1_gpio_pin;
	w1_gpio_pdata.ext_pullup_enable_pin = w1_gpio_pullup;
	bcm_register_device_dt(&w1_device);
#endif
	bcm_register_device_dt(&bcm2708_fb_device);
	bcm_register_device_dt(&bcm2708_usb_device);

#ifdef CONFIG_MMC_BCM2835
	bcm_register_device_dt(&bcm2835_emmc_device);
#endif
	bcm2708_init_led();
	bcm2708_init_uart1();

	/* Only create the platform devices for the ALSA driver in the
	   absence of an enabled "audio" DT node */
	if (!use_dt ||
	    !of_device_is_available(of_find_node_by_path("/audio"))) {
		for (i = 0; i < ARRAY_SIZE(bcm2708_alsa_devices); i++)
			bcm_register_device(&bcm2708_alsa_devices[i]);
	}

	bcm_register_device_dt(&bcm2708_spi_device);

	if (vc_i2c_override) {
		bcm_register_device_dt(&bcm2708_bsc0_device);
		bcm_register_device_dt(&bcm2708_bsc1_device);
	} else if ((boardrev & 0xffffff) == 0x2 || (boardrev & 0xffffff) == 0x3) {
		bcm_register_device_dt(&bcm2708_bsc0_device);
	} else {
		bcm_register_device_dt(&bcm2708_bsc1_device);
	}

	bcm_register_device_dt(&bcm2835_thermal_device);

#if defined(CONFIG_SND_BCM2708_SOC_I2S) || defined(CONFIG_SND_BCM2708_SOC_I2S_MODULE)
	bcm_register_device_dt(&bcm2708_i2s_device);
#endif

#if defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DAC) || defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DAC_MODULE)
        bcm_register_device_dt(&snd_hifiberry_dac_device);
        bcm_register_device_dt(&snd_pcm5102a_codec_device);
#endif

#if defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DACPLUS) || defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DACPLUS_MODULE)
        bcm_register_device_dt(&snd_rpi_hifiberry_dacplus_device);
        i2c_register_board_info_dt(1, snd_pcm512x_hbdacplus_i2c_devices, ARRAY_SIZE(snd_pcm512x_hbdacplus_i2c_devices));
#endif

#if defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DIGI) || defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_DIGI_MODULE)
        bcm_register_device_dt(&snd_hifiberry_digi_device);
        i2c_register_board_info_dt(1, snd_wm8804_i2c_devices, ARRAY_SIZE(snd_wm8804_i2c_devices));
#endif

#if defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_AMP) || defined(CONFIG_SND_BCM2708_SOC_HIFIBERRY_AMP_MODULE)
        bcm_register_device_dt(&snd_hifiberry_amp_device);
        i2c_register_board_info_dt(1, snd_tas5713_i2c_devices, ARRAY_SIZE(snd_tas5713_i2c_devices));
#endif

#if defined(CONFIG_SND_BCM2708_SOC_RPI_DAC) || defined(CONFIG_SND_BCM2708_SOC_RPI_DAC_MODULE)
        bcm_register_device_dt(&snd_rpi_dac_device);
        bcm_register_device_dt(&snd_pcm1794a_codec_device);
#endif

#if defined(CONFIG_SND_BCM2708_SOC_IQAUDIO_DAC) || defined(CONFIG_SND_BCM2708_SOC_IQAUDIO_DAC_MODULE)
        bcm_register_device_dt(&snd_rpi_iqaudio_dac_device);
        i2c_register_board_info_dt(1, snd_pcm512x_i2c_devices, ARRAY_SIZE(snd_pcm512x_i2c_devices));
#endif

	if (!use_dt) {
		for (i = 0; i < ARRAY_SIZE(amba_devs); i++) {
			struct amba_device *d = amba_devs[i];
			amba_device_register(d, &iomem_resource);
		}
	}
	system_rev = boardrev;
	system_serial_low = serial;

#ifdef CONFIG_BCM2708_SPIDEV
	if (!use_dt)
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
	do {
		stc = readl(__io_address(ST_BASE + 0x04));
		/* We could take a FIQ here, which may push ST above STC3 */
		writel(stc + cycles, __io_address(ST_BASE + 0x18));
	} while ((signed long) cycles >= 0 &&
				(signed long) (readl(__io_address(ST_BASE + 0x04)) - stc)
				>= (signed long) cycles);
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
	.flags = IRQF_TIMER | IRQF_IRQPOLL,
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
	 * Make irqs happen for the system timer
	 */
	setup_irq(IRQ_TIMER3, &bcm2708_timer_irq);

	sched_clock_register(bcm2708_read_sched_clock, 32, STC_FREQ_HZ);

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
	bcm_register_device_dt(&bcm2708_led_device);
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

static const char * const bcm2708_compat[] = {
	"brcm,bcm2708",
	NULL
};

MACHINE_START(BCM2708, "BCM2708")
    /* Maintainer: Broadcom Europe Ltd. */
	.map_io = bcm2708_map_io,
	.init_irq = bcm2708_init_irq,
	.init_time = bcm2708_timer_init,
	.init_machine = bcm2708_init,
	.init_early = bcm2708_init_early,
	.reserve = board_reserve,
	.restart	= bcm2708_restart,
	.dt_compat = bcm2708_compat,
MACHINE_END

module_param(boardrev, uint, 0644);
module_param(serial, uint, 0644);
module_param(uart_clock, uint, 0644);
module_param(disk_led_gpio, uint, 0644);
module_param(disk_led_active_low, uint, 0644);
module_param(reboot_part, uint, 0644);
module_param(w1_gpio_pin, uint, 0644);
module_param(w1_gpio_pullup, uint, 0644);
module_param(vc_i2c_override, bool, 0644);
MODULE_PARM_DESC(vc_i2c_override, "Allow the use of VC's I2C peripheral.");
module_param(pps_gpio_pin, int, 0644);
MODULE_PARM_DESC(pps_gpio_pin, "Set GPIO pin to reserve for PPS");
