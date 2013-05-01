#include "dwc_cc.h"
#include "dwc_notifier.h"

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kthread.h>

MODULE_DESCRIPTION("DWC Common Library - Portable version");
MODULE_AUTHOR("Synopsys Inc.");
MODULE_LICENSE ("GPL");

static int dwc_common_port_init_module(void)
{
	printk( KERN_DEBUG "Module dwc_common_port init\n" );
#ifdef DEBUG_MEMORY
	dwc_memory_debug_start();
#endif
	dwc_alloc_notification_manager();
	return 0;
}

static void dwc_common_port_exit_module(void)
{
	printk( KERN_DEBUG "Module dwc_common_port exit\n" );
	dwc_free_notification_manager();
#ifdef DEBUG_MEMORY
	dwc_memory_debug_stop();
#endif
}

module_init(dwc_common_port_init_module);
module_exit(dwc_common_port_exit_module);

/* CC */
EXPORT_SYMBOL(dwc_cc_if_alloc);
EXPORT_SYMBOL(dwc_cc_if_free);
EXPORT_SYMBOL(dwc_cc_clear);
EXPORT_SYMBOL(dwc_cc_add);
EXPORT_SYMBOL(dwc_cc_remove);
EXPORT_SYMBOL(dwc_cc_change);
EXPORT_SYMBOL(dwc_cc_data_for_save);
EXPORT_SYMBOL(dwc_cc_restore_from_data);
EXPORT_SYMBOL(dwc_cc_match_chid);
EXPORT_SYMBOL(dwc_cc_match_cdid);
EXPORT_SYMBOL(dwc_cc_ck);
EXPORT_SYMBOL(dwc_cc_chid);
EXPORT_SYMBOL(dwc_cc_cdid);
EXPORT_SYMBOL(dwc_cc_name);

/* Notification */
EXPORT_SYMBOL(dwc_alloc_notification_manager);
EXPORT_SYMBOL(dwc_free_notification_manager);
EXPORT_SYMBOL(dwc_register_notifier);
EXPORT_SYMBOL(dwc_unregister_notifier);
EXPORT_SYMBOL(dwc_add_observer);
EXPORT_SYMBOL(dwc_remove_observer);
EXPORT_SYMBOL(dwc_notify);

/* Memory Debugging Routines */
#ifdef DEBUG_MEMORY
EXPORT_SYMBOL(dwc_alloc_debug);
EXPORT_SYMBOL(dwc_alloc_atomic_debug);
EXPORT_SYMBOL(dwc_free_debug);
EXPORT_SYMBOL(dwc_dma_alloc_debug);
EXPORT_SYMBOL(dwc_dma_alloc_atomic_debug);
EXPORT_SYMBOL(dwc_dma_free_debug);
#endif

/* OS-Level Implementations */

/* This is the Linux kernel implementation of the DWC platform library. */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/usb.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30))
#include <linux/usb_gadget.h>
#else
#include <linux/usb/gadget.h>
#endif
#include <linux/random.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/uaccess.h>
#include <asm/unaligned.h>
#include <asm/page.h>
#include <linux/scatterlist.h>

/* MISC */

void *DWC_MEMSET(void *dest, uint8_t byte, uint32_t size)
{
	return memset(dest, byte, size);
}
EXPORT_SYMBOL(DWC_MEMSET);

void *DWC_MEMCPY(void *dest, void const *src, uint32_t size)
{
	return memcpy(dest, src, size);
}
EXPORT_SYMBOL(DWC_MEMCPY);

void *DWC_MEMMOVE(void *dest, void *src, uint32_t size)
{
	return memmove(dest, src, size);
}
EXPORT_SYMBOL(DWC_MEMMOVE);

int DWC_MEMCMP(void *m1, void *m2, uint32_t size)
{
	return memcmp(m1, m2, size);
}
EXPORT_SYMBOL(DWC_MEMCMP);

int DWC_STRNCMP(void *s1, void *s2, uint32_t size)
{
	return strncmp(s1, s2, size);
}
EXPORT_SYMBOL(DWC_STRNCMP);

int DWC_STRCMP(void *s1, void *s2)
{
	return strcmp(s1, s2);
}
EXPORT_SYMBOL(DWC_STRCMP);

int DWC_STRLEN(char const *str)
{
	return strlen(str);
}
EXPORT_SYMBOL(DWC_STRLEN);

char *DWC_STRCPY(char *to, const char *from)
{
	return strcpy(to, from);
}
EXPORT_SYMBOL(DWC_STRCPY);

char *DWC_STRDUP(char const *str)
{
	int len = DWC_STRLEN(str) + 1;
	char *new = DWC_ALLOC_ATOMIC(len);
	if (!new) {
		return NULL;
	}
	DWC_MEMCPY(new, str, len);
	return new;
}
EXPORT_SYMBOL(DWC_STRDUP);

int DWC_ATOI(char *str, int32_t *value)
{
	char *end = NULL;
	*value = simple_strtol(str, &end, 0);
	if (*end == '\0') {
		return 0;
	}
	return -1;
}
EXPORT_SYMBOL(DWC_ATOI);

int DWC_ATOUI(char *str, uint32_t *value)
{
	char *end = NULL;
	*value = simple_strtoul(str, &end, 0);
	if (*end == '\0') {
		return 0;
	}
	return -1;
}
EXPORT_SYMBOL(DWC_ATOUI);


/* From usbstring.c */
int DWC_UTF8_TO_UTF16LE(uint8_t const *s, uint16_t *cp, unsigned len)
{
	int	count = 0;
	u8	c;
	u16	uchar;

	/* this insists on correct encodings, though not minimal ones.
	 * BUT it currently rejects legit 4-byte UTF-8 code points,
	 * which need surrogate pairs.  (Unicode 3.1 can use them.)
	 */
	while (len != 0 && (c = (u8) *s++) != 0) {
		if (unlikely(c & 0x80)) {
			// 2-byte sequence:
			// 00000yyyyyxxxxxx = 110yyyyy 10xxxxxx
			if ((c & 0xe0) == 0xc0) {
				uchar = (c & 0x1f) << 6;

				c = (u8) *s++;
				if ((c & 0xc0) != 0xc0)
					goto fail;
				c &= 0x3f;
				uchar |= c;

			// 3-byte sequence (most CJKV characters):
			// zzzzyyyyyyxxxxxx = 1110zzzz 10yyyyyy 10xxxxxx
			} else if ((c & 0xf0) == 0xe0) {
				uchar = (c & 0x0f) << 12;

				c = (u8) *s++;
				if ((c & 0xc0) != 0xc0)
					goto fail;
				c &= 0x3f;
				uchar |= c << 6;

				c = (u8) *s++;
				if ((c & 0xc0) != 0xc0)
					goto fail;
				c &= 0x3f;
				uchar |= c;

				/* no bogus surrogates */
				if (0xd800 <= uchar && uchar <= 0xdfff)
					goto fail;

			// 4-byte sequence (surrogate pairs, currently rare):
			// 11101110wwwwzzzzyy + 110111yyyyxxxxxx
			//     = 11110uuu 10uuzzzz 10yyyyyy 10xxxxxx
			// (uuuuu = wwww + 1)
			// FIXME accept the surrogate code points (only)

			} else
				goto fail;
		} else
			uchar = c;
		put_unaligned (cpu_to_le16 (uchar), cp++);
		count++;
		len--;
	}
	return count;
fail:
	return -1;
}
EXPORT_SYMBOL(DWC_UTF8_TO_UTF16LE);

/* dwc_debug.h */

dwc_bool_t DWC_IN_IRQ(void)
{
	return in_irq();
}
EXPORT_SYMBOL(DWC_IN_IRQ);

int DWC_IN_BH(void)
{
	return in_softirq();
}
EXPORT_SYMBOL(DWC_IN_BH);

void DWC_VPRINTF(char *format, va_list args)
{
	vprintk(format, args);
}
EXPORT_SYMBOL(DWC_VPRINTF);

int DWC_VSNPRINTF(char *str, int size, char *format, va_list args)
{
	return vsnprintf(str, size, format, args);
}

void DWC_PRINTF(char *format, ...)
{
	va_list args;
	va_start(args, format);
	DWC_VPRINTF(format, args);
	va_end(args);
}
EXPORT_SYMBOL(DWC_PRINTF);

int DWC_SPRINTF(char *buffer, char *format, ...)
{
	int retval;
	va_list args;
	va_start(args, format);
	retval = vsprintf(buffer, format, args);
	va_end(args);
	return retval;
}
EXPORT_SYMBOL(DWC_SPRINTF);

int DWC_SNPRINTF(char *buffer, int size, char *format, ...)
{
	int retval;
	va_list args;
	va_start(args, format);
	retval = vsnprintf(buffer, size, format, args);
	va_end(args);
	return retval;
}
EXPORT_SYMBOL(DWC_SNPRINTF);

void __DWC_WARN(char *format, ...)
{
	va_list args;
	va_start(args, format);
	DWC_PRINTF(KERN_WARNING);
	DWC_VPRINTF(format, args);
	va_end(args);
}
EXPORT_SYMBOL(__DWC_WARN);

void __DWC_ERROR(char *format, ...)
{
	va_list args;
	va_start(args, format);
	DWC_PRINTF(KERN_ERR);
	DWC_VPRINTF(format, args);
	va_end(args);
}
EXPORT_SYMBOL(__DWC_ERROR);

void DWC_EXCEPTION(char *format, ...)
{
	va_list args;
	va_start(args, format);
	DWC_PRINTF(KERN_ERR);
	DWC_VPRINTF(format, args);
	va_end(args);
	BUG_ON(1);
}
EXPORT_SYMBOL(DWC_EXCEPTION);

#ifdef DEBUG
void __DWC_DEBUG(char *format, ...)
{
	va_list args;
	va_start(args, format);
	DWC_PRINTF(KERN_DEBUG);
	DWC_VPRINTF(format, args);
	va_end(args);
}
EXPORT_SYMBOL(__DWC_DEBUG);
#endif



/* dwc_mem.h */

#if 0
dwc_pool_t *DWC_DMA_POOL_CREATE(uint32_t size,
				uint32_t align,
				uint32_t alloc)
{
	struct dma_pool *pool = dma_pool_create("Pool", NULL,
						size, align, alloc);
	return (dwc_pool_t *)pool;
}

void DWC_DMA_POOL_DESTROY(dwc_pool_t *pool)
{
	dma_pool_destroy((struct dma_pool *)pool);
}

void *DWC_DMA_POOL_ALLOC(dwc_pool_t *pool, U64 *dma_addr)
{
	return dma_pool_alloc((struct dma_pool *)pool, GFP_KERNEL, dma_addr);
}

void *DWC_DMA_POOL_ZALLOC(dwc_pool_t *pool, U64 *dma_addr)
{
	void *vaddr = DWC_DMA_POOL_ALLOC(pool, dma_addr);
	memset();
}

void DWC_DMA_POOL_FREE(dwc_pool_t *pool, void *vaddr, void *daddr)
{
	dma_pool_free(pool, vaddr, daddr);
}

#endif

void *__DWC_DMA_ALLOC(uint32_t size, dwc_dma_t *dma_addr)
{
	void *buf = dma_alloc_coherent(NULL, (size_t)size, dma_addr, GFP_KERNEL);
	if (!buf) {
		return NULL;
	}
	memset(buf, 0, (size_t)size);
	return buf;
}
EXPORT_SYMBOL(__DWC_DMA_ALLOC);

void __DWC_DMA_FREE(uint32_t size, void *virt_addr, dwc_dma_t dma_addr)
{
	dma_free_coherent(NULL, size, virt_addr, dma_addr);
}
EXPORT_SYMBOL(__DWC_DMA_FREE);

void *__DWC_DMA_ALLOC_ATOMIC(uint32_t size, dwc_dma_t *dma_addr)
{
	void *buf = dma_alloc_coherent(NULL, (size_t)size, dma_addr, GFP_ATOMIC);
	if (!buf) {
		return NULL;
	}
	memset(buf, 0, (size_t)size);
	return buf;
}
EXPORT_SYMBOL(__DWC_DMA_ALLOC_ATOMIC);

void *__DWC_ALLOC(uint32_t size)
{
	return kzalloc(size, GFP_KERNEL);
}
EXPORT_SYMBOL(__DWC_ALLOC);

void *__DWC_ALLOC_ATOMIC(uint32_t size)
{
	return kzalloc(size, GFP_ATOMIC);
}
EXPORT_SYMBOL(__DWC_ALLOC_ATOMIC);

void __DWC_FREE(void *addr)
{
	kfree(addr);
}
EXPORT_SYMBOL(__DWC_FREE);

/* Byte Ordering Conversions. */
uint32_t DWC_CPU_TO_LE32(void *p)
{
#ifdef __LITTLE_ENDIAN
	return *((uint32_t *)p);
#else
	uint8_t *u_p = (uint8_t *)p;

	return (u_p[3] | (u_p[2] << 8) | (u_p[1] << 16) | (u_p[0] << 24));
#endif
}
EXPORT_SYMBOL(DWC_CPU_TO_LE32);

uint32_t DWC_CPU_TO_BE32(void *p)
{
#ifdef __BIG_ENDIAN
	return *((uint32_t *)p);
#else
	uint8_t *u_p = (uint8_t *)p;

	return (u_p[3] | (u_p[2] << 8) | (u_p[1] << 16) | (u_p[0] << 24));
#endif
}
EXPORT_SYMBOL(DWC_CPU_TO_BE32);

uint32_t DWC_LE32_TO_CPU(void *p)
{
#ifdef __LITTLE_ENDIAN
	return *((uint32_t *)p);
#else
	uint8_t *u_p = (uint8_t *)p;

	return (u_p[3] | (u_p[2] << 8) | (u_p[1] << 16) | (u_p[0] << 24));

#endif
}
EXPORT_SYMBOL(DWC_LE32_TO_CPU);

uint32_t DWC_BE32_TO_CPU(void *p)
{
#ifdef __BIG_ENDIAN
	return *((uint32_t *)p);
#else
	uint8_t *u_p = (uint8_t *)p;

	return (u_p[3] | (u_p[2] << 8) | (u_p[1] << 16) | (u_p[0] << 24));
#endif
}
EXPORT_SYMBOL(DWC_BE32_TO_CPU);

uint16_t DWC_CPU_TO_LE16(void *p)
{
#ifdef __LITTLE_ENDIAN
	return *((uint16_t *)p);
#else
	uint8_t *u_p = (uint8_t *)p;
	return (u_p[1] | (u_p[0] << 8));
#endif
}
EXPORT_SYMBOL(DWC_CPU_TO_LE16);

uint16_t DWC_CPU_TO_BE16(void *p)
{
#ifdef __BIG_ENDIAN
	return *((uint16_t *)p);
#else
	uint8_t *u_p = (uint8_t *)p;
	return (u_p[1] | (u_p[0] << 8));
#endif
}
EXPORT_SYMBOL(DWC_CPU_TO_BE16);

uint16_t DWC_LE16_TO_CPU(void *p)
{
#ifdef __LITTLE_ENDIAN
	return *((uint16_t *)p);
#else
	uint8_t *u_p = (uint8_t *)p;
	return (u_p[1] | (u_p[0] << 8));
#endif
}
EXPORT_SYMBOL(DWC_LE16_TO_CPU);

uint16_t DWC_BE16_TO_CPU(void *p)
{
#ifdef __BIG_ENDIAN
	return *((uint16_t *p)p);
#else
	uint8_t *u_p = (uint8_t *)p;
	return (u_p[1] | (u_p[0] << 8));
#endif
}
EXPORT_SYMBOL(DWC_BE16_TO_CPU);


/* Registers */

uint32_t DWC_READ_REG32(uint32_t volatile *reg)
{
	return readl(reg);
}
EXPORT_SYMBOL(DWC_READ_REG32);

#if 0
uint64_t DWC_READ_REG64(uint64_t volatile *reg)
{
}
#endif

void DWC_WRITE_REG32(uint32_t volatile *reg, uint32_t value)
{
	writel(value, reg);
}
EXPORT_SYMBOL(DWC_WRITE_REG32);

#if 0
void DWC_WRITE_REG64(uint64_t volatile *reg, uint64_t value)
{
}
#endif

void DWC_MODIFY_REG32(uint32_t volatile *reg, uint32_t clear_mask, uint32_t set_mask)
{
	writel( (readl(reg) & ~clear_mask) | set_mask, reg );
}
EXPORT_SYMBOL(DWC_MODIFY_REG32);

#if 0
void DWC_MODIFY_REG64(uint64_t volatile *reg, uint64_t value)
{
}
#endif



/* Threading */

typedef struct work_container
{
	dwc_work_callback_t cb;
	void *data;
	dwc_workq_t *wq;
	char *name;

#ifdef DEBUG
	DWC_CIRCLEQ_ENTRY(work_container) entry;
#endif

	struct delayed_work work;
} work_container_t;

#ifdef DEBUG
DWC_CIRCLEQ_HEAD(work_container_queue, work_container);
#endif

struct dwc_workq
{
	struct workqueue_struct *wq;
	int pending;
	dwc_spinlock_t *lock;
	dwc_waitq_t *waitq;

#ifdef DEBUG
	struct work_container_queue entries;
#endif
};

static void do_work(struct work_struct *work)
{
	int64_t flags;
	struct delayed_work *dw = container_of(work, struct delayed_work, work);
	work_container_t *container = container_of(dw, struct work_container, work);
	dwc_workq_t *wq = container->wq;

	container->cb(container->data);

#ifdef DEBUG
	DWC_CIRCLEQ_REMOVE(&wq->entries, container, entry);
#endif

	if (container->name) {
        	DWC_DEBUG("Work done: %s, container=%p",
                          container->name, container); //GRAYG
		DWC_FREE(container->name);
	}
	DWC_FREE(container);

	DWC_SPINLOCK_IRQSAVE(wq->lock, &flags);
	wq->pending --;
	DWC_SPINUNLOCK_IRQRESTORE(wq->lock, flags);
	DWC_WAITQ_TRIGGER(wq->waitq);
}

static int work_done(void *data)
{
	dwc_workq_t *workq = (dwc_workq_t *)data;
	return workq->pending == 0;
}

int DWC_WORKQ_WAIT_WORK_DONE(dwc_workq_t *workq, int timeout)
{
	return DWC_WAITQ_WAIT_TIMEOUT(workq->waitq, work_done, workq, timeout);
}
EXPORT_SYMBOL(DWC_WORKQ_WAIT_WORK_DONE);

dwc_workq_t *DWC_WORKQ_ALLOC(char *name)
{
	dwc_workq_t *wq = DWC_ALLOC(sizeof(*wq));
	wq->wq = create_singlethread_workqueue(name);
	wq->pending = 0;
	wq->lock = DWC_SPINLOCK_ALLOC();
	wq->waitq = DWC_WAITQ_ALLOC();
#ifdef DEBUG
	DWC_CIRCLEQ_INIT(&wq->entries);
#endif
	return wq;
}
EXPORT_SYMBOL(DWC_WORKQ_ALLOC);

void DWC_WORKQ_FREE(dwc_workq_t *wq)
{
#ifdef DEBUG
	if (wq->pending != 0) {
		struct work_container *wc;
		DWC_ERROR("Destroying work queue with pending work");
		DWC_CIRCLEQ_FOREACH(wc, &wq->entries, entry) {
			DWC_ERROR("Work %s still pending", wc->name);
		}
	}
#endif
	destroy_workqueue((struct workqueue_struct *)wq->wq);
	DWC_SPINLOCK_FREE(wq->lock);
	DWC_WAITQ_FREE(wq->waitq);
	DWC_FREE(wq);
}
EXPORT_SYMBOL(DWC_WORKQ_FREE);

void DWC_WORKQ_SCHEDULE(dwc_workq_t *wq, dwc_work_callback_t work_cb, void *data, char *format, ...)
{
	int64_t flags;
	work_container_t *container;
	static char name[128];

	va_list args;
	va_start(args, format);
        if (format)
        	DWC_VSNPRINTF(name, 128, format, args);
	va_end(args);

	DWC_SPINLOCK_IRQSAVE(wq->lock, &flags);
	wq->pending ++;
	DWC_SPINUNLOCK_IRQRESTORE(wq->lock, flags);
	DWC_WAITQ_TRIGGER(wq->waitq);

	container = DWC_ALLOC_ATOMIC(sizeof(*container));

	container->data = data;
	container->cb = work_cb;
	container->wq = wq;
        if (format) {
                container->name = DWC_STRDUP(name);
                DWC_DEBUG("Queueing work: %s, contianer=%p",
                          container->name, container);
        } else
                container->name = NULL;
        
	INIT_WORK(&container->work.work, do_work);

#ifdef DEBUG
	DWC_CIRCLEQ_INSERT_TAIL(&wq->entries, container, entry);
#endif

	queue_work(wq->wq, &container->work.work);

}
EXPORT_SYMBOL(DWC_WORKQ_SCHEDULE);

void DWC_WORKQ_SCHEDULE_DELAYED(dwc_workq_t *wq, dwc_work_callback_t work_cb, void *data, uint32_t time, char *format, ...)
{
	int64_t flags;
	work_container_t *container;
	static char name[128];

	va_list args;
	va_start(args, format);
        if (format)
        	DWC_VSNPRINTF(name, 128, format, args);
	va_end(args);

	DWC_SPINLOCK_IRQSAVE(wq->lock, &flags);
	wq->pending ++;
	DWC_SPINUNLOCK_IRQRESTORE(wq->lock, flags);
	DWC_WAITQ_TRIGGER(wq->waitq);

	container = DWC_ALLOC_ATOMIC(sizeof(*container));

	container->data = data;
	container->cb = work_cb;
	container->wq = wq;
        if (format) { //GRAYG
        	container->name = DWC_STRDUP(name);
                DWC_DEBUG("Queueing work: %s, contianer=%p",
                          container->name, container);
        } else
           container->name = NULL;
	INIT_DELAYED_WORK(&container->work, do_work);

#ifdef DEBUG
	DWC_CIRCLEQ_INSERT_TAIL(&wq->entries, container, entry);
#endif

	queue_delayed_work(wq->wq, &container->work, msecs_to_jiffies(time));

}
EXPORT_SYMBOL(DWC_WORKQ_SCHEDULE_DELAYED);


int DWC_WORKQ_PENDING(dwc_workq_t *wq)
{
	return wq->pending;
}
EXPORT_SYMBOL(DWC_WORKQ_PENDING);

dwc_spinlock_t *DWC_SPINLOCK_ALLOC(void)
{
	spinlock_t *sl = (spinlock_t *)1;
#if defined(CONFIG_PREEMPT) || defined(CONFIG_SMP)
	sl = DWC_ALLOC(sizeof(*sl));
	spin_lock_init(sl);
#endif
	return (dwc_spinlock_t *)sl;
}
EXPORT_SYMBOL(DWC_SPINLOCK_ALLOC);

void DWC_SPINLOCK_FREE(dwc_spinlock_t *lock)
{
#if defined(CONFIG_PREEMPT) || defined(CONFIG_SMP)
	DWC_FREE(lock);
#endif
}
EXPORT_SYMBOL(DWC_SPINLOCK_FREE);

void DWC_SPINLOCK(dwc_spinlock_t *lock)
{
#if defined(CONFIG_PREEMPT) || defined(CONFIG_SMP)
	spin_lock((spinlock_t *)lock);
#endif
}
EXPORT_SYMBOL(DWC_SPINLOCK);

void DWC_SPINUNLOCK(dwc_spinlock_t *lock)
{
#if defined(CONFIG_PREEMPT) || defined(CONFIG_SMP)
	spin_unlock((spinlock_t *)lock);
#endif
}
EXPORT_SYMBOL(DWC_SPINUNLOCK);

void DWC_SPINLOCK_IRQSAVE(dwc_spinlock_t *lock, uint64_t *flags)
{
	unsigned long f;
#if defined(CONFIG_PREEMPT) || defined(CONFIG_SMP)
        spin_lock_irqsave((spinlock_t *)lock, f);
#else
	local_irq_save(f);
#endif
        *flags = f;
}
EXPORT_SYMBOL(DWC_SPINLOCK_IRQSAVE);

void DWC_SPINUNLOCK_IRQRESTORE(dwc_spinlock_t *lock, uint64_t flags)
{
#if defined(CONFIG_PREEMPT) || defined(CONFIG_SMP)
        spin_unlock_irqrestore((spinlock_t *)lock, flags);
#else
        // in kernel 2.6.31, at least, we check for unsigned long
        local_irq_restore((unsigned long)flags);
#endif
}
EXPORT_SYMBOL(DWC_SPINUNLOCK_IRQRESTORE);

dwc_mutex_t *DWC_MUTEX_ALLOC(void)
{
	dwc_mutex_t *mutex = (dwc_mutex_t*)DWC_ALLOC(sizeof(struct mutex));
	struct mutex *m = (struct mutex *)mutex;
	mutex_init(m);
	return mutex;
}
EXPORT_SYMBOL(DWC_MUTEX_ALLOC);

#if (defined(DWC_LINUX) && defined(CONFIG_DEBUG_MUTEXES))
#else
void DWC_MUTEX_FREE(dwc_mutex_t *mutex)
{
	mutex_destroy((struct mutex *)mutex);
	DWC_FREE(mutex);
}
EXPORT_SYMBOL(DWC_MUTEX_FREE);
#endif

void DWC_MUTEX_LOCK(dwc_mutex_t *mutex)
{
	struct mutex *m = (struct mutex *)mutex;
	mutex_lock(m);
}
EXPORT_SYMBOL(DWC_MUTEX_LOCK);

int DWC_MUTEX_TRYLOCK(dwc_mutex_t *mutex)
{
	struct mutex *m = (struct mutex *)mutex;
	return mutex_trylock(m);
}
EXPORT_SYMBOL(DWC_MUTEX_TRYLOCK);

void DWC_MUTEX_UNLOCK(dwc_mutex_t *mutex)
{
	struct mutex *m = (struct mutex *)mutex;
	mutex_unlock(m);
}
EXPORT_SYMBOL(DWC_MUTEX_UNLOCK);

dwc_thread_t *DWC_THREAD_RUN(dwc_thread_function_t thread_function, char *name, void *data)
{
	struct task_struct *thread = kthread_run(thread_function, data, name);
	if (thread == ERR_PTR(-ENOMEM)) {
		return NULL;
	}
	return (dwc_thread_t *)thread;
}
EXPORT_SYMBOL(DWC_THREAD_RUN);

int DWC_THREAD_STOP(dwc_thread_t *thread)
{
	return kthread_stop((struct task_struct *)thread);
}
EXPORT_SYMBOL(DWC_THREAD_STOP);

dwc_bool_t DWC_THREAD_SHOULD_STOP()
{
	return kthread_should_stop();
}
EXPORT_SYMBOL(DWC_THREAD_SHOULD_STOP);

/* Timers */

struct dwc_timer
{
	struct timer_list *t;
	char *name;
	dwc_timer_callback_t cb;
	void *data;
	uint8_t scheduled;
	dwc_spinlock_t *lock;
};

static void set_scheduled(dwc_timer_t *t, int s)
{
	uint64_t flags;
	DWC_SPINLOCK_IRQSAVE(t->lock, &flags);
	t->scheduled = s;
	DWC_SPINUNLOCK_IRQRESTORE(t->lock, flags);
}

static int get_scheduled(dwc_timer_t *t)
{
	int s;
	uint64_t flags;
	DWC_SPINLOCK_IRQSAVE(t->lock, &flags);
	s = t->scheduled;
	DWC_SPINUNLOCK_IRQRESTORE(t->lock, flags);
	return s;
}

static void timer_callback(unsigned long data)
{
	dwc_timer_t *timer = (dwc_timer_t *)data;
	set_scheduled(timer, 0);
	DWC_DEBUG("Timer %s callback", timer->name);
	timer->cb(timer->data);
}

dwc_timer_t *DWC_TIMER_ALLOC(char *name, dwc_timer_callback_t cb, void *data)
{
	dwc_timer_t *t = DWC_ALLOC(sizeof(*t));
	if (!t) {
		DWC_ERROR("Cannot allocate memory for timer");
		return NULL;
	}
	t->t = DWC_ALLOC(sizeof(*t->t));
	if (!t->t) {
		DWC_ERROR("Cannot allocate memory for timer->t");
		goto no_timer;
	}

	t->name = DWC_STRDUP(name);
	if (!t->name) {
		DWC_ERROR("Cannot allocate memory for timer->name");
		goto no_name;
	}

	t->lock = DWC_SPINLOCK_ALLOC();
	if (!t->lock) {
		DWC_ERROR("Cannot allocate memory for lock");
		goto no_lock;
	}
	t->scheduled = 0;
	t->t->base = &boot_tvec_bases;
	t->t->expires = jiffies;
	setup_timer(t->t, timer_callback, (unsigned long)t);

	t->cb = cb;
	t->data = data;

	return t;

 no_lock:
	DWC_FREE(t->name);
 no_name:
	DWC_FREE(t->t);
 no_timer:
	DWC_FREE(t);
	return NULL;
}
EXPORT_SYMBOL(DWC_TIMER_ALLOC);

void DWC_TIMER_FREE(dwc_timer_t *timer)
{
	if (get_scheduled(timer)) {
		del_timer(timer->t);
	}

	DWC_SPINLOCK_FREE(timer->lock);
	DWC_FREE(timer->t);
	DWC_FREE(timer->name);
	DWC_FREE(timer);
}
EXPORT_SYMBOL(DWC_TIMER_FREE);

void DWC_TIMER_SCHEDULE(dwc_timer_t *timer, uint32_t time)
{
	if (!get_scheduled(timer)) {
		set_scheduled(timer, 1);
		//cgg: DWC_DEBUG("Scheduling timer %s to expire in +%d msec", timer->name, time);
		timer->t->expires = jiffies + msecs_to_jiffies(time);
		add_timer(timer->t);
	}
	else {
                //cgg: DWC_DEBUG("Modifying timer %s to expire in +%d msec", timer->name, time);
		mod_timer(timer->t, jiffies + msecs_to_jiffies(time));
	}
}
EXPORT_SYMBOL(DWC_TIMER_SCHEDULE);

void DWC_TIMER_CANCEL(dwc_timer_t *timer)
{
	del_timer(timer->t);
}
EXPORT_SYMBOL(DWC_TIMER_CANCEL);

struct dwc_tasklet
{
	struct tasklet_struct t;
	dwc_tasklet_callback_t cb; 
	void *data;
};

static void tasklet_callback(unsigned long data)
{
	dwc_tasklet_t *t = (dwc_tasklet_t *)data;
	t->cb(t->data);
}

dwc_tasklet_t *DWC_TASK_ALLOC(dwc_tasklet_callback_t cb, void *data)
{
	dwc_tasklet_t *t = DWC_ALLOC(sizeof(*t));
	
	if(t) {
		t->data = data;
		t->cb = cb;
		tasklet_init(&t->t, tasklet_callback, (unsigned long)t);
	} else {
		DWC_ERROR("Cannot allocate memory for tasklet\n");
	}
	
	return t;
}
EXPORT_SYMBOL(DWC_TASK_ALLOC);

void DWC_TASK_FREE(dwc_tasklet_t *t)
{
	DWC_FREE(t);
}
EXPORT_SYMBOL(DWC_TASK_FREE);

void DWC_TASK_SCHEDULE(dwc_tasklet_t *task)
{
	tasklet_schedule(&task->t);
}
EXPORT_SYMBOL(DWC_TASK_SCHEDULE);

/* Timing */

void DWC_UDELAY(uint32_t usecs)
{
	udelay(usecs);
}
EXPORT_SYMBOL(DWC_UDELAY);

void DWC_MDELAY(uint32_t msecs)
{
	mdelay(msecs);
}
EXPORT_SYMBOL(DWC_MDELAY);

void DWC_MSLEEP(uint32_t msecs)
{
	msleep(msecs);
}
EXPORT_SYMBOL(DWC_MSLEEP);

uint32_t DWC_TIME(void)
{
	return jiffies_to_msecs(jiffies);
}
EXPORT_SYMBOL(DWC_TIME);


/* Wait Queues */

struct dwc_waitq
{
	wait_queue_head_t queue;
	int abort;
};

dwc_waitq_t *DWC_WAITQ_ALLOC(void)
{
	dwc_waitq_t *wq = DWC_ALLOC(sizeof(*wq));
	init_waitqueue_head(&wq->queue);
	wq->abort = 0;
	return wq;
}
EXPORT_SYMBOL(DWC_WAITQ_ALLOC);

void DWC_WAITQ_FREE(dwc_waitq_t *wq)
{
	DWC_FREE(wq);
}
EXPORT_SYMBOL(DWC_WAITQ_FREE);

static int32_t check_result(dwc_waitq_t *wq, int result)
{	int32_t msecs;
	if (result > 0) {
		msecs = jiffies_to_msecs(result);
		if (!msecs) {
			return 1;
		}
		return msecs;
	}

	if (result == 0) {
		return -DWC_E_TIMEOUT;
	}

	if ((result == -ERESTARTSYS) || (wq->abort == 1)) {
		return -DWC_E_ABORT;
	}

	return -DWC_E_UNKNOWN;
}

int32_t DWC_WAITQ_WAIT(dwc_waitq_t *wq, dwc_waitq_condition_t condition, void *data)
{
	int result = wait_event_interruptible(wq->queue,
						  condition(data) || wq->abort);
	return check_result(wq, result);
}
EXPORT_SYMBOL(DWC_WAITQ_WAIT);

int32_t DWC_WAITQ_WAIT_TIMEOUT(dwc_waitq_t *wq, dwc_waitq_condition_t condition,
			       void *data, int32_t msecs)
{
	int result = wait_event_interruptible_timeout(wq->queue,
							  condition(data) || wq->abort,
							  msecs_to_jiffies(msecs));
	return check_result(wq, result);
}
EXPORT_SYMBOL(DWC_WAITQ_WAIT_TIMEOUT);

void DWC_WAITQ_TRIGGER(dwc_waitq_t *wq)
{
	wake_up_interruptible(&wq->queue);
}
EXPORT_SYMBOL(DWC_WAITQ_TRIGGER);

void DWC_WAITQ_ABORT(dwc_waitq_t *wq)
{
	wq->abort = 1;
	DWC_WAITQ_TRIGGER(wq);
}
EXPORT_SYMBOL(DWC_WAITQ_ABORT);
