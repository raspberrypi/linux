/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_SHARED_STRING_H
#define _ASM_X86_SHARED_STRING_H

/*
 * Returns:	0 (equal)
 * 		1 (not equal)
 *
 * In contrast, the regular memcmp() follows glibc return value semantics.
 */
static __always_inline int __inline_memcmp(const void *s1, const void *s2, size_t len)
{
	bool diff;

	/*
	 * Make sure ZF is properly set in the len==0 case because in it,
	 * RCX==0 and the REPE; CMPSB won't get executed.
	 *
	 * The "cc" clobber has no meaning anymore, just source compatibility.
	 * On x86 the flag status bits are automatically added to the clobber
	 * set when there are no =@ccXY constraints. Keep it as documentation.
	 */
	asm volatile("test %3, %3\n\t"
		     "repe cmpsb"
		     : "=@ccnz" (diff), "+D" (s1), "+S" (s2), "+c" (len)
		     : : /* "cc", */ "memory");

	return diff;
}

#endif /* _ASM_X86_SHARED_STRING_H */
