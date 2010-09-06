@ Copyright (C) 2010 Texas Instruments, Inc - http://www.ti.com/
@
@ Description: NEON/VFP accelerated functions for armv7 architecture
@  Created on: Sept 3, 2010
@      Author: Rob Clark <rob@ti.com>
@
@ This library is free software; you can redistribute it and/or
@ modify it under the terms of the GNU Library General Public
@ License as published by the Free Software Foundation; either
@ version 2 of the License, or (at your option) any later version.
@
@ This library is distributed in the hope that it will be useful,
@ but WITHOUT ANY WARRANTY; without even the implied warranty of
@ MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
@ Library General Public License for more details.
@
@ You should have received a copy of the GNU Library General Public
@ License along with this library; if not, write to the
@ Free Software Foundation, Inc., 59 Temple Place - Suite 330,
@ Boston, MA 02111-1307, USA.

       .fpu neon
       .text

       .align
       .global V4L2ShadowBlitTransparentARGB32
       .type   V4L2ShadowBlitTransparentARGB32, %function
@void
@V4L2ShadowBlitTransparentARGB32 (void *winBase, int winStride, int w, int h)
@{
V4L2ShadowBlitTransparentARGB32:
        push            {lr}
        vmov.u32        q0,  #0x00000000
1:      @ begin of outer loop
        mov             r12, r2                  @ i
        cmp             r12, #4
        mov             lr,  r0                  @ win
        blt             3f
        @ begin of inner loop
2:      @ handle remaining >= 4 pixels
        vst1.32         {q0},  [lr]!
        sub             r12, r12, #4
        cmp             r12, #4
        bge             2b
3:      @ handle remaining >= 2 pixels
        cmp             r12, #2
        blt             4f
        vst1.32         {d0}, [lr]!
        sub             r12,  r12, #2
4:      @ handle remaining >= 1 pixels
        cmp             r12, #1
        blt             5f
        vst1.32         {d0[0]}, [lr]!
5:
        sub             r3,  r3,  #1
        cmp             r3,  #0
        add             r0,  r0,  r1             @ winBase += winStride
        bgt             1b
        pop             {pc}
@}

       .align
       .global V4L2ShadowBlitSolidARGB32
       .type   V4L2ShadowBlitSolidARGB32, %function
@void
@V4L2ShadowBlitSolidARGB32 (void *winBase, int winStride,
@        void *shaBase, int shaStride, int w, int h)
@{
V4L2ShadowBlitSolidARGB32:
        pld             [r2]
        push            {r4,r5,r6,lr}
        ldr             r4,  [sp, #16]           @ w
        ldr             r5,  [sp, #20]           @ h
        vmov.u32        q0,  #0xff000000
1:      @ begin of outer loop
        mov             r12, r4                  @ i
        cmp             r12, #16
        mov             r6,  r0                  @ win
        mov             lr,  r2                  @ sha
        blt             3f
        @ begin of inner loop
2:      @ handle remaining >= 16 pixels
        pld             [lr, #32]
        vld1.32         {q8},  [lr]!
        vld1.32         {q9},  [lr]!
        vld1.32         {q10}, [lr]!
        vld1.32         {q11}, [lr]!
        vorr            q8,  q8,  q0
        vorr            q9,  q9,  q0
        vorr            q10, q10, q0
        vorr            q11, q11, q0
        vst1.32         {q8},  [r6]!
        vst1.32         {q9},  [r6]!
        vst1.32         {q10}, [r6]!
        vst1.32         {q11}, [r6]!
        sub             r12, r12, #16
        cmp             r12, #16
        bge             2b
3:      @ handle remaining >= 8 pixels
        cmp             r12, #4
        blt             4f
        vld1.32         {q8},  [lr]!
        vld1.32         {q9},  [lr]!
        vorr            q8,  q8,  q0
        vorr            q9,  q9,  q0
        vst1.32         {q8},  [r6]!
        vst1.32         {q9},  [r6]!
        sub             r12,  r12, #4
4:      @ handle remaining >= 4 pixels
        cmp             r12, #4
        blt             5f
        vld1.32         {q1}, [lr]!
        vorr            q1,   q1,  q0
        vst1.32         {q1}, [r6]!
        sub             r12,  r12, #4
5:      @ handle remaining >= 2 pixels
        cmp             r12, #2
        blt             6f
        vld1.32         {d4}, [lr]!
        vorr            d4,   d4,  d0
        vst1.32         {d4}, [r6]!
        sub             r12,  r12, #2
6:      @ handle remaining >= 1 pixels
        cmp             r12, #1
        blt             7f
        vld1.32         {d4[0]}, [lr]!
        vorr            d4,   d4,  d0
        vst1.32         {d4[0]}, [r6]!
7:
        sub             r5,  r5,  #1
        cmp             r5,  #0
        add             r0,  r0,  r1             @ winBase += winStride
        add             r2,  r2,  r3             @ shaBase += shaStride
        bgt             1b
        pop             {r4,r5,r6,pc}
@}

