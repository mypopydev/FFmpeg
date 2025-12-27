/*
 * Test for av1_ts bitstream filter memory handling
 *
 * This test verifies that the av1_ts BSF correctly handles memory
 * when converting between Low Overhead and Start Code formats.
 *
 * Bug being tested: In av1_from_ts_filter, the original implementation
 * stored a pointer to pkt->data in in_data, then called av_new_packet()
 * which frees pkt and allocates new memory, leaving in_data as a
 * dangling pointer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

/* Minimal AV1 OBU header for testing:
 * - OBU type = 1 (sequence header)
 * - obu_extension_flag = 0
 * - obu_has_size_field = 1
 * - obu_size = 2 (LEB128 encoded)
 * - 2 bytes of dummy payload
 */
static const uint8_t low_overhead_obu[] = {
    0x0A,       /* OBU header: type=1, ext=0, has_size=1 */
    0x02,       /* OBU size (LEB128): 2 bytes */
    0x00, 0x00  /* Dummy payload */
};

/* Same OBU in Start Code format (Annex B):
 * - 3-byte start code 0x000001
 * - Same OBU data
 */
static const uint8_t start_code_obu[] = {
    0x00, 0x00, 0x01,  /* Start code */
    0x0A,              /* OBU header */
    0x02,              /* OBU size */
    0x00, 0x00         /* Dummy payload */
};

/* Test OBU with data that could trigger emulation prevention:
 * Contains 0x00 0x00 0x01 pattern that needs escaping
 * Includes Temporal Delimiter at start to prevent auto-insertion
 */
static const uint8_t obu_with_escape_pattern[] = {
    0x12, 0x00,                   /* Temporal Delimiter OBU (type=2, size=0) */
    0x0A,                         /* OBU header */
    0x05,                         /* OBU size: 5 bytes */
    0xAA, 0x00, 0x00, 0x01, 0xBB  /* Payload with 0x000001 pattern */
};

/* Same OBU after escaping (with emulation prevention byte 0x03) */
static const uint8_t escaped_obu[] = {
    0x00, 0x00, 0x01,                    /* Start code */
    0x12, 0x00,                          /* Temporal Delimiter OBU */
    0x00, 0x00, 0x01,                    /* Start code */
    0x0A,                                /* OBU header */
    0x05,                                /* OBU size */
    0xAA, 0x00, 0x00, 0x03, 0x01, 0xBB   /* Escaped payload */
};

static int test_to_ts_mode(void)
{
    const AVBitStreamFilter *filter;
    AVBSFContext *bsf_ctx = NULL;
    AVPacket *pkt_in = NULL;
    AVPacket *pkt_out = NULL;
    int ret;

    printf("Testing to_ts mode (Low Overhead -> Start Code)...\n");

    filter = av_bsf_get_by_name("av1_ts");
    if (!filter) {
        fprintf(stderr, "av1_ts BSF not found\n");
        return -1;
    }

    ret = av_bsf_alloc(filter, &bsf_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to allocate BSF context\n");
        return ret;
    }

    /* Configure for to_ts mode */
    av_opt_set(bsf_ctx->priv_data, "mode", "to_ts", 0);

    bsf_ctx->par_in->codec_id = AV_CODEC_ID_AV1;
    ret = av_bsf_init(bsf_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to init BSF\n");
        av_bsf_free(&bsf_ctx);
        return ret;
    }

    pkt_in = av_packet_alloc();
    pkt_out = av_packet_alloc();
    if (!pkt_in || !pkt_out) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    /* Create input packet with Low Overhead data */
    ret = av_new_packet(pkt_in, sizeof(low_overhead_obu));
    if (ret < 0)
        goto cleanup;
    memcpy(pkt_in->data, low_overhead_obu, sizeof(low_overhead_obu));

    /* Send to BSF */
    ret = av_bsf_send_packet(bsf_ctx, pkt_in);
    if (ret < 0) {
        fprintf(stderr, "Failed to send packet: %d\n", ret);
        goto cleanup;
    }

    /* Receive filtered packet */
    ret = av_bsf_receive_packet(bsf_ctx, pkt_out);
    if (ret < 0) {
        fprintf(stderr, "Failed to receive packet: %d\n", ret);
        goto cleanup;
    }

    /* Verify output has start code */
    if (pkt_out->size < 3 ||
        pkt_out->data[0] != 0x00 ||
        pkt_out->data[1] != 0x00 ||
        pkt_out->data[2] != 0x01) {
        fprintf(stderr, "Output missing start code\n");
        ret = -1;
        goto cleanup;
    }

    printf("  to_ts mode: PASSED (output size: %d, has start code)\n", pkt_out->size);
    ret = 0;

cleanup:
    av_packet_free(&pkt_in);
    av_packet_free(&pkt_out);
    av_bsf_free(&bsf_ctx);
    return ret;
}

static int test_from_ts_mode(void)
{
    const AVBitStreamFilter *filter;
    AVBSFContext *bsf_ctx = NULL;
    AVPacket *pkt_in = NULL;
    AVPacket *pkt_out = NULL;
    int ret;

    printf("Testing from_ts mode (Start Code -> Low Overhead)...\n");

    filter = av_bsf_get_by_name("av1_ts");
    if (!filter) {
        fprintf(stderr, "av1_ts BSF not found\n");
        return -1;
    }

    ret = av_bsf_alloc(filter, &bsf_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to allocate BSF context\n");
        return ret;
    }

    /* Configure for from_ts mode */
    av_opt_set(bsf_ctx->priv_data, "mode", "from_ts", 0);

    bsf_ctx->par_in->codec_id = AV_CODEC_ID_AV1;
    ret = av_bsf_init(bsf_ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to init BSF\n");
        av_bsf_free(&bsf_ctx);
        return ret;
    }

    pkt_in = av_packet_alloc();
    pkt_out = av_packet_alloc();
    if (!pkt_in || !pkt_out) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    /* Create input packet with Start Code data */
    ret = av_new_packet(pkt_in, sizeof(start_code_obu));
    if (ret < 0)
        goto cleanup;
    memcpy(pkt_in->data, start_code_obu, sizeof(start_code_obu));

    /* Send to BSF */
    ret = av_bsf_send_packet(bsf_ctx, pkt_in);
    if (ret < 0) {
        fprintf(stderr, "Failed to send packet: %d\n", ret);
        goto cleanup;
    }

    /* Receive filtered packet - THIS IS WHERE THE BUG WOULD MANIFEST
     * If in_data becomes a dangling pointer after av_new_packet(),
     * this will either crash or produce garbage output.
     */
    ret = av_bsf_receive_packet(bsf_ctx, pkt_out);
    if (ret < 0) {
        fprintf(stderr, "Failed to receive packet: %d\n", ret);
        goto cleanup;
    }

    /* Verify output matches expected Low Overhead format */
    if (pkt_out->size != sizeof(low_overhead_obu)) {
        fprintf(stderr, "Output size mismatch: expected %zu, got %d\n",
                sizeof(low_overhead_obu), pkt_out->size);
        ret = -1;
        goto cleanup;
    }

    if (memcmp(pkt_out->data, low_overhead_obu, sizeof(low_overhead_obu)) != 0) {
        fprintf(stderr, "Output data mismatch\n");
        printf("  Expected: ");
        for (size_t i = 0; i < sizeof(low_overhead_obu); i++)
            printf("%02x ", low_overhead_obu[i]);
        printf("\n  Got:      ");
        for (int i = 0; i < pkt_out->size; i++)
            printf("%02x ", pkt_out->data[i]);
        printf("\n");
        ret = -1;
        goto cleanup;
    }

    printf("  from_ts mode: PASSED (output size: %d, data matches)\n", pkt_out->size);
    ret = 0;

cleanup:
    av_packet_free(&pkt_in);
    av_packet_free(&pkt_out);
    av_bsf_free(&bsf_ctx);
    return ret;
}

static int test_escape_handling(void)
{
    const AVBitStreamFilter *filter;
    AVBSFContext *bsf_ctx = NULL;
    AVPacket *pkt_in = NULL;
    AVPacket *pkt_out = NULL;
    int ret;

    printf("Testing emulation prevention byte handling...\n");

    filter = av_bsf_get_by_name("av1_ts");
    if (!filter) {
        fprintf(stderr, "av1_ts BSF not found\n");
        return -1;
    }

    /* Test to_ts: should add escape bytes */
    ret = av_bsf_alloc(filter, &bsf_ctx);
    if (ret < 0)
        return ret;

    av_opt_set(bsf_ctx->priv_data, "mode", "to_ts", 0);
    bsf_ctx->par_in->codec_id = AV_CODEC_ID_AV1;
    ret = av_bsf_init(bsf_ctx);
    if (ret < 0) {
        av_bsf_free(&bsf_ctx);
        return ret;
    }

    pkt_in = av_packet_alloc();
    pkt_out = av_packet_alloc();
    if (!pkt_in || !pkt_out) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    ret = av_new_packet(pkt_in, sizeof(obu_with_escape_pattern));
    if (ret < 0)
        goto cleanup;
    memcpy(pkt_in->data, obu_with_escape_pattern, sizeof(obu_with_escape_pattern));

    ret = av_bsf_send_packet(bsf_ctx, pkt_in);
    if (ret < 0)
        goto cleanup;

    ret = av_bsf_receive_packet(bsf_ctx, pkt_out);
    if (ret < 0)
        goto cleanup;

    /* Verify escape byte was inserted */
    if (pkt_out->size != sizeof(escaped_obu)) {
        fprintf(stderr, "Escape test: size mismatch, expected %zu got %d\n",
                sizeof(escaped_obu), pkt_out->size);
        fprintf(stderr, "  Expected: ");
        for (size_t i = 0; i < sizeof(escaped_obu); i++)
            fprintf(stderr, "%02x ", escaped_obu[i]);
        fprintf(stderr, "\n  Got:      ");
        for (int i = 0; i < pkt_out->size; i++)
            fprintf(stderr, "%02x ", pkt_out->data[i]);
        fprintf(stderr, "\n");
        ret = -1;
        goto cleanup;
    }

    if (memcmp(pkt_out->data, escaped_obu, sizeof(escaped_obu)) != 0) {
        fprintf(stderr, "Escape test: data mismatch\n");
        ret = -1;
        goto cleanup;
    }

    printf("  Escape insertion: PASSED\n");

    /* Now test from_ts: should remove escape bytes */
    av_packet_unref(pkt_in);
    av_packet_unref(pkt_out);
    av_bsf_free(&bsf_ctx);

    ret = av_bsf_alloc(filter, &bsf_ctx);
    if (ret < 0)
        return ret;

    av_opt_set(bsf_ctx->priv_data, "mode", "from_ts", 0);
    bsf_ctx->par_in->codec_id = AV_CODEC_ID_AV1;
    ret = av_bsf_init(bsf_ctx);
    if (ret < 0) {
        av_bsf_free(&bsf_ctx);
        return ret;
    }

    ret = av_new_packet(pkt_in, sizeof(escaped_obu));
    if (ret < 0)
        goto cleanup;
    memcpy(pkt_in->data, escaped_obu, sizeof(escaped_obu));

    ret = av_bsf_send_packet(bsf_ctx, pkt_in);
    if (ret < 0)
        goto cleanup;

    ret = av_bsf_receive_packet(bsf_ctx, pkt_out);
    if (ret < 0)
        goto cleanup;

    if (pkt_out->size != sizeof(obu_with_escape_pattern)) {
        fprintf(stderr, "Unescape test: size mismatch, expected %zu got %d\n",
                sizeof(obu_with_escape_pattern), pkt_out->size);
        ret = -1;
        goto cleanup;
    }

    if (memcmp(pkt_out->data, obu_with_escape_pattern, sizeof(obu_with_escape_pattern)) != 0) {
        fprintf(stderr, "Unescape test: data mismatch\n");
        ret = -1;
        goto cleanup;
    }

    printf("  Escape removal: PASSED\n");
    ret = 0;

cleanup:
    av_packet_free(&pkt_in);
    av_packet_free(&pkt_out);
    av_bsf_free(&bsf_ctx);
    return ret;
}

static int test_roundtrip(void)
{
    const AVBitStreamFilter *filter;
    AVBSFContext *to_ts_ctx = NULL, *from_ts_ctx = NULL;
    AVPacket *pkt_orig = NULL;
    AVPacket *pkt_ts = NULL;
    AVPacket *pkt_back = NULL;
    int ret;

    printf("Testing roundtrip conversion...\n");

    filter = av_bsf_get_by_name("av1_ts");
    if (!filter)
        return -1;

    /* Setup to_ts BSF */
    ret = av_bsf_alloc(filter, &to_ts_ctx);
    if (ret < 0)
        return ret;
    av_opt_set(to_ts_ctx->priv_data, "mode", "to_ts", 0);
    to_ts_ctx->par_in->codec_id = AV_CODEC_ID_AV1;
    ret = av_bsf_init(to_ts_ctx);
    if (ret < 0)
        goto cleanup;

    /* Setup from_ts BSF */
    ret = av_bsf_alloc(filter, &from_ts_ctx);
    if (ret < 0)
        goto cleanup;
    av_opt_set(from_ts_ctx->priv_data, "mode", "from_ts", 0);
    from_ts_ctx->par_in->codec_id = AV_CODEC_ID_AV1;
    ret = av_bsf_init(from_ts_ctx);
    if (ret < 0)
        goto cleanup;

    pkt_orig = av_packet_alloc();
    pkt_ts = av_packet_alloc();
    pkt_back = av_packet_alloc();
    if (!pkt_orig || !pkt_ts || !pkt_back) {
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    /* Start with Low Overhead OBU containing escape pattern */
    ret = av_new_packet(pkt_orig, sizeof(obu_with_escape_pattern));
    if (ret < 0)
        goto cleanup;
    memcpy(pkt_orig->data, obu_with_escape_pattern, sizeof(obu_with_escape_pattern));

    /* Convert to TS format */
    ret = av_bsf_send_packet(to_ts_ctx, pkt_orig);
    if (ret < 0)
        goto cleanup;
    ret = av_bsf_receive_packet(to_ts_ctx, pkt_ts);
    if (ret < 0)
        goto cleanup;

    /* Convert back from TS format */
    ret = av_bsf_send_packet(from_ts_ctx, pkt_ts);
    if (ret < 0)
        goto cleanup;
    ret = av_bsf_receive_packet(from_ts_ctx, pkt_back);
    if (ret < 0)
        goto cleanup;

    /* Verify roundtrip produces identical data */
    if (pkt_back->size != sizeof(obu_with_escape_pattern)) {
        fprintf(stderr, "Roundtrip: size mismatch\n");
        ret = -1;
        goto cleanup;
    }

    if (memcmp(pkt_back->data, obu_with_escape_pattern, sizeof(obu_with_escape_pattern)) != 0) {
        fprintf(stderr, "Roundtrip: data mismatch\n");
        ret = -1;
        goto cleanup;
    }

    printf("  Roundtrip: PASSED\n");
    ret = 0;

cleanup:
    av_packet_free(&pkt_orig);
    av_packet_free(&pkt_ts);
    av_packet_free(&pkt_back);
    av_bsf_free(&to_ts_ctx);
    av_bsf_free(&from_ts_ctx);
    return ret;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int failed = 0;

    printf("=== AV1 TS BSF Memory Test ===\n\n");

    if (test_to_ts_mode() < 0)
        failed++;

    if (test_from_ts_mode() < 0)
        failed++;

    if (test_escape_handling() < 0)
        failed++;

    if (test_roundtrip() < 0)
        failed++;

    printf("\n=== Results: %d tests, %d failed ===\n",
           4, failed);

    return failed > 0 ? 1 : 0;
}
