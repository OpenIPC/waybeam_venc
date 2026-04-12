#include "maruko_runtime.h"

#include "maruko_config.h"
#include "maruko_controls.h"
#include "maruko_iq.h"
#include "maruko_pipeline.h"
#include "scene_detector.h"
#include "venc_api.h"
#include "venc_config.h"
#include "venc_httpd.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	VencConfig vcfg;
	MarukoBackendContext backend;
} MarukoRunnerContext;

static void maruko_bind_controls(MarukoRunnerContext *ctx)
{
	maruko_controls_bind(&ctx->backend, &ctx->vcfg);
}

static void maruko_reset_scene(MarukoBackendContext *backend)
{
	scene_init(&backend->scene, backend->cfg.scene_threshold,
		backend->cfg.scene_holdoff);
}

static int maruko_runner_init(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;
	MarukoBackendContext *backend = &ctx->backend;
	int ret;

	venc_httpd_start(ctx->vcfg.system.web_port);

	ret = maruko_pipeline_init(backend);
	if (ret != 0)
		return ret;

	ret = maruko_pipeline_configure_graph(backend);
	if (ret != 0)
		return ret;

	maruko_iq_init();
	maruko_bind_controls(ctx);
	maruko_reset_scene(backend);
	venc_api_register(&ctx->vcfg, "maruko", maruko_controls_callbacks());
	if (ctx->vcfg.video0.qp_delta != 0 &&
	    maruko_controls_callbacks()->apply_qp_delta) {
		maruko_controls_callbacks()->apply_qp_delta(
			ctx->vcfg.video0.qp_delta);
	}

	if (ctx->vcfg.audio.enabled) {
		fprintf(stderr, "WARNING: audio output is not supported on maruko backend\n");
	}

	return 0;
}

static int maruko_reinit_pipeline(MarukoRunnerContext *ctx)
{
	MarukoBackendContext *backend = &ctx->backend;
	int reinit_mode;
	int sys_initialized;
	int ret;

	/* Save current sensor state — live sensor mode changes are not
	 * supported (the ISP hangs).  We force the same mode after
	 * config reload so find_best_mode() cannot pick a different one. */
	int prev_pad = (int)backend->sensor.pad_id;
	int prev_mode = backend->sensor.mode_index;
	uint32_t prev_max_fps = backend->sensor.mode.maxFps;

	reinit_mode = venc_api_get_reinit();
	venc_api_clear_reinit();

	if (reinit_mode == 1) {
		printf("> [maruko] reinit: reloading config from disk\n");
		venc_config_defaults(&ctx->vcfg);
		(void)venc_config_load(VENC_CONFIG_DEFAULT_PATH, &ctx->vcfg);
	}

	/* Lock sensor to the mode that was running before teardown.
	 * Switching sensor modes on a live reinit hangs the ISP. */
	ctx->vcfg.sensor.index = prev_pad;
	ctx->vcfg.sensor.mode = prev_mode;
	if (prev_max_fps > 0 && ctx->vcfg.video0.fps > prev_max_fps) {
		printf("> [maruko] Reinit: clamping FPS %u -> %u "
			"(sensor mode change not supported during reinit)\n",
			ctx->vcfg.video0.fps, prev_max_fps);
		ctx->vcfg.video0.fps = prev_max_fps;
	}

	if (maruko_config_from_venc(&ctx->vcfg, &backend->cfg) != 0)
		return -1;

	/* Preserve MI_SYS state across reinit — MI_SYS_Init is only
	 * called once and MI_SYS_Exit only on final shutdown. */
	sys_initialized = backend->system_initialized;
	g_maruko_running = 1;
	backend->output.socket_handle = -1;
	backend->venc_channel = 0;
	backend->system_initialized = sys_initialized;

	if (!sys_initialized) {
		ret = maruko_pipeline_init(backend);
		if (ret != 0)
			return ret;
	}

	ret = maruko_pipeline_configure_graph(backend);
	if (ret != 0)
		return ret;

	maruko_bind_controls(ctx);
	maruko_reset_scene(backend);
	if (ctx->vcfg.video0.qp_delta != 0 &&
	    maruko_controls_callbacks()->apply_qp_delta) {
		maruko_controls_callbacks()->apply_qp_delta(
			ctx->vcfg.video0.qp_delta);
	}
	return 0;
}

static int maruko_runner_run(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;
	int result;

	for (;;) {
		result = maruko_pipeline_run(&ctx->backend);
		if (result != 1)
			break;

		printf("> [maruko] reinit: tearing down pipeline graph\n");
		maruko_pipeline_teardown_graph(&ctx->backend);

		if (maruko_reinit_pipeline(ctx) != 0) {
			result = -1;
			break;
		}
	}

	return result;
}

static void maruko_runner_teardown(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;

	maruko_pipeline_teardown(&ctx->backend);
}

static int maruko_prepare(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;

	setvbuf(stdout, NULL, _IONBF, 0);

	if (maruko_config_from_venc(&ctx->vcfg, &ctx->backend.cfg) != 0) {
		return 1;
	}

	printf("> Maruko backend selected\n");
	g_maruko_running = 1;
	maruko_pipeline_install_signal_handlers();

	ctx->backend.output.socket_handle = -1;
	ctx->backend.venc_channel = 0;
	return 0;
}

static VencConfig *maruko_config(void *opaque)
{
	MarukoRunnerContext *ctx = opaque;

	return &ctx->vcfg;
}

static int maruko_map_pipeline_result(int result)
{
	return result == 0 ? 0 : 2;
}

static const BackendOps g_backend_ops = {
	.name = "maruko",
	.config_path = VENC_CONFIG_DEFAULT_PATH,
	.context_size = sizeof(MarukoRunnerContext),
	.config = maruko_config,
	.prepare = maruko_prepare,
	.init = maruko_runner_init,
	.run = maruko_runner_run,
	.teardown = maruko_runner_teardown,
	.map_pipeline_result = maruko_map_pipeline_result,
};

const BackendOps *maruko_runtime_backend_ops(void)
{
	return &g_backend_ops;
}
