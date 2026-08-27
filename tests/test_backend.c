#include "backend.h"

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
	VencConfig cfg;
	int prepared;
	int initialized;
} TestBackendContext;

static int g_config_calls;
static int g_prepare_calls;
static int g_init_calls;
static int g_run_calls;
static int g_teardown_calls;
static int g_map_calls;
static int g_prepare_result;
static int g_init_result;
static int g_run_result;
static int g_last_mapped_result;
static int g_zeroed_context_ok;
static int g_context_sequence_ok;
static int g_config_copied_ok;
static uint16_t g_expected_web_port;
static int g_expected_verbose;
/* When set, prepare() overwrites the context's copy of the config.  Used to
 * prove backend_execute() hands the backend its OWN copy and never a pointer
 * into the caller's. */
static uint16_t g_prepare_overwrites_web_port;

static void reset_backend_test_state(void)
{
	g_config_calls = 0;
	g_prepare_calls = 0;
	g_init_calls = 0;
	g_run_calls = 0;
	g_teardown_calls = 0;
	g_map_calls = 0;
	g_prepare_result = 0;
	g_init_result = 0;
	g_run_result = 0;
	g_last_mapped_result = 0;
	g_zeroed_context_ok = 1;
	g_context_sequence_ok = 1;
	g_config_copied_ok = 1;
	g_expected_web_port = 0;
	g_expected_verbose = 0;
	g_prepare_overwrites_web_port = 0;
}

/* Build the config main() would have parsed.  The backend no longer reads the
 * config file — it is read exactly once in main() and passed down — so these
 * tests hand backend_execute() a value rather than a path. */
static void make_config(VencConfig *cfg, uint16_t web_port, int verbose)
{
	venc_config_defaults(cfg);
	cfg->system.web_port = web_port;
	cfg->system.verbose = verbose != 0;
}

static VencConfig *test_backend_config(void *opaque)
{
	TestBackendContext *ctx = opaque;

	g_config_calls++;
	return &ctx->cfg;
}

static int test_backend_prepare(void *opaque)
{
	TestBackendContext *ctx = opaque;

	g_prepare_calls++;
	if (!ctx || ctx->prepared != 0 || ctx->initialized != 0) {
		g_zeroed_context_ok = 0;
	}
	if (!ctx) {
		return -99;
	}
	if (ctx->cfg.system.web_port != g_expected_web_port ||
	    ctx->cfg.system.verbose != (g_expected_verbose != 0)) {
		g_config_copied_ok = 0;
	}
	if (g_prepare_overwrites_web_port != 0) {
		ctx->cfg.system.web_port = g_prepare_overwrites_web_port;
	}

	ctx->prepared = 1;
	return g_prepare_result;
}

static int test_backend_init(void *opaque)
{
	TestBackendContext *ctx = opaque;

	g_init_calls++;
	if (!ctx || !ctx->prepared) {
		g_context_sequence_ok = 0;
		return -10;
	}

	ctx->initialized = 1;
	return g_init_result;
}

static int test_backend_run(void *opaque)
{
	TestBackendContext *ctx = opaque;

	g_run_calls++;
	if (!ctx || !ctx->prepared || !ctx->initialized) {
		g_context_sequence_ok = 0;
		return -11;
	}

	return g_run_result;
}

static void test_backend_teardown(void *opaque)
{
	TestBackendContext *ctx = opaque;

	g_teardown_calls++;
	if (!ctx || !ctx->prepared) {
		g_context_sequence_ok = 0;
	}
}

static int test_backend_map_result(int result)
{
	g_map_calls++;
	g_last_mapped_result = result;
	return result == 0 ? 0 : 42;
}

static BackendOps make_ops(void)
{
	BackendOps backend = {
		.name = "test",
		.context_size = sizeof(TestBackendContext),
		.config = test_backend_config,
		.prepare = test_backend_prepare,
		.init = test_backend_init,
		.run = test_backend_run,
		.teardown = test_backend_teardown,
		.map_pipeline_result = test_backend_map_result,
	};

	return backend;
}

static int test_backend_execute_success(void)
{
	BackendOps backend = make_ops();
	VencConfig cfg;
	int failures = 0;
	int ret;

	reset_backend_test_state();
	g_expected_web_port = 4321;
	g_expected_verbose = 1;
	make_config(&cfg, 4321, 1);
	ret = backend_execute(&backend, &cfg);

	CHECK("backend success return", ret == 0);
	CHECK("backend success config call", g_config_calls == 1);
	CHECK("backend success prepare", g_prepare_calls == 1);
	CHECK("backend success init", g_init_calls == 1);
	CHECK("backend success run", g_run_calls == 1);
	CHECK("backend success teardown", g_teardown_calls == 1);
	CHECK("backend success map", g_map_calls == 1);
	CHECK("backend success zeroed ctx", g_zeroed_context_ok);
	CHECK("backend success config copied", g_config_copied_ok);
	CHECK("backend success sequence", g_context_sequence_ok);
	CHECK("backend success mapped input", g_last_mapped_result == 0);
	return failures;
}

static int test_backend_execute_prepare_failure(void)
{
	BackendOps backend = make_ops();
	VencConfig cfg;
	int failures = 0;
	int ret;

	reset_backend_test_state();
	g_expected_web_port = 5555;
	make_config(&cfg, 5555, 0);
	g_prepare_result = 7;
	ret = backend_execute(&backend, &cfg);

	CHECK("backend prepare fail return", ret == 7);
	CHECK("backend prepare fail config call", g_config_calls == 1);
	CHECK("backend prepare fail prepare", g_prepare_calls == 1);
	CHECK("backend prepare fail init skipped", g_init_calls == 0);
	CHECK("backend prepare fail run skipped", g_run_calls == 0);
	CHECK("backend prepare fail teardown skipped", g_teardown_calls == 0);
	CHECK("backend prepare fail map skipped", g_map_calls == 0);
	CHECK("backend prepare fail config copied", g_config_copied_ok);
	return failures;
}

static int test_backend_execute_pipeline_mapping(void)
{
	BackendOps backend = make_ops();
	VencConfig cfg;
	int failures = 0;
	int ret;

	reset_backend_test_state();
	g_expected_web_port = 2468;
	make_config(&cfg, 2468, 0);
	g_run_result = -9;
	ret = backend_execute(&backend, &cfg);

	CHECK("backend map return", ret == 42);
	CHECK("backend map config call", g_config_calls == 1);
	CHECK("backend map prepare", g_prepare_calls == 1);
	CHECK("backend map init", g_init_calls == 1);
	CHECK("backend map run", g_run_calls == 1);
	CHECK("backend map teardown", g_teardown_calls == 1);
	CHECK("backend map called", g_map_calls == 1);
	CHECK("backend map input", g_last_mapped_result == -9);
	CHECK("backend map config copied", g_config_copied_ok);
	return failures;
}

/* The backend context owns a MUTABLE copy — the live-apply path writes to it
 * all through the run.  If backend_execute ever aliased the caller's config
 * instead of copying it, main()'s snapshot would drift under the beacon that
 * was started from the same struct. */
static int test_backend_execute_config_copy_is_independent(void)
{
	BackendOps backend = make_ops();
	VencConfig cfg;
	int failures = 0;
	int ret;

	reset_backend_test_state();
	g_expected_web_port = 1111;
	make_config(&cfg, 1111, 0);
	g_prepare_overwrites_web_port = 2222;
	ret = backend_execute(&backend, &cfg);

	CHECK("backend copy return", ret == 0);
	CHECK("backend copy reached backend", g_config_copied_ok);
	CHECK("backend copy caller untouched", cfg.system.web_port == 1111);
	return failures;
}

static int test_backend_execute_init_failure(void)
{
	BackendOps backend = make_ops();
	VencConfig cfg;
	int failures = 0;
	int ret;

	reset_backend_test_state();
	g_expected_web_port = 9876;
	g_init_result = -7;
	make_config(&cfg, 9876, 0);
	ret = backend_execute(&backend, &cfg);

	CHECK("backend init fail return", ret == 42);
	CHECK("backend init fail config call", g_config_calls == 1);
	CHECK("backend init fail prepare", g_prepare_calls == 1);
	CHECK("backend init fail init", g_init_calls == 1);
	CHECK("backend init fail run skipped", g_run_calls == 0);
	CHECK("backend init fail teardown", g_teardown_calls == 1);
	CHECK("backend init fail map called", g_map_calls == 1);
	CHECK("backend init fail mapped input", g_last_mapped_result == -7);
	CHECK("backend init fail config copied", g_config_copied_ok);
	return failures;
}

static int test_backend_execute_invalid_ops(void)
{
	BackendOps backend = {0};
	BackendOps complete = make_ops();
	VencConfig cfg;
	int failures = 0;
	int ret;

	reset_backend_test_state();
	make_config(&cfg, 1234, 0);

	ret = backend_execute(NULL, &cfg);
	CHECK("backend invalid null", ret == -1);

	/* A missing config is a caller bug, not a startup path: main() loads it
	 * and aborts on failure before backend_execute is ever reached. */
	ret = backend_execute(&complete, NULL);
	CHECK("backend invalid null config", ret == -1);
	CHECK("backend invalid null config no ctx", g_config_calls == 0);

	ret = backend_execute(&backend, &cfg);
	CHECK("backend invalid empty", ret == -1);

	backend.name = "test";
	backend.context_size = sizeof(TestBackendContext);
	backend.prepare = test_backend_prepare;
	ret = backend_execute(&backend, &cfg);
	CHECK("backend invalid missing config accessor", ret == -1);

	backend.config = test_backend_config;
	ret = backend_execute(&backend, &cfg);
	CHECK("backend invalid missing init", ret == -1);

	backend.init = test_backend_init;
	ret = backend_execute(&backend, &cfg);
	CHECK("backend invalid missing run", ret == -1);

	backend.run = test_backend_run;
	ret = backend_execute(&backend, &cfg);
	CHECK("backend invalid missing teardown", ret == -1);
	return failures;
}

int test_backend(void)
{
	int failures = 0;

	failures += test_backend_execute_success();
	failures += test_backend_execute_prepare_failure();
	failures += test_backend_execute_pipeline_mapping();
	failures += test_backend_execute_config_copy_is_independent();
	failures += test_backend_execute_init_failure();
	failures += test_backend_execute_invalid_ops();
	return failures;
}
