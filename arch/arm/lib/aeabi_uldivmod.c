/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Provide __aeabi_uldivmod for ARM EABI when the compiler emits it.
 * Use the kernel's do_div helper so modules and built-ins have a definition.
 */

#include <linux/types.h>
#include <linux/export.h>
#include <asm/div64.h>

u64 __aeabi_uldivmod(u64 dividend, u32 divisor)
{
	/* do_div updates dividend to the quotient and returns the remainder. */
	(void)do_div(dividend, divisor);
	return dividend;
}
EXPORT_SYMBOL(__aeabi_uldivmod);


