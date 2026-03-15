#include "backend.h"

#include <stdio.h>
#include <stdlib.h>

static int backend_run_pipeline(const BackendOps *backend, void *ctx)
{
	int ret;

	ret = backend->init(ctx);
	if (ret != 0) {
		backend->teardown(ctx);
		return ret;
	}

	ret = backend->run(ctx);
	backend->teardown(ctx);
	return ret;
}

int backend_execute(const BackendOps *backend)
{
	void *ctx;
	VencConfig *cfg;
	int ret;

	if (!backend || !backend->config_path || !backend->config ||
	    !backend->prepare || !backend->init || !backend->run ||
	    !backend->teardown ||
	    backend->context_size == 0) {
		return -1;
	}

	ctx = calloc(1, backend->context_size);
	if (!ctx) {
		fprintf(stderr, "ERROR: unable to allocate backend context for %s\n",
			backend->name ? backend->name : "unknown");
		return 1;
	}

	cfg = backend->config(ctx);
	if (!cfg) {
		free(ctx);
		return -1;
	}

	venc_config_defaults(cfg);
	if (venc_config_load(backend->config_path, cfg) != 0) {
		free(ctx);
		return 1;
	}

	ret = backend->prepare(ctx);
	if (ret == 0) {
		ret = backend_run_pipeline(backend, ctx);
		if (backend->map_pipeline_result) {
			ret = backend->map_pipeline_result(ret);
		}
	}

	free(ctx);
	return ret;
}
