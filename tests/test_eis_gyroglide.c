/*
 * Unit tests for GyroGlide-Lite EIS backend.
 * Host-compiled (no hardware dependencies).
 *
 * Build: cc -std=c99 -Wall -Wextra -g -O0 -D_GNU_SOURCE -Iinclude \
 *        -DEIS_GYROGLIDE_TEST tests/test_eis_gyroglide.c \
 *        src/eis_gyroglide.c src/eis.c \
 *        -lpthread -lm -o tests/test_eis_gyroglide
 * Run:   ./tests/test_eis_gyroglide
 */

/* Provide minimal SDK stubs before any includes */
#ifndef EIS_GYROGLIDE_TEST
#define EIS_GYROGLIDE_TEST
#endif

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── SDK type stubs ─────────────────────────────────────────────────── */

#ifndef SIGMASTAR_TYPES_H
#define SIGMASTAR_TYPES_H

typedef int MI_S32;
typedef unsigned int MI_U32;
typedef unsigned short MI_U16;
typedef int MI_BOOL;
typedef int MI_VPE_CHANNEL;
typedef int MI_VPE_PORT;
typedef int MI_VENC_CHN;

typedef struct {
	unsigned short x;
	unsigned short y;
	unsigned short width;
	unsigned short height;
} i6_common_rect;

#endif

/* Stub: record last crop for verification */
static i6_common_rect g_last_crop = {0};
static int g_crop_call_count = 0;

MI_S32 MI_VPE_SetPortCrop(MI_VPE_CHANNEL chn, MI_VPE_PORT port,
	i6_common_rect *crop)
{
	(void)chn;
	(void)port;
	if (crop)
		g_last_crop = *crop;
	g_crop_call_count++;
	return 0;
}

/* Guard: star6e.h must not be included in test build */
#define STAR6E_PLATFORM_H

/* Now include the EIS headers */
#include "eis.h"
#include "eis_gyroglide.h"
#include "eis_ring.h"

/* ── Test framework ─────────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
	tests_run++; \
	printf("  %-50s ", #name); \
	fflush(stdout); \
} while (0)

#define PASS() do { \
	tests_passed++; \
	printf("PASS\n"); \
} while (0)

#define FAIL(msg) do { \
	printf("FAIL: %s\n", msg); \
} while (0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
	if (fabsf((a) - (b)) > (tol)) { \
		printf("FAIL: %s (got %.6f, expected %.6f)\n", msg, (double)(a), (double)(b)); \
		return; \
	} \
} while (0)

/* ── Helper: make a default config ──────────────────────────────────── */

static EisConfig default_cfg(void)
{
	EisConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.mode = "gyroglide";
	cfg.margin_percent = 10;
	cfg.capture_w = 1920;
	cfg.capture_h = 1080;
	cfg.vpe_channel = 0;
	cfg.vpe_port = 0;
	cfg.pixels_per_radian = 960.0f;
	cfg.test_mode = 0;
	cfg.swap_xy = 0;
	cfg.invert_x = 0;
	cfg.invert_y = 0;
	cfg.gain = 0.8f;
	cfg.deadband_rad = 0.001f;
	cfg.recenter_rate = 1.0f;
	cfg.max_slew_px = 8.0f;
	cfg.bias_alpha = 0.001f;
	return cfg;
}

static struct timespec make_ts(long sec, long nsec)
{
	struct timespec ts = { .tv_sec = sec, .tv_nsec = nsec };
	return ts;
}

/* ── Helpers: push samples using real monotonic clock ────────────────── */

#include <unistd.h>

static struct timespec now_ts(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts;
}

/* Push n samples at rate_hz, each with gyro (gx, gy, 0).
 * Uses real timestamps spaced by 1/rate_hz.  Total time ≈ n/rate_hz. */
static void push_real_samples(EisState *st, float gx, float gy,
	int n, int rate_hz)
{
	int interval_us = 1000000 / rate_hz;
	for (int i = 0; i < n; i++) {
		struct timespec ts = now_ts();
		eis_push_sample(st, gx, gy, 0.0f, &ts);
		if (i < n - 1)
			usleep((unsigned)interval_us);
	}
}

/* Sleep 2ms to ensure frame_dt > 0.0001 threshold */
static void frame_gap(void)
{
	usleep(2000);
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static void test_create_destroy(void)
{
	TEST(create_destroy);
	EisConfig cfg = default_cfg();
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);

	EisStatus status;
	eis_get_status(st, &status);
	assert(status.crop_w > 0);
	assert(status.crop_h > 0);
	assert(status.margin_x > 0);
	assert(status.margin_y > 0);
	assert(status.update_count == 0);

	eis_destroy(st);
	PASS();
}

static void test_create_invalid_margin(void)
{
	TEST(create_invalid_margin);
	EisConfig cfg = default_cfg();
	cfg.margin_percent = 0;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st == NULL);

	cfg.margin_percent = 50;
	st = eis_gyroglide_create(&cfg);
	assert(st == NULL);
	PASS();
}

static void test_create_small_capture(void)
{
	TEST(create_small_capture);
	EisConfig cfg = default_cfg();
	cfg.capture_w = 32;
	cfg.capture_h = 32;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st == NULL);
	PASS();
}

static void test_margin_geometry(void)
{
	TEST(margin_geometry);
	EisConfig cfg = default_cfg();
	cfg.capture_w = 2000;
	cfg.capture_h = 1000;
	cfg.margin_percent = 10;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);

	EisStatus s;
	eis_get_status(st, &s);
	assert(s.margin_x == 100);
	assert(s.margin_y == 50);
	assert(s.crop_w == 2000 - 200);
	assert(s.crop_h == 1000 - 100);

	eis_destroy(st);
	PASS();
}

static void test_batch_integration(void)
{
	TEST(batch_integration);
	EisConfig cfg = default_cfg();
	cfg.deadband_rad = 0.0f;
	cfg.recenter_rate = 0.0f;
	cfg.max_slew_px = 0.0f;
	cfg.bias_alpha = 0.0f;
	cfg.gain = 1.0f;
	cfg.pixels_per_radian = 100.0f;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);
	eis_set_imu_active(st, 1);

	/* Push real-time samples with known gyro rate */
	push_real_samples(st, 1.0f, 0.0f, 10, 200);
	frame_gap();

	eis_update(st);

	EisStatus s;
	eis_get_status(st, &s);
	assert(s.update_count == 1);
	assert(s.last_n_samples > 0);
	/* Offset should be nonzero (positive gyro → positive offset) */
	assert(s.offset_x > 0.0f);

	eis_destroy(st);
	PASS();
}

static void test_deadband_suppresses_noise(void)
{
	TEST(deadband_suppresses_noise);
	EisConfig cfg = default_cfg();
	cfg.deadband_rad = 1.0f;  /* huge deadband — nothing should pass */
	cfg.recenter_rate = 0.0f;
	cfg.max_slew_px = 0.0f;
	cfg.bias_alpha = 0.0f;
	cfg.gain = 1.0f;
	cfg.pixels_per_radian = 100.0f;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);
	eis_set_imu_active(st, 1);

	/* Push small gyro (will integrate to well below 1.0 rad deadband) */
	push_real_samples(st, 0.01f, 0.01f, 5, 200);
	frame_gap();
	eis_update(st);

	EisStatus s;
	eis_get_status(st, &s);
	ASSERT_NEAR(s.offset_x, 0.0f, 0.01f, "deadband x");
	ASSERT_NEAR(s.offset_y, 0.0f, 0.01f, "deadband y");

	eis_destroy(st);
	PASS();
}

static void test_recenter_decay(void)
{
	TEST(recenter_decay);
	EisConfig cfg = default_cfg();
	cfg.deadband_rad = 0.0f;
	cfg.recenter_rate = 50.0f;  /* very aggressive recenter */
	cfg.max_slew_px = 0.0f;
	cfg.bias_alpha = 0.0f;
	cfg.gain = 1.0f;
	cfg.pixels_per_radian = 1000.0f;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);
	eis_set_imu_active(st, 1);  /* disable sim generator */

	/* Baseline update to establish t_prev */
	frame_gap();
	eis_update(st);

	/* Push impulse (after baseline so t_prev is set) */
	push_real_samples(st, 2.0f, 0.0f, 10, 200);
	frame_gap();
	eis_update(st);

	EisStatus s1;
	eis_get_status(st, &s1);
	float pos_after_impulse = fabsf(s1.offset_x);

	/* Multiple updates with no new samples — position should decay */
	for (int i = 0; i < 20; i++) {
		usleep(5000);  /* 5ms gaps for meaningful decay */
		eis_update(st);
	}

	EisStatus s2;
	eis_get_status(st, &s2);

	/* After aggressive recenter (rate=50, total ~100ms),
	 * position should have decayed significantly */
	if (pos_after_impulse > 0.1f)
		assert(fabsf(s2.offset_x) < pos_after_impulse);

	eis_destroy(st);
	PASS();
}

static void test_clamp_to_margin(void)
{
	TEST(clamp_to_margin);
	EisConfig cfg = default_cfg();
	cfg.deadband_rad = 0.0f;
	cfg.recenter_rate = 0.0f;
	cfg.max_slew_px = 0.0f;
	cfg.bias_alpha = 0.0f;
	cfg.gain = 1.0f;
	cfg.pixels_per_radian = 10000.0f;
	cfg.capture_w = 200;
	cfg.capture_h = 200;
	cfg.margin_percent = 10;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);
	eis_set_imu_active(st, 1);

	/* Push extreme gyro to exceed margin */
	push_real_samples(st, 10.0f, 10.0f, 10, 200);
	frame_gap();
	eis_update(st);

	EisStatus s;
	eis_get_status(st, &s);
	assert(fabsf(s.offset_x) <= (float)s.margin_x + 0.01f);
	assert(fabsf(s.offset_y) <= (float)s.margin_y + 0.01f);

	eis_destroy(st);
	PASS();
}

static void test_slew_limit(void)
{
	TEST(slew_limit);
	EisConfig cfg = default_cfg();
	cfg.deadband_rad = 0.0f;
	cfg.recenter_rate = 0.0f;
	cfg.max_slew_px = 2.0f;
	cfg.bias_alpha = 0.0f;
	cfg.gain = 1.0f;
	cfg.pixels_per_radian = 1000.0f;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);
	eis_set_imu_active(st, 1);

	/* Baseline update */
	frame_gap();
	eis_update(st);

	EisStatus s1;
	eis_get_status(st, &s1);
	float prev_x = s1.offset_x;

	/* Push large impulse */
	push_real_samples(st, 5.0f, 0.0f, 5, 200);
	frame_gap();
	eis_update(st);

	EisStatus s2;
	eis_get_status(st, &s2);
	float delta = fabsf(s2.offset_x - prev_x);
	assert(delta <= 2.0f + 0.01f);

	eis_destroy(st);
	PASS();
}

static void test_axis_swap(void)
{
	TEST(axis_swap);
	EisConfig cfg = default_cfg();
	cfg.swap_xy = 1;
	cfg.deadband_rad = 0.0f;
	cfg.recenter_rate = 0.0f;
	cfg.max_slew_px = 0.0f;
	cfg.bias_alpha = 0.0f;
	cfg.gain = 1.0f;
	cfg.pixels_per_radian = 1000.0f;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);
	eis_set_imu_active(st, 1);

	/* Push X-only gyro — with swap, should move Y not X */
	push_real_samples(st, 1.0f, 0.0f, 5, 200);
	frame_gap();
	eis_update(st);

	EisStatus s;
	eis_get_status(st, &s);
	/* After swap: input gx→internal gy, input gy(0)→internal gx(0) */
	assert(fabsf(s.offset_y) >= fabsf(s.offset_x) ||
		(fabsf(s.offset_x) < 0.1f && fabsf(s.offset_y) < 0.1f));

	eis_destroy(st);
	PASS();
}

static void test_axis_invert(void)
{
	TEST(axis_invert);
	EisConfig cfg = default_cfg();
	cfg.invert_x = 1;
	cfg.deadband_rad = 0.0f;
	cfg.recenter_rate = 0.0f;
	cfg.max_slew_px = 0.0f;
	cfg.bias_alpha = 0.0f;
	cfg.gain = 1.0f;
	cfg.pixels_per_radian = 1000.0f;
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);
	eis_set_imu_active(st, 1);

	/* Push positive X gyro — with invert, offset should be negative */
	push_real_samples(st, 1.0f, 0.0f, 5, 200);
	frame_gap();
	eis_update(st);

	EisStatus s;
	eis_get_status(st, &s);
	assert(s.offset_x <= 0.01f);  /* negative or zero */

	eis_destroy(st);
	PASS();
}

static void test_zero_samples_frame(void)
{
	TEST(zero_samples_frame);
	EisConfig cfg = default_cfg();
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);
	eis_set_imu_active(st, 1);  /* disable sim so truly zero samples */

	frame_gap();
	eis_update(st);
	frame_gap();
	eis_update(st);

	EisStatus s;
	eis_get_status(st, &s);
	ASSERT_NEAR(s.offset_x, 0.0f, 0.01f, "zero-sample x");
	ASSERT_NEAR(s.offset_y, 0.0f, 0.01f, "zero-sample y");
	assert(s.update_count == 2);

	eis_destroy(st);
	PASS();
}

static void test_crop_coordinates_even_aligned(void)
{
	TEST(crop_coordinates_even_aligned);
	EisConfig cfg = default_cfg();
	EisState *st = eis_gyroglide_create(&cfg);
	assert(st != NULL);
	eis_set_imu_active(st, 1);

	push_real_samples(st, 0.5f, 0.3f, 5, 200);
	frame_gap();
	eis_update(st);

	EisStatus s;
	eis_get_status(st, &s);
	assert((s.crop_x & 1) == 0);
	assert((s.crop_y & 1) == 0);

	eis_destroy(st);
	PASS();
}

static void test_ring_buffer_basic(void)
{
	TEST(ring_buffer_basic);
	EisMotionRing ring;
	eis_ring_init(&ring);

	/* Push 5 samples */
	for (int i = 0; i < 5; i++) {
		EisMotionSample s = {0};
		s.ts = make_ts(0, i * 1000000L);
		s.gyro_x = (float)i;
		eis_ring_push(&ring, &s);
	}

	assert(eis_ring_count(&ring) == 5);

	/* Read all */
	EisMotionSample out[10];
	uint32_t n = eis_ring_read_range(&ring, make_ts(0, 0),
		make_ts(0, 4000000L), out, 10);
	assert(n == 5);
	assert(out[0].gyro_x == 0.0f);
	assert(out[4].gyro_x == 4.0f);

	/* Read subset */
	n = eis_ring_read_range(&ring, make_ts(0, 1000000L),
		make_ts(0, 3000000L), out, 10);
	assert(n == 3);

	eis_ring_destroy(&ring);
	PASS();
}

static void test_dispatch_null_safety(void)
{
	TEST(dispatch_null_safety);
	/* All dispatch functions should handle NULL gracefully */
	eis_update(NULL);
	eis_push_sample(NULL, 0, 0, 0, NULL);
	eis_set_imu_active(NULL, 1);

	EisStatus s;
	eis_get_status(NULL, &s);
	assert(s.crop_w == 0);

	eis_destroy(NULL);  /* should not crash */
	PASS();
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void)
{
	printf("\n=== GyroGlide-Lite EIS unit tests ===\n\n");

	test_create_destroy();
	test_create_invalid_margin();
	test_create_small_capture();
	test_margin_geometry();
	test_batch_integration();
	test_deadband_suppresses_noise();
	test_recenter_decay();
	test_clamp_to_margin();
	test_slew_limit();
	test_axis_swap();
	test_axis_invert();
	test_zero_samples_frame();
	test_crop_coordinates_even_aligned();
	test_ring_buffer_basic();
	test_dispatch_null_safety();

	printf("\n  %d/%d tests passed\n\n", tests_passed, tests_run);
	return (tests_passed == tests_run) ? 0 : 1;
}
