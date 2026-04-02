#include "enc_ctrl.h"
#include "test_helpers.h"

#include <string.h>

static int test_enc_ctrl_init_shutdown(void)
{
	GopConfig cfg;
	int failures = 0;

	gop_config_defaults(&cfg);

	CHECK("init ok", enc_ctrl_init(&cfg, -1, 0, 4000000, 60, 0) == 0);
	CHECK("is active", enc_ctrl_is_active() == 1);
	CHECK("double init fails", enc_ctrl_init(&cfg, -1, 0, 4000000, 60, 0) == -1);

	enc_ctrl_shutdown();
	CHECK("not active after shutdown", enc_ctrl_is_active() == 0);

	return failures;
}

static int test_enc_ctrl_frame_flow(void)
{
	GopConfig cfg;
	EncoderFrameStats stats, latest;
	SceneEstimate scene;
	int failures = 0;
	uint32_t i;

	gop_config_defaults(&cfg);
	enc_ctrl_init(&cfg, -1, 0, 4000000, 60, 0);

	/* Feed frames */
	for (i = 0; i < 10; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_size_bytes = 5000 + i * 100;
		stats.qp_avg = 28;
		stats.frame_type = ENC_FRAME_P;
		enc_ctrl_on_frame_stats(&stats);
	}

	CHECK("frame count", enc_ctrl_frame_count() == 10);

	CHECK("get latest ok", enc_ctrl_get_latest(&latest) == 0);
	CHECK("latest has data", latest.frame_size_bytes > 0);

	CHECK("get scene ok", enc_ctrl_get_scene(&scene) == 0);
	CHECK("scene has complexity", scene.complexity > 0);

	{
		GopState state;
		uint16_t fsi;
		CHECK("get gop state ok", enc_ctrl_get_gop_state(&state, &fsi) == 0);
		CHECK("gop state normal", state == GOP_STATE_NORMAL);
	}

	enc_ctrl_shutdown();

	return failures;
}

static int test_enc_ctrl_idr_request(void)
{
	GopConfig cfg;
	EncoderFrameStats stats;
	int failures = 0;
	uint32_t i;

	gop_config_defaults(&cfg);
	cfg.enable_variable_gop = 1;
	cfg.min_gop_length = 5;
	cfg.max_gop_length = 300;
	enc_ctrl_init(&cfg, -1, 0, 4000000, 60, 0);

	/* Feed enough frames to pass min_gop */
	for (i = 0; i < 10; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_size_bytes = 5000;
		stats.qp_avg = 28;
		stats.frame_type = ENC_FRAME_P;
		enc_ctrl_on_frame_stats(&stats);
	}

	/* Request IDR */
	CHECK("request idr ok", enc_ctrl_request_idr(4) == 0);

	/* Feed another frame — should trigger the IDR request */
	memset(&stats, 0, sizeof(stats));
	stats.frame_size_bytes = 5000;
	stats.qp_avg = 28;
	stats.frame_type = ENC_FRAME_P;
	enc_ctrl_on_frame_stats(&stats);

	enc_ctrl_shutdown();

	return failures;
}

static int test_enc_ctrl_history(void)
{
	GopConfig cfg;
	EncoderFrameStats stats;
	EncoderFrameStats buf[32];
	uint16_t count;
	int failures = 0;
	uint32_t i;

	gop_config_defaults(&cfg);
	enc_ctrl_init(&cfg, -1, 0, 4000000, 60, 0);

	for (i = 0; i < 20; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_size_bytes = 1000 * (i + 1);
		stats.qp_avg = 25;
		stats.frame_type = ENC_FRAME_P;
		enc_ctrl_on_frame_stats(&stats);
	}

	count = enc_ctrl_get_history(buf, 32);
	CHECK("history count", count == 20);
	CHECK("history first size", buf[0].frame_size_bytes == 1000);
	CHECK("history last size", buf[19].frame_size_bytes == 20000);

	enc_ctrl_shutdown();

	return failures;
}

static int test_enc_ctrl_not_init(void)
{
	SceneEstimate scene;
	EncoderFrameStats stats;
	int failures = 0;

	/* All calls should fail gracefully when not initialized */
	CHECK("scene not init", enc_ctrl_get_scene(&scene) == -1);
	CHECK("request not init", enc_ctrl_request_idr(0) == -1);
	CHECK("bitrate not init", enc_ctrl_set_bitrate(1000000) == -1);
	CHECK("frame not init", enc_ctrl_on_frame_stats(&stats) == -1);
	CHECK("not active", enc_ctrl_is_active() == 0);
	CHECK("count zero", enc_ctrl_frame_count() == 0);

	return failures;
}

int test_enc_ctrl(void)
{
	int failures = 0;

	failures += test_enc_ctrl_not_init();
	failures += test_enc_ctrl_init_shutdown();
	failures += test_enc_ctrl_frame_flow();
	failures += test_enc_ctrl_idr_request();
	failures += test_enc_ctrl_history();

	return failures;
}
