/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARCH_S390_PERCPU__
#define __ARCH_S390_PERCPU__

#include <linux/preempt.h>
#include <asm/cmpxchg.h>
#include <asm/march.h>

/*
 * s390 uses its own implementation for per cpu data, the offset of
 * the cpu local data area is cached in the cpu's lowcore memory.
 */
#define __my_cpu_offset get_lowcore()->percpu_offset

#define arch_raw_cpu_ptr(_ptr)						\
({									\
	unsigned long tcp_ptr__;					\
									\
	tcp_ptr__ = (__force unsigned long)(_ptr);			\
	asm_inline volatile(						\
	ALTERNATIVE("ag		%[__ptr__],%[offzero](%%r0)\n",		\
		    "ag		%[__ptr__],%[offalt](%%r0)\n",		\
		    ALT_FEATURE(MFEATURE_LOWCORE))			\
	: [__ptr__] "+d" (tcp_ptr__)					\
	: [offzero] "i" (LC_PERCPU_OFFSET),				\
	  [offalt] "i" (LC_PERCPU_OFFSET + LOWCORE_ALT_ADDRESS),	\
	  "m" (((struct lowcore *)0)->percpu_offset)			\
	: "cc");							\
	(TYPEOF_UNQUAL(*(_ptr)) __force __kernel *)tcp_ptr__;		\
})

/*
 * We use a compare-and-swap loop since that uses less cpu cycles than
 * disabling and enabling interrupts like the generic variant would do.
 */
#define arch_this_cpu_to_op_simple(pcp, val, op)			\
({									\
	typedef typeof(pcp) pcp_op_T__;					\
	pcp_op_T__ old__, new__, prev__;				\
	pcp_op_T__ *ptr__;						\
	preempt_disable_notrace();					\
	ptr__ = raw_cpu_ptr(&(pcp));					\
	prev__ = READ_ONCE(*ptr__);					\
	do {								\
		old__ = prev__;						\
		new__ = old__ op (val);					\
		prev__ = cmpxchg(ptr__, old__, new__);			\
	} while (prev__ != old__);					\
	preempt_enable_notrace();					\
	new__;								\
})

#define this_cpu_add_1(pcp, val)	arch_this_cpu_to_op_simple(pcp, val, +)
#define this_cpu_add_2(pcp, val)	arch_this_cpu_to_op_simple(pcp, val, +)
#define this_cpu_add_return_1(pcp, val) arch_this_cpu_to_op_simple(pcp, val, +)
#define this_cpu_add_return_2(pcp, val) arch_this_cpu_to_op_simple(pcp, val, +)
#define this_cpu_and_1(pcp, val)	arch_this_cpu_to_op_simple(pcp, val, &)
#define this_cpu_and_2(pcp, val)	arch_this_cpu_to_op_simple(pcp, val, &)
#define this_cpu_or_1(pcp, val)		arch_this_cpu_to_op_simple(pcp, val, |)
#define this_cpu_or_2(pcp, val)		arch_this_cpu_to_op_simple(pcp, val, |)

/*
 * Macros to be used for percpu code section based on atomic instructions.
 *
 * Avoid the need to use preempt_disable() / preempt_disable() pairs and the
 * conditional preempt_schedule_notrace() function calls which come with
 * this. The idea is that this_cpu operations based on atomic instructions are
 * guarded with mviy instructions:
 *
 * - The first mviy instruction writes the register number, which contains the
 *   percpu address variable to lowcore. This also indicates that a percpu
 *   code section is executed.
 *
 * - The first mviy instruction following the mviy instruction must be the ag
 *   instruction which adds the percpu offset to the percpu address register.
 *
 * - Afterwards the atomic percpu operation follows.
 *
 * - Then a second mviy instruction writes a zero to lowcore, which indicates
 *   the end of the percpu code section.
 *
 * - In case of an interrupt/exception/nmi the register number which was
 *   written to lowcore is copied to the exception frame (pt_regs), and a zero
 *   is written to lowcore.
 *
 * - On return to the previous context it is checked if a percpu code section
 *   was executed (saved register number not zero), and if the process was
 *   migrated to a different cpu. If the percpu offset was already added to
 *   the percpu address register (instruction address does _not_ point to the
 *   ag instruction) the content of the percpu address register is adjusted so
 *   it points to percpu variable of the new cpu.
 *
 * Inline assemblies making use of this typically have a code sequence like:
 *
 *   __PCPU_BEGIN(...) <- start of percpu code section
 *   atomic_op	       <- atomic op
 *   __PCPU_END(...)   <- end of percpu code section
 */

#define LC_ALT_ADDR	__stringify(LOWCORE_ALT_ADDRESS)

#define DEFINE_GR_NUM								\
	".macro _GR_NUM opd, gr\n"						\
	".set	\\opd,255\n"							\
	".irp rs,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"			\
	"	.ifc \\gr,%%r\\rs\n"						\
	"		\\opd = \\rs\n"						\
	"	.endif\n"							\
	".endr\n"								\
	".if \\opd == 255\n"							\
	"	.error \"Illegal register number\"\n"				\
	".endif\n"								\
	".endm\n"

#define UNDEF_GR_NUM								\
	".purgem _GR_NUM\n"

#define __PCPU_MVIY(lcreg, imm)							\
	ALTERNATIVE("	mviy	" lcreg			"(%%r0)," imm "\n",	\
		    "	mviy	" lcreg "+" LC_ALT_ADDR "(%%r0)," imm "\n",	\
		    ALT_FEATURE(MFEATURE_LOWCORE))

#define __PCPU_AG(reg, lcoff)							\
	ALTERNATIVE("	ag	" reg ", " lcoff		 "(%%r0)\n",	\
		    "	ag	" reg ", " lcoff "+" LC_ALT_ADDR "(%%r0)\n",	\
		    ALT_FEATURE(MFEATURE_LOWCORE))

#define __PCPU_BEGIN(lcreg, lcoff, reg)						\
	DEFINE_GR_NUM								\
	"_GR_NUM .Lreg, " reg "\n"						\
	UNDEF_GR_NUM								\
	__PCPU_MVIY(lcreg, ".Lreg")						\
	__PCPU_AG(reg, lcoff)

#define __PCPU_END(lcreg)							\
	__PCPU_MVIY(lcreg, "0")

#ifndef MARCH_HAS_Z196_FEATURES

#define this_cpu_add_4(pcp, val)	arch_this_cpu_to_op_simple(pcp, val, +)
#define this_cpu_add_8(pcp, val)	arch_this_cpu_to_op_simple(pcp, val, +)
#define this_cpu_add_return_4(pcp, val) arch_this_cpu_to_op_simple(pcp, val, +)
#define this_cpu_add_return_8(pcp, val) arch_this_cpu_to_op_simple(pcp, val, +)
#define this_cpu_and_4(pcp, val)	arch_this_cpu_to_op_simple(pcp, val, &)
#define this_cpu_and_8(pcp, val)	arch_this_cpu_to_op_simple(pcp, val, &)
#define this_cpu_or_4(pcp, val)		arch_this_cpu_to_op_simple(pcp, val, |)
#define this_cpu_or_8(pcp, val)		arch_this_cpu_to_op_simple(pcp, val, |)

#else /* MARCH_HAS_Z196_FEATURES */

#define arch_this_cpu_add(pcp, val, op1, op2, szcast)				\
do {										\
	typedef typeof(pcp) pcp_op_T__;						\
	pcp_op_T__ val__ = (val);						\
	pcp_op_T__ old__, *ptr__;						\
										\
	ptr__ = PERCPU_PTR(&(pcp));						\
	if (__builtin_constant_p(val__) &&					\
	    ((szcast)val__ > -129) && ((szcast)val__ < 128)) {			\
		asm volatile(							\
			__PCPU_BEGIN("%[lcreg]","%[lcoff]","%[ptr__]")		\
			op2 "   0(%[ptr__]),%[val__]\n"				\
			__PCPU_END("%[lcreg]")					\
			: [ptr__] "+&a" (ptr__), "+m" (*ptr__),			\
			  "=m" (((struct lowcore *)0)->percpu_register)		\
			: [val__] "i" ((szcast)val__),				\
			  [lcreg] "i" (LC_PERCPU_REGISTER),			\
			  [lcoff] "i" (LC_PERCPU_OFFSET),			\
			  "m" (((struct lowcore *)0)->percpu_offset)		\
			: "cc");						\
	} else {								\
		asm volatile(							\
			__PCPU_BEGIN("%[lcreg]","%[lcoff]","%[ptr__]")		\
			op1 "   %[old__],%[val__],0(%[ptr__])\n"		\
			__PCPU_END("%[lcreg]")					\
			: [old__] "=&d" (old__),				\
			  [ptr__] "+&a" (ptr__),  "+m" (*ptr__),		\
			  "=m" (((struct lowcore *)0)->percpu_register)		\
			: [val__] "d" (val__),					\
			  [lcreg] "i" (LC_PERCPU_REGISTER),			\
			  [lcoff] "i" (LC_PERCPU_OFFSET),			\
			  "m" (((struct lowcore *)0)->percpu_offset)		\
			: "cc");						\
	}									\
} while (0)

#define this_cpu_add_4(pcp, val) arch_this_cpu_add(pcp, val, "laa", "asi", int)
#define this_cpu_add_8(pcp, val) arch_this_cpu_add(pcp, val, "laag", "agsi", long)

#define arch_this_cpu_add_return(pcp, val, op)				\
({									\
	typedef typeof(pcp) pcp_op_T__; 				\
	pcp_op_T__ val__ = (val);					\
	pcp_op_T__ old__, *ptr__;					\
									\
	ptr__ = PERCPU_PTR(&(pcp));					\
	asm_inline volatile(						\
		__PCPU_BEGIN("%[lcreg]","%[lcoff]","%[ptr__]")		\
		op "	%[old__],%[val__],0(%[ptr__])\n"		\
		__PCPU_END("%[lcreg]")					\
		: [old__] "=&d" (old__),				\
		  [ptr__] "+&a" (ptr__), "+m" (*ptr__),			\
		  "=m" (((struct lowcore *)0)->percpu_register)		\
		: [val__] "d" (val__),					\
		  [lcreg] "i" (LC_PERCPU_REGISTER),			\
		  [lcoff] "i" (LC_PERCPU_OFFSET),			\
		  "m" (((struct lowcore *)0)->percpu_offset)		\
		: "cc");						\
	old__ + val__;							\
})

#define this_cpu_add_return_4(pcp, val) arch_this_cpu_add_return(pcp, val, "laa")
#define this_cpu_add_return_8(pcp, val) arch_this_cpu_add_return(pcp, val, "laag")

#define arch_this_cpu_to_op(pcp, val, op)				\
do {									\
	typedef typeof(pcp) pcp_op_T__; 				\
	pcp_op_T__ val__ = (val);					\
	pcp_op_T__ old__, *ptr__;					\
									\
	ptr__ = PERCPU_PTR(&(pcp));					\
	asm_inline volatile(						\
		__PCPU_BEGIN("%[lcreg]","%[lcoff]","%[ptr__]")		\
		op "    %[old__],%[val__],0(%[ptr__])\n"		\
		__PCPU_END("%[lcreg]")					\
		: [old__] "=&d" (old__),				\
		  [ptr__] "+&a" (ptr__), "+m" (*ptr__),			\
		  "=m" (((struct lowcore *)0)->percpu_register)		\
		: [val__] "d" (val__),					\
		  [lcreg] "i" (LC_PERCPU_REGISTER),			\
		  [lcoff] "i" (LC_PERCPU_OFFSET),			\
		  "m" (((struct lowcore *)0)->percpu_offset)		\
		: "cc");						\
} while (0)

#define this_cpu_and_4(pcp, val)	arch_this_cpu_to_op(pcp, val, "lan")
#define this_cpu_and_8(pcp, val)	arch_this_cpu_to_op(pcp, val, "lang")
#define this_cpu_or_4(pcp, val)		arch_this_cpu_to_op(pcp, val, "lao")
#define this_cpu_or_8(pcp, val)		arch_this_cpu_to_op(pcp, val, "laog")

#endif /* MARCH_HAS_Z196_FEATURES */

#define arch_this_cpu_read(pcp, op)					\
({									\
	typedef typeof(pcp) pcp_op_T__;					\
	unsigned long res__;						\
	pcp_op_T__ *ptr__;						\
									\
	ptr__ = PERCPU_PTR(&(pcp));					\
	asm_inline volatile(						\
		__PCPU_BEGIN("%[lcreg]","%[lcoff]","%[ptr__]")		\
		op "	%[res__],0(%[ptr__])\n"				\
		__PCPU_END("%[lcreg]")					\
		: [res__] "=&d" (res__), [ptr__] "+&a" (ptr__),		\
		  "=m" (((struct lowcore *)0)->percpu_register)		\
		: [lcreg] "i" (LC_PERCPU_REGISTER),			\
		  [lcoff] "i" (LC_PERCPU_OFFSET),			\
		  "m" (*ptr__),						\
		  "m" (((struct lowcore *)0)->percpu_offset)		\
		: "cc");						\
	(pcp_op_T__)res__;						\
})

#define this_cpu_read_1(pcp) arch_this_cpu_read(pcp, "llgc")
#define this_cpu_read_2(pcp) arch_this_cpu_read(pcp, "llgh")
#define this_cpu_read_4(pcp) arch_this_cpu_read(pcp, "llgf")
#define this_cpu_read_8(pcp) arch_this_cpu_read(pcp, "lg")

#define arch_this_cpu_write(pcp, val, op)				\
do {									\
	typedef typeof(pcp) pcp_op_T__;					\
	pcp_op_T__ *ptr__, val__ = (val);				\
									\
	ptr__ = PERCPU_PTR(&(pcp));					\
	asm_inline volatile(						\
		__PCPU_BEGIN("%[lcreg]","%[lcoff]","%[ptr__]")		\
		op "    %[val__],0(%[ptr__])\n"				\
		__PCPU_END("%[lcreg]")					\
		: [ptr__] "+&a" (ptr__), "=m" (*ptr__),			\
		  "=m" (((struct lowcore *)0)->percpu_register)		\
		: [val__] "d" (val__),					\
		  [lcreg] "i" (LC_PERCPU_REGISTER),			\
		  [lcoff] "i" (LC_PERCPU_OFFSET),			\
		  "m" (((struct lowcore *)0)->percpu_offset)		\
		: "cc");						\
} while (0)

#define this_cpu_write_1(pcp, val) arch_this_cpu_write(pcp, val, "stc")
#define this_cpu_write_2(pcp, val) arch_this_cpu_write(pcp, val, "sth")
#define this_cpu_write_4(pcp, val) arch_this_cpu_write(pcp, val, "st")
#define this_cpu_write_8(pcp, val) arch_this_cpu_write(pcp, val, "stg")

#define arch_this_cpu_cmpxchg(pcp, oval, nval)				\
({									\
	typedef typeof(pcp) pcp_op_T__;					\
	pcp_op_T__ ret__;						\
	pcp_op_T__ *ptr__;						\
	preempt_disable_notrace();					\
	ptr__ = raw_cpu_ptr(&(pcp));					\
	ret__ = cmpxchg(ptr__, oval, nval);				\
	preempt_enable_notrace();					\
	ret__;								\
})

#define this_cpu_cmpxchg_1(pcp, oval, nval) arch_this_cpu_cmpxchg(pcp, oval, nval)
#define this_cpu_cmpxchg_2(pcp, oval, nval) arch_this_cpu_cmpxchg(pcp, oval, nval)
#define this_cpu_cmpxchg_4(pcp, oval, nval) arch_this_cpu_cmpxchg(pcp, oval, nval)
#define this_cpu_cmpxchg_8(pcp, oval, nval) arch_this_cpu_cmpxchg(pcp, oval, nval)

#define this_cpu_cmpxchg64(pcp, o, n)	this_cpu_cmpxchg_8(pcp, o, n)

#define this_cpu_cmpxchg128(pcp, oval, nval)				\
({									\
	typedef typeof(pcp) pcp_op_T__;					\
	u128 old__ = (oval), new__ = (nval);				\
	pcp_op_T__ *ptr__;						\
									\
	ptr__ = PERCPU_PTR(&(pcp));					\
	asm_inline volatile(						\
		__PCPU_BEGIN("%[lcreg]","%[lcoff]","%[ptr__]")		\
		"	cdsg	%[old__],%[new__],0(%[ptr__])\n"	\
		__PCPU_END("%[lcreg]")					\
		: [old__] "+&d" (old__), [ptr__] "+&a" (ptr__),		\
		  "+m" (*ptr__),					\
		  "=m" (((struct lowcore *)0)->percpu_register)		\
		: [new__] "d" (new__),					\
		  [lcreg] "i" (LC_PERCPU_REGISTER),			\
		  [lcoff] "i" (LC_PERCPU_OFFSET),			\
		  "m" (((struct lowcore *)0)->percpu_offset)		\
		: "memory", "cc");					\
	old__;								\
})

#define arch_this_cpu_xchg(pcp, nval)					\
({									\
	typeof(pcp) *ptr__;						\
	typeof(pcp) ret__;						\
	preempt_disable_notrace();					\
	ptr__ = raw_cpu_ptr(&(pcp));					\
	ret__ = xchg(ptr__, nval);					\
	preempt_enable_notrace();					\
	ret__;								\
})

#define this_cpu_xchg_1(pcp, nval) arch_this_cpu_xchg(pcp, nval)
#define this_cpu_xchg_2(pcp, nval) arch_this_cpu_xchg(pcp, nval)
#define this_cpu_xchg_4(pcp, nval) arch_this_cpu_xchg(pcp, nval)
#define this_cpu_xchg_8(pcp, nval) arch_this_cpu_xchg(pcp, nval)

#include <asm-generic/percpu.h>

#endif /* __ARCH_S390_PERCPU__ */
