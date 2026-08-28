#ifndef MARUKO_VIDEO_H
#define MARUKO_VIDEO_H

#include "h26x_param_sets.h"
#include "hevc_rtp.h"
#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_output.h"
#include "rtp_session.h"

#include <stddef.h>

/* SVC-T per-frame reference type (MI_VENC_H264eRefType_e, H265 aliases it):
 * E_MI_VENC_ENHANCE_P_NOTFORREF = 5 — the droppable top enhance-layer frame.
 * The SigmaStar enum inserts E_MI_VENC_BASE_P_REFTOIDR at index 1, so this is
 * 5, NOT 4 (value 4 is ENHANCE_P_REFBYENHANCE — referenced, non-droppable).
 * i6c shares the i6e enum. See star6e.h for the device-verified rationale. */
#define MARUKO_REFTYPE_ENHANCE_P_NOTFORREF 5

typedef RtpSessionState MarukoRtpState;

/** Initialize RTP session state for Maruko video output. */
void maruko_video_init_rtp_state(MarukoRtpState *rtp,
	PAYLOAD_TYPE_E codec, uint32_t sensor_fps);

/** Return non-zero when every vendor pack exposes descriptors for all of its
 * NAL units.  The i6c ABI has a fixed-size packetInfo table; callers must drop
 * the whole access unit rather than silently emit only the table prefix. */
int maruko_video_stream_packet_info_complete(const i6c_venc_strm *stream);

/** Flatten a Maruko VENC stream's NAL payload into ONE malloc'd Annex-B
 *  access unit.  The i6c twin of star6e_output_stream_flatten(); see that
 *  declaration for the contract.  Caller (or the writer queue it is handed
 *  to) owns and frees the buffer. */
uint8_t *maruko_video_stream_flatten(const i6c_venc_strm *stream,
	size_t *out_len, int *out_is_idr);

/** Reject an incomplete access unit before any consumer sees it. Returns 1
 * when rejected, emits a one-time warning, and requests paced recovery when
 * the dropped picture participates in the reference chain. */
int maruko_video_reject_incomplete_access_unit(const i6c_venc_strm *stream,
	MarukoOutput *output);

/** Packetize and send one encoder frame over RTP.
 *  If stats is non-NULL and the stream is H.265 in RTP mode, per-frame
 *  packetizer counters are accumulated into *stats. */
size_t maruko_video_send_frame(const i6c_venc_strm *stream,
	MarukoOutput *output, MarukoRtpState *rtp,
	H26xParamSets *params, MarukoBackendConfig *cfg, HevcRtpStats *stats);

#endif /* MARUKO_VIDEO_H */
