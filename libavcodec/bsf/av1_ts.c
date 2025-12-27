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
#define START_CODE_SIZE 3
#define MIN_PACKET_SIZE 4
#define BUFFER_PADDING 64

/* Temporal Delimiter OBU: type=2, extension_flag=0, has_size_field=1, reserved=0
 * Binary: 0 0010 0 1 0 = 0x12, followed by size=0 (LEB128) */
static const uint8_t temporal_delimiter_obu[] = { 0x12, 0x00 };

typedef struct AV1TSContext {
    const AVClass *class;
    int mode;  /* 0 = to TS (add start codes), 1 = from TS (remove start codes) */
} AV1TSContext;

/**
 * Prepare output packet by allocating buffer and copying properties from input
 * Returns 0 on success, negative AVERROR on failure
 */
static int prepare_output_packet(AVBSFContext *ctx, AVPacket *pkt,
                                 const AVPacket *in, int out_size)
{
    int ret = av_new_packet(pkt, out_size);
    if (ret < 0)
        return ret;

    ret = av_packet_copy_props(pkt, in);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }
    return 0;
}

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
            dst[j++] = src[i + 2];  /* Write the third byte after insertion byte */
            i += 2;  /* Skip the two zeros and the third byte */
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
 * Check if the first OBU in data is a valid Temporal Delimiter
 * Returns 1 if a valid TD is present, 0 otherwise
 *
 * A valid Temporal Delimiter OBU must:
 * - Have OBU type = AV1_OBU_TEMPORAL_DELIMITER (2)
 * - Have has_size_field = 1
 * - Have size = 0 (LEB128 encoded)
 */
static int has_temporal_delimiter(const uint8_t *data, int size)
{
    if (size < 2)
        return 0;

    /* OBU header: forbidden(1) | type(4) | extension(1) | has_size(1) | reserved(1)
     * Temporal Delimiter type = 2, so header byte with has_size=1 is 0x12
     * with has_size=0 is 0x10 (but per spec, TD should have has_size=1) */
    int obu_type = (data[0] >> 3) & 0x0f;
    int has_size = (data[0] >> 1) & 0x01;

    if (obu_type != AV1_OBU_TEMPORAL_DELIMITER)
        return 0;

    /* Per AV1 spec, Temporal Delimiter should have has_size_field = 1
     * and should have zero payload size */
    if (!has_size) {
        /* TD with has_size=0 is valid per spec but uncommon.
         * In this case, the entire TD is just the header byte. */
        return 1;
    }

    /* has_size=1, next byte(s) contain the size in LEB128 format.
     * For TD, size must be 0, which is encoded as a single 0 byte. */
    if (data[1] != 0) {
        /* Size is not zero, so this is not a valid Temporal Delimiter */
        return 0;
    }

    return 1;
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
    AVPacket *in = NULL;

    /* Move input packet to 'in' */
    in = av_packet_alloc();
    if (!in)
        return AVERROR(ENOMEM);

    ret = ff_bsf_get_packet_ref(ctx, in);
    if (ret < 0) {
        av_packet_free(&in);
        return ret;
    }

    in_data = in->data;
    in_size = in->size;

    av_log(ctx, AV_LOG_DEBUG, "av1_to_ts: input size %d\n", in_size);

    /* Check if packet starts with Temporal Delimiter.
     * If not, we'll insert one to ensure proper TU boundaries.
     */
    int need_td = !has_temporal_delimiter(in_data, in_size);
    if (need_td) {
        av_log(ctx, AV_LOG_DEBUG, "Inserting Temporal Delimiter OBU\n");
    }

    /* Calculate maximum output size:
     * Worst case: every byte could need escape, so 3/2 * original size (approx * 2)
     * Plus overhead for start codes and TD insertion
     */
    max_out_size = in_size * 2 + BUFFER_PADDING + (need_td ? (START_CODE_SIZE + sizeof(temporal_delimiter_obu)) : 0);

    ret = prepare_output_packet(ctx, pkt, in, max_out_size);
    if (ret < 0) {
        av_packet_free(&in);
        return ret;
    }

    out_data = pkt->data;

    /* Insert Temporal Delimiter if needed */
    if (need_td) {
        /* Write start code for TD */
        out_data[out_size++] = 0;
        out_data[out_size++] = 0;
        out_data[out_size++] = 1;
        /* Write TD OBU (no escaping needed - it's just 0x12 0x00) */
        memcpy(out_data + out_size, temporal_delimiter_obu, sizeof(temporal_delimiter_obu));
        out_size += sizeof(temporal_delimiter_obu);
    }

    /* Parse and convert each OBU */
    while (in_size > 0) {
        int obu_size;
        int escaped_size;

        ret = ff_av1_extract_obu(&obu, in_data, in_size, ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to extract OBU\n");
            av_packet_unref(pkt);
            av_packet_free(&in);
            return ret;
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
    av_packet_free(&in);
    return 0;
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
    AVPacket *in = NULL;

    /* Allocate separate input packet to preserve data while we allocate output. */
    in = av_packet_alloc();
    if (!in)
        return AVERROR(ENOMEM);

    ret = ff_bsf_get_packet_ref(ctx, in);
    if (ret < 0) {
        av_packet_free(&in);
        return ret;
    }

    in_data = in->data;
    in_size = in->size;

    /* Output size will be smaller (no start codes, fewer escape bytes) */
    ret = prepare_output_packet(ctx, pkt, in, in_size + BUFFER_PADDING);
    if (ret < 0) {
        av_packet_free(&in);
        return ret;
    }

    out_data = pkt->data;

    /* Find and process each OBU */
    while (in_size > 0) {
        const uint8_t *obu_start;
        int obu_size;
        int unescaped_size;

        /* Check for start code 0x000001 */
        if (in_size < MIN_PACKET_SIZE || in_data[0] != 0 || in_data[1] != 0 || in_data[2] != 1) {
            av_log(ctx, AV_LOG_ERROR, "Missing start code at position %d\n",
                   (int)(in_data - pkt->data));
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        in_data += START_CODE_SIZE;
        in_size -= START_CODE_SIZE;
        obu_start = in_data;

        /* Find next start code or end of data */
        obu_size = 0;
        while (obu_size + START_CODE_SIZE <= in_size) {
            if (in_data[obu_size] == 0 && in_data[obu_size + 1] == 0 &&
                in_data[obu_size + 2] == 1) {
                break;
            }
            obu_size++;
        }
        if (obu_size + START_CODE_SIZE > in_size) {
            obu_size = in_size;  /* Last OBU */
        }

        /* Remove emulation prevention bytes */
        unescaped_size = remove_escape(out_data + out_size, obu_start, obu_size);
        out_size += unescaped_size;

        in_data += obu_size;
        in_size -= obu_size;
    }

    av_shrink_packet(pkt, out_size);
    av_packet_free(&in);
    return 0;

fail:
    av_packet_unref(pkt);
    av_packet_free(&in);
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
