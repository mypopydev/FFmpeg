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
 * Super fast blur filter
 *
 * @see http://incubator.quasimondo.com/processing/superfast_blur.php
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
    uint32_t *vMAX;

    uint8_t *r;
    uint8_t *g;
    uint8_t *b;

    uint8_t *dv;
} SuperFastBlurContext;

#define OFFSET(x) offsetof(SuperFastBlurContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption superfastblur_options[] = {
    { "radius", "Radius of the super fast blurring box", OFFSET(radius),  AV_OPT_TYPE_INT, {.i64 = 2}, 1, 10, FLAGS },
    { "r",      "Radius of the super fast blurring box", OFFSET(radius),  AV_OPT_TYPE_INT, {.i64 = 2}, 1, 10, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(superfastblur);

static av_cold int init(AVFilterContext *ctx)
{
    SuperFastBlurContext *s = ctx->priv;

    // This line precalculates a lookup table for all the possible
    // mean values that can occur. This is to avoid costly division
    // in the inner loop. On some systems doing the division directly
    // instead of a doing an array lookup might actually be faster
    // nowadays.
    uint32_t div = 2 * s->radius + 1;
    s->dv = av_malloc(sizeof(*s->dv) * 256 * div);
    if (!s->dv)
        return AVERROR(ENOMEM);
    for (int i = 0; i < 256 * div; i++)
        s->dv[i] = i / div;

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
    SuperFastBlurContext *s = ctx->priv;

    uint32_t wm = inlink->w - 1;
    uint32_t wh = inlink->w * inlink->h;

    s->vMIN = av_malloc(sizeof(wm) * FFMAX(inlink->w, inlink->h));
    s->vMAX = av_malloc(sizeof(wm) * FFMAX(inlink->w, inlink->h));
    s->r = av_malloc(sizeof(*s->r) * wh);
    s->g = av_malloc(sizeof(*s->g) * wh);
    s->b = av_malloc(sizeof(*s->b) * wh);

    if (!s->vMIN || !s->vMAX || !s->r || !s->g || !s->b)
        return AVERROR(ENOMEM);

    return 0;
}

/*
 * Super Fast Blur v1.1+
 * by Mario Klingemann <http://incubator.quasimondo.com>
 * Original address: http://incubator.quasimondo.com/processing/superfastblur.pde
 *
 * Tip: Multiple invocations of this filter with a small
 * radius will approximate a gaussian blur quite well.
 */
static void superfast_blur(SuperFastBlurContext *s, uint8_t *pix, int w, int h, int nb_comps)
{
    uint32_t wm, hm;
    uint32_t *vMIN, *vMAX;
    uint8_t *r, *g, *b, *dv;
    uint32_t rsum, gsum, bsum;
    uint32_t p, p1, p2, yi, yw;

    int radius;

    int x, y, i, yp;

    wm = w - 1;
    hm = h - 1;

    vMIN = s->vMIN;
    vMAX = s->vMAX;
    r = s->r;
    g = s->g;
    b = s->b;

    dv = s->dv;

    radius = s->radius;

    yw = yi = 0;
    for (y = 0; y < h; y++) {
        rsum = gsum = bsum = 0;
        // The reason why this algorithm is fast is that it uses a sliding
        // window and thus reduces the number of required pixel lookups.
        // The window slides from the left edge to the right (and in the
        // second pass from top to bottom) and only adds one pixel at the
        // right and removes one from the left. The code above initializes
        // the window by prefilling the window with the leftmost edge pixel
        // depending on the kernel size.
        for (i = -radius; i <= radius; i++) {
            p = (yi + FFMIN(wm, FFMAX(i, 0))) * nb_comps;
            rsum += pix[p];
            gsum += pix[p + 1];
            bsum += pix[p + 2];
        }

        for (x = 0; x < w; x++) {
            r[yi] = dv[rsum];
            g[yi] = dv[gsum];
            b[yi] = dv[bsum];

            // adds a new pixel but at the same time handles the border
            // conditions (when the window tries to read or remove pixels
            // outside the bitmap).
            if (y == 0) {
                vMIN[x] = FFMIN(x + radius + 1, wm);
                vMAX[x] = FFMAX(x - radius, 0);
            }
            p1 = (yw + vMIN[x]) * nb_comps;
            p2 = (yw + vMAX[x]) * nb_comps;
            rsum += pix[p1]     - pix[p2];
            gsum += pix[p1 + 1] - pix[p2 + 1];
            bsum += pix[p1 + 2] - pix[p2 + 2];
            yi++;
        }
        yw += w;
    }

    for (x = 0; x < w; x++) {
        rsum = gsum = bsum = 0;
        yp = -radius * w;
        for (i = -radius; i <= radius; i++) {
            yi = FFMAX(0, yp) + x;
            rsum += r[yi];
            gsum += g[yi];
            bsum += b[yi];
            yp += w;
        }

        yi = x;
        for (y = 0; y < h; y++) {
            pix[yi * nb_comps]     = dv[rsum];
            pix[yi * nb_comps + 1] = dv[gsum];
            pix[yi * nb_comps + 2] = dv[bsum];

            if (x == 0) {
                vMIN[y] = FFMIN(y + radius + 1, hm) * w;
                vMAX[y] = FFMAX(y - radius, 0) * w;
            }
            p1 = x + vMIN[y];
            p2 = x + vMAX[y];

            // rsum, gsum and bsum is the accumulated sum of pixels inside
            // the sliding window. What you see is the new pixel on the
            // right side being added to the sum and the leftmost pixel
            // i nthe window being removed from the sum.
            rsum += r[p1] - r[p2];
            gsum += g[p1] - g[p2];
            bsum += b[p1] - b[p2];
            yi += w;
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    SuperFastBlurContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    superfast_blur(s, in->data[0], inlink->w, inlink->h, desc->nb_components);

    return ff_filter_frame(outlink, in);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SuperFastBlurContext *s = ctx->priv;

    av_freep(&s->r);
    av_freep(&s->g);
    av_freep(&s->b);
    av_freep(&s->vMIN);
    av_freep(&s->vMAX);
    av_freep(&s->dv);
}

static const AVFilterPad superfastblur_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad superfastblur_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_superfastblur = {
    .name          = "superfastblur",
    .description   = NULL_IF_CONFIG_SMALL("Blur the input with super fast blur algorithm."),
    .priv_size     = sizeof(SuperFastBlurContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = superfastblur_inputs,
    .outputs       = superfastblur_outputs,
    .priv_class    = &superfastblur_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
