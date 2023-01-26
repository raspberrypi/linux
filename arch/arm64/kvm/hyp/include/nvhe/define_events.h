/* SPDX-License-Identifier: GPL-2.0 */

#ifndef HYP_EVENT_FILE
# define __HYP_EVENT_FILE <asm/kvm_hypevents.h>
#else
# define __HYP_EVENT_FILE __stringify(HYP_EVENT_FILE)
#endif

#undef HYP_EVENT
#define HYP_EVENT(__name, __proto, __struct, __assign, __printk)	\
	atomic_t __ro_after_init __name##_enabled = ATOMIC_INIT(0);	\
	struct hyp_event_id hyp_event_id_##__name			\
	__section(".hyp.event_ids") = {					\
		.data = (void *)&__name##_enabled,			\
	}

#define HYP_EVENT_MULTI_READ
#include __HYP_EVENT_FILE
#undef HYP_EVENT_MULTI_READ

#undef HYP_EVENT
