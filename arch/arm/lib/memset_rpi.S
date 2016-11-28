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

/*
 *  void *memset(void *s, int c, size_t n);
 *  On entry:
 *  a1 = pointer to buffer to fill
 *  a2 = byte pattern to fill with (caller-narrowed)
 *  a3 = number of bytes to fill
 *  On exit:
 *  a1 preserved
 */
ENTRY(mmioset)
ENTRY(memset)
ENTRY(__memset32)
ENTRY(__memset64)

        S       .req    a1
        DAT0    .req    a2
        N       .req    a3
        DAT1    .req    a4
        DAT2    .req    ip
        DAT3    .req    lr

        orr     DAT0, DAT0, DAT0, lsl #8
        push    {S, lr}
        orr     DAT0, DAT0, DAT0, lsl #16
        mov     DAT1, DAT0

        /* See if we're guaranteed to have at least one 16-byte aligned 16-byte write */
        cmp     N, #31
        blo     170f

161:    sub     N, N, #16     /* simplifies inner loop termination */
        /* Leading words and bytes */
        tst     S, #15
        beq     164f
        rsb     DAT3, S, #0   /* bits 0-3 = number of leading bytes until aligned */
        movs    DAT2, DAT3, lsl #31
        submi   N, N, #1
        strmib  DAT0, [S], #1
        subcs   N, N, #2
        strcsh  DAT0, [S], #2
        movs    DAT2, DAT3, lsl #29
        submi   N, N, #4
        strmi   DAT0, [S], #4
        subcs   N, N, #8
        stmcsia S!, {DAT0, DAT1}
164:    /* Delayed set up of DAT2 and DAT3 so we could use them as scratch registers above */
        mov     DAT2, DAT0
        mov     DAT3, DAT0
        /* Now the inner loop of 16-byte stores */
165:    stmia   S!, {DAT0, DAT1, DAT2, DAT3}
        subs    N, N, #16
        bhs     165b
166:    /* Trailing words and bytes */
        movs    N, N, lsl #29
        stmcsia S!, {DAT0, DAT1}
        strmi   DAT0, [S], #4
        movs    N, N, lsl #2
        strcsh  DAT0, [S], #2
        strmib  DAT0, [S]
199:    pop     {S, pc}

170:    /* Short case */
        mov     DAT2, DAT0
        mov     DAT3, DAT0
        tst     S, #3
        beq     174f
172:    subs    N, N, #1
        blo     199b
        strb    DAT0, [S], #1
        tst     S, #3
        bne     172b
174:    tst     N, #16
        stmneia S!, {DAT0, DAT1, DAT2, DAT3}
        b       166b

        .unreq  S
        .unreq  DAT0
        .unreq  N
        .unreq  DAT1
        .unreq  DAT2
        .unreq  DAT3
ENDPROC(__memset64)
ENDPROC(__memset32)
ENDPROC(memset)
ENDPROC(mmioset)
