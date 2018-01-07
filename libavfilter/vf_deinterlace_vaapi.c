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
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vaapi.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "vaapi_vpp.h"

#define MAX_REFERENCES 8

typedef struct DeintVAAPIContext {
    VAAPIVPPContext *vpp_ctx;

    int                mode;
    int                field_rate;
    int                auto_enable;

    VAProcFilterCapDeinterlacing
                       deint_caps[VAProcDeinterlacingCount];
    int             nb_deint_caps;
    VAProcPipelineCaps pipeline_caps;

    int                queue_depth;
    int                queue_count;
    AVFrame           *frame_queue[MAX_REFERENCES];
    int                extra_delay_for_timestamps;

} DeintVAAPIContext;

static const char *deint_vaapi_mode_name(int mode)
{
    switch (mode) {
#define D(name) case VAProcDeinterlacing ## name: return #name
        D(Bob);
        D(Weave);
        D(MotionAdaptive);
        D(MotionCompensated);
#undef D
    default:
        return "Invalid";
    }
}

static int deint_vaapi_query_formats(AVFilterContext *avctx)
{
    return ff_vaapi_vpp_query_formats(avctx);
}

static int deint_vaapi_config_input(AVFilterLink *inlink)
{
    AVFilterContext   *avctx = inlink->dst;
    DeintVAAPIContext *ctx = avctx->priv;

    return ff_vaapi_vpp_config_input(ctx->vpp_ctx, inlink);
}

static int deint_vaapi_build_filter_params(AVFilterContext *avctx)
{
    DeintVAAPIContext *ctx = avctx->priv;
    VAStatus vas;
    VAProcFilterParameterBufferDeinterlacing params;
    int i, err;

    ctx->nb_deint_caps = VAProcDeinterlacingCount;
    vas = vaQueryVideoProcFilterCaps(ctx->vpp_ctx->hwctx->display,
                                     ctx->vpp_ctx->va_context,
                                     VAProcFilterDeinterlacing,
                                     &ctx->deint_caps,
                                     &ctx->nb_deint_caps);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to query deinterlacing "
               "caps: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    if (ctx->mode == VAProcDeinterlacingNone) {
        for (i = 0; i < ctx->nb_deint_caps; i++) {
            if (ctx->deint_caps[i].type > ctx->mode)
                ctx->mode = ctx->deint_caps[i].type;
        }
        av_log(avctx, AV_LOG_VERBOSE, "Picking %d (%s) as default "
               "deinterlacing mode.\n", ctx->mode,
               deint_vaapi_mode_name(ctx->mode));
    } else {
        for (i = 0; i < ctx->nb_deint_caps; i++) {
            if (ctx->deint_caps[i].type == ctx->mode)
                break;
        }
        if (i >= ctx->nb_deint_caps) {
            av_log(avctx, AV_LOG_ERROR, "Deinterlacing mode %d (%s) is "
                   "not supported.\n", ctx->mode,
                   deint_vaapi_mode_name(ctx->mode));
        }
    }

    params.type      = VAProcFilterDeinterlacing;
    params.algorithm = ctx->mode;
    params.flags     = 0;

    err = ff_vaapi_vpp_make_param_buffer(ctx->vpp_ctx,
                                         VAProcFilterParameterBufferType,
                                         &params, sizeof(params));
    if(err != 0)
        return AVERROR(EIO);
    vas = vaQueryVideoProcPipelineCaps(ctx->vpp_ctx->hwctx->display,
                                       ctx->vpp_ctx->va_context,
                                       ctx->vpp_ctx->filter_bufs, 1,
                                       &ctx->pipeline_caps);
    if (vas != VA_STATUS_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to query pipeline "
               "caps: %d (%s).\n", vas, vaErrorStr(vas));
        return AVERROR(EIO);
    }

    ctx->extra_delay_for_timestamps = ctx->field_rate == 2 &&
        ctx->pipeline_caps.num_backward_references == 0;

    ctx->queue_depth = ctx->pipeline_caps.num_backward_references +
                       ctx->pipeline_caps.num_forward_references +
                       ctx->extra_delay_for_timestamps + 1;
    if (ctx->queue_depth > MAX_REFERENCES) {
        av_log(avctx, AV_LOG_ERROR, "Pipeline requires too many "
               "references (%u forward, %u back).\n",
               ctx->pipeline_caps.num_forward_references,
               ctx->pipeline_caps.num_backward_references);
        return AVERROR(ENOSYS);
    }

    return 0;
}

static int deint_vaapi_config_output(AVFilterLink *outlink)
{
    AVFilterContext    *avctx = outlink->src;
    AVFilterLink      *inlink = avctx->inputs[0];
    DeintVAAPIContext    *ctx = avctx->priv;
    int err;

    ctx->vpp_ctx->output_width = avctx->inputs[0]->w;
    ctx->vpp_ctx->output_height = avctx->inputs[0]->h;

    if (err = ff_vaapi_vpp_config_output(ctx->vpp_ctx))
        goto fail;
    err = deint_vaapi_build_filter_params(avctx);
    if (err < 0)
        goto fail;

    outlink->w = inlink->w;
    outlink->h = inlink->h;

    outlink->time_base  = av_mul_q(inlink->time_base,
                                   (AVRational) { 1, ctx->field_rate });
    outlink->frame_rate = av_mul_q(inlink->frame_rate,
                                   (AVRational) { ctx->field_rate, 1 });

    outlink->hw_frames_ctx = av_buffer_ref(ctx->vpp_ctx->output_frames_ref);
    if (!outlink->hw_frames_ctx) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    return 0;

fail:
    return err;
}

static int deint_vaapi_filter_frame(AVFilterLink *inlink, AVFrame *input_frame)
{
    AVFilterContext   *avctx = inlink->dst;
    AVFilterLink    *outlink = avctx->outputs[0];
    DeintVAAPIContext *ctx = avctx->priv;
    AVFrame *output_frame = NULL;
    VASurfaceID input_surface, output_surface;
    VASurfaceID backward_references[MAX_REFERENCES];
    VASurfaceID forward_references[MAX_REFERENCES];
    VAProcPipelineParameterBuffer params;
    VAProcFilterParameterBufferDeinterlacing filter_params;
    VARectangle input_region;
    int err, i, field, current_frame_index;

    av_log(avctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input_frame->format),
           input_frame->width, input_frame->height, input_frame->pts);

    if (ctx->queue_count < ctx->queue_depth) {
        ctx->frame_queue[ctx->queue_count++] = input_frame;
        if (ctx->queue_count < ctx->queue_depth) {
            // Need more reference surfaces before we can continue.
            return 0;
        }
    } else {
        av_frame_free(&ctx->frame_queue[0]);
        for (i = 0; i + 1 < ctx->queue_count; i++)
            ctx->frame_queue[i] = ctx->frame_queue[i + 1];
        ctx->frame_queue[i] = input_frame;
    }

    current_frame_index = ctx->pipeline_caps.num_forward_references;

    input_frame = ctx->frame_queue[current_frame_index];
    input_surface = (VASurfaceID)(uintptr_t)input_frame->data[3];
    for (i = 0; i < ctx->pipeline_caps.num_forward_references; i++)
        forward_references[i] = (VASurfaceID)(uintptr_t)
            ctx->frame_queue[current_frame_index - i - 1]->data[3];
    for (i = 0; i < ctx->pipeline_caps.num_backward_references; i++)
        backward_references[i] = (VASurfaceID)(uintptr_t)
            ctx->frame_queue[current_frame_index + i + 1]->data[3];

    av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for "
           "deinterlace input.\n", input_surface);
    av_log(avctx, AV_LOG_DEBUG, "Backward references:");
    for (i = 0; i < ctx->pipeline_caps.num_backward_references; i++)
        av_log(avctx, AV_LOG_DEBUG, " %#x", backward_references[i]);
    av_log(avctx, AV_LOG_DEBUG, "\n");
    av_log(avctx, AV_LOG_DEBUG, "Forward  references:");
    for (i = 0; i < ctx->pipeline_caps.num_forward_references; i++)
        av_log(avctx, AV_LOG_DEBUG, " %#x", forward_references[i]);
    av_log(avctx, AV_LOG_DEBUG, "\n");

    for (field = 0; field < ctx->field_rate; field++) {
        output_frame = ff_get_video_buffer(outlink, ctx->vpp_ctx->output_width,
                                           ctx->vpp_ctx->output_height);
        if (!output_frame) {
            err = AVERROR(ENOMEM);
            goto fail;
        }

        output_surface = (VASurfaceID)(uintptr_t)output_frame->data[3];
        av_log(avctx, AV_LOG_DEBUG, "Using surface %#x for "
               "deinterlace output.\n", output_surface);

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
            ff_vaapi_proc_colour_standard(input_frame->colorspace);

        params.output_region = NULL;
        params.output_background_color = 0xff000000;
        params.output_color_standard = params.surface_color_standard;

        params.pipeline_flags = 0;
        params.filter_flags   = VA_FRAME_PICTURE;

        if (!ctx->auto_enable || input_frame->interlaced_frame) {
            filter_params.type      = VAProcFilterDeinterlacing;
            filter_params.algorithm = ctx->mode;

            if (input_frame->top_field_first) {
                filter_params.flags |= field ? VA_DEINTERLACING_BOTTOM_FIELD : 0;
            } else {
                filter_params.flags |= VA_DEINTERLACING_BOTTOM_FIELD_FIRST;
                filter_params.flags |= field ? 0 : VA_DEINTERLACING_BOTTOM_FIELD;
            }
            err = ff_vaapi_vpp_make_param_buffer(ctx->vpp_ctx,
                                                 VAProcFilterParameterBufferType,
                                                 &filter_params, sizeof(filter_params));
            if(err != 0)
                goto fail;

            params.filters     = ctx->vpp_ctx->num_filter_bufs > 0 ? &ctx->vpp_ctx->filter_bufs[0] : NULL;
            params.num_filters = ctx->vpp_ctx->num_filter_bufs;

            params.forward_references = forward_references;
            params.num_forward_references =
                ctx->pipeline_caps.num_forward_references;
            params.backward_references = backward_references;
            params.num_backward_references =
                ctx->pipeline_caps.num_backward_references;

        } else {
            params.filters     = NULL;
            params.num_filters = 0;
        }

        err = vaapi_vpp_output_surface_ready(ctx->vpp_ctx, output_surface);
        if (err != 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to ready surface.");
            goto fail;
        }

        err = vaapi_vpp_make_pipeline_param(ctx->vpp_ctx, &params);
        if (err != 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to make vpp params.");
            goto fail;
        }

        err = vaapi_vpp_apply_pipeline_param(ctx->vpp_ctx);
        if (err != 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to apply vpp params.");
            goto fail;
        }

        ff_vaapi_vpp_destroy_param_buffer(ctx->vpp_ctx);

        err = av_frame_copy_props(output_frame, input_frame);
        if (err < 0)
            goto fail;

        if (ctx->field_rate == 2) {
            if (field == 0)
                output_frame->pts = 2 * input_frame->pts;
            else
                output_frame->pts = input_frame->pts +
                    ctx->frame_queue[current_frame_index + 1]->pts;
        }
        output_frame->interlaced_frame = 0;

        av_log(avctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
               av_get_pix_fmt_name(output_frame->format),
               output_frame->width, output_frame->height, output_frame->pts);

        err = ff_filter_frame(outlink, output_frame);
        if (err < 0)
            break;
    }

    return err;

fail:
    av_frame_free(&output_frame);
    return err;
}

static av_cold int deint_vaapi_init(AVFilterContext *avctx)
{
    DeintVAAPIContext *ctx = avctx->priv;

    ctx->vpp_ctx = av_mallocz(sizeof(VAAPIVPPContext));
    if (!ctx->vpp_ctx)
        return AVERROR(ENOMEM);
    ff_vaapi_vpp_init(ctx->vpp_ctx);
    ctx->vpp_ctx->output_format = AV_PIX_FMT_NONE;

    return 0;
}

static av_cold void deint_vaapi_uninit(AVFilterContext *avctx)
{
    DeintVAAPIContext *ctx = avctx->priv;

    if (ctx->vpp_ctx->valid_ids == 1) {
        ff_vaapi_vpp_uninit(ctx->vpp_ctx);
        av_free(ctx->vpp_ctx);
    }
}

#define OFFSET(x) offsetof(DeintVAAPIContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption deint_vaapi_options[] = {
    { "mode", "Deinterlacing mode",
      OFFSET(mode), AV_OPT_TYPE_INT, { .i64 = VAProcDeinterlacingNone },
      VAProcDeinterlacingNone, VAProcDeinterlacingCount - 1, FLAGS, "mode" },
    { "default", "Use the highest-numbered (and therefore possibly most advanced) deinterlacing algorithm",
      0, AV_OPT_TYPE_CONST, { .i64 = VAProcDeinterlacingNone }, .unit = "mode" },
    { "bob", "Use the bob deinterlacing algorithm",
      0, AV_OPT_TYPE_CONST, { .i64 = VAProcDeinterlacingBob }, .unit = "mode" },
    { "weave", "Use the weave deinterlacing algorithm",
      0, AV_OPT_TYPE_CONST, { .i64 = VAProcDeinterlacingWeave }, .unit = "mode" },
    { "motion_adaptive", "Use the motion adaptive deinterlacing algorithm",
      0, AV_OPT_TYPE_CONST, { .i64 = VAProcDeinterlacingMotionAdaptive }, .unit = "mode" },
    { "motion_compensated", "Use the motion compensated deinterlacing algorithm",
      0, AV_OPT_TYPE_CONST, { .i64 = VAProcDeinterlacingMotionCompensated }, .unit = "mode" },

    { "rate", "Generate output at frame rate or field rate",
      OFFSET(field_rate), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 2, FLAGS, "rate" },
    { "frame", "Output at frame rate (one frame of output for each field-pair)",
      0, AV_OPT_TYPE_CONST, { .i64 = 1 }, .unit = "rate" },
    { "field", "Output at field rate (one frame of output for each field)",
      0, AV_OPT_TYPE_CONST, { .i64 = 2 }, .unit = "rate" },

    { "auto", "Only deinterlace fields, passing frames through unchanged",
      OFFSET(auto_enable), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS },

    { NULL },
};

static const AVClass deint_vaapi_class = {
    .class_name = "deinterlace_vaapi",
    .item_name  = av_default_item_name,
    .option     = deint_vaapi_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad deint_vaapi_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &deint_vaapi_filter_frame,
        .config_props = &deint_vaapi_config_input,
    },
    { NULL }
};

static const AVFilterPad deint_vaapi_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = &deint_vaapi_config_output,
    },
    { NULL }
};

AVFilter ff_vf_deinterlace_vaapi = {
    .name           = "deinterlace_vaapi",
    .description    = NULL_IF_CONFIG_SMALL("Deinterlacing of VAAPI surfaces"),
    .priv_size      = sizeof(DeintVAAPIContext),
    .init           = &deint_vaapi_init,
    .uninit         = &deint_vaapi_uninit,
    .query_formats  = &deint_vaapi_query_formats,
    .inputs         = deint_vaapi_inputs,
    .outputs        = deint_vaapi_outputs,
    .priv_class     = &deint_vaapi_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
