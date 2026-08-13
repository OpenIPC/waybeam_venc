#include "venc_api.h"
#include "venc_config.h"

#include <stdio.h>
#include <string.h>

static int expect_valid(const char *name, VencConfig *cfg, int valid)
{
	const char *error = venc_api_validate_loaded_config(cfg);
	int ok = valid ? error == NULL : error != NULL;

	printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name,
		error ? ": " : "", error ? error : "");
	return ok ? 0 : 1;
}

int main(void)
{
	VencConfig cfg;
	int failures = 0;

	venc_config_defaults(&cfg);
	failures += expect_valid("cv610_defaults", &cfg, 1);
	if (venc_config_load("config/waybeam.default.cv610.json", &cfg) != 0) {
		printf("  FAIL  cv610_sample_load\n");
		return 1;
	}
	failures += expect_valid("cv610_sample", &cfg, 1);
	cfg.video0.fps = 120;
	failures += expect_valid("cv610_reject_fps", &cfg, 0);
	cfg.video0.fps = 60;
	cfg.video0.width = 1280;
	cfg.video0.height = 720;
	failures += expect_valid("cv610_reject_size", &cfg, 0);
	cfg.video0.width = 1920;
	cfg.video0.height = 1080;
	strcpy(cfg.outgoing.server, "shm://venc_wfb");
	failures += expect_valid("cv610_reject_packet_shm", &cfg, 0);
	strcpy(cfg.outgoing.server, "unix://venc_wfb");
	failures += expect_valid("cv610_accept_unix", &cfg, 1);
	strcpy(cfg.video0.rc_mode, "vbr");
	failures += expect_valid("cv610_reject_vbr", &cfg, 0);
	strcpy(cfg.video0.rc_mode, "cbr");
	strcpy(cfg.video0.framing, "stab");
	failures += expect_valid("cv610_reject_framing", &cfg, 0);
	strcpy(cfg.video0.framing, "off");
	cfg.video0.gop_size = 2000.0;
	failures += expect_valid("cv610_reject_gop", &cfg, 0);
	cfg.video0.gop_size = 3.0;
	cfg.audio.enabled = true;
	strcpy(cfg.audio.codec, "opus");
	cfg.audio.sample_rate = 48000;
	cfg.audio.channels = 1;
	cfg.outgoing.audio_port = 5601;
	failures += expect_valid("cv610_accept_opus", &cfg, 1);
	cfg.audio.sample_rate = 16000;
	failures += expect_valid("cv610_reject_audio_rate", &cfg, 0);
	cfg.audio.sample_rate = 48000;
	cfg.audio.channels = 2;
	failures += expect_valid("cv610_reject_audio_channels", &cfg, 0);
	cfg.audio.channels = 1;
	strcpy(cfg.audio.codec, "pcm");
	failures += expect_valid("cv610_reject_audio_codec", &cfg, 0);
	strcpy(cfg.audio.codec, "opus");
	cfg.outgoing.audio_port = -1;
	failures += expect_valid("cv610_reject_invalid_audio_port", &cfg, 0);

	return failures ? 1 : 0;
}
