#include "venc_config.h"
#include "pipeline_common.h"
#include "star6e_recorder.h"
#include "../lib/cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Round a float to 6 significant digits before cJSON serialization.
 * Prevents artifacts like 0.001f -> 0.0010000000474974513 in JSON. */
static double float_clean(float v)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%.6g", (double)v);
	return strtod(buf, NULL);
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void safe_strcpy(char *dst, size_t dst_size, const char *src)
{
	if (!src) {
		dst[0] = '\0';
		return;
	}
	size_t len = strlen(src);
	if (len >= dst_size)
		len = dst_size - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static const char *json_get_string(const cJSON *obj, const char *key,
	const char *fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsString(item) && item->valuestring)
		return item->valuestring;
	return fallback;
}

static int json_get_int(const cJSON *obj, const char *key, int fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsNumber(item))
		return item->valueint;
	return fallback;
}

static bool json_get_bool(const cJSON *obj, const char *key, bool fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsBool(item))
		return cJSON_IsTrue(item) ? true : false;
	return fallback;
}

static double json_get_double(const cJSON *obj, const char *key, double fallback)
{
	const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsNumber(item))
		return item->valuedouble;
	return fallback;
}

/* ── Defaults ────────────────────────────────────────────────────────── */

void venc_config_defaults(VencConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	/* system */
	cfg->system.web_port = 80;
	cfg->system.overclock_level = 1;
	cfg->system.verbose = false;

	/* sensor */
	cfg->sensor.index = -1;
	cfg->sensor.mode = -1;
	cfg->sensor.unlock_enabled = true;
	cfg->sensor.unlock_cmd = 0x23;
	cfg->sensor.unlock_reg = 0x300a;
	cfg->sensor.unlock_value = 0x80;
	cfg->sensor.unlock_dir = 0;

	/* isp */
	cfg->isp.sensor_bin[0] = '\0';
	cfg->isp.exposure = 0;
	cfg->isp.legacy_ae = true;
	cfg->isp.ae_fps = 15;
	safe_strcpy(cfg->isp.awb_mode, sizeof(cfg->isp.awb_mode), "auto");
	cfg->isp.awb_ct = 5500;

	/* image */
	cfg->image.mirror = false;
	cfg->image.flip = false;
	cfg->image.rotate = 0;

	/* video0 */
	safe_strcpy(cfg->video0.codec, sizeof(cfg->video0.codec), "h265");
	safe_strcpy(cfg->video0.rc_mode, sizeof(cfg->video0.rc_mode), "cbr");
	cfg->video0.fps = 60;
	cfg->video0.width = 1920;
	cfg->video0.height = 1080;
	cfg->video0.bitrate = 8192;
	cfg->video0.gop_size = 1.0;
	cfg->video0.qp_delta = -4;
	cfg->video0.frame_lost = true;

	/* outgoing */
	cfg->outgoing.enabled = false;
	cfg->outgoing.server[0] = '\0';
	safe_strcpy(cfg->outgoing.stream_mode, sizeof(cfg->outgoing.stream_mode), "rtp");
	cfg->outgoing.max_payload_size = 1400;
	cfg->outgoing.connected_udp = true;

	/* fpv */
	cfg->fpv.roi_enabled = true;
	cfg->fpv.roi_qp = 0;
	cfg->fpv.roi_steps = 2;
	cfg->fpv.roi_center = 0.4;
	cfg->fpv.noise_level = 0;

	/* audio */
	cfg->audio.enabled = false;
	cfg->audio.sample_rate = 48000;
	cfg->audio.channels = 1;
	safe_strcpy(cfg->audio.codec, sizeof(cfg->audio.codec), "opus");
	cfg->audio.volume = 80;
	cfg->audio.mute = false;
	cfg->outgoing.audio_port = 5601;
	cfg->outgoing.sidecar_port = 5602;

	/* imu */
	cfg->imu.enabled = false;
	safe_strcpy(cfg->imu.i2c_device, sizeof(cfg->imu.i2c_device), "/dev/i2c-1");
	cfg->imu.i2c_addr = 0x68;
	cfg->imu.sample_rate_hz = 200;
	cfg->imu.gyro_range_dps = 1000;
	safe_strcpy(cfg->imu.cal_file, sizeof(cfg->imu.cal_file), "/etc/imu.cal");
	cfg->imu.cal_samples = 400;

	/* eis */
	cfg->eis.enabled = false;
	safe_strcpy(cfg->eis.mode, sizeof(cfg->eis.mode), "gyroglide");
	cfg->eis.margin_percent = 30;
	cfg->eis.test_mode = false;
	cfg->eis.swap_xy = false;
	cfg->eis.invert_x = false;
	cfg->eis.invert_y = false;
	cfg->eis.gain = 1.0f;
	cfg->eis.deadband_rad = 0.0f;
	cfg->eis.recenter_rate = 0.5f;
	cfg->eis.max_slew_px = 0.0f;
	cfg->eis.bias_alpha = 0.001f;

	/* record */
	cfg->record.enabled = false;
	safe_strcpy(cfg->record.dir, sizeof(cfg->record.dir), RECORDER_DEFAULT_DIR);
	safe_strcpy(cfg->record.format, sizeof(cfg->record.format), "ts");
	safe_strcpy(cfg->record.mode, sizeof(cfg->record.mode), "mirror");
	cfg->record.max_seconds = 300;
	cfg->record.max_mb = 500;
	cfg->record.bitrate = 0;
	cfg->record.fps = 0;
	cfg->record.gop_size = 0;
	cfg->record.server[0] = '\0';

	/* debug */
	cfg->debug.show_osd = false;
}

/* ── Load from JSON file ─────────────────────────────────────────────── */

static char *read_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	if (sz <= 0) {
		fclose(f);
		return NULL;
	}
	fseek(f, 0, SEEK_SET);

	char *buf = (char *)malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}

	size_t n = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[n] = '\0';
	if (out_len)
		*out_len = n;
	return buf;
}

static void load_system(const cJSON *root, VencConfigSystem *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "system");
	if (!obj) return;
	s->web_port = (uint16_t)json_get_int(obj, "webPort", s->web_port);
	s->overclock_level = json_get_int(obj, "overclockLevel", s->overclock_level);
	if (s->overclock_level < 0) s->overclock_level = 0;
	if (s->overclock_level > 2) s->overclock_level = 2;
	s->verbose = json_get_bool(obj, "verbose", s->verbose);
}

static void load_sensor(const cJSON *root, VencConfigSensor *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "sensor");
	if (!obj) return;
	s->index = json_get_int(obj, "index", s->index);
	s->mode = json_get_int(obj, "mode", s->mode);
	s->unlock_enabled = json_get_bool(obj, "unlockEnabled", s->unlock_enabled);
	s->unlock_cmd = (uint32_t)json_get_int(obj, "unlockCmd", (int)s->unlock_cmd);
	s->unlock_reg = (uint16_t)json_get_int(obj, "unlockReg", (int)s->unlock_reg);
	s->unlock_value = (uint16_t)json_get_int(obj, "unlockValue", (int)s->unlock_value);
	s->unlock_dir = json_get_int(obj, "unlockDir", s->unlock_dir);
	if (s->unlock_dir < 0) s->unlock_dir = 0;
	if (s->unlock_dir > 1) s->unlock_dir = 1;
}

static void load_isp(const cJSON *root, VencConfigIsp *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "isp");
	if (!obj) return;
	safe_strcpy(s->sensor_bin, sizeof(s->sensor_bin),
		json_get_string(obj, "sensorBin", s->sensor_bin));
	s->exposure = (uint32_t)json_get_int(obj, "exposure", (int)s->exposure);
	s->legacy_ae = json_get_bool(obj, "legacyAe", s->legacy_ae);
	s->ae_fps = (uint32_t)json_get_int(obj, "aeFps", (int)s->ae_fps);
	s->gain_max = (uint32_t)json_get_int(obj, "gainMax", (int)s->gain_max);
	safe_strcpy(s->awb_mode, sizeof(s->awb_mode),
		json_get_string(obj, "awbMode", s->awb_mode));
	s->awb_ct = (uint32_t)json_get_int(obj, "awbCt", (int)s->awb_ct);
}

static void load_image(const cJSON *root, VencConfigImage *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "image");
	if (!obj) return;
	s->mirror = json_get_bool(obj, "mirror", s->mirror);
	s->flip = json_get_bool(obj, "flip", s->flip);
	s->rotate = json_get_int(obj, "rotate", s->rotate);
	if (s->rotate == 180) {
		s->mirror = true;
		s->flip = true;
	} else {
		s->rotate = 0;
	}
}

static int parse_resolution(const char *str, uint32_t *w, uint32_t *h)
{
	/* Accept "WxH", "720p", "1080p", "4MP" */
	if (!strcmp(str, "720p")) {
		*w = 1280; *h = 720;
		return 0;
	}
	if (!strcmp(str, "1080p")) {
		*w = 1920; *h = 1080;
		return 0;
	}
	if (!strcmp(str, "4MP")) {
		*w = 2688; *h = 1520;
		return 0;
	}
	if (sscanf(str, "%ux%u", w, h) == 2)
		return 0;
	return -1;
}

static void load_video0(const cJSON *root, VencConfigVideo *v)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "video0");
	if (!obj) return;

	safe_strcpy(v->codec, sizeof(v->codec),
		json_get_string(obj, "codec", v->codec));
	safe_strcpy(v->rc_mode, sizeof(v->rc_mode),
		json_get_string(obj, "rcMode", v->rc_mode));
	v->fps = (uint32_t)json_get_int(obj, "fps", (int)v->fps);

	const char *size = json_get_string(obj, "size", NULL);
	if (size) {
		uint32_t w, h;
		if (parse_resolution(size, &w, &h) == 0) {
			v->width = w;
			v->height = h;
		} else {
			fprintf(stderr, "[venc_config] WARNING: invalid video0.size '%s', "
				"keeping %ux%u\n", size, v->width, v->height);
		}
	}

	v->bitrate = (uint32_t)json_get_int(obj, "bitrate", (int)v->bitrate);
	v->gop_size = json_get_double(obj, "gopSize", v->gop_size);
	v->qp_delta = json_get_int(obj, "qpDelta", v->qp_delta);
	if (v->qp_delta < -12) v->qp_delta = -12;
	if (v->qp_delta > 12) v->qp_delta = 12;
	v->frame_lost = json_get_bool(obj, "frameLost", v->frame_lost);
}

static void load_outgoing(const cJSON *root, VencConfigOutgoing *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "outgoing");
	if (!obj) return;
	s->enabled = json_get_bool(obj, "enabled", s->enabled);
	safe_strcpy(s->server, sizeof(s->server),
		json_get_string(obj, "server", s->server));
	safe_strcpy(s->stream_mode, sizeof(s->stream_mode),
		json_get_string(obj, "streamMode", s->stream_mode));
	s->max_payload_size = (uint16_t)json_get_int(obj, "maxPayloadSize",
		(int)s->max_payload_size);
	s->connected_udp = json_get_bool(obj, "connectedUdp", s->connected_udp);
	s->audio_port = (uint16_t)json_get_int(obj, "audioPort",
		(int)s->audio_port);
	s->sidecar_port = (uint16_t)json_get_int(obj, "sidecarPort",
		(int)s->sidecar_port);
}

static void load_audio(const cJSON *root, VencConfigAudio *a)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "audio");
	if (!obj) return;
	a->enabled = json_get_bool(obj, "enabled", a->enabled);
	a->sample_rate = (uint32_t)json_get_int(obj, "sampleRate",
		(int)a->sample_rate);
	if (a->sample_rate < 8000) a->sample_rate = 8000;
	if (a->sample_rate > 48000) a->sample_rate = 48000;
	a->channels = (uint32_t)json_get_int(obj, "channels",
		(int)a->channels);
	if (a->channels < 1) a->channels = 1;
	if (a->channels > 2) a->channels = 2;
	safe_strcpy(a->codec, sizeof(a->codec),
		json_get_string(obj, "codec", a->codec));
	a->volume = json_get_int(obj, "volume", a->volume);
	if (a->volume < 0) a->volume = 0;
	if (a->volume > 100) a->volume = 100;
	a->mute = json_get_bool(obj, "mute", a->mute);
}

static void load_imu(const cJSON *root, VencConfigImu *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "imu");
	if (!obj) return;
	s->enabled = json_get_bool(obj, "enabled", s->enabled);
	safe_strcpy(s->i2c_device, sizeof(s->i2c_device),
		json_get_string(obj, "i2cDevice", s->i2c_device));
	const char *addr_str = json_get_string(obj, "i2cAddr", NULL);
	if (addr_str)
		s->i2c_addr = (uint8_t)strtoul(addr_str, NULL, 0);
	s->sample_rate_hz = json_get_int(obj, "sampleRateHz", s->sample_rate_hz);
	if (s->sample_rate_hz < 25) s->sample_rate_hz = 25;
	if (s->sample_rate_hz > 1600) s->sample_rate_hz = 1600;
	s->gyro_range_dps = json_get_int(obj, "gyroRangeDps", s->gyro_range_dps);
	safe_strcpy(s->cal_file, sizeof(s->cal_file),
		json_get_string(obj, "calFile", s->cal_file));
	s->cal_samples = json_get_int(obj, "calSamples", s->cal_samples);
	if (s->cal_samples < 10) s->cal_samples = 10;
}

static void load_eis(const cJSON *root, VencConfigEis *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "eis");
	if (!obj) return;
	s->enabled = json_get_bool(obj, "enabled", s->enabled);
	safe_strcpy(s->mode, sizeof(s->mode),
		json_get_string(obj, "mode", s->mode));
	s->margin_percent = json_get_int(obj, "marginPercent", s->margin_percent);
	if (s->margin_percent < 1) s->margin_percent = 1;
	if (s->margin_percent > 30) s->margin_percent = 30;
	s->test_mode = json_get_bool(obj, "testMode", s->test_mode);
	s->swap_xy = json_get_bool(obj, "swapXY", s->swap_xy);
	s->invert_x = json_get_bool(obj, "invertX", s->invert_x);
	s->invert_y = json_get_bool(obj, "invertY", s->invert_y);
	s->gain = (float)json_get_double(obj, "gain", s->gain);
	if (s->gain < 0.0f) s->gain = 0.0f;
	if (s->gain > 1.0f) s->gain = 1.0f;
	s->deadband_rad = (float)json_get_double(obj, "deadbandRad",
		s->deadband_rad);
	if (s->deadband_rad < 0.0f) s->deadband_rad = 0.0f;
	s->recenter_rate = (float)json_get_double(obj, "recenterRate",
		s->recenter_rate);
	if (s->recenter_rate < 0.0f) s->recenter_rate = 0.0f;
	s->max_slew_px = (float)json_get_double(obj, "maxSlewPx",
		s->max_slew_px);
	if (s->max_slew_px < 0.0f) s->max_slew_px = 0.0f;
	s->bias_alpha = (float)json_get_double(obj, "biasAlpha",
		s->bias_alpha);
	if (s->bias_alpha < 0.0f) s->bias_alpha = 0.0f;
}

static void load_record(const cJSON *root, VencConfigRecord *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "record");
	if (!obj) return;
	s->enabled = json_get_bool(obj, "enabled", s->enabled);
	safe_strcpy(s->dir, sizeof(s->dir),
		json_get_string(obj, "dir", s->dir));
	safe_strcpy(s->format, sizeof(s->format),
		json_get_string(obj, "format", s->format));
	safe_strcpy(s->mode, sizeof(s->mode),
		json_get_string(obj, "mode", s->mode));
	s->max_seconds = (uint32_t)json_get_int(obj, "maxSeconds",
		(int)s->max_seconds);
	s->max_mb = (uint32_t)json_get_int(obj, "maxMB", (int)s->max_mb);
	s->bitrate = (uint32_t)json_get_int(obj, "bitrate", (int)s->bitrate);
	s->fps = (uint32_t)json_get_int(obj, "fps", (int)s->fps);
	s->gop_size = json_get_double(obj, "gopSize", s->gop_size);
	safe_strcpy(s->server, sizeof(s->server),
		json_get_string(obj, "server", s->server));
}

static void load_fpv(const cJSON *root, VencConfigFpv *s)
{
	const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "fpv");
	if (!obj) return;
	s->roi_enabled = json_get_bool(obj, "roiEnabled", s->roi_enabled);
	s->roi_qp = json_get_int(obj, "roiQp", s->roi_qp);
	if (s->roi_qp < -30) s->roi_qp = -30;
	if (s->roi_qp > 30) s->roi_qp = 30;
	s->roi_steps = (uint16_t)json_get_int(obj, "roiSteps", (int)s->roi_steps);
	if (s->roi_steps < 1) s->roi_steps = 1;
	if (s->roi_steps > PIPELINE_ROI_MAX_STEPS) s->roi_steps = PIPELINE_ROI_MAX_STEPS;
	s->roi_center = json_get_double(obj, "roiCenter", s->roi_center);
	if (s->roi_center < 0.1) s->roi_center = 0.1;
	if (s->roi_center > 0.9) s->roi_center = 0.9;
	s->noise_level = json_get_int(obj, "noiseLevel", s->noise_level);
	if (s->noise_level < 0) s->noise_level = 0;
	if (s->noise_level > 7) s->noise_level = 7;
}

int venc_config_load(const char *path, VencConfig *cfg)
{
	if (!cfg)
		return -1;

	size_t len = 0;
	char *data = read_file(path, &len);
	if (!data) {
		if (errno == ENOENT) {
			fprintf(stderr, "[venc_config] %s not found, using defaults\n", path);
			return 0;
		}
		fprintf(stderr, "[venc_config] ERROR: cannot read %s: %s\n",
			path, strerror(errno));
		return -1;
	}

	cJSON *root = cJSON_Parse(data);
	if (!root) {
		const char *err = cJSON_GetErrorPtr();
		fprintf(stderr, "[venc_config] ERROR: JSON parse error in %s near: %.30s\n",
			path, err ? err : "(unknown)");
		free(data);
		return -1;
	}
	free(data);

	load_system(root, &cfg->system);
	load_sensor(root, &cfg->sensor);
	load_isp(root, &cfg->isp);
	load_image(root, &cfg->image);
	load_video0(root, &cfg->video0);
	load_outgoing(root, &cfg->outgoing);
	load_fpv(root, &cfg->fpv);
	load_audio(root, &cfg->audio);
	load_imu(root, &cfg->imu);
	load_eis(root, &cfg->eis);
	load_record(root, &cfg->record);
	{
		const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "debug");
		if (obj)
			cfg->debug.show_osd = json_get_bool(obj, "showOsd",
				cfg->debug.show_osd);
	}

	cJSON_Delete(root);
	fprintf(stderr, "[venc_config] Loaded config from %s\n", path);
	return 0;
}

/* ── URI parser ──────────────────────────────────────────────────────── */

int venc_config_parse_server_uri(const char *uri, char *host, size_t host_len,
	uint16_t *port)
{
	if (!uri || !host || !port)
		return -1;

	const char *p = uri;

	if (strncmp(p, "udp://", 6) == 0) {
		p += 6;
	} else {
		fprintf(stderr, "[venc_config] ERROR: unsupported URI scheme in '%s' "
			"(expected udp://)\n", uri);
		return -1;
	}

	/* Find colon separating host:port */
	const char *colon = strrchr(p, ':');
	if (!colon || colon == p) {
		fprintf(stderr, "[venc_config] ERROR: missing host:port in '%s'\n", uri);
		return -1;
	}

	size_t hlen = (size_t)(colon - p);
	if (hlen >= host_len)
		hlen = host_len - 1;
	memcpy(host, p, hlen);
	host[hlen] = '\0';

	long pval = strtol(colon + 1, NULL, 10);
	if (pval <= 0 || pval > 65535) {
		fprintf(stderr, "[venc_config] ERROR: invalid port in '%s'\n", uri);
		return -1;
	}
	*port = (uint16_t)pval;

	return 0;
}

/* ── Serialize to JSON ────────────────────────────────────────────────── */

static cJSON *config_to_cjson(const VencConfig *cfg)
{
	cJSON *root = cJSON_CreateObject();
	if (!root)
		return NULL;

	/* system */
	cJSON *sys = cJSON_AddObjectToObject(root, "system");
	if (sys) {
		cJSON_AddNumberToObject(sys, "webPort", cfg->system.web_port);
		cJSON_AddNumberToObject(sys, "overclockLevel", cfg->system.overclock_level);
		cJSON_AddBoolToObject(sys, "verbose", cfg->system.verbose);
	}

	/* sensor */
	cJSON *snr = cJSON_AddObjectToObject(root, "sensor");
	if (snr) {
		cJSON_AddNumberToObject(snr, "index", cfg->sensor.index);
		cJSON_AddNumberToObject(snr, "mode", cfg->sensor.mode);
		cJSON_AddBoolToObject(snr, "unlockEnabled", cfg->sensor.unlock_enabled);
		cJSON_AddNumberToObject(snr, "unlockCmd", cfg->sensor.unlock_cmd);
		cJSON_AddNumberToObject(snr, "unlockReg", cfg->sensor.unlock_reg);
		cJSON_AddNumberToObject(snr, "unlockValue", cfg->sensor.unlock_value);
		cJSON_AddNumberToObject(snr, "unlockDir", cfg->sensor.unlock_dir);
	}

	/* isp */
	cJSON *isp = cJSON_AddObjectToObject(root, "isp");
	if (isp) {
		cJSON_AddStringToObject(isp, "sensorBin", cfg->isp.sensor_bin);
		cJSON_AddNumberToObject(isp, "exposure", cfg->isp.exposure);
		cJSON_AddBoolToObject(isp, "legacyAe", cfg->isp.legacy_ae);
		cJSON_AddNumberToObject(isp, "aeFps", cfg->isp.ae_fps);
		cJSON_AddNumberToObject(isp, "gainMax", cfg->isp.gain_max);
		cJSON_AddStringToObject(isp, "awbMode", cfg->isp.awb_mode);
		cJSON_AddNumberToObject(isp, "awbCt", cfg->isp.awb_ct);
	}

	/* image */
	cJSON *img = cJSON_AddObjectToObject(root, "image");
	if (img) {
		cJSON_AddBoolToObject(img, "mirror", cfg->image.mirror);
		cJSON_AddBoolToObject(img, "flip", cfg->image.flip);
		cJSON_AddNumberToObject(img, "rotate", cfg->image.rotate);
	}

	/* video0 */
	cJSON *vid = cJSON_AddObjectToObject(root, "video0");
	if (vid) {
		cJSON_AddStringToObject(vid, "codec", cfg->video0.codec);
		cJSON_AddStringToObject(vid, "rcMode", cfg->video0.rc_mode);
		cJSON_AddNumberToObject(vid, "fps", cfg->video0.fps);
		char size_buf[32];
		snprintf(size_buf, sizeof(size_buf), "%ux%u",
			cfg->video0.width, cfg->video0.height);
		cJSON_AddStringToObject(vid, "size", size_buf);
		cJSON_AddNumberToObject(vid, "bitrate", cfg->video0.bitrate);
		cJSON_AddNumberToObject(vid, "gopSize", cfg->video0.gop_size);
		cJSON_AddNumberToObject(vid, "qpDelta", cfg->video0.qp_delta);
		cJSON_AddBoolToObject(vid, "frameLost", cfg->video0.frame_lost);
	}

	/* outgoing */
	cJSON *out = cJSON_AddObjectToObject(root, "outgoing");
	if (out) {
		cJSON_AddBoolToObject(out, "enabled", cfg->outgoing.enabled);
		cJSON_AddStringToObject(out, "server", cfg->outgoing.server);
		cJSON_AddStringToObject(out, "streamMode", cfg->outgoing.stream_mode);
		cJSON_AddNumberToObject(out, "maxPayloadSize", cfg->outgoing.max_payload_size);
		cJSON_AddBoolToObject(out, "connectedUdp", cfg->outgoing.connected_udp);
		cJSON_AddNumberToObject(out, "audioPort", cfg->outgoing.audio_port);
		cJSON_AddNumberToObject(out, "sidecarPort", cfg->outgoing.sidecar_port);
	}

	/* fpv */
	cJSON *fpv = cJSON_AddObjectToObject(root, "fpv");
	if (fpv) {
		cJSON_AddBoolToObject(fpv, "roiEnabled", cfg->fpv.roi_enabled);
		cJSON_AddNumberToObject(fpv, "roiQp", cfg->fpv.roi_qp);
		cJSON_AddNumberToObject(fpv, "roiSteps", cfg->fpv.roi_steps);
		cJSON_AddNumberToObject(fpv, "roiCenter", cfg->fpv.roi_center);
		cJSON_AddNumberToObject(fpv, "noiseLevel", cfg->fpv.noise_level);
	}

	/* audio */
	cJSON *aud = cJSON_AddObjectToObject(root, "audio");
	if (aud) {
		cJSON_AddBoolToObject(aud, "enabled", cfg->audio.enabled);
		cJSON_AddNumberToObject(aud, "sampleRate", cfg->audio.sample_rate);
		cJSON_AddNumberToObject(aud, "channels", cfg->audio.channels);
		cJSON_AddStringToObject(aud, "codec", cfg->audio.codec);
		cJSON_AddNumberToObject(aud, "volume", cfg->audio.volume);
		cJSON_AddBoolToObject(aud, "mute", cfg->audio.mute);
	}

	/* imu */
	cJSON *imu = cJSON_AddObjectToObject(root, "imu");
	if (imu) {
		cJSON_AddBoolToObject(imu, "enabled", cfg->imu.enabled);
		cJSON_AddStringToObject(imu, "i2cDevice", cfg->imu.i2c_device);
		char addr_buf[8];
		snprintf(addr_buf, sizeof(addr_buf), "0x%02x", cfg->imu.i2c_addr);
		cJSON_AddStringToObject(imu, "i2cAddr", addr_buf);
		cJSON_AddNumberToObject(imu, "sampleRateHz", cfg->imu.sample_rate_hz);
		cJSON_AddNumberToObject(imu, "gyroRangeDps", cfg->imu.gyro_range_dps);
		cJSON_AddStringToObject(imu, "calFile", cfg->imu.cal_file);
		cJSON_AddNumberToObject(imu, "calSamples", cfg->imu.cal_samples);
	}

	/* eis */
	cJSON *eis = cJSON_AddObjectToObject(root, "eis");
	if (eis) {
		cJSON_AddBoolToObject(eis, "enabled", cfg->eis.enabled);
		cJSON_AddStringToObject(eis, "mode", cfg->eis.mode);
		cJSON_AddNumberToObject(eis, "marginPercent", cfg->eis.margin_percent);
		cJSON_AddBoolToObject(eis, "testMode", cfg->eis.test_mode);
		cJSON_AddBoolToObject(eis, "swapXY", cfg->eis.swap_xy);
		cJSON_AddBoolToObject(eis, "invertX", cfg->eis.invert_x);
		cJSON_AddBoolToObject(eis, "invertY", cfg->eis.invert_y);
		cJSON_AddNumberToObject(eis, "gain", float_clean(cfg->eis.gain));
		cJSON_AddNumberToObject(eis, "deadbandRad", float_clean(cfg->eis.deadband_rad));
		cJSON_AddNumberToObject(eis, "recenterRate", float_clean(cfg->eis.recenter_rate));
		cJSON_AddNumberToObject(eis, "maxSlewPx", float_clean(cfg->eis.max_slew_px));
		cJSON_AddNumberToObject(eis, "biasAlpha", float_clean(cfg->eis.bias_alpha));
	}

	/* record */
	cJSON *rec = cJSON_AddObjectToObject(root, "record");
	if (rec) {
		cJSON_AddBoolToObject(rec, "enabled", cfg->record.enabled);
		cJSON_AddStringToObject(rec, "dir", cfg->record.dir);
		cJSON_AddStringToObject(rec, "format", cfg->record.format);
		cJSON_AddStringToObject(rec, "mode", cfg->record.mode);
		cJSON_AddNumberToObject(rec, "maxSeconds", cfg->record.max_seconds);
		cJSON_AddNumberToObject(rec, "maxMB", cfg->record.max_mb);
		cJSON_AddNumberToObject(rec, "bitrate", cfg->record.bitrate);
		cJSON_AddNumberToObject(rec, "fps", cfg->record.fps);
		cJSON_AddNumberToObject(rec, "gopSize", cfg->record.gop_size);
		cJSON_AddStringToObject(rec, "server", cfg->record.server);
	}

	/* debug */
	cJSON *dbg = cJSON_AddObjectToObject(root, "debug");
	if (dbg)
		cJSON_AddBoolToObject(dbg, "showOsd", cfg->debug.show_osd);

	return root;
}

char *venc_config_to_json_string(const VencConfig *cfg)
{
	if (!cfg)
		return NULL;
	cJSON *root = config_to_cjson(cfg);
	if (!root)
		return NULL;
	char *str = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return str;
}

int venc_config_save(const char *path, const VencConfig *cfg)
{
	cJSON *root = config_to_cjson(cfg);
	if (!root) return -1;
	char *json = cJSON_Print(root);  /* pretty-printed */
	cJSON_Delete(root);
	if (!json) return -1;

	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "[venc_config] ERROR: cannot write %s: %s\n",
			path, strerror(errno));
		free(json);
		return -1;
	}
	fprintf(f, "%s\n", json);
	fclose(f);
	free(json);
	fprintf(stderr, "[venc_config] Config saved to %s\n", path);
	return 0;
}
