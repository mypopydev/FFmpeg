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

#include "jsonrpc.h"

typedef struct PythonContext {
    const AVClass *class;

    const char *source_file; /* python file */
    PyObject *pName;
    PyObject *pModule;

    const char *func_name;
    PyObject *pFunc;
    const char *args;
    PyObject *pArgs, *pValue;

    int nargs;

    const char *rpc;
    const char *rpcfile; /* json-rpc file */

    const char *cmd;

    json_t *requests;

    uint64_t index;
} PythonContext;

static json_t *jsonrpc_request(AVFilterContext *avctx, json_t *json_request, AVFrame *in);

static av_cold int init(AVFilterContext *ctx)
{
    PythonContext *python = ctx->priv;
    int i;
    int len = 0;

    Py_Initialize();
    PyRun_SimpleString("import sys");
    PyRun_SimpleString("sys.path.append(\".\")");

    if (python->source_file) {
        python->pName = PyString_FromString(python->source_file);
        python->pModule = PyImport_Import(python->pName);
        Py_DECREF(python->pName);
    }

    if (python->rpc)
        python->requests = jsonrpc_parser(python->rpc, strlen(python->rpc),
                                          python);

    if (python->rpcfile)
        python->requests = jsonrpc_parser_file(python->rpcfile, python);

    len = json_array_size(python->requests);
    for (i=0; i < len; i++) {
        json_t *request = json_array_get(python->requests, i);
        printf(" == Type : %d\n",  json_typeof(request));
        jsonrpc_request(ctx, request, NULL);
    }

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

static int run_python_rpc(json_t *req)
{
    return 0;
}

static json_t *jsonrpc_request(AVFilterContext *avctx, json_t *json_request, AVFrame *in)
{
    size_t flags = 0;
    json_error_t error;
    const char *str_version = NULL;
    int rc;
    json_t *data = NULL;
    int valid_id = 0;
    const char *str_method = NULL;
    json_t *json_params = NULL;
    json_t *json_id = NULL;

    const PythonContext *python = avctx->priv;
    PyObject *pModule =python->pModule;
    PyObject *pFunc = NULL;
    PyObject *pCallback = NULL;
    PyObject *pCallbackRet = NULL;
    PyObject *pArgs = NULL;
    PyObject *pValue = NULL;

    rc = json_unpack_ex(json_request, &error, flags, "{s:s,s:s,s?o,s?o}",
                        "jsonrpc", &str_version,
                        "method", &str_method,
                        "params", &json_params,
                        "id", &json_id);
    if (rc==-1) {
        data = json_string(error.text);
        goto invalid;
    }

    if (0 != strcmp(str_version, "2.0")) {
        data = json_string("\"jsonrpc\" MUST be exactly \"2.0\"");
        goto invalid;
    }

    if (json_id) {
        if (!json_is_string(json_id) && !json_is_number(json_id) && !json_is_null(json_id)) {
            data = json_string("\"id\" MUST contain a String, Number, or NULL value if included");
            goto invalid;
        }
    }

    /*  Note that we only return json_id in the error response after we have
     *  established that it is jsonrpc/2.0 compliant otherwise we would be
     *  returning a non-compliant response ourselves! */
    valid_id = 1;
#if 0
    if (!pModule) {
        PyErr_Print();
        fprintf(stderr, "Failed to load python file\"%s\"\n",  python->source_file);
        goto invalid;
    }

    pFunc = PyObject_GetAttrString(pModule, str_method);
    /* pFunc is a new reference */
    if (pFunc && PyCallable_Check(pFunc) && json_params) {
#endif

        if (!json_is_array(json_params)) {
            data = json_string("\"params\" MUST be Array if included");
            goto invalid;
        }

        size_t len = json_array_size(json_params);
        const char *callback = NULL;
        const char *cmd = NULL;
        int nframes;
        json_t *info = NULL;
        pArgs = PyTuple_New(len);

        /* use s,s,i,[],AVFrame and AVFrame always the last one param in rpc call
         *     Callback(s), command list(s), frame index(i), information for next round([]), AVframe
         */
        rc = json_unpack_ex(json_params, &error, flags, "[s,s,i,[]]", &callback, &cmd, &nframes, &info);
        if (rc == 0) {
            PyObject *p;
            PyObject *info;

            printf("param : %s\n",  callback);
            printf("param : %s\n",  cmd);
            pCallback = PyObject_GetAttrString(pModule, callback);
            if (pCallback && PyCallable_Check(pCallback)) {
                PyObject *pCmds = PyTuple_New(1);
                PyTuple_SetItem(pCmds, 0, Py_BuildValue("s", cmd));
                pCallbackRet = PyObject_CallObject(pCallback, pCmds);
            }

            printf("param : %d\n",  nframes);
            //printf("param type : %d\n",  json_typeof(param));
            pArgs = PyTuple_New(4); /* s, i, [], AVFrame */
            PyTuple_SetItem(pArgs, 0, pCallbackRet);
            PyTuple_SetItem(pArgs, 1, Py_BuildValue("i", nframes));
            /* information for next round */
            PyTuple_SetItem(pArgs, 2, Py_BuildValue("s", cmd));

            PyTuple_SetItem(pArgs, 3, pCallbackRet); /* FIXME */


            pValue = PyObject_CallObject(pFunc, pArgs);

        }
        int i;
        for (i=0; i < len; i++) {
            json_t *param = json_array_get(json_params, i);
            /*
              json_t *rep = jsonrpc_handle_request_single(req, method_table, userdata);
              if (rep) {
              if (!json_response)
              json_response = json_array();
              json_array_append_new(json_response, rep);
              }
            */
            printf("param type : %d\n",  json_typeof(param));
        }
#if 0
    }
#endif

    return NULL;

invalid:
    if (!valid_id)
        json_id = NULL;
    return jsonrpc_error_response(json_id,
                                  jsonrpc_error_object_predefined(JSONRPC_INVALID_REQUEST, data));
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

    //python_run(ctx, python->nargs);

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
    { "rpc", "json-rpc format to python script", OFFSET(rpc), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
    { "rpcfile", "read json-rpc from file", OFFSET(rpcfile), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS },
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
