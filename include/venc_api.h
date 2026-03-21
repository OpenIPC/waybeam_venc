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
	int (*apply_exposure)(uint32_t us);
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
} VencApplyCallbacks;

/* Register all API routes with the httpd.
 * cfg points to the live config (read for GET, modified by SET).
 * backend_name is "star6e" or "maruko" (used in /api/v1/version).
 * cb may be NULL (set endpoints will return not_implemented). */
int venc_api_register(VencConfig *cfg, const char *backend_name,
	const VencApplyCallbacks *cb);

/* Pipeline reinit request flag (shared between API and backend).
 *   0 = no reinit pending
 *   1 = reload config from disk + reinit pipeline (SIGHUP / /api/v1/restart)
 *   2 = reinit pipeline with current in-memory config (API field change) */
void venc_api_request_reinit(int mode);
int  venc_api_get_reinit(void);
void venc_api_clear_reinit(void);

/* Record control flags (set by HTTP thread, consumed by main loop). */
void venc_api_request_record_start(const char *dir);
void venc_api_request_record_stop(void);

/* Returns 1 if start requested, copies dir into buf. */
int  venc_api_get_record_start(char *buf, size_t buf_size);
/* Returns 1 if stop requested, clears the flag. */
int  venc_api_get_record_stop(void);

/* Dual VENC channel API -- live controls for ch1 (dual-stream mode).
 * Call venc_api_dual_register() after pipeline_start_dual() to enable
 * the dual endpoints.  Call venc_api_dual_unregister() on
 * pipeline stop to disable them. */
void venc_api_dual_register(int channel, uint32_t bitrate, uint32_t fps,
	uint32_t gop);
void venc_api_dual_unregister(void);

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

#ifdef __cplusplus
}
#endif

#endif /* VENC_API_H */
