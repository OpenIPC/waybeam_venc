#include "backend.h"

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
static int g_config_loaded_ok;
static uint16_t g_expected_web_port;
static int g_expected_verbose;

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
	g_config_loaded_ok = 1;
	g_expected_web_port = 0;
	g_expected_verbose = 0;
}

static int create_temp_config(char *path, size_t path_size, const char *json)
{
	char tmpl[] = "/tmp/test_backend_XXXXXX";
	int fd;
	FILE *file;

	if (!path || path_size == 0 || !json) {
		return -1;
	}

	fd = mkstemp(tmpl);
	if (fd < 0) {
		return -1;
	}

	file = fdopen(fd, "w");
	if (!file) {
		close(fd);
		unlink(tmpl);
		return -1;
	}

	if (fputs(json, file) == EOF || fclose(file) != 0) {
		unlink(tmpl);
		return -1;
	}

	snprintf(path, path_size, "%s", tmpl);
	return 0;
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
		g_config_loaded_ok = 0;
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

static int test_backend_execute_success(void)
{
	char config_path[64] = {0};
	BackendOps backend = {
		.name = "test",
		.config_path = config_path,
		.context_size = sizeof(TestBackendContext),
		.config = test_backend_config,
		.prepare = test_backend_prepare,
		.init = test_backend_init,
		.run = test_backend_run,
		.teardown = test_backend_teardown,
		.map_pipeline_result = test_backend_map_result,
	};
	int failures = 0;
	int ret;

	reset_backend_test_state();
	g_expected_web_port = 4321;
	g_expected_verbose = 1;
	CHECK("backend success config file",
		create_temp_config(config_path, sizeof(config_path),
			"{\"system\":{\"webPort\":4321,\"verbose\":true}}\n") == 0);
	ret = backend_execute(&backend);

	CHECK("backend success return", ret == 0);
	CHECK("backend success config call", g_config_calls == 1);
	CHECK("backend success prepare", g_prepare_calls == 1);
	CHECK("backend success init", g_init_calls == 1);
	CHECK("backend success run", g_run_calls == 1);
	CHECK("backend success teardown", g_teardown_calls == 1);
	CHECK("backend success map", g_map_calls == 1);
	CHECK("backend success zeroed ctx", g_zeroed_context_ok);
	CHECK("backend success config loaded", g_config_loaded_ok);
	CHECK("backend success sequence", g_context_sequence_ok);
	CHECK("backend success mapped input", g_last_mapped_result == 0);
	if (config_path[0] != '\0')
		unlink(config_path);
	return failures;
}

static int test_backend_execute_prepare_failure(void)
{
	char config_path[64] = {0};
	BackendOps backend = {
		.name = "test",
		.config_path = config_path,
		.context_size = sizeof(TestBackendContext),
		.config = test_backend_config,
		.prepare = test_backend_prepare,
		.init = test_backend_init,
		.run = test_backend_run,
		.teardown = test_backend_teardown,
		.map_pipeline_result = test_backend_map_result,
	};
	int failures = 0;
	int ret;

	reset_backend_test_state();
	g_expected_web_port = 5555;
	CHECK("backend prepare fail config file",
		create_temp_config(config_path, sizeof(config_path),
			"{\"system\":{\"webPort\":5555}}\n") == 0);
	g_prepare_result = 7;
	ret = backend_execute(&backend);

	CHECK("backend prepare fail return", ret == 7);
	CHECK("backend prepare fail config call", g_config_calls == 1);
	CHECK("backend prepare fail prepare", g_prepare_calls == 1);
	CHECK("backend prepare fail init skipped", g_init_calls == 0);
	CHECK("backend prepare fail run skipped", g_run_calls == 0);
	CHECK("backend prepare fail teardown skipped", g_teardown_calls == 0);
	CHECK("backend prepare fail map skipped", g_map_calls == 0);
	CHECK("backend prepare fail config loaded", g_config_loaded_ok);
	if (config_path[0] != '\0')
		unlink(config_path);
	return failures;
}

static int test_backend_execute_pipeline_mapping(void)
{
	char config_path[64] = {0};
	BackendOps backend = {
		.name = "test",
		.config_path = config_path,
		.context_size = sizeof(TestBackendContext),
		.config = test_backend_config,
		.prepare = test_backend_prepare,
		.init = test_backend_init,
		.run = test_backend_run,
		.teardown = test_backend_teardown,
		.map_pipeline_result = test_backend_map_result,
	};
	int failures = 0;
	int ret;

	reset_backend_test_state();
	g_expected_web_port = 2468;
	CHECK("backend map config file",
		create_temp_config(config_path, sizeof(config_path),
			"{\"system\":{\"webPort\":2468}}\n") == 0);
	g_run_result = -9;
	ret = backend_execute(&backend);

	CHECK("backend map return", ret == 42);
	CHECK("backend map config call", g_config_calls == 1);
	CHECK("backend map prepare", g_prepare_calls == 1);
	CHECK("backend map init", g_init_calls == 1);
	CHECK("backend map run", g_run_calls == 1);
	CHECK("backend map teardown", g_teardown_calls == 1);
	CHECK("backend map called", g_map_calls == 1);
	CHECK("backend map input", g_last_mapped_result == -9);
	CHECK("backend map config loaded", g_config_loaded_ok);
	if (config_path[0] != '\0')
		unlink(config_path);
	return failures;
}

static int test_backend_execute_config_failure(void)
{
	char config_path[64] = {0};
	BackendOps backend = {
		.name = "test",
		.config_path = config_path,
		.context_size = sizeof(TestBackendContext),
		.config = test_backend_config,
		.prepare = test_backend_prepare,
		.init = test_backend_init,
		.run = test_backend_run,
		.teardown = test_backend_teardown,
		.map_pipeline_result = test_backend_map_result,
	};
	int failures = 0;
	int ret;

	reset_backend_test_state();
	CHECK("backend config fail file",
		create_temp_config(config_path, sizeof(config_path),
			"{ this is not json }\n") == 0);
	ret = backend_execute(&backend);

	CHECK("backend config fail return", ret == 1);
	CHECK("backend config fail config call", g_config_calls == 1);
	CHECK("backend config fail prepare skipped", g_prepare_calls == 0);
	CHECK("backend config fail init skipped", g_init_calls == 0);
	CHECK("backend config fail run skipped", g_run_calls == 0);
	CHECK("backend config fail teardown skipped", g_teardown_calls == 0);
	CHECK("backend config fail map skipped", g_map_calls == 0);
	if (config_path[0] != '\0')
		unlink(config_path);
	return failures;
}

static int test_backend_execute_init_failure(void)
{
	char config_path[64] = {0};
	BackendOps backend = {
		.name = "test",
		.config_path = config_path,
		.context_size = sizeof(TestBackendContext),
		.config = test_backend_config,
		.prepare = test_backend_prepare,
		.init = test_backend_init,
		.run = test_backend_run,
		.teardown = test_backend_teardown,
		.map_pipeline_result = test_backend_map_result,
	};
	int failures = 0;
	int ret;

	reset_backend_test_state();
	g_expected_web_port = 9876;
	g_init_result = -7;
	CHECK("backend init fail config file",
		create_temp_config(config_path, sizeof(config_path),
			"{\"system\":{\"webPort\":9876}}\n") == 0);
	ret = backend_execute(&backend);

	CHECK("backend init fail return", ret == 42);
	CHECK("backend init fail config call", g_config_calls == 1);
	CHECK("backend init fail prepare", g_prepare_calls == 1);
	CHECK("backend init fail init", g_init_calls == 1);
	CHECK("backend init fail run skipped", g_run_calls == 0);
	CHECK("backend init fail teardown", g_teardown_calls == 1);
	CHECK("backend init fail map called", g_map_calls == 1);
	CHECK("backend init fail mapped input", g_last_mapped_result == -7);
	CHECK("backend init fail config loaded", g_config_loaded_ok);
	if (config_path[0] != '\0')
		unlink(config_path);
	return failures;
}

static int test_backend_execute_invalid_ops(void)
{
	BackendOps backend = {0};
	int failures = 0;
	int ret;

	ret = backend_execute(NULL);
	CHECK("backend invalid null", ret == -1);

	ret = backend_execute(&backend);
	CHECK("backend invalid empty", ret == -1);

	backend.name = "test";
	backend.config_path = "/tmp/test_backend_missing";
	backend.context_size = sizeof(TestBackendContext);
	backend.prepare = test_backend_prepare;
	ret = backend_execute(&backend);
	CHECK("backend invalid missing config accessor", ret == -1);

	backend.config = test_backend_config;
	ret = backend_execute(&backend);
	CHECK("backend invalid missing init", ret == -1);

	backend.init = test_backend_init;
	ret = backend_execute(&backend);
	CHECK("backend invalid missing run", ret == -1);

	backend.run = test_backend_run;
	ret = backend_execute(&backend);
	CHECK("backend invalid missing teardown", ret == -1);
	return failures;
}

int test_backend(void)
{
	int failures = 0;

	failures += test_backend_execute_success();
	failures += test_backend_execute_prepare_failure();
	failures += test_backend_execute_pipeline_mapping();
	failures += test_backend_execute_config_failure();
	failures += test_backend_execute_init_failure();
	failures += test_backend_execute_invalid_ops();
	return failures;
}
