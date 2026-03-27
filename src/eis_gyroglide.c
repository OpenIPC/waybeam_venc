#include "eis_gyroglide.h"
#include "eis_ring.h"
#include "star6e.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define GGL_BATCH_MAX      64   /* max samples per frame @ 400Hz/30fps ≈ 13 */
#define GGL_DT_MAX         0.05f  /* 50ms sanity cap on sample gap */
#define GGL_BIAS_THRESH    0.05f  /* rad/s — only adapt bias below this rate */
#define GGL_EDGE_THRESH    0.8f   /* fraction of margin for edge-aware recenter */
#define GGL_EDGE_STRENGTH  5.0f   /* ramp multiplier for edge recenter */
#define GGL_EDGE_RATE      3.0f   /* extra decay rate at margin edge */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Simulated gyro (same pattern as legacy for test_mode)              */
/* ------------------------------------------------------------------ */

#define SIM_SAMPLE_RATE  200

#define SIM_X_FREQ1  0.5
#define SIM_X_AMP1   0.15
#define SIM_X_FREQ2  2.3
#define SIM_X_AMP2   0.08

#define SIM_Y_FREQ1  0.35
#define SIM_Y_AMP1   0.10
#define SIM_Y_FREQ2  1.7
#define SIM_Y_AMP2   0.06

/* ------------------------------------------------------------------ */
/* Internal state                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
	/* Config (set once at init) */
	uint16_t capture_w, capture_h;
	uint16_t crop_w, crop_h;
	uint16_t margin_x, margin_y;
	int vpe_channel, vpe_port;
	float pixels_per_radian;
	float gain;
	float deadband_rad;
	float recenter_rate;
	float max_slew_px;
	float bias_alpha;
	int test_mode;
	int swap_xy, invert_x, invert_y;

	/* Runtime state */
	float pos_x, pos_y;           /* crop offset in pixels from center */
	float prev_out_x, prev_out_y; /* previous frame output (for slew) */
	float bias_x, bias_y;         /* runtime gyro bias estimate (rad/s) */
	uint16_t crop_x, crop_y;      /* actual crop coordinates */
	struct timespec t_prev;        /* last update timestamp */
	struct timespec start_ts;
	uint32_t update_count;
	uint32_t last_n_samples;

	EisMotionRing ring;
	double sim_time;
	int imu_active;
} GyroglideState;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline uint16_t align2(uint16_t v)
{
	return v & ~(uint16_t)1;
}

static inline float clampf(float v, float lo, float hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static inline float ts_diff_sec(struct timespec a, struct timespec b)
{
	return (float)(b.tv_sec - a.tv_sec)
		+ (float)(b.tv_nsec - a.tv_nsec) * 1e-9f;
}

static inline double ts_to_sec_d(struct timespec a, struct timespec b)
{
	return (double)(b.tv_sec - a.tv_sec)
		+ (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

static inline float fabsf_safe(float v)
{
	return v < 0.0f ? -v : v;
}

static int apply_crop(const GyroglideState *st)
{
	i6_common_rect crop = {
		.x = st->crop_x,
		.y = st->crop_y,
		.width = st->crop_w,
		.height = st->crop_h,
	};
	return MI_VPE_SetPortCrop(st->vpe_channel, st->vpe_port, &crop);
}

/* ------------------------------------------------------------------ */
/* Simulated gyro producer                                            */
/* ------------------------------------------------------------------ */

static struct timespec ts_interpolate(struct timespec base, long long dt_ns,
	int i, int n)
{
	long long frac_ns = dt_ns * (i + 1) / n;
	struct timespec out = base;
	out.tv_nsec += (long)(frac_ns % 1000000000LL);
	out.tv_sec += (time_t)(frac_ns / 1000000000LL);
	if (out.tv_nsec >= 1000000000L) {
		out.tv_nsec -= 1000000000L;
		out.tv_sec++;
	}
	return out;
}

static void sim_generate_samples(GyroglideState *st, double dt)
{
	if (dt <= 0.0)
		return;

	int n_samples = (int)(dt * SIM_SAMPLE_RATE);
	if (n_samples < 1)
		n_samples = 1;
	if (n_samples > (int)EIS_RING_CAPACITY)
		n_samples = (int)EIS_RING_CAPACITY;

	double step = dt / n_samples;
	double t = st->sim_time;
	long long dt_ns = (long long)(dt * 1e9);

	double two_pi = 2.0 * M_PI;
	for (int i = 0; i < n_samples; i++) {
		t += step;

		EisMotionSample s;
		s.ts = ts_interpolate(st->t_prev, dt_ns, i, n_samples);
		s.gyro_x = (float)(SIM_X_AMP1 * sin(two_pi * SIM_X_FREQ1 * t)
			+ SIM_X_AMP2 * sin(two_pi * SIM_X_FREQ2 * t));
		s.gyro_y = (float)(SIM_Y_AMP1 * sin(two_pi * SIM_Y_FREQ1 * t)
			+ SIM_Y_AMP2 * sin(two_pi * SIM_Y_FREQ2 * t));
		s.gyro_z = 0.0f;

		eis_ring_push(&st->ring, &s);
	}

	st->sim_time = t;
}

/* ------------------------------------------------------------------ */
/* Core algorithm — called per video frame                            */
/* ------------------------------------------------------------------ */

/*
 * gyroglide_compute — Pure computation: integrate batch, update position,
 * return crop coordinates.  Separated from I/O for testability.
 *
 * Inputs:  st (state), t_now (frame timestamp),
 *          samples/n (batch from ring buffer)
 * Outputs: *out_crop_x, *out_crop_y
 * Side effects: mutates st->pos_x/y, bias_x/y, prev_out_x/y
 */
static void gyroglide_compute(GyroglideState *st, struct timespec t_now,
	const EisMotionSample *samples, uint32_t n,
	uint16_t *out_crop_x, uint16_t *out_crop_y)
{
	float frame_dt = ts_diff_sec(st->t_prev, t_now);
	if (frame_dt < 0.001f)
		frame_dt = 0.001f;

	/* ── Step 1: Timestamp-based gyro integration ── */
	float delta_x = 0.0f, delta_y = 0.0f;

	/* Scalar loop: NEON benchmarked at 0.5x (slower) on Cortex-A7 due to
	 * gather-load overhead from non-contiguous EisMotionSample struct. */
	for (uint32_t i = 0; i < n; i++) {
		float dt_i;
		if (i == 0)
			dt_i = ts_diff_sec(st->t_prev, samples[0].ts);
		else
			dt_i = ts_diff_sec(samples[i - 1].ts, samples[i].ts);

		if (dt_i < 0.0f) dt_i = 0.0f;
		if (dt_i > GGL_DT_MAX) dt_i = GGL_DT_MAX;

		/* Bias-corrected gyro */
		float gx = samples[i].gyro_x - st->bias_x;
		float gy = samples[i].gyro_y - st->bias_y;

		delta_x += gx * dt_i;
		delta_y += gy * dt_i;

		/* Slow bias tracking (only when motion is small) */
		if (fabsf_safe(samples[i].gyro_x) < GGL_BIAS_THRESH)
			st->bias_x += st->bias_alpha
				* (samples[i].gyro_x - st->bias_x);
		if (fabsf_safe(samples[i].gyro_y) < GGL_BIAS_THRESH)
			st->bias_y += st->bias_alpha
				* (samples[i].gyro_y - st->bias_y);
	}

	/* Tail extrapolation: last sample → frame time */
	if (n > 0) {
		float dt_tail = ts_diff_sec(samples[n - 1].ts, t_now);
		if (dt_tail > 0.0f && dt_tail < 0.02f) {
			delta_x += (samples[n - 1].gyro_x - st->bias_x)
				* dt_tail;
			delta_y += (samples[n - 1].gyro_y - st->bias_y)
				* dt_tail;
		}
	}

	/* ── Step 2: Deadband ── */
	if (fabsf_safe(delta_x) < st->deadband_rad) delta_x = 0.0f;
	if (fabsf_safe(delta_y) < st->deadband_rad) delta_y = 0.0f;

	/* ── Step 3: Position update ── */
	st->pos_x += delta_x * st->pixels_per_radian * st->gain;
	st->pos_y += delta_y * st->pixels_per_radian * st->gain;

	/* Recenter only when there is no significant motion.
	 * During active shake the crop should track fully;
	 * recenter activates only when the camera is still,
	 * preventing bias drift from pushing crop to the edge. */
	float motion = fabsf_safe(delta_x) + fabsf_safe(delta_y);
	float motion_px = motion * st->pixels_per_radian;
	int is_idle = (motion_px < 0.5f);

	if (is_idle) {
		float decay = 1.0f - st->recenter_rate * frame_dt;
		if (decay < 0.0f) decay = 0.0f;
		st->pos_x *= decay;
		st->pos_y *= decay;
	}

	/* ── Step 4: Edge-aware recentering ── */
	/* Always active regardless of motion — prevents hard saturation
	 * at margin limits during sustained rotation. */
	if (st->margin_x > 0) {
		float edge_x = fabsf_safe(st->pos_x) / (float)st->margin_x;
		if (edge_x > GGL_EDGE_THRESH) {
			float extra = (edge_x - GGL_EDGE_THRESH)
				* GGL_EDGE_STRENGTH;
			float mult = 1.0f - extra * frame_dt * GGL_EDGE_RATE;
			if (mult < 0.0f) mult = 0.0f;
			st->pos_x *= mult;
		}
	}
	if (st->margin_y > 0) {
		float edge_y = fabsf_safe(st->pos_y) / (float)st->margin_y;
		if (edge_y > GGL_EDGE_THRESH) {
			float extra = (edge_y - GGL_EDGE_THRESH)
				* GGL_EDGE_STRENGTH;
			float mult = 1.0f - extra * frame_dt * GGL_EDGE_RATE;
			if (mult < 0.0f) mult = 0.0f;
			st->pos_y *= mult;
		}
	}

	/* ── Step 5: Slew limit ── */
	if (st->max_slew_px > 0.0f) {
		float dx = st->pos_x - st->prev_out_x;
		float dy = st->pos_y - st->prev_out_y;
		if (dx > st->max_slew_px) st->pos_x = st->prev_out_x + st->max_slew_px;
		if (dx < -st->max_slew_px) st->pos_x = st->prev_out_x - st->max_slew_px;
		if (dy > st->max_slew_px) st->pos_y = st->prev_out_y + st->max_slew_px;
		if (dy < -st->max_slew_px) st->pos_y = st->prev_out_y - st->max_slew_px;
	}

	/* ── Step 6: Clamp to margin (with NaN guard) ── */
	if (st->pos_x != st->pos_x) st->pos_x = 0.0f;  /* NaN check */
	if (st->pos_y != st->pos_y) st->pos_y = 0.0f;
	st->pos_x = clampf(st->pos_x, -(float)st->margin_x,
		(float)st->margin_x);
	st->pos_y = clampf(st->pos_y, -(float)st->margin_y,
		(float)st->margin_y);

	/* Record for next frame's slew limit */
	st->prev_out_x = st->pos_x;
	st->prev_out_y = st->pos_y;

	/* ── Step 7: Compute crop coordinates ── */
	int cx = (int)st->margin_x + (int)st->pos_x;
	int cy = (int)st->margin_y + (int)st->pos_y;

	int max_x = (int)st->capture_w - (int)st->crop_w;
	int max_y = (int)st->capture_h - (int)st->crop_h;
	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	if (cx > max_x) cx = max_x;
	if (cy > max_y) cy = max_y;

	*out_crop_x = align2((uint16_t)cx);
	*out_crop_y = align2((uint16_t)cy);
}

/* ------------------------------------------------------------------ */
/* EisOps implementation                                              */
/* ------------------------------------------------------------------ */

static int gyroglide_update(void *ctx)
{
	GyroglideState *st = (GyroglideState *)ctx;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	float frame_dt = ts_diff_sec(st->t_prev, now);

	if (frame_dt < 0.0001f)
		return 0;

	if (!st->imu_active)
		sim_generate_samples(st, (double)frame_dt);

	/* Test mode: inject visible sine wobble */
	if (st->test_mode) {
		double elapsed = ts_to_sec_d(st->start_ts, now);
		float wobble_x = (float)(st->margin_x * 0.8
			* sin(2.0 * M_PI * 0.5 * elapsed));
		float wobble_y = (float)(st->margin_y * 0.8
			* sin(2.0 * M_PI * 0.3 * elapsed));

		int cx = (int)st->margin_x + (int)wobble_x;
		int cy = (int)st->margin_y + (int)wobble_y;
		int max_x = (int)st->capture_w - (int)st->crop_w;
		int max_y = (int)st->capture_h - (int)st->crop_h;
		if (cx < 0) cx = 0;
		if (cy < 0) cy = 0;
		if (cx > max_x) cx = max_x;
		if (cy > max_y) cy = max_y;

		st->crop_x = align2((uint16_t)cx);
		st->crop_y = align2((uint16_t)cy);
		int ret = apply_crop(st);

		st->t_prev = now;
		st->update_count++;
		return ret;
	}

	/* Extract batch from ring buffer */
	EisMotionSample batch[GGL_BATCH_MAX];
	uint32_t n = eis_ring_read_range(&st->ring, st->t_prev, now,
		batch, GGL_BATCH_MAX);
	st->last_n_samples = n;

	/* Compute new crop position */
	uint16_t new_cx, new_cy;
	gyroglide_compute(st, now, batch, n, &new_cx, &new_cy);

	st->crop_x = new_cx;
	st->crop_y = new_cy;
	int ret = apply_crop(st);

	st->t_prev = now;
	st->update_count++;

	return ret;
}

static void gyroglide_push_sample(void *ctx, float gx, float gy, float gz,
	const struct timespec *ts)
{
	GyroglideState *st = (GyroglideState *)ctx;

	EisMotionSample s;
	if (ts)
		s.ts = *ts;
	else
		clock_gettime(CLOCK_MONOTONIC, &s.ts);

	float sx = gx, sy = gy;
	if (st->swap_xy) {
		float tmp = sx;
		sx = sy;
		sy = tmp;
	}
	if (st->invert_x) sx = -sx;
	if (st->invert_y) sy = -sy;

	s.gyro_x = sx;
	s.gyro_y = sy;
	s.gyro_z = gz;

	eis_ring_push(&st->ring, &s);
}

static void gyroglide_set_imu_active(void *ctx, int active)
{
	GyroglideState *st = (GyroglideState *)ctx;
	st->imu_active = active;
}

static void gyroglide_get_status(void *ctx, EisStatus *out)
{
	GyroglideState *st = (GyroglideState *)ctx;

	if (!out)
		return;

	out->crop_x = st->crop_x;
	out->crop_y = st->crop_y;
	out->crop_w = st->crop_w;
	out->crop_h = st->crop_h;
	out->margin_x = st->margin_x;
	out->margin_y = st->margin_y;
	out->offset_x = st->pos_x;
	out->offset_y = st->pos_y;
	out->update_count = st->update_count;
	out->last_n_samples = st->last_n_samples;
	out->raw_angle_x = st->pos_x;   /* reuse: position state */
	out->raw_angle_y = st->pos_y;
	out->smooth_angle_x = st->bias_x;  /* reuse: bias state */
	out->smooth_angle_y = st->bias_y;
	out->ring_count = eis_ring_count(&st->ring);
}

static void gyroglide_destroy(void *ctx)
{
	GyroglideState *st = (GyroglideState *)ctx;
	if (!st)
		return;

	printf("  - EIS   : gyroglide shutdown after %u updates\n",
		st->update_count);
	eis_ring_destroy(&st->ring);
	free(st);
}

/* ------------------------------------------------------------------ */
/* Vtable                                                             */
/* ------------------------------------------------------------------ */

static const EisOps gyroglide_ops = {
	.update = gyroglide_update,
	.push_sample = gyroglide_push_sample,
	.set_imu_active = gyroglide_set_imu_active,
	.get_status = gyroglide_get_status,
	.destroy = gyroglide_destroy,
};

/* ------------------------------------------------------------------ */
/* Factory                                                            */
/* ------------------------------------------------------------------ */

/* VPE SetPortCrop minimum: crop must be at least 70% of capture
 * dimensions to avoid VPE pipeline stalls when scaling is active.
 * Empirically validated on SSC30KQ: 30% margin works, 33% stalls. */
#define GGL_MAX_MARGIN_PERCENT 30
#define GGL_MIN_CROP_FRAC      70  /* percent of capture */

EisState *eis_gyroglide_create(const EisConfig *cfg)
{
	if (!cfg || cfg->margin_percent <= 0 || cfg->margin_percent >= 50) {
		fprintf(stderr, "EIS: invalid margin_percent %d (must be 1..49)\n",
			cfg ? cfg->margin_percent : 0);
		return NULL;
	}
	if (cfg->capture_w < 64 || cfg->capture_h < 64) {
		fprintf(stderr, "EIS: capture dimensions too small\n");
		return NULL;
	}

	int margin_pct = cfg->margin_percent;
	if (margin_pct > GGL_MAX_MARGIN_PERCENT) {
		fprintf(stderr, "EIS: margin_percent %d exceeds VPE safe limit,"
			" clamping to %d%%\n", margin_pct, GGL_MAX_MARGIN_PERCENT);
		margin_pct = GGL_MAX_MARGIN_PERCENT;
	}

	GyroglideState *st = calloc(1, sizeof(*st));
	if (!st)
		return NULL;

	st->capture_w = cfg->capture_w;
	st->capture_h = cfg->capture_h;
	st->vpe_channel = cfg->vpe_channel;
	st->vpe_port = cfg->vpe_port;
	st->pixels_per_radian = cfg->pixels_per_radian > 0.0f
		? cfg->pixels_per_radian : (float)cfg->capture_w / 2.0f;
	st->gain = cfg->gain > 0.0f ? cfg->gain : 1.0f;
	st->deadband_rad = cfg->deadband_rad >= 0.0f
		? cfg->deadband_rad : 0.0f;
	st->recenter_rate = cfg->recenter_rate > 0.0f
		? cfg->recenter_rate : 0.5f;
	st->max_slew_px = cfg->max_slew_px >= 0.0f
		? cfg->max_slew_px : 0.0f;
	st->bias_alpha = cfg->bias_alpha >= 0.0f
		? cfg->bias_alpha : 0.001f;
	st->test_mode = cfg->test_mode;
	st->swap_xy = cfg->swap_xy;
	st->invert_x = cfg->invert_x;
	st->invert_y = cfg->invert_y;

	uint16_t total_margin_x = align2(
		(uint16_t)(cfg->capture_w * margin_pct / 100));
	uint16_t total_margin_y = align2(
		(uint16_t)(cfg->capture_h * margin_pct / 100));

	st->margin_x = total_margin_x / 2;
	st->margin_y = total_margin_y / 2;
	st->crop_w = align2(cfg->capture_w - total_margin_x);
	st->crop_h = align2(cfg->capture_h - total_margin_y);

	/* Enforce minimum crop size (VPE stalls below ~70% of capture) */
	uint16_t min_w = align2((uint16_t)(cfg->capture_w * GGL_MIN_CROP_FRAC / 100));
	uint16_t min_h = align2((uint16_t)(cfg->capture_h * GGL_MIN_CROP_FRAC / 100));
	if (min_w < 64) min_w = 64;
	if (min_h < 64) min_h = 64;

	if (st->crop_w < min_w || st->crop_h < min_h) {
		st->crop_w = min_w > st->crop_w ? min_w : st->crop_w;
		st->crop_h = min_h > st->crop_h ? min_h : st->crop_h;
		st->margin_x = (cfg->capture_w - st->crop_w) / 2;
		st->margin_y = (cfg->capture_h - st->crop_h) / 2;
		fprintf(stderr, "EIS: crop clamped to %ux%u (margin ±%u,±%u)"
			" to stay within VPE safe range\n",
			st->crop_w, st->crop_h, st->margin_x, st->margin_y);
	}

	if (st->crop_w < 64 || st->crop_h < 64) {
		fprintf(stderr, "EIS: margin too large, crop window %ux%u\n",
			st->crop_w, st->crop_h);
		free(st);
		return NULL;
	}

	st->crop_x = st->margin_x;
	st->crop_y = st->margin_y;

	eis_ring_init(&st->ring);

	clock_gettime(CLOCK_MONOTONIC, &st->start_ts);
	st->t_prev = st->start_ts;
	st->sim_time = 0.0;

	printf("  - EIS   : gyroglide mode, margin %d%% (%u,%u px per side)\n",
		cfg->margin_percent, st->margin_x, st->margin_y);
	printf("  - EIS   : crop %ux%u within %ux%u capture\n",
		st->crop_w, st->crop_h, cfg->capture_w, cfg->capture_h);
	printf("  - EIS   : gain=%.2f, recenter=%.2f/s, deadband=%.4f rad\n",
		st->gain, st->recenter_rate, st->deadband_rad);
	printf("  - EIS   : max_slew=%.1f px, bias_alpha=%.4f, px/rad=%.0f\n",
		st->max_slew_px, st->bias_alpha, st->pixels_per_radian);
	if (st->swap_xy || st->invert_x || st->invert_y)
		printf("  - EIS   : axis: %s%s%s\n",
			st->swap_xy ? "swap-xy " : "",
			st->invert_x ? "invert-x " : "",
			st->invert_y ? "invert-y " : "");

	int ret = apply_crop(st);
	if (ret != 0) {
		fprintf(stderr, "EIS: initial MI_VPE_SetPortCrop failed %d\n", ret);
		eis_ring_destroy(&st->ring);
		free(st);
		return NULL;
	}

	/* Wrap in dispatch handle */
	EisState *handle = calloc(1, sizeof(*handle));
	if (!handle) {
		eis_ring_destroy(&st->ring);
		free(st);
		return NULL;
	}
	handle->ops = &gyroglide_ops;
	handle->ctx = st;
	return handle;
}
