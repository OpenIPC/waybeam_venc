#include "venc_shm_throttle.h"

#include <string.h>

static uint16_t clamp_permille(uint32_t v)
{
	if (v < VENC_SHM_THROTTLE_FLOOR_PERMILLE)
		return (uint16_t)VENC_SHM_THROTTLE_FLOOR_PERMILLE;
	if (v > VENC_SHM_THROTTLE_FULL_PERMILLE)
		return (uint16_t)VENC_SHM_THROTTLE_FULL_PERMILLE;
	return (uint16_t)v;
}

/* Record a floor entry/exit edge for the caller's log-once reporting. */
static void note_floor_edge(VencShmThrottle *t)
{
	uint8_t now_at_floor =
		(t->permille <= VENC_SHM_THROTTLE_FLOOR_PERMILLE) ? 1u : 0u;

	if (now_at_floor == t->at_floor)
		return;
	t->at_floor = now_at_floor;
	t->floor_edge_pending = now_at_floor ? 1u : 2u;
}

static void set_permille(VencShmThrottle *t, uint32_t value)
{
	uint16_t next = clamp_permille(value);

	if (next == t->permille)
		return;
	t->permille = next;
	t->pending_change = 1;
	note_floor_edge(t);
}

void venc_shm_throttle_reset(VencShmThrottle *t, uint64_t now_us)
{
	if (!t)
		return;
	memset(t, 0, sizeof(*t));
	t->permille = (uint16_t)VENC_SHM_THROTTLE_FULL_PERMILLE;
	t->window_low_slots = VENC_SHM_THROTTLE_NO_SAMPLE;
	t->window_start_us = now_us;
	t->enabled = 1;
}

void venc_shm_throttle_set_enabled(VencShmThrottle *t, int enabled,
	uint64_t now_us)
{
	uint8_t want = enabled ? 1u : 0u;

	if (!t || t->enabled == want)
		return;

	t->enabled = want;
	t->window_low_slots = VENC_SHM_THROTTLE_NO_SAMPLE;
	t->window_start_us = now_us;
	t->seeded = 0;
	t->drop_charged = 0;

	/* Both directions land on 1000: disabling must actively release the
	 * clamp (otherwise the encoder stays pinned wherever the loop last
	 * left it), and enabling starts from unclamped so the first decision
	 * is made on fresh evidence. */
	set_permille(t, VENC_SHM_THROTTLE_FULL_PERMILLE);
}

int venc_shm_throttle_is_enabled(const VencShmThrottle *t)
{
	return (t && t->enabled) ? 1 : 0;
}

void venc_shm_throttle_observe(VencShmThrottle *t, uint32_t used_slots,
	uint64_t full_drops)
{
	if (!t || !t->enabled)
		return;

	if (used_slots < t->window_low_slots)
		t->window_low_slots = used_slots;

	/* First observation only seeds the baseline — a producer that has
	 * been dropping since before the throttle was enabled must not be
	 * charged for that history in one step. */
	if (!t->seeded) {
		t->last_full_drops = full_drops;
		t->seeded = 1;
		return;
	}

	if (full_drops > t->last_full_drops) {
		t->last_full_drops = full_drops;
		/* A drop already happened: react now, do not wait for the
		 * window to close.  Harder than the occupancy decrease
		 * because occupancy is a prediction and this is proof.
		 *
		 * Once per window, though: a full ring increments full_drops
		 * on every frame, and an uncapped charge would be 0.6^20 in
		 * one window -- straight to the floor with no chance to
		 * settle at an intermediate rate. */
		if (!t->drop_charged) {
			t->drop_charged = 1;
			set_permille(t, (uint32_t)t->permille * 3u / 5u);
		}
	} else if (full_drops < t->last_full_drops) {
		/* Counter went backwards — the ring was re-created under us.
		 * Re-seed rather than treating the wrap as a huge drop burst. */
		t->last_full_drops = full_drops;
	}
}

int venc_shm_throttle_tick(VencShmThrottle *t, uint64_t now_us)
{
	int changed;

	if (!t)
		return 0;

	if (now_us - t->window_start_us >= VENC_SHM_THROTTLE_WINDOW_US) {
		/* NO_SAMPLE means no frame was written in this window at all
		 * (encoder idle, output disabled).  No evidence either way,
		 * so hold -- decreasing on a silent window would clamp an
		 * idle encoder for no reason. */
		if (t->enabled &&
		    t->window_low_slots != VENC_SHM_THROTTLE_NO_SAMPLE) {
			if (t->window_low_slots >=
			    VENC_SHM_THROTTLE_ENGAGE_SLOTS)
				set_permille(t,
					(uint32_t)t->permille * 4u / 5u);
			else if (t->window_low_slots <=
			         VENC_SHM_THROTTLE_RECOVER_SLOTS)
				set_permille(t, (uint32_t)t->permille +
					VENC_SHM_THROTTLE_AI_STEP);
		}
		t->window_low_slots = VENC_SHM_THROTTLE_NO_SAMPLE;
		t->drop_charged = 0;
		t->window_start_us = now_us;
	}

	changed = t->pending_change ? 1 : 0;
	t->pending_change = 0;
	return changed;
}

uint16_t venc_shm_throttle_permille(const VencShmThrottle *t)
{
	return t ? t->permille : (uint16_t)VENC_SHM_THROTTLE_FULL_PERMILLE;
}

uint32_t venc_shm_throttle_scale(uint16_t permille, uint32_t kbps)
{
	if (permille >= VENC_SHM_THROTTLE_FULL_PERMILLE)
		return kbps;
	if (permille < VENC_SHM_THROTTLE_FLOOR_PERMILLE)
		permille = (uint16_t)VENC_SHM_THROTTLE_FLOOR_PERMILLE;
	return (uint32_t)(((uint64_t)kbps * permille) /
		VENC_SHM_THROTTLE_FULL_PERMILLE);
}

uint32_t venc_shm_throttle_apply(const VencShmThrottle *t, uint32_t kbps)
{
	return venc_shm_throttle_scale(venc_shm_throttle_permille(t), kbps);
}

int venc_shm_throttle_floor_edge(VencShmThrottle *t)
{
	uint8_t edge;

	if (!t || !t->floor_edge_pending)
		return 0;
	edge = t->floor_edge_pending;
	t->floor_edge_pending = 0;
	return (edge == 1u) ? 1 : -1;
}
