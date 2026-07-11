#ifndef ATTITUDE_EST_H
#define ATTITUDE_EST_H

/*
 * Complementary-filter attitude estimator (roll/pitch/yaw from a 6-axis
 * IMU sample stream). Feeds the rtp_sidecar ATTITUDE trailer
 * (protocols/rtp-sidecar.md).
 *
 * Convention (sensor frame, aerospace-style): x forward, y right,
 * z down. Roll about x (right-wing-down positive), pitch about y
 * (nose-up positive), yaw about z. Roll/pitch are gravity-referenced
 * via the accelerometer; yaw is pure gyro integration and drifts —
 * relative heading only. Physical mounting differences are corrected
 * by the caller (attitude.mount_deg / invert flags), not here.
 *
 * Pure C, no SDK/IMU dependencies — unit-testable on the host.
 */

#include <stdint.h>

typedef struct {
	float roll_rad;
	float pitch_rad;
	float yaw_rad;
	float tc_s;           /* complementary time constant (s)          */
	uint64_t last_us;     /* previous sample timestamp, 0 = no sample */
	uint32_t samples;     /* processed sample count                   */
	int have_init;        /* first accel snapped roll/pitch           */
} AttitudeEst;

#define ATTITUDE_EST_DEFAULT_TC_S     2.0f
/* Settled once this many samples are in (1 s at the 200 Hz default ODR). */
#define ATTITUDE_EST_SETTLE_SAMPLES   200u

/* tc_s <= 0 selects ATTITUDE_EST_DEFAULT_TC_S. */
void attitude_est_init(AttitudeEst *e, float tc_s);

/*
 * Feed one IMU sample. gyro in rad/s, accel in m/s^2, ts_us monotonic.
 * The first sample with a plausible gravity vector snaps roll/pitch;
 * subsequent samples integrate gyro and lean toward the accel gravity
 * reference with the configured time constant. Out-of-order or
 * far-future timestamps clamp dt to keep the filter sane.
 */
void attitude_est_update(AttitudeEst *e,
	float gx, float gy, float gz,
	float ax, float ay, float az,
	uint64_t ts_us);

/* 1 once the settle window has passed and roll/pitch are trustworthy. */
int attitude_est_settled(const AttitudeEst *e);

/* 1 while the estimator is initialised and its state is finite. Callers
 * that stamp a "valid" flag on the wire should gate it on this so a
 * corrupt-sample-poisoned filter never advertises a bogus level attitude. */
int attitude_est_healthy(const AttitudeEst *e);

/*
 * Sensor→camera axis remap, applied to gyro and accel vectors BEFORE
 * attitude_est_update. Two parts, composed into one 3x3 matrix:
 *
 *  1. Signed permutation from two axis names ("+x".."-z"): fwd = the
 *     sensor axis pointing out of the lens, down = the sensor axis
 *     pointing down when the camera is level; right is derived
 *     (right = down × fwd). Covers all 24 axis-aligned mountings.
 *  2. Fine boresight trim for a board tilted inside the housing:
 *     trim_roll/pitch_deg = the roll/pitch the estimator READS while
 *     the camera is held level (the "lay it flat" calibration values).
 *     Corrected input-side as R_y(pitch)·R_x(roll) — exact, unlike an
 *     output-side Euler subtraction which cross-couples at large tilt.
 *
 * The output-side attitude.mount_deg / invert trims remain for sign
 * fixes only.
 */
typedef struct {
	float r[3][3];   /* v_cam = r · v_sensor */
} AttitudeAxisMap;

/* -1 on unparseable names or non-orthogonal fwd/down (identity kept,
 * trims still applied). Trim angles in degrees. */
int attitude_axis_map_init(AttitudeAxisMap *m,
	const char *fwd, const char *down,
	float trim_roll_deg, float trim_pitch_deg);

void attitude_axis_map_apply(const AttitudeAxisMap *m,
	float in_x, float in_y, float in_z, float out[3]);

/*
 * Solve the level-pose boresight trims from a time-averaged raw accel
 * vector (m/s², sensor frame) captured while the camera is held level:
 * the returned trims are exactly what attitude_axis_map_init expects.
 * -1 on invalid axes or implausible gravity magnitude (<0.5 g / >1.5 g
 * — device moving or IMU misconfigured).
 */
int attitude_axis_map_solve_trims(const char *fwd, const char *down,
	float ax, float ay, float az,
	float *trim_roll_deg, float *trim_pitch_deg);

/* Angles in 0.1° units, clamped to int16 range. */
int16_t attitude_est_roll_cdeg(const AttitudeEst *e);
int16_t attitude_est_pitch_cdeg(const AttitudeEst *e);
int16_t attitude_est_yaw_cdeg(const AttitudeEst *e);

#endif /* ATTITUDE_EST_H */
