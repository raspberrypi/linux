#include "dwc_os.h"
#include "dwc_list.h"

/* Memory Debugging */
#ifdef DEBUG_MEMORY

struct allocation
{
	void *addr;
	char *func;
	int line;
	uint32_t size;
	int dma;
	DWC_CIRCLEQ_ENTRY(allocation) entry;
};

DWC_CIRCLEQ_HEAD(allocation_queue, allocation);

struct allocation_manager
{
	struct allocation_queue allocations;

	/* statistics */
	int num;
	int num_freed;
	int num_active;
	uint32_t total;
	uint32_t current;
	uint32_t max;
};


static struct allocation_manager *manager = NULL;

static void add_allocation(uint32_t size, char const* func, int line, void *addr, int dma)
{
	struct allocation *a = __DWC_ALLOC_ATOMIC(sizeof(*a));
	a->func = __DWC_ALLOC_ATOMIC(DWC_STRLEN(func)+1);
	DWC_MEMCPY(a->func, func, DWC_STRLEN(func)+1);
	a->line = line;
	a->size = size;
	a->addr = addr;
	a->dma = dma;
	DWC_CIRCLEQ_INSERT_TAIL(&manager->allocations, a, entry);

	/* Update stats */
	manager->num ++;
	manager->num_active ++;
	manager->total += size;
	manager->current += size;
	if (manager->max < manager->current) {
		manager->max = manager->current;
	}
}

static struct allocation *find_allocation(void *addr)
{
	struct allocation *a;
	DWC_CIRCLEQ_FOREACH(a, &manager->allocations, entry) {
		if (a->addr == addr) {
			return a;
		}
	}
	return NULL;
}

static void free_allocation(void *addr, char const* func, int line)
{
	struct allocation *a = find_allocation(addr);
	if (!a && func && (line >= 0)) {
		DWC_ASSERT(0, "Free of address %p that was never allocated or already freed %s:%d", addr, func, line);
		return;
	}
	DWC_CIRCLEQ_REMOVE(&manager->allocations, a, entry);

	manager->num_active --;
	manager->num_freed ++;
	manager->current -= a->size;
	__DWC_FREE(a->func);
	__DWC_FREE(a);
}

void dwc_memory_debug_start(void)
{
	DWC_ASSERT(manager == NULL, "Memory debugging has already started\n");
	if (manager == NULL) {
		manager = __DWC_ALLOC(sizeof(*manager));
	}

	DWC_CIRCLEQ_INIT(&manager->allocations);
	manager->num = 0;
	manager->num_freed = 0;
	manager->num_active = 0;
	manager->total = 0;
	manager->current = 0;
	manager->max = 0;
}

void dwc_memory_debug_stop(void)
{
	struct allocation *a;
	dwc_memory_debug_report();

	DWC_CIRCLEQ_FOREACH(a, &manager->allocations, entry) {
		DWC_ERROR("Memory leaked from %s:%d\n", a->func, a->line);
		free_allocation(a->addr, NULL, -1);
	}

	__DWC_FREE(manager);
}

void dwc_memory_debug_report(void)
{
	struct allocation *a;
	DWC_PRINTF("\n\n\n----------------- Memory Debugging Report -----------------\n\n");
	DWC_PRINTF("Num Allocations = %d\n", manager->num);
	DWC_PRINTF("Freed = %d\n", manager->num_freed);
	DWC_PRINTF("Active = %d\n", manager->num_active);
	DWC_PRINTF("Current Memory Used = %d\n", manager->current);
	DWC_PRINTF("Total Memory Used = %d\n", manager->total);
	DWC_PRINTF("Maximum Memory Used at Once = %d\n", manager->max);
	DWC_PRINTF("Unfreed allocations:\n");

	DWC_CIRCLEQ_FOREACH(a, &manager->allocations, entry) {
		DWC_PRINTF("    addr=%p, size=%d from %s:%d, DMA=%d\n", a->addr, a->size, a->func, a->line, a->dma);
	}
}



/* The replacement functions */
void *dwc_alloc_debug(uint32_t size, char const* func, int line)
{
	void *addr = __DWC_ALLOC(size);
	add_allocation(size, func, line, addr, 0);
	return addr;
}

void *dwc_alloc_atomic_debug(uint32_t size, char const* func, int line)
{
	void *addr = __DWC_ALLOC_ATOMIC(size);
	add_allocation(size, func, line, addr, 0);
	return addr;
}

void dwc_free_debug(void *addr, char const* func, int line)
{
	free_allocation(addr, func, line);
	__DWC_FREE(addr);
}

void *dwc_dma_alloc_debug(uint32_t size, dwc_dma_t *dma_addr, char const *func, int line)
{
	void *addr = __DWC_DMA_ALLOC(size, dma_addr);
	add_allocation(size, func, line, addr, 1);
	return addr;
}

void *dwc_dma_alloc_atomic_debug(uint32_t size, dwc_dma_t *dma_addr, char const *func, int line)
{
	void *addr = __DWC_DMA_ALLOC_ATOMIC(size, dma_addr);
	add_allocation(size, func, line, addr, 1);
	return addr;
}

void dwc_dma_free_debug(uint32_t size, void *virt_addr, dwc_dma_t dma_addr, char const *func, int line)
{
	free_allocation(virt_addr, func, line);
	__DWC_DMA_FREE(size, virt_addr, dma_addr);
}

#endif /* DEBUG_MEMORY */
