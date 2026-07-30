/*
 * Ring-fill bitrate clamp for the frame-shm:// egress transport.
 *
 * Problem this solves.  When the frame ring fills, the producer discards a
 * whole *already-encoded* frame (venc_frame_ring_begin_write returns -1).
 * The drop lands after encode, so the H.265 reference chain breaks and the
 * receiver decodes garbage until the next IDR or the end of the GDR cycle.
 * The v0.9.2 post-encode frame skip was reverted for exactly this reason —
 * dropping frames is not a backpressure strategy, it is the damage.
 *
 * The ring is 8 slots x 384 KB.  At 100 fps that is 80 ms of queue, so by
 * the time it is *full* the stream has already blown its latency budget.
 * Occupancy, not the drop counter, is the control variable.
 *
 * Authority split.  This clamp owns instantaneous safety on a ~200 ms
 * timescale; an external rate controller (waybeam-link) keeps sole
 * ownership of steady-state rate on a 1.5-8 s timescale.  The clamp is a
 * multiplier applied to whatever bitrate was configured — it NEVER writes
 * video0.bitrate.  Consequences that fall out of that: an external
 * controller's write-on-change cache stays coherent (every write still
 * succeeds and lands in config), the WebUI slider and curl stay truthful,
 * and any number of controllers compose with it, because a clamp is
 * idempotent.  Returning an error from the setter instead would have made
 * a write-on-change controller re-push the same value forever.
 *
 * Cascade stability: the 200 ms window must stay >=5x faster than the
 * slowest external actuator loop (currently ~1500 ms settle, so ~10x).
 * That inner/outer separation is what keeps the two controllers from
 * coupling into oscillation.  Preserve it if you retune either side.
 *
 * Control law (AIMD, evaluated on each window's LOW-water mark):
 *   low_water >= ENGAGE_SLOTS   -> permille = max(FLOOR, permille * 4/5)
 *   full_drops increased        -> permille = max(FLOOR, permille * 3/5),
 *                                  immediately, but at most ONCE per window
 *   low_water <= RECOVER_SLOTS  -> permille = min(1000, permille + 50)
 *
 * Low-water, not high-water, and that distinction is the whole difference
 * between this working and not.  Measured on a Star6E at 100 fps into an
 * 8-slot ring with a perfectly healthy consumer, the ring routinely spikes
 * to 2-3 slots inside a 200 ms window and drains again -- the consumer
 * reads one frame per event-loop iteration, so short bursts are normal.
 * High-water saw those bursts as congestion and clamped the encoder
 * 15-25 % essentially at random, oscillating 740-1000 with nothing wrong.
 * Low-water asks the question that actually matters: did the ring fail to
 * drain *at any point* in the whole window?  If it touched bottom the
 * consumer is keeping up and the spike was transient; if it never got
 * below ENGAGE_SLOTS across 20 frame times, that is real standing backlog.
 *
 * The drop charge is capped at once per window for the same reason it has
 * to exist at all: when the ring is full, full_drops increments on EVERY
 * frame, so an uncapped per-frame x3/5 is 1000 * 0.6^20 within one window
 * -- an instant slam to the floor on the first congested window, with no
 * chance for the multiplicative decrease to find an intermediate rate.
 *
 * Multiplicative-decrease / additive-increase, so recovery is deliberately
 * slower than the retreat: 1000 -> floor takes 7 windows (~1.4 s), floor ->
 * 1000 takes 15 windows (~3.0 s).  The 250 floor means a wedged consumer
 * cannot drive the encoder to nothing.
 *
 * Policy is compile-time, following include/idr_rate_limit.h.  The single
 * runtime knob is outgoing.shm_throttle (bool).  Exposing high/low water
 * marks as config was tried in v0.9.2 and removed as "more noise than
 * useful"; that judgement has not changed.
 *
 * No SDK dependency, no syscalls, no clock of its own — the caller passes
 * now_us.  Pure and host-unit-testable; see tests/test_venc_shm_throttle.c.
 *
 * Threading: one instance per output, owned by the pipeline thread that
 * writes the ring.  Not internally synchronised and does not need to be.
 */

#ifndef VENC_SHM_THROTTLE_H
#define VENC_SHM_THROTTLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VENC_SHM_THROTTLE_WINDOW_US   200000u  /* control period */
#define VENC_SHM_THROTTLE_ENGAGE_SLOTS     2u  /* >= this -> decrease */
#define VENC_SHM_THROTTLE_RECOVER_SLOTS    1u  /* <= this -> increase */
#define VENC_SHM_THROTTLE_FLOOR_PERMILLE 250u
#define VENC_SHM_THROTTLE_FULL_PERMILLE 1000u
#define VENC_SHM_THROTTLE_AI_STEP         50u  /* additive increase */

#define VENC_SHM_THROTTLE_NO_SAMPLE  0xFFFFFFFFu

typedef struct {
	uint16_t permille;          /* current clamp, FLOOR..1000 */
	uint32_t window_low_slots;  /* low-water used_slots this window,
	                             * NO_SAMPLE until the first observation */
	uint64_t window_start_us;
	uint64_t last_full_drops;
	uint8_t  seeded;            /* last_full_drops is meaningful */
	uint8_t  enabled;
	uint8_t  pending_change;     /* consumed by _tick */
	uint8_t  drop_charged;       /* drop MD already applied this window */
	uint8_t  at_floor;           /* edge state for _floor_edge */
	uint8_t  floor_edge_pending; /* 1 = entered, 2 = left */
} VencShmThrottle;

/* Zero state, permille = 1000, enabled.  Safe to call at any time; a
 * caller that re-creates its ring should call this so a stale window
 * boundary or drop count cannot leak across the restart. */
void venc_shm_throttle_reset(VencShmThrottle *t, uint64_t now_us);

/* Enable/disable.  Idempotent — safe to call every frame from config.
 * Disabling restores permille to 1000 and raises a pending change, so the
 * caller re-applies the unclamped bitrate rather than leaving the encoder
 * pinned at whatever the clamp last programmed.  Re-enabling starts from
 * 1000 with a fresh window. */
void venc_shm_throttle_set_enabled(VencShmThrottle *t, int enabled,
	uint64_t now_us);

int venc_shm_throttle_is_enabled(const VencShmThrottle *t);

/* Called once per encoded frame — no syscalls, a few loads and compares.
 * `used_slots` is the ring occupancy observed for this frame and
 * `full_drops` the producer's lifetime full-ring drop count. */
void venc_shm_throttle_observe(VencShmThrottle *t, uint32_t used_slots,
	uint64_t full_drops);

/* Returns 1 when the clamp factor changed and the caller must re-apply the
 * bitrate, 0 otherwise.  Call once per frame after _observe. */
int venc_shm_throttle_tick(VencShmThrottle *t, uint64_t now_us);

uint16_t venc_shm_throttle_permille(const VencShmThrottle *t);

/* Scale a bitrate by a clamp factor.  Takes the permille rather than the
 * struct because the apply path lives in the backend control layer, which
 * is reached from the HTTP thread and holds only the published factor —
 * the throttle instance itself belongs to the pipeline thread. */
uint32_t venc_shm_throttle_scale(uint16_t permille, uint32_t kbps);

/* Convenience wrapper for callers that do hold the instance. */
uint32_t venc_shm_throttle_apply(const VencShmThrottle *t, uint32_t kbps);

/* Floor-pinned transition, consumed by the caller's logging.  Returns 1
 * exactly once when the clamp reaches the floor, -1 exactly once when it
 * leaves, 0 otherwise.  Silent floor-pinning reads as "working" when it is
 * not: at the floor the clamp has spent all its authority and the ring is
 * still backing up, which usually means the consumer is not draining
 * rather than that the encoder is too fast. */
int venc_shm_throttle_floor_edge(VencShmThrottle *t);

#ifdef __cplusplus
}
#endif

#endif /* VENC_SHM_THROTTLE_H */
