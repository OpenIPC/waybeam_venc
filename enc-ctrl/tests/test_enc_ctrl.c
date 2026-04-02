#include "enc_ctrl.h"
#include "test_helpers.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

static int test_enc_ctrl_init_shutdown(void)
{
	GopConfig cfg;
	int failures = 0;

	gop_config_defaults(&cfg);

	CHECK("init ok", enc_ctrl_init(&cfg, -1, 0, 4000000, 60) == 0);
	CHECK("is active", enc_ctrl_is_active() == 1);
	CHECK("double init fails", enc_ctrl_init(&cfg, -1, 0, 4000000, 60) == -1);

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
	enc_ctrl_init(&cfg, -1, 0, 4000000, 60);

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
	enc_ctrl_init(&cfg, -1, 0, 4000000, 60);

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

static int test_enc_ctrl_idr_waits_for_min_gop(void)
{
	GopConfig cfg;
	EncoderFrameStats stats;
	GopState state;
	uint16_t fsi = 0;
	int failures = 0;
	uint32_t i;

	gop_config_defaults(&cfg);
	cfg.enable_variable_gop = 1;
	cfg.min_gop_length = 5;
	cfg.max_gop_length = 300;
	enc_ctrl_init(&cfg, -1, 0, 4000000, 60);

	for (i = 0; i < 2; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_size_bytes = 5000;
		stats.qp_avg = 28;
		stats.frame_type = ENC_FRAME_P;
		enc_ctrl_on_frame_stats(&stats);
	}

	CHECK("request idr before min ok", enc_ctrl_request_idr(4) == 0);

	for (i = 0; i < 2; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_size_bytes = 5000;
		stats.qp_avg = 28;
		stats.frame_type = ENC_FRAME_P;
		enc_ctrl_on_frame_stats(&stats);
	}

	CHECK("pre-min get gop ok", enc_ctrl_get_gop_state(&state, &fsi) == 0);
	CHECK("pre-min state normal", state == GOP_STATE_NORMAL);
	CHECK("pre-min fsi", fsi == 4);

	memset(&stats, 0, sizeof(stats));
	stats.frame_size_bytes = 5000;
	stats.qp_avg = 28;
	stats.frame_type = ENC_FRAME_P;
	enc_ctrl_on_frame_stats(&stats);

	CHECK("at-min get gop ok", enc_ctrl_get_gop_state(&state, &fsi) == 0);
	CHECK("at-min idr ready", state == GOP_STATE_IDR_READY);
	CHECK("at-min fsi", fsi == 5);

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
	enc_ctrl_init(&cfg, -1, 0, 4000000, 60);

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

	/* Snapshot is non-destructive: second call returns same data */
	count = enc_ctrl_get_history(buf, 32);
	CHECK("history non-destructive", count == 20);
	CHECK("history repeat first", buf[0].frame_size_bytes == 1000);

	enc_ctrl_shutdown();

	return failures;
}

static int test_enc_ctrl_fps_updates_gop_lengths(void)
{
	GopConfig cfg;
	EncoderFrameStats stats;
	TelemetryRecord rec;
	GopState state;
	uint16_t fsi = 0;
	int failures = 0;
	uint32_t i;

	gop_config_defaults(&cfg);
	cfg.enable_variable_gop = 1;
	cfg.min_gop_length = 30;
	cfg.max_gop_length = 120;
	CHECK("fps update init ok", enc_ctrl_init(&cfg, -1, 0, 4000000, 60) == 0);

	for (i = 0; i < 90; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_size_bytes = 5000;
		stats.qp_avg = 28;
		stats.frame_type = ENC_FRAME_P;
		enc_ctrl_on_frame_stats(&stats);
	}

	CHECK("fps update pre-state", enc_ctrl_get_gop_state(&state, &fsi) == 0);
	CHECK("fps update pre-normal", state == GOP_STATE_NORMAL);
	CHECK("fps update pre-fsi", fsi == 90);

	CHECK("fps update call ok", enc_ctrl_set_fps(30, 60, 15) == 0);

	memset(&stats, 0, sizeof(stats));
	stats.frame_size_bytes = 5000;
	stats.qp_avg = 28;
	stats.frame_type = ENC_FRAME_P;
	enc_ctrl_on_frame_stats(&stats);

	CHECK("fps update post-state", enc_ctrl_get_gop_state(&state, &fsi) == 0);
	CHECK("fps update forced idr", state == GOP_STATE_FORCED_IDR);
	CHECK("fps update post-fsi", fsi == 91);

	memset(&stats, 0, sizeof(stats));
	stats.frame_size_bytes = 30000;
	stats.qp_avg = 30;
	stats.frame_type = ENC_FRAME_IDR;
	enc_ctrl_on_frame_stats(&stats);

	CHECK("fps update reset state", enc_ctrl_get_gop_state(&state, &fsi) == 0);
	CHECK("fps update reset normal", state == GOP_STATE_NORMAL);
	CHECK("fps update reset fsi", fsi == 0);
	CHECK("fps update telemetry ok", enc_ctrl_get_latest_telemetry(&rec) == 0);
	CHECK("fps update telemetry idr", rec.frame_type == ENC_FRAME_IDR);
	CHECK("fps update telemetry reset fsi", rec.frames_since_idr == 0);

	enc_ctrl_shutdown();
	return failures;
}

static void *enc_ctrl_control_thread(void *arg)
{
	uintptr_t i;

	(void)arg;

	for (i = 0; i < 2000; i++) {
		SceneEstimate scene;
		GopState state;

		if (enc_ctrl_request_idr((uint8_t)(i & 3)) != 0)
			return (void *)1;
		if (enc_ctrl_set_bitrate(3000000 + (uint32_t)(i % 4) * 250000) != 0)
			return (void *)1;
		{
			uint32_t fps = 30 + (uint32_t)(i % 3) * 15;
			uint16_t max_gop = (uint16_t)(fps * 2);
			uint16_t min_gop = (uint16_t)(fps / 2);

			if (enc_ctrl_set_fps(fps, max_gop, min_gop) != 0)
				return (void *)1;
		}
		if (enc_ctrl_get_scene(&scene) != 0)
			return (void *)1;
		if (enc_ctrl_get_gop_state(&state, NULL) != 0)
			return (void *)1;
	}

	return NULL;
}

static int test_enc_ctrl_thread_safety(void)
{
	GopConfig cfg;
	EncoderFrameStats stats;
	pthread_t worker;
	void *thread_rc = NULL;
	int failures = 0;
	int frame_feed_ok = 1;
	uint32_t i;

	gop_config_defaults(&cfg);
	cfg.enable_variable_gop = 1;
	cfg.min_gop_length = 5;
	cfg.max_gop_length = 120;
	CHECK("thread init ok", enc_ctrl_init(&cfg, -1, 0, 4000000, 60) == 0);

	if (pthread_create(&worker, NULL, enc_ctrl_control_thread, NULL) != 0) {
		enc_ctrl_shutdown();
		CHECK("thread create", 0);
		return failures;
	}

	for (i = 0; i < 2000; i++) {
		memset(&stats, 0, sizeof(stats));
		stats.frame_size_bytes = 4000 + (i % 7) * 200;
		stats.qp_avg = 24 + (uint8_t)(i % 5);
		stats.frame_type = (i % 61 == 60) ? ENC_FRAME_IDR : ENC_FRAME_P;
		if (enc_ctrl_on_frame_stats(&stats) != 0) {
			frame_feed_ok = 0;
			break;
		}
	}

	pthread_join(worker, &thread_rc);
	CHECK("thread frame feed", frame_feed_ok);
	CHECK("thread worker ok", thread_rc == NULL);

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
	failures += test_enc_ctrl_idr_waits_for_min_gop();
	failures += test_enc_ctrl_history();
	failures += test_enc_ctrl_fps_updates_gop_lengths();
	failures += test_enc_ctrl_thread_safety();

	return failures;
}
