#ifndef MARUKO_IQ_H
#define MARUKO_IQ_H

/** Initialize Maruko IQ parameter system.
 *  Opens libmi_isp.so and resolves MI_ISP_IQ_* symbols.
 *  Returns 0 on success, -1 on failure. */
int maruko_iq_init(void);

/** Release resources. */
void maruko_iq_cleanup(void);

/** Query all IQ parameters as a JSON string (caller frees). */
char *maruko_iq_query(void);

/** Set a single IQ parameter. Supports dot-notation for fields
 *  (e.g. "colortrans.y_ofst"). Returns 0 on success. */
int maruko_iq_set(const char *param, const char *value);

/** Import a JSON blob (output of maruko_iq_query). Returns 0 on success. */
int maruko_iq_import(const char *json_str);

#endif /* MARUKO_IQ_H */
