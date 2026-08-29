#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "idr_rate_limit.h"
#include "test_helpers.h"

/* Unit test for the per-channel IDR rate limiter.  The gate uses
 * wb_monotonic_us() under the hood, so tests that need to cross the
 * spacing boundary call nanosleep() to advance real monotonic time —
 * we don't mock the clock (it's a trivial vDSO read in production). */

static void sleep_us(uint64_t us)
{
	struct timespec ts;
	ts.tv_sec  = (time_t)(us / 1000000ULL);
	ts.tv_nsec = (long)((us % 1000000ULL) * 1000ULL);
	nanosleep(&ts, NULL);
}

int test_idr_rate_limit(void)
{
	int failures = 0;

	idr_rate_limit_reset();

	/* First call on any channel is always honored. */
	CHECK("idr_first_call_honored", idr_rate_limit_allow(0) == 1);
	CHECK("idr_first_honored_count", idr_rate_limit_honored(0) == 1);
	CHECK("idr_first_dropped_count", idr_rate_limit_dropped(0) == 0);

	/* Back-to-back requests inside the lockout window are coalesced. */
	CHECK("idr_burst2_dropped", idr_rate_limit_allow(0) == 0);
	CHECK("idr_burst3_dropped", idr_rate_limit_allow(0) == 0);
	CHECK("idr_burst4_dropped", idr_rate_limit_allow(0) == 0);
	CHECK("idr_burst_honored_still_1", idr_rate_limit_honored(0) == 1);
	CHECK("idr_burst_dropped_3",       idr_rate_limit_dropped(0) == 3);

	/* Per-channel independence: channel 1 should still get its first. */
	CHECK("idr_chn1_first_honored", idr_rate_limit_allow(1) == 1);
	CHECK("idr_chn1_honored_count", idr_rate_limit_honored(1) == 1);
	CHECK("idr_chn0_unaffected",    idr_rate_limit_honored(0) == 1);

	/* Out-of-range channels bypass the gate (safer fallback). */
	CHECK("idr_bypass_negative", idr_rate_limit_allow(-1) == 1);
	CHECK("idr_bypass_max",      idr_rate_limit_allow(IDR_RATE_LIMIT_MAX_CHANNELS) == 1);
	CHECK("idr_bypass_huge",     idr_rate_limit_allow(9999) == 1);

	/* After the full spacing interval, next request on channel 0 is honored.
	 * 100 ms + 10 ms slack for scheduling noise on the test host. */
	sleep_us(IDR_RATE_LIMIT_MIN_SPACING_US + 10000);
	CHECK("idr_after_spacing_honored", idr_rate_limit_allow(0) == 1);
	CHECK("idr_after_honored_count",   idr_rate_limit_honored(0) == 2);
	CHECK("idr_after_dropped_stable",  idr_rate_limit_dropped(0) == 3);

	/* ── Bootstrap IDRs bypass the spacing gate ───────────────────────
	 *
	 * Output enable, a destination change and a live fps rebind hand the
	 * stream to a receiver that has seen no parameter set.  Coalescing one
	 * of those leaves it with nothing to start from until the next GOP or
	 * GDR cycle -- while the caller is told the apply succeeded. */
	idr_rate_limit_reset();

	/* Arm the window with an ordinary request, then prove an ordinary one
	 * behind it really is swallowed (the defect this guards). */
	CHECK("idr_boot_arm", idr_rate_limit_allow(0) == 1);
	CHECK("idr_boot_ordinary_coalesced", idr_rate_limit_allow(0) == 0);
	CHECK("idr_boot_ordinary_dropped", idr_rate_limit_dropped(0) == 1);

	/* A bootstrap inside that same window is honored anyway... */
	idr_rate_limit_force(0);
	CHECK("idr_boot_honored_counted", idr_rate_limit_honored(0) == 2);
	/* ...and is NOT booked as a drop. */
	CHECK("idr_boot_not_dropped", idr_rate_limit_dropped(0) == 1);

	/* It re-arms the window, so an ordinary request right behind a
	 * bootstrap still coalesces -- a bootstrap must not open a hole for a
	 * storm to pour through. */
	CHECK("idr_boot_rearms_window", idr_rate_limit_allow(0) == 0);
	CHECK("idr_boot_rearm_dropped", idr_rate_limit_dropped(0) == 2);

	/* Back-to-back bootstraps are each honored: two of these in one window
	 * means two genuinely discontinuous events (enable then a destination
	 * change), and both receivers need a start point. */
	idr_rate_limit_force(0);
	CHECK("idr_boot_back_to_back", idr_rate_limit_honored(0) == 3);

	/* Per-channel, like the rest of the gate. */
	CHECK("idr_boot_chn1_untouched", idr_rate_limit_honored(1) == 0);

	/* Out-of-range channels are inert rather than corrupting state. */
	idr_rate_limit_force(-1);
	idr_rate_limit_force(IDR_RATE_LIMIT_MAX_CHANNELS);
	CHECK("idr_boot_bypass_no_crash", idr_rate_limit_honored(0) == 3);

	/* Reset clears counters. */
	idr_rate_limit_reset();
	CHECK("idr_reset_honored", idr_rate_limit_honored(0) == 0);
	CHECK("idr_reset_dropped", idr_rate_limit_dropped(0) == 0);
	CHECK("idr_reset_first",   idr_rate_limit_allow(0) == 1);

	return failures;
}
