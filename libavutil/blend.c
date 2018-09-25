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

#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavutil/mem.h"
#include "libavutil/x86/asm.h"
#include "libavutil/blend.h"

#include "libavutil/x86/blend.h"

static void ff_global_blend_row_c(const uint8_t *src0,
                                  const uint8_t *src1,
                                  const uint8_t *alpha, /* XXX: only use alpha[0] */
                                  uint8_t *dst,
                                  int width)
{
    int x;
    for (x = 0; x < width - 1; x += 2) {
        dst[0] = (src0[0] * alpha[0] + src1[0] * (255 - alpha[0]) + 255) >> 8;
        dst[1] = (src0[1] * alpha[0] + src1[1] * (255 - alpha[0]) + 255) >> 8;
        src0 += 2;
        src1 += 2;
        dst  += 2;
    }
    if (width & 1) {
        dst[0] = (src0[0] * alpha[0] + src1[0] * (255 - alpha[0]) + 255) >> 8;
    }
}

void av_global_blend_row(const uint8_t *src0,
                         const uint8_t *src1,
                         const uint8_t *alpha,
                         uint8_t *dst,
                         int width)
{
    blend_row blend_row_fn = NULL;

#if ARCH_X86
    blend_row_fn = ff_blend_row_init_x86(1);
#endif

    if (!blend_row_fn)
        blend_row_fn = ff_global_blend_row_c;

    blend_row_fn(src0, src1, alpha, dst, width);
}

static void ff_per_pixel_blend_row_c(const uint8_t *src0,
                                     const uint8_t *src1,
                                     const uint8_t *alpha,
                                     uint8_t *dst,
                                     int width)
{
    int x;
    for (x = 0; x < width - 1; x += 2) {
        dst[0] = (src0[0] * alpha[0] + src1[0] * (255 - alpha[0]) + 255) >> 8;
        dst[1] = (src0[1] * alpha[0] + src1[1] * (255 - alpha[0]) + 255) >> 8;
        src0 += 2;
        src1 += 2;
        dst  += 2;
        alpha+= 2;
    }
    if (width & 1) {
        dst[0] = (src0[0] * alpha[0] + src1[0] * (255 - alpha[0]) + 255) >> 8;
    }
}

void av_per_pixel_blend_row(const uint8_t *src0,
                            const uint8_t *src1,
                            const uint8_t *alpha,
                            uint8_t *dst,
                            int width)
{
    blend_row blend_row_fn = NULL;

#if ARCH_X86
    blend_row_fn = ff_blend_row_init_x86(0);
#endif

    if (!blend_row_fn)
        blend_row_fn = ff_per_pixel_blend_row_c;

    blend_row_fn(src0, src1, alpha, dst, width);
}

