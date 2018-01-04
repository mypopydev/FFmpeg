/*
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

#ifndef AVFILTER_VF_VPP_VAAPI_H
#define AVFILTER_VF_VPP_VAAPI_H

#include <va/va.h>
#include <va/va_vpp.h>

#include "libavutil/hwcontext.h"

#include "avfilter.h"

typedef struct VAAPIVPPContext {
    const AVClass *class;

    AVVAAPIDeviceContext *hwctx;
    AVBufferRef *device_ref;

    int valid_ids;
    VAConfigID  va_config;
    VAContextID va_context;

    AVBufferRef       *input_frames_ref;
    AVHWFramesContext *input_frames;

    AVBufferRef       *output_frames_ref;
    AVHWFramesContext *output_frames;

    enum AVPixelFormat output_format;

    int                output_width;  // computed width
    int                output_height; // computed height

    int                num_filter_bufs;
    VABufferID         filter_bufs[VAProcFilterCount];
} VAAPIVPPContext;

int ff_vaapi_query_formats(AVFilterContext *avctx);

int ff_vaapi_vpp_make_param_buffer(VAAPIVPPContext *ctx,
                                      int type,
                                      const void *data,
                                      size_t size);

int ff_vaapi_vpp_make_param_array(VAAPIVPPContext *ctx,
                                      int type, size_t count,
                                      const void *data,
                                      size_t size);

int vaapi_vpp_output_surface_ready(VAAPIVPPContext *ctx, VASurfaceID output_surface);

int vaapi_vpp_make_pipeline_param(VAAPIVPPContext *ctx, VAProcPipelineParameterBuffer *params);

int vaapi_vpp_apply_pipeline_param(VAAPIVPPContext *ctx);

int ff_vaapi_vpp_destroy_param_buffer(VAAPIVPPContext *ctx);

int vaapi_vpp_init(VAAPIVPPContext *ctx);

int vaapi_vpp_filter_frame(VAAPIVPPContext *ctx, AVFrame *input_frame, AVFrame *output_frame);

void vaapi_vpp_uninit(VAAPIVPPContext *ctx);

int vaapi_vpp_config_input(VAAPIVPPContext *ctx, AVFilterLink *inlink);

int vaapi_vpp_config_input1(VAAPIVPPContext *ctx, AVFilterLink *inlink);

int vaapi_vpp_config_output(VAAPIVPPContext *ctx);

int vaapi_proc_colour_standard(enum AVColorSpace av_cs);
#endif
