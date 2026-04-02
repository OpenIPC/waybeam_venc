#include "scene_estimator.h"
#include "test_helpers.h"

#include <string.h>

static EncoderFrameStats make_stats(uint32_t seq, uint32_t size,
	uint8_t qp, uint8_t frame_type)
{
	EncoderFrameStats s;

	memset(&s, 0, sizeof(s));
	s.frame_seq = seq;
	s.frame_size_bytes = size;
	s.qp_avg = qp;
	s.frame_type = frame_type;
	return s;
}

static int test_scene_est_init(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	int failures = 0;

	scene_est_init(&state, 250, 2, 5000);
	scene_est_get(&state, &est);

	CHECK("scene est init complexity", est.complexity == 0);
	CHECK("scene est init no scene change", est.scene_change == 0);

	return failures;
}

static int test_scene_est_stable(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	EncoderFrameStats stats;
	int failures = 0;
	uint32_t i;

	scene_est_init(&state, 250, 2, 5000);

	/* Feed 30 stable frames — no scene change expected */
	for (i = 0; i < 30; i++) {
		stats = make_stats(i, 5000 + (i % 3) * 100, 28, ENC_FRAME_P);
		scene_est_update(&state, &stats);
	}

	scene_est_get(&state, &est);

	CHECK("stable no scene change", est.scene_change == 0);
	CHECK("stable complexity mid range", est.complexity > 80 && est.complexity < 180);
	CHECK("stable budget ratio near 1000",
		est.budget_ratio > 900 && est.budget_ratio < 1200);

	return failures;
}

static int test_scene_est_spike(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	EncoderFrameStats stats;
	int failures = 0;
	uint32_t i;

	scene_est_init(&state, 250, 2, 5000);

	/* Feed stable P-frames to warm up EMA */
	for (i = 0; i < 15; i++) {
		stats = make_stats(i, 5000, 28, ENC_FRAME_P);
		scene_est_update(&state, &stats);
	}

	scene_est_get(&state, &est);
	CHECK("before spike no change", est.scene_change == 0);

	/* First spike frame — holdoff requires 2 consecutive */
	stats = make_stats(15, 15000, 35, ENC_FRAME_P);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("first spike no change (holdoff)", est.scene_change == 0);

	/* Second spike frame — should trigger */
	stats = make_stats(16, 16000, 36, ENC_FRAME_P);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("second spike triggers change", est.scene_change == 1);
	CHECK("spike high complexity", est.complexity > 200);

	return failures;
}

static int test_scene_est_no_false_positive(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	EncoderFrameStats stats;
	int failures = 0;
	uint32_t i;
	int false_positives = 0;

	scene_est_init(&state, 250, 2, 5000);

	/* Feed gradually increasing frames — should NOT trigger scene change */
	for (i = 0; i < 60; i++) {
		uint32_t size = 5000 + i * 50;  /* gradual increase */
		stats = make_stats(i, size, 28 + (uint8_t)(i / 10), ENC_FRAME_P);
		scene_est_update(&state, &stats);
		scene_est_get(&state, &est);
		if (est.scene_change)
			false_positives++;
	}

	CHECK("no false positives on gradual", false_positives == 0);

	return failures;
}

static int test_scene_est_clear_change(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	EncoderFrameStats stats;
	int failures = 0;
	uint32_t i;

	scene_est_init(&state, 250, 2, 5000);

	/* Warm up */
	for (i = 0; i < 15; i++) {
		stats = make_stats(i, 5000, 28, ENC_FRAME_P);
		scene_est_update(&state, &stats);
	}

	/* Trigger scene change */
	stats = make_stats(15, 15000, 35, ENC_FRAME_P);
	scene_est_update(&state, &stats);
	stats = make_stats(16, 16000, 36, ENC_FRAME_P);
	scene_est_update(&state, &stats);

	scene_est_get(&state, &est);
	CHECK("change triggered", est.scene_change == 1);

	/* Clear */
	scene_est_clear_change(&state);
	scene_est_get(&state, &est);
	CHECK("change cleared", est.scene_change == 0);

	return failures;
}

static int test_scene_est_budget_ratio(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	EncoderFrameStats stats;
	int failures = 0;

	scene_est_init(&state, 250, 2, 10000);

	/* Frame exactly at target */
	stats = make_stats(0, 10000, 28, ENC_FRAME_P);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("budget ratio at target", est.budget_ratio == 1000);

	/* Frame at half target */
	stats = make_stats(1, 5000, 22, ENC_FRAME_P);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("budget ratio half", est.budget_ratio == 500);

	/* Frame at 2x target */
	stats = make_stats(2, 20000, 38, ENC_FRAME_P);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("budget ratio 2x", est.budget_ratio == 2000);

	return failures;
}

int test_scene_estimator(void)
{
	int failures = 0;

	failures += test_scene_est_init();
	failures += test_scene_est_stable();
	failures += test_scene_est_spike();
	failures += test_scene_est_no_false_positive();
	failures += test_scene_est_clear_change();
	failures += test_scene_est_budget_ratio();

	return failures;
}
