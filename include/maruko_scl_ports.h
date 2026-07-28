#ifndef MARUKO_SCL_PORTS_H
#define MARUKO_SCL_PORTS_H

#include <stddef.h>

/*
 * Arbiter for the shareable i6c SCL output taps (Maruko).
 *
 * The SCL has four outputs and every one has an owner:
 *   port0 — main encode output, hardware-bound to VENC (and 1:N to the MJPEG
 *           snapshot channel).  Never user-drained: registering a user output
 *           depth on a bound port faults the MI_SYS allocator.
 *   port1 — dual/second stream, when configured.
 *   port2 — stab framing tap.
 *   port3 — NPU detector tap, and the grayscale snapshot tap.
 *
 * Only port3 has more than one possible claimant, so this module arbitrates
 * that one: the detector holds it for a whole run, while a grayscale snapshot
 * wants it transiently.  A claim while another owner holds it is refused, so
 * the snapshot can never reprogram the tap underneath a running detector.
 *
 * Mirrors star6e_vpe_ports for the i6e side.  Mutators are mutex-guarded, so
 * unlike that module these may be called from any thread (the snapshot claim
 * runs on the HTTP thread).
 */

/* Claim/release the shared tap.  `owner` is a static label ("detect" |
 * "snapshot").  claim returns 0 on success (or if `owner` already holds it),
 * -1 when a different owner holds it.  release is a no-op unless `owner`
 * currently holds the tap. */
int  maruko_scl_tap_claim(const char *owner);
void maruko_scl_tap_release(const char *owner);

/* Copy the current owner into `buf` ("" when free). */
void maruko_scl_tap_owner_copy(char *buf, size_t len);

/* Drop any claim — pipeline teardown, so a restart starts clean. */
void maruko_scl_tap_reset(void);

#endif /* MARUKO_SCL_PORTS_H */
