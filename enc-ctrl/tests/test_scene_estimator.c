#include "scene_estimator.h"
#include "test_helpers.h"

#include <string.h>

static EncoderFrameStats make_stats(uint32_t seq, uint32_t size,
	uint8_t qp, uint8_t frame_type, uint16_t intra_ratio)
{
	EncoderFrameStats s;

	memset(&s, 0, sizeof(s));
	s.frame_seq = seq;
	s.frame_size_bytes = size;
	s.qp_avg = qp;
	s.frame_type = frame_type;
	s.intra_ratio = intra_ratio;
	return s;
}

static int test_scene_est_init(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	int failures = 0;

	scene_est_init(&state, 150, 2, 5000);
	scene_est_get(&state, &est);

	CHECK("scene est init complexity", est.complexity == 0);
	CHECK("scene est init no scene change", est.scene_change == 0);

	return failures;
}

static int test_scene_est_stable_cbr(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	EncoderFrameStats stats;
	int failures = 0;
	uint32_t i;

	scene_est_init(&state, 150, 2, 5000);

	/* Feed 30 stable CBR frames — constant size, low intra ratio */
	for (i = 0; i < 30; i++) {
		stats = make_stats(i, 5000, 0, ENC_FRAME_P, 10 + (i % 5));
		scene_est_update(&state, &stats);
	}

	scene_est_get(&state, &est);

	CHECK("stable cbr no scene change", est.scene_change == 0);
	CHECK("stable cbr low complexity", est.complexity < 20);
	CHECK("stable cbr budget ratio near 1000",
		est.budget_ratio > 900 && est.budget_ratio < 1200);

	return failures;
}

static int test_scene_est_intra_spike(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	EncoderFrameStats stats;
	int failures = 0;
	uint32_t i;

	scene_est_init(&state, 150, 2, 5000);

	/* Feed stable P-frames to warm up (low intra ratio, constant size) */
	for (i = 0; i < 15; i++) {
		stats = make_stats(i, 5000, 0, ENC_FRAME_P, 10);
		scene_est_update(&state, &stats);
	}

	scene_est_get(&state, &est);
	CHECK("before spike no change", est.scene_change == 0);

	/* First spike: intra_ratio jumps to 300 (30%) — holdoff needs 2 */
	stats = make_stats(15, 5000, 0, ENC_FRAME_P, 300);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("first intra spike no change (holdoff)", est.scene_change == 0);
	CHECK("first intra spike high complexity", est.complexity > 50);

	/* Second spike: triggers scene change */
	stats = make_stats(16, 5000, 0, ENC_FRAME_P, 350);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("second intra spike triggers change", est.scene_change == 1);
	CHECK("second intra spike complexity", est.complexity > 70);

	return failures;
}

static int test_scene_est_vbr_size_spike(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	EncoderFrameStats stats;
	int failures = 0;
	uint32_t i;

	/* Secondary signal: frame size spike for VBR modes */
	scene_est_init(&state, 150, 2, 5000);

	for (i = 0; i < 15; i++) {
		stats = make_stats(i, 5000, 0, ENC_FRAME_P, 10);
		scene_est_update(&state, &stats);
	}

	/* Frame size 3x EMA but low intra — triggers via secondary */
	stats = make_stats(15, 15000, 0, ENC_FRAME_P, 10);
	scene_est_update(&state, &stats);
	stats = make_stats(16, 16000, 0, ENC_FRAME_P, 10);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("vbr size spike triggers change", est.scene_change == 1);

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

	scene_est_init(&state, 150, 2, 5000);

	/* Feed gradually increasing intra ratio — should NOT trigger */
	for (i = 0; i < 60; i++) {
		uint16_t ratio = (uint16_t)(10 + i * 2);  /* 10 → 130, stays below 150 */
		stats = make_stats(i, 5000, 0, ENC_FRAME_P, ratio);
		scene_est_update(&state, &stats);
		scene_est_get(&state, &est);
		if (est.scene_change)
			false_positives++;
	}

	CHECK("no false positives on gradual intra", false_positives == 0);

	return failures;
}

static int test_scene_est_clear_change(void)
{
	SceneEstimatorState state;
	SceneEstimate est;
	EncoderFrameStats stats;
	int failures = 0;
	uint32_t i;

	scene_est_init(&state, 150, 2, 5000);

	/* Warm up */
	for (i = 0; i < 15; i++) {
		stats = make_stats(i, 5000, 0, ENC_FRAME_P, 10);
		scene_est_update(&state, &stats);
	}

	/* Trigger scene change via intra spike */
	stats = make_stats(15, 5000, 0, ENC_FRAME_P, 300);
	scene_est_update(&state, &stats);
	stats = make_stats(16, 5000, 0, ENC_FRAME_P, 350);
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

	scene_est_init(&state, 150, 2, 10000);

	/* Frame exactly at target */
	stats = make_stats(0, 10000, 0, ENC_FRAME_P, 10);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("budget ratio at target", est.budget_ratio == 1000);

	/* Frame at half target */
	stats = make_stats(1, 5000, 0, ENC_FRAME_P, 10);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("budget ratio half", est.budget_ratio == 500);

	/* Frame at 2x target */
	stats = make_stats(2, 20000, 0, ENC_FRAME_P, 10);
	scene_est_update(&state, &stats);
	scene_est_get(&state, &est);
	CHECK("budget ratio 2x", est.budget_ratio == 2000);

	return failures;
}

int test_scene_estimator(void)
{
	int failures = 0;

	failures += test_scene_est_init();
	failures += test_scene_est_stable_cbr();
	failures += test_scene_est_intra_spike();
	failures += test_scene_est_vbr_size_spike();
	failures += test_scene_est_no_false_positive();
	failures += test_scene_est_clear_change();
	failures += test_scene_est_budget_ratio();

	return failures;
}
