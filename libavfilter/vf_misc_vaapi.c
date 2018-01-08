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
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "vaapi_vpp.h"

typedef struct MiscVAAPIContext {
    VAProcFilterCap denoise_caps;
    int denoise;         // enable denoise algo. level is the optional
                         // value from the interval [-1, 100], -1 means disabled

    VAProcFilterCap sharpness_caps;
    int sharpness;       // enable sharpness. level is the optional value
                         // from the interval [-1, 100], -1 means disabled
    int num_filter_bufs;
    VABufferID filter_bufs[VAProcFilterCount];
} MiscVAAPIContext;

static float map_to_range(
    int input, int in_min, int in_max,
    float out_min, float out_max)
{
    return (input - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

static int misc_vaapi_build_filter_params(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    MiscVAAPIContext *ctx    = vpp_ctx->priv;

    VAStatus vas;
    VAProcFilterParameterBufferColorBalance misc_params[4];
    VAProcFilterCapColorBalance misc_caps[VAProcColorBalanceCount];
    uint32_t num_denoise_caps = 1;
    uint32_t num_sharpness_caps = 1;

    VAProcFilterParameterBuffer denoise;
    VAProcFilterParameterBuffer sharpness;

    memset(&misc_params, 0, sizeof(misc_params));
    memset(&misc_caps, 0, sizeof(misc_caps));

    if (ctx->denoise != -1) {
        vas = vaQueryVideoProcFilterCaps(vpp_ctx->hwctx->display, vpp_ctx->va_context,
                                         VAProcFilterNoiseReduction,
                                         &ctx->denoise_caps, &num_denoise_caps);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "Failed to query denoise caps "
                   "context: %d (%s).\n", vas, vaErrorStr(vas));
            return AVERROR(EIO);
        }

        denoise.type  = VAProcFilterNoiseReduction;
        denoise.value =  map_to_range(ctx->denoise, 0, 100,
                                      ctx->denoise_caps.range.min_value,
                                      ctx->denoise_caps.range.max_value);
        vaapi_vpp_make_param_buffers(vpp_ctx, VAProcFilterParameterBufferType,
                                     &denoise, sizeof(denoise), 1);
    }

    if (ctx->sharpness != -1) {
        vas = vaQueryVideoProcFilterCaps(vpp_ctx->hwctx->display, vpp_ctx->va_context,
                                         VAProcFilterSharpening,
                                         &ctx->sharpness_caps, &num_sharpness_caps);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "Failed to query sharpness caps "
                   "context: %d (%s).\n", vas, vaErrorStr(vas));
            return AVERROR(EIO);
        }

        sharpness.type  = VAProcFilterSharpening;
        sharpness.value = map_to_range(ctx->sharpness,
                                       0, 100,
                                       ctx->sharpness_caps.range.min_value,
                                       ctx->sharpness_caps.range.max_value);
        vaapi_vpp_make_param_buffers(vpp_ctx,
                                     VAProcFilterParameterBufferType,
                                     &sharpness, sizeof(sharpness), 1);
    }

    return 0;
}

static int misc_vaapi_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx   = outlink->src;
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    MiscVAAPIContext *ctx    = vpp_ctx->priv;
    int err;

    // multiple filters aren't supported in the driver:
    // sharpness can't work with noise reduction(de-noise), deinterlacing
    // color balance, skin tone enhancement...
    if (ctx->denoise != -1 && ctx->sharpness != -1) {
        av_log(ctx, AV_LOG_ERROR, "Do not support multiply filters (sharpness "
               "can't work with the other filters).\n");
        return AVERROR(EINVAL);
    }

    err = vaapi_vpp_config_output(outlink);
    if (err < 0)
        return err;

    return 0;
}

static int misc_vaapi_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext *avctx   = inlink->dst;
    AVFilterLink *outlink    = avctx->outputs[0];
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    MiscVAAPIContext *ctx    = vpp_ctx->priv;
    AVFrame *output_frame    = NULL;
    VASurfaceID input_surface, output_surface;
    VARectangle input_region;

    VAProcPipelineParameterBuffer params;
    int err;

    av_log(ctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_frame->format),
           input_frame->width, input_frame->height, input_frame->pts);

    if (vpp_ctx->va_context == VA_INVALID_ID)
        return AVERROR(EINVAL);

    input_surface = (VASurfaceID)(uintptr_t)input_frame->data[3];
    av_log(ctx, AV_LOG_DEBUG, "Using surface %#x for misc vpp input.\n",
           input_surface);

    output_frame = ff_get_video_buffer(outlink, vpp_ctx->output_width,
                                       vpp_ctx->output_height);
    if (!output_frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    output_surface = (VASurfaceID)(uintptr_t)output_frame->data[3];
    av_log(ctx, AV_LOG_DEBUG, "Using surface %#x for misc vpp output.\n",
           output_surface);
    memset(&params, 0, sizeof(params));
    input_region = (VARectangle) {
        .x      = 0,
        .y      = 0,
        .width  = input_frame->width,
        .height = input_frame->height,
    };

    if (vpp_ctx->nb_filter_buffers) {
        params.filters     = &vpp_ctx->filter_buffers[0];
        params.num_filters = vpp_ctx->nb_filter_buffers;
    }
    params.surface = input_surface;
    params.surface_region = &input_region;
    params.surface_color_standard =
        vaapi_vpp_colour_standard(input_frame->colorspace);

    params.output_region = NULL;
    params.output_background_color = 0xff000000;
    params.output_color_standard = params.surface_color_standard;

    params.pipeline_flags = 0;
    params.filter_flags = VA_FRAME_PICTURE;

    err = vaapi_vpp_render_picture(vpp_ctx, &params, output_surface);
    if (err < 0)
        goto fail;

    err = av_frame_copy_props(output_frame, input_frame);
    if (err < 0)
        goto fail;
    av_frame_free(&input_frame);

    av_log(ctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output_frame->format),
           output_frame->width, output_frame->height, output_frame->pts);

    return ff_filter_frame(outlink, output_frame);

fail:
    av_frame_free(&input_frame);
    av_frame_free(&output_frame);
    return err;
}

static av_cold int misc_vaapi_init(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx = avctx->priv;

    vaapi_vpp_ctx_init(vpp_ctx);
    vpp_ctx->pipeline_uninit     = vaapi_vpp_pipeline_uninit;
    vpp_ctx->build_filter_params = misc_vaapi_build_filter_params;
    vpp_ctx->output_format = AV_PIX_FMT_NONE;

    return 0;
}

static av_cold void misc_vaapi_uninit(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    MiscVAAPIContext *ctx    = vpp_ctx->priv;
    for (int i = 0; i < ctx->num_filter_bufs; i++)
        vaDestroyBuffer(vpp_ctx->hwctx->display, ctx->filter_bufs[i]);

    vaapi_vpp_ctx_uninit(avctx);
}

#define OFFSET(x) (offsetof(VAAPIVPPContext, priv_data) + \
                   offsetof(MiscVAAPIContext, x))
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption misc_vaapi_options[] = {
    { "denoise", "denoise level [-1, 100], -1 means disabled",
      OFFSET(denoise), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 100, .flags = FLAGS },
    { "sharpness", "sharpness level [-1, 100], -1 means disabled",
      OFFSET(sharpness), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 100, .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(misc_vaapi);

static const AVFilterPad misc_vaapi_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &misc_vaapi_filter_frame,
        .config_props = &vaapi_vpp_config_input,
    },
    { NULL }
};

static const AVFilterPad misc_vaapi_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &misc_vaapi_config_output,
    },
    { NULL }
};

AVFilter ff_vf_misc_vaapi = {
    .name          = "misc_vaapi",
    .description   = NULL_IF_CONFIG_SMALL("Misc VAAPI VPP for de-noise, sharpness"),
    .priv_size     = (sizeof(VAAPIVPPContext) +
                      sizeof(MiscVAAPIContext)),
    .init          = &misc_vaapi_init,
    .uninit        = &misc_vaapi_uninit,
    .query_formats = &vaapi_vpp_query_formats,
    .inputs        = misc_vaapi_inputs,
    .outputs       = misc_vaapi_outputs,
    .priv_class    = &misc_vaapi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
