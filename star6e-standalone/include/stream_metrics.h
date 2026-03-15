#ifndef STREAM_METRICS_H
#define STREAM_METRICS_H

#include <stddef.h>
#include <time.h>

typedef struct {
	unsigned int interval_frames;
	size_t interval_bytes;
	struct timespec ts_start;
	struct timespec ts_last;
} StreamMetricsState;

typedef struct {
	long uptime_s;
	long elapsed_ms;
	unsigned int fps;
	unsigned int kbps;
	unsigned int avg_bytes;
} StreamMetricsSample;

/** Reset metrics accumulator to zero. */
void stream_metrics_reset(StreamMetricsState *state);

/** Mark the start time for a new metrics interval. */
void stream_metrics_start(StreamMetricsState *state,
	const struct timespec *now);

/** Record one frame's byte count for throughput calculation. */
void stream_metrics_record_frame(StreamMetricsState *state, size_t frame_bytes);

/** Calculate FPS, bitrate, and average frame size if interval elapsed. */
int stream_metrics_sample(StreamMetricsState *state,
	const struct timespec *now, StreamMetricsSample *sample);

#endif /* STREAM_METRICS_H */
