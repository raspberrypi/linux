.. SPDX-License-Identifier: GPL-2.0

==============================================================
Broadcom Raspberry Pi AXI Performance Monitoring Unit (PMU)
==============================================================

Broadcom BCM2835 and BCM2711 SoCs used in Raspberry Pi boards include two AXI
bus-watcher performance monitor blocks:

- **System monitor** (``monitor=0``): Observes the primary System AXI fabric
  and is read directly via MMIO.
- **VPU monitor** (``monitor=1``): Observes the VideoCore VPU AXI fabric and
  is polled asynchronously every 100 ms in process context through the
  Raspberry Pi firmware mailbox (``RPI_FIRMWARE_GET_PERIPH_REG`` /
  ``RPI_FIRMWARE_SET_PERIPH_REG``). If the running firmware does not grant
  access to the VPU monitor registers, the ``vpu_*`` events are hidden from
  sysfs at probe time.

Each monitor provides three programmable hardware bus watchers. A single bus
watcher monitors one ``(bus, filter)`` pair and simultaneously tracks all
counter metrics for that bus and filter. Multiple perf events targeting the
same ``(monitor, bus, filter)`` share a single hardware bus watcher without
multiplexing; monitoring more than three distinct ``(bus, filter)`` pairs on
the same monitor multiplexes the watchers via perf.

Event Format and Counters
=========================

The PMU is registered under ``/sys/bus/event_source/devices/rpi_axi_pmu`` and
exposes the following ``format`` fields:

- ``counter`` (``config:0-3``): Counter metric selector.
- ``bus`` (``config:4-8``): Monitored AXI bus index.
- ``monitor`` (``config:9-9``): ``0`` for System monitor, ``1`` for VPU
  monitor.
- ``filter`` (``config:10-15``): Optional AXI master ID filter (``0`` disables
  filtering and counts all masters on the selected bus).

Each bus watcher provides the following counter metrics:

======= ======== ====== =======================================================
Counter Suffix   Width  Description
======= ======== ====== =======================================================
0       atwait   28-bit Address transaction wait cycles
1       atrans   32-bit Address transactions (reads plus writes)
2       amax     28-bit Maximum address transaction latency (cycles)
3       wwait    28-bit Write data wait cycles
4       wtrans   32-bit Write data beats
5       wmax     28-bit Maximum write transaction latency (cycles)
6       rwait    28-bit Read data wait cycles
7       rtrans   32-bit Read data beats
8       rmax     28-bit Maximum read transaction latency (cycles)
9       rpend    8-bit  Pending read transactions (instantaneous level)
======= ======== ====== =======================================================

Level Counters (``*max`` and ``rpend``)
---------------------------------------

Whereas ``*trans`` and ``*wait`` are cumulative counters, ``amax``, ``wmax``,
``rmax``, and ``rpend`` represent hardware levels/watermarks:

- ``rpend`` is an instantaneous hardware register holding the number of reads
  pending at the time of the register read. Across each ``perf stat`` read
  interval, the driver tracks the peak ``rpend`` sampled and resets the
  per-interval baseline on each read.
- ``amax``, ``wmax``, and ``rmax`` are sticky hardware peak-hold registers
  that retain the maximum latency observed since the bus watcher was started
  (as resetting the hardware bus watcher mid-run would clear concurrent
  ``*trans`` and ``*wait`` counters sharing the watcher).

Note that level counters are only meaningful when they are not multiplexed;
when events are multiplexed, ``perf`` scales counts by enabled/running time,
which does not apply to peak/level values.

Named Event Aliases and Master Filters
======================================

Sysfs event aliases are provided under ``events/`` in the form
``<bus>_<counter>`` for the System monitor and ``vpu_<bus>_<counter>`` for the
VPU monitor.

BCM2835 / BCM2837
-----------------

- **System buses (0..15):** ``dma_l2`` (0), ``trans`` (1), ``jpeg`` (2),
  ``system_uc`` (3), ``dma_uc`` (4), ``system_l2`` (5), ``ccp2tx`` (6),
  ``mphi_rx`` (7), ``mphi_tx`` (8), ``hvs`` (9), ``h264`` (10), ``isp`` (11),
  ``v3d`` (12), ``peripheral`` (13), ``cpu_uc`` (14), ``cpu_l2`` (15).
- **VPU buses (0..14):** ``vpu_vpu1_d_l2`` (0), ``vpu_vpu0_d_l2`` (1),
  ``vpu_vpu1_i_l2`` (2), ``vpu_vpu0_i_l2`` (3), ``vpu_system_l2`` (4),
  ``vpu_dma_l2`` (5), ``vpu_vpu1_d_uc`` (6), ``vpu_vpu0_d_uc`` (7),
  ``vpu_vpu1_i_uc`` (8), ``vpu_vpu0_i_uc`` (9), ``vpu_vpu_uc`` (10),
  ``vpu_l2_out`` (11), ``vpu_dma_uc`` (12), ``vpu_l2_in`` (13),
  ``vpu_sdram`` (14).
- **5-bit AXI master ID filters (1..31):** ``CORE0_V`` (1), ``ICACHE0`` (2),
  ``DCACHE0`` (3), ``CORE1_V`` (4), ``ICACHE1`` (5), ``DCACHE1`` (6),
  ``L2_MAIN`` (7), ``HOST_PORT`` (8), ``HOST_PORT2`` (9), ``HVS`` (10),
  ``ISP`` (11), ``VIDEO_DCT`` (12), ``VIDEO_SD2AXI`` (13), ``CAM0`` (14),
  ``CAM1`` (15), ``DMA0`` (16), ``DMA1`` (17), ``DMA2_VPU`` (18),
  ``JPEG`` (19), ``VIDEO_CME`` (20), ``TRANSPOSER`` (21), ``VIDEO_FME`` (22),
  ``CCP2TX`` (23), ``USB`` (24), ``V3D0`` (25), ``V3D1`` (26), ``V3D2`` (27),
  ``AVE`` (28), ``DEBUG`` (29), ``CPU`` (30), ``M30`` (31).

BCM2711
-------

- **System buses (0..11):** ``dma_l2`` (0), ``trans`` (1), ``jpeg`` (2),
  ``vpu_uc`` (3), ``dma_uc`` (4), ``system_l2`` (5), ``hvs`` (6),
  ``argon`` (7), ``h264`` (8), ``peripheral`` (9), ``arm_uc`` (10),
  ``arm_l2`` (11).
- **VPU buses (0..13):** Same ``0..13`` layout as BCM2835 (without
  ``vpu_sdram``).
- **5-bit AXI master ID filters (1..31):** Same as BCM2835 except ``ARGON``
  (8), ``PCIE`` (9), ``DMA2`` (18), ``GIGE`` (23), ``GISB_AXI`` (28),
  ``ARM`` (30), and ``EMMCSTB`` (31).

Examples
========

Count all metrics on the ``arm_uc`` bus during a memory copy workload (all
sharing a single hardware bus watcher)::

  perf stat -a \
    -e rpi_axi_pmu/arm_uc_atrans/ \
    -e rpi_axi_pmu/arm_uc_wtrans/ \
    -e rpi_axi_pmu/arm_uc_rtrans/ \
    -e rpi_axi_pmu/arm_uc_wmax/ \
    -e rpi_axi_pmu/arm_uc_rpend/ \
    -- dd if=/dev/zero of=/dev/null bs=1M count=512

Filter ``peripheral`` bus transactions by the ARM master ID (``filter=30`` on
BCM2835/BCM2711) at 1-second intervals::

  perf stat -a -I 1000 \
    -e rpi_axi_pmu/peripheral_atrans/ \
    -e rpi_axi_pmu/peripheral_atrans,filter=30/ \
    -- sleep 5
