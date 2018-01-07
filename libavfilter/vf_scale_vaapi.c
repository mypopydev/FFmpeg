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

#include <va/va.h>
#include <va/va_vpp.h>

#include "libavutil/avassert.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vaapi.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "scale.h"
#include "video.h"
#include "vaapi_vpp.h"

typedef struct ScaleVAAPIContext {
    const AVClass *class;

    VAAPIVPPContext *vpp_ctx;

    char *output_format_string;

    char *w_expr;      // width expression string
    char *h_expr;      // height expression string
} ScaleVAAPIContext;


static int scale_vaapi_query_formats(AVFilterContext *avctx)
{
    return vaapi_vpp_query_formats(avctx);
}

static int scale_vaapi_pipeline_uninit(AVFilterContext *avctx)
{
    ScaleVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp_ctx = ctx->vpp_ctx;

    return vaapi_vpp_pipeline_uninit(vpp_ctx);
}

static int scale_vaapi_config_input(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    ScaleVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp_ctx = ctx->vpp_ctx;

    return vaapi_vpp_config_input(inlink, vpp_ctx);
}

static int scale_vaapi_config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];
    AVFilterContext *avctx = outlink->src;
    ScaleVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp_ctx = ctx->vpp_ctx;
    int err;

    if ((err = ff_scale_eval_dimensions(ctx,
                                        ctx->w_expr, ctx->h_expr,
                                        inlink, outlink,
                                        &vpp_ctx->output_width, &vpp_ctx->output_height)) < 0)
        goto fail;

    err = vaapi_vpp_config_output(outlink, vpp_ctx);

fail:
    return err;
}

static int vaapi_proc_colour_standard(enum AVColorSpace av_cs)
{
    return vaapi_vpp_colour_standard(av_cs);
}

static int scale_vaapi_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink *outlink = avctx->outputs[0];
    ScaleVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp_ctx = ctx->vpp_ctx;
    AVFrame *output_frame = NULL;
    VASurfaceID input_surface, output_surface;
    VAProcPipelineParameterBuffer params;
    VARectangle input_region;
    int err;

    av_log(ctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_frame->format),
           input_frame->width, input_frame->height, input_frame->pts);

    if (vpp_ctx->va_context == VA_INVALID_ID)
        return AVERROR(EINVAL);

    input_surface = (VASurfaceID)(uintptr_t)input_frame->data[3];
    av_log(ctx, AV_LOG_DEBUG, "Using surface %#x for scale input.\n",
           input_surface);

    output_frame = ff_get_video_buffer(outlink, vpp_ctx->output_width,
                                       vpp_ctx->output_height);
    if (!output_frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    output_surface = (VASurfaceID)(uintptr_t)output_frame->data[3];
    av_log(ctx, AV_LOG_DEBUG, "Using surface %#x for scale output.\n",
           output_surface);

    memset(&params, 0, sizeof(params));

    // If there were top/left cropping, it could be taken into
    // account here.
    input_region = (VARectangle) {
        .x      = 0,
        .y      = 0,
        .width  = input_frame->width,
        .height = input_frame->height,
    };

    params.surface = input_surface;
    params.surface_region = &input_region;
    params.surface_color_standard =
        vaapi_proc_colour_standard(input_frame->colorspace);

    params.output_region = 0;
    params.output_background_color = 0xff000000;
    params.output_color_standard = params.surface_color_standard;

    params.pipeline_flags = 0;
    params.filter_flags = VA_FILTER_SCALING_HQ;

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

static av_cold int scale_vaapi_init(AVFilterContext *avctx)
{
    ScaleVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp_ctx;

    ctx->vpp_ctx = av_mallocz(sizeof(VAAPIVPPContext));
    if (!ctx->vpp_ctx)
        return AVERROR(ENOMEM);

    vpp_ctx = ctx->vpp_ctx;

    vaapi_vpp_ctx_init(vpp_ctx);
    vpp_ctx->pipeline_uninit = scale_vaapi_pipeline_uninit;

    if (ctx->output_format_string) {
        vpp_ctx->output_format = av_get_pix_fmt(ctx->output_format_string);
        if (vpp_ctx->output_format == AV_PIX_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Invalid output format.\n");
            return AVERROR(EINVAL);
        }
    } else {
        // Use the input format once that is configured.
        vpp_ctx->output_format = AV_PIX_FMT_NONE;
    }

    return 0;
}

static av_cold void scale_vaapi_uninit(AVFilterContext *avctx)
{
    ScaleVAAPIContext *ctx = avctx->priv;
    VAAPIVPPContext *vpp_ctx = ctx->vpp_ctx;

    vaapi_vpp_ctx_uninit(avctx, vpp_ctx);
}


#define OFFSET(x) offsetof(ScaleVAAPIContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption scale_vaapi_options[] = {
    { "w", "Output video width",
      OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = FLAGS },
    { "h", "Output video height",
      OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = FLAGS },
    { "format", "Output video format (software format of hardware frames)",
      OFFSET(output_format_string), AV_OPT_TYPE_STRING, .flags = FLAGS },
    { NULL },
};

static const AVClass scale_vaapi_class = {
    .class_name = "scale_vaapi",
    .item_name  = av_default_item_name,
    .option     = scale_vaapi_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad scale_vaapi_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &scale_vaapi_filter_frame,
        .config_props = &scale_vaapi_config_input,
    },
    { NULL }
};

static const AVFilterPad scale_vaapi_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &scale_vaapi_config_output,
    },
    { NULL }
};

AVFilter ff_vf_scale_vaapi = {
    .name          = "scale_vaapi",
    .description   = NULL_IF_CONFIG_SMALL("Scale to/from VAAPI surfaces."),
    .priv_size     = sizeof(ScaleVAAPIContext),
    .init          = &scale_vaapi_init,
    .uninit        = &scale_vaapi_uninit,
    .query_formats = &scale_vaapi_query_formats,
    .inputs        = scale_vaapi_inputs,
    .outputs       = scale_vaapi_outputs,
    .priv_class    = &scale_vaapi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
