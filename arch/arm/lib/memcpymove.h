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

.macro unaligned_words  backwards, align, use_pld, words, r0, r1, r2, r3, r4, r5, r6, r7, r8
 .if words == 1
  .if backwards
        mov     r1, r0, lsl #32-align*8
        ldr     r0, [S, #-4]!
        orr     r1, r1, r0, lsr #align*8
        str     r1, [D, #-4]!
  .else
        mov     r0, r1, lsr #align*8
        ldr     r1, [S, #4]!
        orr     r0, r0, r1, lsl #32-align*8
        str     r0, [D], #4
  .endif
 .elseif words == 2
  .if backwards
        ldr     r1, [S, #-4]!
        mov     r2, r0, lsl #32-align*8
        ldr     r0, [S, #-4]!
        orr     r2, r2, r1, lsr #align*8
        mov     r1, r1, lsl #32-align*8
        orr     r1, r1, r0, lsr #align*8
        stmdb   D!, {r1, r2}
  .else
        ldr     r1, [S, #4]!
        mov     r0, r2, lsr #align*8
        ldr     r2, [S, #4]!
        orr     r0, r0, r1, lsl #32-align*8
        mov     r1, r1, lsr #align*8
        orr     r1, r1, r2, lsl #32-align*8
        stmia   D!, {r0, r1}
  .endif
 .elseif words == 4
  .if backwards
        ldmdb   S!, {r2, r3}
        mov     r4, r0, lsl #32-align*8
        ldmdb   S!, {r0, r1}
        orr     r4, r4, r3, lsr #align*8
        mov     r3, r3, lsl #32-align*8
        orr     r3, r3, r2, lsr #align*8
        mov     r2, r2, lsl #32-align*8
        orr     r2, r2, r1, lsr #align*8
        mov     r1, r1, lsl #32-align*8
        orr     r1, r1, r0, lsr #align*8
        stmdb   D!, {r1, r2, r3, r4}
  .else
        ldmib   S!, {r1, r2}
        mov     r0, r4, lsr #align*8
        ldmib   S!, {r3, r4}
        orr     r0, r0, r1, lsl #32-align*8
        mov     r1, r1, lsr #align*8
        orr     r1, r1, r2, lsl #32-align*8
        mov     r2, r2, lsr #align*8
        orr     r2, r2, r3, lsl #32-align*8
        mov     r3, r3, lsr #align*8
        orr     r3, r3, r4, lsl #32-align*8
        stmia   D!, {r0, r1, r2, r3}
  .endif
 .elseif words == 8
  .if backwards
        ldmdb   S!, {r4, r5, r6, r7}
        mov     r8, r0, lsl #32-align*8
        ldmdb   S!, {r0, r1, r2, r3}
   .if use_pld
        pld     [S, OFF]
   .endif
        orr     r8, r8, r7, lsr #align*8
        mov     r7, r7, lsl #32-align*8
        orr     r7, r7, r6, lsr #align*8
        mov     r6, r6, lsl #32-align*8
        orr     r6, r6, r5, lsr #align*8
        mov     r5, r5, lsl #32-align*8
        orr     r5, r5, r4, lsr #align*8
        mov     r4, r4, lsl #32-align*8
        orr     r4, r4, r3, lsr #align*8
        mov     r3, r3, lsl #32-align*8
        orr     r3, r3, r2, lsr #align*8
        mov     r2, r2, lsl #32-align*8
        orr     r2, r2, r1, lsr #align*8
        mov     r1, r1, lsl #32-align*8
        orr     r1, r1, r0, lsr #align*8
        stmdb   D!, {r5, r6, r7, r8}
        stmdb   D!, {r1, r2, r3, r4}
  .else
        ldmib   S!, {r1, r2, r3, r4}
        mov     r0, r8, lsr #align*8
        ldmib   S!, {r5, r6, r7, r8}
   .if use_pld
        pld     [S, OFF]
   .endif
        orr     r0, r0, r1, lsl #32-align*8
        mov     r1, r1, lsr #align*8
        orr     r1, r1, r2, lsl #32-align*8
        mov     r2, r2, lsr #align*8
        orr     r2, r2, r3, lsl #32-align*8
        mov     r3, r3, lsr #align*8
        orr     r3, r3, r4, lsl #32-align*8
        mov     r4, r4, lsr #align*8
        orr     r4, r4, r5, lsl #32-align*8
        mov     r5, r5, lsr #align*8
        orr     r5, r5, r6, lsl #32-align*8
        mov     r6, r6, lsr #align*8
        orr     r6, r6, r7, lsl #32-align*8
        mov     r7, r7, lsr #align*8
        orr     r7, r7, r8, lsl #32-align*8
        stmia   D!, {r0, r1, r2, r3}
        stmia   D!, {r4, r5, r6, r7}
  .endif
 .endif
.endm

.macro memcpy_leading_15bytes  backwards, align
        movs    DAT1, DAT2, lsl #31
        sub     N, N, DAT2
 .if backwards
        ldrmib  DAT0, [S, #-1]!
        ldrcsh  DAT1, [S, #-2]!
        strmib  DAT0, [D, #-1]!
        strcsh  DAT1, [D, #-2]!
 .else
        ldrmib  DAT0, [S], #1
        ldrcsh  DAT1, [S], #2
        strmib  DAT0, [D], #1
        strcsh  DAT1, [D], #2
 .endif
        movs    DAT1, DAT2, lsl #29
 .if backwards
        ldrmi   DAT0, [S, #-4]!
  .if align == 0
        ldmcsdb S!, {DAT1, DAT2}
  .else
        ldrcs   DAT2, [S, #-4]!
        ldrcs   DAT1, [S, #-4]!
  .endif
        strmi   DAT0, [D, #-4]!
        stmcsdb D!, {DAT1, DAT2}
 .else
        ldrmi   DAT0, [S], #4
  .if align == 0
        ldmcsia S!, {DAT1, DAT2}
  .else
        ldrcs   DAT1, [S], #4
        ldrcs   DAT2, [S], #4
  .endif
        strmi   DAT0, [D], #4
        stmcsia D!, {DAT1, DAT2}
 .endif
.endm

.macro memcpy_trailing_15bytes  backwards, align
        movs    N, N, lsl #29
 .if backwards
  .if align == 0
        ldmcsdb S!, {DAT0, DAT1}
  .else
        ldrcs   DAT1, [S, #-4]!
        ldrcs   DAT0, [S, #-4]!
  .endif
        ldrmi   DAT2, [S, #-4]!
        stmcsdb D!, {DAT0, DAT1}
        strmi   DAT2, [D, #-4]!
 .else
  .if align == 0
        ldmcsia S!, {DAT0, DAT1}
  .else
        ldrcs   DAT0, [S], #4
        ldrcs   DAT1, [S], #4
  .endif
        ldrmi   DAT2, [S], #4
        stmcsia D!, {DAT0, DAT1}
        strmi   DAT2, [D], #4
 .endif
        movs    N, N, lsl #2
 .if backwards
        ldrcsh  DAT0, [S, #-2]!
        ldrmib  DAT1, [S, #-1]
        strcsh  DAT0, [D, #-2]!
        strmib  DAT1, [D, #-1]
 .else
        ldrcsh  DAT0, [S], #2
        ldrmib  DAT1, [S]
        strcsh  DAT0, [D], #2
        strmib  DAT1, [D]
 .endif
.endm

.macro memcpy_long_inner_loop  backwards, align
 .if align != 0
  .if backwards
        ldr     DAT0, [S, #-align]!
  .else
        ldr     LAST, [S, #-align]!
  .endif
 .endif
110:
 .if align == 0
  .if backwards
        ldmdb   S!, {DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, LAST}
        pld     [S, OFF]
        stmdb   D!, {DAT4, DAT5, DAT6, LAST}
        stmdb   D!, {DAT0, DAT1, DAT2, DAT3}
  .else
        ldmia   S!, {DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, LAST}
        pld     [S, OFF]
        stmia   D!, {DAT0, DAT1, DAT2, DAT3}
        stmia   D!, {DAT4, DAT5, DAT6, LAST}
  .endif
 .else
        unaligned_words  backwards, align, 1, 8, DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, DAT7, LAST
 .endif
        subs    N, N, #32
        bhs     110b
        /* Just before the final (prefetch_distance+1) 32-byte blocks, deal with final preloads */
        preload_trailing  backwards, S, N, OFF
        add     N, N, #(prefetch_distance+2)*32 - 32
120:
 .if align == 0
  .if backwards
        ldmdb   S!, {DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, LAST}
        stmdb   D!, {DAT4, DAT5, DAT6, LAST}
        stmdb   D!, {DAT0, DAT1, DAT2, DAT3}
  .else
        ldmia   S!, {DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, LAST}
        stmia   D!, {DAT0, DAT1, DAT2, DAT3}
        stmia   D!, {DAT4, DAT5, DAT6, LAST}
  .endif
 .else
        unaligned_words  backwards, align, 0, 8, DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, DAT7, LAST
 .endif
        subs    N, N, #32
        bhs     120b
        tst     N, #16
 .if align == 0
  .if backwards
        ldmnedb S!, {DAT0, DAT1, DAT2, LAST}
        stmnedb D!, {DAT0, DAT1, DAT2, LAST}
  .else
        ldmneia S!, {DAT0, DAT1, DAT2, LAST}
        stmneia D!, {DAT0, DAT1, DAT2, LAST}
  .endif
 .else
        beq     130f
        unaligned_words  backwards, align, 0, 4, DAT0, DAT1, DAT2, DAT3, LAST
130:
 .endif
        /* Trailing words and bytes */
        tst      N, #15
        beq      199f
 .if align != 0
        add     S, S, #align
 .endif
        memcpy_trailing_15bytes  backwards, align
199:
        pop     {DAT3, DAT4, DAT5, DAT6, DAT7}
        pop     {D, DAT1, DAT2, pc}
.endm

.macro memcpy_medium_inner_loop  backwards, align
120:
 .if backwards
  .if align == 0
        ldmdb   S!, {DAT0, DAT1, DAT2, LAST}
  .else
        ldr     LAST, [S, #-4]!
        ldr     DAT2, [S, #-4]!
        ldr     DAT1, [S, #-4]!
        ldr     DAT0, [S, #-4]!
  .endif
        stmdb   D!, {DAT0, DAT1, DAT2, LAST}
 .else
  .if align == 0
        ldmia   S!, {DAT0, DAT1, DAT2, LAST}
  .else
        ldr     DAT0, [S], #4
        ldr     DAT1, [S], #4
        ldr     DAT2, [S], #4
        ldr     LAST, [S], #4
  .endif
        stmia   D!, {DAT0, DAT1, DAT2, LAST}
 .endif
        subs     N, N, #16
        bhs      120b
        /* Trailing words and bytes */
        tst      N, #15
        beq      199f
        memcpy_trailing_15bytes  backwards, align
199:
        pop     {D, DAT1, DAT2, pc}
.endm

.macro memcpy_short_inner_loop  backwards, align
        tst     N, #16
 .if backwards
  .if align == 0
        ldmnedb S!, {DAT0, DAT1, DAT2, LAST}
  .else
        ldrne   LAST, [S, #-4]!
        ldrne   DAT2, [S, #-4]!
        ldrne   DAT1, [S, #-4]!
        ldrne   DAT0, [S, #-4]!
  .endif
        stmnedb D!, {DAT0, DAT1, DAT2, LAST}
 .else
  .if align == 0
        ldmneia S!, {DAT0, DAT1, DAT2, LAST}
  .else
        ldrne   DAT0, [S], #4
        ldrne   DAT1, [S], #4
        ldrne   DAT2, [S], #4
        ldrne   LAST, [S], #4
  .endif
        stmneia D!, {DAT0, DAT1, DAT2, LAST}
 .endif
        memcpy_trailing_15bytes  backwards, align
199:
        pop     {D, DAT1, DAT2, pc}
.endm

.macro memcpy backwards
        D       .req    a1
        S       .req    a2
        N       .req    a3
        DAT0    .req    a4
        DAT1    .req    v1
        DAT2    .req    v2
        DAT3    .req    v3
        DAT4    .req    v4
        DAT5    .req    v5
        DAT6    .req    v6
        DAT7    .req    sl
        LAST    .req    ip
        OFF     .req    lr

        .cfi_startproc

        push    {D, DAT1, DAT2, lr}

        .cfi_def_cfa_offset 16
        .cfi_rel_offset D, 0
        .cfi_undefined  S
        .cfi_undefined  N
        .cfi_undefined  DAT0
        .cfi_rel_offset DAT1, 4
        .cfi_rel_offset DAT2, 8
        .cfi_undefined  LAST
        .cfi_rel_offset lr, 12

 .if backwards
        add     D, D, N
        add     S, S, N
 .endif

        /* See if we're guaranteed to have at least one 16-byte aligned 16-byte write */
        cmp     N, #31
        blo     170f
        /* To preload ahead as we go, we need at least (prefetch_distance+2) 32-byte blocks */
        cmp     N, #(prefetch_distance+3)*32 - 1
        blo     160f

        /* Long case */
        push    {DAT3, DAT4, DAT5, DAT6, DAT7}

        .cfi_def_cfa_offset 36
        .cfi_rel_offset D, 20
        .cfi_rel_offset DAT1, 24
        .cfi_rel_offset DAT2, 28
        .cfi_rel_offset DAT3, 0
        .cfi_rel_offset DAT4, 4
        .cfi_rel_offset DAT5, 8
        .cfi_rel_offset DAT6, 12
        .cfi_rel_offset DAT7, 16
        .cfi_rel_offset lr, 32

        /* Adjust N so that the decrement instruction can also test for
         * inner loop termination. We want it to stop when there are
         * (prefetch_distance+1) complete blocks to go. */
        sub     N, N, #(prefetch_distance+2)*32
        preload_leading_step1  backwards, DAT0, S
 .if backwards
        /* Bug in GAS: it accepts, but mis-assembles the instruction
         * ands    DAT2, D, #60, 2
         * which sets DAT2 to the number of leading bytes until destination is aligned and also clears C (sets borrow)
         */
        .word   0xE210513C
        beq     154f
 .else
        ands    DAT2, D, #15
        beq     154f
        rsb     DAT2, DAT2, #16 /* number of leading bytes until destination aligned */
 .endif
        preload_leading_step2  backwards, DAT0, S, DAT2, OFF
        memcpy_leading_15bytes backwards, 1
154:    /* Destination now 16-byte aligned; we have at least one prefetch as well as at least one 16-byte output block */
        /* Prefetch offset is best selected such that it lies in the first 8 of each 32 bytes - but it's just as easy to aim for the first one */
 .if backwards
        rsb     OFF, S, #3
        and     OFF, OFF, #28
        sub     OFF, OFF, #32*(prefetch_distance+1)
 .else
        and     OFF, S, #28
        rsb     OFF, OFF, #32*prefetch_distance
 .endif
        movs    DAT0, S, lsl #31
        bhi     157f
        bcs     156f
        bmi     155f
        memcpy_long_inner_loop  backwards, 0
155:    memcpy_long_inner_loop  backwards, 1
156:    memcpy_long_inner_loop  backwards, 2
157:    memcpy_long_inner_loop  backwards, 3

        .cfi_def_cfa_offset 16
        .cfi_rel_offset D, 0
        .cfi_rel_offset DAT1, 4
        .cfi_rel_offset DAT2, 8
        .cfi_same_value DAT3
        .cfi_same_value DAT4
        .cfi_same_value DAT5
        .cfi_same_value DAT6
        .cfi_same_value DAT7
        .cfi_rel_offset lr, 12

160:    /* Medium case */
        preload_all  backwards, 0, 0, S, N, DAT2, OFF
        sub     N, N, #16     /* simplifies inner loop termination */
 .if backwards
        ands    DAT2, D, #15
        beq     164f
 .else
        ands    DAT2, D, #15
        beq     164f
        rsb     DAT2, DAT2, #16
 .endif
        memcpy_leading_15bytes backwards, align
164:    /* Destination now 16-byte aligned; we have at least one 16-byte output block */
        tst     S, #3
        bne     140f
        memcpy_medium_inner_loop  backwards, 0
140:    memcpy_medium_inner_loop  backwards, 1

170:    /* Short case, less than 31 bytes, so no guarantee of at least one 16-byte block */
        teq     N, #0
        beq     199f
        preload_all  backwards, 1, 0, S, N, DAT2, LAST
        tst     D, #3
        beq     174f
172:    subs    N, N, #1
        blo     199f
 .if backwards
        ldrb    DAT0, [S, #-1]!
        strb    DAT0, [D, #-1]!
 .else
        ldrb    DAT0, [S], #1
        strb    DAT0, [D], #1
 .endif
        tst     D, #3
        bne     172b
174:    /* Destination now 4-byte aligned; we have 0 or more output bytes to go */
        tst     S, #3
        bne     140f
        memcpy_short_inner_loop  backwards, 0
140:    memcpy_short_inner_loop  backwards, 1

        .cfi_endproc

        .unreq  D
        .unreq  S
        .unreq  N
        .unreq  DAT0
        .unreq  DAT1
        .unreq  DAT2
        .unreq  DAT3
        .unreq  DAT4
        .unreq  DAT5
        .unreq  DAT6
        .unreq  DAT7
        .unreq  LAST
        .unreq  OFF
.endm
