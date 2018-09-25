/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "libavutil/cpu.h"
#include "libavutil/mem.h"
#include "libavutil/x86/cpu.h"
#include "libavutil/x86/asm.h"
#include "libavutil/x86/blend.h"

#if HAVE_SSSE3_INLINE && HAVE_6REGS
// per-pixel blend (8 pixels at a time.)
// dst[i] = ((src0[i]*alpah[i])+(src1[i]*(255-alpha[i]))+255)/256
static void ff_per_pixel_blend_row_ssse3(const uint8_t *src0,
                                         const uint8_t *src1,
                                         const uint8_t *alpha,
                                         uint8_t *dst,
                                         int width)
{
    int aligned_w = width/8 * 8;
    int width_u = width - aligned_w;
    uint8_t *src0_u  = (uint8_t *)src0 + aligned_w;
    uint8_t *src1_u  = (uint8_t *)src1 + aligned_w;
    uint8_t *alpha_u = (uint8_t *)alpha + aligned_w;
    uint8_t *dst_u  = dst + aligned_w;
    int i;

    if (aligned_w > 0) {
        __asm__ volatile(
            "pcmpeqb    %%xmm3,%%xmm3                  \n\t"
            "psllw      $0x8,%%xmm3                    \n\t"
            "mov        $0x80808080,%%eax              \n\t"
            "movd       %%eax,%%xmm3                   \n\t"
            "pshufd     $0x0,%%xmm4,%%xmm4             \n\t"
            "mov        $0x807f807f,%%eax              \n\t"
            "movd       %%eax,%%xmm5                   \n\t"
            "pshufd     $0x0,%%xmm5,%%xmm5             \n\t"
            "sub        %2,%0                          \n\t"
            "sub        %2,%1                          \n\t"
            "sub        %2,%3                          \n\t"

            // 8 pixel per loop.
            "1:                                        \n\t"
            "movq       (%2),%%xmm0                    \n\t"
            "punpcklbw  %%xmm0,%%xmm0                  \n\t"
            "pxor       %%xmm3,%%xmm0                  \n\t"
            "movq       (%0,%2,1),%%xmm1               \n\t"
            "movq       (%1,%2,1),%%xmm2               \n\t"
            "punpcklbw  %%xmm2,%%xmm1                  \n\t"
            "psubb      %%xmm4,%%xmm1                  \n\t"
            "pmaddubsw  %%xmm1,%%xmm0                  \n\t"
            "paddw      %%xmm5,%%xmm0                  \n\t"
            "psrlw      $0x8,%%xmm0                    \n\t"
            "packuswb   %%xmm0,%%xmm0                  \n\t"
            "movq       %%xmm0,(%3,%2,1)               \n\t"
            "lea        0x8(%2),%2                     \n\t"
            "sub        $0x8,%4                        \n\t"
            "jg        1b                              \n\t"
            : "+r"(src0),       // %0
              "+r"(src1),       // %1
              "+r"(alpha),      // %2
              "+r"(dst),        // %3
              "+rm"(aligned_w)  // %4
            ::"memory",
             "cc", "eax", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5");
    }

    for (i = 0; i < width_u - 1; i += 2) {
        dst_u[0] = (src0_u[0] * alpha_u[0] + src1_u[0] * (255 - alpha_u[0]) + 255) >> 8;
        dst_u[1] = (src0_u[1] * alpha_u[0] + src1_u[1] * (255 - alpha_u[0]) + 255) >> 8;
        src0_u += 2;
        src1_u += 2;
        dst_u  += 2;
        alpha_u+= 2;
    }
    if (width_u & 1) {
        dst_u[0] = (src0_u[0] * alpha_u[0] + src1_u[0] * (255 - alpha_u[0]) + 255) >> 8;
    }
}

// global blend (8 pixels at a time).
// dst[i] = ((src0[i]*alpah[0])+(src1[i]*(255-alpha[0]))+255)/256
static void ff_global_blend_row_ssse3(const uint8_t *src0,
                                      const uint8_t *src1,
                                      const uint8_t *alpha,
                                      uint8_t *dst,
                                      int width)
{
    int aligned_w = width/8 * 8;
    int width_u = width - aligned_w;
    uint8_t *src0_u = (uint8_t *)src0 + aligned_w;
    uint8_t *src1_u = (uint8_t *)src1 + aligned_w;
    uint8_t *dst_u  = dst + aligned_w;
    int i;

    if (aligned_w > 0) {
        __asm__ volatile(
            "pcmpeqb    %%xmm3,%%xmm3                  \n\t"
            "psllw      $0x8,%%xmm3                    \n\t"
            "mov        $0x80808080,%%eax              \n\t"
            "movd       %%eax,%%xmm4                   \n\t"
            "pshufd     $0x0,%%xmm4,%%xmm4             \n\t"
            "mov        $0x807f807f,%%eax              \n\t"
            "movd       %%eax,%%xmm5                   \n\t"
            "pshufd     $0x0,%%xmm5,%%xmm5             \n\t"
            // a => xmm6 [a a a a a a a a a a a a a a a a ]
            "movb       (%2),%%al                      \n\t"
            "movd       %%eax,%%xmm6                   \n\t" // xmm6 = x x x x x x x x x x x x x x x a
            "punpcklbw  %%xmm6,%%xmm6                  \n\t" // xmm6 = x x x x x x x x x x x x x x a a
            "punpcklbw  %%xmm6,%%xmm6                  \n\t" // xmm6 = x x x x x x x x x x x x a a a a
            "punpcklbw  %%xmm6,%%xmm6                  \n\t" // xmm6 = x x x x x x x x a a a a a a a a
            "punpcklbw  %%xmm6,%%xmm6                  \n\t" // xmm6 = a a a a a a a a a a a a a a a a

            // 8 pixel per loop.
            "1:                                        \n\t"
            "movdqu     %%xmm6,%%xmm0                  \n\t" // xmm0 = xmm6
            "pxor       %%xmm3,%%xmm0                  \n\t"

            "movq       (%0),%%xmm1                    \n\t"
            "movq       (%1),%%xmm2                    \n\t"
            "punpcklbw  %%xmm2,%%xmm1                  \n\t"
            "psubb      %%xmm4,%%xmm1                  \n\t"

            "pmaddubsw  %%xmm1,%%xmm0                  \n\t"
            "paddw      %%xmm5,%%xmm0                  \n\t"
            "psrlw      $0x8,%%xmm0                    \n\t"
            "packuswb   %%xmm0,%%xmm0                  \n\t"
            "movq       %%xmm0,(%3)                    \n\t"

            "lea        0x8(%0),%0                     \n\t" // src0+8
            "lea        0x8(%1),%1                     \n\t" // src1+8
            "lea        0x8(%3),%3                     \n\t" // dst+8
            "sub        $0x8,%4                        \n\t"
            "jg        1b                              \n\t"
            : "+r"(src0),       // %0
              "+r"(src1),       // %1
              "+r"(alpha),      // %2
              "+r"(dst),        // %3
              "+rm"(aligned_w)  // %4
            ::"memory",
             "cc", "eax", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6");
    }

    for (i = 0; i < width_u - 1; i += 2) {
        dst_u[0] = (src0_u[0] * alpha[0] + src1_u[0] * (255 - alpha[0]) + 255) >> 8;
        dst_u[1] = (src0_u[1] * alpha[0] + src1_u[1] * (255 - alpha[0]) + 255) >> 8;
        src0_u += 2;
        src1_u += 2;
        dst_u  += 2;
    }
    if (width_u & 1) {
        dst_u[0] = (src0_u[0] * alpha[0] + src1_u[0] * (255 - alpha[0]) + 255) >> 8;
    }
}
#endif

#if HAVE_AVX2_INLINE && HAVE_6REGS
// per-pixe blend (32 pixels at a time).
// dst[i] = ((src0[i]*alpah[i])+(src1[i]*(255-alpha[i]))+255)/256
static void ff_per_pixel_blend_row_avx2(const uint8_t *src0,
                                        const uint8_t *src1,
                                        const uint8_t *alpha,
                                        uint8_t *dst,
                                        int width)
{
    int aligned_w = width/32 * 32;
    int width_u = width - aligned_w;
    uint8_t *src0_u  = (uint8_t *)src0 + aligned_w;
    uint8_t *src1_u  = (uint8_t *)src1 + aligned_w;
    uint8_t *alpha_u = (uint8_t *)alpha + aligned_w;
    uint8_t *dst_u  = dst + aligned_w;
    int i;

    if (aligned_w > 0) {
        __asm__ volatile(
            "vpcmpeqb   %%ymm5,%%ymm5,%%ymm5           \n\t"
            "vpsllw     $0x8,%%ymm5,%%ymm5             \n\t"
            "mov        $0x80808080,%%eax              \n\t"
            "vmovd      %%eax,%%xmm6                   \n\t"
            "vbroadcastss %%xmm6,%%ymm6                \n\t"
            "mov        $0x807f807f,%%eax              \n\t"
            "vmovd      %%eax,%%xmm7                   \n\t"
            "vbroadcastss %%xmm7,%%ymm7                \n\t"
            "sub        %2,%0                          \n\t"
            "sub        %2,%1                          \n\t"
            "sub        %2,%3                          \n\t"

            // 32 pixel per loop.
            "1:                                        \n\t"
            "vmovdqu    (%2),%%ymm0                    \n\t"
            "vpunpckhbw %%ymm0,%%ymm0,%%ymm3           \n\t"
            "vpunpcklbw %%ymm0,%%ymm0,%%ymm0           \n\t"
            "vpxor      %%ymm5,%%ymm3,%%ymm3           \n\t"
            "vpxor      %%ymm5,%%ymm0,%%ymm0           \n\t"
            "vmovdqu    (%0,%2,1),%%ymm1               \n\t"
            "vmovdqu    (%1,%2,1),%%ymm2               \n\t"
            "vpunpckhbw %%ymm2,%%ymm1,%%ymm4           \n\t"
            "vpunpcklbw %%ymm2,%%ymm1,%%ymm1           \n\t"
            "vpsubb     %%ymm6,%%ymm4,%%ymm4           \n\t"
            "vpsubb     %%ymm6,%%ymm1,%%ymm1           \n\t"
            "vpmaddubsw %%ymm4,%%ymm3,%%ymm3           \n\t"
            "vpmaddubsw %%ymm1,%%ymm0,%%ymm0           \n\t"
            "vpaddw     %%ymm7,%%ymm3,%%ymm3           \n\t"
            "vpaddw     %%ymm7,%%ymm0,%%ymm0           \n\t"
            "vpsrlw     $0x8,%%ymm3,%%ymm3             \n\t"
            "vpsrlw     $0x8,%%ymm0,%%ymm0             \n\t"
            "vpackuswb  %%ymm3,%%ymm0,%%ymm0           \n\t"
            "vmovdqu    %%ymm0,(%3,%2,1)               \n\t"
            "lea        0x20(%2),%2                    \n\t"
            "sub        $0x20,%4                       \n\t"
            "jg        1b                              \n\t"
            "vzeroupper                                \n\t"
            : "+r"(src0),      // %0
              "+r"(src1),      // %1
              "+r"(alpha),     // %2
              "+r"(dst),       // %3
              "+rm"(aligned_w) // %4
            ::"memory",
             "cc", "eax", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
             "xmm7");
    }

    for (i = 0; i < width_u - 1; i += 2) {
        dst_u[0] = (src0_u[0] * alpha_u[0] + src1_u[0] * (255 - alpha_u[0]) + 255) >> 8;
        dst_u[1] = (src0_u[1] * alpha_u[0] + src1_u[1] * (255 - alpha_u[0]) + 255) >> 8;
        src0_u += 2;
        src1_u += 2;
        dst_u  += 2;
        alpha_u+= 2;
    }
    if (width_u & 1) {
        dst_u[0] = (src0_u[0] * alpha_u[0] + src1_u[0] * (255 - alpha_u[0]) + 255) >> 8;
    }
}

// global blend (32 pixels at a time)
// dst[i] = ((src0[i]*alpah[0])+(src1[i]*(255-alpha[0]))+255)/256
static void ff_global_blend_row_avx2(const uint8_t *src0,
                                     const uint8_t *src1,
                                     const uint8_t *alpha,
                                     uint8_t *dst,
                                     int width)
{
    int aligned_w = width/32 * 32;
    int width_u = width - aligned_w;
    uint8_t *src0_u = (uint8_t *)src0 + aligned_w;
    uint8_t *src1_u = (uint8_t *)src1 + aligned_w;
    uint8_t *dst_u  = dst + aligned_w;
    int i;

    if (aligned_w > 0) {
        __asm__ volatile(
            "vpcmpeqb   %%ymm5,%%ymm5,%%ymm5           \n\t"
            "vpsllw     $0x8,%%ymm5,%%ymm5             \n\t"
            "mov        $0x80808080,%%eax              \n\t"
            "vmovd      %%eax,%%xmm6                   \n\t"
            "vbroadcastss %%xmm6,%%ymm6                \n\t"
            "mov        $0x807f807f,%%eax              \n\t"
            "vmovd      %%eax,%%xmm7                   \n\t"
            "vbroadcastss %%xmm7,%%ymm7                \n\t"
            // a => ymm8 [a a a a a a a a a a a a a a a a
            //            a a a a a a a a a a a a a a a a
            //            a a a a a a a a a a a a a a a a
            //            a a a a a a a a a a a a a a a a]
            "movb       (%2),%%al                      \n\t"
            "movd       %%eax,%%xmm8                   \n\t" // xmm8 = x x x x x x x x x x x x x x x a
            "punpcklbw  %%xmm8,%%xmm8                  \n\t" // xmm8 = x x x x x x x x x x x x x x a a
            "punpcklbw  %%xmm8,%%xmm8                  \n\t" // xmm8 = x x x x x x x x x x x x a a a a
            "vbroadcastss %%xmm8,%%ymm8                \n\t"

            // 32 pixel per loop.
            "1:                                        \n\t"
            "vmovdqu    %%ymm8,%%ymm0                  \n\t"
            "vpunpckhbw %%ymm0,%%ymm0,%%ymm3           \n\t"
            "vpunpcklbw %%ymm0,%%ymm0,%%ymm0           \n\t"
            "vpxor      %%ymm5,%%ymm3,%%ymm3           \n\t"
            "vpxor      %%ymm5,%%ymm0,%%ymm0           \n\t"

            "vmovdqu    (%0),%%ymm1                    \n\t"
            "vmovdqu    (%1),%%ymm2                    \n\t"
            "vpunpckhbw %%ymm2,%%ymm1,%%ymm4           \n\t"
            "vpunpcklbw %%ymm2,%%ymm1,%%ymm1           \n\t"
            "vpsubb     %%ymm6,%%ymm4,%%ymm4           \n\t"
            "vpsubb     %%ymm6,%%ymm1,%%ymm1           \n\t"
            "vpmaddubsw %%ymm4,%%ymm3,%%ymm3           \n\t"
            "vpmaddubsw %%ymm1,%%ymm0,%%ymm0           \n\t"
            "vpaddw     %%ymm7,%%ymm3,%%ymm3           \n\t"
            "vpaddw     %%ymm7,%%ymm0,%%ymm0           \n\t"
            "vpsrlw     $0x8,%%ymm3,%%ymm3             \n\t"
            "vpsrlw     $0x8,%%ymm0,%%ymm0             \n\t"
            "vpackuswb  %%ymm3,%%ymm0,%%ymm0           \n\t"

            "vmovdqu    %%ymm0,(%3)                    \n\t"
            "lea        0x20(%0),%0                    \n\t"
            "lea        0x20(%1),%1                    \n\t"
            "lea        0x20(%3),%3                    \n\t"
            "sub        $0x20,%4                       \n\t"
            "jg        1b                              \n\t"
            "vzeroupper                                \n\t"
            : "+r"(src0),       // %0
              "+r"(src1),       // %1
              "+r"(alpha),      // %2
              "+r"(dst),        // %3
              "+rm"(aligned_w)  // %4
            ::"memory",
             "cc", "eax", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
             "xmm7", "xmm8");
    }

    for (i = 0; i < width_u - 1; i += 2) {
        dst_u[0] = (src0_u[0] * alpha[0] + src1_u[0] * (255 - alpha[0]) + 255) >> 8;
        dst_u[1] = (src0_u[1] * alpha[0] + src1_u[1] * (255 - alpha[0]) + 255) >> 8;
        src0_u += 2;
        src1_u += 2;
        dst_u  += 2;
    }
    if (width_u & 1) {
        dst_u[0] = (src0_u[0] * alpha[0] + src1_u[0] * (255 - alpha[0]) + 255) >> 8;
    }
}
#endif

av_cold blend_row ff_blend_row_init_x86(int global)
{
    blend_row blend_row_fn = NULL;
    int cpu_flags = av_get_cpu_flags();

    if (global) {
#if HAVE_SSSE3_INLINE && HAVE_6REGS
        if (EXTERNAL_SSSE3(cpu_flags)) {
            blend_row_fn = ff_global_blend_row_ssse3;
        }
#endif

#if HAVE_AVX2_INLINE && HAVE_6REGS
        if (EXTERNAL_AVX2_FAST(cpu_flags)) {
            blend_row_fn = ff_global_blend_row_avx2;
        }
#endif
    } else {
#if HAVE_SSSE3_INLINE && HAVE_6REGS
        if (EXTERNAL_SSSE3(cpu_flags)) {
            blend_row_fn = ff_per_pixel_blend_row_ssse3;
        }
#endif

#if HAVE_AVX2_INLINE && HAVE_6REGS
        if (EXTERNAL_AVX2_FAST(cpu_flags)) {
            blend_row_fn = ff_per_pixel_blend_row_avx2;
        }
#endif
    }

    return blend_row_fn;
}
