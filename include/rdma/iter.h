/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. */

#ifndef _RDMA_ITER_H_
#define _RDMA_ITER_H_

#include <linux/scatterlist.h>
#include <rdma/ib_umem.h>

/**
 * IB block DMA iterator
 *
 * Iterates the DMA-mapped SGL in contiguous memory blocks aligned
 * to a HW supported page size.
 */
struct ib_block_iter {
	/* internal states */
	struct scatterlist *__sg;	/* sg holding the current aligned block */
	dma_addr_t __dma_addr;		/* unaligned DMA address of this block */
	size_t __sg_numblocks;		/* ib_umem_num_dma_blocks() */
	unsigned int __sg_nents;	/* number of SG entries */
	unsigned int __sg_advance;	/* number of bytes to advance in sg in next step */
	unsigned int __pg_bit;		/* alignment of current block */
};

void __rdma_block_iter_start(struct ib_block_iter *biter,
			     struct scatterlist *sglist,
			     unsigned int nents,
			     unsigned long pgsz);
bool __rdma_block_iter_next(struct ib_block_iter *biter);

/**
 * rdma_block_iter_dma_address - get the aligned dma address of the current
 * block held by the block iterator.
 * @biter: block iterator holding the memory block
 */
static inline dma_addr_t
rdma_block_iter_dma_address(struct ib_block_iter *biter)
{
	return biter->__dma_addr & ~(BIT_ULL(biter->__pg_bit) - 1);
}

/**
 * rdma_for_each_block - iterate over contiguous memory blocks of the sg list
 * @sglist: sglist to iterate over
 * @biter: block iterator holding the memory block
 * @nents: maximum number of sg entries to iterate over
 * @pgsz: best HW supported page size to use
 *
 * Callers may use rdma_block_iter_dma_address() to get each
 * blocks aligned DMA address.
 */
#define rdma_for_each_block(sglist, biter, nents, pgsz)		\
	for (__rdma_block_iter_start(biter, sglist, nents,	\
				     pgsz);			\
	     __rdma_block_iter_next(biter);)

static inline void __rdma_umem_block_iter_start(struct ib_block_iter *biter,
						struct ib_umem *umem,
						unsigned long pgsz)
{
	__rdma_block_iter_start(biter, umem->sgt_append.sgt.sgl,
				umem->sgt_append.sgt.nents, pgsz);
	biter->__sg_advance = ib_umem_offset(umem) & ~(pgsz - 1);
	biter->__sg_numblocks = ib_umem_num_dma_blocks(umem, pgsz);
}

static inline bool __rdma_umem_block_iter_next(struct ib_block_iter *biter)
{
	return __rdma_block_iter_next(biter) && biter->__sg_numblocks--;
}

/**
 * rdma_umem_for_each_dma_block - iterate over contiguous DMA blocks of the umem
 * @umem: umem to iterate over
 * @pgsz: Page size to split the list into
 *
 * pgsz must be <= PAGE_SIZE or computed by ib_umem_find_best_pgsz(). The
 * returned DMA blocks will be aligned to pgsz and span the range:
 * ALIGN_DOWN(umem->address, pgsz) to ALIGN(umem->address + umem->length, pgsz)
 *
 * Performs exactly ib_umem_num_dma_blocks() iterations.
 */
#define rdma_umem_for_each_dma_block(umem, biter, pgsz)                        \
	for (__rdma_umem_block_iter_start(biter, umem, pgsz);                  \
	     __rdma_umem_block_iter_next(biter);)

#endif /* _RDMA_ITER_H_ */
