#ifndef MARUKO_VIDEO_H
#define MARUKO_VIDEO_H

#include "h26x_param_sets.h"
#include "hevc_rtp.h"
#include "maruko_bindings.h"
#include "maruko_config.h"
#include "maruko_output.h"
#include "rtp_session.h"

#include <stddef.h>

typedef RtpSessionState MarukoRtpState;

/** Initialize RTP session state for Maruko video output. */
void maruko_video_init_rtp_state(MarukoRtpState *rtp,
	PAYLOAD_TYPE_E codec, uint32_t sensor_fps);

/** Packetize and send one encoder frame over RTP.
 *  If stats is non-NULL and the stream is H.265 in RTP mode, per-frame
 *  packetizer counters are accumulated into *stats. */
size_t maruko_video_send_frame(const i6c_venc_strm *stream,
	const MarukoOutput *output, MarukoRtpState *rtp,
	H26xParamSets *params, MarukoBackendConfig *cfg, HevcRtpStats *stats);

#endif /* MARUKO_VIDEO_H */
