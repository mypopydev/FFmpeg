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
#include <float.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "internal.h"
#include "opencl.h"
#include "opencl_source.h"
#include "video.h"

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_GBRP,
};

static int is_format_supported(enum AVPixelFormat fmt)
{
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == fmt)
            return 1;
    return 0;
}

typedef struct NLMeansOpenCLContext {
    OpenCLFilterContext   ocf;
    int                   initialised;
    cl_kernel             vert_kernel;
    cl_kernel             horiz_kernel;
    cl_kernel             accum_kernel;
    cl_kernel             average_kernel;
    double                sigma;
    float                 h;
    int                   chroma_w;
    int                   chroma_h;
    int                   patch_size;
    int                   patch_size_uv;
    int                   research_size;
    int                   research_size_uv;
    cl_command_queue      command_queue;
} NLMeansOpenCLContext;

static int nlmeans_opencl_init(AVFilterContext *avctx)
{
    NLMeansOpenCLContext *ctx = avctx->priv;
    cl_int cle;
    int err;

    ctx->h = ctx->sigma * 10;
    if (!ctx->research_size_uv)
        ctx->research_size_uv = ctx->research_size;
    if (!ctx->patch_size_uv)
        ctx->patch_size_uv = ctx->patch_size;

    err = ff_opencl_filter_load_program(avctx, &ff_opencl_source_nlmeans, 1);
    if (err < 0)
        goto fail;

    ctx->command_queue = clCreateCommandQueue(ctx->ocf.hwctx->context,
                                              ctx->ocf.hwctx->device_id,
                                              0, &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create OpenCL "
                     "command queue %d.\n", cle);

    ctx->vert_kernel = clCreateKernel(ctx->ocf.program, "vert_sum", &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create vert_sum kernel %d.\n", cle);

    ctx->horiz_kernel = clCreateKernel(ctx->ocf.program, "horiz_sum", &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create horiz_sum kernel %d.\n", cle);

    ctx->accum_kernel = clCreateKernel(ctx->ocf.program, "weight_accum", &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create accum kernel %d.\n", cle);

    ctx->average_kernel = clCreateKernel(ctx->ocf.program, "average", &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create average kernel %d.\n", cle);

    ctx->initialised = 1;
    return 0;

fail:
    if (ctx->command_queue)
        clReleaseCommandQueue(ctx->command_queue);
    if (ctx->vert_kernel)
        clReleaseKernel(ctx->vert_kernel);
    if (ctx->horiz_kernel)
        clReleaseKernel(ctx->horiz_kernel);
    if (ctx->accum_kernel)
        clReleaseKernel(ctx->accum_kernel);
    if (ctx->average_kernel)
        clReleaseKernel(ctx->average_kernel);
    return err;
}

static int nlmeans_plane(AVFilterContext *avctx, cl_mem dst, cl_mem src,
                         int w, int h, int p, int r)
{
    NLMeansOpenCLContext *ctx = avctx->priv;
    const float zero = 0.0f;
    const size_t worksize1[] = {h};
    const size_t worksize2[] = {w};
    const size_t worksize3[2] = {w, h};
    int dx, dy, err = 0, weight_buf_size;
    cl_mem ii, weight, sum;
    cl_int cle;
    int nb_pixel, *tmp, *dxdy, idx = 0;

    weight_buf_size = w * h * sizeof(int);
    ii = clCreateBuffer(ctx->ocf.hwctx->context, 0, 4 * weight_buf_size,
                               NULL, &cle);
    weight = clCreateBuffer(ctx->ocf.hwctx->context, 0, weight_buf_size,
                               NULL, &cle);
    sum = clCreateBuffer(ctx->ocf.hwctx->context, 0, weight_buf_size,
                               NULL, &cle);
    cle = clEnqueueFillBuffer(ctx->command_queue, weight, &zero, sizeof(float),
                              0, weight_buf_size, 0, NULL, NULL);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to fill weight buffer: %d.\n",
                     cle);
    cle = clEnqueueFillBuffer(ctx->command_queue, sum, &zero, sizeof(float),
                              0, weight_buf_size, 0, NULL, NULL);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to fill sum buffer: %d.\n",
                     cle);

    nb_pixel = (2*r+1) *(2*r+1)-1;
    dxdy = av_malloc(nb_pixel * 2 * sizeof(int));
    tmp = av_malloc(nb_pixel * 2 * sizeof(int));

    if (!dxdy || !tmp)
        goto fail;

    for (dx = -r; dx <= r; dx++) {
        for (dy = -r; dy <= r; dy++) {
            if (dx || dy) {
                tmp[idx++] = dx;
                tmp[idx++] = dy;
            }
        }
    }
    // repack dx/dy seperately, as we want to do four pairs of dx/dy in a batch
    for (int i = 0; i < nb_pixel/4;i++) {
        dxdy[i * 8] = tmp[i * 8];         // dx0
        dxdy[i * 8 + 1] = tmp[i * 8 + 2]; // dx1
        dxdy[i * 8 + 2] = tmp[i * 8 + 4]; // dx2
        dxdy[i * 8 + 3] = tmp[i * 8 + 6]; // dx3
        dxdy[i * 8 + 4] = tmp[i * 8 + 1]; // dy0
        dxdy[i * 8 + 5] = tmp[i * 8 + 3]; // dy1
        dxdy[i * 8 + 6] = tmp[i * 8 + 5]; // dy2
        dxdy[i * 8 + 7] = tmp[i * 8 + 7]; // dy3
    }
    av_freep(&tmp);

    for (int i = 0; i < nb_pixel / 4; i++) {
        int *dx_cur = dxdy + 8 * i;
        int *dy_cur = dxdy + 8 * i + 4;

        // ii(x,y) = sum of [u(i,y) - u(i+dx,y+dy)]^2 for all i < x
        CL_SET_KERNEL_ARG(ctx->horiz_kernel, 0, cl_mem, &ii);
        CL_SET_KERNEL_ARG(ctx->horiz_kernel, 1, cl_mem, &src);
        CL_SET_KERNEL_ARG(ctx->horiz_kernel, 2, cl_int, &w);
        CL_SET_KERNEL_ARG(ctx->horiz_kernel, 3, cl_int, &h);
        CL_SET_KERNEL_ARG(ctx->horiz_kernel, 4, cl_int4, dx_cur);
        CL_SET_KERNEL_ARG(ctx->horiz_kernel, 5, cl_int4, dy_cur);
        cle = clEnqueueNDRangeKernel(ctx->command_queue, ctx->horiz_kernel, 1,
                               NULL, worksize1, NULL, 0, NULL, NULL);
        CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to enqueue horiz_kernel: %d.\n",
                         cle);

        // ii(x,y) = ii(x,0) + ii(x,1) +...+ ii(x,y-1)
        CL_SET_KERNEL_ARG(ctx->vert_kernel, 0, cl_mem, &ii);
        CL_SET_KERNEL_ARG(ctx->vert_kernel, 1, cl_int, &w);
        CL_SET_KERNEL_ARG(ctx->vert_kernel, 2, cl_int, &h);
        cle = clEnqueueNDRangeKernel(ctx->command_queue, ctx->vert_kernel,
                                     1, NULL, worksize2, NULL, 0, NULL, NULL);
        CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to enqueue vert_kernel: %d.\n",
                         cle);

        // accumlate weights
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 0, cl_mem, &sum);
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 1, cl_mem, &weight);
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 2, cl_mem, &ii);
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 3, cl_mem, &src);
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 4, cl_int, &w);
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 5, cl_int, &h);
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 6, cl_int, &p);
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 7, cl_float, &ctx->h);
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 8, cl_int4, dx_cur);
        CL_SET_KERNEL_ARG(ctx->accum_kernel, 9, cl_int4, dy_cur);
        cle = clEnqueueNDRangeKernel(ctx->command_queue, ctx->accum_kernel,
                                     2, NULL, worksize3, NULL, 0, NULL, NULL);
        CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to enqueue kernel: %d.\n", cle);
    }
    av_freep(&dxdy);

    // average
    CL_SET_KERNEL_ARG(ctx->average_kernel, 0, cl_mem, &dst);
    CL_SET_KERNEL_ARG(ctx->average_kernel, 1, cl_mem, &src);
    CL_SET_KERNEL_ARG(ctx->average_kernel, 2, cl_mem, &sum);
    CL_SET_KERNEL_ARG(ctx->average_kernel, 3, cl_mem, &weight);
    cle = clEnqueueNDRangeKernel(ctx->command_queue, ctx->average_kernel, 2,
                                 NULL, worksize3, NULL, 0, NULL, NULL);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to enqueue average kernel: %d.\n",
                     cle);
    cle = clFinish(ctx->command_queue);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to finish kernel: %d.\n", cle);
fail:
    if (tmp)
      av_freep(&tmp);
    if (dxdy)
      av_freep(&dxdy);
    clFinish(ctx->command_queue);
    clReleaseMemObject(ii);
    clReleaseMemObject(weight);
    clReleaseMemObject(sum);
    return err;
}

static int nlmeans_opencl_filter_frame(AVFilterLink *inlink, AVFrame *input)
{
    AVFilterContext    *avctx = inlink->dst;
    AVFilterLink     *outlink = avctx->outputs[0];
    NLMeansOpenCLContext *ctx = avctx->priv;
    AVFrame *output = NULL;
    AVHWFramesContext *input_frames_ctx;
    const AVPixFmtDescriptor *desc;
    enum AVPixelFormat in_format;
    cl_mem src, dst;
    int w, h, err, p, patch, research;

    av_log(ctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input->format),
           input->width, input->height, input->pts);

    if (!input->hw_frames_ctx)
        return AVERROR(EINVAL);
    input_frames_ctx = (AVHWFramesContext*)input->hw_frames_ctx->data;
    in_format = input_frames_ctx->sw_format;

    output = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!output) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = av_frame_copy_props(output, input);
    if (err < 0)
        goto fail;

    if (!ctx->initialised) {
        desc = av_pix_fmt_desc_get(in_format);
        if (!is_format_supported(in_format)) {
            err = AVERROR(EINVAL);
            av_log(avctx, AV_LOG_ERROR, "input format %s not supported\n",
                   av_get_pix_fmt_name(in_format));
            goto fail;
        }
        ctx->chroma_w = AV_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
        ctx->chroma_h = AV_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);

        err = nlmeans_opencl_init(avctx);
        if (err < 0)
            goto fail;
    }

    for (p = 0; p < FF_ARRAY_ELEMS(output->data); p++) {
        src = (cl_mem) input->data[p];
        dst = (cl_mem) output->data[p];

        if (!dst)
            break;
        w = p ? ctx->chroma_w : inlink->w;
        h = p ? ctx->chroma_h : inlink->h;
        patch = (p ? ctx->patch_size_uv : ctx->patch_size) / 2;
        research = (p ? ctx->research_size_uv : ctx->research_size) / 2;
        err = nlmeans_plane(avctx, dst, src, w, h, patch, research);
        if (err < 0)
            goto fail;
    }

    av_frame_free(&input);

    av_log(ctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output->format),
           output->width, output->height, output->pts);

    return ff_filter_frame(outlink, output);

fail:
    clFinish(ctx->command_queue);
    av_frame_free(&input);
    av_frame_free(&output);
    return err;
}

#define RELEASE_KERNEL(k)                                    \
do {                                                         \
    if (k) {                                                 \
        cle = clReleaseKernel(k);                            \
        if (cle != CL_SUCCESS)                               \
            av_log(avctx, AV_LOG_ERROR, "Failed to release " \
                   "kernel: %d.\n", cle);                    \
    }                                                        \
} while(0)

static av_cold void nlmeans_opencl_uninit(AVFilterContext *avctx)
{
    NLMeansOpenCLContext *ctx = avctx->priv;
    cl_int cle;

    RELEASE_KERNEL(ctx->vert_kernel);
    RELEASE_KERNEL(ctx->horiz_kernel);
    RELEASE_KERNEL(ctx->accum_kernel);
    RELEASE_KERNEL(ctx->average_kernel);

    if (ctx->command_queue) {
        cle = clReleaseCommandQueue(ctx->command_queue);
        if (cle != CL_SUCCESS)
            av_log(avctx, AV_LOG_ERROR, "Failed to release "
                   "command queue: %d.\n", cle);
    }

    ff_opencl_filter_uninit(avctx);
}

#define OFFSET(x) offsetof(NLMeansOpenCLContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption nlmeans_opencl_options[] = {
    { "s",  "denoising strength", OFFSET(sigma), AV_OPT_TYPE_DOUBLE, { .dbl = 1.0 }, 1.0, 30.0, FLAGS },
    { "p",  "patch size",                   OFFSET(patch_size),    AV_OPT_TYPE_INT, { .i64 = 2*3+1 }, 0, 99, FLAGS },
    { "pc", "patch size for chroma planes", OFFSET(patch_size_uv), AV_OPT_TYPE_INT, { .i64 = 0 },     0, 99, FLAGS },
    { "r",  "research window",                   OFFSET(research_size),    AV_OPT_TYPE_INT, { .i64 = 7*2+1 }, 0, 99, FLAGS },
    { "rc", "research window for chroma planes", OFFSET(research_size_uv), AV_OPT_TYPE_INT, { .i64 = 0 },     0, 99, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(nlmeans_opencl);

static const AVFilterPad nlmeans_opencl_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &nlmeans_opencl_filter_frame,
        .config_props = &ff_opencl_filter_config_input,
    },
    { NULL }
};

static const AVFilterPad nlmeans_opencl_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = &ff_opencl_filter_config_output,
    },
    { NULL }
};

AVFilter ff_vf_nlmeans_opencl = {
    .name           = "nlmeans_opencl",
    .description    = NULL_IF_CONFIG_SMALL("Non-local means denoiser through OpenCL"),
    .priv_size      = sizeof(NLMeansOpenCLContext),
    .priv_class     = &nlmeans_opencl_class,
    .init           = &ff_opencl_filter_init,
    .uninit         = &nlmeans_opencl_uninit,
    .query_formats  = &ff_opencl_filter_query_formats,
    .inputs         = nlmeans_opencl_inputs,
    .outputs        = nlmeans_opencl_outputs,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
