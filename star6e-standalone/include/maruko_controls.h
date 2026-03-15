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

#endif /* MARUKO_CONTROLS_H */
