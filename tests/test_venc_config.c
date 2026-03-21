/*
 * Unit tests for venc_config.c
 *
 * Tests: defaults, JSON loading, URI parsing, round-trip serialization,
 * missing file fallback, bad JSON handling, resolution parsing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "venc_config.h"
#include "test_helpers.h"

/* ── Helper: write a temp JSON file ──────────────────────────────────── */

static char *write_temp_json(const char *json)
{
	char *path = strdup("/tmp/venc_test_XXXXXX");
	int fd = mkstemp(path);
	if (fd < 0) { free(path); return NULL; }
	write(fd, json, strlen(json));
	close(fd);
	return path;
}

/* ── Tests ───────────────────────────────────────────────────────────── */

static int test_defaults(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	CHECK("defaults_web_port", cfg.system.web_port == 80);
	CHECK("defaults_overclock", cfg.system.overclock_level == 1);
	CHECK("defaults_verbose", cfg.system.verbose == false);

	CHECK("defaults_sensor_index", cfg.sensor.index == -1);
	CHECK("defaults_sensor_mode", cfg.sensor.mode == -1);
	CHECK("defaults_unlock_enabled", cfg.sensor.unlock_enabled == true);
	CHECK("defaults_unlock_cmd", cfg.sensor.unlock_cmd == 0x23);
	CHECK("defaults_unlock_reg", cfg.sensor.unlock_reg == 0x300a);
	CHECK("defaults_unlock_value", cfg.sensor.unlock_value == 0x80);

	CHECK("defaults_exposure", cfg.isp.exposure == 0);
	CHECK("defaults_mirror", cfg.image.mirror == false);
	CHECK("defaults_flip", cfg.image.flip == false);

	CHECK("defaults_codec", strcmp(cfg.video0.codec, "h265") == 0);
	CHECK("defaults_rc_mode", strcmp(cfg.video0.rc_mode, "cbr") == 0);
	CHECK("defaults_fps", cfg.video0.fps == 60);
	CHECK("defaults_width", cfg.video0.width == 1920);
	CHECK("defaults_height", cfg.video0.height == 1080);
	CHECK("defaults_bitrate", cfg.video0.bitrate == 8192);
	CHECK("defaults_gop_size", cfg.video0.gop_size == 1.0);
	CHECK("defaults_qp_delta", cfg.video0.qp_delta == -4);
	CHECK("defaults_frame_lost", cfg.video0.frame_lost == true);
	CHECK("defaults_enabled", cfg.outgoing.enabled == false);
	CHECK("defaults_server", cfg.outgoing.server[0] == '\0');
	CHECK("defaults_stream_mode", strcmp(cfg.outgoing.stream_mode, "rtp") == 0);
	CHECK("defaults_payload", cfg.outgoing.max_payload_size == 1400);
	CHECK("defaults_connected_udp", cfg.outgoing.connected_udp == true);

	CHECK("defaults_roi_on", cfg.fpv.roi_enabled == true);
	CHECK("defaults_roi_qp", cfg.fpv.roi_qp == 0);
	CHECK("defaults_roi_steps", cfg.fpv.roi_steps == 2);
	CHECK("defaults_noise", cfg.fpv.noise_level == 0);

	CHECK("defaults_audio_off", cfg.audio.enabled == false);
	CHECK("defaults_audio_rate", cfg.audio.sample_rate == 48000);
	CHECK("defaults_audio_ch", cfg.audio.channels == 1);
	CHECK("defaults_audio_codec", strcmp(cfg.audio.codec, "opus") == 0);
	CHECK("defaults_audio_vol", cfg.audio.volume == 80);
	CHECK("defaults_audio_port", cfg.outgoing.audio_port == 5601);

	return failures;
}

static int test_load_full_json(void)
{
	int failures = 0;
	const char *json =
		"{"
		"  \"system\": { \"webPort\": 8080, \"overclockLevel\": 1, \"verbose\": true },"
		"  \"sensor\": { \"index\": 2, \"mode\": 3, \"unlockEnabled\": false },"
		"  \"isp\": { \"sensorBin\": \"/etc/sensors/imx415.bin\", \"exposure\": 5000 },"
		"  \"image\": { \"mirror\": true, \"flip\": true },"
		"  \"video0\": { \"codec\": \"h264\", \"rcMode\": \"vbr\", \"fps\": 90,"
		"    \"size\": \"1280x720\", \"bitrate\": 4096, \"gopSize\": 1, \"qpDelta\": -7,"
		"    \"frameLost\": false },"
		"  \"outgoing\": { \"enabled\": true, \"server\": \"udp://10.0.0.1:6000\", \"streamMode\": \"compact\", \"maxPayloadSize\": 1200, \"connectedUdp\": false },"
		"  \"fpv\": { \"roiEnabled\": true, \"roiQp\": -18, \"roiSteps\": 2, \"noiseLevel\": 5 }"
		"}";

	char *path = write_temp_json(json);
	CHECK("tmpfile_created", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("load_ok", ret == 0);
	CHECK("load_web_port", cfg.system.web_port == 8080);
	CHECK("load_overclock", cfg.system.overclock_level == 1);
	CHECK("load_verbose", cfg.system.verbose == true);
	CHECK("load_sensor_index", cfg.sensor.index == 2);
	CHECK("load_sensor_mode", cfg.sensor.mode == 3);
	CHECK("load_unlock_off", cfg.sensor.unlock_enabled == false);
	CHECK("load_isp_bin", strcmp(cfg.isp.sensor_bin, "/etc/sensors/imx415.bin") == 0);
	CHECK("load_exposure", cfg.isp.exposure == 5000);
	CHECK("load_mirror", cfg.image.mirror == true);
	CHECK("load_flip", cfg.image.flip == true);
	CHECK("load_codec", strcmp(cfg.video0.codec, "h264") == 0);
	CHECK("load_rc", strcmp(cfg.video0.rc_mode, "vbr") == 0);
	CHECK("load_fps", cfg.video0.fps == 90);
	CHECK("load_width", cfg.video0.width == 1280);
	CHECK("load_height", cfg.video0.height == 720);
	CHECK("load_bitrate", cfg.video0.bitrate == 4096);
	CHECK("load_gop", cfg.video0.gop_size == 1);
	CHECK("load_qp_delta", cfg.video0.qp_delta == -7);
	CHECK("load_frame_lost_off", cfg.video0.frame_lost == false);
	CHECK("load_enabled", cfg.outgoing.enabled == true);
	CHECK("load_server", strcmp(cfg.outgoing.server, "udp://10.0.0.1:6000") == 0);
	CHECK("load_stream_mode", strcmp(cfg.outgoing.stream_mode, "compact") == 0);
	CHECK("load_payload", cfg.outgoing.max_payload_size == 1200);
	CHECK("load_connected_udp", cfg.outgoing.connected_udp == false);
	CHECK("load_roi_on", cfg.fpv.roi_enabled == true);
	CHECK("load_roi_qp", cfg.fpv.roi_qp == -18);
	CHECK("load_roi_steps", cfg.fpv.roi_steps == 2);
	CHECK("load_noise", cfg.fpv.noise_level == 5);

	return failures;
}

static int test_load_partial_json(void)
{
	int failures = 0;
	const char *json = "{ \"video0\": { \"fps\": 120 } }";

	char *path = write_temp_json(json);
	CHECK("partial_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("partial_ok", ret == 0);
	CHECK("partial_fps_set", cfg.video0.fps == 120);
	/* All other fields retain defaults */
	CHECK("partial_bitrate_default", cfg.video0.bitrate == 8192);
	CHECK("partial_codec_default", strcmp(cfg.video0.codec, "h265") == 0);
	CHECK("partial_web_port_default", cfg.system.web_port == 80);

	return failures;
}

static int test_load_missing_file(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load("/tmp/nonexistent_venc_test_file.json", &cfg);
	CHECK("missing_file_ok", ret == 0);
	/* Defaults preserved */
	CHECK("missing_defaults_fps", cfg.video0.fps == 60);
	return failures;
}

static int test_load_bad_json(void)
{
	int failures = 0;
	const char *bad = "{ this is not json }";
	char *path = write_temp_json(bad);
	CHECK("bad_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("bad_json_fails", ret == -1);
	return failures;
}

static int test_uri_parsing(void)
{
	int failures = 0;
	char host[64];
	uint16_t port;

	/* UDP URI */
	int ret = venc_config_parse_server_uri("udp://10.0.0.1:6000",
		host, sizeof(host), &port);
	CHECK("udp_ok", ret == 0);
	CHECK("udp_host", strcmp(host, "10.0.0.1") == 0);
	CHECK("udp_port", port == 6000);

	/* Bad scheme */
	ret = venc_config_parse_server_uri("http://bad:80",
		host, sizeof(host), &port);
	CHECK("bad_scheme_fails", ret == -1);

	/* rtp:// scheme rejected */
	ret = venc_config_parse_server_uri("rtp://192.168.1.2:5600",
		host, sizeof(host), &port);
	CHECK("rtp_scheme_fails", ret == -1);

	/* Missing port */
	ret = venc_config_parse_server_uri("udp://host",
		host, sizeof(host), &port);
	CHECK("missing_port_fails", ret == -1);

	/* NULL args */
	ret = venc_config_parse_server_uri(NULL, host, sizeof(host), &port);
	CHECK("null_uri_fails", ret == -1);

	return failures;
}

static int test_roundtrip(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);
	cfg.video0.fps = 90;
	cfg.video0.bitrate = 12000;
	cfg.video0.qp_delta = 6;
	cfg.system.verbose = true;

	char *json = venc_config_to_json_string(&cfg);
	CHECK("serialize_ok", json != NULL);
	if (!json) return failures;

	/* Write serialized JSON to file and reload */
	char *path = write_temp_json(json);
	free(json);
	CHECK("roundtrip_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg2;
	venc_config_defaults(&cfg2);
	int ret = venc_config_load(path, &cfg2);
	unlink(path);
	free(path);

	CHECK("roundtrip_load_ok", ret == 0);
	CHECK("roundtrip_fps", cfg2.video0.fps == 90);
	CHECK("roundtrip_bitrate", cfg2.video0.bitrate == 12000);
	CHECK("roundtrip_qp_delta", cfg2.video0.qp_delta == 6);
	CHECK("roundtrip_verbose", cfg2.system.verbose == true);
	/* Unchanged fields preserved */
	CHECK("roundtrip_codec", strcmp(cfg2.video0.codec, "h265") == 0);
	CHECK("roundtrip_gop", cfg2.video0.gop_size == 1.0);

	return failures;
}

static int test_overclock_clamping(void)
{
	int failures = 0;
	const char *json = "{ \"system\": { \"overclockLevel\": 5 } }";
	char *path = write_temp_json(json);
	CHECK("clamp_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("overclock_clamped_to_2", cfg.system.overclock_level == 2);
	return failures;
}

static int test_noise_level_clamping(void)
{
	int failures = 0;
	const char *json = "{ \"fpv\": { \"noiseLevel\": 10 } }";
	char *path = write_temp_json(json);
	CHECK("noise_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("noise_clamped_to_7", cfg.fpv.noise_level == 7);
	return failures;
}

static int test_resolution_aliases(void)
{
	int failures = 0;

	/* 720p alias */
	const char *json_720 = "{ \"video0\": { \"size\": \"720p\" } }";
	char *path = write_temp_json(json_720);
	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("720p_width", cfg.video0.width == 1280);
	CHECK("720p_height", cfg.video0.height == 720);

	/* 1080p alias */
	const char *json_1080 = "{ \"video0\": { \"size\": \"1080p\" } }";
	path = write_temp_json(json_1080);
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("1080p_width", cfg.video0.width == 1920);
	CHECK("1080p_height", cfg.video0.height == 1080);

	/* 4MP alias */
	const char *json_4mp = "{ \"video0\": { \"size\": \"4MP\" } }";
	path = write_temp_json(json_4mp);
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("4mp_width", cfg.video0.width == 2688);
	CHECK("4mp_height", cfg.video0.height == 1520);

	/* WxH format */
	const char *json_wxh = "{ \"video0\": { \"size\": \"640x480\" } }";
	path = write_temp_json(json_wxh);
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("wxh_width", cfg.video0.width == 640);
	CHECK("wxh_height", cfg.video0.height == 480);

	return failures;
}

static int test_rotate_180(void)
{
	int failures = 0;
	const char *json = "{ \"image\": { \"rotate\": 180 } }";
	char *path = write_temp_json(json);
	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("rotate180_mirror", cfg.image.mirror == true);
	CHECK("rotate180_flip", cfg.image.flip == true);
	return failures;
}

static int test_sample_config_file(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);

	/* Load the shipped sample config relative to the test binary's
	 * expected working directory (repo root) */
	int ret = venc_config_load("config/venc.default.json", &cfg);
	CHECK("sample_load_ok", ret == 0);
	if (ret != 0) return failures;

	CHECK("sample_fps_30", cfg.video0.fps == 60);
	CHECK("sample_codec_h265", strcmp(cfg.video0.codec, "h265") == 0);
	CHECK("sample_enabled", cfg.outgoing.enabled == false);
	CHECK("sample_server", strcmp(cfg.outgoing.server, "") == 0);
	CHECK("sample_stream_mode", strcmp(cfg.outgoing.stream_mode, "rtp") == 0);
	CHECK("sample_bitrate", cfg.video0.bitrate == 8192);
	CHECK("sample_web_port", cfg.system.web_port == 80);
	CHECK("sample_audio_off", cfg.audio.enabled == false);
	CHECK("sample_audio_port", cfg.outgoing.audio_port == 5601);

	return failures;
}

static int test_audio_config(void)
{
	int failures = 0;
	const char *json =
		"{"
		"  \"audio\": { \"enabled\": true, \"sampleRate\": 48000,"
		"    \"channels\": 2, \"codec\": \"g711a\", \"volume\": 50 },"
		"  \"outgoing\": { \"audioPort\": 5700 }"
		"}";

	char *path = write_temp_json(json);
	CHECK("audio_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg;
	venc_config_defaults(&cfg);
	int ret = venc_config_load(path, &cfg);
	unlink(path);
	free(path);

	CHECK("audio_load_ok", ret == 0);
	CHECK("audio_enabled", cfg.audio.enabled == true);
	CHECK("audio_rate", cfg.audio.sample_rate == 48000);
	CHECK("audio_channels", cfg.audio.channels == 2);
	CHECK("audio_codec", strcmp(cfg.audio.codec, "g711a") == 0);
	CHECK("audio_volume", cfg.audio.volume == 50);
	CHECK("audio_port", cfg.outgoing.audio_port == 5700);

	return failures;
}

static int test_audio_volume_clamping(void)
{
	int failures = 0;

	/* Volume > 100 should clamp to 100 */
	const char *json_high = "{ \"audio\": { \"volume\": 150 } }";
	char *path = write_temp_json(json_high);
	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("vol_clamped_100", cfg.audio.volume == 100);

	/* Volume < 0 should clamp to 0 */
	const char *json_low = "{ \"audio\": { \"volume\": -10 } }";
	path = write_temp_json(json_low);
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("vol_clamped_0", cfg.audio.volume == 0);

	return failures;
}

static int test_audio_channel_clamping(void)
{
	int failures = 0;

	const char *json = "{ \"audio\": { \"channels\": 5 } }";
	char *path = write_temp_json(json);
	VencConfig cfg;
	venc_config_defaults(&cfg);
	venc_config_load(path, &cfg);
	unlink(path);
	free(path);
	CHECK("ch_clamped_2", cfg.audio.channels == 2);

	return failures;
}

static int test_audio_roundtrip(void)
{
	int failures = 0;
	VencConfig cfg;
	venc_config_defaults(&cfg);
	cfg.audio.enabled = true;
	cfg.audio.sample_rate = 48000;
	cfg.audio.channels = 2;
	snprintf(cfg.audio.codec, sizeof(cfg.audio.codec), "g711u");
	cfg.audio.volume = 60;
	cfg.outgoing.audio_port = 5700;

	char *json = venc_config_to_json_string(&cfg);
	CHECK("audio_serialize_ok", json != NULL);
	if (!json) return failures;

	char *path = write_temp_json(json);
	free(json);
	CHECK("audio_rt_tmpfile", path != NULL);
	if (!path) return failures;

	VencConfig cfg2;
	venc_config_defaults(&cfg2);
	int ret = venc_config_load(path, &cfg2);
	unlink(path);
	free(path);

	CHECK("audio_rt_load_ok", ret == 0);
	CHECK("audio_rt_enabled", cfg2.audio.enabled == true);
	CHECK("audio_rt_rate", cfg2.audio.sample_rate == 48000);
	CHECK("audio_rt_ch", cfg2.audio.channels == 2);
	CHECK("audio_rt_codec", strcmp(cfg2.audio.codec, "g711u") == 0);
	CHECK("audio_rt_vol", cfg2.audio.volume == 60);
	CHECK("audio_rt_port", cfg2.outgoing.audio_port == 5700);

	return failures;
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int test_venc_config(void)
{
	int failures = 0;
	failures += test_defaults();
	failures += test_load_full_json();
	failures += test_load_partial_json();
	failures += test_load_missing_file();
	failures += test_load_bad_json();
	failures += test_uri_parsing();
	failures += test_roundtrip();
	failures += test_overclock_clamping();
	failures += test_noise_level_clamping();
	failures += test_resolution_aliases();
	failures += test_rotate_180();
	failures += test_sample_config_file();
	failures += test_audio_config();
	failures += test_audio_volume_clamping();
	failures += test_audio_channel_clamping();
	failures += test_audio_roundtrip();
	return failures;
}
