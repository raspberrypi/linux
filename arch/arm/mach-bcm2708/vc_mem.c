/*****************************************************************************
* Copyright 2010 - 2011 Broadcom Corporation.  All rights reserved.
*
* Unless you and Broadcom execute a separate written software license
* agreement governing use of this software, this software is licensed to you
* under the terms of the GNU General Public License version 2, available at
* http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
*
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a
* license other than the GPL, without Broadcom's express prior written
* consent.
*****************************************************************************/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/dma-mapping.h>

#ifdef CONFIG_ARCH_KONA
#include <chal/chal_ipc.h>
#elif CONFIG_ARCH_BCM2708
#else
#include <csp/chal_ipc.h>
#endif

#include "mach/vc_mem.h"
#include <mach/vcio.h>

#define DRIVER_NAME  "vc-mem"

// Uncomment to enable debug logging
// #define ENABLE_DBG

#if defined(ENABLE_DBG)
#define LOG_DBG( fmt, ... )  printk( KERN_INFO fmt "\n", ##__VA_ARGS__ )
#else
#define LOG_DBG( fmt, ... )
#endif
#define LOG_ERR( fmt, ... )  printk( KERN_ERR fmt "\n", ##__VA_ARGS__ )

// Device (/dev) related variables
static dev_t vc_mem_devnum = 0;
static struct class *vc_mem_class = NULL;
static struct cdev vc_mem_cdev;
static int vc_mem_inited = 0;

// Proc entry
static struct proc_dir_entry *vc_mem_proc_entry;

/*
 * Videocore memory addresses and size
 *
 * Drivers that wish to know the videocore memory addresses and sizes should
 * use these variables instead of the MM_IO_BASE and MM_ADDR_IO defines in
 * headers. This allows the other drivers to not be tied down to a a certain
 * address/size at compile time.
 *
 * In the future, the goal is to have the videocore memory virtual address and
 * size be calculated at boot time rather than at compile time. The decision of
 * where the videocore memory resides and its size would be in the hands of the
 * bootloader (and/or kernel). When that happens, the values of these variables
 * would be calculated and assigned in the init function.
 */
// in the 2835 VC in mapped above ARM, but ARM has full access to VC space
unsigned long mm_vc_mem_phys_addr = 0x00000000;
unsigned int mm_vc_mem_size = 0;
unsigned int mm_vc_mem_base = 0;

EXPORT_SYMBOL(mm_vc_mem_phys_addr);
EXPORT_SYMBOL(mm_vc_mem_size);
EXPORT_SYMBOL(mm_vc_mem_base);

static uint phys_addr = 0;
static uint mem_size = 0;
static uint mem_base = 0;


/****************************************************************************
*
*   vc_mem_open
*
***************************************************************************/

static int
vc_mem_open(struct inode *inode, struct file *file)
{
	(void) inode;
	(void) file;

	LOG_DBG("%s: called file = 0x%p", __func__, file);

	return 0;
}

/****************************************************************************
*
*   vc_mem_release
*
***************************************************************************/

static int
vc_mem_release(struct inode *inode, struct file *file)
{
	(void) inode;
	(void) file;

	LOG_DBG("%s: called file = 0x%p", __func__, file);

	return 0;
}

/****************************************************************************
*
*   vc_mem_get_size
*
***************************************************************************/

static void
vc_mem_get_size(void)
{
}

/****************************************************************************
*
*   vc_mem_get_base
*
***************************************************************************/

static void
vc_mem_get_base(void)
{
}

/****************************************************************************
*
*   vc_mem_get_current_size
*
***************************************************************************/

int
vc_mem_get_current_size(void)
{
	return mm_vc_mem_size;
}

EXPORT_SYMBOL_GPL(vc_mem_get_current_size);

/****************************************************************************
*
*   vc_mem_ioctl
*
***************************************************************************/

static long
vc_mem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rc = 0;

	(void) cmd;
	(void) arg;

	LOG_DBG("%s: called file = 0x%p", __func__, file);

	switch (cmd) {
	case VC_MEM_IOC_MEM_PHYS_ADDR:
		{
			LOG_DBG("%s: VC_MEM_IOC_MEM_PHYS_ADDR=0x%p",
				__func__, (void *) mm_vc_mem_phys_addr);

			if (copy_to_user((void *) arg, &mm_vc_mem_phys_addr,
					 sizeof (mm_vc_mem_phys_addr)) != 0) {
				rc = -EFAULT;
			}
			break;
		}
	case VC_MEM_IOC_MEM_SIZE:
		{
			// Get the videocore memory size first
			vc_mem_get_size();

			LOG_DBG("%s: VC_MEM_IOC_MEM_SIZE=%u", __func__,
				mm_vc_mem_size);

			if (copy_to_user((void *) arg, &mm_vc_mem_size,
					 sizeof (mm_vc_mem_size)) != 0) {
				rc = -EFAULT;
			}
			break;
		}
	case VC_MEM_IOC_MEM_BASE:
		{
			// Get the videocore memory base
			vc_mem_get_base();

			LOG_DBG("%s: VC_MEM_IOC_MEM_BASE=%u", __func__,
				mm_vc_mem_base);

			if (copy_to_user((void *) arg, &mm_vc_mem_base,
					 sizeof (mm_vc_mem_base)) != 0) {
				rc = -EFAULT;
			}
			break;
		}
	default:
		{
			return -ENOTTY;
		}
	}
	LOG_DBG("%s: file = 0x%p returning %d", __func__, file, rc);

	return rc;
}

/****************************************************************************
*
*   vc_mem_mmap
*
***************************************************************************/

static int
vc_mem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int rc = 0;
	unsigned long length = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	LOG_DBG("%s: vm_start = 0x%08lx vm_end = 0x%08lx vm_pgoff = 0x%08lx",
		__func__, (long) vma->vm_start, (long) vma->vm_end,
		(long) vma->vm_pgoff);

	if (offset + length > mm_vc_mem_size) {
		LOG_ERR("%s: length %ld is too big", __func__, length);
		return -EINVAL;
	}
	// Do not cache the memory map
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	rc = remap_pfn_range(vma, vma->vm_start,
			     (mm_vc_mem_phys_addr >> PAGE_SHIFT) +
			     vma->vm_pgoff, length, vma->vm_page_prot);
	if (rc != 0) {
		LOG_ERR("%s: remap_pfn_range failed (rc=%d)", __func__, rc);
	}

	return rc;
}

/****************************************************************************
*
*   File Operations for the driver.
*
***************************************************************************/

static const struct file_operations vc_mem_fops = {
	.owner = THIS_MODULE,
	.open = vc_mem_open,
	.release = vc_mem_release,
	.unlocked_ioctl = vc_mem_ioctl,
	.mmap = vc_mem_mmap,
};

/****************************************************************************
*
*   vc_mem_proc_read
*
***************************************************************************/

static int
vc_mem_proc_read(char *buf, char **start, off_t offset, int count, int *eof,
		 void *data)
{
	char *p = buf;

	(void) start;
	(void) count;
	(void) data;

	if (offset > 0) {
		*eof = 1;
		return 0;
	}
	// Get the videocore memory size first
	vc_mem_get_size();

	p += sprintf(p, "Videocore memory:\n");
	if (mm_vc_mem_phys_addr != 0)
		p += sprintf(p, "   Physical address: 0x%p\n",
			     (void *) mm_vc_mem_phys_addr);
	else
		p += sprintf(p, "   Physical address: 0x00000000\n");
	p += sprintf(p, "   Length (bytes):   %u\n", mm_vc_mem_size);

	*eof = 1;
	return p - buf;
}

/****************************************************************************
*
*   vc_mem_proc_write
*
***************************************************************************/

static int
vc_mem_proc_write(struct file *file, const char __user * buffer,
		  unsigned long count, void *data)
{
	int rc = -EFAULT;
	char input_str[10];

	memset(input_str, 0, sizeof (input_str));

	if (count > sizeof (input_str)) {
		LOG_ERR("%s: input string length too long", __func__);
		goto out;
	}

	if (copy_from_user(input_str, buffer, count - 1)) {
		LOG_ERR("%s: failed to get input string", __func__);
		goto out;
	}

	if (strncmp(input_str, "connect", strlen("connect")) == 0) {
		// Get the videocore memory size from the videocore
		vc_mem_get_size();
	}

      out:
	return rc;
}

/****************************************************************************
*
*   vc_mem_init
*
***************************************************************************/

static int __init
vc_mem_init(void)
{
	int rc = -EFAULT;
	struct device *dev;

	LOG_DBG("%s: called", __func__);

	mm_vc_mem_phys_addr = phys_addr;
	mm_vc_mem_size = mem_size;
	mm_vc_mem_base = mem_base;

	vc_mem_get_size();

	printk("vc-mem: phys_addr:0x%08lx mem_base=0x%08x mem_size:0x%08x(%u MiB)\n",
		mm_vc_mem_phys_addr, mm_vc_mem_base, mm_vc_mem_size, mm_vc_mem_size / (1024 * 1024));

	if ((rc = alloc_chrdev_region(&vc_mem_devnum, 0, 1, DRIVER_NAME)) < 0) {
		LOG_ERR("%s: alloc_chrdev_region failed (rc=%d)", __func__, rc);
		goto out_err;
	}

	cdev_init(&vc_mem_cdev, &vc_mem_fops);
	if ((rc = cdev_add(&vc_mem_cdev, vc_mem_devnum, 1)) != 0) {
		LOG_ERR("%s: cdev_add failed (rc=%d)", __func__, rc);
		goto out_unregister;
	}

	vc_mem_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(vc_mem_class)) {
		rc = PTR_ERR(vc_mem_class);
		LOG_ERR("%s: class_create failed (rc=%d)", __func__, rc);
		goto out_cdev_del;
	}

	dev = device_create(vc_mem_class, NULL, vc_mem_devnum, NULL,
			    DRIVER_NAME);
	if (IS_ERR(dev)) {
		rc = PTR_ERR(dev);
		LOG_ERR("%s: device_create failed (rc=%d)", __func__, rc);
		goto out_class_destroy;
	}

#if 0
	vc_mem_proc_entry = create_proc_entry(DRIVER_NAME, 0444, NULL);
	if (vc_mem_proc_entry == NULL) {
		rc = -EFAULT;
		LOG_ERR("%s: create_proc_entry failed", __func__);
		goto out_device_destroy;
	}
	vc_mem_proc_entry->read_proc = vc_mem_proc_read;
	vc_mem_proc_entry->write_proc = vc_mem_proc_write;
#endif

	vc_mem_inited = 1;
	return 0;

      out_device_destroy:
	device_destroy(vc_mem_class, vc_mem_devnum);

      out_class_destroy:
	class_destroy(vc_mem_class);
	vc_mem_class = NULL;

      out_cdev_del:
	cdev_del(&vc_mem_cdev);

      out_unregister:
	unregister_chrdev_region(vc_mem_devnum, 1);

      out_err:
	return -1;
}

/****************************************************************************
*
*   vc_mem_exit
*
***************************************************************************/

static void __exit
vc_mem_exit(void)
{
	LOG_DBG("%s: called", __func__);

	if (vc_mem_inited) {
#if 0
		remove_proc_entry(vc_mem_proc_entry->name, NULL);
#endif
		device_destroy(vc_mem_class, vc_mem_devnum);
		class_destroy(vc_mem_class);
		cdev_del(&vc_mem_cdev);
		unregister_chrdev_region(vc_mem_devnum, 1);
	}
}

module_init(vc_mem_init);
module_exit(vc_mem_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Broadcom Corporation");

module_param(phys_addr, uint, 0644);
module_param(mem_size, uint, 0644);
module_param(mem_base, uint, 0644);

