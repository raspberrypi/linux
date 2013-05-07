#ifndef _VC_SUPPORT_H_
#define _VC_SUPPORT_H_

/*
 * vc_support.h
 *
 *  Created on: 25 Nov 2012
 *      Author: Simon
 */

enum {
/*
      If a MEM_HANDLE_T is discardable, the memory manager may resize it to size
      0 at any time when it is not locked or retained.
   */
   MEM_FLAG_DISCARDABLE = 1 << 0,

   /*
      If a MEM_HANDLE_T is allocating (or normal), its block of memory will be
      accessed in an allocating fashion through the cache.
   */
   MEM_FLAG_NORMAL = 0 << 2,
   MEM_FLAG_ALLOCATING = MEM_FLAG_NORMAL,

   /*
      If a MEM_HANDLE_T is direct, its block of memory will be accessed
      directly, bypassing the cache.
   */
   MEM_FLAG_DIRECT = 1 << 2,

   /*
      If a MEM_HANDLE_T is coherent, its block of memory will be accessed in a
      non-allocating fashion through the cache.
   */
   MEM_FLAG_COHERENT = 2 << 2,

   /*
      If a MEM_HANDLE_T is L1-nonallocating, its block of memory will be accessed by
      the VPU in a fashion which is allocating in L2, but only coherent in L1.
   */
   MEM_FLAG_L1_NONALLOCATING = (MEM_FLAG_DIRECT | MEM_FLAG_COHERENT),

   /*
      If a MEM_HANDLE_T is zero'd, its contents are set to 0 rather than
      MEM_HANDLE_INVALID on allocation and resize up.
   */
   MEM_FLAG_ZERO = 1 << 4,

   /*
      If a MEM_HANDLE_T is uninitialised, it will not be reset to a defined value
      (either zero, or all 1's) on allocation.
    */
   MEM_FLAG_NO_INIT = 1 << 5,

   /*
      Hints.
   */
   MEM_FLAG_HINT_PERMALOCK = 1 << 6, /* Likely to be locked for long periods of time. */
};

unsigned int AllocateVcMemory(unsigned int *pHandle, unsigned int size, unsigned int alignment, unsigned int flags);
unsigned int ReleaseVcMemory(unsigned int handle);
unsigned int LockVcMemory(unsigned int *pBusAddress, unsigned int handle);
unsigned int UnlockVcMemory(unsigned int handle);

unsigned int ExecuteVcCode(unsigned int code,
		unsigned int r0, unsigned int r1, unsigned int r2, unsigned int r3, unsigned int r4, unsigned int r5);

#endif
