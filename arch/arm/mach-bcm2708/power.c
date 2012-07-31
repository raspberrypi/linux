/*
 *	 linux/arch/arm/mach-bcm2708/power.c
 *
 *	 Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This device provides a shared mechanism for controlling the power to
 * VideoCore subsystems.
 */

#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/bug.h>
#include <mach/power.h>
#include <mach/vcio.h>
#include <mach/arm_power.h>

#define DRIVER_NAME "bcm2708_power"

#define BCM_POWER_MAXCLIENTS 4
#define BCM_POWER_NOCLIENT (1<<31)

/* Some drivers expect there devices to be permanently powered */
#define BCM_POWER_ALWAYS_ON (BCM_POWER_USB)

#if 1
#define DPRINTK printk
#else
#define DPRINTK if (0) printk
#endif

struct state_struct {
	uint32_t global_request;
	uint32_t client_request[BCM_POWER_MAXCLIENTS];
	struct semaphore client_mutex;
	struct semaphore mutex;
} g_state;

int bcm_power_open(BCM_POWER_HANDLE_T *handle)
{
	BCM_POWER_HANDLE_T i;
	int ret = -EBUSY;

	down(&g_state.client_mutex);

	for (i = 0; i < BCM_POWER_MAXCLIENTS; i++) {
		if (g_state.client_request[i] == BCM_POWER_NOCLIENT) {
			g_state.client_request[i] = BCM_POWER_NONE;
			*handle = i;
			ret = 0;
			break;
		}
	}

	up(&g_state.client_mutex);

	DPRINTK("bcm_power_open() -> %d\n", *handle);

	return ret;
}
EXPORT_SYMBOL_GPL(bcm_power_open);

int bcm_power_request(BCM_POWER_HANDLE_T handle, uint32_t request)
{
	int rc = 0;

	DPRINTK("bcm_power_request(%d, %x)\n", handle, request);

	if ((handle < BCM_POWER_MAXCLIENTS) &&
	    (g_state.client_request[handle] != BCM_POWER_NOCLIENT)) {
		if (down_interruptible(&g_state.mutex) != 0) {
			DPRINTK("bcm_power_request -> interrupted\n");
			return -EINTR;
		}

		if (request != g_state.client_request[handle]) {
			uint32_t others_request = 0;
			uint32_t global_request;
			BCM_POWER_HANDLE_T i;

			for (i = 0; i < BCM_POWER_MAXCLIENTS; i++) {
				if (i != handle)
					others_request |=
					    g_state.client_request[i];
			}
			others_request &= ~BCM_POWER_NOCLIENT;

			global_request = request | others_request;
			if (global_request != g_state.global_request) {
				uint32_t actual;

				/* Send a request to VideoCore */
				bcm_mailbox_write(MBOX_CHAN_POWER,
						  global_request << 4);

				/* Wait for a response during power-up */
				if (global_request & ~g_state.global_request) {
					rc = bcm_mailbox_read(MBOX_CHAN_POWER,
							      &actual);
					DPRINTK
					    ("bcm_mailbox_read -> %08x, %d\n",
					     actual, rc);
					actual >>= 4;
				} else {
					rc = 0;
					actual = global_request;
				}

				if (rc == 0) {
					if (actual != global_request) {
						printk(KERN_ERR
						     "%s: prev global %x, new global %x, actual %x, request %x, others_request %x\n",
						     __func__,
						     g_state.global_request,
						     global_request, actual, request, others_request);
						/* A failure */
						BUG_ON((others_request & actual)
						       != others_request);
						request &= actual;
						rc = -EIO;
					}

					g_state.global_request = actual;
					g_state.client_request[handle] =
					    request;
				}
			}
		}
		up(&g_state.mutex);
	} else {
		rc = -EINVAL;
	}
	DPRINTK("bcm_power_request -> %d\n", rc);
	return rc;
}
EXPORT_SYMBOL_GPL(bcm_power_request);

int bcm_power_close(BCM_POWER_HANDLE_T handle)
{
	int rc;

	DPRINTK("bcm_power_close(%d)\n", handle);

	rc = bcm_power_request(handle, BCM_POWER_NONE);
	if (rc == 0)
		g_state.client_request[handle] = BCM_POWER_NOCLIENT;

	return rc;
}
EXPORT_SYMBOL_GPL(bcm_power_close);

static int __init bcm_power_init(void)
{
#if defined(BCM_POWER_ALWAYS_ON)
	BCM_POWER_HANDLE_T always_on_handle;
#endif
	int rc = 0;
	int i;

	printk(KERN_INFO "bcm_power: Broadcom power driver\n");
	bcm_mailbox_write(MBOX_CHAN_POWER, 0);

	for (i = 0; i < BCM_POWER_MAXCLIENTS; i++)
		g_state.client_request[i] = BCM_POWER_NOCLIENT;

	sema_init(&g_state.client_mutex, 1);
	sema_init(&g_state.mutex, 1);

	g_state.global_request = 0;

#if defined(BCM_POWER_ALWAYS_ON)
	if (BCM_POWER_ALWAYS_ON) {
		bcm_power_open(&always_on_handle);
		bcm_power_request(always_on_handle, BCM_POWER_ALWAYS_ON);
	}
#endif

	return rc;
}

static void __exit bcm_power_exit(void)
{
	bcm_mailbox_write(MBOX_CHAN_POWER, 0);
}

arch_initcall(bcm_power_init);	/* Initialize early */
module_exit(bcm_power_exit);

MODULE_AUTHOR("Phil Elwell");
MODULE_DESCRIPTION("Interface to BCM2708 power management");
MODULE_LICENSE("GPL");
