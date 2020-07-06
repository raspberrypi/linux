#ifndef _ASM_CLOCKSOURCE_H
#define _ASM_CLOCKSOURCE_H

enum vdso_arch_clockmode {
	/* vdso clocksource not usable */
	VDSO_CLOCKMODE_NONE,
	/* vdso clocksource usable */
	VDSO_CLOCKMODE_ARCHTIMER,
	VDSO_CLOCKMODE_ARCHTIMER_NOCOMPAT = VDSO_CLOCKMODE_ARCHTIMER,
};

struct arch_clocksource_data {
	/* Usable for direct VDSO access? */
	enum vdso_arch_clockmode clock_mode;
};

#endif
