// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/poll.h>
#include <linux/vmalloc.h>
#include <linux/io_uring.h>

#include <uapi/linux/io_uring.h>

#include "io_uring.h"
#include "opdef.h"
#include "kbuf.h"
#include "memmap.h"

/* BIDs are addressed by a 16-bit field in a CQE */
#define MAX_BIDS_PER_BGID (1 << 16)

/* Mapped buffer ring, return io_uring_buf from head */
#define io_ring_head_to_buf(br, head, mask)	&(br)->bufs[(head) & (mask)]

struct io_provide_buf {
	struct file			*file;
	__u64				addr;
	__u32				len;
	__u32				bgid;
	__u32				nbufs;
	__u16				bid;
};

static bool io_kbuf_inc_commit(struct io_buffer_list *bl, int len)
{
	/* No data consumed, return false early to avoid consuming the buffer */
	if (!len)
		return false;

	while (len) {
		struct io_uring_buf *buf;
		u32 buf_len, this_len;

		buf = io_ring_head_to_buf(bl->buf_ring, bl->head, bl->mask);
		buf_len = READ_ONCE(buf->len);
		this_len = min_t(u32, len, buf_len);
		buf_len -= this_len;
		/* Stop looping for invalid buffer length of 0 */
		if (buf_len > bl->min_left_sub_one || !this_len) {
			WRITE_ONCE(buf->addr, READ_ONCE(buf->addr) + this_len);
			WRITE_ONCE(buf->len, buf_len);
			return false;
		}
		WRITE_ONCE(buf->len, 0);
		bl->head++;
		len -= this_len;
	}
	return true;
}

bool io_kbuf_commit(struct io_kiocb *req,
		    struct io_buffer_list *bl, int len, int nr)
{
	if (unlikely(!(req->flags & REQ_F_BUFFERS_COMMIT)))
		return true;

	req->flags &= ~REQ_F_BUFFERS_COMMIT;

	if (unlikely(len < 0))
		return true;
	if (bl->flags & IOBL_INC)
		return io_kbuf_inc_commit(bl, len);
	bl->head += nr;
	return true;
}

static inline struct io_buffer_list *io_buffer_get_list(struct io_ring_ctx *ctx,
							unsigned int bgid)
{
	lockdep_assert_held(&ctx->uring_lock);

	return xa_load(&ctx->io_bl_xa, bgid);
}

static int io_buffer_add_list(struct io_ring_ctx *ctx,
			      struct io_buffer_list *bl, unsigned int bgid)
{
	/*
	 * Store buffer group ID and finally mark the list as visible.
	 * The normal lookup doesn't care about the visibility as we're
	 * always under the ->uring_lock, but the RCU lookup from mmap does.
	 */
	bl->bgid = bgid;
	atomic_set(&bl->refs, 1);
	return xa_err(xa_store(&ctx->io_bl_xa, bgid, bl, GFP_KERNEL));
}

void io_kbuf_drop_legacy(struct io_kiocb *req)
{
	if (WARN_ON_ONCE(!(req->flags & REQ_F_BUFFER_SELECTED)))
		return;
	req->buf_index = req->kbuf->bgid;
	req->flags &= ~REQ_F_BUFFER_SELECTED;
	kfree(req->kbuf);
	req->kbuf = NULL;
}

bool io_kbuf_recycle_legacy(struct io_kiocb *req, unsigned issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_buffer_list *bl;
	struct io_buffer *buf;

	io_ring_submit_lock(ctx, issue_flags);

	buf = req->kbuf;
	bl = io_buffer_get_list(ctx, buf->bgid);
	/*
	 * If the buffer list was upgraded to a ring-based one, or removed,
	 * while the request was in-flight in io-wq, drop it.
	 */
	req->buf_index = buf->bgid;
	if (bl && !(bl->flags & IOBL_BUF_RING))
		list_add(&buf->list, &bl->buf_list);
	else
		kfree(buf);
	req->flags &= ~REQ_F_BUFFER_SELECTED;
	req->kbuf = NULL;

	io_ring_submit_unlock(ctx, issue_flags);
	return true;
}

static void __user *io_provided_buffer_select(struct io_kiocb *req, size_t *len,
					      struct io_buffer_list *bl)
{
	if (!list_empty(&bl->buf_list)) {
		struct io_buffer *kbuf;

		kbuf = list_first_entry(&bl->buf_list, struct io_buffer, list);
		list_del(&kbuf->list);
		if (*len == 0 || *len > kbuf->len)
			*len = kbuf->len;
		if (list_empty(&bl->buf_list))
			req->flags |= REQ_F_BL_EMPTY;
		req->flags |= REQ_F_BUFFER_SELECTED;
		req->kbuf = kbuf;
		req->buf_index = kbuf->bid;
		return u64_to_user_ptr(kbuf->addr);
	}
	return NULL;
}

static int io_provided_buffers_select(struct io_kiocb *req, size_t *len,
				      struct io_buffer_list *bl,
				      struct iovec *iov)
{
	void __user *buf;

	buf = io_provided_buffer_select(req, len, bl);
	if (unlikely(!buf))
		return -ENOBUFS;

	iov[0].iov_base = buf;
	iov[0].iov_len = *len;
	return 1;
}

static struct io_br_sel io_ring_buffer_select(struct io_kiocb *req, size_t *len,
					      struct io_buffer_list *bl,
					      unsigned int issue_flags)
{
	struct io_uring_buf_ring *br = bl->buf_ring;
	__u16 tail, head = bl->head;
	struct io_br_sel sel = { };
	struct io_uring_buf *buf;
	u32 buf_len;

	tail = smp_load_acquire(&br->tail);
	if (unlikely(tail == head))
		return sel;

	if ((__u16)(head + 1) == tail)
		req->flags |= REQ_F_BL_EMPTY;

	buf = io_ring_head_to_buf(br, head, bl->mask);
	buf_len = READ_ONCE(buf->len);
	if (*len == 0 || *len > buf_len)
		*len = buf_len;
	req->flags |= REQ_F_BUFFER_RING | REQ_F_BUFFERS_COMMIT;
	req->buf_index = READ_ONCE(buf->bid);
	sel.buf_list = bl;
	sel.addr = u64_to_user_ptr(READ_ONCE(buf->addr));

	if (issue_flags & IO_URING_F_UNLOCKED || !io_file_can_poll(req)) {
		/*
		 * If we came in unlocked, we have no choice but to consume the
		 * buffer here, otherwise nothing ensures that the buffer won't
		 * get used by others. This does mean it'll be pinned until the
		 * IO completes, coming in unlocked means we're being called from
		 * io-wq context and there may be further retries in async hybrid
		 * mode. For the locked case, the caller must call commit when
		 * the transfer completes (or if we get -EAGAIN and must poll of
		 * retry).
		 */
		if (!io_kbuf_commit(req, sel.buf_list, *len, 1))
			req->flags |= REQ_F_BUF_MORE;
		sel.buf_list = NULL;
	}
	return sel;
}

struct io_br_sel io_buffer_select(struct io_kiocb *req, size_t *len,
				  unsigned int issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_br_sel sel = { };
	struct io_buffer_list *bl;

	io_ring_submit_lock(req->ctx, issue_flags);

	bl = io_buffer_get_list(ctx, req->buf_index);
	if (likely(bl)) {
		if (bl->flags & IOBL_BUF_RING)
			sel = io_ring_buffer_select(req, len, bl, issue_flags);
		else
			sel.addr = io_provided_buffer_select(req, len, bl);
	}
	io_ring_submit_unlock(req->ctx, issue_flags);
	return sel;
}

/* cap it at a reasonable 256, will be one page even for 4K */
#define PEEK_MAX_IMPORT		256

static int io_ring_buffers_peek(struct io_kiocb *req, struct buf_sel_arg *arg,
				struct io_buffer_list *bl)
{
	struct io_uring_buf_ring *br = bl->buf_ring;
	struct iovec *iov = arg->iovs;
	int nr_iovs = arg->nr_iovs;
	__u16 nr_avail, tail, head;
	struct io_uring_buf *buf;

	tail = smp_load_acquire(&br->tail);
	head = bl->head;
	nr_avail = min_t(__u16, tail - head, UIO_MAXIOV);
	if (unlikely(!nr_avail))
		return -ENOBUFS;

	buf = io_ring_head_to_buf(br, head, bl->mask);
	if (arg->max_len) {
		u32 len = READ_ONCE(buf->len);
		size_t needed;

		if (unlikely(!len))
			return -ENOBUFS;
		needed = (arg->max_len + len - 1) / len;
		needed = min_not_zero(needed, (size_t) PEEK_MAX_IMPORT);
		if (nr_avail > needed)
			nr_avail = needed;
	}

	/*
	 * only alloc a bigger array if we know we have data to map, eg not
	 * a speculative peek operation.
	 */
	if (arg->mode & KBUF_MODE_EXPAND && nr_avail > nr_iovs && arg->max_len) {
		iov = kmalloc_array(nr_avail, sizeof(struct iovec), GFP_KERNEL);
		if (unlikely(!iov))
			return -ENOMEM;
		if (arg->mode & KBUF_MODE_FREE)
			kfree(arg->iovs);
		arg->iovs = iov;
		nr_iovs = nr_avail;
	} else if (nr_avail < nr_iovs) {
		nr_iovs = nr_avail;
	}

	/* set it to max, if not set, so we can use it unconditionally */
	if (!arg->max_len)
		arg->max_len = INT_MAX;

	req->buf_index = READ_ONCE(buf->bid);
	do {
		u32 len = READ_ONCE(buf->len);

		/* truncate end piece, if needed, for non partial buffers */
		if (len > arg->max_len) {
			len = arg->max_len;
			if (!(bl->flags & IOBL_INC)) {
				arg->partial_map = 1;
				if (iov != arg->iovs)
					break;
			}
		}

		iov->iov_base = u64_to_user_ptr(READ_ONCE(buf->addr));
		iov->iov_len = len;
		iov++;

		arg->out_len += len;
		arg->max_len -= len;
		if (!arg->max_len)
			break;

		buf = io_ring_head_to_buf(br, ++head, bl->mask);
	} while (--nr_iovs);

	if (head == tail)
		req->flags |= REQ_F_BL_EMPTY;

	req->flags |= REQ_F_BUFFER_RING;
	return iov - arg->iovs;
}

int io_buffers_select(struct io_kiocb *req, struct buf_sel_arg *arg,
		      struct io_br_sel *sel, unsigned int issue_flags)
{
	struct io_ring_ctx *ctx = req->ctx;
	int ret = -ENOENT;

	io_ring_submit_lock(ctx, issue_flags);
	sel->buf_list = io_buffer_get_list(ctx, req->buf_index);
	if (unlikely(!sel->buf_list))
		goto out_unlock;

	if (sel->buf_list->flags & IOBL_BUF_RING) {
		ret = io_ring_buffers_peek(req, arg, sel->buf_list);
		/*
		 * Don't recycle these buffers if we need to go through poll.
		 * Nobody else can use them anyway, and holding on to provided
		 * buffers for a send/write operation would happen on the app
		 * side anyway with normal buffers. Besides, we already
		 * committed them, they cannot be put back in the queue.
		 */
		if (ret > 0) {
			req->flags |= REQ_F_BUFFERS_COMMIT | REQ_F_BL_NO_RECYCLE;
			if (!io_kbuf_commit(req, sel->buf_list, arg->out_len, ret))
				req->flags |= REQ_F_BUF_MORE;
		}
	} else {
		ret = io_provided_buffers_select(req, &arg->out_len, sel->buf_list, arg->iovs);
	}
out_unlock:
	if (issue_flags & IO_URING_F_UNLOCKED) {
		sel->buf_list = NULL;
		mutex_unlock(&ctx->uring_lock);
	}
	return ret;
}

int io_buffers_peek(struct io_kiocb *req, struct buf_sel_arg *arg,
		    struct io_br_sel *sel)
{
	struct io_ring_ctx *ctx = req->ctx;
	struct io_buffer_list *bl;
	int ret;

	lockdep_assert_held(&ctx->uring_lock);

	bl = io_buffer_get_list(ctx, req->buf_index);
	if (unlikely(!bl))
		return -ENOENT;

	if (bl->flags & IOBL_BUF_RING) {
		ret = io_ring_buffers_peek(req, arg, bl);
		if (ret > 0)
			req->flags |= REQ_F_BUFFERS_COMMIT;
		sel->buf_list = bl;
		return ret;
	}

	/* don't support multiple buffer selections for legacy */
	sel->buf_list = NULL;
	return io_provided_buffers_select(req, &arg->max_len, bl, arg->iovs);
}

static inline bool __io_put_kbuf_ring(struct io_kiocb *req,
				      struct io_buffer_list *bl, int len, int nr)
{
	bool ret = true;

	if (bl) {
		ret = io_kbuf_commit(req, bl, len, nr);
		req->buf_index = bl->bgid;
	}
	if (ret && (req->flags & REQ_F_BUF_MORE))
		ret = false;

	req->flags &= ~(REQ_F_BUFFER_RING | REQ_F_BUF_MORE);
	return ret;
}

unsigned int __io_put_kbufs(struct io_kiocb *req, struct io_buffer_list *bl,
			    int len, int nbufs)
{
	unsigned int ret;

	ret = IORING_CQE_F_BUFFER | (req->buf_index << IORING_CQE_BUFFER_SHIFT);

	if (unlikely(!(req->flags & REQ_F_BUFFER_RING))) {
		io_kbuf_drop_legacy(req);
		return ret;
	}

	if (!__io_put_kbuf_ring(req, bl, len, nbufs))
		ret |= IORING_CQE_F_BUF_MORE;
	return ret;
}

static int __io_remove_buffers(struct io_ring_ctx *ctx,
			       struct io_buffer_list *bl, unsigned nbufs)
{
	unsigned i = 0;

	/* shouldn't happen */
	if (!nbufs)
		return 0;

	if (bl->flags & IOBL_BUF_RING) {
		i = bl->buf_ring->tail - bl->head;
		if (bl->buf_nr_pages) {
			int j;

			if (!(bl->flags & IOBL_MMAP)) {
				for (j = 0; j < bl->buf_nr_pages; j++)
					unpin_user_page(bl->buf_pages[j]);
			}
			io_pages_unmap(bl->buf_ring, &bl->buf_pages,
					&bl->buf_nr_pages, bl->flags & IOBL_MMAP);
			bl->flags &= ~IOBL_MMAP;
		}
		/* make sure it's seen as empty */
		INIT_LIST_HEAD(&bl->buf_list);
		bl->flags &= ~IOBL_BUF_RING;
		return i;
	}

	lockdep_assert_held(&ctx->uring_lock);

	while (!list_empty(&bl->buf_list)) {
		struct io_buffer *nxt;

		nxt = list_first_entry(&bl->buf_list, struct io_buffer, list);
		list_del(&nxt->list);
		kfree(nxt);

		if (++i == nbufs)
			return i;
		cond_resched();
	}

	return i;
}

void io_put_bl(struct io_ring_ctx *ctx, struct io_buffer_list *bl)
{
	if (atomic_dec_and_test(&bl->refs)) {
		__io_remove_buffers(ctx, bl, -1U);
		kfree_rcu(bl, rcu);
	}
}

void io_destroy_buffers(struct io_ring_ctx *ctx)
{
	struct io_buffer_list *bl;
	unsigned long index;

	xa_for_each(&ctx->io_bl_xa, index, bl) {
		xa_erase(&ctx->io_bl_xa, bl->bgid);
		io_put_bl(ctx, bl);
	}
}

static void io_destroy_bl(struct io_ring_ctx *ctx, struct io_buffer_list *bl)
{
	xa_erase(&ctx->io_bl_xa, bl->bgid);
	io_put_bl(ctx, bl);
}

int io_remove_buffers_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_provide_buf *p = io_kiocb_to_cmd(req, struct io_provide_buf);
	u64 tmp;

	if (sqe->rw_flags || sqe->addr || sqe->len || sqe->off ||
	    sqe->splice_fd_in)
		return -EINVAL;

	tmp = READ_ONCE(sqe->fd);
	if (!tmp || tmp > MAX_BIDS_PER_BGID)
		return -EINVAL;

	memset(p, 0, sizeof(*p));
	p->nbufs = tmp;
	p->bgid = READ_ONCE(sqe->buf_group);
	return 0;
}

int io_remove_buffers(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_provide_buf *p = io_kiocb_to_cmd(req, struct io_provide_buf);
	struct io_ring_ctx *ctx = req->ctx;
	struct io_buffer_list *bl;
	int ret = 0;

	io_ring_submit_lock(ctx, issue_flags);

	ret = -ENOENT;
	bl = io_buffer_get_list(ctx, p->bgid);
	if (bl) {
		ret = -EINVAL;
		/* can't use provide/remove buffers command on mapped buffers */
		if (!(bl->flags & IOBL_BUF_RING))
			ret = __io_remove_buffers(ctx, bl, p->nbufs);
	}
	io_ring_submit_unlock(ctx, issue_flags);
	if (ret < 0)
		req_set_fail(req);
	io_req_set_res(req, ret, 0);
	return IOU_OK;
}

int io_provide_buffers_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	unsigned long size, tmp_check;
	struct io_provide_buf *p = io_kiocb_to_cmd(req, struct io_provide_buf);
	u64 tmp;

	if (sqe->rw_flags || sqe->splice_fd_in)
		return -EINVAL;

	tmp = READ_ONCE(sqe->fd);
	if (!tmp || tmp > MAX_BIDS_PER_BGID)
		return -E2BIG;
	p->nbufs = tmp;
	p->addr = READ_ONCE(sqe->addr);
	p->len = READ_ONCE(sqe->len);
	if (!p->len)
		return -EINVAL;

	if (check_mul_overflow((unsigned long)p->len, (unsigned long)p->nbufs,
				&size))
		return -EOVERFLOW;
	if (check_add_overflow((unsigned long)p->addr, size, &tmp_check))
		return -EOVERFLOW;

	size = (unsigned long)p->len * p->nbufs;
	if (!access_ok(u64_to_user_ptr(p->addr), size))
		return -EFAULT;

	p->bgid = READ_ONCE(sqe->buf_group);
	tmp = READ_ONCE(sqe->off);
	if (tmp > USHRT_MAX)
		return -E2BIG;
	if (tmp + p->nbufs > MAX_BIDS_PER_BGID)
		return -EINVAL;
	p->bid = tmp;
	return 0;
}

static int io_add_buffers(struct io_ring_ctx *ctx, struct io_provide_buf *pbuf,
			  struct io_buffer_list *bl)
{
	struct io_buffer *buf;
	u64 addr = pbuf->addr;
	int i, bid = pbuf->bid;

	for (i = 0; i < pbuf->nbufs; i++) {
		buf = kmalloc(sizeof(*buf), GFP_KERNEL_ACCOUNT);
		if (!buf)
			break;

		list_add_tail(&buf->list, &bl->buf_list);
		buf->addr = addr;
		buf->len = min_t(__u32, pbuf->len, MAX_RW_COUNT);
		buf->bid = bid;
		buf->bgid = pbuf->bgid;
		addr += pbuf->len;
		bid++;
		cond_resched();
	}

	return i ? 0 : -ENOMEM;
}

int io_provide_buffers(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_provide_buf *p = io_kiocb_to_cmd(req, struct io_provide_buf);
	struct io_ring_ctx *ctx = req->ctx;
	struct io_buffer_list *bl;
	int ret = 0;

	io_ring_submit_lock(ctx, issue_flags);

	bl = io_buffer_get_list(ctx, p->bgid);
	if (unlikely(!bl)) {
		bl = kzalloc(sizeof(*bl), GFP_KERNEL_ACCOUNT);
		if (!bl) {
			ret = -ENOMEM;
			goto err;
		}
		INIT_LIST_HEAD(&bl->buf_list);
		ret = io_buffer_add_list(ctx, bl, p->bgid);
		if (ret) {
			/*
			 * Doesn't need rcu free as it was never visible, but
			 * let's keep it consistent throughout.
			 */
			kfree_rcu(bl, rcu);
			goto err;
		}
	}
	/* can't add buffers via this command for a mapped buffer ring */
	if (bl->flags & IOBL_BUF_RING) {
		ret = -EINVAL;
		goto err;
	}

	ret = io_add_buffers(ctx, p, bl);
err:
	io_ring_submit_unlock(ctx, issue_flags);

	if (ret < 0)
		req_set_fail(req);
	io_req_set_res(req, ret, 0);
	return IOU_OK;
}

static int io_pin_pbuf_ring(struct io_uring_buf_reg *reg,
			    struct io_buffer_list *bl)
{
	struct io_uring_buf_ring *br = NULL;
	struct page **pages;
	int nr_pages, ret;

	pages = io_pin_pages(reg->ring_addr,
			     flex_array_size(br, bufs, reg->ring_entries),
			     &nr_pages);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	br = vmap(pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!br) {
		ret = -ENOMEM;
		goto error_unpin;
	}

#ifdef SHM_COLOUR
	/*
	 * On platforms that have specific aliasing requirements, SHM_COLOUR
	 * is set and we must guarantee that the kernel and user side align
	 * nicely. We cannot do that if IOU_PBUF_RING_MMAP isn't set and
	 * the application mmap's the provided ring buffer. Fail the request
	 * if we, by chance, don't end up with aligned addresses. The app
	 * should use IOU_PBUF_RING_MMAP instead, and liburing will handle
	 * this transparently.
	 */
	if ((reg->ring_addr | (unsigned long) br) & (SHM_COLOUR - 1)) {
		ret = -EINVAL;
		goto error_unpin;
	}
#endif
	bl->buf_pages = pages;
	bl->buf_nr_pages = nr_pages;
	bl->buf_ring = br;
	bl->flags |= IOBL_BUF_RING;
	bl->flags &= ~IOBL_MMAP;
	return 0;
error_unpin:
	unpin_user_pages(pages, nr_pages);
	kvfree(pages);
	vunmap(br);
	return ret;
}

static int io_alloc_pbuf_ring(struct io_ring_ctx *ctx,
			      struct io_uring_buf_reg *reg,
			      struct io_buffer_list *bl)
{
	size_t ring_size;

	ring_size = reg->ring_entries * sizeof(struct io_uring_buf_ring);

	bl->buf_ring = io_pages_map(&bl->buf_pages, &bl->buf_nr_pages, ring_size);
	if (IS_ERR(bl->buf_ring)) {
		bl->buf_ring = NULL;
		return -ENOMEM;
	}

	bl->flags |= (IOBL_BUF_RING | IOBL_MMAP);
	return 0;
}

int io_register_pbuf_ring(struct io_ring_ctx *ctx, void __user *arg)
{
	struct io_uring_buf_reg reg;
	struct io_buffer_list *bl, *free_bl = NULL;
	int ret;

	lockdep_assert_held(&ctx->uring_lock);

	if (copy_from_user(&reg, arg, sizeof(reg)))
		return -EFAULT;
	if (!mem_is_zero(reg.resv, sizeof(reg.resv)))
		return -EINVAL;
	if (reg.flags & ~(IOU_PBUF_RING_MMAP | IOU_PBUF_RING_INC))
		return -EINVAL;
	if (!(reg.flags & IOU_PBUF_RING_MMAP)) {
		if (!reg.ring_addr)
			return -EFAULT;
		if (reg.ring_addr & ~PAGE_MASK)
			return -EINVAL;
	} else {
		if (reg.ring_addr)
			return -EINVAL;
	}

	if (!is_power_of_2(reg.ring_entries))
		return -EINVAL;

	/* cannot disambiguate full vs empty due to head/tail size */
	if (reg.ring_entries >= 65536)
		return -EINVAL;

	/* minimum left byte count is a property of incremental buffers */
	if (!(reg.flags & IOU_PBUF_RING_INC) && reg.min_left)
		return -EINVAL;

	bl = io_buffer_get_list(ctx, reg.bgid);
	if (bl) {
		/* if mapped buffer ring OR classic exists, don't allow */
		if (bl->flags & IOBL_BUF_RING || !list_empty(&bl->buf_list))
			return -EEXIST;
		io_destroy_bl(ctx, bl);
	}

	free_bl = bl = kzalloc(sizeof(*bl), GFP_KERNEL_ACCOUNT);
	if (!bl)
		return -ENOMEM;

	if (!(reg.flags & IOU_PBUF_RING_MMAP))
		ret = io_pin_pbuf_ring(&reg, bl);
	else
		ret = io_alloc_pbuf_ring(ctx, &reg, bl);

	if (!ret) {
		bl->nr_entries = reg.ring_entries;
		bl->mask = reg.ring_entries - 1;
		if (reg.min_left)
			bl->min_left_sub_one = reg.min_left - 1;
		if (reg.flags & IOU_PBUF_RING_INC)
			bl->flags |= IOBL_INC;

		io_buffer_add_list(ctx, bl, reg.bgid);
		return 0;
	}

	kfree_rcu(free_bl, rcu);
	return ret;
}

int io_unregister_pbuf_ring(struct io_ring_ctx *ctx, void __user *arg)
{
	struct io_uring_buf_reg reg;
	struct io_buffer_list *bl;

	lockdep_assert_held(&ctx->uring_lock);

	if (copy_from_user(&reg, arg, sizeof(reg)))
		return -EFAULT;
	if (!mem_is_zero(reg.resv, sizeof(reg.resv)) || reg.flags)
		return -EINVAL;

	bl = io_buffer_get_list(ctx, reg.bgid);
	if (!bl)
		return -ENOENT;
	if (!(bl->flags & IOBL_BUF_RING))
		return -EINVAL;

	xa_erase(&ctx->io_bl_xa, bl->bgid);
	io_put_bl(ctx, bl);
	return 0;
}

int io_register_pbuf_status(struct io_ring_ctx *ctx, void __user *arg)
{
	struct io_uring_buf_status buf_status;
	struct io_buffer_list *bl;

	if (copy_from_user(&buf_status, arg, sizeof(buf_status)))
		return -EFAULT;
	if (!mem_is_zero(buf_status.resv, sizeof(buf_status.resv)))
		return -EINVAL;

	bl = io_buffer_get_list(ctx, buf_status.buf_group);
	if (!bl)
		return -ENOENT;
	if (!(bl->flags & IOBL_BUF_RING))
		return -EINVAL;

	buf_status.head = bl->head;
	if (copy_to_user(arg, &buf_status, sizeof(buf_status)))
		return -EFAULT;

	return 0;
}

struct io_buffer_list *io_pbuf_get_bl(struct io_ring_ctx *ctx,
				      unsigned long bgid)
{
	struct io_buffer_list *bl;
	bool ret;

	/*
	 * We have to be a bit careful here - we're inside mmap and cannot grab
	 * the uring_lock. This means the buffer_list could be simultaneously
	 * going away, if someone is trying to be sneaky. Look it up under rcu
	 * so we know it's not going away, and attempt to grab a reference to
	 * it. If the ref is already zero, then fail the mapping. If successful,
	 * the caller will call io_put_bl() to drop the the reference at at the
	 * end. This may then safely free the buffer_list (and drop the pages)
	 * at that point, vm_insert_pages() would've already grabbed the
	 * necessary vma references.
	 */
	rcu_read_lock();
	bl = xa_load(&ctx->io_bl_xa, bgid);
	/* must be a mmap'able buffer ring and have pages */
	ret = false;
	if (bl && bl->flags & IOBL_MMAP)
		ret = atomic_inc_not_zero(&bl->refs);
	rcu_read_unlock();

	if (ret)
		return bl;

	return ERR_PTR(-EINVAL);
}

int io_pbuf_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct io_ring_ctx *ctx = file->private_data;
	loff_t pgoff = vma->vm_pgoff << PAGE_SHIFT;
	struct io_buffer_list *bl;
	int bgid, ret;

	bgid = (pgoff & ~IORING_OFF_MMAP_MASK) >> IORING_OFF_PBUF_SHIFT;
	bl = io_pbuf_get_bl(ctx, bgid);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	ret = io_uring_mmap_pages(ctx, vma, bl->buf_pages, bl->buf_nr_pages);
	io_put_bl(ctx, bl);
	return ret;
}
