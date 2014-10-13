/*
Copyright (c) 2013, Raspberry Pi Foundation
Copyright (c) 2013, RISC OS Open Ltd
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <linux/linkage.h>
#include "arm-mem.h"

/* Prevent the stack from becoming executable */
#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif

    .text
    .arch armv6
    .object_arch armv4
    .arm
    .altmacro
    .p2align 2

.macro memcmp_process_head  unaligned
 .if unaligned
        ldr     DAT0, [S_1], #4
        ldr     DAT1, [S_1], #4
        ldr     DAT2, [S_1], #4
        ldr     DAT3, [S_1], #4
 .else
        ldmia   S_1!, {DAT0, DAT1, DAT2, DAT3}
 .endif
        ldmia   S_2!, {DAT4, DAT5, DAT6, DAT7}
.endm

.macro memcmp_process_tail
        cmp     DAT0, DAT4
        cmpeq   DAT1, DAT5
        cmpeq   DAT2, DAT6
        cmpeq   DAT3, DAT7
        bne     200f
.endm

.macro memcmp_leading_31bytes
        movs    DAT0, OFF, lsl #31
        ldrmib  DAT0, [S_1], #1
        ldrcsh  DAT1, [S_1], #2
        ldrmib  DAT4, [S_2], #1
        ldrcsh  DAT5, [S_2], #2
        movpl   DAT0, #0
        movcc   DAT1, #0
        movpl   DAT4, #0
        movcc   DAT5, #0
        submi   N, N, #1
        subcs   N, N, #2
        cmp     DAT0, DAT4
        cmpeq   DAT1, DAT5
        bne     200f
        movs    DAT0, OFF, lsl #29
        ldrmi   DAT0, [S_1], #4
        ldrcs   DAT1, [S_1], #4
        ldrcs   DAT2, [S_1], #4
        ldrmi   DAT4, [S_2], #4
        ldmcsia S_2!, {DAT5, DAT6}
        movpl   DAT0, #0
        movcc   DAT1, #0
        movcc   DAT2, #0
        movpl   DAT4, #0
        movcc   DAT5, #0
        movcc   DAT6, #0
        submi   N, N, #4
        subcs   N, N, #8
        cmp     DAT0, DAT4
        cmpeq   DAT1, DAT5
        cmpeq   DAT2, DAT6
        bne     200f
        tst     OFF, #16
        beq     105f
        memcmp_process_head  1
        sub     N, N, #16
        memcmp_process_tail
105:
.endm

.macro memcmp_trailing_15bytes  unaligned
        movs    N, N, lsl #29
 .if unaligned
        ldrcs   DAT0, [S_1], #4
        ldrcs   DAT1, [S_1], #4
 .else
        ldmcsia S_1!, {DAT0, DAT1}
 .endif
        ldrmi   DAT2, [S_1], #4
        ldmcsia S_2!, {DAT4, DAT5}
        ldrmi   DAT6, [S_2], #4
        movcc   DAT0, #0
        movcc   DAT1, #0
        movpl   DAT2, #0
        movcc   DAT4, #0
        movcc   DAT5, #0
        movpl   DAT6, #0
        cmp     DAT0, DAT4
        cmpeq   DAT1, DAT5
        cmpeq   DAT2, DAT6
        bne     200f
        movs    N, N, lsl #2
        ldrcsh  DAT0, [S_1], #2
        ldrmib  DAT1, [S_1]
        ldrcsh  DAT4, [S_2], #2
        ldrmib  DAT5, [S_2]
        movcc   DAT0, #0
        movpl   DAT1, #0
        movcc   DAT4, #0
        movpl   DAT5, #0
        cmp     DAT0, DAT4
        cmpeq   DAT1, DAT5
        bne     200f
.endm

.macro memcmp_long_inner_loop  unaligned
110:
        memcmp_process_head  unaligned
        pld     [S_2, #prefetch_distance*32 + 16]
        memcmp_process_tail
        memcmp_process_head  unaligned
        pld     [S_1, OFF]
        memcmp_process_tail
        subs    N, N, #32
        bhs     110b
        /* Just before the final (prefetch_distance+1) 32-byte blocks,
         * deal with final preloads */
        preload_trailing  0, S_1, N, DAT0
        preload_trailing  0, S_2, N, DAT0
        add     N, N, #(prefetch_distance+2)*32 - 16
120:
        memcmp_process_head  unaligned
        memcmp_process_tail
        subs    N, N, #16
        bhs     120b
        /* Trailing words and bytes */
        tst     N, #15
        beq     199f
        memcmp_trailing_15bytes  unaligned
199:    /* Reached end without detecting a difference */
        mov     a1, #0
        setend  le
        pop     {DAT1-DAT6, pc}
.endm

.macro memcmp_short_inner_loop  unaligned
        subs    N, N, #16     /* simplifies inner loop termination */
        blo     122f
120:
        memcmp_process_head  unaligned
        memcmp_process_tail
        subs    N, N, #16
        bhs     120b
122:    /* Trailing words and bytes */
        tst     N, #15
        beq     199f
        memcmp_trailing_15bytes  unaligned
199:    /* Reached end without detecting a difference */
        mov     a1, #0
        setend  le
        pop     {DAT1-DAT6, pc}
.endm

/*
 * int memcmp(const void *s1, const void *s2, size_t n);
 * On entry:
 * a1 = pointer to buffer 1
 * a2 = pointer to buffer 2
 * a3 = number of bytes to compare (as unsigned chars)
 * On exit:
 * a1 = >0/=0/<0 if s1 >/=/< s2
 */

.set prefetch_distance, 2

ENTRY(memcmp)
        S_1     .req    a1
        S_2     .req    a2
        N       .req    a3
        DAT0    .req    a4
        DAT1    .req    v1
        DAT2    .req    v2
        DAT3    .req    v3
        DAT4    .req    v4
        DAT5    .req    v5
        DAT6    .req    v6
        DAT7    .req    ip
        OFF     .req    lr

        push    {DAT1-DAT6, lr}
        setend  be /* lowest-addressed bytes are most significant */

        /* To preload ahead as we go, we need at least (prefetch_distance+2) 32-byte blocks */
        cmp     N, #(prefetch_distance+3)*32 - 1
        blo     170f

        /* Long case */
        /* Adjust N so that the decrement instruction can also test for
         * inner loop termination. We want it to stop when there are
         * (prefetch_distance+1) complete blocks to go. */
        sub     N, N, #(prefetch_distance+2)*32
        preload_leading_step1  0, DAT0, S_1
        preload_leading_step1  0, DAT1, S_2
        tst     S_2, #31
        beq     154f
        rsb     OFF, S_2, #0 /* no need to AND with 15 here */
        preload_leading_step2  0, DAT0, S_1, OFF, DAT2
        preload_leading_step2  0, DAT1, S_2, OFF, DAT2
        memcmp_leading_31bytes
154:    /* Second source now cacheline (32-byte) aligned; we have at
         * least one prefetch to go. */
        /* Prefetch offset is best selected such that it lies in the
         * first 8 of each 32 bytes - but it's just as easy to aim for
         * the first one */
        and     OFF, S_1, #31
        rsb     OFF, OFF, #32*prefetch_distance
        tst     S_1, #3
        bne     140f
        memcmp_long_inner_loop  0
140:    memcmp_long_inner_loop  1

170:    /* Short case */
        teq     N, #0
        beq     199f
        preload_all 0, 0, 0, S_1, N, DAT0, DAT1
        preload_all 0, 0, 0, S_2, N, DAT0, DAT1
        tst     S_2, #3
        beq     174f
172:    subs    N, N, #1
        blo     199f
        ldrb    DAT0, [S_1], #1
        ldrb    DAT4, [S_2], #1
        cmp     DAT0, DAT4
        bne     200f
        tst     S_2, #3
        bne     172b
174:    /* Second source now 4-byte aligned; we have 0 or more bytes to go */
        tst     S_1, #3
        bne     140f
        memcmp_short_inner_loop  0
140:    memcmp_short_inner_loop  1

200:    /* Difference found: determine sign. */
        movhi   a1, #1
        movlo   a1, #-1
        setend  le
        pop     {DAT1-DAT6, pc}

        .unreq  S_1
        .unreq  S_2
        .unreq  N
        .unreq  DAT0
        .unreq  DAT1
        .unreq  DAT2
        .unreq  DAT3
        .unreq  DAT4
        .unreq  DAT5
        .unreq  DAT6
        .unreq  DAT7
        .unreq  OFF
ENDPROC(memcmp)
