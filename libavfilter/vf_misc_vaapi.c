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

typedef struct MiscVAAPIContext {
    VPPVAAPIContext *vpp_ctx;
    int denoise;         // enable denoise algo. level is the optional
                         // value from the interval [-1, 100], -1 means disabled
    int sharpness;       // enable sharpness. level is the optional value
                         // from the interval [-1, 100], -1 means disabled
} MiscVAAPIContext;


static int misc_vaapi_query_formats(AVFilterContext *avctx)
{
    return ff_vaapi_query_formats(avctx);
}

static int misc_vaapi_config_input(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    MiscVAAPIContext *ctx = avctx->priv;

    return vaapi_vpp_config_input(ctx->vpp_ctx, inlink);
}

static int misc_vaapi_build_filter_params(AVFilterContext *avctx)
{
    MiscVAAPIContext *ctx = avctx->priv;
    VAStatus vas;
    VAProcFilterCap denoise_caps;
    VAProcFilterCap sharpness_caps;
    unsigned int num_denoise_caps = 1;
    unsigned int num_sharpness_caps = 1;
    VAProcFilterParameterBuffer denoise;
    VAProcFilterParameterBuffer sharpness;
    int err = 0;

    memset(&denoise_caps, 0, sizeof(denoise_caps));
    memset(&sharpness_caps, 0, sizeof(sharpness_caps));

    if (ctx->denoise != -1) {
        vas = vaQueryVideoProcFilterCaps(ctx->vpp_ctx->hwctx->display, ctx->vpp_ctx->va_context,
                                         VAProcFilterNoiseReduction,
                                         &denoise_caps, &num_denoise_caps);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "Failed to query denoise caps "
                   "context: %d (%s).\n", vas, vaErrorStr(vas));
            err = AVERROR(EIO);
            return err;
        }

        denoise.type  = VAProcFilterNoiseReduction;
        denoise.value =  av_clip(ctx->denoise,
                            denoise_caps.range.min_value,
                            denoise_caps.range.max_value);
        err = ff_vaapi_vpp_make_param_buffer(ctx->vpp_ctx,
                                       VAProcFilterParameterBufferType,
                                       &denoise, sizeof(denoise));
        if (err < 0)
            return err;
    }

    if (ctx->sharpness != -1) {
        vas = vaQueryVideoProcFilterCaps(ctx->vpp_ctx->hwctx->display, ctx->vpp_ctx->va_context,
                                         VAProcFilterSharpening,
                                         &sharpness_caps, &num_sharpness_caps);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "Failed to query sharpness caps "
                   "context: %d (%s).\n", vas, vaErrorStr(vas));
            err = AVERROR(EIO);
            return err;
        }

        sharpness.type  = VAProcFilterSharpening;
        sharpness.value =  av_clip(ctx->sharpness,
                sharpness_caps.range.min_value,
                sharpness_caps.range.max_value);

        err = ff_vaapi_vpp_make_param_buffer(ctx->vpp_ctx,
                                       VAProcFilterParameterBufferType,
                                       &sharpness, sizeof(sharpness));
        if (err < 0)
            return err;
    }
    return 0;
}

static int misc_vaapi_config_output(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[0];
    AVFilterContext *avctx = outlink->src;
    MiscVAAPIContext *ctx = avctx->priv;
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
    err = misc_vaapi_build_filter_params(avctx);
    if (err != 0)
        goto fail;
    return 0;

fail:
    return err;
}


static int misc_vaapi_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink *outlink = avctx->outputs[0];
    MiscVAAPIContext *ctx = avctx->priv;
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
    ff_vaapi_vpp_destroy_param_buffer(ctx->vpp_ctx);
    av_frame_copy_props(output_frame, input_frame);
    av_frame_free(&input_frame);

    return ff_filter_frame(outlink, output_frame);
fail:
    av_frame_free(&input_frame);
    av_frame_free(&output_frame);
    return err;
}

static av_cold int misc_vaapi_init(AVFilterContext *avctx)
{
    MiscVAAPIContext *ctx = avctx->priv;

    ctx->vpp_ctx = av_mallocz(sizeof(VPPVAAPIContext));
    if (!ctx->vpp_ctx)
        return AVERROR(ENOMEM);
    vaapi_vpp_init(ctx->vpp_ctx);
    ctx->vpp_ctx->output_format = AV_PIX_FMT_NONE;
    return 0;
}

static av_cold void misc_vaapi_uninit(AVFilterContext *avctx)
{
    MiscVAAPIContext *ctx = avctx->priv;
    ff_vaapi_vpp_destroy_param_buffer(ctx->vpp_ctx);
    if (ctx->vpp_ctx->valid_ids == 1) {
        vaapi_vpp_uninit(ctx->vpp_ctx);
        av_free(ctx->vpp_ctx);
    }
}


#define OFFSET(x) offsetof(MiscVAAPIContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption misc_vaapi_options[] = {
    { "denoise", "denoise level [-1, 9], -1 means disabled",
      OFFSET(denoise), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 9, .flags = FLAGS },
    { "sharpness", "sharpness level [-1, 9], -1 means disabled",
      OFFSET(sharpness), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 9, .flags = FLAGS },
    { NULL },
};

static const AVClass misc_vaapi_class = {
    .class_name = "misc_vaapi",
    .item_name  = av_default_item_name,
    .option     = misc_vaapi_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad misc_vaapi_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &misc_vaapi_filter_frame,
        .config_props = &misc_vaapi_config_input,
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
    .description   = NULL_IF_CONFIG_SMALL("misc vpp to/from VAAPI surfaces."),
    .priv_size     = sizeof(MiscVAAPIContext),
    .init          = &misc_vaapi_init,
    .uninit        = &misc_vaapi_uninit,
    .query_formats = &misc_vaapi_query_formats,
    .inputs        = misc_vaapi_inputs,
    .outputs       = misc_vaapi_outputs,
    .priv_class    = &misc_vaapi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
