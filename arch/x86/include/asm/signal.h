/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SIGNAL_H
#define _ASM_X86_SIGNAL_H

#ifndef __ASSEMBLY__
#include <linux/linkage.h>

/* Most things should be clean enough to redefine this at will, if care
   is taken to make libc match.  */

#define _NSIG		64

#ifdef __i386__
# define _NSIG_BPW	32
#else
# define _NSIG_BPW	64
#endif

#define _NSIG_WORDS	(_NSIG / _NSIG_BPW)

typedef unsigned long old_sigset_t;		/* at least 32 bits */

typedef struct {
	unsigned long sig[_NSIG_WORDS];
} sigset_t;

/* non-uapi in-kernel SA_FLAGS for those indicates ABI for a signal frame */
#define SA_IA32_ABI	0x02000000u
#define SA_X32_ABI	0x01000000u

/*
 * Because some traps use the IST stack, we must keep preemption
 * disabled while calling do_trap(), but do_trap() may call
 * force_sig_info() which will grab the signal spin_locks for the
 * task, which in PREEMPT_RT_FULL are mutexes.  By defining
 * ARCH_RT_DELAYS_SIGNAL_SEND the force_sig_info() will set
 * TIF_NOTIFY_RESUME and set up the signal to be sent on exit of the
 * trap.
 */
#if defined(CONFIG_PREEMPT_RT_FULL)
#define ARCH_RT_DELAYS_SIGNAL_SEND
#endif

#ifndef CONFIG_COMPAT
typedef sigset_t compat_sigset_t;
#endif

#endif /* __ASSEMBLY__ */
#include <uapi/asm/signal.h>
#ifndef __ASSEMBLY__
extern void do_signal(struct pt_regs *regs);

#define __ARCH_HAS_SA_RESTORER

#include <uapi/asm/sigcontext.h>

#ifdef __i386__

#define __HAVE_ARCH_SIG_BITOPS

#define sigaddset(set,sig)		    \
	(__builtin_constant_p(sig)	    \
	 ? __const_sigaddset((set), (sig))  \
	 : __gen_sigaddset((set), (sig)))

static inline void __gen_sigaddset(sigset_t *set, int _sig)
{
	asm("btsl %1,%0" : "+m"(*set) : "Ir"(_sig - 1) : "cc");
}

static inline void __const_sigaddset(sigset_t *set, int _sig)
{
	unsigned long sig = _sig - 1;
	set->sig[sig / _NSIG_BPW] |= 1 << (sig % _NSIG_BPW);
}

#define sigdelset(set, sig)		    \
	(__builtin_constant_p(sig)	    \
	 ? __const_sigdelset((set), (sig))  \
	 : __gen_sigdelset((set), (sig)))


static inline void __gen_sigdelset(sigset_t *set, int _sig)
{
	asm("btrl %1,%0" : "+m"(*set) : "Ir"(_sig - 1) : "cc");
}

static inline void __const_sigdelset(sigset_t *set, int _sig)
{
	unsigned long sig = _sig - 1;
	set->sig[sig / _NSIG_BPW] &= ~(1 << (sig % _NSIG_BPW));
}

static inline int __const_sigismember(sigset_t *set, int _sig)
{
	unsigned long sig = _sig - 1;
	return 1 & (set->sig[sig / _NSIG_BPW] >> (sig % _NSIG_BPW));
}

static inline int __gen_sigismember(sigset_t *set, int _sig)
{
	unsigned char ret;
	asm("btl %2,%1\n\tsetc %0"
	    : "=qm"(ret) : "m"(*set), "Ir"(_sig-1) : "cc");
	return ret;
}

#define sigismember(set, sig)			\
	(__builtin_constant_p(sig)		\
	 ? __const_sigismember((set), (sig))	\
	 : __gen_sigismember((set), (sig)))

struct pt_regs;

#else /* __i386__ */

#undef __HAVE_ARCH_SIG_BITOPS

#endif /* !__i386__ */

#endif /* __ASSEMBLY__ */
#endif /* _ASM_X86_SIGNAL_H */
