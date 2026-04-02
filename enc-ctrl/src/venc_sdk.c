#include "venc_sdk_if.h"

#include <stdlib.h>
#include <string.h>

/*
 * SDK interface implementation.
 *
 * Real SDK calls go through the MI_VENC API declared in star6e.h.
 * For off-target (host) builds, a mock implementation is used.
 *
 * SDK capability findings (from i6c_venc.h / sigmastar_types.h):
 *
 * Available per-frame from MI_VENC_GetStream (i6c_venc_strm):
 *   - packet[].length         → frame size (sum of all packs)
 *   - packet[].timestamp      → capture timestamp
 *   - packet[].naluType       → frame type (I-slice vs P-slice)
 *   - h264Info.startQual      → starting QP (closest to avg QP available)
 *   - h264Info.size            → encoded frame size
 *   - h265Info.startQual      → same for HEVC
 *   - sequence                 → frame sequence number
 *
 * Available from MI_VENC_Query (i6c_venc_stat):
 *   - bitrate, fpsNum, fpsDen, curPacks, leftPics, etc.
 *   - Useful for channel-level stats, not per-frame.
 *
 * Available for control:
 *   - MI_VENC_RequestIdr(chn, instant) → IDR insertion
 *   - MI_VENC_SetChnAttr(chn, attr)   → bitrate, GOP changes
 *   - MI_VENC_SetRcParam(chn, param)  → QP range (Star6E only)
 *
 * NOT directly available:
 *   - Per-frame average QP (startQual is close but not exact)
 *   - Per-frame max QP
 *   - SAD / macroblock variance / intra-inter ratio
 *   - Encode latency (must be measured externally)
 */

struct VencSdkContext {
	int      chn;
	int      codec;     /* 0 = H.264, 1 = H.265 */
	uint8_t  is_mock;
	uint8_t  _pad[3];
};

/* ── Mock implementation ────────────────────────────────────────────── */

static int mock_request_idr(VencSdkContext *ctx, int instant)
{
	(void)ctx;
	(void)instant;
	return 0;
}

static int mock_set_bitrate(VencSdkContext *ctx, uint32_t bitrate_bps)
{
	(void)ctx;
	(void)bitrate_bps;
	return 0;
}

static int mock_set_qp_range(VencSdkContext *ctx, uint8_t qp_min,
	uint8_t qp_max)
{
	(void)ctx;
	(void)qp_min;
	(void)qp_max;
	return 0;
}

static int mock_get_qp_range(VencSdkContext *ctx, uint8_t *qp_min,
	uint8_t *qp_max)
{
	(void)ctx;
	if (qp_min) *qp_min = 10;
	if (qp_max) *qp_max = 48;
	return 0;
}

static int mock_extract_frame_stats(VencSdkContext *ctx,
	const void *stream, uint32_t frame_seq,
	EncoderFrameStats *out)
{
	(void)ctx;
	(void)stream;

	if (!out)
		return -1;

	memset(out, 0, sizeof(*out));
	out->frame_seq = frame_seq;
	return 0;
}

static const VencSdkOps g_mock_ops = {
	.request_idr = mock_request_idr,
	.set_bitrate = mock_set_bitrate,
	.set_qp_range = mock_set_qp_range,
	.get_qp_range = mock_get_qp_range,
	.extract_frame_stats = mock_extract_frame_stats,
};

/* ── Real SDK implementation (cross-compiled with MI_VENC) ──────────── */

#if defined(PLATFORM_STAR6E) || defined(PLATFORM_MARUKO)

#include "star6e.h"

#include <stdio.h>

static int real_request_idr(VencSdkContext *ctx, int instant)
{
	if (!ctx)
		return -1;
	return MI_VENC_RequestIdr(ctx->chn, instant ? 1 : 0);
}

static int real_set_bitrate(VencSdkContext *ctx, uint32_t bitrate_bps)
{
	MI_VENC_ChnAttr_t attr;
	int rc;

	if (!ctx)
		return -1;

	memset(&attr, 0, sizeof(attr));
	rc = MI_VENC_GetChnAttr(ctx->chn, &attr);
	if (rc != 0) {
		fprintf(stderr, "[enc_ctrl] GetChnAttr failed: %d\n", rc);
		return -1;
	}

	/* Update bitrate in the rate control union based on mode */
	switch (attr.rate.mode) {
	case 1:  /* H264 CBR */
		attr.rate.h264Cbr.bitrate = bitrate_bps / 1024;
		break;
	case 2:  /* H264 VBR */
		attr.rate.h264Vbr.maxBitrate = bitrate_bps / 1024;
		break;
	case 9:  /* H265 CBR */
		attr.rate.h265Cbr.bitrate = bitrate_bps / 1024;
		break;
	case 10: /* H265 VBR */
		attr.rate.h265Vbr.maxBitrate = bitrate_bps / 1024;
		break;
	default:
		fprintf(stderr, "[enc_ctrl] unsupported rate mode %d for bitrate set\n",
			attr.rate.mode);
		return -1;
	}

	rc = MI_VENC_SetChnAttr(ctx->chn, &attr);
	if (rc != 0) {
		fprintf(stderr, "[enc_ctrl] SetChnAttr failed: %d\n", rc);
		return -1;
	}

	return 0;
}

static int real_set_qp_range(VencSdkContext *ctx, uint8_t qp_min,
	uint8_t qp_max)
{
#if !defined(PLATFORM_MARUKO)
	MI_VENC_RcParam_t param;
	int rc;

	if (!ctx)
		return -1;

	memset(&param, 0, sizeof(param));
	rc = MI_VENC_GetRcParam(ctx->chn, &param);
	if (rc != 0)
		return -1;

	if (ctx->codec == 0) {
		param.stParamH264Cbr.u32MinQp = qp_min;
		param.stParamH264Cbr.u32MaxQp = qp_max;
		param.stParamH264Cbr.u32MinIQp = qp_min;
		param.stParamH264Cbr.u32MaxIQp = qp_max;
	} else {
		param.stParamH265Cbr.u32MinQp = qp_min;
		param.stParamH265Cbr.u32MaxQp = qp_max;
		param.stParamH265Cbr.u32MinIQp = qp_min;
		param.stParamH265Cbr.u32MaxIQp = qp_max;
	}

	return MI_VENC_SetRcParam(ctx->chn, &param);
#else
	(void)ctx;
	(void)qp_min;
	(void)qp_max;
	return -1; /* Maruko lacks MI_VENC_GetRcParam/SetRcParam */
#endif
}

static int real_get_qp_range(VencSdkContext *ctx, uint8_t *qp_min,
	uint8_t *qp_max)
{
#if !defined(PLATFORM_MARUKO)
	MI_VENC_RcParam_t param;
	int rc;

	if (!ctx)
		return -1;

	memset(&param, 0, sizeof(param));
	rc = MI_VENC_GetRcParam(ctx->chn, &param);
	if (rc != 0)
		return -1;

	if (ctx->codec == 0) {
		if (qp_min) *qp_min = (uint8_t)param.stParamH264Cbr.u32MinQp;
		if (qp_max) *qp_max = (uint8_t)param.stParamH264Cbr.u32MaxQp;
	} else {
		if (qp_min) *qp_min = (uint8_t)param.stParamH265Cbr.u32MinQp;
		if (qp_max) *qp_max = (uint8_t)param.stParamH265Cbr.u32MaxQp;
	}

	return 0;
#else
	(void)ctx;
	if (qp_min) *qp_min = 0;
	if (qp_max) *qp_max = 51;
	return -1;
#endif
}

static int real_extract_frame_stats(VencSdkContext *ctx,
	const void *stream_ptr, uint32_t frame_seq,
	EncoderFrameStats *out)
{
	const MI_VENC_Stream_t *stream = (const MI_VENC_Stream_t *)stream_ptr;
	uint32_t total_size = 0;
	uint8_t frame_type = ENC_FRAME_P;
	uint8_t qp = 0;
	unsigned int i;

	if (!ctx || !stream || !out)
		return -1;

	memset(out, 0, sizeof(*out));
	out->frame_seq = frame_seq;

	/* Sum pack lengths for total frame size */
	for (i = 0; i < stream->count; i++) {
		if (stream->packet)
			total_size += stream->packet[i].length;
	}
	out->frame_size_bytes = total_size;

	/* Timestamp from first pack */
	if (stream->count > 0 && stream->packet)
		out->timestamp_us = stream->packet[0].timestamp;

	/* Frame type from NAL type */
	if (stream->count > 0 && stream->packet) {
		if (ctx->codec == 0) {
			/* H.264: check naluType.h264Nalu */
			int nalu = stream->packet[0].naluType.h264Nalu;
			if (nalu == 5) /* I6C_VENC_NALU_H264_ISLICE */
				frame_type = ENC_FRAME_IDR;
			else if (nalu == 9) /* IPSLICE */
				frame_type = ENC_FRAME_I;
		} else {
			/* H.265: check naluType.h265Nalu */
			int nalu = stream->packet[0].naluType.h265Nalu;
			if (nalu == 19) /* I6C_VENC_NALU_H265_ISLICE */
				frame_type = ENC_FRAME_IDR;
		}
	}
	out->frame_type = frame_type;

	/* QP from stream info — startQual is the closest available */
	if (ctx->codec == 0)
		qp = (uint8_t)stream->h264Info.startQual;
	else
		qp = (uint8_t)stream->h265Info.startQual;

	out->qp_avg = qp;

	return 0;
}

static const VencSdkOps g_real_ops = {
	.request_idr = real_request_idr,
	.set_bitrate = real_set_bitrate,
	.set_qp_range = real_set_qp_range,
	.get_qp_range = real_get_qp_range,
	.extract_frame_stats = real_extract_frame_stats,
};

#endif /* PLATFORM_STAR6E || PLATFORM_MARUKO */

/* ── Public API ─────────────────────────────────────────────────────── */

VencSdkContext *venc_sdk_create(int chn, int codec)
{
	VencSdkContext *ctx = (VencSdkContext *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->chn = chn;
	ctx->codec = codec;
	ctx->is_mock = 0;
	return ctx;
}

const VencSdkOps *venc_sdk_ops(void)
{
#if defined(PLATFORM_STAR6E) || defined(PLATFORM_MARUKO)
	return &g_real_ops;
#else
	return &g_mock_ops;
#endif
}

void venc_sdk_destroy(VencSdkContext *ctx)
{
	free(ctx);
}

VencSdkContext *venc_sdk_create_mock(void)
{
	VencSdkContext *ctx = (VencSdkContext *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->is_mock = 1;
	return ctx;
}
