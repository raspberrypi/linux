// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kbuild.h>
#include <nvhe/memory.h>
#include <nvhe/pkvm.h>
#include <nvhe/trace.h>

int main(void)
{
	DEFINE(STRUCT_HYP_PAGE_SIZE,	sizeof(struct hyp_page));
	DEFINE(HYP_SPINLOCK_SIZE,       sizeof(hyp_spinlock_t));
	return 0;
}
