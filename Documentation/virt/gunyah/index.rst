.. SPDX-License-Identifier: GPL-2.0

=================
Gunyah Hypervisor
=================

.. toctree::
   :maxdepth: 1

   message-queue

Gunyah is a Type-1 hypervisor which is independent of any OS kernel, and runs in
a more privileged CPU level (EL2 on Aarch64). It does not depend on a less
privileged operating system for its core functionality. This increases its
security and can support a much smaller trusted computing base than a Type-2
hypervisor.

Gunyah is an open source hypervisor. The source repository is available at
https://github.com/quic/gunyah-hypervisor.

Gunyah provides these following features.

- Scheduling:

  A scheduler for virtual CPUs (vCPUs) on physical CPUs enables time-sharing
  of the CPUs. Gunyah supports two models of scheduling which can coexist on
  a running system:

    1. Hypervisor vCPU scheduling in which Gunyah hypervisor schedules vCPUS on
       its own. The default is a real-time priority with round-robin scheduler.
    2. "Proxy" scheduling in which an owner-VM can donate the remainder of its
       own vCPU's time slice to an owned-VM's vCPU via a hypercall.

- Memory Management:

  APIs handling memory, abstracted as objects, limiting direct use of physical
  addresses. Memory ownership and usage tracking of all memory under its control.
  Memory partitioning between VMs is a fundamental security feature.

- Interrupt Virtualization:

  Interrupt ownership is tracked and interrupt delivery is directly to the
  assigned VM. Gunyah makes use of hardware interrupt virtualization where
  possible.

- Inter-VM Communication:

  There are several different mechanisms provided for communicating between VMs.

    1. Message queues
    2. Doorbells
    3. Virtio MMIO transport
    4. Shared memory

- Virtual platform:

  Architectural devices such as interrupt controllers and CPU timers are
  directly provided by the hypervisor as well as core virtual platform devices
  and system APIs such as ARM PSCI.

- Device Virtualization:

  Para-virtualization of devices is supported using inter-VM communication and
  virtio transport support. Select stage 2 faults by virtual machines that use
  proxy-scheduled vCPUs can be handled directly by Linux to provide Type-2
  hypervisor style on-demand paging and/or device emulation.

Architectures supported
=======================
AArch64 with a GICv3 or GICv4.1

Resources and Capabilities
==========================

Services/resources provided by the Gunyah hypervisor are accessible to a
virtual machine through capabilities. A capability is an access control
token granting the holder a set of permissions to operate on a specific
hypervisor object (conceptually similar to a file-descriptor).
For example, inter-VM communication using Gunyah doorbells and message queues
is performed using hypercalls taking Capability ID arguments for the required
IPC objects. These resources are described in Linux as a struct gunyah_resource.

Unlike UNIX file descriptors, there is no path-based or similar lookup of
an object to create a new Capability, meaning simpler security analysis.
Creation of a new Capability requires the holding of a set of privileged
Capabilities which are typically never given out by the Resource Manager (RM).

Gunyah itself provides no APIs for Capability ID discovery. Enumeration of
Capability IDs is provided by RM as a higher level service to VMs.

Resource Manager
================

The Gunyah Resource Manager (RM) is a privileged application VM supporting the
Gunyah Hypervisor. It provides policy enforcement aspects of the virtualization
system. The resource manager can be treated as an extension of the Hypervisor
but is separated to its own partition to ensure that the hypervisor layer itself
remains small and secure and to maintain a separation of policy and mechanism in
the platform. The resource manager runs at arm64 NS-EL1, similar to other
virtual machines.

Communication with the resource manager from other virtual machines happens as
described in message-queue.rst. Details about the specific messages can be found
in drivers/virt/gunyah/rsc_mgr.c

::

  +-------+   +--------+   +--------+
  |  RM   |   |  VM_A  |   |  VM_B  |
  +-.-.-.-+   +---.----+   +---.----+
    | |           |            |
  +-.-.-----------.------------.----+
  | | \==========/             |    |
  |  \========================/     |
  |            Gunyah               |
  +---------------------------------+

The source for the resource manager is available at
https://github.com/quic/gunyah-resource-manager.

The resource manager provides the following features:

- VM lifecycle management: allocating a VM, starting VMs, destruction of VMs
- VM access control policy, including memory sharing and lending
- Interrupt routing configuration
- Forwarding of system-level events (e.g. VM shutdown) to owner VM
- Resource (capability) discovery

A VM requires boot configuration to establish communication with the resource
manager. This is provided to VMs via a 'hypervisor' device tree node which is
overlaid to the VMs DT by the RM. This node lets guests know they are running
as a Gunyah guest VM, how to communicate with resource manager, and basic
description and capabilities of this VM. See
Documentation/devicetree/bindings/firmware/gunyah-hypervisor.yaml for a
description of this node.
