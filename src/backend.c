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

int backend_execute(const BackendOps *backend, const VencConfig *cfg)
{
	void *ctx;
	VencConfig *ctx_cfg;
	int ret;

	if (!backend || !cfg || !backend->config ||
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

	ctx_cfg = backend->config(ctx);
	if (!ctx_cfg) {
		free(ctx);
		return -1;
	}

	/* VencConfig is a flat POD with no owning pointers, so the backend gets
	 * its own mutable copy of main()'s parse by plain assignment. */
	*ctx_cfg = *cfg;

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
