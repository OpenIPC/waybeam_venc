#ifndef VENC_SDK_IF_H
#define VENC_SDK_IF_H

/*
 * MI_VENC SDK interface — all SDK calls isolated here.
 * Nothing else in enc-ctrl touches MI_VENC directly.
 * This makes it possible to mock the SDK for off-target testing.
 */

#include "enc_types.h"

#include <stdint.h>

/* Opaque SDK context (hides MI_VENC types from callers) */
typedef struct VencSdkContext VencSdkContext;

/* SDK operations — function pointers for mockability */
typedef struct {
	/** Request IDR insertion. Returns 0 on success. */
	int (*request_idr)(VencSdkContext *ctx, int instant);

	/** Set bitrate in bps. Returns 0 on success. */
	int (*set_bitrate)(VencSdkContext *ctx, uint32_t bitrate_bps);

	/** Set QP range. Returns 0 on success. */
	int (*set_qp_range)(VencSdkContext *ctx, uint8_t qp_min, uint8_t qp_max);

	/** Get current QP range. Returns 0 on success. */
	int (*get_qp_range)(VencSdkContext *ctx, uint8_t *qp_min, uint8_t *qp_max);

	/** Extract per-frame stats from SDK stream data.
	 *  Called from the encoder output loop after MI_VENC_GetStream.
	 *  Populates stats from available SDK fields + NAL parsing fallbacks. */
	int (*extract_frame_stats)(VencSdkContext *ctx,
		const void *stream, uint32_t frame_seq,
		EncoderFrameStats *out);
} VencSdkOps;

/** Create real SDK context for a given VENC channel.
 *  chn: MI_VENC channel index.
 *  codec: 0 = H.264, 1 = H.265.
 *  Returns NULL on failure. */
VencSdkContext *venc_sdk_create(int chn, int codec);

/** Get the operations table for a real SDK context. */
const VencSdkOps *venc_sdk_ops(void);

/** Destroy SDK context. */
void venc_sdk_destroy(VencSdkContext *ctx);

/** Create a mock SDK context for testing.
 *  All operations are no-ops that return success. */
VencSdkContext *venc_sdk_create_mock(void);

#endif /* VENC_SDK_IF_H */
