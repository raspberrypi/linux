/*
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
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/broadcom/vc_mem.h>
#include <linux/platform_device.h>

#define DRIVER_NAME  "vc-mem"

struct vc_mem {
	struct miscdevice misc;

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
	/* In the 2835 VC in mapped above ARM, but ARM has full access to VC space */
	unsigned long phys_addr;
	u32 base;
	u32 size;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_entry;
#endif
};

unsigned long mm_vc_mem_phys_addr;
EXPORT_SYMBOL(mm_vc_mem_phys_addr);
unsigned int mm_vc_mem_size;
EXPORT_SYMBOL(mm_vc_mem_size);

static int
vc_mem_open(struct inode *inode, struct file *file)
{
	(void)inode;

	pr_debug("%s: called file = 0x%p\n", __func__, file);

	file->private_data = container_of(file->private_data, struct vc_mem, misc);

	return 0;
}

static int
vc_mem_release(struct inode *inode, struct file *file)
{
	(void)inode;

	pr_debug("%s: called file = 0x%p\n", __func__, file);

	return 0;
}

static long
vc_mem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	struct vc_mem *drv = file->private_data;

	pr_debug("%s: called file = 0x%p, cmd %08x\n", __func__, file, cmd);

	switch (cmd) {
	case VC_MEM_IOC_MEM_PHYS_ADDR:
		pr_debug("%s: VC_MEM_IOC_MEM_PHYS_ADDR=%lx\n",
			 __func__, drv->phys_addr);

		if (copy_to_user((void __user *)arg, &drv->phys_addr, sizeof(drv->phys_addr)))
			rc = -EFAULT;
		break;

	case VC_MEM_IOC_MEM_SIZE:
		pr_debug("%s: VC_MEM_IOC_MEM_SIZE=%x\n",
			 __func__, drv->size);

		if (copy_to_user((void __user *)arg, &drv->size, sizeof(drv->size)))
			rc = -EFAULT;
		break;

	case VC_MEM_IOC_MEM_BASE:
	case VC_MEM_IOC_MEM_LOAD:
		pr_debug("%s: VC_MEM_IOC_MEM_BASE=%x\n",
			 __func__, drv->base);

		if (copy_to_user((void __user *)arg, &drv->base, sizeof(drv->base)))
			rc = -EFAULT;
		break;

	default:
		rc = -ENOTTY;
	}
	pr_debug("%s: file = 0x%p returning %d\n", __func__, file, rc);

	return rc;
}

#ifdef CONFIG_COMPAT
static long
vc_mem_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	struct vc_mem *drv = file->private_data;

	switch (cmd) {
	case VC_MEM_IOC_MEM_PHYS_ADDR32:
		{
			/* This isn't correct, but will cover us for now as
			 * VideoCore is 32bit only.
			 */
			compat_ulong_t phys_addr = drv->phys_addr;

			pr_debug("%s: VC_MEM_IOC_MEM_PHYS_ADDR32=0x%p\n",
				 __func__, (unsigned int)phys_addr);

			if (copy_to_user((void __user *)arg, &phys_addr, sizeof(phys_addr)))
				rc = -EFAULT;
			break;
		}

	default:
		rc = vc_mem_ioctl(file, cmd, arg);
		break;
	}

	return rc;
}
#endif

static int
vc_mem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int rc = 0;
	struct vc_mem *drv = filp->private_data;
	unsigned long length = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	pr_debug("%s: vm_start = 0x%08lx vm_end = 0x%08lx vm_pgoff = 0x%08lx\n",
		 __func__, (long)vma->vm_start, (long)vma->vm_end,
		 (long)vma->vm_pgoff);

	if (offset > drv->size || length > drv->size - offset) {
		pr_err("%s: length %ld is too big\n", __func__, length);
		return -EINVAL;
	}
	/* Do not cache the memory map */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	rc = remap_pfn_range(vma, vma->vm_start,
			     (drv->phys_addr >> PAGE_SHIFT) + vma->vm_pgoff,
			     length, vma->vm_page_prot);
	if (rc)
		pr_err("%s: remap_pfn_range failed (rc=%d)\n", __func__, rc);

	return rc;
}

/* File Operations for the driver. */
static const struct file_operations vc_mem_fops = {
	.owner = THIS_MODULE,
	.open = vc_mem_open,
	.release = vc_mem_release,
	.unlocked_ioctl = vc_mem_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = vc_mem_compat_ioctl,
#endif
	.mmap = vc_mem_mmap,
};

#ifdef CONFIG_DEBUG_FS
static int vc_mem_ulong_get(void *data, u64 *val)
{
	*val = *(unsigned long *)data;
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(vc_mem_phys_addr_fops, vc_mem_ulong_get, NULL, "0x%08llx\n");
#endif /* CONFIG_DEBUG_FS */

static int vc_mem_probe(struct platform_device *pdev)
{
	int ret;
	struct vc_mem *drv;
	const __be32 *addrp;
	int n_addr_bytes;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;
	platform_set_drvdata(pdev, drv);

	// Note that the "reg" property provided by firmware is does not
	// follow any conventions. It is just a bunch of 32-bit addresses.
	addrp = of_get_property(pdev->dev.of_node, "reg", &n_addr_bytes);
	if (!addrp || n_addr_bytes != 12)
		return -ENODEV;
	drv->base = be32_to_cpu(addrp[0]);
	drv->size = be32_to_cpu(addrp[1]);

	drv->phys_addr = 0; // TODO: translate base using ranges, subtract base?

	drv->misc.minor = MISC_DYNAMIC_MINOR;
	drv->misc.name = DRIVER_NAME;
	drv->misc.fops = &vc_mem_fops;
	drv->misc.parent = &pdev->dev;
	ret = misc_register(&drv->misc);
	if (ret < 0) {
		dev_err(&pdev->dev, "misc_register: %d\n", ret);
		return ret;
	}

#ifdef CONFIG_DEBUG_FS
	drv->debugfs_entry = debugfs_create_dir(DRIVER_NAME, NULL);

	debugfs_create_file_unsafe("vc_mem_phys_addr",
				   0444,
				   drv->debugfs_entry,
				   &drv->phys_addr,
				   &vc_mem_phys_addr_fops);
	debugfs_create_x32("vc_mem_size",
			   0444,
			   drv->debugfs_entry,
			   &drv->size);
	debugfs_create_x32("vc_mem_base",
			   0444,
			   drv->debugfs_entry,
			   &drv->base);
#endif

	mm_vc_mem_phys_addr = drv->phys_addr;
	mm_vc_mem_size = drv->size;

	dev_info(&pdev->dev, "phys_addr:0x%08lx mem_base=0x%08x mem_size:0x%08x(%u MiB)\n",
		 drv->phys_addr, drv->base, drv->size, drv->size / (1024 * 1024));

	return 0;
}

static int vc_mem_remove(struct platform_device *pdev)
{
	struct vc_mem *drv;

	drv = platform_get_drvdata(pdev);
	if (!drv)
		return 0;

#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(drv->debugfs_entry);
#endif

	misc_deregister(&drv->misc);

	return 0;
}

static const struct of_device_id vc_mem_of_match[] = {
	{ .compatible = "raspberrypi,vc-mem", },
	{},
};
MODULE_DEVICE_TABLE(of, rpi_firmware_of_match);

static struct platform_driver vc_mem_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = vc_mem_of_match,
	},
	.probe		= vc_mem_probe,
	.remove		= vc_mem_remove,
};

static int __init vc_mem_init(void)
{
	return platform_driver_register(&vc_mem_driver);
}

static void __exit vc_mem_exit(void)
{
	platform_driver_unregister(&vc_mem_driver);
}

module_init(vc_mem_init);
module_exit(vc_mem_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Broadcom Corporation");
