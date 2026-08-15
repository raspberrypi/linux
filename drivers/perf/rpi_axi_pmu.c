// SPDX-License-Identifier: GPL-2.0-only

/**
 * DOC: Raspberry Pi AXI Bus Performance Monitoring Unit (PMU) Driver
 *
 * This driver exposes the performance monitoring hardware on Raspberry Pi
 * System-on-Chips to the Linux perf subsystem:
 * - Raspberry Pi 1, 2, 3, 4, Compute Modules 1-4, Zero, Zero W (SoCs BCM2835/2836/2837/2711).
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
 *   Protects active event array (events[]), event generation sequence counters (event_gen[]),
 *   bus watcher allocation/refcounting, active_vpu_events counter, and MMIO register updates
 *   (MON_SYSTEM) against SMP race conditions and ABA pointer recycling races.
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

#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/version.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/perf_event.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <soc/bcm2835/raspberrypi-firmware.h>

/* --- PLATFORM CONSTANTS & ENUMERATIONS --------------------------- */

/**
 * enum rpi_axi_chip - Supported Broadcom SoC generations
 * @CHIP_BCM2835: BCM2835 / BCM2836 / BCM2837 / BCM2711 (RPi 1-4, CM 1-4, Zero/W)
 */
enum rpi_axi_chip {
	CHIP_BCM2835 = 0,
};

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
	BCM2835_SB_TRANS,
	BCM2835_SB_JPEG,
	BCM2835_SB_SYSTEM_UC,
	BCM2835_SB_DMA_UC,
	BCM2835_SB_SYSTEM_L2,
	BCM2835_SB_CCP2TX,
	BCM2835_SB_MPHI_RX,
	BCM2835_SB_MPHI_TX,
	BCM2835_SB_HVS,
	BCM2835_SB_H264,
	BCM2835_SB_ISP,
	BCM2835_SB_V3D,
	BCM2835_SB_PERIPHERAL,
	BCM2835_SB_CPU_UC,
	BCM2835_SB_CPU_L2,
	BCM2835_SB_MAX
};

/**
 * enum vpu_bus - AXI buses monitored by VPU Monitor on BCM2835-BCM2711 (RPi 1-4)
 * @VPU__VPU1_D_L2: VideoCore VPU Core 1 Data L2 cache bus
 * @VPU__VPU0_D_L2: VideoCore VPU Core 0 Data L2 cache bus
 * @VPU__VPU1_I_L2: VideoCore VPU Core 1 Instruction L2 cache bus
 * @VPU__VPU0_I_L2: VideoCore VPU Core 0 Instruction L2 cache bus
 * @VPU__SYSTEM_L2: VPU System L2 cache interconnect bus
 * @VPU__L2_FLUSH: VPU L2 cache flush controller bus
 * @VPU__DMA_L2: VPU DMA L2 cache interconnect bus
 * @VPU__VPU1_D_UC: VideoCore VPU Core 1 Data Uncached memory bus
 * @VPU__VPU0_D_UC: VideoCore VPU Core 0 Data Uncached memory bus
 * @VPU__VPU1_I_UC: VideoCore VPU Core 1 Instruction Uncached memory bus
 * @VPU__VPU0_I_UC: VideoCore VPU Core 0 Instruction Uncached memory bus
 * @VPU__SYSTEM_UC: VPU System Uncached memory bus
 * @VPU__L2_OUT: VPU L2 cache outbound memory bus
 * @VPU__DMA_UC: VPU DMA Uncached memory bus
 * @VPU__SDRAM: VPU SDRAM memory controller bus
 * @VPU__L2_IN: VPU L2 cache inbound memory bus
 * @VPU_MAX: Total count of monitored VPU buses
 */
enum vpu_bus {
	VPU__VPU1_D_L2 = 0,
	VPU__VPU0_D_L2,
	VPU__VPU1_I_L2,
	VPU__VPU0_I_L2,
	VPU__SYSTEM_L2,
	VPU__L2_FLUSH,
	VPU__DMA_L2,
	VPU__VPU1_D_UC,
	VPU__VPU0_D_UC,
	VPU__VPU1_I_UC,
	VPU__VPU0_I_UC,
	VPU__SYSTEM_UC,
	VPU__L2_OUT,
	VPU__DMA_UC,
	VPU__SDRAM,
	VPU__L2_IN,
	VPU_MAX
};

/**
 * enum counter - 32-bit hardware performance counter metrics per bus watcher unit
 * @CNT_ATRANS: Total address phase transaction count
 * @CNT_ATWAIT: Total address phase wait / stall cycles
 * @CNT_WTRANS: Total write data phase transaction count
 * @CNT_WTWAIT: Total write data phase wait / stall cycles
 * @CNT_RTRANS: Total read data phase transaction count
 * @CNT_RTWAIT: Total read data phase wait / stall cycles
 * @CNT_MAX: Total metric counters per watcher unit
 */
enum counter {
	CNT_ATRANS = 0,
	CNT_ATWAIT,
	CNT_WTRANS,
	CNT_WTWAIT,
	CNT_RTRANS,
	CNT_RTWAIT,
	CNT_MAX
};

/**
 * enum bcm2835_filter - AXI master ID filter options for BCM2835-BCM2711 (RPi 1-4)
 * @BCM2835_FLT_NONE: Disable master ID filtering (monitor all traffic on bus)
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
	BCM2835_FLT_NONE = 0,
	BCM2835_FLT_CORE0_V,
	BCM2835_FLT_ICACHE0,
	BCM2835_FLT_DCACHE0,
	BCM2835_FLT_CORE1_V,
	BCM2835_FLT_ICACHE1,
	BCM2835_FLT_DCACHE1,
	BCM2835_FLT_L2_MAIN,
	BCM2835_FLT_HOST_PORT,
	BCM2835_FLT_HOST_PORT2,
	BCM2835_FLT_HVS,
	BCM2835_FLT_ISP,
	BCM2835_FLT_VIDEO_DCT,
	BCM2835_FLT_VIDEO_SD2AXI,
	BCM2835_FLT_CAM0,
	BCM2835_FLT_CAM1,
	BCM2835_FLT_DMA0,
	BCM2835_FLT_DMA1,
	BCM2835_FLT_DMA2_VPU,
	BCM2835_FLT_JPEG,
	BCM2835_FLT_VIDEO_CME,
	BCM2835_FLT_TRANSPOSER,
	BCM2835_FLT_VIDEO_FME,
	BCM2835_FLT_CCP2TX,
	BCM2835_FLT_USB,
	BCM2835_FLT_V3D0,
	BCM2835_FLT_V3D1,
	BCM2835_FLT_V3D2,
	BCM2835_FLT_AVE,
	BCM2835_FLT_DEBUG,
	BCM2835_FLT_CPU,
	BCM2835_FLT_M30,
	BCM2835_FLT_MAX
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

#define BW_CTRL_RESET_BIT		BIT(31)
#define BW_CTRL_ENABLE_BIT		BIT(30)
#define BW_CTRL_ENABLE_ID_FILTER_BIT	BIT(29)
#define BW_CTRL_LIMIT_HALT_BIT		BIT(28)

#define BW_CTRL_BUS_WATCH_SHIFT		0
#define BW_CTRL_BUS_WATCH_MASK		GENMASK(5, 0)
#define BW_CTRL_BUS_FILTER_SHIFT	8
#define BW_CTRL_BUS_FILTER_MASK		GENMASK(12, 8)

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
 * [10-14] : Filter ID
 * [9]     : Monitor ID (0 = System, 1 = VPU)
 * [4-8]   : Bus index
 * [0-3]   : Counter enum
 */
#define RPI_AXI_CFG_FILTER_SHIFT	10
#define RPI_AXI_CFG_FILTER_MASK		0x1F
#define RPI_AXI_CFG_MONITOR_SHIFT	9
#define RPI_AXI_CFG_MONITOR_MASK	0x1
#define RPI_AXI_CFG_BUS_SHIFT		4
#define RPI_AXI_CFG_BUS_MASK		0x1F
#define RPI_AXI_CFG_COUNTER_MASK	0xF

/**
 * config_to_filter() - Extracts AXI filter ID from perf event config
 * @config: 64-bit config value from struct perf_event_attr
 *
 * Return: Filter ID value (bits 10-14).
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
 */
struct rpi_axi_hw_events {
	int monitored_bus[NUM_BUS_WATCHERS_PER_MONITOR];
	int filter[NUM_BUS_WATCHERS_PER_MONITOR];
	int refcount[NUM_BUS_WATCHERS_PER_MONITOR];
	int num_monitored;
bool monitor_running;
	bool enabled[NUM_BUS_WATCHERS_PER_MONITOR];
	bool vpu_disable_pending[NUM_BUS_WATCHERS_PER_MONITOR];
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
		hw_events->filter[i] = BCM2835_FLT_NONE;
		hw_events->refcount[i] = 0;
		hw_events->enabled[i] = false;
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
			hw_events->num_monitored++;
			return i;
		}
	}
	return -1;
}

/* Maximum simultaneous active perf_events tracked by PMU */
#define RPI_AXI_MAX_EVENTS 60

/**
 * struct rpi_axi_pmu - Root PMU driver context
 * @pmu: Core Linux perf PMU structure
 * @pdev: Owning platform_device pointer
 * @chip: Detected Broadcom SoC generation
 * @firmware: Raspberry Pi firmware handle for VideoCore mailbox calls (BCM2835-BCM2711)
 * @cpu: CPU core assigned to process uncore PMU events
 * @cpuhp_node: Dynamic CPU hotplug instance node
\n * @is_registered: True if the PMU backend has securely finished initialization
 * @lock: Spinlock protecting events[] list, watcher refcounts, and MMIO counter updates
 * @vpu_mutex: Mutex serializing VideoCore Mailbox IPC transactions in process context
 * @hrtimer: High-resolution timer for periodic 32-bit counter overflow polling
 * @vpu_work: Deferred work structure for safe process-context VPU mailbox reads
 * @active_events: Count of active perf_events currently monitored
 * @active_vpu_events: Count of active VPU perf_events currently monitored
 * @events: Array of active perf_event pointers
 * @event_gen: Per-slot generation sequence counters to prevent ABA pointer recycling races
 * @monitor: Per-monitor state array (System and VPU)
 */
struct rpi_axi_pmu {
	struct pmu		pmu;
	struct platform_device	*pdev;
	enum rpi_axi_chip	chip;
	struct rpi_firmware	*firmware;

	int			cpu;
	struct hlist_node	cpuhp_node;
	bool			is_registered;

	raw_spinlock_t		lock;
	struct mutex		vpu_mutex;

	struct hrtimer		hrtimer;
	struct work_struct	vpu_work;
	int			active_events;
	int			active_vpu_events;
	struct perf_event	*events[RPI_AXI_MAX_EVENTS];
	u64			event_gen[RPI_AXI_MAX_EVENTS];

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

	if (config >> 15 != 0)
		return false;

	if (mon >= MON_MAX)
		return false;

	if (!pmu->monitor[mon].use_mailbox_interface && !pmu->monitor[mon].base_address)
		return false;

	if (mon == MON_SYSTEM) {
		if (bus >= BCM2835_SB_MAX)
			return false;
	} else {
		if (bus >= VPU_MAX)
			return false;
	}
	if (filter >= BCM2835_FLT_MAX)
		return false;
	return counter < CNT_MAX;
}

PMU_FORMAT_ATTR(filter,		"config:10-14");
PMU_FORMAT_ATTR(monitor,	"config:9");
PMU_FORMAT_ATTR(bus,		"config:4-8");
PMU_FORMAT_ATTR(counter,	"config:0-3");

static struct attribute *rpi_axi_pmu_formats_attr[] = {
	&format_attr_filter.attr,
	&format_attr_monitor.attr,
	&format_attr_bus.attr,
	&format_attr_counter.attr,
	NULL,
};

static const struct attribute_group rpi_axi_pmu_format_group = {
	.name	= "format",
	.attrs	= rpi_axi_pmu_formats_attr,
};

/*
 * Event Attribute Scaling & Unit Definitions:
 *
 * Uncore AXI bus transaction events (_rtrans, _wtrans, _atrans) report hardware
 * transaction beats. On Broadcom BCM2835 AXI interconnects, single-beat
 * transactions transfer 32 bytes per beat, while contiguous DMA page bursts may
 * use 64-byte double-beats. Setting .scale="32" and .unit="Bytes" provides a
 * close (~95%) byte throughput approximation in perf stat output (accounting for
 * 64-byte burst beats and VideoCore L2 cache prefetch hits).
 */
/* --- SYSFS NAMED EVENT ALIASES ------------------------------------ */

/* DMA Engine L2 Cache Bus Events (bus=0, BCM2835_SB_DMA_L2) */
PMU_EVENT_ATTR_STRING(dma_l2_atrans, rpi_axi_pmu_event_dma_l2_atrans,
		      "monitor=0,bus=0,counter=0");
PMU_EVENT_ATTR_STRING(dma_l2_atrans.scale, rpi_axi_pmu_event_dma_l2_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma_l2_atrans.unit, rpi_axi_pmu_event_dma_l2_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(dma_l2_atwait, rpi_axi_pmu_event_dma_l2_atwait,
		      "monitor=0,bus=0,counter=1");
PMU_EVENT_ATTR_STRING(dma_l2_wtrans, rpi_axi_pmu_event_dma_l2_wtrans,
		      "monitor=0,bus=0,counter=2");
PMU_EVENT_ATTR_STRING(dma_l2_wtrans.scale, rpi_axi_pmu_event_dma_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma_l2_wtrans.unit, rpi_axi_pmu_event_dma_l2_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(dma_l2_wtwait, rpi_axi_pmu_event_dma_l2_wtwait,
		      "monitor=0,bus=0,counter=3");
PMU_EVENT_ATTR_STRING(dma_l2_rtrans, rpi_axi_pmu_event_dma_l2_rtrans,
		      "monitor=0,bus=0,counter=4");
PMU_EVENT_ATTR_STRING(dma_l2_rtrans.scale, rpi_axi_pmu_event_dma_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma_l2_rtrans.unit, rpi_axi_pmu_event_dma_l2_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(dma_l2_rtwait, rpi_axi_pmu_event_dma_l2_rtwait,
		      "monitor=0,bus=0,counter=5");

/* Image Transposer Engine Events (bus=1, BCM2835_SB_TRANS: format rotation) */
PMU_EVENT_ATTR_STRING(trans_atrans, rpi_axi_pmu_event_trans_atrans,
		      "monitor=0,bus=1,counter=0");
PMU_EVENT_ATTR_STRING(trans_atrans.scale, rpi_axi_pmu_event_trans_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(trans_atrans.unit, rpi_axi_pmu_event_trans_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(trans_atwait, rpi_axi_pmu_event_trans_atwait,
		      "monitor=0,bus=1,counter=1");
PMU_EVENT_ATTR_STRING(trans_wtrans, rpi_axi_pmu_event_trans_wtrans,
		      "monitor=0,bus=1,counter=2");
PMU_EVENT_ATTR_STRING(trans_wtrans.scale, rpi_axi_pmu_event_trans_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(trans_wtrans.unit, rpi_axi_pmu_event_trans_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(trans_wtwait, rpi_axi_pmu_event_trans_wtwait,
		      "monitor=0,bus=1,counter=3");
PMU_EVENT_ATTR_STRING(trans_rtrans, rpi_axi_pmu_event_trans_rtrans,
		      "monitor=0,bus=1,counter=4");
PMU_EVENT_ATTR_STRING(trans_rtrans.scale, rpi_axi_pmu_event_trans_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(trans_rtrans.unit, rpi_axi_pmu_event_trans_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(trans_rtwait, rpi_axi_pmu_event_trans_rtwait,
		      "monitor=0,bus=1,counter=5");

/* Hardware JPEG Codec Accelerator Bus Events (bus=2, BCM2835_SB_JPEG) */
PMU_EVENT_ATTR_STRING(jpeg_atrans, rpi_axi_pmu_event_jpeg_atrans,
		      "monitor=0,bus=2,counter=0");
PMU_EVENT_ATTR_STRING(jpeg_atrans.scale, rpi_axi_pmu_event_jpeg_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(jpeg_atrans.unit, rpi_axi_pmu_event_jpeg_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(jpeg_atwait, rpi_axi_pmu_event_jpeg_atwait,
		      "monitor=0,bus=2,counter=1");
PMU_EVENT_ATTR_STRING(jpeg_wtrans, rpi_axi_pmu_event_jpeg_wtrans,
		      "monitor=0,bus=2,counter=2");
PMU_EVENT_ATTR_STRING(jpeg_wtrans.scale, rpi_axi_pmu_event_jpeg_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(jpeg_wtrans.unit, rpi_axi_pmu_event_jpeg_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(jpeg_wtwait, rpi_axi_pmu_event_jpeg_wtwait,
		      "monitor=0,bus=2,counter=3");
PMU_EVENT_ATTR_STRING(jpeg_rtrans, rpi_axi_pmu_event_jpeg_rtrans,
		      "monitor=0,bus=2,counter=4");
PMU_EVENT_ATTR_STRING(jpeg_rtrans.scale, rpi_axi_pmu_event_jpeg_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(jpeg_rtrans.unit, rpi_axi_pmu_event_jpeg_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(jpeg_rtwait, rpi_axi_pmu_event_jpeg_rtwait,
		      "monitor=0,bus=2,counter=5");

/* System Uncached Memory Bus Events (bus=3, BCM2835_SB_SYSTEM_UC) */
PMU_EVENT_ATTR_STRING(system_uc_atrans, rpi_axi_pmu_event_system_uc_atrans,
		      "monitor=0,bus=3,counter=0");
PMU_EVENT_ATTR_STRING(system_uc_atrans.scale, rpi_axi_pmu_event_system_uc_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(system_uc_atrans.unit, rpi_axi_pmu_event_system_uc_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(system_uc_atwait, rpi_axi_pmu_event_system_uc_atwait,
		      "monitor=0,bus=3,counter=1");
PMU_EVENT_ATTR_STRING(system_uc_wtrans, rpi_axi_pmu_event_system_uc_wtrans,
		      "monitor=0,bus=3,counter=2");
PMU_EVENT_ATTR_STRING(system_uc_wtrans.scale, rpi_axi_pmu_event_system_uc_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(system_uc_wtrans.unit, rpi_axi_pmu_event_system_uc_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(system_uc_wtwait, rpi_axi_pmu_event_system_uc_wtwait,
		      "monitor=0,bus=3,counter=3");
PMU_EVENT_ATTR_STRING(system_uc_rtrans, rpi_axi_pmu_event_system_uc_rtrans,
		      "monitor=0,bus=3,counter=4");
PMU_EVENT_ATTR_STRING(system_uc_rtrans.scale, rpi_axi_pmu_event_system_uc_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(system_uc_rtrans.unit, rpi_axi_pmu_event_system_uc_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(system_uc_rtwait, rpi_axi_pmu_event_system_uc_rtwait,
		      "monitor=0,bus=3,counter=5");

/* DMA Engine Uncached Memory Bus Events (bus=4, BCM2835_SB_DMA_UC) */
PMU_EVENT_ATTR_STRING(dma_uc_atrans, rpi_axi_pmu_event_dma_uc_atrans,
		      "monitor=0,bus=4,counter=0");
PMU_EVENT_ATTR_STRING(dma_uc_atrans.scale, rpi_axi_pmu_event_dma_uc_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma_uc_atrans.unit, rpi_axi_pmu_event_dma_uc_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(dma_uc_atwait, rpi_axi_pmu_event_dma_uc_atwait,
		      "monitor=0,bus=4,counter=1");
PMU_EVENT_ATTR_STRING(dma_uc_wtrans, rpi_axi_pmu_event_dma_uc_wtrans,
		      "monitor=0,bus=4,counter=2");
PMU_EVENT_ATTR_STRING(dma_uc_wtrans.scale, rpi_axi_pmu_event_dma_uc_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma_uc_wtrans.unit, rpi_axi_pmu_event_dma_uc_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(dma_uc_wtwait, rpi_axi_pmu_event_dma_uc_wtwait,
		      "monitor=0,bus=4,counter=3");
PMU_EVENT_ATTR_STRING(dma_uc_rtrans, rpi_axi_pmu_event_dma_uc_rtrans,
		      "monitor=0,bus=4,counter=4");
PMU_EVENT_ATTR_STRING(dma_uc_rtrans.scale, rpi_axi_pmu_event_dma_uc_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma_uc_rtrans.unit, rpi_axi_pmu_event_dma_uc_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(dma_uc_rtwait, rpi_axi_pmu_event_dma_uc_rtwait,
		      "monitor=0,bus=4,counter=5");

/* System Main L2 Cache Bus Events (bus=5, BCM2835_SB_SYSTEM_L2) */
PMU_EVENT_ATTR_STRING(system_l2_atrans, rpi_axi_pmu_event_system_l2_atrans,
		      "monitor=0,bus=5,counter=0");
PMU_EVENT_ATTR_STRING(system_l2_atrans.scale, rpi_axi_pmu_event_system_l2_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(system_l2_atrans.unit, rpi_axi_pmu_event_system_l2_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(system_l2_atwait, rpi_axi_pmu_event_system_l2_atwait,
		      "monitor=0,bus=5,counter=1");
PMU_EVENT_ATTR_STRING(system_l2_wtrans, rpi_axi_pmu_event_system_l2_wtrans,
		      "monitor=0,bus=5,counter=2");
PMU_EVENT_ATTR_STRING(system_l2_wtrans.scale, rpi_axi_pmu_event_system_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(system_l2_wtrans.unit, rpi_axi_pmu_event_system_l2_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(system_l2_wtwait, rpi_axi_pmu_event_system_l2_wtwait,
		      "monitor=0,bus=5,counter=3");
PMU_EVENT_ATTR_STRING(system_l2_rtrans, rpi_axi_pmu_event_system_l2_rtrans,
		      "monitor=0,bus=5,counter=4");
PMU_EVENT_ATTR_STRING(system_l2_rtrans.scale, rpi_axi_pmu_event_system_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(system_l2_rtrans.unit, rpi_axi_pmu_event_system_l2_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(system_l2_rtwait, rpi_axi_pmu_event_system_l2_rtwait,
		      "monitor=0,bus=5,counter=5");

/* Legacy Camera & Message Passing Bus Events (bus=6-8, CCP2TX, MPHI_RX, MPHI_TX) */
PMU_EVENT_ATTR_STRING(ccp2tx_atrans, rpi_axi_pmu_event_ccp2tx_atrans,
		      "monitor=0,bus=6,counter=0");
PMU_EVENT_ATTR_STRING(ccp2tx_atrans.scale, rpi_axi_pmu_event_ccp2tx_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(ccp2tx_atrans.unit, rpi_axi_pmu_event_ccp2tx_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(mphi_rx_atrans, rpi_axi_pmu_event_mphi_rx_atrans,
		      "monitor=0,bus=7,counter=0");
PMU_EVENT_ATTR_STRING(mphi_rx_atrans.scale, rpi_axi_pmu_event_mphi_rx_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(mphi_rx_atrans.unit, rpi_axi_pmu_event_mphi_rx_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(mphi_tx_atrans, rpi_axi_pmu_event_mphi_tx_atrans,
		      "monitor=0,bus=8,counter=0");
PMU_EVENT_ATTR_STRING(mphi_tx_atrans.scale, rpi_axi_pmu_event_mphi_tx_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(mphi_tx_atrans.unit, rpi_axi_pmu_event_mphi_tx_atrans_unit,
		      "Bytes");

/* Hardware Video Scaler Display Engine Bus Events (bus=9, BCM2835_SB_HVS) */
PMU_EVENT_ATTR_STRING(hvs_atrans, rpi_axi_pmu_event_hvs_atrans,
		      "monitor=0,bus=9,counter=0");
PMU_EVENT_ATTR_STRING(hvs_atrans.scale, rpi_axi_pmu_event_hvs_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(hvs_atrans.unit, rpi_axi_pmu_event_hvs_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(hvs_atwait, rpi_axi_pmu_event_hvs_atwait,
		      "monitor=0,bus=9,counter=1");
PMU_EVENT_ATTR_STRING(hvs_wtrans, rpi_axi_pmu_event_hvs_wtrans,
		      "monitor=0,bus=9,counter=2");
PMU_EVENT_ATTR_STRING(hvs_wtrans.scale, rpi_axi_pmu_event_hvs_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(hvs_wtrans.unit, rpi_axi_pmu_event_hvs_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(hvs_wtwait, rpi_axi_pmu_event_hvs_wtwait,
		      "monitor=0,bus=9,counter=3");
PMU_EVENT_ATTR_STRING(hvs_rtrans, rpi_axi_pmu_event_hvs_rtrans,
		      "monitor=0,bus=9,counter=4");
PMU_EVENT_ATTR_STRING(hvs_rtrans.scale, rpi_axi_pmu_event_hvs_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(hvs_rtrans.unit, rpi_axi_pmu_event_hvs_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(hvs_rtwait, rpi_axi_pmu_event_hvs_rtwait,
		      "monitor=0,bus=9,counter=5");

/* H.264 Video Encoder/Decoder Bus Events (bus=10, BCM2835_SB_H264) */
PMU_EVENT_ATTR_STRING(h264_atrans, rpi_axi_pmu_event_h264_atrans,
		      "monitor=0,bus=10,counter=0");
PMU_EVENT_ATTR_STRING(h264_atrans.scale, rpi_axi_pmu_event_h264_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(h264_atrans.unit, rpi_axi_pmu_event_h264_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(h264_atwait, rpi_axi_pmu_event_h264_atwait,
		      "monitor=0,bus=10,counter=1");
PMU_EVENT_ATTR_STRING(h264_wtrans, rpi_axi_pmu_event_h264_wtrans,
		      "monitor=0,bus=10,counter=2");
PMU_EVENT_ATTR_STRING(h264_wtrans.scale, rpi_axi_pmu_event_h264_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(h264_wtrans.unit, rpi_axi_pmu_event_h264_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(h264_wtwait, rpi_axi_pmu_event_h264_wtwait,
		      "monitor=0,bus=10,counter=3");
PMU_EVENT_ATTR_STRING(h264_rtrans, rpi_axi_pmu_event_h264_rtrans,
		      "monitor=0,bus=10,counter=4");
PMU_EVENT_ATTR_STRING(h264_rtrans.scale, rpi_axi_pmu_event_h264_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(h264_rtrans.unit, rpi_axi_pmu_event_h264_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(h264_rtwait, rpi_axi_pmu_event_h264_rtwait,
		      "monitor=0,bus=10,counter=5");

/* Image Sensor Processor Camera Pipeline Bus Events (bus=11, BCM2835_SB_ISP) */

/* VideoCore V3D 3D Graphics Hardware Pipeline Bus Events (bus=12, BCM2835_SB_V3D) */
PMU_EVENT_ATTR_STRING(v3d_atrans, rpi_axi_pmu_event_v3d_atrans,
		      "monitor=0,bus=12,counter=0");
PMU_EVENT_ATTR_STRING(v3d_atrans.scale, rpi_axi_pmu_event_v3d_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(v3d_atrans.unit, rpi_axi_pmu_event_v3d_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(v3d_atwait, rpi_axi_pmu_event_v3d_atwait,
		      "monitor=0,bus=12,counter=1");
PMU_EVENT_ATTR_STRING(v3d_wtrans, rpi_axi_pmu_event_v3d_wtrans,
		      "monitor=0,bus=12,counter=2");
PMU_EVENT_ATTR_STRING(v3d_wtrans.scale, rpi_axi_pmu_event_v3d_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(v3d_wtrans.unit, rpi_axi_pmu_event_v3d_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(v3d_wtwait, rpi_axi_pmu_event_v3d_wtwait,
		      "monitor=0,bus=12,counter=3");
PMU_EVENT_ATTR_STRING(v3d_rtrans, rpi_axi_pmu_event_v3d_rtrans,
		      "monitor=0,bus=12,counter=4");
PMU_EVENT_ATTR_STRING(v3d_rtrans.scale, rpi_axi_pmu_event_v3d_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(v3d_rtrans.unit, rpi_axi_pmu_event_v3d_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(v3d_rtwait, rpi_axi_pmu_event_v3d_rtwait,
		      "monitor=0,bus=12,counter=5");

/* Low-Speed Peripherals Bus Events (bus=13, BCM2835_SB_PERIPHERAL: UART, SPI, I2C, GPIO) */

/* CPU Uncached Memory Bus Events (bus=14, BCM2835_SB_CPU_UC) */

/* CPU L2 Cache Bus Events (bus=15, BCM2835_SB_CPU_L2) */
PMU_EVENT_ATTR_STRING(cpu_l2_atrans, rpi_axi_pmu_event_cpu_l2_atrans,
		      "monitor=0,bus=15,counter=0");
PMU_EVENT_ATTR_STRING(cpu_l2_atrans.scale, rpi_axi_pmu_event_cpu_l2_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(cpu_l2_atrans.unit, rpi_axi_pmu_event_cpu_l2_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(cpu_l2_atwait, rpi_axi_pmu_event_cpu_l2_atwait,
		      "monitor=0,bus=15,counter=1");
PMU_EVENT_ATTR_STRING(cpu_l2_wtrans, rpi_axi_pmu_event_cpu_l2_wtrans,
		      "monitor=0,bus=15,counter=2");
PMU_EVENT_ATTR_STRING(cpu_l2_wtrans.scale, rpi_axi_pmu_event_cpu_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(cpu_l2_wtrans.unit, rpi_axi_pmu_event_cpu_l2_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(cpu_l2_wtwait, rpi_axi_pmu_event_cpu_l2_wtwait,
		      "monitor=0,bus=15,counter=3");
PMU_EVENT_ATTR_STRING(cpu_l2_rtrans, rpi_axi_pmu_event_cpu_l2_rtrans,
		      "monitor=0,bus=15,counter=4");
PMU_EVENT_ATTR_STRING(cpu_l2_rtrans.scale, rpi_axi_pmu_event_cpu_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(cpu_l2_rtrans.unit, rpi_axi_pmu_event_cpu_l2_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(cpu_l2_rtwait, rpi_axi_pmu_event_cpu_l2_rtwait,
		      "monitor=0,bus=15,counter=5");

/* --- VIDEOCORE VPU MONITOR EVENTS (MON_VPU = 1, RPi 1-4) ------------------ */

/* VideoCore Core 0 & Core 1 Data/Instruction L2 Cache Bus Events */
PMU_EVENT_ATTR_STRING(vpu1_d_l2_atrans, rpi_axi_pmu_event_vpu1_d_l2_atrans,
		      "monitor=1,bus=0,counter=0");
PMU_EVENT_ATTR_STRING(vpu1_d_l2_atrans.scale, rpi_axi_pmu_event_vpu1_d_l2_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu1_d_l2_atrans.unit, rpi_axi_pmu_event_vpu1_d_l2_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu0_d_l2_atrans, rpi_axi_pmu_event_vpu0_d_l2_atrans,
		      "monitor=1,bus=1,counter=0");
PMU_EVENT_ATTR_STRING(vpu0_d_l2_atrans.scale, rpi_axi_pmu_event_vpu0_d_l2_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu0_d_l2_atrans.unit, rpi_axi_pmu_event_vpu0_d_l2_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu1_i_l2_atrans, rpi_axi_pmu_event_vpu1_i_l2_atrans,
		      "monitor=1,bus=2,counter=0");
PMU_EVENT_ATTR_STRING(vpu1_i_l2_atrans.scale, rpi_axi_pmu_event_vpu1_i_l2_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu1_i_l2_atrans.unit, rpi_axi_pmu_event_vpu1_i_l2_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu0_i_l2_atrans, rpi_axi_pmu_event_vpu0_i_l2_atrans,
		      "monitor=1,bus=3,counter=0");
PMU_EVENT_ATTR_STRING(vpu0_i_l2_atrans.scale, rpi_axi_pmu_event_vpu0_i_l2_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu0_i_l2_atrans.unit, rpi_axi_pmu_event_vpu0_i_l2_atrans_unit,
		      "Bytes");

/* VPU System L2 Cache Interconnect Bus Events */
PMU_EVENT_ATTR_STRING(vpu_system_l2_atrans, rpi_axi_pmu_event_vpu_system_l2_atrans,
		      "monitor=1,bus=4,counter=0");
PMU_EVENT_ATTR_STRING(vpu_system_l2_atrans.scale, rpi_axi_pmu_event_vpu_system_l2_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_system_l2_atrans.unit, rpi_axi_pmu_event_vpu_system_l2_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wtrans, rpi_axi_pmu_event_vpu_system_l2_wtrans,
		      "monitor=1,bus=4,counter=2");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wtrans.scale, rpi_axi_pmu_event_vpu_system_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_system_l2_wtrans.unit, rpi_axi_pmu_event_vpu_system_l2_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rtrans, rpi_axi_pmu_event_vpu_system_l2_rtrans,
		      "monitor=1,bus=4,counter=4");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rtrans.scale, rpi_axi_pmu_event_vpu_system_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_system_l2_rtrans.unit, rpi_axi_pmu_event_vpu_system_l2_rtrans_unit,
		      "Bytes");

/* VPU Cache Flush Controller & VPU DMA L2 Cache Bus Events */
PMU_EVENT_ATTR_STRING(vpu_l2_flush_atrans, rpi_axi_pmu_event_vpu_l2_flush_atrans,
		      "monitor=1,bus=5,counter=0");
PMU_EVENT_ATTR_STRING(vpu_l2_flush_atrans.scale, rpi_axi_pmu_event_vpu_l2_flush_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_l2_flush_atrans.unit, rpi_axi_pmu_event_vpu_l2_flush_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_atrans, rpi_axi_pmu_event_vpu_dma_l2_atrans,
		      "monitor=1,bus=6,counter=0");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_atrans.scale, rpi_axi_pmu_event_vpu_dma_l2_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_dma_l2_atrans.unit, rpi_axi_pmu_event_vpu_dma_l2_atrans_unit,
		      "Bytes");

/* VideoCore Core 0 & Core 1 Data/Instruction Uncached Memory Bus Events */
PMU_EVENT_ATTR_STRING(vpu1_d_uc_atrans, rpi_axi_pmu_event_vpu1_d_uc_atrans,
		      "monitor=1,bus=7,counter=0");
PMU_EVENT_ATTR_STRING(vpu1_d_uc_atrans.scale, rpi_axi_pmu_event_vpu1_d_uc_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu1_d_uc_atrans.unit, rpi_axi_pmu_event_vpu1_d_uc_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu0_d_uc_atrans, rpi_axi_pmu_event_vpu0_d_uc_atrans,
		      "monitor=1,bus=8,counter=0");
PMU_EVENT_ATTR_STRING(vpu0_d_uc_atrans.scale, rpi_axi_pmu_event_vpu0_d_uc_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu0_d_uc_atrans.unit, rpi_axi_pmu_event_vpu0_d_uc_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu1_i_uc_atrans, rpi_axi_pmu_event_vpu1_i_uc_atrans,
		      "monitor=1,bus=9,counter=0");
PMU_EVENT_ATTR_STRING(vpu1_i_uc_atrans.scale, rpi_axi_pmu_event_vpu1_i_uc_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu1_i_uc_atrans.unit, rpi_axi_pmu_event_vpu1_i_uc_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu0_i_uc_atrans, rpi_axi_pmu_event_vpu0_i_uc_atrans,
		      "monitor=1,bus=10,counter=0");
PMU_EVENT_ATTR_STRING(vpu0_i_uc_atrans.scale, rpi_axi_pmu_event_vpu0_i_uc_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu0_i_uc_atrans.unit, rpi_axi_pmu_event_vpu0_i_uc_atrans_unit,
		      "Bytes");

/* VPU System Uncached Memory Bus Events */
PMU_EVENT_ATTR_STRING(vpu_system_uc_atrans, rpi_axi_pmu_event_vpu_system_uc_atrans,
		      "monitor=1,bus=11,counter=0");
PMU_EVENT_ATTR_STRING(vpu_system_uc_atrans.scale, rpi_axi_pmu_event_vpu_system_uc_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_system_uc_atrans.unit, rpi_axi_pmu_event_vpu_system_uc_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu_system_uc_wtrans, rpi_axi_pmu_event_vpu_system_uc_wtrans,
		      "monitor=1,bus=11,counter=2");
PMU_EVENT_ATTR_STRING(vpu_system_uc_wtrans.scale, rpi_axi_pmu_event_vpu_system_uc_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_system_uc_wtrans.unit, rpi_axi_pmu_event_vpu_system_uc_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu_system_uc_rtrans, rpi_axi_pmu_event_vpu_system_uc_rtrans,
		      "monitor=1,bus=11,counter=4");
PMU_EVENT_ATTR_STRING(vpu_system_uc_rtrans.scale, rpi_axi_pmu_event_vpu_system_uc_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_system_uc_rtrans.unit, rpi_axi_pmu_event_vpu_system_uc_rtrans_unit,
		      "Bytes");

/* VPU L2 Cache Outbound & VPU DMA Uncached Memory Bus Events */
PMU_EVENT_ATTR_STRING(vpu_l2_out_atrans, rpi_axi_pmu_event_vpu_l2_out_atrans,
		      "monitor=1,bus=12,counter=0");
PMU_EVENT_ATTR_STRING(vpu_l2_out_atrans.scale, rpi_axi_pmu_event_vpu_l2_out_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_l2_out_atrans.unit, rpi_axi_pmu_event_vpu_l2_out_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_atrans, rpi_axi_pmu_event_vpu_dma_uc_atrans,
		      "monitor=1,bus=13,counter=0");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_atrans.scale, rpi_axi_pmu_event_vpu_dma_uc_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_dma_uc_atrans.unit, rpi_axi_pmu_event_vpu_dma_uc_atrans_unit,
		      "Bytes");

/* VPU SDRAM Memory Controller Bus Events & Stall Wait Cycles */
PMU_EVENT_ATTR_STRING(vpu_sdram_atrans, rpi_axi_pmu_event_vpu_sdram_atrans,
		      "monitor=1,bus=14,counter=0");
PMU_EVENT_ATTR_STRING(vpu_sdram_atrans.scale, rpi_axi_pmu_event_vpu_sdram_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_sdram_atrans.unit, rpi_axi_pmu_event_vpu_sdram_atrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu_sdram_atwait, rpi_axi_pmu_event_vpu_sdram_atwait,
		      "monitor=1,bus=14,counter=1");
PMU_EVENT_ATTR_STRING(vpu_sdram_wtrans, rpi_axi_pmu_event_vpu_sdram_wtrans,
		      "monitor=1,bus=14,counter=2");
PMU_EVENT_ATTR_STRING(vpu_sdram_wtrans.scale, rpi_axi_pmu_event_vpu_sdram_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_sdram_wtrans.unit, rpi_axi_pmu_event_vpu_sdram_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu_sdram_wtwait, rpi_axi_pmu_event_vpu_sdram_wtwait,
		      "monitor=1,bus=14,counter=3");
PMU_EVENT_ATTR_STRING(vpu_sdram_rtrans, rpi_axi_pmu_event_vpu_sdram_rtrans,
		      "monitor=1,bus=14,counter=4");
PMU_EVENT_ATTR_STRING(vpu_sdram_rtrans.scale, rpi_axi_pmu_event_vpu_sdram_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_sdram_rtrans.unit, rpi_axi_pmu_event_vpu_sdram_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(vpu_sdram_rtwait, rpi_axi_pmu_event_vpu_sdram_rtwait,
		      "monitor=1,bus=14,counter=5");

/* VPU L2 Cache Inbound Memory Bus Events */
PMU_EVENT_ATTR_STRING(vpu_l2_in_atrans, rpi_axi_pmu_event_vpu_l2_in_atrans,
		      "monitor=1,bus=15,counter=0");
PMU_EVENT_ATTR_STRING(vpu_l2_in_atrans.scale, rpi_axi_pmu_event_vpu_l2_in_atrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(vpu_l2_in_atrans.unit, rpi_axi_pmu_event_vpu_l2_in_atrans_unit,
		      "Bytes");

/* --- AXI MASTER FILTERED ALIASES (RPi 1-4, BCM2835-BCM2711) --------------- */

/* CPU Core 0 & Core 1 Instruction and Data Cache Filtered Events */
PMU_EVENT_ATTR_STRING(cpu0_icache_rtrans, rpi_axi_pmu_event_cpu0_icache_rtrans,
		      "monitor=0,bus=15,counter=4,filter=2");
PMU_EVENT_ATTR_STRING(cpu0_icache_rtrans.scale, rpi_axi_pmu_event_cpu0_icache_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(cpu0_icache_rtrans.unit, rpi_axi_pmu_event_cpu0_icache_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(cpu0_dcache_rtrans, rpi_axi_pmu_event_cpu0_dcache_rtrans,
		      "monitor=0,bus=15,counter=4,filter=3");
PMU_EVENT_ATTR_STRING(cpu0_dcache_rtrans.scale, rpi_axi_pmu_event_cpu0_dcache_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(cpu0_dcache_rtrans.unit, rpi_axi_pmu_event_cpu0_dcache_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(cpu0_dcache_wtrans, rpi_axi_pmu_event_cpu0_dcache_wtrans,
		      "monitor=0,bus=15,counter=2,filter=3");
PMU_EVENT_ATTR_STRING(cpu0_dcache_wtrans.scale, rpi_axi_pmu_event_cpu0_dcache_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(cpu0_dcache_wtrans.unit, rpi_axi_pmu_event_cpu0_dcache_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(cpu1_icache_rtrans, rpi_axi_pmu_event_cpu1_icache_rtrans,
		      "monitor=0,bus=15,counter=4,filter=5");
PMU_EVENT_ATTR_STRING(cpu1_icache_rtrans.scale, rpi_axi_pmu_event_cpu1_icache_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(cpu1_icache_rtrans.unit, rpi_axi_pmu_event_cpu1_icache_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(cpu1_dcache_rtrans, rpi_axi_pmu_event_cpu1_dcache_rtrans,
		      "monitor=0,bus=15,counter=4,filter=6");
PMU_EVENT_ATTR_STRING(cpu1_dcache_rtrans.scale, rpi_axi_pmu_event_cpu1_dcache_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(cpu1_dcache_rtrans.unit, rpi_axi_pmu_event_cpu1_dcache_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(cpu1_dcache_wtrans, rpi_axi_pmu_event_cpu1_dcache_wtrans,
		      "monitor=0,bus=15,counter=2,filter=6");
PMU_EVENT_ATTR_STRING(cpu1_dcache_wtrans.scale, rpi_axi_pmu_event_cpu1_dcache_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(cpu1_dcache_wtrans.unit, rpi_axi_pmu_event_cpu1_dcache_wtrans_unit,
		      "Bytes");

/* System DMA Channels 0 & 1 L2 Cache Filtered Events */
PMU_EVENT_ATTR_STRING(dma0_l2_rtrans, rpi_axi_pmu_event_dma0_l2_rtrans,
		      "monitor=0,bus=0,counter=4,filter=16");
PMU_EVENT_ATTR_STRING(dma0_l2_rtrans.scale, rpi_axi_pmu_event_dma0_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma0_l2_rtrans.unit, rpi_axi_pmu_event_dma0_l2_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(dma0_l2_wtrans, rpi_axi_pmu_event_dma0_l2_wtrans,
		      "monitor=0,bus=0,counter=2,filter=16");
PMU_EVENT_ATTR_STRING(dma0_l2_wtrans.scale, rpi_axi_pmu_event_dma0_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma0_l2_wtrans.unit, rpi_axi_pmu_event_dma0_l2_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(dma1_l2_rtrans, rpi_axi_pmu_event_dma1_l2_rtrans,
		      "monitor=0,bus=0,counter=4,filter=17");
PMU_EVENT_ATTR_STRING(dma1_l2_rtrans.scale, rpi_axi_pmu_event_dma1_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma1_l2_rtrans.unit, rpi_axi_pmu_event_dma1_l2_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(dma1_l2_wtrans, rpi_axi_pmu_event_dma1_l2_wtrans,
		      "monitor=0,bus=0,counter=2,filter=17");
PMU_EVENT_ATTR_STRING(dma1_l2_wtrans.scale, rpi_axi_pmu_event_dma1_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(dma1_l2_wtrans.unit, rpi_axi_pmu_event_dma1_l2_wtrans_unit,
		      "Bytes");

/* VideoCore V3D Pipes 0 & 1 L2 Cache Filtered Events */
PMU_EVENT_ATTR_STRING(v3d0_system_l2_rtrans, rpi_axi_pmu_event_v3d0_system_l2_rtrans,
		      "monitor=0,bus=5,counter=4,filter=25");
PMU_EVENT_ATTR_STRING(v3d0_system_l2_rtrans.scale, rpi_axi_pmu_event_v3d0_system_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(v3d0_system_l2_rtrans.unit, rpi_axi_pmu_event_v3d0_system_l2_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(v3d0_system_l2_wtrans, rpi_axi_pmu_event_v3d0_system_l2_wtrans,
		      "monitor=0,bus=5,counter=2,filter=25");
PMU_EVENT_ATTR_STRING(v3d0_system_l2_wtrans.scale, rpi_axi_pmu_event_v3d0_system_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(v3d0_system_l2_wtrans.unit, rpi_axi_pmu_event_v3d0_system_l2_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(v3d1_system_l2_rtrans, rpi_axi_pmu_event_v3d1_system_l2_rtrans,
		      "monitor=0,bus=5,counter=4,filter=26");
PMU_EVENT_ATTR_STRING(v3d1_system_l2_rtrans.scale, rpi_axi_pmu_event_v3d1_system_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(v3d1_system_l2_rtrans.unit, rpi_axi_pmu_event_v3d1_system_l2_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(v3d1_system_l2_wtrans, rpi_axi_pmu_event_v3d1_system_l2_wtrans,
		      "monitor=0,bus=5,counter=2,filter=26");
PMU_EVENT_ATTR_STRING(v3d1_system_l2_wtrans.scale, rpi_axi_pmu_event_v3d1_system_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(v3d1_system_l2_wtrans.unit, rpi_axi_pmu_event_v3d1_system_l2_wtrans_unit,
		      "Bytes");

/* HVS Display Engine, ISP Camera, and USB 2.0 Host Controller Filtered Events */
PMU_EVENT_ATTR_STRING(hvs_system_l2_rtrans, rpi_axi_pmu_event_hvs_system_l2_rtrans,
		      "monitor=0,bus=5,counter=4,filter=10");
PMU_EVENT_ATTR_STRING(hvs_system_l2_rtrans.scale, rpi_axi_pmu_event_hvs_system_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(hvs_system_l2_rtrans.unit, rpi_axi_pmu_event_hvs_system_l2_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(hvs_system_l2_wtrans, rpi_axi_pmu_event_hvs_system_l2_wtrans,
		      "monitor=0,bus=5,counter=2,filter=10");
PMU_EVENT_ATTR_STRING(hvs_system_l2_wtrans.scale, rpi_axi_pmu_event_hvs_system_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(hvs_system_l2_wtrans.unit, rpi_axi_pmu_event_hvs_system_l2_wtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(usb_system_l2_rtrans, rpi_axi_pmu_event_usb_system_l2_rtrans,
		      "monitor=0,bus=5,counter=4,filter=24");
PMU_EVENT_ATTR_STRING(usb_system_l2_rtrans.scale, rpi_axi_pmu_event_usb_system_l2_rtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(usb_system_l2_rtrans.unit, rpi_axi_pmu_event_usb_system_l2_rtrans_unit,
		      "Bytes");
PMU_EVENT_ATTR_STRING(usb_system_l2_wtrans, rpi_axi_pmu_event_usb_system_l2_wtrans,
		      "monitor=0,bus=5,counter=2,filter=24");
PMU_EVENT_ATTR_STRING(usb_system_l2_wtrans.scale, rpi_axi_pmu_event_usb_system_l2_wtrans_scale,
		      "32");
PMU_EVENT_ATTR_STRING(usb_system_l2_wtrans.unit, rpi_axi_pmu_event_usb_system_l2_wtrans_unit,
		      "Bytes");

static struct attribute *rpi_axi_pmu_events_attrs[] = {
	/* System Monitor events */
	&rpi_axi_pmu_event_dma_l2_atrans.attr.attr,
	&rpi_axi_pmu_event_dma_l2_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma_l2_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma_l2_atwait.attr.attr,
	&rpi_axi_pmu_event_dma_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_dma_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma_l2_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma_l2_wtwait.attr.attr,
	&rpi_axi_pmu_event_dma_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_dma_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma_l2_rtwait.attr.attr,
	&rpi_axi_pmu_event_trans_atrans.attr.attr,
	&rpi_axi_pmu_event_trans_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_trans_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_trans_atwait.attr.attr,
	&rpi_axi_pmu_event_trans_wtrans.attr.attr,
	&rpi_axi_pmu_event_trans_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_trans_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_trans_wtwait.attr.attr,
	&rpi_axi_pmu_event_trans_rtrans.attr.attr,
	&rpi_axi_pmu_event_trans_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_trans_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_trans_rtwait.attr.attr,
	&rpi_axi_pmu_event_jpeg_atrans.attr.attr,
	&rpi_axi_pmu_event_jpeg_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_jpeg_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_jpeg_atwait.attr.attr,
	&rpi_axi_pmu_event_jpeg_wtrans.attr.attr,
	&rpi_axi_pmu_event_jpeg_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_jpeg_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_jpeg_wtwait.attr.attr,
	&rpi_axi_pmu_event_jpeg_rtrans.attr.attr,
	&rpi_axi_pmu_event_jpeg_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_jpeg_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_jpeg_rtwait.attr.attr,
	&rpi_axi_pmu_event_system_uc_atrans.attr.attr,
	&rpi_axi_pmu_event_system_uc_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_system_uc_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_system_uc_atwait.attr.attr,
	&rpi_axi_pmu_event_system_uc_wtrans.attr.attr,
	&rpi_axi_pmu_event_system_uc_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_system_uc_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_system_uc_wtwait.attr.attr,
	&rpi_axi_pmu_event_system_uc_rtrans.attr.attr,
	&rpi_axi_pmu_event_system_uc_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_system_uc_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_system_uc_rtwait.attr.attr,
	&rpi_axi_pmu_event_dma_uc_atrans.attr.attr,
	&rpi_axi_pmu_event_dma_uc_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma_uc_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma_uc_atwait.attr.attr,
	&rpi_axi_pmu_event_dma_uc_wtrans.attr.attr,
	&rpi_axi_pmu_event_dma_uc_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma_uc_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma_uc_wtwait.attr.attr,
	&rpi_axi_pmu_event_dma_uc_rtrans.attr.attr,
	&rpi_axi_pmu_event_dma_uc_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma_uc_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma_uc_rtwait.attr.attr,
	&rpi_axi_pmu_event_system_l2_atrans.attr.attr,
	&rpi_axi_pmu_event_system_l2_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_system_l2_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_system_l2_atwait.attr.attr,
	&rpi_axi_pmu_event_system_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_system_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_system_l2_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_system_l2_wtwait.attr.attr,
	&rpi_axi_pmu_event_system_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_system_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_system_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_system_l2_rtwait.attr.attr,
	&rpi_axi_pmu_event_ccp2tx_atrans.attr.attr,
	&rpi_axi_pmu_event_ccp2tx_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_ccp2tx_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_mphi_rx_atrans.attr.attr,
	&rpi_axi_pmu_event_mphi_rx_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_mphi_rx_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_mphi_tx_atrans.attr.attr,
	&rpi_axi_pmu_event_mphi_tx_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_mphi_tx_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_hvs_atrans.attr.attr,
	&rpi_axi_pmu_event_hvs_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_hvs_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_hvs_atwait.attr.attr,
	&rpi_axi_pmu_event_hvs_wtrans.attr.attr,
	&rpi_axi_pmu_event_hvs_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_hvs_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_hvs_wtwait.attr.attr,
	&rpi_axi_pmu_event_hvs_rtrans.attr.attr,
	&rpi_axi_pmu_event_hvs_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_hvs_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_hvs_rtwait.attr.attr,
	&rpi_axi_pmu_event_h264_atrans.attr.attr,
	&rpi_axi_pmu_event_h264_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_h264_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_h264_atwait.attr.attr,
	&rpi_axi_pmu_event_h264_wtrans.attr.attr,
	&rpi_axi_pmu_event_h264_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_h264_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_h264_wtwait.attr.attr,
	&rpi_axi_pmu_event_h264_rtrans.attr.attr,
	&rpi_axi_pmu_event_h264_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_h264_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_h264_rtwait.attr.attr,
	&rpi_axi_pmu_event_v3d_atrans.attr.attr,
	&rpi_axi_pmu_event_v3d_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_v3d_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_v3d_atwait.attr.attr,
	&rpi_axi_pmu_event_v3d_wtrans.attr.attr,
	&rpi_axi_pmu_event_v3d_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_v3d_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_v3d_wtwait.attr.attr,
	&rpi_axi_pmu_event_v3d_rtrans.attr.attr,
	&rpi_axi_pmu_event_v3d_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_v3d_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_v3d_rtwait.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_atrans.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_atwait.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_wtwait.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_cpu_l2_rtwait.attr.attr,
	/* VPU Monitor events (RPi 1-4) */
	&rpi_axi_pmu_event_vpu1_d_l2_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu1_d_l2_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu1_d_l2_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu0_d_l2_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu0_d_l2_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu0_d_l2_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu1_i_l2_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu1_i_l2_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu1_i_l2_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu0_i_l2_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu0_i_l2_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu0_i_l2_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_system_l2_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu_system_l2_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_system_l2_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_system_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_vpu_system_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_system_l2_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_system_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_vpu_system_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_system_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_l2_flush_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu_l2_flush_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_l2_flush_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_dma_l2_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu_dma_l2_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_dma_l2_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu1_d_uc_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu1_d_uc_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu1_d_uc_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu0_d_uc_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu0_d_uc_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu0_d_uc_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu1_i_uc_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu1_i_uc_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu1_i_uc_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu0_i_uc_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu0_i_uc_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu0_i_uc_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_system_uc_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu_system_uc_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_system_uc_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_system_uc_wtrans.attr.attr,
	&rpi_axi_pmu_event_vpu_system_uc_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_system_uc_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_system_uc_rtrans.attr.attr,
	&rpi_axi_pmu_event_vpu_system_uc_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_system_uc_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_l2_out_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu_l2_out_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_l2_out_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_dma_uc_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu_dma_uc_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_dma_uc_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_atrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_atwait.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_wtrans.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_wtwait.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_rtrans.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_vpu_sdram_rtwait.attr.attr,
	&rpi_axi_pmu_event_vpu_l2_in_atrans.attr.attr,
	&rpi_axi_pmu_event_vpu_l2_in_atrans_scale.attr.attr,
	&rpi_axi_pmu_event_vpu_l2_in_atrans_unit.attr.attr,
	/* Filtered Event Aliases (RPi 1-4) */
	&rpi_axi_pmu_event_cpu0_icache_rtrans.attr.attr,
	&rpi_axi_pmu_event_cpu0_icache_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_cpu0_icache_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_cpu0_dcache_rtrans.attr.attr,
	&rpi_axi_pmu_event_cpu0_dcache_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_cpu0_dcache_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_cpu0_dcache_wtrans.attr.attr,
	&rpi_axi_pmu_event_cpu0_dcache_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_cpu0_dcache_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_cpu1_icache_rtrans.attr.attr,
	&rpi_axi_pmu_event_cpu1_icache_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_cpu1_icache_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_cpu1_dcache_rtrans.attr.attr,
	&rpi_axi_pmu_event_cpu1_dcache_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_cpu1_dcache_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_cpu1_dcache_wtrans.attr.attr,
	&rpi_axi_pmu_event_cpu1_dcache_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_cpu1_dcache_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma0_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_dma0_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma0_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma0_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_dma0_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma0_l2_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma1_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_dma1_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma1_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_dma1_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_dma1_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_dma1_l2_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_v3d0_system_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_v3d0_system_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_v3d0_system_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_v3d0_system_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_v3d0_system_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_v3d0_system_l2_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_v3d1_system_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_v3d1_system_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_v3d1_system_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_v3d1_system_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_v3d1_system_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_v3d1_system_l2_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_hvs_system_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_hvs_system_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_hvs_system_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_hvs_system_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_hvs_system_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_hvs_system_l2_wtrans_unit.attr.attr,
	&rpi_axi_pmu_event_usb_system_l2_rtrans.attr.attr,
	&rpi_axi_pmu_event_usb_system_l2_rtrans_scale.attr.attr,
	&rpi_axi_pmu_event_usb_system_l2_rtrans_unit.attr.attr,
	&rpi_axi_pmu_event_usb_system_l2_wtrans.attr.attr,
	&rpi_axi_pmu_event_usb_system_l2_wtrans_scale.attr.attr,
	&rpi_axi_pmu_event_usb_system_l2_wtrans_unit.attr.attr,
	NULL,
};

static const struct attribute_group rpi_axi_pmu_events_group = {
	.name = "events",
	.attrs = rpi_axi_pmu_events_attrs,
};

/**
 * cpumask_show() - Sysfs attribute callback to display assigned PMU CPU core
 * @dev: Pointer to device structure
 * @attr: Pointer to device attribute
 * @buf: Output buffer for cpumask string representation
 *
 * Return: Number of bytes written to buffer.
 */
static ssize_t cpumask_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct rpi_axi_pmu *rpi_pmu = pmu_to_rpi_axi_pmu(pmu);

	return cpumap_print_to_pagebuf(true, buf, cpumask_of(rpi_pmu->cpu));
}
static DEVICE_ATTR_RO(cpumask);

static struct attribute *rpi_axi_pmu_cpumask_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static const struct attribute_group rpi_axi_pmu_cpumask_group = {
	.attrs = rpi_axi_pmu_cpumask_attrs,
};

static const struct attribute_group *rpi_axi_pmu_attr_groups[] = {
	&rpi_axi_pmu_format_group,
	&rpi_axi_pmu_events_group,
	&rpi_axi_pmu_cpumask_group,
	NULL,
};

/**
 * rpi_axi_pmu__validate_event() - Validates single event resource capability
 * @pmu: Pointer to core PMU struct
 * @fake_hw_events: Temporary array of watcher structures for pre-flight check
 * @event: Pointer to perf_event being validated
 *
 * Return: true if event can be scheduled, false otherwise.
 */
static bool rpi_axi_pmu__validate_event(struct pmu *pmu,
					struct rpi_axi_hw_events fake_hw_events[MON_MAX],
					struct perf_event *event)
{
	enum monitor mon;

	if (is_software_event(event))
		return true;
	if (event->pmu != pmu)
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

	rpi_axi_hw_events__init(&fake_hw_events[MON_SYSTEM]);
	rpi_axi_hw_events__init(&fake_hw_events[MON_VPU]);

	if (!rpi_axi_pmu__validate_event(event->pmu, fake_hw_events, leader))
		return false;

	for_each_sibling_event(sibling, leader) {
		if (!rpi_axi_pmu__validate_event(event->pmu, fake_hw_events, sibling))
			return false;
	}
	return rpi_axi_pmu__validate_event(event->pmu, fake_hw_events, event);
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

	if (!config_is_valid(pmu, event->attr.config)) {
		dev_dbg(dev, "Invalid event config\n");
		return -EINVAL;
	}

	if (is_sampling_event(event)) {
		dev_dbg(dev, "Sampling not supported\n");
		return -EOPNOTSUPP;
	}

	if (event->cpu < 0) {
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
		u32 tmp[3] = {pmu->monitor[mon].mailbox + GEN_CTRL, 1, set};
		int err;

		might_sleep();
		lockdep_assert_held(&pmu->vpu_mutex);
		if (WARN_ON_ONCE(in_interrupt() || irqs_disabled()))
			return;

		err = rpi_firmware_property(pmu->firmware,
					    RPI_FIRMWARE_SET_PERIPH_REG,
					    tmp, sizeof(tmp));

		if (err < 0 || tmp[1] != 1)
			dev_err(&pmu->pdev->dev, "Failed to set monitor control\n");
	} else {
		lockdep_assert_held(&pmu->lock);
		writel(set, pmu->monitor[mon].base_address + GEN_CTRL);
	}
}

static int watcher_offset(const struct rpi_axi_pmu *pmu, enum monitor mon, int idx)
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
	int watcher = watcher_offset(pmu, mon, idx);

	if (pmu->monitor[mon].use_mailbox_interface) {
		u32 tmp[3] = {pmu->monitor[mon].mailbox + watcher, 1, set};
		int err;

		might_sleep();
		lockdep_assert_held(&pmu->vpu_mutex);
		if (WARN_ON_ONCE(in_interrupt() || irqs_disabled()))
			return;

		err = rpi_firmware_property(pmu->firmware,
					    RPI_FIRMWARE_SET_PERIPH_REG,
					    tmp, sizeof(tmp));
		if (err < 0 || tmp[1] != 1)
			dev_err(&pmu->pdev->dev, "Failed to set bus watcher control\n");
	} else {
		lockdep_assert_held(&pmu->lock);
		writel(set, pmu->monitor[mon].base_address + watcher);
	}
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

	if (pmu->monitor[mon].hw_events.enabled[idx])
		return;

	bus_control = BW_CTRL_ENABLE_BIT | (bus & 0x3F);
	if (filter) {
		bus_control |= BW_CTRL_ENABLE_ID_FILTER_BIT;
		bus_control |= (filter & 0x1F) << BW_CTRL_BUS_FILTER_SHIFT;
	}
	set_bus_watcher_control(pmu, mon, idx, BW_CTRL_RESET_BIT);
	set_bus_watcher_control(pmu, mon, idx, bus_control);

	if (!pmu->monitor[mon].hw_events.monitor_running) {
		set_monitor_control(pmu, mon, GEN_CTL_RESET_BIT);
		set_monitor_control(pmu, mon, GEN_CTL_ENABLE_BIT | GEN_CTL_WATCH_BIT);
	}
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
	case CNT_WTRANS: return BW_WTRANS_OFFSET;
	case CNT_WTWAIT: return BW_WTWAIT_OFFSET;
	case CNT_RTRANS: return BW_RTRANS_OFFSET;
	case CNT_RTWAIT: return BW_RTWAIT_OFFSET;
	default: return 0;
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
	int watcher = watcher_offset(pmu, mon, idx);
	int offset = counter_offset(counter);
	u32 ret;

	/* Use READ_ONCE to prevent KCSAN data race warnings during lockless IPC reads */
	if (!READ_ONCE(pmu->monitor[mon].hw_events.enabled[idx]))
		return 0;

	if (pmu->monitor[mon].use_mailbox_interface) {
		u32 tmp[3] = {
			pmu->monitor[mon].mailbox + watcher + offset,
			1, -1
		};
		int err;

		might_sleep();
		lockdep_assert_held(&pmu->vpu_mutex);
		if (WARN_ON_ONCE(in_interrupt() || irqs_disabled()))
			return -1;

		err = rpi_firmware_property(pmu->firmware,
					    RPI_FIRMWARE_GET_PERIPH_REG,
					    tmp, sizeof(tmp));

		if (err < 0 || tmp[1] != 1) {
			dev_err(&pmu->pdev->dev, "Failed to read bus watcher\n");
			/*
			 * Return U32_MAX on IPC failure to indicate invalid read.
			 * Valid hardware counters are 31-bit and masked to 0x7FFFFFFF,
			 * so U32_MAX is unambiguously an error state.
			 */
			return U32_MAX;
		}
		ret = tmp[2] & 0x7FFFFFFF;
	} else {
		void __iomem *addr = pmu->monitor[mon].base_address + watcher + offset;

		lockdep_assert_held(&pmu->lock);
		ret = readl(addr) & 0x7FFFFFFF;
	}
	return ret;
}

/**
 * rpi_axi_pmu_read() - Perf callback to update event counter value
 * @event: Pointer to perf_event being read
 *
 * Thread-safe counter update implementation:
 * - System Monitor (MON_SYSTEM): Uses fast MMIO (~15ns), serialized via pmu->lock spinlock.
 * - VPU Monitor (MON_VPU): Async background polling in process context (vpu_work).
 *   Direct read() syscalls return cached count immediately without blocking.
 */
static void rpi_axi_pmu_read(struct perf_event *event)
{
	struct rpi_axi_pmu *pmu = pmu_to_rpi_axi_pmu(event->pmu);
	enum monitor mon = config_to_monitor(event->attr.config);
	enum counter counter = config_to_counter(event->attr.config);
	u64 prev_count, new_count;
	unsigned long flags;
	u32 delta;

	/* Mailbox VPU counters are polled asynchronously in background vpu_work.
	 * MMIO monitors (System) are read synchronously.
	 */
	if (event->hw.idx < 0)
		return;

	if (pmu->monitor[mon].use_mailbox_interface)
		return;

	raw_spin_lock_irqsave(&pmu->lock, flags);
	if (!pmu->monitor[mon].hw_events.enabled[event->hw.idx] ||
	    (event->hw.state & PERF_HES_STOPPED)) {
		raw_spin_unlock_irqrestore(&pmu->lock, flags);
		return;
	}
	prev_count = local64_read(&event->hw.prev_count);
	new_count = rpi_axi_pmu_read_counter(pmu, mon, event->hw.idx, counter);
	if (new_count == U32_MAX) {
		/*
		 * U32_MAX indicates a hardware or IPC read failure. Ignore the update
		 * to prevent spurious artificial counter spikes from underflows.
		 */
		raw_spin_unlock_irqrestore(&pmu->lock, flags);
		return;
	}
	local64_set(&event->hw.prev_count, new_count);
	delta = (new_count - prev_count) & 0x7FFFFFFF;
	local64_add(delta, &event->count);
	raw_spin_unlock_irqrestore(&pmu->lock, flags);
}

/**
 * rpi_axi_pmu_vpu_work_handler() - Background work handler for VPU monitor counter reads
 * @work: Pointer to work_struct inside struct rpi_axi_pmu
 *
 * Runs in kernel process context where sleeping is allowed. Reads fresh VPU counter values over
 * VideoCore Mailbox IPC under vpu_mutex and updates the cached perf event count safely.
 * ABA pointer recycling races are prevented by validating per-slot sequence counters (event_gen).
 * Multiplexing counter rotation baselines are established using the PERF_HES_UPTODATE flag.
 */
static void rpi_axi_pmu_vpu_work_handler(struct work_struct *work)
{
	struct rpi_axi_pmu *pmu = container_of(work, struct rpi_axi_pmu, vpu_work);

	might_sleep();
	mutex_lock(&pmu->vpu_mutex);
	raw_spin_lock_irq(&pmu->lock);

	for (int idx = 0; idx < NUM_BUS_WATCHERS_PER_MONITOR; idx++) {
		if (pmu->monitor[MON_VPU].hw_events.vpu_disable_pending[idx]) {
			pmu->monitor[MON_VPU].hw_events.vpu_disable_pending[idx] = false;
			if (pmu->monitor[MON_VPU].use_mailbox_interface) {
				raw_spin_unlock_irq(&pmu->lock);
				rpi_axi_pmu_disable_bus_watcher(pmu, MON_VPU, idx);
				raw_spin_lock_irq(&pmu->lock);
			} else {
				rpi_axi_pmu_disable_bus_watcher(pmu, MON_VPU, idx);
			}
		}
	}

	for (int i = 0; i < RPI_AXI_MAX_EVENTS; i++) {
		struct perf_event *event = pmu->events[i];
		enum counter counter;
		u64 gen;
		int idx;
		u64 prev_count, new_count;
		u32 delta;

		if (!event || (event->hw.state & PERF_HES_STOPPED) ||
		    config_to_monitor(event->attr.config) != MON_VPU)
			continue;

		gen = pmu->event_gen[i];
		counter = config_to_counter(event->attr.config);
		idx = event->hw.idx;

		/* If VPU bus watcher is not enabled on hardware, enable it in process context */
		if (!pmu->monitor[MON_VPU].hw_events.enabled[idx]) {
			int bus = pmu->monitor[MON_VPU].hw_events.monitored_bus[idx];
			int filter = pmu->monitor[MON_VPU].hw_events.filter[idx];

			if (pmu->monitor[MON_VPU].use_mailbox_interface) {
				raw_spin_unlock_irq(&pmu->lock);
				rpi_axi_pmu_enable_bus_watcher(pmu, MON_VPU, idx, bus, filter);
				raw_spin_lock_irq(&pmu->lock);
			} else {
				rpi_axi_pmu_enable_bus_watcher(pmu, MON_VPU, idx, bus, filter);
			}
			pmu->monitor[MON_VPU].hw_events.monitor_running = true;
		}

		/* Drop spinlock during Mailbox IPC read only if using mailbox interface */
		if (pmu->monitor[MON_VPU].use_mailbox_interface) {
			raw_spin_unlock_irq(&pmu->lock);
			new_count = rpi_axi_pmu_read_counter(pmu, MON_VPU, idx, counter);
			raw_spin_lock_irq(&pmu->lock);
		} else {
			new_count = rpi_axi_pmu_read_counter(pmu, MON_VPU, idx, counter);
		}

		/*
		 * U32_MAX indicates a hardware or IPC read failure. Ignore the update
		 * to prevent spurious artificial counter spikes from underflows.
		 */
		if (new_count == U32_MAX)
			continue;

		/*
		 * Verify event pointer and generation sequence counter match
		 * (ABA pointer recycling race prevention).
		 */
		if (pmu->events[i] == event &&
		    pmu->event_gen[i] == gen &&
		    !(event->hw.state & PERF_HES_STOPPED)) {
			WRITE_ONCE(pmu->monitor[MON_VPU].hw_events.enabled[idx], true);
			if (!(event->hw.state & PERF_HES_UPTODATE)) {
				/* Initial baseline read for newly started/rotated VPU event */
				local64_set(&event->hw.prev_count, new_count);
				event->hw.state |= PERF_HES_UPTODATE;
			} else {
				prev_count = local64_read(&event->hw.prev_count);
				local64_set(&event->hw.prev_count, new_count);
				delta = (new_count - prev_count) & 0x7FFFFFFF;
				local64_add(delta, &event->count);
			}
		}
	}

	if (pmu->monitor[MON_VPU].hw_events.num_monitored == 0 &&
	    pmu->monitor[MON_VPU].hw_events.monitor_running) {
		if (pmu->monitor[MON_VPU].use_mailbox_interface) {
			raw_spin_unlock_irq(&pmu->lock);
			set_monitor_control(pmu, MON_VPU, GEN_CTL_RESET_BIT);
			raw_spin_lock_irq(&pmu->lock);
		} else {
			set_monitor_control(pmu, MON_VPU, GEN_CTL_RESET_BIT);
		}
		pmu->monitor[MON_VPU].hw_events.monitor_running = false;
	}

	raw_spin_unlock_irq(&pmu->lock);
	mutex_unlock(&pmu->vpu_mutex);
}

/**
 * rpi_axi_pmu_timer_handler() - Periodic hrtimer callback for 32-bit counter overflow polling
 * @timer: Pointer to hrtimer structure
 *
 * Runs in hard IRQ / atomic context.
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
		u64 prev_count, new_count;
		u32 delta;

		if (!event || (event->hw.state & PERF_HES_STOPPED) ||
		    config_to_monitor(event->attr.config) != MON_SYSTEM)
			continue;

		counter = config_to_counter(event->attr.config);
		prev_count = local64_read(&event->hw.prev_count);
		new_count = rpi_axi_pmu_read_counter(pmu, MON_SYSTEM,
						    event->hw.idx, counter);
		/*
		 * U32_MAX indicates a hardware or IPC read failure. Ignore the update
		 * to prevent spurious artificial counter spikes from underflows.
		 */
		if (new_count != U32_MAX) {
			local64_set(&event->hw.prev_count, new_count);
			delta = (new_count - prev_count) & 0x7FFFFFFF;
			local64_add(delta, &event->count);
		}
	}

	if (pmu->active_vpu_events > 0)
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
	event->hw.state = 0;
	if (mon == MON_SYSTEM) {
		int bus = pmu->monitor[mon].hw_events.monitored_bus[event->hw.idx];
		int filter = pmu->monitor[mon].hw_events.filter[event->hw.idx];

		rpi_axi_pmu_enable_bus_watcher(pmu, mon, event->hw.idx, bus, filter);
		WRITE_ONCE(pmu->monitor[mon].hw_events.enabled[event->hw.idx], true);
		pmu->monitor[mon].hw_events.monitor_running = true;
		local64_set(&event->hw.prev_count,
			    rpi_axi_pmu_read_counter(pmu, mon, event->hw.idx, counter));
		event->hw.state |= PERF_HES_UPTODATE;
	} else if (mon == MON_VPU) {
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
			pmu->event_gen[i]++;
			break;
		}
	}

	if (slot < 0) {
		hw_events->refcount[idx]--;
		if (hw_events->refcount[idx] == 0) {
			hw_events->monitored_bus[idx] = -1;
			hw_events->filter[idx] = BCM2835_FLT_NONE;
			hw_events->num_monitored--;
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
	unsigned long spinflags;

	if (event->hw.state & PERF_HES_STOPPED)
		return;

	if (flags & PERF_EF_UPDATE)
		rpi_axi_pmu_read(event);

	raw_spin_lock_irqsave(&pmu->lock, spinflags);
	if (flags & PERF_EF_UPDATE)
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
			pmu->event_gen[i]++;
			break;
		}
	}

	if (pmu->monitor[mon].hw_events.monitored_bus[idx] >= 0) {
		pmu->monitor[mon].hw_events.refcount[idx]--;
		if (pmu->monitor[mon].hw_events.refcount[idx] == 0) {
			pmu->monitor[mon].hw_events.monitored_bus[idx] = -1;
			pmu->monitor[mon].hw_events.filter[idx] = BCM2835_FLT_NONE;
			pmu->monitor[mon].hw_events.num_monitored--;
			if (mon == MON_SYSTEM) {
				WRITE_ONCE(pmu->monitor[MON_SYSTEM].hw_events.enabled[idx], false);
				rpi_axi_pmu_disable_bus_watcher(pmu, mon, idx);
				if (pmu->monitor[mon].hw_events.num_monitored == 0) {
					set_monitor_control(pmu, mon, GEN_CTL_RESET_BIT);
					pmu->monitor[mon].hw_events.monitor_running = false;
				}
			} else if (mon == MON_VPU) {
				WRITE_ONCE(pmu->monitor[MON_VPU].hw_events.enabled[idx], false);
				pmu->monitor[MON_VPU].hw_events.vpu_disable_pending[idx] = true;
				schedule_work(&pmu->vpu_work);
			}
		}
	}

	event->hw.idx = -1;
	pmu->active_events--;
	if (mon == MON_VPU)
		pmu->active_vpu_events--;

	if (pmu->active_events == 0) {
		raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
		return;
	}
	raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
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
	unsigned long spinflags;
	unsigned int target;

	if (cpu != pmu->cpu)
		return 0;

	target = cpumask_any_but(cpu_online_mask, cpu);
	if (target >= nr_cpu_ids)
		return 0;

	if (pmu->is_registered)
		perf_pmu_migrate_context(&pmu->pmu, cpu, target);

	pmu->cpu = target;

	raw_spin_lock_irqsave(&pmu->lock, spinflags);
	if (pmu->active_events > 0) {
		raw_spin_unlock_irqrestore(&pmu->lock, spinflags);
		hrtimer_cancel(&pmu->hrtimer);
		raw_spin_lock_irqsave(&pmu->lock, spinflags);
		if (pmu->active_events > 0)
			hrtimer_start(&pmu->hrtimer, RPI_AXI_PMU_TIMER_INTERVAL,
				      HRTIMER_MODE_REL_SOFT);
	}
	raw_spin_unlock_irqrestore(&pmu->lock, spinflags);

	return 0;
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
		.attr_groups	= rpi_axi_pmu_attr_groups,
		.task_ctx_nr    = perf_invalid_context,
		.event_init     = rpi_axi_pmu_event_init,
		.add            = rpi_axi_pmu_add,
		.del            = rpi_axi_pmu_del,
		.start          = rpi_axi_pmu_start,
		.stop           = rpi_axi_pmu_stop,
		.read           = rpi_axi_pmu_read,
		.capabilities   = PERF_PMU_CAP_NO_EXCLUDE,
	};
	pmu->pdev = pdev;
	pmu->cpu = cpumask_first(cpu_online_mask);
	hrtimer_setup(&pmu->hrtimer, rpi_axi_pmu_timer_handler, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
	INIT_WORK(&pmu->vpu_work, rpi_axi_pmu_vpu_work_handler);

	if (pmu->chip == CHIP_BCM2835) {
		pmu->monitor[MON_SYSTEM].use_mailbox_interface = false;
		fw_node = of_parse_phandle(dev->of_node, "firmware", 0);
		if (fw_node) {
			pmu->firmware = devm_rpi_firmware_get(dev, fw_node);
			of_node_put(fw_node);
			if (!pmu->firmware)
				return -EPROBE_DEFER;
		}
		pmu->monitor[MON_VPU].use_mailbox_interface = !!pmu->firmware;
	} else {
		pmu->monitor[MON_SYSTEM].use_mailbox_interface = false;
		pmu->monitor[MON_VPU].use_mailbox_interface = false;
	}

	for (int i = 0; i < MON_MAX; i++) {
		rpi_axi_hw_events__init(&pmu->monitor[i].hw_events);

		if (pmu->monitor[i].use_mailbox_interface) {
			struct resource *resource = platform_get_resource(pdev, IORESOURCE_MEM, i);

			if (!resource) {
				dev_err(dev, "Error reading mailbox resource %d\n", i);
				ret = -EINVAL;
				goto err_firmware_put;
			}
			pmu->monitor[i].mailbox = (u32)resource->start;
		} else {
			struct resource *resource = platform_get_resource(pdev, IORESOURCE_MEM, i);

			if (!resource)
				continue;

			pmu->monitor[i].base_address = devm_ioremap_resource(dev, resource);
			if (IS_ERR(pmu->monitor[i].base_address)) {
				ret = PTR_ERR(pmu->monitor[i].base_address);
				dev_err(dev, "Error devm_ioremap_resource failed %d\n", ret);
				goto err_firmware_put;
			}
		}
	}

	ret = cpuhp_state_add_instance(rpi_axi_pmu_cpuhp_state, &pmu->cpuhp_node);
	if (ret) {
		dev_err(dev, "Failed to add cpuhp instance %d\n", ret);
		goto err_teardown;
	}

	cpus_read_lock();
	ret = perf_pmu_register(&pmu->pmu, PMU_NAME, /*type=*/-1);
	if (ret) {
		cpus_read_unlock();
		dev_err(dev, "PMU register failed %d\n", ret);
		goto err_cpuhp_remove;
	}

	pmu->is_registered = true;
	cpus_read_unlock();
	return 0;

err_cpuhp_remove:
	cpuhp_state_remove_instance_nocalls(rpi_axi_pmu_cpuhp_state, &pmu->cpuhp_node);
err_teardown:
	hrtimer_cancel(&pmu->hrtimer);
	flush_work(&pmu->vpu_work);
err_firmware_put:
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
		.data = (void *)CHIP_BCM2835,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, rpi_axi_pmu_match);

static struct platform_driver rpi_axi_pmu_driver  = {
	.probe =	rpi_axi_pmu_probe,
	.remove =	rpi_axi_pmu_remove,
	.driver = {
		.name   = PMU_NAME,
		.of_match_table = of_match_ptr(rpi_axi_pmu_match),
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
				      NULL, rpi_axi_pmu_offline_cpu);
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
