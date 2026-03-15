#include "stream_metrics.h"

#include <string.h>

void stream_metrics_reset(StreamMetricsState *state)
{
	if (!state)
		return;

	memset(state, 0, sizeof(*state));
}

void stream_metrics_start(StreamMetricsState *state,
	const struct timespec *now)
{
	if (!state || !now)
		return;

	stream_metrics_reset(state);
	state->ts_start = *now;
	state->ts_last = *now;
}

void stream_metrics_record_frame(StreamMetricsState *state, size_t frame_bytes)
{
	if (!state)
		return;

	state->interval_frames++;
	state->interval_bytes += frame_bytes;
}

int stream_metrics_sample(StreamMetricsState *state,
	const struct timespec *now, StreamMetricsSample *sample)
{
	long elapsed_ms;

	if (!state || !now || !sample)
		return 0;

	elapsed_ms = (now->tv_sec - state->ts_last.tv_sec) * 1000L +
		(now->tv_nsec - state->ts_last.tv_nsec) / 1000000L;
	if (elapsed_ms < 1000)
		return 0;

	memset(sample, 0, sizeof(*sample));
	sample->elapsed_ms = elapsed_ms;
	sample->uptime_s = now->tv_sec - state->ts_start.tv_sec;
	sample->fps = (unsigned int)((state->interval_frames * 1000L) / elapsed_ms);
	sample->kbps = (unsigned int)((state->interval_bytes * 8000UL) /
		((unsigned long)elapsed_ms * 1024UL));
	if (state->interval_frames > 0) {
		sample->avg_bytes = (unsigned int)(state->interval_bytes /
			state->interval_frames);
	}

	state->ts_last = *now;
	state->interval_frames = 0;
	state->interval_bytes = 0;
	return 1;
}
