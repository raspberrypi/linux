#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bill Landolina");
MODULE_DESCRIPTION("CAN driver for Nexus");
MODULE_VERSION("0.1");

static int __init nexus_can_init(void)
{
    printk(KERN_ALERT "Hello world, this is the Nexus CAN driver\n");
    return 0;
}

static void __exit nexus_can_exit(void)
{
    printk(KERN_ALERT "Goodbye cruel world.  The Nexus CAN driver has left the building.\n");
}

module_init(nexus_can_init);
module_exit(nexus_can_exit);
