// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 - Google Inc
 * Author: Mostafa Saleh <smostafa@google.com>
 * Simple module for pKVM SMC filtering.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/kvm_pkvm_module.h>

#define HYP_EVENT_FILE ../../../../drivers/misc/pkvm-smc/pkvm/events.h
#include <asm/kvm_define_hypevents.h>

static unsigned long pkvm_module_token;
int kvm_nvhe_sym(pkvm_smc_filter_hyp_init)(const struct pkvm_module_ops *ops);
extern int kvm_nvhe_sym(permissive);

static bool permissive;
module_param(permissive, bool, 0444);
MODULE_PARM_DESC(permissive, "Only log SMC filter violations.");

static int __init smc_filter_init(void)
{
	int ret;

	kvm_nvhe_sym(permissive) = permissive;
	ret = pkvm_load_el2_module(kvm_nvhe_sym(pkvm_smc_filter_hyp_init),
				   &pkvm_module_token);
	if (ret)
		pr_err("Failed to register pKVM SMC filter: %d\n", ret);
	else
		pr_info("pKVM SMC filter registered successfully with permissive = %s\n",
			permissive ? "true" : "false");

	return ret;
}

module_init(smc_filter_init);

MODULE_AUTHOR("Mostafa Saleh <smostafa@google.com>");
MODULE_DESCRIPTION("pKVM SMC filter");
MODULE_LICENSE("GPL v2");
