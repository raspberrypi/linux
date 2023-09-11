// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kbuild.h>
#include <nvhe/memory.h>
#include <nvhe/pkvm.h>
#include <nvhe/trace.h>

int main(void)
{
	DEFINE(STRUCT_HYP_PAGE_SIZE,	sizeof(struct hyp_page));
#ifdef CONFIG_TRACING
	DEFINE(STRUCT_HYP_BUFFER_PAGE_SIZE,	sizeof(struct hyp_buffer_page));
#endif
	return 0;
}
