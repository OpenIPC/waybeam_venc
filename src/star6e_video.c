#include "star6e_video.h"
#include "star6e_audio.h"

#include "rtp_session.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_us(void)
{
	struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000ULL +
	       (uint64_t)(ts.tv_nsec / 1000);
}

typedef struct {
	RtpPacketizerState *rtp;
	uint32_t frame_ticks;
	H26xParamSets *params;
	size_t max_payload;
	Star6eHevcRtpStats *stats;
} Star6eRtpFrameContext;

static size_t send_frame_output_rtp(const Star6eOutput *output,
	const MI_VENC_Stream_t *stream, void *opaque)
{
	Star6eRtpFrameContext *ctx = opaque;

	if (!ctx)
		return 0;

	return star6e_hevc_rtp_send_frame(stream, output, ctx->rtp,
		ctx->frame_ticks, ctx->params, ctx->max_payload, ctx->stats);
}

void star6e_video_reset(Star6eVideoState *state)
{
	if (!state)
		return;

	rtp_sidecar_sender_close(&state->sidecar);
	memset(state, 0, sizeof(*state));
}

void star6e_video_init(Star6eVideoState *state, const VencConfig *vcfg,
	uint32_t sensor_framerate, const Star6eOutput *output)
{
	if (!state || !vcfg)
		return;

	memset(state, 0, sizeof(*state));
	state->sensor_framerate = sensor_framerate;
	state->max_frame_size = vcfg->outgoing.max_payload_size;
	state->rtp_payload_size = vcfg->outgoing.max_payload_size;

	if (output && star6e_output_is_rtp(output)) {
		RtpSessionState session;

		rtp_session_init(&session, rtp_session_payload_type(PT_H265),
			sensor_framerate);
		state->rtp_state.seq = session.seq;
		state->rtp_state.timestamp = session.timestamp;
		state->rtp_state.ssrc = session.ssrc;
		state->rtp_state.payload_type = session.payload_type;
		state->rtp_frame_ticks = session.frame_ticks;
	}

	if (vcfg->system.verbose) {
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		stream_metrics_start(&state->verbose_metrics, &now);
	}

	rtp_sidecar_sender_init(&state->sidecar, vcfg->outgoing.sidecar_port);
}

size_t star6e_video_send_frame(Star6eVideoState *state,
	const Star6eOutput *output, const MI_VENC_Stream_t *stream,
	int output_enabled, int verbose_enabled)
{
	size_t total_bytes = 0;
	Star6eHevcRtpStats frame_packetizer = {0};

	if (!state || !output || !stream)
		return 0;

	state->frame_counter++;

	if (output_enabled) {
		Star6eRtpFrameContext rtp_frame = {
			.rtp = &state->rtp_state,
			.frame_ticks = state->rtp_frame_ticks,
			.params = &state->param_sets,
			.max_payload = state->rtp_payload_size,
			.stats = verbose_enabled ? &frame_packetizer : NULL,
		};

		rtp_sidecar_poll(&state->sidecar);

		uint32_t frame_rtp_ts = state->rtp_state.timestamp;
		uint16_t seq_before = state->rtp_state.seq;
		uint64_t ready_us = monotonic_us();
		uint64_t capture_us = (stream->count > 0 && stream->packet)
			? stream->packet[0].timestamp : 0;

		total_bytes = star6e_output_send_frame(output, stream,
			state->max_frame_size, send_frame_output_rtp, &rtp_frame);

		rtp_sidecar_send_frame(&state->sidecar,
			state->rtp_state.ssrc, frame_rtp_ts,
			seq_before,
			(uint16_t)(state->rtp_state.seq - seq_before),
			capture_us, ready_us);
	}

	if (!verbose_enabled)
		return total_bytes;

	stream_metrics_record_frame(&state->verbose_metrics, total_bytes);
	if (star6e_output_is_rtp(output)) {
		state->verbose_packetizer_interval.total_nals += frame_packetizer.total_nals;
		state->verbose_packetizer_interval.single_packets += frame_packetizer.single_packets;
		state->verbose_packetizer_interval.ap_packets += frame_packetizer.ap_packets;
		state->verbose_packetizer_interval.ap_nals += frame_packetizer.ap_nals;
		state->verbose_packetizer_interval.fu_packets += frame_packetizer.fu_packets;
		state->verbose_packetizer_interval.rtp_packets += frame_packetizer.rtp_packets;
		state->verbose_packetizer_interval.rtp_payload_bytes += frame_packetizer.rtp_payload_bytes;
	}

	{
		StreamMetricsSample sample;
		struct timespec verbose_ts_now;

		clock_gettime(CLOCK_MONOTONIC, &verbose_ts_now);
		if (stream_metrics_sample(&state->verbose_metrics, &verbose_ts_now,
		    &sample)) {
			int ofd = stdout_filter_real_fd();
			dprintf(ofd, "[verbose] %lds | %u fps | %u kbps | frame %u | avg %u B/frame | %u packs\n",
				sample.uptime_s, sample.fps, sample.kbps,
				state->frame_counter, sample.avg_bytes, stream->count);
			if (star6e_output_is_rtp(output)) {
				unsigned int avg_rtp_payload =
					state->verbose_packetizer_interval.rtp_packets > 0
					? (unsigned int)(state->verbose_packetizer_interval.rtp_payload_bytes /
						state->verbose_packetizer_interval.rtp_packets) : 0;

				dprintf(ofd, "[pktzr] nals %u | rtp %u | fill %u B | single %u | ap %u/%u | fu %u\n",
					state->verbose_packetizer_interval.total_nals,
					state->verbose_packetizer_interval.rtp_packets,
					avg_rtp_payload,
					state->verbose_packetizer_interval.single_packets,
					state->verbose_packetizer_interval.ap_packets,
					state->verbose_packetizer_interval.ap_nals,
					state->verbose_packetizer_interval.fu_packets);
			}
			memset(&state->verbose_packetizer_interval, 0,
				sizeof(state->verbose_packetizer_interval));
		}
	}

	return total_bytes;
}
