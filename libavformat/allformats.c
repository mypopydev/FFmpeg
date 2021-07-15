/*
 * Register all the formats and protocols
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
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

#include "libavutil/thread.h"
#include "libavformat/internal.h"
#include "avformat.h"

/* (de)muxers */
extern const AVOutputFormat ff_a64_muxer;
extern const AVInputFormat  ff_aa_demuxer;
extern const AVInputFormat  ff_aac_demuxer;
extern const AVInputFormat  ff_aax_demuxer;
extern const AVInputFormat  ff_ac3_demuxer;
extern const AVOutputFormat ff_ac3_muxer;
extern const AVInputFormat  ff_ace_demuxer;
extern const AVInputFormat  ff_acm_demuxer;
extern const AVInputFormat  ff_act_demuxer;
extern const AVInputFormat  ff_adf_demuxer;
extern const AVInputFormat  ff_adp_demuxer;
extern const AVInputFormat  ff_ads_demuxer;
extern const AVOutputFormat ff_adts_muxer;
extern const AVInputFormat  ff_adx_demuxer;
extern const AVOutputFormat ff_adx_muxer;
extern const AVInputFormat  ff_aea_demuxer;
extern const AVInputFormat  ff_afc_demuxer;
extern const AVInputFormat  ff_aiff_demuxer;
extern const AVOutputFormat ff_aiff_muxer;
extern const AVInputFormat  ff_aix_demuxer;
extern const AVInputFormat  ff_alp_demuxer;
extern const AVOutputFormat ff_alp_muxer;
extern const AVInputFormat  ff_amr_demuxer;
extern const AVOutputFormat ff_amr_muxer;
extern const AVInputFormat  ff_amrnb_demuxer;
extern const AVInputFormat  ff_amrwb_demuxer;
extern const AVOutputFormat ff_amv_muxer;
extern const AVInputFormat  ff_anm_demuxer;
extern const AVInputFormat  ff_apc_demuxer;
extern const AVInputFormat  ff_ape_demuxer;
extern const AVInputFormat  ff_apm_demuxer;
extern const AVOutputFormat ff_apm_muxer;
extern const AVInputFormat  ff_apng_demuxer;
extern const AVOutputFormat ff_apng_muxer;
extern const AVInputFormat  ff_aptx_demuxer;
extern const AVOutputFormat ff_aptx_muxer;
extern const AVInputFormat  ff_aptx_hd_demuxer;
extern const AVOutputFormat ff_aptx_hd_muxer;
extern const AVInputFormat  ff_aqtitle_demuxer;
extern const AVInputFormat  ff_argo_asf_demuxer;
extern const AVOutputFormat ff_argo_asf_muxer;
extern const AVInputFormat  ff_argo_brp_demuxer;
extern const AVInputFormat  ff_argo_cvg_demuxer;
extern const AVOutputFormat ff_argo_cvg_muxer;
extern const AVInputFormat  ff_asf_demuxer;
extern const AVOutputFormat ff_asf_muxer;
extern const AVInputFormat  ff_asf_o_demuxer;
extern const AVInputFormat  ff_ass_demuxer;
extern const AVOutputFormat ff_ass_muxer;
extern const AVInputFormat  ff_ast_demuxer;
extern const AVOutputFormat ff_ast_muxer;
extern const AVOutputFormat ff_asf_stream_muxer;
extern const AVInputFormat  ff_au_demuxer;
extern const AVOutputFormat ff_au_muxer;
extern const AVInputFormat  ff_av1_demuxer;
extern const AVInputFormat  ff_avi_demuxer;
extern const AVOutputFormat ff_avi_muxer;
extern const AVInputFormat  ff_avisynth_demuxer;
extern const AVOutputFormat ff_avm2_muxer;
extern const AVInputFormat  ff_avr_demuxer;
extern const AVInputFormat  ff_avs_demuxer;
extern const AVInputFormat  ff_avs2_demuxer;
extern const AVOutputFormat ff_avs2_muxer;
extern const AVInputFormat  ff_avs3_demuxer;
extern const AVInputFormat  ff_bethsoftvid_demuxer;
extern const AVInputFormat  ff_bfi_demuxer;
extern const AVInputFormat  ff_bintext_demuxer;
extern const AVInputFormat  ff_bink_demuxer;
extern const AVInputFormat  ff_binka_demuxer;
extern const AVInputFormat  ff_bit_demuxer;
extern const AVOutputFormat ff_bit_muxer;
extern const AVInputFormat  ff_bmv_demuxer;
extern const AVInputFormat  ff_bfstm_demuxer;
extern const AVInputFormat  ff_brstm_demuxer;
extern const AVInputFormat  ff_boa_demuxer;
extern const AVInputFormat  ff_c93_demuxer;
extern const AVInputFormat  ff_caf_demuxer;
extern const AVOutputFormat ff_caf_muxer;
extern const AVInputFormat  ff_cavsvideo_demuxer;
extern const AVOutputFormat ff_cavsvideo_muxer;
extern const AVInputFormat  ff_cdg_demuxer;
extern const AVInputFormat  ff_cdxl_demuxer;
extern const AVInputFormat  ff_cine_demuxer;
extern const AVInputFormat  ff_codec2_demuxer;
extern const AVOutputFormat ff_codec2_muxer;
extern const AVInputFormat  ff_codec2raw_demuxer;
extern const AVOutputFormat ff_codec2raw_muxer;
extern const AVInputFormat  ff_concat_demuxer;
extern const AVOutputFormat ff_crc_muxer;
extern const AVInputFormat  ff_dash_demuxer;
extern const AVOutputFormat ff_dash_muxer;
extern const AVInputFormat  ff_data_demuxer;
extern const AVOutputFormat ff_data_muxer;
extern const AVInputFormat  ff_daud_demuxer;
extern const AVOutputFormat ff_daud_muxer;
extern const AVInputFormat  ff_dcstr_demuxer;
extern const AVInputFormat  ff_derf_demuxer;
extern const AVInputFormat  ff_dfa_demuxer;
extern const AVInputFormat  ff_dhav_demuxer;
extern const AVInputFormat  ff_dirac_demuxer;
extern const AVOutputFormat ff_dirac_muxer;
extern const AVInputFormat  ff_dnxhd_demuxer;
extern const AVOutputFormat ff_dnxhd_muxer;
extern const AVInputFormat  ff_dsf_demuxer;
extern const AVInputFormat  ff_dsicin_demuxer;
extern const AVInputFormat  ff_dss_demuxer;
extern const AVInputFormat  ff_dts_demuxer;
extern const AVOutputFormat ff_dts_muxer;
extern const AVInputFormat  ff_dtshd_demuxer;
extern const AVInputFormat  ff_dv_demuxer;
extern const AVOutputFormat ff_dv_muxer;
extern const AVInputFormat  ff_dvbsub_demuxer;
extern const AVInputFormat  ff_dvbtxt_demuxer;
extern const AVInputFormat  ff_dxa_demuxer;
extern const AVInputFormat  ff_ea_demuxer;
extern const AVInputFormat  ff_ea_cdata_demuxer;
extern const AVInputFormat  ff_eac3_demuxer;
extern const AVOutputFormat ff_eac3_muxer;
extern const AVInputFormat  ff_epaf_demuxer;
extern const AVOutputFormat ff_f4v_muxer;
extern const AVInputFormat  ff_ffmetadata_demuxer;
extern const AVOutputFormat ff_ffmetadata_muxer;
extern const AVOutputFormat ff_fifo_muxer;
extern const AVOutputFormat ff_fifo_test_muxer;
extern const AVInputFormat  ff_filmstrip_demuxer;
extern const AVOutputFormat ff_filmstrip_muxer;
extern const AVInputFormat  ff_fits_demuxer;
extern const AVOutputFormat ff_fits_muxer;
extern const AVInputFormat  ff_flac_demuxer;
extern const AVOutputFormat ff_flac_muxer;
extern const AVInputFormat  ff_flic_demuxer;
extern const AVInputFormat  ff_flv_demuxer;
extern const AVOutputFormat ff_flv_muxer;
extern const AVInputFormat  ff_live_flv_demuxer;
extern const AVInputFormat  ff_fourxm_demuxer;
extern const AVOutputFormat ff_framecrc_muxer;
extern const AVOutputFormat ff_framehash_muxer;
extern const AVOutputFormat ff_framemd5_muxer;
extern const AVInputFormat  ff_frm_demuxer;
extern const AVInputFormat  ff_fsb_demuxer;
extern const AVInputFormat  ff_fwse_demuxer;
extern const AVInputFormat  ff_g722_demuxer;
extern const AVOutputFormat ff_g722_muxer;
extern const AVInputFormat  ff_g723_1_demuxer;
extern const AVOutputFormat ff_g723_1_muxer;
extern const AVInputFormat  ff_g726_demuxer;
extern const AVOutputFormat ff_g726_muxer;
extern const AVInputFormat  ff_g726le_demuxer;
extern const AVOutputFormat ff_g726le_muxer;
extern const AVInputFormat  ff_g729_demuxer;
extern const AVInputFormat  ff_gdv_demuxer;
extern const AVInputFormat  ff_genh_demuxer;
extern const AVInputFormat  ff_gif_demuxer;
extern const AVOutputFormat ff_gif_muxer;
extern const AVInputFormat  ff_gsm_demuxer;
extern const AVOutputFormat ff_gsm_muxer;
extern const AVInputFormat  ff_gxf_demuxer;
extern const AVOutputFormat ff_gxf_muxer;
extern const AVInputFormat  ff_h261_demuxer;
extern const AVOutputFormat ff_h261_muxer;
extern const AVInputFormat  ff_h263_demuxer;
extern const AVOutputFormat ff_h263_muxer;
extern const AVInputFormat  ff_h264_demuxer;
extern const AVOutputFormat ff_h264_muxer;
extern const AVOutputFormat ff_hash_muxer;
extern const AVInputFormat  ff_hca_demuxer;
extern const AVInputFormat  ff_hcom_demuxer;
extern const AVOutputFormat ff_hds_muxer;
extern const AVInputFormat  ff_hevc_demuxer;
extern const AVOutputFormat ff_hevc_muxer;
extern const AVInputFormat  ff_hls_demuxer;
extern const AVOutputFormat ff_hls_muxer;
extern const AVInputFormat  ff_hnm_demuxer;
extern const AVInputFormat  ff_ico_demuxer;
extern const AVOutputFormat ff_ico_muxer;
extern const AVInputFormat  ff_idcin_demuxer;
extern const AVInputFormat  ff_idf_demuxer;
extern const AVInputFormat  ff_iff_demuxer;
extern const AVInputFormat  ff_ifv_demuxer;
extern const AVInputFormat  ff_ilbc_demuxer;
extern const AVOutputFormat ff_ilbc_muxer;
extern const AVInputFormat  ff_image2_demuxer;
extern const AVOutputFormat ff_image2_muxer;
extern const AVInputFormat  ff_image2pipe_demuxer;
extern const AVOutputFormat ff_image2pipe_muxer;
extern const AVInputFormat  ff_image2_alias_pix_demuxer;
extern const AVInputFormat  ff_image2_brender_pix_demuxer;
extern const AVInputFormat  ff_ingenient_demuxer;
extern const AVInputFormat  ff_ipmovie_demuxer;
extern const AVOutputFormat ff_ipod_muxer;
extern const AVInputFormat  ff_ipu_demuxer;
extern const AVInputFormat  ff_ircam_demuxer;
extern const AVOutputFormat ff_ircam_muxer;
extern const AVOutputFormat ff_ismv_muxer;
extern const AVInputFormat  ff_iss_demuxer;
extern const AVInputFormat  ff_iv8_demuxer;
extern const AVInputFormat  ff_ivf_demuxer;
extern const AVOutputFormat ff_ivf_muxer;
extern const AVInputFormat  ff_ivr_demuxer;
extern const AVInputFormat  ff_jacosub_demuxer;
extern const AVOutputFormat ff_jacosub_muxer;
extern const AVInputFormat  ff_jv_demuxer;
extern const AVInputFormat  ff_kux_demuxer;
extern const AVInputFormat  ff_kvag_demuxer;
extern const AVOutputFormat ff_kvag_muxer;
extern const AVOutputFormat ff_latm_muxer;
extern const AVInputFormat  ff_lmlm4_demuxer;
extern const AVInputFormat  ff_loas_demuxer;
extern const AVInputFormat  ff_luodat_demuxer;
extern const AVInputFormat  ff_lrc_demuxer;
extern const AVOutputFormat ff_lrc_muxer;
extern const AVInputFormat  ff_lvf_demuxer;
extern const AVInputFormat  ff_lxf_demuxer;
extern const AVInputFormat  ff_m4v_demuxer;
extern const AVOutputFormat ff_m4v_muxer;
extern const AVInputFormat  ff_mca_demuxer;
extern const AVInputFormat  ff_mcc_demuxer;
extern const AVOutputFormat ff_md5_muxer;
extern const AVInputFormat  ff_matroska_demuxer;
extern const AVOutputFormat ff_matroska_muxer;
extern const AVOutputFormat ff_matroska_audio_muxer;
extern const AVInputFormat  ff_mgsts_demuxer;
extern const AVInputFormat  ff_microdvd_demuxer;
extern const AVOutputFormat ff_microdvd_muxer;
extern const AVInputFormat  ff_mjpeg_demuxer;
extern const AVOutputFormat ff_mjpeg_muxer;
extern const AVInputFormat  ff_mjpeg_2000_demuxer;
extern const AVInputFormat  ff_mlp_demuxer;
extern const AVOutputFormat ff_mlp_muxer;
extern const AVInputFormat  ff_mlv_demuxer;
extern const AVInputFormat  ff_mm_demuxer;
extern const AVInputFormat  ff_mmf_demuxer;
extern const AVOutputFormat ff_mmf_muxer;
extern const AVInputFormat  ff_mods_demuxer;
extern const AVInputFormat  ff_moflex_demuxer;
extern const AVInputFormat  ff_mov_demuxer;
extern const AVOutputFormat ff_mov_muxer;
extern const AVOutputFormat ff_mp2_muxer;
extern const AVInputFormat  ff_mp3_demuxer;
extern const AVOutputFormat ff_mp3_muxer;
extern const AVOutputFormat ff_mp4_muxer;
extern const AVInputFormat  ff_mpc_demuxer;
extern const AVInputFormat  ff_mpc8_demuxer;
extern const AVOutputFormat ff_mpeg1system_muxer;
extern const AVOutputFormat ff_mpeg1vcd_muxer;
extern const AVOutputFormat ff_mpeg1video_muxer;
extern const AVOutputFormat ff_mpeg2dvd_muxer;
extern const AVOutputFormat ff_mpeg2svcd_muxer;
extern const AVOutputFormat ff_mpeg2video_muxer;
extern const AVOutputFormat ff_mpeg2vob_muxer;
extern const AVInputFormat  ff_mpegps_demuxer;
extern const AVInputFormat  ff_mpegts_demuxer;
extern const AVOutputFormat ff_mpegts_muxer;
extern const AVInputFormat  ff_mpegtsraw_demuxer;
extern const AVInputFormat  ff_mpegvideo_demuxer;
extern const AVInputFormat  ff_mpjpeg_demuxer;
extern const AVOutputFormat ff_mpjpeg_muxer;
extern const AVInputFormat  ff_mpl2_demuxer;
extern const AVInputFormat  ff_mpsub_demuxer;
extern const AVInputFormat  ff_msf_demuxer;
extern const AVInputFormat  ff_msnwc_tcp_demuxer;
extern const AVInputFormat  ff_msp_demuxer;
extern const AVInputFormat  ff_mtaf_demuxer;
extern const AVInputFormat  ff_mtv_demuxer;
extern const AVInputFormat  ff_musx_demuxer;
extern const AVInputFormat  ff_mv_demuxer;
extern const AVInputFormat  ff_mvi_demuxer;
extern const AVInputFormat  ff_mxf_demuxer;
extern const AVOutputFormat ff_mxf_muxer;
extern const AVOutputFormat ff_mxf_d10_muxer;
extern const AVOutputFormat ff_mxf_opatom_muxer;
extern const AVInputFormat  ff_mxg_demuxer;
extern const AVInputFormat  ff_nc_demuxer;
extern const AVInputFormat  ff_nistsphere_demuxer;
extern const AVInputFormat  ff_nsp_demuxer;
extern const AVInputFormat  ff_nsv_demuxer;
extern const AVOutputFormat ff_null_muxer;
extern const AVInputFormat  ff_nut_demuxer;
extern const AVOutputFormat ff_nut_muxer;
extern const AVInputFormat  ff_nuv_demuxer;
extern const AVInputFormat  ff_obu_demuxer;
extern const AVOutputFormat ff_oga_muxer;
extern const AVInputFormat  ff_ogg_demuxer;
extern const AVOutputFormat ff_ogg_muxer;
extern const AVOutputFormat ff_ogv_muxer;
extern const AVInputFormat  ff_oma_demuxer;
extern const AVOutputFormat ff_oma_muxer;
extern const AVOutputFormat ff_opus_muxer;
extern const AVInputFormat  ff_paf_demuxer;
extern const AVInputFormat  ff_pcm_alaw_demuxer;
extern const AVOutputFormat ff_pcm_alaw_muxer;
extern const AVInputFormat  ff_pcm_mulaw_demuxer;
extern const AVOutputFormat ff_pcm_mulaw_muxer;
extern const AVInputFormat  ff_pcm_vidc_demuxer;
extern const AVOutputFormat ff_pcm_vidc_muxer;
extern const AVInputFormat  ff_pcm_f64be_demuxer;
extern const AVOutputFormat ff_pcm_f64be_muxer;
extern const AVInputFormat  ff_pcm_f64le_demuxer;
extern const AVOutputFormat ff_pcm_f64le_muxer;
extern const AVInputFormat  ff_pcm_f32be_demuxer;
extern const AVOutputFormat ff_pcm_f32be_muxer;
extern const AVInputFormat  ff_pcm_f32le_demuxer;
extern const AVOutputFormat ff_pcm_f32le_muxer;
extern const AVInputFormat  ff_pcm_s32be_demuxer;
extern const AVOutputFormat ff_pcm_s32be_muxer;
extern const AVInputFormat  ff_pcm_s32le_demuxer;
extern const AVOutputFormat ff_pcm_s32le_muxer;
extern const AVInputFormat  ff_pcm_s24be_demuxer;
extern const AVOutputFormat ff_pcm_s24be_muxer;
extern const AVInputFormat  ff_pcm_s24le_demuxer;
extern const AVOutputFormat ff_pcm_s24le_muxer;
extern const AVInputFormat  ff_pcm_s16be_demuxer;
extern const AVOutputFormat ff_pcm_s16be_muxer;
extern const AVInputFormat  ff_pcm_s16le_demuxer;
extern const AVOutputFormat ff_pcm_s16le_muxer;
extern const AVInputFormat  ff_pcm_s8_demuxer;
extern const AVOutputFormat ff_pcm_s8_muxer;
extern const AVInputFormat  ff_pcm_u32be_demuxer;
extern const AVOutputFormat ff_pcm_u32be_muxer;
extern const AVInputFormat  ff_pcm_u32le_demuxer;
extern const AVOutputFormat ff_pcm_u32le_muxer;
extern const AVInputFormat  ff_pcm_u24be_demuxer;
extern const AVOutputFormat ff_pcm_u24be_muxer;
extern const AVInputFormat  ff_pcm_u24le_demuxer;
extern const AVOutputFormat ff_pcm_u24le_muxer;
extern const AVInputFormat  ff_pcm_u16be_demuxer;
extern const AVOutputFormat ff_pcm_u16be_muxer;
extern const AVInputFormat  ff_pcm_u16le_demuxer;
extern const AVOutputFormat ff_pcm_u16le_muxer;
extern const AVInputFormat  ff_pcm_u8_demuxer;
extern const AVOutputFormat ff_pcm_u8_muxer;
extern const AVInputFormat  ff_pjs_demuxer;
extern const AVInputFormat  ff_pmp_demuxer;
extern const AVInputFormat  ff_pp_bnk_demuxer;
extern const AVOutputFormat ff_psp_muxer;
extern const AVInputFormat  ff_pva_demuxer;
extern const AVInputFormat  ff_pvf_demuxer;
extern const AVInputFormat  ff_qcp_demuxer;
extern const AVInputFormat  ff_r3d_demuxer;
extern const AVInputFormat  ff_rawvideo_demuxer;
extern const AVOutputFormat ff_rawvideo_muxer;
extern const AVInputFormat  ff_realtext_demuxer;
extern const AVInputFormat  ff_redspark_demuxer;
extern const AVInputFormat  ff_rl2_demuxer;
extern const AVInputFormat  ff_rm_demuxer;
extern const AVOutputFormat ff_rm_muxer;
extern const AVInputFormat  ff_roq_demuxer;
extern const AVOutputFormat ff_roq_muxer;
extern const AVInputFormat  ff_rpl_demuxer;
extern const AVInputFormat  ff_rsd_demuxer;
extern const AVInputFormat  ff_rso_demuxer;
extern const AVOutputFormat ff_rso_muxer;
extern const AVInputFormat  ff_rtp_demuxer;
extern const AVOutputFormat ff_rtp_muxer;
extern const AVOutputFormat ff_rtp_mpegts_muxer;
extern const AVInputFormat  ff_rtsp_demuxer;
extern const AVOutputFormat ff_rtsp_muxer;
extern const AVInputFormat  ff_s337m_demuxer;
extern const AVInputFormat  ff_sami_demuxer;
extern const AVInputFormat  ff_sap_demuxer;
extern const AVOutputFormat ff_sap_muxer;
extern const AVInputFormat  ff_sbc_demuxer;
extern const AVOutputFormat ff_sbc_muxer;
extern const AVInputFormat  ff_sbg_demuxer;
extern const AVInputFormat  ff_scc_demuxer;
extern const AVOutputFormat ff_scc_muxer;
extern const AVInputFormat  ff_sdp_demuxer;
extern const AVInputFormat  ff_sdr2_demuxer;
extern const AVInputFormat  ff_sds_demuxer;
extern const AVInputFormat  ff_sdx_demuxer;
extern const AVInputFormat  ff_segafilm_demuxer;
extern const AVOutputFormat ff_segafilm_muxer;
extern const AVOutputFormat ff_segment_muxer;
extern const AVOutputFormat ff_stream_segment_muxer;
extern const AVInputFormat  ff_ser_demuxer;
extern const AVInputFormat  ff_sga_demuxer;
extern const AVInputFormat  ff_shorten_demuxer;
extern const AVInputFormat  ff_siff_demuxer;
extern const AVInputFormat  ff_simbiosis_imx_demuxer;
extern const AVInputFormat  ff_sln_demuxer;
extern const AVInputFormat  ff_smacker_demuxer;
extern const AVInputFormat  ff_smjpeg_demuxer;
extern const AVOutputFormat ff_smjpeg_muxer;
extern const AVOutputFormat ff_smoothstreaming_muxer;
extern const AVInputFormat  ff_smush_demuxer;
extern const AVInputFormat  ff_sol_demuxer;
extern const AVInputFormat  ff_sox_demuxer;
extern const AVOutputFormat ff_sox_muxer;
extern const AVOutputFormat ff_spx_muxer;
extern const AVInputFormat  ff_spdif_demuxer;
extern const AVOutputFormat ff_spdif_muxer;
extern const AVInputFormat  ff_srt_demuxer;
extern const AVOutputFormat ff_srt_muxer;
extern const AVInputFormat  ff_str_demuxer;
extern const AVInputFormat  ff_stl_demuxer;
extern const AVOutputFormat ff_streamhash_muxer;
extern const AVInputFormat  ff_subviewer1_demuxer;
extern const AVInputFormat  ff_subviewer_demuxer;
extern const AVInputFormat  ff_sup_demuxer;
extern const AVOutputFormat ff_sup_muxer;
extern const AVInputFormat  ff_svag_demuxer;
extern const AVInputFormat  ff_svs_demuxer;
extern const AVInputFormat  ff_swf_demuxer;
extern const AVOutputFormat ff_swf_muxer;
extern const AVInputFormat  ff_tak_demuxer;
extern const AVOutputFormat ff_tee_muxer;
extern const AVInputFormat  ff_tedcaptions_demuxer;
extern const AVOutputFormat ff_tg2_muxer;
extern const AVOutputFormat ff_tgp_muxer;
extern const AVInputFormat  ff_thp_demuxer;
extern const AVInputFormat  ff_threedostr_demuxer;
extern const AVInputFormat  ff_tiertexseq_demuxer;
extern const AVOutputFormat ff_mkvtimestamp_v2_muxer;
extern const AVInputFormat  ff_tmv_demuxer;
extern const AVInputFormat  ff_truehd_demuxer;
extern const AVOutputFormat ff_truehd_muxer;
extern const AVInputFormat  ff_tta_demuxer;
extern const AVOutputFormat ff_tta_muxer;
extern const AVOutputFormat ff_ttml_muxer;
extern const AVInputFormat  ff_txd_demuxer;
extern const AVInputFormat  ff_tty_demuxer;
extern const AVInputFormat  ff_ty_demuxer;
extern const AVOutputFormat ff_uncodedframecrc_muxer;
extern const AVInputFormat  ff_v210_demuxer;
extern const AVInputFormat  ff_v210x_demuxer;
extern const AVInputFormat  ff_vag_demuxer;
extern const AVInputFormat  ff_vc1_demuxer;
extern const AVOutputFormat ff_vc1_muxer;
extern const AVInputFormat  ff_vc1t_demuxer;
extern const AVOutputFormat ff_vc1t_muxer;
extern const AVInputFormat  ff_vividas_demuxer;
extern const AVInputFormat  ff_vivo_demuxer;
extern const AVInputFormat  ff_vmd_demuxer;
extern const AVInputFormat  ff_vobsub_demuxer;
extern const AVInputFormat  ff_voc_demuxer;
extern const AVOutputFormat ff_voc_muxer;
extern const AVInputFormat  ff_vpk_demuxer;
extern const AVInputFormat  ff_vplayer_demuxer;
extern const AVInputFormat  ff_vqf_demuxer;
extern const AVInputFormat  ff_vvc_demuxer;
extern const AVOutputFormat ff_vvc_muxer;
extern const AVInputFormat  ff_w64_demuxer;
extern const AVOutputFormat ff_w64_muxer;
extern const AVInputFormat  ff_wav_demuxer;
extern const AVOutputFormat ff_wav_muxer;
extern const AVInputFormat  ff_wc3_demuxer;
extern const AVOutputFormat ff_webm_muxer;
extern const AVInputFormat  ff_webm_dash_manifest_demuxer;
extern const AVOutputFormat ff_webm_dash_manifest_muxer;
extern const AVOutputFormat ff_webm_chunk_muxer;
extern const AVOutputFormat ff_webp_muxer;
extern const AVInputFormat  ff_webvtt_demuxer;
extern const AVOutputFormat ff_webvtt_muxer;
extern const AVInputFormat  ff_wsaud_demuxer;
extern const AVOutputFormat ff_wsaud_muxer;
extern const AVInputFormat  ff_wsd_demuxer;
extern const AVInputFormat  ff_wsvqa_demuxer;
extern const AVInputFormat  ff_wtv_demuxer;
extern const AVOutputFormat ff_wtv_muxer;
extern const AVInputFormat  ff_wve_demuxer;
extern const AVInputFormat  ff_wv_demuxer;
extern const AVOutputFormat ff_wv_muxer;
extern const AVInputFormat  ff_xa_demuxer;
extern const AVInputFormat  ff_xbin_demuxer;
extern const AVInputFormat  ff_xmv_demuxer;
extern const AVInputFormat  ff_xvag_demuxer;
extern const AVInputFormat  ff_xwma_demuxer;
extern const AVInputFormat  ff_yop_demuxer;
extern const AVInputFormat  ff_yuv4mpegpipe_demuxer;
extern const AVOutputFormat ff_yuv4mpegpipe_muxer;
/* image demuxers */
extern const AVInputFormat  ff_image_bmp_pipe_demuxer;
extern const AVInputFormat  ff_image_cri_pipe_demuxer;
extern const AVInputFormat  ff_image_dds_pipe_demuxer;
extern const AVInputFormat  ff_image_dpx_pipe_demuxer;
extern const AVInputFormat  ff_image_exr_pipe_demuxer;
extern const AVInputFormat  ff_image_gif_pipe_demuxer;
extern const AVInputFormat  ff_image_j2k_pipe_demuxer;
extern const AVInputFormat  ff_image_jpeg_pipe_demuxer;
extern const AVInputFormat  ff_image_jpegls_pipe_demuxer;
extern const AVInputFormat  ff_image_pam_pipe_demuxer;
extern const AVInputFormat  ff_image_pbm_pipe_demuxer;
extern const AVInputFormat  ff_image_pcx_pipe_demuxer;
extern const AVInputFormat  ff_image_pgmyuv_pipe_demuxer;
extern const AVInputFormat  ff_image_pgm_pipe_demuxer;
extern const AVInputFormat  ff_image_pgx_pipe_demuxer;
extern const AVInputFormat  ff_image_photocd_pipe_demuxer;
extern const AVInputFormat  ff_image_pictor_pipe_demuxer;
extern const AVInputFormat  ff_image_png_pipe_demuxer;
extern const AVInputFormat  ff_image_ppm_pipe_demuxer;
extern const AVInputFormat  ff_image_psd_pipe_demuxer;
extern const AVInputFormat  ff_image_qdraw_pipe_demuxer;
extern const AVInputFormat  ff_image_sgi_pipe_demuxer;
extern const AVInputFormat  ff_image_svg_pipe_demuxer;
extern const AVInputFormat  ff_image_sunrast_pipe_demuxer;
extern const AVInputFormat  ff_image_tiff_pipe_demuxer;
extern const AVInputFormat  ff_image_webp_pipe_demuxer;
extern const AVInputFormat  ff_image_xbm_pipe_demuxer;
extern const AVInputFormat  ff_image_xpm_pipe_demuxer;
extern const AVInputFormat  ff_image_xwd_pipe_demuxer;

/* external libraries */
extern const AVOutputFormat ff_chromaprint_muxer;
extern const AVInputFormat  ff_libgme_demuxer;
extern const AVInputFormat  ff_libmodplug_demuxer;
extern const AVInputFormat  ff_libopenmpt_demuxer;
extern const AVInputFormat  ff_vapoursynth_demuxer;

#include "libavformat/muxer_list.c"
#include "libavformat/demuxer_list.c"

static const AVInputFormat * const *indev_list = NULL;
static const AVOutputFormat * const *outdev_list = NULL;

const AVOutputFormat *av_muxer_iterate(void **opaque)
{
    static const uintptr_t size = sizeof(muxer_list)/sizeof(muxer_list[0]) - 1;
    uintptr_t i = (uintptr_t)*opaque;
    const AVOutputFormat *f = NULL;

    if (i < size) {
        f = muxer_list[i];
    } else if (outdev_list) {
        f = outdev_list[i - size];
    }

    if (f)
        *opaque = (void*)(i + 1);
    return f;
}

const AVInputFormat *av_demuxer_iterate(void **opaque)
{
    static const uintptr_t size = sizeof(demuxer_list)/sizeof(demuxer_list[0]) - 1;
    uintptr_t i = (uintptr_t)*opaque;
    const AVInputFormat *f = NULL;

    if (i < size) {
        f = demuxer_list[i];
    } else if (indev_list) {
        f = indev_list[i - size];
    }

    if (f)
        *opaque = (void*)(i + 1);
    return f;
}

static AVMutex avpriv_register_devices_mutex = AV_MUTEX_INITIALIZER;

void avpriv_register_devices(const AVOutputFormat * const o[], const AVInputFormat * const i[])
{
    ff_mutex_lock(&avpriv_register_devices_mutex);
    outdev_list = o;
    indev_list = i;
    ff_mutex_unlock(&avpriv_register_devices_mutex);
}
