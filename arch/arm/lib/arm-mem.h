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

.macro myfunc fname
 .func fname
 .global fname
fname:
.endm

.macro preload_leading_step1  backwards, ptr, base
/* If the destination is already 16-byte aligned, then we need to preload
 * between 0 and prefetch_distance (inclusive) cache lines ahead so there
 * are no gaps when the inner loop starts.
 */
 .if backwards
        sub     ptr, base, #1
        bic     ptr, ptr, #31
 .else
        bic     ptr, base, #31
 .endif
 .set OFFSET, 0
 .rept prefetch_distance+1
        pld     [ptr, #OFFSET]
  .if backwards
   .set OFFSET, OFFSET-32
  .else
   .set OFFSET, OFFSET+32
  .endif
 .endr
.endm

.macro preload_leading_step2  backwards, ptr, base, leading_bytes, tmp
/* However, if the destination is not 16-byte aligned, we may need to
 * preload one more cache line than that. The question we need to ask is:
 * are the leading bytes more than the amount by which the source
 * pointer will be rounded down for preloading, and if so, by how many
 * cache lines?
 */
 .if backwards
/* Here we compare against how many bytes we are into the
 * cache line, counting down from the highest such address.
 * Effectively, we want to calculate
 *     leading_bytes = dst&15
 *     cacheline_offset = 31-((src-leading_bytes-1)&31)
 *     extra_needed = leading_bytes - cacheline_offset
 * and test if extra_needed is <= 0, or rearranging:
 *     leading_bytes + (src-leading_bytes-1)&31 <= 31
 */
        mov     tmp, base, lsl #32-5
        sbc     tmp, tmp, leading_bytes, lsl #32-5
        adds    tmp, tmp, leading_bytes, lsl #32-5
        bcc     61f
        pld     [ptr, #-32*(prefetch_distance+1)]
 .else
/* Effectively, we want to calculate
 *     leading_bytes = (-dst)&15
 *     cacheline_offset = (src+leading_bytes)&31
 *     extra_needed = leading_bytes - cacheline_offset
 * and test if extra_needed is <= 0.
 */
        mov     tmp, base, lsl #32-5
        add     tmp, tmp, leading_bytes, lsl #32-5
        rsbs    tmp, tmp, leading_bytes, lsl #32-5
        bls     61f
        pld     [ptr, #32*(prefetch_distance+1)]
 .endif
61:
.endm

.macro preload_trailing  backwards, base, remain, tmp
        /* We need either 0, 1 or 2 extra preloads */
 .if backwards
        rsb     tmp, base, #0
        mov     tmp, tmp, lsl #32-5
 .else
        mov     tmp, base, lsl #32-5
 .endif
        adds    tmp, tmp, remain, lsl #32-5
        adceqs  tmp, tmp, #0
        /* The instruction above has two effects: ensures Z is only
         * set if C was clear (so Z indicates that both shifted quantities
         * were 0), and clears C if Z was set (so C indicates that the sum
         * of the shifted quantities was greater and not equal to 32) */
        beq     82f
 .if backwards
        sub     tmp, base, #1
        bic     tmp, tmp, #31
 .else
        bic     tmp, base, #31
 .endif
        bcc     81f
 .if backwards
        pld     [tmp, #-32*(prefetch_distance+1)]
81:
        pld     [tmp, #-32*prefetch_distance]
 .else
        pld     [tmp, #32*(prefetch_distance+2)]
81:
        pld     [tmp, #32*(prefetch_distance+1)]
 .endif
82:
.endm

.macro preload_all    backwards, narrow_case, shift, base, remain, tmp0, tmp1
 .if backwards
        sub     tmp0, base, #1
        bic     tmp0, tmp0, #31
        pld     [tmp0]
        sub     tmp1, base, remain, lsl #shift
 .else
        bic     tmp0, base, #31
        pld     [tmp0]
        add     tmp1, base, remain, lsl #shift
        sub     tmp1, tmp1, #1
 .endif
        bic     tmp1, tmp1, #31
        cmp     tmp1, tmp0
        beq     92f
 .if narrow_case
        /* In this case, all the data fits in either 1 or 2 cache lines */
        pld     [tmp1]
 .else
91:
  .if backwards
        sub     tmp0, tmp0, #32
  .else
        add     tmp0, tmp0, #32
  .endif
        cmp     tmp0, tmp1
        pld     [tmp0]
        bne     91b
 .endif
92:
.endm
