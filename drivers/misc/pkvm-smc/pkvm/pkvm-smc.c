// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 - Google Inc
 * Author: Mostafa Saleh <smostafa@google.com>
 * Simple module for pKVM SMC filtering.
 */

#include <asm/kvm_pkvm_module.h>

int pkvm_smc_filter_hyp_init(const struct pkvm_module_ops *ops)
{
	return 0;
}
