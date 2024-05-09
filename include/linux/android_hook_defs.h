/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Include this file from a header that uses vendor hooks.
 * Typical usage:
 * In the header file:
 *
 *    #include <linux/android_hook_defs.h>
 *
 *    DECLARE_INDIRECT_HOOK(android_vh_del_folio_from_lrulist,
 *        TP_PROTO(struct folio *folio, enum lru_list lru),
 *        TP_ARGS(folio, lru));
 *
 *   static __always_inline
 *   void lruvec_del_folio(struct lruvec *lruvec, struct folio *folio)
 *   {
 *        ...
 *        _trace_android_vh_del_folio_from_lrulist(folio, lru);
 *   }
 *
 * In the source file:
 *
 *    DEFINE_INDIRECT_HOOK(android_vh_del_folio_from_lrulist,
 *        TP_PROTO(struct folio *folio, enum lru_list lru),
 *        TP_ARGS(folio, lru));
 */

#ifndef _LINUX_ANDROID_HOOK_DEFS_H
#define _LINUX_ANDROID_HOOK_DEFS_H

/* Users of these macros need TP_PROTO() and TP_ARGS() */
#include <linux/tracepoint.h>

#define DECLARE_INDIRECT_HOOK(name, proto, args)			\
	extern struct tracepoint __tracepoint_##name;			\
	void __trace_##name(proto);					\
	static __always_inline void _trace_##name(proto)		\
	{								\
		if (static_key_false(&__tracepoint_##name.key))		\
			__trace_##name(args);				\
	}

#define DEFINE_INDIRECT_HOOK(name, proto, args)				\
	void __trace_##name(proto)					\
	{								\
		trace_##name(args);					\
	}								\
	EXPORT_SYMBOL_GPL(__trace_##name);

#endif	/* _LINUX_ANDROID_HOOK_DEFS_H */
