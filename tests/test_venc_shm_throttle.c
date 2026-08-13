#include <stdint.h>

#include "test_helpers.h"
#include "venc_shm_throttle.h"

/* Unit tests for the frame-shm ring-fill bitrate clamp.
 *
 * The module takes now_us as a parameter and owns no clock, so every case
 * here drives virtual time — no nanosleep, no wall-clock flake, and the
 * multi-second recovery ramp costs nothing to test. */

#define WIN VENC_SHM_THROTTLE_WINDOW_US

/* Advance one full window at the given occupancy, returning whether the
 * clamp factor changed.  Constant occupancy across the window, so low-water
 * == high-water == used_slots; cases that need them to differ build the
 * window by hand. */
static int step_window(VencShmThrottle *t, uint64_t *now, uint32_t used_slots,
	uint64_t full_drops)
{
	int changed = 0;
	int i;

	/* Several frames per window, as production does at 100 fps. */
	for (i = 0; i < 20; ++i) {
		*now += WIN / 20;
		venc_shm_throttle_observe(t, used_slots, full_drops);
		if (venc_shm_throttle_tick(t, *now))
			changed = 1;
	}
	return changed;
}

int test_venc_shm_throttle(void)
{
	int failures = 0;
	VencShmThrottle t;
	uint64_t now = 1000000;

	/* ── 1. Engage ──────────────────────────────────────────────── */
	venc_shm_throttle_reset(&t, now);
	CHECK("thr_reset_unclamped",
		venc_shm_throttle_permille(&t) == 1000);
	CHECK("thr_reset_enabled", venc_shm_throttle_is_enabled(&t) == 1);
	CHECK("thr_reset_no_floor_edge", venc_shm_throttle_floor_edge(&t) == 0);

	CHECK("thr_engage_reports_change", step_window(&t, &now, 3, 0) == 1);
	CHECK("thr_engage_800", venc_shm_throttle_permille(&t) == 800);

	/* Occupancy at the engage threshold exactly still decreases. */
	(void)step_window(&t, &now, VENC_SHM_THROTTLE_ENGAGE_SLOTS, 0);
	CHECK("thr_engage_at_threshold", venc_shm_throttle_permille(&t) == 640);

	/* ── 2. Drop fast-path: applies mid-window, no window wait ──── */
	venc_shm_throttle_reset(&t, now);
	venc_shm_throttle_observe(&t, 0, 7);   /* seeds the baseline only */
	CHECK("thr_seed_no_change", venc_shm_throttle_permille(&t) == 1000);
	now += WIN / 10;                        /* well inside the window */
	venc_shm_throttle_observe(&t, 0, 8);   /* one new drop */
	CHECK("thr_drop_immediate_600", venc_shm_throttle_permille(&t) == 600);
	CHECK("thr_drop_reports_change",
		venc_shm_throttle_tick(&t, now) == 1);
	CHECK("thr_drop_change_consumed",
		venc_shm_throttle_tick(&t, now) == 0);

	/* Capped at one charge per window.  A full ring increments full_drops
	 * on every frame, so an uncapped charge is 0.6^20 inside one window --
	 * an instant slam to the floor.  This is the assertion that pins it. */
	venc_shm_throttle_observe(&t, 0, 9);
	venc_shm_throttle_observe(&t, 0, 10);
	venc_shm_throttle_observe(&t, 0, 11);
	CHECK("thr_drop_charged_once_per_window",
		venc_shm_throttle_permille(&t) == 600);

	/* ...but the next window charges again.  Closing that window first
	 * applies the additive increase (low-water was 0 throughout, the ring
	 * was draining fine), 600 -> 650, and the fresh drop then takes
	 * 650 * 3/5 = 390.  Both steps are visible on purpose: recovery and
	 * the drop charge compose rather than one masking the other. */
	now += WIN;
	(void)venc_shm_throttle_tick(&t, now);
	CHECK("thr_window_close_applied_ai",
		venc_shm_throttle_permille(&t) == 650);
	venc_shm_throttle_observe(&t, 0, 12);
	CHECK("thr_drop_charges_next_window",
		venc_shm_throttle_permille(&t) == 390);

	/* A ring re-created under us resets the counter; that must not read
	 * as a huge drop burst. */
	now += WIN;
	(void)venc_shm_throttle_tick(&t, now);
	venc_shm_throttle_observe(&t, 0, 0);
	CHECK("thr_counter_rewind_ignored",
		venc_shm_throttle_permille(&t) >= 390);

	/* ── 3. Floor: sustained congestion converges and stays ─────── */
	venc_shm_throttle_reset(&t, now);
	{
		int i;
		for (i = 0; i < 40; ++i)
			(void)step_window(&t, &now, 8, 0);
	}
	CHECK("thr_floor_exact",
		venc_shm_throttle_permille(&t) == VENC_SHM_THROTTLE_MIN_PERMILLE);
	CHECK("thr_floor_no_further_change",
		step_window(&t, &now, 8, 0) == 0);
	/* A drop at the floor cannot push below it either. */
	venc_shm_throttle_observe(&t, 8, 999999);
	venc_shm_throttle_observe(&t, 8, 1000000);
	CHECK("thr_floor_holds_under_drops",
		venc_shm_throttle_permille(&t) == VENC_SHM_THROTTLE_MIN_PERMILLE);

	/* Floor edge fires exactly once on entry, once on exit. */
	venc_shm_throttle_reset(&t, now);
	{
		int entries = 0, exits = 0, i, e;
		for (i = 0; i < 40; ++i) {
			(void)step_window(&t, &now, 8, 0);
			e = venc_shm_throttle_floor_edge(&t);
			if (e > 0) entries++;
			if (e < 0) exits++;
		}
		CHECK("thr_floor_edge_entered_once", entries == 1);
		CHECK("thr_floor_edge_no_exit_yet", exits == 0);
		for (i = 0; i < 40; ++i) {
			(void)step_window(&t, &now, 0, 0);
			e = venc_shm_throttle_floor_edge(&t);
			if (e > 0) entries++;
			if (e < 0) exits++;
		}
		CHECK("thr_floor_edge_left_once", exits == 1);
		CHECK("thr_floor_edge_no_reentry", entries == 1);
	}

	/* ── 4. Recovery ramp: additive, monotonic, no overshoot ────── */
	venc_shm_throttle_reset(&t, now);
	{
		int i;
		uint16_t prev;
		int windows = 0;
		int monotonic = 1;

		for (i = 0; i < 40; ++i)
			(void)step_window(&t, &now, 8, 0);
		CHECK("thr_ramp_starts_at_floor",
			venc_shm_throttle_permille(&t) ==
			VENC_SHM_THROTTLE_MIN_PERMILLE);

		prev = venc_shm_throttle_permille(&t);
		while (venc_shm_throttle_permille(&t) < 1000 && windows < 100) {
			(void)step_window(&t, &now, 0, 0);
			if (venc_shm_throttle_permille(&t) < prev)
				monotonic = 0;
			prev = venc_shm_throttle_permille(&t);
			windows++;
		}
		CHECK("thr_ramp_monotonic", monotonic == 1);
		/* (1000-50)/50 = 19 windows = 3.8 s at a 200 ms window.  The
		 * deeper integrator bound costs 4 extra windows of recovery
		 * versus the old 250 permille floor; that is the price of
		 * being able to reach MCS0-carryable rates at all. */
		CHECK("thr_ramp_19_windows", windows == 19);
		/* Descent is documented in the header; assert it so the two
		 * cannot drift. x4/5 from 1000 reaches the integrator bound in
		 * 14 windows. */
		{
			VencShmThrottle d;
			uint64_t dnow = 0;
			int dw = 0;

			venc_shm_throttle_reset(&d, dnow);
			while (venc_shm_throttle_permille(&d) >
			       VENC_SHM_THROTTLE_MIN_PERMILLE && dw < 100) {
				(void)step_window(&d, &dnow, 8, 0);
				dw++;
			}
			CHECK("thr_descent_14_windows", dw == 14);
			CHECK("thr_descent_reaches_bound",
				venc_shm_throttle_permille(&d) ==
				VENC_SHM_THROTTLE_MIN_PERMILLE);
		}
		CHECK("thr_ramp_no_overshoot",
			venc_shm_throttle_permille(&t) == 1000);
		CHECK("thr_ramp_pins_at_ceiling",
			step_window(&t, &now, 0, 0) == 0);
	}

	/* Occupancy at the recover threshold exactly still increases. */
	venc_shm_throttle_reset(&t, now);
	(void)step_window(&t, &now, 4, 0);
	CHECK("thr_recover_precondition",
		venc_shm_throttle_permille(&t) == 800);
	(void)step_window(&t, &now, VENC_SHM_THROTTLE_RECOVER_SLOTS, 0);
	CHECK("thr_recover_at_threshold",
		venc_shm_throttle_permille(&t) == 850);

	/* ── 5a. Transient bursts must NOT engage the clamp ─────────── */
	/* This is the defect the .232 bench exposed: at 100 fps into an
	 * 8-slot ring a healthy consumer still lets occupancy spike to 2-3
	 * mid-window and then drains it.  Under high-water that read as
	 * congestion and clamped the encoder 15-25 % at random.  Low-water
	 * sees the ring touch bottom and leaves it alone. */
	venc_shm_throttle_reset(&t, now);
	{
		int w, i;
		for (w = 0; w < 30; ++w) {
			for (i = 0; i < 20; ++i) {
				/* spike to 3 for one frame in twenty, drain
				 * to 0 for the rest -- a healthy consumer */
				now += WIN / 20;
				venc_shm_throttle_observe(&t,
					(i == 7) ? 3u : 0u, 0);
				(void)venc_shm_throttle_tick(&t, now);
			}
		}
		CHECK("thr_transient_burst_does_not_engage",
			venc_shm_throttle_permille(&t) == 1000);
	}

	/* Standing backlog -- never drains below 2 -- must engage. */
	venc_shm_throttle_reset(&t, now);
	{
		int w, i;
		for (w = 0; w < 3; ++w) {
			for (i = 0; i < 20; ++i) {
				now += WIN / 20;
				/* varies 2..5, never touches bottom */
				venc_shm_throttle_observe(&t,
					2u + (uint32_t)(i % 4), 0);
				(void)venc_shm_throttle_tick(&t, now);
			}
		}
		CHECK("thr_standing_backlog_engages",
			venc_shm_throttle_permille(&t) == 512);
	}

	/* A window with no frames at all (idle encoder) holds, never clamps. */
	venc_shm_throttle_reset(&t, now);
	(void)step_window(&t, &now, 8, 0);
	CHECK("thr_idle_precondition", venc_shm_throttle_permille(&t) == 800);
	now += WIN * 5;
	CHECK("thr_idle_window_no_change",
		venc_shm_throttle_tick(&t, now) == 0);
	CHECK("thr_idle_window_holds",
		venc_shm_throttle_permille(&t) == 800);

	/* ── 5. No oscillation under a square-wave load ─────────────── */
	venc_shm_throttle_reset(&t, now);
	{
		int i;
		uint16_t lo = 1000, hi = 0;

		/* Settle first, then measure the band over the tail. */
		for (i = 0; i < 60; ++i)
			(void)step_window(&t, &now, (i & 1) ? 3 : 0, 0);
		for (i = 0; i < 40; ++i) {
			uint16_t p;
			(void)step_window(&t, &now, (i & 1) ? 3 : 0, 0);
			p = venc_shm_throttle_permille(&t);
			if (p < lo) lo = p;
			if (p > hi) hi = p;
		}
		CHECK("thr_squarewave_bounded_low",
			lo >= VENC_SHM_THROTTLE_FLOOR_PERMILLE);
		CHECK("thr_squarewave_bounded_high", hi <= 1000);
		/* The amplitude must not grow: one MD and one AI step apart,
		 * which near the floor is a ~50-permille band. */
		CHECK("thr_squarewave_amplitude_small", (hi - lo) <= 100);
	}

	/* ── 6. Disabled ────────────────────────────────────────────── */
	venc_shm_throttle_reset(&t, now);
	(void)step_window(&t, &now, 8, 0);
	CHECK("thr_disable_precondition",
		venc_shm_throttle_permille(&t) == 800);

	venc_shm_throttle_set_enabled(&t, 0, now);
	CHECK("thr_disable_releases_clamp",
		venc_shm_throttle_permille(&t) == 1000);
	CHECK("thr_disable_flags_reapply",
		venc_shm_throttle_tick(&t, now) == 1);
	CHECK("thr_disable_reports_disabled",
		venc_shm_throttle_is_enabled(&t) == 0);

	{
		int i;
		int changed = 0;
		for (i = 0; i < 40; ++i)
			if (step_window(&t, &now, 8, (uint64_t)i))
				changed = 1;
		CHECK("thr_disabled_pins_at_1000",
			venc_shm_throttle_permille(&t) == 1000);
		CHECK("thr_disabled_never_signals", changed == 0);
	}

	venc_shm_throttle_set_enabled(&t, 1, now);
	CHECK("thr_reenable_starts_unclamped",
		venc_shm_throttle_permille(&t) == 1000);
	CHECK("thr_reenable_no_spurious_change",
		venc_shm_throttle_tick(&t, now) == 0);
	(void)step_window(&t, &now, 8, 0);
	CHECK("thr_reenable_engages_again",
		venc_shm_throttle_permille(&t) == 800);

	/* Idempotent enable — calling every frame must not reset the ramp. */
	venc_shm_throttle_set_enabled(&t, 1, now);
	CHECK("thr_enable_idempotent",
		venc_shm_throttle_permille(&t) == 800);

	/* ── Scaling ────────────────────────────────────────────────── */
	CHECK("thr_scale_unclamped_exact",
		venc_shm_throttle_scale(1000, 12345) == 12345);
	CHECK("thr_scale_800", venc_shm_throttle_scale(800, 10000) == 8000);
	CHECK("thr_scale_floor", venc_shm_throttle_scale(250, 10000) == 2500);
	/* Below the floor the result is pinned to the dual floor in KBPS:
	 * min(10 % of 10000, 2500) = 1000. */
	CHECK("thr_scale_below_floor_clamped",
		venc_shm_throttle_scale(10, 10000) == 1000);

	/* Dual floor — the whole point of the absolute cap.  A craft that has
	 * dropped to MCS0 before waybeam-link's 1.5-8 s bitrate demotion
	 * actuates must not be stranded above what MCS0 can carry. */
	/* Percentage binds below the cap (10 % of 20000 = 2000 < 2500). */
	CHECK("thr_scale_dual_pct_binds",
		venc_shm_throttle_scale(50, 20000) == 2000);
	/* Absolute cap binds above it (10 % of 40000 = 4000 > 2500). */
	CHECK("thr_scale_dual_abs_binds",
		venc_shm_throttle_scale(50, 40000) ==
		VENC_SHM_THROTTLE_FLOOR_ABS_KBPS);
	/* They coincide at 25000, today's reference-craft ceiling. */
	CHECK("thr_scale_dual_crossover",
		venc_shm_throttle_scale(10, 25000) ==
		VENC_SHM_THROTTLE_FLOOR_ABS_KBPS);
	/* The measured live rate: was 5459 under the old 250 permille floor. */
	CHECK("thr_scale_dual_measured_rate",
		venc_shm_throttle_scale(10, 21839) == 2183);
	/* Low configured bitrates keep proportional behaviour; the absolute
	 * VENC_BITRATE_MIN_KBPS rail downstream is what catches the rest. */
	CHECK("thr_scale_dual_low_bitrate",
		venc_shm_throttle_scale(10, 8000) == 800);
	CHECK("thr_scale_zero_kbps", venc_shm_throttle_scale(800, 0) == 0);
	/* No overflow on a bitrate far above anything the encoder accepts. */
	CHECK("thr_scale_no_overflow",
		venc_shm_throttle_scale(500, 4000000000u) == 2000000000u);

	/* NULL tolerance — the hooks run on the hot path and must not need
	 * their own guards. */
	venc_shm_throttle_reset(NULL, now);
	venc_shm_throttle_observe(NULL, 8, 1);
	venc_shm_throttle_set_enabled(NULL, 0, now);
	CHECK("thr_null_tick", venc_shm_throttle_tick(NULL, now) == 0);
	CHECK("thr_null_permille", venc_shm_throttle_permille(NULL) == 1000);
	CHECK("thr_null_enabled", venc_shm_throttle_is_enabled(NULL) == 0);
	CHECK("thr_null_floor_edge", venc_shm_throttle_floor_edge(NULL) == 0);
	CHECK("thr_null_apply", venc_shm_throttle_apply(NULL, 5000) == 5000);

	return failures;
}
