/*
 * Copyright (c) 2012-2014 Clément Bœsch <u pkh me>
 *
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

/**
 * @file
 * Edge detection filter
 *
 * @see https://en.wikipedia.org/wiki/Canny_edge_detector
 */
#include <Python.h>
#include <numpy/arrayobject.h>

#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

enum FilterMode {
    MODE_WIRES,
    MODE_COLORMIX,
    NB_MODE
};

struct plane_info {
    uint8_t  *tmpbuf;
    uint16_t *gradients;
    char     *directions;
};

typedef struct PythonContext {
    const AVClass *class;
    struct plane_info planes[3];
    int nb_planes;
    double   low, high;
    uint8_t  low_u8, high_u8;
    int mode;

    const char *source_file;
    PyObject *pName;
    PyObject *pModule;

    const char *func_name;
    PyObject *pFunc;
    const char *args;
    PyObject *pArgs, *pValue;
} PythonContext;

#define OFFSET(x) offsetof(PythonContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption python_options[] = {
    { "high", "set high threshold", OFFSET(high), AV_OPT_TYPE_DOUBLE, {.dbl=50/255.}, 0, 1, FLAGS },
    { "low",  "set low threshold",  OFFSET(low),  AV_OPT_TYPE_DOUBLE, {.dbl=20/255.}, 0, 1, FLAGS },
    { "mode", "set mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=MODE_WIRES}, 0, NB_MODE-1, FLAGS, "mode" },
        { "wires",    "white/gray wires on black",  0, AV_OPT_TYPE_CONST, {.i64=MODE_WIRES},    INT_MIN, INT_MAX, FLAGS, "mode" },
        { "colormix", "mix colors",                 0, AV_OPT_TYPE_CONST, {.i64=MODE_COLORMIX}, INT_MIN, INT_MAX, FLAGS, "mode" },

    { "source", "python source file", OFFSET(source_file), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
    { "func", "function name in program", OFFSET(func_name), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
    { "args", "function args", OFFSET(args), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(python);

static av_cold int init(AVFilterContext *ctx)
{
    PythonContext *python = ctx->priv;

    Py_Initialize();

    python->pName = PyString_FromString(python->source_file);
    python->pModule = PyImport_Import(python->pName);
    Py_DECREF(python->pName);

    python->low_u8  = python->low  * 255. + .5;
    python->high_u8 = python->high * 255. + .5;
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    const PythonContext *python = ctx->priv;
    static const enum AVPixelFormat wires_pix_fmts[] = {AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE};
    static const enum AVPixelFormat colormix_pix_fmts[] = {AV_PIX_FMT_GBRP, AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE};
    AVFilterFormats *fmts_list;
    const enum AVPixelFormat *pix_fmts = NULL;

    if (python->mode == MODE_WIRES) {
        pix_fmts = wires_pix_fmts;
    } else if (python->mode == MODE_COLORMIX) {
        pix_fmts = colormix_pix_fmts;
    } else {
        av_assert0(0);
    }
    fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_props(AVFilterLink *inlink)
{
    int p;
    AVFilterContext *ctx = inlink->dst;
    PythonContext *python = ctx->priv;

    python->nb_planes = inlink->format == AV_PIX_FMT_GRAY8 ? 1 : 3;
    for (p = 0; p < python->nb_planes; p++) {
        struct plane_info *plane = &python->planes[p];

        plane->tmpbuf     = av_malloc(inlink->w * inlink->h);
        plane->gradients  = av_calloc(inlink->w * inlink->h, sizeof(*plane->gradients));
        plane->directions = av_malloc(inlink->w * inlink->h);
        if (!plane->tmpbuf || !plane->gradients || !plane->directions)
            return AVERROR(ENOMEM);
    }
    return 0;
}

static int program_python_load(AVFilterContext *avctx)
{
#if 0
    ProgramOpenCLContext *ctx = avctx->priv;
    cl_int cle;
    int err;

    err = ff_opencl_filter_load_program_from_file(avctx, ctx->source_file);
    if (err < 0)
        return err;

    ctx->command_queue = clCreateCommandQueue(ctx->ocf.hwctx->context,
                                              ctx->ocf.hwctx->device_id,
                                              0, &cle);
    if (!ctx->command_queue) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create OpenCL "
               "command queue: %d.\n", cle);
        return AVERROR(EIO);
    }

    ctx->kernel = clCreateKernel(ctx->ocf.program, ctx->kernel_name, &cle);
    if (!ctx->kernel) {
        if (cle == CL_INVALID_KERNEL_NAME) {
            av_log(avctx, AV_LOG_ERROR, "Kernel function '%s' not found in "
                   "program.\n", ctx->kernel_name);
        } else {
            av_log(avctx, AV_LOG_ERROR, "Failed to create kernel: %d.\n", cle);
        }
        return AVERROR(EIO);
    }

    ctx->loaded = 1;
    return 0;
#endif
    return 0;
}

static int program_python_run(AVFilterContext *avctx)
{
#if 0
    AVFilterLink     *outlink = avctx->outputs[0];
    ProgramOpenCLContext *ctx = avctx->priv;
    AVFrame *output = NULL;
    cl_int cle;
    size_t global_work[2];
    cl_mem src, dst;
    int err, input, plane;

    if (!ctx->loaded) {
        err = program_opencl_load(avctx);
        if (err < 0)
            return err;
    }

    output = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!output) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    for (plane = 0; plane < FF_ARRAY_ELEMS(output->data); plane++) {
        dst = (cl_mem)output->data[plane];
        if (!dst)
            break;

        cle = clSetKernelArg(ctx->kernel, 0, sizeof(cl_mem), &dst);
        if (cle != CL_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set kernel "
                   "destination image argument: %d.\n", cle);
            err = AVERROR_UNKNOWN;
            goto fail;
        }
        cle = clSetKernelArg(ctx->kernel, 1, sizeof(cl_uint), &ctx->index);
        if (cle != CL_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to set kernel "
                   "index argument: %d.\n", cle);
            err = AVERROR_UNKNOWN;
            goto fail;
        }

        for (input = 0; input < ctx->nb_inputs; input++) {
            av_assert0(ctx->frames[input]);

            src = (cl_mem)ctx->frames[input]->data[plane];
            av_assert0(src);

            cle = clSetKernelArg(ctx->kernel, 2 + input, sizeof(cl_mem), &src);
            if (cle != CL_SUCCESS) {
                av_log(avctx, AV_LOG_ERROR, "Failed to set kernel "
                       "source image argument %d: %d.\n", input, cle);
                err = AVERROR_UNKNOWN;
                goto fail;
            }
        }

        err = ff_opencl_filter_work_size_from_image(avctx, global_work,
                                                    output, plane, 0);
        if (err < 0)
            goto fail;

        av_log(avctx, AV_LOG_DEBUG, "Run kernel on plane %d "
               "(%zux%zu).\n", plane, global_work[0], global_work[1]);

        cle = clEnqueueNDRangeKernel(ctx->command_queue, ctx->kernel, 2, NULL,
                                     global_work, NULL, 0, NULL, NULL);
        if (cle != CL_SUCCESS) {
            av_log(avctx, AV_LOG_ERROR, "Failed to enqueue kernel: %d.\n",
                   cle);
            err = AVERROR(EIO);
            goto fail;
        }
    }

    cle = clFinish(ctx->command_queue);
    if (cle != CL_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "Failed to finish command queue: %d.\n",
               cle);
        err = AVERROR(EIO);
        goto fail;
    }

    if (ctx->nb_inputs > 0) {
        err = av_frame_copy_props(output, ctx->frames[0]);
        if (err < 0)
            goto fail;
    } else {
        output->pts = ctx->index;
    }
    ++ctx->index;

    av_log(ctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output->format),
           output->width, output->height, output->pts);

    return ff_filter_frame(outlink, output);

fail:
    clFinish(ctx->command_queue);
    av_frame_free(&output);
    return err;
#endif
    return 0;
}

static int python(AVFilterContext *ctx, int nargs)
{
    const PythonContext *python = ctx->priv;
    PyObject *pModule = python->pModule;
    PyObject *pFunc = python->pFunc;
    PyObject *pArgs = python->pArgs;
    PyObject *pValue = python->pValue;
    int i;

    if (!pModule) {
        PyErr_Print();
        fprintf(stderr, "Failed to load \"%s\"\n",  python->source_file);
        return 1;
    }

    pFunc = PyObject_GetAttrString(pModule, python->func_name);
    /* pFunc is a new reference */
    if (pFunc && PyCallable_Check(pFunc)) {
        pArgs = PyTuple_New(nargs);
        for (i = 0; i < nargs; ++i) {
            pValue = PyInt_FromLong(nargs); // FIXME
            if (!pValue) {
                Py_DECREF(pArgs);
                Py_DECREF(pModule);
                fprintf(stderr, "Cannot convert argument\n");
                return 1;
            }
            /* pValue reference stolen here: */
            PyTuple_SetItem(pArgs, i, pValue);
        }
        pValue = PyObject_CallObject(pFunc, pArgs);
        Py_DECREF(pArgs);
        if (pValue != NULL) {
            printf("Result of call: %ld\n", PyInt_AsLong(pValue));
            Py_DECREF(pValue);
        } else {
            Py_DECREF(pFunc);
            Py_DECREF(pModule);
            PyErr_Print();
            fprintf(stderr,"Call failed\n");
            return 1;
        }
    } else {
        if (PyErr_Occurred())
            PyErr_Print();
        fprintf(stderr, "Cannot find function \"%s\"\n",  python->func_name);
    }
    Py_XDECREF(pFunc);
    Py_DECREF(pModule);

    return 0;
}

static void gaussian_blur(AVFilterContext *ctx, int w, int h,
                                uint8_t *dst, int dst_linesize,
                          const uint8_t *src, int src_linesize)
{
    int i, j;

    memcpy(dst, src, w); dst += dst_linesize; src += src_linesize;
    memcpy(dst, src, w); dst += dst_linesize; src += src_linesize;
    for (j = 2; j < h - 2; j++) {
        dst[0] = src[0];
        dst[1] = src[1];
        for (i = 2; i < w - 2; i++) {
            /* Gaussian mask of size 5x5 with sigma = 1.4 */
            dst[i] = ((src[-2*src_linesize + i-2] + src[2*src_linesize + i-2]) * 2
                    + (src[-2*src_linesize + i-1] + src[2*src_linesize + i-1]) * 4
                    + (src[-2*src_linesize + i  ] + src[2*src_linesize + i  ]) * 5
                    + (src[-2*src_linesize + i+1] + src[2*src_linesize + i+1]) * 4
                    + (src[-2*src_linesize + i+2] + src[2*src_linesize + i+2]) * 2

                    + (src[  -src_linesize + i-2] + src[  src_linesize + i-2]) *  4
                    + (src[  -src_linesize + i-1] + src[  src_linesize + i-1]) *  9
                    + (src[  -src_linesize + i  ] + src[  src_linesize + i  ]) * 12
                    + (src[  -src_linesize + i+1] + src[  src_linesize + i+1]) *  9
                    + (src[  -src_linesize + i+2] + src[  src_linesize + i+2]) *  4

                    + src[i-2] *  5
                    + src[i-1] * 12
                    + src[i  ] * 15
                    + src[i+1] * 12
                    + src[i+2] *  5) / 159;
        }
        dst[i    ] = src[i    ];
        dst[i + 1] = src[i + 1];

        dst += dst_linesize;
        src += src_linesize;
    }
    memcpy(dst, src, w); dst += dst_linesize; src += src_linesize;
    memcpy(dst, src, w);
}

enum {
    DIRECTION_45UP,
    DIRECTION_45DOWN,
    DIRECTION_HORIZONTAL,
    DIRECTION_VERTICAL,
};

static int get_rounded_direction(int gx, int gy)
{
    /* reference angles:
     *   tan( pi/8) = sqrt(2)-1
     *   tan(3pi/8) = sqrt(2)+1
     * Gy/Gx is the tangent of the angle (theta), so Gy/Gx is compared against
     * <ref-angle>, or more simply Gy against <ref-angle>*Gx
     *
     * Gx and Gy bounds = [-1020;1020], using 16-bit arithmetic:
     *   round((sqrt(2)-1) * (1<<16)) =  27146
     *   round((sqrt(2)+1) * (1<<16)) = 158218
     */
    if (gx) {
        int tanpi8gx, tan3pi8gx;

        if (gx < 0)
            gx = -gx, gy = -gy;
        gy <<= 16;
        tanpi8gx  =  27146 * gx;
        tan3pi8gx = 158218 * gx;
        if (gy > -tan3pi8gx && gy < -tanpi8gx)  return DIRECTION_45UP;
        if (gy > -tanpi8gx  && gy <  tanpi8gx)  return DIRECTION_HORIZONTAL;
        if (gy >  tanpi8gx  && gy <  tan3pi8gx) return DIRECTION_45DOWN;
    }
    return DIRECTION_VERTICAL;
}

static void sobel(int w, int h,
                       uint16_t *dst, int dst_linesize,
                         int8_t *dir, int dir_linesize,
                  const uint8_t *src, int src_linesize)
{
    int i, j;

    for (j = 1; j < h - 1; j++) {
        dst += dst_linesize;
        dir += dir_linesize;
        src += src_linesize;
        for (i = 1; i < w - 1; i++) {
            const int gx =
                -1*src[-src_linesize + i-1] + 1*src[-src_linesize + i+1]
                -2*src[                i-1] + 2*src[                i+1]
                -1*src[ src_linesize + i-1] + 1*src[ src_linesize + i+1];
            const int gy =
                -1*src[-src_linesize + i-1] + 1*src[ src_linesize + i-1]
                -2*src[-src_linesize + i  ] + 2*src[ src_linesize + i  ]
                -1*src[-src_linesize + i+1] + 1*src[ src_linesize + i+1];

            dst[i] = FFABS(gx) + FFABS(gy);
            dir[i] = get_rounded_direction(gx, gy);
        }
    }
}

static void non_maximum_suppression(int w, int h,
                                          uint8_t  *dst, int dst_linesize,
                                    const  int8_t  *dir, int dir_linesize,
                                    const uint16_t *src, int src_linesize)
{
    int i, j;

#define COPY_MAXIMA(ay, ax, by, bx) do {                \
    if (src[i] > src[(ay)*src_linesize + i+(ax)] &&     \
        src[i] > src[(by)*src_linesize + i+(bx)])       \
        dst[i] = av_clip_uint8(src[i]);                 \
} while (0)

    for (j = 1; j < h - 1; j++) {
        dst += dst_linesize;
        dir += dir_linesize;
        src += src_linesize;
        for (i = 1; i < w - 1; i++) {
            switch (dir[i]) {
            case DIRECTION_45UP:        COPY_MAXIMA( 1, -1, -1,  1); break;
            case DIRECTION_45DOWN:      COPY_MAXIMA(-1, -1,  1,  1); break;
            case DIRECTION_HORIZONTAL:  COPY_MAXIMA( 0, -1,  0,  1); break;
            case DIRECTION_VERTICAL:    COPY_MAXIMA(-1,  0,  1,  0); break;
            }
        }
    }
}

static void double_threshold(int low, int high, int w, int h,
                                   uint8_t *dst, int dst_linesize,
                             const uint8_t *src, int src_linesize)
{
    int i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            if (src[i] > high) {
                dst[i] = src[i];
                continue;
            }

            if ((!i || i == w - 1 || !j || j == h - 1) &&
                src[i] > low &&
                (src[-src_linesize + i-1] > high ||
                 src[-src_linesize + i  ] > high ||
                 src[-src_linesize + i+1] > high ||
                 src[                i-1] > high ||
                 src[                i+1] > high ||
                 src[ src_linesize + i-1] > high ||
                 src[ src_linesize + i  ] > high ||
                 src[ src_linesize + i+1] > high))
                dst[i] = src[i];
            else
                dst[i] = 0;
        }
        dst += dst_linesize;
        src += src_linesize;
    }
}

static void color_mix(int w, int h,
                            uint8_t *dst, int dst_linesize,
                      const uint8_t *src, int src_linesize)
{
    int i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++)
            dst[i] = (dst[i] + src[i]) >> 1;
        dst += dst_linesize;
        src += src_linesize;
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    PythonContext *python = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int p, direct = 0;
    AVFrame *out;

    if (python->mode != MODE_COLORMIX && av_frame_is_writable(in)) {
        direct = 1;
        out = in;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    for (p = 0; p < python->nb_planes; p++) {
        struct plane_info *plane = &python->planes[p];
        uint8_t  *tmpbuf     = plane->tmpbuf;
        uint16_t *gradients  = plane->gradients;
        int8_t   *directions = plane->directions;

        /* gaussian filter to reduce noise  */
        gaussian_blur(ctx, inlink->w, inlink->h,
                      tmpbuf,      inlink->w,
                      in->data[p], in->linesize[p]);

        /* compute the 16-bits gradients and directions for the next step */
        sobel(inlink->w, inlink->h,
              gradients, inlink->w,
              directions,inlink->w,
              tmpbuf,    inlink->w);

        /* non_maximum_suppression() will actually keep & clip what's necessary and
         * ignore the rest, so we need a clean output buffer */
        memset(tmpbuf, 0, inlink->w * inlink->h);
        non_maximum_suppression(inlink->w, inlink->h,
                                tmpbuf,    inlink->w,
                                directions,inlink->w,
                                gradients, inlink->w);

        /* keep high values, or low values surrounded by high values */
        double_threshold(python->low_u8, python->high_u8,
                         inlink->w, inlink->h,
                         out->data[p], out->linesize[p],
                         tmpbuf,       inlink->w);

        if (python->mode == MODE_COLORMIX) {
            color_mix(inlink->w, inlink->h,
                      out->data[p], out->linesize[p],
                      in->data[p], in->linesize[p]);
        }
    }
  Py_SetProgramName(NULL);  /* optional but recommended */
  Py_Initialize();
  PyRun_SimpleString("from time import time,ctime\n"
                     "print 'Today is',ctime(time())\n");
  Py_Finalize();
    if (!direct)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    int p;
    PythonContext *python = ctx->priv;

    for (p = 0; p < python->nb_planes; p++) {
        struct plane_info *plane = &python->planes[p];
        av_freep(&plane->tmpbuf);
        av_freep(&plane->gradients);
        av_freep(&plane->directions);
    }
}

static const AVFilterPad python_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad python_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_python = {
    .name          = "python",
    .description   = NULL_IF_CONFIG_SMALL("Detect and draw edge."),
    .priv_size     = sizeof(PythonContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = python_inputs,
    .outputs       = python_outputs,
    .priv_class    = &python_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
