#ifndef MARUKO_CONTROLS_H
#define MARUKO_CONTROLS_H

#include "maruko_bindings.h"
#include "maruko_output.h"
#include "maruko_pipeline.h"
#include "venc_api.h"

/** Bind pipeline context to runtime control state. */
void maruko_controls_bind(MarukoBackendContext *backend, VencConfig *vcfg);

/** Return Maruko backend's live control callback table. */
const VencApplyCallbacks *maruko_controls_callbacks(void);

/** Returns the VencConfig pointer registered via maruko_controls_bind, or
 * NULL if bind hasn't run yet.  Used by per-frame producer code (e.g. SHM
 * backpressure) that needs to read live config without threading a pointer
 * through every helper. */
const VencConfig *maruko_controls_vcfg(void);

#endif /* MARUKO_CONTROLS_H */
