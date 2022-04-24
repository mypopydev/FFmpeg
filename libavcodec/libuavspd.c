/*
 * RAW AVS3-P2/IEEE1857.10 video demuxer
 * Copyright (c) 2020 Zhenyu Wang <wangzhenyu@pkusz.edu.cn>
 *                    Bingjie Han <hanbj@pkusz.edu.cn>
 *                    Huiwen Ren  <hwrenx@gmail.com>
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

#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/cpu.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "cavs.h"
#include "codec_internal.h"
#include "internal.h"
#include "xavsdec.h"

#define libuavspd_decode        xavs_decoder

#define AVSP_NAL_START_CODE          0x010000

#define AVSP_SEQ_START_CODE          0xB0
#define AVSP_SEQ_END_CODE            0xB1
#define AVSP_USER_DATA_START_CODE    0xB2
#define AVSP_INTRA_PIC_START_CODE    0xB3
#define AVSP_UNDEF_START_CODE        0xB4
#define AVSP_EXTENSION_START_CODE    0xB5
#define AVSP_INTER_PIC_START_CODE    0xB6
#define AVSP_VIDEO_EDIT_CODE         0xB7

#define AVSP_FIRST_SLICE_START_CODE  0x00

#define AVS3_PROFILE_BASELINE_MAIN   0x20
#define AVS3_PROFILE_BASELINE_MAIN10 0x22

typedef struct avspd_context {
    AVCodecContext  *avctx;

    void            *dec_handle;
    int              frame_threads;

    int              got_seqhdr;
    int              found_seqhdr;

    //avspd_io_frm_t  dec_frame; // XXX

    xavs_stats_t    dec_stats;
    xavs_frame_t    dec_frame;
} avspd_context;

/**
 * find the next start code (I/PB picture, or sequence header/end)
 *
 * @param [in ] : bs_data - pointer to the bitstream buffer
 * @param [in]  : bs_len  - length of bs_data
 * @param [out] : left    - the rest length of bs_data
 *
 * Return       : 1 if a start code is found, otherwise 0
 */
#define AVSPD_CHECK_START_CODE(data_ptr, PIC_START_CODE) \
        (AV_RL32(data_ptr) != (PIC_START_CODE << 24) + AVSP_NAL_START_CODE)
static int avspd_find_next_start_code(const unsigned char *bs_data, int bs_len, int *left)
{
    const unsigned char *data_ptr = bs_data + 4;
    int count = bs_len - 4;

    while (count >= 4 &&
           AVSPD_CHECK_START_CODE(data_ptr, AVSP_INTER_PIC_START_CODE) &&
           AVSPD_CHECK_START_CODE(data_ptr, AVSP_INTRA_PIC_START_CODE) &&
           AVSPD_CHECK_START_CODE(data_ptr, AVSP_SEQ_START_CODE) &&
           AVSPD_CHECK_START_CODE(data_ptr, AVSP_SEQ_END_CODE)) {
        data_ptr++;
        count--;
    }

    if (count >= 4) {
        *left = count;
        return 1;
    }

    return 0;
}

/**
 * find the sequence header (call first before decoding)
 *
 * @param [in ] : bs_data - pointer to the bitstream buffer
 * @param [in]  : bs_len  - length of bs_data
 * @param [out] : left    - the rest length of bs_data
 *
 * Return       : 1 if a start code is found, otherwise 0
 */
static int avspd_find_seq_start_code(const unsigned char *bs_data, int bs_len, int *left)
{
    const unsigned char *data_ptr = bs_data + 4;
    int count = bs_len - 4;

    while (count >= 4 &&
           AVSPD_CHECK_START_CODE(data_ptr, AVSP_SEQ_START_CODE)) {
        data_ptr++;
        count--;
    }

    if (count >= 4) {
        *left = count;
        return 1;
    }

    return 0;
}

#if 0
static void avspd_output_callback(avspd_io_frm_t *dec_frame) {
    avspd_io_frm_t frm_out;
    AVFrame *frm = (AVFrame *)dec_frame->priv;
    int i;

    if (!frm || !frm->data[0]) {
        dec_frame->got_pic = 0;
        av_log(NULL, AV_LOG_ERROR, "Invalid AVFrame in avspd output.\n");
        return;
    }

    frm->pts       = dec_frame->pts;
    frm->pkt_dts   = dec_frame->dts;
    frm->pkt_pos   = dec_frame->pkt_pos;
    frm->pkt_size  = dec_frame->pkt_size;
    frm->coded_picture_number   = dec_frame->dtr;
    frm->display_picture_number = dec_frame->ptr;

    if (dec_frame->type < 0 || dec_frame->type >= 4) {
        av_log(NULL, AV_LOG_WARNING, "Error frame type in avspd: %d.\n", dec_frame->type);
    }

    frm->pict_type = ff_avs3_image_type[dec_frame->type];
    frm->key_frame = (frm->pict_type == AV_PICTURE_TYPE_I);

    for (i = 0; i < 3; i++) {
        frm_out.width [i] = dec_frame->width[i];
        frm_out.height[i] = dec_frame->height[i];
        frm_out.stride[i] = frm->linesize[i];
        frm_out.buffer[i] = frm->data[i];
    }

    avspd_img_cpy_cvt(&frm_out, dec_frame, dec_frame->bit_depth);
}
#endif

static av_cold int libavspd_init(AVCodecContext *avctx)
{
    avspd_context *h = avctx->priv_data;
    int ret;

    xavs_create_t xavs_create;

    memset(&xavs_create, 0, sizeof(xavs_create_t));
    xavs_create.threads_parse =
        avctx->thread_count > 0 ? avctx->thread_count : av_cpu_count();

    // create the AVSP decoder
    ret = libuavspd_decode(NULL, XAVS_CREATE, &xavs_create, NULL);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "decoder created error.");
        return AVERROR_EXTERNAL;
    }

    h->got_seqhdr = 0;
    h->found_seqhdr = 0;
    h->dec_handle = xavs_create.handle;
    if (!h->dec_handle) {
        av_log(avctx, AV_LOG_ERROR, "decoder created get null handle.");
        return AVERROR_EXTERNAL;
    }

    memset(&h->dec_stats, 0, sizeof(xavs_stats_t));
    memset(&h->dec_frame, 0, sizeof(xavs_frame_t));
    h->dec_frame.output.format = XAVS_MT_I420;       // default format

    av_log(avctx, AV_LOG_INFO, "decoder created. %p, version %d\n",
           h->dec_handle, xavs_create.version);
    return 0;
}

static av_cold int libavspd_end(AVCodecContext *avctx)
{
    avspd_context *h = avctx->priv_data;

    if (h->dec_handle) {
        libuavspd_decode(h->dec_handle, XAVS_DESTROY, NULL, NULL);
        h->dec_handle = NULL;
    }
    h->got_seqhdr = 0;

    return 0;
}

static void libavspd_flush(AVCodecContext * avctx)
{
    avspd_context *h = avctx->priv_data;

    if (h->dec_handle) {
        libuavspd_decode(h->dec_handle, XAVS_RESET, NULL, NULL);
    }
}

#define AVSPD_CHECK_INVALID_RANGE(v, l, r) ((v)<(l)||(v)>(r))
static int libavspd_decode_frame(AVCodecContext *avctx, AVFrame *frm,
                                  int *got_frame, AVPacket *avpkt)
{
    avspd_context *h = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    const uint8_t *buf_end;
    const uint8_t *buf_ptr;
    int left_bytes;
    int ret, finish = 0;

    xavs_stats_t dec_stats;
    xavs_frame_t dec_frame;

    int bs_len;

    //avspd_io_frm_t *frm_dec;

    if (!buf_size) {
        // flush the decoder
        av_log(avctx, AV_LOG_ERROR, "get null size packet.\n");
    }

    if (buf_size) {
        av_log(avctx, AV_LOG_INFO, "0x%02x%02x%02x%02x, len %d\n",buf[0],buf[1],buf[2],buf[3], buf_size );
    }

    *got_frame = 0;
    frm->pts = -1;
    frm->pict_type = AV_PICTURE_TYPE_NONE;

    buf_ptr = buf;
    buf_end = buf + buf_size;

    if (!h->found_seqhdr) {
        // find the first sequence header
        for (;;) {
            if (avspd_find_seq_start_code(buf_ptr, buf_end - buf_ptr, &left_bytes)) {
                bs_len = buf_end - buf_ptr - left_bytes;
                h->found_seqhdr = 1;              // set flag
                av_log(avctx, AV_LOG_INFO, "get seq header.\n");
                break;
            }
            break;
        }
    }

#if 0
    frm_dec = &h->dec_frame;

    buf_ptr = buf;
    buf_end = buf + buf_size;

    frm_dec->pkt_pos  = avpkt->pos;
    frm_dec->pkt_size = avpkt->size;

    while (!finish) {
        int bs_len;

        if (h->got_seqhdr) {
            if (!frm->data[0] && (ret = ff_get_buffer(avctx, frm, 0)) < 0) {
                return ret;
            }
            h->dec_frame.priv = frm;   // AVFrame
        }

        if (avspd_find_next_start_code(buf_ptr, buf_end - buf_ptr, &left_bytes)) {
            bs_len = buf_end - buf_ptr - left_bytes;
        } else {
            bs_len = buf_end - buf_ptr;
            finish = 1;
        }
        frm_dec->bs = (unsigned char *)buf_ptr;
        frm_dec->bs_len = bs_len;
        frm_dec->pts = avpkt->pts;
        frm_dec->dts = avpkt->dts;
        avspd_decode(h->dec_handle, frm_dec);
        buf_ptr += bs_len;

        if (frm_dec->nal_type == NAL_SEQ_HEADER) {
            struct avspd_com_seqh_t *seqh = frm_dec->seqhdr;
            if (AVSPD_CHECK_INVALID_RANGE(seqh->frame_rate_code, 0, 15)) {
                av_log(avctx, AV_LOG_ERROR, "Invalid frame rate code: %d.\n", seqh->frame_rate_code);
                seqh->frame_rate_code = 3; // default 25 fps
            } else {
                avctx->framerate.num = ff_avs3_frame_rate_tab[seqh->frame_rate_code].num;
                avctx->framerate.den = ff_avs3_frame_rate_tab[seqh->frame_rate_code].den;
            }
            avctx->has_b_frames  = !seqh->low_delay;
            avctx->pix_fmt = seqh->bit_depth_internal == 8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10LE;
            ret = ff_set_dimensions(avctx, seqh->horizontal_size, seqh->vertical_size);
            if (ret < 0)
                return ret;
            h->got_seqhdr = 1;
        }

        if (frm_dec->got_pic) {
            break;
        }
    }

    *got_frame = h->dec_frame.got_pic;

    if (!(*got_frame)) {
        av_frame_unref(frm);
    }

    return buf_ptr - buf;
#endif
    return 0;
}

const FFCodec ff_libuavspd_decoder = {
    .p.name         = "libuavspd",
    .p.long_name    = NULL_IF_CONFIG_SMALL("libuavspd AVS-P2/16/IEEE1857.10"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_CAVS,
    .priv_data_size = sizeof(avspd_context),
    .init           = libavspd_init,
    .close          = libavspd_end,
    FF_CODEC_DECODE_CB(libavspd_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS,
    .caps_internal  = FF_CODEC_CAP_AUTO_THREADS,
    .flush          = libavspd_flush,
    .p.pix_fmts     = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P,
                                                     AV_PIX_FMT_NONE },
    .p.wrapper_name = "libuavspd",
};
