#ifndef EIS_CROP_H
#define EIS_CROP_H

#include <stdint.h>
#include <time.h>

/*
 * EIS (Electronic Image Stabilization) — crop-based, 2-DOF (pan + tilt).
 *
 * A margin is reserved inside the VPE capture area.  A crop window
 * (smaller than capture by the margin) is shifted per-frame to
 * compensate for camera motion.  The VPE scaler resizes the cropped
 * region to the output resolution.
 *
 * Two sample sources:
 *   1. Simulated gyro (sine waves) — for testing without IMU
 *   2. Real IMU via eis_crop_push_sample() — from imu_bmi270 FIFO drain
 *
 * When eis_crop_set_imu_active(true), simulation is disabled and only
 * real pushed samples are consumed.
 */

typedef struct {
	int margin_percent;       /* overscan margin (default 10 = 10%) */
	uint16_t capture_w;       /* VPE capture dimensions (precrop area) */
	uint16_t capture_h;
	int vpe_channel;
	int vpe_port;
	float filter_tau;         /* LPF time constant in seconds (default 1.0) */
	float pixels_per_radian;  /* gyro-to-pixel scale (default: capture_w/2) */
	int test_mode;            /* 1 = inject visible sine wobble */
	int swap_xy;              /* 1 = swap gyro X/Y -> crop Y/X */
	int invert_x;             /* 1 = negate gyro_x */
	int invert_y;             /* 1 = negate gyro_y */
} EisCropConfig;

typedef struct {
	uint16_t crop_x, crop_y;  /* current crop window position */
	uint16_t crop_w, crop_h;  /* crop window size (constant) */
	uint16_t margin_x;        /* max displacement per axis */
	uint16_t margin_y;
	float offset_x, offset_y; /* filtered displacement in pixels */
	uint32_t update_count;    /* total eis_crop_update calls */
	uint32_t ring_count;      /* samples currently in ring */
	uint32_t last_n_samples;  /* samples used in last update */
	float raw_angle_x, raw_angle_y;
	float smooth_angle_x, smooth_angle_y;
} EisCropStatus;

typedef struct EisCropState EisCropState;

EisCropState *eis_crop_init(const EisCropConfig *cfg);

int eis_crop_update(EisCropState *st);

void eis_crop_get_status(EisCropState *st, EisCropStatus *out);

void eis_crop_push_sample(EisCropState *st, float gyro_x, float gyro_y,
	float gyro_z, const struct timespec *ts);

void eis_crop_set_imu_active(EisCropState *st, int active);

void eis_crop_destroy(EisCropState *st);

#endif
