// SPDX-License-Identifier: GPL-2.0
/*
 * VideoCore Shared Memory CMA allocator
 *
 * Copyright: 2018, Raspberry Pi (Trading) Ltd
 *
 * Based on the Android ION allocator
 * Copyright (C) Linaro 2012
 * Author: <benjamin.gaignard@linaro.org> for ST-Ericsson.
 *
 */

#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/cma.h>
#include <linux/scatterlist.h>

#include "vc_sm_cma.h"

/* CMA heap operations functions */
int vc_sm_cma_buffer_allocate(struct cma *cma_heap,
			      struct vc_sm_cma_alloc_data *buffer,
			      unsigned long len)
{
	/* len should already be page aligned */
	unsigned long num_pages = len / PAGE_SIZE;
	struct sg_table *table;
	struct page *pages;
	int ret;

	pages = cma_alloc(cma_heap, num_pages, 0, GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		goto err;

	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret)
		goto free_mem;

	sg_set_page(table->sgl, pages, len, 0);

	buffer->priv_virt = pages;
	buffer->sg_table = table;
	buffer->cma_heap = cma_heap;
	buffer->num_pages = num_pages;
	return 0;

free_mem:
	kfree(table);
err:
	cma_release(cma_heap, pages, num_pages);
	return -ENOMEM;
}

void vc_sm_cma_buffer_free(struct vc_sm_cma_alloc_data *buffer)
{
	struct cma *cma_heap = buffer->cma_heap;
	struct page *pages = buffer->priv_virt;

	/* release memory */
	if (cma_heap)
		cma_release(cma_heap, pages, buffer->num_pages);

	/* release sg table */
	if (buffer->sg_table) {
		sg_free_table(buffer->sg_table);
		kfree(buffer->sg_table);
		buffer->sg_table = NULL;
	}
}

int __vc_sm_cma_add_heaps(struct cma *cma, void *priv)
{
	struct cma **heap = (struct cma **)priv;
	const char *name = cma_get_name(cma);

	if (!(*heap)) {
		phys_addr_t phys_addr = cma_get_base(cma);

		pr_debug("%s: Adding cma heap %s (start %pap, size %lu) for use by vcsm\n",
			 __func__, name, &phys_addr, cma_get_size(cma));
		*heap = cma;
	} else {
		pr_err("%s: Ignoring heap %s as already set\n",
		       __func__, name);
	}

	return 0;
}

int vc_sm_cma_add_heaps(struct cma **cma_heap)
{
	cma_for_each_area(__vc_sm_cma_add_heaps, cma_heap);
	return 0;
}
