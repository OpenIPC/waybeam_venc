#include "stream_metrics.h"

#include "test_helpers.h"

static int test_stream_metrics_basic(void)
{
	StreamMetricsState state;
	StreamMetricsSample sample;
	struct timespec start = { .tv_sec = 10, .tv_nsec = 0 };
	struct timespec early = { .tv_sec = 10, .tv_nsec = 500000000 };
	struct timespec later = { .tv_sec = 11, .tv_nsec = 500000000 };
	int failures = 0;

	stream_metrics_start(&state, &start);
	stream_metrics_record_frame(&state, 1000);
	stream_metrics_record_frame(&state, 2000);
	CHECK("stream metrics early no sample",
		stream_metrics_sample(&state, &early, &sample) == 0);
	CHECK("stream metrics later sample",
		stream_metrics_sample(&state, &later, &sample) == 1);
	CHECK("stream metrics uptime", sample.uptime_s == 1);
	CHECK("stream metrics elapsed", sample.elapsed_ms == 1500);
	CHECK("stream metrics fps", sample.fps == 1);
	CHECK("stream metrics kbps", sample.kbps == 15);
	CHECK("stream metrics avg bytes", sample.avg_bytes == 1500);
	return failures;
}

static int test_stream_metrics_reset(void)
{
	StreamMetricsState state;
	StreamMetricsSample sample;
	struct timespec start = { .tv_sec = 1, .tv_nsec = 0 };
	struct timespec later = { .tv_sec = 2, .tv_nsec = 100000000 };
	int failures = 0;

	stream_metrics_start(&state, &start);
	stream_metrics_record_frame(&state, 500);
	CHECK("stream metrics sample one",
		stream_metrics_sample(&state, &later, &sample) == 1);
	CHECK("stream metrics sample two empty",
		stream_metrics_sample(&state, &later, &sample) == 0);
	return failures;
}

int test_stream_metrics(void)
{
	int failures = 0;

	failures += test_stream_metrics_basic();
	failures += test_stream_metrics_reset();
	return failures;
}
