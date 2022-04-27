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
    headerset_t     seqhdr;

    unsigned char   *out_buf;

    int             index;
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
    const unsigned char *data_ptr = bs_data;
    int count = bs_len;

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

static av_cold int libavspd_init(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_WARNING, "%s\n", __FUNCTION__);
    avspd_context *h = avctx->priv_data;
    int ret;

    xavs_create_t xavs_create;

    memset(&xavs_create, 0, sizeof(xavs_create_t));
    xavs_create.threads_parse = 1;
    // avctx->thread_count > 0 ? avctx->thread_count : av_cpu_count();

    // create the AVSP decoder
    ret = libuavspd_decode(NULL, XAVS_CREATE, &xavs_create, NULL);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "decoder created error.");
        return AVERROR_EXTERNAL;
    }

    h->got_seqhdr = 0;
    h->found_seqhdr = 0;
    h->dec_handle = xavs_create.handle;

    memset(&h->dec_stats, 0, sizeof(xavs_stats_t));
    memset(&h->dec_stats.seq_set, 0, sizeof(headerset_t));
    memset(&h->dec_frame, 0, sizeof(xavs_frame_t));
    h->dec_frame.output.format = XAVS_MT_I420;       // default format

    h->out_buf  = NULL;   // pointer to output buffer

    h->index = 0;

    av_log(avctx, AV_LOG_INFO, "decoder created. %p, version %d\n",
           h->dec_handle, xavs_create.version);
    return 0;
}

static av_cold int libavspd_end(AVCodecContext *avctx)
{
    av_log(avctx, AV_LOG_WARNING, "%s\n", __FUNCTION__);
    avspd_context *h = avctx->priv_data;

    if (h->dec_handle) {
        libuavspd_decode(h->dec_handle, XAVS_RESET, NULL, NULL);
        libuavspd_decode(h->dec_handle, XAVS_DESTROY, NULL, NULL);
        h->dec_handle = NULL;
    }
    h->got_seqhdr = 0;

    return 0;
}

static void libavspd_flush(AVCodecContext * avctx)
{
    av_log(avctx, AV_LOG_WARNING, "%s\n", __FUNCTION__);
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
    int ret = 0, finish = 0;

    //xavs_stats_t dec_stats;
    //xavs_frame_t dec_frame;

    int dim_x  = 0;                   // get from parsing the .asm/.avs file
    int dim_y  = 0;                   // get from parsing the .asm/.avs file

    int bs_len;

    unsigned char IMGTYPE[4] = {'I', 'P', 'B', '\x0'};  // character of image type
    av_log(avctx, AV_LOG_WARNING, "%s\n", __FUNCTION__);

    if (!buf_size) {
        // flush the decoder
        av_log(avctx, AV_LOG_ERROR, "get null size packet.\n");
        return 0;
    }

    if (buf_size) {
        av_log(avctx, AV_LOG_INFO, "0x%02x%02x%02x%02x, len %d\n",buf[0],buf[1],buf[2],buf[3], buf_size );
    }

    *got_frame = 0;
    frm->pts = -1;
    frm->pict_type = AV_PICTURE_TYPE_NONE;

    h->dec_frame.output.format = XAVS_MT_I420;       // default format
    h->dec_frame.output.pts = avpkt->pts;       // pts
    //h->dec_frame.output.pts = 100;       // default format

    buf_ptr = buf;
    buf_end = buf + buf_size;

    if (!h->found_seqhdr) {
        // find the first sequence header
        if (avspd_find_seq_start_code(buf_ptr, buf_end - buf_ptr, &left_bytes)) {
            bs_len = buf_end - buf_ptr - left_bytes;
            h->found_seqhdr = 1;              // set flag
            av_log(avctx, AV_LOG_INFO, "get seq header.\n");
        } else {
            av_log(avctx, AV_LOG_WARNING, "can't find seq header.\n");
            *got_frame = 0;
            return 0;
        }
    }

    while (!finish) {
#if 0
        if (h->got_seqhdr) {
            if (!frm->data[0] && (ret = ff_get_buffer(avctx, frm, 0)) < 0) {
                return ret;
            }
        }
#endif

        // split the buffer with full NALU
        if (avspd_find_next_start_code(buf_ptr, buf_end - buf_ptr, &left_bytes)) {
            bs_len = buf_end - buf_ptr - left_bytes;
        } else {
            bs_len = buf_end - buf_ptr;
            finish = 1;
        }

        h->dec_frame.bs_buf = (unsigned char *)buf_ptr;
        h->dec_frame.bs_len = bs_len;
        ret = libuavspd_decode(h->dec_handle, XAVS_DECODE, &h->dec_frame, &h->dec_stats);
        av_log(avctx, AV_LOG_INFO, "Decoder 0x%02x%02x%02x%02x, len %d, ret %d, type %d, frame %c\n",buf_ptr[0],buf_ptr[1],buf_ptr[2],buf_ptr[3], bs_len, ret, h->dec_stats.type, IMGTYPE[h->dec_frame.output.type]);
        buf_ptr += bs_len;

        printf("type: %d  xxx\n", h->dec_stats.type);
        switch (h->dec_stats.type) {
        case XAVS_TYPE_DECODED:   // decode one frame
        {
            printf(" (%c) \n", IMGTYPE[h->dec_frame.output.type]);       // image type
            //printf("%I64u, ", h->dec_frame.output.pts);               // test for pts, output

            if (h->got_seqhdr) {
                if (!frm->data[0] && (ret = ff_get_buffer(avctx, frm, 0)) < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Decoder error: unknown frame type\n");
                    return ret;
                }
            }

            if (h->dec_frame.output.type == FRM_I) {
                frm->pict_type = AV_PICTURE_TYPE_I;
                frm->key_frame = 1;
            } else if (h->dec_frame.output.type == FRM_P) {
                frm->pict_type = AV_PICTURE_TYPE_P;
                frm->key_frame = 0;
            } else if  (h->dec_frame.output.type == FRM_B) {
                frm->pict_type = AV_PICTURE_TYPE_B;
                frm->key_frame = 0;
            } else {
                av_log(avctx, AV_LOG_ERROR, "Decoder error: unknown frame type\n");
                return AVERROR_EXTERNAL;
            }

            frm->interlaced_frame = h->dec_stats.seq_set.progressive;
            frm->format           = AV_PIX_FMT_YUV420P;
            frm->width            = avctx->width;
            frm->height           = avctx->height;
            frm->pts              = h->dec_frame.output.pts;

            const uint8_t *y_plane = h->dec_frame.output.raw_data;
            const uint8_t *u_plane = h->dec_frame.output.raw_data + (avctx->width * avctx->height);
            const uint8_t *v_plane = h->dec_frame.output.raw_data + 5*(avctx->width * avctx->height)/4;

            int y_stride  = h->dec_frame.output.raw_stride;
            int uv_stride = h->dec_frame.output.raw_stride/2;

            const uint8_t *planes[4] = { y_plane,  u_plane,   v_plane   };
            const int      stride[4] = { y_stride, uv_stride, uv_stride };

            av_image_copy(frm->data, frm->linesize, planes,
                          stride, avctx->pix_fmt, avctx->width, avctx->height);
            printf("%d  (%c) ddddd\n", h->index++, IMGTYPE[h->dec_frame.output.type]);

            *got_frame = 1;
        }
            break;

        case XAVS_TYPE_ERROR:     // error, current or next frame was not decoded
            printf("%8I64u, ", h->dec_frame.output.pts);               // test for pts, output
            printf("!!!ERROR!!!\n");
            break;

        case XAVS_TYPE_NOTHING:   // nothing was decoded
            break;

        case XAVS_TYPE_SEQ:       // sequence header was decoded
            if (memcmp(&h->seqhdr, &h->dec_stats.seq_set, sizeof(h->seqhdr))) {

                memcpy(&h->seqhdr, &h->dec_stats.seq_set, sizeof(h->seqhdr));

                avctx->has_b_frames  = !h->seqhdr.low_delay;
                avctx->pix_fmt = AV_PIX_FMT_YUV420P;
                avctx->profile = h->dec_stats.seq_set.profile_id;
                avctx->level  =  h->dec_stats.seq_set.level_id;
                ret = ff_set_dimensions(avctx, h->seqhdr.horizontal_size, h->seqhdr.vertical_size);
                if (ret < 0)
                    return ret;
                h->got_seqhdr = 1;
                avctx->framerate = av_d2q(h->seqhdr.frame_rate, 4096);
                //*got_frame = 0;
                //return 0;

                // resize image buffer
                dim_x = h->dec_stats.seq_set.horizontal_size;
                dim_y = h->dec_stats.seq_set.vertical_size;

                printf("Resized frame buffer to %dx%d frame rate %f\n", dim_x, dim_y, h->dec_stats.seq_set.frame_rate);
                printf(" index  type  qp   psnr(Y)   psnr(U)   psnr(V)\n");
                printf("--------------------------------------------------\n");

                // free old output buffer and allocate the new buffer
                if (h->out_buf) {
                    free(h->out_buf);
                }
                h->out_buf = (unsigned char *)malloc(dim_x * dim_y * 4);
                if (h->out_buf == NULL) {
                    printf("Allocing memory error.\n");
                    exit(-1);
                } else {
                    h->dec_frame.output.raw_data   = h->out_buf;
                    h->dec_frame.output.raw_stride = dim_x;
                }

                //*got_frame = 0;
                //return 0;
            }
            break;

        case XAVS_TYPE_EOS:       // end of sequence, decoding work finished
            // runing = 0;        // set flag to terminate
            break;
        }
    }

#if 1
    if (!(*got_frame)) {
        av_frame_unref(frm);
    }
#endif

    return buf_ptr - buf;
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
