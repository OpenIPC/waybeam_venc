#ifndef STAR6E_RUNTIME_H
#define STAR6E_RUNTIME_H

#include "backend.h"

/* Comm name for the watchdog fork.  Must differ from "waybeam" —
 * that is what is_another_waybeam_running() in main.c matches
 * against, and a duplicate match would abort the watchdog with the
 * "already running" banner.  (The respawn child's comm name lives
 * in venc_respawn.c.) */
#define VENC_COMM_WATCHDOG  "waybeam-wd"

/** Return the Star6E backend operations table. */
const BackendOps *star6e_runtime_backend_ops(void);

/* Attitude HTTP hooks (httpd thread). Query returns malloc'd JSON
 * ({"valid":false} until the estimator runs — needs attitude.enabled +
 * imu.enabled). Calibrate blocks ≤3 s collecting accel while the camera
 * is held level, then solves attitude.trimRollDeg/trimPitchDeg; 0 on
 * success, -1 on timeout or implausible gravity. */
char *star6e_attitude_query(void);
int star6e_attitude_calibrate_level(const VencConfig *vcfg,
	float *roll_deg, float *pitch_deg);

#endif /* STAR6E_RUNTIME_H */
