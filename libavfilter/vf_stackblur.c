/*
 * Copyright (c) 2019 Jun Zhao
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
 * Stack blur filter
 *
 * Stack Blur Algorithm by Mario Klingemann <mario@quasimondo.com>
 *
 * @see http://incubator.quasimondo.com/processing/stackblur.pde
 */

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct SuperFastBlurContext {
    const AVClass *class;

    int radius;

    uint32_t *vMIN;
    uint8_t *rgb;
    uint8_t *dv;

    int *stack;
} StackBlurContext;

#define OFFSET(x) offsetof(StackBlurContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption stackblur_options[] = {
    { "radius", "Radius of the stack blurring box", OFFSET(radius),  AV_OPT_TYPE_INT, {.i64 = 2}, 1, 10, FLAGS },
    { "r",      "Radius of the stack blurring box", OFFSET(radius),  AV_OPT_TYPE_INT, {.i64 = 2}, 1, 10, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(stackblur);

static av_cold int init(AVFilterContext *ctx)
{
    StackBlurContext *s = ctx->priv;

    // This line precalculates a lookup table for all the possible
    // mean values that can occur. This is to avoid costly division
    // in the inner loop. On some systems doing the division directly
    // instead of a doing an array lookup might actually be faster
    // nowadays.
    uint32_t div = 2 * s->radius + 1;
    int divsum = (div + 1) >> 1;
    divsum *= divsum;
    s->dv = av_malloc(256 * divsum * sizeof(*s->dv));
    if (!s->dv)
        return AVERROR(ENOMEM);
    for (int i = 0; i < 256 * divsum; i++)
        s->dv[i] = i / divsum;

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    StackBlurContext *s = ctx->priv;

    uint32_t div = 2 * s->radius + 1;
    uint32_t wh = inlink->w * inlink->h;

    s->rgb   = av_malloc(sizeof(*s->rgb) * wh * 3);
    s->vMIN  = av_malloc(FFMAX(inlink->w, inlink->h) * sizeof(*s->vMIN));
    s->stack = av_malloc(div * 3 * sizeof(*s->stack));
    if (!s->vMIN || !s->rgb || !s->stack)
        return AVERROR(ENOMEM);

    return 0;
}

// Stack Blur v1.0
//
// Author: Mario Klingemann <mario@quasimondo.com>
// http://incubator.quasimondo.com
// created Feburary 29, 2004
// This is a compromise between Gaussian Blur and Box blur
// It creates much better looking blurs than Box Blur, but is faster
// than the Gaussian Blur implementation.
//
// Called it Stack Blur because this describes best how this
// filter works internally: it creates a kind of moving stack
// of colors whilst scanning through the image. Thereby it
// just has to add one new block of color to the right side
// of the stack and remove the leftmost color. The remaining
// colors on the topmost layer of the stack are either added on
// or reduced by one, depending on if they are on the right or
// on the left side of the stack.
//
static void stack_blur(StackBlurContext *s, uint8_t *pix, int w, int h, int nb_comps, int radius)
{
    uint32_t wm = w - 1;
    uint32_t hm = h - 1;
    uint32_t wh = w * h;

    uint8_t *rgb = s->rgb;
    uint8_t *r = rgb;
    uint8_t *g = rgb + wh;
    uint8_t *b = rgb + wh * 2;
    int rsum, gsum, bsum, x, y, i, p, yp, yi, yw;

    uint32_t stackpointer;
    uint32_t stackstart;
    int *sir;
    int rbs;
    int r1 = radius + 1;
    int routsum, goutsum, boutsum;
    int rinsum, ginsum, binsum;

    uint32_t div = 2 * s->radius + 1;

    int(*stack)[3] = (int(*)[3])(s->stack);
    uint32_t *vMIN = s->vMIN;
    uint8_t *dv = s->dv;

    yw = yi = 0;

    for (y = 0; y < h; y++) {
        rinsum = ginsum = binsum = routsum = goutsum = boutsum = rsum = gsum = bsum = 0;
        for (i = -radius; i <= radius; i++) {
            p = yi + (FFMIN(wm, FFMAX(i, 0)));
            sir = stack[i + radius];
            sir[0] = pix[(p*nb_comps)];
            sir[1] = pix[(p*nb_comps) + 1];
            sir[2] = pix[(p*nb_comps) + 2];

            rbs = r1 - abs(i);
            rsum += sir[0] * rbs;
            gsum += sir[1] * rbs;
            bsum += sir[2] * rbs;
            if (i > 0) {
                rinsum += sir[0];
                ginsum += sir[1];
                binsum += sir[2];
            } else {
                routsum += sir[0];
                goutsum += sir[1];
                boutsum += sir[2];
            }
        }
        stackpointer = radius;

        for (x = 0; x < w; x++) {
            r[yi] = dv[rsum];
            g[yi] = dv[gsum];
            b[yi] = dv[bsum];

            rsum -= routsum;
            gsum -= goutsum;
            bsum -= boutsum;

            stackstart = stackpointer - radius + div;
            sir = stack[stackstart % div];

            routsum -= sir[0];
            goutsum -= sir[1];
            boutsum -= sir[2];

            if (y == 0)
                vMIN[x] = FFMIN(x + radius + 1, wm);
            p = yw + vMIN[x];

            sir[0] = pix[(p*nb_comps)];
            sir[1] = pix[(p*nb_comps) + 1];
            sir[2] = pix[(p*nb_comps) + 2];
            rinsum += sir[0];
            ginsum += sir[1];
            binsum += sir[2];

            rsum += rinsum;
            gsum += ginsum;
            bsum += binsum;

            stackpointer = (stackpointer + 1) % div;
            sir = stack[(stackpointer) % div];

            routsum += sir[0];
            goutsum += sir[1];
            boutsum += sir[2];

            rinsum -= sir[0];
            ginsum -= sir[1];
            binsum -= sir[2];

            yi++;
        }
        yw += w;
    }
    for (x = 0; x < w; x++) {
        rinsum = ginsum = binsum = routsum = goutsum = boutsum = rsum = gsum = bsum = 0;
        yp = -radius * w;
        for (i = -radius; i <= radius; i++) {
            yi = FFMAX(0, yp) + x;

            sir = stack[i + radius];

            sir[0] = r[yi];
            sir[1] = g[yi];
            sir[2] = b[yi];

            rbs = r1 - abs(i);

            rsum += r[yi] * rbs;
            gsum += g[yi] * rbs;
            bsum += b[yi] * rbs;

            if (i > 0) {
                rinsum += sir[0];
                ginsum += sir[1];
                binsum += sir[2];
            } else {
                routsum += sir[0];
                goutsum += sir[1];
                boutsum += sir[2];
            }

            if (i < hm)
                yp += w;
        }
        yi = x;
        stackpointer = radius;
        for (y = 0; y < h; y++) {
            pix[(yi*nb_comps)]     = dv[rsum];
            pix[(yi*nb_comps) + 1] = dv[gsum];
            pix[(yi*nb_comps) + 2] = dv[bsum];
            rsum -= routsum;
            gsum -= goutsum;
            bsum -= boutsum;

            stackstart = stackpointer - radius + div;
            sir = stack[stackstart % div];

            routsum -= sir[0];
            goutsum -= sir[1];
            boutsum -= sir[2];

            if (x == 0)
                vMIN[y] = FFMIN(y + r1, hm) * w;
            p = x + vMIN[y];

            sir[0] = r[p];
            sir[1] = g[p];
            sir[2] = b[p];

            rinsum += sir[0];
            ginsum += sir[1];
            binsum += sir[2];

            rsum += rinsum;
            gsum += ginsum;
            bsum += binsum;

            stackpointer = (stackpointer + 1) % div;
            sir = stack[stackpointer];

            routsum += sir[0];
            goutsum += sir[1];
            boutsum += sir[2];

            rinsum -= sir[0];
            ginsum -= sir[1];
            binsum -= sir[2];

            yi += w;
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    StackBlurContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    stack_blur(s, in->data[0], inlink->w, inlink->h, desc->nb_components, s->radius);

    return ff_filter_frame(outlink, in);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    StackBlurContext *s = ctx->priv;

    av_freep(&s->rgb);
    av_freep(&s->vMIN);
    av_freep(&s->dv);
    av_freep(&s->stack);
}

static const AVFilterPad stackblur_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad stackblur_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_stackblur = {
    .name          = "stackblur",
    .description   = NULL_IF_CONFIG_SMALL("Blur the input with stack algorithm."),
    .priv_size     = sizeof(StackBlurContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = stackblur_inputs,
    .outputs       = stackblur_outputs,
    .priv_class    = &stackblur_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
