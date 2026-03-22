#ifndef STAR6E_IQ_H
#define STAR6E_IQ_H

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize IQ parameter interface.
 *  Opens libmi_isp.so and resolves MI_ISP_IQ_* symbols.
 *  Returns 0 on success, -1 if library unavailable. */
int star6e_iq_init(void);

/** Release IQ parameter interface resources. */
void star6e_iq_cleanup(void);

/** Query all IQ parameter values.
 *  Returns malloc'd JSON string, caller frees. NULL on failure. */
char *star6e_iq_query(void);

/** Set a single IQ parameter by name.
 *  Supports dot-notation for sub-fields (e.g. "colortrans.y_ofst")
 *  and comma-separated values for arrays.
 *  Returns 0 on success, -1 on error. */
int star6e_iq_set(const char *param, const char *value);

/** Import IQ parameters from JSON string (output of star6e_iq_query).
 *  Applies all params found in the JSON via star6e_iq_set.
 *  Returns 0 if all succeeded, -1 if any failed. */
int star6e_iq_import(const char *json_str);

#ifdef __cplusplus
}
#endif

#endif /* STAR6E_IQ_H */
