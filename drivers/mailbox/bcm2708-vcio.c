/*
 *  Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This device provides a shared mechanism for writing to the mailboxes,
 * semaphores, doorbells etc. that are shared between the ARM and the
 * VideoCore processor
 */

#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_data/mailbox-bcm2708.h>
#include <linux/uaccess.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define DRIVER_NAME "bcm2708_vcio"

extern int bcm_mailbox_write(unsigned chan, uint32_t data28)
{
	struct rpi_firmware *fw = rpi_firmware_get(NULL);

	if (!fw)
		return -ENODEV;

	return rpi_firmware_transaction(fw, chan, data28);
}
EXPORT_SYMBOL_GPL(bcm_mailbox_write);

extern int bcm_mailbox_read(unsigned chan, uint32_t *data28)
{
	struct rpi_firmware *fw = rpi_firmware_get(NULL);

	if (!fw)
		return -ENODEV;

	*data28 = rpi_firmware_transaction_received(fw);

	return 0;
}
EXPORT_SYMBOL_GPL(bcm_mailbox_read);

static DEFINE_MUTEX(mailbox_lock);
extern int bcm_mailbox_property(void *data, int size)
{
	uint32_t success;
	dma_addr_t mem_bus; /* the memory address accessed from videocore */
	void *mem_kern;     /* the memory address accessed from driver */
	int s = 0;

	mutex_lock(&mailbox_lock);
	/* allocate some memory for the messages communicating with GPU */
	mem_kern = dma_alloc_coherent(NULL, PAGE_ALIGN(size), &mem_bus,
				      GFP_KERNEL);
	if (mem_kern) {
		/* create the message */
		memcpy(mem_kern, data, size);

		/* send the message */
		wmb();
		s = bcm_mailbox_write(MBOX_CHAN_PROPERTY, (uint32_t)mem_bus);
		if (s == 0)
			s = bcm_mailbox_read(MBOX_CHAN_PROPERTY, &success);
		if (s == 0) {
			/* copy the response */
			rmb();
			memcpy(data, mem_kern, size);
		}
		dma_free_coherent(NULL, PAGE_ALIGN(size), mem_kern, mem_bus);
	} else {
		s = -ENOMEM;
	}
	if (s != 0)
		pr_err(DRIVER_NAME ": %s failed (%d)\n", __func__, s);

	mutex_unlock(&mailbox_lock);
	return s;
}
EXPORT_SYMBOL_GPL(bcm_mailbox_property);

MODULE_AUTHOR("Gray Girling");
MODULE_DESCRIPTION("ARM I/O to VideoCore processor");
MODULE_LICENSE("GPL");
