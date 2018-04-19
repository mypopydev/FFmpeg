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

/**
 * @file
 * Python script filter
 *
 * Embedding Python in avfilter
 *
 * @see https://docs.python.org/2/extending/embedding.html
 */
#include <Python.h>
#include <numpy/arrayobject.h>

#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct PythonContext {
    const AVClass *class;

    const char *source_file;
    PyObject *pName;
    PyObject *pModule;

    const char *func_name;
    PyObject *pFunc;
    const char *args;
    PyObject *pArgs, *pValue;

    int nargs;
} PythonContext;

static av_cold int init(AVFilterContext *ctx)
{
    PythonContext *python = ctx->priv;

    Py_Initialize();

    python->pName = PyString_FromString(python->source_file);
    python->pModule = PyImport_Import(python->pName);
    Py_DECREF(python->pName);

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    const PythonContext *python = ctx->priv;
    enum AVPixelFormat pix_fmts[] = {
         AV_PIX_FMT_NV12,
         AV_PIX_FMT_VAAPI,
         AV_PIX_FMT_NONE,
    };
    int err;

    if ((err = ff_formats_ref(ff_make_format_list(pix_fmts),
                              &ctx->inputs[0]->out_formats)) < 0)
        return err;
    if ((err = ff_formats_ref(ff_make_format_list(pix_fmts),
                              &ctx->outputs[0]->in_formats)) < 0)
        return err;

    return 0;
}

static int config_props(AVFilterLink *inlink)
{
    int p;
    AVFilterContext *ctx = inlink->dst;
    PythonContext *python = ctx->priv;

    return 0;
}

static int program_python_load(AVFilterContext *avctx)
{
    return 0;
}

static int program_python_run(AVFilterContext *avctx)
{
    return 0;
}

static int python_run(AVFilterContext *ctx, int nargs)
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
            pValue = PyInt_FromLong(2); // FIXME
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
            Py_DECREF(pModule); // FIXME
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
    Py_DECREF(pModule); // FIXME

    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    PythonContext *python = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    int err;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        err = AVERROR(ENOMEM);
        goto fail;
    }

    python_run(ctx, python->nargs);

    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&out);
    return err;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    int p;
    PythonContext *python = ctx->priv;
}


#define OFFSET(x) offsetof(PythonContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption python_options[] = {
    { "source", "python program source file", OFFSET(source_file), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
    { "func", "function name in program", OFFSET(func_name), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
    { "args", "function args", OFFSET(args), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
    { "nargs", "function args number", OFFSET(nargs), AV_OPT_TYPE_INT,   { .i64 = 0 }, 0, INT_MAX, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(python);

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
    .description   = NULL_IF_CONFIG_SMALL("Filter video using an python script."),
    .priv_size     = sizeof(PythonContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = python_inputs,
    .outputs       = python_outputs,
    .priv_class    = &python_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
