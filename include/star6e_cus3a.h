#ifndef STAR6E_CUS3A_H
#define STAR6E_CUS3A_H

#include <stdint.h>

/** Supervisory AE configuration.
 *  The thread enforces FPV constraints (gain cap, shutter cap) via
 *  SetExposureLimit while the ISP's internal AE handles convergence. */
typedef struct {
	uint32_t sensor_fps;       /* sensor output fps (for max shutter calc) */
	uint32_t ae_fps;           /* monitoring rate in Hz (default 15) */
	uint32_t shutter_max_us;   /* 0 = auto from sensor_fps */
	uint32_t gain_max;         /* 0 = use ISP bin default */
	int      verbose;          /* enable periodic status logging */
} Star6eCus3aConfig;

/** Fill config with sensible FPV defaults. */
void star6e_cus3a_config_defaults(Star6eCus3aConfig *cfg);

/**
 * Start the supervisory AE thread.
 *
 * The ISP's internal AE stays in NORMAL state.  This thread monitors
 * HW stats and enforces gain/shutter caps via SetExposureLimit.
 * Call after ISP bin is loaded and CUS3A has been enabled (1,1,1).
 *
 * Returns 0 on success, -1 on error.
 */
int star6e_cus3a_start(const Star6eCus3aConfig *cfg);

/**
 * Stop the supervisory AE thread.
 * Safe to call if the thread was never started.
 */
void star6e_cus3a_stop(void);

/** Signal the thread to stop (non-blocking).  Call join() later. */
void star6e_cus3a_request_stop(void);

/** Wait for the thread to exit after request_stop(). */
void star6e_cus3a_join(void);

/** Return 1 if the supervisory AE thread is running. */
int star6e_cus3a_running(void);

/** Update the max sensor gain at runtime.
 *  Called when the user changes isp.gainMax via API. */
void star6e_cus3a_set_gain_max(uint32_t gain);

#endif /* STAR6E_CUS3A_H */
