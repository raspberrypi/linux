#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/init.h>
#include <linux/sysctl.h>
#include <linux/usb.h>

static struct ctl_table usb_sysctls[] = {
	{
		.procname	= "deny_new_usb",
		.data		= &deny_new_usb,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax_sysadmin,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
};

static struct ctl_table_header *usb_sysctl_table;

int usb_register_sysctl(void)
{
	usb_sysctl_table = register_sysctl("kernel", usb_sysctls);
	if (!usb_sysctl_table) {
		pr_warn("usb: sysctl registration failed\n");
		return -ENOMEM;
	}
	return 0;
}

void usb_unregister_sysctl(void)
{
	unregister_sysctl_table(usb_sysctl_table);
	usb_sysctl_table = NULL;
}
