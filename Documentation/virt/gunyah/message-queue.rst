.. SPDX-License-Identifier: GPL-2.0

Message Queues
==============
Message queue is a simple low-capacity IPC channel between two virtual machines.
It is intended for sending small control and configuration messages. Each
message queue is unidirectional and buffered in the hypervisor. A full-duplex
IPC channel requires a pair of queues.

The size of the queue and the maximum size of the message that can be passed is
fixed at creation of the message queue. Resource manager is presently the only
use case for message queues, and creates messages queues between itself and VMs
with a fixed maximum message size of 240 bytes. Longer messages require a
further protocol on top of the message queue messages themselves. For instance,
communication with the resource manager adds a header field for sending longer
messages which are split into smaller fragments.

The diagram below shows how message queue works. A typical configuration
involves 2 message queues. Message queue 1 allows VM_A to send messages to VM_B.
Message queue 2 allows VM_B to send messages to VM_A.

1. VM_A sends a message of up to 240 bytes in length. It makes a hypercall
   with the message to request the hypervisor to add the message to
   message queue 1's queue. The hypervisor copies memory into the internal
   message queue buffer; the memory doesn't need to be shared between
   VM_A and VM_B.

2. Gunyah raises the corresponding interrupt for VM_B (Rx vIRQ) when any of
   these happens:

   a. gunyah_msgq_send() has PUSH flag. This is a typical case when the message
      queue is being used to implement an RPC-like interface.
   b. Explicitly with gunyah_msgq_push hypercall from VM_A.
   c. Message queue has reached a threshold depth. Typically, this threshold
      depth is the size of the queue (in other words: when queue is full, Rx
      vIRQ is raised).

3. VM_B calls gunyah_msgq_recv() and Gunyah copies message to requested buffer.

4. Gunyah raises the corresponding interrupt for VM_A (Tx vIRQ) when the message
   queue falls below a watermark depth. Typically, this is when the queue is
   drained. Note the watermark depth and the threshold depth for the Rx vIRQ are
   independent values. Coincidentally, this signal is conceptually similar to
   Clear-to-Send.

For VM_B to send a message to VM_A, the process is identical, except that
hypercalls reference message queue 2's capability ID. The IRQ will be different
for the second message queue.

::

      +-------------------+         +-----------------+         +-------------------+
      |        VM_A       |         |Gunyah hypervisor|         |        VM_B       |
      |                   |         |                 |         |                   |
      |                   |         |                 |         |                   |
      |                   |   Tx    |                 |         |                   |
      |                   |-------->|                 | Rx vIRQ |                   |
      |gunyah_msgq_send() | Tx vIRQ |Message queue 1  |-------->|gunyah_msgq_recv() |
      |                   |<------- |                 |         |                   |
      |                   |         |                 |         |                   |
      |                   |         |                 |         |                   |
      |                   |         |                 |   Tx    |                   |
      |                   | Rx vIRQ |                 |<--------|                   |
      |gunyah_msgq_recv() |<--------|Message queue 2  | Tx vIRQ |gunyah_msgq_send() |
      |                   |         |                 |-------->|                   |
      |                   |         |                 |         |                   |
      |                   |         |                 |         |                   |
      +-------------------+         +-----------------+         +---------------+
