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

typedef struct ProcampVAAPIContext {
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
    int output_width;
    int output_height;

    float bright;
    float hue;
    float saturation;
    float contrast;

    VABufferID         filter_buffer;
} ProcampVAAPIContext;

static int procamp_vaapi_query_formats(AVFilterContext *avctx)
{
    int err;
    enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_VAAPI, AV_PIX_FMT_NONE,
    };

    if ((err = ff_formats_ref(ff_make_format_list(pix_fmts),
                              &avctx->inputs[0]->out_formats)) < 0)
        return err;
    if ((err = ff_formats_ref(ff_make_format_list(pix_fmts),
                              &avctx->outputs[0]->in_formats)) < 0)
        return err;

    return 0;
}

static int procamp_vaapi_pipeline_uninit(ProcampVAAPIContext *ctx)
{
    if (ctx->filter_buffer != VA_INVALID_ID) {
        vaDestroyBuffer(ctx->hwctx->display, ctx->filter_buffer);
        ctx->filter_buffer = VA_INVALID_ID;
    }

    if (ctx->va_context != VA_INVALID_ID) {
        vaDestroyContext(ctx->hwctx->display, ctx->va_context);
        ctx->va_context = VA_INVALID_ID;
    }

    if (ctx->va_config != VA_INVALID_ID) {
        vaDestroyConfig(ctx->hwctx->display, ctx->va_config);
        ctx->va_config = VA_INVALID_ID;
    }

    av_buffer_unref(&ctx->output_frames_ref);
    av_buffer_unref(&ctx->device_ref);
    ctx->hwctx = NULL;

    return 0;
}

static int procamp_vaapi_config_input(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    ProcampVAAPIContext *ctx = avctx->priv;

    procamp_vaapi_pipeline_uninit(ctx);

    if (!inlink->hw_frames_ctx) {
        av_log(avctx, AV_LOG_ERROR, "A hardware frames reference is "
               "required to associate the processing device.\n");
        return AVERROR(EINVAL);
    }

    ctx->input_frames_ref = av_buffer_ref(inlink->hw_frames_ctx);
    ctx->input_frames = (AVHWFramesContext*)ctx->input_frames_ref->data;

    return 0;
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
    vas = vaQueryVideoProcFilterCaps(ctx->hwctx->display, ctx->va_context,
                                     VAProcFilterColorBalance, &procamp_caps, &num_caps);

    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to Query procamp "
               "query caps: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    procamp_params[0].type   = VAProcFilterColorBalance;
    procamp_params[0].attrib = VAProcColorBalanceBrightness;
    procamp_params[0].value  = 0.0;
    if (ctx->bright) {
        procamp_params[0].value =
            av_clip(ctx->bright,
                    procamp_caps[VAProcColorBalanceBrightness-1].range.min_value,
                    procamp_caps[VAProcColorBalanceBrightness-1].range.max_value);
    }

    procamp_params[1].type   = VAProcFilterColorBalance;
    procamp_params[1].attrib = VAProcColorBalanceContrast;
    procamp_params[1].value  = 1.0;
    if (ctx->contrast != 1) {
        procamp_params[1].value =
            av_clip(ctx->contrast,
                    procamp_caps[VAProcColorBalanceContrast-1].range.min_value,
                    procamp_caps[VAProcColorBalanceContrast-1].range.max_value);
    }

    procamp_params[2].type   = VAProcFilterColorBalance;
    procamp_params[2].attrib = VAProcColorBalanceHue;
    procamp_params[2].value  = 0.0;
    if (ctx->hue) {
        procamp_params[2].value =
            av_clip(ctx->hue,
                    procamp_caps[VAProcColorBalanceHue-1].range.min_value,
                    procamp_caps[VAProcColorBalanceHue-1].range.max_value);
    }

    procamp_params[3].type   = VAProcFilterColorBalance;
    procamp_params[3].attrib = VAProcColorBalanceSaturation;
    procamp_params[3].value  = 1.0;
    if (ctx->saturation != 1) {
        procamp_params[3].value =
            av_clip(ctx->saturation,
                    procamp_caps[VAProcColorBalanceSaturation-1].range.min_value,
                    procamp_caps[VAProcColorBalanceSaturation-1].range.max_value);
    }

    av_assert0(ctx->filter_buffer == VA_INVALID_ID);
    vas = vaCreateBuffer(ctx->hwctx->display, ctx->va_context,
                         VAProcFilterParameterBufferType,
                         sizeof(procamp_params), 4, &procamp_params,
                         &ctx->filter_buffer);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create procamp "
               "parameter buffer: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    return 0;
}

static int procamp_vaapi_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    ProcampVAAPIContext *ctx = avctx->priv;
    AVVAAPIHWConfig *hwconfig = NULL;
    AVHWFramesConstraints *constraints = NULL;
    AVVAAPIFramesContext *va_frames;
    VAStatus vas;
    int err, i;

    procamp_vaapi_pipeline_uninit(ctx);

    av_assert0(ctx->input_frames);
    ctx->device_ref = av_buffer_ref(ctx->input_frames->device_ref);
    ctx->hwctx = ((AVHWDeviceContext*)ctx->device_ref->data)->hwctx;

    ctx->output_width = ctx->input_frames->width;
    ctx->output_height = ctx->input_frames->height;

    av_assert0(ctx->va_config == VA_INVALID_ID);
    vas = vaCreateConfig(ctx->hwctx->display, VAProfileNone,
                         VAEntrypointVideoProc, 0, 0, &ctx->va_config);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create processing pipeline "
               "config: %d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail;
    }

    hwconfig = av_hwdevice_hwconfig_alloc(ctx->device_ref);
    if (!hwconfig) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    hwconfig->config_id = ctx->va_config;

    constraints = av_hwdevice_get_hwframe_constraints(ctx->device_ref,
                                                      hwconfig);
    if (!constraints) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    if (ctx->output_format == AV_PIX_FMT_NONE)
        ctx->output_format = ctx->input_frames->sw_format;
    if (constraints->valid_sw_formats) {
        for (i = 0; constraints->valid_sw_formats[i] != AV_PIX_FMT_NONE; i++) {
            if (ctx->output_format == constraints->valid_sw_formats[i])
                break;
        }
        if (constraints->valid_sw_formats[i] == AV_PIX_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Hardware does not support output "
                   "format %s.\n", av_get_pix_fmt_name(ctx->output_format));
            err = AVERROR(EINVAL);
            goto fail;
        }
    }

    ctx->output_frames_ref = av_hwframe_ctx_alloc(ctx->device_ref);
    if (!ctx->output_frames_ref) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create HW frame context "
               "for output.\n");
        err = AVERROR(ENOMEM);
        goto fail;
    }

    ctx->output_frames = (AVHWFramesContext*)ctx->output_frames_ref->data;

    ctx->output_frames->format    = AV_PIX_FMT_VAAPI;
    ctx->output_frames->sw_format = ctx->output_format;
    ctx->output_frames->width     = ctx->output_width;
    ctx->output_frames->height    = ctx->output_height;

    // The number of output frames we need is determined by what follows
    // the filter.  If it's an encoder with complex frame reference
    // structures then this could be very high.
    ctx->output_frames->initial_pool_size = 10;

    err = av_hwframe_ctx_init(ctx->output_frames_ref);
    if (err < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to initialise VAAPI frame "
               "context for output: %d\n", err);
        goto fail;
    }

    va_frames = ctx->output_frames->hwctx;

    av_assert0(ctx->va_context == VA_INVALID_ID);
    vas = vaCreateContext(ctx->hwctx->display, ctx->va_config,
                          ctx->output_width, ctx->output_height,
                          VA_PROGRESSIVE,
                          va_frames->surface_ids, va_frames->nb_surfaces,
                          &ctx->va_context);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create processing pipeline "
               "context: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    err = procamp_vaapi_build_filter_params(avctx);
    if (err < 0)
        goto fail;

    outlink->w = ctx->output_width;
    outlink->h = ctx->output_height;

    outlink->hw_frames_ctx = av_buffer_ref(ctx->output_frames_ref);
    if (!outlink->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    av_freep(&hwconfig);
    av_hwframe_constraints_free(&constraints);
    return 0;

fail:
    av_buffer_unref(&ctx->output_frames_ref);
    av_freep(&hwconfig);
    av_hwframe_constraints_free(&constraints);
    return err;
}

static int vaapi_proc_colour_standard(enum AVColorSpace av_cs)
{
    switch(av_cs) {
#define CS(av, va) case AVCOL_SPC_ ## av: return VAProcColorStandard ## va;
        CS(BT709,     BT709);
        CS(BT470BG,   BT601);
        CS(SMPTE170M, SMPTE170M);
        CS(SMPTE240M, SMPTE240M);
#undef CS
    default:
        return VAProcColorStandardNone;
    }
}

static int procamp_vaapi_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext *avctx = inlink->dst;
    AVFilterLink *outlink = avctx->outputs[0];
    ProcampVAAPIContext *ctx = avctx->priv;
    AVFrame *output_frame = NULL;
    VASurfaceID input_surface, output_surface;
    VAProcPipelineParameterBuffer params;
    VARectangle input_region;
    VABufferID params_id;
    VAStatus vas;
    int err;

    av_log(ctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_frame->format),
           input_frame->width, input_frame->height, input_frame->pts);

    if (ctx->va_context == VA_INVALID_ID)
        return AVERROR(EINVAL);

    input_surface = (VASurfaceID)(uintptr_t)input_frame->data[3];
    av_log(ctx, AV_LOG_DEBUG, "Using surface %#x for procamp input.\n",
           input_surface);

    output_frame = av_frame_alloc();
    if (!output_frame) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate output frame.");
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = av_hwframe_get_buffer(ctx->output_frames_ref, output_frame, 0);
    if (err < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get surface for "
               "output: %d\n.", err);
    }

    output_surface = (VASurfaceID)(uintptr_t)output_frame->data[3];
    av_log(ctx, AV_LOG_DEBUG, "Using surface %#x for procamp output.\n",
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
        vaapi_proc_colour_standard(input_frame->colorspace);

    params.output_region = NULL;
    params.output_background_color = 0xff000000;
    params.output_color_standard = params.surface_color_standard;

    params.pipeline_flags = 0;
    params.filter_flags = VA_FRAME_PICTURE;

    params.filters     = &ctx->filter_buffer;
    params.num_filters = 1;

    vas = vaBeginPicture(ctx->hwctx->display,
                         ctx->va_context, output_surface);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to attach new picture: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail;
    }

    vas = vaCreateBuffer(ctx->hwctx->display, ctx->va_context,
                         VAProcPipelineParameterBufferType,
                         sizeof(params), 1, &params, &params_id);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create parameter buffer: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_begin;
    }
    av_log(ctx, AV_LOG_DEBUG, "Procamp parameter buffer is %#x.\n",
           params_id);

    vas = vaRenderPicture(ctx->hwctx->display, ctx->va_context,
                          &params_id, 1);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to render parameter buffer: "
                   "%d (%s).\n", vas, vaErrorStr(vas));
            err = AVERROR(EIO);
            goto fail_after_begin;
    }

    vas = vaEndPicture(ctx->hwctx->display, ctx->va_context);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to start picture processing: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_render;
    }

    if (CONFIG_VAAPI_1 || ctx->hwctx->driver_quirks &
        AV_VAAPI_DRIVER_QUIRK_RENDER_PARAM_BUFFERS) {
        vas = vaDestroyBuffer(ctx->hwctx->display, params_id);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to free parameter buffer: "
                   "%d (%s).\n", vas, vaErrorStr(vas));
            // And ignore.
        }
    }

    err = av_frame_copy_props(output_frame, input_frame);
    if (err < 0)
        goto fail;
    av_frame_free(&input_frame);

    av_log(ctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output_frame->format),
           output_frame->width, output_frame->height, output_frame->pts);

    return ff_filter_frame(outlink, output_frame);

fail_after_begin:
    vaRenderPicture(ctx->hwctx->display, ctx->va_context, &params_id, 1);
fail_after_render:
    vaEndPicture(ctx->hwctx->display, ctx->va_context);
fail:
    av_frame_free(&output_frame);
    return err;
}

static av_cold int procamp_vaapi_init(AVFilterContext *avctx)
{
    ProcampVAAPIContext *ctx = avctx->priv;

    ctx->va_config  = VA_INVALID_ID;
    ctx->va_context = VA_INVALID_ID;
    ctx->filter_buffer = VA_INVALID_ID;
    ctx->valid_ids  = 1;

    ctx->output_format = AV_PIX_FMT_NONE;
    
    return 0;
}

static av_cold void procamp_vaapi_uninit(AVFilterContext *avctx)
{
    ProcampVAAPIContext *ctx = avctx->priv;

    if (ctx->valid_ids)
        procamp_vaapi_pipeline_uninit(ctx);

    av_buffer_unref(&ctx->input_frames_ref);
    av_buffer_unref(&ctx->output_frames_ref);
    av_buffer_unref(&ctx->device_ref);
}


#define OFFSET(x) offsetof(ProcampVAAPIContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption procamp_vaapi_options[] = {
    { "b", "Output video brightness",
      OFFSET(bright),  AV_OPT_TYPE_FLOAT, { .dbl = 0.0 }, -100.0, 100.0, .flags = FLAGS },
    { "s", "Output video saturation",
      OFFSET(saturation), AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS },
    { "c", "Output video contrast",
      OFFSET(contrast),  AV_OPT_TYPE_FLOAT, { .dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS },
    { "h", "Output video hue",
      OFFSET(hue), AV_OPT_TYPE_FLOAT, { .dbl = 0.0 }, -180.0, 180.0, .flags = FLAGS },
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
    .description   = NULL_IF_CONFIG_SMALL("ProcAmp (color balance) adjustments for hue, saturation, brightness, contrast"),
    .priv_size     = sizeof(ProcampVAAPIContext),
    .init          = &procamp_vaapi_init,
    .uninit        = &procamp_vaapi_uninit,
    .query_formats = &procamp_vaapi_query_formats,
    .inputs        = procamp_vaapi_inputs,
    .outputs       = procamp_vaapi_outputs,
    .priv_class    = &procamp_vaapi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
