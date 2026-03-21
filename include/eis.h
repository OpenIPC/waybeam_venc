#ifndef EIS_H
#define EIS_H

/*
 * EIS (Electronic Image Stabilization) — modular dispatch interface.
 *
 * Pipeline code calls eis_create() / eis_update() / eis_push_sample() /
 * eis_get_status() / eis_destroy() without knowing which backend is active.
 *
 * Backends register via EisOps vtable:
 *   - "gyroglide" — GyroGlide-Lite: batched gyro integration + spring recenter
 *
 * Config "mode" field selects the backend at init time.
 * New backends can be added by implementing EisOps and registering
 * in eis.c's eis_create() factory.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Status (common to all backends) ────────────────────────────────── */

typedef struct {
	uint16_t crop_x, crop_y;       /* current crop window position */
	uint16_t crop_w, crop_h;       /* crop window size (constant) */
	uint16_t margin_x, margin_y;   /* max displacement per axis */
	float offset_x, offset_y;      /* filtered displacement in pixels */
	uint32_t update_count;         /* total update calls */
	uint32_t ring_count;           /* samples currently in ring */
	uint32_t last_n_samples;       /* samples used in last update */
	float raw_angle_x, raw_angle_y;
	float smooth_angle_x, smooth_angle_y;
} EisStatus;

/* ── Operations vtable ──────────────────────────────────────────────── */

typedef struct {
	int   (*update)(void *ctx);
	void  (*push_sample)(void *ctx, float gx, float gy, float gz,
		const struct timespec *ts);
	void  (*set_imu_active)(void *ctx, int active);
	void  (*get_status)(void *ctx, EisStatus *out);
	void  (*destroy)(void *ctx);
} EisOps;

/* ── Opaque state handle ────────────────────────────────────────────── */

typedef struct EisState {
	const EisOps *ops;
	void *ctx;   /* backend-private state */
} EisState;

/* ── Config (flat struct, all backends) ─────────────────────────────── */

typedef struct {
	const char *mode;              /* "gyroglide" (default) */

	/* Common */
	int margin_percent;            /* overscan margin (default 10) */
	uint16_t capture_w;
	uint16_t capture_h;
	int vpe_channel;
	int vpe_port;
	float pixels_per_radian;       /* 0 = auto: capture_w/2 */
	int test_mode;
	int swap_xy;
	int invert_x;
	int invert_y;

	/* GyroGlide parameters */
	float gain;                    /* correction gain 0.0–1.0 */
	float deadband_rad;            /* per-frame angle threshold */
	float recenter_rate;           /* return-to-center speed (1/s) */
	float max_slew_px;             /* max crop change per frame */
	float bias_alpha;              /* runtime bias adaptation rate */
} EisConfig;

/* ── Factory ────────────────────────────────────────────────────────── */

/*
 * eis_create — Create EIS instance with the selected backend.
 * Returns NULL on failure (prints error to stderr).
 */
EisState *eis_create(const EisConfig *cfg);

/* ── Inline dispatch wrappers ───────────────────────────────────────── */

static inline int eis_update(EisState *st)
{
	if (!st || !st->ops || !st->ops->update)
		return 0;
	return st->ops->update(st->ctx);
}

static inline void eis_push_sample(EisState *st, float gx, float gy,
	float gz, const struct timespec *ts)
{
	if (st && st->ops && st->ops->push_sample)
		st->ops->push_sample(st->ctx, gx, gy, gz, ts);
}

static inline void eis_set_imu_active(EisState *st, int active)
{
	if (st && st->ops && st->ops->set_imu_active)
		st->ops->set_imu_active(st->ctx, active);
}

static inline void eis_get_status(EisState *st, EisStatus *out)
{
	if (!st || !st->ops || !st->ops->get_status) {
		if (out)
			memset(out, 0, sizeof(*out));
		return;
	}
	st->ops->get_status(st->ctx, out);
}

static inline void eis_destroy(EisState *st)
{
	if (!st)
		return;
	if (st->ops && st->ops->destroy)
		st->ops->destroy(st->ctx);
	else
		free(st->ctx);  /* fallback: free ctx if no destroy op */
	free(st);
}

#endif /* EIS_H */
