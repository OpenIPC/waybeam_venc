#ifndef STAR6E_CUS3A_H
#define STAR6E_CUS3A_H

#include <stdint.h>

/** Supervisory AE configuration.
 *  This thread is a pure exposure-limit enforcer: it re-asserts the user's
 *  gain/shutter min/max on the ISP exposure limit each tick while the ISP's
 *  internal (SDK firmware / bin) AE handles convergence.  It is the ONLY AE
 *  path on Star6E — the historical aeEngine=custom userspace governor (with
 *  its own fps-derived shutter cap and cold-boot fps kick) was retired; the
 *  pipeline owns cold-boot fps recovery.  See star6e_runtime.c. */
typedef struct {
	uint32_t ae_fps;           /* monitoring rate in Hz (default 15) */
	uint32_t shutter_max_us;   /* max exposure ceiling µs (0 = bin default) */
	int      shutter_pin;      /* pin minShutter==maxShutter (180° rule) */
	uint32_t gain_max;         /* 0 = use ISP bin default */
	uint32_t shutter_min_us;   /* min exposure floor µs (0 = bin default;
	                            * ignored while shutter_pin is set) */
	uint32_t gain_min;         /* min sensor gain floor (0 = bin default) */
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

/** Update the max shutter (exposure) at runtime.
 *  Called when the user changes isp.shutterMaxUs via API. */
void star6e_cus3a_set_shutter_max(uint32_t us);

/** Update the min sensor gain floor at runtime.
 *  Called when the user changes isp.gainMin via API. */
void star6e_cus3a_set_gain_min(uint32_t gain);

/** Update the min shutter (exposure) floor at runtime.
 *  Called when the user changes isp.shutterMinUs via API. */
void star6e_cus3a_set_shutter_min(uint32_t us);

#endif /* STAR6E_CUS3A_H */
