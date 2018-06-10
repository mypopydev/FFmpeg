/*
 * Copyright (c) 2018 Jun Zhao
 *
 * VA-API Acceleration API (video decoding/scaling) sample
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
 * VA-API-Accelerated decoding/scaling example.
 *
 * @example vaapi_dec_scaling.c
 * This example shows how to do VA-API-accelerated decoding/scaling with output
 * frames from the HW video surfaces.
 */

#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>

static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;
static FILE *output_file = NULL;
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *filter_graph;
const char *filter_descr =
    "format=vaapi,scale_vaapi=w=iw/2:h=ih/2,hwdownload,format=nv12";
int init_filter = 0;

static int init_hw_decode(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
    int err = 0;

    if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
                                      NULL, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    return err;
}

static int init_filters(AVFormatContext *fmt_ctx, AVCodecContext *dec_ctx,
                        int video_stream_index, const char *filters_descr)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph || !par) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            dec_ctx->width, dec_ctx->height, AV_PIX_FMT_VAAPI,
            time_base.num, time_base.den,
            dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Cannot create buffer source\n");
        goto end;
    }
    par->hw_frames_ctx = dec_ctx->hw_frames_ctx;
    ret = av_buffersrc_parameters_set(buffersrc_ctx, par);
    if (ret < 0)
        goto end;
    av_freep(&par);

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, NULL, filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Cannot create buffer sink\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;

    for (int i = 0; i < filter_graph->nb_filters; i++) {
        filter_graph->filters[i]->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        if (!filter_graph->filters[i]->hw_device_ctx) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt)
            return *p;
    }

    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

static int decode_filte(AVFormatContext *input_ctx, AVCodecContext *avctx,
                        int video_stream, AVPacket *packet)
{
    AVFrame *frame = NULL;
    AVFrame *filt_frame = NULL;
    uint8_t *buffer = NULL;
    int size;
    int ret = 0;

    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        return ret;
    }

    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    if (!frame || !filt_frame) {
        fprintf(stderr, "Can not alloc frame\n");
        ret = AVERROR(ENOMEM);
        return ret;
    }

    while (1) {
        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            av_frame_free(&filt_frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        if (frame->format == hw_pix_fmt) {
            if (!init_filter) {
                if ((ret = init_filters(input_ctx, avctx,
                                        video_stream, filter_descr)) < 0) {
                    fprintf(stderr, "Failed to init filter\n");
                    goto fail;
                }
                init_filter = 1;
            }

            /* push the decoded frame into the filtergraph */
            if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
                goto fail;
            }

            /* pull filtered frames from the filtergraph */
            while (1) {
                ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                    goto fail;

                size = av_image_get_buffer_size(filt_frame->format, filt_frame->width,
                                                filt_frame->height, 1);
                buffer = av_malloc(size);
                if (!buffer) {
                    fprintf(stderr, "Can not alloc buffer\n");
                    ret = AVERROR(ENOMEM);
                    goto fail;
                }
                ret = av_image_copy_to_buffer(buffer, size,
                                              (const uint8_t * const *)filt_frame->data,
                                              (const int *)filt_frame->linesize, filt_frame->format,
                                              filt_frame->width, filt_frame->height, 1);
                if (ret < 0) {
                    fprintf(stderr, "Can not copy image to buffer\n");
                    goto fail;
                }

                if ((ret = fwrite(buffer, 1, size, output_file)) < 0) {
                    fprintf(stderr, "Failed to dump filted raw data.\n");
                    goto fail;
                }

                av_frame_unref(filt_frame);
                av_freep(&buffer);
            }
            av_frame_unref(frame);
        }
    }

fail:
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_freep(&buffer);

    return ret;
}

int main(int argc, char *argv[])
{
    AVFormatContext *input_ctx = NULL;
    int video_stream, ret;
    AVStream *video = NULL;
    AVCodecContext *decoder_ctx = NULL;
    AVCodec *decoder = NULL;
    AVPacket packet;
    enum AVHWDeviceType type;
    int i;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        return -1;
    }

    type = av_hwdevice_find_type_by_name("vaapi");
    if (type == AV_HWDEVICE_TYPE_NONE) {
        fprintf(stderr, "Device type vaapi is not supported.\n");
        fprintf(stderr, "Available device types:");
        while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
            fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
        fprintf(stderr, "\n");
        return -1;
    }

    /* open the input file */
    if (avformat_open_input(&input_ctx, argv[1], NULL, NULL) != 0) {
        fprintf(stderr, "Cannot open input file '%s'\n", argv[2]);
        return -1;
    }

    if (avformat_find_stream_info(input_ctx, NULL) < 0) {
        fprintf(stderr, "Cannot find input stream information.\n");
        return -1;
    }

    /* find the video stream information */
    ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        fprintf(stderr, "Cannot find a video stream in the input file\n");
        return -1;
    }
    video_stream = ret;

    for (i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            fprintf(stderr, "Decoder %s does not support device type %s.\n",
                    decoder->name, av_hwdevice_get_type_name(type));
            return -1;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == type) {
            hw_pix_fmt = config->pix_fmt;
            break;
        }
    }

    if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
        return AVERROR(ENOMEM);

    video = input_ctx->streams[video_stream];
    if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
        return -1;

    decoder_ctx->get_format  = get_hw_format;

    if (init_hw_decode(decoder_ctx, type) < 0)
        return -1;

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
        fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
        return -1;
    }

    /* open the file to dump scaled data */
    output_file = fopen(argv[2], "w+");

    /* actual decoding/scaling and dump the raw data */
    while (ret >= 0) {
        if ((ret = av_read_frame(input_ctx, &packet)) < 0)
            break;

        if (video_stream == packet.stream_index)
            ret = decode_filte(input_ctx, decoder_ctx, video_stream, &packet);

        av_packet_unref(&packet);
    }

    /* flush the decoder/avfilter */
    packet.data = NULL;
    packet.size = 0;
    ret = decode_filte(input_ctx, decoder_ctx, video_stream, &packet);
    av_packet_unref(&packet);

    if (output_file)
        fclose(output_file);
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);

    return 0;
}
