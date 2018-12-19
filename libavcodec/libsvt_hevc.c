/*
* Scalable Video Technology for HEVC encoder library plugin
*
* Copyright (c) 2018 Intel Corporation
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
* License along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "EbErrorCodes.h"
#include "EbTime.h"
#include "EbApi.h"

#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"

#include "internal.h"
#include "avcodec.h"

typedef struct SvtContext {
    AVClass     *class;

    EB_H265_ENC_CONFIGURATION  enc_params;
    EB_COMPONENTTYPE           *svt_handle;

    EB_BUFFERHEADERTYPE        *in_buf;
    EB_BUFFERHEADERTYPE        *out_buf;
    int                         raw_size;

    int         eos_flag;

    // User options.
    int vui_info;
    int hierarchical_level;
    int la_depth;
    int enc_mode;
    int rc_mode;
    int scd;
    int tune;
    int qp;

    int forced_idr;

    int aud;

    int profile;
    int tier;
    int level;

    int base_layer_switch_mode;
} SvtContext;

static int error_mapping(EB_ERRORTYPE svt_ret)
{
    int err;

    switch (svt_ret) {
    case EB_ErrorInsufficientResources:
        err = AVERROR(ENOMEM);
        break;

    case EB_ErrorUndefined:
    case EB_ErrorInvalidComponent:
    case EB_ErrorBadParameter:
        err = AVERROR(EINVAL);
        break;

    case EB_ErrorDestroyThreadFailed:
    case EB_ErrorSemaphoreUnresponsive:
    case EB_ErrorDestroySemaphoreFailed:
    case EB_ErrorCreateMutexFailed:
    case EB_ErrorMutexUnresponsive:
    case EB_ErrorDestroyMutexFailed:
        err = AVERROR_EXTERNAL;
            break;

    case EB_NoErrorEmptyQueue:
        err = AVERROR(EAGAIN);

    case EB_ErrorNone:
        err = 0;
        break;

    default:
        err = AVERROR_UNKNOWN;
    }

    return err;
}

static void free_buffer(SvtContext *svt_enc)
{
    if (svt_enc->in_buf) {
        EB_H265_ENC_INPUT *in_data = (EB_H265_ENC_INPUT *)svt_enc->in_buf->pBuffer;
        av_freep(&in_data);
        av_freep(&svt_enc->in_buf);
    }
    av_freep(&svt_enc->out_buf);
}

static int alloc_buffer(EB_H265_ENC_CONFIGURATION *config, SvtContext *svt_enc)
{
    const int    pack_mode_10bit   =
        (config->encoderBitDepth > 8) && (config->compressedTenBitFormat == 0) ? 1 : 0;
    const size_t luma_size_8bit    =
        config->sourceWidth * config->sourceHeight * (1 << pack_mode_10bit);
    const size_t luma_size_10bit   =
        (config->encoderBitDepth > 8 && pack_mode_10bit == 0) ? luma_size_8bit : 0;

    EB_H265_ENC_INPUT *in_data;

    svt_enc->raw_size = (luma_size_8bit + luma_size_10bit) * 3 / 2;

    // allocate buffer for in and out
    svt_enc->in_buf           = av_mallocz(sizeof(*svt_enc->in_buf));
    svt_enc->out_buf          = av_mallocz(sizeof(*svt_enc->out_buf));
    if (!svt_enc->in_buf || !svt_enc->out_buf)
        goto failed;

    in_data  = av_mallocz(sizeof(*in_data));
    if (!in_data)
        goto failed;
    svt_enc->in_buf->pBuffer  = (unsigned char *)in_data;

    svt_enc->in_buf->nSize        = sizeof(*svt_enc->in_buf);
    svt_enc->in_buf->pAppPrivate  = NULL;
    svt_enc->out_buf->nSize       = sizeof(*svt_enc->out_buf);
    svt_enc->out_buf->nAllocLen   = svt_enc->raw_size;
    svt_enc->out_buf->pAppPrivate = NULL;

    return 0;

failed:
    free_buffer(svt_enc);
    return AVERROR(ENOMEM);
}

static int config_enc_params(EB_H265_ENC_CONFIGURATION *param,
                             AVCodecContext *avctx)
{
    SvtContext *svt_enc = avctx->priv_data;
    int             ret;
    int        ten_bits = 0;

    param->sourceWidth     = avctx->width;
    param->sourceHeight    = avctx->height;

    if (avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE) {
        av_log(avctx, AV_LOG_DEBUG , "Encoder 10 bits depth input\n");
        // Disable Compressed 10-bit format default
        //
        // SVT-HEVC support a compressed 10-bit format allowing the
        // software to achieve a higher speed and channel density levels.
        // The conversion between the 10-bit yuv420p10le and the compressed
        // 10-bit format is a lossless operation. But in FFmpeg, we usually
        // didn't use this format
        param->compressedTenBitFormat = 0;
        ten_bits = 1;
    }

    // Update param from options
    param->hierarchicalLevels     = svt_enc->hierarchical_level - 1;
    param->encMode                = svt_enc->enc_mode;
    param->profile                = svt_enc->profile;
    param->tier                   = svt_enc->tier;
    param->level                  = svt_enc->level;
    param->rateControlMode        = svt_enc->rc_mode;
    param->sceneChangeDetection   = svt_enc->scd;
    param->tune                   = svt_enc->tune;
    param->baseLayerSwitchMode    = svt_enc->base_layer_switch_mode;
    param->qp                     = svt_enc->qp;
    param->accessUnitDelimiter    = svt_enc->aud;

    param->targetBitRate          = avctx->bit_rate;
    param->intraPeriodLength      = avctx->gop_size - 1;

    if (avctx->framerate.num > 0 && avctx->framerate.den > 0) {
        param->frameRateNumerator     = avctx->framerate.num;
        param->frameRateDenominator   = avctx->framerate.den * avctx->ticks_per_frame;
    } else {
        param->frameRateNumerator     = avctx->time_base.den;
        param->frameRateDenominator   = avctx->time_base.num * avctx->ticks_per_frame;
    }

    if (param->rateControlMode) {
        param->maxQpAllowed       = avctx->qmax;
        param->minQpAllowed       = avctx->qmin;
    }

    param->intraRefreshType       =
        !!(avctx->flags & AV_CODEC_FLAG_CLOSED_GOP) + 1;

    // is it repeat headers for MP4 or Annex-b
    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)
        param->codeVpsSpsPps          = 0;
    else
        param->codeVpsSpsPps          = 1;

    if (svt_enc->vui_info)
        param->videoUsabilityInfo = svt_enc->vui_info;

    if (svt_enc->la_depth != -1)
        param->lookAheadDistance  = svt_enc->la_depth;

    if (ten_bits) {
        param->encoderBitDepth        = 10;
    }

    ret = alloc_buffer(param, svt_enc);

    return ret;
}

static void read_in_data(EB_H265_ENC_CONFIGURATION *config,
                         const AVFrame *frame,
                         EB_BUFFERHEADERTYPE *headerPtr)
{
    uint8_t is16bit = config->encoderBitDepth > 8;
    uint64_t luma_size =
        (uint64_t)config->sourceWidth * config->sourceHeight<< is16bit;
    EB_H265_ENC_INPUT *in_data = (EB_H265_ENC_INPUT *)headerPtr->pBuffer;

    // support yuv420p and yuv420p010
    in_data->luma = frame->data[0];
    in_data->cb   = frame->data[1];
    in_data->cr   = frame->data[2];

    // stride info
    in_data->yStride  = frame->linesize[0] >> is16bit;
    in_data->cbStride = frame->linesize[1] >> is16bit;
    in_data->crStride = frame->linesize[2] >> is16bit;

    headerPtr->nFilledLen   += luma_size * 3/2u;
}

static av_cold int eb_enc_init(AVCodecContext *avctx)
{
    SvtContext   *svt_enc = avctx->priv_data;
    EB_ERRORTYPE svt_ret;

    svt_enc->eos_flag = 0;

    svt_ret = EbInitHandle(&svt_enc->svt_handle, svt_enc, &svt_enc->enc_params);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Error init encoder handle\n");
        goto failed;
    }

    svt_ret = config_enc_params(&svt_enc->enc_params, avctx);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Error configure encoder parameters\n");
        goto failed_init_handle;
    }

    svt_ret = EbH265EncSetParameter(svt_enc->svt_handle, &svt_enc->enc_params);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Error setting encoder parameters\n");
        goto failed_init_handle;
    }

    svt_ret = EbInitEncoder(svt_enc->svt_handle);
    if (svt_ret != EB_ErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "Error init encoder\n");
        goto failed_init_handle;
    }

    if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) {
        EB_BUFFERHEADERTYPE headerPtr;
        headerPtr.nSize       = sizeof(headerPtr);
        headerPtr.nFilledLen  = 0; /* in/out */
        headerPtr.pBuffer     = av_malloc(10 * 1024 * 1024);
        headerPtr.nAllocLen   = (10 * 1024 * 1024);

        if (!headerPtr.pBuffer) {
            av_log(avctx, AV_LOG_ERROR,
                   "Cannot allocate buffer size %d.\n", headerPtr.nAllocLen);
            svt_ret = EB_ErrorInsufficientResources;
            goto failed_init_enc;
        }

        svt_ret = EbH265EncStreamHeader(svt_enc->svt_handle, &headerPtr);
        if (svt_ret != EB_ErrorNone) {
            av_log(avctx, AV_LOG_ERROR, "Error when build stream header.\n");
            av_freep(&headerPtr.pBuffer);
            goto failed_init_enc;
        }

        avctx->extradata_size = headerPtr.nFilledLen;
        avctx->extradata = av_mallocz(avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata) {
            av_log(avctx, AV_LOG_ERROR,
                   "Cannot allocate HEVC header of size %d.\n", avctx->extradata_size);
            av_freep(&headerPtr.pBuffer);
            svt_ret = EB_ErrorInsufficientResources;
            goto failed_init_enc;
        }
        memcpy(avctx->extradata, headerPtr.pBuffer, avctx->extradata_size);

        av_freep(&headerPtr.pBuffer);
    }

    return 0;

failed_init_enc:
    EbDeinitEncoder(svt_enc->svt_handle);
failed_init_handle:
    EbDeinitHandle(svt_enc->svt_handle);
failed:
    free_buffer(svt_enc);
    return error_mapping(svt_ret);
}

static int eb_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    SvtContext           *svt_enc = avctx->priv_data;
    EB_BUFFERHEADERTYPE  *headerPtr = svt_enc->in_buf;

    if (!frame) {
        EB_BUFFERHEADERTYPE headerPtrLast;
        headerPtrLast.nAllocLen   = 0;
        headerPtrLast.nFilledLen  = 0;
        headerPtrLast.nTickCount  = 0;
        headerPtrLast.pAppPrivate = NULL;
        headerPtrLast.pBuffer     = NULL;
        headerPtrLast.nFlags      = EB_BUFFERFLAG_EOS;

        EbH265EncSendPicture(svt_enc->svt_handle, &headerPtrLast);
        svt_enc->eos_flag = 1;
        av_log(avctx, AV_LOG_DEBUG, "Finish sending frames!!!\n");
        return 0;
    }

    read_in_data(&svt_enc->enc_params, frame, headerPtr);

    headerPtr->nFlags       = 0;
    headerPtr->pAppPrivate  = NULL;
    headerPtr->pts          = frame->pts;
    switch (frame->pict_type) {
    case AV_PICTURE_TYPE_I:
        headerPtr->sliceType = svt_enc->forced_idr > 0 ? IDR_SLICE : I_SLICE;
        break;
    case AV_PICTURE_TYPE_P:
        headerPtr->sliceType = P_SLICE;
        break;
    case AV_PICTURE_TYPE_B:
        headerPtr->sliceType = B_SLICE;
        break;
    default:
        headerPtr->sliceType = INVALID_SLICE;
        break;
    }
    EbH265EncSendPicture(svt_enc->svt_handle, headerPtr);

    return 0;
}

static int eb_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    SvtContext  *svt_enc = avctx->priv_data;
    EB_BUFFERHEADERTYPE   *headerPtr = svt_enc->out_buf;
    EB_ERRORTYPE          svt_ret;
    int ret;

    if ((ret = ff_alloc_packet2(avctx, pkt, svt_enc->raw_size, 0)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate output packet.\n");
        return ret;
    }
    headerPtr->pBuffer = pkt->data;
    svt_ret = EbH265GetPacket(svt_enc->svt_handle, headerPtr, svt_enc->eos_flag);
    if (svt_ret == EB_NoErrorEmptyQueue)
        return AVERROR(EAGAIN);

    pkt->size = headerPtr->nFilledLen;
    pkt->pts  = headerPtr->pts;
    pkt->dts  = headerPtr->dts;
    if (headerPtr->sliceType == IDR_SLICE)
        pkt->flags |= AV_PKT_FLAG_KEY;
    if (headerPtr->sliceType == NON_REF_SLICE)
        pkt->flags |= AV_PKT_FLAG_DISPOSABLE;

    ret = (headerPtr->nFlags & EB_BUFFERFLAG_EOS) ? AVERROR_EOF : 0;
    return ret;
}

static av_cold int eb_enc_close(AVCodecContext *avctx)
{
    SvtContext *svt_enc = avctx->priv_data;

    EbDeinitEncoder(svt_enc->svt_handle);
    EbDeinitHandle(svt_enc->svt_handle);

    free_buffer(svt_enc);

    return 0;
}

#define OFFSET(x) offsetof(SvtContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "vui", "Enable vui info", OFFSET(vui_info),
      AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, VE },

    { "aud", "Include AUD", OFFSET(aud),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },

    { "hielevel", "Hierarchical prediction levels setting", OFFSET(hierarchical_level),
      AV_OPT_TYPE_INT, { .i64 = 4 }, 1, 4, VE , "hielevel"},
        { "flat",   NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "hielevel" },
        { "2level", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 2 },  INT_MIN, INT_MAX, VE, "hielevel" },
        { "3level", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 3 },  INT_MIN, INT_MAX, VE, "hielevel" },
        { "4level", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 4 },  INT_MIN, INT_MAX, VE, "hielevel" },

    { "la_depth", "Look ahead distance [0, 256]", OFFSET(la_depth),
      AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 256, VE },

    { "preset", "Encoding preset [0, 12] (e,g, for subjective quality tuning mode and >=4k resolution), [0, 10] (for >= 1080p resolution), [0, 9] (for all resolution and modes)",
      OFFSET(enc_mode), AV_OPT_TYPE_INT, { .i64 = 9 }, 0, 12, VE },

    { "profile", "Profile setting, Main Still Picture Profile not supported", OFFSET(profile),
      AV_OPT_TYPE_INT, { .i64 = FF_PROFILE_HEVC_MAIN_10 }, FF_PROFILE_HEVC_MAIN, FF_PROFILE_HEVC_MAIN_10, VE, "profile"},

#define PROFILE(name, value)  name, NULL, 0, AV_OPT_TYPE_CONST, \
    { .i64 = value }, 0, 0, VE, "profile"
        { PROFILE("main",   FF_PROFILE_HEVC_MAIN)    },
        { PROFILE("main10", FF_PROFILE_HEVC_MAIN_10) },
#undef PROFILE

    { "tier", "Set tier (general_tier_flag)", OFFSET(tier),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "tier" },
        { "main", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, VE, "tier" },
        { "high", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, VE, "tier" },

    { "level", "Set level (level_idc)", OFFSET(level),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 0xff, VE, "level" },

#define LEVEL(name, value) name, NULL, 0, AV_OPT_TYPE_CONST, \
      { .i64 = value }, 0, 0, VE, "level"
        { LEVEL("1",   10) },
        { LEVEL("2",   20) },
        { LEVEL("2.1", 21) },
        { LEVEL("3",   30) },
        { LEVEL("3.1", 31) },
        { LEVEL("4",   40) },
        { LEVEL("4.1", 41) },
        { LEVEL("5",   50) },
        { LEVEL("5.1", 51) },
        { LEVEL("5.2", 52) },
        { LEVEL("6",   60) },
        { LEVEL("6.1", 61) },
        { LEVEL("6.2", 62) },
#undef LEVEL

    { "rc", "Bit rate control mode", OFFSET(rc_mode),
      AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE , "rc"},
        { "cqp", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "rc" },
        { "vbr", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "rc" },

    { "qp", "QP value for intra frames", OFFSET(qp),
      AV_OPT_TYPE_INT, { .i64 = 32 }, 0, 51, VE },

    { "sc_detection", "Scene change detection", OFFSET(scd),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },

    { "tune", "Quality tuning mode", OFFSET(tune), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, VE, "tune" },
        { "subjective", "Subjective quality mode", 0,
          AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "tune" },
        { "objective",  "Objective quality mode for PSNR / SSIM / VMAF benchmarking",  0,
          AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "tune" },

    { "bl_mode", "Random Access Prediction Structure type setting", OFFSET(base_layer_switch_mode),
      AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, VE },

    { "forced-idr", "If forcing keyframes, force them as IDR frames.", OFFSET(forced_idr),
      AV_OPT_TYPE_BOOL,   { .i64 = 0 }, -1, 1, VE },

    {NULL},
};

static const AVClass class = {
    .class_name = "libsvt_hevc",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault eb_enc_defaults[] = {
    { "b",         "7M"    },
    { "g",         "64"    },
    { "flags",     "+cgop" },
    { "qmin",      "10"    },
    { "qmax",      "48"    },
    { NULL },
};

AVCodec ff_libsvt_hevc_encoder = {
    .name           = "libsvt_hevc",
    .long_name      = NULL_IF_CONFIG_SMALL("SVT-HEVC(Scalable Video Technology for HEVC) encoder"),
    .priv_data_size = sizeof(SvtContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .init           = eb_enc_init,
    .send_frame     = eb_send_frame,
    .receive_packet = eb_receive_packet,
    .close          = eb_enc_close,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P,
                                                    AV_PIX_FMT_YUV420P10,
                                                    AV_PIX_FMT_NONE },
    .priv_class     = &class,
    .defaults       = eb_enc_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .wrapper_name   = "libsvt_hevc",
};
