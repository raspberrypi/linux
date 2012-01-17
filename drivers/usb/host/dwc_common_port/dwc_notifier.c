#include "dwc_notifier.h"
#include "dwc_list.h"

typedef struct dwc_observer
{
	void *observer;
	dwc_notifier_callback_t callback;
	void *data;
	char *notification;
	DWC_CIRCLEQ_ENTRY(dwc_observer) list_entry;
} observer_t;

DWC_CIRCLEQ_HEAD(observer_queue, dwc_observer);

typedef struct dwc_notifier
{
	void *object;
	struct observer_queue observers;
	DWC_CIRCLEQ_ENTRY(dwc_notifier) list_entry;
} notifier_t;

DWC_CIRCLEQ_HEAD(notifier_queue, dwc_notifier);

typedef struct manager
{
	dwc_workq_t *wq;
	dwc_mutex_t *mutex;
	struct notifier_queue notifiers;
} manager_t;

static manager_t *manager = NULL;

static void create_manager(void)
{
	manager = DWC_ALLOC(sizeof(manager_t));
	DWC_CIRCLEQ_INIT(&manager->notifiers);
	manager->wq = DWC_WORKQ_ALLOC("DWC Notification WorkQ");
}

static void free_manager(void)
{
	DWC_WORKQ_FREE(manager->wq);
	/* All notifiers must have unregistered themselves before this module
	 * can be removed.  Hitting this assertion indicates a programmer
	 * error. */
	DWC_ASSERT(DWC_CIRCLEQ_EMPTY(&manager->notifiers), "Notification manager being freed before all notifiers have been removed");
	DWC_FREE(manager);
}

#ifdef DEBUG
static void dump_manager(void)
{
	notifier_t *n;
	observer_t *o;
	DWC_ASSERT(manager, "Notification manager not found");
	DWC_DEBUG("List of all notifiers and observers:");
	DWC_CIRCLEQ_FOREACH(n, &manager->notifiers, list_entry) {
		DWC_DEBUG("Notifier %p has observers:", n->object);
		DWC_CIRCLEQ_FOREACH(o, &n->observers, list_entry) {
			DWC_DEBUG("    %p watching %s", o->observer, o->notification);
		}
	}
}
#else
#define dump_manager(...)
#endif

static observer_t *alloc_observer(void *observer, char *notification, dwc_notifier_callback_t callback, void *data)
{
	observer_t *new_observer = DWC_ALLOC(sizeof(observer_t));
	DWC_CIRCLEQ_INIT_ENTRY(new_observer, list_entry);
	new_observer->observer = observer;
	new_observer->notification = notification;
	new_observer->callback = callback;
	new_observer->data = data;
	return new_observer;
}

static void free_observer(observer_t *observer)
{
	DWC_FREE(observer);
}

static notifier_t *alloc_notifier(void *object)
{
	notifier_t *notifier;

	if (!object) {
		return NULL;
	}

	notifier = DWC_ALLOC(sizeof(notifier_t));
	DWC_CIRCLEQ_INIT(&notifier->observers);
	DWC_CIRCLEQ_INIT_ENTRY(notifier, list_entry);

	notifier->object = object;
	return notifier;
}

static void free_notifier(notifier_t *notifier)
{
	observer_t *observer;
	DWC_CIRCLEQ_FOREACH(observer, &notifier->observers, list_entry) {
		free_observer(observer);
	}
	DWC_FREE(notifier);
}

static notifier_t *find_notifier(void *object)
{
	notifier_t *notifier;
	if (!object) {
		return NULL;
	}
	DWC_ASSERT(manager, "Notification manager not found");
	DWC_CIRCLEQ_FOREACH(notifier, &manager->notifiers, list_entry) {
		if (notifier->object == object) {
			return notifier;
		}
	}
	return NULL;
}

void dwc_alloc_notification_manager(void)
{
	create_manager();
}

void dwc_free_notification_manager(void)
{
	free_manager();
}

dwc_notifier_t *dwc_register_notifier(void *object)
{
	notifier_t *notifier = find_notifier(object);
	DWC_ASSERT(manager, "Notification manager not found");
	if (notifier) {
		DWC_ERROR("Notifier %p is already registered", object);
		return NULL;
	}

	notifier = alloc_notifier(object);
	DWC_CIRCLEQ_INSERT_TAIL(&manager->notifiers, notifier, list_entry);


	DWC_INFO("Notifier %p registered", object);
	dump_manager();

	return notifier;
}

void dwc_unregister_notifier(dwc_notifier_t *notifier)
{
	DWC_ASSERT(manager, "Notification manager not found");
	if (!DWC_CIRCLEQ_EMPTY(&notifier->observers)) {
		observer_t *o;
		DWC_ERROR("Notifier %p has active observers when removing", notifier->object);
		DWC_CIRCLEQ_FOREACH(o, &notifier->observers, list_entry) {
			DWC_DEBUG("    %p watching %s", o->observer, o->notification);
		}
		DWC_ASSERT(DWC_CIRCLEQ_EMPTY(&notifier->observers), "Notifier %p has active observers when removing", notifier);
	}

	DWC_CIRCLEQ_REMOVE_INIT(&manager->notifiers, notifier, list_entry);
	free_notifier(notifier);

	DWC_INFO("Notifier unregistered");
	dump_manager();
}

/* Add an observer to observe the notifier for a particular state, event, or notification. */
int dwc_add_observer(void *observer, void *object, char *notification, dwc_notifier_callback_t callback, void *data)
{
	notifier_t *notifier = find_notifier(object);
	observer_t *new_observer;
	if (!notifier) {
		DWC_ERROR("Notifier %p is not found when adding observer", object);
		return -1;
	}

	new_observer = alloc_observer(observer, notification, callback, data);

	DWC_CIRCLEQ_INSERT_TAIL(&notifier->observers, new_observer, list_entry);

	DWC_INFO("Added observer %p to notifier %p observing notification %s, callback=%p, data=%p",
		 observer, object, notification, callback, data);

	dump_manager();
	return 0;
}

int dwc_remove_observer(void *observer)
{
	notifier_t *n;
	DWC_ASSERT(manager, "Notification manager not found");
	DWC_CIRCLEQ_FOREACH(n, &manager->notifiers, list_entry) {
		observer_t *o;
		observer_t *o2;
		DWC_CIRCLEQ_FOREACH_SAFE(o, o2, &n->observers, list_entry) {
			if (o->observer == observer) {
				DWC_CIRCLEQ_REMOVE_INIT(&n->observers, o, list_entry);
				DWC_INFO("Removing observer %p from notifier %p watching notification %s:",
					 o->observer, n->object, o->notification);
				free_observer(o);
			}
		}
	}

	dump_manager();
	return 0;
}

typedef struct callback_data
{
	dwc_notifier_callback_t cb;
	void *observer;
	void *data;
	void *object;
	void *notification;
	void *notification_data;
} cb_data_t;

static void cb_task(void *data)
{
	cb_data_t *cb = (cb_data_t *)data;
	cb->cb(cb->object, cb->notification, cb->observer, cb->notification_data, cb->data);
	DWC_FREE(cb);
}

void dwc_notify(dwc_notifier_t *notifier, char *notification, void *notification_data)
{
	observer_t *o;
	DWC_ASSERT(manager, "Notification manager not found");
	DWC_CIRCLEQ_FOREACH(o, &notifier->observers, list_entry) {
		int len = DWC_STRLEN(notification);
		if (DWC_STRLEN(o->notification) != len) {
			continue;
		}

		if (DWC_STRNCMP(o->notification, notification, len) == 0) {
			cb_data_t *cb_data = DWC_ALLOC(sizeof(cb_data_t));
			cb_data->cb = o->callback;
			cb_data->observer = o->observer;
			cb_data->data = o->data;
			cb_data->object = notifier->object;
			cb_data->notification = notification;
			cb_data->notification_data = notification_data;
			DWC_DEBUG("Observer found %p for notification %s", o->observer, notification);
			DWC_WORKQ_SCHEDULE(manager->wq, cb_task, cb_data,
					   "Notify callback from %p for Notification %s, to observer %p",
					   cb_data->object, notification, cb_data->observer);
		}
	}
}

