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
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vaapi.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "internal.h"
#include "vaapi_vpp.h"

typedef struct ProcampVAAPIContext {
    VAAPIVPPContext *vpp_ctx;

    int output_width;
    int output_height;
    int output_bright;
    int output_hue;
    int output_saturation;
    int output_contrast;
} ProcampVAAPIContext;


static int procamp_vaapi_query_formats(AVFilterContext *avctx)
{
    return ff_vaapi_query_formats(avctx);
}

static int procamp_vaapi_config_input(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    ProcampVAAPIContext *ctx = avctx->priv;

    return vaapi_vpp_config_input(ctx->vpp_ctx, inlink);
}

static int procamp_vaapi_build_filter_params(AVFilterContext *avctx)
{
    ProcampVAAPIContext *ctx = avctx->priv;
    VAStatus vas;
    VAProcFilterParameterBufferColorBalance procamp_params[4];
    VAProcFilterCapColorBalance procamp_caps[VAProcColorBalanceCount];
    int num_caps;

    memset(&procamp_params, 0, sizeof(procamp_params));
    memset(&procamp_caps, 0, sizeof(procamp_caps));

    num_caps = VAProcColorBalanceCount;
    vas = vaQueryVideoProcFilterCaps(ctx->vpp_ctx->hwctx->display, ctx->vpp_ctx->va_context, VAProcFilterColorBalance, &procamp_caps, &num_caps);

    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to Query procamp "
               "query caps: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }
    procamp_params[0].type   = VAProcFilterColorBalance;
    procamp_params[0].attrib = VAProcColorBalanceBrightness;
    procamp_params[0].value  = 0;

    if ( ctx->output_bright ) {
        procamp_params[0].value = av_clip(ctx->output_bright,
            procamp_caps[VAProcColorBalanceBrightness-1].range.min_value,
            procamp_caps[VAProcColorBalanceBrightness-1].range.max_value);
    }

    procamp_params[1].type   = VAProcFilterColorBalance;
    procamp_params[1].attrib = VAProcColorBalanceContrast;
    procamp_params[1].value  = 1;

    if ( ctx->output_contrast != 1 ) {
        procamp_params[1].value = av_clip(ctx->output_contrast,
            procamp_caps[VAProcColorBalanceContrast-1].range.min_value,
            procamp_caps[VAProcColorBalanceContrast-1].range.max_value);
    }

    procamp_params[2].type   = VAProcFilterColorBalance;
    procamp_params[2].attrib = VAProcColorBalanceHue;
    procamp_params[2].value  = 0;

    if ( ctx->output_hue) {
        procamp_params[2].value = av_clip(ctx->output_hue,
            procamp_caps[VAProcColorBalanceHue-1].range.min_value,
            procamp_caps[VAProcColorBalanceHue-1].range.max_value);
    }

    procamp_params[3].type   = VAProcFilterColorBalance;
    procamp_params[3].attrib = VAProcColorBalanceSaturation;
    procamp_params[3].value  = 1;

    if ( ctx->output_saturation != 1 ) {
        procamp_params[3].value = av_clip(ctx->output_saturation,
            procamp_caps[VAProcColorBalanceSaturation-1].range.min_value,
            procamp_caps[VAProcColorBalanceSaturation-1].range.max_value);
    }

    return ff_vaapi_vpp_make_param_array(ctx->vpp_ctx,
                VAProcFilterParameterBufferType, 4,
                &procamp_params, sizeof(procamp_params));
}

static int procamp_vaapi_config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];
    AVFilterContext *avctx = outlink->src;
    ProcampVAAPIContext *ctx = avctx->priv;
    int err;

    ctx->vpp_ctx->output_width = avctx->inputs[0]->w;
    ctx->vpp_ctx->output_height = avctx->inputs[0]->h;

    if (err = vaapi_vpp_config_output(ctx->vpp_ctx))
        goto fail;
    outlink->w = inlink->w;
    outlink->h = inlink->h;

    outlink->hw_frames_ctx = av_buffer_ref(ctx->vpp_ctx->output_frames_ref);
    if (!outlink->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = procamp_vaapi_build_filter_params(avctx);
    if (err != 0)
        goto fail;
    return 0;

fail:
    return err;
}

static int procamp_vaapi_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink *outlink = avctx->outputs[0];
    ProcampVAAPIContext *ctx = avctx->priv;
    AVFrame *output_frame = NULL;
    int err;

    output_frame = ff_get_video_buffer(outlink, ctx->vpp_ctx->output_width,
                                       ctx->vpp_ctx->output_height);
    if (!output_frame) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    av_assert0(ctx->vpp_ctx->num_filter_bufs + 1 <= VAProcFilterCount);
    vaapi_vpp_filter_frame(ctx->vpp_ctx, input_frame, output_frame);

    av_frame_copy_props(output_frame, input_frame);
    av_frame_free(&input_frame);

    return ff_filter_frame(outlink, output_frame);
fail:
    av_frame_free(&input_frame);
    av_frame_free(&output_frame);
    return err;
}

static av_cold int procamp_vaapi_init(AVFilterContext *avctx)
{
    ProcampVAAPIContext *ctx = avctx->priv;

    ctx->vpp_ctx = av_mallocz(sizeof(VAAPIVPPContext));
    if (!ctx->vpp_ctx)
        return AVERROR(ENOMEM);
    vaapi_vpp_init(ctx->vpp_ctx);
    ctx->vpp_ctx->output_format = AV_PIX_FMT_NONE;
    return 0;
}

static av_cold void procamp_vaapi_uninit(AVFilterContext *avctx)
{
    ProcampVAAPIContext *ctx = avctx->priv;
    if (ctx->vpp_ctx->valid_ids == 1) {
        ff_vaapi_vpp_destroy_param_buffer(ctx->vpp_ctx);
        vaapi_vpp_uninit(ctx->vpp_ctx);
        av_free(ctx->vpp_ctx);
    }
}


#define OFFSET(x) offsetof(ProcampVAAPIContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption procamp_vaapi_options[] = {
    { "b", "Output video bright",
      OFFSET(output_bright),  AV_OPT_TYPE_INT, { .i64 = 0 }, -100, 100, .flags = FLAGS },
    { "s", "Output video saturation",
      OFFSET(output_saturation), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 10, .flags = FLAGS },
    { "c", "Output video contrast",
      OFFSET(output_contrast),  AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 10, .flags = FLAGS },
    { "h", "Output video hue",
      OFFSET(output_hue), AV_OPT_TYPE_INT, { .i64 = 0 }, -180, 180, .flags = FLAGS },
    { NULL },
};

static const AVClass procamp_vaapi_class = {
    .class_name = "procamp_vaapi",
    .item_name  = av_default_item_name,
    .option     = procamp_vaapi_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad procamp_vaapi_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &procamp_vaapi_filter_frame,
        .config_props = &procamp_vaapi_config_input,
    },
    { NULL }
};

static const AVFilterPad procamp_vaapi_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &procamp_vaapi_config_output,
    },
    { NULL }
};

AVFilter ff_vf_procamp_vaapi = {
    .name          = "procamp_vaapi",
    .description   = NULL_IF_CONFIG_SMALL("Procamp to/from VAAPI surfaces."),
    .priv_size     = sizeof(ProcampVAAPIContext),
    .init          = &procamp_vaapi_init,
    .uninit        = &procamp_vaapi_uninit,
    .query_formats = &procamp_vaapi_query_formats,
    .inputs        = procamp_vaapi_inputs,
    .outputs       = procamp_vaapi_outputs,
    .priv_class    = &procamp_vaapi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
