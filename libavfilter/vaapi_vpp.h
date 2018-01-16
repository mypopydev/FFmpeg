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

#ifndef AVFILTER_VAAPI_VPP_H
#define AVFILTER_VAAPI_VPP_H

#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vaapi.h"

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
    int output_width;   // computed width
    int output_height;  // computed height

    VABufferID         filter_buffers[VAProcFilterCount];
    int                nb_filter_buffers;

    int (*build_filter_params)(AVFilterContext *avctx);

    int (*pipeline_uninit)(AVFilterContext *avctx);

    void (*free)(struct VAAPIVPPContext *ctx);

    /**
     * Arbitrary user data, to be used e.g. by the free() callback.
     */
    void *priv;

} VAAPIVPPContext;

void vaapi_vpp_ctx_init(VAAPIVPPContext *ctx, size_t  priv_size);

void vaapi_vpp_ctx_uninit(AVFilterContext *avctx);

int vaapi_vpp_query_formats(AVFilterContext *avctx);

void vaapi_vpp_pipeline_uninit(VAAPIVPPContext *ctx);

int vaapi_vpp_config_input(AVFilterLink *inlink);

int vaapi_vpp_config_output(AVFilterLink *outlink);

int vaapi_vpp_colour_standard(enum AVColorSpace av_cs);

int vaapi_vpp_make_param_buffers(VAAPIVPPContext *ctx,
                                 int type,
                                 const void *data,
                                 size_t size,
                                 int count);

int vaapi_vpp_render_picture(VAAPIVPPContext *ctx,
                             VAProcPipelineParameterBuffer *params,
                             VASurfaceID output_surface);

#endif /* AVFILTER_VAAPI_VPP_H */
