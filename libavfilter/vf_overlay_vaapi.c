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
#include "framesync.h"

#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/timestamp.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"

static const char *const var_names[] = {
    "main_w",    "W", ///< width  of the main    video
    "main_h",    "H", ///< height of the main    video
    "overlay_w", "w", ///< width  of the overlay video
    "overlay_h", "h", ///< height of the overlay video
    "hsub",
    "vsub",
    "x",
    "y",
    "n",            ///< number of frame
    "pos",          ///< position in the file
    "t",            ///< timestamp expressed in seconds
    NULL
};

enum var_name {
    VAR_MAIN_W,    VAR_MW,
    VAR_MAIN_H,    VAR_MH,
    VAR_OVERLAY_W, VAR_OW,
    VAR_OVERLAY_H, VAR_OH,
    VAR_HSUB,
    VAR_VSUB,
    VAR_X,
    VAR_Y,
    VAR_X0,
    VAR_Y0,
    VAR_N,
    VAR_POS,
    VAR_T,
    VAR_VARS_NB
};

enum EvalMode {
    EVAL_MODE_INIT,
    EVAL_MODE_FRAME,
    EVAL_MODE_NB
};

typedef struct OverlayVAAPIContext {
    const AVClass *class;

    AVVAAPIDeviceContext *hwctx;
    AVBufferRef *device_ref;

    int valid_ids;
    VAConfigID  va_config;
    VAContextID va_context;

    AVBufferRef       *main_frames_ref;
    AVHWFramesContext *main_frames;

    AVBufferRef       *overlay_frames_ref;
    AVHWFramesContext *overlay_frames;

    AVBufferRef       *output_frames_ref;
    AVHWFramesContext *output_frames;

    char *output_format_string;
    enum AVPixelFormat output_format;
    int output_width;
    int output_height;

    FFFrameSync fs;

    int x, y;                   ///< position of overlaid picture
    int x0, y0;
    int hsub, vsub;             ///< chroma subsampling values
    double var_values[VAR_VARS_NB];
    char *x_expr, *y_expr;
    char *x0_expr, *y0_expr;

    int eof_action;             ///< action to take on EOF from source
    int eval_mode;              ///< EvalMode

    float alpha;                //overlay alpha channel
    float luma_min;                //overlay luma key min
    float luma_max;                //overlay luma key max
    AVExpr *x_pexpr, *y_pexpr;
    AVExpr *x0_pexpr, *y0_pexpr;

    VABufferID filter_buffer;
} OverlayVAAPIContext;

static inline int normalize_xy(double d, int chroma_sub)
{
    if (isnan(d))
        return INT_MAX;
    return (int)d & ~((1 << chroma_sub) - 1);
}

static void eval_expr(AVFilterContext *ctx)
{
    OverlayVAAPIContext *s = ctx->priv;

    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);
    s->var_values[VAR_Y] = av_expr_eval(s->y_pexpr, s->var_values, NULL);
    /* It is necessary if x is expressed from y  */
    s->var_values[VAR_X] = av_expr_eval(s->x_pexpr, s->var_values, NULL);

    s->var_values[VAR_X0] = av_expr_eval(s->x0_pexpr, s->var_values, NULL);
    s->var_values[VAR_Y0] = av_expr_eval(s->y0_pexpr, s->var_values, NULL);
    /* It is necessary if x0 is expressed from y0  */
    s->var_values[VAR_X0] = av_expr_eval(s->x0_pexpr, s->var_values, NULL);

    s->x = normalize_xy(s->var_values[VAR_X], s->hsub);
    s->y = normalize_xy(s->var_values[VAR_Y], s->vsub);

    s->x0 = normalize_xy(s->var_values[VAR_X0], s->hsub);
    s->y0 = normalize_xy(s->var_values[VAR_Y0], s->vsub);
}

static int set_expr(AVExpr **pexpr, const char *expr, const char *option, void *log_ctx)
{
    int ret;
    AVExpr *old = NULL;

    if (*pexpr)
        old = *pexpr;
    ret = av_expr_parse(pexpr, expr, var_names,
                        NULL, NULL, NULL, NULL, 0, log_ctx);
    if (ret < 0) {
        av_log(log_ctx, AV_LOG_ERROR,
               "Error when evaluating the expression '%s' for %s\n",
               expr, option);
        *pexpr = old;
        return ret;
    }

    av_expr_free(old);
    return 0;
}

static int overlay_vaapi_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                         char *res, int res_len, int flags)
{
    OverlayVAAPIContext *s = ctx->priv;
    int ret;

    if      (!strcmp(cmd, "x"))
        ret = set_expr(&s->x_pexpr, args, cmd, ctx);
    else if (!strcmp(cmd, "y"))
        ret = set_expr(&s->y_pexpr, args, cmd, ctx);
    else if      (!strcmp(cmd, "x0"))
        ret = set_expr(&s->x0_pexpr, args, cmd, ctx);
    else if (!strcmp(cmd, "y0"))
        ret = set_expr(&s->y0_pexpr, args, cmd, ctx);
    else
        ret = AVERROR(ENOSYS);

    if (ret < 0)
        return ret;

    if (s->eval_mode == EVAL_MODE_INIT) {
        eval_expr(ctx);
        av_log(ctx, AV_LOG_VERBOSE, "x:%f xi:%d y:%f yi:%d, x0:%f x0i:%d y0i:%f yi:%d\n",
               s->var_values[VAR_X], s->x,
               s->var_values[VAR_Y], s->y,
               s->var_values[VAR_X0], s->x0,
               s->var_values[VAR_Y0], s->y0);
    }
    return ret;
}

static int overlay_vaapi_query_formats(AVFilterContext *avctx)
{
    int ret;

    enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_VAAPI, AV_PIX_FMT_NONE,
    };

    /* main and overlay */
    if (ret = ff_formats_ref(ff_make_format_list(pix_fmts),
                            &avctx->inputs[0]->out_formats) < 0)
        return ret;
    if (ret = ff_formats_ref(ff_make_format_list(pix_fmts),
                             &avctx->inputs[1]->out_formats) < 0)
        return ret;

    if (ret = ff_formats_ref(ff_make_format_list(pix_fmts),
                       &avctx->outputs[0]->in_formats) < 0)
        return ret;

    return 0;
}

static int overlay_vaapi_pipeline_uninit(OverlayVAAPIContext *ctx)
{
    if (ctx->va_context != VA_INVALID_ID) {
        vaDestroyContext(ctx->hwctx->display, ctx->va_context);
        ctx->va_context = VA_INVALID_ID;
    }

    if (ctx->filter_buffer != VA_INVALID_ID) {
        vaDestroyBuffer(ctx->hwctx->display, ctx->filter_buffer);
        ctx->filter_buffer = VA_INVALID_ID;
    }

    if (ctx->va_config != VA_INVALID_ID) {
        vaDestroyConfig(ctx->hwctx->display, ctx->va_config);
        ctx->va_config = VA_INVALID_ID;
    }

    av_buffer_unref(&ctx->output_frames_ref);
    av_buffer_unref(&ctx->device_ref);
    ctx->hwctx = 0;

    return 0;
}

static int overlay_vaapi_config_main(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    OverlayVAAPIContext *ctx = avctx->priv;

    overlay_vaapi_pipeline_uninit(ctx);

    if (!inlink->hw_frames_ctx) {
        av_log(avctx, AV_LOG_ERROR, "A hardware frames reference is "
               "required to associate the processing device.\n");
        return AVERROR(EINVAL);
    }

    ctx->main_frames_ref = av_buffer_ref(inlink->hw_frames_ctx);
    ctx->main_frames = (AVHWFramesContext*)ctx->main_frames_ref->data;

    ctx->output_format =  ctx->main_frames->sw_format;

    return 0;
}

static int overlay_vaapi_config_overlay(AVFilterLink *inlink)
{
    AVFilterContext *avctx = inlink->dst;
    OverlayVAAPIContext *ctx = avctx->priv;
    int ret = -1;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(inlink->format);

    overlay_vaapi_pipeline_uninit(ctx);

    if (!inlink->hw_frames_ctx) {
        av_log(avctx, AV_LOG_ERROR, "A hardware frames reference is "
               "required to associate the processing device.\n");
        return AVERROR(EINVAL);
    }

    ctx->overlay_frames_ref = av_buffer_ref(inlink->hw_frames_ctx);
    ctx->overlay_frames = (AVHWFramesContext*)ctx->overlay_frames_ref->data;

    /* Finish the configuration by evaluating the expressions
       now when both inputs are configured. */
    ctx->var_values[VAR_MAIN_W   ] = ctx->var_values[VAR_MW] = avctx->inputs[0]->w;
    ctx->var_values[VAR_MAIN_H   ] = ctx->var_values[VAR_MH] = avctx->inputs[0]->h;
    ctx->var_values[VAR_OVERLAY_W] = ctx->var_values[VAR_OW] = avctx->inputs[1]->w;
    ctx->var_values[VAR_OVERLAY_H] = ctx->var_values[VAR_OH] = avctx->inputs[1]->h;
    ctx->var_values[VAR_HSUB]  = 1<<pix_desc->log2_chroma_w;
    ctx->var_values[VAR_VSUB]  = 1<<pix_desc->log2_chroma_h;
    ctx->var_values[VAR_X]     = NAN;
    ctx->var_values[VAR_Y]     = NAN;
    ctx->var_values[VAR_N]     = 0;
    ctx->var_values[VAR_T]     = NAN;
    ctx->var_values[VAR_POS]   = NAN;

    if ((ret = set_expr(&ctx->x_pexpr,      ctx->x_expr,      "x",      avctx)) < 0 ||
        (ret = set_expr(&ctx->y_pexpr,      ctx->y_expr,      "y",      avctx)) < 0)
        return ret;
    if ((ret = set_expr(&ctx->x0_pexpr,      ctx->x0_expr,      "x0",      avctx)) < 0 ||
        (ret = set_expr(&ctx->y0_pexpr,      ctx->y0_expr,      "y0",      avctx)) < 0)
        return ret;

    if (ctx->eval_mode == EVAL_MODE_INIT) {
        eval_expr(avctx);
        av_log(avctx, AV_LOG_VERBOSE, "x:%f xi:%d y:%f yi:%d\n",
               ctx->var_values[VAR_X], ctx->x,
               ctx->var_values[VAR_Y], ctx->y);
    }

    av_log(avctx, AV_LOG_VERBOSE,
           "main w:%d h:%d fmt:%s overlay w:%d h:%d fmt:%s eof_action:%d\n",
           avctx->inputs[0]->w, avctx->inputs[0]->h,
           av_get_pix_fmt_name(avctx->inputs[0]->format),
           avctx->inputs[1]->w, avctx->inputs[1]->h,
           av_get_pix_fmt_name(avctx->inputs[1]->format),
           ctx->eof_action);

    return 0;
}

static int overlay_vaapi_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    OverlayVAAPIContext *ctx = avctx->priv;
    AVVAAPIHWConfig *hwconfig = NULL;
    AVHWFramesConstraints *constraints = NULL;
    AVVAAPIFramesContext *va_frames;
    AVFilterLink *main_link = outlink->src->inputs[0];
    AVFilterLink *overlay_link = outlink->src->inputs[1];
    VAStatus vas;
    int err, i;

    overlay_vaapi_pipeline_uninit(ctx);

    if ((err = ff_framesync_init_dualinput(&ctx->fs, avctx)) < 0)
        return err;

    ctx->device_ref = av_buffer_ref(ctx->main_frames->device_ref);
    ctx->hwctx = ((AVHWDeviceContext*)ctx->device_ref->data)->hwctx;

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
        ctx->output_format = ctx->main_frames->sw_format;
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

    ctx->output_width =
            (ctx->x0 + main_link->w) > (ctx->x + overlay_link->w) ?
                    (ctx->x0 + main_link->w) : (ctx->x + overlay_link->w);
    ctx->output_height =
            (ctx->y0 + main_link->h) > (ctx->y + overlay_link->h) ?
                    (ctx->y0 + main_link->h) : (ctx->y + overlay_link->h);
    if (ctx->output_width  < constraints->min_width  ||
        ctx->output_height < constraints->min_height ||
        ctx->output_width  > constraints->max_width  ||
        ctx->output_height > constraints->max_height) {
        av_log(ctx, AV_LOG_ERROR, "Hardware does not support scaling to "
               "size %dx%d (constraints: width %d-%d height %d-%d).\n",
               ctx->output_width, ctx->output_height,
               constraints->min_width,  constraints->max_width,
               constraints->min_height, constraints->max_height);
        err = AVERROR(EINVAL);
        goto fail;
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

    if (err < 0)
        goto fail;
    outlink->w = ctx->output_width;
    outlink->h = ctx->output_height;
    outlink->time_base = avctx->inputs[0]->time_base; /* FIXME */

    outlink->hw_frames_ctx = av_buffer_ref(ctx->output_frames_ref);
    if (!outlink->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    av_freep(&hwconfig);
    av_hwframe_constraints_free(&constraints);
    return ff_framesync_configure(&ctx->fs);

fail:
    av_buffer_unref(&ctx->output_frames_ref);
    av_freep(&hwconfig);
    av_hwframe_constraints_free(&constraints);
    return err;
}

/*
static int overlay_request_frame(AVFilterLink *outlink)
{
    OverlayVAAPIContext *s = outlink->src->priv;
    return ff_dualinput_request_frame(&s->fs, outlink);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *inpicref)
{
    OverlayVAAPIContext *s = inlink->dst->priv;
    av_log(inlink->dst, AV_LOG_DEBUG, "Incoming frame (time:%s) from link #%d\n",
           av_ts2timestr(inpicref->pts, &inlink->time_base), FF_INLINK_IDX(inlink));
    return ff_dualinput_filter_frame(&s->fs, inlink, inpicref);
}
*/


#define MAX_OVERLAY_BUFFER 2
#define MAIN_OVERLAY 0
#define TOP_OVERLAY 1
static AVFrame *blend_image(AVFilterContext *avctx, AVFrame *main, const AVFrame *overlay, int x, int y)
{
    AVFilterLink *inlink = avctx->inputs[0];
    OverlayVAAPIContext *ctx = avctx->priv;
    AVFrame *output_frame = NULL;
    VASurfaceID main_surface, overlay_surface, output_surface;
    VAProcPipelineParameterBuffer params[MAX_OVERLAY_BUFFER];
    VABufferID params_id[MAX_OVERLAY_BUFFER];
    VABlendState blend_state;
    VAProcPipelineParameterBuffer filter_params;
    VARectangle main_region, overlay_region, main_out_region, overlay_out_region, out_region;
    VAStatus vas;
    int err;

    av_log(ctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(main->format),
           main->width, main->height, main->pts);

    av_log(inlink->dst, AV_LOG_DEBUG, "Incoming frame (time:%s) from link #%d\n",
           av_ts2timestr(main->pts, &inlink->time_base), FF_INLINK_IDX(inlink));

    if (ctx->va_context == VA_INVALID_ID)
        return NULL;

    main_surface = (VASurfaceID)(uintptr_t)main->data[3];
    av_log(ctx, AV_LOG_DEBUG, "Using surface %#x for main overlay input.\n",
           main_surface);

    overlay_surface = (VASurfaceID)(uintptr_t)overlay->data[3];
    av_log(ctx, AV_LOG_DEBUG, "Using surface %#x for overlay input.\n",
           overlay_surface);

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
    av_log(ctx, AV_LOG_DEBUG, "Using surface %#x for overlay output.\n",
           output_surface);

    memset(&params, 0, sizeof(params));
    memset(&blend_state, 0, sizeof(VABlendState));
    main_region = (VARectangle) {
        .x      = 0,
        .y      = 0,
        .width  = main->width,
        .height = main->height,
    };
    overlay_region = (VARectangle) {
        .x      = 0,
        .y      = 0,
        .width  = overlay->width,
        .height = overlay->height,
    };
    main_out_region = (VARectangle) {
        .x      = ctx->x0,
        .y      = ctx->y0,
        .width  = main->width,
        .height = main->height,
    };
    overlay_out_region = (VARectangle) {
        .x      = ctx->x,
        .y      = ctx->y,
        .width  = overlay->width,
        .height = overlay->height,
    };
    out_region = (VARectangle) {
        .x      = 0,
        .y      = 0,
        .width  = ctx->output_width,
        .height = ctx->output_height,
    };
    //ARGB blend flags set zero because alpha value has mapped into surface
    blend_state.flags = ctx->overlay_frames->sw_format == AV_PIX_FMT_NV12 ?
            VA_BLEND_LUMA_KEY | VA_BLEND_GLOBAL_ALPHA : 0;
    blend_state.global_alpha = ctx->alpha;
    blend_state.min_luma = ctx->luma_min;
    blend_state.max_luma = ctx->luma_max;

    //configure each output picuture's param
    memset(&filter_params, 0, sizeof(filter_params));
    filter_params.output_region = &out_region;
    filter_params.surface_region = &out_region;
    filter_params.surface = output_surface;
    filter_params.surface_color_standard = VAProcColorStandardBT601;
    filter_params.pipeline_flags = VA_PROC_PIPELINE_SUBPICTURES;
    filter_params.output_color_standard = VAProcColorStandardBT601;

    vas = vaCreateBuffer(ctx->hwctx->display, ctx->va_context,
                         VAProcPipelineParameterBufferType,
                         sizeof(filter_params), 1, &filter_params, &ctx->filter_buffer);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create parameter buffer: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_begin;
    }
    vas = vaBeginPicture(ctx->hwctx->display,
                         ctx->va_context, output_surface);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to attach new picture: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail;
    }

    vas = vaRenderPicture(ctx->hwctx->display, ctx->va_context,
                          &ctx->filter_buffer, 1);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to render parameter buffer: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_begin;
    }

    for (int i = 0; i < MAX_OVERLAY_BUFFER; i++) {
        params[i].surface = i == MAIN_OVERLAY ? main_surface : overlay_surface;
        params[i].surface_region = (i == MAIN_OVERLAY ? &main_region : &overlay_region);
        //ARGB fmt use VAProcColorStardardNone as surface color stardard VPG DRIVER
        if (i == MAIN_OVERLAY)
            params[i].surface_color_standard = ctx->main_frames->sw_format == AV_PIX_FMT_NV12 ?
                    VAProcColorStandardBT601 : VAProcColorStandardNone;
        else
            params[i].surface_color_standard = ctx->overlay_frames->sw_format == AV_PIX_FMT_NV12 ?
                    VAProcColorStandardBT601 : VAProcColorStandardNone;
        params[i].output_region = (i == MAIN_OVERLAY ? &main_out_region : &overlay_out_region);
        params[i].output_background_color = 0x00000000;
        params[i].output_color_standard = VAProcColorStandardBT601;

        params[i].pipeline_flags |= VA_PROC_PIPELINE_SUBPICTURES;
        params[i].filter_flags |= VA_FILTER_SCALING_HQ;
        if (i == MAIN_OVERLAY)
            params[i].blend_state = (ctx->main_frames->sw_format == AV_PIX_FMT_NV12 ? NULL : &blend_state);
        else
            params[i].blend_state = &blend_state;

        params[i].filters = 0;
        params[i].num_filters = 0;

        vas = vaCreateBuffer(ctx->hwctx->display, ctx->va_context,
                             VAProcPipelineParameterBufferType,
                             sizeof(params[i]), 1, &params[i], &params_id[i]);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "Failed to create parameter buffer: "
                   "%d (%s).\n", vas, vaErrorStr(vas));
            err = AVERROR(EIO);
            goto fail_after_begin;
        }
        av_log(ctx, AV_LOG_DEBUG, "Pipeline parameter buffer is %#x.\n",
               params_id[i]);

        vas = vaRenderPicture(ctx->hwctx->display, ctx->va_context,
                              &params_id[i], 1);
        if (vas != VA_STATUS_SUCCESS) {
            av_log(ctx, AV_LOG_ERROR, "Failed to render parameter buffer: "
                   "%d (%s).\n", vas, vaErrorStr(vas));
            err = AVERROR(EIO);
            goto fail_after_begin;
        }
    }

    vas = vaEndPicture(ctx->hwctx->display, ctx->va_context);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(ctx, AV_LOG_ERROR, "Failed to start picture processing: "
               "%d (%s).\n", vas, vaErrorStr(vas));
        err = AVERROR(EIO);
        goto fail_after_render;
    }

    if (ctx->filter_buffer != VA_INVALID_ID) {
        vaDestroyBuffer(ctx->hwctx->display, ctx->filter_buffer);
        ctx->filter_buffer = VA_INVALID_ID;
    }

    if (ctx->filter_buffer != VA_INVALID_ID)
        vaDestroyBuffer(ctx->hwctx->display, ctx->filter_buffer);

    for (int i = 0; i < MAX_OVERLAY_BUFFER; i++)
        if (params_id[i] != VA_INVALID_ID)
            vaDestroyBuffer(ctx->hwctx->display, params_id[i]);

    av_frame_copy_props(output_frame, main);
    av_frame_free(&main);

    av_log(ctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output_frame->format),
           output_frame->width, output_frame->height, output_frame->pts);

    return output_frame;

    // We want to make sure that if vaBeginPicture has been called, we also
    // call vaRenderPicture and vaEndPicture.  These calls may well fail or
    // do something else nasty, but once we're in this failure case there
    // isn't much else we can do.
fail_after_begin:
    vaRenderPicture(ctx->hwctx->display, ctx->va_context, &params_id[0], MAX_OVERLAY_BUFFER);
fail_after_render:
    vaEndPicture(ctx->hwctx->display, ctx->va_context);
fail:
    if (ctx->filter_buffer != VA_INVALID_ID)
        vaDestroyBuffer(ctx->hwctx->display, ctx->filter_buffer);
    av_frame_free(&main);
    av_frame_free(&output_frame);
    return NULL;
}


static AVFrame *do_blend(AVFilterContext *ctx, AVFrame *mainpic,
                         const AVFrame *second)
{
    OverlayVAAPIContext *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFrame *output = NULL;

    if (s->eval_mode == EVAL_MODE_FRAME) {
        int64_t pos = av_frame_get_pkt_pos(mainpic);

        s->var_values[VAR_N] = inlink->frame_count_out;
        s->var_values[VAR_T] = mainpic->pts == AV_NOPTS_VALUE ?
            NAN : mainpic->pts * av_q2d(inlink->time_base);
        s->var_values[VAR_POS] = pos == -1 ? NAN : pos;

        s->var_values[VAR_OVERLAY_W] = s->var_values[VAR_OW] = second->width;
        s->var_values[VAR_OVERLAY_H] = s->var_values[VAR_OH] = second->height;
        s->var_values[VAR_MAIN_W   ] = s->var_values[VAR_MW] = mainpic->width;
        s->var_values[VAR_MAIN_H   ] = s->var_values[VAR_MH] = mainpic->height;

        eval_expr(ctx);
        av_log(ctx, AV_LOG_DEBUG, "n:%f t:%f pos:%f x:%f xi:%d y:%f yi:%d\n",
               s->var_values[VAR_N], s->var_values[VAR_T], s->var_values[VAR_POS],
               s->var_values[VAR_X], s->x,
               s->var_values[VAR_Y], s->y);
    }

    {
        output = blend_image(ctx, mainpic, second, s->x, s->y);
    }
    return output;
}

static int blend_frame_for_dualinput(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    AVFrame *main, *overlay, *dst_buf;
    int ret;

    ret = ff_framesync_dualinput_get(fs, &main, &overlay);
    if (ret < 0)
        return ret;
    if (!overlay)
        return ff_filter_frame(ctx->outputs[0], main);
    dst_buf = do_blend(ctx, main, overlay);
    return ff_filter_frame(ctx->outputs[0], dst_buf);
}

static av_cold int overlay_vaapi_init(AVFilterContext *avctx)
{
    OverlayVAAPIContext *ctx = avctx->priv;

    ctx->va_config  = VA_INVALID_ID;
    ctx->va_context = VA_INVALID_ID;
    ctx->valid_ids  = 1;

    if (ctx->output_format_string) {
        ctx->output_format = av_get_pix_fmt(ctx->output_format_string);
        if (ctx->output_format == AV_PIX_FMT_NONE) {
            av_log(ctx, AV_LOG_ERROR, "Invalid output format.\n");
            return AVERROR(EINVAL);
        }
    } else {
        // Use the input format once that is configured.
        ctx->output_format = AV_PIX_FMT_NONE;
    }

    if (!ctx->fs.opt_repeatlast || ctx->eof_action == EOF_ACTION_PASS) {
        ctx->fs.opt_repeatlast = 0;
        ctx->eof_action = EOF_ACTION_PASS;
    }
    if (ctx->fs.opt_shortest || ctx->eof_action == EOF_ACTION_ENDALL) {
        ctx->fs.opt_shortest = 1;
        ctx->eof_action = EOF_ACTION_ENDALL;
    }
    ctx->filter_buffer = VA_INVALID_ID;

    //ctx->fs.process = do_blend;
    ctx->fs.on_event = blend_frame_for_dualinput;
    return 0;
}

static av_cold void overlay_vaapi_uninit(AVFilterContext *avctx)
{
    OverlayVAAPIContext *ctx = avctx->priv;

    if (ctx->valid_ids)
        overlay_vaapi_pipeline_uninit(ctx);

    av_buffer_unref(&ctx->main_frames_ref);
    av_buffer_unref(&ctx->overlay_frames_ref);
    av_buffer_unref(&ctx->output_frames_ref);
    av_buffer_unref(&ctx->device_ref);

    ff_framesync_uninit(&ctx->fs);
    av_expr_free(ctx->x_pexpr); ctx->x_pexpr = NULL;
    av_expr_free(ctx->y_pexpr); ctx->y_pexpr = NULL;
    av_expr_free(ctx->x0_pexpr); ctx->x0_pexpr = NULL;
    av_expr_free(ctx->y0_pexpr); ctx->y0_pexpr = NULL;
}


#define OFFSET(x) offsetof(OverlayVAAPIContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption overlay_vaapi_options[] = {
    { "x", "set the x expression", OFFSET(x_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "y", "set the y expression", OFFSET(y_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "x0", "set the main video x expression", OFFSET(x0_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "y0", "set the main video y expression", OFFSET(y0_expr), AV_OPT_TYPE_STRING, {.str = "0"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "eval", "specify when to evaluate expressions", OFFSET(eval_mode), AV_OPT_TYPE_INT, {.i64 = EVAL_MODE_FRAME}, 0, EVAL_MODE_NB-1, FLAGS, "eval" },
         { "init",  "eval expressions once during initialization", 0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_INIT},  .flags = FLAGS, .unit = "eval" },
         { "frame", "eval expressions per-frame",                  0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_FRAME}, .flags = FLAGS, .unit = "eval" },
    { "alpha", "overlay alpha blend value", OFFSET(alpha), AV_OPT_TYPE_FLOAT, {.dbl=1.0}, 0.0, 1.0, FLAGS },
    { "min_luma", "sensible value lower than max_luma", OFFSET(luma_min), AV_OPT_TYPE_FLOAT, {.dbl=0.0}, 0.0, 1.0, FLAGS },
    { "max_luma", "sensible value larger than min_luma", OFFSET(luma_max), AV_OPT_TYPE_FLOAT, {.dbl=1.0}, 0.0, 1.0, FLAGS },
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(overlay_vaapi, OverlayVAAPIContext, fs);

static const AVFilterPad overlay_vaapi_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
        //.filter_frame = filter_frame,
        .config_props = overlay_vaapi_config_main,
    },
    {
        .name         = "overlay",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = overlay_vaapi_config_overlay,
        //.filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad overlay_vaapi_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = overlay_vaapi_config_output,
        //.request_frame = overlay_request_frame,
    },
    { NULL }
};

AVFilter ff_vf_overlay_vaapi = {
    .name          = "overlay_vaapi",
    .description   = NULL_IF_CONFIG_SMALL("Overlay a video source on top of the input use VAAPI."),
    .priv_size     = sizeof(OverlayVAAPIContext),
    .preinit       = overlay_vaapi_framesync_preinit,
    .init          = overlay_vaapi_init,
    .uninit        = overlay_vaapi_uninit,
    .query_formats = overlay_vaapi_query_formats,
    .process_command = overlay_vaapi_process_command,
    //.activate      = activate, // FIXME TODO
    .inputs        = overlay_vaapi_inputs,
    .outputs       = overlay_vaapi_outputs,
    .priv_class    = &overlay_vaapi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
