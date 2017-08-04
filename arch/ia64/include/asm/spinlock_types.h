/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_IA64_SPINLOCK_TYPES_H
#define _ASM_IA64_SPINLOCK_TYPES_H

typedef struct {
	volatile unsigned int lock;
} arch_spinlock_t;

#define __ARCH_SPIN_LOCK_UNLOCKED	{ 0 }

typedef struct {
	volatile unsigned int read_counter	: 31;
	volatile unsigned int write_lock	:  1;
} arch_rwlock_t;

#define __ARCH_RW_LOCK_UNLOCKED		{ 0, 0 }

#endif
