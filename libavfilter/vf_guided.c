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
 * Guided Image Filter
 *
 * @see http://kaiminghe.com/publications/pami12guidedfilter.pdf
 */

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct GuidedContext {
    const AVClass *class;
    int radius;
    double   eps;
} GuidedContext;

#define OFFSET(x) offsetof(GuidedContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption guided_options[] = {
    { "radius", "set high threshold", OFFSET(radius), AV_OPT_TYPE_INT,    { 3 }, 0, 255, FLAGS },
    { "r",      "set high threshold", OFFSET(radius), AV_OPT_TYPE_INT,    { 3 }, 0, 1, FLAGS },
    { "eps",    "set the filter eps", OFFSET(eps),    AV_OPT_TYPE_DOUBLE, {.dbl=1e-6}, 0, 1, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(guided);

static av_cold int init(AVFilterContext *ctx)
{
    GuidedContext *guided = ctx->priv;

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
     static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P,
        AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUVJ411P,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GBRP,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_props(AVFilterLink *inlink)
{
    int p;
    AVFilterContext *ctx = inlink->dst;
    GuidedContext *guided = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    return 0;
}

static void guided_filter_color(AVFrame *guide, AVFrame *src, AVFrame *dst, int radius, double eps)
{
	int height = src->height;
	int width = src->width;
	int widthstep = guide->linesize[0];
	int gwidthstep = src->linesize[0];
	int nch = 1;
	int gnch = 1;

	int i, j;
	int m, n;
	int w;
	int e = 0;
	int st_row, ed_row;
	int st_col, ed_col;

	double sum_Ir, sum_Ig, sum_Ib;
	double sum_Ir_square, sum_Ig_square, sum_Ib_square;
	double sum_IrIg, sum_IgIb, sum_IrIb;
	double sum_PiIr, sum_PiIg, sum_PiIb;
	double sum_Pi;

	double A, B, C, D, E, F, G, H, I, J, K, L;
	double X, Y, Z;
	double ak_r, ak_g, ak_b;
	double bk;
	double det;

	double tmp_Ir, tmp_Ig, tmp_Ib;
	double tmp_p, tmp_q;

	double *v_ak_r = av_malloc(sizeof(double) * height * width);
	double *v_ak_g = av_malloc(sizeof(double) * height * width);
	double *v_ak_b = av_malloc(sizeof(double) * height * width);
	double *v_bk =   av_malloc(sizeof(double) * height * width);

	int count = 0;

	uint8_t *data_guide = guide->data[0];
	uint8_t *data_src = src->data[0];
	uint8_t *data_dst = dst->data[0];

	for (i = 0; i < height; i++) {
		for (j = 0; j < width; j++) {
			st_row = i - radius, ed_row = i + radius;
			st_col = j - radius, ed_col = j + radius;

			st_row = st_row < 0 ? 0 : st_row;
			ed_row = ed_row >= height ? (height - 1) : ed_row;
			st_col = st_col < 0 ? 0 : st_col;
			ed_col = ed_col >= width ? (width - 1) : ed_col;

			sum_Ir = sum_Ig = sum_Ib = 0;
			sum_Ir_square = sum_Ig_square = sum_Ib_square = 0;
			sum_IrIg = sum_IgIb = sum_IrIb = 0;
			sum_PiIr = sum_PiIg = sum_PiIb = 0;
			sum_Pi = 0;
			w = 0;

			for (m = st_row; m <= ed_row; m++) {
				for (n = st_col; n <= ed_col; n++) {
					tmp_Ib = *(data_guide + m * widthstep + n * nch);
					tmp_Ig = *(data_guide + m * widthstep + n * nch + 1);
					tmp_Ir = *(data_guide + m * widthstep + n * nch + 2);

					tmp_p = *(data_src + m * gwidthstep + n * gnch);

					sum_Ib += tmp_Ib;
					sum_Ig += tmp_Ig;
					sum_Ir += tmp_Ir;

					sum_Ib_square += tmp_Ib * tmp_Ib;
					sum_Ig_square += tmp_Ig * tmp_Ig;
					sum_Ir_square += tmp_Ir * tmp_Ir;

					sum_IrIg += tmp_Ir * tmp_Ig;
					sum_IgIb += tmp_Ig * tmp_Ib;
					sum_IrIb += tmp_Ir * tmp_Ib;

					sum_Pi += tmp_p;
					sum_PiIb += tmp_p * tmp_Ib;
					sum_PiIg += tmp_p * tmp_Ig;
					sum_PiIr += tmp_p * tmp_Ir;

					w++;
				}
			}

			A = (sum_Ir_square + w * eps) * sum_Ig - sum_Ir * sum_IrIg;
			B = sum_IrIg * sum_Ig - sum_Ir * (sum_Ig_square + w * eps);
			C = sum_IrIb * sum_Ig - sum_Ir * sum_IgIb;
			D = sum_PiIr * sum_Ig - sum_PiIg * sum_Ir;
			E = (sum_Ir_square + w * eps) * sum_Ib - sum_IrIb * sum_Ir;
			F = sum_IrIg * sum_Ib - sum_IgIb * sum_Ir;
			G = sum_IrIb * sum_Ib - (sum_Ib_square + w * eps) * sum_Ir;
			H = sum_PiIr * sum_Ib - sum_PiIb * sum_Ir;
			I = (sum_Ir_square + w * eps) * w - sum_Ir * sum_Ir;
			J = sum_IrIg * w - sum_Ig * sum_Ir;
			K = sum_IrIb * w - sum_Ib * sum_Ir;
			L = sum_PiIr * w - sum_Pi * sum_Ir;

			det = A * F * K + B * G * I + C * E * J - C * F * I - A * G * J - B * E * K;
			X = D * F * K + B * G * L + C * H * J - C * F * L - D * G * J - B * H * K;
			Y = A * H * K + D * G * I + C * E * L - C * H * I - D * E * K - A * G * L;
			Z = A * F * L + B * H * I + D * J * E - D * F * I - B * E * L - A * H * J;

			ak_r = X / det;
			ak_g = Y / det;
			ak_b = Z / det;

			bk = (sum_PiIg - sum_IrIg * ak_r - (sum_Ig_square + w * eps) * ak_g - sum_IgIb * ak_b) / sum_Ig;

			tmp_Ib = *(data_guide + i * widthstep + j * nch);
			tmp_Ig = *(data_guide + i * widthstep + j * nch + 1);
			tmp_Ir = *(data_guide + i * widthstep + j * nch + 2);

			tmp_q = ak_b * tmp_Ib + ak_g * tmp_Ig + ak_r * tmp_Ir + bk;
			tmp_q = tmp_q > 255 ? 255 : (tmp_q < 0 ? 0 : tmp_q);

			*(data_dst + i * gwidthstep + j * gnch) = (int)round(tmp_q);

			v_ak_b[count] = ak_b;
			v_ak_g[count] = ak_g;
			v_ak_r[count] = ak_r;
			v_bk[count] = bk;
			count++;
		}
	}

	for (int i = 0; i < height; i++) {
		for (int j = 0; j < width; j++) {
			st_row = i - radius, ed_row = i + radius;
			st_col = j - radius, ed_col = j + radius;

			st_row = st_row < 0 ? 0 : st_row;
			ed_row = ed_row >= height ? (height - 1) : ed_row;
			st_col = st_col < 0 ? 0 : st_col;
			ed_col = ed_col >= width ? (width - 1) : ed_col;

			double ak_r, ak_g, ak_b, bk;
			ak_r = ak_g = ak_b = bk = 0;

			int number = 0;
			for (int m = st_row; m <= ed_row; m++) {
				for (int n = st_col; n <= ed_col; n++) {
					ak_r += v_ak_r[(m) * width + n];
					ak_g += v_ak_g[(m) * width + n];
					ak_b += v_ak_b[(m) * width + n];
					bk += v_bk[(m) * width + n];
					number++;
				}
			}

			ak_r /= number;
			ak_g /= number;
			ak_b /= number;
			bk /= number;

			tmp_Ib = *(data_guide + i * widthstep + j * nch);
			tmp_Ig = *(data_guide + i * widthstep + j * nch + 1);
			tmp_Ir = *(data_guide + i * widthstep + j * nch + 2);

			tmp_q = ak_b * tmp_Ib + ak_g * tmp_Ig + ak_r * tmp_Ir + bk;
			tmp_q = tmp_q > 255 ? 255 : (tmp_q < 0 ? 0 : tmp_q);

			*(data_dst + i * gwidthstep + j * gnch) = (int)round(tmp_q);
		}
	}
	av_freep(&v_ak_b);
	av_freep(&v_ak_g);
	av_freep(&v_ak_r);
	av_freep(&v_bk);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    GuidedContext *guided = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int p, direct = 0;
    AVFrame *out;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    int p;
    GuidedContext *guided = ctx->priv;
}

static const AVFilterPad guided_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad guided_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_guided = {
    .name          = "guided",
    .description   = NULL_IF_CONFIG_SMALL("Guided Image Filtering."),
    .priv_size     = sizeof(GuidedContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = guided_inputs,
    .outputs       = guided_outputs,
    .priv_class    = &guided_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
