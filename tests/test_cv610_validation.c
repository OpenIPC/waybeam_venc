#include "cv610_modes.h"
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

static int expect(const char *name, int ok)
{
	printf("  %s  %s\n", ok ? "PASS" : "FAIL", name);
	return ok ? 0 : 1;
}

/* The mode table is the single source of truth for video0.size, video0.fps,
 * the MIPI bit depth, and the sensor MCLK.  The clock is the one that fails
 * silently on hardware (wrong clock => rate scales by the ratio, with every
 * status endpoint still reporting nominal), so assert it explicitly. */
static int test_mode_table(void)
{
	const Cv610SensorMode *modes;
	const Cv610SensorMode *m;
	size_t count = 0;
	size_t i;
	int failures = 0;

	modes = cv610_mode_table(&count);
	failures += expect("modes_table_non_empty", modes != NULL && count > 0);

	for (i = 0; i < count; i++) {
		m = cv610_mode_lookup(modes[i].width, modes[i].height,
			modes[i].fps);
		failures += expect("modes_every_entry_resolves", m == &modes[i]);
		failures += expect("modes_clock_set", modes[i].sensor_clock_hz != 0);
		failures += expect("modes_raw_bit_sane",
			modes[i].raw_bit == 10 || modes[i].raw_bit == 12);
	}

	/* Unset geometry means "the default mode's size", so fps alone picks. */
	m = cv610_mode_lookup(0, 0, modes[count - 1].fps);
	failures += expect("modes_auto_size_resolves", m == &modes[count - 1]);
	/* A half-set geometry is a typo, not a request. */
	failures += expect("modes_reject_half_set_size",
		cv610_mode_lookup(modes[0].width, 0, modes[0].fps) == NULL);
	failures += expect("modes_reject_unknown_fps",
		cv610_mode_lookup(modes[0].width, modes[0].height, 45) == NULL);
	failures += expect("modes_reject_unknown_size",
		cv610_mode_lookup(1280, 720, modes[0].fps) == NULL);

	/* Control: the 100 fps mode must not share the 30 fps mode's clock.
	 * If these ever converge the whole table is suspect. */
	{
		const Cv610SensorMode *slow = cv610_mode_lookup(1920, 1080, 30);
		const Cv610SensorMode *fast = cv610_mode_lookup(1920, 1080, 100);

		failures += expect("modes_1080p30_and_1080p100_present",
			slow != NULL && fast != NULL);
		if (slow && fast) {
			failures += expect("modes_100fps_uses_27mhz",
				fast->sensor_clock_hz == 27000000u);
			failures += expect("modes_30fps_uses_37125khz",
				slow->sensor_clock_hz == 37125000u);
		}
	}
	return failures;
}

int main(void)
{
	VencConfig cfg;
	int failures = 0;

	failures += test_mode_table();

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
	/* A size the sensor can produce, at a rate it cannot. */
	cfg.video0.width = 1920;
	cfg.video0.height = 1080;
	cfg.video0.fps = 45;
	failures += expect_valid("cv610_reject_size_ok_fps_bad", &cfg, 0);
	/* Half-set geometry must not widen into the default mode. */
	cfg.video0.fps = 60;
	cfg.video0.height = 0;
	failures += expect_valid("cv610_reject_half_set_size", &cfg, 0);
	/* Control: video0.size=auto is still accepted. */
	cfg.video0.width = 0;
	failures += expect_valid("cv610_accept_auto_size", &cfg, 1);
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
	cfg.outgoing.audio_port = 5601;
	strcpy(cfg.outgoing.server, "frame-shm://venc_frame_out");
	failures += expect_valid("cv610_accept_frame_shm_audio_sidechannel",
		&cfg, 1);
	cfg.outgoing.audio_port = 0;
	failures += expect_valid("cv610_reject_frame_shm_audio_port_zero",
		&cfg, 0);

	return failures ? 1 : 0;
}
