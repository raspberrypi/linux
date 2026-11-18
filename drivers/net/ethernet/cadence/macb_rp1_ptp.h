/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MACB_RP1_PTP_H
#define MACB_RP1_PTP_H

#include <linux/errno.h>
#include <linux/ptp_clock_kernel.h>

struct macb;
struct ptp_clock_request;

#if IS_ENABLED(CONFIG_MACB_RP1_PPS)
int rp1_extts_create(struct macb *bp);
int rp1_extts_quiesce(struct macb *bp);
int rp1_extts_destroy(struct macb *bp);
int rp1_extts_enable(struct macb *bp, struct ptp_clock_request *rq, int on);
int rp1_extts_verify(struct macb *bp, unsigned int pin,
		     enum ptp_pin_function func, unsigned int chan);
int rp1_extts_clock_lock(struct macb *bp);
int rp1_extts_clock_unlock(struct macb *bp);
void rp1_extts_rate_lock(struct macb *bp);
void rp1_extts_rate_unlock(struct macb *bp);
#else
static inline int rp1_extts_create(struct macb *bp)
{
	return 0;
}

static inline int rp1_extts_quiesce(struct macb *bp)
{
	return 0;
}

static inline int rp1_extts_destroy(struct macb *bp)
{
	return 0;
}

static inline int rp1_extts_enable(struct macb *bp,
				   struct ptp_clock_request *rq, int on)
{
	return -EOPNOTSUPP;
}

static inline int rp1_extts_verify(struct macb *bp, unsigned int pin,
				   enum ptp_pin_function func, unsigned int chan)
{
	return -EOPNOTSUPP;
}

static inline int rp1_extts_clock_lock(struct macb *bp)
{
	return 0;
}

static inline int rp1_extts_clock_unlock(struct macb *bp)
{
	return 0;
}

static inline void rp1_extts_rate_lock(struct macb *bp)
{
}

static inline void rp1_extts_rate_unlock(struct macb *bp)
{
}
#endif

#endif
