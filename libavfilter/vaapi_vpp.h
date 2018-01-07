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

typedef struct VAAPIVPPContext {
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
    int output_width;
    int output_height;

    VABufferID         filter_buffer;

    int (*build_filter_params)(AVFilterContext *avctx);

    int (*pipeline_uninit)(AVFilterContext *avctx);

    //int (*config_input)(AVFilterLink *inlink);

    //int (*config_output)(AVFilterLink *outlink);

    /*int (*render_picture)(VAAPIVPPContext *vppctx,
                          VAProcPipelineParameterBuffer *params,
                          AVFrame *input_frame,
                          AVFrame *output_frame);*/

    //int (*colour_standard)(enum AVColorSpace av_cs);

    //int (*query_formats)(AVFilterContext *avctx);
} VAAPIVPPContext;

void vaapi_vpp_ctx_init(VAAPIVPPContext *ctx);

void vaapi_vpp_ctx_uninit(AVFilterContext *avctx, VAAPIVPPContext *ctx);

int vaapi_vpp_query_formats(AVFilterContext *avctx);

int vaapi_vpp_pipeline_uninit(VAAPIVPPContext *ctx);

int vaapi_vpp_config_input(AVFilterLink *inlink, VAAPIVPPContext *ctx);

int vaapi_vpp_config_output(AVFilterLink *outlink, VAAPIVPPContext *ctx);

int vaapi_vpp_colour_standard(enum AVColorSpace av_cs);

int vaapi_vpp_render_picture(VAAPIVPPContext *ctx, VAProcPipelineParameterBuffer *params, AVFrame *input_frame, AVFrame *output_frame);

//int vaapi_vpp_init(AVFilterContext *avctx);

//void vaapi_vpp_uninit(AVFilterContext *avctx);
#endif /* AVFILTER_VAAPI_VPP_H */
