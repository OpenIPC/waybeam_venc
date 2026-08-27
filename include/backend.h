#ifndef BACKEND_H
#define BACKEND_H

#ifndef HAVE_BACKEND_STAR6E
#define HAVE_BACKEND_STAR6E 0
#endif

#ifndef HAVE_BACKEND_MARUKO
#define HAVE_BACKEND_MARUKO 0
#endif

#ifndef HAVE_BACKEND_CV610
#define HAVE_BACKEND_CV610 0
#endif

#include <stddef.h>

#include "venc_config.h"

typedef int (*BackendPrepareFn)(void *ctx);
typedef int (*BackendInitFn)(void *ctx);
typedef int (*BackendRunFn)(void *ctx);
typedef void (*BackendTeardownFn)(void *ctx);
typedef int (*BackendMapResultFn)(int result);
typedef VencConfig *(*BackendConfigFn)(void *ctx);

typedef struct {
	const char *name;
	size_t context_size;
	BackendConfigFn config;
	BackendPrepareFn prepare;
	BackendInitFn init;
	BackendRunFn run;
	BackendTeardownFn teardown;
	BackendMapResultFn map_pipeline_result;
} BackendOps;

/** Run the full backend lifecycle (prepare → init → run → teardown).
 *
 *  `cfg` is the config main() already parsed; it is copied into the backend
 *  context, which owns the mutable copy the live-apply path writes to.  The
 *  backend does not read the config file — see the note in src/main.c on why
 *  the file is opened exactly once. */
int backend_execute(const BackendOps *backend, const VencConfig *cfg);

/** Return the backend selected during the last backend_execute call. */
const BackendOps *backend_get_selected(void);

#endif /* BACKEND_H */
