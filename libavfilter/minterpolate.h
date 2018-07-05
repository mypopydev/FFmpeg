/*
 * Copyright (c) 2018 Jun Zhao <mypopy@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVFILTER_MINTERPOLATE_H
#define AVFILTER_MINTERPOLATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct MIDSPContext {
    uint64_t (*sse_sad)(uint8_t *src, int src_stride, uint8_t *dst, int dst_stride);
} MIDSPContext;

void ff_mi_init_x86(MIDSPContext *dsp);

#endif /* AVFILTER_MINTERPOLATE_H */
