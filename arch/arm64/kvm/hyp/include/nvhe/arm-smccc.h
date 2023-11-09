/* SPDX-License-Identifier: GPL-2.0-only */

#include <asm/kvm_hypevents.h>

#include <linux/arm-smccc.h>

#undef arm_smccc_1_1_smc
#define arm_smccc_1_1_smc(...)					\
	do {							\
		__hyp_exit();					\
		__arm_smccc_1_1(SMCCC_SMC_INST, __VA_ARGS__);	\
		__hyp_enter();					\
	} while (0)
