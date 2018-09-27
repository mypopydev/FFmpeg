;******************************************************************************
;* Pixel utilities SIMD
;*
;* Copyright (C) 2002-2004 Michael Niedermayer <michaelni@gmx.at>
;* Copyright (C) 2014 Clément Bœsch <u pkh me>
;*
;* This file is part of FFmpeg.
;*
;* FFmpeg is free software; you can redistribute it and/or
;* modify it under the terms of the GNU Lesser General Public
;* License as published by the Free Software Foundation; either
;* version 2.1 of the License, or (at your option) any later version.
;*
;* FFmpeg is distributed in the hope that it will be useful,
;* but WITHOUT ANY WARRANTY; without even the implied warranty of
;* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;* Lesser General Public License for more details.
;*
;* You should have received a copy of the GNU Lesser General Public
;* License along with FFmpeg; if not, write to the Free Software
;* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
;******************************************************************************

%include "x86util.asm"

SECTION .text

cextern pw_1
cextern pw_4
cextern pw_8

;-------------------------------------------------------------------------------
; int ff_pixelutils_sad_8x8_mmx(const uint8_t *src1, ptrdiff_t stride1,
;                               const uint8_t *src2, ptrdiff_t stride2);
;-------------------------------------------------------------------------------
INIT_MMX mmx
cglobal pixelutils_sad_8x8, 4,4,0, src1, stride1, src2, stride2
    pxor        m7, m7
    pxor        m6, m6
%rep 4
    mova        m0, [src1q]
    mova        m2, [src1q + stride1q]
    mova        m1, [src2q]
    mova        m3, [src2q + stride2q]
    psubusb     m4, m0, m1
    psubusb     m5, m2, m3
    psubusb     m1, m0
    psubusb     m3, m2
    por         m1, m4
    por         m3, m5
    punpcklbw   m0, m1, m7
    punpcklbw   m2, m3, m7
    punpckhbw   m1, m7
    punpckhbw   m3, m7
    paddw       m0, m1
    paddw       m2, m3
    paddw       m0, m2
    paddw       m6, m0
    lea         src1q, [src1q + 2*stride1q]
    lea         src2q, [src2q + 2*stride2q]
%endrep
    psrlq       m0, m6, 32
    paddw       m6, m0
    psrlq       m0, m6, 16
    paddw       m6, m0
    movd        eax, m6
    movzx       eax, ax
    RET

;-------------------------------------------------------------------------------
; int ff_pixelutils_sad_8x8_mmxext(const uint8_t *src1, ptrdiff_t stride1,
;                                  const uint8_t *src2, ptrdiff_t stride2);
;-------------------------------------------------------------------------------
INIT_MMX mmxext
cglobal pixelutils_sad_8x8, 4,4,0, src1, stride1, src2, stride2
    pxor        m2, m2
%rep 4
    mova        m0, [src1q]
    mova        m1, [src1q + stride1q]
    psadbw      m0, [src2q]
    psadbw      m1, [src2q + stride2q]
    paddw       m2, m0
    paddw       m2, m1
    lea         src1q, [src1q + 2*stride1q]
    lea         src2q, [src2q + 2*stride2q]
%endrep
    movd        eax, m2
    RET

;-------------------------------------------------------------------------------
; int ff_pixelutils_sad_16x16_mmxext(const uint8_t *src1, ptrdiff_t stride1,
;                                    const uint8_t *src2, ptrdiff_t stride2);
;-------------------------------------------------------------------------------
INIT_MMX mmxext
cglobal pixelutils_sad_16x16, 4,4,0, src1, stride1, src2, stride2
    pxor        m2, m2
%rep 16
    mova        m0, [src1q]
    mova        m1, [src1q + 8]
    psadbw      m0, [src2q]
    psadbw      m1, [src2q + 8]
    paddw       m2, m0
    paddw       m2, m1
    add         src1q, stride1q
    add         src2q, stride2q
%endrep
    movd        eax, m2
    RET

;-------------------------------------------------------------------------------
; int ff_pixelutils_sad_16x16_sse2(const uint8_t *src1, ptrdiff_t stride1,
;                                  const uint8_t *src2, ptrdiff_t stride2);
;-------------------------------------------------------------------------------
INIT_XMM sse2
cglobal pixelutils_sad_16x16, 4,4,5, src1, stride1, src2, stride2
    movu        m4, [src1q]
    movu        m2, [src2q]
    movu        m1, [src1q + stride1q]
    movu        m3, [src2q + stride2q]
    psadbw      m4, m2
    psadbw      m1, m3
    paddw       m4, m1
%rep 7
    lea         src1q, [src1q + 2*stride1q]
    lea         src2q, [src2q + 2*stride2q]
    movu        m0, [src1q]
    movu        m2, [src2q]
    movu        m1, [src1q + stride1q]
    movu        m3, [src2q + stride2q]
    psadbw      m0, m2
    psadbw      m1, m3
    paddw       m4, m0
    paddw       m4, m1
%endrep
    movhlps     m0, m4
    paddw       m4, m0
    movd        eax, m4
    RET

;-------------------------------------------------------------------------------
; int ff_pixelutils_sad_[au]_16x16_sse2(const uint8_t *src1, ptrdiff_t stride1,
;                                       const uint8_t *src2, ptrdiff_t stride2);
;-------------------------------------------------------------------------------
%macro SAD_XMM_16x16 1
INIT_XMM sse2
cglobal pixelutils_sad_%1_16x16, 4,4,3, src1, stride1, src2, stride2
    mov%1       m2, [src2q]
    psadbw      m2, [src1q]
    mov%1       m1, [src2q + stride2q]
    psadbw      m1, [src1q + stride1q]
    paddw       m2, m1
%rep 7
    lea         src1q, [src1q + 2*stride1q]
    lea         src2q, [src2q + 2*stride2q]
    mov%1       m0, [src2q]
    psadbw      m0, [src1q]
    mov%1       m1, [src2q + stride2q]
    psadbw      m1, [src1q + stride1q]
    paddw       m2, m0
    paddw       m2, m1
%endrep
    movhlps     m0, m2
    paddw       m2, m0
    movd        eax, m2
    RET
%endmacro

SAD_XMM_16x16 a
SAD_XMM_16x16 u


%macro PROCESS_SAD_32x4_U 0
    movu    m1,  [r2]
    movu    m2,  [r2 + 16]
    movu    m3,  [r0]
    movu    m4,  [r0 + 16]
    psadbw  m1,  m3
    psadbw  m2,  m4
    paddd   m1,  m2
    paddd   m0,  m1
    lea     r2,  [r2 + r3]
    lea     r0,  [r0 + r1]

    movu    m1,  [r2]
    movu    m2,  [r2 + 16]
    movu    m3,  [r0]
    movu    m4,  [r0 + 16]
    psadbw  m1,  m3
    psadbw  m2,  m4
    paddd   m1,  m2
    paddd   m0,  m1
    lea     r2,  [r2 + r3]
    lea     r0,  [r0 + r1]

    movu    m1,  [r2]
    movu    m2,  [r2 + 16]
    movu    m3,  [r0]
    movu    m4,  [r0 + 16]
    psadbw  m1,  m3
    psadbw  m2,  m4
    paddd   m1,  m2
    paddd   m0,  m1
    lea     r2,  [r2 + r3]
    lea     r0,  [r0 + r1]

    movu    m1,  [r2]
    movu    m2,  [r2 + 16]
    movu    m3,  [r0]
    movu    m4,  [r0 + 16]
    psadbw  m1,  m3
    psadbw  m2,  m4
    paddd   m1,  m2
    paddd   m0,  m1
    lea     r2,  [r2 + r3]
    lea     r0,  [r0 + r1]
%endmacro

%macro PROCESS_SAD_32x4 1
    mov%1   m1,  [r2]
    mov%1   m2,  [r2 + 16]
    psadbw  m1,  [r0]
    psadbw  m2,  [r0 + 16]
    paddd   m1,  m2
    paddd   m0,  m1
    lea     r2,  [r2 + r3]
    lea     r0,  [r0 + r1]

    mov%1   m1,  [r2]
    mov%1   m2,  [r2 + 16]
    psadbw  m1,  [r0]
    psadbw  m2,  [r0 + 16]
    paddd   m1,  m2
    paddd   m0,  m1
    lea     r2,  [r2 + r3]
    lea     r0,  [r0 + r1]

    mov%1   m1,  [r2]
    mov%1   m2,  [r2 + 16]
    psadbw  m1,  [r0]
    psadbw  m2,  [r0 + 16]
    paddd   m1,  m2
    paddd   m0,  m1
    lea     r2,  [r2 + r3]
    lea     r0,  [r0 + r1]

    mov%1   m1,  [r2]
    mov%1   m2,  [r2 + 16]
    psadbw  m1,  [r0]
    psadbw  m2,  [r0 + 16]
    paddd   m1,  m2
    paddd   m0,  m1
    lea     r2,  [r2 + r3]
    lea     r0,  [r0 + r1]
%endmacro

;-----------------------------------------------------------------------------
; int ff_pixelutils_sad_32x32_sse2(const uint8_t *src1, ptrdiff_t stride1,
;                                  const uint8_t *src2, ptrdiff_t stride2);
;-----------------------------------------------------------------------------
INIT_XMM sse2
cglobal pixelutils_sad_32x32, 4,5,5, src1, stride1, src2, stride2
    pxor  m0,  m0
    mov   r4d, 4
.loop:
    PROCESS_SAD_32x4_U
    PROCESS_SAD_32x4_U
    dec r4d
    jnz .loop

    movhlps m1,  m0
    paddd   m0,  m1
    movd    eax, m0
    RET

;-------------------------------------------------------------------------------
; int ff_pixelutils_sad_[au]_32x32_sse2(const uint8_t *src1, ptrdiff_t stride1,
;                                       const uint8_t *src2, ptrdiff_t stride2);
;-------------------------------------------------------------------------------
%macro SAD_XMM_32x32 1
INIT_XMM sse2
cglobal pixelutils_sad_%1_32x32, 4,5,3, src1, stride1, src2, stride2
    pxor  m0,  m0
    mov   r4d, 4
.loop:
    PROCESS_SAD_32x4 %1
    PROCESS_SAD_32x4 %1
    dec r4d
    jnz .loop

    movhlps m1,  m0
    paddd   m0,  m1
    movd    eax, m0
    RET
%endmacro

SAD_XMM_32x32 a
SAD_XMM_32x32 u

%if HAVE_AVX2_EXTERNAL
;-------------------------------------------------------------------------------
; int ff_pixelutils_sad_32x32_avx2(const uint8_t *src1, ptrdiff_t stride1,
;                                  const uint8_t *src2, ptrdiff_t stride2);
;-------------------------------------------------------------------------------
INIT_YMM avx2
cglobal pixelutils_sad_32x32, 4,7,5, src1, stride1, src2, stride2
    pxor            m0, m0
    mov             r4d, 32/4
    lea             r5, [stride1q * 3]
    lea             r6, [stride2q * 3]

.loop:
    movu           m1, [src1q]               ; row 0 of pix0
    movu           m2, [src2q]               ; row 0 of pix1
    movu           m3, [src1q + stride1q]    ; row 1 of pix0
    movu           m4, [src2q + stride2q]    ; row 1 of pix1

    psadbw         m1, m2
    psadbw         m3, m4
    paddd          m0, m1
    paddd          m0, m3

    movu           m1, [src1q + 2 * stride1q] ; row 2 of pix0
    movu           m2, [src2q + 2 * stride2q] ; row 2 of pix1
    movu           m3, [src1q + r5]           ; row 3 of pix0
    movu           m4, [src2q + r6]           ; row 3 of pix1

    psadbw         m1, m2
    psadbw         m3, m4
    paddd          m0, m1
    paddd          m0, m3

    lea            src2q,     [src2q + 4 * stride2q]
    lea            src1q,     [src1q + 4 * stride1q]

    dec            r4d
    jnz           .loop

    vextracti128   xm1, m0, 1
    paddd          xm0, xm1
    pshufd         xm1, xm0, 2
    paddd          xm0, xm1
    movd           eax, xm0
    RET

;-------------------------------------------------------------------------------
; int ff_pixelutils_sad_[au]_32x32_avx2(const uint8_t *src1, ptrdiff_t stride1,
;                                       const uint8_t *src2, ptrdiff_t stride2);
;-------------------------------------------------------------------------------
%macro SAD_AVX2_32x32 1
INIT_YMM avx2
cglobal pixelutils_sad_%1_32x32, 4,7,3, src1, stride1, src2, stride2
    pxor           m0, m0
    mov            r4d, 32/4
    lea            r5, [stride1q * 3]
    lea            r6, [stride2q * 3]

.loop:
    mov%1          m1, [src2q]                ; row 0 of pix1
    psadbw         m1, [src1q]
    mov%1          m2, [src2q + stride2q]     ; row 1 of pix1
    psadbw         m2, [src1q + stride1q]

    paddd          m0, m1
    paddd          m0, m2

    mov%1          m1, [src2q + 2 * stride2q] ; row 2 of pix1
    psadbw         m1, [src1q + 2 * stride1q]
    mov%1          m2, [src2q + r6]           ; row 3 of pix1
    psadbw         m2, [src1q + r5]

    paddd          m0, m1
    paddd          m0, m2

    lea            src2q,     [src2q + 4 * stride2q]
    lea            src1q,     [src1q + 4 * stride1q]

    dec            r4d
    jnz           .loop

    vextracti128   xm1, m0, 1
    paddd          xm0, xm1
    pshufd         xm1, xm0, 2
    paddd          xm0, xm1
    movd           eax, xm0
    RET
%endmacro

SAD_AVX2_32x32 a
SAD_AVX2_32x32 u
%endif

%macro ABSW2 6 ; dst1, dst2, src1, src2, tmp, tmp
%if cpuflag(ssse3)
    pabsw   %1, %3
    pabsw   %2, %4
%elifidn %1, %3
    pxor    %5, %5
    pxor    %6, %6
    psubw   %5, %1
    psubw   %6, %2
    pmaxsw  %1, %5
    pmaxsw  %2, %6
%else
    pxor    %1, %1
    pxor    %2, %2
    psubw   %1, %3
    psubw   %2, %4
    pmaxsw  %1, %3
    pmaxsw  %2, %4
%endif
%endmacro

%macro HADDUWD 2
%if cpuflag(xop) && sizeof%1 == 16
    vphadduwd %1, %1
%else
    psrld %2, %1, 16
    pslld %1, 16
    psrld %1, 16
    paddd %1, %2
%endif
%endmacro

%macro HADDUW 2
%if cpuflag(xop) && sizeof%1 == 16
    vphadduwq %1, %1
    MOVHL     %2, %1
    paddd     %1, %2
%else
    HADDUWD   %1, %2
    HADDD     %1, %2
%endif
%endmacro
	
;=============================================================================
; SAD MMX
;=============================================================================

%macro SAD_INC_1x16P_MMX 0
    movu    m1, [r0+ 0]
    movu    m2, [r0+ 8]
    movu    m3, [r0+16]
    movu    m4, [r0+24]
    psubw   m1, [r2+ 0]
    psubw   m2, [r2+ 8]
    psubw   m3, [r2+16]
    psubw   m4, [r2+24]
    ABSW2   m1, m2, m1, m2, m5, m6
    ABSW2   m3, m4, m3, m4, m7, m5
    lea     r0, [r0+2*r1]
    lea     r2, [r2+2*r3]
    paddw   m1, m2
    paddw   m3, m4
    paddw   m0, m1
    paddw   m0, m3
%endmacro

%macro SAD_INC_2x8P_MMX 0
    movu    m1, [r0+0]
    movu    m2, [r0+8]
    movu    m3, [r0+2*r1+0]
    movu    m4, [r0+2*r1+8]
    psubw   m1, [r2+0]
    psubw   m2, [r2+8]
    psubw   m3, [r2+2*r3+0]
    psubw   m4, [r2+2*r3+8]
    ABSW2   m1, m2, m1, m2, m5, m6
    ABSW2   m3, m4, m3, m4, m7, m5
    lea     r0, [r0+4*r1]
    lea     r2, [r2+4*r3]
    paddw   m1, m2
    paddw   m3, m4
    paddw   m0, m1
    paddw   m0, m3
%endmacro

%macro SAD_INC_2x4P_MMX 0
    movu    m1, [r0]
    movu    m2, [r0+2*r1]
    psubw   m1, [r2]
    psubw   m2, [r2+2*r3]
    ABSW2   m1, m2, m1, m2, m3, m4
    lea     r0, [r0+4*r1]
    lea     r2, [r2+4*r3]
    paddw   m0, m1
    paddw   m0, m2
%endmacro

;----------------------------------------------------------------------------------
; int  ff_pixelutils_sad16_NxM_mmxext(const uint16_t *src1, ptrdiff_t stride1,
;                                     const uint16_t *src2, ptrdiff_t stride2);
;----------------------------------------------------------------------------------
%macro SAD16_MMX 3
cglobal pixelutils_sad16_%1x%2, 4,5-(%2&4/4)
    pxor    m0, m0
%if %2 == 4
    SAD_INC_%3x%1P_MMX
    SAD_INC_%3x%1P_MMX
%else
    mov    r4d, %2/%3
.loop:
    SAD_INC_%3x%1P_MMX
    dec    r4d
    jg .loop
%endif
%if %1*%2 == 256
    HADDUW  m0, m1
%else
    HADDW   m0, m1
%endif
    movd   eax, m0
    RET
%endmacro

INIT_MMX mmxext
SAD16_MMX 16, 16, 1
SAD16_MMX  8,  8, 2
SAD16_MMX  4,  4, 2
INIT_MMX ssse3
SAD16_MMX  4,  4, 2

;=============================================================================
; SAD XMM
;=============================================================================

%macro SAD_INC_2ROW 1
%if 2*%1 > mmsize
    movu    m1, [r2+ 0]
    movu    m2, [r2+16]
    movu    m3, [r2+2*r3+ 0]
    movu    m4, [r2+2*r3+16]
    psubw   m1, [r0+ 0]
    psubw   m2, [r0+16]
    psubw   m3, [r0+2*r1+ 0]
    psubw   m4, [r0+2*r1+16]
    ABSW2   m1, m2, m1, m2, m5, m6
    lea     r0, [r0+4*r1]
    lea     r2, [r2+4*r3]
    ABSW2   m3, m4, m3, m4, m7, m5
    paddw   m1, m2
    paddw   m3, m4
    paddw   m0, m1
    paddw   m0, m3
%else
    movu    m1, [r2]
    movu    m2, [r2+2*r3]
    psubw   m1, [r0]
    psubw   m2, [r0+2*r1]
    ABSW2   m1, m2, m1, m2, m3, m4
    lea     r0, [r0+4*r1]
    lea     r2, [r2+4*r3]
    paddw   m0, m1
    paddw   m0, m2
%endif
%endmacro

;---------------------------------------------------------------------------------------
; int  ff_pixelutils_sad16_NxM_{sse2,ssse3,avx2}(const uint16_t *src1, ptrdiff_t stride1,
;                                                const uint16_t *src2, ptrdiff_t stride2);
;---------------------------------------------------------------------------------------
%macro SAD 2
cglobal pixelutils_sad16_%1x%2, 4,5-(%2&4/4),8*(%1/mmsize)
    pxor    m0, m0
%if %2 == 4
    SAD_INC_2ROW %1
    SAD_INC_2ROW %1
%else
    mov    r4d, %2/2
.loop:
    SAD_INC_2ROW %1
    dec    r4d
    jg .loop
%endif
    HADDW   m0, m1
    movd   eax, xm0
    RET
%endmacro

INIT_XMM sse2
SAD 16, 16
SAD  8,  8
INIT_XMM sse2, aligned
SAD 16, 16
SAD  8,  8
INIT_XMM ssse3
SAD 16, 16
SAD  8,  8
INIT_XMM ssse3, aligned
SAD 16, 16
SAD  8,  8
INIT_YMM avx2
SAD 16, 16

