.. SPDX-License-Identifier: GPL-2.0

=============================
RP1 Ethernet PTP GPIO support
=============================

The Raspberry Pi RP1 Ethernet controller exposes its Precision Time Protocol
hardware clock (PHC) through the standard Linux PTP device interface. With
``CONFIG_MACB_RP1_PPS`` enabled, the driver also exposes RP1 GPIO pins for
external timestamp input and periodic output. Applications use the standard
PTP clock ioctls; no private userspace interface is added.

The driver advertises GPIO0 through GPIO27 as PTP pins. Select an RP1 GPIO
number, not a 40-pin header pin number. Pin reservation is checked when a
channel is enabled. A pin in use by another peripheral or GPIO interrupt is
rejected. Board wiring and other pin users can make a listed pin unavailable.
This PHC interface is separate from the ``pps-rp1`` overlay, which configures
the LinuxPPS ``pps-gpio`` driver. Do not configure both interfaces on the same
GPIO.

The implementation provides two external timestamp channels and one periodic
output channel. Without PEROUT, channel 0 uses the Ethernet timestamp latch;
channel 1 requires PEROUT to supply a PHC anchor. With PEROUT active, both
EXTTS channels use separate RP1 PIO counter and DMA lanes, and each input must
use a GPIO different from the output GPIO and from the other input GPIO. The
simultaneous counter path currently supports rising edges only. Stop channel 1
before disabling PEROUT. Channel 0 returns to direct latch capture when PEROUT
is stopped.

Periodic output supports a 1-second period. The default output pulse is high
for 500 ms. A duty-cycle request can select a high time from 1 ms through
500 ms, in 5 ns increments. These are hardware programming increments, not a
statement of physical pin accuracy. Counter-derived input timestamps are
mapped into the PHC domain from output-pad monitor samples and PEROUT TSU
markers. The mapping has not been electrically calibrated, so it does not
establish a board-level accuracy bound.

Discover the PHC and the pin table first. The PHC device number may change
between boots or driver registration:

.. code-block:: sh

   sudo testptp -d /dev/ptpN -c
   sudo testptp -d /dev/ptpN -l

For rising-edge input on GPIO18 on channel 0, map the pin to the external
timestamp function, then request events. ``testptp`` disables the capture
request after reading the requested number of events:

.. code-block:: sh

   sudo testptp -d /dev/ptpN -i 0 -L 18,1
   sudo testptp -d /dev/ptpN -i 0 -E 1 -e 8

Channel 0 direct capture also accepts ``-E 2`` for falling edges and ``-E 3``
for both edges. The PTP channel index is selected with ``-i``. The mapping can
be cleared with ``-L 18,0``.

For 1 Hz output on GPIO23 with a 10 ms high pulse:

.. code-block:: sh

   sudo testptp -d /dev/ptpN -i 0 -L 23,2
   sudo testptp -d /dev/ptpN -i 0 -p 1000000000 -w 10000000

For simultaneous output and two rising-edge inputs, map three distinct GPIOs.
Enable PEROUT first, then start one capture process per EXTTS channel:

.. code-block:: sh

   sudo testptp -d /dev/ptpN -i 0 -L 23,2
   sudo testptp -d /dev/ptpN -i 0 -L 18,1
   sudo testptp -d /dev/ptpN -i 1 -L 24,1
   sudo testptp -d /dev/ptpN -i 0 -p 1000000000 -w 10000000
   sudo testptp -d /dev/ptpN -i 0 -E 1 -e 8
   sudo testptp -d /dev/ptpN -i 1 -E 1 -e 8

Disable input mappings before stopping PEROUT. The channel-1 mapping and
capture depend on active output markers:

.. code-block:: sh

   sudo testptp -d /dev/ptpN -i 1 -L 24,0
   sudo testptp -d /dev/ptpN -i 0 -L 18,0
   sudo testptp -d /dev/ptpN -i 0 -p 0
   sudo testptp -d /dev/ptpN -i 0 -L 23,0

The output request remains enabled after ``testptp`` exits. Disable it
explicitly before changing the pin mapping or removing the Ethernet device:

.. code-block:: sh

   sudo testptp -d /dev/ptpN -i 0 -p 0
   sudo testptp -d /dev/ptpN -i 0 -L 23,0

Add ``-H <nanoseconds>`` to request a phase within the 1-second period. The
output is aligned to the PHC, but physical pin delay and jitter have not been
specified by this interface. PHC time steps temporarily inhibit output while
the driver re-establishes the requested phase grid.

The driver creates diagnostic counters under debugfs when it is enabled. They
report capture collection age, input re-arm dead time, output latch error and
frequency-change handling. Debugfs records are diagnostic measurements, not a
calibration certificate or an accuracy guarantee.
