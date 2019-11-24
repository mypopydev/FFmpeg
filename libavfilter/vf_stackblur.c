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

static uint16_t const stackblur_mul[255] = {
    512,512,456,512,328,456,335,512,405,328,271,456,388,335,292,512,
    454,405,364,328,298,271,496,456,420,388,360,335,312,292,273,512,
    482,454,428,405,383,364,345,328,312,298,284,271,259,496,475,456,
    437,420,404,388,374,360,347,335,323,312,302,292,282,273,265,512,
    497,482,468,454,441,428,417,405,394,383,373,364,354,345,337,328,
    320,312,305,298,291,284,278,271,265,259,507,496,485,475,465,456,
    446,437,428,420,412,404,396,388,381,374,367,360,354,347,341,335,
    329,323,318,312,307,302,297,292,287,282,278,273,269,265,261,512,
    505,497,489,482,475,468,461,454,447,441,435,428,422,417,411,405,
    399,394,389,383,378,373,368,364,359,354,350,345,341,337,332,328,
    324,320,316,312,309,305,301,298,294,291,287,284,281,278,274,271,
    268,265,262,259,257,507,501,496,491,485,480,475,470,465,460,456,
    451,446,442,437,433,428,424,420,416,412,408,404,400,396,392,388,
    385,381,377,374,370,367,363,360,357,354,350,347,344,341,338,335,
    332,329,326,323,320,318,315,312,310,307,304,302,299,297,294,292,
    289,287,285,282,280,278,275,273,271,269,267,265,263,261,259
};

static uint8_t const stackblur_shr[255] = {
     9, 11, 12, 13, 13, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17,
    17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18, 18, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 20, 20, 20,
    20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 21,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24
};

typedef struct StackBlurContext {
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
    { "radius", "Radius of the stack blurring box", OFFSET(radius),  AV_OPT_TYPE_INT, {.i64 = 2}, 2, 254, FLAGS },
    { "r",      "Radius of the stack blurring box", OFFSET(radius),  AV_OPT_TYPE_INT, {.i64 = 2}, 2, 254, FLAGS },
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

/// Stackblur algorithm body
static void vstackblur_rgba(uint8_t* src,	///< input image data
                            int w,	        ///< image width
                            int h,	        ///< image height
                            int radius,	        ///< blur intensity (should be in 2..254 range)
                            int cores,		///< total number of working threads
                            int core,		///< current thread number
                            uint8_t* stack	///< stack buffer
				  )
{
    uint32_t x, y, xp, i;
    uint32_t sp;
    uint32_t stack_start;
    uint8_t *stack_ptr;

    uint8_t *src_ptr;
    uint8_t *dst_ptr;

    uint64_t sum_r;
    uint64_t sum_g;
    uint64_t sum_b;
    uint64_t sum_a;
    uint64_t sum_in_r;
    uint64_t sum_in_g;
    uint64_t sum_in_b;
    uint64_t sum_in_a;
    uint64_t sum_out_r;
    uint64_t sum_out_g;
    uint64_t sum_out_b;
    uint64_t sum_out_a;

    uint32_t wm = w - 1;
    uint32_t w4 = w * 4;
    uint32_t div = (radius * 2) + 1;
    uint32_t mul_sum = stackblur_mul[radius];
    uint8_t shr_sum = stackblur_shr[radius];

    int minY = core * h / cores;
    int maxY = (core + 1) * h / cores;

    for(y = minY; y < maxY; y++) {
        sum_r = sum_g = sum_b = sum_a =
     sum_in_r = sum_in_g = sum_in_b = sum_in_a =
    sum_out_r = sum_out_g = sum_out_b = sum_out_a = 0;

        src_ptr = src + w4 * y; // start of line (0,y)

        for(i = 0; i <= radius; i++) {
            stack_ptr    = &stack[ 4 * i ];
            stack_ptr[0] = src_ptr[0];
            stack_ptr[1] = src_ptr[1];
            stack_ptr[2] = src_ptr[2];
            stack_ptr[3] = src_ptr[3];
            sum_r += src_ptr[0] * (i + 1);
            sum_g += src_ptr[1] * (i + 1);
            sum_b += src_ptr[2] * (i + 1);
            sum_a += src_ptr[3] * (i + 1);
            sum_out_r += src_ptr[0];
            sum_out_g += src_ptr[1];
            sum_out_b += src_ptr[2];
            sum_out_a += src_ptr[3];
        }

        for(i = 1; i <= radius; i++) {
            if (i <= wm) src_ptr += 4;
            stack_ptr = &stack[ 4 * (i + radius) ];
            stack_ptr[0] = src_ptr[0];
            stack_ptr[1] = src_ptr[1];
            stack_ptr[2] = src_ptr[2];
            stack_ptr[3] = src_ptr[3];
            sum_r += src_ptr[0] * (radius + 1 - i);
            sum_g += src_ptr[1] * (radius + 1 - i);
            sum_b += src_ptr[2] * (radius + 1 - i);
            sum_a += src_ptr[3] * (radius + 1 - i);
            sum_in_r += src_ptr[0];
            sum_in_g += src_ptr[1];
            sum_in_b += src_ptr[2];
            sum_in_a += src_ptr[3];
        }

        sp = radius;
        xp = radius;
        if (xp > wm) xp = wm;
        src_ptr = src + 4 * (xp + y * w); // img.pix_ptr(xp, y);
        dst_ptr = src + y * w4;           // img.pix_ptr(0, y);
        for(x = 0; x < w; x++) {
            dst_ptr[0] = (sum_r * mul_sum) >> shr_sum;
            dst_ptr[1] = (sum_g * mul_sum) >> shr_sum;
            dst_ptr[2] = (sum_b * mul_sum) >> shr_sum;
            dst_ptr[3] = (sum_a * mul_sum) >> shr_sum;
            dst_ptr += 4;

            sum_r -= sum_out_r;
            sum_g -= sum_out_g;
            sum_b -= sum_out_b;
            sum_a -= sum_out_a;

            stack_start = sp + div - radius;
            if (stack_start >= div) stack_start -= div;
            stack_ptr = &stack[4 * stack_start];

            sum_out_r -= stack_ptr[0];
            sum_out_g -= stack_ptr[1];
            sum_out_b -= stack_ptr[2];
            sum_out_a -= stack_ptr[3];

            if(xp < wm) {
                src_ptr += 4;
                ++xp;
            }

            stack_ptr[0] = src_ptr[0];
            stack_ptr[1] = src_ptr[1];
            stack_ptr[2] = src_ptr[2];
            stack_ptr[3] = src_ptr[3];

            sum_in_r += src_ptr[0];
            sum_in_g += src_ptr[1];
            sum_in_b += src_ptr[2];
            sum_in_a += src_ptr[3];
            sum_r    += sum_in_r;
            sum_g    += sum_in_g;
            sum_b    += sum_in_b;
            sum_a    += sum_in_a;

            ++sp;
            if (sp >= div) sp = 0;
            stack_ptr = &stack[sp*4];

            sum_out_r += stack_ptr[0];
            sum_out_g += stack_ptr[1];
            sum_out_b += stack_ptr[2];
            sum_out_a += stack_ptr[3];
            sum_in_r  -= stack_ptr[0];
            sum_in_g  -= stack_ptr[1];
            sum_in_b  -= stack_ptr[2];
            sum_in_a  -= stack_ptr[3];
        }
    }
}

/// Stackblur algorithm body
static void hstackblur_rgba(uint8_t* src,	///< input image data
                            int w,	        ///< image width
                            int h,	        ///< image height
                            int radius,	        ///< blur intensity (should be in 2..254 range)
                            int cores,		///< total number of working threads
                            int core,		///< current thread number
                            uint8_t* stack	///< stack buffer
				  )
{
    uint32_t x, y, yp, i;
    uint32_t sp;
    uint32_t stack_start;
    uint8_t *stack_ptr;

    uint8_t *src_ptr;
    uint8_t *dst_ptr;

    uint64_t sum_r;
    uint64_t sum_g;
    uint64_t sum_b;
    uint64_t sum_a;
    uint64_t sum_in_r;
    uint64_t sum_in_g;
    uint64_t sum_in_b;
    uint64_t sum_in_a;
    uint64_t sum_out_r;
    uint64_t sum_out_g;
    uint64_t sum_out_b;
    uint64_t sum_out_a;

    uint32_t hm = h - 1;
    uint32_t w4 = w * 4;
    uint32_t div = (radius * 2) + 1;
    uint32_t mul_sum = stackblur_mul[radius];
    uint8_t shr_sum  = stackblur_shr[radius];

    int minX = core * w / cores;
    int maxX = (core + 1) * w / cores;

    for(x = minX; x < maxX; x++) {
        sum_r =	sum_g =	sum_b =	sum_a =
     sum_in_r = sum_in_g = sum_in_b = sum_in_a =
    sum_out_r = sum_out_g = sum_out_b = sum_out_a = 0;

        src_ptr = src + 4 * x; // x,0
        for(i = 0; i <= radius; i++) {
            stack_ptr    = &stack[i * 4];
            stack_ptr[0] = src_ptr[0];
            stack_ptr[1] = src_ptr[1];
            stack_ptr[2] = src_ptr[2];
            stack_ptr[3] = src_ptr[3];
            sum_r           += src_ptr[0] * (i + 1);
            sum_g           += src_ptr[1] * (i + 1);
            sum_b           += src_ptr[2] * (i + 1);
            sum_a           += src_ptr[3] * (i + 1);
            sum_out_r       += src_ptr[0];
            sum_out_g       += src_ptr[1];
            sum_out_b       += src_ptr[2];
            sum_out_a       += src_ptr[3];
        }
        for(i = 1; i <= radius; i++) {
            if(i <= hm) src_ptr += w4; // +stride

            stack_ptr = &stack[4 * (i + radius)];
            stack_ptr[0] = src_ptr[0];
            stack_ptr[1] = src_ptr[1];
            stack_ptr[2] = src_ptr[2];
            stack_ptr[3] = src_ptr[3];
            sum_r += src_ptr[0] * (radius + 1 - i);
            sum_g += src_ptr[1] * (radius + 1 - i);
            sum_b += src_ptr[2] * (radius + 1 - i);
            sum_a += src_ptr[3] * (radius + 1 - i);
            sum_in_r += src_ptr[0];
            sum_in_g += src_ptr[1];
            sum_in_b += src_ptr[2];
            sum_in_a += src_ptr[3];
        }

        sp = radius;
        yp = radius;
        if (yp > hm) yp = hm;
        src_ptr = src + 4 * (x + yp * w); // img.pix_ptr(x, yp);
        dst_ptr = src + 4 * x; 	          // img.pix_ptr(x, 0);
        for(y = 0; y < h; y++) {
            dst_ptr[0] = (sum_r * mul_sum) >> shr_sum;
            dst_ptr[1] = (sum_g * mul_sum) >> shr_sum;
            dst_ptr[2] = (sum_b * mul_sum) >> shr_sum;
            dst_ptr[3] = (sum_a * mul_sum) >> shr_sum;
            dst_ptr += w4;

            sum_r -= sum_out_r;
            sum_g -= sum_out_g;
            sum_b -= sum_out_b;
            sum_a -= sum_out_a;

            stack_start = sp + div - radius;
            if(stack_start >= div) stack_start -= div;
            stack_ptr = &stack[4 * stack_start];

            sum_out_r -= stack_ptr[0];
            sum_out_g -= stack_ptr[1];
            sum_out_b -= stack_ptr[2];
            sum_out_a -= stack_ptr[3];

            if(yp < hm) {
                src_ptr += w4; // stride
                ++yp;
            }

            stack_ptr[0] = src_ptr[0];
            stack_ptr[1] = src_ptr[1];
            stack_ptr[2] = src_ptr[2];
            stack_ptr[3] = src_ptr[3];

            sum_in_r += src_ptr[0];
            sum_in_g += src_ptr[1];
            sum_in_b += src_ptr[2];
            sum_in_a += src_ptr[3];
            sum_r    += sum_in_r;
            sum_g    += sum_in_g;
            sum_b    += sum_in_b;
            sum_a    += sum_in_a;

            ++sp;
            if (sp >= div) sp = 0;
            stack_ptr = &stack[sp*4];

            sum_out_r += stack_ptr[0];
            sum_out_g += stack_ptr[1];
            sum_out_b += stack_ptr[2];
            sum_out_a += stack_ptr[3];
            sum_in_r  -= stack_ptr[0];
            sum_in_g  -= stack_ptr[1];
            sum_in_b  -= stack_ptr[2];
            sum_in_a  -= stack_ptr[3];
        }
    }
}

// Stack Blur v1.0
//
// Author: Mario Klingemann <mario@quasimondo.com>
// http://incubator.quasimondo.com
// created Feburary 29, 2004
//
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
static void stack_blur(StackBlurContext *s, uint8_t *pix, int w, int h, int nb_comps)
{
    uint32_t wm = w - 1;
    uint32_t hm = h - 1;
    uint32_t wh = w * h;

    int radius = s->radius;

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

    uint32_t div = 2 * radius + 1;

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

            rbs = r1 - FFABS(i);
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

            rbs = r1 - FFABS(i);

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

    stack_blur(s, in->data[0], inlink->w, inlink->h, desc->nb_components);

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
