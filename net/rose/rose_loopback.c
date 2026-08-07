// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 * Copyright (C) Jonathan Naylor G4KLX (g4klx@g4klx.demon.co.uk)
 */
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/timer.h>
#include <net/ax25.h>
#include <linux/skbuff.h>
#include <net/rose.h>
#include <linux/init.h>

#define ROSE_LOOPBACK_LIMIT 1000

static struct timer_list loopback_timer;
static struct sk_buff_head loopback_queue;
static void rose_set_loopback_timer(void);
static void rose_loopback_timer(struct timer_list *unused);

static atomic_t loopback_stopping = ATOMIC_INIT(0);

void rose_loopback_init(void)
{
	skb_queue_head_init(&loopback_queue);

	timer_setup(&loopback_timer, rose_loopback_timer, 0);
}

static int rose_loopback_running(void)
{
	return timer_pending(&loopback_timer);
}

int rose_loopback_queue(struct sk_buff *skb, struct rose_neigh *neigh)
{
	struct sk_buff *skbn = NULL;

	if (skb_queue_len(&loopback_queue) < ROSE_LOOPBACK_LIMIT)
		skbn = skb_clone(skb, GFP_ATOMIC);

	if (skbn) {
		consume_skb(skb);
		skb_queue_tail(&loopback_queue, skbn);

		if (!rose_loopback_running())
			rose_set_loopback_timer();
	} else {
		kfree_skb(skb);
	}

	return 1;
}

static void rose_set_loopback_timer(void)
{
	mod_timer(&loopback_timer, jiffies + 10);
}

static void rose_loopback_timer(struct timer_list *unused)
{
	struct sk_buff *skb;
	struct net_device *dev;
	rose_address *dest;
	struct sock *sk;
	unsigned short frametype;
	unsigned int lci_i, lci_o;
	int count;

	if (atomic_read(&loopback_stopping))
		return;

	if (rose_loopback_neigh)
		rose_neigh_hold(rose_loopback_neigh);
	else
		return;

	for (count = 0; count < ROSE_LOOPBACK_LIMIT; count++) {
		skb = skb_dequeue(&loopback_queue);
		if (!skb)
			goto out;

		if (atomic_read(&loopback_stopping)) {
			kfree_skb(skb);
			skb_queue_purge(&loopback_queue);
			goto out;
		}

		if (skb->len < ROSE_MIN_LEN) {
			kfree_skb(skb);
			continue;
		}
		lci_i     = ((skb->data[0] << 8) & 0xF00) + ((skb->data[1] << 0) & 0x0FF);
		frametype = skb->data[2];
		if (frametype == ROSE_CALL_REQUEST &&
		    (skb->len <= ROSE_CALL_REQ_FACILITIES_OFF ||
		     skb->data[ROSE_CALL_REQ_ADDR_LEN_OFF] !=
		     ROSE_CALL_REQ_ADDR_LEN_VAL)) {
			kfree_skb(skb);
			continue;
		}
		dest      = (rose_address *)(skb->data + ROSE_CALL_REQ_DEST_ADDR_OFF);
		lci_o     = ROSE_DEFAULT_MAXVC + 1 - lci_i;

		skb_reset_transport_header(skb);

		sk = rose_find_socket(lci_o, rose_loopback_neigh);
		if (sk) {
			if (rose_process_rx_frame(sk, skb) == 0)
				kfree_skb(skb);
			continue;
		}

		if (frametype == ROSE_CALL_REQUEST) {
			dev = rose_dev_get(dest);
			if (!dev) {
				kfree_skb(skb);
				continue;
			}
			/* rose_kill_by_device() runs on NETDEV_DOWN (IFF_UP cleared)
			 * before the device is unregistered.  If we create a new
			 * socket here after that cleanup, the ref never gets released
			 * because NETDEV_DOWN fires only once.  Drop the call instead.
			 */
			if (!netif_running(dev)) {
				dev_put(dev);
				kfree_skb(skb);
				continue;
			}

			if (rose_rx_call_request(skb, dev, rose_loopback_neigh, lci_o) == 0)
				kfree_skb(skb);
			dev_put(dev);
		} else {
			kfree_skb(skb);
		}
	}

out:
	rose_neigh_put(rose_loopback_neigh);

	if (!atomic_read(&loopback_stopping) && !skb_queue_empty(&loopback_queue))
		mod_timer(&loopback_timer, jiffies + 1);
}

void __exit rose_loopback_clear(void)
{
	struct sk_buff *skb;

	atomic_set(&loopback_stopping, 1);
	/* Pairs with atomic_read() in rose_loopback_timer(): ensure the
	 * stopping flag is visible before we cancel, so a concurrent
	 * callback aborts its loop early rather than re-arming the timer.
	 */
	smp_mb();

	timer_delete_sync(&loopback_timer);

	while ((skb = skb_dequeue(&loopback_queue)) != NULL)
		kfree_skb(skb);
}
