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
 *   low_water >= ENGAGE_SLOTS   -> permille = max(MIN, permille * 4/5)
 *   full_drops increased        -> permille = max(MIN, permille * 3/5),
 *                                  immediately, but at most ONCE per window
 *   low_water <= RECOVER_SLOTS  -> permille = min(1000, permille + 50)
 *
 * MIN here is the integrator's control-range bound
 * (VENC_SHM_THROTTLE_MIN_PERMILLE).  The floor that actually limits the
 * emitted bitrate is the DUAL floor applied in venc_shm_throttle_scale(),
 * which is the only place the real kbps is in scope -- see
 * VENC_SHM_THROTTLE_FLOOR_PERMILLE / _FLOOR_ABS_KBPS below.
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
 * slower than the retreat: 1000 -> bottom takes 14 windows (~2.8 s) on the
 * x4/5 engage path or 6 (~1.2 s) on the x3/5 drop path, and bottom -> 1000
 * takes 19 windows (~3.8 s).  (Counts are asserted in
 * tests/test_venc_shm_throttle.c so they cannot silently drift if the
 * multipliers or the bound are retuned.)  The dual floor below means a wedged
 * consumer cannot drive the encoder to nothing, while still permitting a
 * rate MCS0 can actually carry.
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
/* Clamp floor — DUAL, and both halves are load-bearing.
 *
 * The percentage alone was wrong in shape: it ties how hard the clamp may
 * squeeze to the bitrate the OPERATOR configured, which has nothing to do
 * with what the radio can carry.  At the measured 21839 kbps the old 250
 * permille floor bottomed out at 5459 kbps.  If the link has dropped to MCS0
 * but waybeam-link's §9.6 bitrate demotion (1.5-8 s outer loop) has not
 * actuated yet, this clamp is the ONLY thing defending the ring, and 5.5 Mbps
 * is several times what MCS0 can deliver — so the ring stays full and every
 * referenced frame is a chain-breaking drop for the whole window.
 *
 * Hence a second, ABSOLUTE cap on the floor.  Derivation: MCS0/HT20 is
 * 6.5 Mbps PHY, roughly 4-5 Mbps goodput after 802.11 overhead, and a craft
 * running 300/200 permille FEC leaves ~3.2-4 Mbps for video.  2500 kbps of
 * video (~3.1 Mbps of airtime at that FEC) is comfortably deliverable there
 * and sits clear of VENC_BITRATE_MIN_KBPS (1000), which is the real encoder
 * collapse rail and still backstops everything downstream.
 *
 * Note the two coincide at 25000 kbps, today's configured ceiling on the
 * reference craft; the absolute term only starts binding above that.  It is
 * here so the floor keeps meaning "never clamp above what the radio can
 * plausibly carry" when a higher-bitrate mode is configured, instead of
 * silently mis-scaling again. */
#define VENC_SHM_THROTTLE_FLOOR_PERMILLE 100u
#define VENC_SHM_THROTTLE_FLOOR_ABS_KBPS 2500u
/* Control-range bound for the AIMD state only — NOT the policy floor.  The
 * policy floor is applied in venc_shm_throttle_scale(), which is the only
 * place the actual kbps is known, and the absolute cap can require an
 * effective ratio below FLOOR_PERMILLE (2500/40000 = 62 permille at 40 Mbps).
 * This exists purely so the integrator cannot run away toward zero and take
 * an unbounded number of AIMD windows to climb back. */
#define VENC_SHM_THROTTLE_MIN_PERMILLE    50u
#define VENC_SHM_THROTTLE_FULL_PERMILLE 1000u

/* Minimum clamp movement worth re-programming the encoder for.
 *
 * On SigmaStar the only bitrate actuator is MI_VENC_SetChnAttr, and it emits
 * an IDR whenever the programmed rate actually changes (device-measured on a
 * SSC338Q, 2026-08-23: ten writes of *distinct* values produced ten extra
 * IRAP access units, ten writes of the *same* value produced none).  So every
 * clamp step this loop applies costs one keyframe — the largest frame the
 * encoder makes — pushed into a ring that is already backing up.  Applying
 * each +50 additive-increase step individually turns a recovery ramp into a
 * keyframe train, and a fill signal that oscillates never settles.
 *
 * 100 permille (10%) is coarse enough to suppress that chatter and still far
 * finer than the control law's own steps (x4/5 decrease, x3/5 on a drop).
 * Full release and the floor are exempt: leaving the encoder clamped because
 * the last 10% did not clear a deadband would be worse than the keyframe. */
#define VENC_SHM_THROTTLE_APPLY_DEADBAND_PERMILLE 100u

/* Should `want` be programmed, given what was last successfully applied?
 * Pure so tests can drive it without a ring.
 *
 * THE DEADBAND MUST NEVER COST REACH.  Two properties guarantee it:
 *
 *   1. Both rails are exempt.  `want` at or below the floor, or at full
 *      release, always applies — the two states where being off by up to
 *      10% actually matters (a ring still backing up at the floor, or an
 *      encoder left clamped once the consumer recovered).
 *   2. The comparison is against the last *applied* value, not the last
 *      *wanted* one, so suppressed movement accumulates.  A control law
 *      walking down in sub-deadband steps therefore still arrives: each
 *      blocked step widens the gap to `applied` until it crosses, and once
 *      the law is within the floor band property 1 takes over.  It is never
 *      possible for the clamp to stall above the floor while the law keeps
 *      asking for less. tests/test_venc_shm_throttle.c drives the full
 *      descent and asserts the floor is reached.
 *
 * What the deadband does cost is precision at rest: if the law settles at a
 * value within the deadband of `applied`, the encoder stays at `applied`.
 * That is a bounded <=10% conservative error on a clamp whose own steps are
 * x4/5 and x3/5 — and it is the entire point, since on SigmaStar each
 * distinct programmed rate costs an IDR. */
static inline int venc_shm_throttle_should_apply(uint16_t want, uint16_t applied)
{
	unsigned int delta;

	if (want == applied)
		return 0;
	/* Rails first: never deadband the floor or a full release. */
	if (want >= VENC_SHM_THROTTLE_FULL_PERMILLE ||
	    want <= VENC_SHM_THROTTLE_FLOOR_PERMILLE)
		return 1;
	delta = (want > applied) ? (unsigned int)(want - applied)
				 : (unsigned int)(applied - want);
	return delta >= VENC_SHM_THROTTLE_APPLY_DEADBAND_PERMILLE;
}
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
