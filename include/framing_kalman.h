#ifndef FRAMING_KALMAN_H
#define FRAMING_KALMAN_H

#include <stdint.h>

/* Kalman trajectory smoother — the SINGLE control law shared by the Star6E and
 * Maruko "stab"/"stab-fill" framing modules (ported from the reference Python
 * stabilizer, processVar / measVar).  It lives here, not duplicated per-SoC,
 * precisely because the four tuning constants must NOT drift between backends.
 *
 * The filter smooths the cumulative position estimate; the applied
 * compensation is the high-pass component (facc − estimate), so slow camera
 * pans pass through unchanged while fast shake is removed, and the estimate
 * eases the offset back to centre on its own (no separate EMA / gated
 * recenter).  Q (process noise / pan response) and R (measurement noise /
 * smoothness) are exposed as config knobs (video0.stab_kalman_q / _r); the
 * defaults below are what the presets seed and the fallback if a hand-edited
 * config supplies an out-of-range value. */

#define FRAMING_KALMAN_Q_DEFAULT  0.03   /* matches Python processVar */
#define FRAMING_KALMAN_R_DEFAULT  2.0    /* matches Python measVar */
/* Sane bounds for a hand-edited config (the HTTP validator enforces tighter UI
 * ranges; reset() falls back to the default outside these). */
#define FRAMING_KALMAN_Q_MIN  0.0001
#define FRAMING_KALMAN_Q_MAX  5.0
#define FRAMING_KALMAN_R_MIN  0.01
#define FRAMING_KALMAN_R_MAX  200.0

typedef struct {
	double facc_x, facc_y;   /* raw cumulative trajectory (integrated meas) */
	double x_est, y_est;     /* smoothed position estimate per axis */
	double x_p, y_p;         /* estimate uncertainty per axis */
	double q, r;             /* noise params (fixed per run, never re-written) */
} FramingKalman;

/* Reset all state (matches the Python count==0 init: facc=0, X=0, P=1) and
 * clamp q/r into sane bounds, falling back to the compile-time default when a
 * hand-edited config supplies an out-of-range value (q→0 stops tracking,
 * r→0 cancels all compensation).  Call once per stab activation (MUT_RESTART). */
void framing_kalman_reset(FramingKalman *k, double q_cfg, double r_cfg);

/* One measurement step.  Integrates (meas_dx,meas_dy) into the trajectory, then
 * runs either the pause glide-home (paused!=0; tau = glide-home frames) or the
 * Kalman predict/update, and returns the applied high-pass compensation
 * (facc − estimate) rounded and clamped to ±max per axis.  facc is NOT clamped
 * (the filter is recursive on the trajectory; clamping it creates artifacts) —
 * only the returned acc is clamped to the border budget. */
void framing_kalman_step(FramingKalman *k, double meas_dx, double meas_dy,
	int paused, uint32_t tau, int max_x, int max_y, int *acc_x, int *acc_y);

#endif /* FRAMING_KALMAN_H */
