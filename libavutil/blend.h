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
#ifndef AVUTIL_BLEND_H
#define AVUTIL_BLEND_H

#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavutil/mem.h"
#include "libavutil/x86/asm.h"

/**
 * Global alpha blending by row
 *
 * dst[i] = (src[i]*alpha[0]+(255-alpha[0])*src1[i]+255)>>8
 */
void av_global_blend_row(const uint8_t *src0,
                         const uint8_t *src1,
                         const uint8_t *alpha, /* XXX: only use alpha[0] */
                         uint8_t *dst,
                         int width);

/**
 * Per-pixel alpha blending by row
 *
 * dst[i] = (src[i]*alpha[i]+(255-alpha[i])*src1[i]+255)>>8
 */
void av_per_pixel_blend_row(const uint8_t *src0,
                            const uint8_t *src1,
                            const uint8_t *alpha,
                            uint8_t *dst,
                            int width);
#endif
