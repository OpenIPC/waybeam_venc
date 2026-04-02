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
	int                chn;
	int                codec;     /* 0 = H.264, 1 = H.265 */
	const VencSdkOps  *ops;      /* bound ops table */
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

	/* Update bitrate in the rate control union based on mode.
	 * Mode values from i6c_venc_ratemode enum. */
	{
		unsigned int kbps = bitrate_bps / 1024;

		switch (attr.rate.mode) {
		case 1:  /* H264 CBR */
			attr.rate.h264Cbr.bitrate = kbps;
			break;
		case 2:  /* H264 VBR */
			attr.rate.h264Vbr.maxBitrate = kbps;
			break;
		case 5:  /* H264 AVBR */
			attr.rate.h264Avbr.maxBitrate = kbps;
			break;
		case 9:  /* H265 CBR */
			attr.rate.h265Cbr.bitrate = kbps;
			break;
		case 10: /* H265 VBR */
			attr.rate.h265Vbr.maxBitrate = kbps;
			break;
		case 12: /* H265 AVBR */
			attr.rate.h265Avbr.maxBitrate = kbps;
			break;
		default:
			fprintf(stderr,
				"[enc_ctrl] unsupported rate mode %d for bitrate set\n",
				attr.rate.mode);
			return -1;
		}
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

static uint8_t detect_frame_type(const MI_VENC_Stream_t *stream, int codec)
{
	unsigned int i;

	if (!stream->packet)
		return ENC_FRAME_P;

	/* Iterate all packs and their packetInfo[] entries to find
	 * the slice NAL.  The first pack is often SPS/PPS, not the
	 * slice itself — checking only packet[0].naluType would
	 * misclassify most I-frames. */
	for (i = 0; i < stream->count; i++) {
		const MI_VENC_Pack_t *pack = &stream->packet[i];
		unsigned int k;
		unsigned int nal_count = pack->packNum;

		if (nal_count > 8)
			nal_count = 8;

		if (nal_count > 0) {
			for (k = 0; k < nal_count; k++) {
				if (codec == 0) {
					int nt = pack->packetInfo[k].packType.h264Nalu;
					if (nt == 5) /* ISLICE */
						return ENC_FRAME_IDR;
					if (nt == 9) /* IPSLICE */
						return ENC_FRAME_I;
				} else {
					int nt = pack->packetInfo[k].packType.h265Nalu;
					if (nt == 19) /* ISLICE / IDR_W_RADL */
						return ENC_FRAME_IDR;
				}
			}
		} else {
			/* Fallback: single-NAL pack, use top-level naluType */
			if (codec == 0) {
				int nt = pack->naluType.h264Nalu;
				if (nt == 5)
					return ENC_FRAME_IDR;
				if (nt == 9)
					return ENC_FRAME_I;
			} else {
				int nt = pack->naluType.h265Nalu;
				if (nt == 19)
					return ENC_FRAME_IDR;
			}
		}
	}

	return ENC_FRAME_P;
}

static int real_extract_frame_stats(VencSdkContext *ctx,
	const void *stream_ptr, uint32_t frame_seq,
	EncoderFrameStats *out)
{
	const MI_VENC_Stream_t *stream = (const MI_VENC_Stream_t *)stream_ptr;
	uint32_t total_size = 0;
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

	/* Frame type: iterate packs/packetInfo to find slice NAL */
	out->frame_type = detect_frame_type(stream, ctx->codec);

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
#if defined(PLATFORM_STAR6E) || defined(PLATFORM_MARUKO)
	ctx->ops = &g_real_ops;
#else
	ctx->ops = &g_mock_ops;
#endif
	return ctx;
}

const VencSdkOps *venc_sdk_get_ops(const VencSdkContext *ctx)
{
	if (!ctx)
		return &g_mock_ops;
	return ctx->ops;
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

	ctx->ops = &g_mock_ops;
	return ctx;
}
