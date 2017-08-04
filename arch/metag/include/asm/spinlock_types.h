/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_METAG_SPINLOCK_TYPES_H
#define _ASM_METAG_SPINLOCK_TYPES_H

typedef struct {
	volatile unsigned int lock;
} arch_spinlock_t;

#define __ARCH_SPIN_LOCK_UNLOCKED	{ 0 }

typedef struct {
	volatile unsigned int lock;
} arch_rwlock_t;

#define __ARCH_RW_LOCK_UNLOCKED		{ 0 }

#endif /* _ASM_METAG_SPINLOCK_TYPES_H */
