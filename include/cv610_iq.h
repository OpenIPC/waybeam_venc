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
