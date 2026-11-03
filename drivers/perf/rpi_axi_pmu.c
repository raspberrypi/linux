// SPDX-License-Identifier: GPL-2.0-only

/**
 * DOC: Raspberry Pi AXI Bus Performance Monitoring Unit (PMU) Driver
 *
 * This driver exposes the performance monitoring hardware on Raspberry Pi
 * System-on-Chips to the Linux perf subsystem:
 * - Raspberry Pi 1, 2, 3, 4, Compute Modules 1-4, Zero, Zero W (SoCs BCM2835/2836/2837/2711).
 * - Raspberry Pi 5, Compute Module 5 (SoC BCM2712).
 *
 * Architecture Overview:
 * ----------------------
 * The Broadcom AXI performance hardware provides up to two independent monitors:
 * 1. System Monitor (MON_SYSTEM = 0):
 *    Monitors system-level AXI traffic (ARM CPU L2/UC, DMA, V3D, ISP, HVS, PCIe/RP1).
 *    Directly memory-mapped via ARM physical IO memory space (MMIO).
 *    Read latency: ~10-20 nanoseconds (fast, atomic-safe, non-blocking).
 *
 * 2. VPU Monitor (MON_VPU = 1):
 *    Monitors VideoCore VPU buses (VPU0/1 Data/Instruction L2/UC, SDRAM, etc.).
 *    Accessible through VideoCore firmware mailbox IPC (RPI_FIRMWARE_SET/GET_PERIPH_REG).
 *    Read latency: ~10-100 microseconds (IPC over VPU mailbox).
 *
 * Synchronization & Concurrency Model:
 * ------------------------------------
 * - Spinlock (pmu->lock):
 *   Protects active event array (events[]),
 *   bus watcher allocation/refcounting, active_vpu_events counter, and MMIO register updates
 *   (MON_SYSTEM) against SMP race conditions.
 *
 * - Mutex (pmu->vpu_mutex):
 *   Serializes VideoCore Mailbox IPC transactions (MON_VPU) in process context,
 *   preventing concurrent mailbox buffer corruption across multiple CPUs.
 *
 * - Cached Async VPU Reads & Multiplexing (MON_VPU):
 *   Polled periodically in process context by vpu_work when active_vpu_events > 0.
 *   Uses PERF_HES_UPTODATE state flag to safely establish counter baselines during
 *   event rotation / multiplexing. User read() syscalls return cached cumulative event counter
 *   instantly without blocking.
 */

#include <linux/bitfield.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include <soc/bcm2835/raspberrypi-firmware.h>

/* --- PLATFORM CONSTANTS & ENUMERATIONS --------------------------- */

/**
 * enum rpi_axi_chip - Supported Broadcom SoC generations
 * @CHIP_BCM2835: BCM2835 / BCM2836 / BCM2837 (RPi 1-3, CM 1-3, Zero/W)
 * @CHIP_BCM2711: BCM2711 (RPi 4, CM 4)
 * @CHIP_BCM2712: BCM2712 (RPi 5, CM 5)
 */
enum rpi_axi_chip {
	CHIP_BCM2835 = 0,
	CHIP_BCM2711,
	CHIP_BCM2712,
};

/**
 * enum monitor - Hardware performance monitor blocks
 * @MON_SYSTEM: System AXI monitor (MMIO)
 * @MON_VPU: VideoCore VPU AXI monitor (Mailbox IPC)
 * @MON_MAX: Total number of monitor blocks
 */
enum monitor {
	MON_SYSTEM = 0,
	MON_VPU,
	MON_MAX
};

/* Number of hardware bus watcher units per monitor */
#define NUM_BUS_WATCHERS_PER_MONITOR 3

/**
 * enum bcm2835_system_bus - AXI buses monitored by System Monitor on BCM2835-BCM2711 (RPi 1-4)
 * @BCM2835_SB_DMA_L2: DMA engine L2 cache interconnect bus
 * @BCM2835_SB_TRANS: Transposer engine bus (image format rotation & 2D matrix conversion)
 * @BCM2835_SB_JPEG: Hardware JPEG codec acceleration bus
 * @BCM2835_SB_SYSTEM_UC: System Uncached memory bus
 * @BCM2835_SB_DMA_UC: DMA Uncached memory bus
 * @BCM2835_SB_SYSTEM_L2: System main L2 cache bus
 * @BCM2835_SB_CCP2TX: Compact Camera Port 2 (CCP2) transmitter bus
 * @BCM2835_SB_MPHI_RX: Message Passing Host Interface (MPHI) receive bus
 * @BCM2835_SB_MPHI_TX: Message Passing Host Interface (MPHI) transmit bus
 * @BCM2835_SB_HVS: Hardware Video Scaler (HVS) multi-layer display composition engine bus
 * @BCM2835_SB_H264: H.264 / AVC hardware video encoder/decoder bus
 * @BCM2835_SB_ISP: Image Sensor Processor (ISP) camera processing pipeline bus
 * @BCM2835_SB_V3D: VideoCore V3D 3D graphics hardware pipeline bus
 * @BCM2835_SB_PERIPHERAL: System peripherals bus (UART, SPI, I2C, GPIO, PWM, PCM)
 * @BCM2835_SB_CPU_UC: ARM CPU Uncached memory bus
 * @BCM2835_SB_CPU_L2: ARM CPU L2 cache bus
 * @BCM2835_SB_MAX: Total count of monitored system buses on BCM2835-BCM2711
 */
enum bcm2835_system_bus {
	BCM2835_SB_DMA_L2 = 0,
	BCM2835_SB_TRANS = 1,
	BCM2835_SB_JPEG = 2,
	BCM2835_SB_SYSTEM_UC = 3,
	BCM2835_SB_DMA_UC = 4,
	BCM2835_SB_SYSTEM_L2 = 5,
	BCM2835_SB_CCP2TX = 6,
	BCM2835_SB_MPHI_RX = 7,
	BCM2835_SB_MPHI_TX = 8,
	BCM2835_SB_HVS = 9,
	BCM2835_SB_H264 = 10,
	BCM2835_SB_ISP = 11,
	BCM2835_SB_V3D = 12,
	BCM2835_SB_PERIPHERAL = 13,
	BCM2835_SB_CPU_UC = 14,
	BCM2835_SB_CPU_L2 = 15,
	BCM2835_SB_MAX,
};

/**
 * enum bcm2712_system_bus - AXI buses monitored by the system monitor on BCM2712 C0 (RPi 5)
 * @BCM2712_SB_VPU_UC: VPU uncached bus
 * @BCM2712_SB_DISPLAY: Display (HVS) bus
 * @BCM2712_SB_V3D: V3D GPU bus
 * @BCM2712_SB_ARM: Arm CPU cluster bus
 * @BCM2712_SB_XPT: XPT bus (unused on Raspberry Pi)
 * @BCM2712_SB_PCIE_02: PCIe2 (RP1 south bridge on Raspberry Pi 5; BSTM_TOP in Broadcom docs)
 * @BCM2712_SB_PCIE_01: External PCIe bus
 * @BCM2712_SB_ARGON: Argon (HEVC decoder and PiSP back end) bus
 * @BCM2712_SB_EMMC2: SD/eMMC2 controller bus (ARB3 in Broadcom documentation)
 * @BCM2712_SB_SD_DMA: SD/eMMC, DMA0 and DMA1 bus (SRC in Broadcom documentation)
 * @BCM2712_SB_HVDP: HVDP bus (unused on Raspberry Pi)
 * @BCM2712_SB_PER: Peripheral access bus
 * @BCM2712_SB_SYSTEM_L2: System L2 cache bus
 * @BCM2712_SB_MAX: Number of buses
 */
enum bcm2712_system_bus {
	BCM2712_SB_VPU_UC = 0,
	BCM2712_SB_DISPLAY = 1,
	BCM2712_SB_V3D = 2,
	BCM2712_SB_ARM = 3,
	BCM2712_SB_XPT = 4,
	BCM2712_SB_PCIE_02 = 5,
	BCM2712_SB_PCIE_01 = 6,
	BCM2712_SB_ARGON = 7,
	BCM2712_SB_EMMC2 = 8,
	BCM2712_SB_SD_DMA = 9,
	BCM2712_SB_HVDP = 10,
	BCM2712_SB_PER = 11,
	BCM2712_SB_SYSTEM_L2 = 12,
	BCM2712_SB_MAX,
};

/**
 * enum bcm2712d0_system_bus - AXI buses monitored by the system monitor on BCM2712 D0 and later
 * @BCM2712D0_SB_VPU_UC: VPU uncached bus
 * @BCM2712D0_SB_DISPLAY: Display (HVS) bus
 * @BCM2712D0_SB_V3D: V3D GPU bus
 * @BCM2712D0_SB_ARM: Arm CPU cluster bus
 * @BCM2712D0_SB_PCIE_02: PCIe2 (RP1 south bridge on Raspberry Pi 5) bus
 * @BCM2712D0_SB_ARGON: Argon (HEVC decoder, PiSP back end and external PCIe) bus
 * @BCM2712D0_SB_EMMC2: SD/eMMC2 controller bus
 * @BCM2712D0_SB_SD_DMA: SD/eMMC, DMA0 and DMA1 bus
 * @BCM2712D0_SB_PER: Peripheral access bus
 * @BCM2712D0_SB_SYSTEM_L2: System L2 cache bus
 * @BCM2712D0_SB_MAX: Number of buses
 */
enum bcm2712d0_system_bus {
	BCM2712D0_SB_VPU_UC = 0,
	BCM2712D0_SB_DISPLAY = 1,
	BCM2712D0_SB_V3D = 2,
	BCM2712D0_SB_ARM = 3,
	BCM2712D0_SB_PCIE_02 = 4,
	BCM2712D0_SB_ARGON = 5,
	BCM2712D0_SB_EMMC2 = 6,
	BCM2712D0_SB_SD_DMA = 7,
	BCM2712D0_SB_PER = 8,
	BCM2712D0_SB_SYSTEM_L2 = 9,
	BCM2712D0_SB_MAX,
};

/**
 * enum bcm2835_vpu_bus - AXI buses monitored by VPU Monitor on BCM2835 (RPi 1-3)
 * @BCM2835_VB_VPU1_D_L2: VideoCore VPU Core 1 Data L2 cache bus
 * @BCM2835_VB_VPU0_D_L2: VideoCore VPU Core 0 Data L2 cache bus
 * @BCM2835_VB_VPU1_I_L2: VideoCore VPU Core 1 Instruction L2 cache bus
 * @BCM2835_VB_VPU0_I_L2: VideoCore VPU Core 0 Instruction L2 cache bus
 * @BCM2835_VB_SYSTEM_L2: VPU System L2 cache interconnect bus
 * @BCM2835_VB_DMA_L2: VPU DMA L2 cache interconnect bus
 * @BCM2835_VB_VPU1_D_UC: VideoCore VPU Core 1 Data Uncached memory bus
 * @BCM2835_VB_VPU0_D_UC: VideoCore VPU Core 0 Data Uncached memory bus
 * @BCM2835_VB_VPU1_I_UC: VideoCore VPU Core 1 Instruction Uncached memory bus
 * @BCM2835_VB_VPU0_I_UC: VideoCore VPU Core 0 Instruction Uncached memory bus
 * @BCM2835_VB_VPU_UC: VPU Uncached memory bus
 * @BCM2835_VB_L2_OUT: VPU L2 cache outbound memory bus
 * @BCM2835_VB_DMA_UC: VPU DMA Uncached memory bus
 * @BCM2835_VB_L2_IN: VPU L2 cache inbound memory bus
 * @BCM2835_VB_SDRAM: VPU SDRAM memory controller bus
 * @BCM2835_VB_MAX: Total count of monitored VPU buses on BCM2835
 */
enum bcm2835_vpu_bus {
	BCM2835_VB_VPU1_D_L2 = 0,
	BCM2835_VB_VPU0_D_L2 = 1,
	BCM2835_VB_VPU1_I_L2 = 2,
	BCM2835_VB_VPU0_I_L2 = 3,
	BCM2835_VB_SYSTEM_L2 = 4,
	BCM2835_VB_DMA_L2 = 5,
	BCM2835_VB_VPU1_D_UC = 6,
	BCM2835_VB_VPU0_D_UC = 7,
	BCM2835_VB_VPU1_I_UC = 8,
	BCM2835_VB_VPU0_I_UC = 9,
	BCM2835_VB_VPU_UC = 10,
	BCM2835_VB_L2_OUT = 11,
	BCM2835_VB_DMA_UC = 12,
	BCM2835_VB_L2_IN = 13,
	BCM2835_VB_SDRAM = 14,
	BCM2835_VB_MAX,
};

/**
 * enum counter - 32-bit hardware performance counter metrics per bus watcher unit
 * @CNT_ATWAIT: Total address phase wait / stall cycles
 * @CNT_ATRANS: Total address phase transaction count
 * @CNT_AMAX: Maximum address phase latency
 * @CNT_WWAIT: Total write data phase wait / stall cycles
 * @CNT_WTRANS: Total write data phase transaction count
 * @CNT_WMAX: Maximum write data phase latency
 * @CNT_RWAIT: Total read data phase wait / stall cycles
 * @CNT_RTRANS: Total read data phase transaction count
 * @CNT_RMAX: Maximum read data phase latency
 * @CNT_RPEND: Total read pending cycles
 * @CNT_RATRANS: Total read address phase transaction count
 * @CNT_MAX: Total metric counters per watcher unit
 */
enum counter {
	CNT_ATWAIT = 0,
	CNT_ATRANS = 1,
	CNT_AMAX = 2,
	CNT_WWAIT = 3,
	CNT_WTRANS = 4,
	CNT_WMAX = 5,
	CNT_RWAIT = 6,
	CNT_RTRANS = 7,
	CNT_RMAX = 8,
	CNT_RPEND = 9,
	CNT_RATRANS = 10,
	CNT_MAX,
};

/**
 * enum bcm2835_filter - AXI master ID filter options for BCM2835-BCM2711 (RPi 1-4)
 * @BCM2835_FLT_0: Disable master ID filtering (monitor all traffic on bus)
 * @BCM2835_FLT_CORE0_V: VideoCore Core 0 master ID
 * @BCM2835_FLT_ICACHE0: CPU Core 0 Instruction Cache master ID
 * @BCM2835_FLT_DCACHE0: CPU Core 0 Data Cache master ID
 * @BCM2835_FLT_CORE1_V: VideoCore Core 1 master ID
 * @BCM2835_FLT_ICACHE1: CPU Core 1 Instruction Cache master ID
 * @BCM2835_FLT_DCACHE1: CPU Core 1 Data Cache master ID
 * @BCM2835_FLT_L2_MAIN: Main L2 cache controller master ID
 * @BCM2835_FLT_HOST_PORT: Host Interface Port 0 master ID
 * @BCM2835_FLT_HOST_PORT2: Host Interface Port 1 master ID
 * @BCM2835_FLT_HVS: Hardware Video Scaler (HVS) display engine master ID
 * @BCM2835_FLT_ISP: Image Sensor Processor (ISP) camera pipeline master ID
 * @BCM2835_FLT_VIDEO_DCT: Discrete Cosine Transform (DCT) hardware accelerator master ID
 * @BCM2835_FLT_VIDEO_SD2AXI: SD card to AXI bridge master ID
 * @BCM2835_FLT_CAM0: Camera Unicam 0 receiver master ID
 * @BCM2835_FLT_CAM1: Camera Unicam 1 receiver master ID
 * @BCM2835_FLT_DMA0: System DMA Channel 0 master ID
 * @BCM2835_FLT_DMA1: System DMA Channel 1 master ID
 * @BCM2835_FLT_DMA2_VPU: VPU DMA engine master ID
 * @BCM2835_FLT_JPEG: JPEG decoder hardware master ID
 * @BCM2835_FLT_VIDEO_CME: Motion Estimation hardware accelerator master ID
 * @BCM2835_FLT_TRANSPOSER: Image Transposer engine master ID
 * @BCM2835_FLT_VIDEO_FME: Fractional Motion Estimation hardware master ID
 * @BCM2835_FLT_CCP2TX: Compact Camera Port 2 transmitter master ID
 * @BCM2835_FLT_USB: USB 2.0 Host/OTG controller master ID
 * @BCM2835_FLT_V3D0: VideoCore V3D graphics pipe 0 master ID
 * @BCM2835_FLT_V3D1: VideoCore V3D graphics pipe 1 master ID
 * @BCM2835_FLT_V3D2: VideoCore V3D graphics pipe 2 master ID
 * @BCM2835_FLT_AVE: Audio-Video Engine master ID
 * @BCM2835_FLT_DEBUG: ARM JTAG/CoreSight debug unit master ID
 * @BCM2835_FLT_CPU: Generic ARM CPU cluster master ID
 * @BCM2835_FLT_M30: Reserved master ID slot 30
 * @BCM2835_FLT_MAX: Maximum filter ID count
 */
enum bcm2835_filter {
	BCM2835_FLT_0 = 0,
	BCM2835_FLT_CORE0_V = 1,
	BCM2835_FLT_ICACHE0 = 2,
	BCM2835_FLT_DCACHE0 = 3,
	BCM2835_FLT_CORE1_V = 4,
	BCM2835_FLT_ICACHE1 = 5,
	BCM2835_FLT_DCACHE1 = 6,
	BCM2835_FLT_L2_MAIN = 7,
	BCM2835_FLT_HOST_PORT = 8,
	BCM2835_FLT_HOST_PORT2 = 9,
	BCM2835_FLT_HVS = 10,
	BCM2835_FLT_ISP = 11,
	BCM2835_FLT_VIDEO_DCT = 12,
	BCM2835_FLT_VIDEO_SD2AXI = 13,
	BCM2835_FLT_CAM0 = 14,
	BCM2835_FLT_CAM1 = 15,
	BCM2835_FLT_DMA0 = 16,
	BCM2835_FLT_DMA1 = 17,
	BCM2835_FLT_DMA2_VPU = 18,
	BCM2835_FLT_JPEG = 19,
	BCM2835_FLT_VIDEO_CME = 20,
	BCM2835_FLT_TRANSPOSER = 21,
	BCM2835_FLT_VIDEO_FME = 22,
	BCM2835_FLT_CCP2TX = 23,
	BCM2835_FLT_USB = 24,
	BCM2835_FLT_V3D0 = 25,
	BCM2835_FLT_V3D1 = 26,
	BCM2835_FLT_V3D2 = 27,
	BCM2835_FLT_AVE = 28,
	BCM2835_FLT_DEBUG = 29,
	BCM2835_FLT_CPU = 30,
	BCM2835_FLT_M30 = 31,
	BCM2835_FLT_MAX,
};

/**
 * enum bcm2711_system_bus - AXI buses monitored by System Monitor on BCM2711 (RPi 4)
 * @BCM2711_SB_DMA_L2: DMA engine L2 cache interconnect bus
 * @BCM2711_SB_TRANS: Transposer engine bus
 * @BCM2711_SB_JPEG: Hardware JPEG codec acceleration bus
 * @BCM2711_SB_VPU_UC: VPU Uncached memory bus
 * @BCM2711_SB_DMA_UC: DMA Uncached memory bus
 * @BCM2711_SB_SYSTEM_L2: System main L2 cache bus
 * @BCM2711_SB_HVS: Hardware Video Scaler (HVS) display engine bus
 * @BCM2711_SB_ARGON: Argon video decoder bus
 * @BCM2711_SB_H264: H.264 hardware video codec bus
 * @BCM2711_SB_PERIPHERAL: System peripherals bus
 * @BCM2711_SB_ARM_UC: ARM CPU Uncached memory bus
 * @BCM2711_SB_ARM_L2: ARM CPU L2 cache bus
 * @BCM2711_SB_MAX: Total count of monitored system buses on BCM2711
 */
enum bcm2711_system_bus {
	BCM2711_SB_DMA_L2 = 0,
	BCM2711_SB_TRANS = 1,
	BCM2711_SB_JPEG = 2,
	BCM2711_SB_VPU_UC = 3,
	BCM2711_SB_DMA_UC = 4,
	BCM2711_SB_SYSTEM_L2 = 5,
	BCM2711_SB_HVS = 6,
	BCM2711_SB_ARGON = 7,
	BCM2711_SB_H264 = 8,
	BCM2711_SB_PERIPHERAL = 9,
	BCM2711_SB_ARM_UC = 10,
	BCM2711_SB_ARM_L2 = 11,
	BCM2711_SB_MAX,
};

/**
 * enum bcm2711_vpu_bus - AXI buses monitored by VPU Monitor on BCM2711 (RPi 4)
 * @BCM2711_VB_VPU1_D_L2: VideoCore VPU Core 1 Data L2 cache bus
 * @BCM2711_VB_VPU0_D_L2: VideoCore VPU Core 0 Data L2 cache bus
 * @BCM2711_VB_VPU1_I_L2: VideoCore VPU Core 1 Instruction L2 cache bus
 * @BCM2711_VB_VPU0_I_L2: VideoCore VPU Core 0 Instruction L2 cache bus
 * @BCM2711_VB_SYSTEM_L2: VPU System L2 cache interconnect bus
 * @BCM2711_VB_DMA_L2: VPU DMA L2 cache interconnect bus
 * @BCM2711_VB_VPU1_D_UC: VideoCore VPU Core 1 Data Uncached memory bus
 * @BCM2711_VB_VPU0_D_UC: VideoCore VPU Core 0 Data Uncached memory bus
 * @BCM2711_VB_VPU1_I_UC: VideoCore VPU Core 1 Instruction Uncached memory bus
 * @BCM2711_VB_VPU0_I_UC: VideoCore VPU Core 0 Instruction Uncached memory bus
 * @BCM2711_VB_VPU_UC: VPU Uncached memory bus
 * @BCM2711_VB_L2_OUT: VPU L2 cache outbound memory bus
 * @BCM2711_VB_DMA_UC: VPU DMA Uncached memory bus
 * @BCM2711_VB_L2_IN: VPU L2 cache inbound memory bus
 * @BCM2711_VB_MAX: Total count of monitored VPU buses on BCM2711
 */
enum bcm2711_vpu_bus {
	BCM2711_VB_VPU1_D_L2 = 0,
	BCM2711_VB_VPU0_D_L2 = 1,
	BCM2711_VB_VPU1_I_L2 = 2,
	BCM2711_VB_VPU0_I_L2 = 3,
	BCM2711_VB_SYSTEM_L2 = 4,
	BCM2711_VB_DMA_L2 = 5,
	BCM2711_VB_VPU1_D_UC = 6,
	BCM2711_VB_VPU0_D_UC = 7,
	BCM2711_VB_VPU1_I_UC = 8,
	BCM2711_VB_VPU0_I_UC = 9,
	BCM2711_VB_VPU_UC = 10,
	BCM2711_VB_L2_OUT = 11,
	BCM2711_VB_DMA_UC = 12,
	BCM2711_VB_L2_IN = 13,
	BCM2711_VB_MAX,
};

/**
 * enum bcm2711_filter - AXI master ID filter options for BCM2711 (RPi 4)
 * @BCM2711_FLT_AIO: Audio/Video I/O master ID (or 0 to disable filtering)
 * @BCM2711_FLT_CORE0_V: VideoCore Core 0 master ID
 * @BCM2711_FLT_ICACHE0: VideoCore Core 0 Instruction Cache master ID
 * @BCM2711_FLT_DCACHE0: VideoCore Core 0 Data Cache master ID
 * @BCM2711_FLT_CORE1_V: VideoCore Core 1 master ID
 * @BCM2711_FLT_ICACHE1: VideoCore Core 1 Instruction Cache master ID
 * @BCM2711_FLT_DCACHE1: VideoCore Core 1 Data Cache master ID
 * @BCM2711_FLT_L2_MAIN: Main L2 cache controller master ID
 * @BCM2711_FLT_ARGON: Argon video decoder master ID
 * @BCM2711_FLT_PCIE: PCIe controller master ID
 * @BCM2711_FLT_HVS: Hardware Video Scaler (HVS) display engine master ID
 * @BCM2711_FLT_ISP: Image Sensor Processor (ISP) camera pipeline master ID
 * @BCM2711_FLT_VIDEO_DCT: Discrete Cosine Transform (DCT) accelerator master ID
 * @BCM2711_FLT_VIDEO_SD2AXI: SD card to AXI bridge master ID
 * @BCM2711_FLT_CAM0: Camera Unicam 0 receiver master ID
 * @BCM2711_FLT_CAM1: Camera Unicam 1 receiver master ID
 * @BCM2711_FLT_DMA0: System DMA Channel 0 master ID
 * @BCM2711_FLT_DMA1: System DMA Channel 1 master ID
 * @BCM2711_FLT_DMA2: VPU DMA engine 2 master ID
 * @BCM2711_FLT_JPEG: JPEG decoder hardware master ID
 * @BCM2711_FLT_VIDEO_CME: Motion Estimation hardware accelerator master ID
 * @BCM2711_FLT_TRANSPOSER: Image Transposer engine master ID
 * @BCM2711_FLT_VIDEO_FME: Fractional Motion Estimation hardware master ID
 * @BCM2711_FLT_GIGE: Gigabit Ethernet controller master ID
 * @BCM2711_FLT_USB: USB controller master ID
 * @BCM2711_FLT_V3D0: VideoCore V3D graphics pipe 0 master ID
 * @BCM2711_FLT_V3D1: VideoCore V3D graphics pipe 1 master ID
 * @BCM2711_FLT_V3D2: VideoCore V3D graphics pipe 2 master ID
 * @BCM2711_FLT_GISB_AXI: GISB to AXI bridge master ID
 * @BCM2711_FLT_DEBUG: Debug unit master ID
 * @BCM2711_FLT_ARM: ARM CPU cluster master ID
 * @BCM2711_FLT_EMMCSTB: EMMC STB controller master ID
 * @BCM2711_FLT_MAX: Maximum filter ID count for BCM2711
 */
enum bcm2711_filter {
	BCM2711_FLT_AIO = 0,
	BCM2711_FLT_CORE0_V = 1,
	BCM2711_FLT_ICACHE0 = 2,
	BCM2711_FLT_DCACHE0 = 3,
	BCM2711_FLT_CORE1_V = 4,
	BCM2711_FLT_ICACHE1 = 5,
	BCM2711_FLT_DCACHE1 = 6,
	BCM2711_FLT_L2_MAIN = 7,
	BCM2711_FLT_ARGON = 8,
	BCM2711_FLT_PCIE = 9,
	BCM2711_FLT_HVS = 10,
	BCM2711_FLT_ISP = 11,
	BCM2711_FLT_VIDEO_DCT = 12,
	BCM2711_FLT_VIDEO_SD2AXI = 13,
	BCM2711_FLT_CAM0 = 14,
	BCM2711_FLT_CAM1 = 15,
	BCM2711_FLT_DMA0 = 16,
	BCM2711_FLT_DMA1 = 17,
	BCM2711_FLT_DMA2 = 18,
	BCM2711_FLT_JPEG = 19,
	BCM2711_FLT_VIDEO_CME = 20,
	BCM2711_FLT_TRANSPOSER = 21,
	BCM2711_FLT_VIDEO_FME = 22,
	BCM2711_FLT_GIGE = 23,
	BCM2711_FLT_USB = 24,
	BCM2711_FLT_V3D0 = 25,
	BCM2711_FLT_V3D1 = 26,
	BCM2711_FLT_V3D2 = 27,
	BCM2711_FLT_GISB_AXI = 28,
	BCM2711_FLT_DEBUG = 29,
	BCM2711_FLT_ARM = 30,
	BCM2711_FLT_EMMCSTB = 31,
	BCM2711_FLT_MAX,
};

/**
 * enum bcm2712_filter - AXI master ID filter options for BCM2712 (RPi 5)
 * @BCM2712_FLT_0: Disable master ID filtering (monitor all traffic on bus)
 * @BCM2712_FLT_VPU_UC0: VPU Uncached 0 master ID
 * @BCM2712_FLT_VPU_IC0: VPU I-Cache 0 master ID
 * @BCM2712_FLT_VPU_DC0: VPU D-Cache 0 master ID
 * @BCM2712_FLT_VPU_UC1: VPU Uncached 1 master ID
 * @BCM2712_FLT_VPU_IC1: VPU I-Cache 1 master ID
 * @BCM2712_FLT_VPU_DC1: VPU D-Cache 1 master ID
 * @BCM2712_FLT_VPU_L2: VPU L2 Cache master ID
 * @BCM2712_FLT_DMA2: DMA2 master ID
 * @BCM2712_FLT_VPU_DEBUG: VPU Debug master ID
 * @BCM2712_FLT_ARM: Arm Quad-Core CPU Cluster master ID
 * @BCM2712_FLT_DMA0: DMA0 master ID
 * @BCM2712_FLT_DMA1: DMA1 master ID
 * @BCM2712_FLT_RAAGA: RAAGA Audio Engine master ID
 * @BCM2712_FLT_BBSI: BBSI master ID
 * @BCM2712_FLT_PCIE0: PCIe 0 master ID
 * @BCM2712_FLT_PCIE1: PCIe 1 master ID
 * @BCM2712_FLT_PCIE2: PCIe 2 (RP1) master ID
 * @BCM2712_FLT_UMR: UMR master ID
 * @BCM2712_FLT_SAGE: SAGE master ID
 * @BCM2712_FLT_HVDP: HVDP master ID
 * @BCM2712_FLT_BSP: BSP master ID
 * @BCM2712_FLT_HVS: Hardware Video Scaler (HVS) display engine master ID
 * @BCM2712_FLT_HVS_WMK: HVS Watermark master ID
 * @BCM2712_FLT_MOP0: MOP0 master ID
 * @BCM2712_FLT_MOP1: MOP1 master ID
 * @BCM2712_FLT_MBVN: MBVN master ID
 * @BCM2712_FLT_DSI: DSI Display Interface master ID
 * @BCM2712_FLT_XPT: XPT master ID
 * @BCM2712_FLT_EMMC0: SD/eMMC Controller 0 master ID
 * @BCM2712_FLT_GENET: Gigabit Ethernet Controller master ID
 * @BCM2712_FLT_USB: USB Controller master ID
 * @BCM2712_FLT_ARGON: Argon master ID
 * @BCM2712_FLT_UNICAM: Unicam master ID
 * @BCM2712_FLT_PISP: ISP master ID
 * @BCM2712_FLT_PISPFE: ISP Front End master ID
 * @BCM2712_FLT_JPEG: JPEG decoder master ID
 * @BCM2712_FLT_EMMC1: SD/eMMC 1 master ID
 * @BCM2712_FLT_EMMC2: SD/eMMC 2 master ID
 * @BCM2712_FLT_TRC: TRC master ID
 * @BCM2712_FLT_BSTM0: BSTM0 master ID
 * @BCM2712_FLT_BSTM1: BSTM1 master ID
 * @BCM2712_FLT_BSTM0_SEC: BSTM0 Secure master ID
 * @BCM2712_FLT_BSTM1_SEC: BSTM1 Secure master ID
 * @BCM2712_FLT_AIO: AIO master ID
 * @BCM2712_FLT_MAP: MAP master ID
 * @BCM2712_FLT_SYS_DMA: System DMA master ID
 * @BCM2712_FLT_MMUCACHE0: MMU Cache 0 master ID
 * @BCM2712_FLT_MMUCACHE1: MMU Cache 1 master ID
 * @BCM2712_FLT_MPUCACHE0: MPU Cache 0 master ID
 * @BCM2712_FLT_MPUCACHE1: MPU Cache 1 master ID
 * @BCM2712_FLT_MAX: Maximum filter ID count for BCM2712
 */
enum bcm2712_filter {
	BCM2712_FLT_0 = 0,
	BCM2712_FLT_VPU_UC0 = 1,
	BCM2712_FLT_VPU_IC0 = 2,
	BCM2712_FLT_VPU_DC0 = 3,
	BCM2712_FLT_VPU_UC1 = 4,
	BCM2712_FLT_VPU_IC1 = 5,
	BCM2712_FLT_VPU_DC1 = 6,
	BCM2712_FLT_VPU_L2 = 8,
	BCM2712_FLT_DMA2 = 9,
	BCM2712_FLT_VPU_DEBUG = 10,
	BCM2712_FLT_ARM = 11,
	BCM2712_FLT_DMA0 = 12,
	BCM2712_FLT_DMA1 = 13,
	BCM2712_FLT_RAAGA = 14,
	BCM2712_FLT_BBSI = 16,
	BCM2712_FLT_PCIE0 = 18,
	BCM2712_FLT_PCIE1 = 19,
	BCM2712_FLT_PCIE2 = 20,
	BCM2712_FLT_UMR = 27,
	BCM2712_FLT_SAGE = 28,
	BCM2712_FLT_HVDP = 29,
	BCM2712_FLT_BSP = 30,
	BCM2712_FLT_HVS = 32,
	BCM2712_FLT_HVS_WMK = 33,
	BCM2712_FLT_MOP0 = 34,
	BCM2712_FLT_MOP1 = 35,
	BCM2712_FLT_MBVN = 36,
	BCM2712_FLT_DSI = 37,
	BCM2712_FLT_XPT = 38,
	BCM2712_FLT_EMMC0 = 39,
	BCM2712_FLT_GENET = 40,
	BCM2712_FLT_USB = 41,
	BCM2712_FLT_ARGON = 42,
	BCM2712_FLT_UNICAM = 43,
	BCM2712_FLT_PISP = 44,
	BCM2712_FLT_PISPFE = 45,
	BCM2712_FLT_JPEG = 46,
	BCM2712_FLT_EMMC1 = 47,
	BCM2712_FLT_EMMC2 = 48,
	BCM2712_FLT_TRC = 52,
	BCM2712_FLT_BSTM0 = 53,
	BCM2712_FLT_BSTM1 = 54,
	BCM2712_FLT_BSTM0_SEC = 55,
	BCM2712_FLT_BSTM1_SEC = 56,
	BCM2712_FLT_AIO = 57,
	BCM2712_FLT_MAP = 58,
	BCM2712_FLT_SYS_DMA = 59,
	BCM2712_FLT_MMUCACHE0 = 60,
	BCM2712_FLT_MMUCACHE1 = 61,
	BCM2712_FLT_MPUCACHE0 = 62,
	BCM2712_FLT_MPUCACHE1 = 63,
	BCM2712_FLT_MAX = 64,
};

/* Hardware register offsets & control bitwise constants */
#define GEN_CTRL			0x00
#define GEN_CTL_ENABLE_BIT		BIT(0)
#define GEN_CTL_RESET_BIT		BIT(1)
#define GEN_CTL_WATCH_BIT		BIT(2)

#define BW_STRIDE			0x40
#define BW0_CTRL			0x40
#define BW1_CTRL			0x80
#define BW2_CTRL			0xc0

/*
 * Event counter registers are contiguous and logically relative to their
 * respective Watcher Control register. These offsets are dynamically added
 * to the computed base (BWn_CTRL) to derive the physical address.
 */
#define BW_ATRANS_OFFSET		0x04
#define BW_ATWAIT_OFFSET		0x08
#define BW_AMAX_OFFSET			0x0c
#define BW_WTRANS_OFFSET		0x10
#define BW_WTWAIT_OFFSET		0x14
#define BW_WMAX_OFFSET			0x18
#define BW_RTRANS_OFFSET		0x1c
#define BW_RTWAIT_OFFSET		0x20
#define BW_RMAX_OFFSET			0x24
#define BW_RPEND_OFFSET			0x28
#define BW_RATRANS_OFFSET		0x2c

#define BW_CTRL_RESET_BIT		BIT(31)
#define BW_CTRL_ENABLE_BIT		BIT(30)
#define BW_CTRL_ENABLE_ID_FILTER_BIT	BIT(29)
#define BW_CTRL_LIMIT_HALT_BIT		BIT(28)

#define BW_CTRL_BUS_WATCH_SHIFT		0
#define BW_CTRL_BUS_WATCH_MASK		GENMASK(4, 0)
#define BW_CTRL_BUS_FILTER_SHIFT	8
#define BW_CTRL_BUS_FILTER_MASK		GENMASK(12, 8)
#define BW_CTRL_2712_FILTER_MASK	GENMASK(13, 8)
#define BW_CTRL_AXI_ID			GENMASK(17, 8)
#define BW_CTRL_AXI_ID_MASK		GENMASK(27, 18)
#define AXI_ID_MASTER			GENMASK(8, 3)
/* The D0 VPU monitor has a 6-bit ID holding the master directly, with its own mask */
#define BW_CTRL_VPU_ID			GENMASK(13, 8)
#define BW_CTRL_VPU_ID_MASK		GENMASK(23, 18)

/*
 * RPI_AXI_PMU_TIMER_INTERVAL determines the background polling frequency
 * for VideoCore VPU Mailbox IPC counters.
 *
 * A balance is required:
 * - IPC Overhead: Polling overly fast (e.g., 10ms) generates excessive CPU
 *   wakeups and VideoCore IPC interrupts on older CPUs (Pi 1/2).
 * - Accuracy: Polling overly slow (e.g., 2000ms) causes short time-multiplexed
 *   profiling sessions (under the interval) to mathematically strand residual
 *   counts since the mailbox cannot be queried synchronously inside pmu->read().
 *
 * 100ms (10 Hz) provides reasonably accurate profiling without heavy overhead.
 */
#define RPI_AXI_PMU_TIMER_INTERVAL ms_to_ktime(100)

static enum cpuhp_state rpi_axi_pmu_cpuhp_state;
/* --- PMU API & CONFIG DECODING ---------------------------------- */
#define PMU_NAME "rpi_axi_pmu"

/*
 * perf_event_attr config format:
 * [10-15] : Filter ID
 * [9]     : Monitor ID (0 = System, 1 = VPU)
 * [4-8]   : Bus index
 * [0-3]   : Counter enum
 */
#define RPI_AXI_CFG_FILTER_SHIFT	10
#define RPI_AXI_CFG_FILTER_MASK		0x3F
#define RPI_AXI_CFG_MONITOR_SHIFT	9
#define RPI_AXI_CFG_MONITOR_MASK	0x1
#define RPI_AXI_CFG_BUS_SHIFT		4
#define RPI_AXI_CFG_BUS_MASK		0x1F
#define RPI_AXI_CFG_COUNTER_MASK	0xF

/**
 * config_to_filter() - Extracts AXI filter ID from perf event config
 * @config: 64-bit config value from struct perf_event_attr
 *
 * Return: Filter ID value (bits 10-15).
 */
static int config_to_filter(__u64 config)
{
	return (config >> RPI_AXI_CFG_FILTER_SHIFT) & RPI_AXI_CFG_FILTER_MASK;
}

/**
 * config_to_monitor() - Extracts Monitor ID from perf event config
 * @config: 64-bit config value from struct perf_event_attr
 *
 * Return: Monitor enum (bit 9: 0 = System, 1 = VPU).
 */
static enum monitor config_to_monitor(__u64 config)
{
	return (config >> RPI_AXI_CFG_MONITOR_SHIFT) & RPI_AXI_CFG_MONITOR_MASK;
}

/**
 * config_to_bus() - Extracts bus index from perf event config
 * @config: 64-bit config value from struct perf_event_attr
 *
 * Return: Bus index (bits 4-8).
 */
static int config_to_bus(__u64 config)
{
	return (config >> RPI_AXI_CFG_BUS_SHIFT) & RPI_AXI_CFG_BUS_MASK;
}

/**
 * config_to_counter() - Extracts metric counter type from perf event config
 * @config: 64-bit config value from struct perf_event_attr
 *
 * Return: Counter enum (bits 0-3).
 */
static enum counter config_to_counter(__u64 config)
{
	return config & RPI_AXI_CFG_COUNTER_MASK;
}

struct rpi_axi_pmu;
/**
 * config_is_valid() - Validates whether event config bitfields match SoC capabilities
 * @pmu: Pointer to rpi_axi_pmu driver context
 * @config: 64-bit config value from struct perf_event_attr
 *
 * Return: true if valid for the detected chip, false otherwise.
 */
static bool config_is_valid(struct rpi_axi_pmu *pmu, __u64 config);

/**
 * struct rpi_axi_hw_events - Hardware resource tracking per monitor
 * @monitored_bus: Array tracking which bus index is assigned to each of the 3 watchers
 * @filter: Array tracking the filter applied to each watcher
 * @refcount: Reference count of active events sharing each watcher
 * @num_monitored: Total count of active watchers in use
 * @enabled: Hardware enabled status for each watcher
 * @monitor_running: Flag indicating if the hardware monitor loop is globally active
 * @vpu_disable_pending: Array tracking asynchronous hardware disable requests for VPU
 * @config_gen: Bumped whenever a watcher is allocated or freed
 */
struct rpi_axi_hw_events {
	int monitored_bus[NUM_BUS_WATCHERS_PER_MONITOR];
	int filter[NUM_BUS_WATCHERS_PER_MONITOR];
	int refcount[NUM_BUS_WATCHERS_PER_MONITOR];
	int num_monitored;
	bool monitor_running;
	bool enabled[NUM_BUS_WATCHERS_PER_MONITOR];
	bool vpu_disable_pending[NUM_BUS_WATCHERS_PER_MONITOR];
	unsigned int config_gen[NUM_BUS_WATCHERS_PER_MONITOR];
};

/**
 * rpi_axi_hw_events__init() - Resets hardware watcher tracking data
 * @hw_events: Pointer to rpi_axi_hw_events structure
 */
static void rpi_axi_hw_events__init(struct rpi_axi_hw_events *hw_events)
{
	hw_events->num_monitored = 0;
	hw_events->monitor_running = false;
	for (int i = 0; i < NUM_BUS_WATCHERS_PER_MONITOR; i++) {
		hw_events->monitored_bus[i] = -1;
		hw_events->filter[i] = 0;
		hw_events->refcount[i] = 0;
		hw_events->enabled[i] = false;
		hw_events->vpu_disable_pending[i] = false;
		hw_events->config_gen[i] = 0;
	}
}

/**
 * rpi_axi_hw_events__get_alloc_event_idx() - Allocates or reuses a bus watcher index
 * @hw_events: Pointer to hardware watcher tracking state
 * @event: Pointer to perf_event being initialized
 *
 * Return: Watcher index (0..2) on success, -1 if all 3 watchers are busy.
 */
static int rpi_axi_hw_events__get_alloc_event_idx(struct rpi_axi_hw_events *hw_events,
						  const struct perf_event *event)
{
	int bus = config_to_bus(event->attr.config);
	int filter = config_to_filter(event->attr.config);

	for (int i = 0; i < NUM_BUS_WATCHERS_PER_MONITOR; i++) {
		if (hw_events->monitored_bus[i] == bus && hw_events->filter[i] == filter) {
			hw_events->refcount[i]++;
			return i;
		}
	}
	if (hw_events->num_monitored == NUM_BUS_WATCHERS_PER_MONITOR)
		return -1;
	for (int i = 0; i < NUM_BUS_WATCHERS_PER_MONITOR; i++) {
		if (hw_events->monitored_bus[i] == -1) {
			hw_events->monitored_bus[i] = bus;
			hw_events->filter[i] = filter;
			hw_events->refcount[i] = 1;
			hw_events->vpu_disable_pending[i] = false;
			hw_events->config_gen[i]++;
			hw_events->num_monitored++;
			return i;
		}
	}
	return -1;
}

/* Maximum simultaneous active perf_events tracked by PMU */
#define RPI_AXI_MAX_EVENTS 66

/**
 * struct rpi_axi_pmu - Root PMU driver context
 * @pmu: Core Linux perf PMU structure
 * @pdev: Owning platform_device pointer
 * @chip: Detected Broadcom SoC generation (CHIP_BCM2835)
 * @axi_id_mask: Bus watchers match a full AXI ID under a mask (BCM2712 D0)
 * @firmware: Raspberry Pi firmware handle for VideoCore mailbox calls (BCM2835-BCM2711)
 * @cpu: CPU core assigned to process uncore PMU events
 * @cpuhp_node: Dynamic CPU hotplug instance node
 * @lock: Spinlock protecting events[] list, watcher refcounts, and MMIO counter updates
 * @vpu_mutex: Mutex serializing VideoCore Mailbox IPC transactions in process context
 * @hrtimer: High-resolution timer for periodic 32-bit counter overflow polling
 * @vpu_work: Deferred work structure for safe process-context VPU mailbox reads
 * @active_events: Count of active perf_events currently monitored
 * @active_vpu_events: Count of active VPU perf_events currently monitored
 * @events: Array of active perf_event pointers
 * @monitor: Per-monitor state array (System and VPU)
 */
struct rpi_axi_pmu {
	struct pmu		pmu;
	struct platform_device	*pdev;
	enum rpi_axi_chip	chip;
	bool			axi_id_mask;
	struct rpi_firmware	*firmware;
	int			cpu;
	struct hlist_node	cpuhp_node;
	raw_spinlock_t		lock;
	struct mutex		vpu_mutex;
	struct hrtimer		hrtimer;
	struct work_struct	vpu_work;
	int			active_events;
	int			active_vpu_events;
	struct perf_event	*events[RPI_AXI_MAX_EVENTS];
	struct {
		struct rpi_axi_hw_events hw_events;
		bool use_mailbox_interface;
		union {
			u32 mailbox;
			void __iomem *base_address;
		};
	}  monitor[MON_MAX];
};

#define pmu_to_rpi_axi_pmu(p) (container_of(p, struct rpi_axi_pmu, pmu))

static bool config_is_valid(struct rpi_axi_pmu *pmu, __u64 config)
{
	enum monitor mon = config_to_monitor(config);
	int bus = config_to_bus(config);
	int filter = config_to_filter(config);
	int counter = config_to_counter(config);

	if (config >> 16 != 0)
		return false;

	if (mon >= MON_MAX)
		return false;

	if (!pmu->monitor[mon].use_mailbox_interface && !pmu->monitor[mon].base_address)
		return false;

	if (filter >= 64)
		return false;

	if (counter == CNT_RATRANS &&
	    (pmu->chip == CHIP_BCM2835 || pmu->chip == CHIP_BCM2711))
		return false;

	switch (pmu->chip) {
	case CHIP_BCM2835:
		if (mon == MON_SYSTEM && bus >= BCM2835_SB_MAX)
			return false;
		if (mon == MON_VPU && bus >= BCM2835_VB_MAX)
			return false;
		if (filter >= BCM2835_FLT_MAX)
			return false;
		break;
	case CHIP_BCM2711:
		if (mon == MON_SYSTEM && bus >= BCM2711_SB_MAX)
			return false;
		if (mon == MON_VPU && bus >= BCM2711_VB_MAX)
			return false;
		if (filter >= BCM2711_FLT_MAX)
			return false;
		break;
	case CHIP_BCM2712:
		if (mon == MON_SYSTEM &&
		    bus >= (pmu->axi_id_mask ? BCM2712D0_SB_MAX : BCM2712_SB_MAX))
			return false;
		if (mon == MON_VPU && bus >= BCM2711_VB_MAX)
			return false;
		if (filter >= BCM2712_FLT_MAX)
			return false;
		break;
	}

	return counter < CNT_MAX;
}

PMU_EVENT_ATTR_STRING(dma_l2_atwait, bcm2835_dma_l2_atwait, "monitor=0,bus=0,counter=0");
PMU_EVENT_ATTR_STRING(dma_l2_atrans, bcm2835_dma_l2_atrans, "monitor=0,bus=0,counter=1");
PMU_EVENT_ATTR_STRING(dma_l2_amax, bcm2835_dma_l2_amax, "monitor=0,bus=0,counter=2");
PMU_EVENT_ATTR_STRING(dma_l2_wwait, bcm2835_dma_l2_wwait, "monitor=0,bus=0,counter=3");
PMU_EVENT_ATTR_STRING(dma_l2_wtrans, bcm2835_dma_l2_wtrans, "monitor=0,bus=0,counter=4");
PMU_EVENT_ATTR_STRING(dma_l2_wmax, bcm2835_dma_l2_wmax, "monitor=0,bus=0,counter=5");
PMU_EVENT_ATTR_STRING(dma_l2_rwait, bcm2835_dma_l2_rwait, "monitor=0,bus=0,counter=6");
PMU_EVENT_ATTR_STRING(dma_l2_rtrans, bcm2835_dma_l2_rtrans, "monitor=0,bus=0,counter=7");
PMU_EVENT_ATTR_STRING(dma_l2_rmax, bcm2835_dma_l2_rmax, "monitor=0,bus=0,counter=8");
PMU_EVENT_ATTR_STRING(dma_l2_rpend, bcm2835_dma_l2_rpend, "monitor=0,bus=0,counter=9");
PMU_EVENT_ATTR_STRING(trans_atwait, bcm2835_trans_atwait, "monitor=0,bus=1,counter=0");
PMU_EVENT_ATTR_STRING(trans_atrans, bcm2835_trans_atrans, "monitor=0,bus=1,counter=1");
PMU_EVENT_ATTR_STRING(trans_amax, bcm2835_trans_amax, "monitor=0,bus=1,counter=2");
PMU_EVENT_ATTR_STRING(trans_wwait, bcm2835_trans_wwait, "monitor=0,bus=1,counter=3");
PMU_EVENT_ATTR_STRING(trans_wtrans, bcm2835_trans_wtrans, "monitor=0,bus=1,counter=4");
PMU_EVENT_ATTR_STRING(trans_wmax, bcm2835_trans_wmax, "monitor=0,bus=1,counter=5");
PMU_EVENT_ATTR_STRING(trans_rwait, bcm2835_trans_rwait, "monitor=0,bus=1,counter=6");
PMU_EVENT_ATTR_STRING(trans_rtrans, bcm2835_trans_rtrans, "monitor=0,bus=1,counter=7");
PMU_EVENT_ATTR_STRING(trans_rmax, bcm2835_trans_rmax, "monitor=0,bus=1,counter=8");
PMU_EVENT_ATTR_STRING(trans_rpend, bcm2835_trans_rpend, "monitor=0,bus=1,counter=9");
PMU_EVENT_ATTR_STRING(jpeg_atwait, bcm2835_jpeg_atwait, "monitor=0,bus=2,counter=0");
PMU_EVENT_ATTR_STRING(jpeg_atrans, bcm2835_jpeg_atrans, "monitor=0,bus=2,counter=1");
PMU_EVENT_ATTR_STRING(jpeg_amax, bcm2835_jpeg_amax, "monitor=0,bus=2,counter=2");
PMU_EVENT_ATTR_STRING(jpeg_wwait, bcm2835_jpeg_wwait, "monitor=0,bus=2,counter=3");
PMU_EVENT_ATTR_STRING(jpeg_wtrans, bcm2835_jpeg_wtrans, "monitor=0,bus=2,counter=4");
PMU_EVENT_ATTR_STRING(jpeg_wmax, bcm2835_jpeg_wmax, "monitor=0,bus=2,counter=5");
PMU_EVENT_ATTR_STRING(jpeg_rwait, bcm2835_jpeg_rwait, "monitor=0,bus=2,counter=6");
PMU_EVENT_ATTR_STRING(jpeg_rtrans, bcm2835_jpeg_rtrans, "monitor=0,bus=2,counter=7");
PMU_EVENT_ATTR_STRING(jpeg_rmax, bcm2835_jpeg_rmax, "monitor=0,bus=2,counter=8");
PMU_EVENT_ATTR_STRING(jpeg_rpend, bcm2835_jpeg_rpend, "monitor=0,bus=2,counter=9");
PMU_EVENT_ATTR_STRING(system_uc_atwait, bcm2835_system_uc_atwait, "monitor=0,bus=3,counter=0");
PMU_EVENT_ATTR_STRING(system_uc_atrans, bcm2835_system_uc_atrans, "monitor=0,bus=3,counter=1");
PMU_EVENT_ATTR_STRING(system_uc_amax, bcm2835_system_uc_amax, "monitor=0,bus=3,counter=2");
PMU_EVENT_ATTR_STRING(system_uc_wwait, bcm2835_system_uc_wwait, "monitor=0,bus=3,counter=3");
PMU_EVENT_ATTR_STRING(system_uc_wtrans, bcm2835_system_uc_wtrans, "monitor=0,bus=3,counter=4");
PMU_EVENT_ATTR_STRING(system_uc_wmax, bcm2835_system_uc_wmax, "monitor=0,bus=3,counter=5");
PMU_EVENT_ATTR_STRING(system_uc_rwait, bcm2835_system_uc_rwait, "monitor=0,bus=3,counter=6");
PMU_EVENT_ATTR_STRING(system_uc_rtrans, bcm2835_system_uc_rtrans, "monitor=0,bus=3,counter=7");
PMU_EVENT_ATTR_STRING(system_uc_rmax, bcm2835_system_uc_rmax, "monitor=0,bus=3,counter=8");
PMU_EVENT_ATTR_STRING(system_uc_rpend, bcm2835_system_uc_rpend, "monitor=0,bus=3,counter=9");
PMU_EVENT_ATTR_STRING(dma_uc_atwait, bcm2835_dma_uc_atwait, "monitor=0,bus=4,counter=0");
PMU_EVENT_ATTR_STRING(dma_uc_atrans, bcm2835_dma_uc_atrans, "monitor=0,bus=4,counter=1");
PMU_EVENT_ATTR_STRING(dma_uc_amax, bcm2835_dma_uc_amax, "monitor=0,bus=4,counter=2");
PMU_EVENT_ATTR_STRING(dma_uc_wwait, bcm2835_dma_uc_wwait, "monitor=0,bus=4,counter=3");
PMU_EVENT_ATTR_STRING(dma_uc_wtrans, bcm2835_dma_uc_wtrans, "monitor=0,bus=4,counter=4");
PMU_EVENT_ATTR_STRING(dma_uc_wmax, bcm2835_dma_uc_wmax, "monitor=0,bus=4,counter=5");
PMU_EVENT_ATTR_STRING(dma_uc_rwait, bcm2835_dma_uc_rwait, "monitor=0,bus=4,counter=6");
PMU_EVENT_ATTR_STRING(dma_uc_rtrans, bcm2835_dma_uc_rtrans, "monitor=0,bus=4,counter=7");
PMU_EVENT_ATTR_STRING(dma_uc_rmax, bcm2835_dma_uc_rmax, "monitor=0,bus=4,counter=8");
PMU_EVENT_ATTR_STRING(dma_uc_rpend, bcm2835_dma_uc_rpend, "monitor=0,bus=4,counter=9");
PMU_EVENT_ATTR_STRING(system_l2_atwait, bcm2835_system_l2_atwait, "monitor=0,bus=5,counter=0");
PMU_EVENT_ATTR_STRING(system_l2_atrans, bcm2835_system_l2_atrans, "monitor=0,bus=5,counter=1");
PMU_EVENT_ATTR_STRING(system_l2_amax, bcm2835_system_l2_amax, "monitor=0,bus=5,counter=2");
PMU_EVENT_ATTR_STRING(system_l2_wwait, bcm2835_system_l2_wwait, "monitor=0,bus=5,counter=3");
PMU_EVENT_ATTR_STRING(system_l2_wtrans, bcm2835_system_l2_wtrans, "monitor=0,bus=5,counter=4");
PMU_EVENT_ATTR_STRING(system_l2_wmax, bcm2835_system_l2_wmax, "monitor=0,bus=5,counter=5");
PMU_EVENT_ATTR_STRING(system_l2_rwait, bcm2835_system_l2_rwait, "monitor=0,bus=5,counter=6");
PMU_EVENT_ATTR_STRING(system_l2_rtrans, bcm2835_system_l2_rtrans, "monitor=0,bus=5,counter=7");
PMU_EVENT_ATTR_STRING(system_l2_rmax, bcm2835_system_l2_rmax, "monitor=0,bus=5,counter=8");
PMU_EVENT_ATTR_STRING(system_l2_rpend, bcm2835_system_l2_rpend, "monitor=0,bus=5,counter=9");
PMU_EVENT_ATTR_STRING(ccp2tx_atwait, bcm2835_ccp2tx_atwait, "monitor=0,bus=6,counter=0");
PMU_EVENT_ATTR_STRING(ccp2tx_atrans, bcm2835_ccp2tx_atrans, "monitor=0,bus=6,counter=1");
PMU_EVENT_ATTR_STRING(ccp2tx_amax, bcm2835_ccp2tx_amax, "monitor=0,bus=6,counter=2");
PMU_EVENT_ATTR_STRING(ccp2tx_wwait, bcm2835_ccp2tx_wwait, "monitor=0,bus=6,counter=3");
PMU_EVENT_ATTR_STRING(ccp2tx_wtrans, bcm2835_ccp2tx_wtrans, "monitor=0,bus=6,counter=4");
PMU_EVENT_ATTR_STRING(ccp2tx_wmax, bcm2835_ccp2tx_wmax, "monitor=0,bus=6,counter=5");
PMU_EVENT_ATTR_STRING(ccp2tx_rwait, bcm2835_ccp2tx_rwait, "monitor=0,bus=6,counter=6");
PMU_EVENT_ATTR_STRING(ccp2tx_rtrans, bcm2835_ccp2tx_rtrans, "monitor=0,bus=6,counter=7");
PMU_EVENT_ATTR_STRING(ccp2tx_rmax, bcm2835_ccp2tx_rmax, "monitor=0,bus=6,counter=8");
PMU_EVENT_ATTR_STRING(ccp2tx_rpend, bcm2835_ccp2tx_rpend, "monitor=0,bus=6,counter=9");
PMU_EVENT_ATTR_STRING(mphi_rx_atwait, bcm2835_mphi_rx_atwait, "monitor=0,bus=7,counter=0");
PMU_EVENT_ATTR_STRING(mphi_rx_atrans, bcm2835_mphi_rx_atrans, "monitor=0,bus=7,counter=1");
PMU_EVENT_ATTR_STRING(mphi_rx_amax, bcm2835_mphi_rx_amax, "monitor=0,bus=7,counter=2");
PMU_EVENT_ATTR_STRING(mphi_rx_wwait, bcm2835_mphi_rx_wwait, "monitor=0,bus=7,counter=3");
PMU_EVENT_ATTR_STRING(mphi_rx_wtrans, bcm2835_mphi_rx_wtrans, "monitor=0,bus=7,counter=4");
PMU_EVENT_ATTR_STRING(mphi_rx_wmax, bcm2835_mphi_rx_wmax, "monitor=0,bus=7,counter=5");
PMU_EVENT_ATTR_STRING(mphi_rx_rwait, bcm2835_mphi_rx_rwait, "monitor=0,bus=7,counter=6");
PMU_EVENT_ATTR_STRING(mphi_rx_rtrans, bcm2835_mphi_rx_rtrans, "monitor=0,bus=7,counter=7");
PMU_EVENT_ATTR_STRING(mphi_rx_rmax, bcm2835_mphi_rx_rmax, "monitor=0,bus=7,counter=8");
PMU_EVENT_ATTR_STRING(mphi_rx_rpend, bcm2835_mphi_rx_rpend, "monitor=0,bus=7,counter=9");
PMU_EVENT_ATTR_STRING(mphi_tx_atwait, bcm2835_mphi_tx_atwait, "monitor=0,bus=8,counter=0");
PMU_EVENT_ATTR_STRING(mphi_tx_atrans, bcm2835_mphi_tx_atrans, "monitor=0,bus=8,counter=1");
PMU_EVENT_ATTR_STRING(mphi_tx_amax, bcm2835_mphi_tx_amax, "monitor=0,bus=8,counter=2");
PMU_EVENT_ATTR_STRING(mphi_tx_wwait, bcm2835_mphi_tx_wwait, "monitor=0,bus=8,counter=3");
PMU_EVENT_ATTR_STRING(mphi_tx_wtrans, bcm2835_mphi_tx_wtrans, "monitor=0,bus=8,counter=4");
PMU_EVENT_ATTR_STRING(mphi_tx_wmax, bcm2835_mphi_tx_wmax, "monitor=0,bus=8,counter=5");
PMU_EVENT_ATTR_STRING(mphi_tx_rwait, bcm2835_mphi_tx_rwait, "monitor=0,bus=8,counter=6");
PMU_EVENT_ATTR_STRING(mphi_tx_rtrans, bcm2835_mphi_tx_rtrans, "monitor=0,bus=8,counter=7");
PMU_EVENT_ATTR_STRING(mphi_tx_rmax, bcm2835_mphi_tx_rmax, "monitor=0,bus=8,counter=8");
PMU_EVENT_ATTR_STRING(mphi_tx_rpend, bcm2835_mphi_tx_rpend, "monitor=0,bus=8,counter=9");
PMU_EVENT_ATTR_STRING(hvs_atwait, bcm2835_hvs_atwait, "monitor=0,bus=9,counter=0");
PMU_EVENT_ATTR_STRING(hvs_atrans, bcm2835_hvs_atrans, "monitor=0,bus=9,counter=1");
PMU_EVENT_ATTR_STRING(hvs_amax, bcm2835_hvs_amax, "monitor=0,bus=9,counter=2");
PMU_EVENT_ATTR_STRING(hvs_wwait, bcm2835_hvs_wwait, "monitor=0,bus=9,counter=3");
PMU_EVENT_ATTR_STRING(hvs_wtrans, bcm2835_hvs_wtrans, "monitor=0,bus=9,counter=4");
PMU_EVENT_ATTR_STRING(hvs_wmax, bcm2835_hvs_wmax, "monitor=0,bus=9,counter=5");
PMU_EVENT_ATTR_STRING(hvs_rwait, bcm2835_hvs_rwait, "monitor=0,bus=9,counter=6");
PMU_EVENT_ATTR_STRING(hvs_rtrans, bcm2835_hvs_rtrans, "monitor=0,bus=9,counter=7");
PMU_EVENT_ATTR_STRING(hvs_rmax, bcm2835_hvs_rmax, "monitor=0,bus=9,counter=8");
PMU_EVENT_ATTR_STRING(hvs_rpend, bcm2835_hvs_rpend, "monitor=0,bus=9,counter=9");
PMU_EVENT_ATTR_STRING(h264_atwait, bcm2835_h264_atwait, "monitor=0,bus=10,counter=0");
PMU_EVENT_ATTR_STRING(h264_atrans, bcm2835_h264_atrans, "monitor=0,bus=10,counter=1");
PMU_EVENT_ATTR_STRING(h264_amax, bcm2835_h264_amax, "monitor=0,bus=10,counter=2");
PMU_EVENT_ATTR_STRING(h264_wwait, bcm2835_h264_wwait, "monitor=0,bus=10,counter=3");
PMU_EVENT_ATTR_STRING(h264_wtrans, bcm2835_h264_wtrans, "monitor=0,bus=10,counter=4");
PMU_EVENT_ATTR_STRING(h264_wmax, bcm2835_h264_wmax, "monitor=0,bus=10,counter=5");
PMU_EVENT_ATTR_STRING(h264_rwait, bcm2835_h264_rwait, "monitor=0,bus=10,counter=6");
PMU_EVENT_ATTR_STRING(h264_rtrans, bcm2835_h264_rtrans, "monitor=0,bus=10,counter=7");
PMU_EVENT_ATTR_STRING(h264_rmax, bcm2835_h264_rmax, "monitor=0,bus=10,counter=8");
PMU_EVENT_ATTR_STRING(h264_rpend, bcm2835_h264_rpend, "monitor=0,bus=10,counter=9");
PMU_EVENT_ATTR_STRING(isp_atwait, bcm2835_isp_atwait, "monitor=0,bus=11,counter=0");
PMU_EVENT_ATTR_STRING(isp_atrans, bcm2835_isp_atrans, "monitor=0,bus=11,counter=1");
PMU_EVENT_ATTR_STRING(isp_amax, bcm2835_isp_amax, "monitor=0,bus=11,counter=2");
PMU_EVENT_ATTR_STRING(isp_wwait, bcm2835_isp_wwait, "monitor=0,bus=11,counter=3");
PMU_EVENT_ATTR_STRING(isp_wtrans, bcm2835_isp_wtrans, "monitor=0,bus=11,counter=4");
PMU_EVENT_ATTR_STRING(isp_wmax, bcm2835_isp_wmax, "monitor=0,bus=11,counter=5");
PMU_EVENT_ATTR_STRING(isp_rwait, bcm2835_isp_rwait, "monitor=0,bus=11,counter=6");
PMU_EVENT_ATTR_STRING(isp_rtrans, bcm2835_isp_rtrans, "monitor=0,bus=11,counter=7");
PMU_EVENT_ATTR_STRING(isp_rmax, bcm2835_isp_rmax, "monitor=0,bus=11,counter=8");
PMU_EVENT_ATTR_STRING(isp_rpend, bcm2835_isp_rpend, "monitor=0,bus=11,counter=9");
PMU_EVENT_ATTR_STRING(v3d_atwait, bcm2835_v3d_atwait, "monitor=0,bus=12,counter=0");
PMU_EVENT_ATTR_STRING(v3d_atrans, bcm2835_v3d_atrans, "monitor=0,bus=12,counter=1");
PMU_EVENT_ATTR_STRING(v3d_amax, bcm2835_v3d_amax, "monitor=0,bus=12,counter=2");
PMU_EVENT_ATTR_STRING(v3d_wwait, bcm2835_v3d_wwait, "monitor=0,bus=12,counter=3");
PMU_EVENT_ATTR_STRING(v3d_wtrans, bcm2835_v3d_wtrans, "monitor=0,bus=12,counter=4");
PMU_EVENT_ATTR_STRING(v3d_wmax, bcm2835_v3d_wmax, "monitor=0,bus=12,counter=5");
PMU_EVENT_ATTR_STRING(v3d_rwait, bcm2835_v3d_rwait, "monitor=0,bus=12,counter=6");
PMU_EVENT_ATTR_STRING(v3d_rtrans, bcm2835_v3d_rtrans, "monitor=0,bus=12,counter=7");
PMU_EVENT_ATTR_STRING(v3d_rmax, bcm2835_v3d_rmax, "monitor=0,bus=12,counter=8");
PMU_EVENT_ATTR_STRING(v3d_rpend, bcm2835_v3d_rpend, "monitor=0,bus=12,counter=9");
PMU_EVENT_ATTR_STRING(peripheral_atwait, bcm2835_peripheral_atwait, "monitor=0,bus=13,counter=0");
PMU_EVENT_ATTR_STRING(peripheral_atrans, bcm2835_peripheral_atrans, "monitor=0,bus=13,counter=1");
PMU_EVENT_ATTR_STRING(peripheral_amax, bcm2835_peripheral_amax, "monitor=0,bus=13,counter=2");
PMU_EVENT_ATTR_STRING(peripheral_wwait, bcm2835_peripheral_wwait, "monitor=0,bus=13,counter=3");
PMU_EVENT_ATTR_STRING(peripheral_wtrans, bcm2835_peripheral_wtrans, "monitor=0,bus=13,counter=4");
PMU_EVENT_ATTR_STRING(peripheral_wmax, bcm2835_peripheral_wmax, "monitor=0,bus=13,counter=5");
PMU_EVENT_ATTR_STRING(peripheral_rwait, bcm2835_peripheral_rwait, "monitor=0,bus=13,counter=6");
PMU_EVENT_ATTR_STRING(peripheral_rtrans, bcm2835_peripheral_rtrans, "monitor=0,bus=13,counter=7");
PMU_EVENT_ATTR_STRING(peripheral_rmax, bcm2835_peripheral_rmax, "monitor=0,bus=13,counter=8");
PMU_EVENT_ATTR_STRING(peripheral_rpend, bcm2835_peripheral_rpend, "monitor=0,bus=13,counter=9");
PMU_EVENT_ATTR_STRING(cpu_uc_atwait, bcm2835_cpu_uc_atwait, "monitor=0,bus=14,counter=0");
PMU_EVENT_ATTR_STRING(cpu_uc_atrans, bcm2835_cpu_uc_atrans, "monitor=0,bus=14,counter=1");
PMU_EVENT_ATTR_STRING(cpu_uc_amax, bcm2835_cpu_uc_amax, "monitor=0,bus=14,counter=2");
PMU_EVENT_ATTR_STRING(cpu_uc_wwait, bcm2835_cpu_uc_wwait, "monitor=0,bus=14,counter=3");
PMU_EVENT_ATTR_STRING(cpu_uc_wtrans, bcm2835_cpu_uc_wtrans, "monitor=0,bus=14,counter=4");
PMU_EVENT_ATTR_STRING(cpu_uc_wmax, bcm2835_cpu_uc_wmax, "monitor=0,bus=14,counter=5");
PMU_EVENT_ATTR_STRING(cpu_uc_rwait, bcm2835_cpu_uc_rwait, "monitor=0,bus=14,counter=6");
PMU_EVENT_ATTR_STRING(cpu_uc_rtrans, bcm2835_cpu_uc_rtrans, "monitor=0,bus=14,counter=7");
PMU_EVENT_ATTR_STRING(cpu_uc_rmax, bcm2835_cpu_uc_rmax, "monitor=0,bus=14,counter=8");
PMU_EVENT_ATTR_STRING(cpu_uc_rpend, bcm2835_cpu_uc_rpend, "monitor=0,bus=14,counter=9");
PMU_EVENT_ATTR_STRING(cpu_l2_atwait, bcm2835_cpu_l2_atwait, "monitor=0,bus=15,counter=0");
PMU_EVENT_ATTR_STRING(cpu_l2_atrans, bcm2835_cpu_l2_atrans, "monitor=0,bus=15,counter=1");
PMU_EVENT_ATTR_STRING(cpu_l2_amax, bcm2835_cpu_l2_amax, "monitor=0,bus=15,counter=2");
PMU_EVENT_ATTR_STRING(cpu_l2_wwait, bcm2835_cpu_l2_wwait, "monitor=0,bus=15,counter=3");
PMU_EVENT_ATTR_STRING(cpu_l2_wtrans, bcm2835_cpu_l2_wtrans, "monitor=0,bus=15,counter=4");
PMU_EVENT_ATTR_STRING(cpu_l2_wmax, bcm2835_cpu_l2_wmax, "monitor=0,bus=15,counter=5");
PMU_EVENT_ATTR_STRING(cpu_l2_rwait, bcm2835_cpu_l2_rwait, "monitor=0,bus=15,counter=6");
PMU_EVENT_ATTR_STRING(cpu_l2_rtrans, bcm2835_cpu_l2_rtrans, "monitor=0,bus=15,counter=7");
PMU_EVENT_ATTR_STRING(cpu_l2_rmax, bcm2835_cpu_l2_rmax, "monitor=0,bus=15,counter=8");
PMU_EVENT_ATTR_STRING(cpu_l2_rpend, bcm2835_cpu_l2_rpend, "monitor=0,bus=15,counter=9");

PMU_FORMAT_ATTR(monitor, "config:9-9");
PMU_FORMAT_ATTR(bus, "config:4-8");
PMU_FORMAT_ATTR(counter, "config:0-3");
PMU_FORMAT_ATTR(filter, "config:10-15");

static struct attribute *rpi_axi_pmu_format_attrs[] = {
	&format_attr_monitor.attr,
	&format_attr_bus.attr,
	&format_attr_counter.attr,
	&format_attr_filter.attr,
	NULL
};

static const struct attribute_group rpi_axi_pmu_format_group = {
	.name = "format",
	.attrs = rpi_axi_pmu_format_attrs,
};

static ssize_t cpumask_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct rpi_axi_pmu *rpi_pmu = pmu_to_rpi_axi_pmu(pmu);

	return sysfs_emit(buf, "%*pbl\n", cpumask_pr_args(cpumask_of(rpi_pmu->cpu)));
}
static DEVICE_ATTR_RO(cpumask);
static struct attribute *rpi_axi_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group rpi_axi_pmu_cpumask_group = {
	.attrs = rpi_axi_pmu_cpumask_attrs,
};

static umode_t rpi_axi_pmu_events_is_visible(struct kobject *kobj,
					     struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pmu *pmu = dev_get_drvdata(dev);
	struct rpi_axi_pmu *rpi_pmu = pmu_to_rpi_axi_pmu(pmu);
	struct perf_pmu_events_attr *pmu_attr;

	pmu_attr = container_of(attr, struct perf_pmu_events_attr, attr.attr);
	if (!rpi_pmu->monitor[MON_VPU].use_mailbox_interface &&
	    pmu_attr->event_str &&
	    !strncmp(pmu_attr->event_str, "monitor=1,", 10))
		return 0;

	return attr->mode;
}

PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_atwait, bcm2835_vpu_vpu1_d_l2_atwait, "monitor=1,bus=0,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_atrans, bcm2835_vpu_vpu1_d_l2_atrans, "monitor=1,bus=0,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_amax, bcm2835_vpu_vpu1_d_l2_amax, "monitor=1,bus=0,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_wwait, bcm2835_vpu_vpu1_d_l2_wwait, "monitor=1,bus=0,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_wtrans, bcm2835_vpu_vpu1_d_l2_wtrans, "monitor=1,bus=0,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_wmax, bcm2835_vpu_vpu1_d_l2_wmax, "monitor=1,bus=0,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rwait, bcm2835_vpu_vpu1_d_l2_rwait, "monitor=1,bus=0,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rtrans, bcm2835_vpu_vpu1_d_l2_rtrans, "monitor=1,bus=0,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rmax, bcm2835_vpu_vpu1_d_l2_rmax, "monitor=1,bus=0,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rpend, bcm2835_vpu_vpu1_d_l2_rpend, "monitor=1,bus=0,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_atwait, bcm2835_vpu_vpu0_d_l2_atwait, "monitor=1,bus=1,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_atrans, bcm2835_vpu_vpu0_d_l2_atrans, "monitor=1,bus=1,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_amax, bcm2835_vpu_vpu0_d_l2_amax, "monitor=1,bus=1,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_wwait, bcm2835_vpu_vpu0_d_l2_wwait, "monitor=1,bus=1,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_wtrans, bcm2835_vpu_vpu0_d_l2_wtrans, "monitor=1,bus=1,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_wmax, bcm2835_vpu_vpu0_d_l2_wmax, "monitor=1,bus=1,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rwait, bcm2835_vpu_vpu0_d_l2_rwait, "monitor=1,bus=1,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rtrans, bcm2835_vpu_vpu0_d_l2_rtrans, "monitor=1,bus=1,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rmax, bcm2835_vpu_vpu0_d_l2_rmax, "monitor=1,bus=1,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rpend, bcm2835_vpu_vpu0_d_l2_rpend, "monitor=1,bus=1,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_atwait, bcm2835_vpu_vpu1_i_l2_atwait, "monitor=1,bus=2,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_atrans, bcm2835_vpu_vpu1_i_l2_atrans, "monitor=1,bus=2,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_amax, bcm2835_vpu_vpu1_i_l2_amax, "monitor=1,bus=2,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_wwait, bcm2835_vpu_vpu1_i_l2_wwait, "monitor=1,bus=2,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_wtrans, bcm2835_vpu_vpu1_i_l2_wtrans, "monitor=1,bus=2,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_wmax, bcm2835_vpu_vpu1_i_l2_wmax, "monitor=1,bus=2,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rwait, bcm2835_vpu_vpu1_i_l2_rwait, "monitor=1,bus=2,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rtrans, bcm2835_vpu_vpu1_i_l2_rtrans, "monitor=1,bus=2,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rmax, bcm2835_vpu_vpu1_i_l2_rmax, "monitor=1,bus=2,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rpend, bcm2835_vpu_vpu1_i_l2_rpend, "monitor=1,bus=2,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_atwait, bcm2835_vpu_vpu0_i_l2_atwait, "monitor=1,bus=3,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_atrans, bcm2835_vpu_vpu0_i_l2_atrans, "monitor=1,bus=3,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_amax, bcm2835_vpu_vpu0_i_l2_amax, "monitor=1,bus=3,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_wwait, bcm2835_vpu_vpu0_i_l2_wwait, "monitor=1,bus=3,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_wtrans, bcm2835_vpu_vpu0_i_l2_wtrans, "monitor=1,bus=3,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_wmax, bcm2835_vpu_vpu0_i_l2_wmax, "monitor=1,bus=3,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rwait, bcm2835_vpu_vpu0_i_l2_rwait, "monitor=1,bus=3,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rtrans, bcm2835_vpu_vpu0_i_l2_rtrans, "monitor=1,bus=3,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rmax, bcm2835_vpu_vpu0_i_l2_rmax, "monitor=1,bus=3,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rpend, bcm2835_vpu_vpu0_i_l2_rpend, "monitor=1,bus=3,counter=9");
PMU_EVENT_ATTR_STRING(vpu_system_l2_atwait, bcm2835_vpu_system_l2_atwait, "monitor=1,bus=4,counter=0");
PMU_EVENT_ATTR_STRING(vpu_system_l2_atrans, bcm2835_vpu_system_l2_atrans, "monitor=1,bus=4,counter=1");
PMU_EVENT_ATTR_STRING(vpu_system_l2_amax, bcm2835_vpu_system_l2_amax, "monitor=1,bus=4,counter=2");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wwait, bcm2835_vpu_system_l2_wwait, "monitor=1,bus=4,counter=3");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wtrans, bcm2835_vpu_system_l2_wtrans, "monitor=1,bus=4,counter=4");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wmax, bcm2835_vpu_system_l2_wmax, "monitor=1,bus=4,counter=5");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rwait, bcm2835_vpu_system_l2_rwait, "monitor=1,bus=4,counter=6");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rtrans, bcm2835_vpu_system_l2_rtrans, "monitor=1,bus=4,counter=7");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rmax, bcm2835_vpu_system_l2_rmax, "monitor=1,bus=4,counter=8");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rpend, bcm2835_vpu_system_l2_rpend, "monitor=1,bus=4,counter=9");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_atwait, bcm2835_vpu_dma_l2_atwait, "monitor=1,bus=5,counter=0");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_atrans, bcm2835_vpu_dma_l2_atrans, "monitor=1,bus=5,counter=1");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_amax, bcm2835_vpu_dma_l2_amax, "monitor=1,bus=5,counter=2");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_wwait, bcm2835_vpu_dma_l2_wwait, "monitor=1,bus=5,counter=3");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_wtrans, bcm2835_vpu_dma_l2_wtrans, "monitor=1,bus=5,counter=4");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_wmax, bcm2835_vpu_dma_l2_wmax, "monitor=1,bus=5,counter=5");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rwait, bcm2835_vpu_dma_l2_rwait, "monitor=1,bus=5,counter=6");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rtrans, bcm2835_vpu_dma_l2_rtrans, "monitor=1,bus=5,counter=7");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rmax, bcm2835_vpu_dma_l2_rmax, "monitor=1,bus=5,counter=8");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rpend, bcm2835_vpu_dma_l2_rpend, "monitor=1,bus=5,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_atwait, bcm2835_vpu_vpu1_d_uc_atwait, "monitor=1,bus=6,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_atrans, bcm2835_vpu_vpu1_d_uc_atrans, "monitor=1,bus=6,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_amax, bcm2835_vpu_vpu1_d_uc_amax, "monitor=1,bus=6,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_wwait, bcm2835_vpu_vpu1_d_uc_wwait, "monitor=1,bus=6,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_wtrans, bcm2835_vpu_vpu1_d_uc_wtrans, "monitor=1,bus=6,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_wmax, bcm2835_vpu_vpu1_d_uc_wmax, "monitor=1,bus=6,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rwait, bcm2835_vpu_vpu1_d_uc_rwait, "monitor=1,bus=6,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rtrans, bcm2835_vpu_vpu1_d_uc_rtrans, "monitor=1,bus=6,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rmax, bcm2835_vpu_vpu1_d_uc_rmax, "monitor=1,bus=6,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rpend, bcm2835_vpu_vpu1_d_uc_rpend, "monitor=1,bus=6,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_atwait, bcm2835_vpu_vpu0_d_uc_atwait, "monitor=1,bus=7,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_atrans, bcm2835_vpu_vpu0_d_uc_atrans, "monitor=1,bus=7,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_amax, bcm2835_vpu_vpu0_d_uc_amax, "monitor=1,bus=7,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_wwait, bcm2835_vpu_vpu0_d_uc_wwait, "monitor=1,bus=7,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_wtrans, bcm2835_vpu_vpu0_d_uc_wtrans, "monitor=1,bus=7,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_wmax, bcm2835_vpu_vpu0_d_uc_wmax, "monitor=1,bus=7,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rwait, bcm2835_vpu_vpu0_d_uc_rwait, "monitor=1,bus=7,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rtrans, bcm2835_vpu_vpu0_d_uc_rtrans, "monitor=1,bus=7,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rmax, bcm2835_vpu_vpu0_d_uc_rmax, "monitor=1,bus=7,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rpend, bcm2835_vpu_vpu0_d_uc_rpend, "monitor=1,bus=7,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_atwait, bcm2835_vpu_vpu1_i_uc_atwait, "monitor=1,bus=8,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_atrans, bcm2835_vpu_vpu1_i_uc_atrans, "monitor=1,bus=8,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_amax, bcm2835_vpu_vpu1_i_uc_amax, "monitor=1,bus=8,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_wwait, bcm2835_vpu_vpu1_i_uc_wwait, "monitor=1,bus=8,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_wtrans, bcm2835_vpu_vpu1_i_uc_wtrans, "monitor=1,bus=8,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_wmax, bcm2835_vpu_vpu1_i_uc_wmax, "monitor=1,bus=8,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rwait, bcm2835_vpu_vpu1_i_uc_rwait, "monitor=1,bus=8,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rtrans, bcm2835_vpu_vpu1_i_uc_rtrans, "monitor=1,bus=8,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rmax, bcm2835_vpu_vpu1_i_uc_rmax, "monitor=1,bus=8,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rpend, bcm2835_vpu_vpu1_i_uc_rpend, "monitor=1,bus=8,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_atwait, bcm2835_vpu_vpu0_i_uc_atwait, "monitor=1,bus=9,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_atrans, bcm2835_vpu_vpu0_i_uc_atrans, "monitor=1,bus=9,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_amax, bcm2835_vpu_vpu0_i_uc_amax, "monitor=1,bus=9,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_wwait, bcm2835_vpu_vpu0_i_uc_wwait, "monitor=1,bus=9,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_wtrans, bcm2835_vpu_vpu0_i_uc_wtrans, "monitor=1,bus=9,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_wmax, bcm2835_vpu_vpu0_i_uc_wmax, "monitor=1,bus=9,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rwait, bcm2835_vpu_vpu0_i_uc_rwait, "monitor=1,bus=9,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rtrans, bcm2835_vpu_vpu0_i_uc_rtrans, "monitor=1,bus=9,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rmax, bcm2835_vpu_vpu0_i_uc_rmax, "monitor=1,bus=9,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rpend, bcm2835_vpu_vpu0_i_uc_rpend, "monitor=1,bus=9,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_atwait, bcm2835_vpu_vpu_uc_atwait, "monitor=1,bus=10,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_atrans, bcm2835_vpu_vpu_uc_atrans, "monitor=1,bus=10,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_amax, bcm2835_vpu_vpu_uc_amax, "monitor=1,bus=10,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_wwait, bcm2835_vpu_vpu_uc_wwait, "monitor=1,bus=10,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_wtrans, bcm2835_vpu_vpu_uc_wtrans, "monitor=1,bus=10,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_wmax, bcm2835_vpu_vpu_uc_wmax, "monitor=1,bus=10,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rwait, bcm2835_vpu_vpu_uc_rwait, "monitor=1,bus=10,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rtrans, bcm2835_vpu_vpu_uc_rtrans, "monitor=1,bus=10,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rmax, bcm2835_vpu_vpu_uc_rmax, "monitor=1,bus=10,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rpend, bcm2835_vpu_vpu_uc_rpend, "monitor=1,bus=10,counter=9");
PMU_EVENT_ATTR_STRING(vpu_l2_out_atwait, bcm2835_vpu_l2_out_atwait, "monitor=1,bus=11,counter=0");
PMU_EVENT_ATTR_STRING(vpu_l2_out_atrans, bcm2835_vpu_l2_out_atrans, "monitor=1,bus=11,counter=1");
PMU_EVENT_ATTR_STRING(vpu_l2_out_amax, bcm2835_vpu_l2_out_amax, "monitor=1,bus=11,counter=2");
PMU_EVENT_ATTR_STRING(vpu_l2_out_wwait, bcm2835_vpu_l2_out_wwait, "monitor=1,bus=11,counter=3");
PMU_EVENT_ATTR_STRING(vpu_l2_out_wtrans, bcm2835_vpu_l2_out_wtrans, "monitor=1,bus=11,counter=4");
PMU_EVENT_ATTR_STRING(vpu_l2_out_wmax, bcm2835_vpu_l2_out_wmax, "monitor=1,bus=11,counter=5");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rwait, bcm2835_vpu_l2_out_rwait, "monitor=1,bus=11,counter=6");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rtrans, bcm2835_vpu_l2_out_rtrans, "monitor=1,bus=11,counter=7");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rmax, bcm2835_vpu_l2_out_rmax, "monitor=1,bus=11,counter=8");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rpend, bcm2835_vpu_l2_out_rpend, "monitor=1,bus=11,counter=9");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_atwait, bcm2835_vpu_dma_uc_atwait, "monitor=1,bus=12,counter=0");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_atrans, bcm2835_vpu_dma_uc_atrans, "monitor=1,bus=12,counter=1");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_amax, bcm2835_vpu_dma_uc_amax, "monitor=1,bus=12,counter=2");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_wwait, bcm2835_vpu_dma_uc_wwait, "monitor=1,bus=12,counter=3");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_wtrans, bcm2835_vpu_dma_uc_wtrans, "monitor=1,bus=12,counter=4");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_wmax, bcm2835_vpu_dma_uc_wmax, "monitor=1,bus=12,counter=5");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rwait, bcm2835_vpu_dma_uc_rwait, "monitor=1,bus=12,counter=6");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rtrans, bcm2835_vpu_dma_uc_rtrans, "monitor=1,bus=12,counter=7");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rmax, bcm2835_vpu_dma_uc_rmax, "monitor=1,bus=12,counter=8");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rpend, bcm2835_vpu_dma_uc_rpend, "monitor=1,bus=12,counter=9");
PMU_EVENT_ATTR_STRING(vpu_l2_in_atwait, bcm2835_vpu_l2_in_atwait, "monitor=1,bus=13,counter=0");
PMU_EVENT_ATTR_STRING(vpu_l2_in_atrans, bcm2835_vpu_l2_in_atrans, "monitor=1,bus=13,counter=1");
PMU_EVENT_ATTR_STRING(vpu_l2_in_amax, bcm2835_vpu_l2_in_amax, "monitor=1,bus=13,counter=2");
PMU_EVENT_ATTR_STRING(vpu_l2_in_wwait, bcm2835_vpu_l2_in_wwait, "monitor=1,bus=13,counter=3");
PMU_EVENT_ATTR_STRING(vpu_l2_in_wtrans, bcm2835_vpu_l2_in_wtrans, "monitor=1,bus=13,counter=4");
PMU_EVENT_ATTR_STRING(vpu_l2_in_wmax, bcm2835_vpu_l2_in_wmax, "monitor=1,bus=13,counter=5");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rwait, bcm2835_vpu_l2_in_rwait, "monitor=1,bus=13,counter=6");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rtrans, bcm2835_vpu_l2_in_rtrans, "monitor=1,bus=13,counter=7");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rmax, bcm2835_vpu_l2_in_rmax, "monitor=1,bus=13,counter=8");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rpend, bcm2835_vpu_l2_in_rpend, "monitor=1,bus=13,counter=9");
PMU_EVENT_ATTR_STRING(vpu_sdram_atwait, bcm2835_vpu_sdram_atwait, "monitor=1,bus=14,counter=0");
PMU_EVENT_ATTR_STRING(vpu_sdram_atrans, bcm2835_vpu_sdram_atrans, "monitor=1,bus=14,counter=1");
PMU_EVENT_ATTR_STRING(vpu_sdram_amax, bcm2835_vpu_sdram_amax, "monitor=1,bus=14,counter=2");
PMU_EVENT_ATTR_STRING(vpu_sdram_wwait, bcm2835_vpu_sdram_wwait, "monitor=1,bus=14,counter=3");
PMU_EVENT_ATTR_STRING(vpu_sdram_wtrans, bcm2835_vpu_sdram_wtrans, "monitor=1,bus=14,counter=4");
PMU_EVENT_ATTR_STRING(vpu_sdram_wmax, bcm2835_vpu_sdram_wmax, "monitor=1,bus=14,counter=5");
PMU_EVENT_ATTR_STRING(vpu_sdram_rwait, bcm2835_vpu_sdram_rwait, "monitor=1,bus=14,counter=6");
PMU_EVENT_ATTR_STRING(vpu_sdram_rtrans, bcm2835_vpu_sdram_rtrans, "monitor=1,bus=14,counter=7");
PMU_EVENT_ATTR_STRING(vpu_sdram_rmax, bcm2835_vpu_sdram_rmax, "monitor=1,bus=14,counter=8");
PMU_EVENT_ATTR_STRING(vpu_sdram_rpend, bcm2835_vpu_sdram_rpend, "monitor=1,bus=14,counter=9");

static struct attribute *bcm2835_events[] = {
	&bcm2835_dma_l2_atwait.attr.attr,
	&bcm2835_dma_l2_atrans.attr.attr,
	&bcm2835_dma_l2_amax.attr.attr,
	&bcm2835_dma_l2_wwait.attr.attr,
	&bcm2835_dma_l2_wtrans.attr.attr,
	&bcm2835_dma_l2_wmax.attr.attr,
	&bcm2835_dma_l2_rwait.attr.attr,
	&bcm2835_dma_l2_rtrans.attr.attr,
	&bcm2835_dma_l2_rmax.attr.attr,
	&bcm2835_dma_l2_rpend.attr.attr,
	&bcm2835_trans_atwait.attr.attr,
	&bcm2835_trans_atrans.attr.attr,
	&bcm2835_trans_amax.attr.attr,
	&bcm2835_trans_wwait.attr.attr,
	&bcm2835_trans_wtrans.attr.attr,
	&bcm2835_trans_wmax.attr.attr,
	&bcm2835_trans_rwait.attr.attr,
	&bcm2835_trans_rtrans.attr.attr,
	&bcm2835_trans_rmax.attr.attr,
	&bcm2835_trans_rpend.attr.attr,
	&bcm2835_jpeg_atwait.attr.attr,
	&bcm2835_jpeg_atrans.attr.attr,
	&bcm2835_jpeg_amax.attr.attr,
	&bcm2835_jpeg_wwait.attr.attr,
	&bcm2835_jpeg_wtrans.attr.attr,
	&bcm2835_jpeg_wmax.attr.attr,
	&bcm2835_jpeg_rwait.attr.attr,
	&bcm2835_jpeg_rtrans.attr.attr,
	&bcm2835_jpeg_rmax.attr.attr,
	&bcm2835_jpeg_rpend.attr.attr,
	&bcm2835_system_uc_atwait.attr.attr,
	&bcm2835_system_uc_atrans.attr.attr,
	&bcm2835_system_uc_amax.attr.attr,
	&bcm2835_system_uc_wwait.attr.attr,
	&bcm2835_system_uc_wtrans.attr.attr,
	&bcm2835_system_uc_wmax.attr.attr,
	&bcm2835_system_uc_rwait.attr.attr,
	&bcm2835_system_uc_rtrans.attr.attr,
	&bcm2835_system_uc_rmax.attr.attr,
	&bcm2835_system_uc_rpend.attr.attr,
	&bcm2835_dma_uc_atwait.attr.attr,
	&bcm2835_dma_uc_atrans.attr.attr,
	&bcm2835_dma_uc_amax.attr.attr,
	&bcm2835_dma_uc_wwait.attr.attr,
	&bcm2835_dma_uc_wtrans.attr.attr,
	&bcm2835_dma_uc_wmax.attr.attr,
	&bcm2835_dma_uc_rwait.attr.attr,
	&bcm2835_dma_uc_rtrans.attr.attr,
	&bcm2835_dma_uc_rmax.attr.attr,
	&bcm2835_dma_uc_rpend.attr.attr,
	&bcm2835_system_l2_atwait.attr.attr,
	&bcm2835_system_l2_atrans.attr.attr,
	&bcm2835_system_l2_amax.attr.attr,
	&bcm2835_system_l2_wwait.attr.attr,
	&bcm2835_system_l2_wtrans.attr.attr,
	&bcm2835_system_l2_wmax.attr.attr,
	&bcm2835_system_l2_rwait.attr.attr,
	&bcm2835_system_l2_rtrans.attr.attr,
	&bcm2835_system_l2_rmax.attr.attr,
	&bcm2835_system_l2_rpend.attr.attr,
	&bcm2835_ccp2tx_atwait.attr.attr,
	&bcm2835_ccp2tx_atrans.attr.attr,
	&bcm2835_ccp2tx_amax.attr.attr,
	&bcm2835_ccp2tx_wwait.attr.attr,
	&bcm2835_ccp2tx_wtrans.attr.attr,
	&bcm2835_ccp2tx_wmax.attr.attr,
	&bcm2835_ccp2tx_rwait.attr.attr,
	&bcm2835_ccp2tx_rtrans.attr.attr,
	&bcm2835_ccp2tx_rmax.attr.attr,
	&bcm2835_ccp2tx_rpend.attr.attr,
	&bcm2835_mphi_rx_atwait.attr.attr,
	&bcm2835_mphi_rx_atrans.attr.attr,
	&bcm2835_mphi_rx_amax.attr.attr,
	&bcm2835_mphi_rx_wwait.attr.attr,
	&bcm2835_mphi_rx_wtrans.attr.attr,
	&bcm2835_mphi_rx_wmax.attr.attr,
	&bcm2835_mphi_rx_rwait.attr.attr,
	&bcm2835_mphi_rx_rtrans.attr.attr,
	&bcm2835_mphi_rx_rmax.attr.attr,
	&bcm2835_mphi_rx_rpend.attr.attr,
	&bcm2835_mphi_tx_atwait.attr.attr,
	&bcm2835_mphi_tx_atrans.attr.attr,
	&bcm2835_mphi_tx_amax.attr.attr,
	&bcm2835_mphi_tx_wwait.attr.attr,
	&bcm2835_mphi_tx_wtrans.attr.attr,
	&bcm2835_mphi_tx_wmax.attr.attr,
	&bcm2835_mphi_tx_rwait.attr.attr,
	&bcm2835_mphi_tx_rtrans.attr.attr,
	&bcm2835_mphi_tx_rmax.attr.attr,
	&bcm2835_mphi_tx_rpend.attr.attr,
	&bcm2835_hvs_atwait.attr.attr,
	&bcm2835_hvs_atrans.attr.attr,
	&bcm2835_hvs_amax.attr.attr,
	&bcm2835_hvs_wwait.attr.attr,
	&bcm2835_hvs_wtrans.attr.attr,
	&bcm2835_hvs_wmax.attr.attr,
	&bcm2835_hvs_rwait.attr.attr,
	&bcm2835_hvs_rtrans.attr.attr,
	&bcm2835_hvs_rmax.attr.attr,
	&bcm2835_hvs_rpend.attr.attr,
	&bcm2835_h264_atwait.attr.attr,
	&bcm2835_h264_atrans.attr.attr,
	&bcm2835_h264_amax.attr.attr,
	&bcm2835_h264_wwait.attr.attr,
	&bcm2835_h264_wtrans.attr.attr,
	&bcm2835_h264_wmax.attr.attr,
	&bcm2835_h264_rwait.attr.attr,
	&bcm2835_h264_rtrans.attr.attr,
	&bcm2835_h264_rmax.attr.attr,
	&bcm2835_h264_rpend.attr.attr,
	&bcm2835_isp_atwait.attr.attr,
	&bcm2835_isp_atrans.attr.attr,
	&bcm2835_isp_amax.attr.attr,
	&bcm2835_isp_wwait.attr.attr,
	&bcm2835_isp_wtrans.attr.attr,
	&bcm2835_isp_wmax.attr.attr,
	&bcm2835_isp_rwait.attr.attr,
	&bcm2835_isp_rtrans.attr.attr,
	&bcm2835_isp_rmax.attr.attr,
	&bcm2835_isp_rpend.attr.attr,
	&bcm2835_v3d_atwait.attr.attr,
	&bcm2835_v3d_atrans.attr.attr,
	&bcm2835_v3d_amax.attr.attr,
	&bcm2835_v3d_wwait.attr.attr,
	&bcm2835_v3d_wtrans.attr.attr,
	&bcm2835_v3d_wmax.attr.attr,
	&bcm2835_v3d_rwait.attr.attr,
	&bcm2835_v3d_rtrans.attr.attr,
	&bcm2835_v3d_rmax.attr.attr,
	&bcm2835_v3d_rpend.attr.attr,
	&bcm2835_peripheral_atwait.attr.attr,
	&bcm2835_peripheral_atrans.attr.attr,
	&bcm2835_peripheral_amax.attr.attr,
	&bcm2835_peripheral_wwait.attr.attr,
	&bcm2835_peripheral_wtrans.attr.attr,
	&bcm2835_peripheral_wmax.attr.attr,
	&bcm2835_peripheral_rwait.attr.attr,
	&bcm2835_peripheral_rtrans.attr.attr,
	&bcm2835_peripheral_rmax.attr.attr,
	&bcm2835_peripheral_rpend.attr.attr,
	&bcm2835_cpu_uc_atwait.attr.attr,
	&bcm2835_cpu_uc_atrans.attr.attr,
	&bcm2835_cpu_uc_amax.attr.attr,
	&bcm2835_cpu_uc_wwait.attr.attr,
	&bcm2835_cpu_uc_wtrans.attr.attr,
	&bcm2835_cpu_uc_wmax.attr.attr,
	&bcm2835_cpu_uc_rwait.attr.attr,
	&bcm2835_cpu_uc_rtrans.attr.attr,
	&bcm2835_cpu_uc_rmax.attr.attr,
	&bcm2835_cpu_uc_rpend.attr.attr,
	&bcm2835_cpu_l2_atwait.attr.attr,
	&bcm2835_cpu_l2_atrans.attr.attr,
	&bcm2835_cpu_l2_amax.attr.attr,
	&bcm2835_cpu_l2_wwait.attr.attr,
	&bcm2835_cpu_l2_wtrans.attr.attr,
	&bcm2835_cpu_l2_wmax.attr.attr,
	&bcm2835_cpu_l2_rwait.attr.attr,
	&bcm2835_cpu_l2_rtrans.attr.attr,
	&bcm2835_cpu_l2_rmax.attr.attr,
	&bcm2835_cpu_l2_rpend.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_atwait.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_atrans.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_amax.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_wwait.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_wtrans.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_wmax.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_rwait.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_rtrans.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_rmax.attr.attr,
	&bcm2835_vpu_vpu1_d_l2_rpend.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_atwait.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_atrans.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_amax.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_wwait.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_wtrans.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_wmax.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_rwait.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_rtrans.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_rmax.attr.attr,
	&bcm2835_vpu_vpu0_d_l2_rpend.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_atwait.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_atrans.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_amax.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_wwait.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_wtrans.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_wmax.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_rwait.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_rtrans.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_rmax.attr.attr,
	&bcm2835_vpu_vpu1_i_l2_rpend.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_atwait.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_atrans.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_amax.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_wwait.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_wtrans.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_wmax.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_rwait.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_rtrans.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_rmax.attr.attr,
	&bcm2835_vpu_vpu0_i_l2_rpend.attr.attr,
	&bcm2835_vpu_system_l2_atwait.attr.attr,
	&bcm2835_vpu_system_l2_atrans.attr.attr,
	&bcm2835_vpu_system_l2_amax.attr.attr,
	&bcm2835_vpu_system_l2_wwait.attr.attr,
	&bcm2835_vpu_system_l2_wtrans.attr.attr,
	&bcm2835_vpu_system_l2_wmax.attr.attr,
	&bcm2835_vpu_system_l2_rwait.attr.attr,
	&bcm2835_vpu_system_l2_rtrans.attr.attr,
	&bcm2835_vpu_system_l2_rmax.attr.attr,
	&bcm2835_vpu_system_l2_rpend.attr.attr,
	&bcm2835_vpu_dma_l2_atwait.attr.attr,
	&bcm2835_vpu_dma_l2_atrans.attr.attr,
	&bcm2835_vpu_dma_l2_amax.attr.attr,
	&bcm2835_vpu_dma_l2_wwait.attr.attr,
	&bcm2835_vpu_dma_l2_wtrans.attr.attr,
	&bcm2835_vpu_dma_l2_wmax.attr.attr,
	&bcm2835_vpu_dma_l2_rwait.attr.attr,
	&bcm2835_vpu_dma_l2_rtrans.attr.attr,
	&bcm2835_vpu_dma_l2_rmax.attr.attr,
	&bcm2835_vpu_dma_l2_rpend.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_atwait.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_atrans.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_amax.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_wwait.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_wtrans.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_wmax.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_rwait.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_rtrans.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_rmax.attr.attr,
	&bcm2835_vpu_vpu1_d_uc_rpend.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_atwait.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_atrans.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_amax.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_wwait.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_wtrans.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_wmax.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_rwait.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_rtrans.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_rmax.attr.attr,
	&bcm2835_vpu_vpu0_d_uc_rpend.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_atwait.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_atrans.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_amax.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_wwait.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_wtrans.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_wmax.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_rwait.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_rtrans.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_rmax.attr.attr,
	&bcm2835_vpu_vpu1_i_uc_rpend.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_atwait.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_atrans.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_amax.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_wwait.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_wtrans.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_wmax.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_rwait.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_rtrans.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_rmax.attr.attr,
	&bcm2835_vpu_vpu0_i_uc_rpend.attr.attr,
	&bcm2835_vpu_vpu_uc_atwait.attr.attr,
	&bcm2835_vpu_vpu_uc_atrans.attr.attr,
	&bcm2835_vpu_vpu_uc_amax.attr.attr,
	&bcm2835_vpu_vpu_uc_wwait.attr.attr,
	&bcm2835_vpu_vpu_uc_wtrans.attr.attr,
	&bcm2835_vpu_vpu_uc_wmax.attr.attr,
	&bcm2835_vpu_vpu_uc_rwait.attr.attr,
	&bcm2835_vpu_vpu_uc_rtrans.attr.attr,
	&bcm2835_vpu_vpu_uc_rmax.attr.attr,
	&bcm2835_vpu_vpu_uc_rpend.attr.attr,
	&bcm2835_vpu_l2_out_atwait.attr.attr,
	&bcm2835_vpu_l2_out_atrans.attr.attr,
	&bcm2835_vpu_l2_out_amax.attr.attr,
	&bcm2835_vpu_l2_out_wwait.attr.attr,
	&bcm2835_vpu_l2_out_wtrans.attr.attr,
	&bcm2835_vpu_l2_out_wmax.attr.attr,
	&bcm2835_vpu_l2_out_rwait.attr.attr,
	&bcm2835_vpu_l2_out_rtrans.attr.attr,
	&bcm2835_vpu_l2_out_rmax.attr.attr,
	&bcm2835_vpu_l2_out_rpend.attr.attr,
	&bcm2835_vpu_dma_uc_atwait.attr.attr,
	&bcm2835_vpu_dma_uc_atrans.attr.attr,
	&bcm2835_vpu_dma_uc_amax.attr.attr,
	&bcm2835_vpu_dma_uc_wwait.attr.attr,
	&bcm2835_vpu_dma_uc_wtrans.attr.attr,
	&bcm2835_vpu_dma_uc_wmax.attr.attr,
	&bcm2835_vpu_dma_uc_rwait.attr.attr,
	&bcm2835_vpu_dma_uc_rtrans.attr.attr,
	&bcm2835_vpu_dma_uc_rmax.attr.attr,
	&bcm2835_vpu_dma_uc_rpend.attr.attr,
	&bcm2835_vpu_l2_in_atwait.attr.attr,
	&bcm2835_vpu_l2_in_atrans.attr.attr,
	&bcm2835_vpu_l2_in_amax.attr.attr,
	&bcm2835_vpu_l2_in_wwait.attr.attr,
	&bcm2835_vpu_l2_in_wtrans.attr.attr,
	&bcm2835_vpu_l2_in_wmax.attr.attr,
	&bcm2835_vpu_l2_in_rwait.attr.attr,
	&bcm2835_vpu_l2_in_rtrans.attr.attr,
	&bcm2835_vpu_l2_in_rmax.attr.attr,
	&bcm2835_vpu_l2_in_rpend.attr.attr,
	&bcm2835_vpu_sdram_atwait.attr.attr,
	&bcm2835_vpu_sdram_atrans.attr.attr,
	&bcm2835_vpu_sdram_amax.attr.attr,
	&bcm2835_vpu_sdram_wwait.attr.attr,
	&bcm2835_vpu_sdram_wtrans.attr.attr,
	&bcm2835_vpu_sdram_wmax.attr.attr,
	&bcm2835_vpu_sdram_rwait.attr.attr,
	&bcm2835_vpu_sdram_rtrans.attr.attr,
	&bcm2835_vpu_sdram_rmax.attr.attr,
	&bcm2835_vpu_sdram_rpend.attr.attr,
	NULL,
};

static const struct attribute_group rpi_axi_pmu_bcm2835_events_group = {
	.name = "events",
	.attrs = bcm2835_events,
	.is_visible = rpi_axi_pmu_events_is_visible,
};

PMU_EVENT_ATTR_STRING(dma_l2_atwait, bcm2711_dma_l2_atwait, "monitor=0,bus=0,counter=0");
PMU_EVENT_ATTR_STRING(dma_l2_atrans, bcm2711_dma_l2_atrans, "monitor=0,bus=0,counter=1");
PMU_EVENT_ATTR_STRING(dma_l2_amax, bcm2711_dma_l2_amax, "monitor=0,bus=0,counter=2");
PMU_EVENT_ATTR_STRING(dma_l2_wwait, bcm2711_dma_l2_wwait, "monitor=0,bus=0,counter=3");
PMU_EVENT_ATTR_STRING(dma_l2_wtrans, bcm2711_dma_l2_wtrans, "monitor=0,bus=0,counter=4");
PMU_EVENT_ATTR_STRING(dma_l2_wmax, bcm2711_dma_l2_wmax, "monitor=0,bus=0,counter=5");
PMU_EVENT_ATTR_STRING(dma_l2_rwait, bcm2711_dma_l2_rwait, "monitor=0,bus=0,counter=6");
PMU_EVENT_ATTR_STRING(dma_l2_rtrans, bcm2711_dma_l2_rtrans, "monitor=0,bus=0,counter=7");
PMU_EVENT_ATTR_STRING(dma_l2_rmax, bcm2711_dma_l2_rmax, "monitor=0,bus=0,counter=8");
PMU_EVENT_ATTR_STRING(dma_l2_rpend, bcm2711_dma_l2_rpend, "monitor=0,bus=0,counter=9");
PMU_EVENT_ATTR_STRING(trans_atwait, bcm2711_trans_atwait, "monitor=0,bus=1,counter=0");
PMU_EVENT_ATTR_STRING(trans_atrans, bcm2711_trans_atrans, "monitor=0,bus=1,counter=1");
PMU_EVENT_ATTR_STRING(trans_amax, bcm2711_trans_amax, "monitor=0,bus=1,counter=2");
PMU_EVENT_ATTR_STRING(trans_wwait, bcm2711_trans_wwait, "monitor=0,bus=1,counter=3");
PMU_EVENT_ATTR_STRING(trans_wtrans, bcm2711_trans_wtrans, "monitor=0,bus=1,counter=4");
PMU_EVENT_ATTR_STRING(trans_wmax, bcm2711_trans_wmax, "monitor=0,bus=1,counter=5");
PMU_EVENT_ATTR_STRING(trans_rwait, bcm2711_trans_rwait, "monitor=0,bus=1,counter=6");
PMU_EVENT_ATTR_STRING(trans_rtrans, bcm2711_trans_rtrans, "monitor=0,bus=1,counter=7");
PMU_EVENT_ATTR_STRING(trans_rmax, bcm2711_trans_rmax, "monitor=0,bus=1,counter=8");
PMU_EVENT_ATTR_STRING(trans_rpend, bcm2711_trans_rpend, "monitor=0,bus=1,counter=9");
PMU_EVENT_ATTR_STRING(jpeg_atwait, bcm2711_jpeg_atwait, "monitor=0,bus=2,counter=0");
PMU_EVENT_ATTR_STRING(jpeg_atrans, bcm2711_jpeg_atrans, "monitor=0,bus=2,counter=1");
PMU_EVENT_ATTR_STRING(jpeg_amax, bcm2711_jpeg_amax, "monitor=0,bus=2,counter=2");
PMU_EVENT_ATTR_STRING(jpeg_wwait, bcm2711_jpeg_wwait, "monitor=0,bus=2,counter=3");
PMU_EVENT_ATTR_STRING(jpeg_wtrans, bcm2711_jpeg_wtrans, "monitor=0,bus=2,counter=4");
PMU_EVENT_ATTR_STRING(jpeg_wmax, bcm2711_jpeg_wmax, "monitor=0,bus=2,counter=5");
PMU_EVENT_ATTR_STRING(jpeg_rwait, bcm2711_jpeg_rwait, "monitor=0,bus=2,counter=6");
PMU_EVENT_ATTR_STRING(jpeg_rtrans, bcm2711_jpeg_rtrans, "monitor=0,bus=2,counter=7");
PMU_EVENT_ATTR_STRING(jpeg_rmax, bcm2711_jpeg_rmax, "monitor=0,bus=2,counter=8");
PMU_EVENT_ATTR_STRING(jpeg_rpend, bcm2711_jpeg_rpend, "monitor=0,bus=2,counter=9");
PMU_EVENT_ATTR_STRING(vpu_uc_atwait, bcm2711_vpu_uc_atwait, "monitor=0,bus=3,counter=0");
PMU_EVENT_ATTR_STRING(vpu_uc_atrans, bcm2711_vpu_uc_atrans, "monitor=0,bus=3,counter=1");
PMU_EVENT_ATTR_STRING(vpu_uc_amax, bcm2711_vpu_uc_amax, "monitor=0,bus=3,counter=2");
PMU_EVENT_ATTR_STRING(vpu_uc_wwait, bcm2711_vpu_uc_wwait, "monitor=0,bus=3,counter=3");
PMU_EVENT_ATTR_STRING(vpu_uc_wtrans, bcm2711_vpu_uc_wtrans, "monitor=0,bus=3,counter=4");
PMU_EVENT_ATTR_STRING(vpu_uc_wmax, bcm2711_vpu_uc_wmax, "monitor=0,bus=3,counter=5");
PMU_EVENT_ATTR_STRING(vpu_uc_rwait, bcm2711_vpu_uc_rwait, "monitor=0,bus=3,counter=6");
PMU_EVENT_ATTR_STRING(vpu_uc_rtrans, bcm2711_vpu_uc_rtrans, "monitor=0,bus=3,counter=7");
PMU_EVENT_ATTR_STRING(vpu_uc_rmax, bcm2711_vpu_uc_rmax, "monitor=0,bus=3,counter=8");
PMU_EVENT_ATTR_STRING(vpu_uc_rpend, bcm2711_vpu_uc_rpend, "monitor=0,bus=3,counter=9");
PMU_EVENT_ATTR_STRING(dma_uc_atwait, bcm2711_dma_uc_atwait, "monitor=0,bus=4,counter=0");
PMU_EVENT_ATTR_STRING(dma_uc_atrans, bcm2711_dma_uc_atrans, "monitor=0,bus=4,counter=1");
PMU_EVENT_ATTR_STRING(dma_uc_amax, bcm2711_dma_uc_amax, "monitor=0,bus=4,counter=2");
PMU_EVENT_ATTR_STRING(dma_uc_wwait, bcm2711_dma_uc_wwait, "monitor=0,bus=4,counter=3");
PMU_EVENT_ATTR_STRING(dma_uc_wtrans, bcm2711_dma_uc_wtrans, "monitor=0,bus=4,counter=4");
PMU_EVENT_ATTR_STRING(dma_uc_wmax, bcm2711_dma_uc_wmax, "monitor=0,bus=4,counter=5");
PMU_EVENT_ATTR_STRING(dma_uc_rwait, bcm2711_dma_uc_rwait, "monitor=0,bus=4,counter=6");
PMU_EVENT_ATTR_STRING(dma_uc_rtrans, bcm2711_dma_uc_rtrans, "monitor=0,bus=4,counter=7");
PMU_EVENT_ATTR_STRING(dma_uc_rmax, bcm2711_dma_uc_rmax, "monitor=0,bus=4,counter=8");
PMU_EVENT_ATTR_STRING(dma_uc_rpend, bcm2711_dma_uc_rpend, "monitor=0,bus=4,counter=9");
PMU_EVENT_ATTR_STRING(system_l2_atwait, bcm2711_system_l2_atwait, "monitor=0,bus=5,counter=0");
PMU_EVENT_ATTR_STRING(system_l2_atrans, bcm2711_system_l2_atrans, "monitor=0,bus=5,counter=1");
PMU_EVENT_ATTR_STRING(system_l2_amax, bcm2711_system_l2_amax, "monitor=0,bus=5,counter=2");
PMU_EVENT_ATTR_STRING(system_l2_wwait, bcm2711_system_l2_wwait, "monitor=0,bus=5,counter=3");
PMU_EVENT_ATTR_STRING(system_l2_wtrans, bcm2711_system_l2_wtrans, "monitor=0,bus=5,counter=4");
PMU_EVENT_ATTR_STRING(system_l2_wmax, bcm2711_system_l2_wmax, "monitor=0,bus=5,counter=5");
PMU_EVENT_ATTR_STRING(system_l2_rwait, bcm2711_system_l2_rwait, "monitor=0,bus=5,counter=6");
PMU_EVENT_ATTR_STRING(system_l2_rtrans, bcm2711_system_l2_rtrans, "monitor=0,bus=5,counter=7");
PMU_EVENT_ATTR_STRING(system_l2_rmax, bcm2711_system_l2_rmax, "monitor=0,bus=5,counter=8");
PMU_EVENT_ATTR_STRING(system_l2_rpend, bcm2711_system_l2_rpend, "monitor=0,bus=5,counter=9");
PMU_EVENT_ATTR_STRING(hvs_atwait, bcm2711_hvs_atwait, "monitor=0,bus=6,counter=0");
PMU_EVENT_ATTR_STRING(hvs_atrans, bcm2711_hvs_atrans, "monitor=0,bus=6,counter=1");
PMU_EVENT_ATTR_STRING(hvs_amax, bcm2711_hvs_amax, "monitor=0,bus=6,counter=2");
PMU_EVENT_ATTR_STRING(hvs_wwait, bcm2711_hvs_wwait, "monitor=0,bus=6,counter=3");
PMU_EVENT_ATTR_STRING(hvs_wtrans, bcm2711_hvs_wtrans, "monitor=0,bus=6,counter=4");
PMU_EVENT_ATTR_STRING(hvs_wmax, bcm2711_hvs_wmax, "monitor=0,bus=6,counter=5");
PMU_EVENT_ATTR_STRING(hvs_rwait, bcm2711_hvs_rwait, "monitor=0,bus=6,counter=6");
PMU_EVENT_ATTR_STRING(hvs_rtrans, bcm2711_hvs_rtrans, "monitor=0,bus=6,counter=7");
PMU_EVENT_ATTR_STRING(hvs_rmax, bcm2711_hvs_rmax, "monitor=0,bus=6,counter=8");
PMU_EVENT_ATTR_STRING(hvs_rpend, bcm2711_hvs_rpend, "monitor=0,bus=6,counter=9");
PMU_EVENT_ATTR_STRING(argon_atwait, bcm2711_argon_atwait, "monitor=0,bus=7,counter=0");
PMU_EVENT_ATTR_STRING(argon_atrans, bcm2711_argon_atrans, "monitor=0,bus=7,counter=1");
PMU_EVENT_ATTR_STRING(argon_amax, bcm2711_argon_amax, "monitor=0,bus=7,counter=2");
PMU_EVENT_ATTR_STRING(argon_wwait, bcm2711_argon_wwait, "monitor=0,bus=7,counter=3");
PMU_EVENT_ATTR_STRING(argon_wtrans, bcm2711_argon_wtrans, "monitor=0,bus=7,counter=4");
PMU_EVENT_ATTR_STRING(argon_wmax, bcm2711_argon_wmax, "monitor=0,bus=7,counter=5");
PMU_EVENT_ATTR_STRING(argon_rwait, bcm2711_argon_rwait, "monitor=0,bus=7,counter=6");
PMU_EVENT_ATTR_STRING(argon_rtrans, bcm2711_argon_rtrans, "monitor=0,bus=7,counter=7");
PMU_EVENT_ATTR_STRING(argon_rmax, bcm2711_argon_rmax, "monitor=0,bus=7,counter=8");
PMU_EVENT_ATTR_STRING(argon_rpend, bcm2711_argon_rpend, "monitor=0,bus=7,counter=9");
PMU_EVENT_ATTR_STRING(h264_atwait, bcm2711_h264_atwait, "monitor=0,bus=8,counter=0");
PMU_EVENT_ATTR_STRING(h264_atrans, bcm2711_h264_atrans, "monitor=0,bus=8,counter=1");
PMU_EVENT_ATTR_STRING(h264_amax, bcm2711_h264_amax, "monitor=0,bus=8,counter=2");
PMU_EVENT_ATTR_STRING(h264_wwait, bcm2711_h264_wwait, "monitor=0,bus=8,counter=3");
PMU_EVENT_ATTR_STRING(h264_wtrans, bcm2711_h264_wtrans, "monitor=0,bus=8,counter=4");
PMU_EVENT_ATTR_STRING(h264_wmax, bcm2711_h264_wmax, "monitor=0,bus=8,counter=5");
PMU_EVENT_ATTR_STRING(h264_rwait, bcm2711_h264_rwait, "monitor=0,bus=8,counter=6");
PMU_EVENT_ATTR_STRING(h264_rtrans, bcm2711_h264_rtrans, "monitor=0,bus=8,counter=7");
PMU_EVENT_ATTR_STRING(h264_rmax, bcm2711_h264_rmax, "monitor=0,bus=8,counter=8");
PMU_EVENT_ATTR_STRING(h264_rpend, bcm2711_h264_rpend, "monitor=0,bus=8,counter=9");
PMU_EVENT_ATTR_STRING(peripheral_atwait, bcm2711_peripheral_atwait, "monitor=0,bus=9,counter=0");
PMU_EVENT_ATTR_STRING(peripheral_atrans, bcm2711_peripheral_atrans, "monitor=0,bus=9,counter=1");
PMU_EVENT_ATTR_STRING(peripheral_amax, bcm2711_peripheral_amax, "monitor=0,bus=9,counter=2");
PMU_EVENT_ATTR_STRING(peripheral_wwait, bcm2711_peripheral_wwait, "monitor=0,bus=9,counter=3");
PMU_EVENT_ATTR_STRING(peripheral_wtrans, bcm2711_peripheral_wtrans, "monitor=0,bus=9,counter=4");
PMU_EVENT_ATTR_STRING(peripheral_wmax, bcm2711_peripheral_wmax, "monitor=0,bus=9,counter=5");
PMU_EVENT_ATTR_STRING(peripheral_rwait, bcm2711_peripheral_rwait, "monitor=0,bus=9,counter=6");
PMU_EVENT_ATTR_STRING(peripheral_rtrans, bcm2711_peripheral_rtrans, "monitor=0,bus=9,counter=7");
PMU_EVENT_ATTR_STRING(peripheral_rmax, bcm2711_peripheral_rmax, "monitor=0,bus=9,counter=8");
PMU_EVENT_ATTR_STRING(peripheral_rpend, bcm2711_peripheral_rpend, "monitor=0,bus=9,counter=9");
PMU_EVENT_ATTR_STRING(arm_uc_atwait, bcm2711_arm_uc_atwait, "monitor=0,bus=10,counter=0");
PMU_EVENT_ATTR_STRING(arm_uc_atrans, bcm2711_arm_uc_atrans, "monitor=0,bus=10,counter=1");
PMU_EVENT_ATTR_STRING(arm_uc_amax, bcm2711_arm_uc_amax, "monitor=0,bus=10,counter=2");
PMU_EVENT_ATTR_STRING(arm_uc_wwait, bcm2711_arm_uc_wwait, "monitor=0,bus=10,counter=3");
PMU_EVENT_ATTR_STRING(arm_uc_wtrans, bcm2711_arm_uc_wtrans, "monitor=0,bus=10,counter=4");
PMU_EVENT_ATTR_STRING(arm_uc_wmax, bcm2711_arm_uc_wmax, "monitor=0,bus=10,counter=5");
PMU_EVENT_ATTR_STRING(arm_uc_rwait, bcm2711_arm_uc_rwait, "monitor=0,bus=10,counter=6");
PMU_EVENT_ATTR_STRING(arm_uc_rtrans, bcm2711_arm_uc_rtrans, "monitor=0,bus=10,counter=7");
PMU_EVENT_ATTR_STRING(arm_uc_rmax, bcm2711_arm_uc_rmax, "monitor=0,bus=10,counter=8");
PMU_EVENT_ATTR_STRING(arm_uc_rpend, bcm2711_arm_uc_rpend, "monitor=0,bus=10,counter=9");
PMU_EVENT_ATTR_STRING(arm_l2_atwait, bcm2711_arm_l2_atwait, "monitor=0,bus=11,counter=0");
PMU_EVENT_ATTR_STRING(arm_l2_atrans, bcm2711_arm_l2_atrans, "monitor=0,bus=11,counter=1");
PMU_EVENT_ATTR_STRING(arm_l2_amax, bcm2711_arm_l2_amax, "monitor=0,bus=11,counter=2");
PMU_EVENT_ATTR_STRING(arm_l2_wwait, bcm2711_arm_l2_wwait, "monitor=0,bus=11,counter=3");
PMU_EVENT_ATTR_STRING(arm_l2_wtrans, bcm2711_arm_l2_wtrans, "monitor=0,bus=11,counter=4");
PMU_EVENT_ATTR_STRING(arm_l2_wmax, bcm2711_arm_l2_wmax, "monitor=0,bus=11,counter=5");
PMU_EVENT_ATTR_STRING(arm_l2_rwait, bcm2711_arm_l2_rwait, "monitor=0,bus=11,counter=6");
PMU_EVENT_ATTR_STRING(arm_l2_rtrans, bcm2711_arm_l2_rtrans, "monitor=0,bus=11,counter=7");
PMU_EVENT_ATTR_STRING(arm_l2_rmax, bcm2711_arm_l2_rmax, "monitor=0,bus=11,counter=8");
PMU_EVENT_ATTR_STRING(arm_l2_rpend, bcm2711_arm_l2_rpend, "monitor=0,bus=11,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_atwait, bcm2711_vpu_vpu1_d_l2_atwait, "monitor=1,bus=0,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_atrans, bcm2711_vpu_vpu1_d_l2_atrans, "monitor=1,bus=0,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_amax, bcm2711_vpu_vpu1_d_l2_amax, "monitor=1,bus=0,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_wwait, bcm2711_vpu_vpu1_d_l2_wwait, "monitor=1,bus=0,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_wtrans, bcm2711_vpu_vpu1_d_l2_wtrans, "monitor=1,bus=0,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_wmax, bcm2711_vpu_vpu1_d_l2_wmax, "monitor=1,bus=0,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rwait, bcm2711_vpu_vpu1_d_l2_rwait, "monitor=1,bus=0,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rtrans, bcm2711_vpu_vpu1_d_l2_rtrans, "monitor=1,bus=0,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rmax, bcm2711_vpu_vpu1_d_l2_rmax, "monitor=1,bus=0,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rpend, bcm2711_vpu_vpu1_d_l2_rpend, "monitor=1,bus=0,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_atwait, bcm2711_vpu_vpu0_d_l2_atwait, "monitor=1,bus=1,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_atrans, bcm2711_vpu_vpu0_d_l2_atrans, "monitor=1,bus=1,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_amax, bcm2711_vpu_vpu0_d_l2_amax, "monitor=1,bus=1,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_wwait, bcm2711_vpu_vpu0_d_l2_wwait, "monitor=1,bus=1,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_wtrans, bcm2711_vpu_vpu0_d_l2_wtrans, "monitor=1,bus=1,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_wmax, bcm2711_vpu_vpu0_d_l2_wmax, "monitor=1,bus=1,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rwait, bcm2711_vpu_vpu0_d_l2_rwait, "monitor=1,bus=1,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rtrans, bcm2711_vpu_vpu0_d_l2_rtrans, "monitor=1,bus=1,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rmax, bcm2711_vpu_vpu0_d_l2_rmax, "monitor=1,bus=1,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rpend, bcm2711_vpu_vpu0_d_l2_rpend, "monitor=1,bus=1,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_atwait, bcm2711_vpu_vpu1_i_l2_atwait, "monitor=1,bus=2,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_atrans, bcm2711_vpu_vpu1_i_l2_atrans, "monitor=1,bus=2,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_amax, bcm2711_vpu_vpu1_i_l2_amax, "monitor=1,bus=2,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_wwait, bcm2711_vpu_vpu1_i_l2_wwait, "monitor=1,bus=2,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_wtrans, bcm2711_vpu_vpu1_i_l2_wtrans, "monitor=1,bus=2,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_wmax, bcm2711_vpu_vpu1_i_l2_wmax, "monitor=1,bus=2,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rwait, bcm2711_vpu_vpu1_i_l2_rwait, "monitor=1,bus=2,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rtrans, bcm2711_vpu_vpu1_i_l2_rtrans, "monitor=1,bus=2,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rmax, bcm2711_vpu_vpu1_i_l2_rmax, "monitor=1,bus=2,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rpend, bcm2711_vpu_vpu1_i_l2_rpend, "monitor=1,bus=2,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_atwait, bcm2711_vpu_vpu0_i_l2_atwait, "monitor=1,bus=3,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_atrans, bcm2711_vpu_vpu0_i_l2_atrans, "monitor=1,bus=3,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_amax, bcm2711_vpu_vpu0_i_l2_amax, "monitor=1,bus=3,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_wwait, bcm2711_vpu_vpu0_i_l2_wwait, "monitor=1,bus=3,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_wtrans, bcm2711_vpu_vpu0_i_l2_wtrans, "monitor=1,bus=3,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_wmax, bcm2711_vpu_vpu0_i_l2_wmax, "monitor=1,bus=3,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rwait, bcm2711_vpu_vpu0_i_l2_rwait, "monitor=1,bus=3,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rtrans, bcm2711_vpu_vpu0_i_l2_rtrans, "monitor=1,bus=3,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rmax, bcm2711_vpu_vpu0_i_l2_rmax, "monitor=1,bus=3,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rpend, bcm2711_vpu_vpu0_i_l2_rpend, "monitor=1,bus=3,counter=9");
PMU_EVENT_ATTR_STRING(vpu_system_l2_atwait, bcm2711_vpu_system_l2_atwait, "monitor=1,bus=4,counter=0");
PMU_EVENT_ATTR_STRING(vpu_system_l2_atrans, bcm2711_vpu_system_l2_atrans, "monitor=1,bus=4,counter=1");
PMU_EVENT_ATTR_STRING(vpu_system_l2_amax, bcm2711_vpu_system_l2_amax, "monitor=1,bus=4,counter=2");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wwait, bcm2711_vpu_system_l2_wwait, "monitor=1,bus=4,counter=3");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wtrans, bcm2711_vpu_system_l2_wtrans, "monitor=1,bus=4,counter=4");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wmax, bcm2711_vpu_system_l2_wmax, "monitor=1,bus=4,counter=5");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rwait, bcm2711_vpu_system_l2_rwait, "monitor=1,bus=4,counter=6");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rtrans, bcm2711_vpu_system_l2_rtrans, "monitor=1,bus=4,counter=7");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rmax, bcm2711_vpu_system_l2_rmax, "monitor=1,bus=4,counter=8");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rpend, bcm2711_vpu_system_l2_rpend, "monitor=1,bus=4,counter=9");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_atwait, bcm2711_vpu_dma_l2_atwait, "monitor=1,bus=5,counter=0");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_atrans, bcm2711_vpu_dma_l2_atrans, "monitor=1,bus=5,counter=1");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_amax, bcm2711_vpu_dma_l2_amax, "monitor=1,bus=5,counter=2");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_wwait, bcm2711_vpu_dma_l2_wwait, "monitor=1,bus=5,counter=3");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_wtrans, bcm2711_vpu_dma_l2_wtrans, "monitor=1,bus=5,counter=4");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_wmax, bcm2711_vpu_dma_l2_wmax, "monitor=1,bus=5,counter=5");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rwait, bcm2711_vpu_dma_l2_rwait, "monitor=1,bus=5,counter=6");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rtrans, bcm2711_vpu_dma_l2_rtrans, "monitor=1,bus=5,counter=7");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rmax, bcm2711_vpu_dma_l2_rmax, "monitor=1,bus=5,counter=8");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rpend, bcm2711_vpu_dma_l2_rpend, "monitor=1,bus=5,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_atwait, bcm2711_vpu_vpu1_d_uc_atwait, "monitor=1,bus=6,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_atrans, bcm2711_vpu_vpu1_d_uc_atrans, "monitor=1,bus=6,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_amax, bcm2711_vpu_vpu1_d_uc_amax, "monitor=1,bus=6,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_wwait, bcm2711_vpu_vpu1_d_uc_wwait, "monitor=1,bus=6,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_wtrans, bcm2711_vpu_vpu1_d_uc_wtrans, "monitor=1,bus=6,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_wmax, bcm2711_vpu_vpu1_d_uc_wmax, "monitor=1,bus=6,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rwait, bcm2711_vpu_vpu1_d_uc_rwait, "monitor=1,bus=6,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rtrans, bcm2711_vpu_vpu1_d_uc_rtrans, "monitor=1,bus=6,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rmax, bcm2711_vpu_vpu1_d_uc_rmax, "monitor=1,bus=6,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rpend, bcm2711_vpu_vpu1_d_uc_rpend, "monitor=1,bus=6,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_atwait, bcm2711_vpu_vpu0_d_uc_atwait, "monitor=1,bus=7,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_atrans, bcm2711_vpu_vpu0_d_uc_atrans, "monitor=1,bus=7,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_amax, bcm2711_vpu_vpu0_d_uc_amax, "monitor=1,bus=7,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_wwait, bcm2711_vpu_vpu0_d_uc_wwait, "monitor=1,bus=7,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_wtrans, bcm2711_vpu_vpu0_d_uc_wtrans, "monitor=1,bus=7,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_wmax, bcm2711_vpu_vpu0_d_uc_wmax, "monitor=1,bus=7,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rwait, bcm2711_vpu_vpu0_d_uc_rwait, "monitor=1,bus=7,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rtrans, bcm2711_vpu_vpu0_d_uc_rtrans, "monitor=1,bus=7,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rmax, bcm2711_vpu_vpu0_d_uc_rmax, "monitor=1,bus=7,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rpend, bcm2711_vpu_vpu0_d_uc_rpend, "monitor=1,bus=7,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_atwait, bcm2711_vpu_vpu1_i_uc_atwait, "monitor=1,bus=8,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_atrans, bcm2711_vpu_vpu1_i_uc_atrans, "monitor=1,bus=8,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_amax, bcm2711_vpu_vpu1_i_uc_amax, "monitor=1,bus=8,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_wwait, bcm2711_vpu_vpu1_i_uc_wwait, "monitor=1,bus=8,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_wtrans, bcm2711_vpu_vpu1_i_uc_wtrans, "monitor=1,bus=8,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_wmax, bcm2711_vpu_vpu1_i_uc_wmax, "monitor=1,bus=8,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rwait, bcm2711_vpu_vpu1_i_uc_rwait, "monitor=1,bus=8,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rtrans, bcm2711_vpu_vpu1_i_uc_rtrans, "monitor=1,bus=8,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rmax, bcm2711_vpu_vpu1_i_uc_rmax, "monitor=1,bus=8,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rpend, bcm2711_vpu_vpu1_i_uc_rpend, "monitor=1,bus=8,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_atwait, bcm2711_vpu_vpu0_i_uc_atwait, "monitor=1,bus=9,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_atrans, bcm2711_vpu_vpu0_i_uc_atrans, "monitor=1,bus=9,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_amax, bcm2711_vpu_vpu0_i_uc_amax, "monitor=1,bus=9,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_wwait, bcm2711_vpu_vpu0_i_uc_wwait, "monitor=1,bus=9,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_wtrans, bcm2711_vpu_vpu0_i_uc_wtrans, "monitor=1,bus=9,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_wmax, bcm2711_vpu_vpu0_i_uc_wmax, "monitor=1,bus=9,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rwait, bcm2711_vpu_vpu0_i_uc_rwait, "monitor=1,bus=9,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rtrans, bcm2711_vpu_vpu0_i_uc_rtrans, "monitor=1,bus=9,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rmax, bcm2711_vpu_vpu0_i_uc_rmax, "monitor=1,bus=9,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rpend, bcm2711_vpu_vpu0_i_uc_rpend, "monitor=1,bus=9,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_atwait, bcm2711_vpu_vpu_uc_atwait, "monitor=1,bus=10,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_atrans, bcm2711_vpu_vpu_uc_atrans, "monitor=1,bus=10,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_amax, bcm2711_vpu_vpu_uc_amax, "monitor=1,bus=10,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_wwait, bcm2711_vpu_vpu_uc_wwait, "monitor=1,bus=10,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_wtrans, bcm2711_vpu_vpu_uc_wtrans, "monitor=1,bus=10,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_wmax, bcm2711_vpu_vpu_uc_wmax, "monitor=1,bus=10,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rwait, bcm2711_vpu_vpu_uc_rwait, "monitor=1,bus=10,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rtrans, bcm2711_vpu_vpu_uc_rtrans, "monitor=1,bus=10,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rmax, bcm2711_vpu_vpu_uc_rmax, "monitor=1,bus=10,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rpend, bcm2711_vpu_vpu_uc_rpend, "monitor=1,bus=10,counter=9");
PMU_EVENT_ATTR_STRING(vpu_l2_out_atwait, bcm2711_vpu_l2_out_atwait, "monitor=1,bus=11,counter=0");
PMU_EVENT_ATTR_STRING(vpu_l2_out_atrans, bcm2711_vpu_l2_out_atrans, "monitor=1,bus=11,counter=1");
PMU_EVENT_ATTR_STRING(vpu_l2_out_amax, bcm2711_vpu_l2_out_amax, "monitor=1,bus=11,counter=2");
PMU_EVENT_ATTR_STRING(vpu_l2_out_wwait, bcm2711_vpu_l2_out_wwait, "monitor=1,bus=11,counter=3");
PMU_EVENT_ATTR_STRING(vpu_l2_out_wtrans, bcm2711_vpu_l2_out_wtrans, "monitor=1,bus=11,counter=4");
PMU_EVENT_ATTR_STRING(vpu_l2_out_wmax, bcm2711_vpu_l2_out_wmax, "monitor=1,bus=11,counter=5");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rwait, bcm2711_vpu_l2_out_rwait, "monitor=1,bus=11,counter=6");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rtrans, bcm2711_vpu_l2_out_rtrans, "monitor=1,bus=11,counter=7");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rmax, bcm2711_vpu_l2_out_rmax, "monitor=1,bus=11,counter=8");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rpend, bcm2711_vpu_l2_out_rpend, "monitor=1,bus=11,counter=9");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_atwait, bcm2711_vpu_dma_uc_atwait, "monitor=1,bus=12,counter=0");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_atrans, bcm2711_vpu_dma_uc_atrans, "monitor=1,bus=12,counter=1");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_amax, bcm2711_vpu_dma_uc_amax, "monitor=1,bus=12,counter=2");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_wwait, bcm2711_vpu_dma_uc_wwait, "monitor=1,bus=12,counter=3");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_wtrans, bcm2711_vpu_dma_uc_wtrans, "monitor=1,bus=12,counter=4");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_wmax, bcm2711_vpu_dma_uc_wmax, "monitor=1,bus=12,counter=5");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rwait, bcm2711_vpu_dma_uc_rwait, "monitor=1,bus=12,counter=6");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rtrans, bcm2711_vpu_dma_uc_rtrans, "monitor=1,bus=12,counter=7");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rmax, bcm2711_vpu_dma_uc_rmax, "monitor=1,bus=12,counter=8");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rpend, bcm2711_vpu_dma_uc_rpend, "monitor=1,bus=12,counter=9");
PMU_EVENT_ATTR_STRING(vpu_l2_in_atwait, bcm2711_vpu_l2_in_atwait, "monitor=1,bus=13,counter=0");
PMU_EVENT_ATTR_STRING(vpu_l2_in_atrans, bcm2711_vpu_l2_in_atrans, "monitor=1,bus=13,counter=1");
PMU_EVENT_ATTR_STRING(vpu_l2_in_amax, bcm2711_vpu_l2_in_amax, "monitor=1,bus=13,counter=2");
PMU_EVENT_ATTR_STRING(vpu_l2_in_wwait, bcm2711_vpu_l2_in_wwait, "monitor=1,bus=13,counter=3");
PMU_EVENT_ATTR_STRING(vpu_l2_in_wtrans, bcm2711_vpu_l2_in_wtrans, "monitor=1,bus=13,counter=4");
PMU_EVENT_ATTR_STRING(vpu_l2_in_wmax, bcm2711_vpu_l2_in_wmax, "monitor=1,bus=13,counter=5");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rwait, bcm2711_vpu_l2_in_rwait, "monitor=1,bus=13,counter=6");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rtrans, bcm2711_vpu_l2_in_rtrans, "monitor=1,bus=13,counter=7");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rmax, bcm2711_vpu_l2_in_rmax, "monitor=1,bus=13,counter=8");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rpend, bcm2711_vpu_l2_in_rpend, "monitor=1,bus=13,counter=9");

static struct attribute *bcm2711_events[] = {
	&bcm2711_dma_l2_atwait.attr.attr,
	&bcm2711_dma_l2_atrans.attr.attr,
	&bcm2711_dma_l2_amax.attr.attr,
	&bcm2711_dma_l2_wwait.attr.attr,
	&bcm2711_dma_l2_wtrans.attr.attr,
	&bcm2711_dma_l2_wmax.attr.attr,
	&bcm2711_dma_l2_rwait.attr.attr,
	&bcm2711_dma_l2_rtrans.attr.attr,
	&bcm2711_dma_l2_rmax.attr.attr,
	&bcm2711_dma_l2_rpend.attr.attr,
	&bcm2711_trans_atwait.attr.attr,
	&bcm2711_trans_atrans.attr.attr,
	&bcm2711_trans_amax.attr.attr,
	&bcm2711_trans_wwait.attr.attr,
	&bcm2711_trans_wtrans.attr.attr,
	&bcm2711_trans_wmax.attr.attr,
	&bcm2711_trans_rwait.attr.attr,
	&bcm2711_trans_rtrans.attr.attr,
	&bcm2711_trans_rmax.attr.attr,
	&bcm2711_trans_rpend.attr.attr,
	&bcm2711_jpeg_atwait.attr.attr,
	&bcm2711_jpeg_atrans.attr.attr,
	&bcm2711_jpeg_amax.attr.attr,
	&bcm2711_jpeg_wwait.attr.attr,
	&bcm2711_jpeg_wtrans.attr.attr,
	&bcm2711_jpeg_wmax.attr.attr,
	&bcm2711_jpeg_rwait.attr.attr,
	&bcm2711_jpeg_rtrans.attr.attr,
	&bcm2711_jpeg_rmax.attr.attr,
	&bcm2711_jpeg_rpend.attr.attr,
	&bcm2711_vpu_uc_atwait.attr.attr,
	&bcm2711_vpu_uc_atrans.attr.attr,
	&bcm2711_vpu_uc_amax.attr.attr,
	&bcm2711_vpu_uc_wwait.attr.attr,
	&bcm2711_vpu_uc_wtrans.attr.attr,
	&bcm2711_vpu_uc_wmax.attr.attr,
	&bcm2711_vpu_uc_rwait.attr.attr,
	&bcm2711_vpu_uc_rtrans.attr.attr,
	&bcm2711_vpu_uc_rmax.attr.attr,
	&bcm2711_vpu_uc_rpend.attr.attr,
	&bcm2711_dma_uc_atwait.attr.attr,
	&bcm2711_dma_uc_atrans.attr.attr,
	&bcm2711_dma_uc_amax.attr.attr,
	&bcm2711_dma_uc_wwait.attr.attr,
	&bcm2711_dma_uc_wtrans.attr.attr,
	&bcm2711_dma_uc_wmax.attr.attr,
	&bcm2711_dma_uc_rwait.attr.attr,
	&bcm2711_dma_uc_rtrans.attr.attr,
	&bcm2711_dma_uc_rmax.attr.attr,
	&bcm2711_dma_uc_rpend.attr.attr,
	&bcm2711_system_l2_atwait.attr.attr,
	&bcm2711_system_l2_atrans.attr.attr,
	&bcm2711_system_l2_amax.attr.attr,
	&bcm2711_system_l2_wwait.attr.attr,
	&bcm2711_system_l2_wtrans.attr.attr,
	&bcm2711_system_l2_wmax.attr.attr,
	&bcm2711_system_l2_rwait.attr.attr,
	&bcm2711_system_l2_rtrans.attr.attr,
	&bcm2711_system_l2_rmax.attr.attr,
	&bcm2711_system_l2_rpend.attr.attr,
	&bcm2711_hvs_atwait.attr.attr,
	&bcm2711_hvs_atrans.attr.attr,
	&bcm2711_hvs_amax.attr.attr,
	&bcm2711_hvs_wwait.attr.attr,
	&bcm2711_hvs_wtrans.attr.attr,
	&bcm2711_hvs_wmax.attr.attr,
	&bcm2711_hvs_rwait.attr.attr,
	&bcm2711_hvs_rtrans.attr.attr,
	&bcm2711_hvs_rmax.attr.attr,
	&bcm2711_hvs_rpend.attr.attr,
	&bcm2711_argon_atwait.attr.attr,
	&bcm2711_argon_atrans.attr.attr,
	&bcm2711_argon_amax.attr.attr,
	&bcm2711_argon_wwait.attr.attr,
	&bcm2711_argon_wtrans.attr.attr,
	&bcm2711_argon_wmax.attr.attr,
	&bcm2711_argon_rwait.attr.attr,
	&bcm2711_argon_rtrans.attr.attr,
	&bcm2711_argon_rmax.attr.attr,
	&bcm2711_argon_rpend.attr.attr,
	&bcm2711_h264_atwait.attr.attr,
	&bcm2711_h264_atrans.attr.attr,
	&bcm2711_h264_amax.attr.attr,
	&bcm2711_h264_wwait.attr.attr,
	&bcm2711_h264_wtrans.attr.attr,
	&bcm2711_h264_wmax.attr.attr,
	&bcm2711_h264_rwait.attr.attr,
	&bcm2711_h264_rtrans.attr.attr,
	&bcm2711_h264_rmax.attr.attr,
	&bcm2711_h264_rpend.attr.attr,
	&bcm2711_peripheral_atwait.attr.attr,
	&bcm2711_peripheral_atrans.attr.attr,
	&bcm2711_peripheral_amax.attr.attr,
	&bcm2711_peripheral_wwait.attr.attr,
	&bcm2711_peripheral_wtrans.attr.attr,
	&bcm2711_peripheral_wmax.attr.attr,
	&bcm2711_peripheral_rwait.attr.attr,
	&bcm2711_peripheral_rtrans.attr.attr,
	&bcm2711_peripheral_rmax.attr.attr,
	&bcm2711_peripheral_rpend.attr.attr,
	&bcm2711_arm_uc_atwait.attr.attr,
	&bcm2711_arm_uc_atrans.attr.attr,
	&bcm2711_arm_uc_amax.attr.attr,
	&bcm2711_arm_uc_wwait.attr.attr,
	&bcm2711_arm_uc_wtrans.attr.attr,
	&bcm2711_arm_uc_wmax.attr.attr,
	&bcm2711_arm_uc_rwait.attr.attr,
	&bcm2711_arm_uc_rtrans.attr.attr,
	&bcm2711_arm_uc_rmax.attr.attr,
	&bcm2711_arm_uc_rpend.attr.attr,
	&bcm2711_arm_l2_atwait.attr.attr,
	&bcm2711_arm_l2_atrans.attr.attr,
	&bcm2711_arm_l2_amax.attr.attr,
	&bcm2711_arm_l2_wwait.attr.attr,
	&bcm2711_arm_l2_wtrans.attr.attr,
	&bcm2711_arm_l2_wmax.attr.attr,
	&bcm2711_arm_l2_rwait.attr.attr,
	&bcm2711_arm_l2_rtrans.attr.attr,
	&bcm2711_arm_l2_rmax.attr.attr,
	&bcm2711_arm_l2_rpend.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_atwait.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_atrans.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_amax.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_wwait.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_wtrans.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_wmax.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_rwait.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_rtrans.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_rmax.attr.attr,
	&bcm2711_vpu_vpu1_d_l2_rpend.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_atwait.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_atrans.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_amax.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_wwait.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_wtrans.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_wmax.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_rwait.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_rtrans.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_rmax.attr.attr,
	&bcm2711_vpu_vpu0_d_l2_rpend.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_atwait.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_atrans.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_amax.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_wwait.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_wtrans.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_wmax.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_rwait.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_rtrans.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_rmax.attr.attr,
	&bcm2711_vpu_vpu1_i_l2_rpend.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_atwait.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_atrans.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_amax.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_wwait.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_wtrans.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_wmax.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_rwait.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_rtrans.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_rmax.attr.attr,
	&bcm2711_vpu_vpu0_i_l2_rpend.attr.attr,
	&bcm2711_vpu_system_l2_atwait.attr.attr,
	&bcm2711_vpu_system_l2_atrans.attr.attr,
	&bcm2711_vpu_system_l2_amax.attr.attr,
	&bcm2711_vpu_system_l2_wwait.attr.attr,
	&bcm2711_vpu_system_l2_wtrans.attr.attr,
	&bcm2711_vpu_system_l2_wmax.attr.attr,
	&bcm2711_vpu_system_l2_rwait.attr.attr,
	&bcm2711_vpu_system_l2_rtrans.attr.attr,
	&bcm2711_vpu_system_l2_rmax.attr.attr,
	&bcm2711_vpu_system_l2_rpend.attr.attr,
	&bcm2711_vpu_dma_l2_atwait.attr.attr,
	&bcm2711_vpu_dma_l2_atrans.attr.attr,
	&bcm2711_vpu_dma_l2_amax.attr.attr,
	&bcm2711_vpu_dma_l2_wwait.attr.attr,
	&bcm2711_vpu_dma_l2_wtrans.attr.attr,
	&bcm2711_vpu_dma_l2_wmax.attr.attr,
	&bcm2711_vpu_dma_l2_rwait.attr.attr,
	&bcm2711_vpu_dma_l2_rtrans.attr.attr,
	&bcm2711_vpu_dma_l2_rmax.attr.attr,
	&bcm2711_vpu_dma_l2_rpend.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_atwait.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_atrans.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_amax.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_wwait.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_wtrans.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_wmax.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_rwait.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_rtrans.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_rmax.attr.attr,
	&bcm2711_vpu_vpu1_d_uc_rpend.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_atwait.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_atrans.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_amax.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_wwait.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_wtrans.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_wmax.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_rwait.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_rtrans.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_rmax.attr.attr,
	&bcm2711_vpu_vpu0_d_uc_rpend.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_atwait.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_atrans.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_amax.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_wwait.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_wtrans.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_wmax.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_rwait.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_rtrans.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_rmax.attr.attr,
	&bcm2711_vpu_vpu1_i_uc_rpend.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_atwait.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_atrans.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_amax.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_wwait.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_wtrans.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_wmax.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_rwait.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_rtrans.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_rmax.attr.attr,
	&bcm2711_vpu_vpu0_i_uc_rpend.attr.attr,
	&bcm2711_vpu_vpu_uc_atwait.attr.attr,
	&bcm2711_vpu_vpu_uc_atrans.attr.attr,
	&bcm2711_vpu_vpu_uc_amax.attr.attr,
	&bcm2711_vpu_vpu_uc_wwait.attr.attr,
	&bcm2711_vpu_vpu_uc_wtrans.attr.attr,
	&bcm2711_vpu_vpu_uc_wmax.attr.attr,
	&bcm2711_vpu_vpu_uc_rwait.attr.attr,
	&bcm2711_vpu_vpu_uc_rtrans.attr.attr,
	&bcm2711_vpu_vpu_uc_rmax.attr.attr,
	&bcm2711_vpu_vpu_uc_rpend.attr.attr,
	&bcm2711_vpu_l2_out_atwait.attr.attr,
	&bcm2711_vpu_l2_out_atrans.attr.attr,
	&bcm2711_vpu_l2_out_amax.attr.attr,
	&bcm2711_vpu_l2_out_wwait.attr.attr,
	&bcm2711_vpu_l2_out_wtrans.attr.attr,
	&bcm2711_vpu_l2_out_wmax.attr.attr,
	&bcm2711_vpu_l2_out_rwait.attr.attr,
	&bcm2711_vpu_l2_out_rtrans.attr.attr,
	&bcm2711_vpu_l2_out_rmax.attr.attr,
	&bcm2711_vpu_l2_out_rpend.attr.attr,
	&bcm2711_vpu_dma_uc_atwait.attr.attr,
	&bcm2711_vpu_dma_uc_atrans.attr.attr,
	&bcm2711_vpu_dma_uc_amax.attr.attr,
	&bcm2711_vpu_dma_uc_wwait.attr.attr,
	&bcm2711_vpu_dma_uc_wtrans.attr.attr,
	&bcm2711_vpu_dma_uc_wmax.attr.attr,
	&bcm2711_vpu_dma_uc_rwait.attr.attr,
	&bcm2711_vpu_dma_uc_rtrans.attr.attr,
	&bcm2711_vpu_dma_uc_rmax.attr.attr,
	&bcm2711_vpu_dma_uc_rpend.attr.attr,
	&bcm2711_vpu_l2_in_atwait.attr.attr,
	&bcm2711_vpu_l2_in_atrans.attr.attr,
	&bcm2711_vpu_l2_in_amax.attr.attr,
	&bcm2711_vpu_l2_in_wwait.attr.attr,
	&bcm2711_vpu_l2_in_wtrans.attr.attr,
	&bcm2711_vpu_l2_in_wmax.attr.attr,
	&bcm2711_vpu_l2_in_rwait.attr.attr,
	&bcm2711_vpu_l2_in_rtrans.attr.attr,
	&bcm2711_vpu_l2_in_rmax.attr.attr,
	&bcm2711_vpu_l2_in_rpend.attr.attr,
	NULL,
};

static const struct attribute_group rpi_axi_pmu_bcm2711_events_group = {
	.name = "events",
	.attrs = bcm2711_events,
	.is_visible = rpi_axi_pmu_events_is_visible,
};

PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_atwait, bcm2712_vpu_vpu1_d_l2_atwait, "monitor=1,bus=0,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_atrans, bcm2712_vpu_vpu1_d_l2_atrans, "monitor=1,bus=0,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_amax, bcm2712_vpu_vpu1_d_l2_amax, "monitor=1,bus=0,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_wwait, bcm2712_vpu_vpu1_d_l2_wwait, "monitor=1,bus=0,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_wtrans, bcm2712_vpu_vpu1_d_l2_wtrans, "monitor=1,bus=0,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_wmax, bcm2712_vpu_vpu1_d_l2_wmax, "monitor=1,bus=0,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rwait, bcm2712_vpu_vpu1_d_l2_rwait, "monitor=1,bus=0,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rtrans, bcm2712_vpu_vpu1_d_l2_rtrans, "monitor=1,bus=0,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rmax, bcm2712_vpu_vpu1_d_l2_rmax, "monitor=1,bus=0,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_rpend, bcm2712_vpu_vpu1_d_l2_rpend, "monitor=1,bus=0,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_l2_ratrans, bcm2712_vpu_vpu1_d_l2_ratrans, "monitor=1,bus=0,counter=10");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_atwait, bcm2712_vpu_vpu0_d_l2_atwait, "monitor=1,bus=1,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_atrans, bcm2712_vpu_vpu0_d_l2_atrans, "monitor=1,bus=1,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_amax, bcm2712_vpu_vpu0_d_l2_amax, "monitor=1,bus=1,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_wwait, bcm2712_vpu_vpu0_d_l2_wwait, "monitor=1,bus=1,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_wtrans, bcm2712_vpu_vpu0_d_l2_wtrans, "monitor=1,bus=1,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_wmax, bcm2712_vpu_vpu0_d_l2_wmax, "monitor=1,bus=1,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rwait, bcm2712_vpu_vpu0_d_l2_rwait, "monitor=1,bus=1,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rtrans, bcm2712_vpu_vpu0_d_l2_rtrans, "monitor=1,bus=1,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rmax, bcm2712_vpu_vpu0_d_l2_rmax, "monitor=1,bus=1,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_rpend, bcm2712_vpu_vpu0_d_l2_rpend, "monitor=1,bus=1,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_l2_ratrans, bcm2712_vpu_vpu0_d_l2_ratrans, "monitor=1,bus=1,counter=10");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_atwait, bcm2712_vpu_vpu1_i_l2_atwait, "monitor=1,bus=2,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_atrans, bcm2712_vpu_vpu1_i_l2_atrans, "monitor=1,bus=2,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_amax, bcm2712_vpu_vpu1_i_l2_amax, "monitor=1,bus=2,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_wwait, bcm2712_vpu_vpu1_i_l2_wwait, "monitor=1,bus=2,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_wtrans, bcm2712_vpu_vpu1_i_l2_wtrans, "monitor=1,bus=2,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_wmax, bcm2712_vpu_vpu1_i_l2_wmax, "monitor=1,bus=2,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rwait, bcm2712_vpu_vpu1_i_l2_rwait, "monitor=1,bus=2,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rtrans, bcm2712_vpu_vpu1_i_l2_rtrans, "monitor=1,bus=2,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rmax, bcm2712_vpu_vpu1_i_l2_rmax, "monitor=1,bus=2,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_rpend, bcm2712_vpu_vpu1_i_l2_rpend, "monitor=1,bus=2,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_l2_ratrans, bcm2712_vpu_vpu1_i_l2_ratrans, "monitor=1,bus=2,counter=10");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_atwait, bcm2712_vpu_vpu0_i_l2_atwait, "monitor=1,bus=3,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_atrans, bcm2712_vpu_vpu0_i_l2_atrans, "monitor=1,bus=3,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_amax, bcm2712_vpu_vpu0_i_l2_amax, "monitor=1,bus=3,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_wwait, bcm2712_vpu_vpu0_i_l2_wwait, "monitor=1,bus=3,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_wtrans, bcm2712_vpu_vpu0_i_l2_wtrans, "monitor=1,bus=3,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_wmax, bcm2712_vpu_vpu0_i_l2_wmax, "monitor=1,bus=3,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rwait, bcm2712_vpu_vpu0_i_l2_rwait, "monitor=1,bus=3,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rtrans, bcm2712_vpu_vpu0_i_l2_rtrans, "monitor=1,bus=3,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rmax, bcm2712_vpu_vpu0_i_l2_rmax, "monitor=1,bus=3,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_rpend, bcm2712_vpu_vpu0_i_l2_rpend, "monitor=1,bus=3,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_l2_ratrans, bcm2712_vpu_vpu0_i_l2_ratrans, "monitor=1,bus=3,counter=10");
PMU_EVENT_ATTR_STRING(vpu_system_l2_atwait, bcm2712_vpu_system_l2_atwait, "monitor=1,bus=4,counter=0");
PMU_EVENT_ATTR_STRING(vpu_system_l2_atrans, bcm2712_vpu_system_l2_atrans, "monitor=1,bus=4,counter=1");
PMU_EVENT_ATTR_STRING(vpu_system_l2_amax, bcm2712_vpu_system_l2_amax, "monitor=1,bus=4,counter=2");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wwait, bcm2712_vpu_system_l2_wwait, "monitor=1,bus=4,counter=3");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wtrans, bcm2712_vpu_system_l2_wtrans, "monitor=1,bus=4,counter=4");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wmax, bcm2712_vpu_system_l2_wmax, "monitor=1,bus=4,counter=5");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rwait, bcm2712_vpu_system_l2_rwait, "monitor=1,bus=4,counter=6");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rtrans, bcm2712_vpu_system_l2_rtrans, "monitor=1,bus=4,counter=7");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rmax, bcm2712_vpu_system_l2_rmax, "monitor=1,bus=4,counter=8");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rpend, bcm2712_vpu_system_l2_rpend, "monitor=1,bus=4,counter=9");
PMU_EVENT_ATTR_STRING(vpu_system_l2_ratrans, bcm2712_vpu_system_l2_ratrans, "monitor=1,bus=4,counter=10");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_atwait, bcm2712_vpu_dma_l2_atwait, "monitor=1,bus=5,counter=0");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_atrans, bcm2712_vpu_dma_l2_atrans, "monitor=1,bus=5,counter=1");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_amax, bcm2712_vpu_dma_l2_amax, "monitor=1,bus=5,counter=2");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_wwait, bcm2712_vpu_dma_l2_wwait, "monitor=1,bus=5,counter=3");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_wtrans, bcm2712_vpu_dma_l2_wtrans, "monitor=1,bus=5,counter=4");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_wmax, bcm2712_vpu_dma_l2_wmax, "monitor=1,bus=5,counter=5");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rwait, bcm2712_vpu_dma_l2_rwait, "monitor=1,bus=5,counter=6");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rtrans, bcm2712_vpu_dma_l2_rtrans, "monitor=1,bus=5,counter=7");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rmax, bcm2712_vpu_dma_l2_rmax, "monitor=1,bus=5,counter=8");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_rpend, bcm2712_vpu_dma_l2_rpend, "monitor=1,bus=5,counter=9");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_ratrans, bcm2712_vpu_dma_l2_ratrans, "monitor=1,bus=5,counter=10");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_atwait, bcm2712_vpu_vpu1_d_uc_atwait, "monitor=1,bus=6,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_atrans, bcm2712_vpu_vpu1_d_uc_atrans, "monitor=1,bus=6,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_amax, bcm2712_vpu_vpu1_d_uc_amax, "monitor=1,bus=6,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_wwait, bcm2712_vpu_vpu1_d_uc_wwait, "monitor=1,bus=6,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_wtrans, bcm2712_vpu_vpu1_d_uc_wtrans, "monitor=1,bus=6,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_wmax, bcm2712_vpu_vpu1_d_uc_wmax, "monitor=1,bus=6,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rwait, bcm2712_vpu_vpu1_d_uc_rwait, "monitor=1,bus=6,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rtrans, bcm2712_vpu_vpu1_d_uc_rtrans, "monitor=1,bus=6,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rmax, bcm2712_vpu_vpu1_d_uc_rmax, "monitor=1,bus=6,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_rpend, bcm2712_vpu_vpu1_d_uc_rpend, "monitor=1,bus=6,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_d_uc_ratrans, bcm2712_vpu_vpu1_d_uc_ratrans, "monitor=1,bus=6,counter=10");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_atwait, bcm2712_vpu_vpu0_d_uc_atwait, "monitor=1,bus=7,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_atrans, bcm2712_vpu_vpu0_d_uc_atrans, "monitor=1,bus=7,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_amax, bcm2712_vpu_vpu0_d_uc_amax, "monitor=1,bus=7,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_wwait, bcm2712_vpu_vpu0_d_uc_wwait, "monitor=1,bus=7,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_wtrans, bcm2712_vpu_vpu0_d_uc_wtrans, "monitor=1,bus=7,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_wmax, bcm2712_vpu_vpu0_d_uc_wmax, "monitor=1,bus=7,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rwait, bcm2712_vpu_vpu0_d_uc_rwait, "monitor=1,bus=7,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rtrans, bcm2712_vpu_vpu0_d_uc_rtrans, "monitor=1,bus=7,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rmax, bcm2712_vpu_vpu0_d_uc_rmax, "monitor=1,bus=7,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_rpend, bcm2712_vpu_vpu0_d_uc_rpend, "monitor=1,bus=7,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_d_uc_ratrans, bcm2712_vpu_vpu0_d_uc_ratrans, "monitor=1,bus=7,counter=10");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_atwait, bcm2712_vpu_vpu1_i_uc_atwait, "monitor=1,bus=8,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_atrans, bcm2712_vpu_vpu1_i_uc_atrans, "monitor=1,bus=8,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_amax, bcm2712_vpu_vpu1_i_uc_amax, "monitor=1,bus=8,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_wwait, bcm2712_vpu_vpu1_i_uc_wwait, "monitor=1,bus=8,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_wtrans, bcm2712_vpu_vpu1_i_uc_wtrans, "monitor=1,bus=8,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_wmax, bcm2712_vpu_vpu1_i_uc_wmax, "monitor=1,bus=8,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rwait, bcm2712_vpu_vpu1_i_uc_rwait, "monitor=1,bus=8,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rtrans, bcm2712_vpu_vpu1_i_uc_rtrans, "monitor=1,bus=8,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rmax, bcm2712_vpu_vpu1_i_uc_rmax, "monitor=1,bus=8,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_rpend, bcm2712_vpu_vpu1_i_uc_rpend, "monitor=1,bus=8,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu1_i_uc_ratrans, bcm2712_vpu_vpu1_i_uc_ratrans, "monitor=1,bus=8,counter=10");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_atwait, bcm2712_vpu_vpu0_i_uc_atwait, "monitor=1,bus=9,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_atrans, bcm2712_vpu_vpu0_i_uc_atrans, "monitor=1,bus=9,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_amax, bcm2712_vpu_vpu0_i_uc_amax, "monitor=1,bus=9,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_wwait, bcm2712_vpu_vpu0_i_uc_wwait, "monitor=1,bus=9,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_wtrans, bcm2712_vpu_vpu0_i_uc_wtrans, "monitor=1,bus=9,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_wmax, bcm2712_vpu_vpu0_i_uc_wmax, "monitor=1,bus=9,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rwait, bcm2712_vpu_vpu0_i_uc_rwait, "monitor=1,bus=9,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rtrans, bcm2712_vpu_vpu0_i_uc_rtrans, "monitor=1,bus=9,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rmax, bcm2712_vpu_vpu0_i_uc_rmax, "monitor=1,bus=9,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_rpend, bcm2712_vpu_vpu0_i_uc_rpend, "monitor=1,bus=9,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu0_i_uc_ratrans, bcm2712_vpu_vpu0_i_uc_ratrans, "monitor=1,bus=9,counter=10");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_atwait, bcm2712_vpu_vpu_uc_atwait, "monitor=1,bus=10,counter=0");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_atrans, bcm2712_vpu_vpu_uc_atrans, "monitor=1,bus=10,counter=1");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_amax, bcm2712_vpu_vpu_uc_amax, "monitor=1,bus=10,counter=2");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_wwait, bcm2712_vpu_vpu_uc_wwait, "monitor=1,bus=10,counter=3");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_wtrans, bcm2712_vpu_vpu_uc_wtrans, "monitor=1,bus=10,counter=4");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_wmax, bcm2712_vpu_vpu_uc_wmax, "monitor=1,bus=10,counter=5");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rwait, bcm2712_vpu_vpu_uc_rwait, "monitor=1,bus=10,counter=6");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rtrans, bcm2712_vpu_vpu_uc_rtrans, "monitor=1,bus=10,counter=7");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rmax, bcm2712_vpu_vpu_uc_rmax, "monitor=1,bus=10,counter=8");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_rpend, bcm2712_vpu_vpu_uc_rpend, "monitor=1,bus=10,counter=9");
PMU_EVENT_ATTR_STRING(vpu_vpu_uc_ratrans, bcm2712_vpu_vpu_uc_ratrans, "monitor=1,bus=10,counter=10");
PMU_EVENT_ATTR_STRING(vpu_l2_out_atwait, bcm2712_vpu_l2_out_atwait, "monitor=1,bus=11,counter=0");
PMU_EVENT_ATTR_STRING(vpu_l2_out_atrans, bcm2712_vpu_l2_out_atrans, "monitor=1,bus=11,counter=1");
PMU_EVENT_ATTR_STRING(vpu_l2_out_amax, bcm2712_vpu_l2_out_amax, "monitor=1,bus=11,counter=2");
PMU_EVENT_ATTR_STRING(vpu_l2_out_wwait, bcm2712_vpu_l2_out_wwait, "monitor=1,bus=11,counter=3");
PMU_EVENT_ATTR_STRING(vpu_l2_out_wtrans, bcm2712_vpu_l2_out_wtrans, "monitor=1,bus=11,counter=4");
PMU_EVENT_ATTR_STRING(vpu_l2_out_wmax, bcm2712_vpu_l2_out_wmax, "monitor=1,bus=11,counter=5");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rwait, bcm2712_vpu_l2_out_rwait, "monitor=1,bus=11,counter=6");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rtrans, bcm2712_vpu_l2_out_rtrans, "monitor=1,bus=11,counter=7");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rmax, bcm2712_vpu_l2_out_rmax, "monitor=1,bus=11,counter=8");
PMU_EVENT_ATTR_STRING(vpu_l2_out_rpend, bcm2712_vpu_l2_out_rpend, "monitor=1,bus=11,counter=9");
PMU_EVENT_ATTR_STRING(vpu_l2_out_ratrans, bcm2712_vpu_l2_out_ratrans, "monitor=1,bus=11,counter=10");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_atwait, bcm2712_vpu_dma_uc_atwait, "monitor=1,bus=12,counter=0");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_atrans, bcm2712_vpu_dma_uc_atrans, "monitor=1,bus=12,counter=1");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_amax, bcm2712_vpu_dma_uc_amax, "monitor=1,bus=12,counter=2");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_wwait, bcm2712_vpu_dma_uc_wwait, "monitor=1,bus=12,counter=3");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_wtrans, bcm2712_vpu_dma_uc_wtrans, "monitor=1,bus=12,counter=4");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_wmax, bcm2712_vpu_dma_uc_wmax, "monitor=1,bus=12,counter=5");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rwait, bcm2712_vpu_dma_uc_rwait, "monitor=1,bus=12,counter=6");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rtrans, bcm2712_vpu_dma_uc_rtrans, "monitor=1,bus=12,counter=7");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rmax, bcm2712_vpu_dma_uc_rmax, "monitor=1,bus=12,counter=8");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_rpend, bcm2712_vpu_dma_uc_rpend, "monitor=1,bus=12,counter=9");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_ratrans, bcm2712_vpu_dma_uc_ratrans, "monitor=1,bus=12,counter=10");
PMU_EVENT_ATTR_STRING(vpu_l2_in_atwait, bcm2712_vpu_l2_in_atwait, "monitor=1,bus=13,counter=0");
PMU_EVENT_ATTR_STRING(vpu_l2_in_atrans, bcm2712_vpu_l2_in_atrans, "monitor=1,bus=13,counter=1");
PMU_EVENT_ATTR_STRING(vpu_l2_in_amax, bcm2712_vpu_l2_in_amax, "monitor=1,bus=13,counter=2");
PMU_EVENT_ATTR_STRING(vpu_l2_in_wwait, bcm2712_vpu_l2_in_wwait, "monitor=1,bus=13,counter=3");
PMU_EVENT_ATTR_STRING(vpu_l2_in_wtrans, bcm2712_vpu_l2_in_wtrans, "monitor=1,bus=13,counter=4");
PMU_EVENT_ATTR_STRING(vpu_l2_in_wmax, bcm2712_vpu_l2_in_wmax, "monitor=1,bus=13,counter=5");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rwait, bcm2712_vpu_l2_in_rwait, "monitor=1,bus=13,counter=6");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rtrans, bcm2712_vpu_l2_in_rtrans, "monitor=1,bus=13,counter=7");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rmax, bcm2712_vpu_l2_in_rmax, "monitor=1,bus=13,counter=8");
PMU_EVENT_ATTR_STRING(vpu_l2_in_rpend, bcm2712_vpu_l2_in_rpend, "monitor=1,bus=13,counter=9");
PMU_EVENT_ATTR_STRING(vpu_l2_in_ratrans, bcm2712_vpu_l2_in_ratrans, "monitor=1,bus=13,counter=10");
PMU_EVENT_ATTR_STRING(vpu_uc_atwait, bcm2712_vpu_uc_atwait, "monitor=0,bus=0,counter=0");
PMU_EVENT_ATTR_STRING(vpu_uc_atrans, bcm2712_vpu_uc_atrans, "monitor=0,bus=0,counter=1");
PMU_EVENT_ATTR_STRING(vpu_uc_amax, bcm2712_vpu_uc_amax, "monitor=0,bus=0,counter=2");
PMU_EVENT_ATTR_STRING(vpu_uc_wwait, bcm2712_vpu_uc_wwait, "monitor=0,bus=0,counter=3");
PMU_EVENT_ATTR_STRING(vpu_uc_wtrans, bcm2712_vpu_uc_wtrans, "monitor=0,bus=0,counter=4");
PMU_EVENT_ATTR_STRING(vpu_uc_wmax, bcm2712_vpu_uc_wmax, "monitor=0,bus=0,counter=5");
PMU_EVENT_ATTR_STRING(vpu_uc_rwait, bcm2712_vpu_uc_rwait, "monitor=0,bus=0,counter=6");
PMU_EVENT_ATTR_STRING(vpu_uc_rtrans, bcm2712_vpu_uc_rtrans, "monitor=0,bus=0,counter=7");
PMU_EVENT_ATTR_STRING(vpu_uc_rmax, bcm2712_vpu_uc_rmax, "monitor=0,bus=0,counter=8");
PMU_EVENT_ATTR_STRING(vpu_uc_rpend, bcm2712_vpu_uc_rpend, "monitor=0,bus=0,counter=9");
PMU_EVENT_ATTR_STRING(vpu_uc_ratrans, bcm2712_vpu_uc_ratrans, "monitor=0,bus=0,counter=10");
PMU_EVENT_ATTR_STRING(display_atwait, bcm2712_display_atwait, "monitor=0,bus=1,counter=0");
PMU_EVENT_ATTR_STRING(display_atrans, bcm2712_display_atrans, "monitor=0,bus=1,counter=1");
PMU_EVENT_ATTR_STRING(display_amax, bcm2712_display_amax, "monitor=0,bus=1,counter=2");
PMU_EVENT_ATTR_STRING(display_wwait, bcm2712_display_wwait, "monitor=0,bus=1,counter=3");
PMU_EVENT_ATTR_STRING(display_wtrans, bcm2712_display_wtrans, "monitor=0,bus=1,counter=4");
PMU_EVENT_ATTR_STRING(display_wmax, bcm2712_display_wmax, "monitor=0,bus=1,counter=5");
PMU_EVENT_ATTR_STRING(display_rwait, bcm2712_display_rwait, "monitor=0,bus=1,counter=6");
PMU_EVENT_ATTR_STRING(display_rtrans, bcm2712_display_rtrans, "monitor=0,bus=1,counter=7");
PMU_EVENT_ATTR_STRING(display_rmax, bcm2712_display_rmax, "monitor=0,bus=1,counter=8");
PMU_EVENT_ATTR_STRING(display_rpend, bcm2712_display_rpend, "monitor=0,bus=1,counter=9");
PMU_EVENT_ATTR_STRING(display_ratrans, bcm2712_display_ratrans, "monitor=0,bus=1,counter=10");
PMU_EVENT_ATTR_STRING(v3d_atwait, bcm2712_v3d_atwait, "monitor=0,bus=2,counter=0");
PMU_EVENT_ATTR_STRING(v3d_atrans, bcm2712_v3d_atrans, "monitor=0,bus=2,counter=1");
PMU_EVENT_ATTR_STRING(v3d_amax, bcm2712_v3d_amax, "monitor=0,bus=2,counter=2");
PMU_EVENT_ATTR_STRING(v3d_wwait, bcm2712_v3d_wwait, "monitor=0,bus=2,counter=3");
PMU_EVENT_ATTR_STRING(v3d_wtrans, bcm2712_v3d_wtrans, "monitor=0,bus=2,counter=4");
PMU_EVENT_ATTR_STRING(v3d_wmax, bcm2712_v3d_wmax, "monitor=0,bus=2,counter=5");
PMU_EVENT_ATTR_STRING(v3d_rwait, bcm2712_v3d_rwait, "monitor=0,bus=2,counter=6");
PMU_EVENT_ATTR_STRING(v3d_rtrans, bcm2712_v3d_rtrans, "monitor=0,bus=2,counter=7");
PMU_EVENT_ATTR_STRING(v3d_rmax, bcm2712_v3d_rmax, "monitor=0,bus=2,counter=8");
PMU_EVENT_ATTR_STRING(v3d_rpend, bcm2712_v3d_rpend, "monitor=0,bus=2,counter=9");
PMU_EVENT_ATTR_STRING(v3d_ratrans, bcm2712_v3d_ratrans, "monitor=0,bus=2,counter=10");
PMU_EVENT_ATTR_STRING(arm_atwait, bcm2712_arm_atwait, "monitor=0,bus=3,counter=0");
PMU_EVENT_ATTR_STRING(arm_atrans, bcm2712_arm_atrans, "monitor=0,bus=3,counter=1");
PMU_EVENT_ATTR_STRING(arm_amax, bcm2712_arm_amax, "monitor=0,bus=3,counter=2");
PMU_EVENT_ATTR_STRING(arm_wwait, bcm2712_arm_wwait, "monitor=0,bus=3,counter=3");
PMU_EVENT_ATTR_STRING(arm_wtrans, bcm2712_arm_wtrans, "monitor=0,bus=3,counter=4");
PMU_EVENT_ATTR_STRING(arm_wmax, bcm2712_arm_wmax, "monitor=0,bus=3,counter=5");
PMU_EVENT_ATTR_STRING(arm_rwait, bcm2712_arm_rwait, "monitor=0,bus=3,counter=6");
PMU_EVENT_ATTR_STRING(arm_rtrans, bcm2712_arm_rtrans, "monitor=0,bus=3,counter=7");
PMU_EVENT_ATTR_STRING(arm_rmax, bcm2712_arm_rmax, "monitor=0,bus=3,counter=8");
PMU_EVENT_ATTR_STRING(arm_rpend, bcm2712_arm_rpend, "monitor=0,bus=3,counter=9");
PMU_EVENT_ATTR_STRING(arm_ratrans, bcm2712_arm_ratrans, "monitor=0,bus=3,counter=10");
PMU_EVENT_ATTR_STRING(xpt_atwait, bcm2712_xpt_atwait, "monitor=0,bus=4,counter=0");
PMU_EVENT_ATTR_STRING(xpt_atrans, bcm2712_xpt_atrans, "monitor=0,bus=4,counter=1");
PMU_EVENT_ATTR_STRING(xpt_amax, bcm2712_xpt_amax, "monitor=0,bus=4,counter=2");
PMU_EVENT_ATTR_STRING(xpt_wwait, bcm2712_xpt_wwait, "monitor=0,bus=4,counter=3");
PMU_EVENT_ATTR_STRING(xpt_wtrans, bcm2712_xpt_wtrans, "monitor=0,bus=4,counter=4");
PMU_EVENT_ATTR_STRING(xpt_wmax, bcm2712_xpt_wmax, "monitor=0,bus=4,counter=5");
PMU_EVENT_ATTR_STRING(xpt_rwait, bcm2712_xpt_rwait, "monitor=0,bus=4,counter=6");
PMU_EVENT_ATTR_STRING(xpt_rtrans, bcm2712_xpt_rtrans, "monitor=0,bus=4,counter=7");
PMU_EVENT_ATTR_STRING(xpt_rmax, bcm2712_xpt_rmax, "monitor=0,bus=4,counter=8");
PMU_EVENT_ATTR_STRING(xpt_rpend, bcm2712_xpt_rpend, "monitor=0,bus=4,counter=9");
PMU_EVENT_ATTR_STRING(xpt_ratrans, bcm2712_xpt_ratrans, "monitor=0,bus=4,counter=10");
PMU_EVENT_ATTR_STRING(pcie_02_atwait, bcm2712_pcie_02_atwait, "monitor=0,bus=5,counter=0");
PMU_EVENT_ATTR_STRING(pcie_02_atrans, bcm2712_pcie_02_atrans, "monitor=0,bus=5,counter=1");
PMU_EVENT_ATTR_STRING(pcie_02_amax, bcm2712_pcie_02_amax, "monitor=0,bus=5,counter=2");
PMU_EVENT_ATTR_STRING(pcie_02_wwait, bcm2712_pcie_02_wwait, "monitor=0,bus=5,counter=3");
PMU_EVENT_ATTR_STRING(pcie_02_wtrans, bcm2712_pcie_02_wtrans, "monitor=0,bus=5,counter=4");
PMU_EVENT_ATTR_STRING(pcie_02_wmax, bcm2712_pcie_02_wmax, "monitor=0,bus=5,counter=5");
PMU_EVENT_ATTR_STRING(pcie_02_rwait, bcm2712_pcie_02_rwait, "monitor=0,bus=5,counter=6");
PMU_EVENT_ATTR_STRING(pcie_02_rtrans, bcm2712_pcie_02_rtrans, "monitor=0,bus=5,counter=7");
PMU_EVENT_ATTR_STRING(pcie_02_rmax, bcm2712_pcie_02_rmax, "monitor=0,bus=5,counter=8");
PMU_EVENT_ATTR_STRING(pcie_02_rpend, bcm2712_pcie_02_rpend, "monitor=0,bus=5,counter=9");
PMU_EVENT_ATTR_STRING(pcie_02_ratrans, bcm2712_pcie_02_ratrans, "monitor=0,bus=5,counter=10");
PMU_EVENT_ATTR_STRING(pcie_01_atwait, bcm2712_pcie_01_atwait, "monitor=0,bus=6,counter=0");
PMU_EVENT_ATTR_STRING(pcie_01_atrans, bcm2712_pcie_01_atrans, "monitor=0,bus=6,counter=1");
PMU_EVENT_ATTR_STRING(pcie_01_amax, bcm2712_pcie_01_amax, "monitor=0,bus=6,counter=2");
PMU_EVENT_ATTR_STRING(pcie_01_wwait, bcm2712_pcie_01_wwait, "monitor=0,bus=6,counter=3");
PMU_EVENT_ATTR_STRING(pcie_01_wtrans, bcm2712_pcie_01_wtrans, "monitor=0,bus=6,counter=4");
PMU_EVENT_ATTR_STRING(pcie_01_wmax, bcm2712_pcie_01_wmax, "monitor=0,bus=6,counter=5");
PMU_EVENT_ATTR_STRING(pcie_01_rwait, bcm2712_pcie_01_rwait, "monitor=0,bus=6,counter=6");
PMU_EVENT_ATTR_STRING(pcie_01_rtrans, bcm2712_pcie_01_rtrans, "monitor=0,bus=6,counter=7");
PMU_EVENT_ATTR_STRING(pcie_01_rmax, bcm2712_pcie_01_rmax, "monitor=0,bus=6,counter=8");
PMU_EVENT_ATTR_STRING(pcie_01_rpend, bcm2712_pcie_01_rpend, "monitor=0,bus=6,counter=9");
PMU_EVENT_ATTR_STRING(pcie_01_ratrans, bcm2712_pcie_01_ratrans, "monitor=0,bus=6,counter=10");
PMU_EVENT_ATTR_STRING(argon_atwait, bcm2712_argon_atwait, "monitor=0,bus=7,counter=0");
PMU_EVENT_ATTR_STRING(argon_atrans, bcm2712_argon_atrans, "monitor=0,bus=7,counter=1");
PMU_EVENT_ATTR_STRING(argon_amax, bcm2712_argon_amax, "monitor=0,bus=7,counter=2");
PMU_EVENT_ATTR_STRING(argon_wwait, bcm2712_argon_wwait, "monitor=0,bus=7,counter=3");
PMU_EVENT_ATTR_STRING(argon_wtrans, bcm2712_argon_wtrans, "monitor=0,bus=7,counter=4");
PMU_EVENT_ATTR_STRING(argon_wmax, bcm2712_argon_wmax, "monitor=0,bus=7,counter=5");
PMU_EVENT_ATTR_STRING(argon_rwait, bcm2712_argon_rwait, "monitor=0,bus=7,counter=6");
PMU_EVENT_ATTR_STRING(argon_rtrans, bcm2712_argon_rtrans, "monitor=0,bus=7,counter=7");
PMU_EVENT_ATTR_STRING(argon_rmax, bcm2712_argon_rmax, "monitor=0,bus=7,counter=8");
PMU_EVENT_ATTR_STRING(argon_rpend, bcm2712_argon_rpend, "monitor=0,bus=7,counter=9");
PMU_EVENT_ATTR_STRING(argon_ratrans, bcm2712_argon_ratrans, "monitor=0,bus=7,counter=10");
PMU_EVENT_ATTR_STRING(emmc2_atwait, bcm2712_emmc2_atwait, "monitor=0,bus=8,counter=0");
PMU_EVENT_ATTR_STRING(emmc2_atrans, bcm2712_emmc2_atrans, "monitor=0,bus=8,counter=1");
PMU_EVENT_ATTR_STRING(emmc2_amax, bcm2712_emmc2_amax, "monitor=0,bus=8,counter=2");
PMU_EVENT_ATTR_STRING(emmc2_wwait, bcm2712_emmc2_wwait, "monitor=0,bus=8,counter=3");
PMU_EVENT_ATTR_STRING(emmc2_wtrans, bcm2712_emmc2_wtrans, "monitor=0,bus=8,counter=4");
PMU_EVENT_ATTR_STRING(emmc2_wmax, bcm2712_emmc2_wmax, "monitor=0,bus=8,counter=5");
PMU_EVENT_ATTR_STRING(emmc2_rwait, bcm2712_emmc2_rwait, "monitor=0,bus=8,counter=6");
PMU_EVENT_ATTR_STRING(emmc2_rtrans, bcm2712_emmc2_rtrans, "monitor=0,bus=8,counter=7");
PMU_EVENT_ATTR_STRING(emmc2_rmax, bcm2712_emmc2_rmax, "monitor=0,bus=8,counter=8");
PMU_EVENT_ATTR_STRING(emmc2_rpend, bcm2712_emmc2_rpend, "monitor=0,bus=8,counter=9");
PMU_EVENT_ATTR_STRING(emmc2_ratrans, bcm2712_emmc2_ratrans, "monitor=0,bus=8,counter=10");
PMU_EVENT_ATTR_STRING(sd_dma_atwait, bcm2712_sd_dma_atwait, "monitor=0,bus=9,counter=0");
PMU_EVENT_ATTR_STRING(sd_dma_atrans, bcm2712_sd_dma_atrans, "monitor=0,bus=9,counter=1");
PMU_EVENT_ATTR_STRING(sd_dma_amax, bcm2712_sd_dma_amax, "monitor=0,bus=9,counter=2");
PMU_EVENT_ATTR_STRING(sd_dma_wwait, bcm2712_sd_dma_wwait, "monitor=0,bus=9,counter=3");
PMU_EVENT_ATTR_STRING(sd_dma_wtrans, bcm2712_sd_dma_wtrans, "monitor=0,bus=9,counter=4");
PMU_EVENT_ATTR_STRING(sd_dma_wmax, bcm2712_sd_dma_wmax, "monitor=0,bus=9,counter=5");
PMU_EVENT_ATTR_STRING(sd_dma_rwait, bcm2712_sd_dma_rwait, "monitor=0,bus=9,counter=6");
PMU_EVENT_ATTR_STRING(sd_dma_rtrans, bcm2712_sd_dma_rtrans, "monitor=0,bus=9,counter=7");
PMU_EVENT_ATTR_STRING(sd_dma_rmax, bcm2712_sd_dma_rmax, "monitor=0,bus=9,counter=8");
PMU_EVENT_ATTR_STRING(sd_dma_rpend, bcm2712_sd_dma_rpend, "monitor=0,bus=9,counter=9");
PMU_EVENT_ATTR_STRING(sd_dma_ratrans, bcm2712_sd_dma_ratrans, "monitor=0,bus=9,counter=10");
PMU_EVENT_ATTR_STRING(hvdp_atwait, bcm2712_hvdp_atwait, "monitor=0,bus=10,counter=0");
PMU_EVENT_ATTR_STRING(hvdp_atrans, bcm2712_hvdp_atrans, "monitor=0,bus=10,counter=1");
PMU_EVENT_ATTR_STRING(hvdp_amax, bcm2712_hvdp_amax, "monitor=0,bus=10,counter=2");
PMU_EVENT_ATTR_STRING(hvdp_wwait, bcm2712_hvdp_wwait, "monitor=0,bus=10,counter=3");
PMU_EVENT_ATTR_STRING(hvdp_wtrans, bcm2712_hvdp_wtrans, "monitor=0,bus=10,counter=4");
PMU_EVENT_ATTR_STRING(hvdp_wmax, bcm2712_hvdp_wmax, "monitor=0,bus=10,counter=5");
PMU_EVENT_ATTR_STRING(hvdp_rwait, bcm2712_hvdp_rwait, "monitor=0,bus=10,counter=6");
PMU_EVENT_ATTR_STRING(hvdp_rtrans, bcm2712_hvdp_rtrans, "monitor=0,bus=10,counter=7");
PMU_EVENT_ATTR_STRING(hvdp_rmax, bcm2712_hvdp_rmax, "monitor=0,bus=10,counter=8");
PMU_EVENT_ATTR_STRING(hvdp_rpend, bcm2712_hvdp_rpend, "monitor=0,bus=10,counter=9");
PMU_EVENT_ATTR_STRING(hvdp_ratrans, bcm2712_hvdp_ratrans, "monitor=0,bus=10,counter=10");
PMU_EVENT_ATTR_STRING(per_atwait, bcm2712_per_atwait, "monitor=0,bus=11,counter=0");
PMU_EVENT_ATTR_STRING(per_atrans, bcm2712_per_atrans, "monitor=0,bus=11,counter=1");
PMU_EVENT_ATTR_STRING(per_amax, bcm2712_per_amax, "monitor=0,bus=11,counter=2");
PMU_EVENT_ATTR_STRING(per_wwait, bcm2712_per_wwait, "monitor=0,bus=11,counter=3");
PMU_EVENT_ATTR_STRING(per_wtrans, bcm2712_per_wtrans, "monitor=0,bus=11,counter=4");
PMU_EVENT_ATTR_STRING(per_wmax, bcm2712_per_wmax, "monitor=0,bus=11,counter=5");
PMU_EVENT_ATTR_STRING(per_rwait, bcm2712_per_rwait, "monitor=0,bus=11,counter=6");
PMU_EVENT_ATTR_STRING(per_rtrans, bcm2712_per_rtrans, "monitor=0,bus=11,counter=7");
PMU_EVENT_ATTR_STRING(per_rmax, bcm2712_per_rmax, "monitor=0,bus=11,counter=8");
PMU_EVENT_ATTR_STRING(per_rpend, bcm2712_per_rpend, "monitor=0,bus=11,counter=9");
PMU_EVENT_ATTR_STRING(per_ratrans, bcm2712_per_ratrans, "monitor=0,bus=11,counter=10");
PMU_EVENT_ATTR_STRING(system_l2_atwait, bcm2712_system_l2_atwait, "monitor=0,bus=12,counter=0");
PMU_EVENT_ATTR_STRING(system_l2_atrans, bcm2712_system_l2_atrans, "monitor=0,bus=12,counter=1");
PMU_EVENT_ATTR_STRING(system_l2_amax, bcm2712_system_l2_amax, "monitor=0,bus=12,counter=2");
PMU_EVENT_ATTR_STRING(system_l2_wwait, bcm2712_system_l2_wwait, "monitor=0,bus=12,counter=3");
PMU_EVENT_ATTR_STRING(system_l2_wtrans, bcm2712_system_l2_wtrans, "monitor=0,bus=12,counter=4");
PMU_EVENT_ATTR_STRING(system_l2_wmax, bcm2712_system_l2_wmax, "monitor=0,bus=12,counter=5");
PMU_EVENT_ATTR_STRING(system_l2_rwait, bcm2712_system_l2_rwait, "monitor=0,bus=12,counter=6");
PMU_EVENT_ATTR_STRING(system_l2_rtrans, bcm2712_system_l2_rtrans, "monitor=0,bus=12,counter=7");
PMU_EVENT_ATTR_STRING(system_l2_rmax, bcm2712_system_l2_rmax, "monitor=0,bus=12,counter=8");
PMU_EVENT_ATTR_STRING(system_l2_rpend, bcm2712_system_l2_rpend, "monitor=0,bus=12,counter=9");
PMU_EVENT_ATTR_STRING(system_l2_ratrans, bcm2712_system_l2_ratrans, "monitor=0,bus=12,counter=10");
PMU_EVENT_ATTR_STRING(vpu_uc_atwait, bcm2712d0_vpu_uc_atwait, "monitor=0,bus=0,counter=0");
PMU_EVENT_ATTR_STRING(vpu_uc_atrans, bcm2712d0_vpu_uc_atrans, "monitor=0,bus=0,counter=1");
PMU_EVENT_ATTR_STRING(vpu_uc_amax, bcm2712d0_vpu_uc_amax, "monitor=0,bus=0,counter=2");
PMU_EVENT_ATTR_STRING(vpu_uc_wwait, bcm2712d0_vpu_uc_wwait, "monitor=0,bus=0,counter=3");
PMU_EVENT_ATTR_STRING(vpu_uc_wtrans, bcm2712d0_vpu_uc_wtrans, "monitor=0,bus=0,counter=4");
PMU_EVENT_ATTR_STRING(vpu_uc_wmax, bcm2712d0_vpu_uc_wmax, "monitor=0,bus=0,counter=5");
PMU_EVENT_ATTR_STRING(vpu_uc_rwait, bcm2712d0_vpu_uc_rwait, "monitor=0,bus=0,counter=6");
PMU_EVENT_ATTR_STRING(vpu_uc_rtrans, bcm2712d0_vpu_uc_rtrans, "monitor=0,bus=0,counter=7");
PMU_EVENT_ATTR_STRING(vpu_uc_rmax, bcm2712d0_vpu_uc_rmax, "monitor=0,bus=0,counter=8");
PMU_EVENT_ATTR_STRING(vpu_uc_rpend, bcm2712d0_vpu_uc_rpend, "monitor=0,bus=0,counter=9");
PMU_EVENT_ATTR_STRING(vpu_uc_ratrans, bcm2712d0_vpu_uc_ratrans, "monitor=0,bus=0,counter=10");
PMU_EVENT_ATTR_STRING(display_atwait, bcm2712d0_display_atwait, "monitor=0,bus=1,counter=0");
PMU_EVENT_ATTR_STRING(display_atrans, bcm2712d0_display_atrans, "monitor=0,bus=1,counter=1");
PMU_EVENT_ATTR_STRING(display_amax, bcm2712d0_display_amax, "monitor=0,bus=1,counter=2");
PMU_EVENT_ATTR_STRING(display_wwait, bcm2712d0_display_wwait, "monitor=0,bus=1,counter=3");
PMU_EVENT_ATTR_STRING(display_wtrans, bcm2712d0_display_wtrans, "monitor=0,bus=1,counter=4");
PMU_EVENT_ATTR_STRING(display_wmax, bcm2712d0_display_wmax, "monitor=0,bus=1,counter=5");
PMU_EVENT_ATTR_STRING(display_rwait, bcm2712d0_display_rwait, "monitor=0,bus=1,counter=6");
PMU_EVENT_ATTR_STRING(display_rtrans, bcm2712d0_display_rtrans, "monitor=0,bus=1,counter=7");
PMU_EVENT_ATTR_STRING(display_rmax, bcm2712d0_display_rmax, "monitor=0,bus=1,counter=8");
PMU_EVENT_ATTR_STRING(display_rpend, bcm2712d0_display_rpend, "monitor=0,bus=1,counter=9");
PMU_EVENT_ATTR_STRING(display_ratrans, bcm2712d0_display_ratrans, "monitor=0,bus=1,counter=10");
PMU_EVENT_ATTR_STRING(v3d_atwait, bcm2712d0_v3d_atwait, "monitor=0,bus=2,counter=0");
PMU_EVENT_ATTR_STRING(v3d_atrans, bcm2712d0_v3d_atrans, "monitor=0,bus=2,counter=1");
PMU_EVENT_ATTR_STRING(v3d_amax, bcm2712d0_v3d_amax, "monitor=0,bus=2,counter=2");
PMU_EVENT_ATTR_STRING(v3d_wwait, bcm2712d0_v3d_wwait, "monitor=0,bus=2,counter=3");
PMU_EVENT_ATTR_STRING(v3d_wtrans, bcm2712d0_v3d_wtrans, "monitor=0,bus=2,counter=4");
PMU_EVENT_ATTR_STRING(v3d_wmax, bcm2712d0_v3d_wmax, "monitor=0,bus=2,counter=5");
PMU_EVENT_ATTR_STRING(v3d_rwait, bcm2712d0_v3d_rwait, "monitor=0,bus=2,counter=6");
PMU_EVENT_ATTR_STRING(v3d_rtrans, bcm2712d0_v3d_rtrans, "monitor=0,bus=2,counter=7");
PMU_EVENT_ATTR_STRING(v3d_rmax, bcm2712d0_v3d_rmax, "monitor=0,bus=2,counter=8");
PMU_EVENT_ATTR_STRING(v3d_rpend, bcm2712d0_v3d_rpend, "monitor=0,bus=2,counter=9");
PMU_EVENT_ATTR_STRING(v3d_ratrans, bcm2712d0_v3d_ratrans, "monitor=0,bus=2,counter=10");
PMU_EVENT_ATTR_STRING(arm_atwait, bcm2712d0_arm_atwait, "monitor=0,bus=3,counter=0");
PMU_EVENT_ATTR_STRING(arm_atrans, bcm2712d0_arm_atrans, "monitor=0,bus=3,counter=1");
PMU_EVENT_ATTR_STRING(arm_amax, bcm2712d0_arm_amax, "monitor=0,bus=3,counter=2");
PMU_EVENT_ATTR_STRING(arm_wwait, bcm2712d0_arm_wwait, "monitor=0,bus=3,counter=3");
PMU_EVENT_ATTR_STRING(arm_wtrans, bcm2712d0_arm_wtrans, "monitor=0,bus=3,counter=4");
PMU_EVENT_ATTR_STRING(arm_wmax, bcm2712d0_arm_wmax, "monitor=0,bus=3,counter=5");
PMU_EVENT_ATTR_STRING(arm_rwait, bcm2712d0_arm_rwait, "monitor=0,bus=3,counter=6");
PMU_EVENT_ATTR_STRING(arm_rtrans, bcm2712d0_arm_rtrans, "monitor=0,bus=3,counter=7");
PMU_EVENT_ATTR_STRING(arm_rmax, bcm2712d0_arm_rmax, "monitor=0,bus=3,counter=8");
PMU_EVENT_ATTR_STRING(arm_rpend, bcm2712d0_arm_rpend, "monitor=0,bus=3,counter=9");
PMU_EVENT_ATTR_STRING(arm_ratrans, bcm2712d0_arm_ratrans, "monitor=0,bus=3,counter=10");
PMU_EVENT_ATTR_STRING(pcie_02_atwait, bcm2712d0_pcie_02_atwait, "monitor=0,bus=4,counter=0");
PMU_EVENT_ATTR_STRING(pcie_02_atrans, bcm2712d0_pcie_02_atrans, "monitor=0,bus=4,counter=1");
PMU_EVENT_ATTR_STRING(pcie_02_amax, bcm2712d0_pcie_02_amax, "monitor=0,bus=4,counter=2");
PMU_EVENT_ATTR_STRING(pcie_02_wwait, bcm2712d0_pcie_02_wwait, "monitor=0,bus=4,counter=3");
PMU_EVENT_ATTR_STRING(pcie_02_wtrans, bcm2712d0_pcie_02_wtrans, "monitor=0,bus=4,counter=4");
PMU_EVENT_ATTR_STRING(pcie_02_wmax, bcm2712d0_pcie_02_wmax, "monitor=0,bus=4,counter=5");
PMU_EVENT_ATTR_STRING(pcie_02_rwait, bcm2712d0_pcie_02_rwait, "monitor=0,bus=4,counter=6");
PMU_EVENT_ATTR_STRING(pcie_02_rtrans, bcm2712d0_pcie_02_rtrans, "monitor=0,bus=4,counter=7");
PMU_EVENT_ATTR_STRING(pcie_02_rmax, bcm2712d0_pcie_02_rmax, "monitor=0,bus=4,counter=8");
PMU_EVENT_ATTR_STRING(pcie_02_rpend, bcm2712d0_pcie_02_rpend, "monitor=0,bus=4,counter=9");
PMU_EVENT_ATTR_STRING(pcie_02_ratrans, bcm2712d0_pcie_02_ratrans, "monitor=0,bus=4,counter=10");
PMU_EVENT_ATTR_STRING(argon_atwait, bcm2712d0_argon_atwait, "monitor=0,bus=5,counter=0");
PMU_EVENT_ATTR_STRING(argon_atrans, bcm2712d0_argon_atrans, "monitor=0,bus=5,counter=1");
PMU_EVENT_ATTR_STRING(argon_amax, bcm2712d0_argon_amax, "monitor=0,bus=5,counter=2");
PMU_EVENT_ATTR_STRING(argon_wwait, bcm2712d0_argon_wwait, "monitor=0,bus=5,counter=3");
PMU_EVENT_ATTR_STRING(argon_wtrans, bcm2712d0_argon_wtrans, "monitor=0,bus=5,counter=4");
PMU_EVENT_ATTR_STRING(argon_wmax, bcm2712d0_argon_wmax, "monitor=0,bus=5,counter=5");
PMU_EVENT_ATTR_STRING(argon_rwait, bcm2712d0_argon_rwait, "monitor=0,bus=5,counter=6");
PMU_EVENT_ATTR_STRING(argon_rtrans, bcm2712d0_argon_rtrans, "monitor=0,bus=5,counter=7");
PMU_EVENT_ATTR_STRING(argon_rmax, bcm2712d0_argon_rmax, "monitor=0,bus=5,counter=8");
PMU_EVENT_ATTR_STRING(argon_rpend, bcm2712d0_argon_rpend, "monitor=0,bus=5,counter=9");
PMU_EVENT_ATTR_STRING(argon_ratrans, bcm2712d0_argon_ratrans, "monitor=0,bus=5,counter=10");
PMU_EVENT_ATTR_STRING(emmc2_atwait, bcm2712d0_emmc2_atwait, "monitor=0,bus=6,counter=0");
PMU_EVENT_ATTR_STRING(emmc2_atrans, bcm2712d0_emmc2_atrans, "monitor=0,bus=6,counter=1");
PMU_EVENT_ATTR_STRING(emmc2_amax, bcm2712d0_emmc2_amax, "monitor=0,bus=6,counter=2");
PMU_EVENT_ATTR_STRING(emmc2_wwait, bcm2712d0_emmc2_wwait, "monitor=0,bus=6,counter=3");
PMU_EVENT_ATTR_STRING(emmc2_wtrans, bcm2712d0_emmc2_wtrans, "monitor=0,bus=6,counter=4");
PMU_EVENT_ATTR_STRING(emmc2_wmax, bcm2712d0_emmc2_wmax, "monitor=0,bus=6,counter=5");
PMU_EVENT_ATTR_STRING(emmc2_rwait, bcm2712d0_emmc2_rwait, "monitor=0,bus=6,counter=6");
PMU_EVENT_ATTR_STRING(emmc2_rtrans, bcm2712d0_emmc2_rtrans, "monitor=0,bus=6,counter=7");
PMU_EVENT_ATTR_STRING(emmc2_rmax, bcm2712d0_emmc2_rmax, "monitor=0,bus=6,counter=8");
PMU_EVENT_ATTR_STRING(emmc2_rpend, bcm2712d0_emmc2_rpend, "monitor=0,bus=6,counter=9");
PMU_EVENT_ATTR_STRING(emmc2_ratrans, bcm2712d0_emmc2_ratrans, "monitor=0,bus=6,counter=10");
PMU_EVENT_ATTR_STRING(sd_dma_atwait, bcm2712d0_sd_dma_atwait, "monitor=0,bus=7,counter=0");
PMU_EVENT_ATTR_STRING(sd_dma_atrans, bcm2712d0_sd_dma_atrans, "monitor=0,bus=7,counter=1");
PMU_EVENT_ATTR_STRING(sd_dma_amax, bcm2712d0_sd_dma_amax, "monitor=0,bus=7,counter=2");
PMU_EVENT_ATTR_STRING(sd_dma_wwait, bcm2712d0_sd_dma_wwait, "monitor=0,bus=7,counter=3");
PMU_EVENT_ATTR_STRING(sd_dma_wtrans, bcm2712d0_sd_dma_wtrans, "monitor=0,bus=7,counter=4");
PMU_EVENT_ATTR_STRING(sd_dma_wmax, bcm2712d0_sd_dma_wmax, "monitor=0,bus=7,counter=5");
PMU_EVENT_ATTR_STRING(sd_dma_rwait, bcm2712d0_sd_dma_rwait, "monitor=0,bus=7,counter=6");
PMU_EVENT_ATTR_STRING(sd_dma_rtrans, bcm2712d0_sd_dma_rtrans, "monitor=0,bus=7,counter=7");
PMU_EVENT_ATTR_STRING(sd_dma_rmax, bcm2712d0_sd_dma_rmax, "monitor=0,bus=7,counter=8");
PMU_EVENT_ATTR_STRING(sd_dma_rpend, bcm2712d0_sd_dma_rpend, "monitor=0,bus=7,counter=9");
PMU_EVENT_ATTR_STRING(sd_dma_ratrans, bcm2712d0_sd_dma_ratrans, "monitor=0,bus=7,counter=10");
PMU_EVENT_ATTR_STRING(per_atwait, bcm2712d0_per_atwait, "monitor=0,bus=8,counter=0");
PMU_EVENT_ATTR_STRING(per_atrans, bcm2712d0_per_atrans, "monitor=0,bus=8,counter=1");
PMU_EVENT_ATTR_STRING(per_amax, bcm2712d0_per_amax, "monitor=0,bus=8,counter=2");
PMU_EVENT_ATTR_STRING(per_wwait, bcm2712d0_per_wwait, "monitor=0,bus=8,counter=3");
PMU_EVENT_ATTR_STRING(per_wtrans, bcm2712d0_per_wtrans, "monitor=0,bus=8,counter=4");
PMU_EVENT_ATTR_STRING(per_wmax, bcm2712d0_per_wmax, "monitor=0,bus=8,counter=5");
PMU_EVENT_ATTR_STRING(per_rwait, bcm2712d0_per_rwait, "monitor=0,bus=8,counter=6");
PMU_EVENT_ATTR_STRING(per_rtrans, bcm2712d0_per_rtrans, "monitor=0,bus=8,counter=7");
PMU_EVENT_ATTR_STRING(per_rmax, bcm2712d0_per_rmax, "monitor=0,bus=8,counter=8");
PMU_EVENT_ATTR_STRING(per_rpend, bcm2712d0_per_rpend, "monitor=0,bus=8,counter=9");
PMU_EVENT_ATTR_STRING(per_ratrans, bcm2712d0_per_ratrans, "monitor=0,bus=8,counter=10");
PMU_EVENT_ATTR_STRING(system_l2_atwait, bcm2712d0_system_l2_atwait, "monitor=0,bus=9,counter=0");
PMU_EVENT_ATTR_STRING(system_l2_atrans, bcm2712d0_system_l2_atrans, "monitor=0,bus=9,counter=1");
PMU_EVENT_ATTR_STRING(system_l2_amax, bcm2712d0_system_l2_amax, "monitor=0,bus=9,counter=2");
PMU_EVENT_ATTR_STRING(system_l2_wwait, bcm2712d0_system_l2_wwait, "monitor=0,bus=9,counter=3");
PMU_EVENT_ATTR_STRING(system_l2_wtrans, bcm2712d0_system_l2_wtrans, "monitor=0,bus=9,counter=4");
PMU_EVENT_ATTR_STRING(system_l2_wmax, bcm2712d0_system_l2_wmax, "monitor=0,bus=9,counter=5");
PMU_EVENT_ATTR_STRING(system_l2_rwait, bcm2712d0_system_l2_rwait, "monitor=0,bus=9,counter=6");
PMU_EVENT_ATTR_STRING(system_l2_rtrans, bcm2712d0_system_l2_rtrans, "monitor=0,bus=9,counter=7");
PMU_EVENT_ATTR_STRING(system_l2_rmax, bcm2712d0_system_l2_rmax, "monitor=0,bus=9,counter=8");
PMU_EVENT_ATTR_STRING(system_l2_rpend, bcm2712d0_system_l2_rpend, "monitor=0,bus=9,counter=9");
PMU_EVENT_ATTR_STRING(system_l2_ratrans, bcm2712d0_system_l2_ratrans, "monitor=0,bus=9,counter=10");

static struct attribute *bcm2712_events[] = {
	&bcm2712_vpu_uc_atwait.attr.attr,
	&bcm2712_vpu_uc_atrans.attr.attr,
	&bcm2712_vpu_uc_amax.attr.attr,
	&bcm2712_vpu_uc_wwait.attr.attr,
	&bcm2712_vpu_uc_wtrans.attr.attr,
	&bcm2712_vpu_uc_wmax.attr.attr,
	&bcm2712_vpu_uc_rwait.attr.attr,
	&bcm2712_vpu_uc_rtrans.attr.attr,
	&bcm2712_vpu_uc_rmax.attr.attr,
	&bcm2712_vpu_uc_rpend.attr.attr,
	&bcm2712_vpu_uc_ratrans.attr.attr,
	&bcm2712_display_atwait.attr.attr,
	&bcm2712_display_atrans.attr.attr,
	&bcm2712_display_amax.attr.attr,
	&bcm2712_display_wwait.attr.attr,
	&bcm2712_display_wtrans.attr.attr,
	&bcm2712_display_wmax.attr.attr,
	&bcm2712_display_rwait.attr.attr,
	&bcm2712_display_rtrans.attr.attr,
	&bcm2712_display_rmax.attr.attr,
	&bcm2712_display_rpend.attr.attr,
	&bcm2712_display_ratrans.attr.attr,
	&bcm2712_v3d_atwait.attr.attr,
	&bcm2712_v3d_atrans.attr.attr,
	&bcm2712_v3d_amax.attr.attr,
	&bcm2712_v3d_wwait.attr.attr,
	&bcm2712_v3d_wtrans.attr.attr,
	&bcm2712_v3d_wmax.attr.attr,
	&bcm2712_v3d_rwait.attr.attr,
	&bcm2712_v3d_rtrans.attr.attr,
	&bcm2712_v3d_rmax.attr.attr,
	&bcm2712_v3d_rpend.attr.attr,
	&bcm2712_v3d_ratrans.attr.attr,
	&bcm2712_arm_atwait.attr.attr,
	&bcm2712_arm_atrans.attr.attr,
	&bcm2712_arm_amax.attr.attr,
	&bcm2712_arm_wwait.attr.attr,
	&bcm2712_arm_wtrans.attr.attr,
	&bcm2712_arm_wmax.attr.attr,
	&bcm2712_arm_rwait.attr.attr,
	&bcm2712_arm_rtrans.attr.attr,
	&bcm2712_arm_rmax.attr.attr,
	&bcm2712_arm_rpend.attr.attr,
	&bcm2712_arm_ratrans.attr.attr,
	&bcm2712_xpt_atwait.attr.attr,
	&bcm2712_xpt_atrans.attr.attr,
	&bcm2712_xpt_amax.attr.attr,
	&bcm2712_xpt_wwait.attr.attr,
	&bcm2712_xpt_wtrans.attr.attr,
	&bcm2712_xpt_wmax.attr.attr,
	&bcm2712_xpt_rwait.attr.attr,
	&bcm2712_xpt_rtrans.attr.attr,
	&bcm2712_xpt_rmax.attr.attr,
	&bcm2712_xpt_rpend.attr.attr,
	&bcm2712_xpt_ratrans.attr.attr,
	&bcm2712_pcie_02_atwait.attr.attr,
	&bcm2712_pcie_02_atrans.attr.attr,
	&bcm2712_pcie_02_amax.attr.attr,
	&bcm2712_pcie_02_wwait.attr.attr,
	&bcm2712_pcie_02_wtrans.attr.attr,
	&bcm2712_pcie_02_wmax.attr.attr,
	&bcm2712_pcie_02_rwait.attr.attr,
	&bcm2712_pcie_02_rtrans.attr.attr,
	&bcm2712_pcie_02_rmax.attr.attr,
	&bcm2712_pcie_02_rpend.attr.attr,
	&bcm2712_pcie_02_ratrans.attr.attr,
	&bcm2712_pcie_01_atwait.attr.attr,
	&bcm2712_pcie_01_atrans.attr.attr,
	&bcm2712_pcie_01_amax.attr.attr,
	&bcm2712_pcie_01_wwait.attr.attr,
	&bcm2712_pcie_01_wtrans.attr.attr,
	&bcm2712_pcie_01_wmax.attr.attr,
	&bcm2712_pcie_01_rwait.attr.attr,
	&bcm2712_pcie_01_rtrans.attr.attr,
	&bcm2712_pcie_01_rmax.attr.attr,
	&bcm2712_pcie_01_rpend.attr.attr,
	&bcm2712_pcie_01_ratrans.attr.attr,
	&bcm2712_argon_atwait.attr.attr,
	&bcm2712_argon_atrans.attr.attr,
	&bcm2712_argon_amax.attr.attr,
	&bcm2712_argon_wwait.attr.attr,
	&bcm2712_argon_wtrans.attr.attr,
	&bcm2712_argon_wmax.attr.attr,
	&bcm2712_argon_rwait.attr.attr,
	&bcm2712_argon_rtrans.attr.attr,
	&bcm2712_argon_rmax.attr.attr,
	&bcm2712_argon_rpend.attr.attr,
	&bcm2712_argon_ratrans.attr.attr,
	&bcm2712_emmc2_atwait.attr.attr,
	&bcm2712_emmc2_atrans.attr.attr,
	&bcm2712_emmc2_amax.attr.attr,
	&bcm2712_emmc2_wwait.attr.attr,
	&bcm2712_emmc2_wtrans.attr.attr,
	&bcm2712_emmc2_wmax.attr.attr,
	&bcm2712_emmc2_rwait.attr.attr,
	&bcm2712_emmc2_rtrans.attr.attr,
	&bcm2712_emmc2_rmax.attr.attr,
	&bcm2712_emmc2_rpend.attr.attr,
	&bcm2712_emmc2_ratrans.attr.attr,
	&bcm2712_sd_dma_atwait.attr.attr,
	&bcm2712_sd_dma_atrans.attr.attr,
	&bcm2712_sd_dma_amax.attr.attr,
	&bcm2712_sd_dma_wwait.attr.attr,
	&bcm2712_sd_dma_wtrans.attr.attr,
	&bcm2712_sd_dma_wmax.attr.attr,
	&bcm2712_sd_dma_rwait.attr.attr,
	&bcm2712_sd_dma_rtrans.attr.attr,
	&bcm2712_sd_dma_rmax.attr.attr,
	&bcm2712_sd_dma_rpend.attr.attr,
	&bcm2712_sd_dma_ratrans.attr.attr,
	&bcm2712_hvdp_atwait.attr.attr,
	&bcm2712_hvdp_atrans.attr.attr,
	&bcm2712_hvdp_amax.attr.attr,
	&bcm2712_hvdp_wwait.attr.attr,
	&bcm2712_hvdp_wtrans.attr.attr,
	&bcm2712_hvdp_wmax.attr.attr,
	&bcm2712_hvdp_rwait.attr.attr,
	&bcm2712_hvdp_rtrans.attr.attr,
	&bcm2712_hvdp_rmax.attr.attr,
	&bcm2712_hvdp_rpend.attr.attr,
	&bcm2712_hvdp_ratrans.attr.attr,
	&bcm2712_per_atwait.attr.attr,
	&bcm2712_per_atrans.attr.attr,
	&bcm2712_per_amax.attr.attr,
	&bcm2712_per_wwait.attr.attr,
	&bcm2712_per_wtrans.attr.attr,
	&bcm2712_per_wmax.attr.attr,
	&bcm2712_per_rwait.attr.attr,
	&bcm2712_per_rtrans.attr.attr,
	&bcm2712_per_rmax.attr.attr,
	&bcm2712_per_rpend.attr.attr,
	&bcm2712_per_ratrans.attr.attr,
	&bcm2712_system_l2_atwait.attr.attr,
	&bcm2712_system_l2_atrans.attr.attr,
	&bcm2712_system_l2_amax.attr.attr,
	&bcm2712_system_l2_wwait.attr.attr,
	&bcm2712_system_l2_wtrans.attr.attr,
	&bcm2712_system_l2_wmax.attr.attr,
	&bcm2712_system_l2_rwait.attr.attr,
	&bcm2712_system_l2_rtrans.attr.attr,
	&bcm2712_system_l2_rmax.attr.attr,
	&bcm2712_system_l2_rpend.attr.attr,
	&bcm2712_system_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_atwait.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_atrans.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_amax.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_wwait.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_wtrans.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_wmax.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_rwait.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_rtrans.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_rmax.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_rpend.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_atwait.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_atrans.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_amax.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_wwait.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_wtrans.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_wmax.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_rwait.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_rtrans.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_rmax.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_rpend.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_atwait.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_atrans.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_amax.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_wwait.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_wtrans.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_wmax.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_rwait.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_rtrans.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_rmax.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_rpend.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_atwait.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_atrans.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_amax.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_wwait.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_wtrans.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_wmax.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_rwait.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_rtrans.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_rmax.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_rpend.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_ratrans.attr.attr,
	&bcm2712_vpu_system_l2_atwait.attr.attr,
	&bcm2712_vpu_system_l2_atrans.attr.attr,
	&bcm2712_vpu_system_l2_amax.attr.attr,
	&bcm2712_vpu_system_l2_wwait.attr.attr,
	&bcm2712_vpu_system_l2_wtrans.attr.attr,
	&bcm2712_vpu_system_l2_wmax.attr.attr,
	&bcm2712_vpu_system_l2_rwait.attr.attr,
	&bcm2712_vpu_system_l2_rtrans.attr.attr,
	&bcm2712_vpu_system_l2_rmax.attr.attr,
	&bcm2712_vpu_system_l2_rpend.attr.attr,
	&bcm2712_vpu_system_l2_ratrans.attr.attr,
	&bcm2712_vpu_dma_l2_atwait.attr.attr,
	&bcm2712_vpu_dma_l2_atrans.attr.attr,
	&bcm2712_vpu_dma_l2_amax.attr.attr,
	&bcm2712_vpu_dma_l2_wwait.attr.attr,
	&bcm2712_vpu_dma_l2_wtrans.attr.attr,
	&bcm2712_vpu_dma_l2_wmax.attr.attr,
	&bcm2712_vpu_dma_l2_rwait.attr.attr,
	&bcm2712_vpu_dma_l2_rtrans.attr.attr,
	&bcm2712_vpu_dma_l2_rmax.attr.attr,
	&bcm2712_vpu_dma_l2_rpend.attr.attr,
	&bcm2712_vpu_dma_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_amax.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_ratrans.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_amax.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_ratrans.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_amax.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_ratrans.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_amax.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_ratrans.attr.attr,
	&bcm2712_vpu_vpu_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu_uc_amax.attr.attr,
	&bcm2712_vpu_vpu_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu_uc_ratrans.attr.attr,
	&bcm2712_vpu_l2_out_atwait.attr.attr,
	&bcm2712_vpu_l2_out_atrans.attr.attr,
	&bcm2712_vpu_l2_out_amax.attr.attr,
	&bcm2712_vpu_l2_out_wwait.attr.attr,
	&bcm2712_vpu_l2_out_wtrans.attr.attr,
	&bcm2712_vpu_l2_out_wmax.attr.attr,
	&bcm2712_vpu_l2_out_rwait.attr.attr,
	&bcm2712_vpu_l2_out_rtrans.attr.attr,
	&bcm2712_vpu_l2_out_rmax.attr.attr,
	&bcm2712_vpu_l2_out_rpend.attr.attr,
	&bcm2712_vpu_l2_out_ratrans.attr.attr,
	&bcm2712_vpu_dma_uc_atwait.attr.attr,
	&bcm2712_vpu_dma_uc_atrans.attr.attr,
	&bcm2712_vpu_dma_uc_amax.attr.attr,
	&bcm2712_vpu_dma_uc_wwait.attr.attr,
	&bcm2712_vpu_dma_uc_wtrans.attr.attr,
	&bcm2712_vpu_dma_uc_wmax.attr.attr,
	&bcm2712_vpu_dma_uc_rwait.attr.attr,
	&bcm2712_vpu_dma_uc_rtrans.attr.attr,
	&bcm2712_vpu_dma_uc_rmax.attr.attr,
	&bcm2712_vpu_dma_uc_rpend.attr.attr,
	&bcm2712_vpu_dma_uc_ratrans.attr.attr,
	&bcm2712_vpu_l2_in_atwait.attr.attr,
	&bcm2712_vpu_l2_in_atrans.attr.attr,
	&bcm2712_vpu_l2_in_amax.attr.attr,
	&bcm2712_vpu_l2_in_wwait.attr.attr,
	&bcm2712_vpu_l2_in_wtrans.attr.attr,
	&bcm2712_vpu_l2_in_wmax.attr.attr,
	&bcm2712_vpu_l2_in_rwait.attr.attr,
	&bcm2712_vpu_l2_in_rtrans.attr.attr,
	&bcm2712_vpu_l2_in_rmax.attr.attr,
	&bcm2712_vpu_l2_in_rpend.attr.attr,
	&bcm2712_vpu_l2_in_ratrans.attr.attr,
	NULL,
};

static struct attribute *bcm2712d0_events[] = {
	&bcm2712d0_vpu_uc_atwait.attr.attr,
	&bcm2712d0_vpu_uc_atrans.attr.attr,
	&bcm2712d0_vpu_uc_amax.attr.attr,
	&bcm2712d0_vpu_uc_wwait.attr.attr,
	&bcm2712d0_vpu_uc_wtrans.attr.attr,
	&bcm2712d0_vpu_uc_wmax.attr.attr,
	&bcm2712d0_vpu_uc_rwait.attr.attr,
	&bcm2712d0_vpu_uc_rtrans.attr.attr,
	&bcm2712d0_vpu_uc_rmax.attr.attr,
	&bcm2712d0_vpu_uc_rpend.attr.attr,
	&bcm2712d0_vpu_uc_ratrans.attr.attr,
	&bcm2712d0_display_atwait.attr.attr,
	&bcm2712d0_display_atrans.attr.attr,
	&bcm2712d0_display_amax.attr.attr,
	&bcm2712d0_display_wwait.attr.attr,
	&bcm2712d0_display_wtrans.attr.attr,
	&bcm2712d0_display_wmax.attr.attr,
	&bcm2712d0_display_rwait.attr.attr,
	&bcm2712d0_display_rtrans.attr.attr,
	&bcm2712d0_display_rmax.attr.attr,
	&bcm2712d0_display_rpend.attr.attr,
	&bcm2712d0_display_ratrans.attr.attr,
	&bcm2712d0_v3d_atwait.attr.attr,
	&bcm2712d0_v3d_atrans.attr.attr,
	&bcm2712d0_v3d_amax.attr.attr,
	&bcm2712d0_v3d_wwait.attr.attr,
	&bcm2712d0_v3d_wtrans.attr.attr,
	&bcm2712d0_v3d_wmax.attr.attr,
	&bcm2712d0_v3d_rwait.attr.attr,
	&bcm2712d0_v3d_rtrans.attr.attr,
	&bcm2712d0_v3d_rmax.attr.attr,
	&bcm2712d0_v3d_rpend.attr.attr,
	&bcm2712d0_v3d_ratrans.attr.attr,
	&bcm2712d0_arm_atwait.attr.attr,
	&bcm2712d0_arm_atrans.attr.attr,
	&bcm2712d0_arm_amax.attr.attr,
	&bcm2712d0_arm_wwait.attr.attr,
	&bcm2712d0_arm_wtrans.attr.attr,
	&bcm2712d0_arm_wmax.attr.attr,
	&bcm2712d0_arm_rwait.attr.attr,
	&bcm2712d0_arm_rtrans.attr.attr,
	&bcm2712d0_arm_rmax.attr.attr,
	&bcm2712d0_arm_rpend.attr.attr,
	&bcm2712d0_arm_ratrans.attr.attr,
	&bcm2712d0_pcie_02_atwait.attr.attr,
	&bcm2712d0_pcie_02_atrans.attr.attr,
	&bcm2712d0_pcie_02_amax.attr.attr,
	&bcm2712d0_pcie_02_wwait.attr.attr,
	&bcm2712d0_pcie_02_wtrans.attr.attr,
	&bcm2712d0_pcie_02_wmax.attr.attr,
	&bcm2712d0_pcie_02_rwait.attr.attr,
	&bcm2712d0_pcie_02_rtrans.attr.attr,
	&bcm2712d0_pcie_02_rmax.attr.attr,
	&bcm2712d0_pcie_02_rpend.attr.attr,
	&bcm2712d0_pcie_02_ratrans.attr.attr,
	&bcm2712d0_argon_atwait.attr.attr,
	&bcm2712d0_argon_atrans.attr.attr,
	&bcm2712d0_argon_amax.attr.attr,
	&bcm2712d0_argon_wwait.attr.attr,
	&bcm2712d0_argon_wtrans.attr.attr,
	&bcm2712d0_argon_wmax.attr.attr,
	&bcm2712d0_argon_rwait.attr.attr,
	&bcm2712d0_argon_rtrans.attr.attr,
	&bcm2712d0_argon_rmax.attr.attr,
	&bcm2712d0_argon_rpend.attr.attr,
	&bcm2712d0_argon_ratrans.attr.attr,
	&bcm2712d0_emmc2_atwait.attr.attr,
	&bcm2712d0_emmc2_atrans.attr.attr,
	&bcm2712d0_emmc2_amax.attr.attr,
	&bcm2712d0_emmc2_wwait.attr.attr,
	&bcm2712d0_emmc2_wtrans.attr.attr,
	&bcm2712d0_emmc2_wmax.attr.attr,
	&bcm2712d0_emmc2_rwait.attr.attr,
	&bcm2712d0_emmc2_rtrans.attr.attr,
	&bcm2712d0_emmc2_rmax.attr.attr,
	&bcm2712d0_emmc2_rpend.attr.attr,
	&bcm2712d0_emmc2_ratrans.attr.attr,
	&bcm2712d0_sd_dma_atwait.attr.attr,
	&bcm2712d0_sd_dma_atrans.attr.attr,
	&bcm2712d0_sd_dma_amax.attr.attr,
	&bcm2712d0_sd_dma_wwait.attr.attr,
	&bcm2712d0_sd_dma_wtrans.attr.attr,
	&bcm2712d0_sd_dma_wmax.attr.attr,
	&bcm2712d0_sd_dma_rwait.attr.attr,
	&bcm2712d0_sd_dma_rtrans.attr.attr,
	&bcm2712d0_sd_dma_rmax.attr.attr,
	&bcm2712d0_sd_dma_rpend.attr.attr,
	&bcm2712d0_sd_dma_ratrans.attr.attr,
	&bcm2712d0_per_atwait.attr.attr,
	&bcm2712d0_per_atrans.attr.attr,
	&bcm2712d0_per_amax.attr.attr,
	&bcm2712d0_per_wwait.attr.attr,
	&bcm2712d0_per_wtrans.attr.attr,
	&bcm2712d0_per_wmax.attr.attr,
	&bcm2712d0_per_rwait.attr.attr,
	&bcm2712d0_per_rtrans.attr.attr,
	&bcm2712d0_per_rmax.attr.attr,
	&bcm2712d0_per_rpend.attr.attr,
	&bcm2712d0_per_ratrans.attr.attr,
	&bcm2712d0_system_l2_atwait.attr.attr,
	&bcm2712d0_system_l2_atrans.attr.attr,
	&bcm2712d0_system_l2_amax.attr.attr,
	&bcm2712d0_system_l2_wwait.attr.attr,
	&bcm2712d0_system_l2_wtrans.attr.attr,
	&bcm2712d0_system_l2_wmax.attr.attr,
	&bcm2712d0_system_l2_rwait.attr.attr,
	&bcm2712d0_system_l2_rtrans.attr.attr,
	&bcm2712d0_system_l2_rmax.attr.attr,
	&bcm2712d0_system_l2_rpend.attr.attr,
	&bcm2712d0_system_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_atwait.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_atrans.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_amax.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_wwait.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_wtrans.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_wmax.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_rwait.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_rtrans.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_rmax.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_rpend.attr.attr,
	&bcm2712_vpu_vpu1_d_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_atwait.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_atrans.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_amax.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_wwait.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_wtrans.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_wmax.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_rwait.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_rtrans.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_rmax.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_rpend.attr.attr,
	&bcm2712_vpu_vpu0_d_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_atwait.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_atrans.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_amax.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_wwait.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_wtrans.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_wmax.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_rwait.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_rtrans.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_rmax.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_rpend.attr.attr,
	&bcm2712_vpu_vpu1_i_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_atwait.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_atrans.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_amax.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_wwait.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_wtrans.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_wmax.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_rwait.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_rtrans.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_rmax.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_rpend.attr.attr,
	&bcm2712_vpu_vpu0_i_l2_ratrans.attr.attr,
	&bcm2712_vpu_system_l2_atwait.attr.attr,
	&bcm2712_vpu_system_l2_atrans.attr.attr,
	&bcm2712_vpu_system_l2_amax.attr.attr,
	&bcm2712_vpu_system_l2_wwait.attr.attr,
	&bcm2712_vpu_system_l2_wtrans.attr.attr,
	&bcm2712_vpu_system_l2_wmax.attr.attr,
	&bcm2712_vpu_system_l2_rwait.attr.attr,
	&bcm2712_vpu_system_l2_rtrans.attr.attr,
	&bcm2712_vpu_system_l2_rmax.attr.attr,
	&bcm2712_vpu_system_l2_rpend.attr.attr,
	&bcm2712_vpu_system_l2_ratrans.attr.attr,
	&bcm2712_vpu_dma_l2_atwait.attr.attr,
	&bcm2712_vpu_dma_l2_atrans.attr.attr,
	&bcm2712_vpu_dma_l2_amax.attr.attr,
	&bcm2712_vpu_dma_l2_wwait.attr.attr,
	&bcm2712_vpu_dma_l2_wtrans.attr.attr,
	&bcm2712_vpu_dma_l2_wmax.attr.attr,
	&bcm2712_vpu_dma_l2_rwait.attr.attr,
	&bcm2712_vpu_dma_l2_rtrans.attr.attr,
	&bcm2712_vpu_dma_l2_rmax.attr.attr,
	&bcm2712_vpu_dma_l2_rpend.attr.attr,
	&bcm2712_vpu_dma_l2_ratrans.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_amax.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu1_d_uc_ratrans.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_amax.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu0_d_uc_ratrans.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_amax.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu1_i_uc_ratrans.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_amax.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu0_i_uc_ratrans.attr.attr,
	&bcm2712_vpu_vpu_uc_atwait.attr.attr,
	&bcm2712_vpu_vpu_uc_atrans.attr.attr,
	&bcm2712_vpu_vpu_uc_amax.attr.attr,
	&bcm2712_vpu_vpu_uc_wwait.attr.attr,
	&bcm2712_vpu_vpu_uc_wtrans.attr.attr,
	&bcm2712_vpu_vpu_uc_wmax.attr.attr,
	&bcm2712_vpu_vpu_uc_rwait.attr.attr,
	&bcm2712_vpu_vpu_uc_rtrans.attr.attr,
	&bcm2712_vpu_vpu_uc_rmax.attr.attr,
	&bcm2712_vpu_vpu_uc_rpend.attr.attr,
	&bcm2712_vpu_vpu_uc_ratrans.attr.attr,
	&bcm2712_vpu_l2_out_atwait.attr.attr,
	&bcm2712_vpu_l2_out_atrans.attr.attr,
	&bcm2712_vpu_l2_out_amax.attr.attr,
	&bcm2712_vpu_l2_out_wwait.attr.attr,
	&bcm2712_vpu_l2_out_wtrans.attr.attr,
	&bcm2712_vpu_l2_out_wmax.attr.attr,
	&bcm2712_vpu_l2_out_rwait.attr.attr,
	&bcm2712_vpu_l2_out_rtrans.attr.attr,
	&bcm2712_vpu_l2_out_rmax.attr.attr,
	&bcm2712_vpu_l2_out_rpend.attr.attr,
	&bcm2712_vpu_l2_out_ratrans.attr.attr,
	&bcm2712_vpu_dma_uc_atwait.attr.attr,
	&bcm2712_vpu_dma_uc_atrans.attr.attr,
	&bcm2712_vpu_dma_uc_amax.attr.attr,
	&bcm2712_vpu_dma_uc_wwait.attr.attr,
	&bcm2712_vpu_dma_uc_wtrans.attr.attr,
	&bcm2712_vpu_dma_uc_wmax.attr.attr,
	&bcm2712_vpu_dma_uc_rwait.attr.attr,
	&bcm2712_vpu_dma_uc_rtrans.attr.attr,
	&bcm2712_vpu_dma_uc_rmax.attr.attr,
	&bcm2712_vpu_dma_uc_rpend.attr.attr,
	&bcm2712_vpu_dma_uc_ratrans.attr.attr,
	&bcm2712_vpu_l2_in_atwait.attr.attr,
	&bcm2712_vpu_l2_in_atrans.attr.attr,
	&bcm2712_vpu_l2_in_amax.attr.attr,
	&bcm2712_vpu_l2_in_wwait.attr.attr,
	&bcm2712_vpu_l2_in_wtrans.attr.attr,
	&bcm2712_vpu_l2_in_wmax.attr.attr,
	&bcm2712_vpu_l2_in_rwait.attr.attr,
	&bcm2712_vpu_l2_in_rtrans.attr.attr,
	&bcm2712_vpu_l2_in_rmax.attr.attr,
	&bcm2712_vpu_l2_in_rpend.attr.attr,
	&bcm2712_vpu_l2_in_ratrans.attr.attr,
	NULL,
};

static const struct attribute_group rpi_axi_pmu_bcm2712_events_group = {
	.name = "events",
	.attrs = bcm2712_events,
	.is_visible = rpi_axi_pmu_events_is_visible,
};

static const struct attribute_group rpi_axi_pmu_bcm2712d0_events_group = {
	.name = "events",
	.attrs = bcm2712d0_events,
	.is_visible = rpi_axi_pmu_events_is_visible,
};

static const struct attribute_group *rpi_axi_pmu_bcm2835_attr_groups[] = {
	&rpi_axi_pmu_format_group,
	&rpi_axi_pmu_bcm2835_events_group,
	&rpi_axi_pmu_cpumask_group,
	NULL,
};

static const struct attribute_group *rpi_axi_pmu_bcm2711_attr_groups[] = {
	&rpi_axi_pmu_format_group,
	&rpi_axi_pmu_bcm2711_events_group,
	&rpi_axi_pmu_cpumask_group,
	NULL,
};

static const struct attribute_group *rpi_axi_pmu_bcm2712_attr_groups[] = {
	&rpi_axi_pmu_format_group,
	&rpi_axi_pmu_bcm2712_events_group,
	&rpi_axi_pmu_cpumask_group,
	NULL,
};

static const struct attribute_group *rpi_axi_pmu_bcm2712d0_attr_groups[] = {
	&rpi_axi_pmu_format_group,
	&rpi_axi_pmu_bcm2712d0_events_group,
	&rpi_axi_pmu_cpumask_group,
	NULL,
};

/**
 * rpi_axi_pmu__validate_event() - Validates single event resource capability
 * @pmu: Pointer to core PMU struct
 * @fake_hw_events: Temporary array of watcher structures for pre-flight check
 * @event: Pointer to perf_event being validated
 * @num_events: Running count of PMU events in the group
 *
 * Return: true if event can be scheduled, false otherwise.
 */
static bool rpi_axi_pmu__validate_event(struct pmu *pmu,
					struct rpi_axi_hw_events fake_hw_events[MON_MAX],
					struct perf_event *event, int *num_events)
{
	enum monitor mon;

	if (is_software_event(event))
		return true;
	if (event->pmu != pmu)
		return false;
	if (++(*num_events) > RPI_AXI_MAX_EVENTS)
		return false;
	mon = config_to_monitor(event->attr.config);
	return rpi_axi_hw_events__get_alloc_event_idx(&fake_hw_events[mon], event) >= 0;
}

/**
 * rpi_axi_pmu__validate_group() - Validates event group scheduleability
 * @event: Pointer to leader or sibling perf_event
 *
 * Simulates bus watcher allocation across both monitors to guarantee all events
 * in the group can run simultaneously.
 *
 * Return: true if group is valid, false otherwise.
 */
static bool rpi_axi_pmu__validate_group(struct perf_event *event)
{
	struct perf_event *sibling, *leader = event->group_leader;
	struct rpi_axi_hw_events fake_hw_events[MON_MAX];
	int num_events = 0;

	if (leader == event)
		return true;

	rpi_axi_hw_events__init(&fake_hw_events[MON_SYSTEM]);
	rpi_axi_hw_events__init(&fake_hw_events[MON_VPU]);
	if (!rpi_axi_pmu__validate_event(event->pmu, fake_hw_events, leader, &num_events))
		return false;
	for_each_sibling_event(sibling, leader) {
		if (!rpi_axi_pmu__validate_event(event->pmu, fake_hw_events, sibling, &num_events))
			return false;
	}
	return rpi_axi_pmu__validate_event(event->pmu, fake_hw_events, event, &num_events);
}

/**
 * rpi_axi_pmu_event_init() - Perf callback to initialize a perf_event
 * @event: Pointer to perf_event to initialize
 *
 * Return: 0 on success, negative error code on failure.
 */
static int rpi_axi_pmu_event_init(struct perf_event *event)
{
	struct rpi_axi_pmu *pmu;
	struct device *dev;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;
	pmu = pmu_to_rpi_axi_pmu(event->pmu);
	dev = pmu->pmu.dev;
	if (!config_is_valid(pmu, event->attr.config) ||
	    event->attr.config1 || event->attr.config2) {
		dev_dbg(dev, "Invalid event config\n");
		return -EINVAL;
	}

	if (is_sampling_event(event)) {
		dev_dbg(dev, "Sampling not supported\n");
		return -EOPNOTSUPP;
	}

	if (event->cpu < 0 || event->attach_state & PERF_ATTACH_TASK) {
		dev_dbg(dev, "Per-task data not supported\n");
		return -EOPNOTSUPP;
	}

	if (event->cpu != pmu->cpu) {
		dev_dbg(dev, "Can only bind to PMU CPU %d\n", pmu->cpu);
		return -EINVAL;
	}

	if (!rpi_axi_pmu__validate_group(event)) {
		dev_dbg(dev, "Invalid event or grouping of events\n");
		return -EINVAL;
	}
	event->hw.idx = -1;
	return 0;
}

/**
 * set_monitor_control() - Writes to global monitor control register
 * @pmu: Pointer to PMU context
 * @mon: Monitor selection (MON_SYSTEM or MON_VPU)
 * @set: Control bitmask to write
 */
static void set_monitor_control(struct rpi_axi_pmu *pmu, enum monitor mon, u32 set)
{
	if (pmu->monitor[mon].use_mailbox_interface) {
		__le32 tmp[3] = {
			cpu_to_le32(pmu->monitor[mon].mailbox + GEN_CTRL),
			cpu_to_le32(1),
			cpu_to_le32(set),
		};
		int err;

		might_sleep();
		lockdep_assert_held(&pmu->vpu_mutex);
		if (WARN_ON_ONCE(in_interrupt() || irqs_disabled()))
			return;
		err = rpi_firmware_property(pmu->firmware,
					    RPI_FIRMWARE_SET_PERIPH_REG,
					    tmp, sizeof(tmp));
		if (err < 0 || le32_to_cpu(tmp[1]) != 1)
			dev_err(&pmu->pdev->dev, "Failed to set monitor control\n");
	} else {
		lockdep_assert_held(&pmu->lock);
		writel(set, pmu->monitor[mon].base_address + GEN_CTRL);
	}
}

static int watcher_offset(int idx)
{
	return BW0_CTRL + idx * BW_STRIDE;
}

/**
 * set_bus_watcher_control() - Writes to a specific bus watcher control register
 * @pmu: Pointer to PMU context
 * @mon: Monitor selection
 * @idx: Watcher index (0..2)
 * @set: Control bitmask to write
 */
static void set_bus_watcher_control(struct rpi_axi_pmu *pmu, enum monitor mon, int idx, u32 set)
{
	int watcher = watcher_offset(idx);

	if (pmu->monitor[mon].use_mailbox_interface) {
		__le32 tmp[3] = {
			cpu_to_le32(pmu->monitor[mon].mailbox + watcher),
			cpu_to_le32(1),
			cpu_to_le32(set),
		};
		int err;

		might_sleep();
		lockdep_assert_held(&pmu->vpu_mutex);
		if (WARN_ON_ONCE(in_interrupt() || irqs_disabled()))
			return;
		err = rpi_firmware_property(pmu->firmware,
					    RPI_FIRMWARE_SET_PERIPH_REG,
					    tmp, sizeof(tmp));
		if (err < 0 || le32_to_cpu(tmp[1]) != 1)
			dev_err(&pmu->pdev->dev, "Failed to set bus watcher control\n");
	} else {
		lockdep_assert_held(&pmu->lock);
		writel(set, pmu->monitor[mon].base_address + watcher);
	}
}

static u32 rpi_axi_pmu_filter_bits(struct rpi_axi_pmu *pmu, enum monitor mon, int filter)
{
	if (pmu->axi_id_mask && mon == MON_VPU)
		return FIELD_PREP(BW_CTRL_VPU_ID, filter) |
		       FIELD_PREP(BW_CTRL_VPU_ID_MASK, GENMASK(5, 0));
	if (pmu->axi_id_mask)
		return FIELD_PREP(BW_CTRL_AXI_ID, FIELD_PREP(AXI_ID_MASTER, filter)) |
		       FIELD_PREP(BW_CTRL_AXI_ID_MASK, AXI_ID_MASTER);
	if (pmu->chip == CHIP_BCM2712)
		return FIELD_PREP(BW_CTRL_2712_FILTER_MASK, filter);
	return FIELD_PREP(BW_CTRL_BUS_FILTER_MASK, filter);
}

/**
 * rpi_axi_pmu_enable_bus_watcher() - Enables a hardware bus watcher unit
 * @pmu: Pointer to PMU context
 * @mon: Monitor selection
 * @idx: Watcher index (0..2)
 * @bus: Monitored AXI bus index
 * @filter: AXI master ID filter
 */
static void rpi_axi_pmu_enable_bus_watcher(struct rpi_axi_pmu *pmu, enum monitor mon,
					    int idx, int bus, int filter)
{
	int bus_control;

	if (READ_ONCE(pmu->monitor[mon].hw_events.enabled[idx]))
		return;
	bus_control = BW_CTRL_ENABLE_BIT |
		      ((bus << BW_CTRL_BUS_WATCH_SHIFT) & BW_CTRL_BUS_WATCH_MASK);
	if (filter) {
		bus_control |= BW_CTRL_ENABLE_ID_FILTER_BIT;
		bus_control |= rpi_axi_pmu_filter_bits(pmu, mon, filter);
	}
	if (!pmu->monitor[mon].hw_events.monitor_running) {
		set_monitor_control(pmu, mon, GEN_CTL_RESET_BIT);
		set_monitor_control(pmu, mon, GEN_CTL_ENABLE_BIT | GEN_CTL_WATCH_BIT);
	}
	set_bus_watcher_control(pmu, mon, idx, BW_CTRL_RESET_BIT);
	set_bus_watcher_control(pmu, mon, idx, bus_control);
}

/**
 * rpi_axi_pmu_disable_bus_watcher() - Resets and disables a hardware bus watcher unit
 * @pmu: Pointer to PMU context
 * @mon: Monitor selection
 * @idx: Watcher index (0..2)
 */
static void rpi_axi_pmu_disable_bus_watcher(struct rpi_axi_pmu *pmu, enum monitor mon, int idx)
{
	set_bus_watcher_control(pmu, mon, idx, BW_CTRL_RESET_BIT);
}

static int counter_offset(enum counter counter)
{
	switch (counter) {
	case CNT_ATRANS: return BW_ATRANS_OFFSET;
	case CNT_ATWAIT: return BW_ATWAIT_OFFSET;
	case CNT_AMAX: return BW_AMAX_OFFSET;
	case CNT_WTRANS: return BW_WTRANS_OFFSET;
	case CNT_WWAIT: return BW_WTWAIT_OFFSET;
	case CNT_WMAX: return BW_WMAX_OFFSET;
	case CNT_RTRANS: return BW_RTRANS_OFFSET;
	case CNT_RWAIT: return BW_RTWAIT_OFFSET;
	case CNT_RMAX: return BW_RMAX_OFFSET;
	case CNT_RATRANS: return BW_RATRANS_OFFSET;
	case CNT_RPEND: return BW_RPEND_OFFSET;
	default: return 0;
	}
}

static u32 counter_width_mask(enum counter counter)
{
	switch (counter) {
	case CNT_ATWAIT:
	case CNT_AMAX:
	case CNT_WWAIT:
	case CNT_WMAX:
	case CNT_RWAIT:
	case CNT_RMAX:
		return GENMASK(27, 0);
	case CNT_RPEND:
		return GENMASK(7, 0);
	default:
		return U32_MAX;
	}
}

/*
 * The max latency (CNT_AMAX, CNT_WMAX, CNT_RMAX) and pending read (CNT_RPEND)
 * registers hold a hardware level/watermark rather than a running transaction
 * or cycle count:
 * - CNT_RPEND is an instantaneous register holding the number of reads
 *   currently pending at the time of the register read.
 * - CNT_AMAX, CNT_WMAX, and CNT_RMAX are sticky hardware peak-hold registers
 *   that retain the maximum latency observed since the bus watcher was started
 *   (pulsing BW_CTRL_RESET_BIT on read would reset concurrent transaction and
 *   wait counters sharing the same bus watcher).
 *
 * Because userspace perf tools (such as 'perf stat' and 'perf stat -I') treat
 * event->count as a monotonically non-decreasing cumulative counter and compute
 * per-interval values by subtracting the event->count value read at the end of
 * the previous interval, we cannot simply overwrite event->count with the raw
 * register value (which would underflow u64 when the level decreases between
 * intervals, or report 0 when the peak latency stays constant) nor add the raw
 * level on every 100ms timer poll.
 *
 * Instead, for level counters, event->hw.prev_count tracks the maximum level
 * observed since the event was started or last read by userspace (reset to 0 in
 * rpi_axi_pmu_start() and rpi_axi_pmu_read()). Whenever a sample exceeds
 * prev_count, only the increment (new_count - prev_count) is added to
 * event->count. Thus, across any read interval starting at baseline C,
 * event->count equals C + max(samples in interval), and subtracting the
 * previous read's baseline C yields the maximum level observed during that
 * interval (per-interval peak for CNT_RPEND, and current hardware watermark for
 * CNT_AMAX/WMAX/RMAX).
 */
static bool counter_is_level(enum counter counter)
{
	return counter == CNT_AMAX || counter == CNT_WMAX ||
	       counter == CNT_RMAX || counter == CNT_RPEND;
}

static void rpi_axi_pmu_event_update(struct perf_event *event, u32 new_count)
{
	enum counter counter = config_to_counter(event->attr.config);
	u32 mask = counter_width_mask(counter);
	u32 prev_count = local64_read(&event->hw.prev_count);

	if (counter_is_level(counter)) {
		new_count &= mask;
		if (new_count > prev_count) {
			local64_set(&event->hw.prev_count, new_count);
			local64_add(new_count - prev_count, &event->count);
		}
	} else {
		local64_set(&event->hw.prev_count, new_count);
		local64_add((new_count - prev_count) & mask, &event->count);
	}
}

/**
 * rpi_axi_pmu_read_counter() - Reads raw 32-bit hardware counter from watcher
 * @pmu: Pointer to PMU context
 * @mon: Monitor selection
 * @idx: Watcher index (0..2)
 * @counter: Metric counter type (enum counter, 0..8)
 *
 * Return: 32-bit raw hardware counter value.
 */
static u32 rpi_axi_pmu_read_counter(struct rpi_axi_pmu *pmu, enum monitor mon, int idx,
				    enum counter counter)
{
	int watcher = watcher_offset(idx);
	int offset = counter_offset(counter);

	if (!READ_ONCE(pmu->monitor[mon].hw_events.enabled[idx]))
		return 0;
	lockdep_assert_held(&pmu->lock);
	return readl(pmu->monitor[mon].base_address + watcher + offset);
}

/* All three VPU bus watchers, from BW0_CTRL to BW2_RATRANS, in one mailbox call */
#define VPU_READ_WORDS	((BW2_CTRL + BW_RATRANS_OFFSET - BW0_CTRL) / 4 + 1)

static int rpi_axi_pmu_vpu_read_watchers(struct rpi_axi_pmu *pmu, u32 *regs)
{
	__le32 tmp[2 + VPU_READ_WORDS] = {
		cpu_to_le32(pmu->monitor[MON_VPU].mailbox + BW0_CTRL),
		cpu_to_le32(VPU_READ_WORDS),
	};
	int err;

	lockdep_assert_held(&pmu->vpu_mutex);
	err = rpi_firmware_property(pmu->firmware, RPI_FIRMWARE_GET_PERIPH_REG,
				    tmp, sizeof(tmp));
	if (err || le32_to_cpu(tmp[1]) != VPU_READ_WORDS) {
		dev_err_ratelimited(&pmu->pdev->dev, "Failed to read bus watchers\n");
		return -EIO;
	}
	for (int i = 0; i < VPU_READ_WORDS; i++)
		regs[i] = le32_to_cpu(tmp[2 + i]);
	return 0;
}

/**
 * rpi_axi_pmu_update() - Samples hardware counter and updates event->count
 * @event: Pointer to perf_event being updated
 * @reset_level: True on userspace read() to reset the level counter baseline
 *
 * Thread-safe counter update implementation:
 * - System Monitor (MON_SYSTEM): Uses fast MMIO (~15ns), serialized via pmu->lock spinlock.
 * - VPU Monitor (MON_VPU): Async background polling in process context (vpu_work).
 */
static void rpi_axi_pmu_update(struct perf_event *event, bool reset_level)
{
	struct rpi_axi_pmu *pmu = pmu_to_rpi_axi_pmu(event->pmu);
	enum monitor mon = config_to_monitor(event->attr.config);
	enum counter counter = config_to_counter(event->attr.config);
	unsigned long flags;

	raw_spin_lock_irqsave(&pmu->lock, flags);
	/* Mailbox VPU counters are polled asynchronously in background vpu_work.
	 * MMIO monitors (System) are read synchronously.
	 */
	if (!pmu->monitor[mon].use_mailbox_interface &&
	    event->hw.idx >= 0 &&
	    pmu->monitor[mon].hw_events.enabled[event->hw.idx] &&
	    !(event->hw.state & PERF_HES_STOPPED) &&
	    (event->hw.state & PERF_HES_UPTODATE)) {
		u32 new_count = rpi_axi_pmu_read_counter(pmu, mon, event->hw.idx, counter);

		rpi_axi_pmu_event_update(event, new_count);
	}
	if (reset_level && counter_is_level(counter))
		local64_set(&event->hw.prev_count, 0);
	raw_spin_unlock_irqrestore(&pmu->lock, flags);
}

/**
 * rpi_axi_pmu_read() - Perf callback to update event counter value on userspace read
 * @event: Pointer to perf_event being read
 *
 * Updates the event count and, for level counters, resets event->hw.prev_count
 * to 0 under pmu->lock so the current event->count serves as the baseline for
 * the next read interval.
 */
static void rpi_axi_pmu_read(struct perf_event *event)
{
	rpi_axi_pmu_update(event, true);
}

/**
 * rpi_axi_pmu_vpu_work_handler() - Background work handler for VPU monitor counter reads
 * @work: Pointer to work_struct inside struct rpi_axi_pmu
 *
 * Runs in process context under vpu_mutex. Programs newly allocated VPU bus
 * watchers, then reads all of them in a single mailbox call and updates the
 * VPU events. A watcher reassigned while the lock was dropped is detected by
 * its config_gen and skipped. A newly started event takes its first read as
 * its baseline (PERF_HES_UPTODATE).
 */
static void rpi_axi_pmu_vpu_work_handler(struct work_struct *work)
{
	struct rpi_axi_pmu *pmu = container_of(work, struct rpi_axi_pmu, vpu_work);
	struct rpi_axi_hw_events *hw = &pmu->monitor[MON_VPU].hw_events;
	unsigned int gen[NUM_BUS_WATCHERS_PER_MONITOR];
	bool valid[NUM_BUS_WATCHERS_PER_MONITOR];
	u32 regs[VPU_READ_WORDS];
	bool any = false;

	might_sleep();
	mutex_lock(&pmu->vpu_mutex);
	raw_spin_lock_irq(&pmu->lock);
	for (int idx = 0; idx < NUM_BUS_WATCHERS_PER_MONITOR; idx++) {
		if (hw->vpu_disable_pending[idx]) {
			hw->vpu_disable_pending[idx] = false;
			raw_spin_unlock_irq(&pmu->lock);
			rpi_axi_pmu_disable_bus_watcher(pmu, MON_VPU, idx);
			raw_spin_lock_irq(&pmu->lock);
		}
		if (hw->monitored_bus[idx] >= 0 && !hw->enabled[idx]) {
			int bus = hw->monitored_bus[idx];
			int filter = hw->filter[idx];
			unsigned int g = hw->config_gen[idx];

			raw_spin_unlock_irq(&pmu->lock);
			rpi_axi_pmu_enable_bus_watcher(pmu, MON_VPU, idx, bus, filter);
			raw_spin_lock_irq(&pmu->lock);
			hw->monitor_running = true;
			/* Leave it disabled if the watcher was reassigned meanwhile */
			if (hw->config_gen[idx] == g)
				WRITE_ONCE(hw->enabled[idx], true);
		}
		gen[idx] = hw->config_gen[idx];
		valid[idx] = hw->enabled[idx];
		any |= valid[idx];
	}

	if (any) {
		int err;

		raw_spin_unlock_irq(&pmu->lock);
		err = rpi_axi_pmu_vpu_read_watchers(pmu, regs);
		raw_spin_lock_irq(&pmu->lock);

		for (int i = 0; !err && i < RPI_AXI_MAX_EVENTS; i++) {
			struct perf_event *event = pmu->events[i];
			enum counter counter;
			int idx;
			u32 new_count;

			if (!event || (event->hw.state & PERF_HES_STOPPED) ||
			    config_to_monitor(event->attr.config) != MON_VPU)
				continue;
			idx = event->hw.idx;
			if (idx < 0 || !valid[idx] || hw->config_gen[idx] != gen[idx])
				continue;
			counter = config_to_counter(event->attr.config);
			new_count = regs[(watcher_offset(idx) + counter_offset(counter) - BW0_CTRL) / 4];
			if (!(event->hw.state & PERF_HES_UPTODATE)) {
				event->hw.state |= PERF_HES_UPTODATE;
				if (!counter_is_level(counter)) {
					local64_set(&event->hw.prev_count, new_count);
					continue;
				}
			}
			rpi_axi_pmu_event_update(event, new_count);
		}
	}

	if (hw->num_monitored == 0 && hw->monitor_running) {
		raw_spin_unlock_irq(&pmu->lock);
		set_monitor_control(pmu, MON_VPU, GEN_CTL_RESET_BIT);
		raw_spin_lock_irq(&pmu->lock);
		hw->monitor_running = false;
	}

	raw_spin_unlock_irq(&pmu->lock);
	mutex_unlock(&pmu->vpu_mutex);
}

/**
 * rpi_axi_pmu_timer_handler() - Periodic hrtimer callback for 32-bit counter overflow polling
 * @timer: Pointer to hrtimer structure
 *
 * Runs in softirq / atomic context.
 * Holding pmu->lock, reads System monitor counters (MON_SYSTEM) via fast MMIO.
 * Schedules background work (vpu_work) if active_vpu_events > 0.
 *
 * Return: HRTIMER_RESTART.
 */
static enum hrtimer_restart rpi_axi_pmu_timer_handler(struct hrtimer *timer)
{
	struct rpi_axi_pmu *pmu = container_of(timer, struct rpi_axi_pmu, hrtimer);
	unsigned long flags;

	raw_spin_lock_irqsave(&pmu->lock, flags);
	if (pmu->active_events == 0) {
		raw_spin_unlock_irqrestore(&pmu->lock, flags);
		return HRTIMER_NORESTART;
	}
	for (int i = 0; i < RPI_AXI_MAX_EVENTS; i++) {
		struct perf_event *event = pmu->events[i];
		enum counter counter;
		enum monitor mon;
		u32 new_count;

		if (!event || (event->hw.state & PERF_HES_STOPPED))
			continue;
		mon = config_to_monitor(event->attr.config);
		if (pmu->monitor[mon].use_mailbox_interface)
			continue;
		counter = config_to_counter(event->attr.config);
		new_count = rpi_axi_pmu_read_counter(pmu, mon,
						    event->hw.idx, counter);
		rpi_axi_pmu_event_update(event, new_count);
	}

	if (pmu->active_vpu_events > 0 && pmu->monitor[MON_VPU].use_mailbox_interface)
		schedule_work(&pmu->vpu_work);
	hrtimer_forward_now(timer, RPI_AXI_PMU_TIMER_INTERVAL);
	raw_spin_unlock_irqrestore(&pmu->lock, flags);
	return HRTIMER_RESTART;
}

/**
 * rpi_axi_pmu_start() - Starts monitoring on an allocated bus watcher
 * @event: Pointer to perf_event being started
 * @flags: Start control flags
 */
static void rpi_axi_pmu_start(struct perf_event *event, int flags)
{
	struct rpi_axi_pmu *pmu = pmu_to_rpi_axi_pmu(event->pmu);
	enum monitor mon = config_to_monitor(event->attr.config);
	enum counter counter = config_to_counter(event->attr.config);
	unsigned long spinflags;

	raw_spin_lock_irqsave(&pmu->lock, spinflags);
	if (event->hw.idx < 0) {
		raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
		return;
	}
	event->hw.state = 0;
	if (counter_is_level(counter))
		local64_set(&event->hw.prev_count, 0);
	if (!pmu->monitor[mon].use_mailbox_interface) {
		int bus = pmu->monitor[mon].hw_events.monitored_bus[event->hw.idx];
		int filter = pmu->monitor[mon].hw_events.filter[event->hw.idx];

		rpi_axi_pmu_enable_bus_watcher(pmu, mon, event->hw.idx, bus, filter);
		WRITE_ONCE(pmu->monitor[mon].hw_events.enabled[event->hw.idx], true);
		pmu->monitor[mon].hw_events.monitor_running = true;
		if (!counter_is_level(counter))
			local64_set(&event->hw.prev_count,
				    rpi_axi_pmu_read_counter(pmu, mon, event->hw.idx, counter));
		event->hw.state |= PERF_HES_UPTODATE;
	} else {
		schedule_work(&pmu->vpu_work);
	}
	raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
}

/**
 * rpi_axi_pmu_add() - Allocates a bus watcher resource and adds event to PMU
 * @event: Pointer to perf_event being added
 * @flags: Add control flags (e.g. PERF_EF_START)
 *
 * Return: 0 on success, -EAGAIN if no watcher slot is available.
 */
static int rpi_axi_pmu_add(struct perf_event *event, int flags)
{
	struct rpi_axi_pmu *pmu = pmu_to_rpi_axi_pmu(event->pmu);
	enum monitor mon = config_to_monitor(event->attr.config);
	struct rpi_axi_hw_events *hw_events = &pmu->monitor[mon].hw_events;
	unsigned long spinflags;
	int idx, slot = -1;

	raw_spin_lock_irqsave(&pmu->lock, spinflags);
	idx = rpi_axi_hw_events__get_alloc_event_idx(hw_events, event);
	if (idx < 0) {
		raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
		return -EAGAIN;
	}

	for (int i = 0; i < RPI_AXI_MAX_EVENTS; i++) {
		if (!pmu->events[i]) {
			slot = i;
			pmu->events[i] = event;
			break;
		}
	}

	if (slot < 0) {
		hw_events->refcount[idx]--;
		if (hw_events->refcount[idx] == 0) {
			hw_events->monitored_bus[idx] = -1;
			hw_events->filter[idx] = 0;
			hw_events->config_gen[idx]++;
			hw_events->num_monitored--;
			if (pmu->monitor[mon].use_mailbox_interface) {
				hw_events->vpu_disable_pending[idx] = true;
				schedule_work(&pmu->vpu_work);
			} else {
				rpi_axi_pmu_disable_bus_watcher(pmu, mon, idx);
				if (hw_events->num_monitored == 0) {
					set_monitor_control(pmu, mon, GEN_CTL_RESET_BIT);
					hw_events->monitor_running = false;
				}
			}
		}
		raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
		return -ENOSPC;
	}

	pmu->active_events++;
	if (mon == MON_VPU)
		pmu->active_vpu_events++;
	if (pmu->active_events == 1)
		hrtimer_start(&pmu->hrtimer, RPI_AXI_PMU_TIMER_INTERVAL,
			      HRTIMER_MODE_REL_SOFT);
	event->hw.idx = idx;
	event->hw.state = PERF_HES_STOPPED;
	raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
	if (flags & PERF_EF_START)
		rpi_axi_pmu_start(event, /*flags=*/0);
	return 0;
}

/**
 * rpi_axi_pmu_stop() - Stops counter monitoring for an event
 * @event: Pointer to perf_event being stopped
 * @flags: Stop control flags (e.g. PERF_EF_UPDATE)
 */
static void rpi_axi_pmu_stop(struct perf_event *event, int flags)
{
	struct rpi_axi_pmu *pmu = pmu_to_rpi_axi_pmu(event->pmu);
	enum monitor mon = config_to_monitor(event->attr.config);
	unsigned long spinflags;

	if (event->hw.state & PERF_HES_STOPPED)
		return;
	if (flags & PERF_EF_UPDATE)
		rpi_axi_pmu_update(event, false);
	raw_spin_lock_irqsave(&pmu->lock, spinflags);
	if ((flags & PERF_EF_UPDATE) && !pmu->monitor[mon].use_mailbox_interface)
		event->hw.state |= PERF_HES_UPTODATE;
	event->hw.state |= PERF_HES_STOPPED;
	raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
}

/**
 * rpi_axi_pmu_del() - Deallocates watcher resource and removes event from PMU
 * @event: Pointer to perf_event being removed
 * @flags: Delete control flags
 */
static void rpi_axi_pmu_del(struct perf_event *event, int flags)
{
	struct rpi_axi_pmu *pmu = pmu_to_rpi_axi_pmu(event->pmu);
	enum monitor mon = config_to_monitor(event->attr.config);
	unsigned long spinflags;
	int idx = event->hw.idx;

	if (idx < 0)
		return;
	rpi_axi_pmu_stop(event, PERF_EF_UPDATE);
	raw_spin_lock_irqsave(&pmu->lock, spinflags);
	for (int i = 0; i < RPI_AXI_MAX_EVENTS; i++) {
		if (pmu->events[i] == event) {
			pmu->events[i] = NULL;
			break;
		}
	}

	if (pmu->monitor[mon].hw_events.monitored_bus[idx] >= 0) {
		pmu->monitor[mon].hw_events.refcount[idx]--;
		if (pmu->monitor[mon].hw_events.refcount[idx] == 0) {
			pmu->monitor[mon].hw_events.monitored_bus[idx] = -1;
			pmu->monitor[mon].hw_events.filter[idx] = 0;
			pmu->monitor[mon].hw_events.config_gen[idx]++;
			pmu->monitor[mon].hw_events.num_monitored--;
			if (!pmu->monitor[mon].use_mailbox_interface) {
				WRITE_ONCE(pmu->monitor[mon].hw_events.enabled[idx], false);
				rpi_axi_pmu_disable_bus_watcher(pmu, mon, idx);
				if (pmu->monitor[mon].hw_events.num_monitored == 0) {
					set_monitor_control(pmu, mon, GEN_CTL_RESET_BIT);
					pmu->monitor[mon].hw_events.monitor_running = false;
				}
			} else {
				WRITE_ONCE(pmu->monitor[mon].hw_events.enabled[idx], false);
				pmu->monitor[mon].hw_events.vpu_disable_pending[idx] = true;
				schedule_work(&pmu->vpu_work);
			}
		}
	}

	event->hw.idx = -1;
	pmu->active_events--;
	if (mon == MON_VPU)
		pmu->active_vpu_events--;
	raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
}

/**
 * rpi_axi_pmu_online_cpu() - CPU hotplug callback when a CPU comes online
 * @cpu: CPU core number coming online
 * @node: Pointer to hlist_node inside struct rpi_axi_pmu
 *
 * Return: 0.
 */
static int rpi_axi_pmu_online_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct rpi_axi_pmu *pmu = hlist_entry_safe(node, struct rpi_axi_pmu, cpuhp_node);

	if (pmu->cpu == -1)
		pmu->cpu = cpu;
	return 0;
}

/**
 * rpi_axi_pmu_offline_cpu() - CPU hotplug notifier callback when designated CPU goes offline
 * @cpu: CPU core number being taken offline
 * @node: Pointer to hlist_node inside struct rpi_axi_pmu
 *
 * Return: 0.
 */
static int rpi_axi_pmu_offline_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct rpi_axi_pmu *pmu = hlist_entry_safe(node, struct rpi_axi_pmu, cpuhp_node);
	unsigned int target;

	if (cpu != pmu->cpu)
		return 0;
	target = cpumask_any_but(cpu_online_mask, cpu);
	if (target >= nr_cpu_ids) {
		pmu->cpu = -1;
		return 0;
	}
	perf_pmu_migrate_context(&pmu->pmu, cpu, target);
	pmu->cpu = target;
	return 0;
}

/*
 * Some firmware accepts GET/SET_PERIPH_REG for the VPU monitor address without
 * reaching the hardware, so check that a written enable bit reads back.
 */
static bool rpi_axi_pmu_vpu_accessible(struct rpi_axi_pmu *pmu)
{
	u32 addr = pmu->monitor[MON_VPU].mailbox + GEN_CTRL;
	__le32 tmp[3] = {
		cpu_to_le32(addr),
		cpu_to_le32(1),
		cpu_to_le32(GEN_CTL_ENABLE_BIT),
	};
	bool ok;

	if (rpi_firmware_property(pmu->firmware, RPI_FIRMWARE_SET_PERIPH_REG,
				  tmp, sizeof(tmp)) || le32_to_cpu(tmp[1]) != 1)
		return false;

	tmp[0] = cpu_to_le32(addr);
	tmp[1] = cpu_to_le32(1);
	tmp[2] = cpu_to_le32(0);
	ok = !rpi_firmware_property(pmu->firmware, RPI_FIRMWARE_GET_PERIPH_REG,
				    tmp, sizeof(tmp)) &&
	     le32_to_cpu(tmp[1]) == 1 &&
	     (le32_to_cpu(tmp[2]) & GEN_CTL_ENABLE_BIT);

	tmp[0] = cpu_to_le32(addr);
	tmp[1] = cpu_to_le32(1);
	tmp[2] = cpu_to_le32(0);
	rpi_firmware_property(pmu->firmware, RPI_FIRMWARE_SET_PERIPH_REG, tmp, sizeof(tmp));

	return ok;
}

/**
 * rpi_axi_pmu__init() - Internal PMU driver initialization called during probe
 * @pmu: Pointer to PMU context
 * @pdev: Platform device pointer
 *
 * Return: 0 on success, negative error code on failure.
 */
static int rpi_axi_pmu__init(struct rpi_axi_pmu *pmu, struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *fw_node;
	int ret;

	raw_spin_lock_init(&pmu->lock);
	mutex_init(&pmu->vpu_mutex);
	pmu->chip = (enum rpi_axi_chip)(uintptr_t)of_device_get_match_data(dev);
	pmu->pmu = (struct pmu) {
		.module = THIS_MODULE,
		.task_ctx_nr    = perf_invalid_context,
		.event_init     = rpi_axi_pmu_event_init,
		.add            = rpi_axi_pmu_add,
		.del            = rpi_axi_pmu_del,
		.start          = rpi_axi_pmu_start,
		.stop           = rpi_axi_pmu_stop,
		.read           = rpi_axi_pmu_read,
		.capabilities   = PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT,
	};
	pmu->pdev = pdev;
	hrtimer_setup(&pmu->hrtimer, rpi_axi_pmu_timer_handler, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
	INIT_WORK(&pmu->vpu_work, rpi_axi_pmu_vpu_work_handler);
	pmu->monitor[MON_SYSTEM].use_mailbox_interface = false;
	pmu->monitor[MON_VPU].use_mailbox_interface = false;
	if (IS_ENABLED(CONFIG_RASPBERRYPI_FIRMWARE)) {
		fw_node = rpi_firmware_find_node();
		if (!fw_node) {
			dev_err(dev, "Missing firmware node\n");
			return -ENOENT;
		}
		pmu->firmware = devm_rpi_firmware_get(dev, fw_node);
		of_node_put(fw_node);
		if (!pmu->firmware)
			return -EPROBE_DEFER;
		pmu->monitor[MON_VPU].use_mailbox_interface = true;
	}

	for (int i = 0; i < MON_MAX; i++) {
		rpi_axi_hw_events__init(&pmu->monitor[i].hw_events);
		if (pmu->monitor[i].use_mailbox_interface) {
			int addr_cells = of_n_addr_cells(dev->of_node);
			int size_cells = of_n_size_cells(dev->of_node);
			int index = i * (addr_cells + size_cells) + (addr_cells - 1);

			if (of_property_read_u32_index(dev->of_node, "reg", index,
						       &pmu->monitor[i].mailbox)) {
				dev_err(dev, "Error reading mailbox resource %d\n", i);
				ret = -EINVAL;
				return ret;
			}
		} else if (i == MON_SYSTEM) {
			struct resource *resource = platform_get_resource(pdev, IORESOURCE_MEM, i);

			if (!resource)
				continue;
			pmu->monitor[i].base_address = devm_ioremap_resource(dev, resource);
			if (IS_ERR(pmu->monitor[i].base_address)) {
				ret = PTR_ERR(pmu->monitor[i].base_address);
				dev_err(dev, "Error devm_ioremap_resource failed %d\n", ret);
				return ret;
			}
		}
	}

	/* Only BCM2712 D0 and later implement the AXI ID mask field */
	if (pmu->chip == CHIP_BCM2712 && pmu->monitor[MON_SYSTEM].base_address) {
		void __iomem *ctrl = pmu->monitor[MON_SYSTEM].base_address + BW0_CTRL;
		u32 old = readl(ctrl);

		writel(BW_CTRL_AXI_ID_MASK, ctrl);
		pmu->axi_id_mask = !!(readl(ctrl) & BW_CTRL_AXI_ID_MASK);
		writel(old, ctrl);
	}

	if (pmu->monitor[MON_VPU].use_mailbox_interface &&
	    !rpi_axi_pmu_vpu_accessible(pmu)) {
		dev_warn(dev, "VPU monitor not accessible through firmware\n");
		pmu->monitor[MON_VPU].use_mailbox_interface = false;
		pmu->monitor[MON_VPU].base_address = NULL;
	}

	/*
	 * VPU counters are only read every timer interval, and cannot be read when
	 * an event is stopped, so a short multiplexing slice loses most of a VPU count.
	 */
	if (pmu->monitor[MON_VPU].use_mailbox_interface)
		pmu->pmu.hrtimer_interval_ms = 10 * ktime_to_ms(RPI_AXI_PMU_TIMER_INTERVAL);

	pmu->cpu = -1;
	ret = cpuhp_state_add_instance(rpi_axi_pmu_cpuhp_state, &pmu->cpuhp_node);
	if (ret) {
		dev_err(dev, "Failed to add cpuhp instance %d\n", ret);
		goto err_teardown;
	}

	if (pmu->chip == CHIP_BCM2712 && pmu->axi_id_mask)
		pmu->pmu.attr_groups = (const struct attribute_group **)rpi_axi_pmu_bcm2712d0_attr_groups;
	else if (pmu->chip == CHIP_BCM2712)
		pmu->pmu.attr_groups = (const struct attribute_group **)rpi_axi_pmu_bcm2712_attr_groups;
	else if (pmu->chip == CHIP_BCM2711)
		pmu->pmu.attr_groups = (const struct attribute_group **)rpi_axi_pmu_bcm2711_attr_groups;
	else
		pmu->pmu.attr_groups = (const struct attribute_group **)rpi_axi_pmu_bcm2835_attr_groups;
	ret = perf_pmu_register(&pmu->pmu, PMU_NAME, /*type=*/-1);
	if (ret) {
		cpuhp_state_remove_instance_nocalls(rpi_axi_pmu_cpuhp_state, &pmu->cpuhp_node);
		dev_err(dev, "PMU register failed %d\n", ret);
		goto err_teardown;
	}

	return 0;
err_teardown:
	hrtimer_cancel(&pmu->hrtimer);
	flush_work(&pmu->vpu_work);
	return ret;
}

/**
 * rpi_axi_pmu__exit() - Internal PMU teardown called during remove
 * @pmu: Pointer to PMU context
 */
static void rpi_axi_pmu__exit(struct rpi_axi_pmu *pmu)
{
	cpuhp_state_remove_instance_nocalls(rpi_axi_pmu_cpuhp_state, &pmu->cpuhp_node);
	/*
	 * perf_pmu_unregister implicitly detaches all active events, which triggers
	 * rpi_axi_pmu_del(). For VPU events, this conservatively schedules vpu_work
	 * to disable the hardware via Mailbox IPC in process context.
	 */
	perf_pmu_unregister(&pmu->pmu);
	hrtimer_cancel(&pmu->hrtimer);
	/*
	 * flush_work MUST be used instead of cancel_work_sync. If we cancel it,
	 * the deferred hardware disable commands emitted by perf_pmu_unregister
	 * are silently dropped, permanently orphaning and leaving the VideoCore VPU
	 * monitor running indefinitely.
	 *
	 * Note: Because perf_pmu_unregister and hrtimer_cancel have both completed,
	 * the work queue is permanently sealed. No new work can be scheduled, guaranteeing
	 * flush_work cannot race and is completely safe from Use-After-Free during unload.
	 */
	flush_work(&pmu->vpu_work);
}

/* --- DRIVER ENTRY POINTS ----------------------------------------- */

/**
 * rpi_axi_pmu_probe() - Platform driver probe entry point
 * @pdev: Pointer to platform_device
 *
 * Return: 0 on success, negative error code on failure.
 */
static int rpi_axi_pmu_probe(struct platform_device *pdev)
{
	struct rpi_axi_pmu *pmu;

	pmu = devm_kzalloc(&pdev->dev, sizeof(*pmu), GFP_KERNEL);
	if (!pmu)
		return -ENOMEM;
	platform_set_drvdata(pdev, pmu);
	return rpi_axi_pmu__init(pmu, pdev);
}

/**
 * rpi_axi_pmu_remove() - Platform driver remove entry point
 * @pdev: Pointer to platform_device
 */
static void rpi_axi_pmu_remove(struct platform_device *pdev)
{
	struct rpi_axi_pmu *pmu;

	pmu = platform_get_drvdata(pdev);
	rpi_axi_pmu__exit(pmu);
}

/* Devices matching this driver in Device Tree */
static const struct of_device_id rpi_axi_pmu_match[] = {
	{
		.compatible = "brcm,bcm2835-axiperf",
		.data = (void *)CHIP_BCM2835,
	},
	{
		.compatible = "brcm,bcm2711-axiperf",
		.data = (void *)CHIP_BCM2711,
	},
	{
		.compatible = "brcm,bcm2712-axiperf",
		.data = (void *)CHIP_BCM2712,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, rpi_axi_pmu_match);

static struct platform_driver rpi_axi_pmu_driver = {
	.probe =	rpi_axi_pmu_probe,
	.remove =	rpi_axi_pmu_remove,
	.driver = {
		.name   = PMU_NAME,
		.of_match_table = rpi_axi_pmu_match,
		.suppress_bind_attrs = true,
	},
};

/**
 * rpi_axi_pmu_driver_init() - Module initialization entry point
 *
 * Registers the dynamic CPU hotplug state and the platform driver.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int __init rpi_axi_pmu_driver_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "perf/rpi_axi_pmu:online",
				      rpi_axi_pmu_online_cpu, rpi_axi_pmu_offline_cpu);
	if (ret < 0)
		return ret;
	rpi_axi_pmu_cpuhp_state = ret;
	ret = platform_driver_register(&rpi_axi_pmu_driver);
	if (ret)
		cpuhp_remove_multi_state(rpi_axi_pmu_cpuhp_state);
	return ret;
}
module_init(rpi_axi_pmu_driver_init);

/**
 * rpi_axi_pmu_driver_exit() - Module cleanup entry point
 *
 * Unregisters the platform driver and CPU hotplug state.
 */
static void __exit rpi_axi_pmu_driver_exit(void)
{
	platform_driver_unregister(&rpi_axi_pmu_driver);
	cpuhp_remove_multi_state(rpi_axi_pmu_cpuhp_state);
}
module_exit(rpi_axi_pmu_driver_exit);
MODULE_AUTHOR("Ian Rogers <irogers@google.com>");
MODULE_DESCRIPTION("Broadcom Raspberry Pi AXI Performance Monitor driver");
MODULE_LICENSE("GPL");
