/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_CLOCKSOURCE_H
#define _ASM_CLOCKSOURCE_H

#include <asm/vdso/clocksource.h>

struct arch_clocksource_data {
	/* Usable for direct VDSO access? */
	enum vdso_arch_clockmode clock_mode;
};

#endif
