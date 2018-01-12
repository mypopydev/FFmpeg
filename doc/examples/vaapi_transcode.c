/*
 * Video Acceleration API (video transcoding) transcode sample
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
 * Intel VAAPI-accelerated transcoding example.
 *
 * @example vaapi_transcode.c
 * This example shows how to do VAAPI-accelerated transcoding.
 * Usage: vaapi_transcode input_stream h264_vaapi output_stream
 */

#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/avassert.h>
#include <errno.h>

static AVFormatContext *ifmt_ctx = NULL;
static AVBufferRef *hw_device_ctx = NULL;
static AVCodecContext *decoder_ctx = NULL, *encoder_ctx = NULL;
static int video_stream = -1;
static FILE *fout;
static int initialized = 0;

static enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx,
                                           const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    }

    fprintf(stderr, "Failed to get vaapi pix format\n");
    return AV_PIX_FMT_NONE;
}

static int open_input_file(const char *filename)
{
    int ret;
    AVCodec *decoder = NULL;
    AVStream *video = NULL;

    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Cannot open input file '%s', Error code: %s\n",
                filename, av_err2str(ret));
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Cannot find input stream information. Error code: %s\n",
                av_err2str(ret));
        return ret;
    }

    ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        fprintf(stderr, "Cannot find a video stream in the input file.\
                Error code: %s\n", av_err2str(ret));
        return ret;
    }
    video_stream = ret;

    if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
        return AVERROR(ENOMEM);

    video = ifmt_ctx->streams[video_stream];
    if ((ret = avcodec_parameters_to_context(decoder_ctx, video->codecpar)) < 0) {
        fprintf(stderr, "avcodec_parameters_to_context error. Error code: %s\n",
                av_err2str(ret));
        return ret;
    }

    decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    decoder_ctx->get_format    = get_vaapi_format;

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0)
        fprintf(stderr, "Failed to open codec for decoding. Error code: %s\n",
                av_err2str(ret));

    return ret;
}

static int encode_write(AVFrame *frame)
{
    int ret = 0;
    AVPacket enc_pkt;

    av_init_packet(&enc_pkt);
    enc_pkt.data = NULL;
    enc_pkt.size = 0;

    if ((ret = avcodec_send_frame(encoder_ctx, frame)) < 0) {
        fprintf(stderr, "Error during encoding. Error code: %s\n", av_err2str(ret));
        goto end;
    }
    while (1) {
        ret = avcodec_receive_packet(encoder_ctx, &enc_pkt);
        if (ret)
            break;

        enc_pkt.stream_index = 0;
        ret = fwrite(enc_pkt.data, 1, enc_pkt.size, fout);
    }

end:
    if (ret == AVERROR_EOF)
        return 0;
    ret = ((ret == AVERROR(EAGAIN)) ? 0:-1);
    return ret;
}

static int dec_enc(AVPacket *pkt, AVCodec *enc_codec)
{
    AVFrame *frame;
    int ret = 0;

    ret = avcodec_send_packet(decoder_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding. Error code: %s\n", av_err2str(ret));
        return ret;
    }

    while (ret >= 0) {
        if (!(frame = av_frame_alloc()))
            return AVERROR(ENOMEM);

        ret = avcodec_receive_frame(decoder_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding. Error code: %s\n", av_err2str(ret));
            goto fail;
        }

        if (!initialized) {
            /* Here we need to ref hw_frames_ctx of decoder to initialize encoder's codec.
               Only after we get a decoded frame, can we obtain its hw_frames_ctx */
            encoder_ctx->hw_frames_ctx = av_buffer_ref(decoder_ctx->hw_frames_ctx);
            if (!encoder_ctx->hw_frames_ctx) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            if ((ret = avcodec_open2(encoder_ctx, enc_codec, NULL)) < 0) {
                fprintf(stderr, "Error while open encode codec. Error code: %s\n",
                        av_err2str(ret));
                goto fail;
            }
            initialized = 1;
        }

        if ((ret = encode_write(frame)) < 0)
            fprintf(stderr, "Error during encoding and writing.\n");

fail:
        av_frame_free(&frame);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int main(int argc, char **argv) {
    int ret = 0;
    AVPacket dec_pkt;
    AVCodec *enc_codec;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <input file> <encode codec> <output file>\n", argv[0]);
        return -1;
    }

    av_register_all();

    ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to create a VAAPI device. Error code: %s\n", av_err2str(ret));
        return -1;
    }

    if ((ret = open_input_file(argv[1])) < 0)
        goto end;

    if (!(enc_codec = avcodec_find_encoder_by_name(argv[2]))) {
        fprintf(stderr, "Could not find encoder '%s'\n", argv[2]);
        ret = -1;
        goto end;
    }

    if (!(fout = fopen(argv[3], "w+b"))) {
        fprintf(stderr, "Failed to open output file: %s\n", strerror(errno));
        ret = -1;
        goto end;
    }

    if (!(encoder_ctx= avcodec_alloc_context3(enc_codec))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* set AVCodecContext Parameters */
    encoder_ctx->width     = decoder_ctx->width;
    encoder_ctx->height    = decoder_ctx->height;
    encoder_ctx->time_base = (AVRational){1, 25};
    encoder_ctx->pix_fmt   = AV_PIX_FMT_VAAPI;

    /* read all packets and only transcoding video */
    while (ret >= 0) {
        if ((ret = av_read_frame(ifmt_ctx, &dec_pkt)) < 0)
            break;

        if (video_stream == dec_pkt.stream_index)
            ret = dec_enc(&dec_pkt, enc_codec);

        av_packet_unref(&dec_pkt);
    }

    /* flush decoder */
    dec_pkt.data = NULL;
    dec_pkt.size = 0;
    ret = dec_enc(&dec_pkt, enc_codec);
    av_packet_unref(&dec_pkt);

    /* flush encoder */
    ret = encode_write(NULL);

end:
    avformat_close_input(&ifmt_ctx);
    if (fout)
        fclose(fout);
    avcodec_free_context(&decoder_ctx);
    avcodec_free_context(&encoder_ctx);
    av_buffer_unref(&hw_device_ctx);
    return ret;
}
