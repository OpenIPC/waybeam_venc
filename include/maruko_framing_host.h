#ifndef MARUKO_FRAMING_HOST_H
#define MARUKO_FRAMING_HOST_H

/* Host callbacks a Maruko framing module needs from the pipeline — thin
 * non-static wrappers over otherwise-file-static helpers in
 * src/maruko_pipeline.c (parallel of include/star6e_framing_host.h).  Kept
 * behind wrappers so the module never reaches into pipeline internals. */

#include "maruko_pipeline.h"

/* Re-emit the AE meter crop for the current framing window (pct = crop
 * fraction, x/y = normalized pan centre) — wraps maruko_apply_ae_crop so the
 * exposure meter tracks the stabilized crop as it pans, mirroring the zoom
 * path.  No-op if AE-crop is disabled/unresolved (handled inside). */
void maruko_framing_apply_ae_crop(MarukoBackendContext *ctx,
	double pct, double x, double y);

/* Stop the 30 Hz pan-ramp thread so the stab detector is the sole writer of
 * the SCL port-0 crop while it is active (R2: single occupied crop stage). */
void maruko_framing_pan_ramp_stop(void);

/* 1 when the pipeline graph was configured for stab-fill this run (VENC in
 * NORMAL_FRMBASE + unbound, SCL port0 RAW + unbound).  The fill module checks
 * this in setup_ports — pushing into a RING-configured VENC would starve. */
int maruko_framing_fill_graph_ready(void);

#endif /* MARUKO_FRAMING_HOST_H */
