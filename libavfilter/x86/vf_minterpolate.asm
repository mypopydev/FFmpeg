;*****************************************************************************
;* Copyright (c) 2018 Jun Zhao <mypopydev@gmail.com> 
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
;****************************************************************************

%include "libavutil/x86/x86util.asm"

SECTION .text

;; INIT_XMM sse2
;; cglobal sad_16x16, 4, 7, 5, src, src_stride, dst, dst_stride, \
;;                             src_stride3, dst_stride3, cnt
;;     lea    src_stride3q, [src_strideq*3]
;;     lea    dst_stride3q, [dst_strideq*3]
;;     mov            cntd, 4
;;     pxor             m0, m0
;; .loop:
;;     mova             m1, [srcq+src_strideq*0]
;;     mova             m2, [srcq+src_strideq*1]
;;     mova             m3, [srcq+src_strideq*2]
;;     mova             m4, [srcq+src_stride3q]
;;     lea            srcq, [srcq+src_strideq*4]
;;     psadbw           m1, [dstq+dst_strideq*0]
;;     psadbw           m2, [dstq+dst_strideq*1]
;;     psadbw           m3, [dstq+dst_strideq*2]
;;     psadbw           m4, [dstq+dst_stride3q]
;;     lea            srcq, [dstq+dst_strideq*4]
;;     paddw            m1, m2
;;     paddw            m3, m4
;;     paddw            m0, m1
;;     paddw            m0, m3
;;     dec            cntd
;;     jg .loop
;;     movhlps          m1, m0
;;     paddw            m0, m1
;;     movd            eax, m0
;;     RET

INIT_XMM sse2
cglobal sad_16x16, 4, 7, 9, src, src_stride, dst, dst_stride, \
        src_stride3, dst_stride3, cnt
     lea    src_stride3q, [src_strideq*3]
     lea    dst_stride3q, [dst_strideq*3]
     mov            cntd, 4
     pxor             m0, m0
.loop:
     movu             m1, [srcq+src_strideq*0]
     movu             m2, [srcq+src_strideq*1]
     movu             m3, [srcq+src_strideq*2]
     movu             m4, [srcq+src_stride3q]
	lea            srcq, [srcq+src_strideq*4]
	
     movu             m5, [dstq+dst_strideq*0]
     movu             m6, [dstq+dst_strideq*1]
     movu             m7, [dstq+dst_strideq*2]
     movu             m8, [dstq+dst_stride3q]
     psadbw           m1, m5
     psadbw           m2, m6
     psadbw           m3, m7
     psadbw           m4, m8
     lea            dstq, [dstq+dst_strideq*4]
     paddw            m1, m2
     paddw            m3, m4
     paddw            m0, m1
     paddw            m0, m3
     dec            cntd
     jg .loop
	
     movhlps          m1, m0
     paddw            m0, m1
     movd            eax, m0
     RET 
	

;; %macro SAD_FN 4
;; %if %4 == 0
;; %if %3 == 5
;; cglobal sad%1x%2, 4, %3, 5, src, src_stride, ref, ref_stride, n_rows
;; %else ; %3 == 7
;; cglobal sad%1x%2, 4, %3, 6, src, src_stride, ref, ref_stride, \
;;                             src_stride3, ref_stride3, n_rows
;; %endif ; %3 == 5/7
;; %else ; avg
;; %if %3 == 5
;; cglobal sad%1x%2_avg, 5, 1 + %3, 5, src, src_stride, ref, ref_stride, \
;;                                     second_pred, n_rows
;; %else ; %3 == 7
;; cglobal sad%1x%2_avg, 5, ARCH_X86_64 + %3, 6, src, src_stride, \
;;                                               ref, ref_stride, \
;;                                               second_pred, \
;;                                               src_stride3, ref_stride3
;; %if ARCH_X86_64
;; %define n_rowsd r7d
;; %else ; x86-32
;; %define n_rowsd dword r0m
;; %endif ; x86-32/64
;; %endif ; %3 == 5/7
;; %endif ; avg/sad
;;   movsxdifnidn src_strideq, src_strided
;;   movsxdifnidn ref_strideq, ref_strided
;; %if %3 == 7
;;   lea         src_stride3q, [src_strideq*3]
;;   lea         ref_stride3q, [ref_strideq*3]
;; %endif ; %3 == 7
;; %endmacro
	
;; INIT_XMM sse2
;; ;; SAD16XN 32 ; sad16x32_sse2
;; ;; SAD16XN 16 ; sad16x16_sse2
;; ;; SAD16XN  8 ; sad16x8_sse2
;; ;; SAD16XN 32, 1 ; sad16x32_avg_sse2
;; ;; SAD16XN 16, 1 ; sad16x16_avg_sse2
;; ;; SAD16XN  8, 1 ; sad16x8_avg_sse2

;; ; unsigned int vpx_sad8x{8,16}_sse2(uint8_t *src, int src_stride,
;; ;                                   uint8_t *ref, int ref_stride);
;; %macro SAD8XN 1-2 0
;;   SAD_FN 8, %1, 7, %2
;;   mov              n_rowsd, %1/4
;;   pxor                  m0, m0

;; .loop:
;;   movh                  m1, [refq]
;;   movhps                m1, [refq+ref_strideq]
;;   movh                  m2, [refq+ref_strideq*2]
;;   movhps                m2, [refq+ref_stride3q]
	
;;   movh                  m3, [srcq]
;;   movhps                m3, [srcq+src_strideq]
;;   movh                  m4, [srcq+src_strideq*2]
;;   movhps                m4, [srcq+src_stride3q]
	
;;   psadbw                m1, m3
;;   psadbw                m2, m4
	
;;   lea                 refq, [refq+ref_strideq*4]
;;   paddd                 m0, m1
;;   lea                 srcq, [srcq+src_strideq*4]
;;   paddd                 m0, m2
;;   dec              n_rowsd
;;   jg .loop

;;   movhlps               m1, m0
;;   paddd                 m0, m1
;;   movd                 eax, m0
;;   RET
;; %endmacro
