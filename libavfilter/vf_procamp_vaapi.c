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

typedef struct ProcampVAAPIContext {
    float bright;
    float hue;
    float saturation;
    float contrast;
} ProcampVAAPIContext;

static int procamp_vaapi_build_filter_params(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    ProcampVAAPIContext *ctx = vpp_ctx->priv;
    VAStatus vas;
    VAProcFilterParameterBufferColorBalance procamp_params[4];
    VAProcFilterCapColorBalance procamp_caps[VAProcColorBalanceCount];
    int num_caps;

    memset(&procamp_params, 0, sizeof(procamp_params));
    memset(&procamp_caps, 0, sizeof(procamp_caps));

    num_caps = VAProcColorBalanceCount;
    vas = vaQueryVideoProcFilterCaps(vpp_ctx->hwctx->display, vpp_ctx->va_context,
                                     VAProcFilterColorBalance, &procamp_caps, &num_caps);

    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to query procamp "
               "filter caps: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    procamp_params[0].type   = VAProcFilterColorBalance;
    procamp_params[0].attrib = VAProcColorBalanceBrightness;
    procamp_params[0].value  = ctx->bright;

    procamp_params[1].type   = VAProcFilterColorBalance;
    procamp_params[1].attrib = VAProcColorBalanceContrast;
    procamp_params[1].value  = ctx->contrast;

    procamp_params[2].type   = VAProcFilterColorBalance;
    procamp_params[2].attrib = VAProcColorBalanceHue;
    procamp_params[2].value  = ctx->hue;

    procamp_params[3].type   = VAProcFilterColorBalance;
    procamp_params[3].attrib = VAProcColorBalanceSaturation;
    procamp_params[3].value  = ctx->saturation;

    return vaapi_vpp_make_param_buffers(vpp_ctx,
                                        VAProcFilterParameterBufferType,
                                        &procamp_params,
                                        sizeof(procamp_params[0]),
                                        4);
}

static int procamp_vaapi_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext *avctx   = inlink->dst;
    AVFilterLink *outlink    = avctx->outputs[0];
    VAAPIVPPContext *vpp_ctx = avctx->priv;
    AVFrame *output_frame = NULL;
    VASurfaceID input_surface, output_surface;
    VAProcPipelineParameterBuffer params;
    VARectangle input_region;
    int err;

    av_log(avctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_frame->format),
           input_frame->width, input_frame->height, input_frame->pts);

    if (vpp_ctx->va_context == VA_INVALID_ID)
        return AVERROR(EINVAL);

    input_surface = (VASurfaceID)(uintptr_t)input_frame->data[3];
    av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for procamp input.\n",
           input_surface);

    output_frame = ff_get_video_buffer(outlink, vpp_ctx->output_width,
                                       vpp_ctx->output_height);
    if (!output_frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    output_surface = (VASurfaceID)(uintptr_t)output_frame->data[3];
    av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for procamp output.\n",
           output_surface);
    memset(&params, 0, sizeof(params));
    input_region = (VARectangle) {
        .x      = 0,
        .y      = 0,
        .width  = input_frame->width,
        .height = input_frame->height,
    };

    params.surface = input_surface;
    params.surface_region = &input_region;
    params.surface_color_standard =
        vaapi_vpp_colour_standard(input_frame->colorspace);

    params.output_region = NULL;
    params.output_background_color = 0xff000000;
    params.output_color_standard = params.surface_color_standard;

    params.pipeline_flags = 0;
    params.filter_flags = VA_FRAME_PICTURE;

    params.filters     = &vpp_ctx->filter_buffers[0];
    params.num_filters = 1;

    err = vaapi_vpp_render_picture(vpp_ctx, &params, output_surface);
    if (err < 0)
        goto fail;

    err = av_frame_copy_props(output_frame, input_frame);
    if (err < 0)
        goto fail;
    av_frame_free(&input_frame);

    av_log(avctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output_frame->format),
           output_frame->width, output_frame->height, output_frame->pts);

    return ff_filter_frame(outlink, output_frame);

fail:
    av_frame_free(&input_frame);
    av_frame_free(&output_frame);
    return err;
}

static av_cold int procamp_vaapi_init(AVFilterContext *avctx)
{
    VAAPIVPPContext *vpp_ctx = avctx->priv;

    vaapi_vpp_ctx_init(vpp_ctx);
    vpp_ctx->pipeline_uninit     = vaapi_vpp_pipeline_uninit;
    vpp_ctx->build_filter_params = procamp_vaapi_build_filter_params;
    vpp_ctx->output_format       = AV_PIX_FMT_NONE;

    return 0;
}

#define OFFSET(x) (offsetof(VAAPIVPPContext, priv_data) + \
                    offsetof(ProcampVAAPIContext, x))
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption procamp_vaapi_options[] = {
    { "b", "Output video brightness",
      OFFSET(bright),  AV_OPT_TYPE_FLOAT, { .dbl = 0.0 }, -100.0, 100.0, .flags = FLAGS },
    { "brightness", "Output video brightness",
      OFFSET(bright),  AV_OPT_TYPE_FLOAT, { .dbl = 0.0 }, -100.0, 100.0, .flags = FLAGS },
    { "s", "Output video saturation",
      OFFSET(saturation), AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS },
    { "saturatio", "Output video saturation",
      OFFSET(saturation), AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS },
    { "c", "Output video contrast",
      OFFSET(contrast),  AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS },
    { "contrast", "Output video contrast",
      OFFSET(contrast),  AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS },
    { "h", "Output video hue",
      OFFSET(hue), AV_OPT_TYPE_FLOAT, { .dbl = 0.0 }, -180.0, 180.0, .flags = FLAGS },
    { "hue", "Output video hue",
      OFFSET(hue), AV_OPT_TYPE_FLOAT, { .dbl = 0.0 }, -180.0, 180.0, .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(procamp_vaapi);

static const AVFilterPad procamp_vaapi_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &procamp_vaapi_filter_frame,
        .config_props = &vaapi_vpp_config_input,
    },
    { NULL }
};

static const AVFilterPad procamp_vaapi_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &vaapi_vpp_config_output,
    },
    { NULL }
};

AVFilter ff_vf_procamp_vaapi = {
    .name          = "procamp_vaapi",
    .description   = NULL_IF_CONFIG_SMALL("ProcAmp (color balance) adjustments for hue, saturation, brightness, contrast"),
    .priv_size     = (sizeof(VAAPIVPPContext) +
                      sizeof(ProcampVAAPIContext)),
    .init          = &procamp_vaapi_init,
    .uninit        = &vaapi_vpp_ctx_uninit,
    .query_formats = &vaapi_vpp_query_formats,
    .inputs        = procamp_vaapi_inputs,
    .outputs       = procamp_vaapi_outputs,
    .priv_class    = &procamp_vaapi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
