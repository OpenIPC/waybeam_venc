/* framing_kalman.c — unified stab trajectory smoother (see framing_kalman.h).
 * Extracted verbatim from src/star6e_framing_stab.c so Star6E and Maruko share
 * one control law with no drift.  Pure math: no MI_* calls, no globals. */
#include "framing_kalman.h"

#include <math.h>

void framing_kalman_reset(FramingKalman *k, double q_cfg, double r_cfg)
{
	k->facc_x = 0.0;
	k->facc_y = 0.0;
	k->x_est = 0.0;
	k->y_est = 0.0;
	k->x_p = 1.0;
	k->y_p = 1.0;
	k->q = (q_cfg >= FRAMING_KALMAN_Q_MIN && q_cfg <= FRAMING_KALMAN_Q_MAX)
		? q_cfg : FRAMING_KALMAN_Q_DEFAULT;
	k->r = (r_cfg >= FRAMING_KALMAN_R_MIN && r_cfg <= FRAMING_KALMAN_R_MAX)
		? r_cfg : FRAMING_KALMAN_R_DEFAULT;
}

void framing_kalman_step(FramingKalman *k, double meas_dx, double meas_dy,
	int paused, uint32_t tau, int max_x, int max_y, int *acc_x, int *acc_y)
{
	int ax, ay;

	k->facc_x += meas_dx;
	k->facc_y += meas_dy;

	if (paused) {
		/* Software pause (D13): glide the applied offset to centre via the
		 * recenter ramp — undo this tick's measurement so facc decays
		 * instead of re-accumulating, then shrink facc + the estimate by
		 * (tau-1)/tau toward 0. */
		double scale;
		if (tau < 2) tau = 2;
		scale = (double)(tau - 1) / (double)tau;
		k->facc_x -= meas_dx;
		k->facc_y -= meas_dy;
		k->facc_x *= scale;
		k->facc_y *= scale;
		k->x_est *= scale;
		k->y_est *= scale;
		if (fabs(k->facc_x) < 0.5) k->facc_x = 0.0;
		if (fabs(k->facc_y) < 0.5) k->facc_y = 0.0;
	} else {
		double pp_x = k->x_p + k->q;
		double k_x  = pp_x / (pp_x + k->r);
		double pp_y = k->y_p + k->q;
		double k_y  = pp_y / (pp_y + k->r);
		k->x_est += k_x * (k->facc_x - k->x_est);
		k->x_p = (1.0 - k_x) * pp_x;
		k->y_est += k_y * (k->facc_y - k->y_est);
		k->y_p = (1.0 - k_y) * pp_y;
	}

	ax = (int)lround(k->facc_x - k->x_est);
	ay = (int)lround(k->facc_y - k->y_est);
	if (ax < -max_x) ax = -max_x;
	if (ax >  max_x) ax =  max_x;
	if (ay < -max_y) ay = -max_y;
	if (ay >  max_y) ay =  max_y;
	*acc_x = ax;
	*acc_y = ay;
}
