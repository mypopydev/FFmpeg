/*
 * HEVC encoding using the SVT(Scalable Video Technology) library
 *
 * copyright (c) 2018 zhengxu.maxwell@gmail.com
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

#include "EbTime.h"
#include "EbTypes.h"
#include "EbErrorCodes.h"
#include "EbTime.h"
#include "EbApi.h"

#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"

#include "internal.h"
#include "avcodec.h"

typedef struct SvtEncoder {
    EB_H265_ENC_CONFIGURATION           enc_params;
    EB_COMPONENTTYPE                    *svt_handle;
    EB_BUFFERHEADERTYPE                 *in_buf;
    EB_BUFFERHEADERTYPE                 *out_buf;
    int                                  raw_size;
} SvtEncoder;

typedef struct SvtParams {
    int vui_info;
    int hierarchical_level;
    int intra_period;
    int la_depth;
    int intra_ref_type;
    int enc_mode;
    int rc_mode;
    int scd;
    int profile;
    int base_layer_switch_mode;
}SvtParams;

typedef struct SvtContext {
    AVClass     *class;
    SvtEncoder  *svt_enc;
    SvtParams   svt_param;
    int         eos_flag;
} SvtContext;

static void free_buffer(SvtEncoder *svt_enc)
{
    if (svt_enc->in_buf) {
        if (svt_enc->in_buf->pBuffer) {
            EB_H265_ENC_INPUT *in_data = (EB_H265_ENC_INPUT* )svt_enc->in_buf->pBuffer;
            if (in_data->luma)    av_freep(&in_data->luma);
            if (in_data->cb)      av_freep(&in_data->cb);
            if (in_data->cr)      av_freep(&in_data->cr);
            if (in_data->lumaExt) av_freep(&in_data->lumaExt);
            if (in_data->cbExt)   av_freep(&in_data->cbExt);
            if (in_data->crExt)   av_freep(&in_data->crExt);
            av_freep(&svt_enc->in_buf->pBuffer);
        }
        av_freep(&svt_enc->in_buf);
    }
    if (svt_enc->out_buf) {
        if (svt_enc->out_buf->pBuffer)
            av_freep(&svt_enc->out_buf->pBuffer);
        av_freep(&svt_enc->out_buf);
    }
}

static EB_ERRORTYPE alloc_buffer(EB_H265_ENC_CONFIGURATION *config, SvtEncoder *svt_enc)
{
    EB_H265_ENC_INPUT  *in_data = NULL;
    EB_ERRORTYPE       ret       = EB_ErrorNone;

    const int    pack_mode_10bit   = (config->encoderBitDepth > 8) && (config->compressedTenBitFormat == 0) ? 1 : 0;
    const size_t luma_size_8bit    = config->sourceWidth * config->sourceHeight * (1 << pack_mode_10bit);
    const size_t chroma_size_8bit  = luma_size_8bit >> 2;
    const size_t luma_size_10bit   = (config->encoderBitDepth > 8 && pack_mode_10bit == 0) ? luma_size_8bit : 0;
    const size_t chroma_size_10bit = (config->encoderBitDepth > 8 && pack_mode_10bit == 0) ? chroma_size_8bit : 0;

    svt_enc->raw_size = (luma_size_8bit + luma_size_10bit) * 3 / 2;

    // allocate buffer for in and out
    svt_enc->in_buf           = av_mallocz(sizeof(EB_BUFFERHEADERTYPE));
    svt_enc->out_buf          = av_mallocz(sizeof(EB_BUFFERHEADERTYPE));
    if (!svt_enc->in_buf || !svt_enc->out_buf)
        goto failed;

    svt_enc->in_buf->pBuffer  = av_mallocz(sizeof(EB_H265_ENC_INPUT));
    svt_enc->out_buf->pBuffer = av_mallocz(svt_enc->raw_size);
    if (!svt_enc->in_buf->pBuffer || !svt_enc->out_buf->pBuffer)
        goto failed;

    svt_enc->in_buf->nSize        = sizeof(EB_BUFFERHEADERTYPE);
    svt_enc->in_buf->pAppPrivate  = NULL;
    svt_enc->out_buf->nSize       = sizeof(EB_BUFFERHEADERTYPE);
    svt_enc->out_buf->nAllocLen   = svt_enc->raw_size;
    svt_enc->out_buf->pAppPrivate = NULL;

    in_data         = (EB_H265_ENC_INPUT* )svt_enc->in_buf->pBuffer;
    in_data->luma   = av_mallocz(luma_size_8bit   * sizeof(unsigned char));
    in_data->cb     = av_mallocz(chroma_size_8bit * sizeof(unsigned char));
    in_data->cr     = av_mallocz(chroma_size_8bit * sizeof(unsigned char));
    if ( !in_data->luma || !in_data->cb || !in_data->cr)
        goto failed;

    if (luma_size_10bit == 0)
        return ret;

    in_data->lumaExt      = av_mallocz(luma_size_10bit   * sizeof(unsigned char));
    in_data->cbExt        = av_mallocz(chroma_size_10bit * sizeof(unsigned char));
    in_data->crExt        = av_mallocz(chroma_size_10bit * sizeof(unsigned char));
    if ( !in_data->lumaExt || !in_data->cbExt || !in_data->crExt)
        goto failed;
    return ret;

failed:
    free_buffer(svt_enc);
    return AVERROR(ENOMEM);
}

static void set_default_params(EB_H265_ENC_CONFIGURATION  *param)
{
    param->frameRate = 60;
    param->frameRateNumerator = 0;
    param->frameRateDenominator = 0;
    param->encoderBitDepth = 8;
    param->compressedTenBitFormat = 0;
    param->inputPictureStride = 0;
    param->framesToBeEncoded = 0;

    param->interlacedVideo = EB_FALSE;
    param->qp = 32;
    param->useQpFile = EB_FALSE;
    param->sceneChangeDetection = 1;
    param->rateControlMode = 0;
    param->lookAheadDistance = 17;
    param->targetBitRate = 7000000;
    param->maxQpAllowed = 48;
    param->minQpAllowed = 10;
    param->baseLayerSwitchMode = 0;
    param->encMode  = 9;
    param->intraPeriodLength = -2;
    param->intraRefreshType = 1;
    param->hierarchicalLevels = 3;
    param->predStructure = EB_PRED_RANDOM_ACCESS;
    param->disableDlfFlag = EB_FALSE;
    param->enableSaoFlag = EB_TRUE;
    param->useDefaultMeHme = EB_TRUE;
    param->enableHmeFlag = EB_TRUE;
    param->enableHmeLevel0Flag = EB_TRUE;
    param->enableHmeLevel1Flag = EB_FALSE;
    param->enableHmeLevel2Flag = EB_FALSE;
    param->searchAreaWidth = 16;
    param->searchAreaHeight = 7;
    param->numberHmeSearchRegionInWidth = 2;
    param->numberHmeSearchRegionInHeight = 2;
    param->hmeLevel0TotalSearchAreaWidth = 64;
    param->hmeLevel0TotalSearchAreaHeight = 25;
    param->hmeLevel0SearchAreaInWidthArray[0] = 32;
    param->hmeLevel0SearchAreaInWidthArray[1] = 32;
    param->hmeLevel0SearchAreaInHeightArray[0] = 12;
    param->hmeLevel0SearchAreaInHeightArray[1] = 13;
    param->hmeLevel1SearchAreaInWidthArray[0] = 1;
    param->hmeLevel1SearchAreaInWidthArray[1] = 1;
    param->hmeLevel1SearchAreaInHeightArray[0] = 1;
    param->hmeLevel1SearchAreaInHeightArray[1] = 1;
    param->hmeLevel2SearchAreaInWidthArray[0] = 1;
    param->hmeLevel2SearchAreaInWidthArray[1] = 1;
    param->hmeLevel2SearchAreaInHeightArray[0] = 1;
    param->hmeLevel2SearchAreaInHeightArray[1] = 1;
    param->constrainedIntra = EB_FALSE;
    param->tune = 0;

    param->videoUsabilityInfo = 0;
    param->highDynamicRangeInput = 0;
    param->accessUnitDelimiter = 0;
    param->bufferingPeriodSEI = 0;
    param->pictureTimingSEI = 0;

    param->bitRateReduction = EB_TRUE;
    param->improveSharpness = EB_TRUE;
    param->registeredUserDataSeiFlag = EB_FALSE;
    param->unregisteredUserDataSeiFlag = EB_FALSE;
    param->recoveryPointSeiFlag = EB_FALSE;
    param->enableTemporalId = 1;
    param->inputOutputBufferFifoInitCount = 50;

    // Annex A parameters
    param->profile = 2;
    param->tier = 0;
    param->level = 0;

    // Latency
    param->injectorFrameRate = 60 << 16;
    param->speedControlFlag = 0;
    param->latencyMode = EB_NORMAL_LATENCY;

    // ASM Type
    param->asmType = ASM_AVX2;
    param->useRoundRobinThreadAssignment = EB_FALSE;
    param->channelId = 0;
    param->activeChannelCount = 1;
}

static EB_ERRORTYPE config_enc_params(EB_H265_ENC_CONFIGURATION  *param, AVCodecContext *avctx)
{
    SvtContext *q       = avctx->priv_data;
    SvtEncoder *svt_enc = q->svt_enc;
    EB_ERRORTYPE    ret = EB_ErrorNone;
    int         tenBits = 0;

    set_default_params(param);

    param->sourceWidth     = avctx->width;
    param->sourceHeight    = avctx->height;

    if (avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE) {
        av_log(NULL, AV_LOG_WARNING , "Encoder 10 bits depth input\n");
        param->compressedTenBitFormat = 0;
        tenBits = 1;
    }

    // Update param from options
    param->hierarchicalLevels     = q->svt_param.hierarchical_level;
    param->encMode                = q->svt_param.enc_mode;
    param->intraRefreshType       = q->svt_param.intra_ref_type;
    param->profile                = q->svt_param.profile;
    param->rateControlMode        = q->svt_param.rc_mode;
    param->sceneChangeDetection   = q->svt_param.scd;
    param->baseLayerSwitchMode    = q->svt_param.base_layer_switch_mode;

    param->targetBitRate          = avctx->bit_rate;

    if ((avctx->time_base.den) && (avctx->time_base.num)) {
        param->frameRate = avctx->time_base.den / avctx->time_base.num;
    }
    else
        param->frameRate = 25;

    if (q->svt_param.vui_info)
        param->videoUsabilityInfo = q->svt_param.vui_info;
    if (q->svt_param.la_depth != -1)
        param->lookAheadDistance  = q->svt_param.la_depth;

    if (tenBits == 1) {
        param->encoderBitDepth        = 10;
        param->profile                = 2;
    }

    ret = alloc_buffer(param, svt_enc);

    return ret;
}

static void read_in_data(EB_H265_ENC_CONFIGURATION *config, const AVFrame* frame, EB_BUFFERHEADERTYPE *headerPtr)
{
    EB_U64 i = 0;
    EB_U64 offset = 0;
    EB_U32 is16bit = config->encoderBitDepth > 8;
    EB_U64 lumaReadSize = (EB_U64)config->sourceWidth * config->sourceHeight<< is16bit;
    EB_U64 rowReadByte  = (EB_U64)config->sourceWidth << is16bit;
    EB_H265_ENC_INPUT *in_data = (EB_H265_ENC_INPUT*)headerPtr->pBuffer;

    // support yuv420p and p010
    for (i = 0; i < config->sourceHeight; i++) {
        memcpy(in_data->luma + offset, frame->data[0] + i * frame->linesize[0], rowReadByte);
        offset += rowReadByte;
    }
    offset = 0;
    for (i = 0; i < config->sourceHeight / 2; i++) {
        memcpy(in_data->cb + offset, frame->data[1] + i * frame->linesize[1], rowReadByte / 2);
        memcpy(in_data->cr + offset, frame->data[2] + i * frame->linesize[2], rowReadByte / 2);
        offset += rowReadByte / 2;
    }
    headerPtr->nFilledLen   += lumaReadSize * 3/2u;
}

static av_cold int eb_enc_init(AVCodecContext *avctx)
{
    SvtContext   *q = avctx->priv_data;
    SvtEncoder   *svt_enc = NULL;
    EB_U32       instanceIndex = 0;
    EB_ERRORTYPE ret = EB_ErrorNone;

    q->svt_enc  = av_mallocz(sizeof(*q->svt_enc));
    if (!q->svt_enc)
        return AVERROR(ENOMEM);
    svt_enc = q->svt_enc;

    q->eos_flag = 0;

    ret = EbInitHandle(&svt_enc->svt_handle, q);
    if (ret != EB_ErrorNone)
        return ret;

    ret = config_enc_params(&svt_enc->enc_params, avctx);
    if (ret != EB_ErrorNone)
        return ret;

    ret = EbH265EncSetParameter(svt_enc->svt_handle, &svt_enc->enc_params);
    if (ret != EB_ErrorNone)
        return ret;

    ret = EbInitEncoder(svt_enc->svt_handle);
    if (ret != EB_ErrorNone)
        return ret;

    ret = EbStartEncoder(svt_enc->svt_handle, instanceIndex);
    if (ret != EB_ErrorNone)
        return ret;

    return ret;
}

static int eb_send_frame(AVCodecContext *avctx, const AVFrame *frame)
{
    SvtContext           *q = avctx->priv_data;
    SvtEncoder           *svt_enc = q->svt_enc;
    EB_BUFFERHEADERTYPE  *headerPtr = svt_enc->in_buf;
    int                  ret = 0;

    if (!frame) {
        EB_BUFFERHEADERTYPE headerPtrLast;
        headerPtrLast.nAllocLen = 0;
        headerPtrLast.nFilledLen = 0;
        headerPtrLast.nTickCount = 0;
        headerPtrLast.pAppPrivate = NULL;
        headerPtrLast.nOffset = 0;
        headerPtrLast.nTimeStamp = 0;
        headerPtrLast.nFlags = EB_BUFFERFLAG_EOS;
        headerPtrLast.pBuffer = NULL;
        EbH265EncSendPicture((EB_HANDLETYPE)svt_enc->svt_handle, &headerPtrLast);
        q->eos_flag = 1;
        av_log(NULL, AV_LOG_INFO, "Finish sending frames!!!\n");
        return ret;
    }

    read_in_data(&svt_enc->enc_params, frame, headerPtr);

    headerPtr->nOffset    = 0;
    headerPtr->nFlags     = 0;
    headerPtr->nFlags     = 0;
    headerPtr->nTimeStamp = 0;
    headerPtr->pAppPrivate = (EB_PTR)EB_NULL;
    EbH265EncSendPicture((EB_HANDLETYPE)svt_enc->svt_handle, headerPtr);

    return ret;
}

static int eb_receive_packet(AVCodecContext *avctx, AVPacket *pkt)
{
    SvtContext  *q = avctx->priv_data;
    SvtEncoder  *svt_enc = q->svt_enc;
    EB_BUFFERHEADERTYPE   *headerPtr = svt_enc->out_buf;
    EB_ERRORTYPE          stream_status = EB_ErrorNone;
    int ret = 0;

    stream_status = EbH265GetPacket((EB_HANDLETYPE)svt_enc->svt_handle, headerPtr, q->eos_flag);
    if ((stream_status == EB_NoErrorEmptyQueue))
        return AVERROR(EAGAIN);

    if ((ret = ff_alloc_packet2(avctx, pkt, svt_enc->raw_size, 0)) < 0)
        return ret;

    memcpy(pkt->data, headerPtr->pBuffer + headerPtr->nOffset, headerPtr->nFilledLen);
    pkt->size = headerPtr->nFilledLen;

    ret = (headerPtr->nFlags & EB_BUFFERFLAG_EOS) ? AVERROR_EOF : 0;
    return ret;
}

static int eb_enc_frame(AVCodecContext *avctx, AVPacket *pkt,
                         const AVFrame *frame, int *got_packet)
{
    return 0;
}

static av_cold int eb_enc_close(AVCodecContext *avctx)
{
    SvtContext *q = avctx->priv_data;
    SvtEncoder   *svt_enc = q->svt_enc;

    EbStopEncoder(svt_enc->svt_handle, 0);
    EbDeinitEncoder(svt_enc->svt_handle);
    EbDeinitHandle(svt_enc->svt_handle);

    free_buffer(svt_enc);
    av_freep(&svt_enc);

    return 0;
}

#define OFFSET(x) offsetof(SvtContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    {"vui", "Enable vui info", OFFSET(svt_param.vui_info), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    {"hielevel", "Hierarchical Prediction Levels [0,5]", OFFSET(svt_param.hierarchical_level), AV_OPT_TYPE_INT, { .i64 = 3 }, 0, 5, VE },
    {"la_depth", "Look Ahead Distance [0,120]", OFFSET(svt_param.la_depth), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 120, VE },
    {"intra_ref_type", "Intra Refresh Type 0: No intra refresh1: CRA (Open GOP) 2: IDR", OFFSET(svt_param.intra_ref_type), AV_OPT_TYPE_INT, { .i64 = 2 }, 0, 2, VE },
    {"enc_mode", "Encode mode [1,6]", OFFSET(svt_param.enc_mode), AV_OPT_TYPE_INT, { .i64 = 6 }, 1, 6, VE },
    {"profile", "Profile now support[1,2],Main Still Picture Profile not supported", OFFSET(svt_param.profile), AV_OPT_TYPE_INT, { .i64 = 2 }, 1, 2, VE },
    {"rc", "RC mode", OFFSET(svt_param.rc_mode), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    {"scd", "scene change dection", OFFSET(svt_param.scd), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    {"bl_mode", "Random Access Prediction Structure Type", OFFSET(svt_param.base_layer_switch_mode), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE },
    {NULL},
};

static const AVClass class = {
    .class_name = "hevc_eb encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault eb_enc_defaults[] = {
    { "b",         "7M"    },
    { "refs",      "0"     },
    { "g",         "90"   },
    { "flags",     "+cgop" },
    { NULL },
};

AVCodec ff_hevc_svt_encoder = {
    .name           = "hevc_svt",
    .long_name      = NULL_IF_CONFIG_SMALL("SVT(Scalable Video Technology) HEVC encoder"),
    .priv_data_size = sizeof(SvtContext),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .init           = eb_enc_init,
    .send_frame     = eb_send_frame,
    .receive_packet = eb_receive_packet,
    .encode2        = eb_enc_frame,
    .close          = eb_enc_close,
    .capabilities   = AV_CODEC_CAP_DELAY,
    .pix_fmts       = (const enum AVPixelFormat[]){ AV_PIX_FMT_YUV420P,
                                                    AV_PIX_FMT_YUV420P10,
                                                    AV_PIX_FMT_NONE },
    .priv_class     = &class,
    .defaults       = eb_enc_defaults,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};

