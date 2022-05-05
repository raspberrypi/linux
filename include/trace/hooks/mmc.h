/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM mmc
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_MMC_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_MMC_H
#include <trace/hooks/vendor_hooks.h>
struct mmc_card;
struct mmc_queue;
struct blk_mq_queue_data;
struct mmc_host;

/*
 * Following tracepoints are not exported in tracefs and provide a
 * mechanism for vendor modules to hook and extend functionality
 */
DECLARE_HOOK(android_vh_mmc_update_mmc_queue,
	TP_PROTO(struct mmc_card *card, struct mmc_queue *mq),
	TP_ARGS(card, mq));

DECLARE_HOOK(android_vh_mmc_sdio_pm_flag_set,
	TP_PROTO(struct mmc_host *host),
	TP_ARGS(host));

#endif /* _TRACE_HOOK_MMC_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
