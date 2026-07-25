#ifndef STAR6E_VPE_PORTS_H
#define STAR6E_VPE_PORTS_H

#include <stdbool.h>

/*
 * Arbiter + observability for the VPE0 scaler output ports (Star6E).
 *
 *   port0 — the main SCL output.  1:N-shareable: the H.265 encoder always
 *           consumes it, and the JPEG snapshot + dual/record channels bind
 *           alongside on demand (a SYS 1:N bind on the SAME buffer, not a
 *           second scaler).  Tracked for observability only — no contention.
 *   port1 — the SINGLE second scaler output.  Exactly one owner at a time:
 *           the "stab" framing tap OR the NPU detector.  This module is the
 *           sole arbiter; a claim while port1 is held by someone else is
 *           refused, so the two features cannot both program the tap.
 *
 * State is process-wide (one pipeline per process) and republished to the
 * /api/v1/config `runtime.vpe_taps` block on every change.  All mutators run
 * on the pipeline thread (start/stop/reload); the HTTP thread only reads the
 * published copy via venc_api.
 */

/* Pipeline lifecycle.  begin(): port0 gains the implicit "main" consumer and
 * port1 starts free.  end(): clear all state and hide the vpe_taps block. */
void star6e_vpe_ports_begin(void);
void star6e_vpe_ports_end(void);

/* Claim/release the singular port1.  `owner` is a static label ("stab" |
 * "detect").  claim returns 0 on success (or if `owner` already holds it),
 * -1 when a different owner holds it.  release is a no-op unless `owner`
 * currently holds port1. */
int  star6e_vpe_port1_claim(const char *owner);
void star6e_vpe_port1_release(const char *owner);
const char *star6e_vpe_port1_owner(void);   /* NULL when free */

/* Mark/clear a port0 1:N consumer for observability ("jpeg" | "record").
 * Unknown names are ignored. */
void star6e_vpe_port0_set(const char *consumer, bool present);

#endif /* STAR6E_VPE_PORTS_H */
