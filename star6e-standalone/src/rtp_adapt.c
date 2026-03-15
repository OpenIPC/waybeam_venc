#include "rtp_adapt.h"

#include <stdio.h>

#define RTP_ADAPTIVE_HYSTERESIS_NUM 3
#define RTP_ADAPTIVE_HYSTERESIS_DEN 20

int rtp_adapt_update(RtpAdaptState *state, int enabled, size_t frame_bytes,
	int verbose)
{
	uint32_t avg;
	uint32_t sample;
	uint32_t fps;
	uint32_t target;
	uint16_t cap;
	uint16_t cur;
	uint32_t delta;
	int at_boundary;

	if (!state) {
		return -1;
	}
	if (!enabled || frame_bytes == 0 || state->target_pkt_rate == 0) {
		return 0;
	}

	avg = state->ewma_frame_bytes >> 8;

	/* Skip frames >3x the running average (likely IDR / scene change). */
	if (avg > 0 && frame_bytes > (size_t)avg * 3) {
		return 0;
	}

	/* EWMA fixed-point <<8: new = old - old/4 + sample/4 (alpha=0.25). */
	sample = (uint32_t)frame_bytes << 8;
	if (state->ewma_frame_bytes == 0) {
		state->ewma_frame_bytes = sample;
	} else {
		state->ewma_frame_bytes = state->ewma_frame_bytes
			- (state->ewma_frame_bytes >> 2)
			+ (sample >> 2);
	}

	/* Rate-limit: only evaluate after cooldown expires (≈2 changes/sec). */
	if (state->adapt_cooldown > 0) {
		state->adapt_cooldown--;
		return 0;
	}

	avg = state->ewma_frame_bytes >> 8;
	fps = state->sensor_fps ? state->sensor_fps : 30;
	target = (avg * fps) / state->target_pkt_rate;
	if (target < RTP_MIN_PAYLOAD) {
		target = RTP_MIN_PAYLOAD;
	}

	cap = state->max_frame_size;
	if (cap < RTP_MIN_PAYLOAD) {
		cap = RTP_MIN_PAYLOAD;
	}
	if (target > cap) {
		target = cap;
	}

	cur = state->payload_size;
	if (target == cur) {
		return 0;
	}

	delta = (target > cur) ? (target - cur) : (cur - target);
	at_boundary = (target == RTP_MIN_PAYLOAD || target == cap);
	if (!at_boundary &&
		delta <= (uint32_t)cur * RTP_ADAPTIVE_HYSTERESIS_NUM /
			RTP_ADAPTIVE_HYSTERESIS_DEN) {
		return 0;
	}

	if (verbose) {
		printf("[rtp-adapt] payload %u -> %u (avg %u B/frame, %u fps, %u pkt/s target)\n",
			cur, (unsigned)target, avg, fps, state->target_pkt_rate);
	}
	state->payload_size = (uint16_t)target;
	state->adapt_cooldown = (uint16_t)(fps / 2);
	return 1;
}
