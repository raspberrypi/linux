/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_VDSOCLOCKSOURCE_H
#define __ASM_VDSOCLOCKSOURCE_H

enum vdso_arch_clockmode {
	/* vdso clocksource not usable */
	VDSO_CLOCKMODE_NONE,
	/* vdso clocksource for both 32 and 64bit tasks */
	VDSO_CLOCKMODE_ARCHTIMER,
	/* vdso clocksource for 64bit tasks only */
	VDSO_CLOCKMODE_ARCHTIMER_NOCOMPAT,
};

#endif
