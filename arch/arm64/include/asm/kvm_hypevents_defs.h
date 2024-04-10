/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ARM64_KVM_HYPEVENTS_DEFS_H
#define __ARM64_KVM_HYPEVENTS_DEFS_H

struct hyp_event_id {
	unsigned short id;
	void *data;
};

#define HYP_EVENT_NAME_MAX 32

struct hyp_event {
	char				name[HYP_EVENT_NAME_MAX];
	bool				*enabled;
	char				*print_fmt;
	struct trace_event_fields	*fields;
	void (*trace_func)(struct ht_iterator *iter);
	int				id;
};

struct hyp_entry_hdr {
	unsigned short id;
};

struct hyp_printk_fmt {
	/* __MUST__ be the first element */
	const char	fmt[127];
	const char	null;
};

/*
 * Hyp events definitions common to the hyp and the host
 */
#define HYP_EVENT_FORMAT(__name, __struct)		\
	struct __packed trace_hyp_format_##__name {	\
		struct hyp_entry_hdr hdr;		\
		__struct				\
	}

#define HE_PROTO(args...)		args
#define HE_STRUCT(args...)		args
#define HE_ASSIGN(args...)		args
#define HE_PRINTK(args...)		args
#define HE_PRINTK_UNKNOWN_FMT(args...)	args

#define he_field(type, item)	type item;
#endif
