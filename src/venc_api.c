#include "venc_api.h"
#include "pipeline_common.h"
#include "sensor_select.h"
#include "star6e_recorder.h"
#include "venc_httpd.h"
#include "venc_webui.h"
#include "cJSON.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Shared state (set by venc_api_register) ─────────────────────────── */

static VencConfig *g_cfg;
static const VencApplyCallbacks *g_cb;
static char g_backend[32];
static int g_api_routes_registered = 0;

/* Mutex protecting g_cfg field access from the httpd thread.
 * All handle_set/handle_get calls run on the httpd pthread; the main
 * streaming thread reads config fields concurrently.  This mutex
 * serializes field reads/writes to prevent torn values on ARM. */
static pthread_mutex_t g_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Sensor info (set by backend after sensor_select) ─────────────────── */

static int g_sensor_pad = -1;
static int g_sensor_mode = -1;
static int g_sensor_forced_pad = -1;

void venc_api_set_sensor_info(int pad, int mode_index, int forced_pad)
{
	pthread_mutex_lock(&g_cfg_mutex);
	g_sensor_pad = pad;
	g_sensor_mode = mode_index;
	g_sensor_forced_pad = forced_pad;
	pthread_mutex_unlock(&g_cfg_mutex);
}

/* ── Reinit flag (shared with backend via accessors) ─────────────────── */

static volatile sig_atomic_t g_reinit = 0;

/* ── Record control flags ────────────────────────────────────────────── */

static volatile sig_atomic_t g_record_start_pending = 0;
static volatile sig_atomic_t g_record_stop_pending = 0;
static char g_record_start_dir[256];
static pthread_mutex_t g_record_mutex = PTHREAD_MUTEX_INITIALIZER;
static VencRecordStatusFn g_record_status_fn;

void venc_api_request_reinit(int mode)
{
	/* Don't downgrade: mode 1 (reload) takes priority over mode 2 (in-memory) */
	if (mode > g_reinit)
		g_reinit = mode;
}

int venc_api_get_reinit(void)
{
	return (int)g_reinit;
}

void venc_api_clear_reinit(void)
{
	g_reinit = 0;
}

void venc_api_request_record_start(const char *dir)
{
	pthread_mutex_lock(&g_record_mutex);
	snprintf(g_record_start_dir, sizeof(g_record_start_dir), "%s",
		dir ? dir : RECORDER_DEFAULT_DIR);
	g_record_start_pending = 1;
	g_record_stop_pending = 0;
	pthread_mutex_unlock(&g_record_mutex);
}

void venc_api_request_record_stop(void)
{
	pthread_mutex_lock(&g_record_mutex);
	g_record_stop_pending = 1;
	g_record_start_pending = 0;
	pthread_mutex_unlock(&g_record_mutex);
}

int venc_api_get_record_start(char *buf, size_t buf_size)
{
	int pending;

	pthread_mutex_lock(&g_record_mutex);
	pending = g_record_start_pending;
	if (pending && buf && buf_size > 0)
		snprintf(buf, buf_size, "%s", g_record_start_dir);
	g_record_start_pending = 0;
	pthread_mutex_unlock(&g_record_mutex);
	return pending;
}

int venc_api_get_record_stop(void)
{
	int pending;

	pthread_mutex_lock(&g_record_mutex);
	pending = g_record_stop_pending;
	g_record_stop_pending = 0;
	pthread_mutex_unlock(&g_record_mutex);
	return pending;
}

void venc_api_set_record_status_fn(VencRecordStatusFn fn)
{
	g_record_status_fn = fn;
}

/* ── Field descriptor table ──────────────────────────────────────────── */

typedef enum { MUT_LIVE, MUT_RESTART } Mutability;
typedef enum { FT_BOOL, FT_INT, FT_UINT, FT_UINT8, FT_UINT16, FT_DOUBLE, FT_FLOAT, FT_STRING, FT_SIZE } FieldType;

typedef struct {
	const char *key;          /* dot-separated JSON path, e.g. "video0.bitrate" */
	FieldType type;
	Mutability mut;
	size_t offset;            /* offsetof into VencConfig */
	size_t size;              /* sizeof the field (for strings) */
} FieldDesc;

#define FIELD(section, member, ft, m) \
	{ #section "." #member, ft, m, \
	  offsetof(VencConfig, section.member), \
	  sizeof(((VencConfig*)0)->section.member) }

static const FieldDesc g_fields[] = {
	FIELD(system, web_port,        FT_UINT16, MUT_RESTART),
	FIELD(system, overclock_level, FT_INT,    MUT_RESTART),
	FIELD(system, verbose,         FT_BOOL,   MUT_LIVE),

	FIELD(sensor, index,           FT_INT,    MUT_RESTART),
	FIELD(sensor, mode,            FT_INT,    MUT_RESTART),
	FIELD(sensor, unlock_enabled,  FT_BOOL,   MUT_RESTART),
	FIELD(sensor, unlock_cmd,      FT_UINT,   MUT_RESTART),
	FIELD(sensor, unlock_reg,      FT_UINT16, MUT_RESTART),
	FIELD(sensor, unlock_value,    FT_UINT16, MUT_RESTART),
	FIELD(sensor, unlock_dir,      FT_INT,    MUT_RESTART),

	FIELD(isp, sensor_bin,         FT_STRING, MUT_RESTART),
	FIELD(isp, gain_max,           FT_UINT,   MUT_LIVE),
	FIELD(isp, awb_mode,           FT_STRING, MUT_LIVE),
	FIELD(isp, awb_ct,             FT_UINT,   MUT_LIVE),

	FIELD(image, mirror,           FT_BOOL,   MUT_RESTART),
	FIELD(image, flip,             FT_BOOL,   MUT_RESTART),
	FIELD(image, rotate,           FT_INT,    MUT_RESTART),

	FIELD(video0, codec,           FT_STRING, MUT_RESTART),
	FIELD(video0, rc_mode,         FT_STRING, MUT_RESTART),
	FIELD(video0, fps,             FT_UINT,   MUT_LIVE),
	{ "video0.size", FT_SIZE, MUT_RESTART,
	  offsetof(VencConfig, video0.width),
	  sizeof(uint32_t) * 2 },  /* covers width + height */
	FIELD(video0, bitrate,         FT_UINT,   MUT_LIVE),
	FIELD(video0, gop_size,        FT_DOUBLE, MUT_LIVE),
	FIELD(video0, qp_delta,        FT_INT,    MUT_LIVE),
	FIELD(video0, frame_lost,      FT_BOOL,   MUT_RESTART),
	FIELD(outgoing, enabled,           FT_BOOL,   MUT_LIVE),
	FIELD(outgoing, server,            FT_STRING, MUT_LIVE),
	FIELD(outgoing, stream_mode,       FT_STRING, MUT_RESTART),
	FIELD(outgoing, max_payload_size,  FT_UINT16, MUT_RESTART),
	FIELD(outgoing, connected_udp,     FT_BOOL,   MUT_RESTART),
	FIELD(outgoing, audio_port,        FT_UINT16, MUT_RESTART),
	FIELD(outgoing, sidecar_port,      FT_UINT16, MUT_RESTART),

	FIELD(isp, legacy_ae,      FT_BOOL,   MUT_RESTART),
	FIELD(isp, ae_fps,         FT_UINT,   MUT_RESTART),

	FIELD(audio, enabled,      FT_BOOL,   MUT_RESTART),
	FIELD(audio, sample_rate,  FT_UINT,   MUT_RESTART),
	FIELD(audio, channels,     FT_UINT,   MUT_RESTART),
	FIELD(audio, codec,        FT_STRING, MUT_RESTART),
	FIELD(audio, volume,       FT_INT,    MUT_RESTART),
	FIELD(audio, mute,         FT_BOOL,   MUT_LIVE),

	FIELD(fpv, roi_enabled,  FT_BOOL,   MUT_LIVE),
	FIELD(fpv, roi_qp,       FT_INT,    MUT_LIVE),
	FIELD(fpv, roi_steps,    FT_UINT16, MUT_LIVE),
	FIELD(fpv, roi_center,   FT_DOUBLE, MUT_LIVE),
	FIELD(fpv, noise_level,  FT_INT,    MUT_RESTART),

	FIELD(imu, enabled,        FT_BOOL,   MUT_RESTART),
	FIELD(imu, i2c_device,     FT_STRING, MUT_RESTART),
	FIELD(imu, i2c_addr,       FT_UINT8,  MUT_RESTART),
	FIELD(imu, sample_rate_hz, FT_INT,    MUT_RESTART),
	FIELD(imu, gyro_range_dps, FT_INT,    MUT_RESTART),
	FIELD(imu, cal_file,       FT_STRING, MUT_RESTART),
	FIELD(imu, cal_samples,    FT_INT,    MUT_RESTART),

	FIELD(eis, enabled,        FT_BOOL,   MUT_RESTART),
	FIELD(eis, mode,           FT_STRING, MUT_RESTART),
	FIELD(eis, margin_percent, FT_INT,    MUT_RESTART),
	FIELD(eis, test_mode,      FT_BOOL,   MUT_RESTART),
	FIELD(eis, swap_xy,        FT_BOOL,   MUT_RESTART),
	FIELD(eis, invert_x,       FT_BOOL,   MUT_RESTART),
	FIELD(eis, invert_y,       FT_BOOL,   MUT_RESTART),
	FIELD(eis, gain,           FT_FLOAT,  MUT_RESTART),
	FIELD(eis, deadband_rad,   FT_FLOAT,  MUT_RESTART),
	FIELD(eis, recenter_rate,  FT_FLOAT,  MUT_RESTART),
	FIELD(eis, max_slew_px,    FT_FLOAT,  MUT_RESTART),
	FIELD(eis, bias_alpha,     FT_FLOAT,  MUT_RESTART),

	FIELD(record, enabled,     FT_BOOL,   MUT_RESTART),
	FIELD(record, dir,         FT_STRING, MUT_RESTART),
	FIELD(record, format,      FT_STRING, MUT_RESTART),
	FIELD(record, mode,        FT_STRING, MUT_RESTART),
	FIELD(record, max_seconds, FT_UINT,   MUT_RESTART),
	FIELD(record, max_mb,      FT_UINT,   MUT_RESTART),
	FIELD(record, bitrate,     FT_UINT,   MUT_RESTART),
	FIELD(record, fps,         FT_UINT,   MUT_RESTART),
	FIELD(record, gop_size,    FT_DOUBLE, MUT_RESTART),
	FIELD(record, server,      FT_STRING, MUT_RESTART),
	FIELD(video0, scene_threshold,  FT_UINT16, MUT_RESTART),
	FIELD(video0, scene_holdoff,   FT_UINT8,  MUT_RESTART),
	FIELD(debug,  show_osd,    FT_BOOL,   MUT_RESTART),
};

#define FIELD_COUNT (sizeof(g_fields) / sizeof(g_fields[0]))

static const FieldDesc *find_field(const char *key)
{
	for (size_t i = 0; i < FIELD_COUNT; i++) {
		if (strcmp(g_fields[i].key, key) == 0)
			return &g_fields[i];
	}
	return NULL;
}

typedef struct {
	const char *alias;
	const char *canonical;
} FieldAlias;

static const FieldAlias g_field_aliases[] = {
	{ "system.webPort", "system.web_port" },
	{ "system.overclockLevel", "system.overclock_level" },
	{ "sensor.unlockEnabled", "sensor.unlock_enabled" },
	{ "sensor.unlockCmd", "sensor.unlock_cmd" },
	{ "sensor.unlockReg", "sensor.unlock_reg" },
	{ "sensor.unlockValue", "sensor.unlock_value" },
	{ "sensor.unlockDir", "sensor.unlock_dir" },
	{ "isp.sensorBin", "isp.sensor_bin" },
	{ "isp.gainMax", "isp.gain_max" },
	{ "isp.awbMode", "isp.awb_mode" },
	{ "isp.awbCt", "isp.awb_ct" },
	{ "video0.rcMode", "video0.rc_mode" },
	{ "video0.gopSize", "video0.gop_size" },
	{ "video0.qpDelta", "video0.qp_delta" },
	{ "video0.frameLost", "video0.frame_lost" },
	{ "outgoing.maxPayloadSize", "outgoing.max_payload_size" },
	{ "outgoing.audioPort", "outgoing.audio_port" },
	{ "fpv.roiEnabled", "fpv.roi_enabled" },
	{ "fpv.roiQp", "fpv.roi_qp" },
	{ "fpv.roiSteps", "fpv.roi_steps" },
	{ "fpv.roiCenter", "fpv.roi_center" },
	{ "fpv.noiseLevel", "fpv.noise_level" },
	{ "isp.legacyAe", "isp.legacy_ae" },
	{ "isp.aeFps", "isp.ae_fps" },
	{ "audio.sampleRate", "audio.sample_rate" },
	{ "imu.i2cDevice", "imu.i2c_device" },
	{ "imu.i2cAddr", "imu.i2c_addr" },
	{ "imu.sampleRateHz", "imu.sample_rate_hz" },
	{ "imu.gyroRangeDps", "imu.gyro_range_dps" },
	{ "imu.calFile", "imu.cal_file" },
	{ "imu.calSamples", "imu.cal_samples" },
	{ "eis.marginPercent", "eis.margin_percent" },
	{ "eis.testMode", "eis.test_mode" },
	{ "eis.swapXY", "eis.swap_xy" },
	{ "eis.invertX", "eis.invert_x" },
	{ "eis.invertY", "eis.invert_y" },
	{ "eis.deadbandRad", "eis.deadband_rad" },
	{ "eis.recenterRate", "eis.recenter_rate" },
	{ "eis.maxSlewPx", "eis.max_slew_px" },
	{ "eis.biasAlpha", "eis.bias_alpha" },
	{ "record.maxSeconds", "record.max_seconds" },
	{ "record.maxMB", "record.max_mb" },
	{ "record.gopSize", "record.gop_size" },
	{ "video0.sceneThreshold", "video0.scene_threshold" },
	{ "video0.sceneHoldoff", "video0.scene_holdoff" },
	{ "outgoing.sidecarPort", "outgoing.sidecar_port" },
	{ "outgoing.connectedUdp", "outgoing.connected_udp" },
	{ "outgoing.streamMode", "outgoing.stream_mode" },
	{ "debug.showOsd", "debug.show_osd" },
};

static const char *canonicalize_field_key(const char *key)
{
	if (!key)
		return NULL;

	for (size_t i = 0; i < sizeof(g_field_aliases) / sizeof(g_field_aliases[0]); i++) {
		if (strcmp(g_field_aliases[i].alias, key) == 0)
			return g_field_aliases[i].canonical;
	}

	return key;
}

int venc_api_field_supported_for_backend(const char *backend_name,
	const char *field_key)
{
	const char *canonical_key;

	(void)backend_name;
	canonical_key = canonicalize_field_key(field_key);
	if (!canonical_key)
		return 0;

	return 1;
}

/* ── Field value helpers ─────────────────────────────────────────────── */

/* Format a field value as a JSON fragment string (caller must free).
 * Uses cJSON for strings to ensure proper escaping of special chars. */
static char *field_to_json_value_from_cfg(const VencConfig *cfg,
	const FieldDesc *f)
{
	const void *ptr;
	char buf[320];

	if (!cfg || !f)
		return strdup("null");

	ptr = (const char *)cfg + f->offset;
	switch (f->type) {
	case FT_BOOL:
		snprintf(buf, sizeof(buf), "%s", *(const bool *)ptr ? "true" : "false");
		return strdup(buf);
	case FT_INT:
		snprintf(buf, sizeof(buf), "%d", *(const int *)ptr);
		return strdup(buf);
	case FT_UINT:
		snprintf(buf, sizeof(buf), "%u", *(const uint32_t *)ptr);
		return strdup(buf);
	case FT_UINT8:
		snprintf(buf, sizeof(buf), "%u", (unsigned)*(const uint8_t *)ptr);
		return strdup(buf);
	case FT_UINT16:
		snprintf(buf, sizeof(buf), "%u", (unsigned)*(const uint16_t *)ptr);
		return strdup(buf);
	case FT_DOUBLE:
		snprintf(buf, sizeof(buf), "%g", *(const double *)ptr);
		return strdup(buf);
	case FT_FLOAT:
		snprintf(buf, sizeof(buf), "%.6g", (double)*(const float *)ptr);
		return strdup(buf);
	case FT_STRING: {
		cJSON *s = cJSON_CreateString((const char *)ptr);
		if (!s) return strdup("\"\"");
		char *json = cJSON_PrintUnformatted(s);
		cJSON_Delete(s);
		return json;
	}
	case FT_SIZE: {
		const uint32_t *wh = (const uint32_t *)ptr;
		snprintf(buf, sizeof(buf), "\"%ux%u\"", wh[0], wh[1]);
		return strdup(buf);
	}
	}
	return strdup("null");
}

static char *field_to_json_value(const FieldDesc *f)
{
	return field_to_json_value_from_cfg(g_cfg, f);
}

/* Parse a string value and write it into the config field.
 * Returns 0 on success, -1 on parse error. */
static int field_from_string_cfg(VencConfig *cfg, const FieldDesc *f,
	const char *val)
{
	void *ptr;

	if (!cfg || !f || !val)
		return -1;

	ptr = (char *)cfg + f->offset;
	switch (f->type) {
	case FT_BOOL:
		if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0)
			*(bool *)ptr = true;
		else if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0)
			*(bool *)ptr = false;
		else
			return -1;
		break;
	case FT_INT: {
		char *end;
		long v = strtol(val, &end, 10);
		if (end == val || *end != '\0') return -1;
		*(int *)ptr = (int)v;
		break;
	}
	case FT_UINT: {
		char *end;
		unsigned long v = strtoul(val, &end, 10);
		if (end == val || *end != '\0') return -1;
		*(uint32_t *)ptr = (uint32_t)v;
		break;
	}
	case FT_UINT8: {
		char *end;
		unsigned long v = strtoul(val, &end, 0);  /* base 0: accepts 0x hex */
		if (end == val || *end != '\0' || v > 255) return -1;
		*(uint8_t *)ptr = (uint8_t)v;
		break;
	}
	case FT_UINT16: {
		char *end;
		unsigned long v = strtoul(val, &end, 10);
		if (end == val || *end != '\0' || v > 65535) return -1;
		*(uint16_t *)ptr = (uint16_t)v;
		break;
	}
	case FT_DOUBLE: {
		char *end;
		double v = strtod(val, &end);
		if (end == val || *end != '\0') return -1;
		*(double *)ptr = v;
		break;
	}
	case FT_FLOAT: {
		char *end;
		float v = (float)strtod(val, &end);
		if (end == val || *end != '\0') return -1;
		*(float *)ptr = v;
		break;
	}
	case FT_STRING:
		snprintf((char *)ptr, f->size, "%s", val);
		break;
	case FT_SIZE: {
		uint32_t w, h;
		if (!strcmp(val, "720p")) { w = 1280; h = 720; }
		else if (!strcmp(val, "1080p")) { w = 1920; h = 1080; }
		else if (!strcmp(val, "4MP")) { w = 2688; h = 1520; }
		else if (sscanf(val, "%ux%u", &w, &h) != 2) return -1;
		uint32_t *wh = (uint32_t *)ptr;
		wh[0] = w;
		wh[1] = h;
		break;
	}
	}
	return 0;
}

/* ── Field-level validation ──────────────────────────────────────────── */

/* Check a single field value after parsing.  Returns NULL if valid,
 * or a static error message string if invalid. */
static const char *validate_field_cfg(const VencConfig *cfg, const char *key)
{
	if (!cfg || !key)
		return "invalid config state";

	if (strcmp(key, "isp.awb_mode") == 0) {
		if (strcmp(cfg->isp.awb_mode, "auto") != 0 &&
		    strcmp(cfg->isp.awb_mode, "ct_manual") != 0)
			return "awb_mode must be 'auto' or 'ct_manual'";
	}
	if (strcmp(key, "video0.qp_delta") == 0) {
		if (cfg->video0.qp_delta < -12 || cfg->video0.qp_delta > 12)
			return "qp_delta must be in range [-12, 12]";
	}
	if (strcmp(key, "fpv.roi_qp") == 0) {
		if (cfg->fpv.roi_qp < -30 || cfg->fpv.roi_qp > 30)
			return "roi_qp must be in range [-30, 30]";
	}
	if (strcmp(key, "fpv.roi_steps") == 0) {
		if (cfg->fpv.roi_steps < 1 ||
		    cfg->fpv.roi_steps > PIPELINE_ROI_MAX_STEPS)
			return "roi_steps must be in range [1, 4]";
	}
	if (strcmp(key, "fpv.roi_center") == 0) {
		if (cfg->fpv.roi_center < 0.1 || cfg->fpv.roi_center > 0.9)
			return "roi_center must be in range [0.1, 0.9]";
	}
	if (strcmp(key, "video0.bitrate") == 0) {
		if (cfg->video0.bitrate == 0 || cfg->video0.bitrate > 200000)
			return "bitrate must be 1-200000 kbps";
	}
	if (strcmp(key, "video0.scene_holdoff") == 0 &&
	    cfg->video0.scene_holdoff == 0)
		return "video0.scene_holdoff must be >= 1";
	return NULL;
}

/* ── Config validation ───────────────────────────────────────────────── */

/* Check config consistency after a field change.  Returns NULL if valid,
 * or a static error message string if invalid. */
static int config_codec_is_h265(const VencConfig *cfg)
{
	return cfg &&
		(strcmp(cfg->video0.codec, "h265") == 0 ||
		 strcmp(cfg->video0.codec, "265") == 0);
}

static const char *validate_backend_config(const char *backend_name,
	const VencConfig *cfg)
{
	if (!cfg)
		return "invalid config state";

	if (backend_name && strcmp(backend_name, "star6e") == 0 &&
	    strcmp(cfg->outgoing.stream_mode, "compact") != 0 &&
	    !config_codec_is_h265(cfg)) {
		return "star6e RTP mode currently supports h265 only";
	}

	return NULL;
}

/* ── Query string helpers ────────────────────────────────────────────── */

/* Find the first key=value in a query string.  Writes key and value into
 * provided buffers.  Returns 0 on success, -1 if no key found. */
static int parse_first_query_param(const char *query, char *key, size_t key_sz,
	char *val, size_t val_sz)
{
	if (!query || !*query) return -1;
	const char *eq = strchr(query, '=');
	const char *amp = strchr(query, '&');
	if (eq) {
		size_t klen = (size_t)(eq - query);
		if (klen >= key_sz) klen = key_sz - 1;
		memcpy(key, query, klen);
		key[klen] = '\0';
		const char *vstart = eq + 1;
		size_t vlen = amp ? (size_t)(amp - vstart) : strlen(vstart);
		if (vlen >= val_sz) vlen = val_sz - 1;
		memcpy(val, vstart, vlen);
		val[vlen] = '\0';
	} else {
		/* key only, no value (used by GET) */
		size_t klen = amp ? (size_t)(amp - query) : strlen(query);
		if (klen >= key_sz) klen = key_sz - 1;
		memcpy(key, query, klen);
		key[klen] = '\0';
		val[0] = '\0';
	}
	return 0;
}

#define SET_QUERY_MAX_PARAMS 16

typedef struct {
	char key[128];
	char canonical_key[128];
	char value[256];
	const FieldDesc *field;
} SetQueryParam;

typedef enum {
	LIVE_GROUP_INVALID = -1,
	LIVE_GROUP_BITRATE = 0,
	LIVE_GROUP_VIDEO_TIMING,
	LIVE_GROUP_QP_DELTA,
	LIVE_GROUP_ROI,
	LIVE_GROUP_GAIN_MAX,
	LIVE_GROUP_AWB,
	LIVE_GROUP_VERBOSE,
	LIVE_GROUP_OUTGOING,
	LIVE_GROUP_MUTE,
	LIVE_GROUP_COUNT
} LiveApplyGroup;

typedef struct {
	int video_fps;
	int video_gop;
	int awb_mode;
	int awb_ct;
	int outgoing_enabled;
	int outgoing_server;
} LiveBatchTouched;

static int make_error_json(const char *code, const char *message, char **out_json)
{
	char buf[1024];
	int len;

	if (!out_json)
		return -1;

	len = snprintf(buf, sizeof(buf),
		"{\"ok\":false,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
		code ? code : "internal_error",
		message ? message : "unknown error");
	if (len >= (int)sizeof(buf))
		len = (int)sizeof(buf) - 1;

	*out_json = strdup(buf);
	return *out_json ? 0 : -1;
}

static int make_handled_error_json(int status, const char *code,
	const char *message, int *status_code, char **response_json)
{
	if (status_code)
		*status_code = status;
	if (make_error_json(code, message, response_json) != 0)
		return -1;
	return 1;
}

static int make_single_set_success_json(const char *field_key,
	const char *json_value, int reinit_pending, char **out_json)
{
	char buf[512];
	int len;

	if (!field_key || !json_value || !out_json)
		return -1;

	if (reinit_pending) {
		len = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"field\":\"%s\",\"value\":%s,"
			"\"reinit_pending\":true}}",
			field_key, json_value);
	} else {
		len = snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"field\":\"%s\",\"value\":%s}}",
			field_key, json_value);
	}
	if (len >= (int)sizeof(buf))
		len = (int)sizeof(buf) - 1;

	*out_json = strdup(buf);
	return *out_json ? 0 : -1;
}

static int make_multi_live_set_success_json(const SetQueryParam *params,
	size_t count, char **out_json)
{
	cJSON *root;
	cJSON *data;
	cJSON *applied;
	size_t i;
	char *str;

	if (!params || count == 0 || !out_json)
		return -1;

	root = cJSON_CreateObject();
	if (!root)
		return -1;

	cJSON_AddBoolToObject(root, "ok", 1);
	data = cJSON_AddObjectToObject(root, "data");
	applied = cJSON_AddArrayToObject(data, "applied");

	for (i = 0; i < count; i++) {
		cJSON *entry;
		cJSON *value_item;
		char *json_value;

		entry = cJSON_CreateObject();
		if (!entry) {
			cJSON_Delete(root);
			return -1;
		}
		cJSON_AddStringToObject(entry, "field", params[i].key);

		json_value = field_to_json_value(params[i].field);
		if (!json_value) {
			cJSON_Delete(entry);
			cJSON_Delete(root);
			return -1;
		}

		value_item = cJSON_Parse(json_value);
		if (!value_item) {
			free(json_value);
			cJSON_Delete(entry);
			cJSON_Delete(root);
			return -1;
		}
		free(json_value);

		cJSON_AddItemToObject(entry, "value", value_item);
		cJSON_AddItemToArray(applied, entry);
	}

	str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!str)
		return -1;

	*out_json = str;
	return 0;
}

static LiveApplyGroup live_group_for_key(const char *canonical_key)
{
	if (!canonical_key)
		return LIVE_GROUP_INVALID;

	if (strcmp(canonical_key, "video0.bitrate") == 0)
		return LIVE_GROUP_BITRATE;
	if (strcmp(canonical_key, "video0.fps") == 0 ||
	    strcmp(canonical_key, "video0.gop_size") == 0)
		return LIVE_GROUP_VIDEO_TIMING;
	if (strcmp(canonical_key, "video0.qp_delta") == 0)
		return LIVE_GROUP_QP_DELTA;
	if (strcmp(canonical_key, "fpv.roi_enabled") == 0 ||
	    strcmp(canonical_key, "fpv.roi_qp") == 0 ||
	    strcmp(canonical_key, "fpv.roi_steps") == 0 ||
	    strcmp(canonical_key, "fpv.roi_center") == 0)
		return LIVE_GROUP_ROI;
	if (strcmp(canonical_key, "isp.gain_max") == 0)
		return LIVE_GROUP_GAIN_MAX;
	if (strcmp(canonical_key, "isp.awb_mode") == 0 ||
	    strcmp(canonical_key, "isp.awb_ct") == 0)
		return LIVE_GROUP_AWB;
	if (strcmp(canonical_key, "system.verbose") == 0)
		return LIVE_GROUP_VERBOSE;
	if (strcmp(canonical_key, "outgoing.enabled") == 0 ||
	    strcmp(canonical_key, "outgoing.server") == 0)
		return LIVE_GROUP_OUTGOING;
	if (strcmp(canonical_key, "audio.mute") == 0)
		return LIVE_GROUP_MUTE;

	return LIVE_GROUP_INVALID;
}

static const char *live_group_name(LiveApplyGroup group)
{
	switch (group) {
	case LIVE_GROUP_BITRATE:
		return "video0.bitrate";
	case LIVE_GROUP_VIDEO_TIMING:
		return "video0.fps/video0.gop_size";
	case LIVE_GROUP_QP_DELTA:
		return "video0.qp_delta";
	case LIVE_GROUP_ROI:
		return "fpv.roi_*";
	case LIVE_GROUP_GAIN_MAX:
		return "isp.gain_max";
	case LIVE_GROUP_AWB:
		return "isp.awb_*";
	case LIVE_GROUP_VERBOSE:
		return "system.verbose";
	case LIVE_GROUP_OUTGOING:
		return "outgoing.*";
	case LIVE_GROUP_MUTE:
		return "audio.mute";
	default:
		return "unknown";
	}
}

static void note_live_group_touch(LiveBatchTouched *touched,
	const char *canonical_key)
{
	if (!touched || !canonical_key)
		return;

	if (strcmp(canonical_key, "video0.fps") == 0)
		touched->video_fps = 1;
	else if (strcmp(canonical_key, "video0.gop_size") == 0)
		touched->video_gop = 1;
	else if (strcmp(canonical_key, "isp.awb_mode") == 0)
		touched->awb_mode = 1;
	else if (strcmp(canonical_key, "isp.awb_ct") == 0)
		touched->awb_ct = 1;
	else if (strcmp(canonical_key, "outgoing.enabled") == 0)
		touched->outgoing_enabled = 1;
	else if (strcmp(canonical_key, "outgoing.server") == 0)
		touched->outgoing_server = 1;
}

static int parse_query_params(const char *query, SetQueryParam *params,
	size_t max_params, size_t *out_count, const char **error_message)
{
	const char *cursor;
	size_t count = 0;

	if (out_count)
		*out_count = 0;
	if (error_message)
		*error_message = NULL;

	if (!query || !*query) {
		if (error_message)
			*error_message = "missing query parameter key=value";
		return -1;
	}

	cursor = query;
	while (*cursor) {
		const char *segment_end = strchr(cursor, '&');
		const char *eq;
		size_t key_len;
		size_t value_len;
		const char *canonical_key;

		if (!segment_end)
			segment_end = cursor + strlen(cursor);
		if (segment_end == cursor) {
			if (error_message)
				*error_message = "empty query parameter";
			return -1;
		}
		if (count >= max_params) {
			if (error_message)
				*error_message = "too many query parameters";
			return -1;
		}

		eq = memchr(cursor, '=', (size_t)(segment_end - cursor));
		if (!eq || eq == cursor) {
			if (error_message)
				*error_message = "missing query parameter key=value";
			return -1;
		}

		key_len = (size_t)(eq - cursor);
		if (key_len >= sizeof(params[count].key))
			key_len = sizeof(params[count].key) - 1;
		memcpy(params[count].key, cursor, key_len);
		params[count].key[key_len] = '\0';

		value_len = (size_t)(segment_end - (eq + 1));
		if (value_len >= sizeof(params[count].value))
			value_len = sizeof(params[count].value) - 1;
		memcpy(params[count].value, eq + 1, value_len);
		params[count].value[value_len] = '\0';

		canonical_key = canonicalize_field_key(params[count].key);
		if (!canonical_key)
			canonical_key = params[count].key;
		snprintf(params[count].canonical_key,
			sizeof(params[count].canonical_key), "%s", canonical_key);
		params[count].field = NULL;
		count++;

		cursor = *segment_end ? segment_end + 1 : segment_end;
	}

	if (out_count)
		*out_count = count;
	return 0;
}

static int live_group_supported_for_cfg(const VencConfig *cfg,
	LiveApplyGroup group, const LiveBatchTouched *touched)
{
	if (!g_cb)
		return 0;

	switch (group) {
	case LIVE_GROUP_BITRATE:
		return g_cb->apply_bitrate != NULL;
	case LIVE_GROUP_VIDEO_TIMING:
		if (touched && touched->video_fps && !g_cb->apply_fps)
			return 0;
		if (cfg && !(cfg->video0.scene_threshold > 0) &&
		    touched && (touched->video_fps || touched->video_gop) &&
		    !g_cb->apply_gop)
			return 0;
		if (cfg && cfg->video0.scene_threshold > 0 &&
		    touched && touched->video_gop)
			return 0;
		return 1;
	case LIVE_GROUP_QP_DELTA:
		return g_cb->apply_qp_delta != NULL;
	case LIVE_GROUP_ROI:
		return g_cb->apply_roi_qp != NULL;
	case LIVE_GROUP_GAIN_MAX:
		return g_cb->apply_gain_max != NULL;
	case LIVE_GROUP_AWB:
		return g_cb->apply_awb_mode != NULL;
	case LIVE_GROUP_VERBOSE:
		return g_cb->apply_verbose != NULL;
	case LIVE_GROUP_OUTGOING:
		if (touched && touched->outgoing_server && !g_cb->apply_server)
			return 0;
		if (touched && touched->outgoing_enabled &&
		    !g_cb->apply_output_enabled)
			return 0;
		return 1;
	case LIVE_GROUP_MUTE:
		return g_cb->apply_mute != NULL;
	default:
		return 0;
	}
}

static void copy_live_group_fields(VencConfig *dst, const VencConfig *src,
	LiveApplyGroup group, const LiveBatchTouched *touched)
{
	if (!dst || !src)
		return;

	switch (group) {
	case LIVE_GROUP_BITRATE:
		dst->video0.bitrate = src->video0.bitrate;
		break;
	case LIVE_GROUP_VIDEO_TIMING:
		if (touched && touched->video_fps)
			dst->video0.fps = src->video0.fps;
		if (touched && touched->video_gop)
			dst->video0.gop_size = src->video0.gop_size;
		break;
	case LIVE_GROUP_QP_DELTA:
		dst->video0.qp_delta = src->video0.qp_delta;
		break;
	case LIVE_GROUP_ROI:
		dst->fpv.roi_enabled = src->fpv.roi_enabled;
		dst->fpv.roi_qp = src->fpv.roi_qp;
		dst->fpv.roi_steps = src->fpv.roi_steps;
		dst->fpv.roi_center = src->fpv.roi_center;
		break;
	case LIVE_GROUP_GAIN_MAX:
		dst->isp.gain_max = src->isp.gain_max;
		break;
	case LIVE_GROUP_AWB:
		if (touched && touched->awb_mode) {
			snprintf(dst->isp.awb_mode, sizeof(dst->isp.awb_mode), "%s",
				src->isp.awb_mode);
		}
		if (touched && touched->awb_ct)
			dst->isp.awb_ct = src->isp.awb_ct;
		break;
	case LIVE_GROUP_VERBOSE:
		dst->system.verbose = src->system.verbose;
		break;
	case LIVE_GROUP_OUTGOING:
		if (touched && touched->outgoing_enabled)
			dst->outgoing.enabled = src->outgoing.enabled;
		if (touched && touched->outgoing_server) {
			snprintf(dst->outgoing.server, sizeof(dst->outgoing.server), "%s",
				src->outgoing.server);
		}
		break;
	case LIVE_GROUP_MUTE:
		dst->audio.mute = src->audio.mute;
		break;
	default:
		break;
	}
}

static void build_live_group_config(VencConfig *out, const VencConfig *base,
	const VencConfig *updates, LiveApplyGroup group,
	const LiveBatchTouched *touched)
{
	if (!out || !base || !updates)
		return;

	*out = *base;
	copy_live_group_fields(out, updates, group, touched);
}

static int commit_config_locked(const VencConfig *cfg)
{
	if (!g_cfg || !cfg)
		return -1;

	*g_cfg = *cfg;
	return 0;
}

static int apply_live_group_for_cfg(const VencConfig *cfg,
	LiveApplyGroup group, const LiveBatchTouched *touched)
{
	int rc;
	int mode;
	uint32_t gop_frames;

	if (!cfg || commit_config_locked(cfg) != 0)
		return -1;
	if (!live_group_supported_for_cfg(cfg, group, touched))
		return -2;

	switch (group) {
	case LIVE_GROUP_BITRATE:
		return g_cb->apply_bitrate(cfg->video0.bitrate);
	case LIVE_GROUP_VIDEO_TIMING:
		if (touched && touched->video_fps) {
			rc = g_cb->apply_fps(cfg->video0.fps);
			if (rc != 0)
				return -1;
		}
		if (!(cfg->video0.scene_threshold > 0) &&
		    touched && (touched->video_fps || touched->video_gop)) {
			gop_frames = pipeline_common_gop_frames(
				cfg->video0.gop_size, cfg->video0.fps);
			rc = g_cb->apply_gop(gop_frames);
			if (rc != 0)
				return -1;
		}
		return 0;
	case LIVE_GROUP_QP_DELTA:
		return g_cb->apply_qp_delta(cfg->video0.qp_delta);
	case LIVE_GROUP_ROI:
		return g_cb->apply_roi_qp(cfg->fpv.roi_qp);
	case LIVE_GROUP_GAIN_MAX:
		return g_cb->apply_gain_max(cfg->isp.gain_max);
	case LIVE_GROUP_AWB:
		mode = strcmp(cfg->isp.awb_mode, "ct_manual") == 0 ? 1 : 0;
		return g_cb->apply_awb_mode(mode, cfg->isp.awb_ct);
	case LIVE_GROUP_VERBOSE:
		return g_cb->apply_verbose(cfg->system.verbose);
	case LIVE_GROUP_OUTGOING:
		if (touched && touched->outgoing_enabled && touched->outgoing_server) {
			if (cfg->outgoing.enabled) {
				rc = g_cb->apply_server(cfg->outgoing.server);
				if (rc != 0)
					return -1;
				rc = g_cb->apply_output_enabled(cfg->outgoing.enabled);
				if (rc != 0)
					return -1;
				return 0;
			}

			rc = g_cb->apply_output_enabled(cfg->outgoing.enabled);
			if (rc != 0)
				return -1;
			rc = g_cb->apply_server(cfg->outgoing.server);
			if (rc != 0)
				return -1;
			return 0;
		}
		if (touched && touched->outgoing_server)
			return g_cb->apply_server(cfg->outgoing.server);
		if (touched && touched->outgoing_enabled)
			return g_cb->apply_output_enabled(cfg->outgoing.enabled);
		return 0;
	case LIVE_GROUP_MUTE:
		return g_cb->apply_mute(cfg->audio.mute);
	default:
		return -2;
	}
}

static int rollback_live_groups(const LiveApplyGroup *groups,
	size_t applied_count, LiveApplyGroup current_group,
	const LiveBatchTouched *touched, const VencConfig *old_cfg,
	VencConfig *actual_cfg)
{
	VencConfig rollback_cfg;
	int rollback_incomplete = 0;
	size_t i;

	if (!old_cfg || !actual_cfg)
		return 1;

	if (current_group != LIVE_GROUP_INVALID) {
		build_live_group_config(&rollback_cfg, actual_cfg, old_cfg,
			current_group, touched);
		if (apply_live_group_for_cfg(&rollback_cfg, current_group,
		    touched) == 0) {
			*actual_cfg = rollback_cfg;
		} else {
			fprintf(stderr,
				"[venc_api] live batch rollback failed for %s\n",
				live_group_name(current_group));
			rollback_incomplete = 1;
			commit_config_locked(actual_cfg);
		}
	}

	for (i = applied_count; i > 0; i--) {
		build_live_group_config(&rollback_cfg, actual_cfg, old_cfg,
			groups[i - 1], touched);
		if (apply_live_group_for_cfg(&rollback_cfg, groups[i - 1],
		    touched) == 0) {
			*actual_cfg = rollback_cfg;
		} else {
			fprintf(stderr,
				"[venc_api] live batch rollback failed for %s\n",
				live_group_name(groups[i - 1]));
			rollback_incomplete = 1;
			commit_config_locked(actual_cfg);
		}
	}

	return rollback_incomplete;
}

static int collect_live_groups(SetQueryParam *params, size_t param_count,
	LiveApplyGroup *group_order, size_t *group_count,
	LiveBatchTouched *touched, int *status_code, char **response_json)
{
	int group_seen[LIVE_GROUP_COUNT] = {0};
	size_t i;

	if (!params || !group_order || !group_count || !touched ||
	    !status_code || !response_json)
		return -1;

	memset(touched, 0, sizeof(*touched));
	*group_count = 0;

	for (i = 0; i < param_count; i++) {
		size_t j;
		LiveApplyGroup group;

		if (!params[i].field)
			params[i].field = find_field(params[i].canonical_key);
		if (!params[i].field) {
			return make_handled_error_json(404, "not_found",
				"unknown config field", status_code,
				response_json);
		}
		if (!venc_api_field_supported_for_backend(g_backend,
		    params[i].canonical_key)) {
			return make_handled_error_json(501, "not_implemented",
				"field not supported on this backend",
				status_code, response_json);
		}
		if (params[i].field->mut != MUT_LIVE) {
			return make_handled_error_json(400, "invalid_request",
				"multi-set only supports live fields; restart-required fields must be set one at a time",
				status_code, response_json);
		}

		for (j = 0; j < i; j++) {
			if (strcmp(params[i].canonical_key,
			    params[j].canonical_key) == 0) {
				return make_handled_error_json(400,
					"invalid_request",
					"duplicate field in multi-set request",
					status_code, response_json);
			}
		}

		group = live_group_for_key(params[i].canonical_key);
		if (group == LIVE_GROUP_INVALID) {
			return make_handled_error_json(400, "invalid_request",
				"field does not support multi-set batching",
				status_code, response_json);
		}
		note_live_group_touch(touched, params[i].canonical_key);
		if (!group_seen[group]) {
			group_seen[group] = 1;
			group_order[*group_count] = group;
			(*group_count)++;
		}
	}

	return 0;
}

static int stage_params_into_cfg(VencConfig *cfg, const SetQueryParam *params,
	size_t param_count, int *status_code, char **response_json)
{
	size_t i;

	if (!cfg || !params || !status_code || !response_json)
		return -1;

	for (i = 0; i < param_count; i++) {
		const char *field_err;

		if (field_from_string_cfg(cfg, params[i].field, params[i].value) != 0) {
			*status_code = 400;
			return make_error_json("validation_failed",
				"invalid value for field", response_json) == 0 ? 1 : -1;
		}

		field_err = validate_field_cfg(cfg, params[i].canonical_key);
		if (field_err) {
			*status_code = 409;
			return make_error_json("validation_failed", field_err,
				response_json) == 0 ? 1 : -1;
		}
	}

	{
		const char *err = validate_backend_config(g_backend, cfg);
		if (err) {
			*status_code = 409;
			return make_error_json("validation_failed", err,
				response_json) == 0 ? 1 : -1;
		}
	}

	return 0;
}

static int preflight_live_group_callbacks(const VencConfig *cfg,
	const LiveApplyGroup *group_order, size_t group_count,
	const LiveBatchTouched *touched, size_t param_count,
	int *status_code, char **response_json)
{
	size_t i;

	if (!cfg || !group_order || !status_code || !response_json)
		return -1;

	for (i = 0; i < group_count; i++) {
		if (!live_group_supported_for_cfg(cfg, group_order[i], touched)) {
			*status_code = 501;
			return make_error_json("not_implemented",
				param_count == 1 ? "apply callback not available" :
				"apply callback not available for one or more live fields",
				response_json) == 0 ? 1 : -1;
		}
	}

	return 0;
}

static int apply_live_group_sequence_locked(const LiveApplyGroup *group_order,
	size_t group_count, const LiveBatchTouched *touched,
	const VencConfig *old_cfg, const VencConfig *new_cfg,
	VencConfig *actual_cfg, int *status_code, char **response_json)
{
	size_t i;

	if (!group_order || !old_cfg || !new_cfg || !actual_cfg ||
	    !status_code || !response_json)
		return -1;

	for (i = 0; i < group_count; i++) {
		VencConfig group_cfg;
		int rollback_incomplete;
		char message[192];

		build_live_group_config(&group_cfg, actual_cfg, new_cfg,
			group_order[i], touched);
		if (apply_live_group_for_cfg(&group_cfg, group_order[i],
		    touched) == 0) {
			*actual_cfg = group_cfg;
			continue;
		}

		commit_config_locked(actual_cfg);
		rollback_incomplete = rollback_live_groups(group_order, i,
			group_order[i], touched, old_cfg, actual_cfg);
		commit_config_locked(actual_cfg);

		snprintf(message, sizeof(message),
			rollback_incomplete ?
			"failed to apply live field group %s; rollback incomplete" :
			"failed to apply live field group %s",
			live_group_name(group_order[i]));
		*status_code = 500;
		return make_error_json("internal_error", message,
			response_json) == 0 ? 1 : -1;
	}

	return 0;
}

static int make_live_set_response_locked(const VencConfig *cfg,
	const SetQueryParam *params, size_t param_count, int single_response,
	int *status_code, char **response_json)
{
	if (!cfg || !params || param_count == 0 || !status_code || !response_json)
		return -1;

	if (single_response) {
		char *jval;
		int rc;

		jval = field_to_json_value_from_cfg(cfg, params[0].field);
		if (!jval) {
			*status_code = 500;
			return make_error_json("internal_error", "out of memory",
				response_json);
		}

		rc = make_single_set_success_json(params[0].key, jval, 0,
			response_json);
		free(jval);
		if (rc != 0)
			return -1;
	} else {
		if (make_multi_live_set_success_json(params, param_count,
		    response_json) != 0) {
			return -1;
		}
	}

	*status_code = 200;
	return 0;
}

static int apply_live_set_query(SetQueryParam *params, size_t param_count,
	int single_response, int *status_code, char **response_json)
{
	LiveApplyGroup group_order[LIVE_GROUP_COUNT];
	LiveBatchTouched touched;
	size_t group_count = 0;
	VencConfig old_cfg;
	VencConfig new_cfg;
	VencConfig actual_cfg;
	int rc;

	rc = collect_live_groups(params, param_count, group_order, &group_count,
		&touched, status_code, response_json);
	if (rc != 0)
		return rc > 0 ? 0 : rc;

	pthread_mutex_lock(&g_cfg_mutex);
	old_cfg = *g_cfg;
	new_cfg = old_cfg;
	actual_cfg = old_cfg;

	rc = stage_params_into_cfg(&new_cfg, params, param_count, status_code,
		response_json);
	if (rc != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return rc > 0 ? 0 : rc;
	}

	rc = preflight_live_group_callbacks(&new_cfg, group_order, group_count,
		&touched, param_count, status_code, response_json);
	if (rc != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return rc > 0 ? 0 : rc;
	}

	rc = apply_live_group_sequence_locked(group_order, group_count, &touched,
		&old_cfg, &new_cfg, &actual_cfg, status_code, response_json);
	if (rc != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return rc > 0 ? 0 : rc;
	}

	if (commit_config_locked(&actual_cfg) != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return -1;
	}

	rc = make_live_set_response_locked(&actual_cfg, params, param_count,
		single_response, status_code, response_json);
	if (rc != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return rc;
	}

	pthread_mutex_unlock(&g_cfg_mutex);
	return 0;
}

static int resolve_set_query_field(const char *key, const char **canonical_key,
	const FieldDesc **field, int *status_code, char **response_json)
{
	if (!key || !*key || !canonical_key || !field || !status_code ||
	    !response_json)
		return -1;

	*canonical_key = canonicalize_field_key(key);
	*field = find_field(*canonical_key);
	if (!*field) {
		*status_code = 404;
		return make_error_json("not_found", "unknown config field",
			response_json) == 0 ? 1 : -1;
	}
	if (!venc_api_field_supported_for_backend(g_backend, *canonical_key)) {
		*status_code = 501;
		return make_error_json("not_implemented",
			"field not supported on this backend",
			response_json) == 0 ? 1 : -1;
	}

	return 0;
}

static void init_single_set_param(SetQueryParam *param, const char *key,
	const char *canonical_key, const char *value, const FieldDesc *field)
{
	if (!param || !key || !canonical_key || !value || !field)
		return;

	memset(param, 0, sizeof(*param));
	snprintf(param->key, sizeof(param->key), "%s", key);
	snprintf(param->canonical_key, sizeof(param->canonical_key), "%s",
		canonical_key);
	snprintf(param->value, sizeof(param->value), "%s", value);
	param->field = field;
}

static int process_restart_set_query(const SetQueryParam *param,
	int *status_code, char **response_json)
{
	VencConfig new_cfg;
	char *jval;
	int rc;

	if (!param || !status_code || !response_json)
		return -1;

	pthread_mutex_lock(&g_cfg_mutex);
	new_cfg = *g_cfg;
	rc = stage_params_into_cfg(&new_cfg, param, 1, status_code,
		response_json);
	if (rc != 0) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return rc > 0 ? 0 : rc;
	}

	*g_cfg = new_cfg;
	venc_api_request_reinit(2);
	jval = field_to_json_value_from_cfg(&new_cfg, param->field);
	pthread_mutex_unlock(&g_cfg_mutex);
	if (!jval) {
		*status_code = 500;
		return make_error_json("internal_error", "out of memory",
			response_json);
	}

	*status_code = 200;
	rc = make_single_set_success_json(param->key, jval, 1, response_json);
	free(jval);
	return rc;
}

static int process_single_set_query(const char *query, int *status_code,
	char **response_json)
{
	char key[128], val[256];
	const char *canonical_key;
	const FieldDesc *f;
	SetQueryParam param;
	int rc;

	if (parse_first_query_param(query, key, sizeof(key), val, sizeof(val)) != 0 ||
	    !*key) {
		*status_code = 400;
		return make_error_json("invalid_request",
			"missing query parameter key=value", response_json);
	}

	rc = resolve_set_query_field(key, &canonical_key, &f, status_code,
		response_json);
	if (rc != 0)
		return rc > 0 ? 0 : rc;

	init_single_set_param(&param, key, canonical_key, val, f);
	if (f->mut == MUT_LIVE) {
		return apply_live_set_query(&param, 1, 1, status_code,
			response_json);
	}

	return process_restart_set_query(&param, status_code, response_json);
}

static int process_multi_live_set_query(const char *query, int *status_code,
	char **response_json)
{
	SetQueryParam params[SET_QUERY_MAX_PARAMS];
	const char *parse_error = NULL;
	size_t param_count = 0;

	if (parse_query_params(query, params, SET_QUERY_MAX_PARAMS, &param_count,
	    &parse_error) != 0) {
		*status_code = 400;
		return make_error_json("invalid_request",
			parse_error ? parse_error : "invalid query parameters",
			response_json);
	}
	if (param_count < 2)
		return process_single_set_query(query, status_code, response_json);

	return apply_live_set_query(params, param_count, 0, status_code,
		response_json);
}

static int process_set_query(const char *query, int *status_code,
	char **response_json)
{
	if (!status_code || !response_json)
		return -1;

	*status_code = 500;
	*response_json = NULL;

	if (query && strchr(query, '&'))
		return process_multi_live_set_query(query, status_code,
			response_json);

	return process_single_set_query(query, status_code, response_json);
}

/* ── Route handlers ──────────────────────────────────────────────────── */

static int handle_version(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	char buf[512];
#ifndef VENC_VERSION
#define VENC_VERSION "unknown"
#endif
	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"app_version\":\"%s\","
		"\"contract_version\":\"0.3.0\","
		"\"config_schema_version\":\"1.0.0\","
		"\"backend\":\"%s\""
		"}}", VENC_VERSION, g_backend);
	return httpd_send_json(fd, 200, buf);
}

static int handle_config(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	pthread_mutex_lock(&g_cfg_mutex);
	char *cfg_json = venc_config_to_json_string(g_cfg);
	pthread_mutex_unlock(&g_cfg_mutex);
	if (!cfg_json)
		return httpd_send_error(fd, 500, "internal_error",
			"failed to serialize config");

	/* Wrap in envelope */
	size_t len = strlen(cfg_json) + 64;
	char *buf = malloc(len);
	if (!buf) {
		free(cfg_json);
		return httpd_send_error(fd, 500, "internal_error", "out of memory");
	}
	snprintf(buf, len, "{\"ok\":true,\"data\":{\"config\":%s}}", cfg_json);
	int ret = httpd_send_json(fd, 200, buf);
	free(buf);
	free(cfg_json);
	return ret;
}

static int handle_capabilities(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	/* Build a JSON object with field mutability */
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "ok", 1);
	cJSON *data = cJSON_AddObjectToObject(root, "data");
	cJSON *fields = cJSON_AddObjectToObject(data, "fields");
	for (size_t i = 0; i < FIELD_COUNT; i++) {
		cJSON *entry = cJSON_AddObjectToObject(fields, g_fields[i].key);
		cJSON_AddStringToObject(entry, "mutability",
			g_fields[i].mut == MUT_LIVE ? "live" : "restart_required");
		cJSON_AddBoolToObject(entry, "supported",
			venc_api_field_supported_for_backend(g_backend,
				g_fields[i].key));
	}
	char *str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!str)
		return httpd_send_error(fd, 500, "internal_error", "out of memory");
	int ret = httpd_send_json(fd, 200, str);
	free(str);
	return ret;
}

static int handle_fps_config(int fd, const HttpRequest *req, void *ctx)
{
	char buf[128];

	(void)req;
	(void)ctx;
	pthread_mutex_lock(&g_cfg_mutex);
	snprintf(buf, sizeof(buf), "{\"ok\":true,\"data\":{\"fps\":%u}}",
		g_cfg ? g_cfg->video0.fps : 0);
	pthread_mutex_unlock(&g_cfg_mutex);
	return httpd_send_json(fd, 200, buf);
}

static int handle_fps_live(int fd, const HttpRequest *req, void *ctx)
{
	uint32_t fps = 0;
	char buf[128];

	(void)req;
	(void)ctx;
	if (g_cb && g_cb->query_live_fps)
		fps = g_cb->query_live_fps();
	if (fps == 0) {
		pthread_mutex_lock(&g_cfg_mutex);
		fps = g_cfg ? g_cfg->video0.fps : 0;
		pthread_mutex_unlock(&g_cfg_mutex);
	}

	snprintf(buf, sizeof(buf), "{\"ok\":true,\"data\":{\"fps\":%u}}", fps);
	return httpd_send_json(fd, 200, buf);
}

static int handle_set(int fd, const HttpRequest *req, void *ctx)
{
	char *json = NULL;
	int status = 500;
	int rc;

	(void)ctx;

	rc = process_set_query(req->query, &status, &json);
	if (rc != 0 || !json) {
		free(json);
		return httpd_send_error(fd, 500, "internal_error", "out of memory");
	}

	rc = httpd_send_json(fd, status, json);
	free(json);
	return rc;
}

static int handle_get(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
	char key[128], dummy[4];
	const char *canonical_key;
	if (parse_first_query_param(req->query, key, sizeof(key),
			dummy, sizeof(dummy)) != 0 || !*key) {
		return httpd_send_error(fd, 400, "invalid_request",
			"missing query parameter (field name)");
	}

	canonical_key = canonicalize_field_key(key);
	const FieldDesc *f = find_field(canonical_key);
	if (!f) {
		return httpd_send_error(fd, 404, "not_found",
			"unknown config field");
	}

	pthread_mutex_lock(&g_cfg_mutex);
	char *jval = field_to_json_value(f);
	pthread_mutex_unlock(&g_cfg_mutex);

	char buf[512];
	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{\"field\":\"%s\",\"value\":%s}}",
		key, jval);
	free(jval);
	return httpd_send_json(fd, 200, buf);
}

static int handle_awb(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_awb_info) {
		return httpd_send_error(fd, 501, "not_implemented",
			"AWB query not available");
	}
	char *json = g_cb->query_awb_info();
	if (!json) {
		return httpd_send_error(fd, 500, "internal_error",
			"AWB query failed");
	}
	int ret = httpd_send_json(fd, 200, json);
	free(json);
	return ret;
}

static int handle_iq(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_iq_info) {
		return httpd_send_error(fd, 501, "not_implemented",
			"IQ query not available");
	}
	char *json = g_cb->query_iq_info();
	if (!json) {
		return httpd_send_error(fd, 500, "internal_error",
			"IQ query failed");
	}
	int ret = httpd_send_json(fd, 200, json);
	free(json);
	return ret;
}

static int handle_iq_set(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
	if (!g_cb || !g_cb->apply_iq_param) {
		return httpd_send_error(fd, 501, "not_implemented",
			"IQ set not available");
	}
	char key[64], val[256];
	if (parse_first_query_param(req->query, key, sizeof(key),
			val, sizeof(val)) != 0 || !*key || !*val) {
		return httpd_send_error(fd, 400, "invalid_request",
			"usage: /api/v1/iq/set?param=value");
	}
	/* Validate value is numeric (with commas for arrays) */
	{
		const char *p = val;
		while (*p == '-' || *p == ',' || (*p >= '0' && *p <= '9')) p++;
		if (*p != '\0') {
			return httpd_send_error(fd, 400, "invalid_request",
				"value must be numeric (comma-separated for arrays)");
		}
	}
	if (g_cb->apply_iq_param(key, val) != 0) {
		return httpd_send_error(fd, 400, "apply_failed",
			"IQ parameter set failed");
	}
	char buf[512];
	if (strchr(val, ','))
		snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"param\":\"%s\",\"value\":[%s]}}",
			key, val);
	else
		snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"param\":\"%s\",\"value\":%s}}",
			key, val);
	return httpd_send_json(fd, 200, buf);
}

#if HAVE_BACKEND_STAR6E
extern int star6e_iq_import(const char *json_str);
#endif

static int handle_iq_import(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
#if HAVE_BACKEND_STAR6E
	if (req->body_len <= 0 || !req->body[0]) {
		return httpd_send_error(fd, 400, "invalid_request",
			"POST JSON body required (output of /api/v1/iq)");
	}
	int ret = star6e_iq_import(req->body);
	if (ret != 0)
		return httpd_send_error(fd, 500, "import_partial",
			"some parameters failed to apply");
	return httpd_send_ok(fd, "{\"imported\":true}");
#else
	(void)req;
	return httpd_send_error(fd, 501, "not_implemented",
		"IQ import not available on this backend");
#endif
}

static int handle_ae(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_ae_info) {
		return httpd_send_error(fd, 501, "not_implemented",
			"AE query not available");
	}
	char *json = g_cb->query_ae_info();
	if (!json) {
		return httpd_send_error(fd, 500, "internal_error",
			"AE query failed");
	}
	int ret = httpd_send_json(fd, 200, json);
	free(json);
	return ret;
}

static int handle_isp_metrics(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->query_isp_metrics) {
		return httpd_send_error(fd, 501, "not_implemented",
			"ISP metrics not available");
	}
	char *text = g_cb->query_isp_metrics();
	if (!text) {
		return httpd_send_error(fd, 500, "internal_error",
			"ISP metrics query failed");
	}
	int ret = httpd_send_text(fd, 200, text);
	free(text);
	return ret;
}

static int handle_restart(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	venc_api_request_reinit(1);
	return httpd_send_ok(fd, "{\"reinit\":true}");
}

static int handle_idr(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	if (!g_cb || !g_cb->request_idr) {
		return httpd_send_error(fd, 501, "not_implemented",
			"IDR request not available");
	}
	if (g_cb->request_idr() != 0) {
		return httpd_send_error(fd, 500, "internal_error",
			"IDR request failed");
	}
	return httpd_send_ok(fd, "{\"idr\":true}");
}

/* ── Record control endpoints ────────────────────────────────────────── */

static int handle_record_start(int fd, const HttpRequest *req, void *ctx)
{
	(void)ctx;
	char dir[256] = {0};
	char dummy[4];

	/* Optional ?dir=/path query parameter */
	if (req->query[0]) {
		char key[64];
		if (parse_first_query_param(req->query, key, sizeof(key),
				dir, sizeof(dir)) == 0 &&
		    strcmp(key, "dir") == 0 && dir[0]) {
			/* Use provided dir */
		} else {
			/* No dir= param, check if config has one */
			pthread_mutex_lock(&g_cfg_mutex);
			snprintf(dir, sizeof(dir), "%s",
				g_cfg->record.dir[0] ? g_cfg->record.dir :
				RECORDER_DEFAULT_DIR);
			pthread_mutex_unlock(&g_cfg_mutex);
		}
	} else {
		pthread_mutex_lock(&g_cfg_mutex);
		snprintf(dir, sizeof(dir), "%s",
			g_cfg->record.dir[0] ? g_cfg->record.dir :
			RECORDER_DEFAULT_DIR);
		pthread_mutex_unlock(&g_cfg_mutex);
	}

	(void)dummy;
	venc_api_request_record_start(dir);

	char buf[512];
	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{\"action\":\"start\",\"dir\":\"%s\"}}",
		dir);
	return httpd_send_json(fd, 200, buf);
}

static int handle_record_stop(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	venc_api_request_record_stop();
	return httpd_send_ok(fd, "{\"action\":\"stop\"}");
}

static int handle_record_status(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	VencRecordStatus st;
	char buf[1024];

	memset(&st, 0, sizeof(st));
	if (g_record_status_fn)
		g_record_status_fn(&st);

	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"active\":%s,"
		"\"format\":\"%s\","
		"\"path\":\"%s\","
		"\"frames\":%u,"
		"\"bytes\":%llu,"
		"\"segments\":%u,"
		"\"stop_reason\":\"%s\""
		"}}",
		st.active ? "true" : "false",
		st.format,
		st.path,
		st.frames_written,
		(unsigned long long)st.bytes_written,
		st.segments,
		st.stop_reason);
	return httpd_send_json(fd, 200, buf);
}

/* ── Dual VENC channel API ───────────────────────────────────────────── */

#include "star6e.h"  /* MI_VENC_* */
#include "star6e_controls.h"

static struct {
	int active;
	MI_VENC_CHN channel;
	uint32_t bitrate;   /* current kbps (may differ from config after adaptive) */
	uint32_t fps;
	uint32_t gop;
	bool frame_lost;
} g_dual;

/* Mutex protecting g_dual field access from the httpd thread.
 * Handlers run on the httpd pthread; register/unregister run on the
 * main thread.  This mutex prevents torn reads during registration
 * and ensures handlers don't start operations on a channel being
 * torn down. */
static pthread_mutex_t g_dual_mutex = PTHREAD_MUTEX_INITIALIZER;

void venc_api_dual_register(int channel, uint32_t bitrate, uint32_t fps,
	uint32_t gop, bool frame_lost)
{
	pthread_mutex_lock(&g_dual_mutex);
	g_dual.channel = (MI_VENC_CHN)channel;
	g_dual.bitrate = bitrate;
	g_dual.fps = fps;
	g_dual.gop = gop;
	g_dual.frame_lost = frame_lost;
	g_dual.active = 1;
	pthread_mutex_unlock(&g_dual_mutex);
}

void venc_api_dual_unregister(void)
{
	pthread_mutex_lock(&g_dual_mutex);
	g_dual.active = 0;
	pthread_mutex_unlock(&g_dual_mutex);
}

static int handle_dual_status(int fd, const HttpRequest *req, void *ctx)
{
	char buf[512];
	int ch;
	uint32_t br, fps, gop;

	(void)req; (void)ctx;

	pthread_mutex_lock(&g_dual_mutex);
	if (!g_dual.active) {
		pthread_mutex_unlock(&g_dual_mutex);
		return httpd_send_error(fd, 404, "not_active",
			"Dual VENC channel is not active");
	}
	ch = (int)g_dual.channel;
	br = g_dual.bitrate;
	fps = g_dual.fps;
	gop = g_dual.gop;
	pthread_mutex_unlock(&g_dual_mutex);

	snprintf(buf, sizeof(buf),
		"{\"ok\":true,\"data\":{"
		"\"active\":true,"
		"\"channel\":%d,"
		"\"bitrate\":%u,"
		"\"fps\":%u,"
		"\"gop\":%u"
		"}}",
		ch, br, fps, gop);
	return httpd_send_json(fd, 200, buf);
}

/* Apply bitrate/gop to ch1 via MI_VENC — same kernel ioctl pattern as ch0. */
static int dual_apply_bitrate(uint32_t kbps)
{
	MI_VENC_ChnAttr_t attr = {0};
	MI_U32 bits;

	if (kbps > 200000)
		kbps = 200000;
	bits = kbps * 1024;

	if (MI_VENC_GetChnAttr(g_dual.channel, &attr) != 0)
		return -1;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.bitrate = bits;  break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.bitrate = bits;  break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.maxBitrate = bits;  break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.maxBitrate = bits;  break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.maxBitrate = bits;  break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.maxBitrate = bits;  break;
	default:
		return -1;
	}

	if (MI_VENC_SetChnAttr(g_dual.channel, &attr) != 0)
		return -1;
#if HAVE_BACKEND_STAR6E
	if (star6e_controls_apply_frame_lost_threshold(g_dual.channel,
	    g_dual.frame_lost, kbps) != 0)
		return -1;
#endif
	g_dual.bitrate = kbps;
	return 0;
}

static int dual_apply_gop(uint32_t gop_frames)
{
	MI_VENC_ChnAttr_t attr = {0};

	if (MI_VENC_GetChnAttr(g_dual.channel, &attr) != 0)
		return -1;

	switch (attr.rate.mode) {
	case I6_VENC_RATEMODE_H265CBR:
		attr.rate.h265Cbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H264CBR:
		attr.rate.h264Cbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H265VBR:
		attr.rate.h265Vbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H264VBR:
		attr.rate.h264Vbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H265AVBR:
		attr.rate.h265Avbr.gop = gop_frames;  break;
	case I6_VENC_RATEMODE_H264AVBR:
		attr.rate.h264Avbr.gop = gop_frames;  break;
	default:
		return -1;
	}

	if (MI_VENC_SetChnAttr(g_dual.channel, &attr) != 0)
		return -1;
	g_dual.gop = gop_frames;
	return 0;
}

static int handle_dual_set(int fd, const HttpRequest *req, void *ctx)
{
	char buf[256];
	const char *q;
	int ret;

	(void)ctx;

	pthread_mutex_lock(&g_dual_mutex);
	if (!g_dual.active) {
		pthread_mutex_unlock(&g_dual_mutex);
		return httpd_send_error(fd, 404, "not_active",
			"Dual VENC channel is not active");
	}
	if (!*req->query) {
		pthread_mutex_unlock(&g_dual_mutex);
		return httpd_send_error(fd, 400, "missing_param",
			"Usage: /api/v1/dual/set?bitrate=N or ?gop=N");
	}

	q = req->query;

	if (strncmp(q, "bitrate=", 8) == 0) {
		char *end;
		unsigned long val = strtoul(q + 8, &end, 10);
		uint32_t kbps;
		if (end == q + 8 || (*end != '\0' && *end != '&') ||
		    val == 0 || val > 200000) {
			pthread_mutex_unlock(&g_dual_mutex);
			return httpd_send_error(fd, 400, "invalid_value",
				"bitrate must be 1-200000 kbps");
		}
		kbps = (uint32_t)val;
		ret = dual_apply_bitrate(kbps);
		pthread_mutex_unlock(&g_dual_mutex);
		if (ret != 0)
			return httpd_send_error(fd, 500, "apply_failed",
				"MI_VENC_SetChnAttr failed");
		snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"field\":\"bitrate\",\"value\":%u}}",
			kbps);
		return httpd_send_json(fd, 200, buf);
	}

	if (strncmp(q, "gop=", 4) == 0) {
		double gop_sec = atof(q + 4);
		uint32_t frames;
		if (gop_sec <= 0) {
			pthread_mutex_unlock(&g_dual_mutex);
			return httpd_send_error(fd, 400, "invalid_value",
				"gop must be > 0 (seconds)");
		}
		frames = (uint32_t)(gop_sec * g_dual.fps + 0.5);
		if (frames < 1) frames = 1;
		ret = dual_apply_gop(frames);
		pthread_mutex_unlock(&g_dual_mutex);
		if (ret != 0)
			return httpd_send_error(fd, 500, "apply_failed",
				"MI_VENC_SetChnAttr failed");
		snprintf(buf, sizeof(buf),
			"{\"ok\":true,\"data\":{\"field\":\"gop\",\"value\":%.2f,\"frames\":%u}}",
			gop_sec, frames);
		return httpd_send_json(fd, 200, buf);
	}

	pthread_mutex_unlock(&g_dual_mutex);
	return httpd_send_error(fd, 400, "unknown_param",
		"Supported: bitrate, gop");
}

static int handle_dual_idr(int fd, const HttpRequest *req, void *ctx)
{
	MI_VENC_CHN ch;
	int ret;

	(void)req; (void)ctx;

	pthread_mutex_lock(&g_dual_mutex);
	if (!g_dual.active) {
		pthread_mutex_unlock(&g_dual_mutex);
		return httpd_send_error(fd, 404, "not_active",
			"Dual VENC channel is not active");
	}
	ch = g_dual.channel;
	pthread_mutex_unlock(&g_dual_mutex);

	ret = MI_VENC_RequestIdr(ch, 1);
	if (ret != 0)
		return httpd_send_error(fd, 500, "idr_failed",
			"MI_VENC_RequestIdr failed");

	return httpd_send_json(fd, 200, "{\"ok\":true,\"data\":{\"idr\":true}}");
}

/* ── Sensor modes ────────────────────────────────────────────────────── */

static int handle_modes(int fd, const HttpRequest *req, void *ctx)
{
	(void)req; (void)ctx;
	pthread_mutex_lock(&g_cfg_mutex);
	int pad = g_sensor_pad, mode = g_sensor_mode, forced = g_sensor_forced_pad;
	pthread_mutex_unlock(&g_cfg_mutex);
	char *json = sensor_modes_json(forced, pad, mode);
	if (!json)
		return httpd_send_error(fd, 500, "modes_failed", "Failed to query sensor modes");
	int rc = httpd_send_json(fd, 200, json);
	free(json);
	return rc;
}

/* ── Registration ────────────────────────────────────────────────────── */

int venc_api_register(VencConfig *cfg, const char *backend_name,
	const VencApplyCallbacks *cb)
{
	int r = 0;

	pthread_mutex_lock(&g_cfg_mutex);
	g_cfg = cfg;
	g_cb = cb;
	snprintf(g_backend, sizeof(g_backend), "%s",
		backend_name ? backend_name : "unknown");
	if (g_api_routes_registered) {
		pthread_mutex_unlock(&g_cfg_mutex);
		return 0;
	}
	g_api_routes_registered = 1;
	pthread_mutex_unlock(&g_cfg_mutex);

	r |= venc_httpd_route("GET", "/api/v1/version",      handle_version, NULL);
	r |= venc_httpd_route("GET", "/api/v1/config",       handle_config, NULL);
	r |= venc_httpd_route("GET", "/api/v1/config.json",  handle_config, NULL);
	r |= venc_httpd_route("GET", "/api/v1/capabilities", handle_capabilities, NULL);
	r |= venc_httpd_route("GET", "/api/v1/set",          handle_set, NULL);
	r |= venc_httpd_route("GET", "/api/v1/get",          handle_get, NULL);
	r |= venc_httpd_route("GET", "/api/v1/fps/config",   handle_fps_config, NULL);
	r |= venc_httpd_route("GET", "/api/v1/fps/live",     handle_fps_live, NULL);
	r |= venc_httpd_route("GET", "/api/v1/restart",      handle_restart, NULL);
	r |= venc_httpd_route("GET", "/api/v1/ae",           handle_ae, NULL);
	r |= venc_httpd_route("GET", "/api/v1/awb",          handle_awb, NULL);
	r |= venc_httpd_route("GET", "/api/v1/iq/set",       handle_iq_set, NULL);
	r |= venc_httpd_route("POST", "/api/v1/iq/import",  handle_iq_import, NULL);
	r |= venc_httpd_route("GET", "/api/v1/iq",           handle_iq, NULL);
	r |= venc_httpd_route("GET", "/api/v1/modes",        handle_modes, NULL);
	r |= venc_httpd_route("GET", "/metrics/isp",         handle_isp_metrics, NULL);
	r |= venc_httpd_route("GET", "/request/idr",         handle_idr, NULL);
	r |= venc_httpd_route("GET", "/api/v1/record/start",  handle_record_start, NULL);
	r |= venc_httpd_route("GET", "/api/v1/record/stop",   handle_record_stop, NULL);
	r |= venc_httpd_route("GET", "/api/v1/record/status", handle_record_status, NULL);
	r |= venc_httpd_route("GET", "/api/v1/dual/status", handle_dual_status, NULL);
	r |= venc_httpd_route("GET", "/api/v1/dual/set",    handle_dual_set, NULL);
	r |= venc_httpd_route("GET", "/api/v1/dual/idr",    handle_dual_idr, NULL);
	r |= venc_webui_register();
	if (r != 0) {
		pthread_mutex_lock(&g_cfg_mutex);
		g_api_routes_registered = 0;
		pthread_mutex_unlock(&g_cfg_mutex);
		fprintf(stderr, "[api] ERROR: failed to register one or more routes\n");
		return -1;
	}
	return 0;
}
