#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

#include <asm/kvm_pkvm_module.h>

#ifndef MODULE
BUILD_BUG("pKVM pl011 UART must be compiled as a module");
#endif

int __kvm_nvhe_pl011_hyp_init(const struct pkvm_module_ops *ops);

static int __init pl011_nvhe_init(void)
{
	unsigned long token;
	int ret;

	ret = pkvm_load_el2_module(__kvm_nvhe_pl011_hyp_init, &token);
	if (ret)
		return ret;

	return 0;
}
module_init(pl011_nvhe_init);

MODULE_LICENSE("GPL");
