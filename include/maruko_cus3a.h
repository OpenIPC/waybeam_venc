#ifndef MARUKO_CUS3A_H
#define MARUKO_CUS3A_H

#include <stdint.h>

/** Supervisory + optional throttle thread for Maruko AE.
 *
 *  Two operating modes are supported, chosen by `throttle_mode`:
 *
 *  - throttle_mode == 0 (native): the SDK's NATIVE AE/AWB algorithms run
 *    inside `3A_Proc_0` at sensor frame rate.  This thread just enforces
 *    gain/shutter caps via MI_ISP_AE_SetExposureLimit and reads stats for
 *    verbose logging.  Equivalent to Star6E's pattern.
 *
 *  - throttle_mode == 1 (throttle): caller has installed a no-op AE
 *    adaptor (see maruko_cus3a_install_noop_adaptor) so the SDK's NATIVE
 *    AE algorithm is bypassed.  This thread drives AE manually at
 *    `ae_fps` Hz via MI_ISP_CUS3A_SetAeParam.  AWB stays NATIVE for
 *    correct white balance.  Saves ~24% of one Cortex-A7 core at 120 fps. */
typedef struct {
	uint32_t sensor_fps;       /* sensor output fps (for max shutter calc) */
	uint32_t ae_fps;           /* monitoring rate in Hz (default 15) */
	uint32_t shutter_max_us;   /* 0 = auto from sensor_fps */
	uint32_t gain_max;         /* 0 = use ISP bin default */
	int      verbose;          /* enable periodic status logging */
	int      throttle_mode;    /* 0 = native (default), 1 = throttle */
} MarukoCus3aConfig;

/** Fill config with sensible FPV defaults (sensor_fps=120, ae_fps=15). */
void maruko_cus3a_config_defaults(MarukoCus3aConfig *cfg);

/**
 * Start the pacing thread.
 *
 * Pre-conditions:
 *   - libmi_isp.so + libcus3a.so already opened by maruko_mi_init
 *   - MI_ISP_CUS3A_Enable(p100, p110) already called
 *   - MI_ISP_EnableUserspace3A called (so 3A_Proc_0 pumps IQ→HW)
 *   - CUS3A_SetRunMode(E_CUS3A_MODE_OFF) called (parks 3A_Proc_0)
 *   - ISP bin loaded
 *
 * Returns 0 on success, -1 on error.
 */
int maruko_cus3a_start(const MarukoCus3aConfig *cfg);

/** Throttle-mode only: install a no-op AE adaptor as ADAPTOR_1 and switch
 *  the active AE adaptor to it.  After this call the SDK's 3A_Proc_0
 *  thread runs a stub for AE (Change=0 every frame) instead of the
 *  native AE algorithm.  AWB remains on NATIVE so white balance still
 *  tracks the scene.  The supervisory thread then drives AE via
 *  MI_ISP_CUS3A_SetAeParam at ae_fps Hz.
 *
 *  Must be called AFTER MI_ISP_CUS3A_Enable + MI_ISP_EnableUserspace3A
 *  and BEFORE the first frame.  Has no effect (and is unnecessary) in
 *  native mode. */
void maruko_cus3a_install_noop_adaptor(void);

/** Stop and join the pacing thread. Safe if never started. */
void maruko_cus3a_stop(void);

/** Signal the thread to stop (non-blocking). Call join() later. */
void maruko_cus3a_request_stop(void);

/** Wait for the thread to exit after request_stop(). */
void maruko_cus3a_join(void);

/** Return 1 if the pacing thread is running. */
int maruko_cus3a_running(void);

/** Update the max sensor gain at runtime. */
void maruko_cus3a_set_gain_max(uint32_t gain);

#endif /* MARUKO_CUS3A_H */
