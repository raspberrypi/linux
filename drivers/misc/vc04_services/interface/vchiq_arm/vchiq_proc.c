/*
 * Copyright (c) 2012 Broadcom Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <linux/proc_fs.h>
#include "vchiq_core.h"
#include "vchiq_arm.h"

struct vchiq_proc_info {
	/* Global 'vc' proc entry used by all instances */
	struct proc_dir_entry *vc_cfg_dir;

	/* one entry per client process */
	struct proc_dir_entry *clients;

	/* log categories */
	struct proc_dir_entry *log_categories;
};

static struct vchiq_proc_info proc_info;

struct proc_dir_entry *vchiq_proc_top(void)
{
	BUG_ON(proc_info.vc_cfg_dir == NULL);
	return proc_info.vc_cfg_dir;
}

/****************************************************************************
*
*   log category entries
*
***************************************************************************/
#define PROC_WRITE_BUF_SIZE 256

#define VCHIQ_LOG_ERROR_STR   "error"
#define VCHIQ_LOG_WARNING_STR "warning"
#define VCHIQ_LOG_INFO_STR    "info"
#define VCHIQ_LOG_TRACE_STR   "trace"

static int log_cfg_read(char *buffer,
	char **start,
	off_t off,
	int count,
	int *eof,
	void *data)
{
	int len = 0;
	char *log_value = NULL;

	switch (*((int *)data)) {
	case VCHIQ_LOG_ERROR:
		log_value = VCHIQ_LOG_ERROR_STR;
		break;
	case VCHIQ_LOG_WARNING:
		log_value = VCHIQ_LOG_WARNING_STR;
		break;
	case VCHIQ_LOG_INFO:
		log_value = VCHIQ_LOG_INFO_STR;
		break;
	case VCHIQ_LOG_TRACE:
		log_value = VCHIQ_LOG_TRACE_STR;
		break;
	default:
		break;
	}

	len += sprintf(buffer + len,
		"%s\n",
		log_value ? log_value : "(null)");

	return len;
}


static int log_cfg_write(struct file *file,
	const char __user *buffer,
	unsigned long count,
	void *data)
{
	int *log_module = data;
	char kbuf[PROC_WRITE_BUF_SIZE + 1];

	(void)file;

	memset(kbuf, 0, PROC_WRITE_BUF_SIZE + 1);
	if (count >= PROC_WRITE_BUF_SIZE)
		count = PROC_WRITE_BUF_SIZE;

	if (copy_from_user(kbuf,
		buffer,
		count) != 0)
		return -EFAULT;
	kbuf[count - 1] = 0;

	if (strncmp("error", kbuf, strlen("error")) == 0)
		*log_module = VCHIQ_LOG_ERROR;
	else if (strncmp("warning", kbuf, strlen("warning")) == 0)
		*log_module = VCHIQ_LOG_WARNING;
	else if (strncmp("info", kbuf, strlen("info")) == 0)
		*log_module = VCHIQ_LOG_INFO;
	else if (strncmp("trace", kbuf, strlen("trace")) == 0)
		*log_module = VCHIQ_LOG_TRACE;
	else
		*log_module = VCHIQ_LOG_DEFAULT;

	return count;
}

/* Log category proc entries */
struct vchiq_proc_log_entry {
	const char *name;
	int *plevel;
	struct proc_dir_entry *dir;
};

static struct vchiq_proc_log_entry vchiq_proc_log_entries[] = {
	{ "core", &vchiq_core_log_level },
	{ "msg",  &vchiq_core_msg_log_level },
	{ "sync", &vchiq_sync_log_level },
	{ "susp", &vchiq_susp_log_level },
	{ "arm",  &vchiq_arm_log_level },
};
static int n_log_entries =
	sizeof(vchiq_proc_log_entries)/sizeof(vchiq_proc_log_entries[0]);

/* create an entry under /proc/vc/log for each log category */
static int vchiq_proc_create_log_entries(struct proc_dir_entry *top)
{
	struct proc_dir_entry *dir;
	size_t i;
	int ret = 0;

	dir = proc_mkdir("log", proc_info.vc_cfg_dir);
	if (!dir)
		return -ENOMEM;
	proc_info.log_categories = dir;

	for (i = 0; i < n_log_entries; i++) {
		dir = create_proc_entry(vchiq_proc_log_entries[i].name,
					0644,
					proc_info.log_categories);
		if (!dir) {
			ret = -ENOMEM;
			break;
		}

		dir->read_proc = &log_cfg_read;
		dir->write_proc = &log_cfg_write;
		dir->data = (void *)vchiq_proc_log_entries[i].plevel;

		vchiq_proc_log_entries[i].dir = dir;
	}
	return ret;
}


int vchiq_proc_init(void)
{
	BUG_ON(proc_info.vc_cfg_dir != NULL);

	proc_info.vc_cfg_dir = proc_mkdir("vc", NULL);
	if (proc_info.vc_cfg_dir == NULL)
		goto fail;

	proc_info.clients = proc_mkdir("clients",
				proc_info.vc_cfg_dir);
	if (!proc_info.clients)
		goto fail;

	if (vchiq_proc_create_log_entries(proc_info.vc_cfg_dir) != 0)
		goto fail;

	return 0;

fail:
	vchiq_proc_deinit();
	vchiq_log_error(vchiq_arm_log_level,
		"%s: failed to create proc directory",
		__func__);

	return -ENOMEM;
}

/* remove all the proc entries */
void vchiq_proc_deinit(void)
{
	/* log category entries */
	if (proc_info.log_categories) {
		size_t i;
		for (i = 0; i < n_log_entries; i++)
			if (vchiq_proc_log_entries[i].dir)
				remove_proc_entry(
					vchiq_proc_log_entries[i].name,
					proc_info.log_categories);

		remove_proc_entry(proc_info.log_categories->name,
				  proc_info.vc_cfg_dir);
	}
	if (proc_info.clients)
		remove_proc_entry(proc_info.clients->name,
				  proc_info.vc_cfg_dir);
	if (proc_info.vc_cfg_dir)
		remove_proc_entry(proc_info.vc_cfg_dir->name, NULL);
}

struct proc_dir_entry *vchiq_clients_top(void)
{
	return proc_info.clients;
}

