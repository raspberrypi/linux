#include <linux/module.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/blkdev.h>

static int adamblock_major;
#define ADAMBLOCK_MINOR	1
static int adamblock_sect_size = 512;
static int adamblock_nsects = 10 * 1024;

struct adamblock_dev {
	int minor;
	spinlock_t lock;
	struct request_queue *queue;
	struct gendisk *disk;
	ssize_t size;
	void *data;
};

struct adamblock_dev *adamblock_dev = NULL;

/*
 * Do an I/O operation for each segment
 */
static int adamblock_handle_io(struct adamblock_dev *adamblock_dev,
		uint64_t pos, ssize_t size, void *buffer, int write)
{
	if (write)
		memcpy(adamblock_dev->data + pos, buffer, size);
	else
		memcpy(buffer, adamblock_dev->data + pos, size);

	return 0;
}

static void adamblock_request(struct request_queue *q)
{
	struct request *rq = NULL;
	int rv = 0;
	uint64_t pos = 0;
	ssize_t size = 0;
	struct bio_vec bvec;
	struct req_iterator iter;
	void *kaddr = NULL;

	pr_info("q=0x%p nr_rqs[0]=%d nr_rqs[1]=%d nr_requests=%lu\n",
			q, q->nr_rqs[0], q->nr_rqs[1], q->nr_requests);
	while ((rq = blk_fetch_request(q)) != NULL) {
		spin_unlock_irq(q->queue_lock);
		BUG_ON(adamblock_dev != rq->rq_disk->private_data);

		pr_debug("rq=0x%p, extra_len=%u nr_physegs=%u\n",
			rq, rq->extra_len, rq->nr_phys_segments);
		pos = blk_rq_pos(rq) * adamblock_sect_size;
		size = blk_rq_bytes(rq);
		if ((pos + size > adamblock_dev->size)) {
			pr_crit("adamblock: Beyond-end write (%llu %zx)\n",
				pos, size);
			rv = -EIO;
			goto skip;
		}

		rq_for_each_segment(bvec, rq, iter) {
			kaddr = kmap(bvec.bv_page);

			pr_debug("bv_page=0x%x kaddr=0x%x pos=%llu bv_len=%u bv_offset=%u\n",
					(unsigned int)bvec.bv_page, (unsigned int)kaddr, pos, bvec.bv_len, bvec.bv_offset);

			rv = adamblock_handle_io(adamblock_dev, pos,
				bvec.bv_len, kaddr + bvec.bv_offset,
				rq_data_dir(rq));
			if (rv < 0)
				goto skip;

			pos += bvec.bv_len;
			kunmap(bvec.bv_page);
		}
skip:

		blk_end_request_all(rq, rv);

		spin_lock_irq(q->queue_lock);
	}
}

static int adamblock_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned command, unsigned long argument)
{
	pr_err("adam block ioctl! bdev=0x%p mode=%d command=%u argument=%lu\n",
			bdev, (int)mode, command, argument);
	return 0;
}

static int adamblock_open(struct block_device *bdev, fmode_t mode)
{
	pr_err("adam block is opened! bdev=0x%p mode=%d\n", bdev, (int)mode);
	return 0;
}

static void adamblock_release(struct gendisk *disk, fmode_t mode)
{
	pr_err("adam block is released! disk=0x%p mode=%d\n", disk, (int)mode);
}

static const struct block_device_operations adamblock_fops = {
	.owner = THIS_MODULE,
	.open = adamblock_open,
	.release = adamblock_release,
	.ioctl = adamblock_ioctl,
};

static int adamblock_alloc(int minor)
{
	struct gendisk *disk;
	int rv = 0;

	adamblock_dev = kzalloc(sizeof(struct adamblock_dev), GFP_KERNEL);
	if (!adamblock_dev) {
		rv = -ENOMEM;
		goto fail;
	}

	adamblock_dev->size = adamblock_sect_size * adamblock_nsects;
	adamblock_dev->data = vmalloc(adamblock_dev->size);
	if (!adamblock_dev->data) {
		rv = -ENOMEM;
		goto fail_dev;
	}
	adamblock_dev->minor = minor;

	spin_lock_init(&adamblock_dev->lock);
	adamblock_dev->queue = blk_init_queue(adamblock_request,
	    &adamblock_dev->lock);
	if (!adamblock_dev->queue) {
		rv = -ENOMEM;
		goto fail_data;
	}

	disk = alloc_disk(minor);
	if (!disk) {
		rv = -ENOMEM;
		goto fail_queue;
	}
	adamblock_dev->disk = disk;

	disk->major = adamblock_major;
	disk->first_minor = minor;
	disk->fops = &adamblock_fops;
	disk->private_data = adamblock_dev;
	disk->queue = adamblock_dev->queue;
	sprintf(disk->disk_name, "adamblock%d", minor);
	set_capacity(disk, adamblock_nsects);
	add_disk(disk);

	return 0;

fail_queue:
	blk_cleanup_queue(adamblock_dev->queue);
fail_data:
	vfree(adamblock_dev->data);
fail_dev:
	kfree(adamblock_dev);
fail:
	return rv;
}

static void adamblock_free(struct adamblock_dev *adamblock_dev)
{
	del_gendisk(adamblock_dev->disk);
	blk_cleanup_queue(adamblock_dev->queue);
	put_disk(adamblock_dev->disk);
	vfree(adamblock_dev->data);
	kfree(adamblock_dev);
}

static int __init adamblock_init(void)
{
	int rv = 0;

	adamblock_major = register_blkdev(0, "adamblock");
	if (adamblock_major < 0)
		return adamblock_major;

	rv = adamblock_alloc(ADAMBLOCK_MINOR);
	if (rv < 0)
		pr_info("adamblock: disk allocation failed with %d\n", rv);

	pr_info("adamblock: module loaded\n");
	return 0;
}

static void __exit adamblock_exit(void)
{
	adamblock_free(adamblock_dev);
	unregister_blkdev(adamblock_major, "adamblock");

	pr_info("adamblock: module unloaded\n");
}

module_init(adamblock_init);
module_exit(adamblock_exit);
MODULE_LICENSE("GPL");
