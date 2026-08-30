#ifndef CV610_IQ_H
#define CV610_IQ_H

/* Runtime IQ control for the Hi3516CV610 ISP.
 *
 * PR #229 seeded this ISP through the IMX662 sensor plugin's
 * cmos_get_isp_default(), which the ISP reads once at pipe start.  This
 * module exposes the same blocks as live knobs, so a value can be corrected
 * without a cross-compile and a reboot.
 *
 * Unlike star6e_iq.c there is no dlopen: the CV610 backend links ss_mpi
 * directly, so the attribute structs are the SDK's own types and the field
 * table is built from offsetof() rather than hand-computed offsets. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Query every exposed IQ group.
 *  Returns malloc'd JSON, caller frees.  NULL when the ISP is not running.
 *  Values are read back from the ISP, not cached, so a rejected set shows. */
char *cv610_iq_query(void);

/** Set one IQ field.
 *  `param` is "<group>.<field>" using the names cv610_iq_query() emits;
 *  `value` is a decimal integer, or a comma-separated list for arrays.
 *  Writing a field under manual_attr also forces that group's op_type to
 *  manual — otherwise the ISP keeps running its auto curve and the set has
 *  no visible effect.
 *  Returns 0 on success, -1 on unknown name, bad value, or an MPI error. */
int cv610_iq_set(const char *param, const char *value);

/** Apply the portable AE ceilings, in the fleet-wide units.
 *  `gain` is analog sensor gain in 22.10 fixed point (1024 == 1x) and maps to
 *  exposure.auto.a_gain_max; `us` is microseconds and maps to
 *  exposure.auto.exp_time_max.  Neither needs a unit conversion -- that is
 *  why they are wired rather than approximated.
 *  0 restores the value the sensor plugin seeded, captured on the first call
 *  so that clearing a ceiling is not a one-way door.
 *  Returns 0 on success, -1 when the ISP is not running or the MPI rejects
 *  the write. */
int cv610_iq_set_gain_max(uint32_t gain);
int cv610_iq_set_shutter_max_us(uint32_t us);

/** Query the AWB loop's applied result plus the exposure that selected it.
 *  Returns malloc'd JSON, caller frees.  NULL when the ISP is not running.
 *  This is ot_isp_wb_info, not the ot_isp_wb_attr that cv610_iq_query()'s
 *  "wb" group reads back -- the attr's manual gains are the last value
 *  written, so they say nothing about where auto WB converged. */
char *cv610_awb_query(void);

#ifdef __cplusplus
}
#endif

#endif /* CV610_IQ_H */
