#ifndef STAR6E_VIDEO_H
#define STAR6E_VIDEO_H

#include "h26x_param_sets.h"
#include "rtp_packetizer.h"
#include "rtp_sidecar.h"
#include "star6e.h"
#include "star6e_hevc_rtp.h"
#include "star6e_output.h"
#include "stream_metrics.h"
#include "venc_config.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
	RtpPacketizerState rtp_state;
	uint32_t rtp_frame_ticks;
	H26xParamSets param_sets;
	uint32_t sensor_framerate;
	uint16_t max_frame_size;
	uint16_t rtp_payload_size;
	unsigned int frame_counter;
	StreamMetricsState verbose_metrics;
	Star6eHevcRtpStats verbose_packetizer_interval;
	RtpSidecarSender sidecar;
} Star6eVideoState;

/** Reset video state to uninitialized (safe to reuse). */
void star6e_video_reset(Star6eVideoState *state);

/** Initialize video RTP state and payload adaptation. */
void star6e_video_init(Star6eVideoState *state, const VencConfig *vcfg,
	uint32_t sensor_framerate, const Star6eOutput *output);

/** Send one encoded frame via configured output mode. */
size_t star6e_video_send_frame(Star6eVideoState *state,
	Star6eOutput *output, const MI_VENC_Stream_t *stream,
	int output_enabled, int verbose_enabled,
	const RtpSidecarEncInfo *enc_info);

#endif /* STAR6E_VIDEO_H */
