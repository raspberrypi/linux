/*****************************************************************************
* Copyright 2012 Broadcom Corporation.  All rights reserved.
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

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mm.h>

#include <vc_lmk.h>

#define DRIVER_NAME  "vc-lmk"

/* Uncomment to enable debug logging */
/* #define ENABLE_DBG */

#if defined(ENABLE_DBG)
#define LOG_DBG(fmt, ...)  printk(KERN_INFO fmt "\n", ##__VA_ARGS__)
#else
#define LOG_DBG(fmt, ...)
#endif
#define LOG_ERR(fmt, ...)  printk(KERN_ERR fmt "\n", ##__VA_ARGS__)

typedef struct {
	struct list_head lmk_list;
	pid_t pid;

} LMK_PRIV_DATA_T;

/* Device (/dev) related variables */
static dev_t vc_lmk_devnum;
static struct class *vc_lmk_class;
static struct cdev vc_lmk_cdev;
/* Proc entry */
static struct proc_dir_entry *vc_lmk_proc_entry;

static LMK_PRIV_DATA_T vc_lmk_data;
static struct mutex vc_lmk_lock;
static int vc_lmk_killed_proc;

/****************************************************************************
*
*   vc_lmk_open
*
***************************************************************************/

static int vc_lmk_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	LMK_PRIV_DATA_T *lmk_data = NULL;

	(void)inode;

	lmk_data = kzalloc(sizeof(LMK_PRIV_DATA_T), GFP_KERNEL);
	if (lmk_data == NULL) {
		LOG_ERR("[%s]: failed to create data tracker", __func__);

		ret = -ENOMEM;
	} else {
		lmk_data->pid = current->tgid;
		file->private_data = lmk_data;

		mutex_lock(&vc_lmk_lock);
		list_add(&(lmk_data->lmk_list), &(vc_lmk_data.lmk_list));
		mutex_unlock(&vc_lmk_lock);

		LOG_DBG("[%s]: adding lmk tracker for pid %d",
			__func__, current->tgid);
	}

	return ret;
}

/****************************************************************************
*
*   vc_lmk_release
*
***************************************************************************/

static int vc_lmk_release(struct inode *inode, struct file *file)
{
	LMK_PRIV_DATA_T *lmk_data, *lmk_data_tmp;
	LMK_PRIV_DATA_T *priv_data = (LMK_PRIV_DATA_T *) file->private_data;

	(void)inode;
	(void)file;

	mutex_lock(&vc_lmk_lock);
	list_for_each_entry_safe(lmk_data, lmk_data_tmp,
				 &(vc_lmk_data.lmk_list), lmk_list) {
		if (lmk_data->pid != priv_data->pid)
			continue;

		LOG_DBG("[%s]: removing lmk tracker for pid %d (%d)",
			__func__, priv_data->pid, current->tgid);

		priv_data = NULL;
		list_del(&(lmk_data->lmk_list));
		kfree(lmk_data);
		break;
	}
	mutex_unlock(&vc_lmk_lock);

	return 0;
}

/****************************************************************************
*
*   vc_lmk_ioctl
*
***************************************************************************/

static long vc_lmk_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	struct vclmk_ioctl_killpid vc_lmk_kill_pid;
	struct vclmk_ioctl_lmk_candidate vc_lmk_candidate;
	struct vclmk_ioctl_lmk_hmem vc_lmk_hmem;
	struct task_struct *process;
	struct task_struct *found = NULL;
	LMK_PRIV_DATA_T *lmk_data, *lmk_data_found = NULL;

	switch (cmd) {
	case VC_LMK_IOC_CAND_PID:
		{
			if (copy_from_user(&vc_lmk_candidate,
					   (void *)arg, _IOC_SIZE(cmd)) != 0) {
				rc = -EFAULT;
			}

			mutex_lock(&vc_lmk_lock);
			list_for_each_entry(lmk_data, &(vc_lmk_data.lmk_list),
					    lmk_list) {
				if (lmk_data->pid !=
				    (pid_t) vc_lmk_candidate.pid)
					continue;

				lmk_data_found = lmk_data;
				break;
			}
			mutex_unlock(&vc_lmk_lock);

			if (lmk_data_found) {
				/* Skip if we are the caller process.
				 */
				if (vc_lmk_candidate.pid != current->tgid) {
					vc_lmk_candidate.candidate = 1;

					LOG_DBG
					    ("[%s]: found lmk tracker for pid %d",
					     __func__, vc_lmk_candidate.pid);
				}
			} else {
				vc_lmk_candidate.candidate = 0;

				LOG_DBG("[%s]: pid %d is NOT lmk material...",
					__func__, vc_lmk_candidate.pid);
			}

			if (!rc &&
			    copy_to_user((void *)arg,
					 &vc_lmk_candidate,
					 _IOC_SIZE(cmd)) != 0) {
				rc = -EFAULT;
			}
			break;
		}

	case VC_LMK_IOC_KILL_PID:
		{
			if (copy_from_user(&vc_lmk_kill_pid,
					   (void *)arg, _IOC_SIZE(cmd)) != 0) {
				rc = -EFAULT;
			}

			read_lock(&tasklist_lock);
			for_each_process(process) {
				struct signal_struct *sig;

				task_lock(process);
				sig = process->signal;
				if (!sig) {
					task_unlock(process);
					continue;
				}

				task_unlock(process);

				if (process->pid == vc_lmk_kill_pid.pid)
					found = process;
			}

			if (found != NULL) {
				force_sig(SIGKILL, found);
				vc_lmk_killed_proc++;
			}
			read_unlock(&tasklist_lock);
			break;
		}

	case VC_LMK_IOC_HMEM_PID:
		{
			int num_pages = 0;

			if (copy_from_user(&vc_lmk_hmem,
					   (void *)arg, _IOC_SIZE(cmd)) != 0) {
				rc = -EFAULT;
			}

			read_lock(&tasklist_lock);
			for_each_process(process) {
				if (process->pid == vc_lmk_hmem.pid)
					found = process;
			}

			if (found != NULL) {
				if (found->mm)
					num_pages = get_mm_rss(found->mm);
			}
			read_unlock(&tasklist_lock);

			vc_lmk_hmem.num_pages = num_pages << (PAGE_SHIFT - 10);
			vc_lmk_hmem.page_size = 1024;

			if (!rc &&
			    copy_to_user((void *)arg,
					 &vc_lmk_hmem, _IOC_SIZE(cmd)) != 0) {
				rc = -EFAULT;
			}
			break;
		}

	default:
		{
			return -ENOTTY;
		}
	}
	LOG_DBG("[%s]: file = 0x%p returning %d", __func__, file, rc);

	return rc;
}

/****************************************************************************
*
*   File Operations for the driver.
*
***************************************************************************/

static const struct file_operations vc_lmk_fops = {
	.owner = THIS_MODULE,
	.open = vc_lmk_open,
	.release = vc_lmk_release,
	.unlocked_ioctl = vc_lmk_ioctl,
};

/****************************************************************************
*
*   vc_lmk_proc_read
*
***************************************************************************/

static int vc_lmk_proc_read(char *buf, char **start, off_t offset, int count,
			    int *eof, void *data)
{
	char *p = buf;

	(void)start;
	(void)count;
	(void)data;

	if (offset > 0) {
		*eof = 1;
		return 0;
	}

	p += sprintf(p, "Killed %d processes so far...\n\n",
		     vc_lmk_killed_proc);

	*eof = 1;
	return p - buf;
}

/****************************************************************************
*
*   vc_lmk_init
*
***************************************************************************/

static int __init vc_lmk_init(void)
{
	int rc;
	struct device *dev;

	LOG_DBG("[%s]: called", __func__);

	rc = alloc_chrdev_region(&vc_lmk_devnum, 0, 1, DRIVER_NAME);
	if (rc < 0) {
		LOG_ERR("[%s]: alloc_chrdev_region failed (rc=%d)", __func__,
			rc);
		goto out_err;
	}

	cdev_init(&vc_lmk_cdev, &vc_lmk_fops);
	rc = cdev_add(&vc_lmk_cdev, vc_lmk_devnum, 1);
	if (rc != 0) {
		LOG_ERR("[%s]: cdev_add failed (rc=%d)", __func__, rc);
		goto out_unregister;
	}

	vc_lmk_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(vc_lmk_class)) {
		rc = PTR_ERR(vc_lmk_class);
		LOG_ERR("[%s]: class_create failed (rc=%d)", __func__, rc);
		goto out_cdev_del;
	}

	dev = device_create(vc_lmk_class, NULL, vc_lmk_devnum, NULL,
			    DRIVER_NAME);
	if (IS_ERR(dev)) {
		rc = PTR_ERR(dev);
		LOG_ERR("[%s]: device_create failed (rc=%d)", __func__, rc);
		goto out_class_destroy;
	}

	vc_lmk_proc_entry = create_proc_entry(DRIVER_NAME, 0660, NULL);
	if (vc_lmk_proc_entry == NULL) {
		rc = -EFAULT;
		LOG_ERR("[%s]: create_proc_entry failed", __func__);
		goto out_device_destroy;
	}
	vc_lmk_proc_entry->read_proc = vc_lmk_proc_read;
	vc_lmk_proc_entry->write_proc = NULL;

	INIT_LIST_HEAD(&vc_lmk_data.lmk_list);
	mutex_init(&vc_lmk_lock);

	return 0;

out_device_destroy:
	device_destroy(vc_lmk_class, vc_lmk_devnum);

out_class_destroy:
	class_destroy(vc_lmk_class);
	vc_lmk_class = NULL;

out_cdev_del:
	cdev_del(&vc_lmk_cdev);

out_unregister:
	unregister_chrdev_region(vc_lmk_devnum, 1);

out_err:
	return rc;
}

/****************************************************************************
*
*   vc_lmk_exit
*
***************************************************************************/

static void __exit vc_lmk_exit(void)
{
	LOG_DBG("[%s]: called", __func__);

	remove_proc_entry(vc_lmk_proc_entry->name, NULL);
	device_destroy(vc_lmk_class, vc_lmk_devnum);
	class_destroy(vc_lmk_class);
	cdev_del(&vc_lmk_cdev);
	unregister_chrdev_region(vc_lmk_devnum, 1);

	mutex_destroy(&vc_lmk_lock);
}

module_init(vc_lmk_init);
module_exit(vc_lmk_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Broadcom Corporation");
