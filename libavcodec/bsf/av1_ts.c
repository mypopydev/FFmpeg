/*
 * AV1 MPEG-TS bitstream filter
 * Copyright (c) 2024
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
 * This bitstream filter converts AV1 bitstreams between Low Overhead Bitstream
 * Format (used in MP4/WebM) and Start Code Based Format (used in MPEG-TS) as
 * defined in "AV1 Codec in MPEG-2 Transport Stream" specification.
 *
 * Start Code Based Format prepends each OBU with a 3-byte start code (0x000001)
 * and applies emulation prevention bytes (0x03) to prevent start code emulation.
 */

#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "bsf.h"
#include "bsf_internal.h"
#include "av1.h"
#include "av1_parse.h"

/* Start code for AV1 OBUs in MPEG-TS */
#define AV1_START_CODE 0x000001

typedef struct AV1TSContext {
    const AVClass *class;
    int mode;  /* 0 = to TS (add start codes), 1 = from TS (remove start codes) */
} AV1TSContext;

/**
 * Write data with emulation prevention bytes
 * Returns number of bytes written
 */
static int write_escaped(uint8_t *dst, const uint8_t *src, int size)
{
    int i, j = 0;
    for (i = 0; i < size; i++) {
        if (i + 2 < size && src[i] == 0 && src[i + 1] == 0 && src[i + 2] <= 3) {
            dst[j++] = 0;
            dst[j++] = 0;
            dst[j++] = 3;  /* emulation_prevention_three_byte */
            i++;
        } else {
            dst[j++] = src[i];
        }
    }
    return j;
}

/**
 * Remove emulation prevention bytes from escaped data
 * Returns number of bytes written
 */
static int remove_escape(uint8_t *dst, const uint8_t *src, int size)
{
    int i, j = 0;
    for (i = 0; i < size; i++) {
        if (i + 2 < size && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 3) {
            dst[j++] = 0;
            dst[j++] = 0;
            i += 2;  /* skip the emulation_prevention_three_byte */
        } else {
            dst[j++] = src[i];
        }
    }
    return j;
}

/**
 * Convert from Low Overhead Bitstream Format to Start Code Based Format
 * (for muxing to MPEG-TS)
 */
static int av1_to_ts_filter(AVBSFContext *ctx, AVPacket *pkt)
{
    int ret;
    const uint8_t *in_data;
    int in_size;
    uint8_t *out_data;
    int out_size = 0;
    int max_out_size;
    AV1OBU obu;

    ret = ff_bsf_get_packet_ref(ctx, pkt);
    if (ret < 0)
        return ret;

    in_data = pkt->data;
    in_size = pkt->size;

    /* Calculate maximum output size:
     * For each OBU: 3 bytes start code + escaped OBU data
     * Worst case: every byte could need escape, so 3/2 * original size
     * Plus 3 bytes per OBU for start code
     */
    max_out_size = in_size * 2 + 64;  /* conservative estimate */

    ret = av_new_packet(pkt, max_out_size);
    if (ret < 0)
        return ret;

    out_data = pkt->data;

    /* Parse and convert each OBU */
    while (in_size > 0) {
        int obu_size;
        int escaped_size;

        ret = ff_av1_extract_obu(&obu, in_data, in_size, ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to extract OBU\n");
            goto fail;
        }

        obu_size = obu.raw_size;

        /* Write start code */
        out_data[out_size++] = 0;
        out_data[out_size++] = 0;
        out_data[out_size++] = 1;

        /* Write OBU with emulation prevention */
        escaped_size = write_escaped(out_data + out_size, obu.raw_data, obu_size);
        out_size += escaped_size;

        in_data += obu_size;
        in_size -= obu_size;
    }

    av_shrink_packet(pkt, out_size);
    return 0;

fail:
    av_packet_unref(pkt);
    return ret;
}

/**
 * Convert from Start Code Based Format to Low Overhead Bitstream Format
 * (for demuxing from MPEG-TS)
 */
static int av1_from_ts_filter(AVBSFContext *ctx, AVPacket *pkt)
{
    int ret;
    const uint8_t *in_data;
    int in_size;
    uint8_t *out_data;
    int out_size = 0;

    ret = ff_bsf_get_packet_ref(ctx, pkt);
    if (ret < 0)
        return ret;

    in_data = pkt->data;
    in_size = pkt->size;

    /* Output size will be smaller (no start codes, fewer escape bytes) */
    ret = av_new_packet(pkt, in_size);
    if (ret < 0)
        return ret;

    out_data = pkt->data;

    /* Find and process each OBU */
    while (in_size > 0) {
        const uint8_t *obu_start;
        int obu_size;
        int unescaped_size;

        /* Find start code 0x000001 */
        if (in_size < 4 || in_data[0] != 0 || in_data[1] != 0 || in_data[2] != 1) {
            av_log(ctx, AV_LOG_ERROR, "Missing start code at position %d\n",
                   (int)(in_data - pkt->data));
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        in_data += 3;
        in_size -= 3;
        obu_start = in_data;

        /* Find next start code or end of data */
        obu_size = 0;
        while (obu_size + 3 <= in_size) {
            if (in_data[obu_size] == 0 && in_data[obu_size + 1] == 0 &&
                in_data[obu_size + 2] == 1) {
                break;
            }
            obu_size++;
        }
        if (obu_size + 3 > in_size) {
            obu_size = in_size;  /* Last OBU */
        }

        /* Remove emulation prevention bytes */
        unescaped_size = remove_escape(out_data + out_size, obu_start, obu_size);
        out_size += unescaped_size;

        in_data += obu_size;
        in_size -= obu_size;
    }

    av_shrink_packet(pkt, out_size);
    return 0;

fail:
    av_packet_unref(pkt);
    return ret;
}

static int av1_ts_filter(AVBSFContext *ctx, AVPacket *pkt)
{
    AV1TSContext *s = ctx->priv_data;

    if (s->mode == 0)
        return av1_to_ts_filter(ctx, pkt);
    else
        return av1_from_ts_filter(ctx, pkt);
}

static const enum AVCodecID av1_ts_codec_ids[] = {
    AV_CODEC_ID_AV1, AV_CODEC_ID_NONE,
};

#define OFFSET(x) offsetof(AV1TSContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_BSF_PARAM)

static const AVOption av1_ts_options[] = {
    { "mode", "Conversion mode", OFFSET(mode), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS, .unit = "mode" },
    { "to_ts",   "Low Overhead to Start Code (for muxing)", 0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, FLAGS, .unit = "mode" },
    { "from_ts", "Start Code to Low Overhead (for demuxing)", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, FLAGS, .unit = "mode" },
    { NULL }
};

static const AVClass av1_ts_class = {
    .class_name = "av1_ts",
    .item_name  = av_default_item_name,
    .option     = av1_ts_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFBitStreamFilter ff_av1_ts_bsf = {
    .p.name         = "av1_ts",
    .p.codec_ids    = av1_ts_codec_ids,
    .p.priv_class   = &av1_ts_class,
    .priv_data_size = sizeof(AV1TSContext),
    .filter         = av1_ts_filter,
};
