#ifndef VENC_API_H
#define VENC_API_H

#include "venc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Callback table for applying live settings changes.
 * Each function returns 0 on success, -1 on error.
 * NULL entries mean the operation is not supported. */
typedef struct {
	int (*apply_bitrate)(uint32_t kbps);
	int (*apply_fps)(uint32_t fps);
	int (*apply_gop)(uint32_t gop_size);
	int (*apply_qp_delta)(int delta);
	int (*apply_roi_qp)(int qp);
	int (*apply_gain_max)(uint32_t gain);
	int (*apply_verbose)(bool on);
	int (*apply_output_enabled)(bool on);
	int (*apply_server)(const char *uri);
	int (*apply_mute)(bool on);
	int (*request_idr)(void);
	/* Live output FPS query. Returns 0 if unavailable. */
	uint32_t (*query_live_fps)(void);
	/* AE query: returns malloc'd JSON string, caller frees. NULL if unsupported. */
	char *(*query_ae_info)(void);
	/* AWB query: returns malloc'd JSON string, caller frees. NULL if unsupported. */
	char *(*query_awb_info)(void);
	/* ISP metrics: returns malloc'd text string, caller frees. NULL if unsupported. */
	char *(*query_isp_metrics)(void);
	/* AWB mode: 0=auto, 1=ct_manual. Returns 0 on success. */
	int (*apply_awb_mode)(int mode, uint32_t ct);
	/* IQ query: returns malloc'd JSON string, caller frees. NULL if unsupported. */
	char *(*query_iq_info)(void);
	/* IQ set: param name + value string. Returns 0 on success, -1 on error. */
	int (*apply_iq_param)(const char *param, const char *value);
	/* Live-update the RTP/compact max packet payload size in bytes.
	 * The validator caps requests at VENC_OUTPUT_PAYLOAD_CEILING_BYTES
	 * and SHM ring slots are sized to that ceiling at startup, so any
	 * value reaching the callback fits every transport — backends do
	 * not need to reject size-based requests. Returns 0 on success,
	 * -1 if the backend has no live state to mutate. */
	int (*apply_max_payload_size)(uint16_t size);
} VencApplyCallbacks;

/* Register all API routes with the httpd.
 * cfg points to the live config (read for GET, modified by SET).
 * backend_name is "star6e" or "maruko" (used in /api/v1/version).
 * cb may be NULL (set endpoints will return not_implemented). */
int venc_api_register(VencConfig *cfg, const char *backend_name,
	const VencApplyCallbacks *cb);

/* Register the config path so restart-required sets can persist to disk
 * before triggering reinit.  Pass NULL to disable save-on-restart. */
void venc_api_set_config_path(const char *path);

/* Return 1 if a config field is supported on the named backend.
 * field_key may be canonical (video0.scene_threshold) or an accepted
 * alias (video0.sceneThreshold). */
int venc_api_field_supported_for_backend(const char *backend_name,
	const char *field_key);

/* Run the same per-field validation that /api/v1/set applies, over an
 * already-parsed config (e.g. just loaded from disk).  Returns NULL when
 * every validated field is in range, or a static error message string
 * pointing at the first offender.  Used by venc_config_load() to refuse
 * boot on configs that would crash MI_VENC_CreateChn or similar. */
const char *venc_api_validate_loaded_config(const VencConfig *cfg);

/* Pipeline reinit request flag (shared between API and backend).
 * One path: full teardown + reload from disk + start.  SIGHUP,
 * /api/v1/restart, /api/v1/defaults, and MUT_RESTART /api/v1/set
 * all enqueue the same request. */
void venc_api_request_reinit(void);
bool venc_api_get_reinit(void);
void venc_api_clear_reinit(void);

/* Record control flags (set by HTTP thread, consumed by main loop). */
void venc_api_request_record_start(const char *dir);
void venc_api_request_record_stop(void);

/* Returns 1 if start requested, copies dir into buf. */
int  venc_api_get_record_start(char *buf, size_t buf_size);
/* Returns 1 if stop requested, clears the flag. */
int  venc_api_get_record_stop(void);

/* Copy the configured recording directory (record.dir) into buf, falling
 * back to RECORDER_DEFAULT_DIR when unset.  Buf is always NUL-terminated
 * (truncated if needed). */
void venc_api_get_record_dir(char *buf, size_t buf_size);

/* Dual VENC channel API -- live controls for ch1 (dual-stream mode).
 * Call venc_api_dual_register() after pipeline_start_dual() to enable
 * the dual endpoints.  Call venc_api_dual_unregister() on
 * pipeline stop to disable them. */
void venc_api_dual_register(int channel, uint32_t bitrate, uint32_t fps,
	uint32_t gop, bool frame_lost);
void venc_api_dual_unregister(void);

/* Sensor info — set by backend after sensor_select() to expose via /api/v1/modes. */
void venc_api_set_sensor_info(int pad, int mode_index, int forced_pad);

/* Active VIF precrop rectangle — set by backend whenever it programs the
 * VIF capture region (initial start and reinit).  Includes any sensor
 * overscan offsets so the rectangle reflects exactly what is in hardware.
 * Exposed in /api/v1/config (runtime block) and /api/v1/ae (Star6E only). */
void venc_api_set_active_precrop(uint16_t x, uint16_t y,
	uint16_t w, uint16_t h);
void venc_api_clear_active_precrop(void);
int  venc_api_get_active_precrop(uint16_t *x, uint16_t *y,
	uint16_t *w, uint16_t *h);

/* Record status callback — set by backend to expose status to HTTP API. */
typedef struct {
	int active;
	uint64_t bytes_written;
	uint32_t frames_written;
	uint32_t segments;
	char path[256];
	char stop_reason[32];
	char format[16];
} VencRecordStatus;

typedef void (*VencRecordStatusFn)(VencRecordStatus *out);
void venc_api_set_record_status_fn(VencRecordStatusFn fn);

/* Fill the supplied struct with live record status via the registered
 * callback.  When no callback is set, zeroes the struct. */
void venc_api_fill_record_status(VencRecordStatus *out);

#ifdef __cplusplus
}
#endif

#endif /* VENC_API_H */
