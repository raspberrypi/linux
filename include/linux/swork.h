#ifndef _LINUX_SWORK_H
#define _LINUX_SWORK_H

#include <linux/list.h>

struct swork_event {
	struct list_head item;
	unsigned long flags;
	void (*func)(struct swork_event *);
};

static inline void INIT_SWORK(struct swork_event *event,
			      void (*func)(struct swork_event *))
{
	event->flags = 0;
	event->func = func;
}

bool swork_queue(struct swork_event *sev);

int swork_get(void);
void swork_put(void);

#endif /* _LINUX_SWORK_H */
